// fastcars - native renderer: AMD FidelityFX Super Resolution 1 for the present (front buffer smaller
// than the swapchain, e.g. --fastcars_native_render_scale < 1). Two passes over the final, tonemapped
// image: EASU (edge-adaptive upscale) into an output-size target, then RCAS (sharpening) onto the
// swapchain. shaders/fsr1 is AMD's ffx_a.h / ffx_fsr1.h (MIT, see its LICENSE.txt).
//
// Own pipeline layout (backend.cpp FsrInit): the backend's descriptor sets (texture heap in
// sets 0-2, samplers in set 3) and 72 bytes of push constants: FsrEasuCon's four uint4 (RCAS uses
// Con0), then the texture and sampler indices.

struct FsrPush {
    uint4 Con0;
    uint4 Con1;
    uint4 Con2;
    uint4 Con3;
    uint TextureIndex;
    uint SamplerIndex;
};

[[vk::push_constant]] ConstantBuffer<FsrPush> g_Fsr;
Texture2D<float4> g_Textures[] : register(t0, space0);
SamplerState g_Samplers[] : register(s0, space3);

#define A_GPU 1
#define A_HLSL 1
#include "fsr1/ffx_a.h"

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return g_Textures[g_Fsr.TextureIndex].GatherRed(g_Samplers[g_Fsr.SamplerIndex], p); }
AF4 FsrEasuGF(AF2 p) { return g_Textures[g_Fsr.TextureIndex].GatherGreen(g_Samplers[g_Fsr.SamplerIndex], p); }
AF4 FsrEasuBF(AF2 p) { return g_Textures[g_Fsr.TextureIndex].GatherBlue(g_Samplers[g_Fsr.SamplerIndex], p); }

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return g_Textures[g_Fsr.TextureIndex].Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "fsr1/ffx_fsr1.h"

float4 PSEasu(float4 position : SV_Position) : SV_Target {
    AF3 c;
    FsrEasuF(c, AU2(position.xy), g_Fsr.Con0, g_Fsr.Con1, g_Fsr.Con2, g_Fsr.Con3);
    return float4(c, 1.0);
}

float4 PSRcas(float4 position : SV_Position) : SV_Target {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(position.xy), g_Fsr.Con0);
    return float4(c, 1.0);
}
