// AADT3 foveation divides image into a foveal and two peripheral regions.
// This shader compresses the peripheral regions of the image
// Written by [AW] and [TK]

#include "FoveatedRendering.hlsli"

Texture2D<float4> compositionTexture;
SamplerState trilinearSampler { Filter = MIN_MAG_MIP_LINEAR; };

float MapUV(float coord, float first_peripheral_start, float center_start, float center_end, float very_first_peripheral_size, float very_peripheral_size, float very_next_peripheral_size, float cSize, float pSize) {
    float rate_peripheral = ((1.0 - (cSize)) / 4.0) / ((2.0 / 3.0) * pSize);
    float rate_very_peripheral = ((1.0 - (cSize)) / 4.0) / ((1.0 / 3.0) * pSize);
    float rate_center = cSize / (center_end - center_start);
    
    if (coord < first_peripheral_start) {
        coord = coord * rate_very_peripheral; // Maps to leftmost 1/8 of the texture
    } else if (coord < center_start) {
        coord = very_first_peripheral_size + ((coord - first_peripheral_start) * rate_peripheral); // Maps to left 1/8 of the texture
    } else if (coord < center_end) {
        coord = very_first_peripheral_size + very_peripheral_size + (coord - center_start) * rate_center; // Maps to the center 1/2 of the texture
    } else if (coord < center_end + ((1.0 - center_end)/3.0)*2.0) {
        coord = very_first_peripheral_size + very_peripheral_size + cSize + (coord - center_end) * rate_peripheral;
    } else {
        coord = very_first_peripheral_size + very_peripheral_size + cSize + very_next_peripheral_size + (coord - (1.0 - ((1.0 - center_end)/3.0))) * rate_very_peripheral; // Maps to the rightmost 1/4 of the texture
    }
    return coord;
}

float4 main(float2 uv : TEXCOORD0) : SV_Target{
    float2 CenterSizeOwn = centerSize;
    float2 PeripheralNewScreenSpace = (((1.0 - CenterSizeOwn) / (2.0 * edgeRatio)) + ((1.0 - CenterSizeOwn) / (2.0 * (edgeRatio / 2.0)))) / 2.0;
    float2 CenterSizeNewScreenSpace = 1.0 - (PeripheralNewScreenSpace * 2.0);
    float2 CenterShiftOwn = centerShift_Left;
	// Determine if this is the right eye based on horizontal position.
    bool isRightEye = uv.x > 0.5;
    float2 eyeUV = TextureToEyeUV(uv, isRightEye); // Transform screen UV to texture UV

    // Re-scale to -1..1
    if (isRightEye) {
        CenterShiftOwn = centerShift_Right;
        CenterShiftOwn.x = 1 - CenterShiftOwn.x;
    }
    CenterShiftOwn.y = 1 - CenterShiftOwn.y;
    CenterShiftOwn.x = 2.0 * CenterShiftOwn.x - 1.0;
    CenterShiftOwn.y = 2.0 * CenterShiftOwn.y - 1.0;
    
    // Peripheral size without center shift (texture-space)
    float2 peripheral_size = (1.0 - CenterSizeOwn) / 4.0;
    
    // Center max shift (texture-space)
    float2 center_max_shift_x = 2.0 * float2(0.5 - (peripheral_size.x * 2.0), 0.5 + (peripheral_size.x  * 2.0)) - 1.0;
    float2 center_max_shift_y = 2.0 * float2(0.5 - (peripheral_size.y * 2.0), 0.5 + (peripheral_size.y  * 2.0)) - 1.0;
    float2 centerShift_clamp;
    centerShift_clamp.x = clamp(CenterShiftOwn.x, center_max_shift_x.x, center_max_shift_x.y);
    centerShift_clamp.y = clamp(CenterShiftOwn.y, center_max_shift_y.x, center_max_shift_y.y);
    
    // Foveation parameters (screen-space)
    float2 center_start;
    center_start.x = (PeripheralNewScreenSpace.x + ((centerShift_clamp.x / center_max_shift_x.y) * PeripheralNewScreenSpace.x));
    center_start.y = (PeripheralNewScreenSpace.y + ((centerShift_clamp.y / center_max_shift_y.y) * PeripheralNewScreenSpace.y));
    float2 center_end = center_start + CenterSizeNewScreenSpace;
    float2 first_peripheral_start = center_start / 3.0;
    
    // Fixed peripheral size (texture-space)
    float left_peripheral_size = (center_start.x / PeripheralNewScreenSpace.x) * peripheral_size.x;
    float very_left_peripheral_size = (center_start.x / PeripheralNewScreenSpace.x) * peripheral_size.x; 
    float right_peripheral_size = ((1.0 - CenterSizeOwn.x) - left_peripheral_size - very_left_peripheral_size) / 2.0;
    
    float bottom_peripheral_size = (center_start.y / PeripheralNewScreenSpace.y) * peripheral_size.y;
    float very_bottom_peripheral_size = (center_start.y / PeripheralNewScreenSpace.y) * peripheral_size.y;
    float top_peripheral_size = ((1.0 - CenterSizeOwn.y) - bottom_peripheral_size - very_bottom_peripheral_size) / 2.0;
    
    // Screen space uv
    float2 compressedUV;
    compressedUV.x = MapUV(eyeUV.x, first_peripheral_start.x, center_start.x, center_end.x, very_left_peripheral_size, left_peripheral_size, right_peripheral_size, CenterSizeOwn.x, PeripheralNewScreenSpace.x);
    compressedUV.y = MapUV(eyeUV.y,  first_peripheral_start.y, center_start.y, center_end.y, very_bottom_peripheral_size, bottom_peripheral_size, top_peripheral_size, CenterSizeOwn.y, PeripheralNewScreenSpace.y);
    
	return compositionTexture.Sample(
		trilinearSampler, EyeToTextureUV(compressedUV, isRightEye));
}