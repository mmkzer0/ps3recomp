# ring110 CSV format (version 1)

One file per run, `/dev_hdd0/tmp/hwprobe/ring110_<L1|L2>_r<run>_n<blocks>c<channels>[_<tag>].csv` (no other
location: a failed open stops the probe), written after the polling loop ends (no file I/O and no drawing while measuring). The file starts
with the versioned hwprobe header (`HWPROBE,<version>` and `HDR,<key>,<value>` rows, see `tools/hwprobe/README.md`;
`analyze_layer1.py` rejects a file without it or with an unknown version). After the header every line is
`TYPE,field,...`; numbers are decimal unless shown as `0x...`. `tb` is the PPU timebase (`META,tbfreq` ticks per
second, expected 79 800 000; the header's `tbfreq` must agree).

`META` rows added by the hwprobe library: `ui_enabled`, `ui_draws`, `ui_deferred`, `ui_overlaps` (draws that
overlapped the timed section; must be 0), `ui_timed_sections`, `ui_max_draw_us`. `abort_reason`: 0 normal, 1 timeout,
2 no index change, 3 user quit (data valid), 4 `audioPortStart` failed, 5 polling thread not created.

| Row | Fields | Meaning |
|---|---|---|
| `META` | `key,value` | run parameters and start-up observations (port config, `idx_before_start`, `idx_after_start`, `start_tb0/1`, polling priority, `max_gap_ticks`, `abort_reason`, seed, ...). Layer 2 adds `l2_*` rows (grid, presets, marker layout) |
| `SENTWORD` | `block,word` | Layer 1: sentinel bit pattern of each block |
| `FINALWORD` | `block,word` | watched word of each block at the end of the run |
| `SYNC` | `tb_a,systime_us,tb_b` | `sysGetSystemTime()` bracketed by two timebase reads; first row at start, then every 10 s, last at the end |
| `EV` | `tb,kind,idx,hi,blk,a,b,c,zmask` | event log, see below |
| `Q4` | `tick,idx,tb0,st_a,tb1,tb2,st_b,tb3,tag(r-1),tag(r),tag(r+1),ts(r-1),ts(r),ts(r+1),rc_tag x3,rc_ts x3` | `audioGetPortBlockTag` + `audioGetPortTimestamp` at an index change. `tick` is the 1-based ordinal of the matching `EV` kind 0 row. `tb0..tb3` bracket the two `sysGetSystemTime()` reads (`st_a` between `tb0`/`tb1`, `st_b` between `tb2`/`tb3`), the calls run between `tb1` and `tb2` |
| `QT` | `tick,m,tb,tag,ts,rc` | Timestamp window scan, written with each `Q4` row (same `tick`). `audioGetPortTimestamp` for `tag = tag(r) - m * step` with `step = tag(r+1) - tag(r)`, i.e. the tag the block at the index had `m` ticks ago, `m = 0..20`; `tb` = timebase before the call, `ts` in microseconds (0 when `rc` is not 0), `rc` 0 or `0x8031070f` (TAG_NOT_FOUND, `-2144270577` as a signed number). Needed because the firmware refuses the tags of blocks that have not been played yet, so the `ts(r-1..r+1)` columns of `Q4` are refused by construction (see `SPEC-NOTES.md` 14) |
| `QE` | `tb,source,d1,d2,d3,idx_at_wake,rc` | audio notify event as received by the event thread; `idx_at_wake` is the `readIndex` value read right after the wake-up |
| `TRIAL` | `run,trial,j,o_target_ms,o_actual_ms,t_c_tb,t_w_tb,index_r,code,test_blk,ref_blk,t_ref_tb,kind,done` | Layer 2 only. `kind` 0 test, 1 reference-only (then `j=99`, `o=-1`, `code=-1`). `code` is the PN id of the test marker (`pn.py`), the reference marker is id 0 |
| `END` | `n_events,n_dropped` | last row; a file without it is truncated |

`EV` kinds (`blk` = block number, `idx` = `readIndex` value at the time of the record, `hi` = upper 32 bits of the
u64 were nonzero, `zmask` = bit b set while the watched word of block b is zero):

| kind | name | a, b, c |
|---|---|---|
| 0 | `IDX` | index changed to `idx` (timestamp = the poll that first saw it) |
| 1 | `SENT` | watched word of `blk` changed: `a` = new word, `b` = next word, `c` = last word of the block |
| 2 | `REFILL` | Layer 1: the guest rewrote the sentinel of `blk`; `a` = sentinel word |
| 3 | `GAP` | polling gap above 20 us, `a` = gap in ticks (includes the probe's own Q4 calls and refills) |
| 4 | `GWRITE` | Layer 2: marker written into `blk` (`a` = code id, `b` = 0 reference, 1 test); timestamp = after the store and `sync` |
| 5 | `GZERO` | Layer 2: the guest zeroed `blk` after a trial because the hardware had not |

Watched word: Layer 1 = sample 0 / channel 0 (sentinel `0.001f * (b + 1)`, rest of block DC `0.001f`);
Layer 2 = sample 32 / channel 0 (first marker sample, +-0.5, otherwise exact zero).
Words are raw IEEE-754 bit patterns of the big-endian float (the all-zero word is +0.0).

The guest's own writes never produce a `SENT` row (the poller updates its cached word), only `REFILL`,
`GWRITE` or `GZERO`.

## Layer 2 marker code (`common/pn.h`, `pn.py`)

128 samples of +-0.5 on channels 0 and 1, starting at sample 32 of the block. Code `id`:
`s = (id * 2654435761) ^ 0x9E3779B9` (uint32), 8 warm-up xorshift32 steps (`13, 17, 5`), then per sample one step
and `sign = +1 if bit 16 of s else -1`. `tools/test_hwprobe_pn.py` checks that the C header and `pn.py` agree.

## What `analyze_layer2.py` reads

From the CSV: `META,layer` (must be `L2`), `tbfreq`, `blocks`, `abort_reason`, `ev_drop`, `ui_overlaps`, `l2_preset`
(`c1` and `c3` get a control line, `l2_c2` selects the C2 report), the `TRIAL` rows (`j`, `o_actual_ms`, `t_c`, `t_w`,
`code`, `test_blk`, `ref_blk`, `t_ref`, `kind`, `done`), the `EV` kind 1 rows with word 0 (the hardware's clears: block
and time, for `x_clear` without the capture), kind 3 (a polling gap logged at the same timebase value as a trial's
`t_c` makes that trial's `t_c` uncertain) and `END`. From the WAV: 48 kHz integer PCM, 16, 24 or 32 bit, 1 or 2
channels; the first seconds must be silent (`--silence-s`, default 3).
