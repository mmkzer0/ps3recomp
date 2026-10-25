/*
 * T0: the Jq2 ring-entry codec on synthetic bytes only.
 *
 * Pins the bit layout, every command type, field boundaries, the writer-bit
 * model (w1 bit 0 is the queue phase, OR'ed with flags&1 for JOB/COPY; a SYNC's
 * w1 is the phase and never the mask), the rejection of values that do not fit,
 * and decode -> encode identity over a fixed-seed sweep. No guest addresses or
 * trace bytes: the evidence for the layout lives in libs/spurs/jq2/ring.hpp.
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

/* Encode, check the two words, decode back to the same entry and the same raw
 * writer bit. */
void expect_encoded(const Entry& entry, bool phase, std::uint32_t w0, std::uint32_t w1)
{
    const EncodeResult encoded = encode(entry, phase);
    CHECK(encoded.ok());
    if (!encoded.ok()) return;
    CHECK(read_word(encoded.bytes, 0) == w0);
    CHECK(read_word(encoded.bytes, 4) == w1);
    const std::array<std::uint8_t, kEntryBytes> expected = make_bytes(w0, w1);
    CHECK(encoded.bytes == expected);
    const Decoded decoded = decode(encoded.bytes);
    CHECK(decoded.entry == entry);
    CHECK(decoded.writer_bit == ((w1 & 1u) != 0));
}

void test_fixed_layouts()
{
    /* JOB: port | tag << 3 | 0, descriptor | size 4 | writer bit. */
    const JobEntry job{0x0BAD0080u, 0xDEADBEE0u, 0x0Au, SizeCode::bytes256};
    expect_encoded(job, false, 0x0BAD00D0u, 0xDEADBEE4u);
    expect_encoded(job, true, 0x0BAD00D0u, 0xDEADBEE5u);

    /* COPY: type 2 in w0, the pool destination in w1. */
    const CopyEntry copy{0x12345680u, 0xCAFEBAB0u, 0x0Fu, SizeCode::bytes64};
    expect_encoded(copy, false, 0x123456FAu, 0xCAFEBAB0u);
    expect_encoded(copy, true, 0x123456FAu, 0xCAFEBAB1u);

    /* SYNC: (mask << 16) | 7, w1 = phase and nothing else. */
    expect_encoded(SyncEntry{0xBEEFu}, true, 0xBEEF0007u, 0x00000001u);
    expect_encoded(SyncEntry{0xBEEFu}, false, 0xBEEF0007u, 0x00000000u);

    /* Decode split: the typed alternative carries the fields, the writer bit
     * comes back raw. */
    const Decoded decoded = decode(make_bytes(0x0BAD00D0u, 0xDEADBEE5u));
    const auto* typed = std::get_if<JobEntry>(&decoded.entry);
    CHECK(typed != nullptr);
    if (typed != nullptr) {
        CHECK(typed->port_ea == 0x0BAD0080u);
        CHECK(typed->descriptor_ea == 0xDEADBEE0u);
        CHECK(typed->tag == 0x0Au);
        CHECK(typed->size == SizeCode::bytes256);
    }
    CHECK(decoded.writer_bit);
}

/* F1 (corrected by m1r-017): w1 bit 0 is the writer's phase bit and nothing
 * else, for JOB, COPY and SYNC. Identical descriptors differ only by it. */
