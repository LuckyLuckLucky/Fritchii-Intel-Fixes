// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <exception>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/windows/crash_handler.h"

#define LOG_CRASH(...) ::Common::Log::FmtCrashMessage(__FILE__, __LINE__, __func__, __VA_ARGS__)

namespace Common::Windows {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

// Everything the handler needs is resolved at install: loading a DLL from a crashed process can
// deadlock on the loader lock.
MiniDumpWriteDumpFn write_dump{};
GetThreadDescriptionFn get_thread_description{};
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter{};
wchar_t dump_path[MAX_PATH]{};
std::atomic<DWORD> reporting_thread{};

// A stack overflow leaves the crashed thread only the few KiB below its guard page, too little
// for Report() (its frame list, the module lookups, the dump): the process then ended without a
// line or a dump. The crashed thread hands the exception to this reporter thread instead, which
// reports it on a stack of its own.
DWORD reporter_thread{};
HANDLE overflow_request{};
HANDLE overflow_done{};
EXCEPTION_POINTERS* overflow_pointers{};
DWORD overflow_thread{};

struct CodeAddress {
    char module[64];
    unsigned long long offset;
};

CodeAddress Locate(const void* address) {
    CodeAddress result{"?", reinterpret_cast<uintptr_t>(address)};
    HMODULE module{};
    char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        GetModuleFileNameA(module, path, sizeof(path)) == 0) {
        return result;
    }
    const char* slash = std::strrchr(path, '\\');
    std::snprintf(result.module, sizeof(result.module), "%s", slash ? slash + 1 : path);
    result.offset -= reinterpret_cast<uintptr_t>(module);
    return result;
}

// Never inlined: its frame (a 1 KiB line, 40 frames) would make OnUnhandledException's too large
// for a thread that overflowed its stack.
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void Report(EXCEPTION_POINTERS* pointers) {
    // Called on the crashed thread's stack, which the dispatcher's frames sit on top of: the
    // stack from the faulting frame on is the part after the exception address.
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    const void* fault = pointers ? pointers->ExceptionRecord->ExceptionAddress : nullptr;
    const USHORT first =
        pointers ? static_cast<USHORT>(std::find(frames, frames + count, fault) - frames) : 0;
    char stack[1024] = "";
    std::size_t used = 0;
    for (USHORT i = first < count ? first : 0; i < count && frames[i]; ++i) {
        const CodeAddress frame = Locate(frames[i]);
        const int written = std::snprintf(stack + used, sizeof(stack) - used, " %s+%#llx",
                                          frame.module, frame.offset);
        if (written < 0 || used + static_cast<std::size_t>(written) >= sizeof(stack)) {
            break;
        }
        used += static_cast<std::size_t>(written);
    }

    char thread_name[64] = "";
    PWSTR description{};
    if (get_thread_description &&
        SUCCEEDED(get_thread_description(GetCurrentThread(), &description))) {
        WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name) - 1,
                            nullptr, nullptr);
        LocalFree(description);
    }

    if (pointers) {
        const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
        const CodeAddress at = Locate(fault);
        LOG_CRASH("Host crash: unhandled exception {:#010x} at {}+{:#x} on thread {} \"{}\", "
                  "info {:#x} {:#x}; stack:{}",
                  record.ExceptionCode, at.module, at.offset, GetCurrentThreadId(), thread_name,
                  record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0,
                  record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0, stack);
    } else {
        LOG_CRASH("Host crash: abort() on thread {} \"{}\"; stack:{}", GetCurrentThreadId(),
                  thread_name, stack);
    }
    if (const u64 refusals = Common::CommitRefusals(); refusals != 0) {
        LOG_CRASH("Windows' commit limit (RAM plus the page file) refused memory {} times after "
                  "5 s of waiting: this crash is most likely running out of memory",
                  refusals);
    }

    if (!write_dump) {
        return;
    }
    const HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION information{GetCurrentThreadId(), pointers, FALSE};
    // The unloaded modules name what a call through a stale pointer went to.
    const BOOL written = write_dump(
        GetCurrentProcess(), GetCurrentProcessId(), file,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
        pointers ? &information : nullptr, nullptr, nullptr);
    const DWORD error = written ? 0 : GetLastError();
    CloseHandle(file);
    LOG_CRASH("Host crash: minidump of process {} {} ({:#x})", GetCurrentProcessId(),
              written ? "written next to the log as eden_crash.dmp" : "failed", error);
}

