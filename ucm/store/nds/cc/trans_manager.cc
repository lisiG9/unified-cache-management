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
#include "trans_manager.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "logger/logger.h"
#include "template/hashset.h"

namespace UC::Nds {

class TransManager::WorkerPool {
    using TaskPair = std::pair<TaskPtr, WaiterPtr>;

public:
    WorkerPool(NdsRuntime& runtime, size_t storeId, bool load, size_t queueDepth,
               TaskIdSet* failureSet)
        : runtime_{runtime}, storeId_{storeId}, load_{load}, queueDepth_{queueDepth},
          failureSet_{failureSet}
    {
    }
    ~WorkerPool() { Close(); }

    Status Start(size_t workers)
    {
        try {
            for (size_t i = 0; i < workers; ++i) {
                threads_.emplace_back([this] { Run(); });
            }
        } catch (const std::exception& error) {
            Close();
            return Status::Error(error.what());
        }
        return Status::OK();
    }

    void Submit(const TaskPtr& task, const WaiterPtr& waiter)
    {
        waiter->Up();
        bool closing = false;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (!closing_ && queue_.size() < queueDepth_) {
                queue_.emplace_back(task, waiter);
                available_.notify_one();
                return;
            }
            closing = closing_;
        }
        const auto status = closing ? Status::Error("NDS worker is closing") : Status::NoSpace();
        task->Fail(status);
        failureSet_->Insert(task->id);
        waiter->Done();
    }

    void Close()
    {
        {
            std::lock_guard<std::mutex> lock{mutex_};
            if (closing_) { return; }
            closing_ = true;
            available_.notify_all();
        }
        for (auto& thread : threads_) {
            if (thread.joinable()) { thread.join(); }
        }
        threads_.clear();
        std::deque<TaskPair> pending;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            pending.swap(queue_);
        }
        for (auto& [task, waiter] : pending) {
            task->Fail(Status::Error("NDS worker closed before dispatch"));
            failureSet_->Insert(task->id);
            waiter->Done();
        }
    }

private:
    void Run()
    {
        for (;;) {
            TaskPair pair;
            {
                std::unique_lock<std::mutex> lock{mutex_};
                available_.wait(lock, [this] { return closing_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (closing_) { return; }
                    continue;
                }
                pair = std::move(queue_.front());
                queue_.pop_front();
            }
            auto& [task, waiter] = pair;
            auto status = Status::OK();
            try {
                std::vector<NdsKey> keys;
                std::vector<void*> addrs;
                keys.reserve(task->desc.size());
                addrs.reserve(task->desc.size());
                for (const auto& shard : task->desc) {
                    keys.push_back(KeyEncoder::Encode(shard.owner, shard.index));
                    addrs.push_back(shard.addrs.front());
                }
                status = runtime_.Transfer(storeId_, load_, keys, addrs);
            } catch (const std::exception& error) {
                status = Status::Error(error.what());
            } catch (...) {
                status = Status::Error("unknown NDS worker exception");
            }
            if (status.Failure()) {
                task->Fail(status);
                failureSet_->Insert(task->id);
                UC_ERROR("NDS task({}) {} failed: {}.", task->id, load_ ? "load" : "dump", status);
            }
            waiter->Done();
        }
    }

    NdsRuntime& runtime_;
    size_t storeId_{0};
    bool load_{false};
    size_t queueDepth_{0};
    TaskIdSet* failureSet_{nullptr};
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<TaskPair> queue_;
    std::vector<std::thread> threads_;
    bool closing_{false};
};

TransManager::TransManager() = default;
TransManager::~TransManager() { Close(); }

Status TransManager::Setup(NdsRuntime& runtime, size_t storeId, size_t loadWorkers,
                           size_t dumpWorkers, size_t queueDepth, size_t timeoutMs)
{
    if (loadWorkers == 0 || dumpWorkers == 0 || queueDepth == 0) {
        return Status::InvalidParam("NDS workers and queue depth must be positive");
    }
    timeoutMs_ = timeoutMs;
    loadPool_ = std::make_unique<WorkerPool>(runtime, storeId, true, queueDepth, &failureSet_);
    dumpPool_ = std::make_unique<WorkerPool>(runtime, storeId, false, queueDepth, &failureSet_);
    auto status = loadPool_->Start(loadWorkers);
    if (status.Failure()) { return status; }
    return dumpPool_->Start(dumpWorkers);
}

void TransManager::Close()
{
    if (loadPool_) { loadPool_->Close(); }
    if (dumpPool_) { dumpPool_->Close(); }
}

void TransManager::Dispatch(TaskPtr task, WaiterPtr waiter)
{
    if (task->type == Task::Type::Load) {
        loadPool_->Submit(task, waiter);
    } else {
        dumpPool_->Submit(task, waiter);
    }
}

}  // namespace UC::Nds
