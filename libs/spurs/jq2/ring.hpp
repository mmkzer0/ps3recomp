/*
 * ps3recomp - SPURS JobQueue2 command-ring entry codec
 *
 * One ring entry is 8 bytes: two big-endian u32 words. The PPU writer stores
 * one entry at ringEA + 8*writeIndex and publishes it after lwsync
 * (jq2fw-028); Create records the ring EA at queue+0x10 and the ring depth at
 * queue+0x14 (jq2fw-018). This module is the entry codec only: it owns no ring
 * indices, queue or port state.
 *
 * Bit layout and evidence:
 *
 *   JOB (command 0) / COPY (command 2), 64-bit BE entry:
 *     w0  31..7  port EA        128-byte aligned. The writer emits
 *                               portEA | (tag << 3) | type; runtime 63/63 JOB
 *                               and 4/4 COPY entries carry the port EA.
 *                               [TRACE-REPORT 4.2; jq2fw-011; jq-ring2]
 *         6..3   tag            0..15. Static: tag at numeric bits 35..38 of
 *                               the 64-bit entry; PM: (w0 >> 3) & 0xF.
 *                               [jq2fw-049; jq2c-022]
 *         2      zero           Port alignment clears bits 6..0, and the tag
 *                               starts at bit 3. [jq2fw-011; jq-ring2]
 *         1..0   command        0 = JOB, 2 = COPY. [jq2c-021; jq-ring2]
 *     w1  31..3  data EA        JOB: the caller's descriptor EA (not copied)
 *                               [jq2fw-023]. COPY: the queue-pool destination
 *                               holding the snapshot [jq2c-020]. Descriptor
 *                               EAs are 16-byte aligned [jq2fw-022], so bit 3
 *                               is zero.
 *         2..1   size code      bits hold 0/1/2, emitted as the firmware's
 *                               0/2/4: 64/128/256 bytes. Static builder
 *                               rlwinm32(size,26,6,30), runtime 128 -> 2 and
 *                               256 -> 4. [JQ-FW-PPU; jq-ring2]
 *         0      flag           API flags bit 0. [TRACE-REPORT 4.2; jq2c-007;
 *                               jq-ring2]
 *
 *   SYNC (w0 low 3 bits == 7):
 *     w0  31..16 mask           16-bit tag mask. [jq2c-021; jq2c-023;
 *                               jq2fw-050; 30/30 jq-ring2 entries]
 *         15..3  zero           Pre-writer value is (mask << 16) | 7.
 *                               [jq2fw-050; jq-ring2]
 *         2..0   7              SYNC discriminator, not the JOBLIST/FLUSH
 *                               forms. [jq2c-021]
 *     w1          word1         Writer mirror. Runtime has word1 == mask, but
 *                               the PM takes the mask from w0 only and the
 *                               pre-writer low word is zero, so the general
 *                               writer source is unresolved [jq2fw-050].
 *                               Preserved verbatim rather than derived.
 *
 * Anything else decodes to UnknownEntry carrying its original bytes. The PM
 * also recognises JOBLIST (command 1) and the FLUSH/marker form (low byte 3)
 * [jq2c-021]; neither is modelled here.
 *
 * decode -> encode is the identity for every 8-byte input. encode() rejects
 * typed values that do not fit a field instead of masking them into shape.
 *
 * Ring geometry: an entry is 8 bytes (jq2fw-018). The jq-ring2 trace dumps 96
 * entries (768 bytes) from the ring pointer; that is the dump window, not the
 * ring capacity. The traced write index reaches 121 without wrapping and
 * queue+0x14 carries the depth (128 on the pool queue, 2048 on the no-pool
 * queue), so kRingDumpEntries is named for what it is. Index arithmetic is the
 * scheduler's concern, not this codec's.
 */

#ifndef PS3RECOMP_SPURS_JQ2_RING_HPP
#define PS3RECOMP_SPURS_JQ2_RING_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace ps3recomp::spurs::jq2 {

/* One command-ring slot, two big-endian u32 words. [jq2fw-018, jq2fw-028] */
inline constexpr std::size_t kEntryBytes = 8;

