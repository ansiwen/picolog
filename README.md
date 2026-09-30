# picolog

A Raspberry Pi **Pico 2** (RP2350) between a target device's diagnostic UART and a host.

- **Live console:** watch the target's serial output in real time from any terminal program.
- **Flight recorder:** the Pico keeps recording into a 256 KiB RAM ring buffer while no host is
  connected. After the target crashes, plug in a laptop and read the last ~256 KiB.

It shows up as a USB device with **two serial ports** and needs no host software:

| Port | Typical name | Behaviour |
|---|---|---|
| live | `/dev/ttyACM0`, `/dev/picolog-live` | new data only, from the moment you open it |
| replay | `/dev/ttyACM1`, `/dev/picolog-replay` | the whole history first, then continues live |

The history lives in RAM only. There is no flash logging, no watchdog and no persistence across
resets (see [Limitations](#limitations)).

## Wiring

| Pico 2 | Connect to |
|---|---|
| GP1 (pin 2), UART0 RX | target's TX |
| GND (e.g. pin 3 or 38) | target's GND |
| GP0 (pin 1), UART0 TX | **left unconfigured**; the Pico never drives the target |

The target must use **3.3 V logic**. 5 V TTL needs a level shifter or divider; real RS-232 needs a
transceiver such as a MAX3232.

The RX pin has its internal pull-up enabled, so the input idles high and never floats when the
target is unplugged.

### Power

The Pico is meant to run from **its own supply**, never only through its USB connector, because USB
gets plugged, unplugged and moved between hosts while the Pico keeps recording.

- Feed a fixed 2.3-5.5 V supply into **VSYS (pin 39)** through a **Schottky diode**, ground to GND
  (e.g. pin 38). This is the "power ORing" recommended in the Pico 2 datasheet: the on-board diode
  D1 (VBUS to VSYS) plus your external diode let whichever supply is higher power the board, and
  neither can back-power the other.
- Do not connect the supply to VBUS (pin 40) or 3V3.
- The datasheet also shows a P-FET variant that switches the external supply off while VBUS is
  present. The plain diode is preferred here because the external supply simply stays connected.

## Build

Requires the Arm GNU toolchain, CMake, and [pico-sdk](https://github.com/raspberrypi/pico-sdk)
2.3.1 with its TinyUSB submodule (`git submodule update --init lib/tinyusb`).

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -S . -B build -G Ninja          # or omit -G Ninja
cmake --build build
```

This produces `build/picolog.uf2` (board `pico2`, platform `rp2350-arm-s`). Options, set with
`-DNAME=value`:

| Option | Default | Meaning |
|---|---|---|
| `PICOLOG_UART_BAUD` | `115200` | baud rate of the captured UART |
| `PICOLOG_UART_RX_PIN` | `1` | GPIO used as UART0 RX |
| `PICOLOG_UART_DATA_BITS` | `8` | 5-8 |
| `PICOLOG_UART_STOP_BITS` | `1` | 1 or 2 |
| `PICOLOG_UART_PARITY` | `NONE` | `NONE`, `EVEN` or `ODD` |
| `PICOLOG_USB_VID` | `0x1209` | USB vendor ID (pid.codes) |
| `PICOLOG_USB_PID` | `0x0001` | USB product ID (pid.codes test PID) |

If you change VID/PID, change them in `udev/99-picolog.rules` too.

Own sources are built with `-Wall -Wextra -Werror`; SDK sources are not.

## Flash

Hold BOOTSEL while plugging the Pico into USB, then either copy `build/picolog.uf2` to the
`RP2350` drive that appears, or:

```sh
picotool load -x build/picolog.elf
```

The firmware does not offer picotool's reset interface, so to re-flash you need BOOTSEL again.

## Host setup

```sh
sudo cp udev/99-picolog.rules /etc/udev/rules.d/
sudo udevadm control --reload
# unplug and replug the Pico
```

The rule tells ModemManager to leave the device alone (`ID_MM_DEVICE_IGNORE=1`) and creates stable
names by interface number: `/dev/picolog-live`, `/dev/picolog-replay`, plus
`/dev/picolog/<serial>-live` and `-replay` if you have several recorders. Without the rule
everything still works using the `ttyACM` numbers, which depend on plug order.

## Usage

The terminal program **must assert DTR** (all common ones do). That is how the firmware knows a
session has started; without DTR nothing is sent.

```sh
# live console
picocom /dev/picolog-live

# retrieve the history, keep following live output, save everything to a file
picocom --logfile crash.log /dev/picolog-replay

# other tools
minicom -D /dev/picolog-replay
screen /dev/picolog-replay 115200
# PuTTY: Serial, /dev/picolog-replay (or COMx on Windows)
```

The baud rate you give the terminal program is irrelevant (it is USB).

A serial port has no end-of-file, so scripts need a timeout:

```sh
stty -F /dev/picolog-replay raw -echo
timeout 5 cat /dev/picolog-replay > crash.log
```

Example replay output:

```
=== picolog replay: 262144 bytes, uptime 3d 04:17:52 ===
...the last 256 KiB the target printed...
=== picolog replay end, live follows ===
...new output continues here...
```

`uptime` is the time since the Pico booted. The header appears only when the port is opened; the
replay is not consumed, so every reconnect replays the full history again.

### Markers

Things the Pico notices are written **into the history** as their own lines so they show up in
replays and live output:

| Marker | Meaning |
|---|---|
| `[picolog: BREAK]` | a BREAK condition on the line (typically the target rebooting or the cable being pulled) |
| `[picolog: framing error]` | framing error, usually wrong baud rate or data format |
| `[picolog: parity error]` | parity error |
| `[picolog: UART FIFO overrun]` | the UART hardware FIFO overflowed |
| `[picolog: capture overrun, N bytes lost]` | the DMA stage-1 buffer overflowed |

Repeated events are aggregated (`[picolog: framing error x37]`) and emitted at most every 100 ms,
so a wrong baud rate cannot flood the history. Markers are appended after the data of the same
moment, so their position is approximate (they can even land in the middle of a line).

A different marker is sent to **one port only** and is never stored:

| Marker | Meaning |
|---|---|
| `[picolog: N bytes dropped]` | this port's reader was too slow and the ring overwrote N bytes it had not been sent yet; sending continues at the oldest byte still available |

## How it works

```
target TX -> UART0 RX FIFO --DMA (ring mode)--> stage-1 ring (32 KiB, aligned)
                                                     | main loop copies
                                                     v
                                        history ring (256 KiB)
                                        ^                     ^
                             live cursor (port 1)   replay cursor (port 2)
```

- **Stage 1:** one DMA channel moves every received byte from `UARTDR` into a 32 KiB ring, with
  no CPU per byte and no interrupts for data. The main loop (single core, never blocks) copies
  what arrived into the history.
- **History:** 256 KiB ring with a monotonic 64-bit write offset. Each port has a cursor (an
  absolute offset). A cursor older than the oldest valid byte means data was dropped.
- **Line errors:** DMA reads of `UARTDR` discard the error bits, so only the UART's error
  interrupts are enabled (RX and receive-timeout stay masked) and they merely count events.
- **Replay port:** after DTR rises, wait 100 ms; then take a snapshot (oldest offset, end offset),
  send the header and the history, the end marker, and continue live exactly at the snapshot end.

### Verified hardware facts

Checked against the RP2350 datasheet and the pico-sdk / TinyUSB sources:

- **`TRIGGER_SELF`, not `ENDLESS`.** `TRANS_COUNT.MODE = 0xF` (endless) exists but its counter does
  not decrement, so received bytes could not be counted. `TRIGGER_SELF` re-triggers the channel
  forever, continues at its current write address, and keeps a live down-counter. The count is
  2^27; bytes produced between two polls are `(prev - now) & (2^27 - 1)`, unambiguous because
  the loop never blocks (2^27 bytes is over 90 s even at 12 Mbaud).
- **`UARTDMACR.DMAONERR` is cleared.** If set, the UART masks the RX DMA request while an error
  interrupt is pending and capture would stall on the first framing error.
- **RP2350-E9** (A2 stepping): leakage holds a floating input near 2.2 V and defeats a pull-*down*.
  The pull-up used here still works. Fixed in stepping A3.
- **`FORCE_VBUS_DETECT`** is set in TinyUSB's RP2040/RP2350 port, so unplugging USB looks like a
  bus *suspend* to the firmware, not a disconnect (next section).

### USB unplug and suspend behaviour

A port counts as connected when the device is configured by a host **and DTR is set**. USB suspend
is deliberately ignored (`tud_cdc_n_connected()` is not used, because it includes `!suspended`):

- an unplug looks like a suspend (see above), and a host that sleeps with a terminal open suspends
  the bus too. Keeping the session across that means a resumed terminal simply continues, with a
  `dropped` marker if the ring wrapped meanwhile, instead of getting a second replay;
- a real replug starts a new session, because the host's bus reset clears the configuration and
  DTR;
- TX data is flushed on every loop, because while suspended TinyUSB does not transmit, the FIFO
  fills up, and without an unconditional flush the queued bytes would never go out after resume.

Data recorded while USB was unplugged is in the replay after replugging.

Other details worth knowing: nothing the host sends is ever used (ModemManager's AT probes are
discarded), the TX FIFO is cleared when a port opens, and the 100 ms open delay exists because
pyserial and other programs flush the tty input right after `open()`, which would otherwise eat the
replay header. The replay snapshot is taken after that delay; taking it at the DTR edge would make
every replay of a full ring under load start with a "dropped" marker.

## Limitations

- **Any reset or power loss clears the history.** It lives in ordinary RAM. There is deliberately no
  persistence across resets, and no watchdog: if the firmware ever hangs it stays hung until
  someone resets it.
- Marker placement is approximate.
- A reader that is too slow (or a suspended host for long enough) gets `dropped` markers; the ring
  keeps 256 KiB, no more.
- BREAK, framing errors etc. are counted per interrupt, not per byte.

## Testing

**Status:** the firmware builds warning-free, and the ring and USB port logic are covered by the
host tests below (including the simulator run of the integration suite). The DMA capture path, the
TinyUSB glue and the USB unplug/suspend behaviour can only be exercised on a real Pico 2, so run
the hardware integration tests there before relying on it.

```sh
make -C test/unit check        # C unit tests (host, ASan + UBSan)
```

- `test_history`: ring semantics, wraparound, 64-bit offsets across 2^32 and 2^40, `appendf`.
- `test_capture`: the DMA counter arithmetic.
- `test_usb_ports`: cursor and replay logic against a fake USB layer: seam without gap or
  duplicate, reconnect replays again, exact dropped counts, drops during replay, independent ports,
  flush without host reads.

**Host simulator** (runs the real `history.c` and `usb_ports.c` on pseudo-terminals; does not model
DMA or BREAK; UART input is paced to the baud rate):

```sh
make -C test/sim
test/sim/picolog_sim /tmp/pl 1000000 65536 &     # DIR [baud] [history_size]
pip install -r test/hw/requirements.txt
cd test/hw
PICOLOG_SIM=1 PICOLOG_ADAPTER=/tmp/pl/uart PICOLOG_LIVE=/tmp/pl/live \
PICOLOG_REPLAY=/tmp/pl/replay PICOLOG_BAUD=1000000 PICOLOG_HISTORY=65536 pytest
```

**Hardware integration** (`test/hw`, pytest + pyserial): a USB-serial adapter wired to GP1 acts as
the target and sends pattern lines `PL<seq> <40 chars> <crc32>`; the tests check them on both ports
for gaps, duplicates and corruption. `picolog_gen.py` is also a CLI (`--count`, `--forever`,
`--rate`, `--break`).

| Variable | Meaning |
|---|---|
| `PICOLOG_ADAPTER` | adapter port; **required**, otherwise everything is skipped |
| `PICOLOG_LIVE`, `PICOLOG_REPLAY` | default `/dev/picolog-live`, `/dev/picolog-replay` |
| `PICOLOG_BAUD` | must match the firmware build (default 115200); the 1 Mbaud stress test needs 1000000 |
| `PICOLOG_HISTORY` | history size in bytes (default 262144) |
| `PICOLOG_SIM` | `1` when using the simulator |
| `PICOLOG_UNPLUG_CMD`, `PICOLOG_PLUG_CMD` | commands to cut/restore USB, e.g. via `uhubctl` |
| `PICOLOG_INTERACTIVE` | `1`: ask a human to unplug/replug (run `pytest -s`) |

The "data sent while USB is unplugged is in the replay" test only runs when unplug commands (or
`PICOLOG_INTERACTIVE`) are given.

**ModemManager (manual check).** With and without the udev rule, confirm there is no corruption and
replay still works. With the rule, `udevadm info /dev/picolog-live | grep ID_MM_DEVICE_IGNORE`
should show `1` and `mmcli -L` should not list the device.

## Project layout

```
CMakeLists.txt, pico_sdk_import.cmake
src/main.c            superloop, init, TinyUSB glue, markers
src/capture.c/.h      UART + DMA stage 1, error IRQ
src/history.c/.h      stage-2 ring, 64-bit offsets (hardware independent)
src/usb_ports.c/.h    per-port cursor logic, DTR handling, replay state machine (hardware independent)
src/usb_descriptors.c
src/tusb_config.h
udev/99-picolog.rules
test/unit/            C unit tests (host)
test/sim/             host simulator on ptys
test/hw/              pattern generator + pytest tests
PLAN.md               the design decisions this implementation follows
```
