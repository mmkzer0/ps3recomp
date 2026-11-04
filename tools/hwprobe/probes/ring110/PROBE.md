# #110 hardware probe spec: what does `*readIndexAddr` mean, and when is a block consumed? (2026-11-02)

Spec of the ring110 probe (its implementation and deviations: `SPEC-NOTES.md`, `FORMAT.md`, `RUN.md`). Target: a PS3
(phat, firmware 4.81 was the first). The firmware's user-port index writer was not found by static reading, so this
probe is the decisive channel.

## 0. Requirement and toolchain

**Console requirement.** Whether the console runs CFW or HEN is unknown to us. The only requirement: it can run an
unsigned PSL1GHT SELF (homebrew) that calls the stock `libaudio.sprx` (`audioInit` and friends). PSL1GHT's own audio
samples (`samples/audio/audiotest`, `audiosync`) are written for exactly that (`063792f` in PSL1GHT history: "libaudio -
does not work yet need to get a dev payload"). No firmware patch of the audio stack, no kernel payload beyond what runs
homebrew. The operator must confirm which environment they have; if neither can run an unsigned SELF, this probe cannot run.

**Toolchain (`tools/hwprobe/toolchain/`; none of this is needed to read the spec).**

| Piece | Source | Notes |
|---|---|---|
| PPU cross compiler and binutils (`powerpc64-ps3-elf-gcc`, newlib) | `github.com/ps3dev/ps3toolchain` (depends on host `autoconf automake bison flex gmp mpfr libelf texinfo wget ...`) | builds on macOS; takes about an hour. Env: `PS3DEV`, `PSL1GHT` |
| PSL1GHT SDK (`make install-ctrl && make && make install`) | `github.com/ps3dev/PSL1GHT`, pin `7e1bd96307eeac8000f18fd75e643d2fa826b5c1` | provides `audio/audio.h`, `sys/event_queue.h`, `sys/systime.h`, `ppu-asm.h` (`mftb`), `sysGetTimebaseFrequency`, `sysGetSystemTime` (`lv2/systime.h:10`), `make` -> `.self`, `make pkg` |
| Missing prototypes | `ppu/sprx/libaudio/exports.h` already exports `audioGetPortTimestamp` (NID `0x4109d08c`) and `audioGetPortBlockTag` (NID `0xe4046afe`), but `audio.h` does not declare them | declare in the probe: `s32 audioGetPortBlockTag(u32 port, u64 block, u64 *tag); s32 audioGetPortTimestamp(u32 port, u64 tag, u64 *stamp);` (no SDK change) |
| Delivery to the console | the fake-signed `.self` over FTP and started from webMAN/multiMAN (`tools/hwprobe/README.md`); `ps3load` or a package would also do, hwprobe builds neither | the operator picks |

Host side: a recorder for the console's audio and Python (`numpy`, `scipy`) for analysis. Both described below; neither exists yet.

## 1. Terms

- `P` = 256 samples / 48000 Hz = **5.3333 ms**, the block period. `n` = block count of the port. A lap = `n*P`
  (`n = 8`: 42.667 ms).
- `t_c(r)`: the PPU timebase time at which a polling loop first sees `*readIndex == r` (the index "changes to r").
  Timebase tick is 12.5 ns (79.8 MHz; read `sysGetTimebaseFrequency()` once and log it). Detection latency is the
  loop period, well below 1 us.
- `latch(b)`: the instant the content of block `b` is frozen for output: whatever the guest writes after it is **not**
  played in this lap.
- `clear(b)`: the instant block `b`'s memory becomes zero, if the system zeroes consumed blocks (RPCS3 and our
  toolkit do; **hardware is unknown**, this is measured).
- **`x` := `latch(r) - t_c(r)`** for the block `r` named by the new index value, in ms. This single number is the
  whole question for emulation purposes (§2).

## 2. What can be separated and what cannot

Block `r+k` is latched at `t_c(r) + x + k*P` (k within the lap), whatever the hypothesis. The hypotheses differ only in `x`:

| Hypothesis | Meaning | `x` |
|---|---|---|
| **B** (RPCS3, our toolkit) | index names the next block to read. The hardware reads `r-1`, clears it, publishes `r` | `x = P` (5.33 ms) |
| **A**, read at the change | index names the block being read; read happens when the index changes | `x ~ 0` |
| **A**, read delayed by half a period | libaudio waits ~2.6 ms after the interval start before it mixes (RPCS3 `cellAudio.cpp:955-956`, hw-derived) | `x ~ 2.6 ms` |
| **B + prefetch** `e` | B, but block `r` was DMA-fetched `e` before its nominal read, so it is already frozen when the guest sees `r` | `x = P - e`; `e = P` gives `x ~ 0`; `e > P` gives `x < 0` |

