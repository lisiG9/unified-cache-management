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

#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include "nds/cc/key_encoder.h"
#include "nds/cc/nds_client.h"

namespace UC::Nds::Test {

class FakeNdsClient : public INdsClient {
    using ObjectKey = std::tuple<uint32_t, uint64_t, uint16_t>;

public:
    int32_t InitMulti(const std::vector<void*>& addrs, const std::vector<uint64_t>& lengths,
                      const std::string& configPath) override
    {
        std::lock_guard<std::mutex> lock{mutex_};
        ++initCalls;
        lastConfigPath = configPath;
        registeredAddrs = addrs;
        registeredLengths = lengths;
        return initResult;
    }
    int32_t IsExists(const std::vector<uint64_t>& lows, const std::vector<uint16_t>& highs,
                     uint32_t nsid) override
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (failNextIo) {
            failNextIo = false;
            return -1;
        }
        int32_t prefix = 0;
        for (size_t i = 0; i < lows.size(); ++i) {
            if (objects_.find(ObjectKey{nsid, lows[i], highs[i]}) == objects_.end()) { break; }
            ++prefix;
        }
        return prefix;
    }
    int32_t BatchGet(const std::vector<uint64_t>& lows, const std::vector<uint16_t>& highs,
                     const std::vector<uint8_t*>& addrs, const std::vector<size_t>& offsets,
                     const std::vector<size_t>& lengths,
                     const std::vector<uint32_t>& nsids) override
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (failNextIo) {
            failNextIo = false;
            return -1;
        }
        for (size_t i = 0; i < lows.size(); ++i) {
            const auto it = objects_.find(ObjectKey{nsids[i], lows[i], highs[i]});
            if (it == objects_.end() || offsets[i] + lengths[i] > it->second.size()) {
                return -1;
            }
            std::memcpy(addrs[i], it->second.data() + offsets[i], lengths[i]);
        }
        return 0;
    }
    int32_t BatchPut(const std::vector<uint64_t>& lows, const std::vector<uint16_t>& highs,
                     const std::vector<uint8_t*>& addrs, const std::vector<size_t>& offsets,
                     const std::vector<size_t>& lengths,
                     const std::vector<uint32_t>& nsids) override
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (failNextIo) {
            failNextIo = false;
            return -1;
        }
        for (size_t i = 0; i < lows.size(); ++i) {
            auto& object = objects_[ObjectKey{nsids[i], lows[i], highs[i]}];
            if (object.size() < offsets[i] + lengths[i]) { object.resize(offsets[i] + lengths[i]); }
            std::memcpy(object.data() + offsets[i], addrs[i], lengths[i]);
        }
        return 0;
    }

    void Erase(uint32_t nsid, const NdsKey& key)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        objects_.erase(ObjectKey{nsid, key.low, key.high});
    }
    bool Contains(uint32_t nsid, const NdsKey& key)
    {
        std::lock_guard<std::mutex> lock{mutex_};
        return objects_.find(ObjectKey{nsid, key.low, key.high}) != objects_.end();
    }

    int initCalls{0};
    int initResult{0};
    bool failNextIo{false};
    std::string lastConfigPath{};
    std::vector<void*> registeredAddrs{};
    std::vector<uint64_t> registeredLengths{};

private:
    std::mutex mutex_;
    std::map<ObjectKey, std::vector<uint8_t>> objects_;
};

}  // namespace UC::Nds::Test