// The innermost frames of a walk, and the outermost ones in a ring. A stack overflow is usually
// deep recursion: the innermost frames show the recursing code, the outermost ones where it was
// entered from.
struct StackWalk {
    static constexpr std::size_t inner_size = 20;
    static constexpr std::size_t outer_size = 12;
    DWORD64 inner[inner_size]{};
    DWORD64 outer[outer_size]{};
    std::size_t frames = 0;

    void Add(DWORD64 address) {
        if (frames < inner_size) {
            inner[frames] = address;
        } else {
            outer[(frames - inner_size) % outer_size] = address;
        }
        ++frames;
    }
};

#if defined(_M_X64) || defined(__x86_64__)
// Whether an address is in a loaded module's code. Asks the kernel rather than the loader, whose
// locks the crashed thread may hold.
bool InModuleCode(DWORD64 address) {
    MEMORY_BASIC_INFORMATION code{};
    return VirtualQuery(reinterpret_cast<const void*>(address), &code, sizeof(code)) != 0 &&
           code.State == MEM_COMMIT && code.Type == MEM_IMAGE &&
           (code.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) !=
               0;
}

// Walks the overflowed thread's stack from the exception's context, while that thread waits in
// the filter below the frames walked. Ends at code outside the loaded modules (JIT code, whose
// unwind data describes only the dispatcher), at a frame without unwind data past the first,
// and when the stack pointer leaves [low, high) or stops growing.
void WalkUnguarded(CONTEXT context, DWORD64 low, DWORD64 high, StackWalk& walk) {
    constexpr std::size_t frame_limit = std::size_t{1} << 20;
    while (walk.frames < frame_limit) {
        walk.Add(context.Rip);
        if (!InModuleCode(context.Rip)) {
            return;
        }
        const DWORD64 rsp = context.Rsp;
        DWORD64 image_base{};
        const PRUNTIME_FUNCTION function =
            RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function != nullptr) {
            PVOID handler_data{};
            DWORD64 establisher_frame{};
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                             &handler_data, &establisher_frame, nullptr);
        } else if (walk.frames == 1 && rsp >= low && rsp + 8 <= high) {
            // A leaf function: its return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64*>(rsp);
            context.Rsp = rsp + 8;
        } else {
            return;
        }
        if (context.Rip == 0 || context.Rsp <= rsp || context.Rsp < low ||
            context.Rsp + 8 > high) {
            return;
        }
    }
}
#endif

// The walk, guarded where the compiler has structured exception handling: unwind data that does
// not match the code can send the unwinder anywhere, and the report must still be written.
void Walk(const EXCEPTION_POINTERS& pointers, StackWalk& walk) {
#if defined(_M_X64) || defined(__x86_64__)
    const CONTEXT& context = *pointers.ContextRecord;
    MEMORY_BASIC_INFORMATION stack{};
    if (VirtualQuery(reinterpret_cast<const void*>(context.Rsp), &stack, sizeof(stack)) == 0 ||
        stack.State != MEM_COMMIT) {
        walk.Add(context.Rip);
        return;
    }
    const DWORD64 low = context.Rsp;
    const DWORD64 high = reinterpret_cast<DWORD64>(stack.BaseAddress) + stack.RegionSize;
#ifdef _MSC_VER
    __try {
        WalkUnguarded(context, low, high, walk);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#else
    walk.Add(context.Rip); // No guard: the faulting address only.
    static_cast<void>(low);
    static_cast<void>(high);
#endif
#else
    walk.Add(reinterpret_cast<std::uintptr_t>(pointers.ExceptionRecord->ExceptionAddress));
#endif
}

// Appends " module+offset" for each address that fits.
void AppendFrames(char* text, std::size_t size, const DWORD64* addresses, std::size_t count) {
    std::size_t used = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const CodeAddress frame =
            Locate(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(addresses[i])));
        const int written =
            std::snprintf(text + used, size - used, " %s+%#llx", frame.module, frame.offset);
        if (written < 0 || used + static_cast<std::size_t>(written) >= size) {
            text[used] = '\0';
            return;
        }
        used += static_cast<std::size_t>(written);
    }
}

