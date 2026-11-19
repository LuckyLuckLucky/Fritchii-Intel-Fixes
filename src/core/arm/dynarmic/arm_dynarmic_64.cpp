// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include "common/logging.h"
#include "common/lowend_sim.h"
#include "common/settings.h"
#include "common/nx2_hitch.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/arm/dynarmic/window_trace.h"
#include "core/core_timing.h"
#include "core/hardware_properties.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/nx2_thread_stats.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"

namespace Core {

using namespace Common::Literals;

namespace {
// EDEN_NX2_GUEST_THREAD_STATS: the JIT's dispatcher counters of each core's JITs (one per process).
std::array<Dynarmic::A64::UserConfig::Counters, Hardware::NUM_CPU_CORES> g_jit_counters{};

// Guest code invalidations (kernel code mapping, nn::ro, gdbstub, guest IC ops) and the page
// copies one raced. Logged at 1, 2, 4, 8... so a log shows whether code changes.
std::atomic<u64> g_code_invalidations{};
std::atomic<u64> g_code_page_races{};

void CountCodeInvalidation(std::size_t core, u64 addr, std::size_t size) {
    const u64 n = g_code_invalidations.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0) {
        LOG_INFO(Core_ARM, "JIT guest code invalidation #{} (core {}): {:#x}, {:#x} bytes", n, core,
                 addr, size);
    }
}

void CountCodePageRace(u64 page) {
    const u64 n = g_code_page_races.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0) {
        LOG_WARNING(Core_ARM,
                    "JIT code page {:#x} copied while it was invalidated: refetched (invalidation "
                    "races avoided: {})",
                    page, n);
    }
}
} // Anonymous namespace

DynarmicCallbacks64::DynarmicCallbacks64(ArmDynarmic64& parent, Kernel::KProcess* process)
    : m_parent{parent}, m_memory(process->GetMemory())
    , m_process(process), m_debugger_enabled{parent.m_system.DebuggerEnabled()}
    , m_check_memory_access{m_debugger_enabled || !Settings::values.cpuopt_ignore_memory_aborts.GetValue()}
{}

u64 DynarmicCallbacks64::MemoryRead(u64 vaddr, size_t size) {
    CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Read);
    u64 value{};
    switch (size) {
    case sizeof(u64): value = m_memory.Read64(vaddr); break;
    case sizeof(u32): value = m_memory.Read32(vaddr); break;
    case sizeof(u16): value = m_memory.Read16(vaddr); break;
    case sizeof(u8): value = m_memory.Read8(vaddr); break;
    default: UNREACHABLE();
    }
    // With memory aborts ignored the guest keeps running on zeros: say where it read from.
    if (auto& unmapped = Core::Memory::UnmappedReadFlag(); unmapped != 0) {
        unmapped = 0;
        // 32 lines a burst (reads a second or more after the previous one start a new burst),
        // and the first 32 bursts each get a guest backtrace from the core's thread once the JIT
        // returns (the pc here is the block's, the backtrace the real call chain). A tester's
        // Jabu-Jabu log (2026-11-17) spent the old 32-a-process budget on the first of three
        // null-object bursts, at area loads; the third ended in the game's crash.
        static std::atomic<u32> logged{};
        static std::atomic<u64> last_ns{};
        static std::atomic<u32> bursts{};
        const u64 now_ns = Common::Nx2Hitch::NowNs();
        if (now_ns - last_ns.exchange(now_ns, std::memory_order_relaxed) > 1'000'000'000ULL) {
            logged.store(0, std::memory_order_relaxed);
            const u32 burst = bursts.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG_ERROR(Core_ARM, "Unmapped read burst #{} begins (guest pointer from zeros or "
                                "wrong data; ignore_memory_aborts keeps the game running)",
                      burst);
            if (burst <= 32) {
                m_parent.RequestBacktrace(nullptr, "unmapped read burst");
            }
        }
        if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_ERROR(Core_ARM, "Unmapped read {:#x} ({} bytes) at pc {:#x} lr {:#x} sp {:#x}", vaddr, size,
                      m_parent.m_jit->GetPC(), m_parent.m_jit->GetRegister(30), m_parent.m_jit->GetSP());
        }
    }
    return value;
}
Dynarmic::A64::Vector DynarmicCallbacks64::MemoryRead128(u64 vaddr) {
    CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Read);
    return {m_memory.Read64(vaddr), m_memory.Read64(vaddr + 8)};
}

