"""Hardware integration tests for picolog.

Setup: a USB-serial adapter whose TX is wired to the Pico's GP1 (plus GND)
acts as the "target". Both the adapter and the Pico are plugged into the
machine running the tests. All tests are skipped unless PICOLOG_ADAPTER is
set.

Environment:
  PICOLOG_ADAPTER      adapter tty, e.g. /dev/ttyUSB0 (required)
  PICOLOG_LIVE         live port   (default /dev/picolog-live)
  PICOLOG_REPLAY       replay port (default /dev/picolog-replay)
  PICOLOG_BAUD         must match the firmware's PICOLOG_UART_BAUD (default 115200)
  PICOLOG_HISTORY      history size in bytes (default 262144)
  PICOLOG_SIM=1        running against test/sim/picolog_sim (skips BREAK, which it cannot model)
  PICOLOG_UNPLUG_CMD / PICOLOG_PLUG_CMD
                       shell commands that cut / restore the Pico's USB connection,
                       e.g. `uhubctl -l 1-1 -p 2 -a off` / `... -a on` on a hub with
                       per-port power switching (the Pico must have its own supply)
  PICOLOG_INTERACTIVE=1 unplug/replug by hand instead (run with `pytest -s`)

Run:  cd test/hw && PICOLOG_ADAPTER=/dev/ttyUSB0 pytest -v test_integration.py
"""

from __future__ import annotations

import os
import random
import subprocess
import threading
import time

import pytest

import pattern

serial = pytest.importorskip("serial")

ADAPTER = os.environ.get("PICOLOG_ADAPTER")
LIVE = os.environ.get("PICOLOG_LIVE", "/dev/picolog-live")
REPLAY = os.environ.get("PICOLOG_REPLAY", "/dev/picolog-replay")
BAUD = int(os.environ.get("PICOLOG_BAUD", "115200"))
HISTORY = int(os.environ.get("PICOLOG_HISTORY", str(256 * 1024)))
SIM = os.environ.get("PICOLOG_SIM") == "1"
UNPLUG_CMD = os.environ.get("PICOLOG_UNPLUG_CMD")
PLUG_CMD = os.environ.get("PICOLOG_PLUG_CMD")
INTERACTIVE = os.environ.get("PICOLOG_INTERACTIVE") == "1"

BYTES_PER_SEC = BAUD / 10  # 8N1

pytestmark = pytest.mark.skipif(not ADAPTER, reason="PICOLOG_ADAPTER not set (no hardware)")


# ---------------------------------------------------------------- helpers


def secs_for(nbytes: int) -> float:
    return nbytes / BYTES_PER_SEC


def wait_for_path(path: str, present: bool, timeout: float = 15.0) -> None:
    deadline = time.monotonic() + timeout
    while os.path.exists(path) != present:
        if time.monotonic() > deadline:
            raise TimeoutError(f"{path} did not {'appear' if present else 'disappear'}")
        time.sleep(0.1)


def usb_disconnect() -> None:
    if UNPLUG_CMD:
        subprocess.run(UNPLUG_CMD, shell=True, check=True)
    else:
        input("\nUnplug the Pico's USB cable (keep its own supply on), then press Enter...")
    wait_for_path(REPLAY, present=False)


def usb_reconnect() -> None:
    if PLUG_CMD:
        subprocess.run(PLUG_CMD, shell=True, check=True)
    else:
        input("\nPlug the Pico's USB cable back in, then press Enter...")
    wait_for_path(REPLAY, present=True)
    time.sleep(1)  # let udev finish (permissions, symlinks)


class Reader:
    """Reads a port in a background thread (DTR is asserted on open)."""

    def __init__(self, path: str):
        self.ser = serial.Serial(path, timeout=0.05)
        self.buf = bytearray()
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self):
        while not self._stop.is_set():
            self.buf += self.ser.read(65536)

    def wait_for(self, needle: bytes, timeout: float) -> None:
        deadline = time.monotonic() + timeout
        while needle not in self.buf:
            if time.monotonic() > deadline:
                raise TimeoutError(f"{needle!r} not seen within {timeout}s")
            time.sleep(0.05)

    def close(self) -> bytes:
        self._stop.set()
        self._t.join()
        self.ser.close()
        return bytes(self.buf)


def read_replay(timeout: float | None = None) -> pattern.Replay:
    r = Reader(REPLAY)
    try:
        r.wait_for(pattern.END_MARKER, timeout or 10 + secs_for(HISTORY) / 50)
        time.sleep(0.3)
    finally:
        data = r.close()
    return pattern.split_replay(data)


def from_seq(data: bytes, seq: int) -> bytes:
    """Cut `data` at the first occurrence of pattern line `seq`."""
    idx = data.find(pattern.line(seq))
    assert idx >= 0, f"line {seq} not found"
    return data[idx:]


def open_reader(path: str) -> Reader:
    r = Reader(path)
    time.sleep(0.3)  # firmware waits 100 ms after DTR before sending
    return r


