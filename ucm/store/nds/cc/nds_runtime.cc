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
#include "nds_runtime.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include "metrics_api.h"

namespace UC::Nds {
namespace {
constexpr size_t kHealthIoBytes = 1ULL << 20;
}

NdsRuntime::NdsRuntime() : client_{std::make_unique<RealNdsClient>()} {}

NdsRuntime& NdsRuntime::Instance()
{
    /* NDS reconnect keeps using the MR table and staging until process exit.
     * Deliberately avoid a DSO-unload destructor; tests use ResetForTest(). */
    static NdsRuntime* runtime = new NdsRuntime();
    return *runtime;
}

Status NdsRuntime::ValidateConfig(const StoreConfig& config) const
{
    if (config.configPath.empty() || config.shardSize == 0 || config.blockSize == 0 ||
        config.blockSize % config.shardSize != 0) {
        return Status::InvalidParam("invalid NDS store path/block/shard geometry");
    }
    if (config.valueSize == 0 || config.stagingValueSize == 0 || config.stagingCapacity == 0) {
        return Status::InvalidParam("invalid NDS value/staging size");
    }
    const bool directGeometry = config.valueSize == config.shardSize &&
                                config.shardSize == config.stagingValueSize;
    if (config.policy == MrPolicy::DirectOnly && !directGeometry) {
        return Status::InvalidParam(
            "nds_mr_policy=direct_only requires value_size==shard_size and read/write batch "
            "alignment: shard={} value={} staging={}",
            config.shardSize, config.valueSize, config.stagingValueSize);
    }
    if (!directGeometry && config.valueSize != config.stagingValueSize) {
        return Status::InvalidParam(
            "NDS staging fallback requires value_size({}) == aligned staging size({}); "
            "direct shard size is {}",
            config.valueSize, config.stagingValueSize, config.shardSize);
    }
    return Status::OK();
}

Expected<size_t> NdsRuntime::RegisterStore(const StoreConfig& config)
{
    auto valid = ValidateConfig(config);
    if (valid.Failure()) { return valid; }
    std::lock_guard<std::mutex> lock{mutex_};
    if (failed_) { return Status::Error("NDS runtime is failed: " + failureMessage_); }
    if (active_ && !config.lookupOnly) {
        return Status::Error("NDS registration is closed; cannot add a worker store after ACTIVE");
    }
    if (!configPath_.empty() && config.configPath != configPath_) {
        return Status::InvalidParam("all NDS stores must use the same nds_config_path");
    }
    if (stagingCapacity_ != 0 && config.stagingCapacity != stagingCapacity_) {
        return Status::InvalidParam("all NDS stores must use the same staging capacity");
    }
    for (const auto& [id, record] : stores_) {
        (void)id;
        if (record->config.nsid == config.nsid && record->config.valueSize != config.valueSize) {
            return Status::InvalidParam("nsid({}) has conflicting NDS value sizes", config.nsid);
        }
    }
    if (active_) {
        const auto it = valueSizes_.find(config.nsid);
        if (it != valueSizes_.end() && it->second != config.valueSize) {
            return Status::InvalidParam("nsid({}) has conflicting NDS value sizes", config.nsid);
        }
    }
    if (configPath_.empty()) { configPath_ = config.configPath; }
    if (stagingCapacity_ == 0) { stagingCapacity_ = config.stagingCapacity; }
    auto record = std::make_shared<Record>();
    record->id = nextStoreId_++;
    record->config = config;
    stores_.emplace(record->id, record);
    if (active_) { valueSizes_[config.nsid] = config.valueSize; }
    return size_t{record->id};
}

Status NdsRuntime::RegisterHostBuffers(size_t storeId,
                                       const std::vector<Detail::HostBufferSpan>& spans)
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (active_) { return Status::Error("NDS registration is closed; cannot add MR spans"); }
    const auto it = stores_.find(storeId);
    if (it == stores_.end()) { return Status::NotFound(); }
    auto& record = *it->second;
    if (record.closed) { return Status::Error("NDS store is closed"); }
    if (record.config.lookupOnly && !spans.empty()) {
        return Status::InvalidParam("lookup-only NDS store cannot register business MR spans");
    }
    if (!record.spans.empty() && !spans.empty()) {
        return Status::DuplicateKey();
    }
    for (const auto& span : spans) {
        const auto base = reinterpret_cast<uintptr_t>(span.addr);
        if (base == 0 || base % 4096 != 0 || span.mappedBytes == 0 || span.slotBytes == 0 ||
            span.slotCount == 0 || span.slotCount > span.mappedBytes / span.slotBytes) {
            return Status::InvalidParam("invalid NDS host buffer span geometry");
        }
        if (base > std::numeric_limits<uintptr_t>::max() - span.mappedBytes) {
            return Status::InvalidParam("NDS host buffer span address overflows");
        }
    }
    if (record.config.policy != MrPolicy::Staging) { record.spans = spans; }
    return Status::OK();
}

