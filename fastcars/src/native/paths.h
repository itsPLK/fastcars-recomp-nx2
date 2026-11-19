// Default locations for native-renderer data and logs: next to the executable.
#pragma once

#include <filesystem>
#include <string>

#if defined(__APPLE__)
#include <cstdlib>
#include <mach-o/dyld.h>
#endif

namespace fastcars::native {

inline std::filesystem::path ExeDir() {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return std::filesystem::current_path();
  std::error_code ec;
  auto exe = std::filesystem::canonical(buf, ec);
  return ec ? std::filesystem::path(buf).parent_path() : exe.parent_path();
#elif defined(__SWITCH__)
  // Switch 2: the app folder on the SD card (sdmc:/switch/fastcars), which windowed_app_main_switch2 makes the
  // working directory before anything else runs.
  return std::filesystem::current_path();
#else
  std::error_code ec;
  auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::current_path() : exe.parent_path();
#endif
}

// SPIR-V of the game's shaders (census output): <exe dir>/spirv. iPhone release: inside the app
// bundle (the player's Mac translated them into the ipa); a Documents/spirv (dev builds) wins.
inline std::string DefaultSpirvDir() {
  return (ExeDir() / "spirv").string();
}

// Dumps and probe output: <exe dir>/logs/<sub>.
inline std::string LogPath(const std::string& sub) { return (ExeDir() / "logs" / sub).string(); }

}  // namespace fastcars::native
