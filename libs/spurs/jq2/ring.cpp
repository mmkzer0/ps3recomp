/*
 * ps3recomp - SPURS JobQueue2 command-ring entry codec
 *
 * Layout evidence lives in ring.hpp. This file is the byte-exact half: all
 * field extraction and insertion is explicit, so decode -> encode is the
 * identity and encode rejects an out-of-field value rather than narrowing it.
 */

#include "jq2/ring.hpp"

#include <algorithm>
#include <optional>
#include <type_traits>

namespace ps3recomp::spurs::jq2 {
namespace {

constexpr std::uint32_t kCommandMask = 0x7u;
constexpr std::uint32_t kTagMask = 0xFu;
constexpr std::uint32_t kTagShift = 3;
constexpr std::uint32_t kSizeCodeMask = 0x6u;
constexpr std::uint32_t kFlagMask = 0x1u;
constexpr std::uint32_t kSyncMarker = 0x7u;
constexpr std::uint32_t kSyncMaskShift = 16;
constexpr std::uint32_t kSyncReservedMask = 0x0000FFF8u;

std::uint32_t read_be32(const std::uint8_t* p) noexcept
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

void write_be32(std::uint8_t* p, std::uint32_t value) noexcept
{
    p[0] = static_cast<std::uint8_t>(value >> 24);
    p[1] = static_cast<std::uint8_t>(value >> 16);
    p[2] = static_cast<std::uint8_t>(value >> 8);
    p[3] = static_cast<std::uint8_t>(value);
}

UnknownEntry unknown_from(std::span<const std::uint8_t, kEntryBytes> bytes)
{
    UnknownEntry entry;
    std::copy(bytes.begin(), bytes.end(), entry.bytes.begin());
    return entry;
}

bool valid_size_code(SizeCode code) noexcept
{
    return code == SizeCode::bytes64 || code == SizeCode::bytes128 ||
           code == SizeCode::bytes256;
}

/* Fields shared by JOB and COPY. The only difference is what w1 names. */
struct JobFields {
    std::uint32_t port_ea;
    std::uint32_t data_ea;
    std::uint8_t tag;
    SizeCode size;
    bool flag;
};

std::optional<JobFields> decode_job_fields(std::uint32_t w0, std::uint32_t w1) noexcept
{
    /* w1 bits 2..1 carry 0/1/2. Value 3 is not a size the firmware emits. */
    const std::uint32_t size_bits = (w1 & kSizeCodeMask) >> 1;
    if (size_bits > 2) return std::nullopt;

    /* w1 bit 3 is descriptor EA bit 3, which 16-byte alignment forces to 0.
     * An entry that sets it is not a JOB/COPY this codec can represent. */
    if ((w1 & 0x8u) != 0) return std::nullopt;

    JobFields fields{};
    fields.port_ea = w0 & ~std::uint32_t(0x7Fu);
    fields.data_ea = w1 & ~std::uint32_t(0x7u);
    fields.tag = static_cast<std::uint8_t>((w0 >> kTagShift) & kTagMask);
    fields.size = static_cast<SizeCode>(size_bits << 1);
    fields.flag = (w1 & kFlagMask) != 0;
    return fields;
}

EncodeResult encode_job_like(std::uint32_t command, std::uint32_t port_ea,
                             std::uint32_t data_ea, std::uint8_t tag, SizeCode size,
                             bool flag, EncodeError alignment_error) noexcept
{
    EncodeResult result;
    if (tag > kTagMask) {
        result.error = EncodeError::tag_out_of_range;
        return result;
    }
    if ((port_ea & 0x7Fu) != 0) {
        result.error = EncodeError::port_ea_misaligned;
        return result;
    }
    if ((data_ea & 0xFu) != 0) {
        result.error = alignment_error;
        return result;
    }
    if (!valid_size_code(size)) {
        result.error = EncodeError::invalid_size_code;
        return result;
    }

    const std::uint32_t w0 =
        port_ea | (std::uint32_t(tag) << kTagShift) | command;
    const std::uint32_t w1 = data_ea | std::uint32_t(size) | (flag ? 1u : 0u);
    write_be32(result.bytes.data(), w0);
    write_be32(result.bytes.data() + 4, w1);
    return result;
}

}  // namespace

Entry decode(std::span<const std::uint8_t, kEntryBytes> bytes) noexcept
{
    const std::uint32_t w0 = read_be32(bytes.data());
    const std::uint32_t w1 = read_be32(bytes.data() + 4);

    switch (w0 & kCommandMask) {
    case static_cast<std::uint32_t>(Command::job):
        if (const auto fields = decode_job_fields(w0, w1))
            return JobEntry{fields->port_ea, fields->data_ea, fields->tag,
                            fields->size, fields->flag};
        break;
    case static_cast<std::uint32_t>(Command::copy):
        if (const auto fields = decode_job_fields(w0, w1))
            return CopyEntry{fields->port_ea, fields->data_ea, fields->tag,
                             fields->size, fields->flag};
        break;
    case static_cast<std::uint32_t>(Command::sync):
        /* SYNC is (mask << 16) | 7; bits 15..3 are written zero. */
        if ((w0 & kSyncReservedMask) == 0)
            return SyncEntry{static_cast<std::uint16_t>(w0 >> kSyncMaskShift), w1};
        break;
    default:
        break;
    }
    return unknown_from(bytes);
}

EncodeResult encode(const Entry& entry) noexcept
{
    return std::visit(
        [](const auto& value) -> EncodeResult {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, UnknownEntry>) {
                EncodeResult result;
                result.bytes = value.bytes;
                return result;
            } else if constexpr (std::is_same_v<T, SyncEntry>) {
                EncodeResult result;
                const std::uint32_t w0 =
                    (std::uint32_t(value.mask) << kSyncMaskShift) | kSyncMarker;
                write_be32(result.bytes.data(), w0);
                write_be32(result.bytes.data() + 4, value.word1);
                return result;
            } else if constexpr (std::is_same_v<T, JobEntry>) {
                return encode_job_like(
                    static_cast<std::uint32_t>(Command::job), value.port_ea,
                    value.descriptor_ea, value.tag, value.size, value.flag,
                    EncodeError::descriptor_ea_misaligned);
            } else {
                static_assert(std::is_same_v<T, CopyEntry>);
                return encode_job_like(
                    static_cast<std::uint32_t>(Command::copy), value.port_ea,
                    value.pool_ea, value.tag, value.size, value.flag,
                    EncodeError::pool_ea_misaligned);
            }
        },
        entry);
}

}  // namespace ps3recomp::spurs::jq2
