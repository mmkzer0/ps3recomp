/*
 * T0: the Jq2 ring-entry codec on synthetic bytes only.
 *
 * Pins the bit layout, every command type, field boundaries, the rejection of
 * values that do not fit, and decode -> encode identity over a fixed-seed
 * sweep. No guest addresses or trace bytes: the evidence for the layout lives
 * in libs/spurs/jq2/ring.hpp.
 */

#include <jq2/ring.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

using namespace ps3recomp::spurs::jq2;

static int g_failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                          \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

std::uint32_t read_word(const std::array<std::uint8_t, kEntryBytes>& bytes,
                        std::size_t offset)
{
    return (std::uint32_t(bytes[offset]) << 24) |
           (std::uint32_t(bytes[offset + 1]) << 16) |
           (std::uint32_t(bytes[offset + 2]) << 8) |
           std::uint32_t(bytes[offset + 3]);
}

std::array<std::uint8_t, kEntryBytes> make_bytes(std::uint32_t w0, std::uint32_t w1)
{
    std::array<std::uint8_t, kEntryBytes> bytes{};
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[i] = static_cast<std::uint8_t>(w0 >> (24 - 8 * i));
        bytes[4 + i] = static_cast<std::uint8_t>(w1 >> (24 - 8 * i));
    }
    return bytes;
}

void dump(const std::array<std::uint8_t, kEntryBytes>& bytes)
{
    for (std::uint8_t b : bytes) std::fprintf(stderr, "%02x", b);
}

/* Encode, check the two words, decode back to the same variant value. */
void expect_encoded(const Entry& entry, std::uint32_t w0, std::uint32_t w1)
{
    const EncodeResult encoded = encode(entry);
    CHECK(encoded.ok());
    if (!encoded.ok()) return;
    CHECK(read_word(encoded.bytes, 0) == w0);
    CHECK(read_word(encoded.bytes, 4) == w1);
    const std::array<std::uint8_t, kEntryBytes> expected = make_bytes(w0, w1);
    CHECK(encoded.bytes == expected);
    CHECK(decode(encoded.bytes) == entry);
}

void test_fixed_layouts()
{
    /* JOB: port | tag << 3 | 0, descriptor | size 4 | flag 1. */
    const JobEntry job{0x0BAD0080u, 0xDEADBEE0u, 0x0Au, SizeCode::bytes256, true};
    expect_encoded(job, 0x0BAD00D0u, 0xDEADBEE5u);

    /* COPY: type 2 in w0, the pool destination in w1. */
    const CopyEntry copy{0x12345680u, 0xCAFEBAB0u, 0x0Fu, SizeCode::bytes64, false};
    expect_encoded(copy, 0x123456FAu, 0xCAFEBAB0u);

    /* SYNC: (mask << 16) | 7, writer word preserved. */
    const SyncEntry sync{0xBEEFu, 0xBEEFu};
    expect_encoded(sync, 0xBEEF0007u, 0x0000BEEFu);

    /* The writer's low word is not derived from the mask. */
    const SyncEntry odd_word{0x0000u, 0xFFFFFFFFu};
    expect_encoded(odd_word, 0x00000007u, 0xFFFFFFFFu);

    /* Decode split: the typed alternative carries the fields. */
    const Entry decoded_entry = decode(make_bytes(0x0BAD00D0u, 0xDEADBEE5u));
    const auto decoded = std::get_if<JobEntry>(&decoded_entry);
    CHECK(decoded != nullptr);
    if (decoded != nullptr) {
        CHECK(decoded->port_ea == 0x0BAD0080u);
        CHECK(decoded->descriptor_ea == 0xDEADBEE0u);
        CHECK(decoded->tag == 0x0Au);
        CHECK(decoded->size == SizeCode::bytes256);
        CHECK(decoded->flag);
    }
}

