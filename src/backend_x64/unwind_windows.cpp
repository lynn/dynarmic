/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * This software may be used and distributed according to the terms of the GNU
 * General Public License version 2 or any later version.
 */

#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "backend_x64/block_of_code.h"
#include "common/assert.h"
#include "common/common_types.h"

using UBYTE = u8;

enum UNWIND_REGISTER_CODES {
    UWRC_RAX,
    UWRC_RCX,
    UWRC_RDX,
    UWRC_RBX,
    UWRC_RSP,
    UWRC_RBP,
    UWRC_RSI,
    UWRC_RDI,
    UWRC_R8,
    UWRC_R9,
    UWRC_R10,
    UWRC_R11,
    UWRC_R12,
    UWRC_R13,
    UWRC_R14,
    UWRC_R15,
};

enum UNWIND_OPCODE {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE = 1,
    UWOP_ALLOC_SMALL = 2,
    UWOP_SET_FPREG = 3,
    UWOP_SAVE_NONVOL = 4,
    UWOP_SAVE_NONVOL_FAR = 5,
    UWOP_SAVE_XMM128 = 8,
    UWOP_SAVE_XMM128_FAR = 9,
    UWOP_PUSH_MACHFRAME = 10,
};

union UNWIND_CODE {
    struct {
        UBYTE CodeOffset;
        UBYTE UnwindOp : 4;
        UBYTE OpInfo : 4;
    } code;
    USHORT FrameOffset;
};

// UNWIND_INFO is a tail-padded structure
struct UNWIND_INFO {
    UBYTE Version : 3;
    UBYTE Flags : 5;
    UBYTE SizeOfProlog;
    UBYTE CountOfCodes;
    UBYTE FrameRegister : 4;
    UBYTE FrameOffset : 4;
    // UNWIND_CODE UnwindCode[];
    // With Flags == 0 there are no additional fields.
};

namespace Dynarmic {
namespace BackendX64 {

struct PrologueInformation {
    std::vector<UNWIND_CODE> unwind_code;
    size_t number_of_unwind_code_entries;
    u8 prolog_size;
};

static PrologueInformation GetPrologueInformation() {
    PrologueInformation ret;

    const auto next_entry = [&]() -> UNWIND_CODE& {
        ret.unwind_code.emplace_back();
        return ret.unwind_code.back();
    };
    const auto push_nonvol = [&](u8 offset, UNWIND_REGISTER_CODES reg) {
        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_PUSH_NONVOL;
        entry.code.OpInfo = reg;
    };
    const auto alloc_large = [&](u8 offset, size_t size) {
        ASSERT(size % 8 == 0);
        size /= 8;

        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_ALLOC_LARGE;
        if (size <= 0xFFFF) {
            entry.code.OpInfo = 0;
            auto& size_entry = next_entry();
            size_entry.FrameOffset = static_cast<USHORT>(size);
        } else {
            entry.code.OpInfo = 1;
            auto& size_entry_1 = next_entry();
            size_entry_1.FrameOffset = static_cast<USHORT>(size);
            auto& size_entry_2 = next_entry();
            size_entry_2.FrameOffset = static_cast<USHORT>(size >> 16);
        }
    };
    const auto save_xmm128 = [&](u8 offset, u8 reg, size_t frame_offset) {
        ASSERT(frame_offset % 16 == 0);

        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_SAVE_XMM128;
        entry.code.OpInfo = reg;
        auto& offset_entry = next_entry();
        offset_entry.FrameOffset = static_cast<USHORT>(frame_offset / 16);
    };

    // This is a list of operations that occur in the prologue.
    // The debugger uses this information to retrieve register values and
    // to calculate the size of the stack frame.
    ret.prolog_size = 89;
    save_xmm128(89, 15, 0xB0);  // +050  44 0F 29 BC 24 B0 00 00 00  movaps  xmmword ptr [rsp+0B0h],xmm15
    save_xmm128(80, 14, 0xA0);  // +047  44 0F 29 B4 24 A0 00 00 00  movaps  xmmword ptr [rsp+0A0h],xmm14
    save_xmm128(71, 13, 0x90);  // +03E  44 0F 29 AC 24 90 00 00 00  movaps  xmmword ptr [rsp+90h],xmm13
    save_xmm128(62, 12, 0x80);  // +035  44 0F 29 A4 24 80 00 00 00  movaps  xmmword ptr [rsp+80h],xmm12
    save_xmm128(53, 11, 0x70);  // +02F  44 0F 29 5C 24 70           movaps  xmmword ptr [rsp+70h],xmm11
    save_xmm128(47, 10, 0x60);  // +029  44 0F 29 54 24 60           movaps  xmmword ptr [rsp+60h],xmm10
    save_xmm128(41, 9, 0x50);   // +023  44 0F 29 4C 24 50           movaps  xmmword ptr [rsp+50h],xmm9
    save_xmm128(35, 8, 0x40);   // +01D  44 0F 29 44 24 40           movaps  xmmword ptr [rsp+40h],xmm8
    save_xmm128(29, 7, 0x30);   // +018  0F 29 7C 24 30              movaps  xmmword ptr [rsp+30h],xmm7
    save_xmm128(24, 6, 0x20);   // +013  0F 29 74 24 20              movaps  xmmword ptr [rsp+20h],xmm6
    alloc_large(19, 0xC8);      // +00C  48 81 EC C8 00 00 00        sub     rsp,0C8h
    push_nonvol(12, UWRC_R15);  // +00A  41 57                       push    r15
    push_nonvol(10, UWRC_R14);  // +008  41 56                       push    r14
    push_nonvol(8, UWRC_R13);   // +006  41 55                       push    r13
    push_nonvol(6, UWRC_R12);   // +004  41 54                       push    r12
    push_nonvol(4, UWRC_RBP);   // +003  55                          push    rbp
    push_nonvol(3, UWRC_RDI);   // +002  57                          push    rdi
    push_nonvol(2, UWRC_RSI);   // +001  56                          push    rsi
    push_nonvol(1, UWRC_RBX);   // +000  53                          push    rbx

    ret.number_of_unwind_code_entries = ret.unwind_code.size();

    // The Windows API requires the size of the unwind_code array
    // to be a multiple of two for alignment reasons.
    if (ret.unwind_code.size() % 2 == 1) {
        auto& last_entry = next_entry();
        last_entry.FrameOffset = 0;
    }
    ASSERT(ret.unwind_code.size() % 2 == 0);

    return ret;
}

struct BlockOfCode::UnwindHandler::Impl final {
    Impl(RUNTIME_FUNCTION* rfuncs_, const u8* base_ptr) : rfuncs(rfuncs_) {
        RtlAddFunctionTable(rfuncs, 1, reinterpret_cast<DWORD64>(base_ptr));
    }

