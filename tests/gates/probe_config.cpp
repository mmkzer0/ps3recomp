/* Drives the configuration API from the command line; test_config.py supplies
 * the files and the environment and checks what comes out.
 *
 *   probe_config [--load <path>] <command> [args]
 *
 *   get            every option: key, value (read through its accessor), source
 *   dump           ps3_config_dump() to stdout
 *   hle <nid>      the [[hle.stub]] for a nid, or "none"
 *   lv2 <num>      the [[lv2.stub]] for a syscall number, or "none"
 *   load-again     ps3_config_load(NULL) after the configuration is in use
 *   race           8 threads read the configuration at once; prints whether
 *                  they all got the same object
 *   vscr           the VSCR a context made by ppu_context_init() starts with,
 *                  and ppu_context_clear()'s, which applies no policy
 */

#include "ps3emu/config.h"
#include "../../runtime/ppu/ppu_context.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

static void print_options()
{
    const ps3_config* c = ps3_config_get();
    for (size_t i = 0; i < ps3_config_option_count(); ++i) {
        const ps3_config_option* o = ps3_config_option_at(i);
        std::printf("%s.%s ", o->section, o->key);
        // Each value goes through the generated accessor, the way a call site reads it.
        if (!std::strcmp(o->section, "ppu") && !std::strcmp(o->key, "vscr_nj"))
            std::printf("%s", ps3_cfg_ppu_vscr_nj() ? "true" : "false");
        else if (!std::strcmp(o->section, "ppu") && !std::strcmp(o->key, "opd_swap_fixup"))
            std::printf("%s", ps3_cfg_ppu_opd_swap_fixup() ? "true" : "false");
        else if (!std::strcmp(o->section, "ppu") && !std::strcmp(o->key, "null_page"))
            std::printf("\"%s\"", ps3_cfg_ppu_null_page());
        else if (!std::strcmp(o->section, "spu") && !std::strcmp(o->key, "dma_repeat_limit"))
            std::printf("%u", ps3_cfg_spu_dma_repeat_limit());
        else if (!std::strcmp(o->section, "spu") && !std::strcmp(o->key, "dma_repeat_stop_secs"))
            std::printf("%u", ps3_cfg_spu_dma_repeat_stop_secs());
        else if (!std::strcmp(o->section, "gcm") && !std::strcmp(o->key, "wait_timeout_ms"))
            std::printf("%u", ps3_cfg_gcm_wait_timeout_ms());
        else if (!std::strcmp(o->section, "gcm") && !std::strcmp(o->key, "park_resync"))
            std::printf("%s", ps3_cfg_gcm_park_resync() ? "true" : "false");
        else if (!std::strcmp(o->section, "gcm") && !std::strcmp(o->key, "park_grace_ms"))
            std::printf("%u", ps3_cfg_gcm_park_grace_ms());
        else if (!std::strcmp(o->section, "trophy") && !std::strcmp(o->key, "register_wait_cap_vblanks"))
            std::printf("%u", ps3_cfg_trophy_register_wait_cap_vblanks());
        else if (!std::strcmp(o->section, "video") && !std::strcmp(o->key, "resolutions"))
            std::printf("\"%s\"", ps3_cfg_video_resolutions());
        else if (!std::strcmp(o->section, "video") && !std::strcmp(o->key, "rsx_capture"))
            std::printf("%s", ps3_cfg_video_rsx_capture() ? "true" : "false");
        else if (!std::strcmp(o->section, "video") && !std::strcmp(o->key, "rsx_capture_dir"))
            std::printf("\"%s\"", ps3_cfg_video_rsx_capture_dir());
        else if (!std::strcmp(o->section, "video") && !std::strcmp(o->key, "rsx_capture_frames"))
            std::printf("%u", ps3_cfg_video_rsx_capture_frames());
        else if (!std::strcmp(o->section, "title") && !std::strcmp(o->key, "elf_sha256"))
            std::printf("\"%s\"", ps3_cfg_title_elf_sha256());
        else if (!std::strcmp(o->section, "diag") && !std::strcmp(o->key, "dump_config"))
            std::printf("%s", ps3_cfg_diag_dump_config() ? "true" : "false");
        else
            std::printf("<unhandled option>");
        std::printf(" %s\n", ps3_config_option_source(i));
    }
    (void)c;
}

int main(int argc, char** argv)
{
    int a = 1;
    if (a + 1 < argc && !std::strcmp(argv[a], "--load")) {
        ps3_config_load(argv[a + 1]);
        a += 2;
    }
    if (a >= argc) return 2;
    const char* cmd = argv[a];

    if (!std::strcmp(cmd, "get")) { print_options(); return 0; }
    if (!std::strcmp(cmd, "dump")) { ps3_config_get(); ps3_config_dump(stdout); return 0; }
    if (!std::strcmp(cmd, "hle") && a + 1 < argc) {
        const ps3_cfg_hle_stub* s = ps3_config_find_hle_stub((uint32_t)std::strtoull(argv[a + 1], nullptr, 0));
        if (!s) { std::puts("none"); return 0; }
        std::printf("nid=0x%08X ret=0x%08X line=%u reason=%s\n", s->nid, s->ret, s->line, s->reason);
        return 0;
    }
    if (!std::strcmp(cmd, "lv2") && a + 1 < argc) {
        const ps3_cfg_lv2_stub* s = ps3_config_find_lv2_stub((uint32_t)std::strtoull(argv[a + 1], nullptr, 0));
        if (!s) { std::puts("none"); return 0; }
        std::printf("num=%u ret=0x%08X line=%u reason=%s\n", s->num, s->ret, s->line, s->reason);
        return 0;
    }
    if (!std::strcmp(cmd, "vscr")) {
        ppu_context init, clear;
        std::memset(&init, 0xFF, sizeof init);    // stale bytes must not leak into a new context
        std::memset(&clear, 0xFF, sizeof clear);
        ppu_context_init(&init);
        ppu_context_clear(&clear);
        std::printf("init=0x%08X clear=0x%08X policy=0x%08X\n", init.vscr, clear.vscr, ps3_ppu_initial_vscr());
        return 0;
    }
    if (!std::strcmp(cmd, "load-again")) {
        ps3_config_get();
        ps3_config_load(nullptr);
        return 3;   // not reached: loading after use is a stop
    }
    if (!std::strcmp(cmd, "race")) {
        std::atomic<int> ready{0};
        std::vector<const ps3_config*> seen(8);
        std::vector<std::thread> ts;
        for (int i = 0; i < 8; ++i)
            ts.emplace_back([i, &ready, &seen] {
                ready.fetch_add(1);
                while (ready.load() < 8) std::this_thread::yield();
                seen[i] = ps3_config_get();
            });
        for (auto& t : ts) t.join();
        std::set<const ps3_config*> distinct(seen.begin(), seen.end());
        std::printf("distinct=%zu\n", distinct.size());
        return 0;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd);
    return 2;
}
