"""Test pattern lines and a stream analyzer for picolog integration tests.

Each generated line looks like

    PL00001234 <40 payload chars> 1a2b3c4d\r\n

with a sequence number and a CRC32 over everything before the CRC, so the
checker can detect gaps, duplicates, reordering and corruption.
"""

from __future__ import annotations

import re
import string
import zlib
from dataclasses import dataclass, field

PAYLOAD_LEN = 40
_ALPHABET = (string.ascii_letters + string.digits).encode()

LINE_RE = re.compile(rb"^PL(\d{8}) ([0-9A-Za-z]{%d}) ([0-9a-f]{8})$" % PAYLOAD_LEN)
MARKER_RE = re.compile(rb"^\[picolog: (.*)\]$")
HEADER_RE = re.compile(
    rb"^=== picolog replay: (\d+) bytes, uptime (\d+)d (\d\d):(\d\d):(\d\d), "
    rb"resets since power-on (\d+) ===$"
)
END_TEXT = b"=== picolog replay end, live follows ==="
END_MARKER = b"\r\n" + END_TEXT + b"\r\n"


def line(seq: int) -> bytes:
    payload = bytes(_ALPHABET[(seq * 31 + i * 7) % len(_ALPHABET)] for i in range(PAYLOAD_LEN))
    body = b"PL%08d %s" % (seq % 100_000_000, payload)
    return body + b" %08x\r\n" % zlib.crc32(body)


LINE_LEN = len(line(0))


def lines(start: int, count: int) -> bytes:
    return b"".join(line(s) for s in range(start, start + count))


@dataclass
class Item:
    kind: str  # line | marker | header | end | blank | other | partial
    text: bytes
    seq: int = -1


@dataclass
class Report:
    items: list[Item]
    errors: list[str] = field(default_factory=list)

    @property
    def seqs(self) -> list[int]:
        return [i.seq for i in self.items if i.kind == "line"]

    @property
    def markers(self) -> list[bytes]:
        return [i.text for i in self.items if i.kind == "marker"]

    def markers_matching(self, needle: bytes) -> list[bytes]:
        return [m for m in self.markers if needle in m]


def parse(data: bytes) -> list[Item]:
    """Split a byte stream into classified lines."""
    out: list[Item] = []
    parts = data.split(b"\n")
    for idx, raw in enumerate(parts):
        last = idx == len(parts) - 1
        if last and raw == b"":
            break
        text = raw[:-1] if raw.endswith(b"\r") else raw
        if last:
            out.append(Item("partial", text))
            continue
        m = LINE_RE.match(text)
        if m and int(m.group(3), 16) == zlib.crc32(text[: -9]):
            out.append(Item("line", text, int(m.group(1))))
        elif MARKER_RE.match(text):
            out.append(Item("marker", MARKER_RE.match(text).group(1)))
        elif HEADER_RE.match(text):
            out.append(Item("header", text))
        elif text == END_TEXT:
            out.append(Item("end", text))
        elif text == b"":
            out.append(Item("blank", text))
        else:
            out.append(Item("other", text))
    return out


# Markers after which a sequence gap is expected.
GAP_MARKERS = (b"bytes dropped", b"capture overrun", b"boot,")


def analyze(data: bytes, strict: bool = True) -> Report:
    """Check that pattern lines are contiguous.

    The first item may be a fragment (a stream can start mid-line). With
    strict=True, any other non-line content is an error. With strict=False,
    picolog markers are allowed; a marker can split a line in two, so one
    missing sequence number is accepted if a fragment ("other") shows up in
    between, and larger gaps are accepted after dropped/overrun/boot markers.
    """
    items = parse(data)
    rep = Report(items)
    prev = None
    fragment_since_prev = False
    gap_marker_since_prev = False
    for n, it in enumerate(items):
        if it.kind == "line":
            if prev is not None and it.seq != prev + 1:
                ok = (
                    not strict
                    and it.seq > prev
                    and (gap_marker_since_prev or (it.seq == prev + 2 and fragment_since_prev))
                )
                if not ok:
                    what = "duplicate/reorder" if it.seq <= prev else "gap"
                    rep.errors.append(f"{what}: line {it.seq} after {prev}")
            prev = it.seq
            fragment_since_prev = False
            gap_marker_since_prev = False
        elif it.kind in ("other", "partial"):
            if n == 0 or it.kind == "partial":
                continue  # leading / trailing fragment
            if strict:
                rep.errors.append(f"unexpected text: {it.text[:80]!r}")
            fragment_since_prev = True
        elif it.kind == "marker":
            if strict:
                rep.errors.append(f"unexpected marker: {it.text!r}")
            if any(g in it.text for g in GAP_MARKERS):
                gap_marker_since_prev = True
        elif it.kind in ("header", "end"):
            if strict:
                rep.errors.append(f"unexpected {it.kind}: {it.text!r}")
        # blank lines are harmless (markers start with \r\n)
    return rep


@dataclass
class Replay:
    header: re.Match
    history: bytes  # bytes between header and end marker
    live: bytes  # bytes after the end marker

    @property
    def size(self) -> int:
        return int(self.header.group(1))

    @property
    def resets(self) -> int:
        return int(self.header.group(6))


def split_replay(data: bytes) -> Replay:
    """Split a replay-port capture into header, history and live parts."""
    nl = data.find(b"\r\n")
    if nl < 0:
        raise ValueError("no header line")
    m = HEADER_RE.match(data[:nl])
    if not m:
        raise ValueError(f"bad header: {data[:nl][:120]!r}")
    rest = data[nl + 2 :]
    end = rest.find(END_MARKER)
    if end < 0:
        raise ValueError("no end marker")
    return Replay(m, rest[:end], rest[end + len(END_MARKER) :])
