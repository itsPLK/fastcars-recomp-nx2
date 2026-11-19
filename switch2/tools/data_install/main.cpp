// data_install: the game files from the player's Disc 1, Disc 2 and Title Update 27, checked against
// switch2/data/expected.txt, into HOME/data (game, update, user) (install.cpp); here in a terminal,
// for the Switch 2 build container.
//   data_install HOME EXPECTED DISC1 DISC2 TU      (exit 0 = installed and verified)
#include <cstdio>
#include <iostream>
#include <string>

#include "install.h"

using namespace loader;

namespace {

std::string Narrow(const std::wstring& s) {
  std::string out;
  for (wchar_t c : s) out += c < 0x80 ? char(c) : '?';
  return out;
}

class TerminalUi : public InstallUi {
 public:
  void Stage(const std::wstring& text) override { std::printf("== %s\n", Narrow(text).c_str()), std::fflush(stdout); }
  void Log(const std::wstring& text) override { std::printf("   %s\n", Narrow(text).c_str()), std::fflush(stdout); }
  void Progress(double fraction) override {
    int pct = int(fraction * 100);
    if (pct != last_) {
      last_ = pct;
      std::printf("PROGRESS %d\n", pct), std::fflush(stdout);
    }
  }
  bool Cancelled() override { return false; }
  // Unattended: a file that does not match the known-good hash is never accepted.
  bool Ask(const std::wstring& question) override {
    std::printf("?? %s -> no\n", Narrow(question).c_str());
    return false;
  }

 private:
  int last_ = -1;
};

std::wstring Wide(const char* s) {
  std::wstring out;
  for (; *s; ++s) out += wchar_t(static_cast<unsigned char>(*s));
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr, "usage: data_install HOME EXPECTED DISC1 DISC2 TU\n");
    return 2;
  }
  InstallInputs in{Wide(argv[3]), Wide(argv[4]), Wide(argv[5])};
  TerminalUi ui;
  std::wstring error;
  if (!RunDataInstall(argv[1], argv[2], in, ui, &error)) {
    std::fprintf(stderr, "data_install: %s\n", Narrow(error).c_str());
    return 1;
  }
  return 0;
}
