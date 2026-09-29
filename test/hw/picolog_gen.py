#!/usr/bin/env python3
"""Pattern generator: drives the picolog target-side UART via a USB-serial
adapter (adapter TX -> Pico GP1, common GND).

    ./picolog_gen.py /dev/ttyUSB0 --baud 115200 --count 10000
    ./picolog_gen.py /dev/ttyUSB0 --forever --rate 200   # 200 lines/s
    ./picolog_gen.py /dev/ttyUSB0 --break                # send one BREAK
"""

from __future__ import annotations

import argparse
import threading
import time

import serial

import pattern


class Generator:
    """Writes pattern lines to a serial port, optionally in a thread."""

    def __init__(self, port: str, baud: int, start: int = 0, chunk: int = 16):
        self.ser = serial.Serial(port, baud, timeout=1, write_timeout=None)
        self.next_seq = start
        self.chunk = chunk
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def last_seq(self) -> int:
        return self.next_seq - 1

    def send(self, count: int, rate: float | None = None) -> None:
        """Send `count` lines (blocking) and wait until they left the adapter."""
        while count > 0 and not self._stop.is_set():
            n = min(self.chunk, count)
            if rate:
                n = 1
            self.ser.write(pattern.lines(self.next_seq, n))
            self.next_seq += n
            count -= n
            if rate:
                time.sleep(1.0 / rate)
        self.ser.flush()

    def send_bytes(self, nbytes: int) -> None:
        self.send(-(-nbytes // pattern.LINE_LEN))

    def send_break(self, duration: float = 0.05) -> None:
        self.ser.flush()
        self.ser.send_break(duration)

    def start(self, rate: float | None = None) -> None:
        """Send continuously in a background thread until stop()."""
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, args=(rate,), daemon=True)
        self._thread.start()

    def _run(self, rate):
        while not self._stop.is_set():
            self.send(self.chunk, rate)

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join()
            self._thread = None
        self.ser.flush()
        self._stop.clear()

    def close(self) -> None:
        self.stop()
        self.ser.close()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--start", type=int, default=0, help="first sequence number")
    ap.add_argument("--count", type=int, default=1000, help="lines to send")
    ap.add_argument("--forever", action="store_true", help="send until Ctrl-C")
    ap.add_argument("--rate", type=float, help="lines per second (default: as fast as the baud rate allows)")
    ap.add_argument("--break", dest="brk", action="store_true", help="send a BREAK and exit")
    args = ap.parse_args()

    gen = Generator(args.port, args.baud, args.start)
    try:
        if args.brk:
            gen.send_break()
        elif args.forever:
            while True:
                gen.send(100, args.rate)
        else:
            gen.send(args.count, args.rate)
    except KeyboardInterrupt:
        pass
    finally:
        print(f"last sequence number sent: {gen.last_seq}")
        gen.ser.close()


if __name__ == "__main__":
    main()