Status NdsRuntime::FinalizeRegistration()
{
    std::shared_ptr<Record> warmupRecord;
    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (failed_) { return Status::Error("NDS runtime is failed: " + failureMessage_); }
        if (active_) { return Status::OK(); }
        if (stores_.empty()) { return Status::Error("cannot finalize NDS without a store"); }
        size_t controlBytes = kHealthIoBytes;
        std::vector<std::pair<uintptr_t, size_t>> ranges;
        std::vector<void*> addrs;
        std::vector<uint64_t> lengths;
        valueSizes_.clear();
        for (const auto& [id, record] : stores_) {
            (void)id;
            if (record->closed) { return Status::Error("cannot finalize a closed NDS store"); }
            const auto& config = record->config;
            const auto [it, inserted] = valueSizes_.emplace(config.nsid, config.valueSize);
            if (!inserted && it->second != config.valueSize) {
                return Status::InvalidParam("nsid({}) has conflicting NDS value sizes", config.nsid);
            }
            controlBytes = std::max(controlBytes, config.valueSize);
            if (warmupRecord == nullptr && config.warmupOnInit) { warmupRecord = record; }
            for (const auto& span : record->spans) {
                const auto base = reinterpret_cast<uintptr_t>(span.addr);
                ranges.emplace_back(base, span.mappedBytes);
                addrs.push_back(span.addr);
                lengths.push_back(span.mappedBytes);
            }
        }
        auto status = staging_.Setup(stagingCapacity_, controlBytes);
        if (status.Failure()) {
            failed_ = true;
            failureMessage_ = status.ToString();
            return status;
        }
        ranges.emplace_back(reinterpret_cast<uintptr_t>(staging_.Base()), staging_.MappedBytes());
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 1; i < ranges.size(); ++i) {
            if (ranges[i].first < ranges[i - 1].first + ranges[i - 1].second) {
                failed_ = true;
                failureMessage_ = "NDS MR spans overlap";
                return Status::InvalidParam("NDS MR spans overlap at index({})", i);
            }
        }
        addrs.push_back(staging_.Base());
        lengths.push_back(staging_.MappedBytes());
        const auto ret = client_->InitMulti(addrs, lengths, configPath_);
        if (ret != 0) {
            failed_ = true;
            failureMessage_ = "NDS::initMulti returned " + std::to_string(ret);
            return Status::Error(failureMessage_);
        }
        if (warmupRecord != nullptr) {
            status = Warmup(warmupRecord);
            if (status.Failure()) {
                failed_ = true;
                failureMessage_ = "NDS warmup failed: " + status.ToString();
                return status;
            }
        }
        active_ = true;
    }
    return Status::OK();
}

void NdsRuntime::UnregisterStore(size_t storeId)
{
    std::lock_guard<std::mutex> lock{mutex_};
    const auto it = stores_.find(storeId);
    if (it == stores_.end()) { return; }
    it->second->closed = true;
    stores_.erase(it);
    if (!active_ && stores_.empty()) {
        configPath_.clear();
        stagingCapacity_ = 0;
    }
}

