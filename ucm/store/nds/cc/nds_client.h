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
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace UC::Nds {

class INdsClient {
public:
    virtual ~INdsClient() = default;
    virtual int32_t InitMulti(const std::vector<void*>& addrs, const std::vector<uint64_t>& lengths,
                              const std::string& configPath) = 0;
    virtual int32_t IsExists(const std::vector<uint64_t>& lows,
                             const std::vector<uint16_t>& highs, uint32_t nsid) = 0;
    virtual int32_t BatchGet(const std::vector<uint64_t>& lows,
                             const std::vector<uint16_t>& highs,
                             const std::vector<uint8_t*>& addrs,
                             const std::vector<size_t>& offsets,
                             const std::vector<size_t>& lengths,
                             const std::vector<uint32_t>& nsids) = 0;
    virtual int32_t BatchPut(const std::vector<uint64_t>& lows,
                             const std::vector<uint16_t>& highs,
                             const std::vector<uint8_t*>& addrs,
                             const std::vector<size_t>& offsets,
                             const std::vector<size_t>& lengths,
                             const std::vector<uint32_t>& nsids) = 0;
};

class RealNdsClient : public INdsClient {
public:
    RealNdsClient();
    int32_t InitMulti(const std::vector<void*>& addrs, const std::vector<uint64_t>& lengths,
                      const std::string& configPath) override;
    int32_t IsExists(const std::vector<uint64_t>& lows, const std::vector<uint16_t>& highs,
                     uint32_t nsid) override;
    int32_t BatchGet(const std::vector<uint64_t>& lows, const std::vector<uint16_t>& highs,
                     const std::vector<uint8_t*>& addrs, const std::vector<size_t>& offsets,
                     const std::vector<size_t>& lengths,
                     const std::vector<uint32_t>& nsids) override;
    int32_t BatchPut(const std::vector<uint64_t>& lows, const std::vector<uint16_t>& highs,
                     const std::vector<uint8_t*>& addrs, const std::vector<size_t>& offsets,
                     const std::vector<size_t>& lengths,
                     const std::vector<uint32_t>& nsids) override;
};

}  // namespace UC::Nds
