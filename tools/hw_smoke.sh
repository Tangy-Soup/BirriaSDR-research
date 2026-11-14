#!/bin/bash
# Hardware smoke suite for BirriaSDR on an MT7921AU (0e8d:7961).
#
#   tools/hw_smoke.sh [--build | --installed] [--both] [--kill] [--soapy] [--all] [--dry-run] [OUTDIR]
#
# Default steps use chain 0 and shipped gain codes only:
#   list, 2437 @ 40 MSps, 2437 @ 20 MSps, 5240 @ 40 MSps (gain 5 14 0), 6135 tune,
#   10 s @ 40 MSps to disk, 5 s @ 80 MSps to disk, final open.
# Opt-in steps:
#   --both   1 s per chain at 2437 with --chain both
#   --kill   kill -9 a forced-gain stream, then reopen (checks the killed-process clear)
#   --soapy  SoapySDR open, ranges, gain round trip, 2 s stream with host AGC
#   --all    all three
# --build (default) runs the build tree (BIRRIA_BUILD, default <repo>/build);
# --installed runs BIRRIA_PREFIX (default /usr/local). --dry-run prints the
# commands and touches nothing. OUTDIR defaults to ./birria_smoke_<stamp>_<mode>
# and needs about 2 GB free while a disk step runs.
#
# The script waits for the dongle to be idle for 30 s before it starts, never
# uses sudo, deletes each recording after checking it, and exits 1 if any step
# fails. A state-9 overrun on a disk step is retried once after 30 s.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BUILD=${BIRRIA_BUILD:-$REPO/build}
PREFIX=${BIRRIA_PREFIX:-/usr/local}
mode=build; both=0; kill9=0; soapy=0; dry=0; out=
for a in "$@"; do
    case "$a" in
        --build) mode=build ;;
        --installed) mode=installed ;;
        --both) both=1 ;;
        --kill) kill9=1 ;;
        --soapy) soapy=1 ;;
        --all) both=1; kill9=1; soapy=1 ;;
        --dry-run) dry=1 ;;
        -*) echo "unknown option $a" >&2; exit 2 ;;
        *) out=$a ;;
    esac
done
[ -n "$out" ] || out=$PWD/birria_smoke_$(date +%Y%m%d_%H%M%S)_$mode

if [ "$mode" = build ]; then
    RX=$BUILD/birria_rx
    export LD_LIBRARY_PATH=$BUILD
    export SOAPY_SDR_ROOT=/nonexistent SOAPY_SDR_PLUGIN_PATH=$BUILD/soapy
else
    RX=$PREFIX/bin/birria_rx
fi

node() { lsusb | awk '/0e8d:7961/ {gsub(":", "", $4); printf "/dev/bus/usb/%s/%s", $2, $4; exit}'; }
holders() { ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c " -> $1\$"; }

pass=0; fail=0; results=()
record() {  # name status detail
    results+=("$(printf '%-12s %-5s %s' "$1" "$2" "$3")")
    if [ "$2" = FAIL ]; then fail=$((fail + 1)); else pass=$((pass + 1)); fi
    echo "== $1: $2 $3"
}

# run NAME EXPECTED_SAMPLES MAX_GAPS ARGS...   (ARGS end with the output path and count)
run() {
    local name=$1 want=$2 maxgaps=$3; shift 3
    local log=$out/$name.log file=$out/$name.cs16
    if [ $dry = 1 ]; then echo "$RX $* $file $want"; return 0; fi
    local try
    for try in 1 2; do
        nice -n 5 "$RX" "$@" "$file" "$want" >"$log" 2>&1
        local rc=$?
        local saved gaps state
        saved=$(sed -n 's/^Saved \([0-9]*\) complex samples.*/\1/p' "$log")
        gaps=$(sed -n 's/^Recovered \([0-9]*\) gaps.*/\1/p' "$log")
        state=$(sed -n 's/.*firmware state=\([0-9]*\).*/\1/p' "$log")
        rm -f "$file"
        [ -f "$file.gaps" ] && mv "$file.gaps" "$out/$name.gaps"
        local detail="rc=$rc saved=${saved:-?} gaps=${gaps:-n/a} state=${state:-?} $(grep -o 'longest gap [0-9.]* ms' "$log")"
        if [ $rc -eq 0 ] && [ "${saved:-0}" -ge "$want" ] && [ "${gaps:-0}" -le "$maxgaps" ]; then
            record "$name" PASS "$detail"; return 0
        fi
        if [ $try = 1 ] && { [ "${state:-0}" = 9 ] || [ "${gaps:-0}" -gt "$maxgaps" ]; }; then
            echo "   $name: overrun or gaps ($detail); retrying once in 30 s"
            cp "$log" "$log.try1"; sleep 30; continue
        fi
        record "$name" FAIL "$detail"; return 1
    done
}

