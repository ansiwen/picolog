# picolog: UART flight recorder and live console for the Raspberry Pi Pico 2

picolog sits between a target device's diagnostic UART output and a host.
It gives you two things:

- **Live console:** watch the target's serial output in real time.
- **Flight recorder:** everything is recorded into a 256 KiB RAM ring buffer
  whether or not a host is connected. After a target crash you plug in a
  laptop and fetch the history.

The Pico shows up as a USB device with two serial ports and needs no custom
host tool:

| Port | Linux (with udev rule) | Behaviour when opened (DTR asserted) |
|---|---|---|
| live   | `/dev/picolog-live`   (`ttyACM0`, interface 00) | Streams new data from now on. |
| replay | `/dev/picolog-replay` (`ttyACM1`, interface 02) | Sends a header, the **whole history**, an end marker, then continues live. |

Reading never consumes the history: every open of the replay port replays
everything again. The history lives in RAM only and is never written to
flash: any reset or power loss of the Pico clears it.

## Wiring

```
target TX  ───────────►  GP1  (Pico 2 pin 2, UART0 RX)
target GND ───────────►  GND  (e.g. Pico 2 pin 3)
```

- The Pico's inputs are **3.3 V logic**. A 5 V TTL target needs a level
  shifter or at least a resistor divider. A real **RS-232** port (±5 to ±12 V,
  often a DB9 or an RJ45 "console" port) needs an RS-232 transceiver such as
  a MAX3232. Connecting either directly will damage the Pico.
- GP0 (UART0 TX) is left unconfigured; picolog never transmits to the target.
- GP1 has the internal pull-up enabled, so an unplugged target reads as an
  idle line rather than garbage.
- Power the Pico from the host's USB, or from any USB supply if you only
  want recording. The history is lost if the Pico loses power, so don't
  move its USB cable from the hub to a laptop to fetch the logs. Either plug
  the laptop into the same hub/supply path, or power the Pico separately
  through VSYS (see the Pico 2 datasheet for the required diode) so the USB
  cable can be swapped.

## Building

Requirements: CMake ≥ 3.13, `arm-none-eabi-gcc`, and
[pico-sdk](https://github.com/raspberrypi/pico-sdk) (developed against
2.3.1) with the TinyUSB submodule.

```sh
git clone -b 2.3.1 https://github.com/raspberrypi/pico-sdk ~/pico-sdk
git -C ~/pico-sdk submodule update --init lib/tinyusb
export PICO_SDK_PATH=~/pico-sdk

cmake -S . -B build
make -C build -j
# -> build/picolog.uf2
```

Build options (`cmake -S . -B build -DNAME=value`):

| Option | Default | Meaning |
|---|---|---|
| `PICOLOG_UART_BAUD` | `115200` | Target baud rate |
| `PICOLOG_UART_DATA_BITS` | `8` | 5 to 8 |
| `PICOLOG_UART_PARITY` | `UART_PARITY_NONE` | or `UART_PARITY_EVEN`, `UART_PARITY_ODD` |
| `PICOLOG_UART_STOP_BITS` | `1` | 1 or 2 |
| `PICOLOG_UART_RX_PIN` | `1` | Must be a UART0 RX-capable pin (1, 13, 17, 29, …) |
| `PICOLOG_USB_VID` / `PICOLOG_USB_PID` | `0x1209` / `0x0001` | pid.codes **test** VID/PID. Use your own for anything beyond development, and update the udev rule to match. |

The firmware is built with `-Wall -Wextra -Werror`.

## Flashing

1. Hold the **BOOTSEL** button while plugging in the Pico 2. It appears as a
   USB drive `RP2350`.
2. Copy `build/picolog.uf2` onto it. The Pico reboots into picolog.

Or with picotool, with the Pico in BOOTSEL mode: `picotool load -x build/picolog.uf2`.
The picolog firmware has no picotool reset interface, so `picotool -f`
cannot force it into BOOTSEL. Use the button.

## Host setup (Linux)

