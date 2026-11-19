// fastcars - native renderer: temporal anti-aliasing and FSR-style temporal upscaling (--fastcars_native_taa).
// Replaces the game's FXAA draw (the tonemapped scene, before the HUD):
//
// Pass 1 (PSTaaPrepare, at scene_w x scene_h into mv_tex[cur], RGBA32F):
//   - Dilates the 3x3 scene depth (reversed z) to find the foreground texel `near_q`.
//   - Computes the 2D screen-space motion vector `motion_uv` from `near_q`'s per-object transform
//     (if `id_tex[near_q] != 0`) or the camera's reprojection matrix (`R0..R3`).
//   - Compares the reprojected clip depth `prev.z / prev.w` and object status against the previous
//     frame's `mv_tex[prev].w` in a 2x2 footprint around `history_uv` to produce a depth-clip +
//     object-ID disocclusion factor in [0, 1].
//   - Writes float4(motion_uv.x, motion_uv.y, packed_disocclusion_and_depth_ratio, packed_depth_and_id).
//
// Pass 2 (PSTaa, at output w x h into taa.tex[cur], RGBA16F):
//   - Reconstructs the current frame at the unjittered output position with a 12-tap deringed 2D
//     Lanczos-2 filter in perceptual tonemapped YCoCg space (Karis luma weighting).
//   - Computes Gaussian-weighted 3x3 YCoCg moments (mean `mu`, std dev `sigma`) and 1-pixel thin
//     feature confidence (`thin_lock`), clipping the 9-tap Catmull-Rom bicubic history toward `mu`
//     against the YCoCg variance ellipsoid `mu +- gamma * sigma`.
//   - Drops disoccluded history immediately to the 12-tap Lanczos-2 current sample.

struct TaaPush {
    float4 R0;       // previous clip position = ndc.x * R0 + ndc.y * R1 + ndc.z * R2 + R3
    float4 R1;       // (ndc: the guest's clip space, y up, of the current unjittered camera)
    float4 R2;
    float4 R3;
    float4 Size;     // xy = 1 / output size; zw = scene size, in pixels
    float4 Depth;    // x = 1 / viewport z scale, y = viewport z offset; zw = jitter in scene pixels
    float Blend;     // weight of a centred scene sample once the history has settled
    uint Mode;       // 0 = resolve, 1 = show motion vectors + disocclusion, 2 = no history (first frame)
    uint ColorIndex;
    uint HistoryIndex;
    uint DepthIndex;
    uint SamplerIndex;  // linear, clamped
    float Kernel;       // a scene sample's weight: exp(-Kernel * (distance from the output pixel's centre)^2)
    uint Objects;       // low 16 bits = per-pixel object ID texture, high 16 = 4x256 object matrix texture
};

[[vk::push_constant]] ConstantBuffer<TaaPush> g_Taa;
Texture2D<float4> g_Textures[] : register(t0, space0);
SamplerState g_Samplers[] : register(s0, space3);

// g_HalfPixelOffset of the guest's 1280x720 viewport (D3D9 pixel centres), in ndc units.
static const float2 kHalfPixel = float2(1.0 / 1280.0, -1.0 / 720.0);

