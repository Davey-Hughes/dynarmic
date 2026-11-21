/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <mcl/assert.hpp>
#include <mcl/hash/xmrx.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_set.h>

#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

using DoNotFastmemMarker = std::tuple<IR::LocationDescriptor, unsigned>;

struct DoNotFastmemMarkerHash {
    size_t operator()(const DoNotFastmemMarker& value) const {
        return mcl::hash::xmrx(std::get<0>(value).Value() ^ static_cast<u64>(std::get<1>(value)));
    }
};

struct FastmemPatchInfo {
    DoNotFastmemMarker marker;
    FakeCall fc;
    bool recompile;
};

using FastmemPatchEntry = std::pair<std::ptrdiff_t, FastmemPatchInfo>;

// A block's fastmem accesses by code offset, in increasing order. It is only searched when an access faults.
struct FastmemPatchList {
    std::span<const FastmemPatchEntry> entries;

    const FastmemPatchInfo* Find(std::ptrdiff_t offset) const {
        const auto it = std::lower_bound(entries.begin(), entries.end(), offset, [](const auto& entry, std::ptrdiff_t value) {
            return entry.first < value;
        });
        return it != entries.end() && it->first == offset ? &it->second : nullptr;
    }
};

// Collects a block's fastmem accesses while it is emitted. Add() appends, which the emitters do in increasing
// offset order (their deferred emits run in the order the accesses were emitted).
class FastmemPatchTable {
public:
    void Add(std::ptrdiff_t offset, FastmemPatchInfo info) {
        ASSERT(entries.empty() || entries.back().first < offset);
        entries.emplace_back(offset, info);
    }

    void Clear() {
        entries.clear();
    }

    std::span<const FastmemPatchEntry> Entries() const {
        return entries;
    }

private:
    std::vector<FastmemPatchEntry> entries;
};

class FastmemManager {
public:
    explicit FastmemManager(ExceptionHandler& eh)
            : exception_handler(eh) {}

    bool SupportsFastmem() const {
        return exception_handler.SupportsFastmem();
    }

    bool ShouldFastmem(DoNotFastmemMarker marker) const {
        return do_not_fastmem.count(marker) == 0;
    }

    void MarkDoNotFastmem(DoNotFastmemMarker marker) {
        do_not_fastmem.insert(marker);
    }

private:
    ExceptionHandler& exception_handler;
    tsl::robin_set<DoNotFastmemMarker, DoNotFastmemMarkerHash> do_not_fastmem;
};

}  // namespace Dynarmic::Backend::Arm64
