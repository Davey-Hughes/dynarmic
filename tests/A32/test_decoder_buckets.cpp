/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <cstring>
#include <optional>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/frontend/A32/decoder/arm.h"
#include "dynarmic/frontend/A32/decoder/arm_combined.h"
#include "dynarmic/frontend/A32/decoder/asimd.h"
#include "dynarmic/frontend/A32/decoder/vfp.h"
#include "dynarmic/frontend/A32/translate/impl/a32_translate_impl.h"

using namespace Dynarmic;
using A32::VFPMatcher;  // Used by DYNARMIC_DECODER_GET_MATCHER.
using V = A32::TranslatorVisitor;

namespace {

struct Match {
    const char* name = nullptr;
    u32 mask = 0;
    u32 expected = 0;

    bool operator==(const Match& other) const {
        const bool same_name = (name == nullptr || other.name == nullptr) ? name == other.name : std::strcmp(name, other.name) == 0;
        return same_name && mask == other.mask && expected == other.expected;
    }
};

Match ToMatch(const A32::VFPMatcher<V>* matcher) {
    return matcher ? Match{matcher->GetName(), matcher->GetMask(), matcher->GetExpected()} : Match{};
}

// The pre-bucketing lookups: a linear search over the matcher list.
struct LinearVFP {
    std::vector<A32::VFPMatcher<V>> unconditional;
    std::vector<A32::VFPMatcher<V>> conditional;

    LinearVFP() {
        std::vector<A32::VFPMatcher<V>> list = {
#define INST(fn, name, bitstring) DYNARMIC_DECODER_GET_MATCHER(VFPMatcher, fn, name, Decoder::detail::StringToArray<32>(bitstring)),
#include "dynarmic/frontend/A32/decoder/vfp.inc"
#undef INST
        };
        const auto division = std::stable_partition(list.begin(), list.end(), [](const auto& matcher) {
            return (matcher.GetMask() & 0xF0000000) == 0xF0000000;
        });
        unconditional.assign(list.begin(), division);
        conditional.assign(division, list.end());
    }

    Match Decode(u32 instruction) const {
        const auto& table = (instruction & 0xF0000000) == 0xF0000000 ? unconditional : conditional;
        const auto iter = std::find_if(table.begin(), table.end(), [instruction](const auto& matcher) { return matcher.Matches(instruction); });
        return iter != table.end() ? ToMatch(&*iter) : Match{};
    }
};

// Every bucket index, each with edge fills and random fills of all bits outside the index
// (bits [31:28], [19:8] and [3:0]), so both the unconditional and conditional VFP spaces are hit.
template<typename F>
void ForEachSweepWord(F&& f) {
    constexpr u32 outside = 0xF00FFF0F;
    std::mt19937 rng{0x5eed};
    for (u32 index = 0; index < 0x1000; ++index) {
        const u32 base = ((index & 0xFF0) << 16) | ((index & 0x00F) << 4);
        f(base);
        f(base | outside);
        f(base | 0xF0000000);
        f(base | 0xE0000000);
        for (int k = 0; k < 4096; ++k) {
            f(base | (static_cast<u32>(rng()) & outside));
        }
    }
}

}  // namespace

TEST_CASE("bucketed VFP/ASIMD decode matches linear search", "[decoder]") {
    const LinearVFP linear_vfp;
    const auto linear_asimd = A32::GetASIMDDecodeTable<V>();

    const auto vfp_tables = A32::GetVFPDecodeTable<V>();
    REQUIRE(!vfp_tables.matchers.empty());
    size_t max_bucket = 0;
    for (const auto& bucket : vfp_tables.unconditional) {
        max_bucket = std::max(max_bucket, bucket.size());
    }
    for (const auto& bucket : vfp_tables.conditional) {
        max_bucket = std::max(max_bucket, bucket.size());
    }
    CHECK(max_bucket < vfp_tables.matchers.size());

    size_t vfp_hits = 0;
    size_t asimd_hits = 0;
    size_t differing = 0;
    ForEachSweepWord([&](u32 word) {
        const auto vfp = A32::DecodeVFP<V>(word);
        const Match vfp_got = vfp ? ToMatch(&vfp->get()) : Match{};
        const Match vfp_want = linear_vfp.Decode(word);

        const auto asimd = A32::DecodeASIMD<V>(word);
        const Match asimd_got = asimd ? Match{asimd->get().GetName(), asimd->get().GetMask(), asimd->get().GetExpected()} : Match{};
        const auto asimd_iter = std::find_if(linear_asimd.begin(), linear_asimd.end(), [word](const auto& m) { return m.Matches(word); });
        const Match asimd_want = asimd_iter != linear_asimd.end() ? Match{asimd_iter->GetName(), asimd_iter->GetMask(), asimd_iter->GetExpected()} : Match{};

        vfp_hits += vfp_want.name != nullptr;
        asimd_hits += asimd_want.name != nullptr;
        if (!(vfp_got == vfp_want) || !(asimd_got == asimd_want)) {
            if (++differing <= 8) {
                UNSCOPED_INFO("word " << std::hex << word);
            }
        }
    });

    CHECK(differing == 0);
    CHECK(vfp_hits > 0);
    CHECK(asimd_hits > 0);
}

TEST_CASE("combined ARM decode matches DecodeVFP, DecodeASIMD then DecodeArm", "[decoder]") {
    const auto table = A32::BuildArmCombinedDecodeTable<V>();
    REQUIRE(table.entries.size() == table.names.size());

    size_t hits = 0;
    size_t differing = 0;
    ForEachSweepWord([&](u32 word) {
        Match want{};
        if (const auto vfp = A32::DecodeVFP<V>(word)) {
            want = ToMatch(&vfp->get());
        } else if (const auto asimd = A32::DecodeASIMD<V>(word)) {
            want = ToMatch(&asimd->get());
        } else if (const auto arm = A32::DecodeArm<V>(word)) {
            want = ToMatch(&arm->get());
        }

        const auto& entry = table.Decode(word);
        const char* const name = table.names[&entry - table.entries.data()];
        const Match got = name ? Match{name, entry.mask, entry.expected} : Match{};

        hits += want.name != nullptr;
        if (!(got == want)) {
            if (++differing <= 8) {
                UNSCOPED_INFO("word " << std::hex << word);
            }
        }
    });

    CHECK(differing == 0);
    CHECK(hits > 0);
}
