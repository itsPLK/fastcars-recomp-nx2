// fastcars - ReXGlue Recompiled Project

#if FASTCARS_GAME_DLL
// The game code lives in fastcars_game.dll (Linux: fastcars_game.so, found through the $ORIGIN rpath),
// built by the installer from the player's own discs.
#include <rex/image_info.h>
extern "C" const rex::PPCImageInfo* fastcars_game_image_config();
#define PPCImageConfig (*fastcars_game_image_config())
#else
#include "generated/default/fastcars_init.h"
#endif

#include "fastcars_app.h"

REX_DEFINE_APP(fastcars, FastcarsApp::Create)