std::optional<u32> DynarmicCallbacks64::MemoryReadCode(u64 vaddr) {
    if (!m_memory.IsValidVirtualAddressRange(vaddr, sizeof(u32)))
        return std::nullopt;
    if (direct_code_reads) {
        return HostCode(vaddr, m_memory.Read32(vaddr));
    }
    auto const aligned_vaddr = vaddr & ~Core::Memory::YUZU_PAGEMASK;
    const u32 generation = code_generation.load(std::memory_order_acquire);
    if (last_code_addr != aligned_vaddr || cached_code_generation != generation) {
        m_memory.ReadBlock(aligned_vaddr, &cached_code_page, sizeof(cached_code_page));
        last_code_addr = aligned_vaddr;
        cached_code_generation = generation;
        if (code_generation.load(std::memory_order_acquire) != generation) [[unlikely]] {
            // An invalidation landed during the copy: the old reset could be lost here and the
            // page copy reused with old code; now the next read fetches the page again.
            CountCodePageRace(aligned_vaddr);
        }
    }
    const u32 instruction =
        HostCode(vaddr, cached_code_page.inst[(vaddr & Core::Memory::YUZU_PAGEMASK) / sizeof(u32)]);
    if (WindowTrace::Enabled() && m_process->GetProgramId() == WindowTrace::ProgramId) {
        if (!m_window_trace) {
            m_window_trace = WindowTrace::sessions.Find(m_process);
        }
        if (m_window_trace) {
            return WindowTrace::InstructionFor(m_window_trace->base, vaddr, instruction);
        }
    }
    return instruction;
}

const HostRoutines::Sites& DynarmicCallbacks64::HostSites() {
    std::call_once(m_host_sites_once, [this] {
        if (!m_debugger_enabled) {
            m_host_sites = HostRoutines::SitesFor(m_parent.m_system, *m_process);
        }
    });
    return m_host_sites;
}

u32 DynarmicCallbacks64::HostCode(u64 vaddr, u32 instruction) {
    const HostRoutines::Sites& sites = HostSites();
    if (sites.last == 0 || vaddr < sites.first || vaddr > sites.last) [[likely]] {
        return instruction;
    }
    return HostRoutines::InstructionFor(m_parent.m_system, *m_process, m_memory, vaddr,
                                        instruction);
}

namespace {
/// The running core's registers for a host routine (pinned registers are in the state block
/// while a callback runs, and reloaded from it after).
class JitRegisters final : public HostRoutines::Registers {
public:
    explicit JitRegisters(Dynarmic::A64::Jit& jit) : m_jit{jit} {}
    u64 Get(int index) const override {
        return m_jit.GetRegister(static_cast<std::size_t>(index));
    }
    void Set(int index, u64 value) override {
        m_jit.SetRegister(static_cast<std::size_t>(index), value);
    }
    void SetPC(u64 pc) override {
        m_jit.SetPC(pc);
    }
    u32 GetPstate() const override {
        return m_jit.GetPstate();
    }
    void SetPstate(u32 value) override {
        m_jit.SetPstate(value);
    }

private:
    Dynarmic::A64::Jit& m_jit;
};
} // Anonymous namespace

void DynarmicCallbacks64::MemoryWrite(Dynarmic::A64::VAddr vaddr, u64 value, std::size_t size) {
    if (CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Write)) {
        switch (size) {
        case sizeof(u64): return m_memory.Write64(vaddr, u64(value));
        case sizeof(u32): return m_memory.Write32(vaddr, u32(value));
        case sizeof(u16): return m_memory.Write16(vaddr, u16(value));
        case sizeof(u8): return m_memory.Write8(vaddr, u8(value));
        default: UNREACHABLE();
        }
    }
}
void DynarmicCallbacks64::MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value) {
    if (CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write)) {
        m_memory.Write64(vaddr, value[0]);
        m_memory.Write64(vaddr + 8, value[1]);
    }
}