// The reporter thread: Report()'s line and dump for the stack overflow a crashed thread handed
// over, with that thread's stack walked from the exception's context.
void ReportOverflow() {
    EXCEPTION_POINTERS* const pointers = overflow_pointers;
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;

    StackWalk walk;
    Walk(*pointers, walk);
    char inner[1024] = "";
    AppendFrames(inner, sizeof(inner), walk.inner, (std::min)(walk.frames, StackWalk::inner_size));
    const std::size_t outer_count =
        walk.frames > StackWalk::inner_size
            ? (std::min)(walk.frames - StackWalk::inner_size, StackWalk::outer_size)
            : 0;
    DWORD64 outermost[StackWalk::outer_size]{};
    for (std::size_t i = 0; i < outer_count; ++i) {
        const std::size_t frame = walk.frames - outer_count + i;
        outermost[i] = walk.outer[(frame - StackWalk::inner_size) % StackWalk::outer_size];
    }
    char outer[640] = "";
    AppendFrames(outer, sizeof(outer), outermost, outer_count);

    char thread_name[64] = "";
    if (const HANDLE thread =
            OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, overflow_thread)) {
        PWSTR description{};
        if (get_thread_description && SUCCEEDED(get_thread_description(thread, &description))) {
            WideCharToMultiByte(CP_UTF8, 0, description, -1, thread_name, sizeof(thread_name) - 1,
                                nullptr, nullptr);
            LocalFree(description);
        }
        CloseHandle(thread);
    }

    const CodeAddress at = Locate(record.ExceptionAddress);
    LOG_CRASH("Host crash: unhandled exception {:#010x} (stack overflow) at {}+{:#x} on thread {} "
              "\"{}\", info {:#x} {:#x}; reported by thread {} \"CrashReporter\"; stack ({} "
              "frames):{}{}{}",
              record.ExceptionCode, at.module, at.offset, overflow_thread, thread_name,
              record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0,
              record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0,
              GetCurrentThreadId(), walk.frames, inner,
              walk.frames > StackWalk::inner_size + StackWalk::outer_size ? " ..." : "", outer);

    if (!write_dump) {
        return;
    }
    const HANDLE file = CreateFileW(dump_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    MINIDUMP_EXCEPTION_INFORMATION information{overflow_thread, pointers, FALSE};
    const BOOL written = write_dump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                    MiniDumpWithThreadInfo, &information, nullptr, nullptr);
    const DWORD error = written ? 0 : GetLastError();
    CloseHandle(file);
    LOG_CRASH("Host crash: minidump of process {} {} ({:#x})", GetCurrentProcessId(),
              written ? "written next to the log as eden_crash.dmp" : "failed", error);
}

DWORD WINAPI ReporterMain(void*) {
    // The C runtime gives a thread its own data from the heap on first use: not during a report.
    errno = 0;
    // Only a crashed thread sets the event, after storing its pointers. Any other wake (a failed
    // wait) ends the thread rather than report nothing.
    while (WaitForSingleObject(overflow_request, INFINITE) == WAIT_OBJECT_0 &&
           overflow_pointers != nullptr) {
        ReportOverflow();
        SetEvent(overflow_done);
    }
    return 0;
}

// Without the reporter thread a stack overflow is reported by the crashed thread, as before.
void StartReporter() {
    overflow_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    overflow_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD id = 0;
    const HANDLE thread = overflow_request && overflow_done
                              ? CreateThread(nullptr, 512 * 1024, ReporterMain, nullptr,
                                             STACK_SIZE_PARAM_IS_A_RESERVATION, &id)
                              : nullptr;
    if (!thread) {
        if (overflow_request) {
            CloseHandle(overflow_request);
        }
        if (overflow_done) {
            CloseHandle(overflow_done);
        }
        overflow_request = nullptr;
        overflow_done = nullptr;
        return;
    }
    if (const auto set_thread_description = reinterpret_cast<SetThreadDescriptionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"KernelBase.dll"),
                                                   "SetThreadDescription")))) {
        set_thread_description(thread, L"CrashReporter");
    }
    CloseHandle(thread);
    reporter_thread = id;
}

