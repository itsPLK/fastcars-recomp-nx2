// The game data install: the player's Disc 1, Disc 2 and Title Update 27, checked against the
// known-good hashes (switch2/data/expected.txt) and copied into HOME/data (game, update, user).
// Used by data_install (main.cpp) in the Switch 2 builder container.
#pragma once

#include <filesystem>
#include <string>

namespace loader {

struct InstallInputs {
  std::wstring disc1;  // Disc 1 (Install): ISO image or extracted folder
  std::wstring disc2;  // Disc 2 (Play): ISO image or extracted folder
  std::wstring tu;     // Title Update 27: the STFS package file or an extracted folder
};

class InstallUi {
 public:
  virtual ~InstallUi() = default;
  virtual void Stage(const std::wstring& text) = 0;   // one line: what is happening now
  virtual void Log(const std::wstring& text) = 0;     // appended to the output and install.log
  virtual void Progress(double fraction) = 0;         // 0..1 for the whole install
  virtual bool Cancelled() = 0;
  virtual bool Ask(const std::wstring& question) = 0;  // yes/no
};

// Copies and checks the game files into home/data. Resumes an interrupted run (data/install_state.txt).
bool RunDataInstall(const std::filesystem::path& home, const std::filesystem::path& expected,
                    const InstallInputs& in, InstallUi& ui, std::wstring* error);
bool IsDataInstalled(const std::filesystem::path& home);

}  // namespace loader