bool DynarmicCallbacks64::MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) {
    return CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive8(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) {
    return CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive16(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) {
    return CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive32(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) {
    return CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive64(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive128(u64 vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) {
    return CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive128(vaddr, value, expected);
}

void DynarmicCallbacks64::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) {
    last_code_addr = u64(-1); //invalidate cached page
    switch (op) {
    case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU: {
        static constexpr u64 ICACHE_LINE_SIZE = 64;
        const u64 cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
        m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
        break;
    }
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
        m_parent.ClearInstructionCache();
        break;
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
    default:
        LOG_DEBUG(Core_ARM, "Unprocesseed instruction cache operation: {}", op);
        break;
    }
    m_parent.m_jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
}

void DynarmicCallbacks64::ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) {
    if (exception == Dynarmic::A64::Exception::Breakpoint) {
        // Fritchii host routines: a BRK compiled in place of a hooked entry's first word.
        const HostRoutines::Sites& sites = HostSites();
        if (sites.last != 0 && pc >= sites.first && pc <= sites.last) {
            JitRegisters regs{*m_parent.m_jit};
            if (HostRoutines::OnBreakpoint(m_parent.m_system, *m_process, m_memory, regs, pc)) {
                return;
            }
        }
    }
    if (m_window_trace && exception == Dynarmic::A64::Exception::Breakpoint) {
        const auto point = WindowTrace::Identify(m_window_trace->base, pc, m_memory.Read32(pc));
        if (point != WindowTrace::Point::None) {
            auto& jit = *m_parent.m_jit;
            const u64 x0 = jit.GetRegister(0);
            const u64 x1 = jit.GetRegister(1);
            u64 native = 0;
            if (point == WindowTrace::Point::WindowInitialize) {
                native = m_memory.Read64(x1 + 0x18);
            } else if (point == WindowTrace::Point::NativeInitialize ||
                       point == WindowTrace::Point::Acquire ||
                       point == WindowTrace::Point::Finalize) {
                native = m_memory.Read64(x0);
            }
            const auto identity = m_window_trace->Record(
                point, m_tpidrro_el0, point == WindowTrace::Point::WindowInitialize ? x0 : 0, native);
            if (!identity) {
                WindowTrace::Resume(point, jit, m_memory);
                return;
            }
            u32 registrations = 0;
            if (identity->native != 0) {
                for (u32 i = 0; i < 8; ++i) {
                    const u64 entry = m_window_trace->base + 0xF633F8 + i * 0x128;
                    registrations += m_memory.Read64(entry) == identity->native;
                }
            }
            LOG_INFO(Core_ARM, "Star Fox window trace session={} {} pc={:#x} lr={:#x} tls={:#x} window={:#x} native={:#x} registrations={} x0={:#x}",
                     m_window_trace->id, WindowTrace::Name(point), pc, jit.GetRegister(30),
                     m_tpidrro_el0, identity->window, identity->native, registrations, x0);
            if (point == WindowTrace::Point::NativeInitialize) {
                const u32 count = static_cast<u32>(jit.GetRegister(2));
                LOG_INFO(Core_ARM, "Star Fox window textures session={} tls={:#x} native={:#x} count={}",
                         m_window_trace->id, m_tpidrro_el0, identity->native, count);
                for (u32 i = 0; i < std::min(count, 4U); ++i) {
                    const u64 texture = x1 + i * 0x28;
                    LOG_INFO(Core_ARM, "Star Fox window texture session={} tls={:#x} native={:#x} index={} {}x{} format={} block_format={} pitch_gobs={} block_height={} fd={} pool_size={:#x} offset={:#x}",
                             m_window_trace->id, m_tpidrro_el0, identity->native,
                             i, m_memory.Read32(texture + 8), m_memory.Read32(texture + 12),
                             m_memory.Read8(texture + 16), m_memory.Read8(texture + 17),
                             m_memory.Read32(texture + 20), m_memory.Read32(texture + 24),
                             m_memory.Read32(texture + 28), m_memory.Read32(texture + 32),
                             m_memory.Read32(texture + 36));
                }
            } else if (point == WindowTrace::Point::ValidateTexture) {
                const u64 texture = jit.GetRegister(25);
                LOG_INFO(Core_ARM, "Star Fox window validation session={} tls={:#x} native={:#x} required_size={:#x} alignment={:#x} pool_size={:#x} offset={:#x}",
                         m_window_trace->id, m_tpidrro_el0, identity->native,
                         m_memory.Read64(jit.GetSP() + 0x10), m_memory.Read32(jit.GetSP() + 0x18),
                         m_memory.Read32(texture + 12), m_memory.Read32(texture + 16));
            } else if (point == WindowTrace::Point::Acquire || point == WindowTrace::Point::Finalize) {
                if (point == WindowTrace::Point::Acquire && registrations == 0) {
                    for (u32 i = 0; i < 8; ++i) {
                        const u64 entry = m_window_trace->base + 0xF633F8 + i * 0x128;
                        LOG_INFO(Core_ARM, "Star Fox window slot session={} tls={:#x} requested={:#x} slot={} native={:#x} active={}",
                                 m_window_trace->id, m_tpidrro_el0, identity->native, i,
                                 m_memory.Read64(entry), m_memory.Read8(entry + 0x120));
                    }
                }
            }
            WindowTrace::Resume(point, jit, m_memory);
            return;
        }
    }
    switch (exception) {
    case Dynarmic::A64::Exception::WaitForInterrupt:
    case Dynarmic::A64::Exception::WaitForEvent:
    case Dynarmic::A64::Exception::SendEvent:
    case Dynarmic::A64::Exception::SendEventLocal:
    case Dynarmic::A64::Exception::Yield:
        if (Kernel::Nx2ThreadStats::g_enabled) {
            Kernel::Nx2ThreadStats::CountHint(
                Kernel::Nx2ThreadStats::t_run,
                static_cast<size_t>(exception) -
                    static_cast<size_t>(Dynarmic::A64::Exception::WaitForInterrupt),
                m_parent.m_system.CoreTiming().GetClockTicks(), m_spin_ticks);
        }
        LOG_TRACE(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X}, cached = {:08X})", std::size_t(exception), pc, m_memory.Read32(pc), MemoryReadCode(pc).value_or(0));
        return;
    case Dynarmic::A64::Exception::NoExecuteFault:
        LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", pc);
        ReturnException(pc, PrefetchAbort);
        return;
    default:
        if (m_debugger_enabled) {
            ReturnException(pc, InstructionBreakpoint);
        } else {
            m_parent.LogBacktrace(m_process);
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})", static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
        }
    }
}