Consequences, stated plainly:

1. **B is separable from the rest** by a single number: `x ~ P` versus `x < P`.
2. **A and B+prefetch are not separable by anything the guest can do or observe.** A with read delay `delta` and B+prefetch with
   `e = P - delta` produce the same `latch`, the same `clear`, the same write-deadline for every block. They differ only in
   the word's name and in output latency `L` (A: audible = latch + L1; prefetch: audible = latch + e + L0), and
   `L0`, `L1` are not independently known. This probe therefore reports `x` and names the class by it (table in §6);
   "A" versus "B+prefetch" is a naming choice with no consequence for the emulator: publish the index so that block `r` is
   consumed `x` after the publication.
3. What the emulator needs is `x`, plus (for completeness) whether and when consumed blocks are zeroed, and the
   tag/timestamp relation. All three are measured below.

## 3. Setup and port parameters

- Console: quit the XMB's background audio and music; no Blu-ray/disc; network may stay connected. Run the probe from the
  XMB menu with no other game. Audio settings (XMB Settings > Sound Settings > Audio Output Settings): the capture path
  chosen below with **Linear PCM, 48 kHz, 2 ch**; Dolby Digital / DTS re-encode **off** (it adds delay and filters
  markers). Master volume of the capture chain set once and not touched.
- `audioInit()`, then open **one 2-channel port, `numBlocks = AUDIO_BLOCK_8`**, `attrib = AUDIO_PORT_INITLEVEL`,
  `level = 1.0f`, as in the PSL1GHT samples. Repeat the whole protocol with `AUDIO_BLOCK_16` and one 8-channel port
  (§7) to show `x` does not depend on the port shape.
- `audioGetPortConfig` once: log `readIndex` address, `portAddr`, `portSize`, `status`, and the `u64` at `readIndex`
  (before start). Map `readIndex` as `volatile u64 *`; each block is `256 * nChannel * 4` bytes of big-endian `float`.
- Pre-fill: before `audioPortStart`, write every block with a block-unique **sentinel** so consumption is visible in
  memory: sample 0 of channel 0 = `0.001f * (b + 1)`, rest of the block = DC `0.001f` (inaudible, nonzero).
- One dedicated PPU thread does all measurement at the highest priority it can, nothing else running. All logging goes
  to a preallocated RAM array (no printf, no file I/O in the measurement loops); write the CSV to `/dev_hdd0/tmp/` or USB
  after the run. A second low-priority thread owns the notify event queue (§4, Q6) and timestamps arrivals in the same
  timebase.
- Discard the first 100 periods after `audioPortStart` (start-up transients).

## 4. Layer 1: guest-only observations (no audio capture). Run first

One polling loop (~0.3 us/iteration): read `*readIndex`, read the sentinel word of each block (n words), `mftb`. On
any change of the index or of a sentinel word, append `(tb, index, sentinel-bitmask)` to RAM. Refill rule: when a
sentinel block is observed zero (or changed), rewrite its sentinel **only while `(b - r) mod n` is at least 3 and at
most `n-3`** (far from the latch), so the refill never races the hardware.

| # | Observation | Output |
|---|---|---|
| Q1 | period and phase: `t_c(r)` series over 2000 ticks | mean/stddev of `t_c(r+1)-t_c(r)` (expect 5.333 ms), the value of the index after `audioPortStart` returns and at the first change (RPCS3: 0 then 1) |
| Q2 | clear timing: for each index change to `r`, `z_b = time sentinel of b first reads zero` for `b in r-2 .. r+1`; `Delta_r(b) = z_b - t_c(r)` | if the system zeroes consumed blocks: `x_clear := median Delta_r(r)`; also *which block label* zeroes within +-1 ms of `t_c(r)` (predictions in §6). If sentinels never go zero: record "no hardware clear" (important: then a late write is replayed every lap) |
| Q3 | content after read: does a consumed block go to exact 0, stay unchanged, or become something else; is it ever written by the hardware outside the zeroing | histogram of the first sentinel word one lap after consumption |
| Q4 | block tag and timestamp: at each `t_c(r)` call `audioGetPortBlockTag(port, b, &tag_b)` for `b in {r-1, r, r+1}`; `audioGetPortTimestamp(port, tag, &ts)` for the tags in the firmware's accepted window, i.e. `tag_r - m * step` for `m = 0..16` (a tag above the current one, or more than 14 block steps old, is refused, see `SPEC-NOTES.md` 14); log `ts` (microseconds, `sysGetSystemTime` domain) and `sysGetSystemTime()` before/after the calls | `ts - t_c(r)` in us for the accepted `m` (convert `t_c` with one `sysGetSystemTime()` / `mftb` pair taken at start and re-taken every 10 s to track drift); also `tag_{r+1} - tag_r` (one block step: 1 << shift, 2 on the console of the smoke run, not necessarily 1) and whether `tag_r` increments at the same tick as the index |
| Q5 | tick consistency: whether the index ever skips or repeats; whether two reads of the u64 straddle a change (`hi != 0`) | counts |
| Q6 | event order: `audioCreateNotifyEventQueue`/`audioSetNotifyEventQueue`; log `t_ev` of every event; `t_ev - t_c(r)` | RPCS3 sends the event after publishing the index (`cellAudio.cpp:706-745`); sign and size on hardware |

