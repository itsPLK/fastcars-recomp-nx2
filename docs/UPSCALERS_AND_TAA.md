# Anti-Aliasing & Upscaling Architecture (Switch 2 / Native Renderer)

This document describes the anti-aliasing and upscaling modes in the native renderer ([fastcars/src/native/backend.cpp](../fastcars/src/native/backend.cpp), [fastcars/src/native/shaders/native_taa.hlsl](../fastcars/src/native/shaders/native_taa.hlsl), [fastcars/src/native/shaders/native_fsr.hlsl](../fastcars/src/native/shaders/native_fsr.hlsl)), how our 2-pass temporal upscaler compares to AMD FidelityFX FSR 2 / 3.1, and the roadmap toward hardware DLSS on Switch 2.

---

## 1. Available Modes & Settings Menu Mapping

The Switch 2 settings menu (`[-]` + `[+]`, implemented in `BuildSwitch` in [fastcars/src/native/menu.cpp](../fastcars/src/native/menu.cpp)) exposes independent **Resolution**, **Anti-aliasing**, **Upscaler (< 100%)**, **Sharpening (RCAS)** and **Frame generation** controls (object motion vectors are always on; `--fastcars_native_taa_objects=false` still turns them off), persisted to `args.txt`.

| Mode | Resolution | Anti-Aliasing | Upscaler (`< 100%`) | Cvars | How It Works |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Native + TAA** | `100%` (`1280×720`) | `TAA (temporal)` | *(N/A at 100%)* | `render_scale=1`, `taa=true` | 8-phase Halton jitter on 3D draws; replaces the game's FXAA draw before HUD with 2-pass temporal AA + RCAS. |
| **Native + FXAA** | `100%` (`1280×720`) | `FXAA (game)` | *(N/A at 100%)* | `render_scale=1`, `taa=false` | Original Xbox 360 FXAA shader at `1280×720`. |
| **FSR 2-Style Temporal Upscaler** *(default `< 100%`)* | `50%–90%` | `TAA (temporal)` | `FSR 2 (temporal, full-res HUD)` | `render_scale<1`, `taa=true`, `taa_upscale=true` | 3D scene rendered at `scene_w × scene_h` with $\lceil 8 / \text{scale}^2 \rceil$-phase Halton jitter and $\log_2(\text{scale})$ texture mip bias. Upscales and accumulates into `1280×720` **before HUD**, so radar/text/menus render at native `1280×720`. |
| **TAA + FSR 1 Spatial** | `50%–90%` | `TAA (temporal)` | `FSR 1 (spatial)` | `render_scale<1`, `taa=true`, `taa_upscale=false`, `fsr=true` | Resolves TAA at `scene_w × scene_h`, draws HUD at `scene_w × scene_h`, then runs AMD FSR 1.0 (EASU + RCAS) at present time to `1280×720`. |
| **TAA + Bilinear** | `50%–90%` | `TAA (temporal)` | `Bilinear` | `render_scale<1`, `taa=true`, `taa_upscale=false`, `fsr=false` | Resolves TAA at `scene_w × scene_h`, draws HUD at `scene_w × scene_h`, bilinear stretch at present. |
| **FXAA + FSR 1 Spatial** | `50%–90%` | `FXAA (game)` | `FSR 1 (spatial)` | `render_scale<1`, `taa=false`, `fsr=true` | Game's FXAA + HUD at `scene_w × scene_h`, then AMD FSR 1.0 (EASU + RCAS) at present time to `1280×720`. |
| **FXAA + Bilinear** | `50%–90%` | `FXAA (game)` | `Bilinear` | `render_scale<1`, `taa=false`, `fsr=false` | Game's FXAA + HUD at `scene_w × scene_h`, bilinear stretch at present. |

---

## 2. Two-Pass Temporal Upscaler Pipeline (`native_taa.hlsl`)

Our temporal upscaler intercepts the game's FXAA draw (`kTaaHookPs = 0x040C1B12B017260A`, where the tonemapped pre-HUD scene is bound in fetch slot 6):

