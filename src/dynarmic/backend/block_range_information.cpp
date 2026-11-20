/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <boost/icl/discrete_interval.hpp>
#include <boost/icl/interval_set.hpp>
#include <mcl/stdint.hpp>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

namespace Dynarmic::Backend {

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::AddRange(boost::icl::discrete_interval<ProgramCounterType> range, IR::LocationDescriptor location) {
    if (boost::icl::is_empty(range)) {
        return;
    }
    const ProgramCounterType first = boost::icl::first(range);
    const ProgramCounterType last = boost::icl::last(range);
    for (ProgramCounterType page = first >> page_bits;; ++page) {
        pages[page].push_back({first, last, location});
        if (page == last >> page_bits) {
            break;
        }
    }
}

template<typename ProgramCounterType>
void BlockRangeInformation<ProgramCounterType>::ClearCache() {
    pages.clear();
}

template<typename ProgramCounterType>
tsl::robin_set<IR::LocationDescriptor> BlockRangeInformation<ProgramCounterType>::InvalidateRanges(const boost::icl::interval_set<ProgramCounterType>& ranges) {
    tsl::robin_set<IR::LocationDescriptor> erase_locations;
    for (const auto& invalidate_interval : ranges) {
        if (boost::icl::is_empty(invalidate_interval)) {
            continue;
        }
        const ProgramCounterType first = boost::icl::first(invalidate_interval);
        const ProgramCounterType last = boost::icl::last(invalidate_interval);
        const auto collect = [&](const std::vector<Entry>& entries) {
            for (const Entry& entry : entries) {
                if (entry.first <= last && first <= entry.last) {
                    erase_locations.insert(entry.location);
                }
            }
        };

        const ProgramCounterType first_page = first >> page_bits;
        const ProgramCounterType last_page = last >> page_bits;
        if (last_page - first_page >= pages.size()) {
            // More pages in the range than pages in use: walking the map is cheaper than looking up every page.
            for (const auto& [page, entries] : pages) {
                if (first_page <= page && page <= last_page) {
                    collect(entries);
                }
            }
            continue;
        }
        for (ProgramCounterType page = first_page;; ++page) {
            if (const auto iter = pages.find(page); iter != pages.end()) {
                collect(iter->second);
            }
            if (page == last_page) {
                break;
            }
        }
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
