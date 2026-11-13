# Setting up a new machine

This folder gets a fresh computer from nothing to a working receiver: the
library, `birria_rx`, the SoapySDR module, USB access, and optionally the
Python decoders and SDR++. It covers Windows with WSL2, which is how this fork
was developed, and native Linux.

| File | Runs on | What it does |
| --- | --- | --- |
| [`setup-linux.sh`](setup-linux.sh) | Ubuntu or Debian, native or WSL2 | Installs packages, builds, runs the offline tests, installs to `/usr/local`, adds the udev rule and keeps `mt7921u` off the dongle. `--decoders` and `--sdrpp` add the extras |
| [`check-dongle.sh`](check-dongle.sh) | Same | Checks the USB link speed, driver binding and permissions, lists the device, and with `--capture` records 0.1 s |
| [`windows/attach-dongle.ps1`](windows/attach-dongle.ps1) | Windows | Finds the dongle, shares it with WSL (one UAC prompt the first time), and attaches it |

You need:

- an MT7921AU dongle with USB ID `0e8d:7961` (this fork was tested on the
  EDUP EP-AX1672) or the COMFAST `3574:6211`.
- a **USB 3** port. The receiver refuses a USB 2 link.
- Ubuntu 24.04 (native or WSL2) or a similar Debian-based system, with about
  2 GB free for the build and SDR++. Recordings take 160 MB per second at
  40 MSps.

## Windows with WSL2

1. **WSL and usbipd-win.** In an administrator PowerShell, install
   Ubuntu and the USB sharing service, then reboot:

   ```powershell
   wsl --install -d Ubuntu-24.04
   winget install --exact dorssel.usbipd-win
   ```

   Open Ubuntu from the Start menu once and create your Linux user.

2. **Get the code.** In the Ubuntu terminal, either clone the release tag:

   ```sh
   git clone --branch v2026.11.13 https://github.com/Tangy-Soup/BirriaSDR-research.git
   cd BirriaSDR-research
   ```

   or download the release's source `.tar.gz` and unpack it in your Linux
   home directory, not under `/mnt/c`, which builds slowly.

3. **Build and install.** Still in Ubuntu:

   ```sh
   bash setup/setup-linux.sh --all
   ```

   Leave out `--all` for just the library, recorder and SoapySDR module. Add
   `--decoders` for the Python decoders or `--sdrpp` for SDR++ on their own.
   The script asks for your sudo password. If it says to restart WSL, run
   `wsl --shutdown` in PowerShell and open Ubuntu again. That turns on
   systemd (so the udev rule applies) and your `plugdev` membership.

4. **Plug in the dongle and attach it.** Use a USB 3 port. Keep the Ubuntu
   terminal open (WSL must be running), and from it run:

   ```sh
   powershell.exe -ExecutionPolicy Bypass -File "$(wslpath -w setup/windows/attach-dongle.ps1)"
   ```

   The first time, Windows shows a UAC prompt for `usbipd bind --force`, and
   the script then asks you to **unplug and replug the dongle**. Windows' own
   Wi-Fi driver may already have loaded its firmware onto the chip, which
   BirriaSDR can't clear, and the replug starts the chip clean. The forced
   bind keeps Windows' driver off the dongle from then on.

5. **Check it.**

   ```sh
   setup/check-dongle.sh --capture
   ```

   It should end with `Ready.`

After every replug, reboot or `wsl --shutdown`, run step 4 again. The bind is
remembered, so there is no prompt and no replug. To have usbipd re-attach on
its own after replugs, add `-AutoAttach` and leave that window open. Bus IDs
change with what else is plugged in, so the script looks the dongle up each
time.

## Native Linux

```sh
git clone --branch v2026.11.13 https://github.com/Tangy-Soup/BirriaSDR-research.git
cd BirriaSDR-research
bash setup/setup-linux.sh --all
```

The script blacklists `mt7921u`, the kernel's Wi-Fi driver for these USB
dongles. PCIe MT7921 cards use `mt7921e` and keep working. If `mt7921u` was
already loaded, unplug the dongle, run `sudo modprobe -r mt7921u`, and plug
it back in. Log out and back in for the `plugdev` group, then run
`setup/check-dongle.sh --capture`.

On other distributions, install the equivalents of `build-essential cmake
pkg-config git libusb-1.0-0-dev usbutils`, plus `libsoapysdr-dev
soapysdr-tools` for the SoapySDR module, and run the script with
`--skip-packages`. SDR++ also needs FFTW, GLFW, VOLK, RtAudio and zstd
development files.

## First things to try

```sh
# 1 s of Wi-Fi channel 6 at fixed gain
birria_rx --frequency 2437000000 --gain 3 8 0 ch6.cs16 40000000 manual

# Decode it (needs --decoders)
tools/decoders/.venv/bin/python tools/decoders/wifi.py ch6.cs16 --center 2437

# Survey channels 1 to 11
cd tools/decoders && .venv/bin/python wifi_scan.py --seconds 0.5

# Live spectrum (needs --sdrpp). On WSL, the d3d12 driver renders on the GPU.
GALLIUM_DRIVER=d3d12 sdrpp
```

In SDR++, choose **SoapySDR** as the source, pick **BirriaSDR ...**, and press
play. Don't start it with `--autostart`, which can crash before the first
window layout. gqrx and GNU Radio work too, as described in the
[SoapySDR module](../README.md#soapysdr-module) section.

## Antenna placement

Chain 0, the receiver BirriaSDR uses by default, is antenna port **A** on the
EP-AX1672. The dongle radiates broadband USB 3 noise into any antenna running
alongside it, so screw the stock antenna **straight out** from the port, with
no bend or extension, and aim it by turning the whole dongle. Wrapping the
dongle and the USB extension's connector in foil, grounded to the USB shells,
lowered the 2.4 GHz noise floor by another 4 to 7 dB. Details are under
[antenna placement and shielding](../README.md#antenna-placement-and-shielding).

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `check-dongle.sh` says 480 Mb/s | The dongle linked at USB 2. Move it to a USB 3 port, or try another cable or extension, and reseat the plug |
| A kernel driver is bound | Native: `sudo modprobe -r mt7921u`, then replug. WSL: the stock kernel has no such driver, so rerun the attach script |
| The device node isn't writable | The udev rule isn't applied: rerun `setup-linux.sh`, make sure systemd runs (WSL: `wsl --shutdown`), and replug or re-attach |
| A start fails at the gain stage with "unexpected hardware or receiver state" | A killed program left a gain override behind. Unplug and replug, then re-attach |
| usbipd says "Device in error state" | Unplug and replug, then rerun the attach script. If Windows put a different driver on it, the script's forced bind fixes that |
| Overruns (firmware state 9) in bursts | Usually transient on usbipd. Wait a few minutes and retry. If they persist after days of uptime, restart the service from an admin PowerShell: `Restart-Service usbipd` |
| SDR++ uses several CPU cores, or shows device gaps | It is rendering on the CPU. Start it with `GALLIUM_DRIVER=d3d12 sdrpp` |
| `birria_rx` refuses to overwrite a file | By design. Pick a new file name |

## Updating

Pull or download the newer release and run `bash setup/setup-linux.sh` again,
with the same options. It rebuilds and reinstalls over the previous version.
Restart SDR++ or any other Soapy application afterwards.
