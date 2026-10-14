# BirriaSDR

> [!IMPORTANT]
>
> This is an experimental proof-of-concept. The production-quality library and the full documentation of the registers will be posted later.

Turn a supported USB 3.0 Wi-Fi dongle (MT7961/MT7921AU) into an experimental **2.4/5 GHz SDR receiver** with **14-bit I/Q at 40 MSps complex**, using a single CyberEther flowgraph. This is an experimental script. Use at your own risk.

**[Download CyberEther](https://cyberether.org/) · [Installation guide](https://cyberether.org/docs/installation) · [Python setup](https://cyberether.org/docs/python-block#choosing-a-python-runtime) · [Open the flowgraph](mediatek-usb-sdr.yml)**

https://github.com/user-attachments/assets/64e9fbbd-1f16-4c7b-905c-5624b9c574b7

## Get started

1. Install **CyberEther** (https://cyberether.org/) and Python 3.11+.
2. Select that Python under **Settings → Runtime → Python Runtime**, then restart CyberEther if you changed it.
3. Connect a COMFAST MT7961/MT7921AU dongle. Disconnect its Wi-Fi connection first.
4. Download [birriaSDR.yml](birriaSDR.yml) and open it in CyberEther. Allow its declared Python dependencies to install. Stock firmware downloads automatically, with checksum verification and caching.
5. Leave `PYTHON=None`. Edit `CHANNEL` near the top of the Python block to tune, then reload. The default is channel 6 (2437 MHz).

**Linux:** the USB worker uses `sudo -n`; it must have permission to run without a password prompt. With USB permissions already configured, set `USE_SUDO=False`. **macOS:** direct USB access is the default.

One output carries all I/Q samples to the spectrum and waterfall, with both averaging settings at 1. The sampled span is 40 MHz; the receiver filter is currently configured for 20 MHz. Firmware runs in RAM; no flashing or compilation is required to use the flowgraph.

## Optional: rebuild the assembly

Use **GNU Binutils 2.35.2 targeting `nds32le-elf`**. From this folder:

```sh
build_dir=$(mktemp -d)
nds32le-elf-as -EL -march=v3 -mno-all-relax -o "$build_dir/raw_dma.o" raw_dma.S
nds32le-elf-ld --no-relax -e producer -T /dev/stdin \
  -o "$build_dir/raw_dma.elf" "$build_dir/raw_dma.o" <<'EOF'
SECTIONS {
  .entry 0x95c7a0 : { *(.entry) }
  .text  0x96c276 : { *(.text) }
}
EOF
nds32le-elf-objcopy -O binary -j .entry "$build_dir/raw_dma.elf" "$build_dir/entry.bin"
nds32le-elf-objcopy -O binary -j .text "$build_dir/raw_dma.elf" "$build_dir/text.bin"
```

The two `.bin` files total **790 bytes** and reproduce the patch already embedded in the flowgraph. Editing the assembly requires updating the block's `PATCH_RECIPE` and resulting firmware hash; see the [assembly header](raw_dma.S) for details.

## License

Original BirriaSDR code, flowgraph, assembly and documentation are **[MIT licensed](https://opensource.org/license/mit)**. **Use at your own risk. The software is provided as is, without warranty; the MIT terms below include the disclaimer of liability.**

The included driver retains its BSD-3-Clause-Clear notices. Downloaded MediaTek firmware and separately installed dependencies retain their own licenses. Full third-party notices are included near the top of the flowgraph's Python code, after the settings.

## Acknowledgements

BirriaSDR builds on open-source drivers and earlier research into using Wi-Fi hardware for raw I/Q capture. Thanks to:

- [Network Weather’s mt76-usb-macos](https://github.com/Network-Weather/mt76-usb-macos) — the Python userspace USB driver that provided the foundation for BirriaSDR’s device access and firmware loading.
- [mt76](https://github.com/openwrt/mt76) and its contributors — the upstream MediaTek driver implementation underlying that userspace work.
- [W1700K SDR](https://github.com/h0m3us3r/w1700k-sdr) by h0m3us3r — related work turning MediaTek Wi-Fi hardware into a wideband SDR, with useful documentation of capture registers and sample-clock controls.
- [MtkICAPtool](https://github.com/MtkWifiRev/MtkICAPtool) by MtkWifiRev — earlier work exposing MediaTek’s internal I/Q capture capabilities.

Third-party code retains its original licenses and copyright notices.
