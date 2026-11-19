#include "menu.h"

#include "touch_controls.h"

#include "paths.h"

#include <fmt/format.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace fastcars::native::menu {

namespace {

struct Input {
  enum Type { kMove, kButton, kWheel, kKey } type;
  float x = 0, y = 0;
  int code = 0;
  bool down = false;
};

std::mutex g_input_mutex;
std::vector<Input> g_input;
std::atomic<bool> g_open{false};

ImGuiContext* g_context = nullptr;
bool g_font_uploaded = false;
float g_scale = 1.0f;

// Every setting the menu shows, by cvar name. Saved to fastcars_settings.ini as name=value.
constexpr const char* kSettings[] = {
    "fastcars_fps_limit",
    "fastcars_native_vsync",
    "fastcars_native_fps",
    "fastcars_native_tile_penumbra_all",
    "fastcars_native_async_pipelines",
    "gpu_skip_rendering_lag_ms",
    "fastcars_native_exact_cache",
    "fastcars_native_texture_recheck_frames",
    "fastcars_native_frame_gen",
    "fastcars_touch_controls",
};

std::filesystem::path SettingsPath() { return ExeDir() / "fastcars_settings.ini"; }

void SaveSettings() {
  std::ofstream out(SettingsPath());
  out << "# Native renderer settings (Esc menu). name=value, applied over the command line.\n";
  for (const char* name : kSettings) out << name << '=' << rex::cvar::GetFlagByName(name) << '\n';
}

bool GetBool(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }
void SetBool(const char* name, bool value) { rex::cvar::SetFlagByName(name, value ? "true" : "false"); }

// Presets: the two ends of the trade-off. "Best image" draws everything exactly as soon as it
// appears (new pipelines are built on the spot, a short hitch the first time an effect shows up)
// with the per-pixel sun shadows; "Max performance" keeps the frame moving (new effects are
// skipped for a frame or two while they compile), lets the game skip shadow work per tile and
// lets the emulated GPU trail the game less.
// How far the emulated GPU trails the game's kicks (ms): 6 is the tested value (foliage stopped
// flickering while driving); 2 flickered. 4 is ~10% faster on Windows (88 -> 98 fps running
// around, 10-29) and no worse than 6 in a headless flicker-catch A/B; not yet checked driving.
constexpr const char* kSafeGpuLag = "6";
constexpr const char* kFastGpuLag = "4";

void ApplyPreset(bool best) {
  SetBool("fastcars_native_tile_penumbra_all", best);
  SetBool("fastcars_native_async_pipelines", !best);
  rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", best ? kSafeGpuLag : kFastGpuLag);
  if (!best) {
    rex::cvar::SetFlagByName("fastcars_fps_limit", "0");
    SetBool("fastcars_native_vsync", false);
  }
  SaveSettings();
}

void EnsureContext(uint32_t h) {
  if (g_context) return;
  IMGUI_CHECKVERSION();
  ImGuiContext* previous = ImGui::GetCurrentContext();
  g_context = ImGui::CreateContext();
  ImGui::SetCurrentContext(g_context);
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  g_scale = std::max(1.0f, float(h) / 720.0f);
  ImFontConfig font;
  font.SizePixels = 15.0f * g_scale;
  io.Fonts->AddFontDefault(&font);
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 4.0f;
  style.ScaleAllSizes(g_scale);
  if (previous) ImGui::SetCurrentContext(previous);
}

void Push(const Input& in) {
  if (!g_open) return;
  std::lock_guard lock(g_input_mutex);
  if (g_input.size() < 1024) g_input.push_back(in);
}

ImGuiKey ToImGui(Key key) {
  switch (key) {
    case Key::kUp: return ImGuiKey_UpArrow;
    case Key::kDown: return ImGuiKey_DownArrow;
    case Key::kLeft: return ImGuiKey_LeftArrow;
    case Key::kRight: return ImGuiKey_RightArrow;
    case Key::kEnter: return ImGuiKey_Enter;
    case Key::kSpace: return ImGuiKey_Space;
    case Key::kTab: return ImGuiKey_Tab;
  }
  return ImGuiKey_None;
}

#ifdef __SWITCH__
}  // namespace
}  // namespace fastcars::native::menu
namespace fastcars::switch2 {
void ReapplyClocks();  // switch2_clocks.cpp
}
namespace fastcars::native::menu {
namespace {
// Switch 2: the settings live in args.txt (next to fastcars.nro), the file the installer's settings
// command edits too; the menu changes options there in place (fastcars-nx2.py apply_setting).
// value empty: the option is removed (its default applies).
void SetArg(const std::string& name, const std::string& value) {
  const std::filesystem::path path = ExeDir() / "args.txt";
  std::vector<std::string> lines;
  {
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
  }
  const std::string option = "--" + name, prefix = option + "=";
  bool done = false;
  std::vector<std::string> out;
  for (const std::string& line : lines) {
    const size_t hash = line.find('#');
    const std::string code = line.substr(0, hash), comment = hash == std::string::npos ? "" : line.substr(hash);
    std::vector<std::string> words;
    bool had = false;
    for (size_t i = 0; i < code.size();) {
      while (i < code.size() && code[i] == ' ') i++;
      size_t j = code.find(' ', i);
      if (j == std::string::npos) j = code.size();
      if (j > i) {
        std::string w = code.substr(i, j - i);
        if (w.rfind(prefix, 0) == 0) had = true;
        else words.push_back(w);
      }
      i = j;
    }
    if (!had) {
      out.push_back(line);
      continue;
    }
    if (!value.empty() && !done) {
      words.push_back(prefix + value);
      done = true;
    }
    if (words.empty() && comment.empty()) continue;
    std::string joined;
    for (const auto& w : words) joined += (joined.empty() ? "" : " ") + w;
    out.push_back(comment.empty() ? joined : joined + (joined.empty() ? "" : "  ") + comment);
  }
  if (!value.empty() && !done) {
    auto at = std::find_if(out.begin(), out.end(), [](const std::string& l) { return l.rfind("# Options", 0) == 0; });
    out.insert(at, prefix + value);
  }
  std::ofstream f(path, std::ios::trunc);
  for (const auto& l : out) f << l << '\n';
  REXLOG_INFO("[native] menu: {} {} (args.txt)", option, value.empty() ? "removed" : "= " + value);
}

void Set(const char* name, const std::string& value, const std::string& args_value) {
  rex::cvar::SetFlagByName(name, value);
  SetArg(name, args_value);
}

void BuildSwitch(uint32_t fps, double frame_ms) {
  ImGui::SetNextWindowPos(ImVec2(40.0f * g_scale, 40.0f * g_scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(580.0f * g_scale, 0.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  ImGui::Text("%u fps  (%.1f ms per frame)", fps, frame_ms);
  ImGui::Separator();

  // Render resolution (live: the scaled targets are made again at the next frame): the 3D scene's
  // size. The percentage is of the screen: the game's 1280x720, or 1920x1080 with the 1080p display
  // (the scene is then upscaled to 1080p, so 1280x720 is 67%).
  static constexpr double kScales[] = {1.0, 0.9, 0.85, 0.75, 0.67, 0.5};
  const bool display_1080p = GetBool("fastcars_native_display_1080p");
  const double screen = display_1080p ? 1.5 : 1.0;
  static std::string scale_labels[7];
  const char* scale_names[7];
  for (int i = 0; i < 6; i++) {
    scale_labels[i] = fmt::format("{}x{} ({:.0f}%)", uint32_t(std::lround(1280 * kScales[i])),
                                  uint32_t(std::lround(720 * kScales[i])), 100.0 * kScales[i] / screen);
    scale_names[i] = scale_labels[i].c_str();
  }
  scale_names[6] = "Custom (args.txt)";
  const double scale = std::atof(rex::cvar::GetFlagByName("fastcars_native_render_scale").c_str());
  int schoice = 6;
  for (int i = 0; i < 6; i++)
    if (std::abs(scale - kScales[i]) < 0.003) schoice = i;
  ImGui::SetNextItemWidth(280.0f * g_scale);
  if (ImGui::Combo("Resolution", &schoice, scale_names, schoice == 6 ? 7 : 6) && schoice < 6) {
    const std::string v = fmt::format("{:g}", kScales[schoice]);
    Set("fastcars_native_render_scale", v, v);  // explicit: the default differs per platform (Switch 2: 1, mobile: 0.75)
  }
  bool d1080 = display_1080p;
  if (ImGui::Checkbox("1080p display (restart the game)", &d1080))
    Set("fastcars_native_display_1080p", d1080 ? "true" : "false", d1080 ? "true" : "");

  // Anti-aliasing: TAA (replaces the game's FXAA draw before the HUD) vs the game's own FXAA.
  bool taa = GetBool("fastcars_native_taa");
  bool taa_upscale = GetBool("fastcars_native_taa_upscale");
  bool fsr1 = GetBool("fastcars_native_fsr");
  int aa_choice = taa ? 0 : 1;
  const char* aa_names[] = {"TAA (temporal)", "FXAA (game)"};
  ImGui::SetNextItemWidth(280.0f * g_scale);
  if (ImGui::Combo("Anti-aliasing", &aa_choice, aa_names, 2)) {
    taa = (aa_choice == 0);
    Set("fastcars_native_taa", taa ? "true" : "false", taa ? "" : "false");
  }

  // Upscaler when Resolution < 100%:
  //   0 = FSR 2 (temporal, full-res HUD) -> fastcars_native_taa=true, fastcars_native_taa_upscale=true
  //   1 = FSR 1 (spatial, EASU + RCAS)   -> fastcars_native_taa_upscale=false, fastcars_native_fsr=true
  //   2 = Bilinear                       -> fastcars_native_taa_upscale=false, fastcars_native_fsr=false
  int up_choice = (taa && taa_upscale) ? 0 : (fsr1 ? 1 : 2);
  const char* up_names[] = {"FSR 2 (temporal, full-res HUD)", "FSR 1 (spatial)", "Bilinear"};
  ImGui::SetNextItemWidth(280.0f * g_scale);
  if (ImGui::Combo(display_1080p ? "Upscaler" : "Upscaler (< 100%)", &up_choice, up_names, 3)) {
    if (up_choice == 0) {
      if (!taa) {
        taa = true;
        Set("fastcars_native_taa", "true", "");
      }
      taa_upscale = true;
      Set("fastcars_native_taa_upscale", "true", "");
    } else {
      taa_upscale = false;
      fsr1 = (up_choice == 1);
      Set("fastcars_native_taa_upscale", "false", "false");
      Set("fastcars_native_fsr", fsr1 ? "true" : "false", fsr1 ? "" : "false");
    }
  }

  // RCAS sharpening: applies to TAA / FSR 2 (fastcars_native_taa_sharpness, default 1.0, < 0 = off) or
  // FSR 1 (fastcars_native_fsr_sharpness, default 0.2).
  const bool uses_fsr1_rcas = scale < 0.999 && (!taa || !taa_upscale) && fsr1;
  const bool has_rcas = taa || uses_fsr1_rcas;
  if (has_rcas) {
    const char* sharp_cvar = uses_fsr1_rcas ? "fastcars_native_fsr_sharpness" : "fastcars_native_taa_sharpness";
    const double def_sharp = uses_fsr1_rcas ? 0.2 : 1.0;
    const double cur_sharp = std::atof(rex::cvar::GetFlagByName(sharp_cvar).c_str());
    static constexpr double kSharpStops[] = {-1.0, 1.4, 1.0, 0.5, 0.2, 0.0};
    const char* sharp_names[] = {"Off", "Low (1.4 stops)", "Medium (1.0 stops)", "High (0.5 stops)",
                                 "Very high (0.2 stops)", "Maximum (0.0 stops)", "Custom (args.txt)"};
    int sh_choice = 6;
    if (cur_sharp < 0.0 && !uses_fsr1_rcas) {
      sh_choice = 0;
    } else {
      for (int i = 1; i < 6; i++)
        if (std::abs(cur_sharp - kSharpStops[i]) < 0.05) sh_choice = i;
    }
    const int first_item = uses_fsr1_rcas ? 1 : 0;
    int combo_idx = sh_choice - first_item;
    const int count = (sh_choice == 6 ? 7 : 6) - first_item;
    ImGui::SetNextItemWidth(280.0f * g_scale);
    if (ImGui::Combo("Sharpening (RCAS)", &combo_idx, sharp_names + first_item, count)) {
      const int picked = combo_idx + first_item;
      if (picked < 6) {
        const double val = kSharpStops[picked];
        const std::string v = fmt::format("{:g}", val);
        Set(sharp_cvar, v, std::abs(val - def_sharp) < 0.01 ? "" : v);
      }
    }
  }

  if (taa) {
    int fg = std::clamp(std::atoi(rex::cvar::GetFlagByName("fastcars_native_frame_gen").c_str()), 0, 2);
    const char* fg_names[] = {"Off", "Interpolated (2x fps, smooth)", "Extrapolated (2x fps, low latency)"};
    ImGui::SetNextItemWidth(280.0f * g_scale);
    if (ImGui::Combo("Frame generation", &fg, fg_names, 3)) {
      const std::string v = std::to_string(fg);
      Set("fastcars_native_frame_gen", v, v);  // explicit: on by default on Switch 2
    }
  }

  ImGui::Separator();
  int limit = std::atoi(rex::cvar::GetFlagByName("fastcars_fps_limit").c_str());
  int lchoice = limit == 30 ? 0 : limit == 60 ? 1 : limit <= 0 ? 2 : 3;
  const char* limits[] = {"30", "60", "Uncapped", "Custom (args.txt)"};
  ImGui::SetNextItemWidth(280.0f * g_scale);
  if (ImGui::Combo("Frame rate limit", &lchoice, limits, lchoice == 3 ? 4 : 3) && lchoice < 3) {
    const char* v = lchoice == 0 ? "30" : lchoice == 1 ? "60" : "0";
    Set("fastcars_fps_limit", v, v);
  }
  bool counter = GetBool("fastcars_native_fps");
  if (ImGui::Checkbox("FPS counter", &counter)) Set("fastcars_native_fps", counter ? "true" : "false", counter ? "true" : "");
  // CPU boost: the default --switch2_clocks (1702 MHz CPU) or "keep" (the system's clock).
  bool boost = rex::cvar::GetFlagByName("switch2_clocks") != "keep";
  if (ImGui::Checkbox("CPU boost (1702 MHz)", &boost)) {
    Set("switch2_clocks", boost ? "1702,0,0" : "keep", boost ? "" : "keep");
    fastcars::switch2::ReapplyClocks();
  }
  ImGui::Separator();
  ImGui::TextDisabled("[-] + [+]: open / close.  D-pad: move.  A: change.  B: close.");
  ImGui::TextDisabled("Changes are saved to args.txt.");
  ImGui::End();
}
#endif

void Build(uint32_t fps, double frame_ms) {
#ifdef __SWITCH__
  return BuildSwitch(fps, frame_ms);
#endif
  ImGui::SetNextWindowPos(ImVec2(40.0f * g_scale, 40.0f * g_scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(520.0f * g_scale, 0.0f), ImGuiCond_FirstUseEver);
  bool open = true;
  if (!ImGui::Begin("Settings (Esc / B to close)", &open, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  ImGui::Text("%u fps  (%.1f ms per frame)", fps, frame_ms);
  ImGui::Separator();

  ImGui::TextUnformatted("Preset");
  if (ImGui::Button("Best image")) ApplyPreset(true);
  ImGui::SameLine();
  if (ImGui::Button("Max performance")) ApplyPreset(false);
  ImGui::TextDisabled("Compare the fps above after switching.");
  ImGui::Separator();

  bool changed = false;
  int limit = std::atoi(rex::cvar::GetFlagByName("fastcars_fps_limit").c_str());
  int choice = limit == 30 ? 0 : limit == 60 ? 1 : limit <= 0 ? 2 : 3;
  const char* limits[] = {"30 (the game's own limiter)", "60", "Uncapped", "Custom (command line)"};
  ImGui::SetNextItemWidth(260.0f * g_scale);
  if (ImGui::Combo("Frame rate limit", &choice, limits, choice == 3 ? 4 : 3) && choice < 3) {
    rex::cvar::SetFlagByName("fastcars_fps_limit", choice == 0 ? "30" : choice == 1 ? "60" : "0");
    changed = true;
  }

  auto check = [&](const char* label, const char* name, const char* help) {
    bool v = GetBool(name);
    if (ImGui::Checkbox(label, &v)) {
      SetBool(name, v);
      changed = true;
    }
    if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
  };
  check("VSync", "fastcars_native_vsync", "Wait for the display refresh: no tearing, fps capped at the refresh rate.");
  check("FPS counter", "fastcars_native_fps", "Frames per second in the top-left corner.");
  ImGui::Separator();
  ImGui::TextUnformatted("Accuracy");
  check("Per-pixel sun shadows", "fastcars_native_tile_penumbra_all",
        "On: every screen tile computes sun shadows per pixel (no blocky shadows or car reflections).\n"
        "Off: the game's own per-tile shortcut, which goes blocky on this renderer (gray 16x16 blocks).");
  check("Compile new effects in the background", "fastcars_native_async_pipelines",
        "On: no hitch when a new effect first appears; it is skipped for a frame or two instead.\n"
        "Off: it is drawn from its first frame, with a short hitch while it is built.");
  {
    bool fast = std::atoi(rex::cvar::GetFlagByName("gpu_skip_rendering_lag_ms").c_str()) < std::atoi(kSafeGpuLag);
    if (ImGui::Checkbox("Low GPU latency", &fast)) {
      rex::cvar::SetFlagByName("gpu_skip_rendering_lag_ms", fast ? kFastGpuLag : kSafeGpuLag);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On: the emulated GPU trails the game by 2 ms instead of 6 (about 20%% more fps).\n"
                        "Off: the tested setting; with On, watch foliage and distant meshes while driving.");
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Textures");
  check("Exact texture checks", "fastcars_native_exact_cache",
        "On: every texture is fully re-checked each frame: no stale gray blocks, but more CPU time.\n"
        "Off: large textures are spot-checked between full checks (faster).");
  {
    int frames = std::atoi(rex::cvar::GetFlagByName("fastcars_native_texture_recheck_frames").c_str());
    ImGui::SetNextItemWidth(200.0f * g_scale);
    if (ImGui::SliderInt("Full re-check every N frames", &frames, 1, 30)) {
      rex::cvar::SetFlagByName("fastcars_native_texture_recheck_frames", std::to_string(frames));
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Lower: textures the game streams in show up sooner (fewer gray blocks), more CPU.");
  }
  ImGui::Separator();
  check("Touch controls", "fastcars_touch_controls", "The on-screen gamepad (hidden while a controller is connected).");
  ImGui::TextDisabled("Controller: RB + Back opens/closes this menu; D-pad + A to change, B to close.");
  if (changed) SaveSettings();
  ImGui::End();
  if (!open) g_open = false;
}

}  // namespace

void OnEscape() {
  bool was_open = g_open.exchange(!g_open.load());
  if (!was_open) {
    std::lock_guard lock(g_input_mutex);
    g_input.clear();
  }
}
bool IsOpen() { return g_open.load(); }
void OnMouseMove(float x, float y) { Push({Input::kMove, x, y}); }
void OnMouseButton(int button, bool down) { Push({Input::kButton, 0, 0, button, down}); }
void OnMouseWheel(float dy) { Push({Input::kWheel, 0, dy}); }
void OnKey(Key key, bool down) { Push({Input::kKey, 0, 0, int(key), down}); }

void TestSet(const std::string& name, const std::string& value) {
  REXLOG_INFO("[native] menu test: {} = {}", name, value);
  rex::cvar::SetFlagByName(name, value);
  if (std::getenv("FASTCARS_IGNORE_SETTINGS")) return;  // benchmark runs leave fastcars_settings.ini alone
  SaveSettings();
  REXLOG_INFO("[native] menu test: saved");
}

void LoadSettings() {
  // FASTCARS_IGNORE_SETTINGS=1: command-line settings only (benchmark runs).
  if (std::getenv("FASTCARS_IGNORE_SETTINGS")) return;
#ifdef __SWITCH__
  return;  // args.txt holds them (SetArg)
#endif
  std::ifstream in(SettingsPath());
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string name = line.substr(0, eq), value = line.substr(eq + 1);
    if (std::find_if(std::begin(kSettings), std::end(kSettings), [&](const char* s) { return name == s; }) ==
        std::end(kSettings))
      continue;
    if (rex::cvar::SetFlagByName(name, value)) REXLOG_INFO("[native] setting {} = {} (fastcars_settings.ini)", name, value);
  }
}

ImDrawData* Frame(uint32_t w, uint32_t h, uint32_t fps, double frame_ms) {
  const bool touch_overlay = touch::Visible();
  if (!g_open && !touch_overlay) return nullptr;
  EnsureContext(h);
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(float(w), float(h));
  io.DeltaTime = 1.0f / 60.0f;
  {
    std::lock_guard lock(g_input_mutex);
    for (const auto& in : g_input) {
      switch (in.type) {
        case Input::kMove: io.AddMousePosEvent(in.x, in.y); break;
        case Input::kButton: io.AddMouseButtonEvent(in.code, in.down); break;
        case Input::kWheel: io.AddMouseWheelEvent(0.0f, in.y); break;
        case Input::kKey: io.AddKeyEvent(ToImGui(Key(in.code)), in.down); break;
      }
    }
    g_input.clear();
  }
  if (!g_font_uploaded) {  // legacy atlas: the backend uploads it before the first draw
    unsigned char* pixels;
    int fw, fh;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &fw, &fh);
  }
  ImGui::NewFrame();
  if (g_open) Build(fps, frame_ms);
  if (touch_overlay) touch::Draw(ImGui::GetForegroundDrawList(), float(w), float(h));
  ImGui::Render();
  ImDrawData* data = ImGui::GetDrawData();
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
  return data;
}

bool TakeFontAtlas(const uint8_t** pixels, int* width, int* height) {
  if (g_font_uploaded || !g_context) return false;
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  unsigned char* p;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&p, width, height);
  *pixels = p;
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
  return true;
}

void SetFontTexture(uint32_t descriptor) {
  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGui::SetCurrentContext(g_context);
  ImGui::GetIO().Fonts->SetTexID(ImTextureID(descriptor));
  g_font_uploaded = true;
  if (previous && previous != g_context) ImGui::SetCurrentContext(previous);
}

}  // namespace fastcars::native::menu
