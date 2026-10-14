# Structure and extension points

For the receive path and hardware setup, see the [receiver overview](hardware.md)
and [MT7921 notes](mt7921.md).

| File | Responsibility |
| --- | --- |
| `include/birriasdr.h` | Public types, units, lifecycle, documented capabilities and reserved controls |
| `src/device.c` | API validation, requested settings, synchronous/asynchronous entry points |
| `src/usb.c` | Descriptor discovery, board qualification, ownership, libusb register transport |
| `src/io.c` | Endian helpers, monotonic deadlines, bounded register polling |
| `src/usb_stream.c` | Queued libusb transfers, bounded host queue, cancellation/draining |
| `src/stream.c` | Acquisition lifecycle, callback batching, terminal checks and cleanup |
| `src/chips/mt7921_boot.c` | WFSYS reset, ROM command framing, original firmware download |
| `src/chips/mt7921_rf.c` | RF SPI, clocks, bias, synthesizers, receive routing |
| `src/chips/mt7921_gain.c` | Manual gain, field masks, readbacks and baseline restoration |
| `src/chips/mt7921_capture.c` | ICAP configuration, mailbox controls, backend registration |
| `src/chips/mt7921_frame.c` | Original firmware event parsing and raw14-to-I/Q conversion |
| `src/chips/mt7925.c` | Explicit unimplemented backend, with no register writes |
| `firmware/iq_stream.S` | Complete original device program and constants |

The common reader consumes our original firmware's event/mailbox protocol.
Another backend can implement this same protocol on its own hardware. A backend
with a different sample format or mailbox protocol will need an explicit parser
and status adapter before sharing that reader; do not feed a different chipset's
native descriptors into the MT7921 parser.

## Adding a chipset or board

1. Add a backend with its own identity, capabilities, default configuration,
   validation and prepare/start/stop/status/reset operations. MT7925 shows the
   deliberately unsupported starting point.
2. Add known USB IDs to the discovery table as **unqualified**. Enumeration is
   not permission to apply another board's RF register values.
3. Implement its transport differences, original firmware, receive setup and
   packet adaptation. Keep analog die identity, board routing and crystal checks
   in the backend. The existing `usb.c` register transport is the MT76 USB family
   convention, not a universal Wi-Fi protocol.
4. Test ordered register sequences, failure cleanup, framing, cancellation and
   raw sample interpretation without hardware. Then perform cold hardware RF
   validation and only enable qualified IDs/capabilities.
5. Implement reserved public controls only with decoded semantics and evidence.
   Until then return `BIRRIASDR_ERROR_UNSUPPORTED` and keep the capability unset.

Do not add guessed calibration tables, silent vendor-firmware fallback or an
implicit software resampler to make an API request appear successful.

## Error and ownership rules

All errors are public `BIRRIASDR_ERROR_*` values. Transport errors, hardware
timeouts, unexpected state, packet corruption and overrun remain distinguishable.
The final statistics preserve the failed stage and reset error separately.
Primary errors are not replaced by a successful cleanup.

A read owns its handle until return. Only cancellation crosses threads, through
one atomic idle/running/cancelling state. The USB event thread never calls application code or performs
RF configuration. Its bounded queue makes host-consumer overload explicit.
Cancellation drains all submitted transfers before their memory is freed, as
required by the [libusb asynchronous I/O lifecycle](https://libusb.sourceforge.io/api-1.0/group__libusb__asyncio.html).
Application callbacks must not close the handle; cancel, wait for read to return,
then close.

`BS_TRY` is a small early-return helper for straight-line register procedures.
Functions that own resources use a single cleanup path instead. Calibration and
gain restoration attempt all required cleanup writes even after an earlier
failure. Unconditional register tables contain only ordered address/value facts;
conditional logic and settling times stay in named functions.

## Tests and firmware

Normal CTest tests require neither hardware nor privileges. The RF trace fixture
was independently generated from the working Python prototype with deterministic
readbacks; it is not generated from the C code under test. Every one of its 300
writes must match in order. Additional fault tests cover standby readback failure,
wrong die, gain rollback and repeated gain restoration.

`test_hardware --run` is an explicit physical-device test and is not registered
with CTest. It resets the dongle and exercises finite reads, gain-mode changes,
asynchronous callback order and cancellation. It requires the same cold-start
and USB ownership conditions as a real application.

The checked-in image header makes normal builds independent of an NDS32 toolchain
or Python. `tools/rebuild_firmware.py --check` verifies source-to-image agreement;
it assembles only the included original assembly and creates its PDA trailer.
Keep it in sync when changing firmware, and update protocol tests if its ABI changes.