void test_field_boundaries()
{
    const std::uint32_t port = 0x40000080u;
    const std::uint32_t desc = 0x80000010u;

    /* tag 0 and 15 fit; 16 does not. */
    expect_encoded(JobEntry{port, desc, 0, SizeCode::bytes64, false}, port, desc);
    expect_encoded(JobEntry{port, desc, 15, SizeCode::bytes64, false}, port | 0x78u, desc);
    CHECK(encode(JobEntry{port, desc, 16, SizeCode::bytes64, false}).error ==
          EncodeError::tag_out_of_range);

    /* Every size code and both flag values, in w1 bits 2..0. */
    expect_encoded(JobEntry{port, desc, 1, SizeCode::bytes64, false}, port | 0x08u, desc);
    expect_encoded(JobEntry{port, desc, 1, SizeCode::bytes64, true}, port | 0x08u, desc | 1u);
    expect_encoded(JobEntry{port, desc, 1, SizeCode::bytes128, false}, port | 0x08u, desc | 2u);
    expect_encoded(JobEntry{port, desc, 1, SizeCode::bytes128, true}, port | 0x08u, desc | 3u);
    expect_encoded(JobEntry{port, desc, 1, SizeCode::bytes256, false}, port | 0x08u, desc | 4u);
    expect_encoded(JobEntry{port, desc, 1, SizeCode::bytes256, true}, port | 0x08u, desc | 5u);
    CHECK(size_bytes(SizeCode::bytes64) == 64);
    CHECK(size_bytes(SizeCode::bytes128) == 128);
    CHECK(size_bytes(SizeCode::bytes256) == 256);

    /* Port: the 128-byte alignment is the field's low edge. */
    expect_encoded(JobEntry{0xFFFFFF80u, desc, 0, SizeCode::bytes256, false}, 0xFFFFFF80u,
                   desc | 4u);
    CHECK(encode(JobEntry{0xFFFFFF81u, desc, 0, SizeCode::bytes256, false}).error ==
          EncodeError::port_ea_misaligned);
    CHECK(encode(JobEntry{0xFFFFFFC0u, desc, 0, SizeCode::bytes256, false}).error ==
          EncodeError::port_ea_misaligned);

    /* Descriptor / pool EA: 16-byte alignment clears w1 bit 3. */
    expect_encoded(JobEntry{port, 0xFFFFFFF0u, 0, SizeCode::bytes64, false}, port, 0xFFFFFFF0u);
    CHECK(encode(JobEntry{port, 0xFFFFFFE4u, 0, SizeCode::bytes64, false}).error ==
          EncodeError::descriptor_ea_misaligned);
    CHECK(encode(JobEntry{port, 0xFFFFFFF8u, 0, SizeCode::bytes64, false}).error ==
          EncodeError::descriptor_ea_misaligned);
    CHECK(encode(CopyEntry{port, 0xFFFFFFE8u, 0, SizeCode::bytes64, false}).error ==
          EncodeError::pool_ea_misaligned);

    /* A size code outside the three the firmware emits has no wire form. */
    CHECK(encode(JobEntry{port, desc, 0, static_cast<SizeCode>(6), false}).error ==
          EncodeError::invalid_size_code);
}

void test_unknown()
{
    const std::uint32_t ports[5] = {0x00000001u, 0x00000003u, 0x00000004u, 0x00000005u,
                                    0x00000006u};
    for (std::uint32_t w0 : ports) {
        const auto bytes = make_bytes(w0, 0x11223344u);
        const Entry entry = decode(bytes);
        CHECK(std::holds_alternative<UnknownEntry>(entry));
        const EncodeResult encoded = encode(entry);
        CHECK(encoded.ok());
        CHECK(encoded.bytes == bytes);
    }

    /* SYNC reserved bits 15..3 must be zero. */
    {
        const auto bytes = make_bytes(0x00001217u, 0x00000001u);
        CHECK(std::holds_alternative<UnknownEntry>(decode(bytes)));
    }

    /* JOB/COPY with a size-code field value of 3, or with descriptor bit 3. */
    {
        const auto bytes = make_bytes(0x00000000u, 0x00000006u);
        const Entry entry = decode(bytes);
        CHECK(std::holds_alternative<UnknownEntry>(entry));
        CHECK(encode(entry).bytes == bytes);
    }
    {
        const auto bytes = make_bytes(0x00000000u, 0x00000008u);
        CHECK(std::holds_alternative<UnknownEntry>(decode(bytes)));
    }

    /* Unknown bytes survive exactly, including a zero type word. */
    const auto bytes = make_bytes(0xDEADBEEFu, 0xCAFEBABEu);
    CHECK(std::holds_alternative<UnknownEntry>(decode(bytes)));
    CHECK(encode(decode(bytes)).bytes == bytes);
}

void test_decode_encode_identity()
{
    /* splitmix64: fixed seed, no library PRNG differences between hosts. */
    std::uint64_t state = 0x5A17C0DEC0FFEE01ull;
    auto next = [&state]() {
        std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };

    std::size_t typed = 0;
    std::size_t unknown = 0;
    constexpr int kIterations = 200000;
    for (int i = 0; i < kIterations; ++i) {
        std::array<std::uint8_t, kEntryBytes> bytes = make_bytes(
            static_cast<std::uint32_t>(next()), static_cast<std::uint32_t>(next()));
        const Entry entry = decode(bytes);
        if (std::holds_alternative<UnknownEntry>(entry)) {
            ++unknown;
        } else {
            ++typed;
        }
        const EncodeResult encoded = encode(entry);
        if (!encoded.ok() || encoded.bytes != bytes) {
            std::fprintf(stderr, "FAIL identity at iteration %d: ", i);
            dump(bytes);
            std::fprintf(stderr, "\n");
            ++g_failures;
            break;
        }
        if (!(decode(encoded.bytes) == entry)) {
            std::fprintf(stderr, "FAIL re-decode at iteration %d: ", i);
            dump(bytes);
            std::fprintf(stderr, "\n");
            ++g_failures;
            break;
        }
    }
    CHECK(typed > 0);
    CHECK(unknown > 0);
}

}  // namespace

int main()
{
    test_fixed_layouts();
    test_field_boundaries();
    test_unknown();
    test_decode_encode_identity();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("jq2 ring codec: all checks passed\n");
    return 0;
}
