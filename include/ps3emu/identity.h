/*
 * ps3recomp - executable identity gate
 *
 * A lift is valid for exactly one executable: its function table is addressed
 * by that image's layout, and its recompiled code stands in for that image's
 * instructions. Loading any other file under it would run guest data as the
 * wrong code or the wrong data, with nothing to say so. The lifter therefore
 * records the SHA-256 of the whole file it lifted (ppu_lift_image_sha256(), in
 * the generated output) and the loader checks the file it is asked to load
 * against it before reading a byte of it as an ELF.
 *
 * title.elf_sha256 (docs/CONFIG.md) adds an optional second check against a
 * hash the operator pins; it can only narrow what is accepted, never widen it.
 *
 * Out of scope here: fingerprints of hook sites. No hooks exist yet; they will
 * need their own check at the point a hook binds to code.
 */

#ifndef PS3EMU_IDENTITY_H
#define PS3EMU_IDENTITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Verify that data[0..len), the contents of `path`, is the image this runtime
 * was lifted from (`lifted_sha256`: 64 hex digits, as ppu_lift_image_sha256()
 * returns) and satisfies the title.elf_sha256 pin if one is set. Returns when it
 * does; otherwise stops (identity-mismatch) with the path and both hashes.
 * A NULL, empty or malformed `lifted_sha256` is a mismatch too: a lift that
 * recorded no identity cannot vouch for any file. */
void ps3_identity_check_image(const char* path, const void* data, size_t len,
                              const char* lifted_sha256);

#ifdef __cplusplus
}
#endif

#endif /* PS3EMU_IDENTITY_H */