void test_writer_bit_jobs()
{
    const std::uint32_t port = 0x40000080u;
    const std::uint32_t desc = 0x80000010u;
    for (const SizeCode size : {SizeCode::bytes64, SizeCode::bytes128, SizeCode::bytes256, SizeCode::bytes384,
                                SizeCode::bytes512, SizeCode::bytes640, SizeCode::bytes768, SizeCode::bytes896}) {
        const std::uint32_t size_bits = static_cast<std::uint32_t>(size);
        const JobEntry job{port, desc, 3, size};
        const CopyEntry copy{port, desc, 3, size};
        const std::uint32_t job_w0 = port | (3u << 3);
        const std::uint32_t copy_w0 = job_w0 | 2u;

        expect_encoded(job, false, job_w0, desc | size_bits);
        expect_encoded(job, true, job_w0, desc | size_bits | 1u);
        expect_encoded(copy, false, copy_w0, desc | size_bits);
        expect_encoded(copy, true, copy_w0, desc | size_bits | 1u);

        /* the two encodings differ in exactly bit 0 of w1 */
        const auto p0 = encode(job, false);
        const auto p1 = encode(job, true);
        CHECK(p0.ok() && p1.ok());
        CHECK(read_word(p0.bytes, 0) == read_word(p1.bytes, 0));
        CHECK((read_word(p0.bytes, 4) ^ read_word(p1.bytes, 4)) == 1u);
    }

    /* Decode of the phase-flipped pair: same descriptor, raw writer bit only. */
    const JobEntry job{port, desc, 7, SizeCode::bytes128};
    const std::uint32_t job_w0 = port | (7u << 3);
    const Decoded low = decode(make_bytes(job_w0, desc | 2u));
    const Decoded high = decode(make_bytes(job_w0, desc | 3u));
    CHECK(low.entry == Entry(job));
    CHECK(high.entry == Entry(job));
    CHECK(!low.writer_bit);
    CHECK(high.writer_bit);
}

/* The API flags&1 lives in w0: the port EA is present iff flags&1 (static,
 * m1r-017), so decode recovers it as port_ea != 0. Independent of the phase. */
void test_api_flag_port_presence()
{
    const std::uint32_t desc = 0x80000010u;
    for (const bool phase : {false, true}) {
        const std::uint32_t bit = phase ? 1u : 0u;
        /* flags&1 = 1: port EA present */
        const JobEntry with_flag{0x40000080u, desc, 5, SizeCode::bytes256};
        expect_encoded(with_flag, phase, 0x400000A8u, desc | 4u | bit);
        CHECK(with_flag.api_flag());
        /* flags&1 = 0: no port EA in w0 */
        const JobEntry no_flag{0, desc, 5, SizeCode::bytes256};
        expect_encoded(no_flag, phase, 0x00000028u, desc | 4u | bit);
        CHECK(!no_flag.api_flag());

        const CopyEntry copy_flag{0x40000080u, desc, 5, SizeCode::bytes64};
        const CopyEntry copy_none{0, desc, 5, SizeCode::bytes64};
        expect_encoded(copy_flag, phase, 0x400000AAu, desc | bit);
        expect_encoded(copy_none, phase, 0x0000002Au, desc | bit);
        CHECK(copy_flag.api_flag() && !copy_none.api_flag());

        /* decode recovers the flag from w0 whatever the phase */
        const Decoded d = decode(make_bytes(0x00000028u, desc | 4u | bit));
        const auto* typed = std::get_if<JobEntry>(&d.entry);
        CHECK(typed != nullptr && !typed->api_flag() && d.writer_bit == phase);
    }
}

void test_writer_bit_sync()
{
    /* SYNC masks with bit 0 clear: the mask is never mirrored into w1. */
    for (const std::uint16_t mask : {std::uint16_t(0x2), std::uint16_t(0x8), std::uint16_t(0x8000),
                                     std::uint16_t(0xBEEE), std::uint16_t(0xFFFE)}) {
        const std::uint32_t w0 = (std::uint32_t(mask) << 16) | 7u;
        expect_encoded(SyncEntry{mask}, false, w0, 0u);
        expect_encoded(SyncEntry{mask}, true, w0, 1u);

        /* decode of the phase-flipped pair: same mask, raw writer bit differs */
        const Decoded d0 = decode(make_bytes(w0, 0u));
        const Decoded d1 = decode(make_bytes(w0, 1u));
        CHECK(d0.entry == Entry(SyncEntry{mask}));
        CHECK(d1.entry == Entry(SyncEntry{mask}));
        CHECK(!d0.writer_bit);
        CHECK(d1.writer_bit);
    }

    /* Mask 1 at phase 1 is the traced shape (w1 == 1 is the phase). */
    expect_encoded(SyncEntry{0x0001u}, true, 0x00010007u, 0x00000001u);
    expect_encoded(SyncEntry{0x0001u}, false, 0x00010007u, 0x00000000u);

    /* The old reading, w1 mirroring the mask, is not a SYNC any more. */
    for (const std::uint32_t w1 : {0x00000002u, 0x0000BEEFu, 0xFFFFFFFFu}) {
        const auto bytes = make_bytes(0xBEEF0007u, w1);
        const Decoded d = decode(bytes);
        CHECK(std::holds_alternative<UnknownEntry>(d.entry));
        CHECK(encode(d.entry, d.writer_bit).bytes == bytes);
    }
}

