# Research probes

Small programs used for the measurements in
[docs/findings.md](../../docs/findings.md). Each one wraps the normal MT7921
backend, adds one experiment around an otherwise ordinary finite capture, and
undoes it before the normal teardown. They are not part of the library, its
API or its tests, and they reach into library internals, so they may stop
building as the library changes.

| Probe | What it does | Writes registers? |
| --- | --- | --- |
| `sram_readback_probe.c` | After a finite capture, reads the capture ring back over EP0 so it can be compared with the streamed samples | No |
| `phy_snapshot_probe.c` | After a finite capture, reads MMIO ranges twice (to separate live fields from static ones) | No |
| `capture_tap_probe.c` | Selects another ICAP tap (`0x830ad448[11:8]`) and optionally the 20 MHz bandwidth class or a coarse AGC hold, then restores them | Yes, masked and restored |
| `phy_overlay_probe.c` | Applies a list of `0xADDRESS 0xMASK 0xVALUE` overlays before GO, reads each back, and restores them before reset | Yes, masked and restored |

**Use with care.** These are receive-only, and every write is read back and
restored, but they still change PHY state on hardware the vendor never meant
to run this way:

- Reading an unmapped or clock-gated page can hang the adapter until it is
  replugged (`0x830d0000` did). Read a few words of a new page on its own
  first.
- PHY fields survive a WFSYS reset, so a restore failure leaves the change in
  place until a replug. Stop register work if a probe reports one.
- Broad overlays can fail: applying 321 stock-differing registers at once
  left 2 unrestored.

## Build

From the repository root, after configuring `build/`:

```sh
for p in tools/research_probes/*.c; do
  gcc -O2 -std=gnu11 -Iinclude -Isrc -Ibuild $(pkg-config --cflags libusb-1.0) \
    "$p" src/*.c src/chips/*.c $(pkg-config --libs libusb-1.0) -lpthread -lm \
    -o build/$(basename "$p" .c)
done
```

Run each with no arguments for its usage line.
