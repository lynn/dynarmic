/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#include <algorithm>

#include "common/assert.h"
#include "frontend/arm/types.h"
#include "frontend/decoder/arm.h"
#include "frontend/decoder/vfp2.h"
#include "frontend/ir/basic_block.h"
#include "frontend/ir/location_descriptor.h"
#include "frontend/translate/translate.h"
#include "frontend/translate/translate_arm/translate_arm.h"

namespace Dynarmic {
namespace Arm {

static bool CondCanContinue(ConditionalState cond_state, const IR::IREmitter& ir) {
    ASSERT_MSG(cond_state != ConditionalState::Break, "Should never happen.");

    if (cond_state == ConditionalState::None)
        return true;

    // TODO: This is more conservative than necessary.
    return std::all_of(ir.block.begin(), ir.block.end(), [](const IR::Inst& inst) { return !inst.WritesToCPSR(); });
}

IR::Block TranslateArm(IR::LocationDescriptor descriptor, MemoryReadCodeFuncType memory_read_code) {
    ArmTranslatorVisitor visitor{descriptor};

    bool should_continue = true;
    while (should_continue && CondCanContinue(visitor.cond_state, visitor.ir)) {
        const u32 arm_pc = visitor.ir.current_location.PC();
        const u32 arm_instruction = memory_read_code(arm_pc);

        if (auto vfp_decoder = DecodeVFP2<ArmTranslatorVisitor>(arm_instruction)) {
            should_continue = vfp_decoder->call(visitor, arm_instruction);
        } else if (auto decoder = DecodeArm<ArmTranslatorVisitor>(arm_instruction)) {
            should_continue = decoder->call(visitor, arm_instruction);
        } else {
            should_continue = visitor.arm_UDF();
        }

        if (visitor.cond_state == ConditionalState::Break) {
            break;
        }

        visitor.ir.current_location = visitor.ir.current_location.AdvancePC(4);
        visitor.ir.block.CycleCount()++;
    }

    if (visitor.cond_state == ConditionalState::Translating || visitor.cond_state == ConditionalState::Trailing) {
        if (should_continue) {
            visitor.ir.SetTerm(IR::Term::LinkBlockFast{visitor.ir.current_location});
        }
    }

    ASSERT_MSG(visitor.ir.block.HasTerminal(), "Terminal has not been set");

    visitor.ir.block.SetEndLocation(visitor.ir.current_location);

    return std::move(visitor.ir.block);
}

bool ArmTranslatorVisitor::ConditionPassed(Cond cond) {
    ASSERT_MSG(cond_state != ConditionalState::Break,
               "This should never happen. We requested a break but that wasn't honored.");
    ASSERT_MSG(cond != Cond::NV, "NV conditional is obsolete");

    if (cond_state == ConditionalState::Translating) {
        if (ir.block.ConditionFailedLocation() != ir.current_location || cond == Cond::AL) {
            cond_state = ConditionalState::Trailing;
        } else {
            if (cond == ir.block.GetCondition()) {
                ir.block.SetConditionFailedLocation({ ir.current_location.AdvancePC(4) });
                ir.block.ConditionFailedCycleCount()++;
                return true;
            }

            // cond has changed, abort
            cond_state = ConditionalState::Break;
            ir.SetTerm(IR::Term::LinkBlockFast{ir.current_location});
            return false;
        }
    }

    if (cond == Cond::AL) {
        // Everything is fine with the world
        return true;
    }

    // non-AL cond

    if (!ir.block.empty()) {
        // We've already emitted instructions. Quit for now, we'll make a new block here later.
        cond_state = ConditionalState::Break;
        ir.SetTerm(IR::Term::LinkBlockFast{ir.current_location});
        return false;
    }

    // We've not emitted instructions yet.
    // We'll emit one instruction, and set the block-entry conditional appropriately.

    cond_state = ConditionalState::Translating;
    ir.block.SetCondition(cond);
    ir.block.SetConditionFailedLocation({ ir.current_location.AdvancePC(4) });
    ir.block.ConditionFailedCycleCount() = 1;
    return true;
}

bool ArmTranslatorVisitor::InterpretThisInstruction() {
    ir.SetTerm(IR::Term::Interpret(ir.current_location));
    return false;
}

bool ArmTranslatorVisitor::UnpredictableInstruction() {
    ASSERT_MSG(false, "UNPREDICTABLE");
    return false;
}

IR::IREmitter::ResultAndCarry ArmTranslatorVisitor::EmitImmShift(IR::Value value, ShiftType type, Imm5 imm5, IR::Value carry_in) {
    switch (type) {
    case ShiftType::LSL:
        return ir.LogicalShiftLeft(value, ir.Imm8(imm5), carry_in);
    case ShiftType::LSR:
        imm5 = imm5 ? imm5 : 32;
        return ir.LogicalShiftRight(value, ir.Imm8(imm5), carry_in);
    case ShiftType::ASR:
        imm5 = imm5 ? imm5 : 32;
        return ir.ArithmeticShiftRight(value, ir.Imm8(imm5), carry_in);
    case ShiftType::ROR:
        if (imm5)
            return ir.RotateRight(value, ir.Imm8(imm5), carry_in);
        else
            return ir.RotateRightExtended(value, carry_in);
    }
    ASSERT_MSG(false, "Unreachable");
    return {};
}

IR::IREmitter::ResultAndCarry ArmTranslatorVisitor::EmitRegShift(IR::Value value, ShiftType type, IR::Value amount, IR::Value carry_in) {
    switch (type) {
    case ShiftType::LSL:
        return ir.LogicalShiftLeft(value, amount, carry_in);
    case ShiftType::LSR:
        return ir.LogicalShiftRight(value, amount, carry_in);
    case ShiftType::ASR:
        return ir.ArithmeticShiftRight(value, amount, carry_in);
    case ShiftType::ROR:
        return ir.RotateRight(value, amount, carry_in);
    }
    ASSERT_MSG(false, "Unreachable");
    return {};
}

} // namespace Arm
} // namespace Dynarmic
