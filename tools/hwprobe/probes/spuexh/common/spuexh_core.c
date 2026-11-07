/* spuexh_core.c - see spuexh_core.h. Plain C, no PSL1GHT and no host-only calls: compiled by ppu-gcc and by the host
 * driver. Time comes from spuexh_now(), defined by the layer. */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spuexh_core.h"
#include "spuexh_badprefix_gen.h"

/* Special probe A: the five double-precision compares/test the retail CBE SPU does not execute (decoded as invalid by the
 * toolkit since #124). They are not in the generated op table (no decode-table entry, no harness op), so their words are
 * fixed here. Opcodes of tools/spu_disasm.py: dfceq 0x3C3, dfcmeq 0x3CB, dfcgt 0x2C3, dfcmgt 0x2CB (RR: opcode<<21 | rb<<14 |
 * ra<<7 | rt) and dftsv 0x3BF (RI7: opcode<<21 | i7<<14 | ra<<7 | rt; the toolkit never decoded its I7, the ISA format is RI7).
 * Registers as the sweep's stubs use them: a = $3, b = $4, result in rt = $3. verify_encodings.py assembles these five lines
 * with spu-as and compares them with the words below (it reads this table), so a typo cannot pass. Evidence tier: hardware
 * (CECHC04, 4.81 REBUG, r002 specialA): all five end as an invalid instruction; the expected answer here is NOT_executed, and an
 * `executed` row is a regression of the model that compare_spuexh.py reports. For dftsv the word has i7 = 0; the cases OR in
 * every 7-bit mask at bit 14. */
#define SPUEXH_DPCMP_RT 3
#define SPUEXH_DPTSV_I7_SHIFT 14
static const struct { const char *name; uint32_t word; int tsv; } spuexh_dpcmp[5] = {
    { "dfceq",  0x78610183u, 0 },
    { "dfcmeq", 0x79610183u, 0 },
    { "dfcgt",  0x58610183u, 0 },
    { "dfcmgt", 0x59610183u, 0 },
    { "dftsv",  0x77E00183u, 1 },
};

/* ------------------------------------------------------------------ configuration */
static void cfg_clear(spuexh_cfg_t *c) {
    memset(c, 0, sizeof *c);
    c->magic = W32(SPUEXH_CFG_MAGIC);
    c->stride_all = W32(1u); c->stride_heavy = W32(1u);
    for (int k = 0; k < SPUEXH_NOPS; k++) spuexh_set_op(c, k, 1);
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) spuexh_set_dom(c, d, 1);
}

int spuexh_runcfg_defaults(spuexh_runcfg_t *rc, const char *preset) {
    if (!preset) preset = "smoke";
    memset(rc, 0, sizeof *rc);
    cfg_clear(&rc->wire);
    rc->spus = 6; rc->stall_s = 120; rc->max_faults = 24;
    rc->special_a = 1; rc->special_b = 0; rc->special_b_max = 0; rc->case_timeout_ms = 2000; rc->b_timeout_ms = 600;
    rc->launch_timeout_ms = 10800000; rc->special_b_count = 0; rc->special_b_both = 1;
    rc->sweep = 1;
    snprintf(rc->preset, sizeof rc->preset, "%s", preset);
    if (!strcmp(preset, "smoke")) { rc->wire.bmax = W32(1u); }                       /* block 0 of every domain, every op */
    else if (!strcmp(preset, "standard")) { rc->wire.stride_all = W32(64u); rc->wire.stride_heavy = W32(64u); }
    else if (!strcmp(preset, "full")) { }                                              /* every block, heavy conversions too */
    else return -1;
    return 0;
}

static int dom_index(const char *n) {
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) if (!strcmp(spuexh_dom_names[d], n)) return d;
    return -1;
}
static int name_match(const char *pat, const char *s) {       /* comma list, trailing * = prefix */
    char buf[512]; snprintf(buf, sizeof buf, "%s", pat);
    for (char *t = buf, *e; t; t = e ? e + 1 : NULL) {
        e = strchr(t, ','); if (e) *e = 0;
        size_t n = strlen(t);
        if (n && t[n - 1] == '*') { if (!strncmp(t, s, n - 1)) return 1; }
        else if (n && !strcmp(t, s)) return 1;
        if (!e) break;
    }
    return 0;
}

/* "NAME:S;NAME:S" into a per-domain table */
static int parse_dom_list(const char *val, uint32_t *tab, char *err, size_t cap) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", val);
    for (char *t = buf, *e; t; t = e ? e + 1 : NULL) {
        e = strchr(t, ';'); if (e) *e = 0;
        char *colon = strchr(t, ':');
        if (!colon) { snprintf(err, cap, "expected NAME:N, got '%s'", t); return -1; }
        *colon = 0;
        int d = dom_index(t);
        if (d < 0) { snprintf(err, cap, "unknown domain '%s'", t); return -1; }
        tab[d] = W32((uint32_t)atoi(colon + 1));
        if (!e) break;
    }
    return 0;
}

static int parse_uint(const char *s, uint64_t max, uint64_t *out) {          /* decimal digits only, no sign, no trailing text */
    char *end;
    if (*s < '0' || *s > '9') return -1;
    const unsigned long long v = strtoull(s, &end, 10);
    if (*end || v > max) return -1;
    *out = v;
    return 0;
}
static int parse_cfg_int(const char *s, int allow_zero, const char *key, int *out, char *err, size_t cap) {
    uint64_t v;
    if (parse_uint(s, INT_MAX, &v) < 0 || (!allow_zero && v == 0)) {
        snprintf(err, cap, "%s must be %s and decimal-only (<= %d)", key, allow_zero ? ">= 0" : "> 0", INT_MAX);
        return -1;
    }
    *out = (int)v;
    return 0;
}


/* Splits "a:b:c" in place into at most `max` fields; returns the field count, or -1 when there are more. */
static int split_colon(char *buf, char **f, int max) {
    int nf = 0;
    for (char *t = buf;;) {
        if (nf == max) return -1;
        f[nf++] = t;
        char *c = strchr(t, ':');
        if (!c) return nf;
        *c = 0; t = c + 1;
    }
}

/* The op of a lanes=/lanesum= entry: a table op, or a conversion at ANY I8 scale ("cflts@45": the instruction takes 0..255,
 * the sweep only the twelve scales of its table), which runs the table's entry of that family with the scale replaced.
 * *op is a table index, *scale the replacing scale or -1 for the op's own. */
static int lane_resolve_op(const char *name, int *op, int *scale) {
    for (int k = 0; k < SPUEXH_NOPS; k++) if (!strcmp(spuexh_ops[k].name, name)) { *op = k; *scale = -1; return 0; }
    const char *at = strchr(name, '@');
    uint64_t s;
    if (!at || at == name || parse_uint(at + 1, 255, &s) < 0) return -1;
    for (int k = 0; k < SPUEXH_NOPS; k++)
        if (!strncmp(spuexh_ops[k].name, name, (size_t)(at - name) + 1) && spuexh_ops[k].fixed_imm >= 0 && spuexh_ops[k].imm_bits == 8) {
            *op = k; *scale = (int)s;
            return 0;
        }
    return -1;
}

static void lane_name(char *dst, size_t cap, int op, int scale) {
    const char *n = spuexh_ops[op].name;
    if (scale < 0) snprintf(dst, cap, "%s", n);
    else snprintf(dst, cap, "%.*s@%d", (int)(strchr(n, '@') - n), n, scale);
}

/* OP:DOMAIN:BLOCK of a lanes=/lanesum= key. The op must be one the domain runs and the block must exist, so a typo fails at
 * start-up instead of dumping inputs the sweep never sees. Fills the number of calls of the block. */
