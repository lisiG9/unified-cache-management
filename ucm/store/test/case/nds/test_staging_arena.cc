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
#include <cstdint>
#include "nds/cc/staging_arena.h"

TEST(NdsStagingArenaTest, LeaseReusesAndCoalescesReleasedRange)
{
    UC::Nds::StagingArena arena;
    ASSERT_TRUE(arena.Setup(8192, 4096).Success());
    ASSERT_EQ(reinterpret_cast<uintptr_t>(arena.Base()) % 4096, 0);
    ASSERT_EQ(arena.MappedBytes(), 8192 + 4096);
    void* firstAddr = nullptr;
    {
        auto first = arena.Acquire(4096);
        ASSERT_TRUE(first);
        auto second = arena.Acquire(4096);
        ASSERT_TRUE(second);
        firstAddr = first.Value().Data();
        EXPECT_NE(first.Value().Data(), second.Value().Data());
    }
    auto whole = arena.Acquire(8192);
    ASSERT_TRUE(whole);
    EXPECT_EQ(whole.Value().Data(), firstAddr);
    EXPECT_EQ(arena.ControlData(), static_cast<uint8_t*>(arena.Base()) + 8192);
}

TEST(NdsStagingArenaTest, RejectsOversizedLease)
{
    UC::Nds::StagingArena arena;
    ASSERT_TRUE(arena.Setup(4096, 4096).Success());
    EXPECT_FALSE(arena.Acquire(8192));
    EXPECT_FALSE(arena.Acquire(0));
}
