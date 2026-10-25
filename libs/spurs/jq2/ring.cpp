/*
 * ps3recomp - SPURS JobQueue2 command-ring entry codec
 *
 * Layout evidence lives in ring.hpp. This file is the byte-exact half: all
 * field extraction and insertion is explicit, so decode -> encode is the
 * identity and encode rejects an out-of-field value rather than narrowing it.
 */

#include "jq2/ring.hpp"

#include <algorithm>
#include <type_traits>

namespace ps3recomp::spurs::jq2 {
namespace {

constexpr std::uint32_t kCommandMask = 0x7u;
constexpr std::uint32_t kTagMask = 0xFu;
constexpr std::uint32_t kTagShift = 3;
constexpr std::uint32_t kSizeCodeMask = 0xEu;
constexpr std::uint32_t kWriterBitMask = 0x1u;
constexpr std::uint32_t kSyncMarker = 0x7u;
constexpr std::uint32_t kSyncMaskShift = 16;
constexpr std::uint32_t kSyncReservedMask = 0x0000FFF8u;
/* SYNC w1 is the phase bit and nothing else. */
constexpr std::uint32_t kSyncW1ReservedMask = ~kWriterBitMask;

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
    return (static_cast<std::uint32_t>(code) & ~kSizeCodeMask) == 0;
}

/* Fields shared by JOB and COPY. The only difference is what w1 names. */
struct JobFields {
    std::uint32_t port_ea;
    std::uint32_t data_ea;
    std::uint8_t tag;
    SizeCode size;
    bool writer_bit;
};

JobFields decode_job_fields(std::uint32_t w0, std::uint32_t w1) noexcept
{
    /* w1 bits 3..1 are the 3-bit size class and bit 0 the phase; the data EA is
     * 16-byte aligned, so bits 3..0 never belong to it. Every class value is a
     * size the firmware emits (64 and 128..896), so none is refused here. */
    JobFields fields{};
    fields.port_ea = w0 & ~std::uint32_t(0x7Fu);
    fields.data_ea = w1 & ~std::uint32_t(0xFu);
    fields.tag = static_cast<std::uint8_t>((w0 >> kTagShift) & kTagMask);
    fields.size = static_cast<SizeCode>(w1 & kSizeCodeMask);
    fields.writer_bit = (w1 & kWriterBitMask) != 0;
    return fields;
}

template <class E>
Decoded decode_job_entry(std::uint32_t w0, std::uint32_t w1) noexcept
{
    const JobFields fields = decode_job_fields(w0, w1);
    return Decoded{E{fields.port_ea, fields.data_ea, fields.tag, fields.size}, fields.writer_bit};
}

EncodeResult encode_job_like(std::uint32_t command, std::uint32_t port_ea,
                             std::uint32_t data_ea, std::uint8_t tag, SizeCode size,
                             bool writer_bit, EncodeError alignment_error) noexcept
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
    const std::uint32_t w1 = data_ea | std::uint32_t(size) | (writer_bit ? 1u : 0u);
    write_be32(result.bytes.data(), w0);
    write_be32(result.bytes.data() + 4, w1);
    return result;
}

}  // namespace

Decoded decode(std::span<const std::uint8_t, kEntryBytes> bytes) noexcept
{
    const std::uint32_t w0 = read_be32(bytes.data());
    const std::uint32_t w1 = read_be32(bytes.data() + 4);
    const bool raw_writer_bit = (w1 & kWriterBitMask) != 0;

    switch (w0 & kCommandMask) {
    case static_cast<std::uint32_t>(Command::job):
        return decode_job_entry<JobEntry>(w0, w1);
    case static_cast<std::uint32_t>(Command::copy):
        return decode_job_entry<CopyEntry>(w0, w1);
    case static_cast<std::uint32_t>(Command::sync):
        /* SYNC is (mask << 16) | 7 with w1 = phase; bits 15..3 and w1 31..1
         * are written zero. */
        if ((w0 & kSyncReservedMask) == 0 && (w1 & kSyncW1ReservedMask) == 0)
            return Decoded{SyncEntry{static_cast<std::uint16_t>(w0 >> kSyncMaskShift)},
                           raw_writer_bit};
        break;
    default:
        break;
    }
    return Decoded{unknown_from(bytes), raw_writer_bit};
}

EncodeResult encode(const Entry& entry, bool writer_phase) noexcept
{
    return std::visit(
        [writer_phase](const auto& value) -> EncodeResult {
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
                write_be32(result.bytes.data() + 4, writer_phase ? 1u : 0u);
                return result;
            } else if constexpr (std::is_same_v<T, JobEntry>) {
                return encode_job_like(
                    static_cast<std::uint32_t>(Command::job), value.port_ea,
                    value.descriptor_ea, value.tag, value.size, writer_phase,
                    EncodeError::descriptor_ea_misaligned);
            } else {
                static_assert(std::is_same_v<T, CopyEntry>);
                return encode_job_like(
                    static_cast<std::uint32_t>(Command::copy), value.port_ea,
                    value.pool_ea, value.tag, value.size, writer_phase,
                    EncodeError::pool_ea_misaligned);
            }
        },
        entry);
}

}  // namespace ps3recomp::spurs::jq2