1. **Pre-Resolve Scene Setup (`TaaBeginFrame`, `TaaCapture`, `TaaRecordDraw`)**:
   - **Sub-pixel camera jitter**: Perspective 3D draws reading `gWorldViewProj` (`c8..c11`) receive a Halton(2, 3) sub-pixel offset via `g_NdcOffset`, with sequence length $\min(32, \lceil 8 / \text{rs}^2 \rceil)$ (e.g., 15 phases at 75% scale, 18 phases at 67% scale).
   - **Negative texture LOD bias**: Scene samplers apply `mipLodBias = log2(rs)` so textures sampled at `960×540` retain native `1280×720` mip sharpness.
   - **Moving rigid-body tracking (`taa objects`)**: G-buffer draws whose `gWorld` (`c0..c3`) moved relative to the camera anchor since the previous frame are matched by mesh key and replayed with depth-equal testing into an object ID target (`taa.id_tex`, `R8_UNORM`) with per-object $4\times 4$ reprojection matrices uploaded to `taa.mat_tex` (`4×256` `R32G32B32A32_FLOAT`).
     Forward draws that write depth over the G-buffer (vehicle glass and lights: alpha-tested, 1 target) are tracked too; their `c8..c11` is not the G-buffer camera's `gWorldViewProj`, so they are placed with `gWorld * VP`, and their ID replay is depth-equal so texels the alpha test discarded keep the ID of what shows through. Shaders that read only `c0..c2` of `gWorld` get their translation from `gWorldViewProj * VP⁻¹`.

2. **Pass 1 — Motion Vector Dilation & Disocclusion (`PSTaaPrepare`, `scene_w × scene_h` $\to$ `taa.mv_tex[cur]`, `RGBA32F`)**:
   - **3×3 Foreground Depth Dilation**: Finds the nearest reversed-Z depth `d_max`, farthest `d_min`, and foreground texel coordinate `near_q` in the 3×3 neighborhood.
   - **2D Velocity Vector (`.xy`)**: Evaluates `near_q`'s per-object reprojection matrix (if `id_tex[near_q] != 0`) or the camera reprojection matrix (`vp_cur⁻¹ * vp_prev`) to produce the dilated 2D UV motion vector `motion_uv`.
   - **Depth-Clip & Object-ID Disocclusion (`.z`)**: Compares the reprojected clip depth (`prev.z / prev.w`) and moving-object status against the 2×2 quad around `history_uv` in the previous frame's `taa.mv_tex[prev].w`. Outputs a disocclusion factor in $[0, 1]$.
   - **Packed Depth + Object Flag (`.w`)**: Stores `cur_z` in $[0, 1]$ for static background texels and `-(cur_z + 1.0)` in $[-2, -1]$ for moving-object texels so the next frame's `PSTaaPrepare` can detect both depth-clip disocclusion and object-to-background transitions in a single fetch.

3. **Pass 2 — Spatial-Temporal Reconstruction (`PSTaa`, `1280×720` $\to$ `taa.tex[cur]`, `RGBA16F`)**:
   - **Perceptual Tonemapped YCoCg**: Applies Karis luminance weighting $1 / (1 + L)$ before converting RGB to YCoCg to suppress sub-pixel HDR/specular fireflies.
   - **12-Tap Deringed 2D Lanczos-2 Upsampling**: Reconstructs the current frame at the unjittered output position using a 12-tap cross-footprint separable 2D Lanczos-2 polynomial filter, clamped to the 3×3 YCoCg `[box_min, box_max]` to prevent negative-lobe ringing.
   - **Gaussian 3×3 Variance Clipping**: Computes distance-weighted first and second YCoCg moments ($\mu, \sigma$) and clips the 9-tap Catmull-Rom bicubic history sample toward $\mu$ against the variance ellipsoid $\mu \pm \gamma \sigma$.
   - **1-Pixel Thin-Feature Lock (`thin_lock`)**: Detects 1-pixel horizontal/vertical luminance ridges (fences, wires, poles, text) and widens the variance acceptance box when not disoccluded.
   - **Auto-Reactive Transparency & Shading Rejection (`reactive`)**: Detects when an entire 3×3 patch shifts in YCoCg beyond local contrast $\sigma$ despite static G-buffer depth (animated water waves, smoke/sparks, car glass, moving shadows) and tightens $\gamma$ / reduces history accumulation to prevent ghost trails.

4. **Pass 3 — FidelityFX RCAS (`fsr_rcas_ps`, `1280×720` $\to$ game target)**:
   - Applies Robust Contrast-Adaptive Sharpening (`fastcars_native_taa_sharpness`) as it copies `taa.tex[cur]` into the full-size `1280×720` output target right before HUD rendering begins.

---

## 3. Comparison with Full AMD FidelityFX FSR 2 / 3.1