Run length: 2000 ticks (10.7 s) per run. Layer 1 alone answers "which block is cleared at the change" (the sharpest
single label test) and the tag/timestamp relation. It cannot see latching if the hardware does not clear.

## 5. Layer 2: controlled-write sweep with audio capture (deadline `x_latch`)

Goal: measure `x_latch` directly as the largest write offset after the index change that is still heard in the same lap.

**Capture.** Record the console's output at 48 kHz, 24-bit, at least 2 ch, for the whole run plus 5 s each side.
Preferred: optical S/PDIF (Linear PCM 2 ch) into a USB interface; alternatives: HDMI audio extractor set to LPCM 2 ch,
or analogue AV out into a line input. No processing (AGC, EQ, "enhance") on the capture path. The recorder clock need
not lock to the console: the analysis uses only differences of arrival times within 0.2 s.

**Markers.** Unique pseudo-noise (PN) bursts, 128 samples, +-0.5 amplitude, both channels, starting at sample 32 of the
block (so a marker is never split across a block boundary). One PN code (id) per trial and one fixed reference code;
detection by matched filter; false-alarm check on silence recorded before the run. Everything outside the markers is
exact digital silence.

**Trial** (repeated for each condition, see grid):

1. Port running, all blocks zero (the guest zeroes them if the hardware does not).
2. Wait for the index change to `r`; record `t_c(r)`. Immediately write the **reference marker** into block `(r+4) mod n`
   (`n = 8`; for `n = 16` use `r+8`). This is early under every hypothesis (`x + 4P` is far in the future), so it is
   always heard and defines the lap's time axis.
3. Busy-wait to `t_c(r) + o` (timebase; `o` from the grid). Write the **test marker** (unique code, tagged with the trial
   id) into block `(r + j) mod n`. Log `t_w` (timebase after the write) and `o_actual = t_w - t_c(r)`.
4. Stay quiet for at least **4 laps** (>= 170 ms, `n = 8`): no writes, only logging (Layer 1 loop keeps running).
   Then zero the two blocks if the hardware did not.
5. Next trial (random condition order; random extra delay of 0-5.3 ms so trials do not phase-lock to the tick).

**Grid.** `j in {-1, 0, +1, +2}`, `o in {0, 0.5, 1.0, ..., 12.0} ms` (25 values) = 100 conditions, **10 repetitions
each** = 1000 trials, about 190 s of recording per run. Add 200 reference-only trials (no test marker) as the baseline
for the marker detector. Finer grid (0.1 ms) over `o in [x-1, x+1]` for the located threshold in a second pass.

**Reading a trial from the capture.** `Delta := a(test) - a(ref)` (arrival times, ms). The reference block is
`4 - j` periods after the test block in the same lap:

- **hit** (test block still unlatched when written): `Delta = -(4-j) * P`;
- **miss** (block was already latched; the write plays one lap later): `Delta = -(4-j) * P + n * P`;
- anything else, or no marker, or a repeating marker each lap: classify (no hardware clear, dropped write, ...).

The capture latency cancels (differences only). The threshold in `o` where hits turn into misses is `o*(j) = x_latch + j*P`.
Estimate by logistic fit of hit probability against `o_actual`; report the 50 % point and the 10-90 % width (the width
is the guest-visible jitter of `latch` against the index change).

Predicted `o*(j)` is `x + j*P`, so each `j` gives an independent estimate of `x`; they must agree within 0.5 ms
(`j = 0` and `+1` inside the 12 ms sweep when `x <= 6.6 ms`, `j = +2` only when `x <= 1.3 ms`; `j = -1` crosses at `x - P`).

## 6. Expected observations by hypothesis (the table)