// Perceptual tonemapped YCoCg (Karis luma weight 1 / (1 + L)): suppresses specular fireflies and
// decouples luminance from chrominance for variance clipping and Lanczos deringing.
float3 RgbToYCoCg(float3 c) {
    c = max(c, 0.0);
    float luma = dot(c, float3(0.25, 0.5, 0.25));
    c *= rcp(1.0 + luma);
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                  0.5 * c.r - 0.5 * c.b,
                 -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 YCoCgToRgb(float3 y) {
    float r = y.x + y.y - y.z;
    float g = y.x + y.z;
    float b = y.x - y.y - y.z;
    float3 c = max(float3(r, g, b), 0.0);
    return c * rcp(max(1.0 - y.x, 1e-4));
}

// AMD FidelityFX Lanczos-2 polynomial approximation over squared distance d2 in [0, 4]:
// (25/16 * (2/5 * d2 - 1)^2 - 9/16) * (1/4 * d2 - 1)^2. Exact 1 at d2=0, exact 0 at d2=1 and d2=4.
float Lanczos2Sq(float d2) {
    d2 = min(d2, 4.0);
    float wb = 0.4 * d2 - 1.0;
    float wa = 0.25 * d2 - 1.0;
    wb *= wb;
    wa *= wa;
    wb = (25.0 / 16.0) * wb - (9.0 / 16.0);
    return wb * wa;
}

// Ray-box clip of history `h` toward neighborhood mean `mu` against `mu +- gamma * sigma` in YCoCg.
float3 ClipHistory(float3 h, float3 mu, float3 sigma, float gamma) {
    float3 extent = max(sigma * gamma, 1e-5);
    float3 delta = h - mu;
    float3 units = abs(delta / extent);
    float max_unit = max(units.x, max(units.y, units.z));
    return max_unit > 1.0 ? mu + delta / max_unit : h;
}

float4 HistoryTap(float2 uv) {
    return g_Textures[g_Taa.HistoryIndex].SampleLevel(g_Samplers[g_Taa.SamplerIndex], uv, 0);
}

// Catmull-Rom from nine bilinear taps: a bilinear history loses its sharpness within a few frames
// of sub-pixel camera motion. Alpha (the accumulated weight) is the centre tap's.
float4 History(float2 uv) {
    float2 inv = g_Taa.Size.xy;
    float2 pos = uv / inv;
    float2 tc1 = floor(pos - 0.5) + 0.5;
    float2 f = pos - tc1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc0 = (tc1 - 1.0) * inv, tc3 = (tc1 + 2.0) * inv, tc12 = (tc1 + w2 / w12) * inv;
    float4 centre = HistoryTap(tc12);
    float3 sum = HistoryTap(float2(tc0.x, tc0.y)).rgb * (w0.x * w0.y) +
                 HistoryTap(float2(tc12.x, tc0.y)).rgb * (w12.x * w0.y) +
                 HistoryTap(float2(tc3.x, tc0.y)).rgb * (w3.x * w0.y) +
                 HistoryTap(float2(tc0.x, tc12.y)).rgb * (w0.x * w12.y) +
                 centre.rgb * (w12.x * w12.y) +
                 HistoryTap(float2(tc3.x, tc12.y)).rgb * (w3.x * w12.y) +
                 HistoryTap(float2(tc0.x, tc3.y)).rgb * (w0.x * w3.y) +
                 HistoryTap(float2(tc12.x, tc3.y)).rgb * (w12.x * w3.y) +
                 HistoryTap(float2(tc3.x, tc3.y)).rgb * (w3.x * w3.y);
    return float4(max(sum, 0.0), centre.a);
}

// mv_tex.z packs the disocclusion (integer part, 0..63) with the surface's depth ratio
// z_prev / z_cur (fraction, halved): PSFrameGen needs the depth a surface has at the generated
// instant, not its depth in frame N (a road rushing toward the camera is nearer in frame N than the
// car it was still behind half a frame earlier).
float PackDisocclusion(float dis, float z_ratio) {
    return round(saturate(dis) * 63.0) + 0.5 * clamp(z_ratio, 0.0, 1.96);
}
float UnpackDisocclusion(float v) { return floor(v) * (1.0 / 63.0); }
float UnpackDepthRatio(float v) { return frac(v) * 2.0; }

// Pass 1 (scene_w x scene_h -> mv_tex[cur], RGBA32F):
//   .xy = dilated 2D UV motion vector (history_uv - uv)
//   .z  = PackDisocclusion(disocclusion in [0, 1] (depth clip + moving-object ID transition),
//                          previous / current depth of this surface)
//   .w  = cur_z in [0, 1] for static pixels, -(cur_z + 1.0) in [-2, -1] for moving-object pixels
float4 PSTaaPrepare(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 scene_size = g_Taa.Size.zw;
    int2 last = int2(scene_size) - 1;
    int2 p = clamp(int2(position.xy), int2(0, 0), last);

    float d_max = 0.0, d_min = 1.0;
    int2 near_q = p;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            float dq = g_Textures[g_Taa.DepthIndex].Load(int3(q, 0)).r;
            if (dq > d_max) {
                d_max = dq;
                near_q = q;
            }
            d_min = min(d_min, dq);
        }
    }

    uint obj_id = 0;
    uint id_tex = g_Taa.Objects & 0xFFFFu;
    if (id_tex != 0)
        obj_id = uint(g_Textures[id_tex].Load(int3(near_q, 0)).r * 255.0 + 0.5);

    float cur_z = saturate((d_max - g_Taa.Depth.y) * g_Taa.Depth.x);
    float packed_w = obj_id != 0 ? -(cur_z + 1.0) : cur_z;
    if (g_Taa.Mode == 2) return float4(0.0, 0.0, PackDisocclusion(1.0, 1.0), packed_w);

    float4 r0 = g_Taa.R0, r1 = g_Taa.R1, r2 = g_Taa.R2, r3 = g_Taa.R3;
    if (obj_id != 0) {
        Texture2D<float4> mat = g_Textures[g_Taa.Objects >> 16];
        r0 = mat.Load(int3(0, int(obj_id), 0));
        r1 = mat.Load(int3(1, int(obj_id), 0));
        r2 = mat.Load(int3(2, int(obj_id), 0));
        r3 = mat.Load(int3(3, int(obj_id), 0));
    }

    // Unjittered UV corresponding to this jittered scene texel:
    float2 uv0 = (position.xy - g_Taa.Depth.zw) / scene_size;
    float2 host = float2(uv0.x * 2.0 - 1.0, 1.0 - uv0.y * 2.0);
    float3 ndc = float3(host - kHalfPixel, (d_max - g_Taa.Depth.y) * g_Taa.Depth.x);
    float4 prev = ndc.x * r0 + ndc.y * r1 + ndc.z * r2 + r3;
    if (prev.w <= 0.0) return float4(0.0, 0.0, PackDisocclusion(1.0, 1.0), packed_w);
    float z_ratio = cur_z > 1e-6 ? max(prev.z / prev.w, 0.0) / cur_z : 1.0;

    float2 motion = prev.xy / prev.w + kHalfPixel - host;  // ndc units
    float2 motion_uv = float2(motion.x, -motion.y) * 0.5;
    float2 history_uv = uv0 + motion_uv;
    if (any(history_uv < 0.0) || any(history_uv > 1.0))
        return float4(motion_uv, PackDisocclusion(1.0, z_ratio), packed_w);

    // Depth-clip & object-ID disocclusion against the 2x2 quad at history_uv in mv_tex[prev]:
    float z_reproj = saturate(prev.z / prev.w);
    float z_spread = max(d_max - d_min, 0.0) * abs(g_Taa.Depth.x);
    float tol = max(z_spread * 2.5, max(z_reproj, 1e-4) * 0.06 + 2e-4);
    int2 h_base = int2(floor(history_uv * scene_size - 0.5));
    bool cur_moving = (obj_id != 0);
    float min_disoccl = 1.0;
    Texture2D<float4> prev_mv = g_Textures[g_Taa.HistoryIndex];
    for (int hy = 0; hy <= 1; hy++) {
        for (int hx = 0; hx <= 1; hx++) {
            int2 hq = clamp(h_base + int2(hx, hy), int2(0, 0), last);
            float pw = prev_mv.Load(int3(hq, 0)).w;
            bool prev_moving = (pw < -0.5);
            float pz = prev_moving ? (-pw - 1.0) : pw;
            float d_dis = saturate((abs(pz - z_reproj) - tol) / max(tol, 1e-4));
            float id_dis = (cur_moving != prev_moving) ? 1.0 : 0.0;
            min_disoccl = min(min_disoccl, max(d_dis, id_dis));
        }
    }
    return float4(motion_uv, PackDisocclusion(min_disoccl, z_ratio), packed_w);
}

