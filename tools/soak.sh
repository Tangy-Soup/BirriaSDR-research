#!/bin/bash
# Run soak.py once the dongle has had no open handle for 30 s.
#
#   tools/soak.sh [MINUTES [OUTDIR [soak.py options...]]]
#
# MINUTES defaults to 120 and OUTDIR to ./birria_soak_<stamp>. soak.py streams through
# SoapySDR, so it uses whichever module SoapySDR finds (set SOAPY_SDR_PLUGIN_PATH to test a
# build tree). Summarise with tools/soak_summary.py OUTDIR.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
min=${1:-120}
out=${2:-$PWD/birria_soak_$(date +%Y%m%d_%H%M%S)}
shift $(( $# < 2 ? $# : 2 ))
mkdir -p "$out"
n=$(lsusb | awk '/0e8d:7961/ {gsub(":", "", $4); printf "/dev/bus/usb/%s/%s", $2, $4; exit}')
[ -n "$n" ] || { echo "no 0e8d:7961 device attached"; exit 2; }
free=0
while [ $free -lt 30 ]; do
    if [ "$(ls -l /proc/[0-9]*/fd 2>/dev/null | grep -c " -> $n\$")" -eq 0 ]; then free=$((free + 2)); else free=0; echo "busy ($n)"; fi
    sleep 2
done
echo "idle 30 s ($n); soak $min min -> $out"
{
    date -Is
    git -C "$REPO" log -1 --format='BirriaSDR %h %s'
    for lib in "$REPO"/build/libbirriasdr.so.*.*.*; do
        cmp -s "$lib" "/usr/local/lib/$(basename "$lib")" && echo "installed = build"
    done
    [ -n "${SOAPY_SDR_PLUGIN_PATH:-}" ] && echo "SOAPY_SDR_PLUGIN_PATH=$SOAPY_SDR_PLUGIN_PATH"
} > "$out/run.txt"
OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 nice -n 5 python3 "$HERE/soak.py" --minutes "$min" --out "$out" "$@" > "$out/soak.log" 2>&1
rc=$?
echo "soak rc=$rc" | tee -a "$out/run.txt"
exit $rc
