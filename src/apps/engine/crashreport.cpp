/*
 * Copyright (c) 2026 The reone project contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "crashreport.h"

#include "reone/system/logger.h"
#include "reone/system/logutil.h"

#ifdef _WIN32

#include <windows.h>
#undef max
#undef min

#include <dbghelp.h>

#include <cstdio>
#include <string>

namespace reone {

namespace {

const char *exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    default: return "EXCEPTION";
    }
}

std::string frameDescription(HANDLE process, DWORD64 address) {
    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] {};
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    std::string text;
    char hex[32];
    std::snprintf(hex, sizeof(hex), "0x%llx", static_cast<unsigned long long>(address));

    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol)) {
        text = symbol->Name;
        // A Release build inlines and reorders; the offset is what lets a
        // suspicious frame be checked against the disassembly rather than
        // trusted on the name alone.
        char off[32];
        std::snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(displacement));
        text += off;
    } else {
        text = hex;
    }

    IMAGEHLP_LINE64 line {};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(process, address, &lineDisplacement, &line) && line.FileName) {
        text += " (";
        text += line.FileName;
        text += ":";
        text += std::to_string(line.LineNumber);
        text += ")";
    }
    return text + " [" + hex + "]";
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS *info) {
    // Everything here runs on a broken process, so it stays inside what is
    // documented as safe from an exception filter and never allocates through
    // anything the fault might have corrupted more than it must.
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto &record = *info->ExceptionRecord;

    char header[256];
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        // Parameter 0 says read (0), write (1) or execute (8); parameter 1 is
        // the address that could not be reached. A null there is a dereferenced
        // null pointer, and a small offset from null is a member of one.
        const char *kind = record.ExceptionInformation[0] == 1   ? "writing"
                           : record.ExceptionInformation[0] == 8 ? "executing"
                                                                 : "reading";
        std::snprintf(header, sizeof(header),
                      "CRASH: ACCESS_VIOLATION %s 0x%llx at 0x%llx", kind,
                      static_cast<unsigned long long>(record.ExceptionInformation[1]),
                      reinterpret_cast<unsigned long long>(record.ExceptionAddress));
    } else {
        std::snprintf(header, sizeof(header), "CRASH: %s (0x%lx) at 0x%llx",
                      exceptionName(record.ExceptionCode),
                      static_cast<unsigned long>(record.ExceptionCode),
                      reinterpret_cast<unsigned long long>(record.ExceptionAddress));
    }
    error(header);

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    // fInvadeProcess deliberately FALSE. Passing TRUE has DbgHelp enumerate and
    // load symbols for every module in the process, and doing that faulted
    // inside SymInitialize on this one - unsurprising, given what is mapped in
    // beside us: Streamline, the NGX plugins, the Vulkan loader, and whatever
    // overlay the machine injects. Losing the crash report to a crash in the
    // crash reporter is the worst available outcome.
    //
    // Only the executable is loaded instead, which is where all of this
    // project's code lives - the libraries are static - so it is every frame
    // worth reading anyway.
    if (!SymInitialize(process, nullptr, FALSE)) {
        error("  (no symbols: SymInitialize failed; addresses only)");
        Logger::instance.flush();
        return EXCEPTION_EXECUTE_HANDLER;
    }
    char exePath[MAX_PATH] {};
    const DWORD exePathLength = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    const auto exeBase = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
    if (exePathLength > 0 && exeBase != 0) {
        SymLoadModuleEx(process, nullptr, exePath, nullptr, exeBase, 0, nullptr, 0);
    }

    // The faulting instruction first, and on its own. It is the one address
    // that is certainly interesting, it needs no unwinding to obtain, and on an
    // optimized build it is often the whole answer.
    error("  at " + frameDescription(process, reinterpret_cast<DWORD64>(record.ExceptionAddress)));

    // RtlCaptureStackBackTrace rather than StackWalk64: an exception filter
    // runs on top of the faulting thread's own stack, so a plain backtrace from
    // here already contains the faulting frame and its callers, and it needs no
    // frame pointer. StackWalk64 seeded from the context record returned
    // nothing at all on this build - x64 unwinds from tables, and /O2 leaves
    // RBP holding something else entirely.
    void *frames[62] {};
    const USHORT captured = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < captured; ++i) {
        error("  #" + std::to_string(i) + " " +
              frameDescription(process, reinterpret_cast<DWORD64>(frames[i])));
    }
    if (captured == 0) {
        error("  (no frames captured)");
    }
    SymCleanup(process);

    // The log is buffered per thread and the process is about to be torn down
    // without unwinding, so this is the only chance to get any of it to disk.
    Logger::instance.flush();
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void installCrashReporter() {
    SetUnhandledExceptionFilter(onUnhandledException);
}

} // namespace reone

#else

namespace reone {

void installCrashReporter() {
}

} // namespace reone

#endif
