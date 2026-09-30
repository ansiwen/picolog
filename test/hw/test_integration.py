"""Integration tests: a USB-serial adapter plays the target, pyserial reads the
picolog USB ports (real device or the host simulator).

Environment:
  PICOLOG_ADAPTER      serial port wired to the Pico's UART RX (required, else all skip)
  PICOLOG_LIVE         live port     (default /dev/picolog-live)
  PICOLOG_REPLAY       replay port   (default /dev/picolog-replay)
  PICOLOG_BAUD         UART baud of the firmware build (default 115200)
  PICOLOG_HISTORY      history size in bytes (default 262144)
  PICOLOG_SIM          1 when running against test/sim/picolog_sim (no BREAK, no unplug)
  PICOLOG_UNPLUG_CMD / PICOLOG_PLUG_CMD
                       shell commands that cut / restore the Pico's USB (e.g. uhubctl)
  PICOLOG_INTERACTIVE  1: ask a human to unplug/replug (run pytest with -s)

Never assume bytes reached the device when write()/flush() return: tests wait
until the last line shows up on the live port (send_recorded).
"""

import os
import re
import subprocess
import threading
import time

import pytest
import serial

import pattern
from picolog_gen import Generator

ADAPTER = os.environ.get("PICOLOG_ADAPTER")
LIVE = os.environ.get("PICOLOG_LIVE", "/dev/picolog-live")
REPLAY = os.environ.get("PICOLOG_REPLAY", "/dev/picolog-replay")
BAUD = int(os.environ.get("PICOLOG_BAUD", "115200"))
HISTORY = int(os.environ.get("PICOLOG_HISTORY", str(256 * 1024)))
SIM = os.environ.get("PICOLOG_SIM") == "1"
UNPLUG_CMD = os.environ.get("PICOLOG_UNPLUG_CMD")
PLUG_CMD = os.environ.get("PICOLOG_PLUG_CMD")
INTERACTIVE = os.environ.get("PICOLOG_INTERACTIVE") == "1"

pytestmark = pytest.mark.skipif(not ADAPTER, reason="PICOLOG_ADAPTER not set")

WIRE_LINES_PER_S = BAUD / 10 / pattern.LINE_LEN


