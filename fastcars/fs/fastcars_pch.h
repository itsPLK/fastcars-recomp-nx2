// fastcars - freestanding prefix header for the recompiled game code (fastcars_game.dll / fastcars_game.so).
//
// The installer runs ReXGlue codegen on the player's own default.xex + TU27 patch and compiles the
// output on their machine with the bundled clang + lld. That compile has no Microsoft headers or
// libraries: no MSVC STL, no UCRT, no Windows SDK. This file replaces the generated fastcars_pch.h,
// which pulls in the SDK headers (and through them the STL, spdlog and fmt), with a self-contained
// version that needs only clang's own resource headers (stdint, immintrin).
//
// Everything the generated code needs from the runtime goes through a small C interface exported
// by fastcars_fast.exe (fastcarsfs_*, src/game_bridge.cpp). Kernel imports (__imp__Ke*, __imp__Nt*, ...)
// are imported from the ReXGlue runtime DLL and hooked guest functions from fastcars_fast.exe.
// On Linux the same names are left undefined in fastcars_game.so and the dynamic loader binds them to
// fastcars_fast and librexruntime.so at load time (no import libraries).
//
// Layouts and inline helpers below are taken from the ReXGlue SDK v0.10.0 headers
// (include/rex/ppc/context.h, ppc/intrinsics.h, ppc/func.h, platform/fpscr.h, image_info.h):
//   Copyright (c) 2026, Tom Clay <tomc@tctechstuff.com>. BSD 3-Clause License.
//   Portions derived from the Xenia project, Copyright (c) 2022 Ben Vanik and contributors.
// layout_check.h (also compiled against the SDK headers, in src/game_bridge.cpp) checks that the
// structures here match the SDK's byte for byte.
#pragma once

#define REX_CONFIG_H_INCLUDED

#define REX_IMAGE_BASE 0x82000000ull
#define REX_IMAGE_SIZE 0x1F50000ull
#define REX_CODE_BASE 0x82210000ull
#define REX_CODE_SIZE 0x16A60F4ull
#define REX_THUNK_RESERVE_SIZE 0x10000ull

#define REXCRT_HEAP 0

#include <stddef.h>
#include <stdint.h>
// arm64 (macOS): the simde headers themselves (MIT, shipped in fs/include), the same SSE-on-NEON
// code the SDK build runs. They need only clang's resource headers (arm_neon.h).
#include <simde/x86/avx.h>
#include <simde/x86/avx2.h>
#include <simde/x86/sse.h>
#include <simde/x86/sse4.1.h>

#define FASTCARSFS_IMPORT

#define INT_MAX 2147483647
#define INT_MIN (-INT_MAX - 1)
#define UINT_MAX 4294967295u
#define LLONG_MAX 9223372036854775807LL
#define LLONG_MIN (-LLONG_MAX - 1)

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using f32 = float;
using f64 = double;

//=============================================================================
// The few standard library names the generated code uses
//=============================================================================

namespace std {
enum memory_order { memory_order_relaxed, memory_order_consume, memory_order_acquire,
                    memory_order_release, memory_order_acq_rel, memory_order_seq_cst };
inline void atomic_thread_fence(memory_order) noexcept { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline float fma(float a, float b, float c) noexcept { return __builtin_fmaf(a, b, c); }
inline double fma(double a, double b, double c) noexcept { return __builtin_fma(a, b, c); }
inline bool isnan(float v) noexcept { return __builtin_isnan(v); }
inline bool isnan(double v) noexcept { return __builtin_isnan(v); }
template <class To, class From>
constexpr To bit_cast(const From& v) noexcept { return __builtin_bit_cast(To, v); }
}  // namespace std

extern "C" void* memcpy(void*, const void*, size_t);
extern "C" void* memset(void*, int, size_t);

// sqrt is the sqrtsd instruction (the DLL is built with -fno-math-errno). log2f comes from the
// system's ucrtbase.dll (Linux: libm.so.6), the same implementation the SDK build calls, so results
// stay identical.
inline double sqrt(double v) noexcept { return __builtin_sqrt(v); }
inline float sqrt(float v) noexcept { return __builtin_sqrtf(v); }
extern "C" FASTCARSFS_IMPORT float log2f(float);


//=============================================================================
// Function type and mapping (SDK ppc/func.h)
//=============================================================================

struct PPCContext;
using PPCFunc = void(PPCContext& ctx, uint8_t* base);

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

struct PPCFuncMapping {
  size_t guest;
  PPCFunc* host;
};

namespace rex {
namespace system {
class KernelState;
}
using RegisterModulesFunc = void (*)(system::KernelState*);

struct PPCCodegenFlags {
  bool skip_lr = false;
  bool ctr_as_local = false;
  bool xer_as_local = false;
  bool reserved_as_local = false;
  bool skip_msr = false;
  bool cr_as_local = false;
  bool non_argument_as_local = false;
  bool non_volatile_as_local = false;
};

struct PPCImageInfo {
  u32 code_base;
  u32 code_size;
  u32 image_base;
  u32 image_size;
  const PPCFuncMapping* func_mappings;
  bool rexcrt_heap = false;
  RegisterModulesFunc register_modules = nullptr;
  PPCCodegenFlags codegen_flags{};
};
}  // namespace rex

//=============================================================================
// Runtime interface exported by fastcars_fast.exe (src/game_bridge.cpp)
//=============================================================================

extern "C" {
FASTCARSFS_IMPORT u32 fastcarsfs_mmio_load(u32 addr);
FASTCARSFS_IMPORT void fastcarsfs_mmio_store(u32 addr, u32 value);
FASTCARSFS_IMPORT u64 fastcarsfs_query_timebase();
FASTCARSFS_IMPORT PPCFunc* fastcarsfs_resolve_indirect(u32 guest_address);
FASTCARSFS_IMPORT void fastcarsfs_global_lock_enter();
FASTCARSFS_IMPORT void fastcarsfs_global_lock_leave();
FASTCARSFS_IMPORT u64 fastcarsfs_global_lock_check();
FASTCARSFS_IMPORT void fastcarsfs_trap(PPCContext* ctx, u8* base, u16 trap_type);
[[noreturn]] FASTCARSFS_IMPORT void fastcarsfs_unimplemented(const char* what);
}

//=============================================================================
// PPC registers and context (SDK ppc/context.h, platform/fpscr.h)
//=============================================================================

constexpr float kPack2101010_Min10 = __builtin_bit_cast(float, 0x403FFE01u);
constexpr float kPack2101010_Max10 = __builtin_bit_cast(float, 0x404001FFu);
constexpr float kPack2101010_Min2 = __builtin_bit_cast(float, 0x40400000u);
constexpr float kPack2101010_Max2 = __builtin_bit_cast(float, 0x40400003u);

namespace rex::platform {
struct FPSCRPlatform {
  // FPCR: RMode bits 22-23, FZ (19) and FZ16 (24); guest nearest/zero/+inf/-inf.
  static constexpr size_t RoundShift = 22;
  static constexpr size_t RoundMaskVal = 3 << RoundShift;
  static constexpr size_t FlushMask = (1 << 19) | (1 << 24);
  static constexpr size_t GuestToHost[] = {0 << RoundShift, 3 << RoundShift, 1 << RoundShift,
                                           2 << RoundShift};
  // Trap enables (IOE, DZE, OFE, UFE, IXE, IDE): cleared, ARM's default.
  static constexpr u32 ExceptionMask = (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12) |
                                       (1 << 15);
  static inline u32 getcsr() noexcept {
    u64 csr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(csr));
    return csr;
  }
  static inline void setcsr(u32 csr) noexcept { __asm__ __volatile__("msr fpcr, %0" : : "r"(u64(csr))); }
  static inline void InitHostExceptions(u32& csr) noexcept { csr &= ~ExceptionMask; }
};
}  // namespace rex::platform

