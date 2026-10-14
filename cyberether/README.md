# BirriaSDR in CyberEther

**[Download CyberEther](https://cyberether.org/) ·
[Installation guide](https://cyberether.org/docs/installation) ·
[Python setup](https://cyberether.org/docs/python-block#choosing-a-python-runtime) ·
[Flowgraph](birriasdr-no-mediatek.yml)**

This is the latest experimental flowgraph with corrected RF initialization and
manual gain controls. Its Python block contains the receiver and our 1,120-byte
device program. It runs independently of the C library: no compilation or
MediaTek firmware download is needed. The device program runs in volatile RAM
and uses the chip's immutable factory ROM; nothing is flashed.

## Run

1. Install the **CyberEther desktop app** with Python-block support and a
   **Python 3.11+** runtime. This flowgraph requires Linux or macOS.
2. In **Settings → Runtime → Python Runtime**, select that interpreter and
   restart CyberEther. Leave `PYTHON = None` in the block so its USB worker uses
   the same interpreter and compatible packages.
3. Connect the tested **COMFAST MT7961/MT7921AU dongle, USB ID `3574:6211`**, to
   USB 3.0. Disconnect its Wi-Fi connection and stop any other receiver using it.
   Follow the USB setup below before opening the graph.
4. Open **birriasdr-no-mediatek.yml** in CyberEther. Allow installation of the
   dependencies declared at the top of the Python block: NumPy, PyUSB and
   libusb-package. Internet access is needed for their first installation.
5. The Python receiver feeds **Output 0** directly into the spectrum/waterfall.
   Open its **Console** if initialization fails. Close the flowgraph to stop
   capture and release the dongle.

The fixed profile is **channel 6, nominal 2437 MHz, chain 0, 40 MSps complex**.
The single CF32 output contains I/Q normalized by 8192; no samples are averaged
or decimated. Both spectrum and waterfall averaging are set to **1**. The
sampled span is 40 MHz; the current analog filter uses a 20 MHz profile and
rolls off. Other channels, 5 GHz and wider filters are not enabled in this graph.

## USB setup

**Raspberry Pi 5:** with `COLD_PI5 = None`, the worker automatically performs
the guarded cold-start procedure, power-cycling the Pi's USB ports and preventing
the kernel Wi-Fi driver from loading firmware. Only the COMFAST dongle and root
hubs may be present; an active USB network or another USB device makes it refuse
the operation. This procedure needs root access.

**Linux:** `USE_SUDO = None` runs the USB worker through `sudo -n` when CyberEther
is not already root. That worker must be authorized to run without a password
prompt. With direct USB permissions configured, set `USE_SUDO = False` and
`COLD_PI5 = False`; then arrange a fresh power-on with kernel Wi-Fi driver binding
prevented yourself. Hosts other than Pi 5 always require that manual cold start.

**macOS:** direct USB access is the default. Reconnect the dongle if another
driver previously loaded vendor firmware. Leave `PYTHON = None`; do not point
the worker at Xcode's Python 3.9. This experimental version has not been tested
on macOS hardware.

A software reset cannot remove a retained vendor ROM patch. If startup reports
a retained patch, repeat the cold-start setup; there is no vendor-firmware
fallback. A NumPy error mentioning two different Python versions indicates an
interpreter mismatch: select Python 3.11+, restart CyberEther and let it resolve
dependencies for that runtime.

## Gain controls

Edit these settings near the top of the Python block and reload it. Reloading
starts a new capture; these are not live gain commands.

| Setting | Default | Values |
| --- | --- | --- |
| `GAIN_MODE` | `'automatic'` | `'automatic'` or `'manual'` |
| `LNA_GAIN` | `1` | `0`–`3` |
| `LPF_GAIN` | `4` | `0`–`8` |
| `DIGITAL_GAIN` | `0` | `0`–`12` |

The three codes apply in manual mode and are not calibrated dB. `LPF_GAIN`
changes filter-stage gain, not bandwidth. Automatic mode releases our overrides;
complete AGC behavior remains unverified.

## Validation and source

This graph contains the RF-corrected Python receiver that delivered 6,029,312
complex samples containing six valid Wi-Fi SIGNAL headers on the Pi 5 before
an overrun stopped it. Sustained GUI streaming remains unvalidated. An overrun
is reported explicitly instead of silently dropping samples; check the receiver
Console and reload to begin a new acquisition.

The [MT7921 receiver notes](../docs/mt7921.md) explain the RF, gain and capture
mechanisms. This graph uses the separate Python receiver and does not inherit
the C library's test results. Its embedded receiver code is unchanged from the
RF-validated graph; only the in-graph instructions have been updated for this
folder.

The matching assembly is [firmware/iq_stream.S](../firmware/iq_stream.S).
See the [library README](../README.md#source-and-hardware-notes) for optional
rebuild instructions. Rebuilding the C image does not update the Python block:
firmware changes also require replacing its `ORIGINAL_IMAGE` and matching
`ORIGINAL_MANIFEST`.

Original work: **MIT, copyright (c) 2026 Luigi C.** Full MIT and retained
BSD-3-Clause-Clear driver notices are embedded in the Python block. See
[LICENSE](../LICENSE) and [NOTICE](../NOTICE). Provided without warranty;
use at your own risk.