void DynarmicCallbacks64::CallSVC(u32 svc) {
    m_parent.m_svc = svc;
    m_parent.m_jit->HaltExecution(SupervisorCall);
}

void DynarmicCallbacks64::AddTicks(u64 ticks) {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
    // rough approximation of the amount of executed ticks in the system, it may be thrown off
    // if not all cores are doing a similar amount of work. Instead of doing this, we should
    // device a way so that timing is consistent across all cores without increasing the ticks 4
    // times.
    u64 amortized_ticks = ticks / Core::Hardware::NUM_CPU_CORES;
    // Always execute at least one tick.
    amortized_ticks = std::max<u64>(amortized_ticks, 1);
    m_parent.m_system.CoreTiming().AddTicks(amortized_ticks);
}

u64 DynarmicCallbacks64::GetTicksRemaining() {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    return std::max<s64>(m_parent.m_system.CoreTiming().downcount, 0);
}

namespace {
// Guest code that waits by polling the counter (title 1's main thread reads it ~17 million
// times a second while waiting for the GPU) keeps a host core busy at full speed, and the
// GPU thread is often on the other half of that SMT core. Two reads within ~1.7 us of each
// other are such a loop: pause, which hands the core's resources to the sibling thread for a
// moment. The guest sees the same counter. EDEN_NX2_CNTPCT_PAUSE=0 turns it off.
bool CntpctPauseOn() {
    static const bool pause_spins = [] {
        const char* value = std::getenv("EDEN_NX2_CNTPCT_PAUSE");
        return value == nullptr || value[0] != '0';
    }();
    return pause_spins;
}
} // Anonymous namespace

