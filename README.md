# csk-flow

Agent-friendly flash + console for CSK6 radios (the C62). Links
`libcskburn_serial` from [LISTENAI/cskburn](https://github.com/LISTENAI/cskburn)
(Apache-2.0) for the burn protocol, then holds the serial port open and
streams the console itself — so the boot window is never missed and there is
no separate console tool to race against.

See `SPEC.md` for the full design. Working now: flash + chip-id +
console-with-`--until`, incremental skip (`--force` to override), `--json`
output, and `--no-console` (flash-only). Not yet done: the persistent daemon.

## The cable reality (tested 2026-11-07)

The C62's PL2303 cable does **not** wire DTR/RTS to BOOT/RESET — every
`cskburn` reset strategy failed to reset the radio (`autoburn-test.sh`). So:

- **Burn mode is manual:** hold BOOT and power the radio on when prompted.
- **The post-flash reboot is manual:** power-cycle to run the new firmware.

What csk-flow still buys you: it waits for the device, flashes, then opens the
console and is *already listening* when you power-cycle — the flash→observe
race is gone, and `--until` ends the capture the moment a line matches instead
of a fixed wait.

## Build

Needs a checkout of cskburn (submodules are not needed for the serial path):

```sh
git clone https://github.com/LISTENAI/cskburn.git ../cskburn-src
cmake -B build -DCSKBURN_DIR=../cskburn-src
cmake --build build
```

## Use

```sh
# C62: dsp_firmware at 0x100000, zephyr at 0x0, stop when the probe is done
build/csk-flow -s /dev/cu.PL2303G-USBtoUART2110 -C venus \
    --until 'WASMPROBE done|FATAL|fault' --timeout 60000 \
    0x100000 dsp_firmware.bin 0x0 zephyr.bin
```

It prints `>>>` prompts to stderr telling you when to enter burn mode and when
to power-cycle; console lines go to stdout with a `[   t.sss]` timestamp.
Exit 0 = `--until` matched (or no `--until`), 1 = flash failed or console
timed out.

Flags:

- `--force` — always write. Without it, each partition whose on-device MD5
  already matches the file is skipped (re-flashing an unchanged image drops
  from ~35 s to ~1 s). MD5 is `src/md5.c`, verified against system `md5`.
- `--json` — emit one JSON result object on stdout (chip id, per-partition
  action/bytes/ms, console line count and match, overall result); console
  lines are counted, not streamed. Human `>>>` prompts still go to stderr.
- `--no-console` — flash (or skip) and exit without the console phase.

```sh
# Fast idempotent re-flash with machine-readable result, no console:
build/csk-flow --json --no-console -s /dev/cu.PL2303G-USBtoUART2110 -C venus \
    0x100000 dsp_firmware.bin 0x0 zephyr.bin
```
