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
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include "fake_nds_client.h"
#include "nds/cc/nds_store.h"

namespace UC::Nds::Test {
namespace {

using BufferPtr = std::unique_ptr<void, decltype(&std::free)>;

BufferPtr AlignedBuffer(size_t bytes, uint8_t value = 0)
{
    void* addr = nullptr;
    if (posix_memalign(&addr, 4096, bytes) != 0) { return {nullptr, &std::free}; }
    std::memset(addr, value, bytes);
    return {addr, &std::free};
}

Detail::BlockId Block(uint8_t id)
{
    Detail::BlockId block{};
    block[0] = static_cast<std::byte>(id);
    block[15] = static_cast<std::byte>(0xff - id);
    return block;
}

Detail::TaskDesc TaskFor(const Detail::BlockId& block, void* addr, size_t shard = 0)
{
    Detail::TaskDesc task;
    task.memoryType = Detail::TaskDesc::MemoryType::DDR;
    task.push_back(Detail::Shard{block, shard, {addr}});
    return task;
}

class NdsStoreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        auto& runtime = NdsRuntime::Instance();
        runtime.ResetForTest();
        auto client = std::make_unique<FakeNdsClient>();
        client_ = client.get();
        ASSERT_TRUE(runtime.SetClientForTest(std::move(client)).Success());
        directory_ = std::filesystem::temp_directory_path() /
                     ("ucm_nds_test_" + std::to_string(getpid()));
        std::filesystem::remove_all(directory_);
        std::filesystem::create_directories(directory_);
        configPath_ = (directory_ / "nds_config.conf").string();
        std::ofstream{configPath_} << "# test config\n";
        std::ofstream{directory_ / "nds_wrapper.conf"}
            << "read_batch_size_kb = 4\n"
            << "write_batch_size_kb = 4\n"
            << "value_size_kb_nsid11 = 4\n";
    }
    void TearDown() override
    {
        NdsRuntime::Instance().ResetForTest();
        std::filesystem::remove_all(directory_);
    }

    Detail::Dictionary Config(size_t shardSize = 4096, const std::string& policy = "direct")
    {
        Detail::Dictionary config;
        config.Set("nds_config_path", configPath_);
        config.SetNumber("nds_nsid", 11);
        config.SetNumber("device_id", 0);
        config.SetNumber("shard_size", shardSize);
        config.SetNumber("block_size", shardSize);
        config.Set("nds_mr_policy", policy);
        config.SetNumber("nds_staging_capacity_bytes", size_t{65536});
        config.Set("nds_warmup_on_init", false);
        config.SetNumber("nds_load_workers", 1);
        config.SetNumber("nds_dump_workers", 1);
        return config;
    }

    std::unique_ptr<Store> MakeStore(const Detail::Dictionary& config)
    {
        auto store = std::make_unique<Store>();
        EXPECT_TRUE(store->Setup(config).Success());
        return store;
    }

    FakeNdsClient* client_{nullptr};
    std::filesystem::path directory_{};
    std::string configPath_{};
};

TEST_F(NdsStoreTest, DirectDumpLookupAndLoadRoundTrip)
{
    constexpr size_t bytes = 4096;
    auto memory = AlignedBuffer(bytes * 2);
    ASSERT_NE(memory.get(), nullptr);
    auto* base = static_cast<uint8_t*>(memory.get());
    auto store = MakeStore(Config());
    ASSERT_NE(store, nullptr);
    ASSERT_TRUE(store->RegisterHostBuffers({{base, bytes * 2, bytes, 2}}).Success());
    ASSERT_TRUE(store->FinalizeRegistration().Success());
    EXPECT_EQ(client_->initCalls, 1);

    const auto block = Block(1);
    std::memset(base, 0x5a, bytes);
    auto dump = store->Dump(TaskFor(block, base));
    ASSERT_TRUE(dump);
    ASSERT_TRUE(store->Wait(dump.Value()).Success());
    ASSERT_TRUE(client_->Contains(11, KeyEncoder::Encode(block, 0)));

    std::memset(base + bytes, 0, bytes);
    auto load = store->Load(TaskFor(block, base + bytes));
    ASSERT_TRUE(load);
    ASSERT_TRUE(store->Wait(load.Value()).Success());
    EXPECT_EQ(std::memcmp(base, base + bytes, bytes), 0);
    auto hits = store->Lookup(&block, 1);
    ASSERT_TRUE(hits);
    ASSERT_EQ(hits.Value().size(), 1);
    EXPECT_TRUE(hits.Value()[0]);
    EXPECT_EQ(NdsRuntime::Instance().DirectShardsForTest(), 2);
    EXPECT_EQ(NdsRuntime::Instance().StagingShardsForTest(), 0);
}