u64 DynarmicCallbacks64::GetCNTPCT() {
    const u64 ticks = m_parent.m_system.CoreTiming().GetClockTicks();
    if (CntpctPauseOn() && ticks - m_last_cntpct < m_spin_ticks) {
        // The guest is polling the clock: a wait, not work (FRITCHII_SIM_CPU_SLOW).
        Common::LowEndSim::CpuPauseNote();
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
    m_last_cntpct = ticks;
    if (Kernel::Nx2ThreadStats::g_enabled) {
        Kernel::Nx2ThreadStats::CountRead(Kernel::Nx2ThreadStats::t_run, ticks, m_spin_ticks);
    }
    return ticks;
}

bool DynarmicCallbacks64::CheckMemoryAccess(u64 addr, u64 size, Kernel::DebugWatchpointType type) {
    if (!m_check_memory_access) {
        return true;
    }

    if (!m_memory.IsValidVirtualAddressRange(addr, size)) {
        LOG_CRITICAL(Core_ARM, "Stopping execution due to unmapped memory access at {:#x}",
                        addr);
        m_parent.m_jit->HaltExecution(PrefetchAbort);
        return false;
    }

    if (!m_debugger_enabled) {
        return true;
    }

    const auto match{m_parent.MatchingWatchpoint(addr, size, type)};
    if (match) {
        m_parent.m_halted_watchpoint = match;
        m_parent.m_jit->HaltExecution(DataAbort);
        return false;
    }

    return true;
}

void DynarmicCallbacks64::ReturnException(u64 pc, Dynarmic::HaltReason hr) {
    m_parent.GetContext(m_parent.m_breakpoint_context);
    m_parent.m_breakpoint_context.pc = pc;
    m_parent.m_jit->HaltExecution(hr);
}

void ArmDynarmic64::MakeJit(Common::PageTable* page_table, std::size_t address_space_bits) {
    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = std::addressof(*m_cb);

    // Memory
    if (page_table) {
        // Dynarmic will not write to the page table, const_cast is safe here
        config.page_table = reinterpret_cast<void**>(
            const_cast<Common::PageTable::PageEntryData*>(page_table->entries.data()));
        config.page_table_address_space_bits = std::uint32_t(address_space_bits);
        config.page_table_pointer_mask = Common::PageTable::ATTRIBUTE_MASK;
        config.page_table_marked_bit = uint8_t(0);
        config.silently_mirror_page_table = false;
        config.absolute_offset_page_table = true;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;

        config.fastmem_pointer = page_table->fastmem_arena ?
            std::optional<uintptr_t>{reinterpret_cast<uintptr_t>(page_table->fastmem_arena)} :
            std::nullopt;
        config.fastmem_address_space_bits = std::uint32_t(address_space_bits);
        config.silently_mirror_fastmem = false;

        config.fastmem_exclusive_access = config.fastmem_pointer != std::nullopt;
        config.recompile_on_exclusive_fastmem_failure = true;

        if (reinterpret_cast<u64>(m_system.DeviceMemory().buffer.BackingBasePointer() +
            Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize()) < (1ULL << 39)) {
            // Systems like FreeBSD allocate memory really low by default, and since we pack our page table entries,
            // we have to manually sign extend when our actual pointer is negative.
            config.page_table_sign_extension = std::uint8_t(Common::PageTable::SIGN_BIT);
        }
    }

    // Multi-process state
    config.processor_id = std::uint8_t(m_core_index);
    config.global_monitor = &m_exclusive_monitor.monitor;
    // Fritchii: where this launch loaded the main image (rtld, then main at +0x20000 in OoT);
    // set before the interfaces are made (KProcess::Initialize), random per launch.
    config.image_base = GetInteger(m_cb->m_process->GetEntryPoint());

    // System registers
    config.tpidrro_el0 = &m_cb->m_tpidrro_el0;
    config.tpidr_el0 = &m_cb->m_tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    // 31.25 MHz for Switch 2 titles (Common::WallClock::SetGuestCNTFRQ, set at boot).
    config.cntfrq_el0 = static_cast<u32>(Common::g_wall_clock.GetGuestCNTFRQ());
    m_cb->m_spin_ticks = config.cntfrq_el0 / 600'000;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Timing
    config.wall_clock_cntpct = m_uses_wall_clock;
    config.enable_cycle_counting = !m_uses_wall_clock;
#if defined(ARCHITECTURE_x86_64)
    // FRITCHII_JIT_INLINE_CNTPCT (boot, default 0): counter reads in the block, not a host call
    // (same value; the polling pause kept). Not with guest thread stats, which count reads in
    // GetCNTPCT.
    if (m_uses_wall_clock && page_table != nullptr && Core::Timing::InlineCntpctEnabled() &&
        !Kernel::Nx2ThreadStats::g_enabled) {
        Core::Timing::RefreshInlineCntpct();
        config.inline_cntpct_factor =
            reinterpret_cast<const std::uint64_t*>(&Core::Timing::g_inline_cntpct_factor);
        config.inline_cntpct_last = CntpctPauseOn() ? &m_cb->m_last_cntpct : nullptr;
        config.inline_cntpct_spin_ticks = static_cast<std::uint32_t>(m_cb->m_spin_ticks);
    }
#endif

    // Code cache size
#if defined(ARCHITECTURE_arm64) || defined(__sun__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
    config.code_cache_size = std::uint32_t(128_MiB);
#else
    config.code_cache_size = std::uint32_t(512_MiB);
#endif

    // FRITCHII_JIT_CACHE_MIB (boot, default 0 = 512 MiB; 512-1536): a larger code cache per core.
    // Precompiles (EDEN_NX2_JIT_WARM, FRITCHII_JIT_BLOCK_LIST) stop while half the space (half
    // the smaller region with FRITCHII_JIT_FAR_CODE) is free, so a block list larger than that
    // is compiled at area loads instead, by every core. Memory is committed as code is written.
    if (const char* value = std::getenv("FRITCHII_JIT_CACHE_MIB"); value != nullptr) {
        const unsigned long mib = std::strtoul(value, nullptr, 10);
        if (mib >= 512 && mib <= 1536) {
            config.code_cache_size = static_cast<std::uint32_t>(mib) << 20;
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_INFO(Core_ARM, "FRITCHII_JIT_CACHE_MIB={}: code cache of {} MiB per core", mib,
                         mib);
            }
        }
    }

    // Allow memory fault handling to work
    if (m_system.DebuggerEnabled()) {
        config.check_halt_on_memory_access = true;
    }

    // null_jit
    if (!page_table) {
        // Don't waste too much memory on null_jit
        config.code_cache_size = std::uint32_t(8_MiB);
    }

    switch (Settings::EffectiveCpuAccuracy()) {
    // Debug mode
    case Settings::CpuAccuracy::Debugging:
        if (!Settings::values.cpuopt_page_tables) {
            config.page_table = nullptr;
        }
        if (!Settings::values.cpuopt_block_linking) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!Settings::values.cpuopt_return_stack_buffer) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        if (!Settings::values.cpuopt_fast_dispatcher) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        if (!Settings::values.cpuopt_context_elimination) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
        }
        if (!Settings::values.cpuopt_const_prop) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
        }
        if (!Settings::values.cpuopt_misc_ir) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
        }
        if (!Settings::values.cpuopt_reduce_misalign_checks) {
            config.only_detect_misalignment_via_page_table_on_page_boundary = false;
        }
        if (!Settings::values.cpuopt_fastmem) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_fastmem_exclusives) {
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_recompile_exclusives) {
            config.recompile_on_exclusive_fastmem_failure = false;
        }
        if (!Settings::values.cpuopt_ignore_memory_aborts) {
            config.check_halt_on_memory_access = true;
        }
        break;
    // Unsafe optimizations
    case Settings::CpuAccuracy::Unsafe:
        config.unsafe_optimizations = true;
        if (!Settings::values.cpuopt_unsafe_host_mmu) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (Settings::values.cpuopt_unsafe_unfuse_fma) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        }
        if (Settings::values.cpuopt_unsafe_reduce_fp_error) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
        }
        if (Settings::values.cpuopt_unsafe_inaccurate_nan) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
        if (Settings::values.cpuopt_unsafe_fastmem_check) {
            config.fastmem_address_space_bits = 64;
        }
        if (Settings::values.cpuopt_unsafe_ignore_global_monitor) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        break;
    // Safe optimisations
    case Settings::CpuAccuracy::Auto:
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        config.fastmem_address_space_bits = 64;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        break;
    // Paranoia mode for debugging optimizations
    case Settings::CpuAccuracy::Paranoid:
        config.unsafe_optimizations = false;
        config.optimizations = Dynarmic::no_optimizations;
        break;
    case Settings::CpuAccuracy::Accurate:
    default:
        break;
    }
    // FRITCHII_JIT_UNSAFE_EXTRA (boot, bits, default 0): Unsafe's float relaxations one at a time on
    // top of the chosen accuracy (Auto already unfuses FMA and ignores the global monitor; the
    // Unsafe sizing run was about +1 fps): 1 = reduced-error FP (estimates), 2 = inaccurate NaNs.
    static const int unsafe_extra = [] {
        const char* value = std::getenv("FRITCHII_JIT_UNSAFE_EXTRA");
        const int bits = value != nullptr ? std::atoi(value) : 0;
        if (bits != 0) {
            LOG_WARNING(Core_ARM, "FRITCHII_JIT_UNSAFE_EXTRA={}: reduced-error FP {}, inaccurate NaN {}",
                        bits, (bits & 1) != 0 ? "on" : "off", (bits & 2) != 0 ? "on" : "off");
        }
        return bits;
    }();
    if (unsafe_extra != 0) {
        config.unsafe_optimizations = true;
        if ((unsafe_extra & 1) != 0) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
        }
        if ((unsafe_extra & 2) != 0) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
    }
    if (!Settings::IsFastmemEnabled()) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }
    // The IR verification pass only asserts, and builds a hash map per compiled block.
    // EDEN_NX2_JIT_FAST_COMPILE=0 keeps it.
    static const bool fast_compile = [] {
        const char* value = std::getenv("EDEN_NX2_JIT_FAST_COMPILE");
        const bool enabled = value == nullptr || value[0] != '0';
        LOG_INFO(Core_ARM, "EDEN_NX2_JIT_FAST_COMPILE: IR verification {}",
                 enabled ? "skipped" : "kept");
        return enabled;
    }();
    if (fast_compile && Settings::EffectiveCpuAccuracy() != Settings::CpuAccuracy::Debugging &&
        Settings::EffectiveCpuAccuracy() != Settings::CpuAccuracy::Paranoid) {
        config.optimizations |= Dynarmic::OptimizationFlag::DisableVerification;
    }
    if (Kernel::Nx2ThreadStats::g_enabled && m_core_index < g_jit_counters.size()) {
        config.counters = &g_jit_counters[m_core_index];
    }
    m_jit.emplace(config);
}

