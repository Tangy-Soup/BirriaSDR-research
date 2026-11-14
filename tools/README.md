# tools

| Script | Purpose |
| --- | --- |
| `rebuild_firmware.py` | Optional rebuild and check of the firmware header from the assembly |
| `hw_smoke.sh` | Hardware smoke suite: open, tune in each band, buffered and disk captures, optional both-chain, kill-and-reopen and SoapySDR steps |
| `hw_smoke_soapy.py` | The SoapySDR step of `hw_smoke.sh --soapy` (ranges, dB gain round trip, short streams with host AGC) |
| `soak.sh`, `soak.py` | Long SoapySDR stream that discards samples and logs one JSON line per minute |
| `soak_summary.py` | Reduces a soak's `soak.jsonl` to rates, overflows, gain changes, usbipd memory slope and CPU |
| [`decoders/`](decoders/README.md) | Python Wi-Fi and BLE decoders for recorded or live I/Q |
| [`research_probes/`](research_probes/README.md) | Small programs behind the measurements in [docs/findings.md](../docs/findings.md), outside the library |

The hardware scripts need an MT7921AU (`0e8d:7961`) attached. They wait until no process has had
the device open for 30 s before they start, so they can share a dongle with other users of the
same machine, and they never use sudo.

## Smoke suite

```bash
tools/hw_smoke.sh --installed --soapy
```

- `--build` (default) runs `build/` (or `BIRRIA_BUILD`); `--installed` runs `/usr/local` (or
  `BIRRIA_PREFIX`).
- Default steps use chain 0 and shipped gain codes: 2437 MHz at 40 and 20 MSps, 5240 MHz at
  40 MSps, a 6135 MHz tune, 10 s at 40 MSps and 5 s at 80 MSps to disk with `--recover`, and a
  final open. The 80 MSps step allows up to 8 recovered gaps.
- `--both` adds 1 s per chain with `--chain both`; `--kill` kills a forced-gain stream with
  `kill -9` and checks that the next open is clean; `--soapy` adds the SoapySDR step; `--all`
  adds all three. `--dry-run` prints the commands and changes nothing.
- Logs go to `OUTDIR` (default `./birria_smoke_<stamp>_<mode>`), with `summary.txt` at the end.
  Each recording is deleted after its check, but a disk step needs about 2 GB free while it
  runs. A firmware overrun (state 9) on a step is retried once after 30 s. The script exits 1
  if any step fails.

## Soak

```bash
tools/soak.sh 120 soak_run --freq-mhz 2437
tools/soak_summary.py soak_run
```

`soak.sh [MINUTES [OUTDIR [soak.py options]]]` streams CS16 on chain 0 at 40 MSps through
whichever SoapySDR module is found (set `SOAPY_SDR_PLUGIN_PATH` to test a build tree), with the
default automatic gain. Rates are timed with the realtime clock because WSL2's monotonic clock
has run 2–10% slow against Windows; each minute's line records the monotonic/realtime ratio.
Realtime can be stepped, so trust the whole-run rate over single minutes. Under WSL the soak also
logs usbipd's working set from `tasklist.exe`.
