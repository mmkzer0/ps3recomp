# hwprobe: homebrew probes that settle HLE semantics against a real PS3

Some behaviour of the HLE (when an index advances, when a block is consumed, what a timestamp means) cannot be
read from the firmware or from an emulator; it can only be measured on hardware. `tools/hwprobe/` is the kit for
that: a PPU cross toolchain in a container, a small C library every probe links, a build script that makes a
fake-signed SELF, an FTP helper, a console runner, and per probe the sources, the run instructions and the
host-side analysis. It contains no game binary, no firmware and no title-specific material, and everything except
the console run works on a Mac or Linux host.

The first probe is `ring110` (issue #110): when does the cellAudio port read index advance relative to the block
being read. See `probes/ring110/RUN.md`. The second is `lv2conf` (#96, #114 to #120): lv2 syscall conformance cases
that run on a console and against the toolkit's HLE, with a console run as the golden reference. See
`probes/lv2conf/RUN.md` and `FORMAT.md`.

| Path | What |
|---|---|
| `toolchain/` | `Containerfile`, `build-toolchain.sh` (idempotent, `--check`), `patches/` (the one local deviation) |
| `build.sh <probe> <layer>` | builds `probes/<probe>/<layer>/` in the container to a fake-signed SELF in `out/` (gitignored) |
| `common/` | the library every probe links: timebase, result header and file writer, on-screen status, pad |
| `mk/ppu_probe.mk` | shared make rules; a layer Makefile is five lines |
| `ps3ftp.py` | copy SELFs and configs to the console, fetch and delete result files |
| `ps3console.py` | automated console runner (upload, mount, play, poll with 8001003A retries, fetch, unmount) |
| `hwprobe_format.py` | reader of the result header (used by every probe's analysis) |
| `probes/<probe>/` | one probe: sources, `cfg/`, `RUN.md`, format notes, analysis scripts |
| `probes/lv2conf/` | lv2conf: one set of syscall conformance cases (`cases/`), run on a console by `layer1` (a hwprobe SELF) and on the host against the toolkit's HLE; both write the rows of `FORMAT.md`, so a console run is the golden reference for the toolkit (`RUN.md`) |

Tests (standalone, no toolchain or console): `python3 tools/test_hwprobe_ring110_analyze.py`,
`python3 tools/test_hwprobe_layer2.py`, `python3 tools/test_hwprobe_pn.py`, `python3 tools/test_hwprobe_ps3ftp.py`,
`python3 tools/test_hwprobe_console.py`, `python3 tools/test_hwprobe_lv2conf.py`.

## Prerequisites

- A host with `podman` (macOS: `brew install podman`, `podman machine init && podman machine start`; the machine
  mounts only `/Users`, so the repository and the toolchain prefix must live under it), `git` and Python 3.
- A console that can run an unsigned or fake-signed SELF (CFW or HEN). The probes call the stock system libraries
  and patch nothing. Verified here: a CECHC04 on CFW 4.81 (CEX, Cobra 7.5) with webMAN MOD 1.47.50.
- An FTP server on the console (webMAN MOD and multiMAN both run one on port 21; any login works).
- Console and host on one network. A direct cable works (see "Direct cable" below).

## Toolchain

```
tools/hwprobe/toolchain/build-toolchain.sh --check    # verify only, exit 0 when complete
tools/hwprobe/toolchain/build-toolchain.sh            # build what is missing (about an hour on a cold start)
```

It fetches `ps3dev/ps3toolchain` at `de85d7d5d11dc0a99d823ece3d440eea46c34d0f` (binutils 2.42, gcc 13.2.0, newlib
1.20.0) and `ps3dev/PSL1GHT` at `7e1bd96307eeac8000f18fd75e643d2fa826b5c1`, checks both pins, applies
`toolchain/patches/ps3toolchain-002-gcc-c-only.patch` and builds inside a container (`ubuntu:24.04` plus apt
build tools, native architecture, no emulation). Local deviations, all deliberate: gcc is built for C only
(C++ costs about 1 GB and 10 minutes and no probe uses it), the SPU toolchain, gdb and ps3libraries are skipped,
PSL1GHT builds `install-ctrl`, `ppu` and `tools` only.

The install prefix defaults to `~/ps3-dev/ps3dev-toolchain/ps3dev`; set `PS3DEV` to move it (the sources go next to
it, override with `HWPROBE_TOOLCHAIN_ROOT`). A finished prefix is recognised from its artifacts and left alone: the
script exits 0 without starting podman. A partial one gets only the missing steps. A source tree on another commit
is an error, never reset. The prefix holds linux binaries; on macOS they run only inside the container, which is
why every build goes through `build.sh`.

## Build a probe

```
tools/hwprobe/build.sh ring110 layer1
tools/hwprobe/build.sh ring110 layer2
```

`<probe>/<layer>` is the directory `probes/<probe>/<layer>/`. The script runs `make` in the container and copies
`<TARGET>.fake.self` (`fself`, the one to use) and `<TARGET>.self` (`make_self`) to `tools/hwprobe/out/`, prints
size and SHA-256 of both and writes `out/SHA256SUMS`. It also passes the `git describe` and a hash of every source
file that went into the binary; the probe writes both into its result header, so a result can be tied to the
sources. Two builds of the same sources differ in bytes (the build time is embedded), the source hash is stable.

## Copy, run, fetch, analyze

Everything on the console lives in `/dev_hdd0/tmp/hwprobe/`; `ps3ftp.py` cannot name another place. The console
address is `--host ADDR` or `HWPROBE_CONSOLE`.

```
export HWPROBE_CONSOLE=ADDR
tools/hwprobe/ps3ftp.py put tools/hwprobe/out/ring110_l1.fake.self
tools/hwprobe/ps3ftp.py put-cfg ring110 tools/hwprobe/probes/ring110/cfg/l1_smoke.cfg   # optional, see the probe's RUN.md
# run it (below)
tools/hwprobe/ps3ftp.py get results --prefix ring110
python3 tools/hwprobe/probes/ring110/analyze_layer1.py results/ring110_L1_*.csv
tools/hwprobe/ps3ftp.py rm-results --prefix ring110     # optional cleanup on the console
```

**Running the SELF.** By hand: in webMAN's file manager (`http://ADDR/dev_hdd0/tmp/hwprobe/`) or multiMAN, open the
SELF, then start the game icon that appears in the XMB Game column. Remotely, with webMAN MOD 1.47.50 (this is what
its file manager links to for a SELF; it is not in the web-command reference, check your version):

```
http://ADDR/mount.ps3/dev_hdd0/tmp/hwprobe/ring110_l1.fake.self    # maps the SELF as the disc, "game loaded"
http://ADDR/play.ps3                                               # starts it
http://ADDR/mount.ps3/unmount                                      # afterwards: drop the mapping
```

Nothing here reboots or shuts the console down; do not use `/restart.ps3`, `/reboot.ps3`, `/shutdown.ps3`.
`ps3console.py` (next section) does these steps for you.

### Automated run with ps3console.py

`ps3console.py` automates the entire upload, mount, play, poll, fetch, and unmount cycle:

```
export HWPROBE_CONSOLE=169.254.2.30
tools/hwprobe/ps3console.py run tools/hwprobe/out/lv2conf_l1.fake.self --prefix lv2conf --run-window 30 \
    --cfg lv2conf tools/hwprobe/probes/lv2conf/cfg/full.cfg --out results
```

Options:
- `--run-window S` (**required**): longest time one launch may take until its first result (seconds, known from probe's `RUN.md`). A `/play.ps3` retry is issued only if a full run window passes with zero file changes (recovering from XMB error 8001003A). As soon as any probe activity is detected (`<probe>_runid.txt`, `<probe>_last.txt`, or a result file), the probe is known to be running and `/play.ps3` is never pressed again in that launch.
- `--probe NAME`: probe name for tracking files (`<probe>_runid.txt`, `<probe>_last.txt`). Defaults to `--prefix` (assumption: prefix == probe name, e.g. for `ring110` and `lv2conf`).
- `--cfg NAME FILE`: upload configuration as `/dev_hdd0/tmp/hwprobe/NAME.cfg` before mounting.
- `--runs N`: number of result files expected (default 1). Completion requires that N result files exist AND `<probe>_last.txt` names the last of them, preventing partial fetches while a result file is open.
- `--timeout S`: overall timeout in seconds (default 120s).
- `--out DIR`: local destination directory for fetched result CSVs (default `results/`).

Safety, connection recovery, and allow-list:
- **HTTP allow-list**: `ps3console.py` strictly restricts HTTP requests to `/mount.ps3/dev_hdd0/tmp/hwprobe/<name>`, `/play.ps3`, and `/mount.ps3/unmount`. Refuses destructive endpoints (`/restart.ps3`, `/reboot.ps3`, `/shutdown.ps3`) and path traversals.
- **Connection recovery**: webMAN frequently drops the FTP control connection 4-6s after `/play.ps3` launches a SELF. All poll and fetch operations use an auto-reconnecting wrapper that catches disconnects (`EOFError`, `OSError`, `ftplib.all_errors`) and retries with backoff until `--timeout`.
- **Safe unmount policy**: `/mount.ps3/unmount` is issued **only after** all result files have been verified complete and downloaded. It is **never** called while a probe may be running; if a timeout or error occurs after probe activity has been observed, the image remains mounted (`console left mounted; probe may still be running`) to avoid sending an exit signal to an active probe.

### Direct cable

With the console cabled straight to the host both sides fall back to link-local addresses
(`169.254.x.y`), and macOS may have no usable route to the console's subnet even though the interface is up. Add a
*network* route over the interface that carries the cable:

```
sudo route -n add -net 169.254.2.0/24 -interface en7      # subnet of the console, interface of the cable
```

Use `-net ... -interface`, not `-host`: the console's address is not known to the host in advance, so a route for
the whole link-local subnet over the cable's interface is what makes it reachable. The console's network settings
and `ifconfig` give the addresses; do not scan the network for it.

## The screen, the pad and the measurement integrity rule

The screen is **status only; the result file is the evidence.** A probe shows its title, the configuration name, the
phase (ready, audio setup, measuring, writing, finished), run k/N, the last summary line and, at the end, a green
`DONE result: <path>` or a red `ERROR ...`. Long paths are shortened from the left so the file name stays whole.
START begins a run and leaves the finished screen (`pad=0` in a probe's config runs unattended; `hold_s` keeps the
final screen for some seconds). `screen=0` leaves video untouched.

Drawing costs PPU time and a flip costs RSX activity, so the rule (written at the top of `common/hwprobe.h`) is:

1. A probe brackets every timed section with `hwp_timed_begin()` / `hwp_timed_end()`.
2. The screen is redrawn only between timed sections, or from a lower-priority thread that never runs during one.
   `hwp_ui_redraw()` refuses to draw while a timed section is open and counts the request.
3. If a draw and a timed section overlap anyway, `hwp_timed_begin()` counts it. The result file carries
   `META,ui_draws`, `ui_deferred`, `ui_overlaps`, `ui_timed_sections`, `ui_max_draw_us` and `ui_enabled`; an analysis
   warns when `ui_overlaps` is not 0.
4. The video signal itself (vblank, scan-out) cannot be paused while the screen is on. A probe sensitive to it
   must be run with the screen off as well and the two compared; `ui_enabled` says which was which.

The text is drawn by the PPU into the display buffers with the PSL1GHT debug-font glyphs, not with the RSX shader
renderer of the PSL1GHT sample: that renderer needs shaders compiled by `cgcomp`, which loads NVIDIA's Cg
runtime, and no such runtime exists for the linux/arm64 container. A CPU blit also has no RSX state to get wrong.

## Result header

Every result file starts with a versioned, machine-readable header; the probe's own rows follow unchanged.

```
HWPROBE,1
HDR,probe,<name>
HDR,probe_version,<constant of the probe>
HDR,git_describe,<git describe --always --dirty of the build, or unknown>
HDR,source_sha256,<hash of the sources that went into the binary, or unknown>
HDR,fw_version,<as declared by the operator, or unknown>
HDR,fw_type,<as declared, or unknown>
HDR,console_model,<as declared, or unknown>
HDR,console_info_source,<operator | unavailable>
HDR,config,<configuration name, "default" when none>
HDR,tbfreq,<timebase ticks per second>
```

PSL1GHT at the pinned commit has no firmware or model query, so the three console fields are never guessed: they
are `unknown` unless the probe's config declares them (`fw_version=`, `fw_type=`, `console_model=`), and
`console_info_source` says which. Write them into the config you upload. The format version is bumped when a row
changes meaning; `hwprobe_format.py` (and so every analysis) rejects a file whose first row is not `HWPROBE,<known
version>` with a clear error, as it does a file without the header or with a missing key. Values contain no commas
or line breaks.

## Add a probe

1. `probes/<name>/` with `layer<N>/{Makefile,source/main.c}`; the Makefile sets `TARGET`, `SOURCES`, `INCLUDES`,
   `LIBS` and includes `mk/ppu_probe.mk` (copy `probes/ring110/layer1/Makefile`). `common/` is added for you.
2. In `main`: `hwp_events_init()`, `hwp_ui_init(title, enable)`, `hwp_ui_set_config/phase/run`, `hwp_ui_redraw()`.
   Optionally wait for START with `hwp_wait_button()`.
3. Bracket each timed section with `hwp_timed_begin()` / `hwp_timed_end()`. Log to RAM while timing; no file I/O,
   no printf, no drawing inside.
4. Afterwards: `hwp_next_run_id()`, `hwp_result_open()`, `hwp_write_header()` first, then your rows (put
   `hwp_ui_write_meta()` among your `META` rows), `hwp_result_note_last()`, `hwp_ui_set_done(path)`, `hwp_ui_redraw()`.
5. Config: read `/dev_usb000/<name>.cfg`, then `/dev_hdd0/tmp/hwprobe/<name>.cfg` (see `ring110.c`).
6. A host analysis that starts with `hwprobe_format.parse_header()`, a synthetic fixture generator and a standalone
   `tools/test_hwprobe_<name>_*.py` that needs no toolchain. Document the rows in `probes/<name>/FORMAT.md`.
7. `tools/hwprobe/build.sh <name> layer1`, copy, run, fetch, analyze.

## Provenance and licences

`common/hwprobe_font.h` carries the glyph data of the PSL1GHT debug font (`samples/graphics/debugfont_renderer`),
and the video/flip setup in `common/hwprobe_ui.c` follows `samples/graphics/rsxtest`; both are PSL1GHT code,
MIT licence, Copyright (c) 2011 PSL1GHT Development Team, notice kept in `hwprobe_font.h`.
