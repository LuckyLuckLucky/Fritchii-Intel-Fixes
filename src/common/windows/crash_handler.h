// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Common::Windows {

/// On an unhandled exception or abort(), logs one Critical "Host crash" line (code, faulting
/// address and stack as module+offset, thread), flushes the log and writes
/// <dump_directory>/eden_crash.dmp. A stack overflow leaves the crashed thread too little stack
/// for that: a reporter thread the handler starts writes both then. Call after
/// Common::Log::Initialize.
void InstallCrashHandler(const std::filesystem::path& dump_directory);

/// Keeps 64 KiB of the calling thread's stack back for a stack overflow, on a stack of 2 MiB or
/// more. Without it Windows raises the overflow with about a page left, which the exception's
/// dispatch can use up before it reaches the handler: the process then ends without a report.
void GuaranteeStackForCrashReport();

/// The hang report's minidump (FRITCHII_HANG_DUMP=1): every thread's stack and state, no heap,
/// written to <directory>/fritchii_hang_<pid>_<n>.dmp while the process keeps running (dbghelp
/// suspends the other threads for the write). At most three a process. Returns what to log.
std::string WriteHangDump(const std::filesystem::path& directory);

/// A fatal error the process ends itself for (the GPU driver lost the Vulkan device), where the
/// exception route is not taken: logs one Critical "Host crash" line with the calling thread's
/// stack, and has a helper process write <dump_directory>/eden_crash.dmp. From another process no
/// lock held by a stopped thread here can block the dump. Waits for the helper for at most 20 s,
/// and the helper ends this process itself if the dump gets stuck, so this can't hang.
void ReportFatalError(const char* what);

/// main(), before anything else: true in the helper process ReportFatalError starts, which has
/// then done its work (return `exit_code` from main).
[[nodiscard]] bool RunDumpHelper(int& exit_code);

/// The hang report's and the shutdown watch's thread list: one line per stack, threads with the
/// same stack on one line ("2 threads [name id (CPU ms), ...]: module+offset < caller ..."),
/// x64 frames as module+offset (resolve them with the build's PDB, e.g. llvm-symbolizer). Each
/// thread is suspended only to read its registers; the unwind runs after it resumes, so a
/// waiting thread's stack is exact. `main_thread_id` is labelled "main".
std::vector<std::string> ThreadStackLines(unsigned long main_thread_id);

} // namespace Common::Windows
