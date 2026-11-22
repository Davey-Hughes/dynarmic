/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>
#include <mcl/type_traits/is_instance_of_template.hpp>
#include <oaknut/oaknut.hpp>
#include <tsl/robin_set.h>

#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/value.h"

namespace Dynarmic::Backend::Arm64 {

class FpsrManager;
class RegAlloc;

struct HostLoc final {
    enum class Kind {
        Gpr,
        Fpr,
        Flags,
        Spill,
    } kind;
    int index;
};

enum RWType {
    Void,
    Read,
    Write,
    ReadWrite,
};

struct Argument final {
public:
    using copyable_reference = std::reference_wrapper<Argument>;

    IR::Type GetType() const;
    bool IsVoid() const { return GetType() == IR::Type::Void; }
    bool IsImmediate() const;

    bool GetImmediateU1() const;
    u8 GetImmediateU8() const;
    u16 GetImmediateU16() const;
    u32 GetImmediateU32() const;
    u64 GetImmediateU64() const;
    IR::Cond GetImmediateCond() const;
    IR::AccType GetImmediateAccType() const;

    // Only valid if not immediate
    HostLoc::Kind CurrentLocationKind() const;
    bool IsInGpr() const { return !IsImmediate() && CurrentLocationKind() == HostLoc::Kind::Gpr; }
    bool IsInFpr() const { return !IsImmediate() && CurrentLocationKind() == HostLoc::Kind::Fpr; }

private:
    friend class RegAlloc;
    explicit Argument(RegAlloc& reg_alloc)
            : reg_alloc{reg_alloc} {}

    bool allocated = false;
    RegAlloc& reg_alloc;
    IR::Value value;
    // Where GetArgumentInfo() found value, unless it is an immediate. Checked before use: value may
    // have moved since.
    HostLoc location{};
};

struct FlagsTag final {
private:
    template<typename>
    friend struct RAReg;

    explicit FlagsTag(int) {}
    int index() const { return 0; }
};

template<typename T>
struct RAReg final {
public:
    static constexpr HostLoc::Kind kind = !std::is_same_v<FlagsTag, T>
                                            ? std::is_base_of_v<oaknut::VReg, T>
                                                ? HostLoc::Kind::Fpr
                                                : HostLoc::Kind::Gpr
                                            : HostLoc::Kind::Flags;

    operator T() const { return reg.value(); }

    operator oaknut::WRegWsp() const
    requires(std::is_same_v<T, oaknut::WReg>)
    {
        return reg.value();
    }

    operator oaknut::XRegSp() const
    requires(std::is_same_v<T, oaknut::XReg>)
    {
        return reg.value();
    }

    T operator*() const { return reg.value(); }
    const T* operator->() const { return &reg.value(); }

    ~RAReg();
    RAReg(RAReg&& other)
            : reg_alloc{other.reg_alloc}
            , rw{std::exchange(other.rw, RWType::Void)}
            , read_value{std::exchange(other.read_value, {})}
            , write_value{std::exchange(other.write_value, nullptr)}
            , locked_value{std::exchange(other.locked_value, nullptr)}
            , locked_location{other.locked_location}
            , reg{std::exchange(other.reg, std::nullopt)} {
    }
    RAReg& operator=(RAReg&&) = delete;

private:
    friend class RegAlloc;
    explicit RAReg(RegAlloc& reg_alloc, RWType rw, const IR::Value& read_value, HostLoc read_location, const IR::Inst* write_value);

    RAReg(const RAReg&) = delete;
    RAReg& operator=(const RAReg&) = delete;

    void Realize();

    RegAlloc& reg_alloc;
    RWType rw;
    IR::Value read_value;
    const IR::Inst* write_value;
    // The value this keeps locked, if any, and where it was last seen. The location is checked before
    // use: realizing another RAReg of the same value can move it.
    const IR::Inst* locked_value = nullptr;
    HostLoc locked_location{};
    std::optional<T> reg;
};

struct HostLocInfo final {
    boost::container::small_vector<const IR::Inst*, 2> values;
    size_t locked = 0;
    bool realized = false;
    size_t uses_this_inst = 0;
    size_t accumulated_uses = 0;
    size_t expected_uses = 0;