static int lane_target(const char *what, char **f, int *op, int *scale, int *dom, uint64_t *blk, uint64_t *calls, char *err, size_t cap) {
    if (lane_resolve_op(f[0], op, scale) < 0) { snprintf(err, cap, "%s: unknown op '%s'", what, f[0]); return -1; }
    *dom = dom_index(f[1]);
    if (*dom < 0) { snprintf(err, cap, "%s: unknown domain '%s'", what, f[1]); return -1; }
    int member = 0;
    for (int q = 0; q < 5; q++) member |= spuexh_ops[*op].dom[q] == *dom;
    if (!member) { snprintf(err, cap, "%s: op '%s' is not run in domain '%s'", what, f[0], f[1]); return -1; }
    const uint64_t nb = exh_nblocks(&exh_doms[*dom]);
    if (parse_uint(f[2], nb - 1, blk) < 0) {
        snprintf(err, cap, "%s: block '%s' is not a block number of domain '%s' (0..%llu)", what, f[2], f[1], (unsigned long long)(nb - 1));
        return -1;
    }
    *calls = exh_doms[*dom].ncalls - (*blk << EXH_BLOCK_LOG2);
    if (*calls > EXH_BLOCK) *calls = EXH_BLOCK;
    return 0;
}

/* lanes=OP:DOMAIN:BLOCK[:COUNT[:START]] (op names such as cflts@173 and domain names contain no colon): COUNT calls from call
 * START of the block (defaults 64 and 0); START + COUNT must lie inside the block. */
static int parse_lanes(spuexh_runcfg_t *rc, const char *val, char *err, size_t cap) {
    char buf[96], *f[5];
    if (strlen(val) >= sizeof buf) { snprintf(err, cap, "lanes entry too long"); return -1; }
    snprintf(buf, sizeof buf, "%s", val);
    const int nf = split_colon(buf, f, 5);
    if (nf < 3) { snprintf(err, cap, "lanes=%s: expected OP:DOMAIN:BLOCK[:COUNT[:START]]", val); return -1; }
    if (rc->nlanes >= SPUEXH_LANES_MAX_ENTRIES) { snprintf(err, cap, "at most %d lanes entries", SPUEXH_LANES_MAX_ENTRIES); return -1; }
    int op, scale, dom;
    uint64_t blk, calls, count = SPUEXH_LANES_DEFAULT_COUNT, start = 0;
    if (lane_target("lanes", f, &op, &scale, &dom, &blk, &calls, err, cap) < 0) return -1;
    if (nf >= 4 && (parse_uint(f[3], SPUEXH_LANES_MAX_COUNT, &count) < 0 || !count)) {
        snprintf(err, cap, "lanes: count '%s' must be 1..%d", f[3], SPUEXH_LANES_MAX_COUNT);
        return -1;
    }
    if (nf == 5 && parse_uint(f[4], calls - 1, &start) < 0) {
        snprintf(err, cap, "lanes: start '%s' must be 0..%llu (the block has %llu calls)", f[4], (unsigned long long)(calls - 1), (unsigned long long)calls);
        return -1;
    }
    if (start + count > calls) {
        snprintf(err, cap, "lanes: start %llu + count %llu exceeds the %llu calls of block %llu", (unsigned long long)start,
                 (unsigned long long)count, (unsigned long long)calls, (unsigned long long)blk);
        return -1;
    }
    rc->lanes[rc->nlanes].op = op; rc->lanes[rc->nlanes].scale = scale; rc->lanes[rc->nlanes].dom = dom;
    rc->lanes[rc->nlanes].blk = (uint32_t)blk; rc->lanes[rc->nlanes].count = (int)count; rc->lanes[rc->nlanes].start = (uint32_t)start;
    rc->nlanes++;
    return 0;
}

/* lanesum=OP:DOMAIN:BLOCK:CHUNK: one hash per chunk of CHUNK calls over the whole block, at most SPUEXH_LANESUM_MAX_CHUNKS chunks. */
static int parse_lanesum(spuexh_runcfg_t *rc, const char *val, char *err, size_t cap) {
    char buf[96], *f[4];
    if (strlen(val) >= sizeof buf) { snprintf(err, cap, "lanesum entry too long"); return -1; }
    snprintf(buf, sizeof buf, "%s", val);
    if (split_colon(buf, f, 4) != 4) { snprintf(err, cap, "lanesum=%s: expected OP:DOMAIN:BLOCK:CHUNK", val); return -1; }
    if (rc->nsums >= SPUEXH_LANESUM_MAX_ENTRIES) { snprintf(err, cap, "at most %d lanesum entries", SPUEXH_LANESUM_MAX_ENTRIES); return -1; }
    int op, scale, dom;
    uint64_t blk, calls, chunk;
    if (lane_target("lanesum", f, &op, &scale, &dom, &blk, &calls, err, cap) < 0) return -1;
    if (parse_uint(f[3], calls, &chunk) < 0 || !chunk) {
        snprintf(err, cap, "lanesum: chunk '%s' must be 1..%llu (the block has that many calls)", f[3], (unsigned long long)calls);
        return -1;
    }
    if ((calls + chunk - 1) / chunk > SPUEXH_LANESUM_MAX_CHUNKS) {
        snprintf(err, cap, "lanesum: chunk %llu makes %llu chunks of the block's %llu calls (at most %d)", (unsigned long long)chunk,
                 (unsigned long long)((calls + chunk - 1) / chunk), (unsigned long long)calls, SPUEXH_LANESUM_MAX_CHUNKS);
        return -1;
    }
    rc->sums[rc->nsums].op = op; rc->sums[rc->nsums].scale = scale; rc->sums[rc->nsums].dom = dom;
    rc->sums[rc->nsums].blk = (uint32_t)blk; rc->sums[rc->nsums].chunk = (uint32_t)chunk;
    rc->nsums++;
    return 0;
}

/* The keys the layer around the sweep handles itself (RUN.md section 3): runcfg_set reports them as "not mine" (0). Every other
 * key that is not a sweep key is a typo or a stale config and fails closed: a misspelt safety key must never fall back to its default. */
static int is_console_key(const char *k) {
    static const char *const keys[] = { "pad", "hold_s", "screen", "tag", "outdir", "fw_version", "fw_type", "console_model", "progress_file" };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) if (!strcmp(k, keys[i])) return 1;
    return 0;
}

/* A 0/1 switch: anything else ("ture", "2", "") is an error, not a silent 0 */
static int parse_flag(const char *v, int *out, const char *key, char *err, size_t cap) {
    if ((v[0] == '0' || v[0] == '1') && !v[1]) { *out = v[0] - '0'; return 0; }
    snprintf(err, cap, "%s must be 0 or 1, got '%s'", key, v);
    return -1;
}