```sh
sudo cp udev/99-picolog.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

This rule:

- creates `/dev/picolog-live` and `/dev/picolog-replay`, plus
  `/dev/picolog/<serial>-live|replay` for setups with several picologs (the
  serial number is the flash unique ID);
- sets `ID_MM_DEVICE_IGNORE` so ModemManager doesn't probe the ports with AT
  commands. picolog ignores host input anyway, but probing opens the ports
  and wastes a replay.

Without the rule, use `/dev/ttyACM*`: the lower number is normally the
live port. Check with `udevadm info /dev/ttyACMx | grep INTERFACE_NUM`.

On Windows and macOS the two ports appear as two COM ports or
`/dev/cu.usbmodem*` devices; the interface names are "picolog live" and
"picolog replay".

## Usage

Any terminal program works as long as it **asserts DTR** when it opens the
port. picocom, minicom, screen, PuTTY, pyserial and `cat` all do. The
baud rate setting on the USB side is irrelevant.

Watch live:

```sh
picocom -q /dev/picolog-live          # quit with C-a C-x
screen /dev/picolog-live              # quit with C-a k
minicom -D /dev/picolog-live
```

Fetch the history after a crash:

```sh
picocom -q --logfile crash.log /dev/picolog-replay
```

A serial port has no end-of-file. After the end marker the replay port keeps
streaming live data, so in scripts use `timeout`:

```sh
stty -F /dev/picolog-replay raw -echo
timeout 5 cat /dev/picolog-replay > crash.log
```

(256 KiB take well under a second over USB. The rest of the timeout just
captures live data.)

On Windows: open the "picolog replay" COM port in PuTTY (connection type
*Serial*) with logging enabled (*Session → Logging → All session output*).

### What you'll see

A replay looks like this:

```
=== picolog replay: 262144 bytes, uptime 3d 04:12:55 ===
...history, oldest first...
=== picolog replay end, live follows ===
...live data...
```

*uptime* is the Pico's time since it started, i.e. how far back the
history could at most reach.

picolog inserts in-band markers into the recorded data. Each marker is on
its own line, and a marker may split a target line in two:

| Marker | Meaning |
|---|---|
| `[picolog: BREAK]` | The target held its TX line low for longer than a character, typically when the target resets, crashes or powers down. |
| `[picolog: framing error]`, `[picolog: parity error]` | Line errors. Many of them usually mean a wrong baud rate or format. Repeated errors are aggregated (`x N`) and emitted at most every 100 ms. |
| `[picolog: UART FIFO overrun]` | The Pico's UART receive FIFO overflowed. This should never happen. |
| `[picolog: capture overrun, N bytes lost]` | The main loop fell more than ~31 KiB behind the DMA. This should never happen. |

These markers are only sent to the port that is falling behind and are not
recorded:

| Marker | Meaning |
|---|---|
| `[picolog: N bytes dropped]` | Your reader was so slow that N unread bytes were overwritten in the ring. picolog skipped to the oldest byte still available. The firmware never blocks on a slow reader. |

## How it works

```
target TX ──► UART0 RX FIFO ──DMA (ring mode)──► stage-1 ring (32 KiB, aligned)
                                                      │  main loop copies
                                                      ▼
                                     history ring (256 KiB)
                                         ▲                     ▲
                              live cursor (port 1)   replay cursor (port 2)