Expected<std::shared_ptr<NdsRuntime::Record>> NdsRuntime::GetRecord(size_t storeId, bool forIo)
{
    std::lock_guard<std::mutex> lock{mutex_};
    if (failed_) { return Status::Error("NDS runtime is failed: " + failureMessage_); }
    if (!active_) { return Status::Error("NDS registration is not finalized"); }
    const auto it = stores_.find(storeId);
    if (it == stores_.end()) { return Status::NotFound(); }
    if (it->second->closed) { return Status::Error("NDS store is closed"); }
    if (forIo && it->second->config.lookupOnly) {
        return Status::Unsupported();
    }
    return std::shared_ptr<Record>{it->second};
}

Expected<int32_t> NdsRuntime::Exists(size_t storeId, const std::vector<NdsKey>& keys)
{
    auto record = GetRecord(storeId, false);
    if (!record) { return record.Error(); }
    if (keys.empty()) { return int32_t{0}; }
    std::vector<uint64_t> lows;
    std::vector<uint16_t> highs;
    lows.reserve(keys.size());
    highs.reserve(keys.size());
    for (const auto& key : keys) {
        lows.push_back(key.low);
        highs.push_back(key.high);
    }
    const auto ret = client_->IsExists(lows, highs, record.Value()->config.nsid);
    if (ret < 0) { return Status::Error("NDS::isExists returned " + std::to_string(ret)); }
    return int32_t{ret};
}

bool NdsRuntime::DirectAddress(const Record& record, const void* addr) const
{
    const auto& config = record.config;
    if (config.policy == MrPolicy::Staging || config.valueSize != config.shardSize ||
        config.shardSize != config.stagingValueSize) {
        return false;
    }
    const auto address = reinterpret_cast<uintptr_t>(addr);
    for (const auto& span : record.spans) {
        if (span.slotBytes != config.shardSize) { continue; }
        const auto base = reinterpret_cast<uintptr_t>(span.addr);
        if (address < base) { continue; }
        const auto offset = address - base;
        const auto validBytes = span.slotBytes * span.slotCount;
        if (offset > validBytes || config.shardSize > validBytes - offset) { continue; }
        if (offset % span.slotBytes > span.slotBytes - config.shardSize) { continue; }
        return true;
    }
    return false;
}

Status NdsRuntime::Transfer(size_t storeId, bool load, const std::vector<NdsKey>& keys,
                            const std::vector<void*>& addrs)
{
    auto record = GetRecord(storeId, true);
    if (!record) { return record.Error(); }
    if (keys.empty() || keys.size() != addrs.size()) {
        return Status::InvalidParam("invalid NDS transfer key/address count");
    }
    const auto& config = record.Value()->config;
    std::vector<uint64_t> lows;
    std::vector<uint16_t> highs;
    std::vector<uint8_t*> ioAddrs;
    std::vector<size_t> offsets(keys.size(), 0);
    std::vector<size_t> lengths(keys.size(), config.shardSize);
    std::vector<uint32_t> nsids(keys.size(), config.nsid);
    std::vector<StagingArena::Lease> leases;
    std::vector<std::pair<size_t, void*>> staged;
    lows.reserve(keys.size());
    highs.reserve(keys.size());
    ioAddrs.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        if (addrs[i] == nullptr) { return Status::InvalidParam("null NDS transfer address"); }
        lows.push_back(keys[i].low);
        highs.push_back(keys[i].high);
        if (DirectAddress(*record.Value(), addrs[i])) {
            ioAddrs.push_back(static_cast<uint8_t*>(addrs[i]));
            directShards_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (config.policy == MrPolicy::DirectOnly) {
            return Status::Error("NDS direct_only rejected an unregistered or misaligned address");
        }
        auto lease = staging_.Acquire(config.stagingValueSize);
        if (!lease) { return lease.Error(); }
        auto* stagingAddr = static_cast<uint8_t*>(lease.Value().Data());
        if (!load) {
            std::memcpy(stagingAddr, addrs[i], config.shardSize);
            if (config.stagingValueSize > config.shardSize) {
                std::memset(stagingAddr + config.shardSize, 0,
                            config.stagingValueSize - config.shardSize);
            }
        }
        staged.emplace_back(i, addrs[i]);
        ioAddrs.push_back(stagingAddr);
        leases.push_back(std::move(lease.Value()));
        stagingShards_.fetch_add(1, std::memory_order_relaxed);
    }
    const auto ret = load ? client_->BatchGet(lows, highs, ioAddrs, offsets, lengths, nsids)
                          : client_->BatchPut(lows, highs, ioAddrs, offsets, lengths, nsids);
    if (ret != 0) {
        if (load) {
            Metrics::UpdateStats(NAME_TO_METRIC_ID("nds_load_failed_keys_total"),
                                 static_cast<double>(keys.size()));
        }
        return Status::Error(std::string{"NDS::batch"} + (load ? "Get" : "Put") + " returned " +
                             std::to_string(ret));
    }
    if (load) {
        Metrics::UpdateStats(NAME_TO_METRIC_ID("nds_load_success_keys_total"),
                             static_cast<double>(keys.size()));
        Metrics::UpdateStats(NAME_TO_METRIC_ID("nds_load_success_bytes_total"),
                             static_cast<double>(keys.size() * config.shardSize));
        size_t leaseIndex = 0;
        for (const auto& [index, original] : staged) {
            (void)index;
            std::memcpy(original, leases[leaseIndex].Data(), config.shardSize);
            ++leaseIndex;
        }
    }
    return Status::OK();
}

