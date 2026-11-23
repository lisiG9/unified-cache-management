/*
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "key_encoder.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace UC::Nds {
namespace {

class Sha256 {
    uint32_t state_[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buffer_[64]{};
    size_t buffered_{0};
    uint64_t totalBytes_{0};

    static uint32_t Rotate(uint32_t value, uint32_t bits)
    {
        return (value >> bits) | (value << (32 - bits));
    }
    void Transform(const uint8_t* block)
    {
        static constexpr uint32_t constants[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
            0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t words[64];
        for (size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                       (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const auto s0 = Rotate(words[i - 15], 7) ^ Rotate(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
            const auto s1 = Rotate(words[i - 2], 17) ^ Rotate(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        uint32_t work[8];
        std::memcpy(work, state_, sizeof(work));
        for (size_t i = 0; i < 64; ++i) {
            const auto s1 = Rotate(work[4], 6) ^ Rotate(work[4], 11) ^ Rotate(work[4], 25);
            const auto choose = (work[4] & work[5]) ^ (~work[4] & work[6]);
            const auto temp1 = work[7] + s1 + choose + constants[i] + words[i];
            const auto s0 = Rotate(work[0], 2) ^ Rotate(work[0], 13) ^ Rotate(work[0], 22);
            const auto majority = (work[0] & work[1]) ^ (work[0] & work[2]) ^ (work[1] & work[2]);
            const auto temp2 = s0 + majority;
            work[7] = work[6];
            work[6] = work[5];
            work[5] = work[4];
            work[4] = work[3] + temp1;
            work[3] = work[2];
            work[2] = work[1];
            work[1] = work[0];
            work[0] = temp1 + temp2;
        }
        for (size_t i = 0; i < 8; ++i) { state_[i] += work[i]; }
    }

public:
    void Update(const void* data, size_t bytes)
    {
        const auto* input = static_cast<const uint8_t*>(data);
        totalBytes_ += bytes;
        while (bytes != 0) {
            const size_t count = std::min(bytes, sizeof(buffer_) - buffered_);
            std::memcpy(buffer_ + buffered_, input, count);
            buffered_ += count;
            input += count;
            bytes -= count;
            if (buffered_ == sizeof(buffer_)) {
                Transform(buffer_);
                buffered_ = 0;
            }
        }
    }
    std::array<uint8_t, 32> Final()
    {
        const uint64_t totalBits = totalBytes_ * 8;
        const uint8_t one = 0x80;
        Update(&one, 1);
        const uint8_t zero = 0;
        while (buffered_ != 56) { Update(&zero, 1); }
        uint8_t length[8];
        for (size_t i = 0; i < 8; ++i) { length[i] = static_cast<uint8_t>(totalBits >> (56 - i * 8)); }
        Update(length, sizeof(length));
        std::array<uint8_t, 32> digest{};
        for (size_t i = 0; i < 8; ++i) {
            digest[i * 4] = static_cast<uint8_t>(state_[i] >> 24);
            digest[i * 4 + 1] = static_cast<uint8_t>(state_[i] >> 16);
            digest[i * 4 + 2] = static_cast<uint8_t>(state_[i] >> 8);
            digest[i * 4 + 3] = static_cast<uint8_t>(state_[i]);
        }
        return digest;
    }
};

}  // namespace

NdsKey KeyEncoder::Encode(const Detail::BlockId& block, size_t shardIndex)
{
    static constexpr char domain[] = "UCM_NDS_KEY_V1";
    Sha256 hash;
    hash.Update(domain, sizeof(domain) - 1);
    hash.Update(block.data(), block.size());
    uint8_t shard[8];
    const uint64_t index = static_cast<uint64_t>(shardIndex);
    for (size_t i = 0; i < sizeof(shard); ++i) {
        shard[i] = static_cast<uint8_t>(index >> (i * 8));
    }
    hash.Update(shard, sizeof(shard));
    const auto digest = hash.Final();
    NdsKey key;
    for (size_t i = 0; i < 8; ++i) { key.low |= static_cast<uint64_t>(digest[i]) << (i * 8); }
    key.high = static_cast<uint16_t>(digest[8]) | (static_cast<uint16_t>(digest[9]) << 8);
    return key;
}

}  // namespace UC::Nds