`P = 5.33 ms`. "Zeroes" columns assume the system zeroes a block at latch (Q2); if it does not, only Layer 2 columns
apply. All times are relative to `t_c(r)` (the guest sees the index become `r`).

| | **B** (RPCS3 / toolkit) | **A, read at change** | **A, read delayed 2.6 ms** | **B + prefetch e = P/2** | **B + prefetch e = P** | deeper prefetch e > P |
|---|---|---|---|---|---|---|
| `x` (latch of block `r`) | **5.33 ms** | ~0 | ~2.6 ms | 2.67 ms | ~0 | < 0 |
| Q2: block that zeroes within +-1 ms of `t_c` | **`r-1`** (at -0.0..-0.5 ms: RPCS3 clears before publishing) | **`r`** (at 0..+0.5 ms) | none; `r-1` at -2.7 ms, `r` at +2.6 ms | none; `r-1` at -2.7 ms, `r` at +2.7 ms | `r` (~0), identical to A | block `r+k` with `x ~ -kP`, else none; `r` zeroed before `t_c` |
| Q2: `Delta_r(r)` / `Delta_r(r-1)` | `P` / ~0 | ~0 / `-P` | 2.6 / -2.7 | 2.67 / -2.67 | ~0 / `-P` | `<0` / `< -P` |
| Q4: `ts(tag_r) - t_c` if the firmware timestamp means "this block starts playing" (not known; record whatever it is) | ~`P` plus constant | ~0 plus constant | ~2.6 plus constant | ~2.7 plus constant | ~0 plus constant | `< 0` |
| Layer 2: `o*(0)` | **~5.3 ms** | ~0 (writes at o=0 are already late or race) | ~2.6 ms | ~2.7 ms | ~0 | negative: all `j=0` writes miss |
| Layer 2: `o*(1)` | ~10.7 ms | ~5.3 | ~7.9 | ~8.0 | ~5.3 | `P + x` (less than `P`) |
| Layer 2: `j = -1` | **race at o ~ 0, miss for all `o` larger** (block `r-1` is latched at the change) | always miss (latched `P` earlier) | always miss | always miss | always miss | always miss |
| Layer 2: which `j` has hit-rate 50 % near `o ~ 0 +- 0.5` | **`j = -1`** | **`j = 0`** | none (50 % point at o = 2.6 for `j=0`) | none (at 2.7) | **`j = 0`** | `j = -x/P` (e.g. `+1` at `x ~ -P`), else none |
| Late write (miss) | heard one lap later: `Delta = -(4-j)P + 8P` | same | same | same | same | same |

Reading the table into a class (use `x_latch`, 95 % CI from the logistic fits, and `x_clear` from Q2):

| `x` estimate | Class |
|---|---|
| `abs(x - P) < 0.7 ms` | **B**: the toolkit's order is the hardware order. Refusals at `r == w` are real; the cure is producer speed |
| `abs(x) < 0.7 ms` | **A (read at the change), equivalently B + one-period prefetch**. Toolkit should publish the index of the block being read at the instant it is consumed |
| `0.7 < x < P - 0.7` | **intermediate**: A with read delay `x` ≡ B + prefetch `e = P - x`. A value near 2.6 ms would match libaudio's half-interval wait (RPCS3 `cellAudio.cpp:955-956`). Toolkit should publish the index `x` before the block is consumed (`P - x` after the previous consume) |
| `x < -0.7 ms` | deeper prefetch: report the exact `x`; then the guest-visible index runs ahead of the block being consumed by more than a period |

Consistency checks that make the classification trustworthy: `x_clear` within 0.7 ms of `x_latch`; the `o*(j)` for
`j = 0, 1, 2` agree on `x`; the `j = -1` hit-rate transition (if any) is at `o ~ x - P`.

## 7. Repetitions and controls

- **Runs**: 3 independent runs on separate cold boots (power off, 30 s, power on), each Layer 1 (2000 ticks) followed by
  Layer 2 (1000 trials + 200 baseline). Plus one warm re-run to check phase determinism. Each run must agree in `x`
  within the stated tolerance; if the 3 estimates differ by more than 1 ms, `x` is not a constant (the failure mode
  matters more than the mean).
- **Shape independence**: repeat Layer 1 and the `j in {-1, 0}` x `o in 0..6 ms` part of Layer 2 with `n = 16` (2 ch) and with an
  8-channel port (`AUDIO_PORT_8CH`, `n = 8`). `x` should be identical.