| Feature | Full AMD FSR 2 / 3.1 | Our `fsr` Branch Pipeline | Status |
| :--- | :--- | :--- | :--- |
| **Pre-HUD hook & Native HUD** | Upscales scene before UI; UI drawn at native res | Replaces FXAA right before HUD; HUD drawn at native `1280×720` | **Matched** |
| **Sub-pixel camera jitter & Mip bias** | Halton(2, 3) sequence scaled by $(1/\text{scale})^2$, negative LOD bias | Halton(2, 3) up to 32 phases + $\log_2(\text{scale})$ mip bias on scene draws | **Matched** |
| **3×3 Depth Dilation + 2D Motion Vectors** | Reconstructs dilated foreground velocity | `PSTaaPrepare` dilates 3×3 reversed-Z depth + replays moving G-buffer objects (`id_tex` / `mat_tex`) | **Matched** |
| **Disocclusion (Depth Clip)** | Reprojects depth and compares against previous frame's depth neighborhood | `PSTaaPrepare` tests reprojected depth + moving-object ID transitions against `mv_tex[prev]` 2×2 quad | **Matched** |
| **Spatial Upsampling Kernel** | Deringed 12-tap 2D Lanczos-2 in tonemapped YCoCg | Deringed 12-tap 2D Lanczos-2 in perceptual Karis YCoCg (`PSTaa`) | **Matched** |
| **History Resampling & Sharpening** | Catmull-Rom bicubic history + RCAS pass | 9-tap Catmull-Rom bicubic history + FidelityFX RCAS pass | **Matched** |
| **Thin-Feature Lock** | Dedicated lock pass + luminance stability | 1-px ridge detector (`thin_lock`) in `PSTaa` | **Simplified (1-pass)** |
| **Reactive / Transparency Mask** | Dedicated engine reactive mask or auto-reactive pass | Statistical 3×3 YCoCg patch-shift auto-reactivity (`reactive`) in `PSTaa`; optional pre-alpha vs post-alpha diff possible | **Statistical auto-reactive** |
| **Skinned Bone Motion Vectors** | Per-bone previous-frame matrices for walking peds | Rigid `gWorld` object tracking (cars, wheels, props; walking ped limbs rely on `reactive` + variance clip) | **Rigid objects only** |
| **GPU Cost on Switch 2** | ~8 compute passes + layout barriers (~1.2–1.8 ms) | **2 fullscreen fragment passes + RCAS** (~0.25 ms, zero compute-to-graphics barrier stalls on NVK) | **Faster on Switch 2** |

---

## 4. Frame Generation (`PSTaaTileMv` + `PSFrameGen`, `--fastcars_native_frame_gen`)

When **Anti-aliasing** is set to `TAA (temporal)`, **Frame generation** doubles the output frame rate (`30 -> 60 fps`) using the clean pre-HUD resolved buffers (`taa.tex[cur]`, `taa.tex[prev]`), dilated 2D motion vectors (`taa.mv_tex[cur]`), and the final front buffer (for stationary HUD isolation):

| Option | Cvar | Sequence | Trade-off |
| :--- | :--- | :--- | :--- |
| `Off` | `fastcars_native_frame_gen=0` | `N` | Native pacing, zero generated frames. |
| `Interpolated (2x fps, smooth)` | `fastcars_native_frame_gen=1` | `N-0.5 -> N` | Highest visual quality (uses both `N-1` and `N` to resolve disocclusions), adds ~1 frame of presentation latency. |
| `Extrapolated (2x fps, low latency)` | `fastcars_native_frame_gen=2` | `N -> N+0.5` | Lowest input latency (`N` presents immediately), projects `N` forward by half a frame. |

### Pipeline Details
- **16×16 Tile Velocity Summary (`PSTaaTileMv`)**: Downsamples `taa.mv_tex[cur]` into `tile_w × tile_h` `RGBA32F` storing both the fastest moving object velocity (`.xy`) and foreground nearest-depth velocity (`.zw`) per tile.
- **Trajectory-Verified Warping (`PSFrameGen`)**: Tests candidate velocities along `q_uv = uv - dir * v` (`dir = +0.5` interpolated, `-0.5` extrapolated) against `mv_tex[cur]`. The per-candidate test is one load and loose (`max(1.5 px, 20%)`); only the winner is verified: the texel its own vector leads to must agree with it (`max(1.5 px, 8%)`), otherwise the pixel is treated as hidden background, so a fast road cannot reach across a slow car. (Verifying every candidate with a second load cost ~35 ms a frame on Switch 2.)
- **Depth at the generated instant**: `mv_tex.z` packs the disocclusion (integer part, 0..63) with the surface's `z_prev / z_cur` (fraction). Candidates are ordered by the depth they have at `N -/+ 0.5` (`FgDepthAt`, linear in `1 / z`), not by their depth in frame `N`: in a chase camera the road below the car is nearer than the car in frame `N`, but was still under it half a frame earlier.
- **Hidden background**: where nothing in frame `N` lands on `uv`, the background is the farther of the surface at `uv` and the one its vector leads to. Interpolated frames take it from `N-1` with that background's own motion; extrapolated frames fill from the background beside the occluder instead of warping the occluder itself.
- **Stationary HUD Preservation**: Compares `front_descriptor` (post-HUD frame $N$) against matching RCAS-sharpened `taa.tex[cur]` (pre-HUD frame $N$) so text, radar, and crosshairs stay crisp and unwarped.
- **VSync / Pacing**: With `fastcars_native_vsync=true`, `VK_PRESENT_MODE_FIFO_KHR` paces both presents onto consecutive VBlanks; with `fastcars_native_vsync=false`, the submitter delays the second present by half the tracked guest frame interval.

