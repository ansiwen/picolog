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
- The Pico keeps power when the target crashes, so the history lives in RAM only.
  No flash logging.
- The history must survive a *Pico* soft/watchdog reset (see "Persistence").
- The user writes no code. Claude implements 100% of firmware, build, udev rule, README and tests.

## Decisions (already made)

| Topic | Decision |
|---|---|
| Hardware | Raspberry Pi Pico 2 (RP2350), Cortex-M33 cores |
| Language / SDK | C, pico-sdk (latest release), TinyUSB from the SDK, CMake |
| Input | UART0, RX on GP1 (TX on GP0 unused/optional), default 115200 8N1, compile-time configurable |
| USB | Composite device with **two CDC-ACM interfaces**, no custom host tool |
| Storage | RAM ring buffer, no flash |
| Concurrency | Single-core superloop on core0 (DMA does the realtime work). Core1 unused. |

## USB interface behavior

### Port 1: "live" (CDC interface 0, typically `/dev/ttyACM0`)
- On DTR rising edge: set this port's read cursor to the current write offset (show only new data).
- Stream new bytes as they arrive. Use non-blocking writes (`tud_cdc_n_write_available`).
- If the host reads too slowly and the cursor falls behind the oldest valid byte,
  jump to the oldest valid byte and emit `\r\n[picolog: N bytes dropped]\r\n`.
- On DTR low: stop sending.

### Port 2: "replay" (CDC interface 1, typically `/dev/ttyACM1`)
- On DTR rising edge: set the cursor to the **oldest valid offset**, send a header
  (`=== picolog replay: N bytes, uptime ..., resets since power-on ... ===`),
  stream the history, then an end marker (`=== picolog replay end, live follows ===`),
  then keep streaming live like port 1.
- Reading never consumes the buffer. Every reconnect replays the full history again.
- Ignore all bytes received from the host on this port (ModemManager sends AT probes).
- Usage: `picocom --logfile crash.log /dev/ttyACM1`. There is no EOF on a serial port;
  scripts use `timeout`.

### Descriptors
- 2x CDC via TinyUSB (`CFG_TUD_CDC 2`), custom `usb_descriptors.c`.
- Do **not** use `pico_stdio_usb` (it claims the CDC interface). If debug printf is needed,
  route stdio to UART1 or disable it.
- Interface strings: "picolog live", "picolog replay".
- VID/PID: for development use the pid.codes test pair `0x1209:0x0001`. Make it configurable in CMake.
- Unique serial number string from the flash unique ID (`pico_unique_board_id`).

## Data path

```
target TX ──► UART0 RX FIFO ──DMA (ring mode)──► stage-1 ring (32 KiB, aligned)
                                                       │  main loop copies
                                                       ▼
                                      history ring (256 KiB, __uninitialized_ram)
                                          ▲                     ▲
                               live cursor (port 1)   replay cursor (port 2)
```

### Stage 1: DMA capture
- One DMA channel: read `uart_get_hw(uart0)->dr` with 8-bit transfers, DREQ = UART0 RX,
  write address ring-wrapped (`channel_config_set_ring(write, 15)` → 32 KiB, buffer aligned to 32 KiB).
  The hardware ring wrap maxes out at 2^15 bytes, which is why stage 1 is small.
- Keep the channel running forever. **Verify:** RP2350 DMA `TRANS_COUNT` supports an
  ENDLESS mode (new vs RP2040). Use it if confirmed; otherwise re-arm/chain before the count runs out.
- Derive the DMA write position from the channel's `write_addr`.
- Slack at 115200 baud is about 2.8 s before stage 1 overwrites unread data, so a superloop is fine.
  Still, detect overrun (bytes produced since last copy > 32 KiB, via the transfer counter) and
  log a `[picolog: capture overrun]` marker.

### Stage 2: history ring
- 256 KiB (power of two, index masking), placed in `__uninitialized_ram` so crt0 doesn't zero it.
  The RP2350 has 520 KiB of SRAM.
