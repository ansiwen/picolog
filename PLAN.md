# picolog — UART flight recorder + live console on Raspberry Pi Pico 2 (RP2350)

## Goal

A Pico 2 sits between a target device's diagnostic UART output and a host.
It must:

1. **Live console:** let a host terminal watch the target's serial output in real time.
2. **Flight recorder:** keep recording into a RAM ring buffer when no host is connected,
   so that after a target crash the history can be retrieved for diagnosis.

Context: the target is an appliance in a data center. The Pico is either
permanently attached to a terminal hub (live logging), or someone plugs in a
laptop on site to fetch the last logs.

Constraints:
- The Pico is **always powered from a separate supply** (VSYS via a Schottky diode),
  never only through its USB connector. USB is plugged, unplugged and moved between
  hosts while the Pico keeps running and recording.
- The history lives in RAM only. No flash logging.
- The user writes no code. Claude implements 100% of firmware, build, udev rule, README and tests.

## Decisions (already made)

| Topic | Decision |
|---|---|
| Hardware | Raspberry Pi Pico 2 (RP2350), Cortex-M33 cores, board `pico2` (platform `rp2350-arm-s`) |
| Language / SDK | C11, pico-sdk 2.3.1, TinyUSB from the SDK, CMake |
| Input | UART0, RX on GP1 (GP0/TX left unconfigured, never drives the target), default 115200 8N1, compile-time configurable |
| USB | Composite device with **two CDC-ACM interfaces**, no custom host tool |
| Storage | 256 KiB RAM ring buffer in ordinary `.bss`, starts empty on every boot |
| Concurrency | Single-core superloop on core0 (DMA does the realtime work). Core1 unused (except the optional `PICOLOG_SELFTEST_TX` test source, compiled out by default). |
| Power | Separate supply into VSYS through a Schottky diode (see "Power") |

### Explicitly out of scope (considered and removed)

Do **not** implement these; they were built once and deliberately removed:
- **Watchdog.** If the firmware hangs it stays hung until someone resets it.
- **History persistence across Pico resets** (`__uninitialized_ram`, persistent header,
  CRC, reset counter). With a separate supply, Pico resets are rare, and without
  a watchdog there is no automatic reset that would wipe the history.
- **Boot marker / reset-reason detection** (POWMAN `CHIP_RESET`, watchdog scratch).
- **Test hooks** for triggering resets from the host.
- "resets since power-on" in the replay header.

## USB interface behavior

### Port 1: "live" (CDC interface 0, typically `/dev/ttyACM0`)
- On DTR rising edge: set this port's read cursor to the current write offset (show only new data).
- Stream new bytes as they arrive. Use non-blocking writes (`tud_cdc_n_write_available`).
- If the host reads too slowly and the cursor falls behind the oldest valid byte,
  jump to the oldest valid byte and emit `\r\n[picolog: N bytes dropped]\r\n`
  (to this port only, not into the history).
- On DTR low: stop sending.

### Port 2: "replay" (CDC interface 1, typically `/dev/ttyACM1`)
- On DTR rising edge: send a header `=== picolog replay: N bytes, uptime Xd HH:MM:SS ===\r\n`
  (uptime = Pico time since boot), stream the history from the **oldest valid offset**,
  then `\r\n=== picolog replay end, live follows ===\r\n`, then keep streaming live like
  port 1, continuing exactly at the replay end offset (no gap, no duplicate).
- Reading never consumes the buffer. Every reconnect replays the full history again.
- Drops during the replay are handled like on the live port (marker, jump to oldest).
- Usage: `picocom --logfile crash.log /dev/ttyACM1`. There is no EOF on a serial port;
  scripts use `timeout`.

### Both ports
- Ignore all bytes received from the host (ModemManager sends AT probes): read-flush
  the RX FIFO on every loop iteration.
- On open (DTR rise): clear the TX FIFO (`tud_cdc_n_write_clear`) to drop stale data.
- **Open delay:** after the DTR rising edge wait **100 ms** before sending anything.
  Reason (found with the simulator): pyserial and other programs flush the tty input
  right after `open()`, which otherwise discards the replay header. The live cursor is
  still set at the DTR edge, so live data from the delay period is delivered.
- **Replay snapshot is taken when sending starts** (after the delay), not at the DTR
  edge: oldest offset, end offset and header are computed then, and the header plus the
  first history bytes are queued into the TX FIFO in the same call. Reason: with a full
  ring and data flowing, a snapshot taken at the DTR edge gets its oldest bytes
  overwritten during the delay, so every replay under load would start with a
  "dropped" marker. Implement as an extra state `REPLAY_PENDING`
  (states: CLOSED, REPLAY_PENDING, REPLAY, LIVE).
