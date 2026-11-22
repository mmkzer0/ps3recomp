/* cellSpurs argument limits. Manufactured policy module only, no game code. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/spu/spu_coherency.h"
#include <sys/wait.h>   /* the stop cases fork: the fixture is POSIX only (the driver skips elsewhere) */
#include <unistd.h>

void spu_indirect_branch(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi,g_barrier_sync_watch;
uint32_t g_spu_image_src_ea,g_spu_image_ls_start,g_spu_image_span;
unsigned long long ps3_ms_now(void) { LARGE_INTEGER n,f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return n.QuadPart*1000/f.QuadPart; }
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
void sys_event_queue_cancel_by_id(u32 id) {(void)id;}
void sys_event_queue_uncancel_by_id(u32 id) {(void)id;}
void (*g_rsx_main_release_hook)(uint32_t line);   /* libs/video/rsx_commands.c */
void ydkj_wake_all_event_flags(void) {}
uint64_t spurs_pm_build_context(uint8_t* ls,uint32_t a,uint32_t b,uint32_t c,uint32_t d){(void)ls;(void)a;(void)b;(void)c;(void)d;return 0;}
void spu_thread_publish_ctx(uint32_t tid,void* ctx){(void)tid;(void)ctx;}

static atomic_uint gate;
static void hold_policy(spu_context* c)
{
    u32 ea=spu_ls_read32(c,0x1C4),wid=spu_ls_read32(c,0x1DC);
    while(!atomic_load(&gate)) Sleep(1);
    vm_write8(ea+0x80+wid,3);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void finalize(u32 ea)
{
    for(unsigned i=0;i<3000;i++) {
        s32 rc=cellSpursFinalize((CellSpurs*)(uintptr_t)ea);
        if (!rc) return;
        assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY);Sleep(1);
    }
    assert(!"finalize failed");
}

/* _cellSpursWorkloadAttributeInitialize through its context handler (upstream #646). */
static ppu_context g_attr_ctx;
static void attr_args(u32 attr, u32 prio, u32 sp)
{
    memset(&g_attr_ctx,0,sizeof g_attr_ctx);
    g_attr_ctx.gpr[1]=sp; g_attr_ctx.gpr[3]=attr; g_attr_ctx.gpr[4]=1; g_attr_ctx.gpr[5]=0x00350001u;
    g_attr_ctx.gpr[6]=0x9000; g_attr_ctx.gpr[7]=16; g_attr_ctx.gpr[8]=0; g_attr_ctx.gpr[9]=prio; g_attr_ctx.gpr[10]=1;
}
/* A refused span ends the process in the named stop hle-guest-range, so the call runs in a forked child (before any
 * SPURS thread exists) and the parent reads the machine line the stop prints on stderr. */
static void attr_init_stops(const char* arg)
{
    int fds[2];
    assert(pipe(fds)==0);
    fflush(stdout);
    const pid_t pid=fork();
    assert(pid>=0);
    if (!pid) {
        close(fds[0]); dup2(fds[1],STDERR_FILENO); close(fds[1]);
        hle_workload_attribute_initialize(&g_attr_ctx);
        _exit(1);   /* returned: the span was not refused */
    }
    close(fds[1]);
    char out[4096]; size_t used=0; ssize_t n;
    while (used<sizeof out-1 && (n=read(fds[0],out+used,sizeof out-1-used))>0) used+=(size_t)n;
    out[used]=0; close(fds[0]);
    int status=0;
    waitpid(pid,&status,0);
    char want[128];
    snprintf(want,sizeof want,"func=_cellSpursWorkloadAttributeInitialize arg=%s",arg);
    const char* line=strstr(out,"PS3STOP v=1");
    if (!(WIFEXITED(status) && WEXITSTATUS(status)==PS3_STOP_EXIT_CODE && line &&
          strstr(line,"kind=hle-guest-range") && strstr(line,want))) {
        printf("FAIL %s: status 0x%X, stop line: %s\n",arg,status,line?line:"(none)");
        exit(1);
    }
}

int main(void)
{
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);

    /* Each span the title chose stops when it leaves guest memory: the attribute, the priorities, and the stack slot
     * the ninth argument comes from. */
    attr_args(0x20000u-0x20u,0,0xD000); attr_init_stops("attr");                /* straddles the end */
    attr_args(0xC000,0x20000u-4u,0xD000); attr_init_stops("priority");
    attr_args(0xC000,0,0x20000u-112u-4u); attr_init_stops("maxContention");     /* r1 + 112 straddles the end */
    puts("SPURS workload attribute spans outside guest memory stop PASS");
    memset(vm_base+0x9000,0x11,16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base+0x9000,16),hold_policy,77,"api limits fixture");
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);

    /* maxContention is a u8 in the instance; 256 must not wrap to 0 ("none"). */
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,256)==0);
    assert(vm_read32(0x8000)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==CELL_SPURS_MAX_SPU);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,300)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==CELL_SPURS_MAX_SPU);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,3)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==3);
    puts("SPURS maxContention above 8 clamps instead of wrapping PASS");

    /* RequestIdleSpu on a slot that is not in use must fail and leave it alone. */
    assert((u32)cellSpursRequestIdleSpu(0x1000,5,2)==CELL_SPURS_POLICY_MODULE_ERROR_SRCH);
    assert(vm_read8(0x1000+SPURS_WKL_IDLE2+5)==0);
    assert((u32)cellSpursRequestIdleSpu(0x1000,16,2)==CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    assert(cellSpursRequestIdleSpu(0x1000,0,2)==0);
    assert(vm_read8(0x1000+SPURS_WKL_IDLE2)==2);
    puts("SPURS RequestIdleSpu rejects a workload slot that is not in use PASS");

    /* cellSpursGetInfo hands out the SPU thread ids a title passes to sys_spu_thread_write_snr; cellSpurs_post_snr,
     * which the syscall falls back to for an id with no thread of its own, takes those and nothing else. */
    memset(vm_base+0xA000,0xEE,0x60);
    assert(cellSpursGetInfo(0x1000,0xA000)==0);
    assert(vm_read32(0xA000)==2);                                  /* nSpus */
    u32 group=vm_read32(0xA024),t0=vm_read32(0xA028),t1=vm_read32(0xA02C);
    assert(group && t0 && t1 && t0!=t1 && group!=t0);
    assert(vm_read32(0xA030)==0 && vm_read32(0xA044)==0);          /* the slots of SPUs this instance lacks */
    assert(vm_read32(0xA048)==0xEEEEEEEEu);                        /* and nothing written past the struct */
    assert(cellSpurs_post_snr(t0,0,0x11)==0 && cellSpurs_post_snr(t1,1,0x22)==0);
    assert(cellSpurs_post_snr(t0,2,1)==-1);                        /* SNR 2 does not exist */
    assert(cellSpurs_post_snr((t0&~0xFu)|2,0,1)==-1);              /* nor does SPU 2 of a 2-SPU instance */
    assert(cellSpurs_post_snr(group,0,1)==-1);                     /* the group id is no SPU thread */
    assert(cellSpurs_post_snr(0x1000,0,1)==-1);                    /* an id of another kind */
    puts("SPURS GetInfo SPU thread ids take sys_spu_thread_write_snr PASS");

    /* libsre 4.81 0x0200AE80 answers this API in the policy module family
     * (ori immediates 0x8xx), checks spurs & 0x7F (ALIGN), refuses count > 8
     * (INVAL), and answers STAT outright on a CellSpurs2 -- the 32-workload
     * flag at +0x74 means the idle-spu byte array at +0x10 does not exist
     * there (it is the second set's ready counts). The pre-#127 code checked
     * neither alignment nor count and answered the wid bound in the core
     * family, so each pin below fails against it. */
    assert((u32)cellSpursRequestIdleSpu(0x1001,0,1)==CELL_SPURS_POLICY_MODULE_ERROR_ALIGN);
    assert((u32)cellSpursRequestIdleSpu(0x1000,0,9)==CELL_SPURS_POLICY_MODULE_ERROR_INVAL);
    assert(vm_read8(0x1000+SPURS_WKL_IDLE2)==2);   /* the refused count wrote nothing */
    assert((u32)cellSpursRequestIdleSpu(0x1000,0x20,1)==CELL_SPURS_POLICY_MODULE_ERROR_INVAL);

    /* SetMaxContention (libsre 4.81 0x02008F88, core family): the INVAL
     * contract is the workload id -- 16 on this instance, 32 on a CellSpurs2
     * -- checked before the enabled mask (SRCH) and the exception word
     * (STAT). */
    assert((u32)cellSpursSetMaxContention((CellSpurs*)0x1000,16,1)==CELL_SPURS_CORE_ERROR_INVAL);
    assert((u32)cellSpursSetMaxContention((CellSpurs*)0x1001,0,1)==CELL_SPURS_CORE_ERROR_ALIGN);
    vm_write32(0x1000+SPURS_WKL_EXCEPTION,1);
    assert((u32)cellSpursSetMaxContention((CellSpurs*)0x1000,0,1)==CELL_SPURS_CORE_ERROR_STAT);
    assert((u32)cellSpursRequestIdleSpu(0x1000,0,1)==CELL_SPURS_POLICY_MODULE_ERROR_STAT);
    vm_write32(0x1000+SPURS_WKL_EXCEPTION,0);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,5)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==5);

    /* A CellSpurs2 instance: the attribute is built by the 7-argument export,
     * initialized with 2 through cellSpursInitializeWithAttribute2. */
    {
        assert(_cellSpursAttributeInitialize((CellSpursAttribute*)0x4000,1,0x00350001u,2,1,1,0)==0);
        assert(cellSpursInitializeWithAttribute2((CellSpurs*)0x5000,(CellSpursAttribute*)0x4000)==0);
        assert(cellSpursAddWorkload((CellSpurs*)0x5000,(u32*)0x8100,(void*)0x9000,16,0,NULL,1,8)==0);
        /* RequestIdleSpu: STAT on a CellSpurs2 -- also for a wid the core
         * bound would have taken (the pre-#127 code answered INVAL for
         * wid >= 16 and SRCH below it). */
        assert((u32)cellSpursRequestIdleSpu(0x5000,0,1)==CELL_SPURS_POLICY_MODULE_ERROR_STAT);
        assert((u32)cellSpursRequestIdleSpu(0x5000,20,1)==CELL_SPURS_POLICY_MODULE_ERROR_STAT);
        /* SetMaxContention accepts the second set's ids as far as 31; 32 is
         * past the firmware bound. The workload table assigned id 1 here
         * (0x1000 took 0), so the nibble check is that id's pair byte. */
        /* The enabled gate holds for the second set too: 31 passes the id
         * bound but is not enabled here, so it answers SRCH (libsre checks
         * the mask at +0xB0 before the store). */
        assert((u32)cellSpursSetMaxContention((CellSpurs*)0x5000,31,3)==CELL_SPURS_CORE_ERROR_SRCH);
        assert((u32)cellSpursSetMaxContention((CellSpurs*)0x5000,32,1)==CELL_SPURS_CORE_ERROR_INVAL);
        assert(cellSpursSetMaxContention((CellSpurs*)0x5000,1,3)==0);
        assert(vm_read8(0x5000+SPURS_WKL_MAXCONT+1)==0x03);
    }
    /* The ninth argument of _cellSpursWorkloadAttributeInitialize comes from the caller's parameter save area (64-bit
     * PowerPC ELF ABI: r1 + 48 + 8 * 8) and reaches the instance through AddWorkloadWithAttribute; the 8-GPR adapter
     * stored 1. */
    {
        memset(vm_base+0xC100,1,8);
        attr_args(0xC000,0xC100,0xD000);
        vm_write64(0xD000+112,5);
        hle_workload_attribute_initialize(&g_attr_ctx);
        assert(g_attr_ctx.gpr[3]==0);
        assert(vm_read32(0xC000+WKATTR_MAX_CONT)==5);
        assert(cellSpursAddWorkloadWithAttribute((CellSpurs*)0x1000,(u32*)0x8200,
                                                 (const CellSpursWorkloadAttribute*)0xC000)==0);
        const u32 w=vm_read32(0x8200);
        assert(w<16);
        assert(vm_read8(0x1000+SPURS_WKL_MAXCONT+w)==5);
        puts("SPURS workload attribute maxContention from the stack reaches the instance PASS");
    }
    /* Both instances run hold_policy (the same image at 0x9000), so an SPU of
     * either may sit in it; finalize answers BUSY until the gate lets it exit.
     * Open the gate before the first finalize, not between them. */
    atomic_store(&gate,1);
    finalize(0x5000);   /* the CellSpurs2 coordinator must stop before exit */
    finalize(0x1000);
    free(vm_base);return 0;
}
