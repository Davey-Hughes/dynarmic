/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <type_traits>

#include <mcl/assert.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/ir/type.h"

namespace Dynarmic::A32 {
enum class ExtReg;
enum class Reg;
}  // namespace Dynarmic::A32

namespace Dynarmic::A64 {
enum class Reg;
enum class Vec;
}  // namespace Dynarmic::A64

namespace Dynarmic::IR {

class Inst;
enum class AccType;
enum class Cond;

/**
 * A representation of a value in the IR.
 * A value may either be an immediate or the result of a microinstruction.
 */
class Value {
public:
    using CoprocessorInfo = std::array<u8, 8>;

    Value()
            : type(Type::Void) {}
    explicit Value(Inst* value)
            : type(Type::Opaque) {
        inner.inst = value;
    }
    explicit Value(A32::Reg value)
            : type(Type::A32Reg) {
        inner.imm_a32regref = value;
    }
    explicit Value(A32::ExtReg value)
            : type(Type::A32ExtReg) {
        inner.imm_a32extregref = value;
    }
    explicit Value(A64::Reg value)
            : type(Type::A64Reg) {
        inner.imm_a64regref = value;
    }
    explicit Value(A64::Vec value)
            : type(Type::A64Vec) {
        inner.imm_a64vecref = value;
    }
    explicit Value(bool value)
            : type(Type::U1) {
        inner.imm_u1 = value;
    }
    explicit Value(u8 value)
            : type(Type::U8) {
        inner.imm_u8 = value;
    }
    explicit Value(u16 value)
            : type(Type::U16) {
        inner.imm_u16 = value;
    }
    explicit Value(u32 value)
            : type(Type::U32) {
        inner.imm_u32 = value;
    }
    explicit Value(u64 value)
            : type(Type::U64) {
        inner.imm_u64 = value;
    }
    explicit Value(CoprocessorInfo value)
            : type(Type::CoprocInfo) {
        inner.imm_coproc = value;
    }
    explicit Value(Cond value)
            : type(Type::Cond) {
        inner.imm_cond = value;
    }
    explicit Value(AccType value)
            : type(Type::AccType) {
        inner.imm_acctype = value;
    }

    static Value EmptyNZCVImmediateMarker();

    bool IsIdentity() const;
    bool IsEmpty() const { return type == Type::Void; }
    bool IsImmediate() const;
    Type GetType() const;

    Inst* GetInst() const {
        ASSERT(type == Type::Opaque);
        return inner.inst;
    }
    Inst* GetInstRecursive() const;
    A32::Reg GetA32RegRef() const {
        ASSERT(type == Type::A32Reg);
        return inner.imm_a32regref;
    }
    A32::ExtReg GetA32ExtRegRef() const {
        ASSERT(type == Type::A32ExtReg);
        return inner.imm_a32extregref;
    }
    A64::Reg GetA64RegRef() const {
        ASSERT(type == Type::A64Reg);
        return inner.imm_a64regref;
    }
    A64::Vec GetA64VecRef() const {
        ASSERT(type == Type::A64Vec);
        return inner.imm_a64vecref;
    }
    bool GetU1() const {
        if (type == Type::U1)
            return inner.imm_u1;
        return ImmediateThroughIdentity(Type::U1).inner.imm_u1;
    }
    u8 GetU8() const {
        if (type == Type::U8)
            return inner.imm_u8;
        return ImmediateThroughIdentity(Type::U8).inner.imm_u8;
    }
    u16 GetU16() const {
        if (type == Type::U16)
            return inner.imm_u16;
        return ImmediateThroughIdentity(Type::U16).inner.imm_u16;
    }
    u32 GetU32() const {
        if (type == Type::U32)
            return inner.imm_u32;
        return ImmediateThroughIdentity(Type::U32).inner.imm_u32;
    }
    u64 GetU64() const {
        if (type == Type::U64)
            return inner.imm_u64;
        return ImmediateThroughIdentity(Type::U64).inner.imm_u64;
    }
    CoprocessorInfo GetCoprocInfo() const {
        if (type == Type::CoprocInfo)
            return inner.imm_coproc;
        return ImmediateThroughIdentity(Type::CoprocInfo).inner.imm_coproc;
    }
    Cond GetCond() const {
        if (type == Type::Cond)
            return inner.imm_cond;
        return ImmediateThroughIdentity(Type::Cond).inner.imm_cond;
    }
    AccType GetAccType() const {
        if (type == Type::AccType)
            return inner.imm_acctype;
        return ImmediateThroughIdentity(Type::AccType).inner.imm_acctype;
    }

