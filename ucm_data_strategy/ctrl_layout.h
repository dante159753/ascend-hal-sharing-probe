/**
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
 * */
#pragma once

#include <atomic>
#include <cstddef>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include "logger/logger.h"
#include "status/status.h"

namespace UC::Cache2 {

inline constexpr size_t kInvalid{std::numeric_limits<size_t>::max()};

// Demo control transport: publish to stdout, receive from stdin.
class CtrlLayout {
public:
    struct RankDataDesc {
        std::atomic<size_t> handle{kInvalid};

        RankDataDesc() = default;
        RankDataDesc(const RankDataDesc& o) : handle(o.handle.load(std::memory_order_relaxed)) {}
        RankDataDesc& operator=(const RankDataDesc& o)
        {
            handle.store(o.handle.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
    };

    CtrlLayout(size_t rankCount, size_t slotsPerRank)
        : rankCount_(rankCount), slotCount_(rankCount * slotsPerRank)
    {}

    size_t SlotCount() const { return slotCount_; }

    Status SetRankDesc(size_t rank, const RankDataDesc& desc)
    {
        if (rank >= rankCount_) { return Status::InvalidParam("rank out of range"); }
        const size_t handle = desc.handle.load(std::memory_order_relaxed);
        UC_INFO("SetRankDesc: rank={} handle={}; copy the DESC line to peers", rank, handle);
        std::cout << "DESC " << rank << ' ' << handle << std::endl;
        return Status::OK();
    }

    Expected<RankDataDesc> GetRankDesc(size_t rank) const
    {
        if (rank >= rankCount_) { return Status::InvalidParam("rank out of range"); }
        for (;;) {
            UC_INFO("GetRankDesc: waiting for rank={}; paste: DESC {} <decimal_handle>", rank,
                    rank);
            std::string line;
            if (!std::getline(std::cin, line)) {
                // Setup catches this and performs its original resource cleanup.
                throw std::runtime_error("stdin closed while waiting for rank descriptor");
            }
            std::istringstream input(line);
            std::string tag;
            std::string handleText;
            std::string extra;
            size_t peerRank = kInvalid;
            if (!(input >> tag >> peerRank >> handleText) || tag != "DESC" || peerRank != rank ||
                (input >> extra) || handleText.find_first_not_of("0123456789") != std::string::npos) {
                UC_WARN("Invalid descriptor; expected rank={}", rank);
                continue;
            }
            size_t handle;
            try {
                handle = std::stoull(handleText);
            } catch (const std::exception&) {
                UC_WARN("Invalid handle: {}", handleText);
                continue;
            }
            if (handle == kInvalid) {
                UC_WARN("Descriptor contains the control layout's unset handle value");
                continue;
            }
            RankDataDesc result;
            result.handle.store(handle, std::memory_order_relaxed);
            return result;
        }
    }

private:
    size_t rankCount_;
    size_t slotCount_;
};

}  // namespace UC::Cache2
