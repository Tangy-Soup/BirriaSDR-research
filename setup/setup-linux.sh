#!/usr/bin/env bash
# Set up a Linux machine (native or WSL2) to run this BirriaSDR fork.
#
# Run it from anywhere inside a checkout or an unpacked release:
#   setup/setup-linux.sh                 # library, birria_rx, SoapySDR module, udev
#   setup/setup-linux.sh --decoders      # also the Python decoders' venv
#   setup/setup-linux.sh --sdrpp         # also SDR++ built with its SoapySDR source
#   setup/setup-linux.sh --all           # all of the above
#
# It asks for sudo for apt, `cmake --install`, the udev rule and the
# driver blacklist. Each step is safe to repeat. Debian or Ubuntu is assumed
# for package installs; on other distributions pass --skip-packages and
# install the equivalents yourself (listed in setup/README.md).

set -euo pipefail

PREFIX=/usr/local
WITH_SOAPY=1
WITH_DECODERS=0
WITH_SDRPP=0
SKIP_PACKAGES=0
RUN_TESTS=1
# SDR++ has no stable plugin ABI, so the Soapy source and SDR++ itself are
# built from one commit. This one was used with the module on 2026-11-13.
SDRPP_COMMIT=8c9f5ee
SDRPP_DIR="${HOME}/src/SDRPlusPlus"

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    cat <<EOF
Options:
  --prefix DIR        install prefix (default ${PREFIX})
  --no-soapy          skip the SoapySDR module
  --decoders          create tools/decoders/.venv with its requirements
  --sdrpp             build and install SDR++ ${SDRPP_COMMIT} with SoapySDR
  --sdrpp-dir DIR     where to clone SDR++ (default ${SDRPP_DIR})
  --all               --decoders and --sdrpp
  --skip-packages     don't run apt
  --skip-tests        don't run ctest
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift ;;
        --no-soapy) WITH_SOAPY=0 ;;
        --decoders) WITH_DECODERS=1 ;;
        --sdrpp) WITH_SDRPP=1 ;;
        --sdrpp-dir) SDRPP_DIR="$2"; shift ;;
        --all) WITH_DECODERS=1; WITH_SDRPP=1 ;;
        --skip-packages) SKIP_PACKAGES=1 ;;
        --skip-tests) RUN_TESTS=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ "${WITH_SDRPP}" = 1 ] && [ "${WITH_SOAPY}" = 0 ]; then
    echo "--sdrpp needs the SoapySDR module; drop --no-soapy." >&2
    exit 2
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"
[ -f CMakeLists.txt ] && [ -f include/birriasdr.h ] || {
    echo "Run this from a BirriaSDR checkout (no CMakeLists.txt in ${ROOT})." >&2
    exit 1
}

step() { printf '\n==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }

IS_WSL=0
if grep -qi microsoft /proc/sys/kernel/osrelease 2>/dev/null; then
    IS_WSL=1
fi

SUDO=sudo
if [ "$(id -u)" = 0 ]; then
    echo "Run this as your normal user, not root: the build tree and the" >&2
    echo "plugdev membership should belong to the account that uses the dongle." >&2
    exit 1
fi

step "Machine: $(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}"), kernel $(uname -r)$([ ${IS_WSL} = 1 ] && echo ', WSL2')"

# --- packages -------------------------------------------------------------
if [ "${SKIP_PACKAGES}" = 0 ]; then
    if ! command -v apt-get >/dev/null; then
        echo "No apt-get here. Install the packages listed in setup/README.md," >&2
        echo "then rerun with --skip-packages." >&2
        exit 1
    fi
    pkgs=(build-essential cmake pkg-config git libusb-1.0-0-dev usbutils xz-utils)
    [ "${WITH_SOAPY}" = 1 ] && pkgs+=(libsoapysdr-dev soapysdr-tools python3-soapysdr python3-numpy)
    [ "${WITH_DECODERS}" = 1 ] && pkgs+=(python3-venv python3-pip)
    [ "${WITH_SDRPP}" = 1 ] && pkgs+=(libfftw3-dev libglfw3-dev libvolk-dev librtaudio-dev libzstd-dev)
    step "Installing packages: ${pkgs[*]}"
    ${SUDO} apt-get update
    ${SUDO} apt-get install -y "${pkgs[@]}"
fi

# --- library, recorder, Soapy module --------------------------------------
step "Building BirriaSDR in ${ROOT}/build"
soapy_flag=OFF
[ "${WITH_SOAPY}" = 1 ] && soapy_flag=ON
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBIRRIASDR_SOAPY="${soapy_flag}"
cmake --build build -j"$(nproc)"
if [ "${RUN_TESTS}" = 1 ]; then
    step "Running the offline tests (no dongle needed)"
    ctest --test-dir build --output-on-failure
fi

step "Installing to ${PREFIX}"
${SUDO} cmake --install build --prefix "${PREFIX}"
${SUDO} ldconfig

# --- USB access -----------------------------------------------------------
step "Installing the udev rule (USB access without sudo)"
${SUDO} install -m 644 udev/60-birriasdr.rules /etc/udev/rules.d/
if [ -d /run/udev ]; then
    ${SUDO} udevadm control --reload-rules
    ${SUDO} udevadm trigger --subsystem-match=usb || true
