// Abort, std::terminate, and __cxa_throw logging hooks for Switch 2.
// Under kaiHEN, CPU faults and aborts kill the process silently; these hooks record
// any C++ exception, std::terminate, or abort() to logs/crash_hooks.txt (flushed + fsynced).

#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <mutex>
#include <typeinfo>

namespace {

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
  __real___cxa_throw(thrown_exception, tinfo, dest);
}

}  // extern "C"