// Pass 2 (output w x h -> taa.tex[cur], RGBA16F):
float4 PSTaa(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> color = g_Textures[g_Taa.ColorIndex];
    float2 scene_size = g_Taa.Size.zw;
    float2 scene_pos = uv * scene_size + g_Taa.Depth.zw;
    int2 last = int2(scene_size) - 1;
    int2 p = clamp(int2(floor(scene_pos)), int2(0, 0), last);

    // 3x3 neighbourhood in perceptual tonemapped YCoCg: Gaussian-weighted moments (mu, sigma),
    // deringing box [box_min, box_max], and 1-pixel thin-feature ridge confidence.
    float3 y_c = RgbToYCoCg(color.Load(int3(p, 0)).rgb);
    float3 box_min = y_c, box_max = y_c;
    float3 m1 = 0.0, m2 = 0.0;
    float mw_sum = 0.0;
    float l_n = y_c.x, l_s = y_c.x, l_w = y_c.x, l_e = y_c.x;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            int2 q = clamp(p + int2(x, y), int2(0, 0), last);
            float3 yq = RgbToYCoCg(color.Load(int3(q, 0)).rgb);
            box_min = min(box_min, yq);
            box_max = max(box_max, yq);
            if (x == 0 && y == -1) l_n = yq.x;
            if (x == 0 && y ==  1) l_s = yq.x;
            if (x == -1 && y == 0) l_w = yq.x;
            if (x ==  1 && y == 0) l_e = yq.x;
            float2 d = float2(q) + 0.5 - scene_pos;
            float mw = exp(-3.0 * dot(d, d));
            m1 += yq * mw;
            m2 += yq * yq * mw;
            mw_sum += mw;
        }
    }
    float3 mu = m1 / mw_sum;
    float3 sigma = sqrt(max(m2 / mw_sum - mu * mu, 0.0));

    // 12-tap deringed 2D Lanczos-2 reconstruction around scene_pos in perceptual YCoCg:
    int2 base = int2(floor(scene_pos - 0.5));
    float2 f = (scene_pos - 0.5) - float2(base);
    float wx[4], wy[4];
    [unroll] for (int k = 0; k < 4; k++) {
        float dx = float(k - 1) - f.x;
        float dy = float(k - 1) - f.y;
        wx[k] = Lanczos2Sq(dx * dx);
        wy[k] = Lanczos2Sq(dy * dy);
    }
    float3 lanczos_sum = 0.0;
    float lanczos_w = 0.0;
    [unroll] for (int iy = 0; iy < 4; iy++) {
        [unroll] for (int ix = 0; ix < 4; ix++) {
            if ((ix == 0 || ix == 3) && (iy == 0 || iy == 3)) continue;
            int2 tq = clamp(base + int2(ix - 1, iy - 1), int2(0, 0), last);
            float3 ty = RgbToYCoCg(color.Load(int3(tq, 0)).rgb);
            float tw = wx[ix] * wy[iy];
            lanczos_sum += ty * tw;
            lanczos_w += tw;
        }
    }
    float3 upsampled = clamp(lanczos_w != 0.0 ? lanczos_sum / lanczos_w : y_c, box_min, box_max);
    if (g_Taa.Mode == 2) return float4(YCoCgToRgb(upsampled), 0.0);

    float4 mv = g_Textures[g_Taa.DepthIndex].Load(int3(p, 0));
    float2 motion_uv = mv.xy;
    float disocclusion = UnpackDisocclusion(mv.z);
    bool obj_moving = (mv.w < -0.5);
    float2 history_uv = uv + motion_uv;

    if (g_Taa.Mode == 1) {
        float2 pixels = motion_uv / g_Taa.Size.xy;
        // Red = speed (20 px a frame saturates), green = has an object motion vector, blue = disocclusion.
        return float4(length(pixels) * 0.05, obj_moving ? 1.0 : 0.0, disocclusion, 1.0);
    }
    if (disocclusion >= 0.99 || any(history_uv < 0.0) || any(history_uv > 1.0))
        return float4(YCoCgToRgb(upsampled), 0.0);

    // Thin-feature confidence (1-pixel horizontal/vertical lines, fences, wires):
    float dv1 = y_c.x - l_n, dv2 = y_c.x - l_s;
    float dh1 = y_c.x - l_w, dh2 = y_c.x - l_e;
    float ridge = max(dv1 * dv2 > 0.0 ? min(abs(dv1), abs(dv2)) : 0.0,
                      dh1 * dh2 > 0.0 ? min(abs(dh1), abs(dh2)) : 0.0);
    float thin_lock = saturate(ridge * 8.0) * (1.0 - disocclusion);

    float4 history = History(history_uv);
    float3 h_ycocg = RgbToYCoCg(history.rgb);
    // Auto-reactive mask for animated transparencies, water ripples, particles, and moving shadows
    // whose whole 3x3 patch shifted in YCoCg beyond local contrast `sigma` despite static depth:
    float3 shift = max(abs(h_ycocg - mu) - sigma, 0.0);
    float reactive = saturate(dot(shift, float3(2.0, 3.0, 3.0)) / max(max(mu.x, h_ycocg.x), 0.15));
    float speed = length(motion_uv / g_Taa.Size.xy);
    float gamma = lerp(1.25, 0.85, saturate(speed * 0.1)) + 0.75 * thin_lock;
    gamma = lerp(gamma, 0.5, max(disocclusion, reactive));
    float3 clipped = ClipHistory(h_ycocg, mu, sigma, gamma);

    float3 pulled = abs(clipped - h_ycocg);
    float3 span = sigma * gamma + 1.0 / 64.0;
    float color_reject = saturate(1.5 * max(pulled.x, max(pulled.y, pulled.z)) /
                                  max(span.x, max(span.y, span.z)));
    float reject = max(disocclusion, max(color_reject, reactive) * (1.0 - 0.5 * thin_lock));
    float accumulated = history.a * (1.0 - reject);

    float2 off = (float2(p) + 0.5 - scene_pos) / (scene_size * g_Taa.Size.xy);  // output pixels
    float near = exp(-g_Taa.Kernel * dot(off, off));
    float weight = max(near, 0.25);
    float3 current = lerp(upsampled, y_c, near / weight * saturate(accumulated + near));
    float blend = max(weight / (accumulated + weight), reject);
    float3 out_ycocg = lerp(clipped, current, blend);
    return float4(YCoCgToRgb(out_ycocg), min(accumulated + weight, 1.0 / g_Taa.Blend - 1.0));
}

