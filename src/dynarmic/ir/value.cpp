/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/ir/value.h"

#include <mcl/assert.hpp>
#include <mcl/bit/bit_field.hpp>

#include "dynarmic/ir/microinstruction.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/type.h"

namespace Dynarmic::IR {

Value Value::EmptyNZCVImmediateMarker() {
    Value result{};
    result.type = Type::NZCVFlags;
    return result;
}

bool Value::IsImmediateThroughIdentity() const {
    return inner.inst->GetArg(0).IsImmediate();
}

Type Value::GetTypeThroughIdentity() const {
    return inner.inst->GetArg(0).GetType();
}

Value Value::ImmediateThroughIdentity(Type expected_type) const {
    Value value = *this;
    while (value.IsIdentity()) {
        value = value.inner.inst->GetArg(0);
    }
    ASSERT(value.type == expected_type);
    return value;
}

Inst* Value::GetInstRecursive() const {
    ASSERT(type == Type::Opaque);
    if (IsIdentity())
        return inner.inst->GetArg(0).GetInstRecursive();
    return inner.inst;
}

s64 Value::GetImmediateAsS64() const {
    ASSERT(IsImmediate());

    switch (GetType()) {
    case IR::Type::U1:
        return s64(GetU1());
    case IR::Type::U8:
        return s64(mcl::bit::sign_extend<8, u64>(GetU8()));
    case IR::Type::U16:
        return s64(mcl::bit::sign_extend<16, u64>(GetU16()));
    case IR::Type::U32:
        return s64(mcl::bit::sign_extend<32, u64>(GetU32()));
    case IR::Type::U64:
        return s64(GetU64());
    default:
        ASSERT_FALSE("GetImmediateAsS64 called on an incompatible Value type.");
    }
}

u64 Value::GetImmediateAsU64() const {
    ASSERT(IsImmediate());

    switch (GetType()) {
    case IR::Type::U1:
        return u64(GetU1());
    case IR::Type::U8:
        return u64(GetU8());
    case IR::Type::U16:
        return u64(GetU16());
    case IR::Type::U32:
        return u64(GetU32());
    case IR::Type::U64:
        return u64(GetU64());
    default:
        ASSERT_FALSE("GetImmediateAsU64 called on an incompatible Value type.");
    }
}

bool Value::IsSignedImmediate(s64 value) const {
    return IsImmediate() && GetImmediateAsS64() == value;
}

bool Value::IsUnsignedImmediate(u64 value) const {
    return IsImmediate() && GetImmediateAsU64() == value;
}

bool Value::HasAllBitsSet() const {
    return IsSignedImmediate(-1);
}

bool Value::IsZero() const {
    return IsUnsignedImmediate(0);
}

}  // namespace Dynarmic::IR