void test_field_boundaries()
{
    const std::uint32_t port = 0x40000080u;
    const std::uint32_t desc = 0x80000010u;

    /* tag 0 and 15 fit; 16 does not. */
    expect_encoded(JobEntry{port, desc, 0, SizeCode::bytes64}, false, port, desc);
    expect_encoded(JobEntry{port, desc, 15, SizeCode::bytes64}, false, port | 0x78u, desc);
    CHECK(encode(JobEntry{port, desc, 16, SizeCode::bytes64}, false).error ==
          EncodeError::tag_out_of_range);

    /* Every size class with the writer bit clear and set, in w1 bits 3..0. */
    const struct {
        SizeCode code;
        std::uint32_t bytes;
    } classes[] = {{SizeCode::bytes64, 64},   {SizeCode::bytes128, 128}, {SizeCode::bytes256, 256},
                   {SizeCode::bytes384, 384}, {SizeCode::bytes512, 512}, {SizeCode::bytes640, 640},
                   {SizeCode::bytes768, 768}, {SizeCode::bytes896, 896}};
    std::uint32_t expected_code = 0;
    for (const auto& cls : classes) {
        CHECK(static_cast<std::uint32_t>(cls.code) == expected_code);
        expect_encoded(JobEntry{port, desc, 1, cls.code}, false, port | 0x08u, desc | expected_code);
        expect_encoded(JobEntry{port, desc, 1, cls.code}, true, port | 0x08u, desc | expected_code | 1u);
        expect_encoded(CopyEntry{port, desc, 1, cls.code}, true, port | 0x0Au, desc | expected_code | 1u);
        CHECK(size_bytes(cls.code) == cls.bytes);
        expected_code += 2;
    }
    CHECK(size_bytes(static_cast<SizeCode>(16)) == 0);

    /* Port: the 128-byte alignment is the field's low edge. */
    expect_encoded(JobEntry{0xFFFFFF80u, desc, 0, SizeCode::bytes256}, false, 0xFFFFFF80u,
                   desc | 4u);
    CHECK(encode(JobEntry{0xFFFFFF81u, desc, 0, SizeCode::bytes256}, false).error ==
          EncodeError::port_ea_misaligned);
    CHECK(encode(JobEntry{0xFFFFFFC0u, desc, 0, SizeCode::bytes256}, true).error ==
          EncodeError::port_ea_misaligned);

    /* Descriptor / pool EA: 16-byte alignment leaves w1 bits 3..0 to the size class and the phase. */
    expect_encoded(JobEntry{port, 0xFFFFFFF0u, 0, SizeCode::bytes64}, false, port,
                   0xFFFFFFF0u);
    CHECK(encode(JobEntry{port, 0xFFFFFFE4u, 0, SizeCode::bytes64}, false).error ==
          EncodeError::descriptor_ea_misaligned);
    CHECK(encode(JobEntry{port, 0xFFFFFFF8u, 0, SizeCode::bytes64}, true).error ==
          EncodeError::descriptor_ea_misaligned);
    CHECK(encode(CopyEntry{port, 0xFFFFFFE8u, 0, SizeCode::bytes64}, false).error ==
          EncodeError::pool_ea_misaligned);

    /* Only the eight classes have a wire form: an odd value or one past 896 does not. */
    for (const unsigned code : {1u, 3u, 5u, 15u, 16u}) {
        CHECK(encode(JobEntry{port, desc, 0, static_cast<SizeCode>(code)}, false).error ==
              EncodeError::invalid_size_code);
        CHECK(encode(CopyEntry{port, desc, 0, static_cast<SizeCode>(code)}, false).error ==
              EncodeError::invalid_size_code);
    }
}