HaltReason ArmDynarmic64::RunThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    if (!Kernel::Nx2ThreadStats::g_enabled || m_core_index >= g_jit_counters.size()) {
        return TranslateHaltReason(m_jit->Run());
    }
    namespace Stats = Kernel::Nx2ThreadStats;
    auto& timing = m_system.CoreTiming();
    Stats::BeginRun();
    const u64 start = timing.GetClockTicks();
    const Dynarmic::HaltReason hr = m_jit->Run();
    const u64 end = timing.GetClockTicks();
    const Stats::Exit exit = Dynarmic::Has(hr, SupervisorCall) ? Stats::Exit::Svc
                             : Dynarmic::Has(hr, BreakLoop)    ? Stats::Exit::Interrupt
                             : Dynarmic::Has(hr, Dynarmic::HaltReason::CacheInvalidation)
                                 ? Stats::Exit::CacheInvalidation
                                 : Stats::Exit::Other;
    const auto& jit = g_jit_counters[m_core_index];
    Stats::EndRun(thread, m_core_index, start, end, exit, m_svc, jit.block_lookups,
                  jit.blocks_compiled, jit.cache_clears);
    return TranslateHaltReason(hr);
}

HaltReason ArmDynarmic64::StepThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

u32 ArmDynarmic64::GetSvcNumber() const {
    return m_svc;
}

