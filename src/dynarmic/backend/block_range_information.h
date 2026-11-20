/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <vector>

#include <boost/icl/discrete_interval.hpp>
#include <boost/icl/interval_set.hpp>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location);
    void ClearCache();
    tsl::robin_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges);

private:
    struct Entry {
        ProgramCounterType first;
        ProgramCounterType last;
        IR::LocationDescriptor location;
    };
    static constexpr int page_bits = 12;

    // Each block's range, filed under every guest page it touches. Invalidation leaves entries in place,
    // and a block added again gets another entry.
    tsl::robin_map<ProgramCounterType, std::vector<Entry>> pages;
};

}  // namespace Dynarmic::Backend
