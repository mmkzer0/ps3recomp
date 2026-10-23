/* spu_workload.h — SPU workload / task dispatch registry.
 *
 * The bridge cellSpurs needs: when a title registers a SPURS workload
 * (cellSpursAddWorkload, a "policy module" SPU binary) or a SPURS task
 * (cellSpursCreateTask, a task ELF), the runtime must actually RUN that SPU
 * program. We do not interpret SPU ELFs at runtime — we statically lift each
 * title's SPU binaries ahead of time (spu_lifter -> a native entry fn). This
 * layer maps a registered SPU image, by content FINGERPRINT, to its pre-lifted
 * native entry, loads the image into a fresh 256 KB local store, and runs it
 * with the SPURS task ABI (job/task arg EA in r3) via the lifted-job adapter.
 *
 * Flow:
 *   1) At init, the title's generated registration code calls
 *      spu_workload_register(fingerprint, lifted_entry, "name") once per lifted
 *      SPU binary. The fingerprint is FNV-1a-64 over the exact ELF image bytes
 *      (the same bytes the title later hands to cellSpurs), computed identically
 *      here and by the offline extractor (so a manifest can be emitted).
 *   2) cellSpursAddWorkload / cellSpursCreateTask call spu_workload_dispatch()
 *      with the guest image -> fingerprint -> lifted entry -> load LS -> run.
 *
 * This is title-agnostic ps3recomp runtime: the registry is populated by the
 * title's lifted SPU set; the dispatch mechanism is generic.
 */
#ifndef SPU_WORKLOAD_H
#define SPU_WORKLOAD_H

#include "spu_context.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*spu_lifted_entry_fn)(spu_context*);

/* FNV-1a 64-bit over a byte range. The canonical workload fingerprint; the
 * offline extractor computes the same value so registrations and the images a
 * title passes to cellSpurs line up. */
uint64_t spu_workload_fingerprint(const void* data, size_t n);

/* Register one lifted SPU binary under its image fingerprint. `name` is borrowed
 * (kept by pointer for logging — pass a string literal / static). Idempotent on
 * fingerprint: a second register of the same fp updates the entry. */
void spu_workload_register(uint64_t fingerprint, spu_lifted_entry_fn fn,
                           const char* name);

/* As spu_workload_register, but binds the image's indirect-branch table id
 * (the value passed to spu_begin_image() before this image's recomp_register).
 * dispatch sets the run context's image_id to this so spu_indirect_branch
 * resolves within the correct image when several share LS addresses. */
void spu_workload_register_img(uint64_t fingerprint, spu_lifted_entry_fn fn,
                               int image_id, const char* name);

/* Look up a lifted entry by fingerprint; NULL if none registered. */
spu_lifted_entry_fn spu_workload_find(uint64_t fingerprint);

/* As spu_workload_find, but also returns the image's indirect-branch table id. */
spu_lifted_entry_fn spu_workload_find_img(uint64_t fingerprint, int* image_id_out);

/* Run a SPURS policy module (raw PM binary loaded at LS 0xA00) under the HLE'd
 * kernel handoff: kernel context at LS 0x100, entry ABI of
 * cellSpursModuleEntry(context, ea) with the workload's u64 data in r4.
 * Returns 0 if the module exited back to the kernel, negative otherwise.
 * (spurs_policy.c) */
/* spu_num selects the module's VIRTUAL SPU (kernel context +0x1C8). The WWS
 * job manager keys its per-SPU ticket LANE off this (lane row = spuNum*16 in
 * the workload sync header) and its command lists contain cross-lane barrier
 * commands, so a workload configured for N SPUs must be dispatched once per
 * spu_num or the un-run lanes deadlock every barrier. */
/* Retire resident policy contexts after the owning scheduler has joined. */
void spurs_policy_release_instance(uint32_t spurs_ea);
int spurs_policy_release_workload(uint32_t spurs_ea, uint32_t wid);

int spu_run_policy_module(spu_lifted_entry_fn entry, int image_id,
                          const uint8_t* pm_host, uint32_t pm_size,
                          uint64_t wkl_data, uint32_t wid, uint32_t spurs_ea,
                          uint32_t spu_num);

