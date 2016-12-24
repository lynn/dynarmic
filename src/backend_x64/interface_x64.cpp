/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#include <memory>
#include <queue>

#include <fmt/format.h>

#ifdef DYNARMIC_USE_LLVM
#include <llvm-c/Disassembler.h>
#include <llvm-c/Target.h>
#endif

#include "backend_x64/block_of_code.h"
#include "backend_x64/emit_x64.h"
#include "backend_x64/jitstate.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/scope_exit.h"
#include "dynarmic/dynarmic.h"
#include "frontend/ir/basic_block.h"
#include "frontend/ir/location_descriptor.h"
#include "frontend/translate/translate.h"
#include "ir_opt/passes.h"

namespace Dynarmic {

using namespace BackendX64;

struct Jit::Impl {
    Impl(Jit* jit, UserCallbacks callbacks)
            : block_of_code(callbacks)
            , jit_state()
            , emitter(&block_of_code, callbacks, jit)
            , callbacks(callbacks)
            , invalid_cache_ranges()
    {}

    BlockOfCode block_of_code;
    JitState jit_state;
    EmitX64 emitter;
    const UserCallbacks callbacks;

    // Requests made during execution to invalidate the cache are queued up here.
    std::queue<std::unique_ptr<Common::AddressRange>> invalid_cache_ranges;

    size_t Execute(size_t cycle_count) {
        u32 pc = jit_state.Reg[15];

        IR::LocationDescriptor descriptor{pc, Arm::PSR{jit_state.Cpsr}, Arm::FPSCR{jit_state.FPSCR_mode}};

        CodePtr code_ptr = GetBasicBlock(descriptor).code_ptr;
        return block_of_code.RunCode(&jit_state, code_ptr, cycle_count);
    }

    std::string Disassemble(const IR::LocationDescriptor& descriptor) {
        auto block = GetBasicBlock(descriptor);
        std::string result = fmt::format("address: {}\nsize: {} bytes\n", block.code_ptr, block.size);

#ifdef DYNARMIC_USE_LLVM
        CodePtr end = block.code_ptr + block.size;
        size_t remaining = block.size;

        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86Disassembler();
        LLVMDisasmContextRef llvm_ctx = LLVMCreateDisasm("x86_64", nullptr, 0, nullptr, nullptr);
        LLVMSetDisasmOptions(llvm_ctx, LLVMDisassembler_Option_AsmPrinterVariant);

        for (CodePtr pos = block.code_ptr; pos < end;) {
            char buffer[80];
            size_t inst_size = LLVMDisasmInstruction(llvm_ctx, const_cast<u8*>(pos), remaining, (u64)pos, buffer, sizeof(buffer));
            ASSERT(inst_size);
            for (CodePtr i = pos; i < pos + inst_size; i++)
                result += fmt::format("{:02x} ", *i);
            for (size_t i = inst_size; i < 10; i++)
                result += "   ";
            result += buffer;
            result += '\n';

            pos += inst_size;
            remaining -= inst_size;
        }

        LLVMDisasmDispose(llvm_ctx);
#else
        result.append("(recompile with DYNARMIC_USE_LLVM=ON to disassemble the generated x86_64 code)\n");
#endif

        return result;
    }

    void PerformCacheInvalidation() {
        if (invalid_cache_ranges.empty()) {
            return;
        }

        jit_state.ResetRSB();
        block_of_code.ClearCache();
        while (!invalid_cache_ranges.empty()) {
            auto range = std::move(invalid_cache_ranges.front());
            invalid_cache_ranges.pop();
            emitter.InvalidateCacheRange(*range);
        }

        // block_of_code.ClearCache();
        // emitter.ClearCache();
        // clear_cache_required = false;
    }

private:
    EmitX64::BlockDescriptor GetBasicBlock(IR::LocationDescriptor descriptor) {
        auto block = emitter.GetBasicBlock(descriptor);
        if (block)
            return *block;

        IR::Block ir_block = Arm::Translate(descriptor, callbacks.MemoryReadCode);
        Optimization::GetSetElimination(ir_block);
        Optimization::DeadCodeElimination(ir_block);
        Optimization::VerificationPass(ir_block);
        return emitter.Emit(ir_block);
    }
};

Jit::Jit(UserCallbacks callbacks) : impl(std::make_unique<Impl>(this, callbacks)) {}

Jit::~Jit() {}

size_t Jit::Run(size_t cycle_count) {
    ASSERT(!is_executing);
    is_executing = true;
    SCOPE_EXIT({ this->is_executing = false; });

    impl->jit_state.halt_requested = false;

    size_t cycles_executed = 0;
    while (cycles_executed < cycle_count && !impl->jit_state.halt_requested) {
        cycles_executed += impl->Execute(cycle_count - cycles_executed);
    }

    impl->PerformCacheInvalidation();

    return cycles_executed;
}

void Jit::HandleNewCacheRange() {
    if (is_executing) {
        impl->jit_state.halt_requested = true;
        return;
    }

    impl->PerformCacheInvalidation();
}

void Jit::ClearCache() {
    impl->invalid_cache_ranges.push(std::make_unique<Common::FullAddressRange>());
    HandleNewCacheRange();
}

void Jit::InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {
    impl->invalid_cache_ranges.push(
        std::make_unique<Common::AddressInterval>(start_address, length));
    HandleNewCacheRange();
}

void Jit::Reset() {
    ASSERT(!is_executing);
    impl->jit_state = {};
}

void Jit::HaltExecution() {
    ASSERT(is_executing);
    impl->jit_state.halt_requested = true;

    // TODO: Uh do other stuff to JitState pls.
}

std::array<u32, 16>& Jit::Regs() {
    return impl->jit_state.Reg;
}
const std::array<u32, 16>& Jit::Regs() const {
    return impl->jit_state.Reg;
}

std::array<u32, 64>& Jit::ExtRegs() {
    return impl->jit_state.ExtReg;
}

const std::array<u32, 64>& Jit::ExtRegs() const {
    return impl->jit_state.ExtReg;
}

u32& Jit::Cpsr() {
    return impl->jit_state.Cpsr;
}

u32 Jit::Cpsr() const {
    return impl->jit_state.Cpsr;
}

u32 Jit::Fpscr() const {
    return impl->jit_state.Fpscr();
}

void Jit::SetFpscr(u32 value) const {
    return impl->jit_state.SetFpscr(value);
}

std::string Jit::Disassemble(const IR::LocationDescriptor& descriptor) {
    return impl->Disassemble(descriptor);
}

} // namespace Dynarmic
