/* Cases for cellSaveData (libs/system/cellSaveData.c vs the firmware's libsysutil savedata worker).
 *
 * The real worker does not touch the disk: it serialises each step into CXML messages (dh_cxml.h, a tagged name/value container)
 * and exchanges them with the system process over a shared-memory ring. The lifted side therefore
 * runs behind a SCRIPTED FAKE PEER: the three ring primitives of the generated override table
 * (vsh.ring_write / vsh.ring_read / vsh.channel_acquire, docs/DIFF_HARNESS.md) are replaced, and the
 * real CXML encoder/decoder, the real argument checks and the real callback marshalling still run.
 *
 * Both sides call the same guest funcStat / funcFile callbacks (host functions registered at guest
 * code addresses) with the same script; every callback records what it was handed (and what the
 * library did with its answer) into a guest-memory log that the comparator diffs.
 *
 * HLE-side state is real: a per-case save root under $TMPDIR holds the directory the case asks
 * for (PS3_GAMEDATA_ROOT, the HLE's existing override). The fake peer's answers are derived from
 * the same scenario, so "directory is new" means the same thing to both sides. */
#include "dh_engine.h"
#include "dh_cxml.h"
#include "ppu/ppu_context.h"
#include "ps3emu/guest_call.h"
#include "system/ps3_psf.h"
#include <cerrno>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace dh;