    bool Contains(const IR::Inst* value) const {
        return std::find(values.begin(), values.end(), value) != values.end();
    }
    void SetupScratchLocation();
    void SetupLocation(const IR::Inst*);
    bool IsCompletelyEmpty() const {
        return values.empty() && !locked && !realized && !accumulated_uses && !expected_uses && !uses_this_inst;
    }
    bool MaybeAllocatable() const;
    bool IsOneRemainingUse() const;
    void UpdateUses();
};

// Where a value is. RegAlloc rewrites the record whenever the value is placed or moved, so a record
// that holds the current block's number is exact until the value's last use.
struct ValueLocationRecord {
    u64 block_mark = 0;
    HostLoc location{};
};

// Per-value tables RegAlloc fills for each block, indexed by IR::Inst::GetName(). The owner keeps
// them between blocks so that their storage is reused instead of allocated again for every block.
// Neither is cleared between blocks: a location record or a defined mark only counts if it holds the
// current block's number.
struct RegAllocBuffers {
    std::vector<ValueLocationRecord> value_locations;
    std::vector<u64> defined_marks;
    u64 block_count = 0;

    // The location records, also kept between blocks. Every record is empty again when a block's emit
    // finishes (RegAlloc::FinishBlock()), so the next block starts from them as they are. dirty stays set
    // when an exception abandons a block; the next RegAlloc then resets every record.
    std::array<HostLocInfo, 32> gprs;
    std::array<HostLocInfo, 32> fprs;
    HostLocInfo flags;
    std::array<HostLocInfo, SpillCount> spills;
    bool dirty = false;
};

class RegAlloc final {
public:
    using ArgumentInfo = std::array<Argument, IR::max_arg_count>;

    explicit RegAlloc(oaknut::CodeGenerator& code, FpsrManager& fpsr_manager, std::span<const int> gpr_order, std::span<const int> fpr_order, RegAllocBuffers& buffers)
            : code{code}, fpsr_manager{fpsr_manager}, gpr_order{gpr_order}, fpr_order{fpr_order}, gprs{buffers.gprs}, fprs{buffers.fprs}, flags{buffers.flags}, spills{buffers.spills}, dirty{buffers.dirty}, value_locations{buffers.value_locations}, defined_marks{buffers.defined_marks}, block_mark{++buffers.block_count} {
        if (dirty) {
            ResetLocations();
        }
        dirty = true;
    }

    ArgumentInfo GetArgumentInfo(IR::Inst* inst);
    bool WasValueDefined(IR::Inst* inst) const;

    auto ReadX(Argument& arg) { return RAReg<oaknut::XReg>{*this, RWType::Read, arg.value, arg.location, nullptr}; }
    auto ReadW(Argument& arg) { return RAReg<oaknut::WReg>{*this, RWType::Read, arg.value, arg.location, nullptr}; }

    auto ReadQ(Argument& arg) { return RAReg<oaknut::QReg>{*this, RWType::Read, arg.value, arg.location, nullptr}; }
    auto ReadD(Argument& arg) { return RAReg<oaknut::DReg>{*this, RWType::Read, arg.value, arg.location, nullptr}; }
    auto ReadS(Argument& arg) { return RAReg<oaknut::SReg>{*this, RWType::Read, arg.value, arg.location, nullptr}; }
    auto ReadH(Argument& arg) { return RAReg<oaknut::HReg>{*this, RWType::Read, arg.value, arg.location, nullptr}; }
    auto ReadB(Argument& arg) { return RAReg<oaknut::BReg>{*this, RWType::Read, arg.value, arg.location, nullptr}; }

    template<size_t size>
    auto ReadReg(Argument& arg) {
        if constexpr (size == 64) {
            return ReadX(arg);
        } else if constexpr (size == 32) {
            return ReadW(arg);
        } else {
            ASSERT_FALSE("Invalid size to ReadReg {}", size);
        }
    }

