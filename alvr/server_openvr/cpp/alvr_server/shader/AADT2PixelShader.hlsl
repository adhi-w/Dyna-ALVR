// AADT2 foveation divides image into foveal and peripheral regions.
// This shader compresses the peripheral regions of the image
// Written by [AW] and [TK]

#include "FoveatedRendering.hlsli"



Texture2D<float4> compositionTexture;

SamplerState trilinearSampler {
	Filter = MIN_MAG_MIP_LINEAR;
	//AddressU = Wrap;
	//AddressV = Wrap;
};

float MapUV(float coord, float center_start, float center_end, float first_peripheral_size, float next_peripheral_size, float cSize, float pSize) {
    float rate_peripheral = ((1.0 - (cSize)) / 2.0) / pSize;
    float rate_center = cSize / (center_end - center_start);
    
    if (coord < center_start) {
        coord = coord * rate_peripheral; // Maps to first peripheral
    } else if (coord < center_end) {
        coord = first_peripheral_size + ((coord - center_start) * rate_center); // Maps to center
        //coord = 0.0;
    } else {
        coord = first_peripheral_size + cSize + ((coord - center_end) * rate_peripheral); // Maps to last peripheral
    }
    return coord;
}

float4 main(float2 uv : TEXCOORD0) : SV_Target{

    float2 CenterSizeOwn = centerSize;
    float2 PeripheralNewScreenSpace = (1.0 / (edgeRatio + 1.0)) / 2.0;
    float2 CenterSizeNewScreenSpace = edgeRatio / (edgeRatio + 1.0);
    float2 CenterShiftOwn = centerShift_Left;

	// Determine if this is the right eye based on horizontal position.
    bool isRightEye = uv.x > 0.5;

    float2 eyeUV = TextureToEyeUV(uv, isRightEye); // Transform screen UV to texture UV

    // Re-scale to -1..1
    if (isRightEye) {
        CenterShiftOwn = centerShift_Right;
        CenterShiftOwn.x = 1.0 - CenterShiftOwn.x;
    }
    CenterShiftOwn.y = 1 - CenterShiftOwn.y;
    CenterShiftOwn.x = 2.0 * CenterShiftOwn.x - 1.0;
    CenterShiftOwn.y = 2.0 * CenterShiftOwn.y - 1.0;

    // Peripheral size without center shift (texture-space)
    float2 peripheral_size = (1.0 - CenterSizeOwn) / 2.0;
    
    // Center max shift (texture-space)
    float2 center_max_shift_x = 2.0 * float2(0.5 - peripheral_size.x, 0.5 + peripheral_size.x) - 1.0;
    float2 center_max_shift_y = 2.0 * float2(0.5 - peripheral_size.y, 0.5 + peripheral_size.y) - 1.0;
    float2 centerShift_clamp;
    centerShift_clamp.x = clamp(CenterShiftOwn.x, center_max_shift_x.x, center_max_shift_x.y);
    centerShift_clamp.y = clamp(CenterShiftOwn.y, center_max_shift_y.x, center_max_shift_y.y);
    
    // Foveation parameters (screen-space)
    float2 center_start;
    center_start.x = (PeripheralNewScreenSpace.x + ((centerShift_clamp.x / center_max_shift_x.y) * PeripheralNewScreenSpace.x));
    center_start.y = (PeripheralNewScreenSpace.y + ((centerShift_clamp.y / center_max_shift_y.y) * PeripheralNewScreenSpace.y));
    float2 center_end = center_start + CenterSizeNewScreenSpace;
    
    // Fixed peripheral size (texture-space)
    float left_peripheral_size = (center_start.x / PeripheralNewScreenSpace.x) * peripheral_size.x;
    float right_peripheral_size = 1.0 - CenterSizeOwn.x - left_peripheral_size;
    
    float bottom_peripheral_size = (center_start.y / PeripheralNewScreenSpace.y) * peripheral_size.y;
    float top_peripheral_size = 1.0 - CenterSizeOwn.y - bottom_peripheral_size;
    
    // Screen space uv
    float2 compressedUV;
    compressedUV.x = MapUV(eyeUV.x, center_start.x, center_end.x, left_peripheral_size, right_peripheral_size, CenterSizeOwn.x, PeripheralNewScreenSpace.x);
    compressedUV.y = MapUV(eyeUV.y, center_start.y, center_end.y, bottom_peripheral_size, top_peripheral_size, CenterSizeOwn.y, PeripheralNewScreenSpace.y);
    
	return compositionTexture.Sample(
		trilinearSampler, EyeToTextureUV(compressedUV, isRightEye));
}