int spuexh_runcfg_set(spuexh_runcfg_t *rc, const char *k, const char *v, char *err, size_t cap) {
    err[0] = 0;
    if (!strcmp(k, "preset")) {
        if (spuexh_runcfg_defaults(rc, v) < 0) { snprintf(err, cap, "unknown preset '%s' (smoke, standard, full)", v); return -1; }
    }
    else if (!strcmp(k, "spus")) { rc->spus = atoi(v); if (rc->spus < 1 || rc->spus > 6) { snprintf(err, cap, "spus must be 1..6"); return -1; } }
    else if (!strcmp(k, "stall_s")) { if (parse_cfg_int(v, 1, k, &rc->stall_s, err, cap) < 0) return -1; }
    else if (!strcmp(k, "max_faults")) { rc->max_faults = atoi(v); if (rc->max_faults < 0) { snprintf(err, cap, "max_faults must be >= 0"); return -1; } }
    else if (!strcmp(k, "special_a")) { if (parse_flag(v, &rc->special_a, "special_a", err, cap) < 0) return -1; }
    else if (!strcmp(k, "special_b")) { if (parse_flag(v, &rc->special_b, "special_b", err, cap) < 0) return -1; }
    else if (!strcmp(k, "special_b_max")) { if (parse_cfg_int(v, 1, k, &rc->special_b_max, err, cap) < 0) return -1; }
    else if (!strcmp(k, "special_b_from")) { if (parse_cfg_int(v, 1, k, &rc->special_b_from, err, cap) < 0) return -1; }
    else if (!strcmp(k, "special_b_count")) { if (parse_cfg_int(v, 1, k, &rc->special_b_count, err, cap) < 0) return -1; }
    else if (!strcmp(k, "special_b_patterns")) {
        if (!strcmp(v, "both")) rc->special_b_both = 1;
        else if (!strcmp(v, "zero")) rc->special_b_both = 0;
        else { snprintf(err, cap, "special_b_patterns must be zero or both, got '%s'", v); return -1; }
    }
    else if (!strcmp(k, "a_ops")) snprintf(rc->a_ops, sizeof rc->a_ops, "%s", v);
    /* every launch kind is bounded: 0 or a negative limit would be an unbounded launch (RUN.md policy) */
    else if (!strcmp(k, "case_timeout_ms")) { if (parse_cfg_int(v, 0, k, &rc->case_timeout_ms, err, cap) < 0) return -1; }
    else if (!strcmp(k, "b_timeout_ms")) { if (parse_cfg_int(v, 0, k, &rc->b_timeout_ms, err, cap) < 0) return -1; }
    else if (!strcmp(k, "launch_timeout_ms")) { if (parse_cfg_int(v, 0, k, &rc->launch_timeout_ms, err, cap) < 0) return -1; }
    else if (!strcmp(k, "sweep")) { if (parse_flag(v, &rc->sweep, "sweep", err, cap) < 0) return -1; }
    else if (!strcmp(k, "stride_all")) rc->wire.stride_all = W32((uint32_t)atoi(v));
    else if (!strcmp(k, "stride_heavy")) rc->wire.stride_heavy = W32((uint32_t)atoi(v));
    else if (!strcmp(k, "bmax")) rc->wire.bmax = W32((uint32_t)atoi(v));
    else if (!strcmp(k, "stride_dom")) return parse_dom_list(v, rc->wire.dom_stride, err, cap) < 0 ? -1 : 1;
    else if (!strcmp(k, "bmax_dom")) return parse_dom_list(v, rc->wire.dom_bmax, err, cap) < 0 ? -1 : 1;
    else if (!strcmp(k, "lanes")) return parse_lanes(rc, v, err, cap) < 0 ? -1 : 1;
    else if (!strcmp(k, "lanesum")) return parse_lanesum(rc, v, err, cap) < 0 ? -1 : 1;
    else if (!strcmp(k, "ops")) snprintf(rc->ops, sizeof rc->ops, "%s", v);
    else if (!strcmp(k, "skip_ops")) snprintf(rc->skip_ops, sizeof rc->skip_ops, "%s", v);
    else if (!strcmp(k, "doms")) snprintf(rc->doms, sizeof rc->doms, "%s", v);
    else if (is_console_key(k)) return 0;
    else { snprintf(err, cap, "unknown key '%s'", k); return -1; }
    return 1;
}

int spuexh_runcfg_finish(spuexh_runcfg_t *rc, char *err, size_t cap) {
    err[0] = 0;
    const int b_total = (rc->special_b_max > 0 && rc->special_b_max < SPUEXH_NBAD) ? rc->special_b_max : SPUEXH_NBAD;
    if (rc->special_b_from >= b_total) {
        snprintf(err, cap, "special_b_from must be < %d (prefix total)", b_total);
        return -1;
    }
    int nop = 0, ndom = 0;
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) {
        int on = !rc->doms[0] || name_match(rc->doms, spuexh_dom_names[d]);
        spuexh_set_dom(&rc->wire, d, on); ndom += on;
    }
    for (int k = 0; k < SPUEXH_NOPS; k++) {
        const char *n = spuexh_ops[k].name;
        int on = (!rc->ops[0] || name_match(rc->ops, n)) && !(rc->skip_ops[0] && name_match(rc->skip_ops, n));
        spuexh_set_op(&rc->wire, k, on); nop += on;
    }
    if (!ndom) { snprintf(err, cap, "doms='%s' matches no domain", rc->doms); return -1; }
    if (!nop) { snprintf(err, cap, "ops='%s' skip_ops='%s' leave no op", rc->ops, rc->skip_ops); return -1; }
    return 0;
}

int spuexh_outdir_ok(const char *dir) {
    static const char root[] = "/dev_hdd0/tmp/hwprobe";
    const size_t rl = sizeof root - 1;
    if (!dir[0] || strncmp(dir, root, rl)) return 0;
    if (dir[rl] && dir[rl] != '/') return 0;                 /* not /dev_hdd0/tmp/hwprobeX */
    /* no ".." path component: the results must stay inside the assigned directory */
    for (const char *p = dir;;) {
        while (*p == '/') p++;
        if (!*p) return 1;
        const char *e = strchr(p, '/');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!e) return 1;
        p = e + 1;
    }
}

void spuexh_plan(const spuexh_cfg_t *c, spuexh_plan_t *p) {
    uint16_t act[SPUEXH_NOPS];
    memset(p, 0, sizeof *p);
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) {
        if (!spuexh_dom_enabled(c, d)) continue;
        const uint64_t nb = exh_nblocks(&exh_doms[d]);
        for (uint64_t b = 0; b < nb; b++) {
            const int na = spuexh_job_ops(c, d, b, act);
            if (!na) continue;
            uint64_t lo = b << EXH_BLOCK_LOG2, hi = lo + EXH_BLOCK; if (hi > exh_doms[d].ncalls) hi = exh_doms[d].ncalls;
            p->jobs++; p->hashes += (uint64_t)na; p->calls += (uint64_t)na * (hi - lo); p->gen_calls += hi - lo;
        }
    }
}

/* ------------------------------------------------------------------ the sweep */
typedef struct {
    spuexh_backend_t *be;
    const spuexh_runcfg_t *rc;
    const spuexh_sink_t *sink;
    int n;                                   /* spu_count of this sweep */
    uint64_t cfg_ea;
    spuexh_params_t *par[6];  uint64_t par_ea[6];
    volatile spuexh_status_t *st[6];  uint64_t st_ea[6];
    spuexh_entry_t *reg[6];   uint64_t reg_ea[6];  uint32_t cap[6];
    uint32_t drained[6];
    uint32_t start_job[6], start_entries[6];
    int finished[6], dead[6], faults_seen[6];
    uint64_t hashes;
    uint64_t dom_ticks[SPUEXH_NDOMS_GEN], dom_jobs[SPUEXH_NDOMS_GEN];
    uint64_t jobs_total;
    uint64_t launches, timing_overflows;
    double t_change, t0, t_ui;
    uint64_t last_sig;
    int stalled, stopped;
} sweep_t;

static void drain(sweep_t *s, int i) {
    __sync_synchronize();
    uint32_t n = W32(s->st[i]->entries);
    __sync_synchronize();
    if (n > s->cap[i]) n = s->cap[i];
    for (uint32_t k = s->drained[i]; k < n; k++) {
        const spuexh_entry_t *e = &s->reg[i][k];
        const unsigned op = W16(e->op), dom = W16(e->dom);
        const uint64_t blk = W32(e->blk), h = W64(e->hash);
        if (dom >= SPUEXH_NDOMS_GEN) continue;
        if (op == SPUEXH_ENT_GEN) { fprintf(s->sink->out, "#gencheck\t%s\t%016llx\n", spuexh_dom_names[dom], (unsigned long long)h); continue; }
        if (op == SPUEXH_ENT_TIME) {
            if (h == SPUEXH_TIME_OVERFLOW) {
                /* the job ran past one decrementer period (~53.8 s at 79.8 MHz): the tick count is not a number */
                fprintf(s->sink->out, "TIMING_OVERFLOW,%s,%llu\n", spuexh_dom_names[dom], (unsigned long long)blk);
                s->timing_overflows++;
            } else {
                s->dom_ticks[dom] += h;
            }
            s->dom_jobs[dom]++; s->jobs_total++;
            continue;
        }
        if (op >= SPUEXH_NOPS) continue;
        fprintf(s->sink->out, "%s\t%s\t%llu\t%016llx\n", spuexh_ops[op].name, spuexh_dom_names[dom],
                (unsigned long long)blk, (unsigned long long)h);
        s->hashes++;
    }
    if (n > s->drained[i]) s->drained[i] = n;     /* never backwards: a relaunched SPU reports 0 until its first flush */
}

