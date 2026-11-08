#!/usr/bin/env bash
#
# Test whether the C62 cable can auto-enter burn mode (no button press).
#
# Run with the radio POWERED ON and RUNNING NORMALLY (OpenRTX UI showing),
# cable connected, and NOT manually held in burn mode. Each reset strategy
# tries to read the chip ID, which needs burn mode but changes nothing. A
# strategy that returns an ID means auto-entry works with that DTR/RTS->
# BOOT/RESET mapping; failures time out quickly and leave the radio running.
#
# usage: autoburn-test.sh [serial-port]

set -u
PORT="${1:-$(ls /dev/cu.PL2303G* 2>/dev/null | head -1)}"
[ -z "$PORT" ] && { echo "no PL2303 serial port found"; exit 1; }

echo "port: $PORT"
echo "Make sure the radio is ON and running normally (not in burn mode)."
echo

WIN=0
for strat in dtr-boot rts-boot rts-boot-inv dual-npn; do
    echo "=== strategy: $strat ==="
    # -C 6 = venus (CSK6); short probe + 3 attempts so a miss fails fast.
    # Capture first, THEN judge: success = a "chip-id:" line appeared.
    # (Don't pipe into sed in the `if` -- that masks cskburn's exit status.)
    out=$(cskburn -s "$PORT" -C 6 --chip-id \
            --reset-strategy "$strat" \
            --probe-timeout 4000 --reset-attempts 3 2>&1)
    echo "$out" | sed 's/^/    /'
    if echo "$out" | grep -qi 'chip-id:'; then
        echo "    >>> $strat: SUCCESS (auto burn-mode entry works)"
        WIN=1
    else
        echo "    >>> $strat: no entry"
    fi
    echo
    sleep 1
done

if [ "$WIN" = 1 ]; then
    echo "RESULT: at least one strategy auto-entered burn mode."
else
    echo "RESULT: no strategy worked -> the cable likely does not wire"
    echo "        DTR/RTS to BOOT/RESET; burn mode stays manual."
fi
