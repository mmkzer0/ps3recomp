/* cellSpurs argument limits. Manufactured policy module only, no game code. */
#include "../libs/spurs/cellSpurs.c"
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/spu/spu_coherency.h"

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

int main(void)
{
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);
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
    finalize(0x5000);   /* the CellSpurs2 coordinator must stop before exit */

    atomic_store(&gate,1);
    finalize(0x1000);
    free(vm_base);return 0;
}
