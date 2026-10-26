/* Call-level differential harness: run one library function through the HLE
 * implementation and through the lifted firmware code, compare what they do.
 *
 * Generic: nothing here names a library. A module contributes (1) a
 * dh_module descriptor (generated, lives outside the repo) and (2) a case
 * table (cases_<lib>.cpp).
 *
 * V1 result model (docs/DIFF_HARNESS.md):
 *   per side   returned | guest-fault{ea,size} | named-stop{kind} |
 *              host-crash{signal} | timeout | harness-blocked{reason}
 *   per case   match | diverge | no-hle | inconclusive | lifted-blocked
 * A match or an equal named stop reached through a Policy::Forward import is
 * common mode: the same HLE code ran on both sides, so it is weaker evidence.
 */
#ifndef PS3RECOMP_DH_ENGINE_H
#define PS3RECOMP_DH_ENGINE_H

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "dh_module.h"

namespace dh {

/* ---- guest arena -------------------------------------------------------- */
constexpr uint32_t ARENA_SIZE = 0x03000000u;   /* covers a module image at 0x02000000 */
/* The whole 4 GiB guest address space is reserved PROT_NONE; the arena above is
 * mapped RW inside it. A lifted access outside the arena therefore faults
 * deterministically as a guest-fault, instead of depending on which host
 * mapping happens to sit above vm_base. */
constexpr uint64_t GUEST_SPACE = 0x100000000ull;
constexpr uint32_t WIN_LO     = 0x00010000u;   /* primed data window [LO,HI) */
constexpr uint32_t WIN_HI     = 0x00100000u;
constexpr uint32_t STACK_TOP  = 0x001FF000u;   /* guest stack, outside the window */
/* The guest stack legitimately differs: the lifted code builds host-visible
 * guest frames there and the HLE handler does not. It is the one arena region
 * left out of the comparison (documented in DIFF_HARNESS.md). */
constexpr uint32_t STACK_SCAN_LO = 0x001EF000u;
constexpr uint32_t STACK_SCAN_HI = 0x00200000u;
constexpr uint32_t TLS_R13    = 0x00210000u;   /* r13; [TLS_R13, +0x1000) is diffed too */
constexpr uint32_t TLS_LEN    = 0x00001000u;
constexpr uint32_t SENTINEL   = 0x02F00000u;   /* LR the callee must return to */
constexpr uint32_t OPD_BASE   = 0x02100000u;   /* harness-owned import descriptors */
constexpr uint32_t TRAMP_BASE = 0x02110000u;   /* harness-owned import "code" */

extern uint8_t* mem;                            /* == vm_base */
/* True in the child that runs the HLE side; pre/post hooks and import models
 * use it to act on one side only. */
bool     on_hle_side();
void     wr8(uint32_t ea, uint8_t v);
void     wr16(uint32_t ea, uint16_t v);
void     wr32(uint32_t ea, uint32_t v);
void     wr64(uint32_t ea, uint64_t v);
uint32_t rd32(uint32_t ea);
uint16_t rd16(uint32_t ea);
void     fill(uint32_t ea, uint32_t n, uint8_t v);

/* ---- import policy ------------------------------------------------------ */
/* Forward: run the registered HLE handler. Model: run a small harness-supplied
 * stand-in (for helpers the HLE core does not register in this fixture). Stop:
 * abort the lifted call with a named reason. */
enum class Policy { Forward, Stop, Model };
struct ImportPolicy {
    std::map<uint32_t, Policy> by_nid;
    std::map<uint32_t, void (*)(void* /*ppu_context* */)> models;
    /* lv2 syscalls the lifted code may make: syscall number -> stand-in (the runtime's own
     * handler where it has one). An absent number stops the lifted call. */
    std::map<uint64_t, void (*)(void* /*ppu_context* */)> syscalls;
    Policy dflt = Policy::Stop;
    Policy of(uint32_t nid) const { auto i = by_nid.find(nid); return i == by_nid.end() ? dflt : i->second; }
};

/* ---- cases -------------------------------------------------------------- */
enum class Expect { Match, Diverge, NoHle, Inconclusive, Blocked };

/* A step argument: a literal, or "value captured into slot N by an earlier step"
 * (+ add). Slots are per side, so a handle one side returned is what that side
 * passes later. */
struct Arg {
    uint64_t lit = 0; int slot = -1; int64_t add = 0;
    Arg() {}
    Arg(uint64_t v) : lit(v) {}
    Arg(int v) : lit((uint64_t)(int64_t)v) {}
    Arg(uint32_t v) : lit(v) {}
    static Arg slotv(int n, int64_t add = 0) { Arg a; a.slot = n; a.add = add; return a; }
};
/* After a step: copy r3 (Ret), or a big-endian word read from guest memory, into a slot. */
struct Capture { int slot; enum Kind { Ret, Mem32, Mem64 } kind; uint32_t ea; };

struct Step {
    std::string func; uint32_t nid = 0;
    /* r3..r10, then the ninth..twelfth argument in the caller's parameter save
     * area (r1+0x70, 0x78, 0x80, 0x88) as the ELFv1 call ABI places them. */
    Arg args[12];
    std::vector<Capture> caps;
    std::function<void()> pre;      /* guest-memory edit before the step; runs on both sides */
    std::function<void()> post;     /* host-side action after a returned step; use on_hle_side() to act on one side */
    bool ret64 = false;             /* compare all of r3, not the low 32 bits */
};
Step step(const char* func, uint32_t nid, std::initializer_list<Arg> args = {});

struct Case {
    std::string id, func;           /* func: single-step shorthand ...          */
    uint32_t    nid = 0;
    uint64_t    args[8] = {0, 0, 0, 0, 0, 0, 0, 0};   /* ... r3..r10          */
    std::vector<Step> steps;        /* or an explicit sequence (func/nid/args then unused) */
    std::function<void()> setup;    /* writes the arena; runs before the fork */
    Expect      expect = Expect::Match;
    std::string note;               /* why a non-Match is expected */
    std::string row;                /* firmware-verdict row this case checks (e.g. "F09"), for reports */
    /* With Expect::Blocked, the harness-blocked reason the case is expected to
     * stop at (e.g. "unknown-instruction"), and optionally its 0-based step.
     * Empty: any harness block matches the declaration. A different block is
     * unexpected, so a new lifter gap cannot hide behind a declared one. */
    std::string blocker;
    int         blocker_step = -1;
    /* A machine-checkable witness for a non-Match verdict: "memory", "return",
     * "status", "imports", "abi", "hang", "fault-span", "stop-fields", ... .
     * Empty: the verdict alone is the declaration. A different witness is
     * unexpected, so a case cannot pass for an unrelated reason. The step
     * (-1 = any), the memory range the first differing byte must fall in, and
     * the stop kind complete the witness. */
    std::string expect_cause;
    int         expect_step = -1;
    uint32_t    expect_range_lo = 0, expect_range_hi = 0;   /* [lo, hi); empty when hi <= lo */
    std::string expect_stop_kind;
    int         timeout_s = 10;     /* per side; a hang is an outcome */
    /* Guest ranges left out of every memory comparison of this case: bytes whose difference is a
     * documented, understood firmware-only artefact (reported in the JSON as "masked"), so that
     * later steps of a sequence stay comparable. */
    std::vector<std::pair<uint32_t, uint32_t>> mask;   /* (ea, length) */
    /* Firmware-only writes inside the module image itself, given as offsets
     * from the module base. The report names them "image+<off>+<len>". */
    std::vector<std::pair<uint32_t, uint32_t>> mask_image;   /* (offset, length) */
};

/* ---- comparator dimensions ---------------------------------------------- */
/* V1 compares five things; each has a switch so tests can disable it and prove
 * that its mutation case goes undetected (tools/diff_harness/test_synth.py).
 * Production runs enable all of them. */
enum Dimension : uint32_t {
    DIM_RETURN     = 1u << 0,   /* r3 after every step and at the end */
    DIM_WRITES     = 1u << 1,   /* every byte the lifted side wrote, the HLE side wrote too */
    DIM_EXTRA      = 1u << 2,   /* every byte the HLE side wrote, the lifted side wrote too (padding) */
    DIM_IMPORTS    = 1u << 3,   /* import call sequence: NID, step, arguments, return */
    DIM_FAULTS     = 1u << 4,   /* guest-fault vs hle-guest-range normalisation */
    DIM_FAULT_SPAN = 1u << 5,   /* a fault pair needs overlapping spans */
    DIM_HANG       = 1u << 6,   /* two timeouts/two host crashes are inconclusive, never match */
    DIM_ALL        = (1u << 7) - 1,
};

/* ---- results ------------------------------------------------------------ */
struct ImportRec {
    uint32_t nid = 0; std::string name; Policy policy = Policy::Stop;
    uint64_t arg[4] = {0, 0, 0, 0}; uint64_t ret = 0; bool has_ret = false; uint32_t step = 0;
};
typedef std::vector<std::pair<uint32_t, uint8_t>> ByteDiff;   /* (ea, new byte) vs the pristine arena */

enum class SideKind { Returned, GuestFault, NamedStop, HostCrash, Timeout, HarnessBlocked };

struct SideState {
    SideKind kind = SideKind::Returned;
    uint32_t term_step = 0;              /* the step in flight when the side ended */
    std::string detail;
    /* guest-fault / named-stop span */
    bool has_span = false; uint64_t span_ea = 0, span_size = 0;
    std::string stop_kind;               /* named-stop kind, e.g. hle-guest-range */
    std::map<std::string, std::string> stop_fields;  /* canonical machine-line fields */
    int signal = 0;                      /* host-crash */
    std::string blocker;                 /* harness-blocked reason */
    std::string last_hle_nid;            /* from the stop line, for common-mode */
};

struct StepRes {
    std::string status;                  /* returned | no-hle | blocked */
    std::string blocker;                 /* harness-blocked reason */
    std::string detail;
    uint64_t r3 = 0, lr = 0, r1 = 0, r2 = 0;
    /* Bit i set: r(14+i) was clobbered by the lifted call. r14..r31 are the
     * ELFv1 nonvolatile set; a lifted function that does not restore them is
     * not a faithful call whatever r3 and memory say. */
    uint32_t abi_bad = 0;
    /* Exact cumulative diff of the compared surface after this step. The
     * comparison uses this, not a hash: a hash collision between two
     * intermediate steps a later step restores would hide a divergence. */
    ByteDiff diff;
    std::vector<std::pair<int, uint64_t>> caps;
};
struct SideResult {
    SideState state;
    std::vector<StepRes> steps;
    uint64_t r3 = 0, lr = 0, r1 = 0, r2 = 0;       /* last completed step */
    ByteDiff diff;                                 /* compared surface vs pristine */
    ByteDiff img_diff;                             /* subset inside the module image */
    std::vector<ImportRec> imports;                /* HLE side: nested calls; lifted side: import stubs */
    bool wire_ok = false;                          /* a valid final record arrived */
};

/* ---- internal-function overrides (lifted side only) --------------------- */
/* Import models stand in for what a module CALLS OUT to. An override stands in for a function
 * INSIDE the module, named by a symbol of the generated table (`dh_symbol`): the lifted code's
 * own calls reach the replacement instead of the lifted body, with the guest registers as the
 * caller left them. It is how a case puts a scripted peer (a fake service behind the module's
 * IPC primitives) where the real peer is not available. The seam is backend-neutral: a
 * later interpreted side would intercept the same guest address through the same symbol.
 *
 * The hook returns true when it handled the call (r3 etc. set as the callee would), false to
 * run the original lifted function. Every handled call is logged as an import record named
 * `override:<symbol>` (policy model), so the report shows what the lifted side did NOT run.
 * The module's tables must carry the symbol and the lifted C must have been wrapped by
 * tools/diff_harness/apply_overrides.py; otherwise the run stops with a harness error. */
typedef bool (*override_fn)(void* /*ppu_context* */);
void override_symbol(const char* symbol, override_fn fn);

struct Options {
    std::string image;        /* override dh_module.default_image */
    std::string out;          /* result file: one JSON document, atomic rename (required) */
    std::string filter;       /* run only cases whose id contains this */
    bool        verbose = false;
    uint32_t    disable_dimensions = 0;   /* diagnostic/test only; see Dimension */
    /* Guards against a vacuous pass (a case table that silently shrank, or a
     * declared DIVERGE that stopped being observed): fail the run unless at
     * least min_cases ran and, when expect_diverge >= 0, exactly that many
     * cases came out as diverge. Test-only like disable_dimensions. */
    int         min_cases = 0;
    int         expect_diverge = -1;
    /* Test-only, like disable_dimensions: run the lifted side without any override, to prove
     * that a case matches BECAUSE of its override (tools/diff_harness/test_synth.py). */
    bool        no_overrides = false;
    /* Test-only free-form switches a case table may read through case_flag() (e.g. the
     * synthetic fixture's "mutate-override", which makes its override answer wrongly). */
    std::set<std::string> case_flags;
};

/* For import/syscall models: abort the lifted call with a named reason (verdict
 * lifted-blocked), or block for good (the case timeout then turns it into a
 * timeout outcome on that side, exactly as the HLE's own blocking does). */
[[noreturn]] void model_stop(const char* reason);
[[noreturn]] void model_block(const char* what);

/* A host function standing in for a guest function that a library calls back (a fiber entry, a
 * callback): register it at a guest code address and put {addr, 0} in an OPD the case passes. */
void register_guest_fn(uint32_t code_addr, void (*fn)(void* /*ppu_context* */));

/* Call the guest function whose OPD is at `opd` from host code (the HLE's guest-caller hook):
 * registered host functions only, r3..r10 = a0..a7. An OPD that names no registered function is
 * a harness block, never a silent return. */
void guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);

bool case_flag(const char* name);
void set_case_flags(const std::set<std::string>& flags);   /* dh_main, before register_cases() */

/* Registered by the library's case file. */
void register_cases(std::vector<Case>& out);
ImportPolicy import_policy();

int run(const dh_module& mod, std::vector<Case>& cases, const ImportPolicy& pol, const Options& opt);

}  // namespace dh
#endif
