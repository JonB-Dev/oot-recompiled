// Crash diagnostics.
//
// CLAUDE.md requires these on day one, and the reason is about to matter for every phase from here:
// the recompiled game is 13343 machine-translated functions, and an access violation in one of them
// reports an address and nothing else. An address alone is useless, because the module is relocated
// at load time, so the same fault prints a different number every run.
//
// So this prints the RVA, which IS stable, and resolves it to a symbol when the debug info is there.
// Turning "0x00007FF78931DB97" into "OoTRecompiled.exe+0x1DB97  Overlay_Relocate" is the difference
// between a lead and a dead end.
//
// WHAT IT NEVER PRINTS: memory contents. Not the faulting address's neighborhood, not register
// dumps that might hold ROM data, not rdram. A crash log is the easy way to leak the thing this
// project is careful about, which is why CLAUDE.md names it specifically.
//
// TWO KINDS OF ENDING, and the exception filter sees only the first. A hardware exception (an
// access violation in a translated function, a divide by zero) reaches the unhandled exception
// filter and is reported with its address, its registers and a stack. The CRT's own endings never
// get there: an invalid parameter to a CRT function (a closed stream printed through, a null
// where a pointer was required), an uncaught C++ exception, abort, a pure virtual call. The
// release CRT ends the process for each of them on the spot, status 0xC0000409, no exception,
// and until a double click on 0.1.0 died exactly that way (main.cpp explains the stream) nothing
// here would have said a word. So the CRT's hooks are installed too, each printing the same kind
// of report and flushing it. The invalid parameter hook goes one step further: after the report
// it RETURNS, so the CRT function fails with an error and the program goes on, which is what a
// person at the keyboard wants from a bad printf.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <io.h>

#include "main/crash_handler.h"
#include "game/dl_check.h"
#include "game/render.h"

namespace {