namespace {
constexpr uint32_t N_AUTOLOAD2 = 0xFBD5C856, N_AUTOSAVE2 = 0x8B7ED64B, N_LISTLOAD2 = 0x1DFBFDD6,
                   N_LISTSAVE2 = 0x2DE0D663, N_FIXEDLOAD2 = 0x2A8EADA2, N_FIXEDSAVE2 = 0x2AAE9EF5;

constexpr uint32_t E_CBRESULT = 0x8002B401, E_PARAM = 0x8002B404, E_NODATA = 0x8002B40B;

/* ---- guest layout --------------------------------------------------------------------------- */
constexpr uint32_t DIRNAME = 0x20000, SETBUF = 0x20100, SETLIST = 0x20200, PREFIX = 0x20240,
                   OPD_STAT = 0x22000, OPD_FILE = 0x22010, OPD_LIST = 0x22020, OPD_FIXED = 0x22030,
                   FNAME = 0x22100, SETBUF_BUF = 0x21000, FILEBUF = 0x24000, SFO = 0x25000, USERDATA = 0x26000,
                   NEWDATA = 0x26100, FOCUS = 0x26200, OBS = 0x30000;
constexpr uint32_t CODE_STAT = 0x00A10000, CODE_FILE = 0x00A10010, CODE_LIST = 0x00A10020, CODE_FIXED = 0x00A10030;
constexpr uint32_t OBS_REC = 0x80, OBS_MAX = 24, OBS_LEN = 0x20 + OBS_REC * OBS_MAX;
constexpr uint32_t SFO_LEN = 0x610;
constexpr uint32_t HLE_SCRATCH = 0x024E0000, HLE_SCRATCH_LEN = 0x20000;   /* libs/system/cellSaveData.c */
constexpr uint32_t BUFLEN = 0x2000;
constexpr uint32_t CELL_OP_READ = 0;

enum DirState { D_MISSING, D_VALID, D_EMPTY, D_BADSFO };

struct FileOp { uint32_t op, type; const char* name; uint32_t off, size, bufsize; };

/* One scenario: what the guest callbacks answer, what the save directory looks like, and how the
 * fake peer behaves. Everything the HLE can see is real; the peer knobs are lifted-side only. */
struct Scn {
    int32_t stat_result = 0;
    bool set_param = false;              /* funcStat fills statSet (saves) */
    uint32_t recreate = 0;
    std::vector<FileOp> ops;             /* funcFile script, then OK_LAST */
    int32_t file_end = 1;                /* result of the call after the script */
    int32_t file_result_at = -1;         /* >=0: call index that answers file_fail instead */
    int32_t file_fail = 0;
    DirState dir = D_MISSING;
    bool dir_has_file = false;
    /* fake peer */
    uint32_t ack = 0;                    /* elmResult of getSaveDataStat */
    uint32_t ack_open = 0;               /* elmResult of every other command */
    bool isnew_raw = false; uint32_t isnew_val = 0;   /* answer elmIsNewData with this instead of the derived value */
    uint32_t hdd_free_kb = 0x100000;
    bool short_stat = false;             /* reply elmDataStat with a blob of the wrong size */
    bool omit_stat = false;              /* reply without elmDataStat */
    bool corrupt = false;                /* reply bytes that are not a CXML message */
    uint32_t file_list_n = 0;            /* entries the peer reports in the stat record */
    uint32_t sb_dirmax = 8, sb_filemax = 4, sb_bufsize = BUFLEN, sb_buf = SETBUF_BUF;   /* the caller's CellSaveDataSetBuf */
};
Scn g_scn;
uint32_t g_sdk_version = 0x00480001;   /* what sys_process_get_sdk_version reports to the lifted worker */
std::string g_root;
std::vector<Case>* g_out;

/* ---- the callbacks -------------------------------------------------------------------------- */
#define CTXP(v) ppu_context* c = (ppu_context*)(v)
uint32_t g_file_calls = 0;

uint32_t obs_next()
{
    uint32_t n = rd32(OBS);
    if (n >= OBS_MAX) return 0;
    wr32(OBS, n + 1);
    return OBS + 0x20 + OBS_REC * n;
}
void obs_cb(uint32_t o, uint32_t cb, uint32_t kind)
{
    wr32(o + 0x00, kind);
    wr32(o + 0x04, rd32(cb + 0));            /* result as the library initialised it */
    wr32(o + 0x08, rd32(cb + 4));            /* progressBarInc */
    wr32(o + 0x0C, rd32(cb + 8));            /* errNeedSizeKB */
    wr32(o + 0x10, rd32(cb + 12) != 0);      /* invalidMsg is set */
    wr32(o + 0x14, rd32(cb + 16));           /* userdata echoed back */
}
void cb_stat(void* v)
{
    CTXP(v);
    const uint32_t cb = (uint32_t)c->gpr[3], get = (uint32_t)c->gpr[4], set = (uint32_t)c->gpr[5];
    if (uint32_t o = obs_next()) {
        obs_cb(o, cb, 1);
        wr32(o + 0x18, rd32(get + 0));       /* hddFreeSizeKB */
        wr32(o + 0x1C, rd32(get + 4));       /* isNewData */
        for (int i = 0; i < 32; i += 4) wr32(o + 0x20 + i, rd32(get + 32 + i));   /* dir.dirName */
        wr32(o + 0x40, rd32(get + 1616));    /* bind */
        wr32(o + 0x44, rd32(get + 1620));    /* sizeKB */
        wr32(o + 0x48, rd32(get + 1624));    /* sysSizeKB */
        wr32(o + 0x4C, rd32(get + 1628));    /* fileNum */
        wr32(o + 0x50, rd32(get + 1632));    /* fileListNum */
        const uint32_t fl = rd32(get + 1636), sb = rd32(SETBUF + 36);
        wr32(o + 0x54, fl == 0 ? 1 : fl == sb ? 2 : 3);   /* fileList: NULL / the caller's setBuf->buf / elsewhere */
        wr32(o + 0x58, rd32(set + 0) != 0);  /* statSet pre-state: setParam, reCreateMode, indicator */
        wr32(o + 0x5C, rd32(set + 4));
        wr32(o + 0x60, rd32(set + 8) != 0);
        wr32(o + 0x64, rd32(get + 64));      /* getParam.title[0..3] */
        wr32(o + 0x68, rd32(get + 64 + 1280));   /* getParam.attribute */
    }
    if (g_scn.set_param) {
        wr32(set + 0, SFO); wr32(set + 4, g_scn.recreate); wr32(set + 8, 0);
    }
    wr32(cb + 0, (uint32_t)g_scn.stat_result);
}
void cb_file(void* v)
{
    CTXP(v);
    const uint32_t cb = (uint32_t)c->gpr[3], get = (uint32_t)c->gpr[4], set = (uint32_t)c->gpr[5];
    const uint32_t call = g_file_calls++;
    if (uint32_t o = obs_next()) {
        obs_cb(o, cb, 2);
        wr32(o + 0x18, rd32(get + 0));       /* excSize: the previous operation's byte count */
        wr32(o + 0x1C, call);
    }
    int32_t res = g_scn.file_end;
    if (call < g_scn.ops.size()) {
        const FileOp& f = g_scn.ops[call];
        wr32(set + 0, f.op); wr32(set + 8, f.type);
        wr32(set + 28, FNAME + 0x40 * call); wr32(set + 32, f.off); wr32(set + 36, f.size);
        wr32(set + 40, f.bufsize); wr32(set + 44, FILEBUF);
        res = 0;
    }
    if (g_scn.file_result_at >= 0 && (int32_t)call == g_scn.file_result_at) res = g_scn.file_fail;
    wr32(cb + 0, (uint32_t)res);
}
void cb_list(void* v)   /* funcList / funcFixed: pick the directory by name, answer OK_LAST */
{
    CTXP(v);
    const uint32_t cb = (uint32_t)c->gpr[3], get = (uint32_t)c->gpr[4], set = (uint32_t)c->gpr[5];
    if (uint32_t o = obs_next()) {
        obs_cb(o, cb, 3);
        wr32(o + 0x18, rd32(get + 0));       /* dirNum */
        wr32(o + 0x1C, rd32(get + 4));       /* dirListNum */
    }
    wr32(set + 0, 0); wr32(set + 4, FOCUS);      /* focusPosition, focusDirName */
    wr32(set + 8, 0); wr32(set + 12, 0);         /* fixedListNum, fixedList */
    wr32(set + 16, 0);                           /* newData */
    wr32(cb + 0, 0);
}
void cb_fixed(void* v)
{
    CTXP(v);
    const uint32_t cb = (uint32_t)c->gpr[3], get = (uint32_t)c->gpr[4], set = (uint32_t)c->gpr[5];
    if (uint32_t o = obs_next()) {
        obs_cb(o, cb, 4);
        wr32(o + 0x18, rd32(get + 0));
        wr32(o + 0x1C, rd32(get + 4));
    }
    wr32(set + 0, DIRNAME); wr32(set + 4, 0); wr32(set + 8, 0);   /* FixedSet {dirName, newIcon, option} */
    wr32(cb + 0, 0);
}

/* ---- host save root ------------------------------------------------------------------------- */
void rm_tree(const std::string& p)
{
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
}
void mkdirs(const std::string& p)
{
    for (size_t i = 1; i < p.size(); ++i) if (p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
    mkdir(p.c_str(), 0755);
}
void put_file(const std::string& path, const void* d, size_t n)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}
void make_host_dir(const Scn& s, const char* dirname)
{
    const char* tmp = getenv("TMPDIR");
    std::string base = std::string(tmp && *tmp ? tmp : "/tmp") + "/dh-savedata-" + std::to_string((long)getpid());
    static bool first = true;
    if (first) { first = false; g_root = base; atexit([] { rm_tree(g_root); }); }
    static int seq = 0;
    std::string root = g_root + "/c" + std::to_string(seq++);
    mkdirs(root + "/dev_hdd0/home/00000001/savedata");
    setenv("PS3_GAMEDATA_ROOT", root.c_str(), 1);   /* read once per HLE child, which is forked after this */
    if (s.dir == D_MISSING) return;
    const std::string d = root + "/dev_hdd0/home/00000001/savedata/" + dirname;
    mkdirs(d);
    if (s.dir == D_VALID) {
        uint8_t buf[4096];
        size_t n = ps3_psf_build(buf, sizeof buf, dirname, "DHTITLE", "DHSUB", "DHDETAIL", 0, "");
        put_file(d + "/PARAM.SFO", buf, n);
    } else if (s.dir == D_BADSFO) {
        static const uint8_t junk[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        put_file(d + "/PARAM.SFO", junk, sizeof junk);
    }
    if (s.dir_has_file) { static const char data[] = "0123456789abcdef"; put_file(d + "/DATA.BIN", data, 16); }
}

/* ---- the fake peer (lifted side only) ------------------------------------------------------- */
/* The "trace-peer" case flag (test-only) writes the request stream beside the report: the harness
 * captures a child's stderr, and the wire format is the thing a developer needs to see. */
FILE* tf()
{
    static FILE* f = fopen("/tmp/dh-savedata-peer.trace", "a");
    return f ? f : stderr;
}

struct Peer {
    std::vector<uint8_t> in;                    /* request bytes of the message being assembled */
    std::vector<std::vector<uint8_t>> replies;   /* complete replies, oldest first */
    size_t next = 0;
    std::vector<std::string> log;                /* command names, in order */
} g_peer;

std::vector<uint8_t> stat_record(const cxml::Msg& req)
{
    std::vector<uint8_t> b(1704, 0);
    auto w32 = [&](size_t o, uint32_t v) { b[o] = (uint8_t)(v >> 24); b[o + 1] = (uint8_t)(v >> 16); b[o + 2] = (uint8_t)(v >> 8); b[o + 3] = (uint8_t)v; };
    w32(0, g_scn.hdd_free_kb);
    const bool isnew = g_scn.dir == D_MISSING || g_scn.dir == D_EMPTY;
    w32(4, isnew ? 1 : 0);
    if (const cxml::Elem* d = req.find("elmDirName")) {
        memcpy(&b[32], d->sval.c_str(), d->sval.size() < 31 ? d->sval.size() : 31);
    }
    if (!isnew) {
        memcpy(&b[64], "DHTITLE", 7);
        w32(1632, 0); w32(1628, g_scn.dir_has_file ? 1 : 0);
    }
    w32(1632, g_scn.file_list_n);
    return b;
}

/* getSaveDataStat is answered by TWO messages (the client reads twice before it calls funcStat):
 * {elmResult, elmIsNewData}, then {elmDataStat}. Every other command gets one {elmResult}. */
std::vector<cxml::Msg> reply_for(const cxml::Msg& req)
{
    std::vector<cxml::Msg> out;
    cxml::Msg r; r.root = req.root;
    if (req.root == "getSaveDataStat") {
        r.add_int("elmResult", g_scn.ack);
        const bool isnew = g_scn.dir == D_MISSING || g_scn.dir == D_EMPTY;
        /* "mutate-peer" (test-only) answers the opposite: a case must NOT keep its verdict if the peer lies */
        r.add_int("elmIsNewData", g_scn.isnew_raw ? g_scn.isnew_val : ((isnew != case_flag("mutate-peer")) ? 1 : 0));
        out.push_back(r);
        if (!g_scn.omit_stat) {
            cxml::Msg d; d.root = req.root;
            auto rec = stat_record(req);
            if (g_scn.short_stat) rec.resize(1000);
            d.add_blob("elmDataStat", rec);
            out.push_back(d);
        }
        return out;
    }
    r.add_int("elmResult", g_scn.ack_open);
    out.push_back(r);
    return out;
}

void peer_request_complete()
{
    cxml::Msg req; std::string why;
    if (!cxml::parse(g_peer.in.data(), g_peer.in.size(), req, why)) {
        g_peer.in.clear();
        model_stop(("fake peer: request is not a CXML message (" + why + ")").c_str());
    }
    g_peer.in.clear();
    static const char* known[] = {"loadModules", "unloadModules", "getSaveDataStat", "openSaveData", "readData", "writeData",
                                  "deleteData", "abort", "getDirList", "openSaveList", "getOwnerShip", "openDeleteList"};
    bool ok = false;
    for (const char* k : known) if (req.root == k) ok = true;
    if (case_flag("trace-peer")) {
        fprintf(tf(), "[peer] %s:", req.root.c_str());
        for (auto& e : req.elems) fprintf(tf(), " %s(t%u,%u,%u)", e.name.c_str(), e.type, e.raw_value, e.raw_aux);
        fprintf(tf(), "\n"); fflush(tf());
    }
    if (!ok) model_stop(("fake peer: unscripted command '" + req.root + "'").c_str());
    g_peer.log.push_back(req.root);
    if (g_scn.corrupt) g_peer.replies.push_back(std::vector<uint8_t>(64, 0x5A));
    else for (const cxml::Msg& m : reply_for(req)) g_peer.replies.push_back(cxml::build(m));
}

/* Observation only: names the check that failed (r3) and where, then runs the original (returns false). */
bool ov_param_error(void* v)
{
    CTXP(v);
    if (case_flag("trace-peer")) { fprintf(tf(), "[param-error] check=%llu lr=%#llx\n", (unsigned long long)c->gpr[3], (unsigned long long)c->lr); fflush(tf()); }
    return false;
}
bool ov_abort_report(void* v)
{
    CTXP(v);
    if (case_flag("trace-peer")) {
        const uint32_t r = (uint32_t)c->gpr[4];
        fprintf(tf(), "[abort-report] lr=%#llx rec:", (unsigned long long)c->lr);
        for (int i = 0; i < 6; ++i) fprintf(tf(), " %08x", rd32(r + 4 * i));
        fprintf(tf(), "\n"); fflush(tf());
    }
    return false;
}
bool ov_channel_acquire(void* v)
{
    ((ppu_context*)v)->gpr[3] = 0;    /* the channel is granted */
    return true;
}
bool ov_ring_write(void* v)
{
    CTXP(v);
    const uint32_t src = (uint32_t)c->gpr[5], len = (uint32_t)c->gpr[6];
    if ((uint64_t)src + len > ARENA_SIZE) model_stop("fake peer: request span outside guest memory");
    if (case_flag("trace-peer")) {
        fprintf(tf(), "[peer] write ch=%llu tag=%#llx src=%#x len=%u last=%llu:", (unsigned long long)c->gpr[3], (unsigned long long)c->gpr[4], src, len, (unsigned long long)(c->gpr[7] & 1));
        for (uint32_t i = 0; i < len && i < 24; ++i) fprintf(tf(), " %02x", mem[src + i]);
        fprintf(tf(), "\n"); fflush(tf());
    }
    g_peer.in.insert(g_peer.in.end(), mem + src, mem + src + len);
    if (c->gpr[7] & 1) peer_request_complete();
    c->gpr[3] = 0;
    return true;
}
bool ov_ring_read(void* v)
{
    CTXP(v);
    const uint32_t dst = (uint32_t)c->gpr[5], len = (uint32_t)c->gpr[6], flag = (uint32_t)c->gpr[7];
    if (case_flag("trace-peer")) { fprintf(tf(), "[peer] read ch=%llu tag=%#llx dst=%#x len=%u flag=%#x next=%zu of %zu lr=%#llx\n", (unsigned long long)c->gpr[3], (unsigned long long)c->gpr[4], dst, len, flag, g_peer.next, g_peer.replies.size(), (unsigned long long)c->lr); fflush(tf()); }
    if (g_peer.next >= g_peer.replies.size()) model_stop("fake peer: the client waits for a reply that no request asked for");
    const auto& rep = g_peer.replies[g_peer.next++];
    if (rep.size() > len) model_stop("fake peer: reply larger than the client's read buffer");
    if ((uint64_t)dst + len > ARENA_SIZE) model_stop("fake peer: reply buffer outside guest memory");
    memcpy(mem + dst, rep.data(), rep.size());
    if (flag) wr8(flag, 1);
    c->gpr[3] = (uint64_t)rep.size();
    return true;
}

/* ---- case plumbing -------------------------------------------------------------------------- */
const std::vector<std::pair<uint32_t, uint32_t>> kMask = {
    {HLE_SCRATCH, HLE_SCRATCH_LEN},        /* the HLE's marshalling scratch: the FW marshals on its own stack */
};
/* The channel buffer and bookkeeping the firmware keeps in its own image (.bss): reply bytes land
 * there, and the channel-in-use flag. The HLE has no such memory. */
const std::vector<std::pair<uint32_t, uint32_t>> kImageMask = {
    {0x1FBC0, 0x6020},      /* [0x0201FBC0, +0x6020): the 0x6000-byte reply buffer of the savedata channel and its header */
    {0x25BC0, 0x30},        /* its in-use flag (0x02025BD0) and neighbours */
    {0x1F970, 0x10},        /* DAT_0201F970: pointer to that buffer */
};

void init_guest(const Scn& s, bool save)
{
    fill(OBS, OBS_LEN, 0);
    fill(DIRNAME, 0x100, 0); memcpy(mem + DIRNAME, "DHSAVE00", 9);
    fill(SETBUF, 0x40, 0);
    wr32(SETBUF + 0, s.sb_dirmax); wr32(SETBUF + 4, s.sb_filemax); wr32(SETBUF + 32, s.sb_bufsize); wr32(SETBUF + 36, s.sb_buf);
    fill(SETBUF_BUF, BUFLEN, 0x3C);
    fill(SETLIST, 0x40, 0); wr32(SETLIST + 0, 0); wr32(SETLIST + 4, 0); wr32(SETLIST + 8, PREFIX);
    memcpy(mem + PREFIX, "DHSAVE", 7);
    memcpy(mem + FOCUS, "DHSAVE00", 9);
    wr32(OPD_STAT, CODE_STAT); wr32(OPD_STAT + 4, 0);
    wr32(OPD_FILE, CODE_FILE); wr32(OPD_FILE + 4, 0);
    wr32(OPD_LIST, CODE_LIST); wr32(OPD_LIST + 4, 0);
    wr32(OPD_FIXED, CODE_FIXED); wr32(OPD_FIXED + 4, 0);
    fill(FNAME, 0x200, 0);
    for (size_t i = 0; i < s.ops.size() && i < 8; ++i) memcpy(mem + FNAME + 0x40 * i, s.ops[i].name, strlen(s.ops[i].name) + 1);
    fill(FILEBUF, 0x1000, 0x6B);
    fill(SFO, SFO_LEN, 0);
    if (s.set_param) {
        memcpy(mem + SFO, "DHSETTITLE", 11); memcpy(mem + SFO + 128, "DHSETSUB", 9); memcpy(mem + SFO + 256, "DHSETDETAIL", 12);
        wr32(SFO + 1280, 0); memcpy(mem + SFO + 1288, "LP", 3);
    }
    wr32(USERDATA, 0xC0DE0001);
    (void)save;
}

/* AutoLoad2/AutoSave2 (version, dirName, errDialog, setBuf, funcStat, funcFile, container, userdata) */
void add_auto(const char* id, const char* row, bool save, const Scn& s, uint32_t version, Arg errDialog,
              Expect e = Expect::Match, const char* note = "", Arg dirName = Arg(DIRNAME), Arg setBuf = Arg(SETBUF),
              Arg funcStat = Arg(OPD_STAT), Arg funcFile = Arg(OPD_FILE))
{
    Case c; c.id = id; c.row = row; c.expect = e; c.note = note;
    c.steps = {step(save ? "cellSaveDataAutoSave2" : "cellSaveDataAutoLoad2", save ? N_AUTOSAVE2 : N_AUTOLOAD2,
                    {Arg((uint32_t)version), dirName, errDialog, setBuf, funcStat, funcFile, Arg((uint32_t)0), Arg((uint32_t)USERDATA)})};
    c.mask = kMask; c.mask_image = kImageMask;
    c.setup = [s, save] {
        g_scn = s; g_file_calls = 0; g_peer = Peer();
        init_guest(s, save);
        make_host_dir(s, "DHSAVE00");
    };
    g_out->push_back(std::move(c));
}
}  // namespace

namespace {
struct Base { const char* id; char v; const char* cause; const char* reason; };
const Base kBase[] = {
#include "cases_libsysutil_savedata_baseline.inc"
};
}  // namespace

namespace dh {
static void model_zero(void* v) { ((ppu_context*)v)->gpr[3] = 0; }
/* The worker asks lv2 for the caller's SDK version (sys_process_getpid = 1, then
 * sys_process_get_sdk_version = 25, which stores a u32 through r4) before it checks its arguments. */
/* NID 0xFA7F693D, called only by the wrapper at 0x0200021C that prints "****** sysutil savedata
 * parameter error : %d ******" (the PARAM path, 0x0200C2F8). Its name is not in the NID database;
 * the call has no guest-visible result, so it is modelled as a diagnostic sink (the text is traced). */
static void model_diag_print(void* v)
{
    ppu_context* c = (ppu_context*)v;
    if (case_flag("trace-peer")) {
        const uint32_t f = (uint32_t)c->gpr[3];
        fprintf(tf(), "[diag] %s %lld\n", f && f < ARENA_SIZE - 64 ? (const char*)(mem + f) : "?", (long long)(int32_t)c->gpr[4]);
        fflush(tf());
    }
    c->gpr[3] = 0;
}
static void model_getpid(void* v) { ((ppu_context*)v)->gpr[3] = 1; }
static void model_sdk_version(void* v)
{
    ppu_context* c = (ppu_context*)v;
    wr32((uint32_t)c->gpr[4], g_sdk_version);
    c->gpr[3] = 0;
}

ImportPolicy import_policy()
{
    ImportPolicy p;
    p.dflt = Policy::Stop;
    const uint32_t fwd[] = {0x68B9B011 /*_sys_memset*/, 0x6BF66EA7 /*_sys_memcpy*/, 0x04E83D2C /*_sys_strncmp*/,
                            0x2D36462B /*_sys_strlen*/, 0x459B4393 /*_sys_strcmp*/, 0x99C88692 /*_sys_strcpy*/,
                            0xD3039D4D /*_sys_strncpy*/, 0x9F04F7AF /*_sys_printf*/};
    for (uint32_t n : fwd) p.by_nid[n] = Policy::Forward;
    /* A single-threaded harness: the worker's lightweight mutex is never contended. */
    const uint32_t mdl[] = {0x1573DC3F /*sys_lwmutex_lock*/, 0x1BC200F4 /*sys_lwmutex_unlock*/,
                            0x2F85C0EF /*sys_lwmutex_create*/, 0xC3476D0C /*sys_lwmutex_destroy*/};
    for (uint32_t n : mdl) { p.by_nid[n] = Policy::Model; p.models[n] = model_zero; }
    p.by_nid[0xFA7F693D] = Policy::Model; p.models[0xFA7F693D] = model_diag_print;
    /* sys_mutex_* / sys_cond_* (100..109): the ring's blocking helpers are overridden, so these are only
     * the abort path's lock/unlock of its own state; one thread, never contended. A cond wait (107) would
     * block for ever, so it stays unmodelled. */
    for (uint64_t n : {100, 101, 102, 103, 104, 105, 106, 108, 109}) p.syscalls[n] = model_zero;
    p.syscalls[1] = model_getpid;
    p.syscalls[25] = model_sdk_version;
    return p;
}

void register_cases(std::vector<Case>& out)
{
    g_out = &out;
    g_ps3_guest_caller = [](uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
        guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
    };
    register_guest_fn(CODE_STAT, cb_stat);
    register_guest_fn(CODE_FILE, cb_file);
    register_guest_fn(CODE_LIST, cb_list);
    register_guest_fn(CODE_FIXED, cb_fixed);
    override_symbol("vsh.channel_acquire", ov_channel_acquire);
    override_symbol("vsh.ring_write", ov_ring_write);
    override_symbol("vsh.ring_read", ov_ring_read);
    override_symbol("savedata.param_error", ov_param_error);
    override_symbol("savedata.abort_report", ov_abort_report);

    auto nm = [](const char* f, int r, uint32_t e, uint32_t v) {
        return std::string(f) + "_r" + (r < 0 ? "m" : "") + std::to_string(r < 0 ? -r : r) + "_e" + std::to_string(e) + "_v" + std::to_string(v);
    };
    /* Matrix: funcStat result x errDialog x version, for AutoLoad2 (an existing save) and AutoSave2 (a new one). */
    for (int save = 0; save < 2; ++save)
        for (int r : {-6, -5, -4, -1, 0, 1, 2})
            for (uint32_t e : {0u, 1u, 2u, 3u})
                for (uint32_t v : {0u, 1u, 2u}) {
                    Scn s; s.stat_result = r;
                    s.dir = save ? D_MISSING : D_VALID; s.set_param = save != 0;
                    add_auto(nm(save ? "autosave2" : "autoload2", r, e, v).c_str(), "F-matrix", save != 0, s, v, Arg(e));
                }
    /* NULL arguments and setBuf boundaries (the fileList pointer is setBuf->buf). */
    {
        Scn s; s.dir = D_VALID;
        add_auto("autoload2_null_dirname", "F-null", false, s, 0, Arg(0u), Expect::Match, "", Arg(0u));
        add_auto("autoload2_null_setbuf", "F-null", false, s, 0, Arg(0u), Expect::Match, "", Arg(DIRNAME), Arg(0u));
        add_auto("autoload2_null_funcstat", "F-null", false, s, 0, Arg(0u), Expect::Match, "", Arg(DIRNAME), Arg(SETBUF), Arg(0u));
        add_auto("autoload2_null_funcfile", "F-null", false, s, 0, Arg(0u), Expect::Match, "", Arg(DIRNAME), Arg(SETBUF), Arg(OPD_STAT), Arg(0u));
        Scn b = s; b.sb_buf = 0;        add_auto("autoload2_setbuf_buf_null", "F-setbuf", false, b, 0, Arg(0u));
        b = s; b.sb_bufsize = 0;        add_auto("autoload2_setbuf_size_0", "F-setbuf", false, b, 0, Arg(0u));
        b = s; b.sb_bufsize = 55;       add_auto("autoload2_setbuf_size_lt_entry", "F-setbuf", false, b, 0, Arg(0u));
        b = s; b.sb_filemax = 0;        add_auto("autoload2_setbuf_filemax_0", "F-setbuf", false, b, 0, Arg(0u));
        b = s; b.sb_filemax = 0x7FFF;   add_auto("autoload2_setbuf_filemax_huge", "F-setbuf", false, b, 0, Arg(0u));
    }
    /* Faults through the fake peer (lifted side only; the HLE has no peer). */
    {
        Scn s; s.dir = D_VALID;
        Scn f = s; f.ack = 0x80029262;                 add_auto("fault_stat_ack_nonzero", "F-fault", false, f, 0, Arg(0u));
        f = s; f.isnew_raw = true; f.isnew_val = 0x80029262;   add_auto("fault_isnew_is_error", "F-fault", false, f, 0, Arg(0u));
        f = s; f.hdd_free_kb = 0;                      add_auto("fault_no_space", "F-fault", false, f, 0, Arg(0u));
        f = s; f.dir = D_BADSFO;                       add_auto("fault_broken_param_sfo", "F-fault", false, f, 0, Arg(0u));
        f = s; f.short_stat = true;                    add_auto("fault_stat_blob_short", "F-fault", false, f, 0, Arg(0u));
        f = s; f.omit_stat = true;                     add_auto("fault_stat_blob_missing", "F-fault", false, f, 0, Arg(0u));
        f = s; f.corrupt = true;                       add_auto("fault_reply_not_a_message", "F-fault", false, f, 0, Arg(0u));
        f = s; f.ack_open = 0x80029262;                add_auto("fault_open_ack_nonzero", "F-fault", false, f, 0, Arg(0u));
    }
    /* A file operation behind a successful stat. */
    {
        Scn s; s.dir = D_VALID; s.dir_has_file = true;
        s.ops = {{CELL_OP_READ, 1, "DATA.BIN", 0, 16, 16}};
        add_auto("autoload2_read_one_file", "F-file", false, s, 0, Arg(0u));
    }

    /* Expectations: declared against the CURRENT HLE (before #48). A row whose reason is "#48" is a
     * divergence that fix removes; flipping it is `D` -> `M` in the baseline table. */
    for (const Base& b : kBase) {
        Case* c = nullptr;
        for (Case& x : out) if (x.id == b.id) { c = &x; break; }
        if (!c) { fprintf(stderr, "[dh] baseline names a case that does not exist: %s\n", b.id); exit(2); }
        const std::string why = !strcmp(b.reason, "#48") ? "#48: the callback-result / PARAM / fileList fix changes this outcome"
                                                         : "baseline: observed against the current HLE; the fake peer models only the commands the first stat stage needs";
        if (b.v == 'M') { c->expect = Expect::Match; }
        else if (b.v == 'D') { c->expect = Expect::Diverge; c->expect_cause = b.cause; c->note = why; }
        else { c->expect = Expect::Blocked; c->blocker = b.cause; c->expect_cause = "blocked"; c->note = why; }
    }
}
}  // namespace dh