else
    note "udev isn't running, so the rule has no effect yet."
fi
if ! id -nG | tr ' ' '\n' | grep -qx plugdev; then
    ${SUDO} usermod -aG plugdev "$(id -un)"
    note "Added $(id -un) to plugdev. Log out and back in (WSL: wsl --shutdown)."
fi

# --- keep the Wi-Fi driver off the dongle ---------------------------------
# BirriaSDR needs the chip without a vendor ROM patch and with its Wi-Fi
# interface unclaimed. mt7921u is the USB driver only; PCIe MT7921 cards use
# mt7921e and are not affected.
step "Keeping mt7921u from binding the dongle"
blacklist=/etc/modprobe.d/birriasdr-mt7921u.conf
if [ ! -f "${blacklist}" ]; then
    printf '# BirriaSDR: keep the Wi-Fi driver off MT7921U dongles used as SDRs.\nblacklist mt7921u\n' \
        | ${SUDO} tee "${blacklist}" >/dev/null
    note "Wrote ${blacklist}."
fi
if grep -qw mt7921u /proc/modules 2>/dev/null; then
    note "mt7921u is loaded right now. Unplug the dongle, run"
    note "  sudo modprobe -r mt7921u"
    note "and plug it back in (a replug also clears any firmware it loaded)."
elif [ "${IS_WSL}" = 1 ]; then
    note "mt7921u isn't loaded (the stock WSL kernel doesn't include it)."
fi

# --- WSL specifics --------------------------------------------------------
if [ "${IS_WSL}" = 1 ]; then
    step "WSL checks"
    if [ "$(ps -p 1 -o comm= 2>/dev/null)" != systemd ]; then
        if ! grep -q '^\s*systemd\s*=\s*true' /etc/wsl.conf 2>/dev/null; then
            if grep -q '^\[boot\]' /etc/wsl.conf 2>/dev/null; then
                note "/etc/wsl.conf has a [boot] section without systemd=true. Add"
                note "  systemd=true"
                note "under it so udev runs, then run 'wsl --shutdown' in Windows."
            else
                printf '\n[boot]\nsystemd=true\n' | ${SUDO} tee -a /etc/wsl.conf >/dev/null
                note "Enabled systemd in /etc/wsl.conf so udev applies the rule."
                note "Run 'wsl --shutdown' in Windows, then reopen this shell."
            fi
        else
            note "systemd is enabled but not running yet: run 'wsl --shutdown' in Windows."
        fi
    else
        note "systemd is running, so udev applies the rule."
    fi
    note "Attach the dongle from Windows with setup/windows/attach-dongle.ps1."
fi

# --- decoders -------------------------------------------------------------
if [ "${WITH_DECODERS}" = 1 ]; then
    step "Creating the decoders' venv in tools/decoders/.venv"
    python3 -m venv tools/decoders/.venv
    tools/decoders/.venv/bin/pip install --upgrade pip
    tools/decoders/.venv/bin/pip install -r tools/decoders/requirements.txt
fi

# --- SDR++ ----------------------------------------------------------------
if [ "${WITH_SDRPP}" = 1 ]; then
    step "Building SDR++ ${SDRPP_COMMIT} with its SoapySDR source in ${SDRPP_DIR}"
    if dpkg -s sdrpp >/dev/null 2>&1; then
        note "The sdrpp .deb is installed. Its nightly build has no SoapySDR source,"
        note "and two copies would fight over the same name. Remove it first:"
        note "  sudo apt remove sdrpp"
        exit 1
    fi
    if [ ! -d "${SDRPP_DIR}/.git" ]; then
        mkdir -p "$(dirname "${SDRPP_DIR}")"
        git clone https://github.com/AlexandreRouma/SDRPlusPlus.git "${SDRPP_DIR}"
    fi
    git -C "${SDRPP_DIR}" fetch --quiet origin
    git -C "${SDRPP_DIR}" checkout --quiet "${SDRPP_COMMIT}"
    # Sources whose libraries aren't installed above are switched off.
    cmake -S "${SDRPP_DIR}" -B "${SDRPP_DIR}/build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        -DOPT_BUILD_SOAPY_SOURCE=ON \
        -DOPT_BUILD_AIRSPY_SOURCE=OFF -DOPT_BUILD_AIRSPYHF_SOURCE=OFF \
        -DOPT_BUILD_HACKRF_SOURCE=OFF -DOPT_BUILD_PLUTOSDR_SOURCE=OFF \
        -DOPT_BUILD_RTL_SDR_SOURCE=OFF -DOPT_BUILD_DISCORD_PRESENCE=OFF
    cmake --build "${SDRPP_DIR}/build" -j"$(nproc)"
    ${SUDO} cmake --install "${SDRPP_DIR}/build"
    ${SUDO} ldconfig
    if [ "${IS_WSL}" = 1 ]; then
        note "Start it with GPU rendering (CPU rendering costs about 3 cores):"
        note "  GALLIUM_DRIVER=d3d12 sdrpp"
    else
        note "Start it with: sdrpp"
    fi
    note "Don't use --autostart: it can crash before the first window layout."
    note "Pick 'SoapySDR' as the source, then 'BirriaSDR ...', and press play."
fi

step "Done. Plug in or attach the dongle, then run setup/check-dongle.sh"