    const char* exception_name(DWORD code) {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
            case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned access";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "float divide by zero";
            case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
            case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer divide by zero";
            case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
            case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
            default:                              return "unknown exception";
        }
    }

    // Print one frame as module + RVA, with a symbol name when one resolves.
    void print_frame(int index, void* addr) {
        HMODULE module = nullptr;
        char module_name[MAX_PATH] = "?";
        uintptr_t rva = 0;

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(addr), &module) &&
            module != nullptr) {
            GetModuleFileNameA(module, module_name, MAX_PATH);
            const char* slash = std::strrchr(module_name, '\\');
            if (slash != nullptr) {
                std::memmove(module_name, slash + 1, std::strlen(slash));
            }
            rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(module);
        }

        // SYMBOL_INFO carries the name inline past the struct, hence the buffer.
        alignas(SYMBOL_INFO) char sym_buffer[sizeof(SYMBOL_INFO) + 512] = {};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(sym_buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;

        DWORD64 displacement = 0;
        const bool resolved = SymFromAddr(GetCurrentProcess(),
                                          reinterpret_cast<DWORD64>(addr),
                                          &displacement,
                                          symbol) != FALSE;

        if (resolved) {
            std::fprintf(stderr, "    %2d  %s+0x%llX  %s+0x%llX\n",
                         index, module_name, static_cast<unsigned long long>(rva),
                         symbol->Name, static_cast<unsigned long long>(displacement));
        }
        else {
            std::fprintf(stderr, "    %2d  %s+0x%llX\n",
                         index, module_name, static_cast<unsigned long long>(rva));
        }
    }

    // The symbol engine is stood up once, the first time a report needs it.
    void ensure_symbols() {
        static bool ready = false;
        if (!ready) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            ready = true;
        }
    }

    void report_banner() {
        std::fprintf(stderr, "\n================ CRASH ================\n");
    }

    // The calling thread and its stack, the frames above `skip` dropped (this function's own).
    void print_stack(ULONG skip) {
        std::fprintf(stderr, "\n  thread %lu\n\n  stack:\n", GetCurrentThreadId());
        ensure_symbols();
        void* frames[32] = {};
        const USHORT captured = CaptureStackBackTrace(skip, 32, frames, nullptr);
        for (USHORT i = 0; i < captured; i++) {
            print_frame(i, frames[i]);
        }
    }

    void report_close() {
        std::fprintf(stderr, "\n  The RVA is stable across runs; the absolute address is not.\n");
        std::fprintf(stderr, "=======================================\n");
        std::fflush(stderr);
    }

    // An uncaught C++ exception does not reach the terminate hook on this ABI: the runtime raises
    // it as this exception code, the search for a handler fails, and the unhandled exception
    // filter is what runs. The record's parameters are the magic, the thrown object, the throw
    // information and the image base its RVAs are relative to; the throw information lists the
    // types the object may be caught as, the first being its own. When std::exception is among
    // them the object's what() is worth the log.
    constexpr DWORD CPP_EXCEPTION_CODE = 0xE06D7363;
    constexpr ULONG_PTR CPP_EXCEPTION_MAGIC = 0x19930520;

    void describe_cpp_exception(const EXCEPTION_RECORD* rec) {
        if (rec->NumberParameters < 4 || rec->ExceptionInformation[0] != CPP_EXCEPTION_MAGIC) {
            return;
        }
        auto* object = reinterpret_cast<char*>(rec->ExceptionInformation[1]);
        const auto* throw_info = reinterpret_cast<const int32_t*>(rec->ExceptionInformation[2]);
        const auto base = static_cast<uintptr_t>(rec->ExceptionInformation[3]);
        if (object == nullptr || throw_info == nullptr || base == 0) {
            return;
        }
        // attributes, unwind, forward compatibility, the catchable type array: all RVAs.
        const int32_t array_rva = throw_info[3];
        if (array_rva == 0) {
            return;
        }
        const auto* array = reinterpret_cast<const int32_t*>(base + static_cast<uintptr_t>(array_rva));
        const int32_t count = array[0];
        for (int32_t i = 0; i < count && i < 16; i++) {
            const int32_t type_rva = array[1 + i];
            if (type_rva == 0) {
                continue;
            }
            // properties, the type descriptor, the displacement to this base (three ints),
            // the size, the copy function.
            const auto* catchable = reinterpret_cast<const int32_t*>(base + static_cast<uintptr_t>(type_rva));
            const int32_t descriptor_rva = catchable[1];
            if (descriptor_rva == 0) {
                continue;
            }
            // The descriptor is a vtable pointer, a spare pointer, then the decorated name.
            const char* name = reinterpret_cast<const char*>(base + static_cast<uintptr_t>(descriptor_rva)) + 2 * sizeof(void*);
            if (i == 0) {
                std::fprintf(stderr, "  type %s\n", name);
            }
            if (std::strcmp(name, ".?AVexception@std@@") == 0) {
                const auto* as_std = reinterpret_cast<const std::exception*>(object + catchable[2]);
                std::fprintf(stderr, "  %s\n", as_std->what());
                break;
            }
        }
    }

    LONG WINAPI handler(EXCEPTION_POINTERS* info) {
        const auto* rec = info->ExceptionRecord;

        report_banner();
        if (rec->ExceptionCode == CPP_EXCEPTION_CODE) {
            std::fprintf(stderr, "  uncaught C++ exception\n");
            describe_cpp_exception(rec);
        }
        else {
            std::fprintf(stderr, "  %s (0x%08lX)\n", exception_name(rec->ExceptionCode), rec->ExceptionCode);
        }

        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            const char* kind = rec->ExceptionInformation[0] == 0 ? "reading"
                             : rec->ExceptionInformation[0] == 1 ? "writing"
                             : "executing";
            // The faulting ADDRESS, not its contents. A null or tiny address means a null pointer
            // dereference; a wild one usually means an unrelocated overlay.
            std::fprintf(stderr, "  while %s 0x%llX\n", kind,
                         static_cast<unsigned long long>(rec->ExceptionInformation[1]));
        }

        // The registers at the fault.
        //
        // This is still ADDRESSES AND VALUES, never memory contents, so it keeps the rule this
        // handler was built around: nothing here can put ROM bytes into a log somebody attaches
        // to a bug report.
        //
        // It earns its place because a faulting address alone says WHERE the read went and not
        // WHERE THE POINTER CAME FROM. When emulated memory is a base pointer plus an offset, the
        // offset is usually still sitting in a register, and that offset is the game's own address,
        // which is the thing worth knowing.
        if (info->ContextRecord != nullptr) {
            const CONTEXT* c = info->ContextRecord;
            const struct { const char* name; DWORD64 value; } regs[] = {
                { "rax", c->Rax }, { "rbx", c->Rbx }, { "rcx", c->Rcx }, { "rdx", c->Rdx },
                { "rsi", c->Rsi }, { "rdi", c->Rdi }, { "rbp", c->Rbp }, { "rsp", c->Rsp },
                { "r8 ", c->R8  }, { "r9 ", c->R9  }, { "r10", c->R10 }, { "r11", c->R11 },
                { "r12", c->R12 }, { "r13", c->R13 }, { "r14", c->R14 }, { "r15", c->R15 },
            };
            std::fprintf(stderr, "\n  registers:\n");
            for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i += 4) {
                std::fprintf(stderr, "    %s %016llX   %s %016llX   %s %016llX   %s %016llX\n",
                             regs[i + 0].name, static_cast<unsigned long long>(regs[i + 0].value),
                             regs[i + 1].name, static_cast<unsigned long long>(regs[i + 1].value),
                             regs[i + 2].name, static_cast<unsigned long long>(regs[i + 2].value),
                             regs[i + 3].name, static_cast<unsigned long long>(regs[i + 3].value));
            }
        }

        // What the renderer was actually given, just before it went wrong. The faulting address
        // says where a read went; this says what was handed over to produce it.
        oot::dl_check::report_recent_tasks();

        // Whether the graphics device is still there. A crash that FOLLOWS a removed device (a
        // resource creation failing, then a null read) is the GPU's fault, not the reader's, and
        // with --gpu-breadcrumbs this says which command the GPU stopped on.
        oot::renderer::report_gpu_state();

        print_stack(0);
        report_close();

        return EXCEPTION_EXECUTE_HANDLER;
    }

    // One report per ending: abort follows terminate and the pure virtual call, and would print
    // a second report for the same ending without this.
    std::atomic<bool> g_reported{false};

    // A report that itself trips the CRT (the stream it prints to closed, say) must not recurse.
    thread_local bool t_reporting = false;

    void __cdecl invalid_parameter(const wchar_t* expression, const wchar_t* function,
                                   const wchar_t* file, unsigned int line, uintptr_t) {
        if (t_reporting) {
            return;
        }
        t_reporting = true;
        report_banner();
        std::fprintf(stderr, "  invalid parameter to a CRT function\n");
        // The four details are filled in by debug builds only; a release build passes nulls.
        if (function != nullptr) {
            std::fprintf(stderr, "  in %ls (%ls:%u): %ls\n", function,
                         file != nullptr ? file : L"?", line,
                         expression != nullptr ? expression : L"?");
        }
        print_stack(1);
        std::fprintf(stderr, "\n  The call fails with an error and the program goes on.\n");
        report_close();
        t_reporting = false;
    }

    [[noreturn]] void on_terminate() {
        if (!g_reported.exchange(true)) {
            report_banner();
            std::fprintf(stderr, "  uncaught C++ exception\n");
            try {
                if (auto current = std::current_exception()) {
                    std::rethrow_exception(current);
                }
            }
            catch (const std::exception& e) {
                std::fprintf(stderr, "  %s\n", e.what());
            }
            catch (...) {
                std::fprintf(stderr, "  (not a std::exception)\n");
            }
            print_stack(1);
            report_close();
        }
        std::abort();
    }

    void __cdecl on_abort(int) {
        if (!g_reported.exchange(true)) {
            report_banner();
            std::fprintf(stderr, "  abort called\n");
            print_stack(1);
            report_close();
        }
        // Returning lets abort finish. With the report fault behavior off (install), that is
        // an exit with status 3 and no dialog; the log holds the reason.
    }

    void __cdecl on_purecall() {
        if (!g_reported.exchange(true)) {
            report_banner();
            std::fprintf(stderr, "  pure virtual function called\n");
            print_stack(1);
            report_close();
        }
        std::abort();
    }

    // For self_test("purecall"): the constructor reaches the pure virtual through a plain member,
    // which is the shape the compiler cannot see through and the CRT catches at run time.
    struct PureBase {
        PureBase() { through(); }
        virtual ~PureBase() = default;
        void through() { pure(); }
        virtual void pure() = 0;
    };
    struct PureDerived : PureBase {
        void pure() override {}
    };

} // namespace

void oot::crash::install() {
    SetUnhandledExceptionFilter(handler);
    _set_invalid_parameter_handler(invalid_parameter);
    _set_purecall_handler(on_purecall);
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_abort);
    // abort() must neither raise a dialog nor hand the process to error reporting after our
    // report: it exits, with status 3, and the log says why.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

bool oot::crash::self_test(const char* kind) {
    if (std::strcmp(kind, "invalid-parameter") == 0) {
        // A descriptor that was never open: the CRT's validation refuses it, the hook reports,
        // the call returns an error and this returns.
        (void)_close(-1);
        return true;
    }
    if (std::strcmp(kind, "terminate") == 0) {
        throw std::runtime_error("crash test: an exception nothing catches");
    }
    if (std::strcmp(kind, "abort") == 0) {
        std::abort();
    }
    if (std::strcmp(kind, "purecall") == 0) {
        PureDerived derived;
        (void)derived;
        return true;
    }
    if (std::strcmp(kind, "access-violation") == 0) {
        volatile int* nowhere = reinterpret_cast<volatile int*>(8);
        *nowhere = 1;
        return true;
    }
    return false;
}
