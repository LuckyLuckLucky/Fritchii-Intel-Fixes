// SPDX-FileCopyrightText: Copyright 2026 Fritchii
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

namespace Common::ShutdownWatch {

// Shutdown watch (2026-11-16, a test whose stop sat at 0% CPU forever after the frontend had
// saved its settings, with nothing in the log). From the moment a stop or a close begins, the
// frontend and the teardown name the step they are in. A "FritchiiShutdownWatch" thread logs a
// "Shutdown watch:" block at Critical when no step and no teardown progress
// (Common::ShutdownProgress) came for FRITCHII_SHUTDOWN_WATCH_S seconds (boot, default 5,
// 0 = off): the step, the steps so far with their times, and the hang report's thread part
// (Nx2Hitch::LogThreadReport: stall snapshot and every thread's stack). 20 s later, still stuck,
// FRITCHII_HANG_DUMP=1 adds a minidump. One report per stuck step; read only.
// FRITCHII_SHUTDOWN_EXIT_S (boot, default 0 = off): once the frontend's own close is done
// (Exiting), that many seconds without progress end the process (exit code 2) after the report:
// everything is saved by then, and a process left hanging holds its memory and its window class.

/// Arms the watch (a stop or a close begins), or names the next step if it is armed already.
/// `step` must live for the whole process (a string literal).
void Begin(const char* step);
/// Names the next step (a string literal); nothing while the watch is not armed.
void Step(const char* step);
// INTEL_FORK_CLOSE_WATCHDOG (boot, seconds, default 20 since IntelFix10; 0 = off): once the user closes the window
// (MarkClosing), that many seconds without any step or teardown progress end the process (exit
// code 2) after the report, also while the game's teardown is still running (a long-session close
// sat at 0 % CPU in 'suspending emulation, closing services' for good, 2026-11-20). Game saves and
// the pipeline cache file are written during play, so only a deadlocked teardown is cut short.
/// The window close began (the app ends after this stop): arms INTEL_FORK_CLOSE_WATCHDOG.
void MarkClosing();
/// Names a step after the frontend's close: only the process exit is left.
void Exiting(const char* step);
/// Disarms (a stop finished and the app stays open).
void End();
/// Flushes the log and ends the process at once (exit code 2), without DLL detach: for when
/// nothing can be saved any more (the GPU driver lost the Vulkan device).
[[noreturn]] void EndProcessNow();
/// A safety net for a report made on the way out: ends the process after `delay` whatever the
/// other threads are doing, without flushing the log.
void EndProcessAfter(std::chrono::seconds delay);

} // namespace Common::ShutdownWatch
