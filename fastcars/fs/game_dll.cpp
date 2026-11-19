// fastcars - entry point of the game DLL (Linux: fastcars_game.so) built on the player's machine (see fastcars_pch.h).
#include "fastcars_pch.h"

#define FASTCARSFS_EXPORT __attribute__((visibility("default")))

extern "C" FASTCARSFS_EXPORT const rex::PPCImageInfo* fastcars_game_image_config() {
  return &PPCImageConfig;
}