    ~Impl() {
        RtlDeleteFunctionTable(rfuncs);
    }

private:
    RUNTIME_FUNCTION* rfuncs = nullptr;
};

BlockOfCode::UnwindHandler::UnwindHandler() = default;
BlockOfCode::UnwindHandler::~UnwindHandler() = default;

void BlockOfCode::UnwindHandler::Register(BlockOfCode* code) {
    const auto prolog_info = GetPrologueInformation();

    code->align(16);
    UNWIND_INFO* unwind_info = static_cast<UNWIND_INFO*>(code->AllocateFromCodeSpace(sizeof(UNWIND_INFO)));
    unwind_info->Version = 1;
    unwind_info->Flags = 0; // No special exception handling required.
    unwind_info->SizeOfProlog = prolog_info.prolog_size;
    unwind_info->CountOfCodes = static_cast<UBYTE>(prolog_info.number_of_unwind_code_entries);
    unwind_info->FrameRegister = 0; // No frame register present
    unwind_info->FrameOffset = 0; // Unused because FrameRegister == 0
    // UNWIND_INFO::UnwindCode field:
    const size_t size_of_unwind_code = sizeof(UNWIND_CODE) * prolog_info.unwind_code.size();
    UNWIND_CODE* unwind_code = static_cast<UNWIND_CODE*>(code->AllocateFromCodeSpace(size_of_unwind_code));
    memcpy(unwind_code, prolog_info.unwind_code.data(), size_of_unwind_code);

    code->align(16);
    RUNTIME_FUNCTION* rfuncs = static_cast<RUNTIME_FUNCTION*>(code->AllocateFromCodeSpace(sizeof(RUNTIME_FUNCTION)));
    rfuncs->BeginAddress = static_cast<DWORD>(reinterpret_cast<u8*>(code->run_code) - code->getCode());
    rfuncs->EndAddress = static_cast<DWORD>(code->maxSize_);
    rfuncs->UnwindData = static_cast<DWORD>(reinterpret_cast<u8*>(unwind_info) - code->getCode());

    impl = std::make_unique<Impl>(rfuncs, code->getCode());
}

} // namespace BackendX64
} // namespace Dynarmic
