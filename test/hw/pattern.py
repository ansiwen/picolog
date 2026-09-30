"""Pattern lines for the picolog tests, and an analyzer for what comes back.

A pattern line is 62 bytes:

    PL<seq:08d> <40 payload chars> <crc32:08x>\\r\\n

The payload is derived from the sequence number and the CRC covers everything
before it, so a receiver can tell corruption from loss from duplication.
"""

import re
import zlib
from dataclasses import dataclass, field

PAYLOAD_LEN = 40
LINE_LEN = 2 + 8 + 1 + PAYLOAD_LEN + 1 + 8 + 2
_ALPHABET = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"

_LINE_RE = re.compile(rb"^PL(\d{8}) ([ -~]{40}) ([0-9a-f]{8})$")
# In-band markers written by the firmware (recorded ones and the per-port one).
_MARKER_RE = re.compile(rb"\r\n\[picolog: ([^\]\r\n]*)\]\r\n")
_HEADER_RE = re.compile(
    rb"=== picolog replay: (\d+) bytes, uptime (\d+)d (\d\d):(\d\d):(\d\d) ===\r\n"
)
REPLAY_END = b"\r\n=== picolog replay end, live follows ===\r\n"


def payload_for(seq: int) -> bytes:
    """40 printable chars derived from seq (cheap LCG, no two nearby seqs alike)."""
    x = (seq * 2654435761 + 12345) & 0xFFFFFFFF
    out = bytearray()
    for _ in range(PAYLOAD_LEN):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(_ALPHABET[(x >> 8) % len(_ALPHABET)])
    return bytes(out)


def make_line(seq: int) -> bytes:
    body = b"PL%08d " % seq + payload_for(seq)
    return body + b" %08x\r\n" % (zlib.crc32(body) & 0xFFFFFFFF)


def make_lines(start: int, count: int) -> bytes:
    return b"".join(make_line(start + i) for i in range(count))


@dataclass
class Analysis:
    seqs: list = field(default_factory=list)      # sequence numbers of valid lines
    gaps: list = field(default_factory=list)      # (after_seq, missing_count)
    dups: list = field(default_factory=list)      # seq numbers seen out of order / twice
    bad_lines: list = field(default_factory=list) # complete lines that failed to parse
    markers: list = field(default_factory=list)   # marker texts, e.g. "BREAK"
    problems: list = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.problems

    @property
    def first(self):
        return self.seqs[0] if self.seqs else None

    @property
    def last(self):
        return self.seqs[-1] if self.seqs else None

    def dropped_bytes(self) -> int:
        total = 0
        for m in self.markers:
            mm = re.match(r"(\d+) bytes dropped$", m)
            if mm:
                total += int(mm.group(1))
        return total


# Markers after which lost or damaged data is legitimate.
_LOSS_MARKERS = re.compile(
    r"bytes dropped|capture overrun|UART FIFO overrun|framing error|parity error|BREAK"
)


def analyze(data: bytes, strict: bool = True) -> Analysis:
    """Check a captured stream of pattern lines.

    The stream is cut at its first line: bytes before the first line terminator
    are ignored (we started reading in the middle of something), as is an
    unterminated tail.

    strict:  no markers at all, every line valid and consecutive.
    lenient: markers are removed (a marker may split a line in two, which then
             joins up again), and gaps / damaged lines are tolerated only if a
             loss-type marker (dropped, overrun, framing, ...) was seen.
    """
    a = Analysis()
    for m in _MARKER_RE.finditer(data):
        a.markers.append(m.group(1).decode("ascii", "replace"))
    if strict and a.markers:
        a.problems.append(f"unexpected markers: {a.markers}")
    data = _MARKER_RE.sub(b"", data)

    parts = data.split(b"\r\n")
    # parts[0]: possibly a partial line; parts[-1]: unterminated tail.
    lines = parts[1:-1]
    loss_ok = (not strict) and any(_LOSS_MARKERS.search(m) for m in a.markers)

    prev = None
    for raw in lines:
        m = _LINE_RE.match(raw)
        if not m:
            a.bad_lines.append(raw)
            if not loss_ok:
                a.problems.append(f"unparsable line: {raw[:70]!r}")
            continue
        seq = int(m.group(1))
        body = raw[: raw.rindex(b" ")]
        if zlib.crc32(body) & 0xFFFFFFFF != int(m.group(3), 16) or m.group(2) != payload_for(seq):
            a.bad_lines.append(raw)
            if not loss_ok:
                a.problems.append(f"corrupt line seq {seq}")
            continue
        if prev is not None:
            if seq == prev + 1:
                pass
            elif seq > prev + 1:
                a.gaps.append((prev, seq - prev - 1))
                if not loss_ok:
                    a.problems.append(f"gap after {prev}: {seq - prev - 1} lines missing")
            else:
                a.dups.append(seq)
                a.problems.append(f"out of order or duplicate: {seq} after {prev}")
        a.seqs.append(seq)
        prev = seq
    return a


@dataclass
class Replay:
    header_bytes: int   # N from the header
    uptime_s: int
    history: bytes      # the replayed history
    live: bytes         # everything after the end marker (None if there is none)
    complete: bool      # end marker seen


def split_replay(data: bytes) -> Replay:
    """Split output of the replay port into header, history and the live tail."""
    m = _HEADER_RE.match(data)
    if not m:
        raise ValueError(f"no replay header at start: {data[:80]!r}")
    up = int(m.group(2)) * 86400 + int(m.group(3)) * 3600 + int(m.group(4)) * 60 + int(m.group(5))
    rest = data[m.end():]
    i = rest.find(REPLAY_END)
    if i < 0:
        return Replay(int(m.group(1)), up, rest, b"", False)
    return Replay(int(m.group(1)), up, rest[:i], rest[i + len(REPLAY_END):], True)


def cut_at(data: bytes, seq: int) -> bytes:
    """Return `data` starting at the line with sequence number `seq`, prefixed with
    a line terminator so analyze() keeps that line (it discards whatever precedes
    the first terminator). Raises ValueError if the line is not there."""
    i = data.find(b"PL%08d " % seq)
    if i < 0:
        raise ValueError(f"line {seq} not found in {len(data)} bytes")
    return b"\r\n" + data[i:]
