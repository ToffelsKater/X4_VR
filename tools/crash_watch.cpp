// External crash recorder: runs the game as a debuggee and logs modules, exceptions, debug strings
// and a minidump on an unhandled exception. The dev build crash_watch_dev.exe (X4VR_DEV_TOOLS) adds
// hardware watchpoints, execution traces and --startup-module (a DLL loaded into this recorder's
// newly created child only, never a live game); the download ships only the plain recorder.
#include <windows.h>
#include <dbghelp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <array>
#include <sstream>
#ifdef X4VR_DEV_TOOLS
#include "startup_module.hpp"
#endif

struct Handle {
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void module(std::ofstream& log, const char* label, const void* base, HANDLE file) {
    wchar_t path[32768]{};
    if (file) GetFinalPathNameByHandleW(file, path, 32768, FILE_NAME_NORMALIZED);
    log << label << " base=0x" << std::hex << reinterpret_cast<uintptr_t>(base) << std::dec
        << " path=" << std::filesystem::path(path).generic_string() << '\n';
    if (file) CloseHandle(file);
}
#ifdef X4VR_DEV_TOOLS
// Optional hardware watchpoints, armed from <report>/watch.txt lines
// "0x<address> <r|w>" (max 4, 8-byte aligned). Logs accessor RIP counts.
struct Watch {
    std::map<DWORD, HANDLE> threads;
    std::array<uintptr_t, 4> address{};
    std::array<bool, 4> write_only{};
    std::map<std::tuple<int, uintptr_t, uintptr_t>, unsigned> hits; // register, accessor RIP, [rsp] (caller of a leaf accessor)
    unsigned total{};
    bool armed{};
    ULONGLONG checked{}, reported{};
    void apply(HANDLE thread) const {
        const bool suspend = SuspendThread(thread) != DWORD(-1);
        CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(thread, &context)) {
            context.Dr0 = address[0]; context.Dr1 = address[1]; context.Dr2 = address[2]; context.Dr3 = address[3];
            DWORD64 dr7 = 0;
            for (int i = 0; i < 4; ++i) if (armed && address[i])
                dr7 |= (1ull << (i*2)) | (DWORD64(write_only[i] ? 1 : 3) << (16+i*4)) | (2ull << (18+i*4));
            context.Dr7 = dr7; context.Dr6 = 0;
            SetThreadContext(thread, &context);
        }
        if (suspend) ResumeThread(thread);
    }
    void poll(const std::filesystem::path& directory, std::ofstream& log) {
        const auto now = GetTickCount64();
        if (now-checked < 500) return;
        checked = now;
        if (armed && now-reported > 2000) { report(log); reported = now; }
        std::ifstream file(directory/"watch.txt");
        if (!file) return;
        std::array<uintptr_t, 4> next{}; std::array<bool, 4> write{};
        std::string line; int count = 0;
        while (count < 4 && std::getline(file, line)) {
            std::istringstream in(line); std::string value, mode;
            if (!(in >> value >> mode)) continue;
            try { next[count] = std::stoull(value, nullptr, 16) & ~uintptr_t(7); } catch (...) { continue; }
            write[count++] = mode == "w";
        }
        file.close();
        std::filesystem::remove(directory/"watch.txt");
        address = next; write_only = write; armed = count > 0; hits.clear(); total = 0;
        for (auto& [id, thread] : threads) apply(thread);
        log << "watch armed=" << count << "\n";
    }
    void report(std::ofstream& log) {
        log << "watch report total=" << total << "\n";
        for (auto& [key, count] : hits)
            log << "  dr" << std::get<0>(key) << " rip=0x" << std::hex << std::get<1>(key) << " stack=0x" << std::get<2>(key)
                << std::dec << " hits=" << count << "\n";
    }
};
// Optional execution breakpoints, read when the game starts from %X4VR_CAPTURE_DIR%/trace_rvas.txt
// (one "0x<rva>" per line, max 4, relative to the game image). The first 3 hits per address log RAX
// and the thread's last Win32 error; after that, every 2 s one line per address that ran: hit
// count and the RCX object's vtable RVA (for methods: which class), so game modes can be compared.
struct Trace {
    std::array<uintptr_t, 4> address{};
    std::array<unsigned, 4> hits{}, recent{};
    std::array<uintptr_t, 4> vtable{};
    ULONGLONG reported{};
    uintptr_t base{};
    bool armed{};
    void load(const void* image) {
        base = reinterpret_cast<uintptr_t>(image);
        wchar_t root[1024]{};
        if (!GetEnvironmentVariableW(L"X4VR_CAPTURE_DIR", root, 1024)) return;
        std::ifstream file(std::filesystem::path(root)/"trace_rvas.txt");
        std::string line; int count = 0;
        while (count < 4 && std::getline(file, line))
            try { address[count] = base + std::stoull(line, nullptr, 16); ++count; } catch (...) {}
        armed = count > 0;
    }
    void apply(HANDLE thread) const {
        CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (!GetThreadContext(thread, &context)) return;
        context.Dr0 = address[0]; context.Dr1 = address[1]; context.Dr2 = address[2]; context.Dr3 = address[3];
        context.Dr7 = 0;
        for (int i = 0; i < 4; ++i) if (address[i]) context.Dr7 |= 1ull << (i*2); // execute, length 1
        SetThreadContext(thread, &context);
    }
    int index(const void* at) const {
        for (int i = 0; i < 4; ++i) if (address[i] && address[i] == reinterpret_cast<uintptr_t>(at)) return i;
        return -1;
    }
    void hit(const DEBUG_EVENT& event, HANDLE process, std::ofstream& log) {
        const int i = index(event.u.Exception.ExceptionRecord.ExceptionAddress);
        Handle thread{OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId)};
        CONTEXT context{}; context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_DEBUG_REGISTERS;
        if (!thread.value || !GetThreadContext(thread.value, &context)) return;
        if (hits[i]++ < 3) {
            struct { LONG exit; PVOID teb; PVOID ids[2]; ULONG_PTR affinity; LONG priority[2]; } basic{};
            using Query = LONG (NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
            static const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
            DWORD last_error = 0;
            if (query && query(thread.value, 0, &basic, sizeof(basic), nullptr) == 0)
                ReadProcessMemory(process, static_cast<const char*>(basic.teb)+0x68, &last_error, sizeof(last_error), nullptr);
            log << "trace rva=0x" << std::hex << address[i]-base << " tid=" << std::dec << event.dwThreadId << " rax=0x" << std::hex
                << context.Rax << std::dec << " last_error=" << last_error << '\n';
        }
        ++recent[i];
        uintptr_t table{};
        if (ReadProcessMemory(process, reinterpret_cast<const void*>(context.Rcx), &table, sizeof(table), nullptr)) vtable[i] = table;
        if (GetTickCount64()-reported > 2000) {
            reported = GetTickCount64();
            for (int k = 0; k < 4; ++k) if (recent[k]) {
                log << "trace t=" << reported << " rva=0x" << std::hex << address[k]-base << " hits=" << std::dec << recent[k]
                    << " vtable=0x" << std::hex << (vtable[k] >= base ? vtable[k]-base : vtable[k]) << std::dec << '\n';
                recent[k] = 0;
            }
        }
        context.EFlags |= 0x10000; // resume flag: execute the instruction instead of trapping again
        context.Dr6 = 0;
        SetThreadContext(thread.value, &context);
    }
};
#endif
int wmain(int argc, wchar_t** argv) {
#ifdef X4VR_DEV_TOOLS
    if (argc != 3 && !(argc == 5 && std::wstring(argv[3]) == L"--startup-module")) {
        std::cerr << "Usage: crash_watch_dev.exe <executable> <new-report-directory> [--startup-module <dll>]\n"; return 2;
    }
#else
    if (argc != 3) { std::cerr << "Usage: crash_watch.exe <executable> <new-report-directory>\n"; return 2; }
#endif
    const auto exe = std::filesystem::absolute(argv[1]);
    const auto directory = std::filesystem::absolute(argv[2]);
    if (!std::filesystem::is_regular_file(exe) || !std::filesystem::create_directory(directory)) {
        std::cerr << "Executable must exist and report directory must be new\n"; return 2;
    }
    std::ofstream log(directory/"debug-events.log");
    if (!log) return 2;
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    // Optional game arguments for the child only (e.g. -skipintro -nocputhrottle).
    if (wchar_t extra[1024]{}; GetEnvironmentVariableW(L"X4VR_GAME_ARGS", extra, 1024) && extra[0])
        command += L" " + std::wstring(extra);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    // A process started under a debugger gets Windows' debug heap (NtGlobalFlag 0x70: no
    // low-fragmentation heap, every allocation checked under one lock). X4's many-threaded
    // loading then crawled: 7+ minutes on 8.00 with mods, versus normal speed without us.
    SetEnvironmentVariableW(L"_NO_DEBUG_HEAP", L"1");
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
        DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, exe.parent_path().c_str(), &startup, &process)) {
        log << "create_failed error=" << GetLastError() << '\n'; return 3;
    }
    Handle process_handle{process.hProcess}, initial_thread{process.hThread};
    if (!DebugSetProcessKillOnExit(FALSE)) {
        const auto error = GetLastError();
        DebugActiveProcessStop(process.dwProcessId);
        log << "detach_policy_failed error=" << error << '\n'; return 3;
    }
    log << "started pid=" << process.dwProcessId << " tick=" << GetTickCount64() << '\n'; log.flush();
    std::cout << "Debug child PID: " << process.dwProcessId << std::endl;
    bool initial_breakpoint = true;
    unsigned exceptions = 0;
    size_t debug_bytes = 0;
    try {
#ifdef X4VR_DEV_TOOLS
    Watch watch;
    Trace trace;
    std::unique_ptr<StartupModule> startup_module;
    if (argc == 5) startup_module = std::make_unique<StartupModule>(process.hProcess, process.hThread,
        process.dwProcessId, process.dwThreadId, argv[4], log);
#endif
    for (;;) {
#ifdef X4VR_DEV_TOOLS
        if (startup_module) startup_module->check_timeout();
        watch.poll(directory, log);
#endif
        DEBUG_EVENT event{};
        if (!WaitForDebugEventEx(&event, 1000)) {
            if (GetLastError() == ERROR_SEM_TIMEOUT) continue;
            log << "wait_failed error=" << GetLastError() << '\n';
            DebugActiveProcessStop(process.dwProcessId); return 4;
        }
        DWORD continuation = DBG_CONTINUE;
        bool exited = false;
        DWORD exit_code = 0;
        if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
#ifdef X4VR_DEV_TOOLS
            watch.threads[event.dwThreadId] = event.u.CreateProcessInfo.hThread;
            trace.load(event.u.CreateProcessInfo.lpBaseOfImage);
            if (trace.armed) { trace.apply(event.u.CreateProcessInfo.hThread); log << "trace armed\n"; }
            if (startup_module) startup_module->process_created(event.u.CreateProcessInfo.lpBaseOfImage);
#endif
            module(log, "process", event.u.CreateProcessInfo.lpBaseOfImage, event.u.CreateProcessInfo.hFile);
        }
#ifdef X4VR_DEV_TOOLS
        else if (event.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT) {
            watch.threads[event.dwThreadId] = event.u.CreateThread.hThread;
            if (watch.armed) watch.apply(event.u.CreateThread.hThread);
            else if (trace.armed) trace.apply(event.u.CreateThread.hThread);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT && trace.armed &&
                   event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP &&
                   trace.index(event.u.Exception.ExceptionRecord.ExceptionAddress) >= 0) {
            trace.hit(event, process_handle.value, log);
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT &&
                   event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP &&
                   watch.threads.count(event.dwThreadId)) {
            // Data breakpoints trap after the access; ExceptionAddress is the next instruction.
            const HANDLE thread = watch.threads[event.dwThreadId];
            CONTEXT context{}; context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context) && (context.Dr6 & 0xf)) {
                CONTEXT control{}; control.ContextFlags = CONTEXT_CONTROL;
                uintptr_t caller = 0; // a leaf accessor such as memcpy: its return address
                if (GetThreadContext(thread, &control)) ReadProcessMemory(process_handle.value, reinterpret_cast<void*>(control.Rsp), &caller, sizeof(caller), nullptr);
                for (int i = 0; i < 4; ++i) if (context.Dr6 & (1ull << i))
                    ++watch.hits[{i, reinterpret_cast<uintptr_t>(event.u.Exception.ExceptionRecord.ExceptionAddress), caller}];
                context.Dr6 = 0;
                if (++watch.total >= 20000) { watch.armed = false; context.Dr7 = 0; watch.report(log); log << "watch auto-disarmed\n"; for (auto& [id, other] : watch.threads) if (other != thread) watch.apply(other); }
                SetThreadContext(thread, &context);
            } else continuation = DBG_EXCEPTION_NOT_HANDLED;
        } else if (event.dwDebugEventCode == EXIT_THREAD_DEBUG_EVENT) {
            watch.threads.erase(event.dwThreadId);
            if (startup_module) startup_module->thread_exited(event);
        }