namespace rex::ppc {

union Register {
  int8_t s8;
  uint8_t u8;
  int16_t s16;
  uint16_t u16;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
  float f32;
  double f64;
};

struct XERRegister {
  uint8_t so;
  uint8_t ov;
  uint8_t ca;
};

struct CRRegister {
  uint8_t lt;
  uint8_t gt;
  uint8_t eq;
  union {
    uint8_t so;
    uint8_t un;
  };

  inline uint32_t raw() const noexcept { return (lt << 3) | (gt << 2) | (eq << 1) | so; }

  inline void set_raw(uint32_t value) noexcept {
    lt = (value >> 3) & 1;
    gt = (value >> 2) & 1;
    eq = (value >> 1) & 1;
    so = value & 1;
  }

  template <typename T>
  inline void compare(T left, T right, const XERRegister& xer) noexcept {
    lt = left < right;
    gt = left > right;
    eq = left == right;
    so = xer.so;
  }

  inline void compare(double left, double right) noexcept {
    un = __builtin_isnan(left) || __builtin_isnan(right);
    lt = !un && (left < right);
    gt = !un && (left > right);
    eq = !un && (left == right);
  }

  inline void setFromMask(simde__m128 mask, int imm) noexcept {
    int m = simde_mm_movemask_ps(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }

  inline void setFromMask(simde__m128i mask, int imm) noexcept {
    int m = simde_mm_movemask_epi8(mask);
    lt = m == imm;
    gt = 0;
    eq = m == 0;
    so = 0;
  }
};

union alignas(0x10) VRegister {
  int8_t s8[16];
  uint8_t u8[16];
  int16_t s16[8];
  uint16_t u16[8];
  int32_t s32[4];
  uint32_t u32[4];
  int64_t s64[2];
  uint64_t u64[2];
  float f32[4];
  double f64[2];
};

constexpr uint32_t kRoundNearest = 0x00;
constexpr uint32_t kRoundTowardZero = 0x01;
constexpr uint32_t kRoundUp = 0x02;
constexpr uint32_t kRoundDown = 0x03;
constexpr uint32_t kRoundMask = 0x03;

struct FPSCRRegister {
  uint32_t csr;

  static constexpr size_t HostToGuest[] = {kRoundNearest, kRoundDown, kRoundUp, kRoundTowardZero};

  using Platform = platform::FPSCRPlatform;
  static constexpr size_t RoundShift = Platform::RoundShift;
  static constexpr size_t RoundMaskVal = Platform::RoundMaskVal;
  static constexpr size_t FlushMask = Platform::FlushMask;
  static constexpr uint32_t GuestMask = uint32_t(RoundMaskVal) | uint32_t(FlushMask);

  inline uint32_t getcsr() noexcept { return Platform::getcsr(); }
  inline void setcsr(uint32_t csr) noexcept { Platform::setcsr(csr); }

  inline void restoreGuestBits(uint32_t saved) noexcept {
    csr = (getcsr() & ~GuestMask) | (saved & GuestMask);
    setcsr(csr);
  }

  inline uint32_t loadFromHost() noexcept {
    csr = getcsr();
    return HostToGuest[(csr & RoundMaskVal) >> RoundShift];
  }

  inline void storeFromGuest(uint32_t value) noexcept {
    csr &= ~RoundMaskVal;
    csr |= Platform::GuestToHost[value & kRoundMask];
    setcsr(csr);
  }

  inline void enableFlushModeUnconditional() noexcept {
    csr |= FlushMask;
    setcsr(csr);
  }

  inline void disableFlushModeUnconditional() noexcept {
    csr &= ~FlushMask;
    setcsr(csr);
  }

  inline void enableFlushMode() noexcept {
    if ((csr & FlushMask) != FlushMask) [[unlikely]] {
      csr |= FlushMask;
      setcsr(csr);
    }
  }

  inline void disableFlushMode() noexcept {
    if ((csr & FlushMask) != 0) [[unlikely]] {
      csr &= ~FlushMask;
      setcsr(csr);
    }
  }