Status NdsRuntime::Warmup(const std::shared_ptr<Record>& record)
{
    std::lock_guard<std::mutex> lock{healthMutex_};
    auto* control = static_cast<uint8_t*>(staging_.ControlData());
    if (control == nullptr) { return Status::Error("NDS control MR is unavailable"); }
    const auto& config = record->config;
    const auto bytes = std::min(kHealthIoBytes, config.valueSize);
    const NdsKey key{0x4e44535f55434d31ULL ^ record->id, 0x4e44};
    const std::vector<uint64_t> lows{key.low};
    const std::vector<uint16_t> highs{key.high};
    const std::vector<uint8_t*> addrs{control};
    const std::vector<size_t> offsets{0};
    const std::vector<size_t> lengths{bytes};
    const std::vector<uint32_t> nsids{config.nsid};
    std::memset(control, static_cast<int>(record->id & 0xff), bytes);
    auto ret = client_->BatchPut(lows, highs, addrs, offsets, lengths, nsids);
    if (ret != 0) { return Status::StoreUnhealthy("NDS warmup put failed"); }
    std::memset(control, 0, bytes);
    ret = client_->BatchGet(lows, highs, addrs, offsets, lengths, nsids);
    if (ret != 0) { return Status::StoreUnhealthy("NDS warmup get failed"); }
    const auto expected = static_cast<uint8_t>(record->id & 0xff);
    for (size_t i = 0; i < bytes; ++i) {
        if (control[i] != expected) { return Status::StoreUnhealthy("NDS warmup data mismatch"); }
    }
    return Status::OK();
}

Status NdsRuntime::CheckHealth(size_t storeId)
{
    auto record = GetRecord(storeId, false);
    if (!record) { return record.Error(); }
    return Warmup(record.Value());
}

Status NdsRuntime::SetClientForTest(std::unique_ptr<INdsClient> client)
{
    if (client == nullptr) { return Status::InvalidParam("null NDS client"); }
    std::lock_guard<std::mutex> lock{mutex_};
    if (active_ || !stores_.empty() || staging_.Base() != nullptr) {
        return Status::Error("cannot replace NDS client after registration");
    }
    client_ = std::move(client);
    return Status::OK();
}

void NdsRuntime::ResetForTest()
{
    std::lock_guard<std::mutex> lock{mutex_};
    stores_.clear();
    valueSizes_.clear();
    staging_.Reset();
    client_ = std::make_unique<RealNdsClient>();
    nextStoreId_ = 1;
    stagingCapacity_ = 0;
    directShards_ = 0;
    stagingShards_ = 0;
    configPath_.clear();
    active_ = false;
    failed_ = false;
    failureMessage_.clear();
}

}  // namespace UC::Nds