#endif
        else if (event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT)
            module(log, "module", event.u.LoadDll.lpBaseOfDll, event.u.LoadDll.hFile);
        else if (event.dwDebugEventCode == OUTPUT_DEBUG_STRING_EVENT && debug_bytes < 4*1024*1024) {
            const auto& info = event.u.DebugString;
            const size_t length = size_t(info.nDebugStringLength)*(info.fUnicode ? sizeof(wchar_t) : 1);
            if (length && length <= 65536) {
                std::vector<char> text(length+sizeof(wchar_t), 0);
                SIZE_T copied{};
                if (ReadProcessMemory(process_handle.value, info.lpDebugStringData, text.data(), length, &copied)) {
                    log << "debug_string tid=" << event.dwThreadId << " text=";
                    if (info.fUnicode) {
                        const int characters = static_cast<int>(copied/sizeof(wchar_t));
                        const auto* wide = reinterpret_cast<const wchar_t*>(text.data());
                        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, wide, characters, nullptr, 0, nullptr, nullptr);
                        if (bytes > 0) {
                            std::string utf8(static_cast<size_t>(bytes), '\0');
                            WideCharToMultiByte(CP_UTF8, 0, wide, characters, utf8.data(), bytes, nullptr, nullptr);
                            log << utf8.c_str();
                        }
                    } else log << text.data();
                    log << '\n'; debug_bytes += copied;
                }
            }
        } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto record = event.u.Exception.ExceptionRecord;
            const bool first = event.u.Exception.dwFirstChance != 0;
