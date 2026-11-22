/* This file is part of the dynarmic project.
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <map>
#include <vector>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/frontend/A32/decoder/arm.h"
#include "dynarmic/frontend/A32/decoder/asimd.h"
#include "dynarmic/frontend/A32/decoder/vfp.h"
#include "dynarmic/frontend/decoder/decoder_detail.h"
#include "dynarmic/frontend/decoder/matcher.h"

namespace Dynarmic::A32 {

template<typename Visitor>
using ArmDirectMatcher = Decoder::DirectMatcher<Visitor, u32>;

/**
 * Decodes an ARM instruction with one lookup to the handler that trying DecodeVFP(), DecodeASIMD()
 * and DecodeArm() in turn would give, or to arm_UDF() where none of them matches.
 */
template<typename Visitor>
struct ArmCombinedDecodeTable {
    using Handler = typename ArmDirectMatcher<Visitor>::handler_function;

    struct Entry {
        u32 mask;
        u32 expected;
        Handler handler;

        bool Matches(u32 instruction) const {
            return (instruction & mask) == expected;
        }

        typename Visitor::instruction_return_type call(Visitor& v, u32 instruction) const {
            return handler(v, instruction);
        }
    };

    /// Whether the condition field is 0b1111, then detail::ToFastLookupIndexArm().
    static size_t Key(u32 instruction) {
        return (static_cast<size_t>((instruction >> 28) == 0xF) << 12) | detail::ToFastLookupIndexArm(instruction);
    }

    const Entry& Decode(u32 instruction) const {
        const Entry* entry = &entries[lists[Key(instruction)]];
        while (!entry->Matches(instruction)) {
            ++entry;
        }
        return *entry;
    }

    /// Where each key's candidates start in entries, in the order the three decoders try them.
    /// Every list ends with an arm_UDF() entry that matches any instruction.
    std::array<u16, 0x2000> lists;
    std::vector<Entry> entries;
    /// The name of each entry's matcher; nullptr for the arm_UDF() entries that end the lists.
    std::vector<const char*> names;
};

template<typename V>
ArmCombinedDecodeTable<V> BuildArmCombinedDecodeTable() {
    using Matcher = ArmDirectMatcher<V>;

    std::vector<Matcher> vfp = {

#define INST(fn, name, bitstring) DYNARMIC_DECODER_GET_DIRECT_MATCHER(VFPMatcher, fn, name, Decoder::detail::StringToArray<32>(bitstring)),
#include "./vfp.inc"
#undef INST

    };
    std::vector<Matcher> asimd = {

#define INST(fn, name, bitstring) DYNARMIC_DECODER_GET_DIRECT_MATCHER(ASIMDMatcher, fn, name, Decoder::detail::StringToArray<32>(bitstring)),
#include "./asimd.inc"
#undef INST

    };
    std::vector<Matcher> arm = {

#define INST(fn, name, bitstring) DYNARMIC_DECODER_GET_DIRECT_MATCHER(ArmMatcher, fn, name, Decoder::detail::StringToArray<32>(bitstring)),
#include "./arm.inc"
#undef INST

    };
    SortASIMDMatchers(asimd);
    SortArmMatchers(arm);
    const Matcher udf = DYNARMIC_DECODER_GET_DIRECT_MATCHER(ArmMatcher, arm_UDF, nullptr, Decoder::detail::StringToArray<32>("--------------------------------"));

    struct Candidate {
        const Matcher* matcher;
        size_t index_mask;
        size_t index_expected;
        bool conditional;    // can be the result for an instruction whose condition field is not 0b1111
        bool unconditional;  // can be the result for an instruction whose condition field is 0b1111
    };
    std::vector<Candidate> candidates;
    const auto add = [&](const Matcher& matcher, bool conditional, bool unconditional) {
        candidates.push_back({&matcher, detail::ToFastLookupIndexArm(matcher.GetMask()), detail::ToFastLookupIndexArm(matcher.GetExpected()), conditional, unconditional});
    };
    // DecodeVFP() tries its matchers with a 0b1111 condition mask on unconditional instructions only,
    // and the others on the rest.
    for (const auto& matcher : vfp) {
        const bool is_unconditional = (matcher.GetMask() & 0xF0000000) == 0xF0000000;
        add(matcher, !is_unconditional, is_unconditional);
    }
    // DecodeASIMD() and DecodeArm() try every matcher; leave out the ones that cannot match a class.
    for (const auto* list : {&asimd, &arm}) {
        for (const auto& matcher : *list) {
            const u32 cond_mask = matcher.GetMask() >> 28;
            const u32 cond_expected = matcher.GetExpected() >> 28;
            add(matcher, cond_mask != 0xF || cond_expected != 0xF, cond_mask == cond_expected);
        }
    }

    ArmCombinedDecodeTable<V> table;
    std::map<std::vector<u16>, u16> list_offsets;
    std::vector<u16> list;
    for (size_t key = 0; key < table.lists.size(); ++key) {
        const bool unconditional = key >= 0x1000;
        const size_t index = key & 0xFFF;
        list.clear();
        for (size_t i = 0; i < candidates.size(); ++i) {
            const Candidate& candidate = candidates[i];
            if ((unconditional ? candidate.unconditional : candidate.conditional) && (index & candidate.index_mask) == candidate.index_expected) {
                list.push_back(static_cast<u16>(i));
            }
        }

        const auto [iter, inserted] = list_offsets.try_emplace(list, static_cast<u16>(table.entries.size()));
        if (inserted) {
            for (const u16 i : list) {
                const Matcher& matcher = *candidates[i].matcher;
                table.entries.push_back({matcher.GetMask(), matcher.GetExpected(), matcher.fn});
                table.names.push_back(matcher.GetName());
            }
            table.entries.push_back({udf.GetMask(), udf.GetExpected(), udf.fn});
            table.names.push_back(nullptr);
            ASSERT(table.entries.size() <= 0x10000);
        }
        table.lists[key] = iter->second;
    }
    return table;
}

template<typename V>
const ArmCombinedDecodeTable<V>& GetArmCombinedDecodeTable() {
    static const auto table = BuildArmCombinedDecodeTable<V>();
    return table;
}

}  // namespace Dynarmic::A32
