/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include <catch2/catch_test_macros.hpp>

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
    FastmemPatchTable table;
    CHECK(table.Find(0) == nullptr);

    table.Add(8, MakeInfo(1));
    table.Add(40, MakeInfo(2));
    table.Add(96, MakeInfo(3));

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

TEST_CASE("fastmem patch table: assign and clear", "[fastmem_patch]") {
    FastmemPatchTable source;
    source.Add(4, MakeInfo(10));
    source.Add(12, MakeInfo(11));

    FastmemPatchTable copy;
    copy.Add(100, MakeInfo(99));
    copy.Assign(source);
    CHECK(copy.Find(100) == nullptr);
    REQUIRE(copy.Find(12) != nullptr);
    CHECK(copy.Find(12)->fc.call_pc == 11);

    source.Clear();
    CHECK(source.Find(4) == nullptr);
    CHECK(copy.Find(4) != nullptr);
}