  inline void InitHost() noexcept {
    csr = getcsr();
    Platform::InitHostExceptions(csr);
    setcsr(csr);
  }
};

}  // namespace rex::ppc

using PPCRegister = rex::ppc::Register;
using PPCXERRegister = rex::ppc::XERRegister;
using PPCCRRegister = rex::ppc::CRRegister;
using PPCVRegister = rex::ppc::VRegister;
using PPCFPSCRRegister = rex::ppc::FPSCRRegister;

#define FASTCARSFS_R(n) PPCRegister n;
#define FASTCARSFS_V(n) PPCVRegister n;

struct alignas(0x40) PPCContext {
  PPCRegister r3;
  PPCRegister r0;
  PPCRegister r1;
  PPCRegister r2;
  PPCRegister r4;
  PPCRegister r5;
  PPCRegister r6;
  PPCRegister r7;
  PPCRegister r8;
  PPCRegister r9;
  PPCRegister r10;
  PPCRegister r11;
  PPCRegister r12;
  PPCRegister r13;
  PPCRegister r14;
  PPCRegister r15;
  PPCRegister r16;
  PPCRegister r17;
  PPCRegister r18;
  PPCRegister r19;
  PPCRegister r20;
  PPCRegister r21;
  PPCRegister r22;
  PPCRegister r23;
  PPCRegister r24;
  PPCRegister r25;
  PPCRegister r26;
  PPCRegister r27;
  PPCRegister r28;
  PPCRegister r29;
  PPCRegister r30;
  PPCRegister r31;

  uint64_t lr;
  PPCRegister ctr;
  PPCXERRegister xer;
  PPCRegister reserved;
  uint32_t msr = 0x200A000;
  PPCCRRegister cr0;
  PPCCRRegister cr1;
  PPCCRRegister cr2;
  PPCCRRegister cr3;
  PPCCRRegister cr4;
  PPCCRRegister cr5;
  PPCCRRegister cr6;
  PPCCRRegister cr7;
  PPCFPSCRRegister fpscr;
  uint8_t vscr_sat = 0;
  uint32_t last_indirect_target = 0;

  FASTCARSFS_R(f0) FASTCARSFS_R(f1) FASTCARSFS_R(f2) FASTCARSFS_R(f3) FASTCARSFS_R(f4) FASTCARSFS_R(f5) FASTCARSFS_R(f6)
  FASTCARSFS_R(f7) FASTCARSFS_R(f8) FASTCARSFS_R(f9) FASTCARSFS_R(f10) FASTCARSFS_R(f11) FASTCARSFS_R(f12) FASTCARSFS_R(f13)
  FASTCARSFS_R(f14) FASTCARSFS_R(f15) FASTCARSFS_R(f16) FASTCARSFS_R(f17) FASTCARSFS_R(f18) FASTCARSFS_R(f19)
  FASTCARSFS_R(f20) FASTCARSFS_R(f21) FASTCARSFS_R(f22) FASTCARSFS_R(f23) FASTCARSFS_R(f24) FASTCARSFS_R(f25)
  FASTCARSFS_R(f26) FASTCARSFS_R(f27) FASTCARSFS_R(f28) FASTCARSFS_R(f29) FASTCARSFS_R(f30) FASTCARSFS_R(f31)

