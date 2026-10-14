// FRW implements non-linear tangent-based foveated rendering.
// This shader has no distinct compressed peripheral regions.
// Written by [AW] and [TK]

#include "FoveatedRendering.hlsli"

Texture2D<float4> compositionTexture;
SamplerState trilinearSampler { Filter = MIN_MAG_MIP_LINEAR; };

float ReScale(float coord, float a, float b, float c, float d) {
    return ((coord - a) / (b - a)) * (d - c) + c;
}

// Optimized mapping function that takes a precomputed angle factor.
float MapUVOptimized(float coord, float magnitude, float centerShift, float angleFactor) {
    // Compute c and d in one go.
    float c = (centerShift <= 0.5) ? (0.5 - centerShift) : 0.0;
    float d = (centerShift <= 0.5) ? 1.0 : (1.5 - centerShift);

    // Rescale the coordinate from [0, 1] to [c, d].
    coord = ReScale(coord, 0.0, 1.0, c, d);

    // Precompute the warped endpoints.
    float mini_x = tan(angleFactor * (c - 0.5)) / magnitude + 0.5;
    float maxi_x = tan(angleFactor * (d - 0.5)) / magnitude + 0.5;

    // Apply the warp to the coordinate.
    coord = tan(angleFactor * (coord - 0.5)) / magnitude + 0.5;

    // Rescale the warped coordinate back to [0, 1].
    coord = ReScale(coord, mini_x, maxi_x, 0.0, 1.0);

    return coord;
}

float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float2 CenterSizeOwn = centerSize; // Used as magnitude.
    float2 CenterShiftOwn = centerShift_Left;

    // Determine if this is the right eye based on horizontal position.
    bool isRightEye = uv.x > 0.5;
    if (isRightEye) {
        CenterShiftOwn = centerShift_Right;
        CenterShiftOwn.x = 1.0 - CenterShiftOwn.x;
    }
    CenterShiftOwn.y = 1.0 - CenterShiftOwn.y;

    // Transform screen UV to texture UV.
    float2 eyeUV = TextureToEyeUV(uv, isRightEye);

    // Precompute the common angle factor.
    float mag = CenterSizeOwn[0];
    float angleFactor = 2.0 * atan(mag * 0.5); // equivalent to 2.0 * atan(magnitude / 2.0)

    // Warp each UV coordinate using the optimized function.
    float2 compressedUV;
    compressedUV.x = MapUVOptimized(eyeUV.x, mag, CenterShiftOwn.x, angleFactor);
    compressedUV.y = MapUVOptimized(eyeUV.y, mag, CenterShiftOwn.y, angleFactor);

    return compositionTexture.Sample(trilinearSampler, EyeToTextureUV(compressedUV, isRightEye));
}