TEST_F(NdsStoreTest, LookupPrefixAndReverseKeepTheirDistinctSemantics)
{
    constexpr size_t bytes = 4096;
    auto memory = AlignedBuffer(bytes * 3, 0x42);
    ASSERT_NE(memory.get(), nullptr);
    auto* base = static_cast<uint8_t*>(memory.get());
    auto store = MakeStore(Config());
    ASSERT_TRUE(store->RegisterHostBuffers({{base, bytes * 3, bytes, 3}}).Success());
    ASSERT_TRUE(store->FinalizeRegistration().Success());
    const Detail::BlockId blocks[]{Block(1), Block(2), Block(3)};
    for (size_t i = 0; i < 3; ++i) {
        auto task = store->Dump(TaskFor(blocks[i], base + i * bytes));
        ASSERT_TRUE(task);
        ASSERT_TRUE(store->Wait(task.Value()).Success());
    }
    client_->Erase(11, KeyEncoder::Encode(blocks[1], 0));

    auto hits = store->Lookup(blocks, 3);
    ASSERT_TRUE(hits);
    EXPECT_EQ(hits.Value(), (std::vector<uint8_t>{true, false, true}));
    auto prefix = store->LookupOnPrefix(blocks, 3);
    ASSERT_TRUE(prefix);
    EXPECT_EQ(prefix.Value(), 0);
    auto reverse = store->LookupOnReverse(blocks, 3);
    ASSERT_TRUE(reverse);
    EXPECT_EQ(reverse.Value(), 2);
}

TEST_F(NdsStoreTest, FailedLoadDoesNotChangeTargetBuffer)
{
    constexpr size_t bytes = 4096;
    auto memory = AlignedBuffer(bytes * 2);
    ASSERT_NE(memory.get(), nullptr);
    auto* base = static_cast<uint8_t*>(memory.get());
    auto store = MakeStore(Config());
    ASSERT_TRUE(store->RegisterHostBuffers({{base, bytes * 2, bytes, 2}}).Success());
    ASSERT_TRUE(store->FinalizeRegistration().Success());
    const auto block = Block(7);
    std::memset(base, 0x11, bytes);
    auto dump = store->Dump(TaskFor(block, base));
    ASSERT_TRUE(dump);
    ASSERT_TRUE(store->Wait(dump.Value()).Success());

    std::memset(base + bytes, 0x33, bytes);
    client_->failNextIo = true;
    auto load = store->Load(TaskFor(block, base + bytes));
    ASSERT_TRUE(load);
    EXPECT_TRUE(store->Wait(load.Value()).Failure());
    for (size_t i = 0; i < bytes; ++i) { EXPECT_EQ(base[bytes + i], 0x33); }
}

TEST_F(NdsStoreTest, MultipleStoresInitializeOnceAndSchedulerIsLookupOnly)
{
    constexpr size_t bytes = 4096;
    auto firstMemory = AlignedBuffer(bytes);
    auto secondMemory = AlignedBuffer(bytes);
    ASSERT_NE(firstMemory.get(), nullptr);
    ASSERT_NE(secondMemory.get(), nullptr);
    auto first = MakeStore(Config());
    auto second = MakeStore(Config());
    ASSERT_TRUE(first->RegisterHostBuffers({{firstMemory.get(), bytes, bytes, 1}}).Success());
    ASSERT_TRUE(second->RegisterHostBuffers({{secondMemory.get(), bytes, bytes, 1}}).Success());
    ASSERT_TRUE(first->FinalizeRegistration().Success());
    ASSERT_TRUE(second->FinalizeRegistration().Success());
    EXPECT_EQ(client_->initCalls, 1);

    auto lateWorker = std::make_unique<Store>();
    EXPECT_TRUE(lateWorker->Setup(Config()).Failure());
    auto schedulerConfig = Config();
    schedulerConfig.SetNumber("device_id", -1);
    auto scheduler = MakeStore(schedulerConfig);
    ASSERT_NE(scheduler, nullptr);
    ASSERT_TRUE(scheduler->FinalizeRegistration().Success());
    EXPECT_EQ(client_->initCalls, 1);
    EXPECT_EQ(scheduler->Load(TaskFor(Block(1), firstMemory.get())).Error(), Status::Unsupported());
}

