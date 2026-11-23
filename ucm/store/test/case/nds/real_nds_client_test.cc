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
#include "nds/cc/nds_client.h"

/* Link-only adapter for the fake-client tests. Every behavioral test injects
 * FakeNdsClient before any I/O, so no real NDS library or runtime is required. */
namespace UC::Nds {

RealNdsClient::RealNdsClient() = default;

int32_t RealNdsClient::InitMulti(const std::vector<void*>&, const std::vector<uint64_t>&,
                                 const std::string&)
{
    return -1;
}
int32_t RealNdsClient::IsExists(const std::vector<uint64_t>&, const std::vector<uint16_t>&,
                                uint32_t)
{
    return -1;
}
int32_t RealNdsClient::BatchGet(const std::vector<uint64_t>&, const std::vector<uint16_t>&,
                                const std::vector<uint8_t*>&, const std::vector<size_t>&,
                                const std::vector<size_t>&, const std::vector<uint32_t>&)
{
    return -1;
}
int32_t RealNdsClient::BatchPut(const std::vector<uint64_t>&, const std::vector<uint16_t>&,
                                const std::vector<uint8_t*>&, const std::vector<size_t>&,
                                const std::vector<size_t>&, const std::vector<uint32_t>&)
{
    return -1;
}

}  // namespace UC::Nds