- **Controls inside a run**: (C0) no writes for 2000 ticks: period/jitter and index sequence; (C1) a marker written at `o = 3 P`
  with `j = 0` on every trial of one batch (always a hit); (C2) a marker written *before* `audioPortStart` into block 0 and 1:
  which one is heard first and when (index value after start; hardware start phase); (C3) a deliberately late write
  (`o = 20 ms`, `j = 0`) to confirm the lap-late arrival rule and the "cleared after read" rule.
- **Phase control for the event queue** (Q6): 2000 events, `t_ev - t_c` histogram.

## 8. Why this settles #110 and what it does not

- An emulator's order, a homebrew convention and firmware structure are not proof. This probe is the only channel
  that measures the guest-visible write deadline.
- Interpretation for producers that refuse to write block `w` while the index reads `r == w`: the index shows `w` for
  one period, from `t_c(w)` to `t_c(w+1)`. Block `w` is still writable for the first `x` of that period and already
  latched for the rest. A producer that becomes ready inside the first `x` and refuses misses a write that would have
  been in time (an *avoidable* refusal). The toolkit today models `x = P`, so its whole window is avoidable. If the
  hardware has `x = P` (B), a refusal seen in the toolkit is hardware-faithful and the cure is producer speed. If the
  hardware has `x < P`, the avoidable share shrinks to `x / P` of today's window (zero at `x ~ 0`); the rest is an
  artefact of the toolkit publishing the index `P - x` too early relative to the block's consume.
- The probe measures the platform's side only; it does not model any producer's own timing.

## 9. Failure modes and how each shows up

| Failure | Symptom | Handling |
|---|---|---|
| Console cannot run unsigned code, or `libaudio` import fails | `audioInit()` nonzero, black screen | requirement §0 is not met; stop |
| Polling thread preempted (lv2 scheduler, system tasks, XMB overlay) | a gap in the `mftb` series around a change (> 50 us) | log the max gap; drop trials whose `t_c` neighbourhood has a gap > 20 us; raise priority; no other threads |
| `readIndex` read is stale (cached) | index never changes or changes in bursts | volatile `ld` plus `lwsync`/`dcbi` variant as a second run; the page is hardware-coherent on PS3 in the homebrew samples, but confirm by the 5.333 ms cadence |
| Hardware does not zero consumed blocks | sentinels never go zero; late markers repeat every lap | Q2 reports "no clear"; Layer 2 still yields `x_latch`; markers must be cleaned by the guest between trials |
| Hardware writes something else into consumed blocks (fade, tag) | Q3 shows nonzero non-sentinel content | record the pattern (RPCS3 tags blocks with `-0.0f` only under buffering, not a hardware fact) |
| Marker split or truncated at the block boundary | partial correlation peak | markers at sample 32..160 of the block; reject if peak < 0.8 of reference |
| Capture path adds delay variation or drops | `Delta` scatter > 0.3 ms | optical/LPCM first; check the reference-only baseline's `Delta` spread; drop the run if the capture clock slips |
| Output re-encoded (Dolby Digital/DTS) | smeared markers, changed delay | disable in Audio Output Settings; verify with a pure-tone test before the run |
| Start-up transient or first-lap effects | first ~20 periods inconsistent | discard 100 periods after start |
| System sounds or another port mixes into the output | unexpected sounds in capture | no XMB sounds, no trophies, no other audio app; the probe's own port only |
| `x` not constant (drifts or bimodal) | `o*` differs between runs or reps | report both modes; likely a power/clock or scheduling effect; repeat at different times (HDD spin, fan) |
| Timestamps (`ts_b`) not in the `sysGetSystemTime` domain or not monotonic | Q4 garbage | log raw, compare against `sysGetSystemTime` before/after, report the apparent offset |
| Two index reads straddle a change (u64 torn) | hi word nonzero | the u64 is aligned; count and discard |
| Port parameters change `x` | `n = 16` / 8 ch differ | that is a result; report per shape |

## 10. Deliverables for the analysis step

- CSV per run: `run, trial, j, o_target_ms, o_actual_ms, t_c_tb, t_w_tb, index_r`, and one row per Layer-1 event
  `(tb, index, sentinel_mask)`; the timebase frequency; the audio WAV (48 kHz, 24-bit); console model and firmware
  string; Audio Output Settings used; capture path used.
- Python (host): marker detection, `Delta` classification, logistic fit for `o*(j)`, tables 6 and 7 filled with
  measured numbers. Not written here.
- Outcome statement in the form: `x_latch = ... ms [95 % CI], x_clear = ... ms, class = B | A | intermediate | deeper, hw clear =
  yes/no, event order = ...`. That sentence is what settles the index publication order of the HLE.
