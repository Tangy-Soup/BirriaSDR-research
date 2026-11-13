#!/usr/bin/env bash
# Check that a connected dongle is ready for BirriaSDR, then optionally take a
# short capture.
#   setup/check-dongle.sh             # read-only checks plus birria_rx --list
#   setup/check-dongle.sh --capture   # also a 0.1 s capture, deleted afterwards
#
# Exit status is 0 when everything needed for streaming passed.

set -uo pipefail

CAPTURE=0
case "${1:-}" in
    --capture) CAPTURE=1 ;;
    "") ;;
    -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fail=0
ok()   { printf '  ok    %s\n' "$*"; }
warn() { printf '  warn  %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; fail=1; }

echo "Dongle"
found=()
for dev in /sys/bus/usb/devices/*; do
    [ -f "${dev}/idVendor" ] || continue
    id="$(cat "${dev}/idVendor"):$(cat "${dev}/idProduct")"
    case "${id}" in
        0e8d:7961|3574:6211) found+=("${dev}") ;;
    esac
done

if [ ${#found[@]} = 0 ]; then
    bad "no 0e8d:7961 or 3574:6211 device on USB"
    if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
        echo "        WSL: attach it from Windows with setup/windows/attach-dongle.ps1"
    fi
    exit 1
fi

for dev in "${found[@]}"; do
    id="$(cat "${dev}/idVendor"):$(cat "${dev}/idProduct")"
    bus=$(cat "${dev}/busnum"); num=$(cat "${dev}/devnum")
    node=$(printf '/dev/bus/usb/%03d/%03d' "${bus}" "${num}")
    product="$(cat "${dev}/product" 2>/dev/null || echo '?')"
    echo "  ${id} '${product}' at ${node}"

    speed=$(cat "${dev}/speed")
    if [ "${speed}" -ge 5000 ] 2>/dev/null; then
        ok "linked at ${speed} Mb/s (USB 3)"
    else
        bad "linked at ${speed} Mb/s; BirriaSDR needs USB 3 (5000). Use a USB 3 port or cable and reseat the plug"
    fi

    drivers=""
    for intf in "${dev}"/*:*; do
        [ -e "${intf}/driver" ] || continue
        d=$(basename "$(readlink "${intf}/driver")")
        [ "${d}" = usbfs ] && continue
        drivers+="${d} on $(basename "${intf}") "
    done
    if [ -z "${drivers}" ]; then
        ok "no kernel driver bound"
    else
        bad "kernel driver bound: ${drivers}. Unload it (sudo modprobe -r mt7921u), then unplug and replug"
    fi

    if [ -r "${node}" ] && [ -w "${node}" ]; then
        ok "${node} is readable and writable by $(id -un)"
    else
        bad "${node} isn't writable by $(id -un) ($(stat -c '%U:%G %a' "${node}" 2>/dev/null)). Install the udev rule (setup/setup-linux.sh), make sure udev runs, and replug"
    fi
done

echo "Host"
mb=$(cat /sys/module/usbcore/parameters/usbfs_memory_mb 2>/dev/null || echo '?')
ok "usbfs_memory_mb = ${mb} (the library fits its USB queue to this)"
if grep -qw mt7921u /proc/modules 2>/dev/null; then
    warn "mt7921u is loaded; it may claim the dongle on the next replug"
fi

RX=""
if command -v birria_rx >/dev/null; then
    RX=$(command -v birria_rx)
elif [ -x "${ROOT}/build/birria_rx" ]; then
    RX="${ROOT}/build/birria_rx"
fi
if [ -z "${RX}" ]; then
    bad "birria_rx not found; run setup/setup-linux.sh"
    exit 1
fi
ok "recorder: ${RX}"

echo "birria_rx --list"
if ! "${RX}" --list; then
    bad "birria_rx --list failed"
fi

if command -v SoapySDRUtil >/dev/null; then
    if SoapySDRUtil --find=driver=birriasdr 2>/dev/null | grep -q 'driver = birriasdr'; then
        ok "SoapySDR finds driver=birriasdr"
    else
        warn "SoapySDR doesn't list driver=birriasdr (module not installed, or the device is busy)"
    fi
fi

if [ "${CAPTURE}" = 1 ] && [ "${fail}" = 0 ]; then
    echo "Capture"
    tmp=$(mktemp -d)
    out="${tmp}/check.cs16"
    if "${RX}" --frequency 2437000000 "${out}" 4194304 manual; then
        size=$(stat -c %s "${out}")
        ok "captured 4,194,304 samples at 2437 MHz (${size} bytes)"
    else
        bad "capture failed; if it stopped at the gain stage with 'unexpected hardware or receiver state', unplug and replug the dongle"
    fi
    rm -rf "${tmp}"
fi

echo
if [ "${fail}" = 0 ]; then
    echo "Ready."
else
    echo "Not ready: fix the FAIL lines above."
fi
exit "${fail}"