    template<size_t size>
    auto ReadVec(Argument& arg) {
        if constexpr (size == 128) {
            return ReadQ(arg);
        } else if constexpr (size == 64) {
            return ReadD(arg);
        } else if constexpr (size == 32) {
            return ReadS(arg);
        } else if constexpr (size == 16) {
            return ReadH(arg);
        } else if constexpr (size == 8) {
            return ReadB(arg);
        } else {
            ASSERT_FALSE("Invalid size to ReadVec {}", size);
        }
    }

    auto WriteX(IR::Inst* inst) { return RAReg<oaknut::XReg>{*this, RWType::Write, {}, {}, inst}; }
    auto WriteW(IR::Inst* inst) { return RAReg<oaknut::WReg>{*this, RWType::Write, {}, {}, inst}; }

    auto WriteQ(IR::Inst* inst) { return RAReg<oaknut::QReg>{*this, RWType::Write, {}, {}, inst}; }
    auto WriteD(IR::Inst* inst) { return RAReg<oaknut::DReg>{*this, RWType::Write, {}, {}, inst}; }
    auto WriteS(IR::Inst* inst) { return RAReg<oaknut::SReg>{*this, RWType::Write, {}, {}, inst}; }
    auto WriteH(IR::Inst* inst) { return RAReg<oaknut::HReg>{*this, RWType::Write, {}, {}, inst}; }
    auto WriteB(IR::Inst* inst) { return RAReg<oaknut::BReg>{*this, RWType::Write, {}, {}, inst}; }

    auto WriteFlags(IR::Inst* inst) { return RAReg<FlagsTag>{*this, RWType::Write, {}, {}, inst}; }

    template<size_t size>
    auto WriteReg(IR::Inst* inst) {
        if constexpr (size == 64) {
            return WriteX(inst);
        } else if constexpr (size == 32) {
            return WriteW(inst);
        } else {
            ASSERT_FALSE("Invalid size to WriteReg {}", size);
        }
    }

    template<size_t size>
    auto WriteVec(IR::Inst* inst) {
        if constexpr (size == 128) {
            return WriteQ(inst);
        } else if constexpr (size == 64) {
            return WriteD(inst);
        } else if constexpr (size == 32) {
            return WriteS(inst);
        } else if constexpr (size == 16) {
            return WriteH(inst);
        } else if constexpr (size == 8) {
            return WriteB(inst);
        } else {
            ASSERT_FALSE("Invalid size to WriteVec {}", size);
        }
    }

    auto ReadWriteX(Argument& arg, const IR::Inst* inst) { return RAReg<oaknut::XReg>{*this, RWType::ReadWrite, arg.value, arg.location, inst}; }
    auto ReadWriteW(Argument& arg, const IR::Inst* inst) { return RAReg<oaknut::WReg>{*this, RWType::ReadWrite, arg.value, arg.location, inst}; }

    auto ReadWriteQ(Argument& arg, const IR::Inst* inst) { return RAReg<oaknut::QReg>{*this, RWType::ReadWrite, arg.value, arg.location, inst}; }
    auto ReadWriteD(Argument& arg, const IR::Inst* inst) { return RAReg<oaknut::DReg>{*this, RWType::ReadWrite, arg.value, arg.location, inst}; }
    auto ReadWriteS(Argument& arg, const IR::Inst* inst) { return RAReg<oaknut::SReg>{*this, RWType::ReadWrite, arg.value, arg.location, inst}; }
    auto ReadWriteH(Argument& arg, const IR::Inst* inst) { return RAReg<oaknut::HReg>{*this, RWType::ReadWrite, arg.value, arg.location, inst}; }
    auto ReadWriteB(Argument& arg, const IR::Inst* inst) { return RAReg<oaknut::BReg>{*this, RWType::ReadWrite, arg.value, arg.location, inst}; }

    template<size_t size>
    auto ReadWriteReg(Argument& arg, const IR::Inst* inst) {
        if constexpr (size == 64) {
            return ReadWriteX(arg, inst);
        } else if constexpr (size == 32) {
            return ReadWriteW(arg, inst);
        } else {
            ASSERT_FALSE("Invalid size to ReadWriteReg {}", size);
        }
    }