// Only the first thread to crash reports. A second one waits a while for it to end the process,
// then falls through to the previous handling, so a report stuck on a lock the second thread
// holds cannot hang the process. A crash inside the report, the reporter thread's included, falls
// through at once.
bool BeginReport() {
    DWORD expected = 0;
    const DWORD self = GetCurrentThreadId();
    if (self == reporter_thread) {
        return false;
    }
    if (reporting_thread.compare_exchange_strong(expected, self)) {
        return true;
    }
    if (expected != self) {
        Sleep(10000);
    }
    return false;
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* pointers) {
    if (BeginReport()) {
        if (pointers->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW &&
            reporter_thread != 0) {
            // Nothing here may need much stack. The wait is bounded, so the process still ends
            // if the reporter blocks on a lock this thread holds.
            overflow_pointers = pointers;
            overflow_thread = GetCurrentThreadId();
            SetEvent(overflow_request);
            WaitForSingleObject(overflow_done, 30000);
        } else {
            Report(pointers);
        }
    }
    return previous_filter ? previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

// std::terminate (an uncaught C++ exception, also on a noexcept thread entry) and failed fatal
// asserts end in abort(), which the C runtime turns into a fail-fast that skips the filter above.
void OnAbort(int) {
    if (BeginReport()) {
        Report(nullptr);
    }
}

// FRITCHII_CRASH_FIRST_CHANCE (boot env, default 0; a diagnostic): an exception in host code
// called from JIT code (a memory callback, an SVC, a soft float helper) unwinds into the JIT's
// frame, whose unwind data cannot describe it, and the process ends without reaching the filter
// above. This vectored handler logs the first exceptions whose address is inside a loaded module
// (JIT code is in none, so its fastmem faults are not logged) before any handler runs, then lets
// the search go on as before.
std::atomic<int> first_chance_logged{0};

LONG WINAPI OnFirstChance(EXCEPTION_POINTERS* pointers) {
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    switch (record.ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
    case 0xE06D7363: // a C++ throw
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HMODULE module{};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(record.ExceptionAddress), &module)) {
        return EXCEPTION_CONTINUE_SEARCH; // JIT code (fastmem faults) or no module
    }
    if (first_chance_logged.fetch_add(1, std::memory_order_relaxed) >= 16) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const CodeAddress at = Locate(record.ExceptionAddress);
    // A C++ throw (MSVC x64): name its types, and what() for a std::exception.
    char types[256] = "";
    const char* what = "";
    if (record.ExceptionCode == 0xE06D7363 && record.NumberParameters >= 4 &&
        record.ExceptionInformation[2] != 0) {
        const auto base = static_cast<uintptr_t>(record.ExceptionInformation[3]);
        const auto* throw_info = reinterpret_cast<const int*>(record.ExceptionInformation[2]);
        const auto* type_array = reinterpret_cast<const int*>(base + unsigned(throw_info[3]));
        std::size_t used = 0;
        for (int i = 0; i < type_array[0] && i < 8; ++i) {
            const auto* catchable = reinterpret_cast<const int*>(base + unsigned(type_array[1 + i]));
            // CatchableType: properties, pType, PMD {mdisp, pdisp, vdisp}, size, copy.
            const char* name =
                reinterpret_cast<const char*>(base + unsigned(catchable[1])) + 2 * sizeof(void*);
            const int written = std::snprintf(types + used, sizeof(types) - used, "%s%s",
                                              i == 0 ? "" : " ", name);
            if (written < 0 || used + static_cast<std::size_t>(written) >= sizeof(types)) {
                break;
            }
            used += static_cast<std::size_t>(written);
            if (std::strcmp(name, ".?AVexception@std@@") == 0 && catchable[3] == -1) {
                const auto object = static_cast<uintptr_t>(record.ExceptionInformation[1]);
                what = reinterpret_cast<const std::exception*>(object + unsigned(catchable[2]))->what();
            }
        }
    }
    LOG_CRASH("First-chance exception (FRITCHII_CRASH_FIRST_CHANCE) {:#010x} at {}+{:#x} on thread "
              "{}, info {:#x} {:#x}, rsp {:#x}; C++ types: {}; what: {}",
              record.ExceptionCode, at.module, at.offset, GetCurrentThreadId(),
              record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0,
              record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0,
              pointers->ContextRecord->Rsp, types, what);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // Anonymous namespace

void InstallCrashHandler(const std::filesystem::path& dump_directory) {
    const std::wstring path = (dump_directory / "eden_crash.dmp").wstring();
    if (path.size() < MAX_PATH) {
        std::wmemcpy(dump_path, path.c_str(), path.size() + 1);
        if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
            write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
    }
    get_thread_description = reinterpret_cast<GetThreadDescriptionFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"KernelBase.dll"), "GetThreadDescription")));
    if (reporter_thread == 0) {
        StartReporter();
    }
    previous_filter = SetUnhandledExceptionFilter(OnUnhandledException);
    std::signal(SIGABRT, OnAbort);
    if (const char* value = std::getenv("FRITCHII_CRASH_FIRST_CHANCE");
        value != nullptr && value[0] != '\0' && value[0] != '0') {
        AddVectoredExceptionHandler(1, OnFirstChance);
    }
}

