/*
 * ps3recomp - tests/spurs_jq2/test_jq2_range.cpp
 *
 * Range validation for JQ2 HLE: checks that guest EAs crossing ppu_vm_size
 * stop as hle-guest-range with the right arg and leave guest memory untouched,
 * while exact-fit spans ending at ppu_vm_size are accepted.
 */

#include "cellSpursJq.h"
#include "jq2/jq2.h"
#include "ps3emu/stop.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" {
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;
}

namespace {

constexpr uint32_t kLimit = 0x10000000; // 256 MB VM size

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s:%d: ", __FILE__, __LINE__);        \
            std::fprintf(stderr, __VA_ARGS__);                                 \
            std::fprintf(stderr, "\n");                                        \
            std::abort();                                                      \
        }                                                                      \
    } while (0)

int run_child(const std::function<void()>& child, std::string& err)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], 2);
        child();
        _exit(0);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) err.append(buf, std::size_t(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void expect_stop(const std::function<void()>& fn, const char* arg)
{
    std::string err;
    const int rc = run_child(fn, err);
    CHECK(rc == 70, "expected exit 70 for stop, got %d", rc);
    CHECK(err.find("kind=hle-guest-range") != std::string::npos,
          "expected kind=hle-guest-range in stderr, got: %s", err.c_str());
    const std::string want_arg = std::string("arg=") + arg;
    CHECK(err.find(want_arg) != std::string::npos,
          "expected %s in stderr, got: %s", want_arg.c_str(), err.c_str());
}

/* Helper to setup a valid dummy queue and attr in safe memory */
void setup_valid_attr(uint32_t ea)
{
    uint32_t classes_be = __builtin_bswap32(4);
    uint32_t max_desc_be = __builtin_bswap32(256);
    std::memcpy(vm_base + ea + 4, &classes_be, 4);
    std::memcpy(vm_base + ea + 8, &max_desc_be, 4);
}

void setup_pool_attr(uint32_t ea)
{
    // Class 0: 2 blocks of 64B = 128B; control area = 0x400 = 1024B. Total = 1152B (0x480).
    std::memset(vm_base + ea, 0, 0x20);
    uint32_t cnt = __builtin_bswap32(2);
    std::memcpy(vm_base + ea, &cnt, 4);
}

// -----------------------------------------------------------------------------
// 1. Attribute setters & Initialize
// -----------------------------------------------------------------------------

void test_attr_initialize()
{
    // Exact fit: [kLimit - 0x14, kLimit)
    const uint32_t exact_ea = kLimit - 0x14;
    std::memset(vm_base + exact_ea, 0x55, 0x14);
    int32_t rc = cellSpursJobQueueAttributeInitialize((CellSpursJobQueueAttribute*)(uintptr_t)exact_ea);
    CHECK(rc == JQ2_OK, "AttributeInitialize exact-fit failed: 0x%X", rc);

    // Bad: [kLimit - 0x14 + 1, kLimit + 1) crosses limit
    const uint32_t bad_ea = kLimit - 0x14 + 1;
    // Fill with sentinel
    std::memset(vm_base + bad_ea, 0xEE, 0x14 - 1);
    expect_stop([bad_ea] {
        cellSpursJobQueueAttributeInitialize((CellSpursJobQueueAttribute*)(uintptr_t)bad_ea);
    }, "attr");

    // Verify memory was untouched
    for (uint32_t i = 0; i < 0x14 - 1; ++i) {
        CHECK(vm_base[bad_ea + i] == 0xEE, "AttributeInitialize modified memory on stop at offset %u", i);
    }
}

void test_attr_set_max_size()
{
    // SetMaxSizeJobDescriptor writes at +0x08, size 4 -> bound [attr+8, +4)
    // Exact fit: attr = kLimit - 12 -> attr + 8 = kLimit - 4, span ends at kLimit
    const uint32_t exact_ea = kLimit - 12;
    std::memset(vm_base + exact_ea, 0, 12);
    int32_t rc = cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(
        (CellSpursJobQueueAttribute*)(uintptr_t)exact_ea, 256);
    CHECK(rc == JQ2_OK, "SetMaxSize exact-fit failed: 0x%X", rc);

    // Bad: attr = kLimit - 11 -> attr + 8 = kLimit - 3, span ends at kLimit + 1
    const uint32_t bad_ea = kLimit - 11;
    std::memset(vm_base + bad_ea, 0xEE, 11);
    expect_stop([bad_ea] {
        cellSpursJobQueueAttributeSetMaxSizeJobDescriptor(
            (CellSpursJobQueueAttribute*)(uintptr_t)bad_ea, 256);
    }, "attr");

    for (uint32_t i = 0; i < 11; ++i) {
        CHECK(vm_base[bad_ea + i] == 0xEE, "SetMaxSize modified memory on stop at offset %u", i);
    }
}

void test_attr_set_grab_parameters()
{
    // SetGrabParameters writes at +0x04 (+4) and +0x10 (+4) -> bounds [attr+4, +4), [attr+0x10, +4)
    // Exact fit: attr = kLimit - 0x14 -> ends at kLimit
    const uint32_t exact_ea = kLimit - 0x14;
    std::memset(vm_base + exact_ea, 0, 0x14);
    int32_t rc = cellSpursJobQueueAttributeSetGrabParameters(
        (CellSpursJobQueueAttribute*)(uintptr_t)exact_ea, 10, 4);
    CHECK(rc == JQ2_OK, "SetGrabParameters exact-fit failed: 0x%X", rc);

    // Bad: attr = kLimit - 0x13 -> ends at kLimit + 1
    const uint32_t bad_ea = kLimit - 0x13;
    std::memset(vm_base + bad_ea, 0xEE, 0x13);
    expect_stop([bad_ea] {
        cellSpursJobQueueAttributeSetGrabParameters(
            (CellSpursJobQueueAttribute*)(uintptr_t)bad_ea, 10, 4);
    }, "attr");

    for (uint32_t i = 0; i < 0x13; ++i) {
        CHECK(vm_base[bad_ea + i] == 0xEE, "SetGrabParameters modified memory on stop at offset %u", i);
    }
}

void test_attr_single_byte_setters()
{
    // SetSubmitWithEntryLock writes 1 byte at attr + 0x0C
    // Exact fit: attr = kLimit - 0x0D -> attr + 0x0C = kLimit - 1, size 1 -> ends at kLimit
    const uint32_t exact_ea = kLimit - 0x0D;
    vm_base[exact_ea + 0x0C] = 0;
    int32_t rc = cellSpursJobQueueAttributeSetSubmitWithEntryLock(
        (CellSpursJobQueueAttribute*)(uintptr_t)exact_ea, 1);
    CHECK(rc == JQ2_OK, "SetSubmitWithEntryLock exact-fit failed: 0x%X", rc);
    CHECK(vm_base[exact_ea + 0x0C] == 1, "SetSubmitWithEntryLock did not write byte");

    // Bad: attr = kLimit - 0x0C -> attr + 0x0C = kLimit, size 1 -> ends at kLimit + 1
    const uint32_t bad_ea = kLimit - 0x0C;
    std::memset(vm_base + bad_ea, 0xEE, 0x0C);
    expect_stop([bad_ea] {
        cellSpursJobQueueAttributeSetSubmitWithEntryLock(
            (CellSpursJobQueueAttribute*)(uintptr_t)bad_ea, 1);
    }, "attr");

    for (uint32_t i = 0; i < 0x0C; ++i) {
        CHECK(vm_base[bad_ea + i] == 0xEE, "SetSubmitWithEntryLock modified memory on stop at offset %u", i);
    }
}

// -----------------------------------------------------------------------------
// 2. Queue create (queue, command_ring, attr, pool, pool_attr)
// -----------------------------------------------------------------------------

void test_queue_create()
{
    const uint32_t safe_spurs = 0x10000;
    const uint32_t safe_queue = 0x20000;
    const uint32_t safe_attr = 0x30000;
    const uint32_t safe_ring = 0x40000;
    const uint32_t safe_pool = 0x50000;
    const uint32_t safe_pool_attr = 0x60000;
    setup_valid_attr(safe_attr);
    setup_pool_attr(safe_pool_attr);

    // Bad queue EA: 128-aligned, crosses limit: [kLimit - 0x1000 + 128, kLimit + 128)
    const uint32_t bad_queue = kLimit - 0x1000 + 128;
    std::memset(vm_base + bad_queue, 0xEE, 0x1000 - 128);
    expect_stop([=] {
        jq2_queue_create(1, 1, safe_spurs, bad_queue, safe_attr, 0, 0, 0, safe_ring, 16, 1, 0);
    }, "queue");
    // Ensure memory untouched
    for (uint32_t i = 0; i < 0x1000 - 128; ++i) {
        CHECK(vm_base[bad_queue + i] == 0xEE, "queue memory modified on stop at offset %u", i);
    }

    // Exact fit queue: [kLimit - 0x1000, kLimit)
    const uint32_t exact_queue = kLimit - 0x1000;
    int32_t rc = jq2_queue_create(1, 1, safe_spurs, exact_queue, safe_attr, 0, 0, 0, safe_ring, 16, 1, 0);
    CHECK(rc == JQ2_OK, "queue create exact-fit failed: 0x%X", rc);
    jq2_queue_shutdown(exact_queue);
    uint32_t ev = 0;
    jq2_queue_join(exact_queue, &ev);

    // Bad attr EA in queue_create: [kLimit - 0x0C + 4, kLimit + 4)
    const uint32_t bad_attr = kLimit - 0x0C + 4;
    expect_stop([=] {
        jq2_queue_create(1, 1, safe_spurs, safe_queue, bad_attr, 0, 0, 0, safe_ring, 16, 1, 0);
    }, "attr");

    // Bad command_ring EA: depth = 16 -> ring_size = 2176 = 0x880
    // 128-aligned bad_ring: kLimit - 0x880 + 128 -> span crosses kLimit by 128 bytes
    const uint32_t bad_ring = kLimit - 0x880 + 128;
    std::memset(vm_base + bad_ring, 0xEE, 0x880 - 128);
    expect_stop([=] {
        jq2_queue_create(1, 1, safe_spurs, safe_queue, safe_attr, 0, 0, 0, bad_ring, 16, 1, 0);
    }, "command_ring");
    for (uint32_t i = 0; i < 0x880 - 128; ++i) {
        CHECK(vm_base[bad_ring + i] == 0xEE, "command_ring memory modified on stop at offset %u", i);
    }

    // Exact fit command_ring: [kLimit - 0x880, kLimit)
    const uint32_t exact_ring = kLimit - 0x880;
    rc = jq2_queue_create(1, 1, safe_spurs, safe_queue, safe_attr, 0, 0, 0, exact_ring, 16, 1, 0);
    CHECK(rc == JQ2_OK, "ring exact-fit failed: 0x%X", rc);
    jq2_queue_shutdown(safe_queue);
    jq2_queue_join(safe_queue, &ev);

    // Bad pool_attr EA: kLimit (128-aligned, non-null) -> [kLimit, kLimit + 0x20) crosses limit
    expect_stop([=] {
        jq2_queue_create(1, 1, safe_spurs, safe_queue, safe_attr, safe_pool, kLimit, 0, safe_ring, 16, 1, 0);
    }, "pool_attr");

    // Bad pool EA: computed pool span for setup_pool_attr is 0x480 bytes
    // kLimit - 0x480 + 128 is 128-aligned, crosses limit by 128 bytes
    const uint32_t bad_pool = kLimit - 0x480 + 128;
    std::memset(vm_base + bad_pool, 0xEE, 0x480 - 128);
    expect_stop([=] {
        jq2_queue_create(1, 1, safe_spurs, safe_queue, safe_attr, bad_pool, safe_pool_attr, 0, safe_ring, 16, 1, 0);
    }, "pool");
    for (uint32_t i = 0; i < 0x480 - 128; ++i) {
        CHECK(vm_base[bad_pool + i] == 0xEE, "pool memory modified on stop at offset %u", i);
    }

    // Exact fit pool: kLimit - 0x480
    const uint32_t exact_pool = kLimit - 0x480;
    rc = jq2_queue_create(1, 1, safe_spurs, safe_queue, safe_attr, exact_pool, safe_pool_attr, 0, safe_ring, 16, 1, 0);
    CHECK(rc == JQ2_OK, "pool exact-fit failed: 0x%X", rc);
    jq2_queue_shutdown(safe_queue);
    jq2_queue_join(safe_queue, &ev);
}

// -----------------------------------------------------------------------------
// 3. Port create and consuming calls
// -----------------------------------------------------------------------------

void test_port_ops()
{
    const uint32_t safe_spurs = 0x10000;
    const uint32_t safe_queue = 0x20000;
    const uint32_t safe_attr = 0x30000;
    const uint32_t safe_ring = 0x40000;
    setup_valid_attr(safe_attr);
    int32_t rc = jq2_queue_create(1, 1, safe_spurs, safe_queue, safe_attr, 0, 0, 0, safe_ring, 16, 1, 0);
    CHECK(rc == JQ2_OK, "queue create failed: 0x%X", rc);

    // Bad port EA in port_create: kLimit (128-aligned, non-null) -> [kLimit, kLimit + 0x80)
    expect_stop([=] {
        jq2_port_create(kLimit, safe_queue);
    }, "port");

    // Exact fit port: kLimit - 0x80 (128-aligned) -> [kLimit - 0x80, kLimit)
    const uint32_t exact_port = kLimit - 0x80;
    rc = jq2_port_create(exact_port, safe_queue);
    CHECK(rc == JQ2_OK, "port create exact-fit failed: 0x%X", rc);
    jq2_port_destroy(exact_port);

    // Port consuming call with out-of-range port: push
    expect_stop([=] {
        jq2_port_push_job(kLimit, 0x70000, 64, 0, 0);
    }, "port");

    // Port consuming call with out-of-range port: sync
    expect_stop([=] {
        jq2_port_sync(kLimit, 4);
    }, "port");

    jq2_queue_shutdown(safe_queue);
    uint32_t ev = 0;
    jq2_queue_join(safe_queue, &ev);
}

// -----------------------------------------------------------------------------
// 4. Join exit_code
// -----------------------------------------------------------------------------

void test_join_exit_code()
{
    const uint32_t safe_spurs = 0x10000;
    const uint32_t safe_queue = 0x20000;
    const uint32_t safe_attr = 0x30000;
    const uint32_t safe_ring = 0x40000;
    setup_valid_attr(safe_attr);
    int32_t rc = jq2_queue_create(1, 1, safe_spurs, safe_queue, safe_attr, 0, 0, 0, safe_ring, 16, 1, 0);
    CHECK(rc == JQ2_OK, "queue create failed: 0x%X", rc);
    rc = cellSpursShutdownJobQueue((void*)(uintptr_t)safe_queue);
    CHECK(rc == JQ2_OK, "shutdown failed: 0x%X", rc);

    // Bad exit_code: kLimit - 3 -> span [kLimit - 3, kLimit + 1) crosses limit
    const uint32_t bad_exit = kLimit - 3;
    std::memset(vm_base + bad_exit, 0xEE, 3);
    expect_stop([=] {
        cellSpursJoinJobQueue((void*)(uintptr_t)safe_queue, (s32*)(uintptr_t)bad_exit);
    }, "exit_code");
    // Verify memory untouched
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(vm_base[bad_exit + i] == 0xEE, "exit_code memory modified on stop at offset %u", i);
    }

    // Exact-fit exit_code: kLimit - 4 -> span [kLimit - 4, kLimit)
    const uint32_t exact_exit = kLimit - 4;
    vm_base[exact_exit] = 0;
    rc = cellSpursJoinJobQueue((void*)(uintptr_t)safe_queue, (s32*)(uintptr_t)exact_exit);
    CHECK(rc == JQ2_OK, "Join exact-fit failed: 0x%X", rc);
}

} // namespace

int main()
{
    // Allocate 256MB VM using MAP_SHARED so parent sees memory untouched/touched
    vm_base = static_cast<uint8_t*>(mmap(nullptr, kLimit, PROT_READ | PROT_WRITE,
                                         MAP_SHARED | MAP_ANON, -1, 0));
    CHECK(vm_base != MAP_FAILED, "mmap failed");
    ppu_vm_size = kLimit;

    std::printf("[test_jq2_range] testing attribute setters...\n");
    test_attr_initialize();
    test_attr_set_max_size();
    test_attr_set_grab_parameters();
    test_attr_single_byte_setters();

    std::printf("[test_jq2_range] testing queue create bounds...\n");
    test_queue_create();

    std::printf("[test_jq2_range] testing port create and port consumers...\n");
    test_port_ops();

    std::printf("[test_jq2_range] testing join exit_code bound...\n");
    test_join_exit_code();

    std::printf("PASS: JQ2 guest memory range validation\n");
    munmap(vm_base, kLimit);
    return 0;
}