void ArmDynarmic64::GetSvcArguments(std::span<uint64_t, 8> args) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        args[i] = j.GetRegister(i);
}

void ArmDynarmic64::SetSvcArguments(std::span<const uint64_t, 8> args) {
    Dynarmic::A64::Jit& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        j.SetRegister(i, args[i]);
}

const Kernel::DebugWatchpoint* ArmDynarmic64::HaltedWatchpoint() const {
    return m_halted_watchpoint;
}

void ArmDynarmic64::RewindBreakpointInstruction() {
    this->SetContext(m_breakpoint_context);
}

ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process, DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor}
    , m_cb(std::in_place, *this, process) // not movable (atomic code_generation)
    , m_core_index{core_index}
{
    auto& page_table = process->GetPageTable().GetBasePageTable();
    auto& page_table_impl = page_table.GetImpl();
    MakeJit(&page_table_impl, page_table.GetAddressSpaceWidth());
}

ArmDynarmic64::~ArmDynarmic64() = default;

void ArmDynarmic64::SetTpidrroEl0(u64 value) {
    m_cb->m_tpidrro_el0 = value;
}

void ArmDynarmic64::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    auto gpr = j.GetRegisters();
    auto fpr = j.GetVectors();
    // TODO: this is inconvenient
    for (size_t i = 0; i < 29; i++)
        ctx.r[i] = gpr[i];
    ctx.fp = gpr[29];
    ctx.lr = gpr[30];
    ctx.sp = j.GetSP();
    ctx.pc = j.GetPC();
    ctx.pstate = j.GetPstate();
    ctx.v = fpr;
    ctx.fpcr = j.GetFpcr();
    ctx.fpsr = j.GetFpsr();
    ctx.tpidr = m_cb->m_tpidr_el0;
}

void ArmDynarmic64::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    Dynarmic::A64::Jit& j = *m_jit;
    // TODO: this is inconvenient
    std::array<u64, 31> gpr;
    for (size_t i = 0; i < 29; i++)
        gpr[i] = ctx.r[i];
    gpr[29] = ctx.fp;
    gpr[30] = ctx.lr;
    j.SetRegisters(gpr);
    j.SetSP(ctx.sp);
    j.SetPC(ctx.pc);
    j.SetPstate(ctx.pstate);
    j.SetVectors(ctx.v);
    j.SetFpcr(ctx.fpcr);
    j.SetFpsr(ctx.fpsr);
    m_cb->m_tpidr_el0 = ctx.tpidr;
}

