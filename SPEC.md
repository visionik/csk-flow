# csk-flow: an agent-friendly flasher for CSK6 radios

Draft spec. A small daemon **linking `libcskburn_serial`** (LISTENAI/cskburn,
Apache-2.0), aimed at unattended, scripted flash/observe loops like the C62
DSP bring-up. Not a wrapper over the `cskburn` binary, and not a fork of the
CLI: the burn protocol, reset-line control and a console reader are already a
reusable library, so we link it and add only the loop, the skip logic and the
output format.

## Why, not another cskburn

`cskburn` is fine for a human at a bench. It hurts in an agent loop:

1. **Manual burn-mode entry + a power-cycle every round.** Needs a person at
   the radio each iteration.
2. **Flash and console are separate tools with a gap.** Everything
   interesting prints in the ~1 s after reset; if the console reader is not
   already attached, the boot banner and probe lines are lost.
3. **Re-probe + full re-flash every time**, even when the image is unchanged
   (~10 s probe + ~33 s write on the C62).
4. **Human-only output.** No structured chip-id / per-partition verify /
   timing / distinct exit codes to branch on.
5. **Dual-core UART (chip-specific).** The DSP seizes the console UART once it
   starts, so M33 prints after that vanish.

## What the library already gives us

`libcskburn_serial` (public header `cskburn_serial.h`) already has the hard
parts:

| Need | Library call |
|---|---|
| open / connect / enter burn mode | `cskburn_serial_open` / `_connect` / `_enter` |
| write / read-back / verify / erase | `cskburn_serial_write` / `_read` / `_verify` / `_erase` |
| chip id (manifest key) | `cskburn_serial_read_chip_id` |
| reset into app, 4 strategies | `cskburn_serial_reset` + `cskburn_reset_strategy_t` |
| **console on the same open port** | **`cskburn_serial_read_logs(dev, baud)`** |
| raw line control | `libserial`: `serial_set_dtr` / `serial_set_rts` |

Two consequences:

- `read_logs` reads the console on the **same device handle** that did the
  flash, so flash -> reset-into-app -> stream happens in one process with no
  window where boot output is lost. This is the whole reason not to shell out
  to a binary.
- All four DTR/RTS -> BOOT/RESET mappings are implemented (via `TIOCMSET`).
  The *software* for unattended burn-mode entry exists today. The only open
  question is hardware: **does the C62's USB-serial cable physically wire
  DTR/RTS to BOOT/RESET?** If yes, auto-entry works now; if no, no software
  can create the wires. Confirm with the schematic or a scope before relying
  on it. This is 80% of the loop value.

## Shape: a daemon, not a one-shot

Hold the serial port open across operations so the 10 s probe is paid once.
Agent talks to it over a Unix socket (line-delimited JSON) or `--json` CLI
calls that attach to a running daemon.

```
csk-flow daemon   --serial <port> [--dsp-serial <port>] [--chip venus]
csk-flow flash    <addr> <file> [<addr> <file> ...]   # incremental
csk-flow reset    [--hold-console] [--until REGEX] [--timeout MS]
csk-flow console  [--duration N] [--until REGEX]
csk-flow erase    <addr>:<size>
csk-flow id | status
```

## The calls that matter

### flash+reset+observe, atomic
One process calls `_write` for each partition, `_reset` into the app, then
`_read_logs` on the same handle — **no window where boot output is lost**
(kills #2 and the timing half of #5). A `--until 'WASMPROBE done|FATAL|fault'`
regex filter over the `read_logs` stream returns the moment the line appears
instead of a fixed wait.

```
csk-flow flash 0x100000 dsp_firmware.bin 0x0 zephyr.bin \
    --reset --hold-console --until 'WASMPROBE done' --timeout 30000 --json
```

### incremental, content-addressed
Keep a manifest `{chip_id: {addr: sha256}}`. Read-verify each partition
(fast) and skip the write when it matches. Re-running the same image drops
from ~33 s to ~1 s (#3). `--force` overrides.

### structured output (#4)
`--json` emits:
```json
{"chip_id":"300EC7029D9132DB",
 "partitions":[{"addr":"0x100000","bytes":1048576,"written":true,"verified":true,"ms":33000},
               {"addr":"0x0","bytes":367484,"written":false,"skipped":"hash-match"}],
 "console":{"matched":"WASMPROBE done","lines":42},
 "exit":"ok"}
```
Distinct exit codes: `ok | device-not-found | verify-mismatch | timeout |
burn-mode-timeout`.

### auto burn-mode entry, honest fallback
Drive BOOT/RESET via DTR/RTS when the cable allows; otherwise emit one
structured `{"prompt":"hold BOOT, cycle power"}` and block (like `-w`) rather
than silently timing out. Unattended wherever the hardware permits.

### dual-UART console (#5)
If the DSP log pads (GPIOA15 RX / GPIOA18 TX on the dev kit) are reachable on
the C62, open both ports and interleave with `[m33]` / `[dsp]` tags and
timestamps. This is the clean fix for the UART-seizing problem — better than
relaying text frames through a WASM app.

## Build order

1. **Hardware check:** does the C62 cable wire DTR/RTS to BOOT/RESET? (scope
   the lines during a normal `cskburn` run, or read the schematic). Decides
   whether burn-mode entry is unattended or still needs a human.
2. **C daemon linking `libcskburn_serial`:** persistent port, one call doing
   flash -> reset -> `read_logs` with a `--until` filter, `--json` output.
   Reuses the lib's protocol/reset/console code; we write only the loop and
   the output. Build the lib via its CMake and link, rather than vendoring.
3. **Incremental flashing:** manifest keyed on `read_chip_id`, skip a
   partition when `_verify` (or a read-back hash) matches. Trivial on top of
   the lib.
4. **Dual-UART console** if the DSP log pads are reachable on the C62.

Step 1 decides the reset path. Steps 2-3 cover the day-to-day loop. If the
C62 needs a reset strategy the lib lacks, add it to `cskburn_reset_strategy_t`
and send it upstream rather than forking.