```

- **Stage 1:** one DMA channel reads `UARTDR` (8-bit, paced by the UART RX
  DREQ) into a 32 KiB buffer with hardware address wrapping. That is the
  largest ring the DMA can wrap, and gives about 2.8 s of slack at 115200 baud.
- **Stage 2:** the single-core superloop copies new bytes into the
  256 KiB history ring. Offsets are monotonic 64-bit counters, and each port
  has its own 64-bit cursor. All producers and consumers run in the same
  core0 loop, so no locking is needed. The UART error IRQ only increments
  counters.
- **USB:** TinyUSB with two CDC-ACM interfaces and non-blocking writes. After
  DTR rises the port waits 100 ms before sending, because programs such as
  pyserial flush their input right after `open()`. For the replay port the
  snapshot (oldest byte, end, header) is taken when sending actually starts.
  The header and the first ~900 history bytes go into the TX FIFO at once, so
  a replay taken while data is flowing doesn't lose its oldest bytes.

### Notes on hardware facts (checked against the RP2350 datasheet and pico-sdk 2.3.1)

- **DMA ENDLESS mode:** confirmed, RP2350 `TRANS_COUNT.MODE = 0xF`. However, in
  ENDLESS mode the counter *does not decrement*, which makes it impossible
  to count received bytes or detect stage-1 overruns. picolog uses the
  equally RP2350-only **TRIGGER_SELF** mode instead (count 2^27). The channel
  re-triggers itself forever, continuing at its current write address, and
  the live count still tells how many bytes arrived. Bytes between two polls
  = `(prev − now) mod 2^27`. That is unambiguous because polls are far less
  than 2^27 bytes apart (over 90 s even at 12 Mbaud; the superloop never
  blocks).
- **UART `DMAONERR`** is kept clear. If set, the UART masks the RX DMA
  request while an error interrupt is pending, and capture would stall on
  the first framing error.
- **RP2350-E9** (A2 stepping: extra leakage on inputs holds a floating
  pad near 2.2 V, and the internal pull-*down* cannot overcome it): the
  datasheet says the pull-*up* still works and removes the condition.
  picolog uses only the pull-up on the RX pad. `gpio_pull_up()` also
  disables the pull-down. E9 is fixed in the A3 stepping.

### Limitations

- Any reset or power loss of the Pico clears the history. There is no
  watchdog: if the Pico firmware hangs, it stays hung (the history is not
  sent anymore, but it is also not wiped) until someone resets it.
- Line-error markers are placed where the main loop noticed the error. That
  can be a few characters after the character in error.
- If a port stays open, the live data after the replay is subject to the same
  "dropped" behaviour as the live port when the reader is slower than the
  target.

## Testing

### Unit tests (no hardware)

```sh
make -C test/unit
```

This builds `history.c` and `usb_ports.c` natively with ASan/UBSan and tests
wraparound, 64-bit offsets (across 2^32), dropped-byte detection, the
replay-to-live seam (no gap or duplicate), slow readers, and both ports
at once. The Python pattern checker has its own tests:

```sh
cd test/hw && python3 -m pytest test_pattern.py
```

### Simulator

`test/sim/picolog_sim` runs the real ring and port logic on Linux with
pseudo-terminals in place of UART and USB. You can point the hardware test
suite at it to check the test code and the port logic end to end. It
doesn't model DMA or BREAK.

```sh
make -C test/sim
test/sim/picolog_sim /tmp/psim 1000000 &
cd test/hw && PICOLOG_SIM=1 PICOLOG_ADAPTER=/tmp/psim/uart PICOLOG_LIVE=/tmp/psim/live \
  PICOLOG_REPLAY=/tmp/psim/replay PICOLOG_BAUD=1000000 python3 -m pytest -v test_integration.py
```

### Hardware integration tests

Setup: a USB-serial adapter (3.3 V) plays the target. Connect its TX to GP1
and GND to GND, and plug both into the test machine. Install the udev rule.

```sh
pip install -r test/hw/requirements.txt
cd test/hw
PICOLOG_ADAPTER=/dev/ttyUSB0 python3 -m pytest -v test_integration.py
```

| Test | Checks |
|---|---|
| `test_live_sees_every_line` | Every line arrives on the live port in order, with no gaps. |
| `test_replay_full_history` | Replay returns the full last 256 KiB in order, with header and end marker. |
| `test_replay_every_reconnect` | Reading doesn't consume: two replays are identical. |
| `test_replay_live_seam_while_streaming` | No gap or duplicate between replay end and live continuation while data flows. |
| `test_both_ports_independently` | Both ports open at once. |
| `test_slow_live_reader_gets_dropped_marker` | A live reader that stops reading gets a dropped marker, and the firmware doesn't stall. |
| `test_break_marker` | A BREAK from the adapter shows up as a marker. |

For the 1 Mbaud stress test, build with `-DPICOLOG_UART_BAUD=1000000` and run
with `PICOLOG_BAUD=1000000` (the adapter must support that rate). The test
lines carry sequence numbers and a CRC32. The generator also works on its own:
`test/hw/picolog_gen.py /dev/ttyUSB0 --forever`.

### ModemManager check (manual)

1. Without the udev rule, with ModemManager running: plug in picolog, run
   `picolog_gen.py --forever`, and watch `journalctl -fu ModemManager` while
   it probes. Afterwards `picocom /dev/ttyACM1` must still give a correct
   replay. picolog discards everything the host sends, so AT probes can't
   corrupt anything.
2. With the rule installed and the device replugged:
   `udevadm info /dev/picolog-replay | grep ID_MM_DEVICE_IGNORE` shows `1`,
   and `mmcli -L` doesn't list picolog.

## Project layout

```
CMakeLists.txt, pico_sdk_import.cmake
src/main.c            superloop, init, markers
src/capture.c/.h      UART + DMA stage 1, error IRQ
src/history.c/.h      stage-2 ring, 64-bit offsets (hardware-independent)
src/usb_ports.c/.h    per-port cursor logic, DTR handling, replay state machine (hardware-independent)
src/usb_descriptors.c, src/tusb_config.h
udev/99-picolog.rules
test/unit/            C unit tests (host)
test/sim/             host simulator (ptys)
test/hw/              pattern generator + pytest integration tests (pyserial)
PLAN.md               original design plan (its watchdog and reset
                      persistence were deliberately left out)
```
