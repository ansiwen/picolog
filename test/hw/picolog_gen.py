#!/usr/bin/env python3
"""Pattern generator for the target side of a picolog test setup.

Drives a USB-serial adapter wired to the Pico's UART RX pin (or, with the host
simulator, the DIR/uart pty) with pattern lines from pattern.py.

    picolog_gen.py /dev/ttyUSB0 --baud 115200 --count 1000
    picolog_gen.py /dev/ttyUSB0 --forever --rate 50
    picolog_gen.py /dev/ttyUSB0 --break
"""

import argparse
import random
import sys
import time

import serial

import pattern


class Generator:
    def __init__(self, port: str, baud: int = 115200, start_seq=None):
        self.ser = serial.Serial(port, baud, timeout=1, write_timeout=30)
        self.baud = baud
        self.next_seq = random.randrange(0, 50_000_000) if start_seq is None else start_seq

    def send(self, count: int, rate: float = 0.0) -> int:
        """Send `count` lines. rate = lines per second, 0 = as fast as the wire allows.
        Returns the sequence number of the last line sent (or first-1 if count == 0)."""
        if rate <= 0:
            chunk = 32
            while count > 0:
                n = min(chunk, count)
                self._write(pattern.make_lines(self.next_seq, n))
                self.next_seq += n
                count -= n
        else:
            period = 1.0 / rate
            t0 = time.monotonic()
            for i in range(count):
                self._write(pattern.make_line(self.next_seq))
                self.next_seq += 1
                delay = t0 + (i + 1) * period - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
        return self.next_seq - 1

    def send_break(self, duration: float = 0.25) -> None:
        self.ser.send_break(duration)

    def _write(self, data: bytes) -> None:
        self.ser.write(data)

    def close(self) -> None:
        self.ser.close()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--start", type=int, default=None, help="first sequence number (default: random)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--count", type=int, default=100)
    g.add_argument("--forever", action="store_true")
    g.add_argument("--break", dest="brk", action="store_true", help="send a BREAK and exit")
    ap.add_argument("--rate", type=float, default=0.0, help="lines per second (default: wire speed)")
    args = ap.parse_args(argv)

    gen = Generator(args.port, args.baud, args.start)
    try:
        if args.brk:
            gen.send_break()
        elif args.forever:
            while True:
                gen.send(100, args.rate)
        else:
            last = gen.send(args.count, args.rate)
            print(f"sent {args.count} lines, last seq {last}")
    except KeyboardInterrupt:
        pass
    finally:
        gen.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