// 16x16 scene-tile velocity summary for PSFrameGen:
//   .xy = fastest velocity in the tile (for finding fast moving cars crossing neighbors)
//   .zw = foreground velocity (nearest reversed-Z texel in the tile)
float4 PSTaaTileMv(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    Texture2D<float4> mv_tex = g_Textures[g_Taa.DepthIndex];
    int2 scene_last = int2(g_Taa.Size.zw) - 1;
    int2 base = int2(position.xy) * 16;
    float2 fast_mv = 0.0, fg_mv = 0.0;
    float max_len2 = -1.0, max_z = -1.0;
    [unroll] for (int y = 1; y < 16; y += 2) {
        [unroll] for (int x = 1; x < 16; x += 2) {
            int2 q = clamp(base + int2(x, y), int2(0, 0), scene_last);
            float4 m = mv_tex.Load(int3(q, 0));
            float z = m.w < -0.5 ? (-m.w - 1.0) : m.w;
            if (z > max_z) {
                max_z = z;
                fg_mv = m.xy;
            }
            float l2 = dot(m.xy, m.xy);
            if (l2 > max_len2 && l2 < 0.25) {
                max_len2 = l2;
                fast_mv = m.xy;
            }
        }
    }
    return float4(fast_mv, fg_mv);
}

