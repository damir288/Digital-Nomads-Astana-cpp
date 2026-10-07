#include "station/sha1.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace station {

namespace {
inline std::uint32_t rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
}  // namespace

std::string sha1_hex(const std::string& data) {
    std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
    std::vector<unsigned char> msg(data.begin(), data.end());
    std::uint64_t bit_len = static_cast<std::uint64_t>(msg.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<unsigned char>(bit_len >> (i * 8)));

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(msg[chunk + 4 * i]) << 24) | (std::uint32_t(msg[chunk + 4 * i + 1]) << 16) |
                   (std::uint32_t(msg[chunk + 4 * i + 2]) << 8) | std::uint32_t(msg[chunk + 4 * i + 3]);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    char out[41];
    std::snprintf(out, sizeof out, "%08x%08x%08x%08x%08x", h0, h1, h2, h3, h4);
    return out;
}

}  // namespace station