- Monotonic **64-bit write offset** (`uint64_t head`). The oldest valid offset is `max(0, head - SIZE)`.
- Cursors are absolute 64-bit offsets. A cursor older than the oldest valid byte means data was dropped.
- Single producer and consumers all run in the same core0 loop, so no locking is needed.
  Document this assumption in the code.

### UART line errors
- DMA 8-bit reads of `UARTDR` discard error bits. Use the UART error interrupts
  (framing error, break, overrun: `UARTRIS`/`UARTIMSC`) to record events.
- In the IRQ, only set flags/counters. The main loop turns them into in-band markers
  (`[picolog: BREAK]`, `[picolog: framing error]`) at the current offset. A BREAK often means
  the target reset or crashed, so this is valuable.

### Persistence across Pico resets
- The metadata header in `__uninitialized_ram`: magic, version, `head`, buffer size, reset counter,
  CRC32 over the header.
- On boot: if magic + CRC are valid and the size matches, keep the history and continue appending.
  Otherwise initialize empty.
- On every boot append a marker: `\r\n[picolog: boot, reset reason <watchdog|por|...>]\r\n`
  (read the reset reason from the RP2350 `POWMAN`/`WATCHDOG` registers; use SDK helpers if available).
- **Verify on hardware:** that the RP2350 bootrom does not overwrite the chosen SRAM region on
  watchdog/soft reset. If it does, move the buffer to a region the bootrom leaves alone and document it.
- Enable the watchdog (e.g. 2 s) and feed it from the main loop.

## Hardware notes
- UART RX pin: enable the internal **pull-up** (UART idle is high). This avoids floating input when the
  target is disconnected. **Check RP2350 erratum E9** (GPIO input pull-down latching) and
  make sure no pull-down is left enabled on the RX pad.
- Target TX to Pico GP1, common GND. The target must be 3.3 V logic. Document a level shifter otherwise.

## Host side (no custom tool)
- `udev/99-picolog.rules`:
  - `ENV{ID_MM_DEVICE_IGNORE}="1"` for the VID/PID (stops ModemManager probing).
  - Stable symlinks by interface number: `/dev/picolog-live` (interface 00), `/dev/picolog-replay` (interface 02).
- README: wiring, build, flash (UF2 via BOOTSEL / picotool), usage with picocom/minicom/screen/PuTTY,
  note that the tool must assert DTR (all common ones do).

## Project layout
```
CMakeLists.txt
pico_sdk_import.cmake
src/main.c            superloop, init, watchdog
src/capture.c/.h      UART + DMA stage 1, error IRQ
src/history.c/.h      stage-2 ring, 64-bit offsets, persistence header + CRC
src/usb_ports.c/.h    per-port cursor logic, DTR handling, replay state machine
src/usb_descriptors.c
src/tusb_config.h
udev/99-picolog.rules
test/                 host-side tests (Python + pyserial)
README.md
```

## Testing plan
1. **Unit tests (host build):** compile `history.c` natively; test wraparound, 64-bit offsets,
   dropped-byte detection, header CRC validation. Make the ring logic hardware-independent for this.
2. **Integration (hardware):** a USB-serial adapter drives the target side with a pattern generator script
   (lines with sequence numbers + CRC). Check:
   - live port sees every line, no gaps at 115200 and at a high rate (e.g. 1 Mbaud stress test);
   - replay returns the full last 256 KiB, correctly ordered, header/end markers present;
   - no gap or duplicate between replay end and live continuation while data is flowing;
   - slow reader on the live port → dropped-bytes marker, no firmware stall;
   - BREAK sent from the adapter shows up as a marker;
   - `watchdog_reboot()` / soft reset → history preserved, boot marker appended;
   - power cycle → clean empty buffer;
   - both ports open at once work independently.
3. **ModemManager:** with and without the udev rule, confirm there's no corruption and replay still works.

## Working style for the implementing session
- Double-check hardware facts against the RP2350 datasheet / pico-sdk source before relying on them.
  Items marked **Verify** above are the ones I'm unsure about.
- Build with `-Wall -Wextra -Werror`.
- Small, reviewable commits: ring + unit tests first, then capture, then USB, then persistence.