    template<size_t size>
    auto ReadWriteVec(Argument& arg, const IR::Inst* inst) {
        if constexpr (size == 128) {
            return ReadWriteQ(arg, inst);
        } else if constexpr (size == 64) {
            return ReadWriteD(arg, inst);
        } else if constexpr (size == 32) {
            return ReadWriteS(arg, inst);
        } else if constexpr (size == 16) {
            return ReadWriteH(arg, inst);
        } else if constexpr (size == 8) {
            return ReadWriteB(arg, inst);
        } else {
            ASSERT_FALSE("Invalid size to ReadWriteVec {}", size);
        }
    }

    void PrepareForCall(std::optional<Argument::copyable_reference> arg0 = {}, std::optional<Argument::copyable_reference> arg1 = {}, std::optional<Argument::copyable_reference> arg2 = {}, std::optional<Argument::copyable_reference> arg3 = {});

    void DefineAsExisting(IR::Inst* inst, Argument& arg);
    // Places inst in the location of an already defined value without emitting code. The caller vouches for
    // what inst's readers see there (see the memory pairs in emit_arm64_memory.cpp).
    void DefineAsExisting(IR::Inst* inst, const IR::Inst* existing);
    void DefineAsRegister(IR::Inst* inst, oaknut::Reg reg);

    void ReadWriteFlags(Argument& read, IR::Inst* write);
    void SpillFlags();
    void SpillAll();

    template<typename... Ts>
    static void Realize(Ts&... rs) {
        static_assert((mcl::is_instance_of_template<RAReg, Ts>() && ...));
        (rs.Realize(), ...);
    }

    void UpdateAllUses();
    void AssertAllUnlocked() const;
    void AssertNoMoreUses() const;
    // Call when the block's code is complete: its location records are empty and can be reused.
    void FinishBlock();

    void EmitVerboseDebuggingOutput();

private:
    friend struct Argument;
    template<typename>
    friend struct RAReg;

    template<HostLoc::Kind kind>
    int GenerateImmediate(const IR::Value& value);
    template<HostLoc::Kind kind>
    int RealizeReadImpl(const IR::Value& value);
    template<HostLoc::Kind kind>
    int RealizeWriteImpl(const IR::Inst* value);
    template<HostLoc::Kind kind>
    int RealizeReadWriteImpl(const IR::Value& read_value, const IR::Inst* write_value);

    int AllocateRegister(const std::array<HostLocInfo, 32>& regs, std::span<const int> order) const;
    // Most registers these are called for are empty, so that is checked before the call.
    void SpillGpr(int index) {
        if (!gprs[index].IsCompletelyEmpty()) {
            SpillNonEmptyGpr(index);
        }
    }
    void SpillFpr(int index) {
        if (!fprs[index].IsCompletelyEmpty()) {
            SpillNonEmptyFpr(index);
        }
    }
    void SpillNonEmptyGpr(int index);
    void SpillNonEmptyFpr(int index);
    int FindFreeSpill() const;

    void LoadCopyInto(const IR::Value& value, oaknut::XReg reg);
    void LoadCopyInto(const IR::Value& value, oaknut::QReg reg);

    void AddToLocationOf(IR::Inst* inst, const IR::Inst* existing);

    // Values without a name have no record and are searched for.
    std::optional<HostLoc> ValueLocation(const IR::Inst* value) const {
        const unsigned name = value->GetName();
        if (name < value_locations.size() && value_locations[name].block_mark == block_mark) {
#ifndef MCL_IGNORE_ASSERTS
            ASSERT(ValueInfo(value_locations[name].location).Contains(value));
#endif
            return value_locations[name].location;
        }
        return FindValueLocation(value);
    }
    std::optional<HostLoc> FindValueLocation(const IR::Inst* value) const;
    void MarkDefined(const IR::Inst* value);
    void SetValueLocation(const IR::Inst* value, HostLoc host_loc);
    // Records host_loc as the location of every value it holds.
    void SetValueLocations(HostLoc host_loc);
    const HostLocInfo& ValueInfo(HostLoc host_loc) const {
        switch (host_loc.kind) {
        case HostLoc::Kind::Gpr:
            return gprs[static_cast<size_t>(host_loc.index)];
        case HostLoc::Kind::Fpr:
            return fprs[static_cast<size_t>(host_loc.index)];
        case HostLoc::Kind::Flags:
            return flags;
        case HostLoc::Kind::Spill:
            return spills[static_cast<size_t>(host_loc.index)];
        }
        ASSERT_FALSE("RegAlloc::ValueInfo: Invalid HostLoc::Kind");
    }
    HostLocInfo& ValueInfo(HostLoc host_loc) {
        return const_cast<HostLocInfo&>(std::as_const(*this).ValueInfo(host_loc));
    }
    HostLocInfo& ValueInfo(const IR::Inst* value);
    // For a caller that knows where value was last: looks it up only if it is no longer there.
    HostLocInfo& ValueInfo(const IR::Inst* value, HostLoc last_location) {
        HostLocInfo& info = ValueInfo(last_location);
        return info.Contains(value) ? info : ValueInfo(value);
    }
    // Locks value in its location, which is returned. last_location is where to look first.
    HostLoc LockValue(const IR::Inst* value, HostLoc last_location);