---

### Frame generation diagnostics

- `--fastcars_native_shot_fg=true` with `--fastcars_native_shot_frames=N` (and `--fastcars_native_shot_max`): screenshots are 2560x720, the generated frame on the left and the real frame it was made from on the right (`logs/shots`).
- `--fastcars_native_frame_gen_debug=true`: generated frames are tinted by how each pixel was made: green = a Frame N surface lands there (yellow while Frame N-1 is blended in), red = hidden background filled from Frame N-1, blue = Frame N-1 refused (it does not show that surface on its way to the pixel: `FgPrevIsSurface`), cyan = hidden background off the screen in Frame N-1.
- What these found (2026-11-17): the hit test needs fixed-point steps where the road's motion changes with height; Frame N-1 must only be used where it shows the same surface moving to the pixel (otherwise copies of the car's underside appeared below the car and at the screen's lower edge).

## 5. Diagnosing Ghosts (missing or wrong object motion vectors)

A part that ghosts in generated frames (or smears under TAA) while its surroundings do not almost always has no object motion vector: it gets the camera's, i.e. the road's. To find out which draws and why, on a console without a keyboard, add to `args.txt`:

```
--fastcars_native_taa_debug=1      # picture = motion vectors: red = speed, green = object vector, blue = disocclusion
--fastcars_native_shot_frames=45   # logs/shots/native_NNNNNN.ppm every 45 guest frames of gameplay, 60 at most
--fastcars_native_taa_diag=true    # the log lines below
```

- **Screenshots**: a tracked object is green (yellow when fast). A part of a moving car that is red or black without green is untracked. Broken glass and lights are simply absent: compare intact cars. Frame generation does not run while the debug view is on.
- **`taa objects: per frame, ...`** (every 600 frames): how many scene draws fell out at each step of `TaaRecordDraw`: `state` (no depth test/write, alpha to mask, rect/inline), `no gWorld` (the shader reads neither `c0..c3` nor `c0..c2`), `mesh new` (mesh key not drawn in the previous frame: vertex shader, stream 0 address/offset, index address, start, count, base vertex), `no instance near` (every previous instance already matched or > 10 m away), `not moving` (< 0.15 px of own motion: the normal case for the static world), `no perspective WVP` (nothing usable in `c8..c11`).
- **`taa objects: <reason> vs ... ps ...`**: each rejected vertex shader once per reason, with the constant registers it reads, its buffers, `c0`, `c3` and `max |WVP - VP|` (≈ 0 means the world matrix is identity: static geometry).
- **`taa probe: <outcome> | ...`**: every kind of depth-writing scene draw within 14 units of the camera near the screen centre (the player's car in the chase camera), with target count, `RB_DEPTHCONTROL`, `RB_COLORCONTROL` and what became of it. `targets 4` is the G-buffer, `targets 1` the forward pass.

What this found on the `fsr` branch (2026-11-17): vehicle glass and lights are forward draws (`targets 1`) with the alpha test and depth writes on, whose `c8..c11` is not `gWorld * VP`; some vehicle shaders read only `c0..c2`. Vehicle wheels use a shader without `c8`: world matrix at `c64..c67`, `gWorldViewProj` at `c68..c71` (found with the register dump of `no perspective WVP` lines).

---

## 6. Roadmap & Future Enhancements

1. **Exact Engine Pre-Alpha Reactive Mask (Optional)**:
   - Compare the G-buffer / pre-transparency scene color against the pre-HUD color at `TaaResolve` time so alpha-blended particles, water, and glass produce an exact per-pixel reactive weight in `PSTaaPrepare`.
2. **Skinned Pedestrian Bone Velocity (Optional)**:
   - Match skinned G-buffer draws and keep the previous frame's bone palette upload so animated pedestrian limbs have exact motion vectors in `taa objects`.
3. **Switch 2 Hardware DLSS Integration**:
   - Because `PSTaaPrepare` already generates a dedicated `scene_w × scene_h` 2D velocity buffer (`taa.mv_tex[cur].xy`), reversed-Z scene depth (`depth->texture`), jittered pre-HUD scene color (`scene`), and jitter offsets `(taa.jx, taa.jy)` before the native-resolution HUD pass, a future Switch 2 DLSS backend can plug directly into `TaaResolve` using the exact same inputs.
