/*
 * ps3recomp - typed, versioned configuration
 *
 * One registry of options (config_options.def), three sources, one precedence:
 *
 *     built-in default  <  config file (TOML)  <  environment
 *
 * and an effective-config dump that says, for every key, what it is and which
 * source set it. Format, keys and examples: docs/CONFIG.md.
 *
 * Any problem with the configuration (unreadable file, wrong schema, unknown
 * key, wrong type, out of range, a PS3RECOMP_* variable that is not an option)
 * is a named stop, kind config-invalid (stop.h). Nothing is ignored, so a typo
 * can never leave an option at a value the operator did not choose.
 *
 * Lifetime and threading
 * ----------------------
 * The configuration is loaded once and is immutable afterwards.
 *
 *   - A title calls ps3_config_load() first thing in main(), before the
 *     executable is loaded and before any thread exists, with the path of its
 *     own config file or NULL. If it does not, the first reader loads it from
 *     the environment alone (PS3RECOMP_CONFIG and PS3RECOMP_*), on whichever
 *     thread that is.
 *   - Loading publishes the finished configuration with a release store; every
 *     reader pairs it with an acquire load. After that nothing writes to it, so
 *     any thread may read through the pointer or the accessors without a lock,
 *     and a pointer from ps3_config_get() stays valid for the life of the
 *     process.
 *   - Calling ps3_config_load() after the configuration is in use (explicitly
 *     loaded, or already read) is a stop, not a reload: a value that changed
 *     under a running guest is exactly the divergence this exists to prevent.
 *     That includes a read from a static initialiser that runs before main().
 */

#ifndef PS3EMU_CONFIG_H
#define PS3EMU_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The `schema` a config file must declare. Bumped when an option is changed or
 * removed, never for an addition. */
#define PS3_CONFIG_SCHEMA_VERSION 1

/* Validators for string options (see config_options.def). */
enum {
    PS3_CFG_V_NONE = 0,
    PS3_CFG_V_SHA256_OR_EMPTY,   /* "" or 64 hex digits; stored lower case */
    PS3_CFG_V_VIDEO_RESOLUTIONS, /* "" or comma-separated cellVideoOut resolution ids; stored sorted, unique */
};

/* ---- typed storage ------------------------------------------------------- */

/* One member per option, named <section>_<key>. Strings are fixed buffers
 * sized by the option's maximum length, so reading one never allocates. */
typedef struct ps3_config {
#define PS3_CFG_BOOL(section, key, def, doc) bool section##_##key;
#define PS3_CFG_U32(section, key, def, min, max, doc) uint32_t section##_##key;
#define PS3_CFG_STR(section, key, def, max_length, validator, doc) \
    char section##_##key[(max_length) + 1];
#include "config_options.def"
#undef PS3_CFG_BOOL
#undef PS3_CFG_U32
#undef PS3_CFG_STR
} ps3_config;

/* The effective configuration. Loads it on first use (see above); never NULL. */
const ps3_config* ps3_config_get(void);

/* Accessors: ps3_cfg_<section>_<key>(). */
#define PS3_CFG_BOOL(section, key, def, doc) \
    static inline bool ps3_cfg_##section##_##key(void) { return ps3_config_get()->section##_##key; }
#define PS3_CFG_U32(section, key, def, min, max, doc) \
    static inline uint32_t ps3_cfg_##section##_##key(void) { return ps3_config_get()->section##_##key; }
#define PS3_CFG_STR(section, key, def, max_length, validator, doc) \
    static inline const char* ps3_cfg_##section##_##key(void) { return ps3_config_get()->section##_##key; }
#include "config_options.def"
#undef PS3_CFG_BOOL
#undef PS3_CFG_U32
#undef PS3_CFG_STR

/* Load the configuration: defaults, then the file, then the environment.
 * `path` is the title's own config file or NULL for none; PS3RECOMP_CONFIG,
 * when set, names the file instead (the environment beats what the title
 * built in). An explicitly named file that cannot be read is a stop. */
void ps3_config_load(const char* path);

/* The file the values came from, or NULL when only defaults and the
 * environment were used. */
const char* ps3_config_file(void);

/* ---- metadata ------------------------------------------------------------ */

typedef enum ps3_config_type {
    PS3_CFG_TYPE_BOOL,
    PS3_CFG_TYPE_STR,
    PS3_CFG_TYPE_U32,               /* unsigned 32-bit integer, range-checked */
} ps3_config_type;

typedef struct ps3_config_option {
    const char*     section;
    const char*     key;
    ps3_config_type type;
    const char*     default_text;   /* "true"/"false" for a bool, decimal for a u32,
                                     * the string itself otherwise */
    const char*     env_name;       /* PS3RECOMP_<SECTION>_<KEY> */
    const char*     doc;
} ps3_config_option;

size_t ps3_config_option_count(void);
const ps3_config_option* ps3_config_option_at(size_t index);   /* NULL past the end */

/* Where option `index` got its value: "default", "file:<path>" or
 * "env:<VARIABLE>". Loads the configuration if needed. */
const char* ps3_config_option_source(size_t index);

/* Write the effective configuration as TOML: `schema`, every option under its
 * section with its source in a trailing comment, then every stub entry. The
 * output is a valid config file that reproduces the same values. */
void ps3_config_dump(FILE* out);

/* ---- structured sections: allow-listed gaps ------------------------------ */
/*
 * [[hle.stub]] nid = 0x..., ret = ..., reason = "..."
 * [[lv2.stub]] num = ..., ret = ..., reason = "..."
 *
 * A gap the runtime would stop on (stop.h: unresolved-nid, unimplemented-syscall)
 * may be allowed to continue, per entry, with an explicit return value and a
 * written reason. There is no blanket form and no environment switch. Each field
 * is required; `reason` must not be blank; a nid or num may appear once. The
 * entries are parsed and validated at load; consuming them is the caller's job.
 *
 * Pointers returned below are valid for the life of the process.
 */

typedef struct ps3_cfg_hle_stub {
    uint32_t    nid;
    uint32_t    ret;      /* value for r3; a negative file value is its two's complement */
    const char* reason;
    uint32_t    line;     /* line of the entry in ps3_config_file() */
} ps3_cfg_hle_stub;

typedef struct ps3_cfg_lv2_stub {
    uint32_t    num;      /* syscall number, 0..1023 */
    uint32_t    ret;
    const char* reason;
    uint32_t    line;
} ps3_cfg_lv2_stub;

/* The entry for `nid` / `num`, or NULL when it is not allow-listed. */
const ps3_cfg_hle_stub* ps3_config_find_hle_stub(uint32_t nid);
const ps3_cfg_lv2_stub* ps3_config_find_lv2_stub(uint32_t num);

/* ---- policy derived from options ----------------------------------------- */

/* The VSCR a new guest PPU context starts with, per ppu.vscr_nj: the one place
 * the option becomes a register value. ppu_context_init() and every runtime
 * site that creates a guest context use it, so no context starts with the
 * policy unapplied. */
uint32_t ps3_ppu_initial_vscr(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_CONFIG_H */
