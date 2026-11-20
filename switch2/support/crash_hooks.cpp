// Abort, std::terminate, and __cxa_throw logging hooks for Switch 2.
// Under kaiHEN, CPU faults and aborts kill the process silently; these hooks record
// any C++ exception, std::terminate, or abort() to logs/crash_hooks.txt (flushed + fsynced).

#include <unistd.h>
#include <unwind.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <typeinfo>

extern "C" {
#include <switch2/memory/heap.h>  // no extern "C" of its own
}

namespace {

// Freed before logging an out-of-memory throw, so fopen and the log line still find memory.
void* g_reserve = nullptr;

struct Trace {
  uintptr_t pc[32];
  int n = 0;
};

_Unwind_Reason_Code TraceFrame(_Unwind_Context* ctx, void* arg) {
  auto* t = static_cast<Trace*>(arg);
  if (t->n >= 32) return _URC_END_OF_STACK;
  t->pc[t->n++] = _Unwind_GetIP(ctx);
  return _URC_NO_REASON;
}

void LogCrashHook(const char* kind, const char* detail, uintptr_t caller) {
  static std::mutex mu;
  std::lock_guard<std::mutex> lock(mu);
  FILE* fp = std::fopen("logs/crash_hooks.txt", "a");
  if (!fp) {
    fp = std::fopen("logs/crash_hooks.txt", "a");  // relative: the working directory is the app folder
  }
  if (!fp) {
    return;
  }
  std::fprintf(fp, "%s: detail=%s caller=0x%llx\n", kind, detail ? detail : "(null)",
               static_cast<unsigned long long>(caller));
  std::fflush(fp);
  fsync(fileno(fp));
  std::fclose(fp);
}

[[noreturn]] void TerminateHandler() {
  const char* type_name = "(none)";
  if (auto eptr = std::current_exception()) {
    try {
      std::rethrow_exception(eptr);
    } catch (const std::exception& e) {
      LogCrashHook("std::terminate(std::exception)", e.what(),
                   reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
      std::abort();
    } catch (...) {
      type_name = "(unknown C++ exception)";
    }
  }
  LogCrashHook("std::terminate", type_name,
               reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  std::abort();
}

__attribute__((constructor)) void InstallTerminateHandler() {
  std::set_terminate(TerminateHandler);
  g_reserve = std::malloc(4 << 20);
}

// bad_alloc: who allocated (backtrace) and how much heap was left. Addresses are runtime ones: the
// load base is LogOutOfMemory's address minus its address in the build's ELF (llvm-nm).
void LogOutOfMemory() {
  static bool once = false;
  if (once) return;
  once = true;
  std::free(g_reserve);
  g_reserve = nullptr;
  Trace t;
  _Unwind_Backtrace(TraceFrame, &t);
  char line[1024];
  int len = std::snprintf(line, sizeof(line), "heap free %llu of %llu MB; LogOutOfMemory at 0x%llx; backtrace:",
                          static_cast<unsigned long long>(heapGetFreeSize() >> 20),
                          static_cast<unsigned long long>(heapGetTotalSize() >> 20),
                          static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&LogOutOfMemory)));
  for (int i = 0; i < t.n && len > 0 && len < int(sizeof(line)) - 20; ++i)
    len += std::snprintf(line + len, sizeof(line) - len, " 0x%llx", static_cast<unsigned long long>(t.pc[i]));
  LogCrashHook("out of memory", line, 0);
}

}  // namespace

extern "C" {

void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) {
  LogCrashHook("abort", "abort() called",
               reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  __real_abort();
}

void __real___cxa_throw(void* thrown_exception, void* tinfo, void (*dest)(void*))
    __attribute__((noreturn));
void __wrap___cxa_throw(void* thrown_exception, void* tinfo, void (*dest)(void*)) {
  const char* name = tinfo ? static_cast<const std::type_info*>(tinfo)->name() : "(null)";
  LogCrashHook("__cxa_throw", name,
               reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
  if (std::strstr(name, "bad_alloc")) LogOutOfMemory();
  __real___cxa_throw(thrown_exception, tinfo, dest);
}

}  // extern "C"
