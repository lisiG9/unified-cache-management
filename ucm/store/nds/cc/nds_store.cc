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
#include "nds_store.h"
#include <limits>
#include <utility>
#include "logger/logger.h"
#include "metrics_api.h"
#include "nds_config.h"

namespace UC::Nds {

Store::Store() : runtime_{NdsRuntime::Instance()} {}
Store::~Store() { ShutdownHostBuffers(); }

Status Store::Setup(const Detail::Dictionary& config)
{
    StoreConfig ndsConfig;
    config.Get("nds_config_path", ndsConfig.configPath);
    ssize_t nsid = 0;
    config.GetNumber("nds_nsid", nsid);
    if (nsid < 0 || static_cast<uint64_t>(nsid) > std::numeric_limits<uint32_t>::max()) {
        return Status::InvalidParam("invalid NDS nsid({})", nsid);
    }
    ndsConfig.nsid = static_cast<uint32_t>(nsid);
    config.GetNumber("shard_size", ndsConfig.shardSize);
    config.GetNumber("block_size", ndsConfig.blockSize);
    int32_t deviceId = -1;
    config.GetNumber("device_id", deviceId);
    ndsConfig.lookupOnly = deviceId < 0;
    lookupOnly_ = ndsConfig.lookupOnly;
    std::string policy = "direct";
    config.Get("nds_mr_policy", policy);
    if (policy == "direct") {
        ndsConfig.policy = MrPolicy::Direct;
    } else if (policy == "direct_only") {
        ndsConfig.policy = MrPolicy::DirectOnly;
    } else if (policy == "staging") {
        ndsConfig.policy = MrPolicy::Staging;
    } else {
        return Status::InvalidParam("invalid nds_mr_policy({})", policy);
    }
    size_t stagingCapacityGb = 0;
    config.GetNumber("nds_staging_capacity_gb", stagingCapacityGb);
    if (stagingCapacityGb != 0) {
        if (stagingCapacityGb > std::numeric_limits<size_t>::max() >> 30) {
            return Status::InvalidParam("NDS staging capacity overflows");
        }
        ndsConfig.stagingCapacity = stagingCapacityGb << 30;
    }
    size_t stagingCapacityBytes = 0;
    config.GetNumber("nds_staging_capacity_bytes", stagingCapacityBytes);
    if (stagingCapacityBytes != 0) { ndsConfig.stagingCapacity = stagingCapacityBytes; }
    config.Get("nds_warmup_on_init", ndsConfig.warmupOnInit);
    auto geometry = LoadGeometry(ndsConfig.configPath, ndsConfig.nsid, ndsConfig.shardSize);
    if (!geometry) { return geometry.Error(); }
    ndsConfig.valueSize = geometry.Value().valueSize;
    ndsConfig.readBatch = geometry.Value().readBatch;
    ndsConfig.writeBatch = geometry.Value().writeBatch;
    ndsConfig.stagingValueSize = geometry.Value().stagingValueSize;
    auto id = runtime_.RegisterStore(ndsConfig);
    if (!id) { return id.Error(); }
    storeId_ = id.Value();
    shardsPerBlock_ = ndsConfig.blockSize / ndsConfig.shardSize;
    size_t loadWorkers = 4;
    size_t dumpWorkers = 4;
    size_t queueDepth = 1024;
    size_t timeoutMs = 30000;
    config.GetNumber("nds_load_workers", loadWorkers);
    config.GetNumber("nds_dump_workers", dumpWorkers);
    config.GetNumber("nds_queue_depth", queueDepth);
    config.GetNumber("timeout_ms", timeoutMs);
    auto status = transMgr_.Setup(runtime_, storeId_, loadWorkers, dumpWorkers, queueDepth, timeoutMs);
    if (status.Failure()) {
        runtime_.UnregisterStore(storeId_);
        storeId_ = 0;
        return status;
    }
    UC_INFO("NDS store({}) setup: nsid={} shard={} block={} value={} policy={} lookup_only={}.",
            storeId_, ndsConfig.nsid, ndsConfig.shardSize, ndsConfig.blockSize,
            ndsConfig.valueSize, policy, ndsConfig.lookupOnly);
    return Status::OK();
}

Expected<std::vector<NdsKey>> Store::BlockKeys(const Detail::BlockId& block) const
{
    std::vector<NdsKey> keys;
    keys.reserve(shardsPerBlock_);
    for (size_t shard = 0; shard < shardsPerBlock_; ++shard) {
        keys.push_back(KeyEncoder::Encode(block, shard));
    }
    return std::move(keys);
}

void Store::RecordLookup(size_t queryBlocks, size_t queryKeys, size_t hitBlocks, size_t hitKeys)
{
    Metrics::UpdateStats(NAME_TO_METRIC_ID("nds_lookup_query_blocks_total"),
                         static_cast<double>(queryBlocks));
    Metrics::UpdateStats(NAME_TO_METRIC_ID("nds_lookup_hit_blocks_total"),
                         static_cast<double>(hitBlocks));
    Metrics::UpdateStats(NAME_TO_METRIC_ID("nds_lookup_query_keys_total"),
                         static_cast<double>(queryKeys));
    Metrics::UpdateStats(NAME_TO_METRIC_ID("nds_lookup_hit_keys_total"),
                         static_cast<double>(hitKeys));
}

Expected<std::vector<uint8_t>> Store::Lookup(const Detail::BlockId* blocks, size_t num)
{
    if (num != 0 && blocks == nullptr) { return Status::InvalidParam("null NDS block array"); }
    std::vector<uint8_t> hits(num, false);
    size_t hitBlocks = 0;
    size_t hitKeys = 0;
    for (size_t i = 0; i < num; ++i) {
        auto keys = BlockKeys(blocks[i]);
        if (!keys) { return keys.Error(); }
        auto ret = runtime_.Exists(storeId_, keys.Value());
        if (!ret) { return ret.Error(); }
        if (static_cast<size_t>(ret.Value()) > keys.Value().size()) {
            return Status::Error("NDS::isExists returned an invalid prefix count");
        }
        hitKeys += static_cast<size_t>(ret.Value());
        hits[i] = static_cast<size_t>(ret.Value()) == keys.Value().size();
        if (hits[i] != 0) { ++hitBlocks; }
    }
    RecordLookup(num, num * shardsPerBlock_, hitBlocks, hitKeys);
    return std::move(hits);
}

Expected<ssize_t> Store::LookupOnPrefix(const Detail::BlockId* blocks, size_t num)
{
    if (num == 0) { return ssize_t{-1}; }
    if (blocks == nullptr) { return Status::InvalidParam("null NDS block array"); }
    std::vector<NdsKey> keys;
    keys.reserve(num * shardsPerBlock_);
    for (size_t i = 0; i < num; ++i) {
        for (size_t shard = 0; shard < shardsPerBlock_; ++shard) {
            keys.push_back(KeyEncoder::Encode(blocks[i], shard));
        }
    }
    auto ret = runtime_.Exists(storeId_, keys);
    if (!ret) { return ret.Error(); }
    if (static_cast<size_t>(ret.Value()) > keys.size()) {
        return Status::Error("NDS::isExists returned an invalid prefix count");
    }
    const auto hitBlocks = static_cast<size_t>(ret.Value()) / shardsPerBlock_;
    RecordLookup(num, keys.size(), hitBlocks, static_cast<size_t>(ret.Value()));
    return static_cast<ssize_t>(hitBlocks) - 1;
}

Expected<ssize_t> Store::LookupOnReverse(const Detail::BlockId* blocks, size_t num)
{
    if (num != 0 && blocks == nullptr) { return Status::InvalidParam("null NDS block array"); }
    size_t queryBlocks = 0;
    size_t queryKeys = 0;
    size_t hitKeys = 0;
    for (ssize_t i = static_cast<ssize_t>(num) - 1; i >= 0; --i) {
        auto keys = BlockKeys(blocks[i]);
        if (!keys) { return keys.Error(); }
        auto ret = runtime_.Exists(storeId_, keys.Value());
        if (!ret) { return ret.Error(); }
        if (static_cast<size_t>(ret.Value()) > keys.Value().size()) {
            return Status::Error("NDS::isExists returned an invalid prefix count");
        }
        ++queryBlocks;
        queryKeys += keys.Value().size();
        hitKeys += static_cast<size_t>(ret.Value());
        if (static_cast<size_t>(ret.Value()) == keys.Value().size()) {
            RecordLookup(queryBlocks, queryKeys, 1, hitKeys);
            return ssize_t{i};
        }
    }
    RecordLookup(queryBlocks, queryKeys, 0, hitKeys);
    return ssize_t{-1};
}

void Store::Prefetch(const Detail::BlockId* blocks, size_t num)
{
    (void)blocks;
    (void)num;
}

Status Store::CheckHealth() { return runtime_.CheckHealth(storeId_); }

Expected<Detail::TaskHandle> Store::Submit(bool load, Detail::TaskDesc task)
{
    if (closing_) { return Status::Error("NDS store is closing"); }
    if (lookupOnly_) { return Status::Unsupported(); }
    if (task.empty()) { return Status::InvalidParam("empty NDS transfer task"); }
    if (task.memoryType != Detail::TaskDesc::MemoryType::DDR) {
        return Status::InvalidParam("NDS backend tasks require memoryType=DDR");
    }
    for (const auto& shard : task) {
        if (shard.index >= shardsPerBlock_ || shard.addrs.size() != 1 || shard.addrs.front() == nullptr) {
            return Status::InvalidParam("invalid NDS shard({}) address/index", shard.index);
        }
    }
    return transMgr_.Submit(Task{load ? Task::Type::Load : Task::Type::Dump, std::move(task)});
}

Expected<Detail::TaskHandle> Store::Load(Detail::TaskDesc task)
{
    return Submit(true, std::move(task));
}
Expected<Detail::TaskHandle> Store::Dump(Detail::TaskDesc task)
{
    return Submit(false, std::move(task));
}
Expected<bool> Store::Check(Detail::TaskHandle taskId) { return transMgr_.Check(taskId); }
Status Store::Wait(Detail::TaskHandle taskId) { return transMgr_.Wait(taskId); }

Status Store::RegisterHostBuffers(const std::vector<Detail::HostBufferSpan>& spans)
{
    return runtime_.RegisterHostBuffers(storeId_, spans);
}
Status Store::FinalizeRegistration() { return runtime_.FinalizeRegistration(); }

void Store::ShutdownHostBuffers()
{
    /* ACTIVE teardown is supported only during process shutdown. The SDK
     * reconnect path retains every MR, so hot-destroying one of several live
     * Cache2 pipelines is intentionally outside this version's contract. */
    if (closing_.exchange(true)) { return; }
    transMgr_.Close();
    if (storeId_ != 0) {
        runtime_.UnregisterStore(storeId_);
        storeId_ = 0;
    }
}

}  // namespace UC::Nds

extern "C" UC::StoreV1* MakeNdsStore() { return new UC::Nds::Store(); }
