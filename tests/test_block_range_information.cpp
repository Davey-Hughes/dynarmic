/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include <random>
#include <set>
#include <utility>

#include <boost/icl/interval_map.hpp>
#include <boost/icl/interval_set.hpp>
#include <catch2/catch_test_macros.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/backend/block_range_information.h"

using namespace Dynarmic;

namespace {

// The interval_map that BlockRangeInformation used before, as the reference.
template<typename PC>
struct ReferenceRanges {
    boost::icl::interval_map<PC, std::set<IR::LocationDescriptor>> map;

    void Add(boost::icl::discrete_interval<PC> range, IR::LocationDescriptor location) {
        map.add(std::make_pair(range, std::set<IR::LocationDescriptor>{location}));
    }

    std::set<IR::LocationDescriptor> Invalidate(const boost::icl::interval_set<PC>& ranges) {
        std::set<IR::LocationDescriptor> out;
        for (const auto& interval : ranges) {
            const auto pair = map.equal_range(interval);
            for (auto it = pair.first; it != pair.second; ++it) {
                out.insert(it->second.begin(), it->second.end());
            }
        }
        return out;
    }
};

template<typename PC>
void CompareWithReference(u64 seed, PC top) {
    std::mt19937_64 rng{seed};
    const auto below = [&](u64 n) { return static_cast<PC>(rng() % n); };

    Backend::BlockRangeInformation<PC> ranges;
    ReferenceRanges<PC> reference;
    std::vector<IR::LocationDescriptor> added;
    // Blocks cluster in a few 64 KiB windows, one at the very top of the address space.
    const PC windows[] = {0, 0x10000, 0x7f0000, static_cast<PC>(top - 0xffff)};

    for (int step = 0; step < 4000; ++step) {
        const u64 kind = rng() % 100;
        if (kind < 80) {
            const PC base = windows[rng() % 4];
            const PC start = static_cast<PC>(base + below((0x10000 - 0x400) / 2) * 2);  // stays under top
            const PC length = static_cast<PC>(rng() % 8 == 0 ? 0 : 2 + below(0x400));  // 0: an empty range
            const auto range = boost::icl::discrete_interval<PC>::closed(start, static_cast<PC>(start + length - 1));
            // Now and then the same location again, as a recompile after an invalidation adds it.
            const IR::LocationDescriptor location = !added.empty() && rng() % 10 == 0 ? added[rng() % added.size()] : IR::LocationDescriptor{rng()};
            if (length == 0) {
                ranges.AddRange(boost::icl::discrete_interval<PC>::right_open(start, start), location);
                reference.Add(boost::icl::discrete_interval<PC>::right_open(start, start), location);
                continue;
            }
            ranges.AddRange(range, location);
            reference.Add(range, location);
            added.push_back(location);
        } else if (kind < 99) {
            boost::icl::interval_set<PC> invalidate;
            const int count = 1 + static_cast<int>(rng() % 3);
            for (int i = 0; i < count; ++i) {
                const u64 size_kind = rng() % 4;
                if (size_kind == 3 && rng() % 4 == 0) {
                    invalidate.add(boost::icl::discrete_interval<PC>::closed(0, top));
                    continue;
                }
                const PC start = static_cast<PC>(windows[rng() % 4] + below(0x10000));
                const u64 size = size_kind == 0 ? 1 + rng() % 4 : size_kind == 1 ? 1 + rng() % 0x2000 : 1 + rng() % 0x100000;
                const PC last = static_cast<u64>(top - start) < size ? top : static_cast<PC>(start + size - 1);
                invalidate.add(boost::icl::discrete_interval<PC>::closed(start, last));
            }
            const auto got = ranges.InvalidateRanges(invalidate);
            const std::set<IR::LocationDescriptor> got_sorted(got.begin(), got.end());
            REQUIRE(got_sorted == reference.Invalidate(invalidate));
        } else {
            ranges.ClearCache();
            reference.map.clear();
            added.clear();
        }
    }
}

}  // namespace

TEST_CASE("BlockRangeInformation matches the interval map it replaced", "[backend]") {
    for (u64 seed = 1; seed <= 8; ++seed) {
        CompareWithReference<u32>(seed, 0xffffffff);
        CompareWithReference<u64>(seed, ~u64{0});
    }
}
