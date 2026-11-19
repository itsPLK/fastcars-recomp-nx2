// fastcars - guest address -> host pointer for host code that has the recompiled code's `base`.
//
// Never `base + address`: the guest windows that alias each other are separate host views on
// desktop (and the 0xE0000000 window sits 4 KB higher on Windows and macOS arm64), and on Switch 2
// they are folded onto one copy whose host block ends at base + 0xC0001000. rex::memory::GuestPtr
// does what the recompiled code's REX_PHYS_HOST_OFFSET does on every platform.
#pragma once

#include <cstdint>

#include <rex/system/xmemory.h>

namespace fastcars {

inline uint8_t* GuestHost(uint8_t* base, uint32_t address) {
  return rex::memory::GuestPtr(base, address);
}
inline const uint8_t* GuestHost(const uint8_t* base, uint32_t address) {
  return rex::memory::GuestPtr(const_cast<uint8_t*>(base), address);
}

}  // namespace fastcars