// 5-tap FidelityFX RCAS sharpening helper used by PSFrameGen so both the generated 3D frame and
// the reference current 3D frame (for HUD isolation) match TaaResolve's RCAS pass.
float3 Rcas5(float3 e, float3 b, float3 d, float3 f, float3 h, float sharp_k) {
    if (sharp_k <= 0.0) return saturate(e);
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobe3 = max(-hitMin, hitMax);
    float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * sharp_k;
    return saturate((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0));
}

// Depth (reversed z, larger = nearer) that the surface in mv texel `m` has at the generated instant
// `dir` frames before frame N: 1 / z is close to linear in time, so interpolate (or extrapolate) it.
float FgDepthAt(float4 m, float dir) {
    float z = m.w < -0.5 ? (-m.w - 1.0) : m.w;
    float ratio = max(UnpackDepthRatio(m.z), 0.05);
    return z / clamp(1.0 - dir + dir / ratio, 0.25, 4.0);
}

// Does the surface at q_uv = uv - dir * cand_mv in Frame N move through `uv` at the generated
// frame's time offset `dir` (+0.5 for N-0.5, -0.5 for N+0.5)? One mv load per candidate (this runs
// for up to 29 candidates a pixel: a second load each cost ~35 ms a frame on Switch 2), so the test
// is loose; PSFrameGen verifies the winner. Of the surfaces that land on `uv`, the nearest one at
// the generated instant wins.
void FgTestVelocity(float2 cand_mv, float dir, float2 uv, float2 scene_size, int2 scene_last,
                    Texture2D<float4> mv_tex, inout float best_z, inout float2 best_mv, inout bool found) {
    float2 q_uv = uv - dir * cand_mv;
    if (any(q_uv < 0.0) || any(q_uv > 1.0)) return;
    float4 mq = mv_tex.Load(int3(clamp(int2(q_uv * scene_size), int2(0, 0), scene_last), 0));
    float cand_px = length(cand_mv * scene_size);
    float err = abs(dir) * length((mq.xy - cand_mv) * scene_size);
    float zq = FgDepthAt(mq, dir);
    if (err <= max(1.5, 0.20 * cand_px) && zq > best_z) {
        best_z = zq;
        best_mv = mq.xy;
        found = true;
    }
}

float3 FgSampleWarped(float2 uv_cur, float2 uv_prev, float w_cur, uint mode) {
    SamplerState samp = g_Samplers[g_Taa.SamplerIndex];
    float3 c_cur = g_Textures[g_Taa.ColorIndex].SampleLevel(samp, uv_cur, 0).rgb;
    if (mode != 1 || w_cur >= 0.999) return saturate(c_cur);
    float3 c_prev = g_Textures[g_Taa.HistoryIndex].SampleLevel(samp, uv_prev, 0).rgb;
    return saturate(lerp(c_prev, c_cur, w_cur));
}