def send_recorded(gen, count: int) -> None:
    """Send `count` lines and wait until the last one is in the history.

    write()/flush() return while bytes may still be queued in the host tty
    layer or the adapter, so completion is observed on the live port.
    """
    r = open_reader(LIVE)
    try:
        gen.send(count)
        r.wait_for(pattern.line(gen.last_seq), 10 + secs_for(count * pattern.LINE_LEN))
    finally:
        r.close()


def lines_for(nbytes: int) -> int:
    return -(-nbytes // pattern.LINE_LEN)


@pytest.fixture
def gen():
    from picolog_gen import Generator

    # Random start so each test can find its own data in the shared history.
    g = Generator(ADAPTER, BAUD, start=random.randrange(10_000_000, 80_000_000))
    yield g
    g.close()


# ---------------------------------------------------------------- tests


def test_live_sees_every_line(gen):
    duration = 10
    r = open_reader(LIVE)
    first = gen.next_seq
    gen.send_bytes(int(BYTES_PER_SEC * duration))
    r.wait_for(pattern.line(gen.last_seq), 10 + duration)
    data = r.close()
    rep = pattern.analyze(from_seq(data, first))
    assert rep.errors == []
    assert rep.seqs[0] == first and rep.seqs[-1] == gen.last_seq


def test_replay_full_history(gen):
    send_recorded(gen, lines_for(int(HISTORY * 1.25)))
    rp = read_replay()
    assert rp.size == HISTORY
    assert len(rp.history) == rp.size
    rep = pattern.analyze(rp.history)
    assert rep.errors == []
    assert rep.seqs[-1] == gen.last_seq
    assert rp.history.endswith(pattern.line(gen.last_seq))


def test_replay_every_reconnect(gen):
    send_recorded(gen, 200)
    a = read_replay()
    b = read_replay()
    assert a.history == b.history  # reading never consumes the buffer


def test_replay_live_seam_while_streaming(gen):
    first = gen.next_seq
    gen.start()
    try:
        time.sleep(2)
        r = Reader(REPLAY)
        r.wait_for(pattern.END_MARKER, 30)
        time.sleep(3)
        data = r.close()
    finally:
        gen.stop()
    rp = pattern.split_replay(data)
    stream = from_seq(rp.history, first) + rp.live
    rep = pattern.analyze(stream)
    assert rep.errors == [], "gap or duplicate across the replay/live seam"
    assert len(pattern.parse(rp.live)) > 10, "no live data after the replay"


def test_both_ports_independently(gen):
    first = gen.next_seq
    gen.start()
    try:
        live, replay = Reader(LIVE), Reader(REPLAY)
        time.sleep(5)
        live_data, replay_data = live.close(), replay.close()
    finally:
        gen.stop()
    rep = pattern.analyze(live_data)
    assert rep.errors == [] and len(rep.seqs) > 10
    rp = pattern.split_replay(replay_data)
    assert pattern.analyze(from_seq(rp.history, first) + rp.live).errors == []


def test_slow_live_reader_gets_dropped_marker(gen):
    # Open (DTR high) but do not read. Linux buffers a few KiB in the tty
    # layer, after which the device's TX FIFO fills up and it must drop.
    ser = serial.Serial(LIVE, timeout=0.05)
    try:
        gen.send_bytes(HISTORY * 2)
        data = bytearray()
        deadline = time.monotonic() + 10 + secs_for(HISTORY)
        while time.monotonic() < deadline:
            data += ser.read(65536)
            if data.endswith(pattern.line(gen.last_seq)):
                break
    finally:
        ser.close()
    rep = pattern.analyze(bytes(data), strict=False)
    assert rep.markers_matching(b"bytes dropped")
    assert rep.errors == []
    assert rep.seqs[-1] == gen.last_seq
    # Firmware did not stall: the replay port still answers.
    assert read_replay().size > 0


@pytest.mark.skipif(SIM, reason="the simulator has no UART line to break")
def test_break_marker(gen):
    r = open_reader(LIVE)
    first = gen.next_seq
    gen.send(20)
    gen.send_break(0.05)
    time.sleep(0.3)  # marker rate limit is 100 ms
    gen.send(20)
    r.wait_for(pattern.line(gen.last_seq), 10)
    time.sleep(0.2)
    data = r.close()
    rep = pattern.analyze(from_seq(data, first), strict=False)
    assert rep.errors == []
    assert rep.markers_matching(b"BREAK")
    assert rep.seqs[-1] == gen.last_seq


@pytest.mark.skipif(
    SIM or not ((UNPLUG_CMD and PLUG_CMD) or INTERACTIVE),
    reason="needs PICOLOG_UNPLUG_CMD/PICOLOG_PLUG_CMD or PICOLOG_INTERACTIVE=1 (with pytest -s)",
)
def test_recording_continues_while_usb_unplugged(gen):
    """The Pico has its own supply: unplugging USB must not lose anything."""
    first = gen.next_seq
    send_recorded(gen, 200)
    usb_disconnect()
    gen.send(500)  # target keeps talking while no host is attached
    usb_reconnect()
    send_recorded(gen, 200)  # also shows the live port works after replug
    rp = read_replay()
    rep = pattern.analyze(from_seq(rp.history, first))
    assert rep.errors == []
    assert rep.seqs[0] == first and rep.seqs[-1] == gen.last_seq
