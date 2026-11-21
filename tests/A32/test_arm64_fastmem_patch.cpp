/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/fastmem.h"

using namespace Dynarmic::Backend::Arm64;

namespace {

FastmemPatchInfo MakeInfo(unsigned tag) {
    return FastmemPatchInfo{
        .marker = {Dynarmic::IR::LocationDescriptor{tag}, tag},
        .fc = {.call_pc = tag},
        .recompile = (tag & 1) != 0,
    };
}

}  // namespace

TEST_CASE("fastmem patch table: find", "[fastmem_patch]") {
    FastmemPatchTable source;
    CHECK(FastmemPatchList{source.Entries()}.Find(0) == nullptr);

    source.Add(8, MakeInfo(1));
    source.Add(40, MakeInfo(2));
    source.Add(96, MakeInfo(3));
    const FastmemPatchList table{source.Entries()};

    const FastmemPatchInfo* const hit = table.Find(40);
    REQUIRE(hit != nullptr);
    CHECK(hit->fc.call_pc == 2);
    CHECK(std::get<1>(hit->marker) == 2);
    CHECK_FALSE(hit->recompile);

    // The first and last entries are found too.
    REQUIRE(table.Find(8) != nullptr);
    CHECK(table.Find(8)->fc.call_pc == 1);
    CHECK(table.Find(8)->recompile);
    REQUIRE(table.Find(96) != nullptr);
    CHECK(table.Find(96)->fc.call_pc == 3);

    CHECK(table.Find(41) == nullptr);
    CHECK(table.Find(0) == nullptr);
    CHECK(table.Find(200) == nullptr);
    CHECK(table.Find(7) == nullptr);
    CHECK(table.Find(97) == nullptr);
}

TEST_CASE("fastmem patch table: stored list", "[fastmem_patch]") {
    FastmemPatchTable source;
    source.Add(4, MakeInfo(10));
    source.Add(12, MakeInfo(11));

    BlockListArena arena;
    const FastmemPatchList stored{arena.Store(source.Entries())};

    // The stored list doesn't follow the table it was copied from.
    source.Clear();
    source.Add(100, MakeInfo(99));
    CHECK(FastmemPatchList{source.Entries()}.Find(4) == nullptr);
    CHECK(stored.Find(100) == nullptr);
    REQUIRE(stored.Find(4) != nullptr);
    CHECK(stored.Find(4)->fc.call_pc == 10);
    REQUIRE(stored.Find(12) != nullptr);
    CHECK(stored.Find(12)->fc.call_pc == 11);

    // A list larger than a chunk is stored whole, and lists stored before it stay valid.
    FastmemPatchTable large;
    for (unsigned i = 0; i < 4096; ++i) {
        large.Add(4 * i, MakeInfo(i));
    }
    const FastmemPatchList stored_large{arena.Store(large.Entries())};
    large.Clear();
    REQUIRE(stored_large.entries.size() == 4096);
    REQUIRE(stored_large.Find(4 * 4095) != nullptr);
    CHECK(stored_large.Find(4 * 4095)->fc.call_pc == 4095);
    REQUIRE(stored.Find(12) != nullptr);
    CHECK(stored.Find(12)->fc.call_pc == 11);

    CHECK(arena.Store(large.Entries()).empty());
}
