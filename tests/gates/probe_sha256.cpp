/* Prints the SHA-256 of an input described on the command line; test_sha256.py
 * compares it with the NIST vectors and with hashlib.
 *
 *   probe_sha256 str <text>
 *   probe_sha256 repeat <byte> <count>      byte repeated count times
 *   probe_sha256 pattern <n> [chunk]        byte i = (i * 31 + 7) & 255, fed in
 *                                           `chunk`-sized updates (default: one)
 */

#include "ps3emu/sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    char hex[PS3_SHA256_HEX_SIZE];
    if (argc == 3 && !std::strcmp(argv[1], "str")) {
        ps3_sha256_hex(argv[2], std::strlen(argv[2]), hex);
    } else if (argc == 4 && !std::strcmp(argv[1], "repeat")) {
        std::vector<unsigned char> data(std::strtoull(argv[3], nullptr, 10),
                                        (unsigned char)std::strtoul(argv[2], nullptr, 0));
        ps3_sha256_hex(data.data(), data.size(), hex);
    } else if ((argc == 3 || argc == 4) && !std::strcmp(argv[1], "pattern")) {
        std::vector<unsigned char> data(std::strtoull(argv[2], nullptr, 10));
        for (size_t i = 0; i < data.size(); ++i) data[i] = (unsigned char)(i * 31 + 7);
        size_t chunk = argc == 4 ? std::strtoull(argv[3], nullptr, 10) : data.size();
        if (!chunk) chunk = 1;
        ps3_sha256_ctx ctx;
        uint8_t digest[PS3_SHA256_DIGEST_SIZE];
        ps3_sha256_init(&ctx);
        for (size_t off = 0; off < data.size(); off += chunk)
            ps3_sha256_update(&ctx, data.data() + off, off + chunk > data.size() ? data.size() - off : chunk);
        ps3_sha256_final(&ctx, digest);
        for (int i = 0; i < PS3_SHA256_DIGEST_SIZE; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    } else {
        return 2;
    }
    std::puts(hex);
    return 0;
}
