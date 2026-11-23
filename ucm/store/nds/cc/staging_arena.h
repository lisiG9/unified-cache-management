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

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include "status/status.h"

namespace UC::Nds {

class StagingArena {
public:
    class Lease {
        StagingArena* arena_{nullptr};
        size_t offset_{0};
        size_t bytes_{0};
        Lease(StagingArena* arena, size_t offset, size_t bytes)
            : arena_{arena}, offset_{offset}, bytes_{bytes}
        {
        }
        friend class StagingArena;

    public:
        Lease() = default;
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        void* Data() const;
        size_t Size() const { return bytes_; }
    };

    StagingArena() = default;
    ~StagingArena();
    StagingArena(const StagingArena&) = delete;
    StagingArena& operator=(const StagingArena&) = delete;

    Status Setup(size_t capacity, size_t controlBytes);
    void Reset();
    Expected<Lease> Acquire(size_t bytes);
    void* Base() const { return base_; }
    size_t MappedBytes() const { return mappedBytes_; }
    void* ControlData() const;
    size_t ControlBytes() const { return controlBytes_; }

private:
    void Release(size_t offset, size_t bytes);

    void* base_{nullptr};
    size_t capacity_{0};
    size_t controlBytes_{0};
    size_t mappedBytes_{0};
    bool closing_{false};
    std::mutex mutex_;
    std::condition_variable available_;
    std::map<size_t, size_t> freeRanges_;
};

}  // namespace UC::Nds