#ifdef X4VR_DEV_TOOLS
            if (startup_module && startup_module->exception(event)) {
                if (record.ExceptionCode == EXCEPTION_BREAKPOINT) initial_breakpoint = false;
            } else
#endif
            if (initial_breakpoint && first && record.ExceptionCode == EXCEPTION_BREAKPOINT) {
                initial_breakpoint = false;
            } else {
                continuation = DBG_EXCEPTION_NOT_HANDLED;
                if (++exceptions <= 1024 || !first) {
                    log << "exception tick=" << GetTickCount64() << " tid=" << event.dwThreadId
                        << " first=" << first << " code=0x" << std::hex << record.ExceptionCode
                        << " address=0x" << reinterpret_cast<uintptr_t>(record.ExceptionAddress) << std::dec << '\n';
                }
                if (!first) {
                    CONTEXT context{}; context.ContextFlags = CONTEXT_ALL;
                    Handle thread{OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, event.dwThreadId)};
                    const bool have_context = thread.value && GetThreadContext(thread.value, &context);
                    EXCEPTION_POINTERS pointers{&record, &context};
                    MINIDUMP_EXCEPTION_INFORMATION info{event.dwThreadId, &pointers, FALSE};
                    const auto path = directory/("crash-" + std::to_string(exceptions) + ".dmp");
                    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
                    const bool ok = file.value != INVALID_HANDLE_VALUE && MiniDumpWriteDump(process_handle.value,
                        process.dwProcessId, file.value, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                        have_context ? &info : nullptr, nullptr, nullptr);
                    log << "dump success=" << ok << " context=" << have_context << " error=" << (ok ? 0 : GetLastError()) << '\n';
                    std::cout << "Unhandled exception captured; dump success=" << ok << std::endl;
                }
            }
        } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            exited = true; exit_code = event.u.ExitProcess.dwExitCode;
            log << "exit code=" << exit_code << " hex=0x" << std::hex << exit_code << std::dec << '\n';
        }
        log.flush();
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, continuation)) {
            log << "continue_failed error=" << GetLastError() << '\n';
            DebugActiveProcessStop(process.dwProcessId); return 4;
        }
        if (exited) return exit_code == 0 ? 0 : 1;
    }
    } catch (const std::exception& error) {
        log << "startup_module_failed: " << error.what() << '\n'; log.flush();
        std::cerr << error.what() << '\n';
        // This is only our newly launched child, stopped before its entry point.
        // Never leave it suspended with an unresolved startup operation. Do not
        // free remote arguments while a helper might still be reading them.
        TerminateProcess(process.hProcess, 5);
        DebugActiveProcessStop(process.dwProcessId);
        return 5;
    }
}