- **"Connected" = device configured by a host AND DTR set**
  (`tud_mounted() && (tud_cdc_n_get_line_state(itf) & 1)`), deliberately *ignoring*
  USB suspend (do not use `tud_cdc_n_connected()`, which includes `!suspended`).
  Reasons: the RP2350 TinyUSB port forces VBUS detect on (`FORCE_VBUS_DETECT` in
  `dcd_rp2040.c`), so an unplug looks like a suspend; a host sleeping with a terminal
  open also suspends. Keeping the session over a suspend means a resumed terminal just
  continues (with a dropped marker if needed) instead of receiving a second replay.
  A real replug still starts a new session because the host's bus reset clears the
  configuration and DTR (`usbd_reset` / `cdcd_reset`).
- **Call `tud_cdc_n_write_flush` on every task call**, not only after a write: while
  suspended TinyUSB does not transmit, the FIFO fills, nothing new can be written, and
  without an unconditional flush the queued data would never be sent after resume.

### Descriptors
- 2x CDC via TinyUSB (`CFG_TUD_CDC 2`), custom `usb_descriptors.c`, with IAD
  (device class MISC / common / IAD), `bcdUSB 0x0200`, bus powered 100 mA.
- Interfaces 0/1 = live, 2/3 = replay. Endpoints: live notif 0x81, out 0x02, in 0x82;
  replay notif 0x83, out 0x04, in 0x84; 64-byte bulk, 8-byte notification.
- Do **not** use `pico_stdio_usb` (it claims the CDC interface) and no UART stdio
  (UART0 is the capture input): `pico_enable_stdio_usb/uart(picolog 0)`.
- Strings: manufacturer "picolog", product "picolog UART recorder",
  interface strings "picolog live", "picolog replay".
- VID/PID: pid.codes test pair `0x1209:0x0001`, configurable in CMake.
- Serial number string from the flash unique ID (`pico_get_unique_board_id_string`).
- `tusb_config.h`: full speed, `CFG_TUD_CDC_TX_BUFSIZE 1024`, RX 64, EP 64.

## Data path

```
target TX ──► UART0 RX FIFO ──DMA (ring mode)──► stage-1 ring (32 KiB, aligned)
                                                       │  main loop copies
                                                       ▼
                                          history ring (256 KiB)
                                          ▲                     ▲
                               live cursor (port 1)   replay cursor (port 2)
```

### Stage 1: DMA capture (`capture.c/.h`)
- UART0: `uart_init`, `uart_set_format` from the CMake options, no flow control, FIFO on.
  Explicitly clear `UARTDMACR.DMAONERR` (if set, the UART masks the RX DMA request while
  an error interrupt is pending and capture stalls on the first framing error) and set `RXDMAE`.
- RX pin: `gpio_set_function(pin, UART_FUNCSEL_NUM(uart0, pin))` and `gpio_pull_up`
  (UART idle is high; also mitigates RP2350-E9, see Hardware notes).
- One DMA channel: read `uart_get_hw(uart0)->dr`, 8-bit, no read increment, write
  increment, `channel_config_set_ring(write, 15)` into a `static uint8_t stage1[32768]`
  aligned to 32 KiB (in `.bss`), DREQ = UART0 RX, high priority, no DMA IRQ.
- **Transfer count mode: TRIGGER_SELF, not ENDLESS** (verified in the RP2350 register
  docs): ENDLESS exists (`TRANS_COUNT.MODE = 0xF`) but its counter does not decrement,
  so received bytes could not be counted. TRIGGER_SELF (`dma_encode_transfer_count_with_self_trigger`)
  re-triggers the channel forever, continuing at its current write address, and keeps a
  live count. Count = 2^27. Bytes produced between two polls =
  `(prev_remaining − now_remaining) & (2^27 − 1)` (read `transfer_count` masked with
  `DMA_CH0_TRANS_COUNT_COUNT_BITS`). Unambiguous because the superloop never blocks
  (2^27 bytes is > 90 s even at 12 Mbaud). Keep this as a pure inline helper `capture_produced()`.
- Overrun: if more than `32 KiB − 1024` bytes are pending, skip the oldest excess
  (so the DMA cannot overwrite what is being copied) and report the skipped count.
- `capture_poll(sink, ctx, events)` hands new bytes to a sink in up to two spans and
  accumulates events.

### Stage 2: history ring (`history.c/.h`, hardware-independent)
- 256 KiB (power of two, index masking) in plain RAM, initialized empty.
- Monotonic **64-bit write offset** (`uint64_t head`). Oldest valid offset = `max(0, head − SIZE)`.
- Cursors are absolute 64-bit offsets. A cursor older than the oldest valid byte means data was dropped.
- API: `history_init`, `history_append` (if `len > size` keep only the last `size`
  bytes), `history_appendf` (printf-style, truncated to 255 bytes, used for markers),
  `history_head`, `history_oldest`, `history_clamp(&cursor)` → dropped count,
  `history_peek(cursor, &ptr)` → contiguous readable bytes (0 for stale cursors).