// Is what Frame N-1 shows at uv_prev the surface `m` (its Frame N mv texel) on its way to `uv`?
// Same kind (moving object or not), not nearer than that surface was, and, moved on with its own
// Frame N-1 motion, landing on `uv` half a frame later. The car's bumper in Frame N-1 stays put on
// the screen, so it fails, and copies of it no longer appear below the car.
bool FgPrevIsSurface(float2 uv, float2 uv_prev, float4 m, float2 scene_size, int2 scene_last) {
    Texture2D<float4> mv_prev = g_Textures[g_Taa.Objects >> 16];
    float4 pm = mv_prev.Load(int3(clamp(int2(uv_prev * scene_size), int2(0, 0), scene_last), 0));
    bool prev_obj = pm.w < -0.5, obj = m.w < -0.5;
    float z_prev = prev_obj ? (-pm.w - 1.0) : pm.w;
    float z_then = (obj ? (-m.w - 1.0) : m.w) * UnpackDepthRatio(m.z);
    float miss = length((uv_prev - 0.5 * pm.xy - uv) * scene_size);
    return prev_obj == obj && z_prev <= z_then * 1.1 + 1e-4 &&
           miss <= max(2.0, 0.25 * length(0.5 * pm.xy * scene_size));
}

// Frame generation pass (output w x h -> fg_tex[slot], RGBA8):
//   ColorIndex   = taa.tex[cur] (clean pre-HUD 3D scene of frame N)
//   HistoryIndex = taa.tex[prev] (clean pre-HUD 3D scene of frame N-1)
//   DepthIndex   = mv_tex[cur] (low 16) | tile_mv[slot] (high 16)
//   Depth.xy     = tile grid size (tile_w, tile_h)
//   Objects      = front_descriptor (frame N with HUD drawn over it, low 16) | mv_tex[prev] (high 16)
//   Blend        = RCAS sharpness multiplier (exp2(-sharp), or 0 when off)
//   Mode         = 1 (interpolated N-0.5) or 2 (extrapolated N+0.5)
float4 PSFrameGen(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 inv = g_Taa.Size.xy;
    int2 out_last = int2(round(1.0 / inv)) - 1;
    int2 p = clamp(int2(position.xy), int2(0, 0), out_last);
    float2 scene_size = g_Taa.Size.zw;
    int2 scene_last = int2(scene_size) - 1;
    int2 sp = clamp(int2(uv * scene_size), int2(0, 0), scene_last);

    Texture2D<float4> mv_tex = g_Textures[g_Taa.DepthIndex & 0xFFFFu];
    Texture2D<float4> tile_tex = g_Textures[g_Taa.DepthIndex >> 16];
    float z_fg = -1.0, z_bg = 2.0;
    float2 mv_here = 0.0, mv_fg = 0.0, mv_bg = 0.0;
    float4 m_here = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            int2 q = clamp(sp + int2(x, y), int2(0, 0), scene_last);
            float4 m = mv_tex.Load(int3(q, 0));
            float z = m.w < -0.5 ? (-m.w - 1.0) : m.w;
            if (x == 0 && y == 0) {
                m_here = m;
                mv_here = m.xy;
            }
            if (z > z_fg) {
                z_fg = z;
                mv_fg = m.xy;
            }
            if (z < z_bg) {
                z_bg = z;
                mv_bg = m.xy;
            }
        }
    }

    float dir = (g_Taa.Mode == 1) ? 0.5 : -0.5;
    float best_z = -1.0, best_dis = 0.0;
    float4 best_m = 0.0;
    float2 best_mv = 0.0;
    bool found = false;
    FgTestVelocity(mv_here, dir, uv, scene_size, scene_last, mv_tex, best_z, best_mv, found);
    if (!found || length((mv_fg - best_mv) * scene_size) > 1.0)
        FgTestVelocity(mv_fg, dir, uv, scene_size, scene_last, mv_tex, best_z, best_mv, found);
    if (!found || length((mv_bg - best_mv) * scene_size) > 1.0)
        FgTestVelocity(mv_bg, dir, uv, scene_size, scene_last, mv_tex, best_z, best_mv, found);

    int2 tile_last = int2(g_Taa.Depth.xy) - 1;
    int2 tp = clamp(sp / 16, int2(0, 0), tile_last);
    static const int2 kTileOffsets[13] = {
        int2( 0,  0), int2(-1,  0), int2( 1,  0), int2( 0, -1), int2( 0,  1),
        int2(-1, -1), int2( 1, -1), int2(-1,  1), int2( 1,  1),
        int2(-2,  0), int2( 2,  0), int2( 0, -2), int2( 0,  2)
    };
    [unroll] for (int t = 0; t < 13; t++) {
        int2 tq = clamp(tp + kTileOffsets[t], int2(0, 0), tile_last);
        float4 tv = tile_tex.Load(int3(tq, 0));
        if (dot(tv.xy, tv.xy) > 1e-8 && (!found || length((tv.xy - best_mv) * scene_size) > 1.0))
            FgTestVelocity(tv.xy, dir, uv, scene_size, scene_last, mv_tex, best_z, best_mv, found);
        if (length((tv.zw - tv.xy) * scene_size) > 1.0 &&
            dot(tv.zw, tv.zw) > 1e-8 && (!found || length((tv.zw - best_mv) * scene_size) > 1.0))
            FgTestVelocity(tv.zw, dir, uv, scene_size, scene_last, mv_tex, best_z, best_mv, found);
    }

    // Verify the winner: the texel its own vector leads to has to agree with it. Otherwise the loose
    // test reached across another surface (a fast road vector across a slow car) and warping with it
    // would drag that surface's pixels along.
    // Up to two fixed-point steps (q = uv - dir * mv(q)): where the motion changes along the way
    // (the road in perspective, faster toward the bottom of the screen) one step missed by a few
    // pixels and a real surface was taken for hidden background (a copy of the car's underside
    // from Frame N-1 appeared below the car).
    if (found) {
        float best_px = length(best_mv * scene_size);
        float tol = max(1.5, 0.08 * best_px);
        float2 v = best_mv;
        bool ok = false;
        [unroll] for (int it = 0; it < 2; it++) {
            float2 q1 = uv - dir * v;
            float4 m1 = mv_tex.Load(int3(clamp(int2(q1 * scene_size), int2(0, 0), scene_last), 0));
            if (!ok && abs(dir) * length((m1.xy - v) * scene_size) <= tol) {
                ok = true;
                best_mv = m1.xy;
                best_dis = UnpackDisocclusion(m1.z);
                best_m = m1;
            }
            v = m1.xy;
        }
        found = ok;
    }

    // Nothing in Frame N passes through `uv` at the generated instant: `uv` shows background that
    // an occluder hides in Frame N. The background is the farther of the surface at `uv` and the one
    // that surface's vector leads to (a car against the road under it, or against a static street).
    bool here_is_bg = true, other_in = false;
    float4 m_hidden = m_here;
    if (!found) {
        float2 q2 = uv - dir * mv_here;
        other_in = all(q2 >= 0.0) && all(q2 <= 1.0);
        if (other_in) {
            float4 m2 = mv_tex.Load(int3(clamp(int2(q2 * scene_size), int2(0, 0), scene_last), 0));
            if (FgDepthAt(m2, dir) < FgDepthAt(m_here, dir)) {
                here_is_bg = false;
                m_hidden = m2;
            }
        }
    }
    float2 mv_hidden = m_hidden.xy;

    float2 uv_cur, uv_prev;
    float w_cur = 1.0;
    uint branch = found ? 1u : 2u;  // --fastcars_native_frame_gen_debug: how this pixel was made
    SamplerState samp = g_Samplers[g_Taa.SamplerIndex];
    if (g_Taa.Mode == 1) {
        if (found) {
            uv_cur = uv - 0.5 * best_mv;
            uv_prev = uv + 0.5 * best_mv;
            w_cur = lerp(0.5, 1.0, best_dis);
            bool cur_in = all(uv_cur >= 0.0) && all(uv_cur <= 1.0);
            bool prev_in = all(uv_prev >= 0.0) && all(uv_prev <= 1.0);
            if (!prev_in) {
                w_cur = 1.0;
            } else if (!cur_in) {
                w_cur = 0.0;
            } else {
                float3 c0 = g_Textures[g_Taa.ColorIndex].SampleLevel(samp, uv_cur, 0).rgb;
                float3 p0 = g_Textures[g_Taa.HistoryIndex].SampleLevel(samp, uv_prev, 0).rgb;
                float l_cur = dot(c0, float3(0.25, 0.5, 0.25));
                float l_prev = dot(p0, float3(0.25, 0.5, 0.25));
                float l_diff = abs(l_cur - l_prev) / max(max(l_cur, l_prev), 0.12);
                w_cur = lerp(w_cur, 1.0, smoothstep(0.25, 0.60, l_diff));
            }
        } else {
            // The background is still visible in Frame N-1, half its own motion back, unless what
            // Frame N-1 has there is something else: a moving object where the background is not
            // one, or a surface nearer than the background was. Then Frame N at `uv` it is.
            uv_cur = uv;
            uv_prev = uv + 0.5 * mv_hidden;
            w_cur = (any(uv_prev < 0.0) || any(uv_prev > 1.0)) ? 1.0 : 0.0;
            if (w_cur != 0.0) branch = 4u;
        }
        // Any use of Frame N-1 (a blend, an off-screen Frame N sample, hidden background) needs
        // Frame N-1 to show that very surface there; otherwise Frame N, clamped to the screen.
        // (At the bottom edge the road's Frame N sample was off the screen and Frame N-1 alone,
        // the car's underside, was used: a copy of the bumper above the screen's lower edge.)
        if (w_cur < 0.999 && !FgPrevIsSurface(uv, uv_prev, found ? best_m : m_hidden, scene_size, scene_last)) {
            w_cur = 1.0;
            uv_cur = clamp(uv_cur, 0.0, 1.0);
            branch = 3u;
        }
    } else {
        // Extrapolated: the uncovered background was never drawn. Fill it from the background next
        // to the occluder (never from the occluder itself: that smears a copy of the car behind it).
        // Off the screen edge the clamped warp stays (a stretch reads better than a held strip).
        if (found)
            uv_cur = uv + 0.5 * best_mv;
        else if (!other_in)
            uv_cur = uv + 0.5 * mv_here;
        else
            uv_cur = here_is_bg ? uv : uv + 0.625 * mv_here;
        uv_prev = uv_cur;
        w_cur = 1.0;
    }

    float sharp_k = g_Taa.Blend;
    float3 w_e = FgSampleWarped(uv_cur, uv_prev, w_cur, g_Taa.Mode);
    float3 scene_warped = w_e;
    if (sharp_k > 0.0) {
        float3 w_b = FgSampleWarped(uv_cur + float2(0.0, -inv.y), uv_prev + float2(0.0, -inv.y), w_cur, g_Taa.Mode);
        float3 w_d = FgSampleWarped(uv_cur + float2(-inv.x, 0.0), uv_prev + float2(-inv.x, 0.0), w_cur, g_Taa.Mode);
        float3 w_f = FgSampleWarped(uv_cur + float2(inv.x, 0.0), uv_prev + float2(inv.x, 0.0), w_cur, g_Taa.Mode);
        float3 w_h = FgSampleWarped(uv_cur + float2(0.0, inv.y), uv_prev + float2(0.0, inv.y), w_cur, g_Taa.Mode);
        scene_warped = Rcas5(w_e, w_b, w_d, w_f, w_h, sharp_k);
    }

    Texture2D<float4> cur_tex = g_Textures[g_Taa.ColorIndex];
    float3 c_e = saturate(cur_tex.Load(int3(p, 0)).rgb);
    float3 scene_cur = c_e;
    if (sharp_k > 0.0) {
        float3 c_b = saturate(cur_tex.Load(int3(clamp(p + int2(0, -1), int2(0, 0), out_last), 0)).rgb);
        float3 c_d = saturate(cur_tex.Load(int3(clamp(p + int2(-1, 0), int2(0, 0), out_last), 0)).rgb);
        float3 c_f = saturate(cur_tex.Load(int3(clamp(p + int2(1, 0), int2(0, 0), out_last), 0)).rgb);
        float3 c_h = saturate(cur_tex.Load(int3(clamp(p + int2(0, 1), int2(0, 0), out_last), 0)).rgb);
        scene_cur = Rcas5(c_e, c_b, c_d, c_f, c_h, sharp_k);
    }

    // Extract HUD from the final resolved front buffer (which has HUD drawn over `scene_cur`):
    float3 front_col = g_Textures[g_Taa.Objects & 0xFFFFu].SampleLevel(samp, uv, 0).rgb;
    float3 hud_delta = front_col - scene_cur;
    float hud_diff = max(abs(hud_delta.r), max(abs(hud_delta.g), abs(hud_delta.b)));
    float hud_opaque = smoothstep(0.03, 0.15, hud_diff);
    float3 out_col = lerp(saturate(scene_warped + hud_delta), front_col, hud_opaque);
    // Kernel > 0.5 (--fastcars_native_frame_gen_debug): tint by branch. Green = a surface of Frame N
    // lands here (yellow while Frame N-1 is blended in), red = hidden background filled from Frame
    // N-1, blue = hidden background whose Frame N-1 fill was refused (Frame N at this pixel), cyan =
    // hidden background off the screen in Frame N-1.
    if (g_Taa.Kernel > 0.5) {
        float3 tint = branch == 1u ? (w_cur < 0.999 ? float3(1, 1, 0) : float3(0, 1, 0))
                    : branch == 2u ? float3(1, 0, 0)
                    : branch == 3u ? float3(0, 0.3, 1) : float3(0, 1, 1);
        out_col = lerp(out_col, tint, 0.45);
    }
    return float4(out_col, 1.0);
}