TEST_F(NdsStoreTest, MisalignedGeometryUsesStagingAndCopiesOnlyPayload)
{
    constexpr size_t payload = 6000;
    std::ofstream{directory_ / "nds_wrapper.conf"}
        << "read_batch_size_kb = 4\n"
        << "write_batch_size_kb = 4\n"
        << "value_size_kb_nsid11 = 8\n";
    auto memory = AlignedBuffer(payload * 2);
    ASSERT_NE(memory.get(), nullptr);
    auto* base = static_cast<uint8_t*>(memory.get());
    auto store = MakeStore(Config(payload));
    ASSERT_NE(store, nullptr);
    ASSERT_TRUE(store->RegisterHostBuffers({{base, payload * 2, payload, 2}}).Success());
    ASSERT_TRUE(store->FinalizeRegistration().Success());

    const auto block = Block(9);
    std::memset(base, 0x6c, payload);
    auto dump = store->Dump(TaskFor(block, base));
    ASSERT_TRUE(dump);
    ASSERT_TRUE(store->Wait(dump.Value()).Success());
    std::memset(base + payload, 0x24, payload);
    auto load = store->Load(TaskFor(block, base + payload));
    ASSERT_TRUE(load);
    ASSERT_TRUE(store->Wait(load.Value()).Success());
    EXPECT_EQ(std::memcmp(base, base + payload, payload), 0);
    EXPECT_EQ(NdsRuntime::Instance().DirectShardsForTest(), 0);
    EXPECT_EQ(NdsRuntime::Instance().StagingShardsForTest(), 2);
    EXPECT_EQ(client_->registeredLengths.back(), 65536 + (1ULL << 20));
}

TEST_F(NdsStoreTest, WarmupAndHealthUseReservedControlArea)
{
    auto memory = AlignedBuffer(4096, 0x58);
    ASSERT_NE(memory.get(), nullptr);
    auto config = Config();
    config.Set("nds_warmup_on_init", true);
    auto store = MakeStore(config);
    ASSERT_TRUE(store->RegisterHostBuffers({{memory.get(), 4096, 4096, 1}}).Success());
    ASSERT_TRUE(store->FinalizeRegistration().Success());
    EXPECT_TRUE(store->CheckHealth().Success());
    for (size_t i = 0; i < 4096; ++i) {
        EXPECT_EQ(static_cast<uint8_t*>(memory.get())[i], 0x58);
    }
}

TEST_F(NdsStoreTest, SameNsidRejectsDifferentValueSizes)
{
    auto first = MakeStore(Config(4096));
    ASSERT_NE(first, nullptr);
    std::ofstream{directory_ / "nds_wrapper.conf"}
        << "read_batch_size_kb = 4\n"
        << "write_batch_size_kb = 4\n"
        << "value_size_kb_nsid11 = 8\n";
    auto second = std::make_unique<Store>();
    EXPECT_TRUE(second->Setup(Config(8192)).Failure());
}

TEST_F(NdsStoreTest, DirectOnlyRejectsMisalignedGeometryAtSetup)
{
    std::ofstream{directory_ / "nds_wrapper.conf"}
        << "read_batch_size_kb = 4\n"
        << "write_batch_size_kb = 4\n"
        << "value_size_kb_nsid11 = 8\n";
    auto store = std::make_unique<Store>();
    EXPECT_TRUE(store->Setup(Config(6000, "direct_only")).Failure());
}

}  // namespace
}  // namespace UC::Nds::Test
