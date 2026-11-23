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
#include "nds/cc/key_encoder.h"

TEST(NdsKeyEncoderTest, MatchesIndependentSha256Vector)
{
    UC::Detail::BlockId block{};
    for (size_t i = 0; i < block.size(); ++i) { block[i] = static_cast<std::byte>(i); }
    const auto key = UC::Nds::KeyEncoder::Encode(block, 0);
    EXPECT_EQ(key.low, 0x3078c1880648ae6aULL);
    EXPECT_EQ(key.high, 0x9cb2);
    EXPECT_EQ(key, UC::Nds::KeyEncoder::Encode(block, 0));
    EXPECT_NE(key, UC::Nds::KeyEncoder::Encode(block, 1));
}

TEST(NdsKeyEncoderTest, OwnerChangesKey)
{
    UC::Detail::BlockId first{};
    UC::Detail::BlockId second{};
    second[15] = std::byte{1};
    EXPECT_NE(UC::Nds::KeyEncoder::Encode(first, 0), UC::Nds::KeyEncoder::Encode(second, 0));
}