class Collector(threading.Thread):
    """Opens a port (which asserts DTR) and collects everything it delivers."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.ser = serial.Serial(path, timeout=0.05)
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._halt = threading.Event()
        self.start()

    def run(self):
        while not self._halt.is_set():
            try:
                d = self.ser.read(self.ser.in_waiting or 1)
            except (serial.SerialException, OSError):
                return
            if d:
                with self._lock:
                    self._buf += d

    def data(self) -> bytes:
        with self._lock:
            return bytes(self._buf)

    def wait_for(self, needle: bytes, timeout: float) -> bool:
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if needle in self.data():
                return True
            time.sleep(0.02)
        return needle in self.data()

    def close(self):
        self._halt.set()
        self.join(1)
        self.ser.close()


@pytest.fixture
def gen():
    g = Generator(ADAPTER, BAUD)
    yield g
    g.close()


@pytest.fixture
def collectors():
    made = []

    def make(path):
        c = Collector(path)
        made.append(c)
        return c

    yield make
    for c in made:
        c.close()


def wire_time(nbytes: int) -> float:
    return nbytes * 10 / BAUD


def send_recorded(gen, live, count, rate=0.0):
    """Send `count` lines and wait until the last one appears on the live port,
    proving it went through the device. Returns the last sequence number."""
    last = gen.send(count, rate)
    assert live.wait_for(pattern.make_line(last), 10 + wire_time(count * pattern.LINE_LEN)), \
        f"line {last} never appeared on the live port"
    return last


def read_replay(quiet=0.5, timeout=60, until=None) -> bytes:
    """Open the replay port, read until the end marker (plus a moment more), close it.
    With `until` (a line), also wait for that line to have come through the device.

    Closing matters: a second open while the first is still open does not raise
    DTR again, so it would not start another replay."""
    rep = Collector(REPLAY)
    try:
        assert rep.wait_for(pattern.REPLAY_END, timeout), "no replay end marker"
        if until is not None:
            assert rep.wait_for(until, timeout), "last line never reached the device"
        time.sleep(quiet)
        return rep.data()
    finally:
        rep.close()


def reopen_pause():
    time.sleep(0.2)  # let the device see DTR go low before the next open


def check_stream(data: bytes, first: int, last: int, strict=True):
    a = pattern.analyze(pattern.cut_at(data, first), strict=strict)
    assert a.ok, a.problems[:5]
    assert (a.first, a.last) == (first, last), (a.first, a.last, first, last)
    return a


# ------------------------------------------------------------------------------

def test_live_sees_every_line(gen, collectors):
    live = collectors(LIVE)
    time.sleep(0.3)  # open delay
    first = gen.next_seq
    last = send_recorded(gen, live, 500)
    a = check_stream(live.data(), first, last)
    assert a.seqs == list(range(first, last + 1))


@pytest.mark.skipif(BAUD < 1_000_000, reason="needs a firmware built for 1 Mbaud")
def test_live_stress_high_baud(gen, collectors):
    live = collectors(LIVE)
    time.sleep(0.3)
    first = gen.next_seq
    last = send_recorded(gen, live, 20000)
    a = check_stream(live.data(), first, last)
    assert len(a.seqs) == 20000


def test_replay_returns_full_history(gen, collectors):
    live = collectors(LIVE)
    time.sleep(0.3)
    last = send_recorded(gen, live, HISTORY // pattern.LINE_LEN + 200)
    live.close()
    r = pattern.split_replay(read_replay())
    assert r.complete
    assert r.header_bytes == HISTORY
    assert len(r.history) == HISTORY
    assert r.uptime_s >= 0
    a = pattern.analyze(r.history)
    assert a.ok, a.problems[:5]
    assert a.last == last
    assert r.live == b""


def test_replay_twice_identical(gen, collectors):
    live = collectors(LIVE)
    time.sleep(0.3)
    send_recorded(gen, live, 200)
    live.close()
    r1 = pattern.split_replay(read_replay())
    reopen_pause()
    r2 = pattern.split_replay(read_replay())
    assert r1.complete and r2.complete
    assert r1.header_bytes == r2.header_bytes
    assert r1.history == r2.history


def test_replay_live_seam(gen, collectors):
    """Data keeps arriving while the replay is sent: no gap, no duplicate."""
    live = collectors(LIVE)
    time.sleep(0.3)
    first = gen.next_seq
    send_recorded(gen, live, 300)
    rep = collectors(REPLAY)
    time.sleep(0.05)  # inside the open delay: the snapshot is taken later
    n = 1500
    last = gen.send(n, rate=min(200.0, WIRE_LINES_PER_S * 0.5))
    assert rep.wait_for(pattern.make_line(last), 30 + wire_time(n * pattern.LINE_LEN))
    r = pattern.split_replay(rep.data())
    assert r.complete
    a = check_stream(r.history + r.live, first, last)
    assert a.seqs == list(range(first, last + 1))


def test_both_ports_at_once(gen, collectors):
    live = collectors(LIVE)
    rep = collectors(REPLAY)
    assert rep.wait_for(pattern.REPLAY_END, 30)
    time.sleep(0.2)
    first = gen.next_seq
    last = gen.send(800)
    assert live.wait_for(pattern.make_line(last), 10 + wire_time(800 * pattern.LINE_LEN))
    assert rep.wait_for(pattern.make_line(last), 10)
    check_stream(live.data(), first, last)
    r = pattern.split_replay(rep.data())
    check_stream(r.history + r.live, first, last)


def test_slow_live_reader_gets_dropped_marker(gen, collectors):
    slow = serial.Serial(LIVE, timeout=0.2)  # open (DTR up) but do not read
    try:
        time.sleep(0.3)
        first = gen.next_seq
        n = (HISTORY * 2 + 65536) // pattern.LINE_LEN
        last = gen.send(n)

        # The firmware must keep answering meanwhile: replay works, has the newest data.
        r = pattern.split_replay(read_replay(until=pattern.make_line(last)))
        assert r.complete
        got = pattern.analyze(r.history + r.live)
        assert got.ok and got.last == last, (got.problems[:3], got.last, last)

        # Now the slow reader wakes up.
        data = bytearray()
        quiet_since = time.monotonic()
        while time.monotonic() - quiet_since < 1.5:
            d = slow.read(65536)
            if d:
                data += d
                quiet_since = time.monotonic()
        assert re.search(rb"\r\n\[picolog: \d+ bytes dropped\]\r\n", bytes(data)), "no dropped marker"
        a = pattern.analyze(pattern.cut_at(bytes(data), first), strict=False)
        assert a.ok, a.problems[:5]
        assert a.dropped_bytes() > 0
        assert a.last == last
    finally:
        slow.close()


@pytest.mark.skipif(SIM, reason="the simulator does not model BREAK")
def test_break_is_recorded(gen, collectors):
    live = collectors(LIVE)
    time.sleep(0.3)
    send_recorded(gen, live, 10)
    gen.send_break()
    last = send_recorded(gen, live, 10)
    assert live.wait_for(b"[picolog: BREAK", 5)
    assert re.search(rb"\[picolog: BREAK( x\d+)?\]", live.data())
    r = pattern.split_replay(read_replay())
    assert b"[picolog: BREAK" in r.history
    a = pattern.analyze(r.history, strict=False)
    assert a.ok and a.last == last


def _run(cmd):
    subprocess.run(cmd, shell=True, check=True)


def _wait_for_path(path, timeout=30):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if os.path.exists(path):
            time.sleep(1.0)  # let udev / the driver settle
            return
        time.sleep(0.2)
    raise AssertionError(f"{path} did not come back")


def test_data_sent_while_usb_unplugged_is_in_replay(gen, collectors, capsys):
    if not (UNPLUG_CMD and PLUG_CMD) and not INTERACTIVE:
        pytest.skip("set PICOLOG_UNPLUG_CMD/PICOLOG_PLUG_CMD or PICOLOG_INTERACTIVE=1 (with -s)")

    live = collectors(LIVE)
    time.sleep(0.3)
    send_recorded(gen, live, 50)
    live.close()

    if UNPLUG_CMD:
        _run(UNPLUG_CMD)
    else:
        with capsys.disabled():
            input("\n>>> Unplug the Pico's USB cable, then press Enter: ")
    time.sleep(1.0)

    first = gen.next_seq
    last = gen.send(300)

    if PLUG_CMD:
        _run(PLUG_CMD)
    else:
        with capsys.disabled():
            input(">>> Plug the USB cable back in, then press Enter: ")
    _wait_for_path(REPLAY)

    r = pattern.split_replay(read_replay())
    assert r.complete
    a = check_stream(r.history + r.live, first, last)
    assert a.seqs == list(range(first, last + 1))