    oaknut::CodeGenerator& code;
    FpsrManager& fpsr_manager;
    std::span<const int> gpr_order;
    std::span<const int> fpr_order;

    // In RegAllocBuffers, kept between blocks.
    std::array<HostLocInfo, 32>& gprs;
    std::array<HostLocInfo, 32>& fprs;
    HostLocInfo& flags;
    std::array<HostLocInfo, SpillCount>& spills;
    bool& dirty;
    void ResetLocations();
    // Spill slots at or above this index have held nothing in this block.
    size_t spills_used{};

    // Locations whose use counts changed in the current instruction, so UpdateAllUses() visits only
    // them: UpdateUses() changes nothing in a location that wasn't touched since the last call.
    // MarkTouched() is called wherever uses_this_inst, expected_uses or values are set.
    static_assert(SpillCount <= 64);
    u32 touched_gprs = 0;
    u32 touched_fprs = 0;
    bool touched_flags = false;
    u64 touched_spills = 0;
    void MarkTouched(const HostLocInfo& info);

    // Location of each named value defined in this block, indexed by IR::Inst::GetName().
    std::vector<ValueLocationRecord>& value_locations;

    mutable size_t alloc_candidate_index{};

    // Values defined in this block: by name in defined_marks, unnamed values (name 0) here.
    std::vector<u64>& defined_marks;
    const u64 block_mark;
    tsl::robin_set<const IR::Inst*> defined_unnamed;
};

template<typename T>
RAReg<T>::RAReg(RegAlloc& reg_alloc, RWType rw, const IR::Value& read_value, HostLoc read_location, const IR::Inst* write_value)
        : reg_alloc{reg_alloc}, rw{rw}, read_value{read_value}, write_value{write_value} {
    if (rw != RWType::Write && !read_value.IsImmediate()) {
        locked_value = read_value.GetInst();
        locked_location = reg_alloc.LockValue(locked_value, read_location);
    }
}

template<typename T>
RAReg<T>::~RAReg() {
    if (locked_value) {
        reg_alloc.ValueInfo(locked_value, locked_location).locked--;
    }
    if (reg) {
        reg_alloc.ValueInfo(HostLoc{kind, reg->index()}).realized = false;
    }
}

template<typename T>
void RAReg<T>::Realize() {
    switch (rw) {
    case RWType::Read:
        if (locked_value && locked_location.kind == kind) {
            if (HostLocInfo& info = reg_alloc.ValueInfo(HostLoc{kind, locked_location.index}); info.Contains(locked_value)) {
                info.realized = true;
                reg = T{locked_location.index};
                break;
            }
        }
        reg = T{reg_alloc.RealizeReadImpl<kind>(read_value)};
        locked_location = HostLoc{kind, reg->index()};
        break;
    case RWType::Write:
        reg = T{reg_alloc.RealizeWriteImpl<kind>(write_value)};
        break;
    case RWType::ReadWrite:
        reg = T{reg_alloc.RealizeReadWriteImpl<kind>(read_value, write_value)};
        break;
    default:
        ASSERT_FALSE("Invalid RWType");
    }
}

}  // namespace Dynarmic::Backend::Arm64
