# csk-flow

**Flashing a CSK6 radio and catching its boot log is two tools and a stopwatch.**
You run `cskburn` to write the image, then scramble to attach a separate serial
monitor before the ~1 second of boot output scrolls past — and if you miss the
window, you reflash and try again. Re-flashing an unchanged image still costs
the full ~35 seconds. Nothing is machine-readable, so a script can't tell a
"device not found" from a "verify failed."

csk-flow is **one command** that fixes all of that:

- **No race.** It flashes, then holds the same serial port open and is already
  listening when you power-cycle — the boot log can't be missed.
- **Stops when it's seen enough.** `--until '<regex>'` ends the capture the
  instant a matching line appears, instead of waiting out a fixed timeout.
- **Skips unchanged writes.** It compares each file's MD5 to the device's and
  skips partitions that already match — a reflash of the same image drops from
  ~35 s to ~1 s.
- **Machine-readable.** `--json` emits one structured result an agent can parse.

It does this by linking `libcskburn_serial` from
[LISTENAI/cskburn](https://github.com/LISTENAI/cskburn) (Apache-2.0) for the
burn protocol — not by wrapping the `cskburn` binary — so flash and console
live in one process. See `SPEC.md` for the design.

## The cable reality

The hardware this was built for (a Retevis C62 on a PL2303 cable) does **not**
wire DTR/RTS to BOOT/RESET — every `cskburn` reset strategy fails to reset the
chip (see `autoburn-test.sh`). Two steps therefore stay manual and **no tool
can automate them on this cable**:

- **Entering burn mode:** hold the BOOT button and power the radio on.
- **The post-flash reboot:** power-cycle to run the new firmware.

csk-flow orchestrates around those two steps; it cannot remove them. If your
cable *does* wire the reset lines, burn-mode entry becomes automatic — but
that's a hardware fact to verify, not something software can supply.

---

## For humans

### Install

You need `cmake`, a C compiler, and a checkout of cskburn (its submodules are
**not** needed — only the serial path is used):

```sh
git clone https://github.com/visionik/csk-flow.git
cd csk-flow
git clone https://github.com/LISTENAI/cskburn.git ../cskburn-src
cmake -B build -DCSKBURN_DIR=../cskburn-src
cmake --build build
```

This produces `build/csk-flow` (CLI) and `build/csk-flowd` (daemon).

### Use

Flash the two partitions and watch the boot log, stopping when it appears:

```sh
build/csk-flow -s /dev/cu.PL2303G-USBtoUART2110 -C venus \
    --until 'Booting Zephyr OS' --timeout 90000 \
    0x100000 dsp_firmware.bin 0x0 zephyr.bin
```

It talks to you through `>>>` prompts on stderr:

1. `>>> Put the radio in BURN MODE now (hold BOOT, power on).`
   → hold BOOT and switch the radio on; release BOOT once it prints `chip-id:`.
2. It reads the chip id and writes (or skips) each partition.
3. `>>> Listening ... POWER-CYCLE the radio now.`
   → turn the radio off, then on (normally, no button).
4. Console lines stream with a `[   t.sss]` timestamp; it exits the moment a
   line matches `--until`.

Find your serial port with `ls /dev/cu.*` (macOS) or `ls /dev/ttyUSB*` (Linux).

---

## For agents

csk-flow is designed to be driven by a program: deterministic flags, a JSON
result, and distinct exit codes. **But read "The cable reality" above** — on a
cable without reset lines, a human must still press BOOT+power and power-cycle.
An agent cannot do those physically, so it must either prompt a human at those
two moments or run on hardware whose cable wires the reset lines.

### Install

```sh
git clone https://github.com/visionik/csk-flow.git
git clone https://github.com/LISTENAI/cskburn.git cskburn-src
cmake -S csk-flow -B csk-flow/build -DCSKBURN_DIR="$PWD/cskburn-src"
cmake --build csk-flow/build
# binaries: csk-flow/build/csk-flow and csk-flow/build/csk-flowd
```

`-DCSKBURN_DIR` must point at a cskburn checkout; the build copies the one
helper it needs (`bin2c.py`) automatically.

### Use (one-shot, JSON)

```sh
csk-flow --json --no-console -s <port> -C venus \
    0x100000 dsp_firmware.bin 0x0 zephyr.bin
```

`--json` prints one object on **stdout** (human `>>>` prompts stay on stderr):

```json
{
  "chip_id": "300ec7029d9132db",
  "flash": "ok",
  "partitions": [
    {"addr": "0x00100000", "bytes": 466832, "action": "skipped", "ms": 0},
    {"addr": "0x00000000", "bytes": 359964, "action": "written", "ms": 33000}
  ],
  "console": {"lines": 0, "matched": null},
  "result": "ok"
}
```

Parse `result` (`ok` | `matched` | `console-timeout` | `flash-failed`) and each
partition's `action` (`written` | `skipped` | `failed`). Exit codes:

| code | meaning |
|------|---------|
| 0 | flashed; and `--until` matched if one was given |
| 1 | flash failed, or `--until` did not match within `--timeout` |
| 2 | bad usage |

Flags an agent will want:

- `--no-console` — flash (or skip) and exit without the observe phase.
- `--force` — always write; otherwise unchanged partitions are skipped.
- `--until <regex>` / `--timeout <ms>` — bounded, deterministic observe.
- `-v` / `-vv` — raise the library log level (DEBUG / TRACE) for diagnosis.

### Use (daemon, for orchestration)

`csk-flowd` holds the port open and serves newline-delimited commands over a
Unix socket, one JSON line per command — so an orchestrator drives many
operations through a single long-lived endpoint instead of re-spawning the CLI:

```sh
csk-flowd -S /tmp/cskd.sock -s <port> -C venus &
printf 'flash --force 0x100000 dsp_firmware.bin 0x0 zephyr.bin\n' | nc -U /tmp/cskd.sock
printf 'console --until "Booting Zephyr OS" --timeout 90000\n'     | nc -U /tmp/cskd.sock
printf 'quit\n'                                                     | nc -U /tmp/cskd.sock
```

Commands: `flash [--force] <addr> <file> ...`, `console [--until REGEX]
[--timeout MS]`, `ping`, `quit`. `--until` may be double-quoted to include
spaces. The daemon serves one command at a time; the same manual BOOT/power and
power-cycle steps apply.

---

## License

[Apache-2.0](LICENSE). See `NOTICE` for attribution. csk-flow links Apache-2.0
`libcskburn_serial` (LISTENAI/cskburn) without bundling it; `src/md5.c` is
public domain.