[ $dry = 1 ] || mkdir -p "$out" || exit 2
n=$(node)
if [ -z "$n" ]; then echo "no 0e8d:7961 device attached"; exit 2; fi
speed=$(for d in /sys/bus/usb/devices/*; do
    [ "$(cat "$d/idVendor" 2>/dev/null):$(cat "$d/idProduct" 2>/dev/null)" = 0e8d:7961 ] && cat "$d/speed"; done | head -1)
echo "node $n; link ${speed:-?} Mb/s; mode $mode; out $out"
[ "${speed:-0}" = 5000 ] || echo "WARNING: the dongle is not at 5000M; ask for a reseat before trusting gap counts"
if [ $dry = 0 ]; then
    [ -x "$RX" ] || { echo "$RX missing"; exit 2; }
    avail=$(df -Pk "$out" | awk 'NR == 2 {print $4}')
    [ "${avail:-0}" -ge 2000000 ] || { echo "less than 2 GB free in $out"; exit 2; }
    free=0
    while [ $free -lt 30 ]; do
        if [ "$(holders "$n")" -eq 0 ]; then free=$((free + 2)); else free=0; echo "busy ($n)"; fi
        sleep 2
    done
    echo "idle 30 s"
    { date -Is; git -C "$REPO" log -1 --format='BirriaSDR %h %s'; echo "mode $mode"; } >"$out/run.txt"
fi

# 1. list
if [ $dry = 1 ]; then echo "$RX --list"
elif "$RX" --list 2>&1 | tee "$out/list.log" | grep -q '0e8d:7961'; then record list PASS ""
else record list FAIL "device not listed"; fi

# 2-5. buffered captures: 1 s at 40 MSps is 40,001,536 samples (a multiple of 2048)
run b24_40   40001536 0 --buffered --frequency 2437000000
run b24_20   20000768 0 --buffered --frequency 2437000000 --rate 20000000 --bandwidth 20000000
run b5_40    40001536 0 --buffered --frequency 5240000000 --gain 5 14 0
run b6_tune  10000384 0 --buffered --frequency 6135000000 --gain 5 14 0

# 6-7. streamed to disk through the ring writer (1.6 GB each, deleted after the check)
run d24_40  400001024 0 --recover --frequency 2437000000
run d24_80  400001024 8 --recover --frequency 2437000000 --rate 80000000

# 8. both chains (opt-in)
if [ $both = 1 ]; then
    run both24 79998976 0 --buffered --chain both --frequency 2437000000 --gain 5 7 0
fi

# 9. killed process, then reopen (opt-in)
if [ $kill9 = 1 ]; then
    if [ $dry = 1 ]; then echo "kill -9 test: $RX --recover --frequency 2437000000 --gain 5 7 0 ...; reopen"
    else
        "$RX" --recover --frequency 2437000000 --gain 5 7 0 "$out/kill.cs16" 4000002048 >"$out/kill_victim.log" 2>&1 &
        victim=$!
        sleep 4
        if kill -0 $victim 2>/dev/null; then
            kill -9 $victim; wait $victim 2>/dev/null
            rm -f "$out/kill.cs16"; [ -f "$out/kill.cs16.gaps" ] && mv "$out/kill.cs16.gaps" "$out/kill_victim.gaps"
            sleep 3
            run kill_reopen 40001536 0 --buffered --frequency 2437000000
            grep -q 'unexpected hardware or receiver state' "$out/kill_reopen.log" &&
                echo "   kill_reopen: leftover gain override; the dongle needs a replug"
        else
            rm -f "$out/kill.cs16"
            record kill_reopen FAIL "victim exited before the kill (see kill_victim.log)"
        fi
    fi
fi

# 10. SoapySDR (opt-in)
if [ $soapy = 1 ]; then
    if [ $dry = 1 ]; then echo "python3 $HERE/hw_smoke_soapy.py"
    elif python3 "$HERE/hw_smoke_soapy.py" >"$out/soapy.log" 2>&1; then record soapy PASS "$(tail -1 "$out/soapy.log")"
    else record soapy FAIL "$(tail -1 "$out/soapy.log")"; fi
fi

# 11. final open
run final 2048000 0 --buffered --frequency 2437000000

[ $dry = 1 ] && exit 0
{ printf '%s\n' "${results[@]}"; echo "pass $pass fail $fail"; } | tee "$out/summary.txt"
[ $fail -eq 0 ]
