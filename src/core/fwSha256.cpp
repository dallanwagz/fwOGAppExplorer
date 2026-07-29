#include "core/fwSha256.h"

extern "C" {
#include "sha256.h"
}

namespace fwog {

std::string sha256Hex(std::span<const uint8_t> data)
{
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data.data(), data.size());

    uint8_t digest[32];
    sha256_final(&ctx, digest);

    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (uint8_t b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

} // namespace fwog
