/* ps3recomp - executable identity gate (contract: include/ps3emu/identity.h) */

#include "ps3emu/identity.h"

#include "ps3emu/config.h"
#include "ps3emu/sha256.h"
#include "ps3emu/stop.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool is_sha256_hex(const char* s)
{
    if (!s || std::strlen(s) != 64) return false;
    for (const char* p = s; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
            return false;
    return true;
}

bool equal_hex(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'F') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

/* "env:PS3RECOMP_TITLE_ELF_SHA256" / "file:<path>" / "default" for the pin. */
std::string pin_source()
{
    for (size_t i = 0; i < ps3_config_option_count(); ++i) {
        const ps3_config_option* o = ps3_config_option_at(i);
        if (!std::strcmp(o->section, "title") && !std::strcmp(o->key, "elf_sha256"))
            return ps3_config_option_source(i);
    }
    return "unknown";
}

} // namespace

extern "C" void ps3_identity_check_image(const char* path, const void* data, size_t len,
                                         const char* lifted_sha256)
{
    // Read the pin first: an invalid configuration is reported as such, whatever
    // the executable turns out to be.
    const char* pin = ps3_cfg_title_elf_sha256();

    char actual[PS3_SHA256_HEX_SIZE];
    ps3_sha256_hex(data, len, actual);

    char size[24];
    std::snprintf(size, sizeof size, "%zu", len);
    const char* shown_path = path ? path : "";

    if (!is_sha256_hex(lifted_sha256)) {
        ps3_stop_field f[] = {
            {"path", shown_path}, {"size", size}, {"actual", actual},
            {"expected", lifted_sha256 ? lifted_sha256 : "(none)"}, {"expected_from", "lift"},
        };
        ps3_stop(PS3_STOP_IDENTITY_MISMATCH,
                 "this runtime's lifted output carries no valid image hash, so it cannot vouch for any executable",
                 "Re-lift with tools/ppu_lifter.py, which records the hash of the file it lifts.",
                 f, 5, nullptr);
    }
    if (!equal_hex(lifted_sha256, actual)) {
        ps3_stop_field f[] = {
            {"path", shown_path}, {"size", size}, {"expected", lifted_sha256}, {"actual", actual},
            {"expected_from", "lift"},
        };
        ps3_stop(PS3_STOP_IDENTITY_MISMATCH,
                 "the executable is not the one this runtime was lifted from",
                 "Load the executable that was lifted, or re-lift from this one (tools/ppu_lifter.py "
                 "records the hash). Do not edit the hash in the generated source.",
                 f, 5, nullptr);
    }
    if (*pin && !equal_hex(pin, actual)) {
        const std::string source = pin_source();
        ps3_stop_field f[] = {
            {"path", shown_path}, {"size", size}, {"expected", pin}, {"actual", actual},
            {"expected_from", "config"}, {"pin_source", source.c_str()},
        };
        ps3_stop(PS3_STOP_IDENTITY_MISMATCH,
                 "the executable does not match the title.elf_sha256 pin",
                 "Load the executable the pin names, or correct title.elf_sha256 (docs/CONFIG.md).",
                 f, 6, nullptr);
    }
}