- Single producer and consumers all run in the same core0 loop, so no locking is needed.
  Document this assumption in the code.

### UART line errors and markers
- DMA 8-bit reads of `UARTDR` discard error bits. Enable only the UART error interrupts
  (`UARTIMSC` FE, PE, BE, OE; RX/RT stay masked). In the IRQ read `UARTMIS`, clear via
  `UARTICR`, and only count: BE → break (and then don't also count FE, since a break
  arrives as a framing error on a 0x00 char), else FE → framing; PE → parity; OE → UART
  FIFO overrun. Counters are read-and-cleared in the main loop with `__atomic_exchange_n`.
- The main loop copies data first, then turns events into in-band markers in the history:
  `\r\n[picolog: BREAK]\r\n`, `[picolog: framing error]`, `[picolog: parity error]`,
  `[picolog: UART FIFO overrun]`, `[picolog: capture overrun, N bytes lost]`; repeated
  events aggregate as `[picolog: <what> xN]`. Rate-limit: at most one emission per
  100 ms (keep accumulating in between), so a wrong baud rate cannot flood the history.

### Main loop (`main.c`)
`history_init` → `capture_init` → `usb_port_init` ×2 → `tud_init(BOARD_TUD_RHPORT)`, then forever:
`tud_task()`, `capture_poll(→ history_append)`, emit markers, `usb_port_task(live)`,
`usb_port_task(replay)`, passing `time_us_64()` as uptime. The TinyUSB glue
(connected/write_available/write/flush/clear_tx/discard_rx) is an ops struct in
`main.c`; `usb_ports.c` only sees the ops, so it compiles and is tested on the host.

## Power

- Feed a fixed ~2.3–5.5 V supply into **VSYS (pin 39)** through a **Schottky diode**, ground
  to GND (e.g. pin 38). This is the Pico 2 datasheet's recommended "power ORing": the
  on-board D1 (VBUS→VSYS) plus the external diode let the higher supply power the board and
  prevent either from back-powering the other. Don't connect the supply to VBUS (pin 40) or 3V3.
- The datasheet's P-FET variant switches the external supply off while VBUS is present;
  the plain-diode variant is preferred here because the external supply stays connected.

## Hardware notes
- UART RX pin: internal **pull-up** enabled (idle high, no floating input when the target is
  disconnected). **RP2350-E9** (A2 stepping): leakage holds a floating input near 2.2 V and
  defeats the pull-*down*; the pull-up still works and removes the condition. `gpio_pull_up`
  disables the pull-down. Fixed in stepping A3.
- Target TX to Pico GP1 (pin 2), common GND. The target must be 3.3 V logic; 5 V TTL needs a
  level shifter/divider, real RS-232 needs a transceiver (e.g. MAX3232).

## Host side (no custom tool)
- `udev/99-picolog.rules` (must pass `udevadm verify`):
  - `ENV{ID_MM_DEVICE_IGNORE}="1"` for the VID/PID on the USB device, its interfaces and ttys.
  - Stable symlinks by interface number: `/dev/picolog-live` (interface 00),
    `/dev/picolog-replay` (interface 02), plus `/dev/picolog/<serial>-live|replay`.
  - Use `IMPORT{builtin}="usb_id"` and `ENV{ID_USB_INTERFACE_NUM}`. Use **only positive**
    `ATTRS` matches: a negative `ATTRS{idVendor}!=` can match any parent (e.g. the root hub)
    and misfire. Multiple `ATTRS` in one rule must match on the same parent device.
- README: overview, wiring, **Power** section, build (options table), flash (UF2 via
  BOOTSEL / `picotool load -x`; no picotool reset interface), host setup, usage with
  picocom/minicom/screen/PuTTY/`stty raw` + `timeout cat`, note that the tool must assert
  DTR, example replay output, marker tables (recorded markers vs. the per-port dropped
  marker), how it works, notes on verified hardware facts (TRIGGER_SELF vs ENDLESS,
  DMAONERR, E9), USB unplug/suspend behaviour, limitations (any reset or power loss clears
  the history, no watchdog, marker placement approximate, slow readers get drops),
  testing, ModemManager manual check, project layout.

## Build
- `CMakeLists.txt` with `PICO_BOARD pico2`, `pico_sdk_import.cmake` copied from the SDK.
- Cache options: `PICOLOG_USB_VID`, `PICOLOG_USB_PID`, `PICOLOG_UART_BAUD`,
  `PICOLOG_UART_RX_PIN`, `PICOLOG_UART_DATA_BITS`, `PICOLOG_UART_STOP_BITS`,
  `PICOLOG_UART_PARITY`, passed as compile definitions.
