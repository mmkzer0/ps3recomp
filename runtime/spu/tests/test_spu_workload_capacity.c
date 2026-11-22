/* The default workload registry holds 4096 images (upstream #787). A title that patches a counter into
 * each of 1024 per-task copies of one image registers 1024 fingerprints for it on top of the normal set;
 * with 256 entries the rest were dropped ("registry full") and those copies ran without their lifted
 * entry. Registers exactly SPU_WORKLOAD_MAX distinct images, finds each again, and checks the next one is
 * refused while an image already in the table can still be re-registered.
 *
 * tools/test_spu_workload_capacity.py builds and runs it, linked as for test_spu_workload_masked.c. */
#include "../spu_workload.c"
#include <assert.h>

/* The rest of spu_workload.c (dispatch, SPURS jobs, the policy-module runner)
 * is outside this fixture. */
uint8_t* vm_base;
uint32_t ppu_vm_size = 0x400000;
uint32_t g_barrier_sync_watch;
void (*g_spu_out_mbox_hook)(uint32_t, uint32_t, int, uint32_t);
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskid, g_ydkj_real_taskset_ea;
unsigned long long ps3_ms_now(void) { return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_coh_unregister(spu_context* c) { (void)c; }
void spu_coh_watch_add(spu_coh_watch* w, uint32_t ea, void (*wake)(void*), void* opaque)
{ (void)w; (void)ea; (void)wake; (void)opaque; }
void spu_coh_watch_remove(spu_coh_watch* w) { (void)w; }
void spu_lockline_lock(void) { }
void spu_lockline_unlock(void) { }
void spu_mfc_release(spu_context* c) { (void)c; }
void spu_mfc_forget_range(uintptr_t lo, uintptr_t hi) { (void)lo; (void)hi; }
int spu_run_spurs_job(spu_lifted_entry_fn e, int id, uint32_t ea, uint32_t sz)
{ (void)e; (void)id; (void)ea; (void)sz; return -1; }
int spu_run_with_halt(void (*f)(spu_context*), spu_context* c) { (void)f; (void)c; return -1; }
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; }
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ (void)ls; (void)a; (void)b; (void)c; (void)d; return 0; }
int spurs_run_taskset_policy_probe(uint32_t a, uint32_t b, uint32_t c, uint64_t d,
                                   uint32_t e, uint8_t* ls, uint32_t f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)ls; (void)f; return -1; }
void tsp_spu_func_00000A00(spu_context* c) { (void)c; }
void ydkj_wake_all_event_flags(void) { }

static void prog(spu_context* c) { (void)c; }
static void prog_again(spu_context* c) { (void)c; }

int main(void)
{
    assert(SPU_WORKLOAD_MAX == 4096);
    for (unsigned i = 0; i < SPU_WORKLOAD_MAX; i++)
        spu_workload_register_img(0x1000ULL + i, prog, (int)(i + 1), "image");
    for (unsigned i = 0; i < SPU_WORKLOAD_MAX; i++) {
        int id = -1;
        assert(spu_workload_find_img(0x1000ULL + i, &id) == prog && id == (int)(i + 1));
    }

    /* Full: a new image is refused, an image already registered is still updated in place. */
    spu_workload_register_img(0x1000ULL + SPU_WORKLOAD_MAX, prog, 9, "overflow");
    assert(spu_workload_find(0x1000ULL + SPU_WORKLOAD_MAX) == NULL);
    spu_workload_register_img(0x1000ULL, prog_again, 7, "again");
    {
        int id = -1;
        assert(spu_workload_find_img(0x1000ULL, &id) == prog_again && id == 7);
    }

    printf("test_spu_workload_capacity: ok\n");
    return 0;
}