/* Stage and enter one SPURS jobchain job (spurs_job.c). Unlike a policy module
 * or a task, a job binary is a raw image built by the SDK's job_elf-to-bin and
 * linked against Sony's job_start_w_crt.o, whose entry contract is
 *   cellSpursJobMain2(CellSpursJobContext2 *ctx, CellSpursJob256 *job)
 * with r3 = ctx, r4 = job and BOTH pointers into local store. This lays out
 * LS the way jm2 does (per
 * _cellSpursCheckJob in the SDK's job_descriptor.h: binary+bss, ioBuffer,
 * oBuffer, unified scratch+stack, cacheBuffer[0..3], all 1024-aligned), fills
 * ioBuffer from the descriptor's input DMA list, builds the context, and runs
 * the lifted entry. `job_desc_size` is the jobchain's sizeJobDescriptor (0 =>
 * assume 256). Returns 0 if the job ran, negative if it could not be staged. */
int spu_run_spurs_job(spu_lifted_entry_fn entry, int image_id,
                      uint32_t job_ea, uint32_t job_desc_size);

/* The same, for a job a queue has admitted. `desc`, desc_size bytes (<= 1 KB),
 * is the descriptor as it was when the job was admitted: every field the loader
 * reads (header, DMA and cache lists, user words) and the copy placed in local
 * store come from it, never from guest memory, so a store to the guest
 * descriptor after admission does not reach the job. `job_ea` is still the EA
 * the job sees as its eaJobDescriptor. Reads past desc_size are 0. The finished
 * local store is not retained for spurs_job_ls_for_handle, so jobs may run on
 * several threads at once. Same return as spu_run_spurs_job. */
int spu_run_spurs_job_snapshot(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea,
                               const uint8_t* desc, uint32_t desc_size);

/* What spu_run_spurs_job_hooked tells a job observer about the local store it
 * laid out, all in bytes and LS offsets: the image from LS 0 (`image_bytes` the
 * descriptor's sizeBinary, `bss_bytes` the eaBinary high word x 16), the
 * descriptor copy, the ioBuffer and oBuffer (sizeInOrInOut / sizeOut), the
 * scratch and stack block, and the CellSpursJobContext2. `r1` is the stack
 * pointer the job is entered with. `image_ea` is the guest EA of the image. */
typedef struct spu_job_layout {
    uint32_t image_ea, image_ls, image_bytes, bss_bytes;
    uint32_t desc_ls, desc_bytes;
    uint32_t io_ls, io_bytes;
    uint32_t out_ls, out_bytes;
    uint32_t scratch_ls, scratch_bytes, stack_bytes;
    uint32_t r1;
    uint32_t ctx_ls;
} spu_job_layout;

/* Called on the running thread with the job's spu_context: `start` after the
 * job is staged and before its first instruction (the place to install an
 * spu_observe observer on `ctx`), `end` once the entry returned. Either may be
 * NULL. A job that ends in a named stop never reaches `end`. */
typedef struct spu_job_hooks {
    void* user;
    void (*start)(void* user, spu_context* ctx, const spu_job_layout* layout);
    void (*end)(void* user, spu_context* ctx);
} spu_job_hooks;

/* spu_run_spurs_job_snapshot with job hooks (NULL: none, the same call). */
int spu_run_spurs_job_hooked(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea,
                             const uint8_t* desc, uint32_t desc_size, const spu_job_hooks* hooks);

/* The JobQueue2 job profile: a job a JobQueue2 worker runs is staged the way
 * the JobQueue2 policy module (PM) stages it, which is NOT the jobchain layout
 * above (docs/RUNTIME.md, "SPURS job start"; evidence in docs/JQ2.md). The PM's
 * placement is fixed by queue and descriptor fields: image base 0x7C00, 128-
 * byte aligned regions in the order image, cache buffers, io, out, scratch+
 * stack, descriptor copy and JobContext2 at their own addresses, a kernel view
 * of exactly seven words, and a jm interface at 0x7A10 = {0x1500, 0x5870}. */
#define SPU_JQ2_IMAGE_LS      0x7C00u   /* image base (the PM's low heap start) */
#define SPU_JQ2_DESC_TOP      0x3F400u  /* descriptor arena top: D0 = top - 4*maxDesc */
#define SPU_JQ2_CTX_LS        0x68D0u   /* JobContext2 of Execute slot 0 (+0x30 per slot) */
#define SPU_JQ2_JM_LS         0x7A10u   /* the jm interface table */
#define SPU_JQ2_JM_CALL_LS    0x1500u   /* jm word 0: the scheduler switch (a stop here) */
#define SPU_JQ2_JM_SWAP_LS    0x5870u   /* jm word 1: the tag-status poll routine */
/* The stop code of the word planted at SPU_JQ2_JM_CALL_LS: a job that ends on
 * it called jm word 0, which one job per worker has no manager to serve. */