std::string WriteHangDump(const std::filesystem::path& directory) {
    static std::atomic<int> written_count{0};
    const int number = written_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (number > 3) {
        return "not written: three hang dumps already this session";
    }
    // Not a crash: loading dbghelp here is safe (the crash handler resolves it at install, when
    // it is installed at all).
    MiniDumpWriteDumpFn writer = write_dump;
    if (writer == nullptr) {
        if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
            writer = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(GetProcAddress(dbghelp, "MiniDumpWriteDump")));
        }
    }
    if (writer == nullptr) {
        return "not written: dbghelp.dll or MiniDumpWriteDump not found";
    }
    const std::filesystem::path path =
        directory / ("fritchii_hang_" + std::to_string(GetCurrentProcessId()) + "_" +
                     std::to_string(number) + ".dmp");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return "not written: could not create " + path.string() + " (error " +
               std::to_string(GetLastError()) + ")";
    }
    // Stacks of every thread (MiniDumpNormal), plus their times and start addresses and the
    // modules unloaded before: a few MiB, no game memory.
    const auto type =
        static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const BOOL written = writer(GetCurrentProcess(), GetCurrentProcessId(), file, type, nullptr,
                                nullptr, nullptr);
    const DWORD error = written ? 0 : GetLastError();
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(path.c_str());
        return "failed (error " + std::to_string(error) + ")";
    }
    return "written: " + path.string() + " (" + std::to_string(size.QuadPart >> 10) + " KiB)";
}

void GuaranteeStackForCrashReport() {
    ULONG_PTR low{};
    ULONG_PTR high{};
    GetCurrentThreadStackLimits(&low, &high);
    if (high - low >= 2 * 1024 * 1024) {
        ULONG bytes = 64 * 1024;
        SetThreadStackGuarantee(&bytes);
    }
}

namespace {

constexpr int ThreadStackDepth = 24;

#if defined(_M_X64) || defined(__x86_64__)
/// Unwinds a copy of a thread's context taken while it was suspended. The thread runs again by
/// then (so no unwinder lock can be held by a suspended thread), which keeps the stack of a
/// waiting thread exact; a busy thread's may be stale past its first frame.
int UnwindThreadUnguarded(CONTEXT& context, DWORD64* frames) {
    int depth = 0;
    while (depth < ThreadStackDepth && context.Rip != 0) {
        frames[depth++] = context.Rip;
        DWORD64 image_base{};
        const PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            // A leaf without unwind data has its return address on top. Past the first frame
            // this is code without unwind data (JIT): stop there.
            if (depth > 1) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
            continue;
        }
        PVOID handler_data{};
        DWORD64 establisher_frame{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher_frame, nullptr);
    }
    return depth;
}

