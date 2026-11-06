# RUN.md: the ring110 probe on a real console

The probe answers one question (issue #110): when does the cellAudio port's read index advance relative to the
block being read, and when is a consumed block cleared (design: `PROBE.md`). It only calls the stock audio library;
it patches nothing. Layer 1 needs no capture hardware and takes about 15 minutes for the whole set. Layer 2 needs
a line-in recording of the console's audio (section 7); `analyze_layer2.py` reads that recording with the run's CSV.

Build, copy and the console-side mechanics are in `tools/hwprobe/README.md`; commands below assume
`export HWPROBE_CONSOLE=ADDR` and are run from the repository root with `PROBE=tools/hwprobe/probes/ring110`.

## 1. Put the probe on the console

```
tools/hwprobe/build.sh ring110 layer1
tools/hwprobe/ps3ftp.py put tools/hwprobe/out/ring110_l1.fake.self
```

The SELF lands in `/dev_hdd0/tmp/hwprobe/`. Start it from webMAN's file manager or multiMAN (then start the game
icon in the XMB Game column), or with the two URLs in the README. If the fake-signed SELF does not start, upload
`out/ring110_l1.self` (`make_self`) instead.

**Remote start (webMAN MOD 1.47.50, observed 2026-11-04).** `/mount.ps3/dev_hdd0/tmp/hwprobe/<self>` then `/play.ps3`. The
first `/play.ps3` after a probe has finished usually ends in an XMB error `8001003A` ("error during start-up") and no result
file; calling `/play.ps3` a second time about 20 s later starts the probe (12 of 13 launches in one campaign; one first play worked).
A driver should play once, wait about 22 s for a new CSV and play again, and unmount (`/mount.ps3/unmount`) between runs.
Do not play a third time while the probe's screen is up.

## 2. Console state for every run

1. Power off, wait 30 s, power on ("cold boot") before every run marked **cold** in section 4.
2. XMB: stop all music and video, no disc in the drive, no other application.
3. Sign out of PSN (no pop-ups or sounds). The network may stay connected.
4. Settings > Sound Settings > Audio Output Settings: Audio Format **Linear PCM**, 48 kHz, 2 ch, Dolby Digital and
   DTS re-encode **off**. Layer 1 needs no capture: use the output you normally use (HDMI is fine). Leave it
   unchanged between runs and write it down. Do not touch the master volume during a run.
5. Do not use the pad while a run measures (the screen says `measuring (screen frozen)`). Do not open the XMB menu.
6. If webMAN is running, leave it, but do not browse its pages during a run.

## 3. Configuration (before each run)

The probe reads `ring110.cfg` from the root of a USB stick (`/dev_usb000/`) first, otherwise from
`/dev_hdd0/tmp/hwprobe/ring110.cfg`. No file means the defaults (8 blocks, 2 channels, Q4 on every 4th tick, screen
on, START to begin and to leave). Every result records the effective values.

- Set one: `tools/hwprobe/ps3ftp.py put-cfg ring110 $PROBE/cfg/l1_n16.cfg`
- Back to defaults: `tools/hwprobe/ps3ftp.py rm-cfg ring110`
- Declare the console in the same file so it reaches the result header (the probe cannot read it from the console):
  `fw_version=`, `fw_type=`, `console_model=`. Without them the header says `unknown`.

Keys outside the measurement itself: `screen` (1 on-screen status, 0 leaves video untouched), `pad` (1 wait for START
before the run and before exit, 0 unattended), `hold_s` (with `pad=0`, keep the final screen this many seconds).
`cfg/l1_smoke.cfg` is a 2-second unattended run for checking the whole chain.

## 4. Layer 1 runs

Each run is about 12 s of measurement plus start-up. With the default `pad=1` the screen first shows `ready`: press
START. After the run it shows `DONE result: <file>`: press START to leave. (If the screen stays black or the app
does not react for 60 s, hold the PS button and choose Quit Game.)

| # | When | cfg | What it answers |
|---|---|---|---|
| 1 | **cold** | none (defaults) | main run, Q1-Q6 |
| 2 | **cold** | none | repeat, run-to-run agreement of `x_clear` |
| 3 | **cold** | none | repeat |
| 4 | warm (no reboot) | none | phase determinism |
| 5 | warm | `cfg/l1_n16.cfg` | shape independence, 16 blocks |
| 6 | warm | `cfg/l1_c8.cfg` | shape independence, one 8-channel port |
| 7 | warm | `cfg/l1_q4all.cfg` | tag/timestamp relation on every tick |
| 8 | warm | `cfg/l1_norefill.cfg` | no guest writes at all (control C0); do blocks stay zero? |
| 9 | warm | `cfg/l1_dcbf.cfg` | stale-cache check of the index read (only if run 1 looks odd) |
| 10 | warm | the config of run 1 plus `screen=0` | does the video signal change the polling gaps? Compare `max_gap_ticks` and the number of `EV` kind 3 rows with run 1 |

Minimum useful set: runs 1 to 3, 5 and 6.

## 5. Fetch and read the results

```
tools/hwprobe/ps3ftp.py get results --prefix ring110
python3 $PROBE/analyze_layer1.py results/ring110_L1_r001_n8c2.csv
python3 $PROBE/analyze_layer1.py results/ring110_L1_*.csv        # all runs and the spread of x_clear
tools/hwprobe/ps3ftp.py rm-results --prefix ring110              # optional cleanup on the console
```

Sanity checks: `Q1 period` is about 5.333 ms; the warnings block is empty; `abort_reason` is 0; `skips` and
`torn` are 0; `ui_overlaps` is 0. `ring110_last.txt` names the last result file, or the reason there is none.

## 6. What to report