  FASTCARSFS_V(v0) FASTCARSFS_V(v1) FASTCARSFS_V(v2) FASTCARSFS_V(v3) FASTCARSFS_V(v4) FASTCARSFS_V(v5) FASTCARSFS_V(v6)
  FASTCARSFS_V(v7) FASTCARSFS_V(v8) FASTCARSFS_V(v9) FASTCARSFS_V(v10) FASTCARSFS_V(v11) FASTCARSFS_V(v12)
  FASTCARSFS_V(v13) FASTCARSFS_V(v14) FASTCARSFS_V(v15) FASTCARSFS_V(v16) FASTCARSFS_V(v17) FASTCARSFS_V(v18)
  FASTCARSFS_V(v19) FASTCARSFS_V(v20) FASTCARSFS_V(v21) FASTCARSFS_V(v22) FASTCARSFS_V(v23) FASTCARSFS_V(v24)
  FASTCARSFS_V(v25) FASTCARSFS_V(v26) FASTCARSFS_V(v27) FASTCARSFS_V(v28) FASTCARSFS_V(v29) FASTCARSFS_V(v30)
  FASTCARSFS_V(v31) FASTCARSFS_V(v32) FASTCARSFS_V(v33) FASTCARSFS_V(v34) FASTCARSFS_V(v35) FASTCARSFS_V(v36)
  FASTCARSFS_V(v37) FASTCARSFS_V(v38) FASTCARSFS_V(v39) FASTCARSFS_V(v40) FASTCARSFS_V(v41) FASTCARSFS_V(v42)
  FASTCARSFS_V(v43) FASTCARSFS_V(v44) FASTCARSFS_V(v45) FASTCARSFS_V(v46) FASTCARSFS_V(v47) FASTCARSFS_V(v48)
  FASTCARSFS_V(v49) FASTCARSFS_V(v50) FASTCARSFS_V(v51) FASTCARSFS_V(v52) FASTCARSFS_V(v53) FASTCARSFS_V(v54)
  FASTCARSFS_V(v55) FASTCARSFS_V(v56) FASTCARSFS_V(v57) FASTCARSFS_V(v58) FASTCARSFS_V(v59) FASTCARSFS_V(v60)
  FASTCARSFS_V(v61) FASTCARSFS_V(v62) FASTCARSFS_V(v63) FASTCARSFS_V(v64) FASTCARSFS_V(v65) FASTCARSFS_V(v66)
  FASTCARSFS_V(v67) FASTCARSFS_V(v68) FASTCARSFS_V(v69) FASTCARSFS_V(v70) FASTCARSFS_V(v71) FASTCARSFS_V(v72)
  FASTCARSFS_V(v73) FASTCARSFS_V(v74) FASTCARSFS_V(v75) FASTCARSFS_V(v76) FASTCARSFS_V(v77) FASTCARSFS_V(v78)
  FASTCARSFS_V(v79) FASTCARSFS_V(v80) FASTCARSFS_V(v81) FASTCARSFS_V(v82) FASTCARSFS_V(v83) FASTCARSFS_V(v84)
  FASTCARSFS_V(v85) FASTCARSFS_V(v86) FASTCARSFS_V(v87) FASTCARSFS_V(v88) FASTCARSFS_V(v89) FASTCARSFS_V(v90)
  FASTCARSFS_V(v91) FASTCARSFS_V(v92) FASTCARSFS_V(v93) FASTCARSFS_V(v94) FASTCARSFS_V(v95) FASTCARSFS_V(v96)
  FASTCARSFS_V(v97) FASTCARSFS_V(v98) FASTCARSFS_V(v99) FASTCARSFS_V(v100) FASTCARSFS_V(v101) FASTCARSFS_V(v102)
  FASTCARSFS_V(v103) FASTCARSFS_V(v104) FASTCARSFS_V(v105) FASTCARSFS_V(v106) FASTCARSFS_V(v107) FASTCARSFS_V(v108)
  FASTCARSFS_V(v109) FASTCARSFS_V(v110) FASTCARSFS_V(v111) FASTCARSFS_V(v112) FASTCARSFS_V(v113) FASTCARSFS_V(v114)
  FASTCARSFS_V(v115) FASTCARSFS_V(v116) FASTCARSFS_V(v117) FASTCARSFS_V(v118) FASTCARSFS_V(v119) FASTCARSFS_V(v120)
  FASTCARSFS_V(v121) FASTCARSFS_V(v122) FASTCARSFS_V(v123) FASTCARSFS_V(v124) FASTCARSFS_V(v125) FASTCARSFS_V(v126)
  FASTCARSFS_V(v127)
};

#undef FASTCARSFS_R
#undef FASTCARSFS_V

#include "layout_check.h"

//=============================================================================
// Vector helpers (SDK ppc/intrinsics.h)
//=============================================================================

namespace rex::ppc {

inline uint8_t VectorMaskL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
};

inline uint8_t VectorMaskR[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
};

inline uint8_t VectorShiftTableL[] = {
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
};

inline uint8_t VectorShiftTableR[] = {
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
};

inline simde__m128i simde_mm_adds_epu32(simde__m128i a, simde__m128i b) {
  return simde_mm_add_epi32(
      a, simde_mm_min_epu32(simde_mm_xor_si128(a, simde_mm_cmpeq_epi32(a, a)), b));
}

inline simde__m128i simde_mm_avg_epi8(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu8(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

inline simde__m128i simde_mm_avg_epi16(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_xor_si128(c,
                            simde_mm_avg_epu16(simde_mm_xor_si128(c, a), simde_mm_xor_si128(c, b)));
}

inline simde__m128i simde_mm_avg_epi32(simde__m128i a, simde__m128i b) {
  simde__m128i sum = simde_mm_add_epi32(simde_mm_srai_epi32(a, 1), simde_mm_srai_epi32(b, 1));
  return simde_mm_add_epi32(sum,
                            simde_mm_and_si128(simde_mm_or_si128(a, b), simde_mm_set1_epi32(1)));
}

inline simde__m128 simde_mm_cvtepu32_ps_(simde__m128i src1) {
  simde__m128i xmm1 = simde_mm_add_epi32(src1, simde_mm_set1_epi32(127));
  simde__m128i xmm0 = simde_mm_slli_epi32(src1, 31 - 8);
  xmm0 = simde_mm_srli_epi32(xmm0, 31);
  xmm0 = simde_mm_add_epi32(xmm0, xmm1);
  xmm0 = simde_mm_srai_epi32(xmm0, 8);
  xmm0 = simde_mm_add_epi32(xmm0, simde_mm_set1_epi32(0x4F800000));
  simde__m128 xmm2 = simde_mm_cvtepi32_ps(src1);
  return simde_mm_blendv_ps(xmm2, simde_mm_castsi128_ps(xmm0), simde_mm_castsi128_ps(src1));
}

inline simde__m128i simde_mm_perm_epi8_(simde__m128i a, simde__m128i b, simde__m128i c) {
  simde__m128i d = simde_mm_set1_epi8(0xF);
  simde__m128i e = simde_mm_sub_epi8(d, simde_mm_and_si128(c, d));
  return simde_mm_blendv_epi8(simde_mm_shuffle_epi8(a, e), simde_mm_shuffle_epi8(b, e),
                              simde_mm_slli_epi32(c, 3));
}

inline simde__m128i simde_mm_cmpgt_epu8(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi8(char(128));
  return simde_mm_cmpgt_epi8(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

inline simde__m128i simde_mm_cmpgt_epu16(simde__m128i a, simde__m128i b) {
  simde__m128i c = simde_mm_set1_epi16(short(32768));
  return simde_mm_cmpgt_epi16(simde_mm_xor_si128(a, c), simde_mm_xor_si128(b, c));
}

inline simde__m128i simde_mm_vctsxs(simde__m128 src1) {
  simde__m128 xmm2 = simde_mm_cmpunord_ps(src1, src1);
  simde__m128i xmm0 = simde_mm_cvttps_epi32(src1);
  simde__m128i xmm1 = simde_mm_cmpeq_epi32(xmm0, simde_mm_set1_epi32(INT_MIN));
  xmm1 = simde_mm_andnot_si128(simde_mm_castps_si128(src1), xmm1);
  simde__m128 dest = simde_mm_blendv_ps(simde_mm_castsi128_ps(xmm0),
                                        simde_mm_castsi128_ps(simde_mm_set1_epi32(INT_MAX)),
                                        simde_mm_castsi128_ps(xmm1));
  return simde_mm_andnot_si128(simde_mm_castps_si128(xmm2), simde_mm_castps_si128(dest));
}

inline simde__m128i simde_mm_vctuxs(simde__m128 src1) {
  simde__m128 nan_mask = simde_mm_cmpunord_ps(src1, src1);
  simde__m128 neg_mask = simde_mm_cmplt_ps(src1, simde_mm_setzero_ps());
  simde__m128 max_val = simde_mm_set1_ps(4294967295.0f);
  simde__m128 overflow_mask = simde_mm_cmpge_ps(src1, max_val);
  simde__m128 clamped = simde_mm_max_ps(src1, simde_mm_setzero_ps());
  clamped = simde_mm_min_ps(clamped, max_val);
  simde__m128 half_range = simde_mm_set1_ps(2147483648.0f);
  simde__m128 high_bit_mask = simde_mm_cmpge_ps(clamped, half_range);
  simde__m128 adjusted = simde_mm_sub_ps(clamped, simde_mm_and_ps(high_bit_mask, half_range));
  simde__m128i low_bits = simde_mm_cvttps_epi32(adjusted);
  simde__m128i high_bit = simde_mm_and_si128(simde_mm_castps_si128(high_bit_mask),
                                             simde_mm_set1_epi32(int(0x80000000u)));
  simde__m128i result = simde_mm_or_si128(low_bits, high_bit);
  result = simde_mm_andnot_si128(simde_mm_castps_si128(nan_mask), result);
  result = simde_mm_andnot_si128(simde_mm_castps_si128(neg_mask), result);
  result = simde_mm_or_si128(
      simde_mm_andnot_si128(simde_mm_castps_si128(overflow_mask), result),
      simde_mm_and_si128(simde_mm_castps_si128(overflow_mask), simde_mm_set1_epi32(-1)));
  return result;
}

inline simde__m128i simde_mm_vsr(simde__m128i a, simde__m128i b) {
  b = simde_mm_srli_epi64(simde_mm_slli_epi64(b, 61), 61);
  return simde_mm_castps_si128(simde_mm_insert_ps(
      simde_mm_castsi128_ps(simde_mm_srl_epi64(a, b)),
      simde_mm_castsi128_ps(simde_mm_srl_epi64(simde_mm_srli_si128(a, 4), b)), 0x10));
}

inline simde__m128i simde_mm_vsl(simde__m128i a, simde__m128i b) {
  int shift = simde_mm_extract_epi8(b, 15) & 0x7;
  if (shift == 0)
    return a;
  // NEON immediate shifts need a constant count: variable vshlq_u64 (negative = right shift).
  uint64x2_t va = vreinterpretq_u64_s64(simde__m128i_to_neon_i64(a));
  uint64x2_t low_shifted = vshlq_u64(va, vdupq_n_s64(shift));
  uint64x2_t high_carry = vshlq_u64(va, vdupq_n_s64(shift - 64));
  uint64x2_t r = vsetq_lane_u64(vgetq_lane_u64(low_shifted, 0), vdupq_n_u64(0), 0);
  r = vsetq_lane_u64(vgetq_lane_u64(low_shifted, 1) | vgetq_lane_u64(high_carry, 0), r, 1);
  return simde__m128i_from_neon_i64(vreinterpretq_s64_u64(r));
}

inline simde__m128i simde_mm_vslo(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  __builtin_memset(dst, 0, sizeof(dst));
  __builtin_memcpy(dst + shift_bytes, src, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
}

inline simde__m128i simde_mm_vsro(simde__m128i a, simde__m128i b) {
  int shift_bytes = (simde_mm_extract_epi8(b, 0) >> 3) & 0xF;
  if (shift_bytes == 0)
    return a;
  if (shift_bytes >= 16)
    return simde_mm_setzero_si128();
  alignas(16) uint8_t src[16], dst[16];
  simde_mm_store_si128((simde__m128i*)src, a);
  __builtin_memset(dst, 0, sizeof(dst));
  __builtin_memcpy(dst, src + shift_bytes, 16 - shift_bytes);
  return simde_mm_load_si128((simde__m128i*)dst);
}

inline simde__m128i simde_mm_sllv_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi32(a_hi, s_hi);
  simde__m128i mask16 = simde_mm_set1_epi32(0xFFFF);
  r_lo = simde_mm_and_si128(r_lo, mask16);
  r_hi = simde_mm_and_si128(r_hi, mask16);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

inline simde__m128i simde_mm_srlv_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi16(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi16(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srlv_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srlv_epi32(a_hi, s_hi);
  return simde_mm_packus_epi32(r_lo, r_hi);
}

inline simde__m128i simde_mm_srav_epi16(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_srai_epi32(simde_mm_unpacklo_epi16(a, a), 16);
  simde__m128i a_hi = simde_mm_srai_epi32(simde_mm_unpackhi_epi16(a, a), 16);
  simde__m128i s_lo = simde_mm_unpacklo_epi16(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi16(count, zero);
  simde__m128i r_lo = simde_mm_srav_epi32(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_srav_epi32(a_hi, s_hi);
  return simde_mm_packs_epi32(r_lo, r_hi);
}

inline simde__m128i simde_mm_sllv_epi8(simde__m128i a, simde__m128i count) {
  simde__m128i zero = simde_mm_setzero_si128();
  simde__m128i a_lo = simde_mm_unpacklo_epi8(a, zero);
  simde__m128i a_hi = simde_mm_unpackhi_epi8(a, zero);
  simde__m128i s_lo = simde_mm_unpacklo_epi8(count, zero);
  simde__m128i s_hi = simde_mm_unpackhi_epi8(count, zero);
  simde__m128i r_lo = simde_mm_sllv_epi16(a_lo, s_lo);
  simde__m128i r_hi = simde_mm_sllv_epi16(a_hi, s_hi);
  simde__m128i mask8 = simde_mm_set1_epi16(0xFF);
  r_lo = simde_mm_and_si128(r_lo, mask8);
  r_hi = simde_mm_and_si128(r_hi, mask8);
  return simde_mm_packus_epi16(r_lo, r_hi);
}

}  // namespace rex::ppc

using rex::ppc::VectorMaskL;
using rex::ppc::VectorMaskR;
using rex::ppc::VectorShiftTableL;
using rex::ppc::VectorShiftTableR;

//=============================================================================
// Image info
//=============================================================================

extern const rex::PPCImageInfo PPCImageConfig;
extern PPCFuncMapping PPCFuncMappings[];

//=============================================================================
// Function definitions
//
// fastcars_hooks.h lists the guest functions fastcars_fast.exe replaces (FASTCARSFS_HOOKED_<name>). For those
// the generated body is only __imp__<name> (exported, so the hook can call the original) and every
// call to <name> resolves to the hook through fastcars_fast.exe's import library (Linux: the dynamic
// loader, from fastcars_fast's dynamic symbol table). Every other function
// gets <name> as a plain alias of __imp__<name>.
//=============================================================================

#include "fastcars_hooks.h"

#define FASTCARSFS_SECOND(a, b, ...) b
#define FASTCARSFS_IS_HOOKED_(...) FASTCARSFS_SECOND(__VA_ARGS__, 0, ~)
#define FASTCARSFS_IS_HOOKED(name) FASTCARSFS_IS_HOOKED_(FASTCARSFS_HOOKED_##name)
#define FASTCARSFS_CAT(a, b) FASTCARSFS_CAT_(a, b)
#define FASTCARSFS_CAT_(a, b) a##b
#if defined(__APPLE__)
// Mach-O has no __attribute__((alias)); an assembler equate gives <name> the same address.
// (It cannot be overridden, which is fine here: hooked functions never get one.)
#define FASTCARSFS_DEFINE_0(name)                                      \
  __asm__(".globl _" #name "\n.set _" #name ", ___imp__" #name); \
  extern "C" REX_FUNC(name);                                       \
  REX_EXTERN(__imp__##name)
#else
#define FASTCARSFS_DEFINE_0(name)                          \
  __attribute__((alias("__imp__" #name), noinline)) \
  extern "C" REX_FUNC(name);                         \
  REX_EXTERN(__imp__##name)
#endif
#define FASTCARSFS_DEFINE_1(name) REX_EXTERN(__imp__##name)

#define DECLARE_REX_FUNC(name) \
  REX_EXTERN(name);            \
  REX_EXTERN(__imp__##name)

#define DEFINE_REX_FUNC(name) FASTCARSFS_CAT(FASTCARSFS_DEFINE_, FASTCARSFS_IS_HOOKED(name))(name)

#if defined(__clang__)
#define REX_FUNC_PROLOGUE() __builtin_assume(((size_t)base & 0x1F) == 0)
#else
#define REX_FUNC_PROLOGUE() ((void)0)
#endif

//=============================================================================
// Memory access
//=============================================================================

// The SDK maps the E0 physical view 4 KB higher only where the host's allocation granularity
// needs it (pch_h.inja: REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)); on Linux
// every view starts at its guest address.
//
// REX_GUEST_FOLD (Switch 2, where one memory cannot be mapped at two addresses): every guest window
// that aliases another lands on a single host copy: physical memory lives once, at
// base + 0xA0000000 (the A window), and the 0x9 XEX window lands on 0x8.
// host = base + addr + fold[addr >> 24]:
//   0x7F000000-0x7FFFFFFF  physical +0 (first 16 MB)  -> +0x21000000
//   0x90000000-0x9FFFFFFF  same memory as 0x8         -> -0x10000000
//   0xA0000000-0xBFFFFFFF  physical +0                -> identity
//   0xC0000000-0xDFFFFFFF  physical +0                -> -0x20000000
//   0xE0000000-0xFFFFFFFF  physical +0x1000           -> -0x40000000 + 0x1000
// everything else is identity, so the host block is 0xC0001000 bytes (3 GB). Must agree with rex::memory::detail::PhysicalHostOffset and the
// heaps' host_address_offset in the SDK (xmemory.h / xmemory.cpp). On desktop every view still
// exists, so a desktop build with REX_GUEST_FOLD reads and writes the same memory (used to check
// it and measure its cost).
#if defined(REX_GUEST_FOLD)
struct FastcarsfsGuestFold {
  u64 v[256];
};
constexpr FastcarsfsGuestFold fastcarsfs_make_guest_fold() {
  FastcarsfsGuestFold t{};
  for (u32 i = 0; i < 256; ++i) {
    u64 o = 0;
    if (i == 0x7F) o = 0x21000000ull;
    else if (i >= 0x90 && i < 0xA0) o = 0ull - 0x10000000ull;
    else if (i >= 0xC0 && i < 0xE0) o = 0ull - 0x20000000ull;
    else if (i >= 0xE0) o = 0ull - 0x40000000ull + 0x1000ull;
    t.v[i] = o;
  }
  return t;
}
inline constexpr FastcarsfsGuestFold fastcarsfs_guest_fold = fastcarsfs_make_guest_fold();
#define REX_PHYS_HOST_OFFSET(addr) (fastcarsfs_guest_fold.v[(u32)(addr) >> 24])
#elif defined(__APPLE__)
#define REX_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)
#else
#define REX_PHYS_HOST_OFFSET(addr) 0u
#endif

#define REX_RAW_ADDR(x) (base + (u32)(x) + REX_PHYS_HOST_OFFSET(x))

// Guest memory accesses, by kind (codegen's memory_classes names stack and in-loop accesses; the
// default codegen uses only the plain names). volatile keeps clang from merging, dropping or
// moving an access, which other threads may depend on, but also from the ordinary optimisations.
//   REX_LOAD_LOOP_*      loads inside a loop: what a spin-wait polls. Always volatile.
//   REX_*_STACK_*        r1-relative: the thread's own frame (spills, saved registers). Plain
//                        unless FASTCARS_VOLATILE_STACK (other threads rarely look into a frame, and
//                        never in a loop, which stays volatile).
//   REX_LOAD_* / STORE_* everything else. volatile unless FASTCARS_PLAIN_MEMORY (experimental: the
//                        game's own synchronisation uses lwarx/stwcx. and sync/lwsync, which are
//                        atomics and fences here; loops stay volatile).
#define FASTCARSFS_LOAD(T, x) (*(volatile T*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define FASTCARSFS_STORE(T, x, y) (*(volatile T*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))
// Plain accesses use may_alias types: guest code reads memory back with another width than it
// wrote (stfd then lwz to convert a float, byte copies of words), which type-based alias analysis
// would otherwise let clang reorder.
typedef u8 __attribute__((__may_alias__)) fastcarsfs_a8;
typedef u16 __attribute__((__may_alias__)) fastcarsfs_a16;
typedef u32 __attribute__((__may_alias__)) fastcarsfs_a32;
typedef u64 __attribute__((__may_alias__)) fastcarsfs_a64;
#define FASTCARSFS_PTYPE_u8 fastcarsfs_a8
#define FASTCARSFS_PTYPE_u16 fastcarsfs_a16
#define FASTCARSFS_PTYPE_u32 fastcarsfs_a32
#define FASTCARSFS_PTYPE_u64 fastcarsfs_a64
#define FASTCARSFS_PLOAD(T, x) (*(FASTCARSFS_PTYPE_##T*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define FASTCARSFS_PSTORE(T, x, y) (*(FASTCARSFS_PTYPE_##T*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = (y))

#if defined(FASTCARS_PLAIN_MEMORY)
#define REX_LOAD_U8(x) FASTCARSFS_PLOAD(u8, x)
#define REX_LOAD_U16(x) __builtin_bswap16(FASTCARSFS_PLOAD(u16, x))
#define REX_LOAD_U32(x) __builtin_bswap32(FASTCARSFS_PLOAD(u32, x))
#define REX_LOAD_U64(x) __builtin_bswap64(FASTCARSFS_PLOAD(u64, x))
#define REX_STORE_U8(x, y) FASTCARSFS_PSTORE(u8, x, y)
#define REX_STORE_U16(x, y) FASTCARSFS_PSTORE(u16, x, __builtin_bswap16(y))
#define REX_STORE_U32(x, y) FASTCARSFS_PSTORE(u32, x, __builtin_bswap32(y))
#define REX_STORE_U64(x, y) FASTCARSFS_PSTORE(u64, x, __builtin_bswap64(y))
#else
#define REX_LOAD_U8(x) FASTCARSFS_LOAD(u8, x)
#define REX_LOAD_U16(x) __builtin_bswap16(FASTCARSFS_LOAD(u16, x))
#define REX_LOAD_U32(x) __builtin_bswap32(FASTCARSFS_LOAD(u32, x))
#define REX_LOAD_U64(x) __builtin_bswap64(FASTCARSFS_LOAD(u64, x))
#define REX_STORE_U8(x, y) FASTCARSFS_STORE(u8, x, y)
#define REX_STORE_U16(x, y) FASTCARSFS_STORE(u16, x, __builtin_bswap16(y))
#define REX_STORE_U32(x, y) FASTCARSFS_STORE(u32, x, __builtin_bswap32(y))
#define REX_STORE_U64(x, y) FASTCARSFS_STORE(u64, x, __builtin_bswap64(y))
#endif

#define REX_LOAD_LOOP_U8(x) FASTCARSFS_LOAD(u8, x)
#define REX_LOAD_LOOP_U16(x) __builtin_bswap16(FASTCARSFS_LOAD(u16, x))
#define REX_LOAD_LOOP_U32(x) __builtin_bswap32(FASTCARSFS_LOAD(u32, x))
#define REX_LOAD_LOOP_U64(x) __builtin_bswap64(FASTCARSFS_LOAD(u64, x))

#if defined(FASTCARS_VOLATILE_STACK)
#define REX_LOAD_STACK_U8(x) REX_LOAD_LOOP_U8(x)
#define REX_LOAD_STACK_U16(x) REX_LOAD_LOOP_U16(x)
#define REX_LOAD_STACK_U32(x) REX_LOAD_LOOP_U32(x)
#define REX_LOAD_STACK_U64(x) REX_LOAD_LOOP_U64(x)
#define REX_STORE_STACK_U8(x, y) FASTCARSFS_STORE(u8, x, y)
#define REX_STORE_STACK_U16(x, y) FASTCARSFS_STORE(u16, x, __builtin_bswap16(y))
#define REX_STORE_STACK_U32(x, y) FASTCARSFS_STORE(u32, x, __builtin_bswap32(y))
#define REX_STORE_STACK_U64(x, y) FASTCARSFS_STORE(u64, x, __builtin_bswap64(y))
#else
#define REX_LOAD_STACK_U8(x) FASTCARSFS_PLOAD(u8, x)
#define REX_LOAD_STACK_U16(x) __builtin_bswap16(FASTCARSFS_PLOAD(u16, x))
#define REX_LOAD_STACK_U32(x) __builtin_bswap32(FASTCARSFS_PLOAD(u32, x))
#define REX_LOAD_STACK_U64(x) __builtin_bswap64(FASTCARSFS_PLOAD(u64, x))
#define REX_STORE_STACK_U8(x, y) FASTCARSFS_PSTORE(u8, x, y)
#define REX_STORE_STACK_U16(x, y) FASTCARSFS_PSTORE(u16, x, __builtin_bswap16(y))
#define REX_STORE_STACK_U32(x, y) FASTCARSFS_PSTORE(u32, x, __builtin_bswap32(y))
#define REX_STORE_STACK_U64(x, y) FASTCARSFS_PSTORE(u64, x, __builtin_bswap64(y))
#endif

#define REX_MEMORY_SIZE 0x100000000ull

#define REX_IS_MMIO_ADDR(addr) ((addr) >= 0x7F000000u && (addr) < 0x80000000u)

#define REX_MM_STORE_U8(addr, val)                                                   \
  do {                                                                               \
    u32 _mmio_addr = (addr);                                                         \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                              \
      fastcarsfs_mmio_store(_mmio_addr, static_cast<u32>(val));                          \
    } else {                                                                         \
      *(volatile u8*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) = (val); \
    }                                                                                \
  } while (0)

#define REX_MM_STORE_U16(addr, val)                                              \
  do {                                                                           \
    u32 _mmio_addr = (addr);                                                     \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                          \
      fastcarsfs_mmio_store(_mmio_addr, static_cast<u32>(val));                      \
    } else {                                                                     \
      *(volatile u16*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =   \
          __builtin_bswap16(val);                                                \
    }                                                                            \
  } while (0)

#define REX_MM_STORE_U32(addr, val)                                              \
  do {                                                                           \
    u32 _mmio_addr = (addr);                                                     \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                          \
      fastcarsfs_mmio_store(_mmio_addr, static_cast<u32>(val));                      \
    } else {                                                                     \
      *(volatile u32*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =   \
          __builtin_bswap32(val);                                                \
    }                                                                            \
  } while (0)

#define REX_MM_STORE_U64(addr, val)                                              \
  do {                                                                           \
    u32 _mmio_addr = (addr);                                                     \
    if (REX_IS_MMIO_ADDR(_mmio_addr)) {                                          \
      u64 _v64 = static_cast<u64>(val);                                          \
      fastcarsfs_mmio_store(_mmio_addr, static_cast<u32>(_v64 >> 32));               \
      fastcarsfs_mmio_store(_mmio_addr + 4, static_cast<u32>(_v64));                 \
    } else {                                                                     \
      *(volatile u64*)(base + _mmio_addr + REX_PHYS_HOST_OFFSET(_mmio_addr)) =   \
          __builtin_bswap64(val);                                                \
    }                                                                            \
  } while (0)

#define REX_MM_LOAD_U8(addr)                                    \
  (REX_IS_MMIO_ADDR(addr) ? static_cast<u8>(fastcarsfs_mmio_load(addr)) \
                          : *(volatile u8*)(base + (addr) + REX_PHYS_HOST_OFFSET(addr)))

#define REX_MM_LOAD_U16(addr)                                    \
  (REX_IS_MMIO_ADDR(addr) ? static_cast<u16>(fastcarsfs_mmio_load(addr)) \
                          : __builtin_bswap16(*(volatile u16*)(base + (addr) + REX_PHYS_HOST_OFFSET(addr))))

#define REX_MM_LOAD_U32(addr)                       \
  (REX_IS_MMIO_ADDR(addr) ? fastcarsfs_mmio_load(addr) \
                          : __builtin_bswap32(*(volatile u32*)(base + (addr) + REX_PHYS_HOST_OFFSET(addr))))

#define REX_MM_LOAD_U64(addr)                                                                \
  (REX_IS_MMIO_ADDR(addr)                                                                    \
       ? ((static_cast<u64>(fastcarsfs_mmio_load(addr)) << 32) | fastcarsfs_mmio_load((addr) + 4)) \
       : __builtin_bswap64(*(volatile u64*)(base + (addr) + REX_PHYS_HOST_OFFSET(addr))))

//=============================================================================
// Calls
//=============================================================================

#define REX_CALL_FUNC(x) x(ctx, base)

#define REX_LOOKUP_FUNC(x, y) \
  (*(PPCFunc**)(x + REX_IMAGE_BASE + REX_IMAGE_SIZE + (u64(u32(y) - REX_CODE_BASE) * 2)))

#define REX_CALL_INDIRECT_FUNC(x)                                               \
  do {                                                                          \
    uint32_t rex_indirect_target_ = (uint32_t)(x);                              \
    PPCFunc* rex_indirect_func_;                                                \
    if ((uint32_t)(rex_indirect_target_ - REX_CODE_BASE) <                      \
        REX_CODE_SIZE + REX_THUNK_RESERVE_SIZE) [[likely]] {                    \
      rex_indirect_func_ = REX_LOOKUP_FUNC(base, rex_indirect_target_);         \
    } else {                                                                    \
      rex_indirect_func_ = nullptr;                                             \
    }                                                                           \
    if (!rex_indirect_func_) [[unlikely]] {                                     \
      ctx.last_indirect_target = rex_indirect_target_;                          \
      rex_indirect_func_ = fastcarsfs_resolve_indirect(rex_indirect_target_);       \
    }                                                                           \
    rex_indirect_func_(ctx, base);                                              \
  } while (0)

//=============================================================================
// Misc
//=============================================================================

#define REX_SET_FLUSH_MODE(enable) \
  do { if (enable) ctx.fpscr.enableFlushMode(); else ctx.fpscr.disableFlushMode(); } while (0)

#define REX_UNIMPLEMENTED(addr, opcode) fastcarsfs_unimplemented("Unimplemented PPC instruction: " opcode)

#define REX_QUERY_TIMEBASE() fastcarsfs_query_timebase()

// Memory barriers (sync, lwsync, eieio; codegen builders/system.cpp). x86: lwsync/eieio are only
// compiler barriers; ARM64 (Android, iPhone, Mac) needs the dmb: the game releases its locks
// with "lwsync; stw 0", and without it the unlock can become visible before the writes made
// under the lock (another thread then reads a half-built list).
#define REX_SYNC() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define REX_LWSYNC() __atomic_thread_fence(__ATOMIC_ACQ_REL)
#define REX_EIEIO() __atomic_thread_fence(__ATOMIC_RELEASE)

inline void ppc_trap(PPCContext& ctx, u8* base, u16 trap_type) {
  if (trap_type != 25) fastcarsfs_trap(&ctx, base, trap_type);
}

#define REX_CHECK_GLOBAL_LOCK() fastcarsfs_global_lock_check()
#define REX_ENTER_GLOBAL_LOCK() fastcarsfs_global_lock_enter()
#define REX_LEAVE_GLOBAL_LOCK() fastcarsfs_global_lock_leave()