- `-Wall -Wextra -Werror` **only on our own sources** (`set_source_files_properties`);
  SDK sources are compiled into the same target and must not get `-Werror`.
- Link `pico_stdlib hardware_dma hardware_uart hardware_irq pico_unique_id tinyusb_device`;
  `pico_add_extra_outputs`.
- `.gitignore`: `build/`, `build-*/`, test binaries, `test/sim/picolog_sim`,
  `__pycache__/`, `*.pyc`, `.pytest_cache/`.

## Project layout
```
CMakeLists.txt
pico_sdk_import.cmake
src/main.c            superloop, init, TinyUSB glue, markers
src/capture.c/.h      UART + DMA stage 1, error IRQ
src/history.c/.h      stage-2 ring, 64-bit offsets (hardware-independent)
src/usb_ports.c/.h    per-port cursor logic, DTR handling, replay state machine (hardware-independent)
src/usb_descriptors.c
src/tusb_config.h
udev/99-picolog.rules
test/unit/            C unit tests (host, ASan/UBSan), Makefile
test/sim/             host simulator on ptys, Makefile
test/hw/              pattern generator + pytest tests (Python + pyserial)
README.md
PLAN.md
```

## Testing plan
1. **Unit tests** (`make -C test/unit`, `-Wall -Wextra -Werror -fsanitize=address,undefined`):
   - `test_history.c`: init, simple append, wraparound, append larger than the ring,
     dropped-byte detection, 64-bit offsets across 2^32 and at 2^40 (set `head` directly),
     `appendf` formatting and truncation.
   - `test_usb_ports.c` with a fake ops layer (per-port connected flag, bytes accepted per
     call, output capture; fake time advances by the open delay per task call): live shows
     only new data and stops on DTR low; replay→live seam has no gap/duplicate while data
     arrives during the open delay and during the replay; every reconnect replays again;
     empty history; slow live reader gets the exact dropped count; drop during replay;
     both ports independent; flush is called even while the host accepts nothing.
   - Check new tests by mutating the code and confirming they fail.
2. **Host simulator** (`test/sim/picolog_sim DIR [baud] [history_size]`): runs the real
   `history.c` + `usb_ports.c` with ptys `DIR/uart`, `DIR/live`, `DIR/replay`. A port is
   "DTR asserted" while its pty slave is open (no `POLLHUP` on the master). UART input is
   paced to the baud rate (10 bits/byte), otherwise tests overflow the ring unrealistically.
   Does not model DMA or BREAK. Run the hw suite against it with `PICOLOG_SIM=1`.
3. **Hardware integration** (`test/hw`, pytest + pyserial): a USB-serial adapter drives
   the target side with pattern lines `PL<seq:08d> <40 payload chars> <crc32:08x>\r\n`.
   - `pattern.py`: line generator, parser/analyzer (strict mode; lenient mode allows markers,
     a marker splitting one line, gaps after dropped/overrun markers), `split_replay`.
   - `picolog_gen.py`: generator class + CLI (`--count`, `--forever`, `--rate`, `--break`).
   - Env: `PICOLOG_ADAPTER` (required, else all skip), `PICOLOG_LIVE`, `PICOLOG_REPLAY`,
     `PICOLOG_BAUD`, `PICOLOG_HISTORY`, `PICOLOG_SIM`, `PICOLOG_UNPLUG_CMD`/`PICOLOG_PLUG_CMD`,
     `PICOLOG_INTERACTIVE`.
   - Each test uses a random start sequence number and cuts the stream at its first line.
     Never assume bytes reached the device when `write()`/`flush()` return: wait until the
     last line is seen on the live port (`send_recorded`).
   - Tests: live sees every line (no gaps; 1 Mbaud stress with a matching firmware build);
     replay returns exactly 256 KiB in order with header/end markers; replay twice is
     identical; no gap/duplicate across the replay/live seam while streaming; both ports at
     once; slow live reader → dropped marker, firmware keeps answering; BREAK → marker
     (skipped in the sim); **data sent while USB is unplugged is in the replay after
     replugging** (unplug via e.g. `uhubctl` commands, or by hand with `PICOLOG_INTERACTIVE=1`).
4. **ModemManager (manual):** with and without the udev rule, confirm there's no corruption
   and replay still works; with the rule, `ID_MM_DEVICE_IGNORE=1` and `mmcli -L` doesn't list it.

## Working style for the implementing session
- Double-check hardware facts against the RP2350 datasheet, the Pico 2 datasheet and the
  pico-sdk/TinyUSB source before relying on them.
- Build with `-Wall -Wextra -Werror` (own sources).
- Small, reviewable commits: ring + unit tests first, then capture, then USB, then
  tooling (udev, tests, simulator) and README.