void ArmDynarmic64::SignalInterrupt(Kernel::KThread* thread) {
    m_jit->HaltExecution(BreakLoop);
}

void ArmDynarmic64::ClearInstructionCache() {
    m_cb->code_generation.fetch_add(1, std::memory_order_release);
    m_jit->ClearCache();
}

std::size_t ArmDynarmic64::WarmCompile(std::span<const u64> keys, const std::atomic<bool>& stop) {
    m_cb->direct_code_reads = true;
    Common::Nx2Hitch::t_warming = true;
    std::size_t done = 0;
    for (const u64 key : keys) {
        if (stop.load(std::memory_order_acquire)) {
            break;
        }
        static_cast<void>(m_jit->PrecompileBlock(key));
        ++done;
    }
    Common::Nx2Hitch::t_warming = false;
    m_cb->direct_code_reads = false;
    return done;
}

namespace {
/// FRITCHII_JIT_HOT_LIST: a text file of "pc fpcr samples" lines (hex, hex, decimal; Fable A's
/// work\fable_a_gc\hotlist-300*.txt). The game's image loads at a random address every launch,
/// so a pc below the image base is an offset from it (the "-rel" lists) and gets this launch's
/// base added; others are taken as absolute (lists from the launch that made them). "{core}" in
/// the path is replaced by the core (Fable A's per-core lists). FRITCHII_JIT_HOT_LIST_ORDER=1
/// keeps the file's order (the per-core lists are hottest tier first), else pc order. Read at
/// every pack (rare): the blocks' location keys.
std::vector<u64> HotPackKeys(u64 image_base, std::size_t core) {
    std::vector<std::pair<u64, u32>> blocks;
    std::string path;
    if (const char* text = std::getenv("FRITCHII_JIT_HOT_LIST"); text != nullptr) {
        path = text;
    }
    if (const std::size_t at = path.find("{core}"); at != std::string::npos) {
        path.replace(at, 6, std::to_string(core));
    }
    std::size_t offsets = 0;
    if (!path.empty()) {
        std::ifstream file{path};
        std::string pc_text, fpcr_text;
        u64 samples = 0;
        while (file >> pc_text >> fpcr_text >> samples) {
            u64 pc = std::strtoull(pc_text.c_str(), nullptr, 16);
            if (pc < image_base) {
                pc += image_base;
                ++offsets;
            }
            blocks.emplace_back(pc, static_cast<u32>(std::strtoul(fpcr_text.c_str(), nullptr, 16)));
        }
    }
    const char* order = std::getenv("FRITCHII_JIT_HOT_LIST_ORDER");
    const bool file_order = order != nullptr && std::atoi(order) == 1;
    if (!file_order) {
        std::sort(blocks.begin(), blocks.end());
    }
    std::vector<u64> result;
    result.reserve(blocks.size());
    for (const auto& [pc, fpcr] : blocks) {
        result.push_back(
            Dynarmic::A64::LocationDescriptor{pc, Dynarmic::FP::FPCR{fpcr}, false}.UniqueHash());
    }
    LOG_INFO(Core_ARM,
             "FRITCHII_JIT_HOT_LIST: core {} {} hot blocks from {} ({} as offsets from the image "
             "base {:#x}, {} absolute), {} order",
             core, result.size(), path, offsets, image_base, result.size() - offsets,
             file_order ? "file" : "pc");
    return result;
}
} // namespace

std::size_t ArmDynarmic64::PackHotCode(const std::atomic<bool>& stop) {
    const std::vector<u64> keys =
        HotPackKeys(GetInteger(m_cb->m_process->GetEntryPoint()), m_core_index);
    if (keys.empty()) {
        return 0;
    }
    // The clear is performed by the first PrecompileBlock (the JIT runs no code here), so the
    // hot blocks are the first code in the emptied cache.
    ClearInstructionCache();
    return WarmCompile(keys, stop);
}

void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {
    // Drop the cached code page too: blocks recompiled after the invalidation would
    // otherwise still decode the old instructions (e.g. a gdbstub breakpoint written
    // into or removed from that page). Usually called from another core's thread (kernel code
    // mapping, nn::ro), so it bumps the generation instead of writing the owner's page tag.
    m_cb->code_generation.fetch_add(1, std::memory_order_release);
    CountCodeInvalidation(m_core_index, addr, size);
    m_jit->InvalidateCacheRange(addr, size);
}

} // namespace Core