static int sweep_progress(void *u) {
    sweep_t *s = (sweep_t *)u;
    uint64_t sig = 0;
    for (int i = 0; i < s->n; i++) {
        if (s->finished[i] || s->dead[i]) continue;
        drain(s, i);
        sig += W32(s->st[i]->jobs_started) * 3ull + W32(s->st[i]->jobs_done) * 5ull + W32(s->st[i]->entries) * 7ull +
               W64(s->st[i]->heartbeat) * 11ull + W32(s->st[i]->state);
    }
    const double now = spuexh_now();
    if (sig != s->last_sig) { s->last_sig = sig; s->t_change = now; }
    else if (s->rc->stall_s > 0 && now - s->t_change > s->rc->stall_s) { s->stalled = 1; return 1; }
    if (now - s->t_ui >= 1.0) {
        s->t_ui = now;
        fflush(s->sink->out);
        if (s->sink->ui) {
            char b[160]; snprintf(b, sizeof b, "hashes=%llu jobs=%llu %.0fs", (unsigned long long)s->hashes,
                                  (unsigned long long)s->jobs_total, now - s->t0);
            if (s->sink->ui(s->sink->uu, b)) { s->stopped = 1; return 1; }
        }
    }
    return 0;
}

static void clean(char *dst, size_t cap, const char *src) {          /* a field of a CSV row: no comma, tab or line break */
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = (src[i] == ',' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r') ? ' ' : src[i];
    dst[i] = 0;
}

/* META rows that describe how a launch ended, written once per launch right after the backend returned:
 *   META,terminate_join,launchN       the backend had to terminate the group before it could be joined (the group had
 *                                     not ended by itself, or an exception event arrived)
 *   META,exit_status_pending,0xCODE   a thread had not exited and sysSpuThreadGetExitStatus returned CODE (first
 *                                     occurrence per code; the codes document lv2's running/stopped-thread semantics,
 *                                     which no source in this tree settles)
 * g_launch_seq numbers every launch of the process (special probes and sweep together), so launchN is unique per file. */
static uint64_t g_launch_seq;
static uint32_t g_pending_codes[8];
static int g_npending_codes;

static void launch_meta(FILE *f, uint64_t launch, const spuexh_thread_res_t *tr, int n) {
    int tj = 0;
    for (int i = 0; i < n; i++) tj |= tr[i].terminate_join != 0;
    if (tj) fprintf(f, "META,terminate_join,launch%llu\n", (unsigned long long)launch);
    for (int i = 0; i < n; i++) {
        const uint32_t code = (uint32_t)tr[i].exit_status_pending;
        if (!code) continue;
        int seen = 0;
        for (int k = 0; k < g_npending_codes; k++) if (g_pending_codes[k] == code) seen = 1;
        if (seen) continue;
        if (g_npending_codes < (int)(sizeof g_pending_codes / sizeof g_pending_codes[0])) g_pending_codes[g_npending_codes++] = code;
        fprintf(f, "META,exit_status_pending,0x%08x\n", (unsigned)code);
    }
}

/* One thread-group launch of the SPUs in idx[0..n): fills the params blocks and runs the backend with the configured
 * launch limit (always positive: no unbounded launch). After the launch, launch_meta() records what the backend had to
 * do to join and one META row per thread records the FPSCR the SPU program read back after setting it to the defined
 * state at start (SPU ISA v1.2 section 9.3). Returns the rv. */
static int sweep_launch(sweep_t *s, const int *idx, int n, spuexh_thread_res_t *tr) {
    uint64_t pea[6];
    const volatile spuexh_status_t *stp[6];
    for (int q = 0; q < n; q++) {
        const int i = idx[q];
        spuexh_params_t *p = s->par[i];
        memset(p, 0, sizeof *p);
        p->magic = W32(SPUEXH_PARAMS_MAGIC); p->mode = W32(SPUEXH_MODE_MAIN);
        p->spu_index = W32((uint32_t)i); p->spu_count = W32((uint32_t)s->n);
        p->start_job = W32(s->start_job[i]); p->results_cap = W32(s->cap[i]); p->start_entries = W32(s->start_entries[i]);
        p->results_ea = W64(s->reg_ea[i]); p->status_ea = W64(s->st_ea[i]); p->cfg_ea = W64(s->cfg_ea);
        p->cfg_size = W32((uint32_t)sizeof(spuexh_cfg_t));
        memset((void *)s->st[i], 0, sizeof(spuexh_status_t));
        pea[q] = s->par_ea[i]; stp[q] = s->st[i];
    }
    __sync_synchronize();
    memset(tr, 0, sizeof(spuexh_thread_res_t) * 6);
    s->t_change = spuexh_now(); s->t_ui = 0; s->stalled = 0;
    const int rv = s->be->run(s->be, n, pea, stp, sweep_progress, s, s->rc->launch_timeout_ms, tr);
    s->launches++;
    const uint64_t ln = ++g_launch_seq;
    launch_meta(s->sink->out, ln, tr, n);
    for (int q = 0; q < n; q++) {
        const volatile spuexh_status_t *st = s->st[idx[q]];
        fprintf(s->sink->out, "META,launch%llu_spu%d_fpscr,%08x.%08x.%08x.%08x\n", (unsigned long long)ln, idx[q],
                W32(st->fpscr_start[0]), W32(st->fpscr_start[1]), W32(st->fpscr_start[2]), W32(st->fpscr_start[3]));
    }
    return rv;
}

/* The FAULT row of a worker that did not finish: the job it was running, its state word and what the backend reported.
 * The job is skipped (start_job moves past it); a worker that keeps losing jobs without starting a new one is marked dead. */
static void sweep_fault_row(sweep_t *s, int i, const spuexh_thread_res_t *tr, FILE *f) {
    const uint32_t started = W32(s->st[i]->jobs_started);
    const uint32_t dom = W32(s->st[i]->last_dom), blk = W32(s->st[i]->last_blk);
    char d1[200];
    clean(d1, sizeof d1, tr->desc);
    fprintf(f, "FAULT,spu%d,%s,%u,job_%u,state_%u,%s%s\n", i,
            dom < SPUEXH_NDOMS_GEN ? spuexh_dom_names[dom] : "none", blk, started, W32(s->st[i]->state),
            s->stalled || tr->timed_out ? "hang:" : "", d1);
    s->faults_seen[i]++;
    if (started > s->start_job[i]) s->start_job[i] = started;
    else if (s->faults_seen[i] > 2) { s->dead[i] = 1; fprintf(f, "META,spu%d_dead,no_progress\n", i); }
    s->start_entries[i] = s->drained[i];
}

/* A group-ending fault or kill describes every unfinished thread with the same event, so a healthy worker may have been
 * collateral. Re-run this worker's unfinished jobs once, alone (single-SPU launch from the interrupted job): only a job
 * that faults there is a FAULT; one that completes is a RETRY, and its hashes are kept. Returns -1 on FATAL. */
static int sweep_verify_alone(sweep_t *s, int i, FILE *f, spuexh_sweep_res_t *res, char *fatal_desc, size_t dcap) {
    const int idx[1] = { i };
    spuexh_thread_res_t tr[6];
    const uint32_t interrupted = W32(s->st[i]->jobs_started);    /* the job that was running when the group died */
    const int rv = sweep_launch(s, idx, 1, tr);
    if (rv == -3) {
        clean(fatal_desc, dcap, tr[0].desc);
        return -1;                               /* uncontainable: the caller stops the whole probe */
    }
    if (rv == -2) {                              /* the launch never ran: retry the job, it is not known to be bad */
        s->stalled = 0;
        return 0;
    }
    drain(s, i);
    if (rv < 0) {                                /* backend refused the launch: keep the skip-job behaviour */
        sweep_fault_row(s, i, &tr[0], f);
        res->faults++;
        return 0;
    }
    if (W32(s->st[i]->state) == 2) {
        fprintf(f, "RETRY,spu%d,job_%u,ok\n", i, interrupted);
        s->finished[i] = 1;
        return 0;
    }
    sweep_fault_row(s, i, &tr[0], f);            /* it faults alone: this job is the FAULT */
    res->faults++;
    return 0;
}

int spuexh_run_sweep(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink, spuexh_sweep_res_t *res) {
    static sweep_t S;                                       /* large; one sweep at a time */
    memset(&S, 0, sizeof S);
    memset(res, 0, sizeof *res);
    S.be = be; S.rc = rc; S.sink = sink;
    const double t_begin = spuexh_now();
    uint16_t act[SPUEXH_NOPS];

    for (int spus = rc->spus; spus >= 1; spus--) {
        /* ---- plan: entries each SPU will produce (upper bound = every job completes) */
        uint64_t ents[6] = {0}; uint32_t njobs[6] = {0}; uint32_t job = 0;
        for (int d = 0; d < SPUEXH_NDOMS_GEN; d++) {
            if (!spuexh_dom_enabled(&rc->wire, d)) continue;
            const uint64_t nb = exh_nblocks(&exh_doms[d]);
            for (uint64_t b = 0; b < nb; b++) {
                const int na = spuexh_job_ops(&rc->wire, d, b, act);
                if (!na) continue;
                const int o = (int)(job++ % (uint32_t)spus);
                ents[o] += (uint64_t)na + 1; njobs[o]++;
            }
        }
        ents[0] += SPUEXH_NDOMS_GEN;                          /* SPU 0 also emits the generator self-check */
        S.n = spus;
        int ok = 1;
        void *cfgp = NULL; uint64_t cfg_ea = 0;
        cfgp = be->alloc(be, 512, &cfg_ea);
        if (!cfgp) ok = 0; else memcpy(cfgp, &rc->wire, sizeof rc->wire);
        for (int i = 0; ok && i < spus; i++) {
            S.par[i] = (spuexh_params_t *)be->alloc(be, sizeof(spuexh_params_t), &S.par_ea[i]);
            S.st[i] = (volatile spuexh_status_t *)be->alloc(be, sizeof(spuexh_status_t), &S.st_ea[i]);
            S.cap[i] = (uint32_t)(ents[i] + 16);
            S.reg[i] = (spuexh_entry_t *)be->alloc(be, (size_t)S.cap[i] * sizeof(spuexh_entry_t), &S.reg_ea[i]);
            if (!S.par[i] || !S.st[i] || !S.reg[i]) ok = 0;
        }
        if (!ok) { fprintf(sink->out, "META,sweep_error,allocation_failed\n"); res->aborted = 1; return -1; }
        S.cfg_ea = cfg_ea;
        memset(S.drained, 0, sizeof S.drained); memset(S.start_job, 0, sizeof S.start_job); memset(S.start_entries, 0, sizeof S.start_entries);
        memset(S.finished, 0, sizeof S.finished); memset(S.dead, 0, sizeof S.dead); memset(S.faults_seen, 0, sizeof S.faults_seen);
        for (int i = 0; i < spus; i++) if (njobs[i] == 0) S.finished[i] = 1;   /* nothing to do for this SPU */
        res->spus_used = spus;
        fprintf(sink->out, "META,spus,%d\n", spus);

        /* ---- launch / fault loop */
        int launched_once = 0;
        for (int attempt = 0;; attempt++) {
            int idx[6], n = 0;
            for (int i = 0; i < spus; i++) if (!S.finished[i] && !S.dead[i]) idx[n++] = i;
            if (!n) break;
            if (attempt > rc->max_faults) { res->aborted = 1; fprintf(sink->out, "META,sweep_error,max_faults_reached\n"); break; }
            if (!launched_once) S.t0 = spuexh_now();
            spuexh_thread_res_t tr[6];
            const int rv = sweep_launch(&S, idx, n, tr);
            if (rv == -3) {
                /* terminate failed and the group may still be live: record it flushed and stop the whole probe
                 * without a join (process exit lets lv2 reclaim the group; RUN.md policy) */
                char d1[200];
                clean(d1, sizeof d1, tr[0].desc);
                fprintf(sink->out, "FATAL,launch,%s\nMETA,sweep_error,fatal_launch\n", d1);
                fflush(sink->out);
                res->aborted = 1;
                return -1;
            }
            if (rv == -2 && !launched_once) { fprintf(sink->out, "META,spus_unavailable,%d\n", spus); ok = 0; break; }
            launched_once = 1;
            if (rv == -2) continue;                 /* a launch failed before any thread ran: retry the group as it is */
            if (rv < 0) { res->aborted = 1; fprintf(sink->out, "META,sweep_error,backend_run_failed_%d\n", rv); break; }
            res->relaunches = S.launches - 1;
            for (int q = 0; q < n; q++) drain(&S, idx[q]);
            int all_done = 1;
            for (int q = 0; q < n; q++) {
                const uint32_t st = W32(S.st[idx[q]]->state);
                if (st != 2 && st != 3) all_done = 0;
            }
            /* worker accounting: finished threads are done. A thread that did not finish is declared FAULT only when the
             * group ended by itself (its own state is 3) or the operator stopped us; after a group-wide fault, hang or
             * launch limit it gets its own single-SPU verification launch first (RETRY vs FAULT). */
            int fatal = 0;
            char d1fatal[200];
            for (int q = 0; q < n; q++) {
                const int i = idx[q];
                if (W32(S.st[i]->state) == 2) { S.finished[i] = 1; continue; }
                if (all_done || S.stopped) {
                    sweep_fault_row(&S, i, &tr[q], sink->out);
                    res->faults++;
                } else if (sweep_verify_alone(&S, i, sink->out, res, d1fatal, sizeof d1fatal) < 0) {
                    fatal = 1;
                    break;
                }
            }
            if (fatal) {
                fprintf(sink->out, "FATAL,launch,%s\nMETA,sweep_error,fatal_launch\n", d1fatal);
                fflush(sink->out);
                res->aborted = 1;
                return -1;
            }
            if (S.stopped) { res->aborted = 1; fprintf(sink->out, "META,sweep_error,stopped_by_operator\n"); break; }
            fflush(sink->out);
        }
        if (!ok && !launched_once) continue;                 /* try one SPU fewer */
        break;
    }
    res->completed = 1;
    for (int i = 0; i < S.n; i++) { if (!S.finished[i]) res->completed = 0; if (S.dead[i]) res->aborted = 1; }
    res->hashes = S.hashes; res->jobs_done = S.jobs_total; res->timing_overflows = S.timing_overflows;
    for (int i = 0; i < S.n; i++) {
        res->busy_ticks += W64(S.st[i]->busy_ticks); res->entries += S.drained[i];
        char b[128];
        snprintf(b, sizeof b, "%08x.%08x.%08x.%08x", W32(S.st[i]->fpscr_end[0]), W32(S.st[i]->fpscr_end[1]), W32(S.st[i]->fpscr_end[2]), W32(S.st[i]->fpscr_end[3]));
        fprintf(sink->out, "META,spu%d_fpscr_end,%s\n", i, b);
        fprintf(sink->out, "META,spu%d_busy_ticks,%llu\n", i, (unsigned long long)W64(S.st[i]->busy_ticks));
    }
    for (int d = 0; d < SPUEXH_NDOMS_GEN; d++)
        if (S.dom_jobs[d]) fprintf(sink->out, "#hwdom\t%s\tjobs=%llu\tticks=%llu\n", spuexh_dom_names[d],
                                   (unsigned long long)S.dom_jobs[d], (unsigned long long)S.dom_ticks[d]);
    res->wall_s = spuexh_now() - t_begin;
    fflush(sink->out);
    return res->completed ? 0 : 1;
}

/* ------------------------------------------------------------------ special probes */
typedef struct {
    spuexh_case_t cases[96];
    spuexh_caseout_t out[96];
} spec_buf_t;

/* Runs the n cases in one group (mode CASES or RAW). Returns the number of cases that reached state 2, -1 when the
 * group was stopped by the supervisor or faulted (the callers re-run the cases one by one), or -2 when the launch
 * could not be contained: the callers record the FATAL row the backend's res describes and stop the probe.
 * Every launch also writes its launch_meta() and FPSCR META rows (RUN.md: one row per launch). */
static int run_cases(spuexh_backend_t *be, int mode, const spuexh_case_t *cs, int n, int limit_ms,
                     spuexh_caseout_t *outs, spuexh_thread_res_t *tr, uint32_t *started_out, FILE *f) {
    static spuexh_params_t *par; static volatile spuexh_status_t *st; static spuexh_case_t *cbuf; static spuexh_caseout_t *obuf;
    static uint64_t par_ea, st_ea, c_ea, o_ea;
    if (!par) {
        par = (spuexh_params_t *)be->alloc(be, sizeof *par, &par_ea);
        st = (volatile spuexh_status_t *)be->alloc(be, sizeof *st, &st_ea);
        cbuf = (spuexh_case_t *)be->alloc(be, 96 * sizeof(spuexh_case_t), &c_ea);
        obuf = (spuexh_caseout_t *)be->alloc(be, 96 * sizeof(spuexh_caseout_t), &o_ea);
        if (!par || !st || !cbuf || !obuf) { par = NULL; return -1; }
    }
    memcpy(cbuf, cs, (size_t)n * sizeof *cs);
    memset(obuf, 0, 96 * sizeof(spuexh_caseout_t));
    memset((void *)st, 0, sizeof *st);
    memset(par, 0, sizeof *par);
    par->magic = W32(SPUEXH_PARAMS_MAGIC); par->mode = W32((uint32_t)mode); par->spu_index = 0; par->spu_count = W32(1u);
    par->ncases = W32((uint32_t)n); par->results_ea = W64(o_ea); par->status_ea = W64(st_ea); par->cases_ea = W64(c_ea);
    __sync_synchronize();
    const volatile spuexh_status_t *stp[1] = { st };
    memset(tr, 0, sizeof *tr);
    const int rv = be->run(be, 1, &par_ea, stp, NULL, NULL, limit_ms, tr);
    __sync_synchronize();
    const uint64_t ln = ++g_launch_seq;
    launch_meta(f, ln, tr, 1);
    fprintf(f, "META,launch%llu_spu0_fpscr,%08x.%08x.%08x.%08x\n", (unsigned long long)ln,
            W32(st->fpscr_start[0]), W32(st->fpscr_start[1]), W32(st->fpscr_start[2]), W32(st->fpscr_start[3]));
    if (started_out) *started_out = W32(st->cases_started);
    if (rv == -3) return -2;
    int ok = 0;
    for (int i = 0; i < n; i++) {
        memcpy(&outs[i], &obuf[i], sizeof outs[i]);
        if (W32(outs[i].state) == 2) ok++;
    }
    return rv < 0 ? -1 : ok;
}

static void hex128(char *dst, const uint32_t *w) {
    snprintf(dst, 40, "%08x%08x%08x%08x", W32(w[0]), W32(w[1]), W32(w[2]), W32(w[3]));
}

/* The FATAL row of a launch the backend could not contain (see backend contract): flushed at once, the probe stops. */
static void fatal_row(FILE *f, const spuexh_thread_res_t *tr, const char *where) {
    char d1[200];
    clean(d1, sizeof d1, tr->desc);
    fprintf(f, "FATAL,%s,%s\n", where, d1);
    fflush(f);
}

static const uint64_t DP_VALS[] = {
    0x0000000000000000ull, 0x8000000000000000ull,                       /* +-0 */
    0x0000000000000001ull, 0x8000000000000001ull,                       /* +-min denormal */
    0x0008000000000000ull, 0x8008000000000000ull,                       /* +-mid denormal (the 3.4 repro operand) */
    0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull,                       /* +-max denormal */
    0x000A56DA8A8D5917ull, 0x800A56DA8A8D5917ull, 0x000F6B9D2B8B96A2ull, 0x800F6B9D2B8B96A2ull,   /* the repro denormals */
    0x0010000000000000ull, 0x8010000000000000ull,                       /* +-min normal */
    0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x4000000000000000ull, 0x4008000000000000ull,
    0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull,                       /* +-max */
    0x7FF0000000000000ull, 0xFFF0000000000000ull,                       /* +-inf */
    0x7FF8000000000000ull, 0x7FF0000000000001ull, 0xFFF8000000000000ull /* qNaN, sNaN, -qNaN */
};
#define NDP ((int)(sizeof DP_VALS / sizeof DP_VALS[0]))

static void putdp(uint32_t *q, int lane, uint64_t v) { q[2 * lane] = W32((uint32_t)(v >> 32)); q[2 * lane + 1] = W32((uint32_t)v); }

int spuexh_run_special_a(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink) {
    static spuexh_case_t cs[96]; static spuexh_caseout_t outs[96]; static spuexh_thread_res_t tr;
    FILE *f = sink->out;
    for (int q = 0; q < 5; q++) {
        const char *const name = spuexh_dpcmp[q].name;
        const uint32_t word = spuexh_dpcmp[q].word;
        const int tsv = spuexh_dpcmp[q].tsv;
        if (rc->a_ops[0] && !name_match(rc->a_ops, name)) continue;
        /* case list of this op */
        static spuexh_case_t all[1700]; int nall = 0;
        if (!tsv) {                              /* binary compares: every ordered pair of DP_VALS, two lanes per call */
            const int pairs = NDP * NDP;
            for (int p = 0; p < pairs; p += 2) {
                spuexh_case_t *c = &all[nall]; memset(c, 0, sizeof *c);
                c->word = W32(word); c->flags = W32((uint32_t)SPUEXH_DPCMP_RT); c->id = W32((uint32_t)nall);
                putdp(c->a, 0, DP_VALS[p / NDP]); putdp(c->b, 0, DP_VALS[p % NDP]);
                if (p + 1 < pairs) { putdp(c->a, 1, DP_VALS[(p + 1) / NDP]); putdp(c->b, 1, DP_VALS[(p + 1) % NDP]); }
                nall++;
            }
        } else {                                 /* dftsv: every I7 on pairs of values */
            for (uint32_t i7 = 0; i7 < 128; i7++)
                for (int p = 0; p < NDP; p += 2) {
                    spuexh_case_t *c = &all[nall]; memset(c, 0, sizeof *c);
                    c->word = W32(word | (i7 << SPUEXH_DPTSV_I7_SHIFT)); c->flags = W32((uint32_t)SPUEXH_DPCMP_RT); c->id = W32((uint32_t)nall);
                    putdp(c->a, 0, DP_VALS[p]); if (p + 1 < NDP) putdp(c->a, 1, DP_VALS[p + 1]);
                    nall++;
                }
        }
        /* validity: case 0 alone, in its own group */
        uint32_t started = 0;
        int ok = run_cases(be, SPUEXH_MODE_CASES, &all[0], 1, rc->case_timeout_ms, outs, &tr, &started, f);
        if (ok == -2) { fatal_row(f, &tr, "launch"); return -1; }
        char d1[200]; clean(d1, sizeof d1, tr.desc);
        fprintf(f, "SPECA_VALID,%s,%s,word_%08x,started_%u,%s\n", name, ok == 1 ? "executed" : "NOT_executed", W32(all[0].word), started, d1);
        fflush(f);
        if (ok != 1) continue;
        /* the rest in batches; a batch that does not complete is re-run case by case */
        for (int b0 = 0; b0 < nall; b0 += 96) {
            const int nb = nall - b0 < 96 ? nall - b0 : 96;
            memcpy(cs, &all[b0], (size_t)nb * sizeof cs[0]);
            int okn = run_cases(be, SPUEXH_MODE_CASES, cs, nb, rc->case_timeout_ms * 4, outs, &tr, NULL, f);
            if (okn == -2) { fatal_row(f, &tr, "launch"); return -1; }
            if (okn == nb) {
                for (int i = 0; i < nb; i++) {
                    char a[40], bb[40], r[40];
                    hex128(a, cs[i].a); hex128(bb, cs[i].b); hex128(r, outs[i].r);
                    fprintf(f, "SPECA,%s,%08x,%u,%s,%s,%s\n", name, W32(cs[i].word), W32(cs[i].id), a, bb, r);
                }
            } else {
                for (int i = 0; i < nb; i++) {
                    spuexh_caseout_t o1; uint32_t st1 = 0; char a[40], bb[40], r[40];
                    int k = run_cases(be, SPUEXH_MODE_CASES, &cs[i], 1, rc->case_timeout_ms, &o1, &tr, &st1, f);
                    if (k == -2) { fatal_row(f, &tr, "launch"); return -1; }
                    hex128(a, cs[i].a); hex128(bb, cs[i].b); hex128(r, o1.r);
                    clean(d1, sizeof d1, tr.desc);
                    if (k == 1) fprintf(f, "SPECA,%s,%08x,%u,%s,%s,%s\n", name, W32(cs[i].word), W32(cs[i].id), a, bb, r);
                    else fprintf(f, "SPECA_FAULT,%s,%08x,%u,%s,%s,%s\n", name, W32(cs[i].word), W32(cs[i].id), a, bb, d1);
                }
            }
            fflush(f);
            if (sink->ui) { char b[96]; snprintf(b, sizeof b, "special A: %s %d/%d", name, b0 + nb, nall); if (sink->ui(sink->uu, b)) return 1; }
        }
    }
    return 0;
}

int spuexh_run_special_b(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink) {
    static spuexh_case_t cs; static spuexh_caseout_t o; static spuexh_thread_res_t tr;
    FILE *f = sink->out;
    const int total = (rc->special_b_max > 0 && rc->special_b_max < SPUEXH_NBAD) ? rc->special_b_max : SPUEXH_NBAD;
    fprintf(f, "META,special_b_prefixes,%d\n", total);
    if (rc->special_b_from > 0) fprintf(f, "META,special_b_from,%d\n", rc->special_b_from);
    fprintf(f, "META,special_b_patterns,%s\nMETA,special_b_count,%d\n", rc->special_b_both ? "both" : "zero",
            rc->special_b_count);
    const int from = rc->special_b_from;
    const int prefixes_left = total - from;
    const int prefix_limit = rc->special_b_count > 0 && rc->special_b_count < prefixes_left ?
                             rc->special_b_count : prefixes_left;
    int prefixes_done = 0;
    fflush(f);
    if (sink->ui) {
        char b[96];
        snprintf(b, sizeof b, "special B: starting %03x (%d prefixes)", spuexh_bad_prefix[from], prefix_limit);
        if (sink->ui(sink->uu, b)) return 1;
    }
    for (int i = from; i < total && (rc->special_b_count == 0 || prefixes_done < rc->special_b_count); i++) {
        const uint32_t pre = spuexh_bad_prefix[i];
        for (int pat = 0; pat < (rc->special_b_both ? 2 : 1); pat++) {
            const uint32_t operand = pat == 0 ? 0u : (uint32_t)exh_mix(0x5E5E000000000000ull + pre) & 0x1FFFFFu;
            const uint32_t word = (pre << 21) | operand;
            memset(&cs, 0, sizeof cs);
            cs.word = W32(word); cs.id = W32((uint32_t)(i * 2 + pat));
            if (sink->ui) {
                char b[96];
                snprintf(b, sizeof b, "special B: trying %03x %s (%d/%d prefixes)", pre, pat ? "rand" : "zero",
                         prefixes_done + 1, prefix_limit);
                if (sink->ui(sink->uu, b)) return 1;
            }
            /* Keep a stuck word visible in both last.txt and the result file before its group can block. */
            fprintf(f, "SPECB_TRY,%d,%03x,%s,%08x\n", i, pre, pat ? "rand" : "zero", word);
            fflush(f);
            uint32_t started = 0;
            const int ok = run_cases(be, SPUEXH_MODE_RAW, &cs, 1, rc->b_timeout_ms, &o, &tr, &started, f);
            if (ok == -2) {
                fatal_row(f, &tr, "launch");
                if (sink->ui) {
                    char b[96];
                    snprintf(b, sizeof b, "special B: FATAL prefix %03x %s", pre, pat ? "rand" : "zero");
                    sink->ui(sink->uu, b);
                }
                return -1;
            }
            const uint32_t state = W32(o.state);
            char d1[200], regs[160]; regs[0] = 0;
            clean(d1, sizeof d1, tr.desc);
            if (ok == 1) {
                const uint32_t nch = W32(o.nchanged);
                for (uint32_t k = 0; k < nch && k < SPUEXH_RAW_MAXCH; k++) {
                    char hx[40]; hex128(hx, o.ch[k].v);
                    size_t L = strlen(regs);
                    snprintf(regs + L, sizeof regs - L, "%s%u:%s", k ? "|" : "", W32(o.ch[k].reg), hx);
                    if (strlen(regs) > 120) break;
                }
            }
            fprintf(f, "SPECB,%d,%03x,%s,%08x,%s,%u,%s,%u,%08x,%s\n", i, pre, pat ? "rand" : "zero", word,
                    ok == 1 ? "executed" : (state == 1 ? "no_return" : "not_started"),
                    ok == 1 ? W32(o.nchanged) : 0u, regs, ok == 1 ? W32(o.arena_hit) : 0u, ok == 1 ? W32(o.arena_addr) : 0u, d1);
            fflush(f);                       /* every word: if the process is lost, the file names the last one tried */
            if (sink->ui) {
                char b[96];
                snprintf(b, sizeof b, "special B: %03x %s (%d/%d prefixes)", pre, pat ? "rand" : "zero",
                         prefixes_done + 1, prefix_limit);
                if (sink->ui(sink->uu, b)) return 1;
            }
        }
        prefixes_done++;
    }
    fprintf(f, "META,special_b_prefixes_done,%d\nMETA,special_b_completed,1\n", prefixes_done);
    fflush(f);
    return 0;
}

/* ------------------------------------------------------------------ lanes jobs */
typedef struct {
    spuexh_params_t *par;
    volatile spuexh_status_t *st;
    uint8_t *buf;                    /* results region: spuexh_lane_t records (LANES) or uint64 hashes (LANESUM) */
    uint64_t par_ea, st_ea, b_ea;
} lane_ctx_t;
#define LANE_BUF_BYTES ((size_t)SPUEXH_LANES_MAX_COUNT * sizeof(spuexh_lane_t) + 128)   /* >= (max chunks + pad) hashes too */

typedef struct {
    int rv;
    uint32_t state, flags, entries;
    spuexh_thread_res_t tr;
} lane_out_t;

/* One launch of a lanes or lanesum job on SPU 0 with the configured launch limit (always positive: no unbounded launch), its
 * launch META rows, and the status words read back. Returns -1 when the launch could not be contained (FATAL row written). */
static int lane_launch(spuexh_backend_t *be, const spuexh_runcfg_t *rc, FILE *f, lane_ctx_t *x, uint32_t mode, int op, int scale,
                       int dom, uint32_t blk, uint32_t start, uint32_t count, uint32_t cap, lane_out_t *o) {
    spuexh_params_t *par = x->par;
    memset(x->buf, 0, LANE_BUF_BYTES);
    memset((void *)x->st, 0, sizeof *x->st);
    memset(par, 0, sizeof *par);
    par->magic = W32(SPUEXH_PARAMS_MAGIC); par->mode = W32(mode); par->spu_index = 0; par->spu_count = W32(1u);
    par->results_cap = W32(cap); par->results_ea = W64(x->b_ea); par->status_ea = W64(x->st_ea);
    par->lane_op = W32((uint32_t)op); par->lane_dom = W32((uint32_t)dom); par->lane_blk = W32(blk);
    par->lane_count = W32(count); par->lane_start = W32(start); par->lane_scale_p1 = W32((uint32_t)(scale + 1));
    __sync_synchronize();
    const volatile spuexh_status_t *stp[1] = { x->st };
    memset(&o->tr, 0, sizeof o->tr);
    o->rv = be->run(be, 1, &x->par_ea, stp, NULL, NULL, rc->launch_timeout_ms, &o->tr);
    __sync_synchronize();
    const uint64_t ln = ++g_launch_seq;
    launch_meta(f, ln, &o->tr, 1);
    fprintf(f, "META,launch%llu_spu0_fpscr,%08x.%08x.%08x.%08x\n", (unsigned long long)ln, W32(x->st->fpscr_start[0]),
            W32(x->st->fpscr_start[1]), W32(x->st->fpscr_start[2]), W32(x->st->fpscr_start[3]));
    if (o->rv == -3) { fatal_row(f, &o->tr, "launch"); return -1; }
    o->state = W32(x->st->state); o->flags = W32(x->st->flags); o->entries = W32(x->st->entries);
    return 0;
}

static void lane_close_row(FILE *f, const char *kind, const char *on, const char *dn, uint32_t blk, uint32_t rows, int complete, const lane_out_t *o) {
    if (complete) {
        fprintf(f, "%s_DONE,%s,%s,%u,%u\n", kind, on, dn, blk, rows);
    } else {
        char d1[200];
        clean(d1, sizeof d1, o->tr.desc);
        fprintf(f, "%s_FAULT,%s,%s,%u,%u,rv_%d state_%u flags_%u %s\n", kind, on, dn, blk, rows, o->rv, o->state, o->flags, d1);
    }
    fflush(f);
}

int spuexh_run_lanes(spuexh_backend_t *be, const spuexh_runcfg_t *rc, const spuexh_sink_t *sink) {
    FILE *f = sink->out;
    if (!rc->nlanes && !rc->nsums) return 0;
    lane_ctx_t x;
    memset(&x, 0, sizeof x);
    x.par = (spuexh_params_t *)be->alloc(be, sizeof *x.par, &x.par_ea);
    if (x.par) x.st = (volatile spuexh_status_t *)be->alloc(be, sizeof *x.st, &x.st_ea);
    if (x.st) x.buf = (uint8_t *)be->alloc(be, LANE_BUF_BYTES, &x.b_ea);
    fprintf(f, "META,lanes,%d\nMETA,lanesums,%d\n", rc->nlanes, rc->nsums);
    if (!x.par || !x.st || !x.buf) { fprintf(f, "FATAL,lanes,allocation_failed\n"); fflush(f); return -1; }
    lane_out_t o;

    for (int q = 0; q < rc->nlanes; q++) {
        const int op = rc->lanes[q].op, dom = rc->lanes[q].dom;
        char on[24];
        lane_name(on, sizeof on, op, rc->lanes[q].scale);
        const char *dn = spuexh_dom_names[dom];
        const uint32_t blk = rc->lanes[q].blk, start = rc->lanes[q].start, count = (uint32_t)rc->lanes[q].count;
        /* pre-log: flushed before the launch, so a lost run names the entry that was being attempted */
        fprintf(f, "LANES,%s,%s,%u,%u,%u,%u\n", on, dn, blk, count, (unsigned)spuexh_ops[op].imm_bits, start);
        fflush(f);
        if (lane_launch(be, rc, f, &x, SPUEXH_MODE_LANES, op, rc->lanes[q].scale, dom, blk, start, count, SPUEXH_LANES_MAX_COUNT, &o) < 0) return -1;
        uint32_t rows = o.entries;
        if (o.rv < 0 || rows > count) rows = o.rv < 0 ? 0 : count;           /* nothing ran, or a status that cannot be right */
        const spuexh_lane_t *recs = (const spuexh_lane_t *)x.buf;
        for (uint32_t k = 0; k < rows; k++) {
            const spuexh_lane_t *L = &recs[k];
            char a[40], b[40], c[40], r[40], im[16];
            hex128(a, L->a); hex128(b, L->b); hex128(c, L->c); hex128(r, L->out);
            if (spuexh_ops[op].imm_bits) snprintf(im, sizeof im, "%u", W32(L->imm)); else snprintf(im, sizeof im, "-");
            fprintf(f, "LANE,%s,%s,%u,%llu,%s,%s,%s,%s,%s\n", on, dn, blk, (unsigned long long)W64(L->call), a, b, c, im, r);
        }
        lane_close_row(f, "LANES", on, dn, blk, rows, o.rv == 0 && o.state == 2 && !(o.flags & 8u) && rows == count, &o);
        if (sink->ui) {
            char s[96];
            snprintf(s, sizeof s, "lanes: %s %s %u (%d/%d)", on, dn, blk, q + 1, rc->nlanes);
            if (sink->ui(sink->uu, s)) return 1;
        }
    }

    for (int q = 0; q < rc->nsums; q++) {
        const int op = rc->sums[q].op, dom = rc->sums[q].dom;
        char on[24];
        lane_name(on, sizeof on, op, rc->sums[q].scale);
        const char *dn = spuexh_dom_names[dom];
        const uint32_t blk = rc->sums[q].blk, chunk = rc->sums[q].chunk;
        uint64_t calls = exh_doms[dom].ncalls - ((uint64_t)blk << EXH_BLOCK_LOG2);
        if (calls > EXH_BLOCK) calls = EXH_BLOCK;
        const uint32_t nchunks = (uint32_t)((calls + chunk - 1) / chunk);
        fprintf(f, "LANESUM_BEGIN,%s,%s,%u,%u,%u\n", on, dn, blk, chunk, nchunks);
        fflush(f);
        if (lane_launch(be, rc, f, &x, SPUEXH_MODE_LANESUM, op, rc->sums[q].scale, dom, blk, 0, chunk, SPUEXH_LANESUM_MAX_CHUNKS, &o) < 0) return -1;
        uint32_t rows = o.entries;
        if (o.rv < 0 || rows > nchunks) rows = o.rv < 0 ? 0 : nchunks;
        const uint64_t *hs = (const uint64_t *)x.buf;
        for (uint32_t k = 0; k < rows; k++) {
            const uint64_t first = (uint64_t)k * chunk;
            const uint64_t n = calls - first < chunk ? calls - first : chunk;
            fprintf(f, "LANESUM,%s,%s,%u,%llu,%llu,%016llx\n", on, dn, blk, (unsigned long long)first, (unsigned long long)n,
                    (unsigned long long)W64(hs[k]));
        }
        lane_close_row(f, "LANESUM", on, dn, blk, rows, o.rv == 0 && o.state == 2 && !(o.flags & 8u) && rows == nchunks, &o);
        if (sink->ui) {
            char s[96];
            snprintf(s, sizeof s, "lanesum: %s %s %u (%d/%d)", on, dn, blk, q + 1, rc->nsums);
            if (sink->ui(sink->uu, s)) return 1;
        }
    }
    return 0;
}
