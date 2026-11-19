// fastcars - on-screen gamepad, Switch 2 build: none yet (the SDL3 touch code does not build there).
// The console's touch screen can drive it later through HID (see docs/HANDOFF_SWITCH2.md).

#include "touch_controls.h"

#include <rex/input/input_driver.h>

namespace fastcars::native::touch {

std::unique_ptr<rex::input::InputDriver> CreateDriver() { return nullptr; }
bool Visible() { return false; }
void Draw(ImDrawList*, float, float) {}

}  // namespace fastcars::native::touch