    /**
     * Retrieves the immediate of a Value instance as a signed 64-bit value.
     *
     * @pre The value contains either a U1, U8, U16, U32, or U64 value.
     *      Breaking this precondition will cause an assertion to be invoked.
     */
    s64 GetImmediateAsS64() const;

    /**
     * Retrieves the immediate of a Value instance as an unsigned 64-bit value.
     *
     * @pre The value contains either a U1, U8, U16, U32, or U64 value.
     *      Breaking this precondition will cause an assertion to be invoked.
     */
    u64 GetImmediateAsU64() const;

    /**
     * Determines whether or not the contained value matches the provided signed one.
     *
     * Note that this function will always return false if the contained
     * value is not a a constant value. In other words, if IsImmediate()
     * would return false on an instance, then so will this function.
     *
     * @param value The value to check against the contained value.
     */
    bool IsSignedImmediate(s64 value) const;

    /**
     * Determines whether or not the contained value matches the provided unsigned one.
     *
     * Note that this function will always return false if the contained
     * value is not a a constant value. In other words, if IsImmediate()
     * would return false on an instance, then so will this function.
     *
     * @param value The value to check against the contained value.
     */
    bool IsUnsignedImmediate(u64 value) const;

    /**
     * Determines whether or not the contained constant value has all bits set.
     *
     * @pre The value contains either a U1, U8, U16, U32, or U64 value.
     *      Breaking this precondition will cause an assertion to be invoked.
     */
    bool HasAllBitsSet() const;

    /**
     * Whether or not the current value contains a representation of zero.
     *
     * Note that this function will always return false if the contained
     * value is not a a constant value. In other words, if IsImmediate()
     * would return false on an instance, then so will this function.
     */
    bool IsZero() const;

private:
    // The identity cases of IsImmediate() and GetType(), out of line so that those two stay inlinable.
    bool IsImmediateThroughIdentity() const;
    Type GetTypeThroughIdentity() const;
    // The same for the immediate getters: the value at the end of the identity chain, which must have this type.
    Value ImmediateThroughIdentity(Type expected_type) const;

    Type type;

    union {
        Inst* inst;  // type == Type::Opaque
        A32::Reg imm_a32regref;
        A32::ExtReg imm_a32extregref;
        A64::Reg imm_a64regref;
        A64::Vec imm_a64vecref;
        bool imm_u1;
        u8 imm_u8;
        u16 imm_u16;
        u32 imm_u32;
        u64 imm_u64;
        CoprocessorInfo imm_coproc;
        Cond imm_cond;
        AccType imm_acctype;
    } inner;
};
static_assert(sizeof(Value) <= 2 * sizeof(u64), "IR::Value should be kept small in size");

template<Type type_>
class TypedValue final : public Value {
public:
    TypedValue() = default;

    template<Type other_type, typename = std::enable_if_t<(other_type & type_) != Type::Void>>
    /* implicit */ TypedValue(const TypedValue<other_type>& value)
            : Value(value) {
        ASSERT((value.GetType() & type_) != Type::Void);
    }

    explicit TypedValue(const Value& value)
            : Value(value) {
        ASSERT((value.GetType() & type_) != Type::Void);
    }

    explicit TypedValue(Inst* inst)
            : TypedValue(Value(inst)) {}
};

using U1 = TypedValue<Type::U1>;
using U8 = TypedValue<Type::U8>;
using U16 = TypedValue<Type::U16>;
using U32 = TypedValue<Type::U32>;
using U64 = TypedValue<Type::U64>;
using U128 = TypedValue<Type::U128>;
using U32U64 = TypedValue<Type::U32 | Type::U64>;
using U16U32U64 = TypedValue<Type::U16 | Type::U32 | Type::U64>;
using UAny = TypedValue<Type::U8 | Type::U16 | Type::U32 | Type::U64>;
using UAnyU128 = TypedValue<Type::U8 | Type::U16 | Type::U32 | Type::U64 | Type::U128>;
using NZCV = TypedValue<Type::NZCVFlags>;
using Table = TypedValue<Type::Table>;

}  // namespace Dynarmic::IR

// The inline definitions of Value::IsIdentity(), IsImmediate() and GetType(), which need Inst.
#include "dynarmic/ir/microinstruction.h"