void test_unknown()
{
    const std::uint32_t ports[5] = {0x00000001u, 0x00000003u, 0x00000004u, 0x00000005u,
                                    0x00000006u};
    for (std::uint32_t w0 : ports) {
        const auto bytes = make_bytes(w0, 0x11223344u);
        const Decoded d = decode(bytes);
        CHECK(std::holds_alternative<UnknownEntry>(d.entry));
        const EncodeResult encoded = encode(d.entry, d.writer_bit);
        CHECK(encoded.ok());
        CHECK(encoded.bytes == bytes);
    }

    /* SYNC reserved bits 15..3 must be zero. */
    {
        const auto bytes = make_bytes(0x00001217u, 0x00000001u);
        CHECK(std::holds_alternative<UnknownEntry>(decode(bytes).entry));
    }

    /* Every value of the 3-bit class in w1 bits 3..1 is a JOB/COPY size, bit 3 (the class's high
     * bit) included: 384..896 are no longer unknown entries. */
    {
        const std::uint32_t bytes[8] = {64, 128, 256, 384, 512, 640, 768, 896};
        for (std::uint32_t cls = 0; cls < 8; ++cls) {
            const std::uint32_t w1 = 0x80000010u | (cls << 1);
            const Decoded job = decode(make_bytes(0x00000000u, w1));
            const auto* typed_job = std::get_if<JobEntry>(&job.entry);
            CHECK(typed_job != nullptr);
            if (typed_job != nullptr) {
                CHECK(size_bytes(typed_job->size) == bytes[cls]);
                CHECK(typed_job->descriptor_ea == 0x80000010u);
            }
            const Decoded copy = decode(make_bytes(0x00000002u, w1 | 1u));
            const auto* typed_copy = std::get_if<CopyEntry>(&copy.entry);
            CHECK(typed_copy != nullptr && copy.writer_bit);
            if (typed_copy != nullptr) CHECK(size_bytes(typed_copy->size) == bytes[cls]);
        }
    }

    /* Unknown bytes survive exactly, including a zero type word, whatever
     * phase arguments are passed. */
    const auto bytes = make_bytes(0xDEADBEEFu, 0xCAFEBABEu);
    const Decoded d = decode(bytes);
    CHECK(std::holds_alternative<UnknownEntry>(d.entry));
    CHECK(d.writer_bit == ((0xCAFEBABEu & 1u) != 0));
    CHECK(encode(d.entry, d.writer_bit).bytes == bytes);
    CHECK(encode(d.entry, !d.writer_bit).bytes == bytes);
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

    std::size_t jobs = 0;
    std::size_t copies = 0;
    std::size_t syncs = 0;
    std::size_t unknown = 0;
    constexpr int kIterations = 200000;
    for (int i = 0; i < kIterations; ++i) {
        std::uint32_t w0 = static_cast<std::uint32_t>(next());
        std::uint32_t w1 = static_cast<std::uint32_t>(next());
        /* Random bytes almost never form a SYNC; shape every fourth entry. */
        if (i % 4 == 3) {
            w0 = (w0 & 0xFFFF0000u) | 7u;
            w1 &= 1u;
        }
        const std::array<std::uint8_t, kEntryBytes> bytes = make_bytes(w0, w1);
        const Decoded decoded = decode(bytes);
        if (std::holds_alternative<JobEntry>(decoded.entry)) ++jobs;
        else if (std::holds_alternative<CopyEntry>(decoded.entry)) ++copies;
        else if (std::holds_alternative<SyncEntry>(decoded.entry)) ++syncs;
        else ++unknown;
        const EncodeResult encoded = encode(decoded.entry, decoded.writer_bit);
        if (!encoded.ok() || encoded.bytes != bytes) {
            std::fprintf(stderr, "FAIL identity at iteration %d: ", i);
            dump(bytes);
            std::fprintf(stderr, "\n");
            ++g_failures;
            break;
        }
        if (!(decode(encoded.bytes) == decoded)) {
            std::fprintf(stderr, "FAIL re-decode at iteration %d: ", i);
            dump(bytes);
            std::fprintf(stderr, "\n");
            ++g_failures;
            break;
        }
    }
    CHECK(jobs > 0);
    CHECK(copies > 0);
    CHECK(syncs > 0);
    CHECK(unknown > 0);
}

}  // namespace

int main()
{
    test_fixed_layouts();
    test_writer_bit_jobs();
    test_api_flag_port_presence();
    test_writer_bit_sync();
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
