// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
namespace Memory {
class Memory;
}
} // namespace Core

namespace Kernel {
class KProcess;
}

// Fritchii host routines: a few of the game's own functions answered by native code instead of
// their translated guest code. The JIT compiles a BRK in place of the function's first
// instruction (MemoryReadCode, guest memory unchanged); the breakpoint calls OnBreakpoint inside
// the block (no exit from the run loop), which either answers the call (x0 the result, PC the
// return address or the function's own epilogue) or declines it (the replaced instruction's
// effect, if any, then the next instruction).
//
// Today: OoT's two zstd decodes (FRITCHII_HOST_ZSTD), on the host's libzstd.
namespace Core::HostRoutines {

/// The registers of the core that reached a routine's entry (the JIT's state while the
/// breakpoint's callback runs).
class Registers {
public:
    virtual u64 Get(int index) const = 0;
    virtual void Set(int index, u64 value) = 0;
    virtual void SetPC(u64 pc) = 0;
    /// PSTATE as the JIT keeps it (NZCV in bits 31..28).
    virtual u32 GetPstate() const = 0;
    virtual void SetPstate(u32 value) = 0;

protected:
    ~Registers() = default;
};

/// Entries a process may have hooked, as an inclusive address range for the JIT's quick check
/// (first = last = 0: none; every routine is off, another game, or the debugger is on).
struct Sites {
    u64 first = 0;
    u64 last = 0;
};

/// Computed once per JIT callbacks object, at the first code read (the image is loaded then).
Sites SitesFor(Core::System& system, const Kernel::KProcess& process);

/// The word the JIT compiles at `vaddr` (inside SitesFor's range): a BRK when `vaddr` is a
/// routine's entry whose code matches (checked once per process), else `instruction`.
u32 InstructionFor(Core::System& system, Kernel::KProcess& process, Core::Memory::Memory& memory,
                   u64 vaddr, u32 instruction);

/// A breakpoint at `pc`: true when it is a routine's (the call was answered or declined, PC
/// set either way); false leaves it to the caller's own breakpoint handling.
bool OnBreakpoint(Core::System& system, Kernel::KProcess& process, Core::Memory::Memory& memory,
                  Registers& regs, u64 pc);

/// Verify mode (FRITCHII_HOST_ZSTD bit 2): stream chunks whose shadow decode differed from what
/// the game's code did, since boot (must be 0; also in the log line).
u64 StreamVerifyMismatches();

} // namespace Core::HostRoutines
