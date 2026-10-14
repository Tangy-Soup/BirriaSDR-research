# Receiver overview

libbirriasdr configures supported Wi-Fi hardware as a receive-only I/Q source
using libusb. Each chipset backend owns its boot, RF setup, gain and capture
procedures; the common library handles USB transfers and the application API.
See [MT7921 / MT7961 / MT7972](mt7921.md) for the implemented receiver and
[architecture.md](architecture.md) for the source layout and extension points.

## Hardware and software roles

The RF front end tunes and filters the input. The chip's capture engine writes
complex samples into internal memory, and our RAM-resident device program
submits that memory to USB DMA. The host checks the resulting stream and
delivers I/Q to the application. Sample packing, memory layout and register
sequences belong to the chipset backend, not the public API.

No MediaTek RAM firmware or ROM patch is downloaded. The current backend uses
the device's immutable factory ROM and our original program; configuration is
volatile and does not write flash or eFuse.

## Ownership and acquisition

The library requires exclusive use of the USB interface. A read prepares the
radio, queues host transfers, starts capture, then drains and resets on exit.
Settings apply between reads; cancellation is the only operation allowed from
another thread during a read. The [public header](../include/birriasdr.h)
documents buffer ownership, units and error behavior.

Stream checks detect missing, repeated, stale or malformed packets and device
faults. Already delivered samples do not make a failed capture complete, and
overruns are reported rather than hidden by dropping or inventing samples.

Chip recognition does not imply receiver support. Unknown controls return
unsupported, and board qualification remains separate from USB identification.
Follow the [README](../README.md#build-and-try) USB
setup instructions before using the library, or the separate
[CyberEther instructions](../cyberether/README.md) for the Python flowgraph.
