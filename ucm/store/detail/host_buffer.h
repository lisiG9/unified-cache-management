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

#include <cstddef>
#include <vector>
#include "status/status.h"

namespace UC::Detail {

struct HostBufferSpan {
    void* addr{nullptr};
    size_t mappedBytes{0};
    size_t slotBytes{0};
    size_t slotCount{0};
};

class HostBufferProvider {
public:
    virtual ~HostBufferProvider() = default;
    virtual Expected<std::vector<HostBufferSpan>> HostBuffers() = 0;
};

class HostBufferConsumer {
public:
    virtual ~HostBufferConsumer() = default;
    virtual Status RegisterHostBuffers(const std::vector<HostBufferSpan>& spans) = 0;
    virtual Status FinalizeRegistration() = 0;
    /* Drains this consumer before any provider mapping is destroyed. */
    virtual void ShutdownHostBuffers() = 0;
};

}  // namespace UC::Detail
