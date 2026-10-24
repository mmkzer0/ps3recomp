/*
 * Kernel-produced event delivery: drop what lv2 drops, stop on a bad id.
 *
 * sys_event_queue_push_from_kernel serves producers with no return channel (an
 * SPU group/mailbox notification, the RSX ISR, the audio-period tick).
 * Oracle: RPCS3 cellAudio.cpp discards queues[i]->send(...)'s result and
 * sys_spu.cpp discards send_run_event's, so a full queue (the guest fell
 * behind) or a queue the guest destroyed is a legal drop: it must be counted
 * per cause and logged once per (producer, cause), never stop.
 *
 * An id that never named a queue (0, past the table, or an in-range slot no
 * create ever used) is a runtime bookkeeping error, not a guest situation, and
 * stops with PS3_STOP_EVENT_DELIVERY_LOST.
 *
 * The stop ends the process, so this probe exposes one scenario per run and
 * run_event_delivery.py asserts the exit status, counters, log lines and the
 * PS3STOP line.
 *
 * usage: test_event_delivery ok|full|destroyed|invalid|invalid-range
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "syscalls/sys_event.h"
#include "ppu_context.h"
#include "ps3emu/error_codes.h"

static uint8_t g_guest[0x10000];
uint8_t* vm_base = g_guest;

int ps3_log_verbose(void) { return 0; }
void ppu_dump_guest_stack(ppu_context* c, const char* t) { (void)c; (void)t; }
int spu_workload_dispatch_async(const uint8_t* e, uint32_t a, uint32_t b) { (void)e; (void)a; (void)b; return 0; }
size_t spu_elf_image_size(const uint8_t* elf, size_t max_scan) { (void)elf; (void)max_scan; return 0; }

static uint32_t queue_create(uint32_t capacity)
{
    uint32_t id = sys_event_queue_create_direct(0, (int32_t)capacity);
    if (id == 0) {
        fprintf(stderr, "queue_create_direct failed\n");
        exit(1);
    }
    return id;
}

static uint32_t queue_destroy(uint32_t id)
{
    ppu_context ctx = {0};
    ctx.gpr[3] = id;
    return (uint32_t)(int32_t)sys_event_queue_destroy(&ctx);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s ok|full|destroyed|invalid|invalid-range\n", argv[0]);
        return 2;
    }

    const uint64_t source = 0x1234;
    const char* producer = "test-event";
    uint32_t qid;

    if (strcmp(argv[1], "ok") == 0) {
        qid = queue_create(2);
        if (sys_event_queue_push_by_id(qid, source, 1, 2, 3) != 0) return 1;
        sys_event_queue_push_from_kernel(qid, source, 1, 2, 3, producer);
        if (g_sys_event_queues[qid - 1].count != 2) {
            fprintf(stderr, "events not counted: count=%d\n", g_sys_event_queues[qid - 1].count);
            return 1;
        }
        if (sys_event_kernel_drop_count(SYS_EVENT_KERNEL_DROP_FULL) != 0 ||
            sys_event_kernel_drop_count(SYS_EVENT_KERNEL_DROP_DESTROYED) != 0) {
            fprintf(stderr, "a delivered event was counted as a drop\n");
            return 1;
        }
        printf("delivered\n");
        return 0;
    }

    if (strcmp(argv[1], "full") == 0) {
        qid = queue_create(2);
        if (sys_event_queue_push_by_id(qid, source, 1, 2, 3) != 0) return 1;
        if (sys_event_queue_push_by_id(qid, source, 1, 2, 3) != 0) return 1;
        /* Two deliveries into the full queue: both drop, both count, and the
         * (producer, cause) log fires once. */
        sys_event_queue_push_from_kernel(qid, source, 1, 2, 3, producer);
        sys_event_queue_push_from_kernel(qid, source, 1, 2, 3, producer);
        printf("drops=%llu pending=%d\n",
               sys_event_kernel_drop_count(SYS_EVENT_KERNEL_DROP_FULL),
               g_sys_event_queues[qid - 1].count);
        return 0;
    }

    if (strcmp(argv[1], "destroyed") == 0) {
        qid = queue_create(2);
        if (queue_destroy(qid) != CELL_OK) return 1;
        sys_event_queue_push_from_kernel(qid, source, 1, 2, 3, producer);
        sys_event_queue_push_from_kernel(qid, source, 1, 2, 3, producer);
        printf("drops=%llu pending=%d\n",
               sys_event_kernel_drop_count(SYS_EVENT_KERNEL_DROP_DESTROYED),
               g_sys_event_queues[qid - 1].count);
        return 0;
    }

    if (strcmp(argv[1], "invalid") == 0) {
        /* In range, but no create ever used this slot: never existed. A
         * destroyed queue (above) must not take this path. */
        sys_event_queue_push_from_kernel(SYS_EVENT_QUEUE_MAX, source, 1, 2, 3, producer);
        fprintf(stderr, "a never-created id returned; its event was silently dropped\n");
        return 1;
    }

    if (strcmp(argv[1], "invalid-range") == 0) {
        sys_event_queue_push_from_kernel(SYS_EVENT_QUEUE_MAX + 1, source, 1, 2, 3, producer);
        fprintf(stderr, "an out-of-range id returned; its event was silently dropped\n");
        return 1;
    }

    fprintf(stderr, "unknown mode %s\n", argv[1]);
    return 2;
}
