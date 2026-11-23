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
#include "staging_arena.h"
#include <cstdlib>
#include <limits>
#include "nds_config.h"

namespace UC::Nds {

StagingArena::~StagingArena() { Reset(); }

StagingArena::Lease::~Lease()
{
    if (arena_ != nullptr) { arena_->Release(offset_, bytes_); }
}

StagingArena::Lease::Lease(Lease&& other) noexcept
    : arena_{other.arena_}, offset_{other.offset_}, bytes_{other.bytes_}
{
    other.arena_ = nullptr;
    other.offset_ = 0;
    other.bytes_ = 0;
}

StagingArena::Lease& StagingArena::Lease::operator=(Lease&& other) noexcept
{
    if (this == &other) { return *this; }
    if (arena_ != nullptr) { arena_->Release(offset_, bytes_); }
    arena_ = other.arena_;
    offset_ = other.offset_;
    bytes_ = other.bytes_;
    other.arena_ = nullptr;
    other.offset_ = 0;
    other.bytes_ = 0;
    return *this;
}

void* StagingArena::Lease::Data() const
{
    if (arena_ == nullptr || arena_->base_ == nullptr) { return nullptr; }
    return static_cast<uint8_t*>(arena_->base_) + offset_;
}

Status StagingArena::Setup(size_t capacity, size_t controlBytes)
{
    if (base_ != nullptr) { return Status::Error("NDS staging arena is already initialized"); }
    capacity_ = AlignUp(capacity, 4096);
    controlBytes_ = AlignUp(controlBytes, 4096);
    if (capacity_ == 0 || controlBytes_ == 0 ||
        capacity_ > std::numeric_limits<size_t>::max() - controlBytes_) {
        return Status::InvalidParam("invalid NDS staging geometry: capacity={} control={}", capacity,
                                    controlBytes);
    }
    mappedBytes_ = capacity_ + controlBytes_;
    if (posix_memalign(&base_, 4096, mappedBytes_) != 0) {
        base_ = nullptr;
        return Status::OutOfMemory();
    }
    freeRanges_.emplace(0, capacity_);
    closing_ = false;
    return Status::OK();
}

void StagingArena::Reset()
{
    {
        std::lock_guard<std::mutex> lock{mutex_};
        closing_ = true;
        available_.notify_all();
    }
    std::free(base_);
    base_ = nullptr;
    capacity_ = 0;
    controlBytes_ = 0;
    mappedBytes_ = 0;
    freeRanges_.clear();
}

Expected<StagingArena::Lease> StagingArena::Acquire(size_t bytes)
{
    if (bytes == 0 || bytes > capacity_) {
        return Status::InvalidParam("invalid NDS staging lease bytes({})", bytes);
    }
    std::unique_lock<std::mutex> lock{mutex_};
    for (;;) {
        if (closing_) { return Status::Error("NDS staging arena is closing"); }
        for (auto it = freeRanges_.begin(); it != freeRanges_.end(); ++it) {
            if (it->second < bytes) { continue; }
            const auto offset = it->first;
            const auto remaining = it->second - bytes;
            freeRanges_.erase(it);
            if (remaining != 0) { freeRanges_.emplace(offset + bytes, remaining); }
            return Lease{this, offset, bytes};
        }
        available_.wait(lock);
    }
}

void* StagingArena::ControlData() const
{
    if (base_ == nullptr) { return nullptr; }
    return static_cast<uint8_t*>(base_) + capacity_;
}

void StagingArena::Release(size_t offset, size_t bytes)
{
    std::lock_guard<std::mutex> lock{mutex_};
    auto next = freeRanges_.lower_bound(offset);
    if (next != freeRanges_.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            offset = prev->first;
            bytes += prev->second;
            freeRanges_.erase(prev);
        }
    }
    next = freeRanges_.lower_bound(offset);
    if (next != freeRanges_.end() && offset + bytes == next->first) {
        bytes += next->second;
        freeRanges_.erase(next);
    }
    freeRanges_.emplace(offset, bytes);
    available_.notify_one();
}

}  // namespace UC::Nds