All `results/ring110_*.csv` (a few MB each), the analyzer output, the Audio Output Settings and output device, how
the SELF was started, which runs were cold and which warm, and anything odd (sounds, black screen, warnings).

## 7. Layer 2: analog capture

Layer 2 plays short noise bursts (+-0.5 full scale, `pn.py` for the codes) and needs the console's output recorded.

**What you need.** An AV MULTI cable (the red and white plugs are the audio) into a **USB audio interface with a line
input**: a MacBook's 3.5 mm jack is a headphone/microphone jack, not a line input (wrong level, and macOS may put
AGC or a gate on it). On the interface: line level, no monitoring mix, no phantom power, 48 kHz (Audio MIDI Setup).
`ffmpeg` on the Mac (`which ffmpeg`; `brew install ffmpeg` if it prints nothing) and Python 3; the analysis needs no
other package.

1. **Output:** Audio Output Settings: AV MULTI / SCART only, HDMI and optical unticked (stereo, 48 kHz).
2. **Cabling:** only the left/right audio plugs to the line-in; HDMI audio must not be selected.
3. **Record** one WAV per run, 48 kHz, 24 bit, 2 ch, started at least 5 s before the probe and stopped at least 5 s
   after it returns (the first 3 s must be silent: they set the noise floor and the detection threshold):

   ```
   ffmpeg -f avfoundation -list_devices true -i ""                  # note the index of the interface under "audio devices"
   ffmpeg -f avfoundation -i ":2" -ar 48000 -ac 2 -c:a pcm_s24le run1.wav     # ":2" = that index; Ctrl-C stops
   ```

   macOS asks once for microphone access for the terminal: allow it. An interface with more than two inputs needs
   `-af "pan=stereo|c0=c0|c1=c1"` (the two channels the plugs are on) instead of the bare `-ac 2` downmix. No AGC,
   EQ, enhancement or gate; set the gain once and leave it.
4. **Level check** (once, and after any gain change): put `cfg/l2_c1.cfg` on the console, record the 8 s run, then

   ```
   python3 $PROBE/analyze_layer2.py results/ring110_L2_r001_n8c2_c1.csv c1.wav
   ```

   The line "marker level" should read about -6 dBFS (-12 to -3 is fine), with no "clips" or "is low" warning, a
   marker-to-noise ratio of 40 dB or more, the same reference cosine on both channels, and the control line says
   every write was a hit. Without the probe, `ffmpeg -i c1.wav -af volumedetect -f null -` should show a
   `max_volume` near -6 dB. Change the interface's gain, never the console's volume.
5. The D/A converter adds a constant delay and smears the bursts by tens of microseconds; that cancels in the
   differences the analysis uses. A free-running recorder clock is fine (only differences within 0.2 s are used; the
   analysis reports the drift against the console timebase as a check).
6. **Runs** (about 4 minutes of capture each): `cfg/l2_main.cfg` x3 cold, then once each `l2_shape16.cfg`, `l2_c1.cfg`,
   `l2_c2.cfg`, `l2_c3.cfg`; then a finer pass with `l2_fine.cfg` (edit `omin_ms` / `omax_ms` around the located
   threshold, one run per `jset`). The main grid steps `o` by 0.5 ms, so its `x` is only as sharp as that;
   the 0.1 ms pass is what fixes it to about 0.1 ms. No XMB sounds, trophies or PSN pop-ups during a run.

**Analysis.** Copy the CSV (`ps3ftp.py get results --prefix ring110`) and run, for each run and its recording:

```
python3 $PROBE/analyze_layer2.py results/ring110_L2_r001_n8c2_main.csv main1.wav            # human summary
python3 $PROBE/analyze_layer2.py results/ring110_L2_r001_n8c2_main.csv main1.wav --json     # the same, machine-readable
```

It measures the noise floor on the leading silence, finds every marker by matched filter, joins them to the trials
by code, classifies each trial (hit, miss, or other: no marker, repeating, partial, wrong delay), fits the hit
probability against the actual write offset for each `j`, and prints `x_latch = o50(j) - j*P` with its 95 %
interval, whether the `j` agree within 0.5 ms, the recorder drift, the false alarms of the detector on the
reference-only trials, and the class of PROBE.md section 6. Read the warnings first. If the markers repeat every lap
the hardware does not clear consumed blocks: `--first-occurrence` then scores each trial by its first arrival.
`l2_c2.cfg` (no trials) prints which of its two markers is heard first. Try the analysis without a console:

```
python3 $PROBE/synth_layer2.py /tmp/demo --x-ms 2.6 && python3 $PROBE/analyze_layer2.py /tmp/demo.csv /tmp/demo.wav
```

## 8. If something goes wrong

| Symptom | Likely cause and what to do |
|---|---|
| App starts and returns at once, no CSV | the screen (or `ring110_last.txt`) names the failing call; `audioInit()` failing means the console does not allow this library from homebrew: report it, no further runs |
| Red `ERROR` on the screen | the text says what; `abort_reason` 2: the read index never changed (the audio server did not run the port); 4: `audioPortStart` failed; 5: the polling thread could not be created |
| `abort_reason` 3 | you quit by hand; the data up to then is valid |
| Warnings about polling gaps > 20 us | the polling thread was preempted (webMAN, system tasks): rerun after a cold boot; compare with a `screen=0` run (run 10) |
| `ps3ftp.py` cannot connect | start webMAN/multiMAN FTP, check the address and, on a direct cable, the route (README) |
| No `ring110_*.csv` but `ring110_last.txt` | the write failed and the probe stopped (it writes nowhere but `/dev_hdd0/tmp/hwprobe/`); check free space on the HDD and rerun |