#define SPU_JQ2_JM_CALL_STOP  0x3B0Fu

/* What the queue contributes to the staging (the PM reads these from the queue
 * block and its kernel context). `max_desc` is the queue's maxDescriptor
 * (queue+0xF14; 0 means 256), `spurs_ea` the CellSpurs EA (queue+0xF10),
 * `wid` the queue's workload id and `spu_num` the SPU the worker stands for
 * (kernel context 0x1D8/0x1DC and 0x1C8). */
typedef struct spu_jq2_env {
    uint32_t max_desc;
    uint32_t spurs_ea;
    uint32_t wid;
    uint32_t spu_num;
} spu_jq2_env;

/* spu_run_spurs_job_hooked with the JobQueue2 profile. A worker runs one job
 * at a time, so the job is the PM's Execute slot 0: its descriptor copy sits at
 * D0 and its context at SPU_JQ2_CTX_LS. The descriptor, DMA-list and cache
 * validation is the loader's one; a heap that would pass D0 is the named stop
 * spu-job-memory with `limit` D0. The job is entered at SPU_JQ2_IMAGE_LS with
 * the PM's registers (docs/JQ2.md). `end` runs with the finished context; the
 * stop code SPU_JQ2_JM_CALL_STOP at SPU_JQ2_JM_CALL_LS + 4 means the job called
 * jm word 0. Same return as spu_run_spurs_job; -1 also for a `max_desc` other
 * than 0 or 256..0x3FF in 128-byte steps (the range queue creation accepts). */
int spu_run_jq2_job(spu_lifted_entry_fn entry, int image_id, uint32_t job_ea,
                    const uint8_t* desc, uint32_t desc_size, const spu_jq2_env* env,
                    const spu_job_hooks* hooks);

/* Fingerprint `image`, look up its lifted entry, and run it as a SPURS jobchain
 * job via spu_run_spurs_job. Runs SYNCHRONOUSLY on the caller's thread: a job
 * is not persistent (it runs to completion and returns to the manager), and a
 * chain's jobs are ordered by its own NEXT/CALL/RET commands, so the jobchain
 * thread is the right place to run them. Returns 1 if a lifted image matched
 * and ran, 0 if none is registered for this binary. */
int spu_workload_dispatch_job(const uint8_t* image, uint32_t image_size,
                              uint32_t job_ea, uint32_t job_desc_size);

/* Load a 32-bit big-endian SPU ELF image into a 256 KB local store: each PT_LOAD
 * segment is copied to its p_vaddr (BSS zero-filled). Returns 1 on success and
 * writes the entry vaddr (e_entry, which may legitimately be 0) to *entry_out;
 * returns 0 if `image` is not a valid SPU ELF or a segment is out of range.
 * `ls` must point to SPU_LS_SIZE bytes (caller-zeroed if a clean BSS is wanted). */
int spu_elf_load_to_ls(const uint8_t* image, size_t image_size, uint8_t* ls,
                       uint32_t* entry_out);

/* Compute the true on-disk size of a 32-bit BE SPU ELF: the extent that spans
 * the section-header table and all program/section content (max p_offset+p_filesz
 * and non-NOBITS sh_offset+sh_size). This matches the offline extractor's sizing,
 * so fingerprints agree. Use it when a caller (e.g. cellSpursCreateTask) is handed
 * an SPU ELF pointer without an explicit length. `max_avail` bounds the read in
 * case the header is corrupt; returns 0 if `image` is not a valid SPU ELF. */
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail);

/* Dispatch a registered SPU image: fingerprint `image`, find its lifted entry,
 * load it into a fresh local store, and run it with `args_ea` in r3 (SPURS task
 * ABI). Returns 1 if an image matched and ran, 0 if no lifted entry is
 * registered for it (caller may fall back to logging/stubbing). */
int spu_workload_dispatch(const uint8_t* image, uint32_t image_size,
                          uint32_t args_ea);

/* Same, but runs the SPU job on its own detached host thread so the PPU caller
 * is not blocked. Required for persistent SPURS service/worker tasks that loop
 * waiting on PPU-side signals (running them inline deadlocks). */
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size,
                                uint32_t args_ea);

/* Number of currently registered lifted SPU binaries (diagnostics/tests). */
unsigned spu_workload_count(void);

#ifdef __cplusplus
}
#endif

#endif /* SPU_WORKLOAD_H */