/// `frames` holds ThreadStackDepth + 1 zeroed entries; a fault keeps the frames found before it.
int UnwindThread(CONTEXT& context, DWORD64* frames) {
    int depth = 0;
#ifdef _MSC_VER
    __try {
        depth = UnwindThreadUnguarded(context, frames);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        depth = 0;
        while (depth < ThreadStackDepth && frames[depth] != 0) {
            ++depth;
        }
    }
#else
    frames[0] = context.Rip; // No guard: the current frame only.
    depth = 1;
#endif
    return depth;
}
#endif

std::string ThreadName(HANDLE thread) {
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                               "GetThreadDescription")));
    std::string name;
    PWSTR wide = nullptr;
    if (get_description != nullptr && SUCCEEDED(get_description(thread, &wide)) &&
        wide != nullptr) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            name.resize(static_cast<size_t>(size - 1));
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), size, nullptr, nullptr);
        }
        LocalFree(wide);
    }
    return name;
}

} // Anonymous namespace

std::vector<std::string> ThreadStackLines(unsigned long main_thread_id) {
    std::vector<std::string> lines;
    const DWORD process = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    std::vector<DWORD> ids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        lines.push_back("no thread list (error " + std::to_string(GetLastError()) + ")");
        return lines;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) {
            ids.push_back(entry.th32ThreadID);
        }
    }
    CloseHandle(snapshot);

    // Threads with the same stack share a line: worker pools wait in the same place.
    struct Group {
        std::string threads;
        size_t count{};
    };
    std::map<std::string, Group> groups;
    std::vector<std::string> order;
    std::vector<DWORD64> frames(ThreadStackDepth + 1);
    size_t unreadable = 0;
    for (const DWORD id : ids) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                             THREAD_QUERY_LIMITED_INFORMATION,
                                         FALSE, id);
        if (thread == nullptr) {
            ++unreadable;
            continue;
        }
        std::string name = ThreadName(thread);
        if (id == main_thread_id) {
            name = name.empty() ? std::string{"main"} : "main " + name;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        double cpu_ms = 0.0;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
            const auto value = [](const FILETIME& time) {
                return (static_cast<unsigned long long>(time.dwHighDateTime) << 32) |
                       time.dwLowDateTime;
            };
            cpu_ms = static_cast<double>(value(kernel) + value(user)) / 1e4;
        }
        std::fill(frames.begin(), frames.end(), DWORD64{0});
        int depth = 0;
#if defined(_M_X64) || defined(__x86_64__)
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        bool have_context = false;
        // Nothing between the suspend and the resume allocates: the thread may hold a heap lock.
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            have_context = GetThreadContext(thread, &context) != FALSE;
            ResumeThread(thread);
        }
        if (have_context) {
            depth = UnwindThread(context, frames.data());
        }
#endif
        CloseHandle(thread);

        std::string stack;
        for (int i = 0; i < depth; ++i) {
            const CodeAddress at =
                Locate(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(frames[i])));
            char text[96];
            std::snprintf(text, sizeof(text), "%s%s+0x%llx", i == 0 ? "" : " < ", at.module,
                          at.offset);
            stack += text;
        }
        if (stack.empty()) {
            stack = "(no stack)";
        }
        Group& group = groups[stack];
        if (group.count == 0) {
            order.push_back(stack);
        }
        char label[160];
        std::snprintf(label, sizeof(label), "%s%s %lu (%.0f ms CPU)", group.count != 0 ? ", " : "",
                      name.empty() ? "unnamed" : name.c_str(), static_cast<unsigned long>(id),
                      cpu_ms);
        group.threads += label;
        ++group.count;
    }
    for (const std::string& stack : order) {
        const Group& group = groups[stack];
        lines.push_back(std::to_string(group.count) +
                        (group.count == 1 ? " thread [" : " threads [") + group.threads +
                        "]: " + stack);
    }
    if (unreadable != 0) {
        lines.push_back(std::to_string(unreadable) + " threads could not be opened");
    }
    return lines;
}

