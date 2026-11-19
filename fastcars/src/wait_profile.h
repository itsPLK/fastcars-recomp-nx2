// fastcars - where the game's threads wait (see wait_profile.cpp).

#pragma once

#include <rex/ppc.h>

#include <cstdint>

namespace fastcars {

// Runs `call` (a hooked game wait function), counted under `wrapper` with --fastcars_wait_profile.
void WaitProfileTimed(uint32_t wrapper, PPCContext& ctx, uint8_t* base, void (*call)(PPCContext&, uint8_t*));

}  // namespace fastcars
