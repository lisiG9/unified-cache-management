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
#include "nds_config.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>

namespace UC::Nds {
namespace {

std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { return {}; }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool ParseKb(const std::string& text, size_t& result)
{
    if (text.empty() || text.front() == '-') { return false; }
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || value == 0 ||
        value > std::numeric_limits<size_t>::max() / 1024) {
        return false;
    }
    result = static_cast<size_t>(value) * 1024;
    return true;
}

bool ParseMb(const std::string& text, size_t& result)
{
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || !std::isfinite(value) ||
        value <= 0) {
        return false;
    }
    const auto bytes = value * 1024.0 * 1024.0;
    if (bytes >= 9007199254740992.0) { return false; }
    result = static_cast<size_t>(bytes + 0.5);
    return result != 0;
}

}  // namespace

size_t AlignUp(size_t value, size_t alignment)
{
    if (alignment == 0 || value > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        return 0;
    }
    return (value + alignment - 1) / alignment * alignment;
}

Expected<Geometry> LoadGeometry(const std::string& ndsConfigPath, uint32_t nsid,
                                size_t shardSize)
{
    if (ndsConfigPath.empty() || shardSize == 0) {
        return Status::InvalidParam("NDS config path and shard size are required");
    }
    Geometry geometry;
    size_t globalValueSize = geometry.valueSize;
    std::optional<size_t> nsidValueSize;
    std::filesystem::path wrapperPath{ndsConfigPath};
    wrapperPath.replace_filename("nds_wrapper.conf");
    std::ifstream input{wrapperPath};
    std::string line;
    size_t lineNumber = 0;
    const auto nsidKey = "value_size_kb_nsid" + std::to_string(nsid);
    while (std::getline(input, line)) {
        ++lineNumber;
        line = Trim(line);
        if (line.empty() || line.front() == '#') { continue; }
        const auto separator = line.find('=');
        if (separator == std::string::npos) { continue; }
        const auto key = Trim(line.substr(0, separator));
        const auto value = Trim(line.substr(separator + 1));
        size_t parsed = 0;
        bool recognized = true;
        bool parsedOk = true;
        if (key == "read_batch_size_kb") {
            parsedOk = ParseKb(value, parsed);
            if (parsedOk) { geometry.readBatch = parsed; }
        } else if (key == "write_batch_size_kb") {
            parsedOk = ParseKb(value, parsed);
            if (parsedOk) { geometry.writeBatch = parsed; }
        } else if (key == "value_size_kb") {
            parsedOk = ParseKb(value, parsed);
            if (parsedOk) { globalValueSize = parsed; }
        } else if (key == "value_size_mb") {
            parsedOk = ParseMb(value, parsed);
            if (parsedOk) { globalValueSize = parsed; }
        } else if (key == nsidKey) {
            parsedOk = ParseKb(value, parsed);
            if (parsedOk) { nsidValueSize = parsed; }
        } else {
            recognized = false;
        }
        if (recognized && !parsedOk) {
            return Status::InvalidParam("invalid NDS wrapper value at {}:{}: {}", wrapperPath.string(),
                                        lineNumber, line);
        }
    }
    if (input.bad()) { return Status::Error("failed to read " + wrapperPath.string()); }
    geometry.valueSize = nsidValueSize.value_or(globalValueSize);
    const auto common = std::gcd(geometry.readBatch, geometry.writeBatch);
    if (common == 0 || geometry.readBatch > std::numeric_limits<size_t>::max() /
                                             (geometry.writeBatch / common)) {
        return Status::InvalidParam("invalid NDS read/write batch geometry");
    }
    geometry.alignment = geometry.readBatch / common * geometry.writeBatch;
    geometry.stagingValueSize = AlignUp(shardSize, geometry.alignment);
    if (geometry.stagingValueSize == 0) {
        return Status::InvalidParam("NDS shard size({}) overflows batch alignment({})", shardSize,
                                    geometry.alignment);
    }
    return std::move(geometry);
}

}  // namespace UC::Nds