namespace {

constexpr const wchar_t* DumpProcessVariable = L"FRITCHII_DUMP_PROCESS";
constexpr const wchar_t* DumpPathVariable = L"FRITCHII_DUMP_PATH";
/// How long the helper may take: while it writes, every thread of the process it dumps is stopped,
/// so only the helper can end that process if the write gets stuck.
constexpr DWORD DumpHelperLimitMs = 15000;

HANDLE dump_target{};

DWORD WINAPI DumpHelperLimit(void*) {
    Sleep(DumpHelperLimitMs);
    TerminateProcess(dump_target, 2);
    ExitProcess(5);
}

} // Anonymous namespace

bool RunDumpHelper(int& exit_code) {
    wchar_t process_text[16]{};
    const DWORD length = GetEnvironmentVariableW(DumpProcessVariable, process_text, 16);
    if (length == 0 || length >= 16) {
        return false;
    }
    exit_code = 1;
    wchar_t path[MAX_PATH]{};
    const DWORD path_length = GetEnvironmentVariableW(DumpPathVariable, path, MAX_PATH);
    if (path_length == 0 || path_length >= MAX_PATH) {
        return true;
    }
    const DWORD process_id = std::wcstoul(process_text, nullptr, 10);
    dump_target = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE |
                                  PROCESS_TERMINATE,
                              FALSE, process_id);
    if (dump_target == nullptr) {
        exit_code = 2;
        return true;
    }
    if (const HANDLE limit = CreateThread(nullptr, 0, DumpHelperLimit, nullptr, 0, nullptr)) {
        CloseHandle(limit);
    }
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    const auto writer = dbghelp != nullptr
                            ? reinterpret_cast<MiniDumpWriteDumpFn>(reinterpret_cast<void*>(
                                  GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                            : nullptr;
    if (writer == nullptr) {
        exit_code = 3;
        return true;
    }
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        exit_code = 4;
        return true;
    }
    const BOOL written =
        writer(dump_target, process_id, file,
               static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
               nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (!written) {
        DeleteFileW(path);
    }
    exit_code = written ? 0 : 6;
    return true;
}

void ReportFatalError(const char* what) {
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    DWORD64 addresses[40]{};
    for (USHORT i = 0; i < count; ++i) {
        addresses[i] = reinterpret_cast<std::uintptr_t>(frames[i]);
    }
    char stack[1024] = "";
    AppendFrames(stack, sizeof(stack), addresses, count);
    LOG_CRASH("Host crash: {} on thread {} \"{}\"; stack:{}", what, GetCurrentThreadId(),
              ThreadName(GetCurrentThread()), stack);

    if (dump_path[0] == L'\0') {
        LOG_CRASH("Host crash: no minidump (the crash handler has no place for it)");
        return;
    }
    std::wstring exe(32768, L'\0');
    const DWORD exe_length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (exe_length == 0 || exe_length >= exe.size()) {
        LOG_CRASH("Host crash: no minidump (the program's path is unknown, error {})",
                  GetLastError());
        return;
    }
    exe.resize(exe_length);
    std::wstring command = L"\"" + exe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION helper{};
    if (!SetEnvironmentVariableW(DumpProcessVariable,
                                 std::to_wstring(GetCurrentProcessId()).c_str()) ||
        !SetEnvironmentVariableW(DumpPathVariable, dump_path) ||
        !CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &helper)) {
        LOG_CRASH("Host crash: no minidump (its helper process did not start, error {})",
                  GetLastError());
        return;
    }
    CloseHandle(helper.hThread);
    // Never ended from here: it may have this process's threads stopped, and only it resumes them.
    const DWORD waited = WaitForSingleObject(helper.hProcess, DumpHelperLimitMs + 5000);
    DWORD helper_code = 0;
    GetExitCodeProcess(helper.hProcess, &helper_code);
    CloseHandle(helper.hProcess);
    if (waited == WAIT_OBJECT_0 && helper_code == 0) {
        LOG_CRASH("Host crash: minidump of process {} written next to the log as eden_crash.dmp "
                  "by a helper process",
                  GetCurrentProcessId());
    } else {
        LOG_CRASH("Host crash: no minidump of process {} (helper {}, code {})",
                  GetCurrentProcessId(), waited == WAIT_OBJECT_0 ? "failed" : "still running",
                  helper_code);
    }
}

} // namespace Common::Windows
