// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/linux/crash_handler.h"

#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <execinfo.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "common/logging.h"

namespace Common::Linux {

namespace {

int g_log_fd = -1;
std::atomic<bool> g_in_handler{false};
constexpr std::array<int, 5> CrashSignals{SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};

/// Async-signal-safe text building: no allocation, no stdio.
struct Line {
    char text[512];
    std::size_t size = 0;

    void Add(const char* s) {
        while (*s != '\0' && size < sizeof(text) - 1) {
            text[size++] = *s++;
        }
    }
    void AddUnsigned(std::uint64_t value, unsigned base) {
        char digits[24];
        std::size_t count = 0;
        do {
            const unsigned digit = static_cast<unsigned>(value % base);
            digits[count++] = static_cast<char>(digit < 10 ? '0' + digit : 'a' + digit - 10);
            value /= base;
        } while (value != 0 && count < sizeof(digits));
        while (count != 0 && size < sizeof(text) - 1) {
            text[size++] = digits[--count];
        }
    }
};

void WriteAll(int fd, const char* data, std::size_t size) {
    while (fd >= 0 && size != 0) {
        const ssize_t written = ::write(fd, data, size);
        if (written <= 0) {
            return;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

const char* SignalName(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV (bad memory access)";
    case SIGBUS:
        return "SIGBUS (bad memory access)";
    case SIGILL:
        return "SIGILL (illegal instruction)";
    case SIGFPE:
        return "SIGFPE (arithmetic error)";
    case SIGABRT:
        return "SIGABRT (abort: an assert, or glibc found the heap corrupted)";
    default:
        return "signal";
    }
}

void OnCrash(int sig, siginfo_t* info, void*) {
    // A second crash while reporting (or a crash in another thread at the same time): straight
    // to the default action.
    if (!g_in_handler.exchange(true)) {
        Line line;
        line.Add("\n[crash] Host crash: ");
        line.Add(SignalName(sig));
        line.Add(" (signal ");
        line.AddUnsigned(static_cast<std::uint64_t>(sig), 10);
        line.Add(")");
        if (sig != SIGABRT && info != nullptr) {
            line.Add(" at address 0x");
            line.AddUnsigned(reinterpret_cast<std::uintptr_t>(info->si_addr), 16);
        }
        char name[17]{};
        if (::prctl(PR_GET_NAME, name, 0, 0, 0) == 0) {
            line.Add(" on thread \"");
            line.Add(name);
            line.Add("\"");
        }
        line.Add("; stack (binary+offset, resolve with the unstripped build):\n");
        void* frames[64];
        const int count = ::backtrace(frames, 64);
        for (const int fd : {g_log_fd, STDERR_FILENO}) {
            WriteAll(fd, line.text, line.size);
            ::backtrace_symbols_fd(frames, count, fd);
        }
        if (g_log_fd >= 0) {
            ::fsync(g_log_fd);
        }
    }
    // Default action now (core dump): the JIT's handler may have been the one the kernel called
    // and this one only its chained fallback, so reset explicitly instead of SA_RESETHAND.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

void LogFatalError(const char* what) {
    Line line;
    line.Add("\n[crash] Host crash: ");
    line.Add(what);
    char name[17]{};
    if (::prctl(PR_GET_NAME, name, 0, 0, 0) == 0) {
        line.Add(" on thread \"");
        line.Add(name);
        line.Add("\"");
    }
    line.Add("; stack (binary+offset, resolve with the unstripped build):\n");
    void* frames[64];
    const int count = ::backtrace(frames, 64);
    for (const int fd : {g_log_fd, STDERR_FILENO}) {
        if (fd >= 0) {
            WriteAll(fd, line.text, line.size);
            ::backtrace_symbols_fd(frames, count, fd);
        }
    }
}

void InstallCrashHandler() {
    if (const auto path = Common::Log::FilePath(); !path.empty()) {
        g_log_fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    // backtrace() loads libgcc on first use (an allocation): do it now, not inside a crash.
    void* warm[1];
    static_cast<void>(::backtrace(warm, 1));
    // A stack overflow on this (the main) thread still reports: its own small stack for the
    // handler. Other threads report everything else.
    static std::array<char, 64 * 1024> alternate_stack;
    stack_t stack{};
    stack.ss_sp = alternate_stack.data();
    stack.ss_size = alternate_stack.size();
    ::sigaltstack(&stack, nullptr);

    struct sigaction action {};
    action.sa_sigaction = &OnCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int sig : CrashSignals) {
        ::sigaction(sig, &action, nullptr);
    }
    LOG_INFO(Common, "Crash handler: crash signals write a Host crash line and stack to this log");
}

} // namespace Common::Linux
