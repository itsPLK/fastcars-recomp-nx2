#pragma once
// Copies memory that may be unmapped without faulting (false if any of it is unreadable):
// process_vm_readv on Linux/Android, mach_vm_read_overwrite on macOS, svcQueryMemory checks then a
// plain copy on Switch 2 (where a fault would end the process). Not used on Windows.

#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif defined(__SWITCH__)
#include <switch.h>
#include <cstring>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace fastcars::native {
inline bool SafeReadMemory(void* dst, const void* src, size_t size) {
#if defined(__APPLE__)
  mach_vm_size_t got = 0;
  return mach_vm_read_overwrite(mach_task_self(), mach_vm_address_t(uintptr_t(src)), size,
                                mach_vm_address_t(uintptr_t(dst)), &got) == KERN_SUCCESS &&
         got == size;
#elif defined(__SWITCH__)
  // Every byte must lie in mapped, readable memory; walk the regions, then copy.
  uintptr_t at = uintptr_t(src);
  const uintptr_t end = at + size;
  while (at < end) {
    MemoryInfo info{};
    u32 page_info = 0;
    if (R_FAILED(svcQueryMemory(&info, &page_info, at)) || info.type == MemType_Unmapped ||
        !(info.perm & Perm_R))
      return false;
    at = uintptr_t(info.addr + info.size);
  }
  std::memcpy(dst, src, size);
  return true;
#else
  iovec local{dst, size}, remote{const_cast<void*>(src), size};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(size);
#endif
}
}  // namespace fastcars::native