/* Entries covered by the jq-ring2 ring dump (768 bytes). This is the traced
 * window, not the ring depth; see the geometry note above. */
inline constexpr std::size_t kRingDumpEntries = 96;

/* Command discriminator in w0 bits 2..0. JOB and COPY are distinguished by
 * bit 1; SYNC additionally sets bit 2. [jq2c-021; jq-ring2 runtime entries] */
enum class Command : std::uint8_t {
    job = 0b000,
    copy = 0b010,
    sync = 0b111,
};

/* Descriptor-size code as the firmware emits it into w1 bits 2..0. The values
 * are the documented 0/2/4 codes, not a byte count. [JQ-FW-PPU; jq-ring2] */
enum class SizeCode : std::uint8_t {
    bytes64 = 0,
    bytes128 = 2,
    bytes256 = 4,
};

/* Byte size named by a size code. Returns 0 for a value outside the enum. */
constexpr std::uint32_t size_bytes(SizeCode code) noexcept
{
    switch (code) {
    case SizeCode::bytes64:
        return 64;
    case SizeCode::bytes128:
        return 128;
    case SizeCode::bytes256:
        return 256;
    }
    return 0;
}

/* A JOB command: w1 names the caller's descriptor, which the push path does
 * not copy. [jq2fw-023] */
struct JobEntry {
    std::uint32_t port_ea = 0;
    std::uint32_t descriptor_ea = 0;
    std::uint8_t tag = 0;
    SizeCode size = SizeCode::bytes64;
    bool flag = false;

    friend bool operator==(const JobEntry&, const JobEntry&) = default;
};

/* A COPY command: w1 names the queue-pool slot that holds the snapshot, not
 * the caller's source buffer. [jq2c-020] */
struct CopyEntry {
    std::uint32_t port_ea = 0;
    std::uint32_t pool_ea = 0;
    std::uint8_t tag = 0;
    SizeCode size = SizeCode::bytes64;
    bool flag = false;

    friend bool operator==(const CopyEntry&, const CopyEntry&) = default;
};

/* A SYNC command. The mask is the only interpreted field; word1 is the
 * writer's low word and is preserved because its general source is not
 * established [jq2fw-050]. */
struct SyncEntry {
    std::uint16_t mask = 0;
    std::uint32_t word1 = 0;

    friend bool operator==(const SyncEntry&, const SyncEntry&) = default;
};

/* An entry this codec does not model, with the exact bytes it decoded. */
struct UnknownEntry {
    std::array<std::uint8_t, kEntryBytes> bytes{};

    friend bool operator==(const UnknownEntry&, const UnknownEntry&) = default;
};

using Entry = std::variant<JobEntry, CopyEntry, SyncEntry, UnknownEntry>;

enum class EncodeError : std::uint8_t {
    none = 0,
    tag_out_of_range,          /* tag > 15, but the JOB/COPY field is 4 bits */
    port_ea_misaligned,        /* port EA bits 6..0 set; Port2 requires 128 */
    descriptor_ea_misaligned,  /* JOB descriptor EA bits 3..0 not zero */
    pool_ea_misaligned,        /* COPY pool EA bits 3..0 not zero */
    invalid_size_code,         /* SizeCode outside 64/128/256 */
};

struct EncodeResult {
    /* Valid only when error == EncodeError::none. */
    std::array<std::uint8_t, kEntryBytes> bytes{};
    EncodeError error = EncodeError::none;

    [[nodiscard]] bool ok() const noexcept { return error == EncodeError::none; }
};

/* Decode exactly one 8-byte entry. Never guesses: bytes that are not a JOB,
 * COPY or SYNC in the layouts above come back as UnknownEntry. */
Entry decode(std::span<const std::uint8_t, kEntryBytes> bytes) noexcept;

/* Encode one entry. UnknownEntry is copied verbatim. A typed entry whose
 * fields do not fit is rejected; no field is truncated or folded. */
EncodeResult encode(const Entry& entry) noexcept;

}  // namespace ps3recomp::spurs::jq2

#endif  // PS3RECOMP_SPURS_JQ2_RING_HPP
