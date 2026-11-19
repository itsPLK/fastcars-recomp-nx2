// First start without shaders: a screen that explains the black screen and the Fetch shaders step,
// and waits for A. Drawn with the libnx text console on the default window before the renderer
// creates its Vulkan swapchain there; the console releases the window again when it closes.
// Called from the SDK's main (rex_switch2_splash, a weak symbol there), after it has changed to the
// app folder. There is no start screen otherwise.

#include <switch.h>

#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game_title.h"    // GAME_TITLE: from the player's own disc (disc_title.py)
#include "fastcars_version.h"  // FASTCARS_VERSION: v<VERSION>-<commit> (make_version.sh)

namespace {

void Center(int row, const char* text, const char* color = "\x1b[37m") {
  int len = 0;
  for (const char* p = text; *p; ++p) len++;
  int col = (80 - len) / 2 + 1;
  std::printf("\x1b[%d;%dH%s%s\x1b[0m", row, col < 1 ? 1 : col, color, text);
}

// True if the shader folder (args.txt: --fastcars_native_spirv_dir=data/spirv) holds any shader.
bool HaveShaders() {
  DIR* dir = opendir("data/spirv");
  if (!dir) return false;
  bool found = false;
  while (dirent* e = readdir(dir)) {
    const size_t n = std::strlen(e->d_name);
    if (n > 4 && std::strcmp(e->d_name + n - 4, ".spv") == 0) {
      found = true;
      break;
    }
  }
  closedir(dir);
  return found;
}

// First start: the game draws nothing until the builder has translated the shaders it records.
// A starts the game, + returns to the homebrew menu.
void NoShadersScreen(PadState& pad) {
  std::printf("\x1b[2J");
  Center(10, "No shaders yet: first start", "\x1b[33;1m");
  Center(13, "This start records the shaders the game needs. The screen stays black");
  Center(14, "the whole time; you will only hear the game.");
  Center(17, "1. Press A to start the game.", "\x1b[37;1m");
  Center(18, "2. When the black screen turns a little grey (a message), press B.", "\x1b[37;1m");
  Center(19, "3. Wait 3-5 minutes, until you hear the game itself (past the menus).", "\x1b[37;1m");
  Center(20, "4. Quit: press HOME, then Close the game.", "\x1b[37;1m");
  Center(21, "5. On your computer, press Fetch shaders in the builder.", "\x1b[37;1m");
  Center(22, "6. Start " GAME_TITLE " again.", "\x1b[37;1m");
  Center(26, FASTCARS_VERSION, "\x1b[90m");
  Center(31, "[A] Start the game        [+] Back to the homebrew menu", "\x1b[32;1m");
  consoleUpdate(nullptr);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_A) return;
    if (down & HidNpadButton_Plus) {
      consoleExit(nullptr);
      std::exit(0);
    }
    consoleUpdate(nullptr);
    svcSleepThread(16000000);
  }
}

}  // namespace

extern "C" void rex_switch2_splash() {
  // No start screen: only a first start without shaders gets the explanation screen.
  if (HaveShaders()) return;
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  NoShadersScreen(pad);
  consoleExit(nullptr);
}
