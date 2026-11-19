// Thread pointer for clang-compiled code on libounce.
//
// libounce keeps each thread's TLS pointer at *(tpidrro_el0 + 0x1F8) and GCC code reads it with
// __aarch64_read_tp (-mtp=soft). Clang's AArch64 TLS sequences read tpidr_el0 instead; the
// variant-1 layout (TCB of 16 bytes, then the TLS block) is the same. tpidr_el0 is
// user-writable and the kernel saves it per thread, so copying the pointer there once per thread
// makes clang's local-exec TLS work, at full speed.
//
// Linked with -Wl,--wrap=__libnx_init_thread -Wl,--wrap=threadCreate (see CMakeLists.txt).
#include <stdlib.h>
#include <switch.h>

void* __aarch64_read_tp(void);

static inline void set_thread_pointer(void) {
    void* tp = __aarch64_read_tp();
    __asm__ volatile("msr tpidr_el0, %0" ::"r"(tp));
}

// Main thread: libounce sets up its thread variables here, before any constructor runs.
void __real___libnx_init_thread(void);
void __wrap___libnx_init_thread(void) {
    __real___libnx_init_thread();
    set_thread_pointer();
}

// Every other thread (pthreads and std::thread go through threadCreate too).
typedef struct {
    ThreadFunc entry;
    void* arg;
} TpTrampoline;

static void tp_trampoline(void* p) {
    TpTrampoline t = *(TpTrampoline*)p;
    free(p);
    set_thread_pointer();
    t.entry(t.arg);
}

Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                           int prio, int cpuid);
Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                           int prio, int cpuid) {
    TpTrampoline* tramp = malloc(sizeof(*tramp));
    if (!tramp) return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    tramp->entry = entry;
    tramp->arg = arg;
    Result rc = __real_threadCreate(t, tp_trampoline, tramp, stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(rc)) free(tramp);
    return rc;
}
