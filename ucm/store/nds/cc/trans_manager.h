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

#include <memory>
#include "nds_runtime.h"
#include "template/task_wrapper.h"
#include "trans_task.h"

namespace UC::Nds {

class TransManager : public Detail::TaskWrapper<Task, Detail::TaskHandle> {
    class WorkerPool;

public:
    TransManager();
    ~TransManager();
    Status Setup(NdsRuntime& runtime, size_t storeId, size_t loadWorkers, size_t dumpWorkers,
                 size_t queueDepth, size_t timeoutMs);
    void Close();

protected:
    Status FailureStatus(const TaskPtr& task) const override { return task->FailureStatus(); }
    void Dispatch(TaskPtr task, WaiterPtr waiter) override;

private:
    std::unique_ptr<WorkerPool> loadPool_;
    std::unique_ptr<WorkerPool> dumpPool_;
};

}  // namespace UC::Nds
