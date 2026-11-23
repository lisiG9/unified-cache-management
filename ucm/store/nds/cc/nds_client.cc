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
#include "nds_client.h"
#include <dlfcn.h>
#include <nds_interface.h>

namespace UC::Nds {
namespace {

template <typename Func>
int32_t NoThrow(Func&& func)
{
    try {
        return func();
    } catch (...) {
        return -1;
    }
}

void PinNdsLibrary()
{
    /* LibraryLoader may close libndsstore while Python shuts down. Keep the
     * wrapper and SDK loaded because their background reconnect threads and MR
     * table intentionally have process lifetime. The handle is never closed. */
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&NDS::initMulti), &info) == 0 ||
        info.dli_fname == nullptr) {
        return;
    }
    (void)dlopen(info.dli_fname, RTLD_LAZY | RTLD_LOCAL);
}

}  // namespace

RealNdsClient::RealNdsClient() { PinNdsLibrary(); }

int32_t RealNdsClient::InitMulti(const std::vector<void*>& addrs,
                                 const std::vector<uint64_t>& lengths,
                                 const std::string& configPath)
{
    return NoThrow([&] { return NDS::initMulti(addrs, lengths, configPath.c_str()); });
}

int32_t RealNdsClient::IsExists(const std::vector<uint64_t>& lows,
                                const std::vector<uint16_t>& highs, uint32_t nsid)
{
    return NoThrow([&] { return NDS::isExists(lows, highs, nsid); });
}

int32_t RealNdsClient::BatchGet(const std::vector<uint64_t>& lows,
                                const std::vector<uint16_t>& highs,
                                const std::vector<uint8_t*>& addrs,
                                const std::vector<size_t>& offsets,
                                const std::vector<size_t>& lengths,
                                const std::vector<uint32_t>& nsids)
{
    return NoThrow([&] { return NDS::batchGet(lows, highs, addrs, offsets, lengths, nsids); });
}

int32_t RealNdsClient::BatchPut(const std::vector<uint64_t>& lows,
                                const std::vector<uint16_t>& highs,
                                const std::vector<uint8_t*>& addrs,
                                const std::vector<size_t>& offsets,
                                const std::vector<size_t>& lengths,
                                const std::vector<uint32_t>& nsids)
{
    return NoThrow([&] { return NDS::batchPut(lows, highs, addrs, offsets, lengths, nsids); });
}

}  // namespace UC::Nds
