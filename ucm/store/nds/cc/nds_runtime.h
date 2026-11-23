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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "host_buffer.h"
#include "key_encoder.h"
#include "nds_client.h"
#include "staging_arena.h"
#include "status/status.h"

namespace UC::Nds {

enum class MrPolicy : uint8_t { Direct, DirectOnly, Staging };

struct StoreConfig {
    uint32_t nsid{0};
    size_t shardSize{0};
    size_t blockSize{0};
    size_t valueSize{0};
    size_t readBatch{0};
    size_t writeBatch{0};
    size_t stagingValueSize{0};
    MrPolicy policy{MrPolicy::Direct};
    bool lookupOnly{false};
    bool warmupOnInit{true};
    std::string configPath{};
    size_t stagingCapacity{16ULL << 30};
};

class NdsRuntime {
    struct Record {
        size_t id{0};
        StoreConfig config{};
        std::vector<Detail::HostBufferSpan> spans{};
        bool closed{false};
    };

public:
    static NdsRuntime& Instance();

    Expected<size_t> RegisterStore(const StoreConfig& config);
    Status RegisterHostBuffers(size_t storeId, const std::vector<Detail::HostBufferSpan>& spans);
    Status FinalizeRegistration();
    void UnregisterStore(size_t storeId);

    Expected<int32_t> Exists(size_t storeId, const std::vector<NdsKey>& keys);
    Status Transfer(size_t storeId, bool load, const std::vector<NdsKey>& keys,
                    const std::vector<void*>& addrs);
    Status CheckHealth(size_t storeId);

    /* Test hooks: legal only while no store or MR is registered. */
    Status SetClientForTest(std::unique_ptr<INdsClient> client);
    void ResetForTest();
    size_t DirectShardsForTest() const { return directShards_.load(); }
    size_t StagingShardsForTest() const { return stagingShards_.load(); }

private:
    NdsRuntime();
    Expected<std::shared_ptr<Record>> GetRecord(size_t storeId, bool forIo);
    Status ValidateConfig(const StoreConfig& config) const;
    Status Warmup(const std::shared_ptr<Record>& record);
    bool DirectAddress(const Record& record, const void* addr) const;

    mutable std::mutex mutex_;
    std::mutex healthMutex_;
    std::unordered_map<size_t, std::shared_ptr<Record>> stores_;
    std::unordered_map<uint32_t, size_t> valueSizes_;
    std::unique_ptr<INdsClient> client_;
    StagingArena staging_;
    size_t nextStoreId_{1};
    size_t stagingCapacity_{0};
    std::atomic<size_t> directShards_{0};
    std::atomic<size_t> stagingShards_{0};
    std::string configPath_{};
    bool active_{false};
    bool failed_{false};
    std::string failureMessage_{};
};

}  // namespace UC::Nds
