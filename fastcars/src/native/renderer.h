// fastcars - native renderer front end (Plume, Vulkan first).
//
// --fastcars_native=off|shadow. In shadow mode the D3D hooks keep calling through, so the emulated
// renderer still drives the game window; the native renderer replays each guest frame into a
// second window for side-by-side comparison. Guest threads only record into a frame packet;
// a dedicated render thread owns every Plume object.

#pragma once

#include <cstdint>

namespace fastcars::native {

// True when the native renderer is enabled (any mode other than off).
bool Active();

struct FramePacket;

// Hands a finished guest frame to the render thread (never blocks; drops frames when behind).
void Submit(FramePacket&& frame);
// Guest Swap of frame_index (before its PM4 swap packet is written): the command processor's
// matching swap waits until the renderer recorded that frame (--fastcars_native=on).
void NoteGuestSwap(uint64_t frame_index);

// The CP idle notification and the guest's D3D fence wait can each be tested as the
// point where streamed buffers become reusable. The fence hook passes its target fence value.
enum class GuestSync { Idle, Fence };
void OnGuestSync(GuestSync sync, uint32_t fence = 0);

// An emptied packet whose buffers kept their capacity (a fresh one when none was returned yet):
// the capture side reuses packets instead of regrowing a multi-MB arena every frame.
FramePacket TakeSpare();

}  // namespace fastcars::native
