const DIV12: f32 = 1.0 / 12.92;
const DIV1: f32 = 1.0 / 1.055;
const THRESHOLD: f32 = 0.04045;
const GAMMA: vec3f = vec3f(2.4);

override ENABLE_SRGB_CORRECTION: bool;
override ENCODING_GAMMA: f32;

// Debug gaze marker: when enabled, the shader will draw a small circle at the
override ENABLE_GAZE_DEBUG: bool = false;
override GAZE_MARKER_RADIUS: f32 = 0.002;
override GAZE_MARKER_FEATHER: f32 = 0.001;
override GAZE_MARKER_R: f32 = 1.0;
override GAZE_MARKER_G: f32 = 0.0;
override GAZE_MARKER_B: f32 = 0.0;

// Debug center-shift marker (FFR): draws a circle at the server-provided per-frame center shift.
override ENABLE_CENTER_SHIFT_DEBUG: bool = false;
override CENTER_SHIFT_MARKER_RADIUS: f32 = 0.002;
override CENTER_SHIFT_MARKER_FEATHER: f32 = 0.001;
// Blue marker
override CENTER_SHIFT_MARKER_R: f32 = 0.0;
override CENTER_SHIFT_MARKER_G: f32 = 0.0;
override CENTER_SHIFT_MARKER_B: f32 = 1.0;

// When true, draws a yellow box representing the foveal region defined by the center size parameters.
override ENABLE_FOVEAL_BOX_DEBUG: bool = false;

override ENABLE_UPSCALING: bool = false;
override UPSCALE_USE_EDGE_DIRECTION: bool = true;
override UPSCALE_EDGE_THRESHOLD: f32 = 4.0/255.0;
override UPSCALE_EDGE_SHARPNESS: f32 = 2.0;

override ENABLE_FFE: bool = false;
// 0: Vanilla (compressAxisAlignedPipeline), 1: D-AADT2 (AADT2Pipeline)
override FOVEATION_METHOD: u32 = 1u;

override VIEW_WIDTH_RATIO: f32 = 0.0;
override VIEW_HEIGHT_RATIO: f32 = 0.0;
override EDGE_X_RATIO: f32 = 0.0;
override EDGE_Y_RATIO: f32 = 0.0;

override C1_X: f32 = 0.0;
override C1_Y: f32 = 0.0;
override C2_X: f32 = 0.0;
override C2_Y: f32 = 0.0;
override LO_BOUND_X: f32 = 0.0;
override LO_BOUND_Y: f32 = 0.0;
override HI_BOUND_X: f32 = 0.0;
override HI_BOUND_Y: f32 = 0.0;

override A_LEFT_X: f32 = 0.0;
override A_LEFT_Y: f32 = 0.0;
override B_LEFT_X: f32 = 0.0;
override B_LEFT_Y: f32 = 0.0;

override A_RIGHT_X: f32 = 0.0;
override A_RIGHT_Y: f32 = 0.0;
override B_RIGHT_X: f32 = 0.0;
override B_RIGHT_Y: f32 = 0.0;
override C_RIGHT_X: f32 = 0.0;
override C_RIGHT_Y: f32 = 0.0;
override CENTER_SIZE_X: f32 = 0.0;
override CENTER_SIZE_Y: f32 = 0.0;

struct PushConstant {
    reprojection_transform: mat4x4f,
    view_idx: u32,
    passthrough_mode: u32, // 0: Blend, 1: RGB chroma key, 2: HSV chroma key
    blend_alpha: f32,
    _align: u32,
    ck_channel0: vec4f,
    ck_channel1: vec4f,
    ck_channel2: vec4f,
}
var<push_constant> pc: PushConstant;

struct GazePos {
    left: vec2f,
    right: vec2f,
}

struct DebugUniform {
    gaze_pos: GazePos,
    center_shift_pos: GazePos,
    // Shared center size for both eyes (normalized [0,1]).
    // Remaining vec2 is padding to keep the uniform buffer size stable.
    center_size: vec2f,
    center_size_pad: vec2f,
}

// Gaze comes from a GPU uniform buffer that the CPU fills every frame
@group(0) @binding(2) var<uniform> dbg: DebugUniform;

@group(0) @binding(0) var stream_texture: texture_2d<f32>;
@group(0) @binding(1) var stream_sampler: sampler;

struct VertexOutput {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vertex_main(@builtin(vertex_index) vertex_index: u32) -> VertexOutput {
    var result: VertexOutput;

    result.uv = vec2f(f32(vertex_index & 1), f32(vertex_index >> 1));
    result.position = pc.reprojection_transform * vec4f(result.uv.x - 0.5, 0.5 - result.uv.y, 0.0, 1.0);

    return result;
}

//============================================================================================================
// ------------------ D-AADT2 helper functions -------------------------------------------------
fn MapUV_inverse_AADT2(coord: f32, center_start: f32, center_end: f32, first_peripheral_size: f32, next_peripheral_size: f32, cSize: f32, pSize: f32) -> f32 {
    let rate_peripheral: f32 = ((1.0 - cSize) / 2.0) / pSize;
    let rate_center: f32 = cSize / (center_end - center_start);
    var out: f32 = coord;
    if (coord < first_peripheral_size) {
        out = coord * (1.0 / rate_peripheral);
    } else if (coord < (first_peripheral_size + cSize)) {
        out = center_start + (coord - first_peripheral_size) * (1.0 / rate_center);
    } else {
        out = center_end + (coord - first_peripheral_size - cSize) * (1.0 / rate_peripheral);
    }
    return out;
}
// ------------------ D-AADT3 helper functions -------------------------------------------------
fn MapUV_inverse_AADT3(coord: f32, first_peripheral_start: f32, center_start: f32, center_end: f32, very_first_peripheral_size: f32, very_peripheral_size: f32, very_next_peripheral_size: f32, cSize: f32, pSize: f32) -> f32 {
    let rate_peripheral: f32 = ((1.0 - (cSize)) / 4.0) / ((2.0 / 3.0) * pSize);
    let rate_very_peripheral: f32 = ((1.0 - (cSize)) / 4.0) / ((1.0 / 3.0) * pSize);
    let rate_center: f32 = cSize / (center_end - center_start);
    var out: f32 = coord;
    if (coord < very_first_peripheral_size) {
        out = coord * (1.0 / rate_very_peripheral); // Maps to leftmost 1/8 of the texture
    } else if (coord < (very_first_peripheral_size + very_peripheral_size)) {
        out = first_peripheral_start + ((coord - very_first_peripheral_size) * (1.0 / rate_peripheral)); // Maps to left 1/8 of the texture
    } else if (coord < (very_first_peripheral_size + very_peripheral_size + cSize)) {
        out = center_start + (coord - very_first_peripheral_size - very_peripheral_size) * (1.0 / rate_center); // Maps to the center 1/2 of the texture
    } else if (coord < (very_first_peripheral_size + very_peripheral_size + cSize + very_next_peripheral_size)) {
        out = center_end + (coord - very_first_peripheral_size - very_peripheral_size - cSize) * (1.0 / rate_peripheral);
    } else {
       out = center_end + (((1.0 - center_end) / 3.0) * 2.0) + (coord - very_first_peripheral_size - very_peripheral_size - cSize - very_next_peripheral_size) * (1.0 / rate_very_peripheral); // Maps to the rightmost 1/4 of the texture
    }
    return out;
}
// ------------------ D-FRW helper functions -------------------------------------------------
fn ReScale(value: f32, oldMin: f32, oldMax: f32, newMin: f32, newMax: f32) -> f32 {
    return ((value - oldMin) / (oldMax - oldMin)) * (newMax - newMin) + newMin;
}

fn MapUV_inverse_FRW(coord_in: f32, magnitude: f32, centerShift: f32, angleFactor: f32) -> f32 {
    var coord: f32 = coord_in;

    // Compute c and d in one go.
    var c: f32 = 0.0;
    var d: f32 = 1.0;
    if (centerShift <= 0.5) {
        c = 0.5 - centerShift;
        d = 1.0;
    } else {
        c = 0.0;
        d = 1.5 - centerShift;
    }
    
    // Precompute the warped endpoints.
    let mini_x_i = tan(angleFactor * (c - 0.5)) / magnitude + 0.5;
    let maxi_x_i = tan(angleFactor * (d - 0.5)) / magnitude + 0.5;
    
    // First re-scale from [0,1] to [mini_x_i, maxi_x_i].
    coord = ReScale(coord, 0.0, 1.0, mini_x_i, maxi_x_i);
    
    // Apply the inverse warp.
    coord = atan(magnitude * (coord - 0.5)) / angleFactor + 0.5;
    
    // Re-scale back from [c, d] to [0,1].
    coord = ReScale(coord, c, d, 0.0, 1.0);
    
    return coord;
}
//-----------------------------------------------------------------------------------------------
/* fn TextureToEyeUV(textureUV: vec2f, isRightEye: f32) -> vec2f {
    return vec2f((textureUV.x + isRightEye * (1.0 - 2.0 * textureUV.x)) * 2.0, textureUV.y);
}

fn EyeToTextureUV(eyeUV: vec2f, isRightEye: f32) -> vec2f {
    return vec2f(eyeUV.x * 0.5 + isRightEye * (1.0 - eyeUV.x), eyeUV.y);
} */
//============================================================================================================


@fragment
fn fragment_main(@location(0) uv: vec2f) -> @location(0) vec4f {
    var corrected_uv = uv;
    // tell upscaler to target a lower resolution for the edges
    var upscale_source_resolution = 1.0;

    // -- Vanilla Shaders ---
    if (ENABLE_FFE && FOVEATION_METHOD == 0u) {
        let view_size_ratio = vec2f(VIEW_WIDTH_RATIO, VIEW_HEIGHT_RATIO);
        let edge_ratio = vec2f(EDGE_X_RATIO, EDGE_Y_RATIO);

        let c1 = vec2f(C1_X, C1_Y);
        let c2 = vec2f(C2_X, C2_Y);
        let lo_bound = vec2f(LO_BOUND_X, LO_BOUND_Y);
        let hi_bound = vec2f(HI_BOUND_X, HI_BOUND_Y);

        let a_left = vec2f(A_LEFT_X, A_LEFT_Y);
        let b_left = vec2f(B_LEFT_X, B_LEFT_Y);

        let a_right = vec2f(A_RIGHT_X, A_RIGHT_Y);
        let b_right = vec2f(B_RIGHT_X, B_RIGHT_Y);
        let c_right = vec2f(C_RIGHT_X, C_RIGHT_Y);

        if pc.view_idx == 1 {
            corrected_uv.x = 1.0 - corrected_uv.x;
        }

        let center = (corrected_uv - c1) * edge_ratio / c2;
        let left_edge = (-b_left + sqrt(b_left * b_left + 4.0 * a_left * corrected_uv)) / (2.0 * a_left);
        let right_edge = (-b_right + sqrt(b_right * b_right - 4.0 * (c_right - a_right * corrected_uv))) / (2.0 * a_right);

        if corrected_uv.x < lo_bound.x {
            corrected_uv.x = left_edge.x;
            upscale_source_resolution = upscale_source_resolution * edge_ratio.x;
        } else if corrected_uv.x > hi_bound.x {
            corrected_uv.x = right_edge.x;
            upscale_source_resolution = upscale_source_resolution * edge_ratio.x;
        } else {
            corrected_uv.x = center.x;
        }

        if corrected_uv.y < lo_bound.y {
            corrected_uv.y = left_edge.y;
            upscale_source_resolution = upscale_source_resolution * edge_ratio.y;
        } else if corrected_uv.y > hi_bound.y {
            corrected_uv.y = right_edge.y;
            upscale_source_resolution = upscale_source_resolution * edge_ratio.y;
        } else {
            corrected_uv.y = center.y;
        }

        corrected_uv = corrected_uv * view_size_ratio;

        if pc.view_idx == 1 {
            corrected_uv.x = 1.0 - corrected_uv.x;
        }
    }
    else if (ENABLE_FFE && FOVEATION_METHOD == 1u) {
            // -- D-AADT2 Shaders ---
            // let EdgeRatio = vec2f(2.0, 2.0);
            // let CenterSize = vec2f(0.4, 0.4);   
            // let CenterShift = vec2f(0.6, 0.5);

            let EdgeRatio = vec2f(EDGE_X_RATIO, EDGE_Y_RATIO);
            // Prefer per-frame center size when provided, otherwise fall back to negotiated constants.
            var CenterSize = dbg.center_size;
            if (CenterSize.x <= 0.0 && CenterSize.y <= 0.0) {
                CenterSize = vec2f(CENTER_SIZE_X, CENTER_SIZE_Y);
            }

            var CenterSizeOwn = CenterSize;
            var PeripheralNewScreenSpace = (vec2f(1.0, 1.0) / (EdgeRatio + vec2f(1.0, 1.0))) / 2.0;
            var CenterSizeNewScreenSpace = EdgeRatio / (EdgeRatio + vec2f(1.0, 1.0));
            var CenterShiftOwn = dbg.center_shift_pos.left;

            var isRightEye: f32 = f32(pc.view_idx);

            // Re-scale to -1..1 for center shift handling
            if (isRightEye == 1.0) {     
                CenterShiftOwn = dbg.center_shift_pos.right;          
                //CenterShiftOwn.x = 1.0 - CenterShiftOwn.x; // this is unnecessary because right eye can use its independent center shift in multiview frames.
            }
            CenterShiftOwn.y = 1.0 - CenterShiftOwn.y;
            CenterShiftOwn.x = 2.0 * CenterShiftOwn.x - 1.0;
            CenterShiftOwn.y = 2.0 * CenterShiftOwn.y - 1.0;
            
            var alignedUV = corrected_uv;

            // Peripheral size without center shift (texture-space)
            var peripheral_size = (vec2f(1.0, 1.0) - CenterSizeOwn) / 2.0;

            // Center max shift (texture-space)
            var center_max_shift_x = 2.0 * vec2f(0.5 - peripheral_size.x, 0.5 + peripheral_size.x) - vec2f(1.0, 1.0);
            var center_max_shift_y = 2.0 * vec2f(0.5 - peripheral_size.y, 0.5 + peripheral_size.y) - vec2f(1.0, 1.0);
            var centerShift_clamp: vec2f;
            centerShift_clamp.x = clamp(CenterShiftOwn.x, center_max_shift_x.x, center_max_shift_x.y);
            centerShift_clamp.y = clamp(CenterShiftOwn.y, center_max_shift_y.x, center_max_shift_y.y);

            // Foveation parameters (screen-space)
            var center_start: vec2f;
            center_start.x = (PeripheralNewScreenSpace.x + ((centerShift_clamp.x / center_max_shift_x.y) * PeripheralNewScreenSpace.x));
            center_start.y = (PeripheralNewScreenSpace.y + ((centerShift_clamp.y / center_max_shift_y.y) * PeripheralNewScreenSpace.y));
            var center_end = center_start + CenterSizeNewScreenSpace;

            // Fixed peripheral size (texture-space)
            var left_peripheral_size = (center_start.x / PeripheralNewScreenSpace.x) * peripheral_size.x;
            var right_peripheral_size = 1.0 - CenterSizeOwn.x - left_peripheral_size;

            var bottom_peripheral_size = (center_start.y / PeripheralNewScreenSpace.y) * peripheral_size.y;
            var top_peripheral_size = 1.0 - CenterSizeOwn.y - bottom_peripheral_size;

            // Screen space uv -> texture-space mapping (inverse)
            var uncompressedUV: vec2f;
            uncompressedUV.x = MapUV_inverse_AADT2(alignedUV.x, center_start.x, center_end.x, left_peripheral_size, right_peripheral_size, CenterSizeOwn.x, PeripheralNewScreenSpace.x);
            uncompressedUV.y = MapUV_inverse_AADT2(alignedUV.y, center_start.y, center_end.y, bottom_peripheral_size, top_peripheral_size, CenterSizeOwn.y, PeripheralNewScreenSpace.y);

            // obtain final texture UV
            corrected_uv = uncompressedUV;             
    }
    else if (ENABLE_FFE && FOVEATION_METHOD == 2u) {
            // -- D-AADT3 Shaders ---
            let EdgeRatio = vec2f(EDGE_X_RATIO, EDGE_Y_RATIO);
            // Prefer per-frame center size when provided, otherwise fall back to negotiated constants.
            var CenterSize = dbg.center_size;
            if (CenterSize.x <= 0.0 && CenterSize.y <= 0.0) {
                CenterSize = vec2f(CENTER_SIZE_X, CENTER_SIZE_Y);
            }

            var CenterSizeOwn = CenterSize;
            var PeripheralNewScreenSpace = (((1.0 - CenterSizeOwn) / (2.0 * EdgeRatio)) + ((1.0 - CenterSizeOwn) / (2.0 * (EdgeRatio / 2.0)))) / 2.0;
            var CenterSizeNewScreenSpace = 1.0 - (PeripheralNewScreenSpace * 2.0);
            var CenterShiftOwn = dbg.center_shift_pos.left;

            var isRightEye: f32 = f32(pc.view_idx);
            // Re-scale to -1..1
            if (isRightEye == 1.0) {
                CenterShiftOwn = dbg.center_shift_pos.right;
                // CenterShiftOwn.x = 1.0 - CenterShiftOwn.x; // this is unnecessary because right eye can use its independent center shift in multiview frames.
            }
            CenterShiftOwn.y = 1.0 - CenterShiftOwn.y;
            CenterShiftOwn.x = 2.0 * CenterShiftOwn.x - 1.0;
            CenterShiftOwn.y = 2.0 * CenterShiftOwn.y - 1.0;

            var alignedUV = corrected_uv;

            // Peripheral size without center shift (texture-space)
            var peripheral_size = (1.0 - CenterSizeOwn) / 4.0;
            
            // Center max shift (texture-space)
            var center_max_shift_x = 2.0 * vec2(0.5 - (peripheral_size.x * 2.0), 0.5 + (peripheral_size.x  * 2.0)) - 1.0;
            var center_max_shift_y = 2.0 * vec2(0.5 - (peripheral_size.y * 2.0), 0.5 + (peripheral_size.y  * 2.0)) - 1.0;
            var centerShift_clamp: vec2f;
            centerShift_clamp.x = clamp(CenterShiftOwn.x, center_max_shift_x.x, center_max_shift_x.y);
            centerShift_clamp.y = clamp(CenterShiftOwn.y, center_max_shift_y.x, center_max_shift_y.y);
            
            // Foveation parameters (screen-space)
            var center_start: vec2f;
            center_start.x = (PeripheralNewScreenSpace.x + ((centerShift_clamp.x / center_max_shift_x.y) * PeripheralNewScreenSpace.x));
            center_start.y = (PeripheralNewScreenSpace.y + ((centerShift_clamp.y / center_max_shift_y.y) * PeripheralNewScreenSpace.y));
            var center_end = center_start + CenterSizeNewScreenSpace;
            var first_peripheral_start = center_start / 3.0;
            
            // Fixed peripheral size (texture-space)
            var left_peripheral_size = (center_start.x / PeripheralNewScreenSpace.x) * peripheral_size.x;
            var very_left_peripheral_size = (center_start.x / PeripheralNewScreenSpace.x) * peripheral_size.x; 
            var right_peripheral_size = ((1.0 - CenterSizeOwn.x) - left_peripheral_size - very_left_peripheral_size) / 2.0;
            
            var bottom_peripheral_size = (center_start.y / PeripheralNewScreenSpace.y) * peripheral_size.y;
            var very_bottom_peripheral_size = (center_start.y / PeripheralNewScreenSpace.y) * peripheral_size.y;
            var top_peripheral_size = ((1.0 - CenterSizeOwn.y) - bottom_peripheral_size - very_bottom_peripheral_size) / 2.0;
            
            // Screen space uv
            var uncompressedUV: vec2f;
            uncompressedUV.x = MapUV_inverse_AADT3(alignedUV.x, first_peripheral_start.x, center_start.x, center_end.x, very_left_peripheral_size, left_peripheral_size, right_peripheral_size, CenterSizeOwn.x, PeripheralNewScreenSpace.x);
            uncompressedUV.y = MapUV_inverse_AADT3(alignedUV.y, first_peripheral_start.y, center_start.y, center_end.y, very_bottom_peripheral_size, bottom_peripheral_size, top_peripheral_size, CenterSizeOwn.y, PeripheralNewScreenSpace.y);

            // obtain final texture UV
            corrected_uv = uncompressedUV;  
    }
    else if (ENABLE_FFE && FOVEATION_METHOD == 3u) {
            // -- D-FRW Shaders ---
            let magnitude: f32 = max(CENTER_SIZE_X, 0.01);
            var CenterShiftOwn = dbg.center_shift_pos.left;
            
            var isRightEye: f32 = f32(pc.view_idx);
            // If this is the right eye, adjust the center shift.
            if (isRightEye == 1.0) {
                CenterShiftOwn = dbg.center_shift_pos.right;
                // CenterShiftOwn.x = 1.0 - CenterShiftOwn.x; // this is unnecessary because right eye can use its independent center shift in multiview frames.
            }
            CenterShiftOwn.y = 1.0 - CenterShiftOwn.y;
            
            // Transform screen UV to aligned eye UV.
            var alignedUV = corrected_uv;
            
            // Precompute the common angle factor to avoid repeated calculations.
            var angleFactor = 2.0 * atan(magnitude / 2.0);
            
            // Decode each coordinate using the optimized inverse warp function.
            var uncompressedUV: vec2f;
            uncompressedUV.x = MapUV_inverse_FRW(alignedUV.x, magnitude, CenterShiftOwn.x, angleFactor);
            uncompressedUV.y = MapUV_inverse_FRW(alignedUV.y, magnitude, CenterShiftOwn.y, angleFactor);
            
            // obtain final texture UV
            corrected_uv = uncompressedUV;  

            // return vec4f(0.0, 0.0, 1.0, 1.0); // D-FRW not implemented yet
    }
    else if (ENABLE_FFE && FOVEATION_METHOD == 4u) {
        // -- GT (Ground Truth / No Foveation) Shaders --
        // No UV correction needed, passthrough the original UVs.
    }

    var color: vec3f;
    if ENABLE_UPSCALING {
        color = sgsr(vec4f(corrected_uv.x, corrected_uv.y, 0.0, 0.0), upscale_source_resolution).xyz;
    } else {
        color = textureSample(stream_texture, stream_sampler, corrected_uv).rgb;
    }

    if ENABLE_SRGB_CORRECTION {
        let condition = vec3f(f32(color.r < THRESHOLD), f32(color.g < THRESHOLD), f32(color.b < THRESHOLD));
        let lowValues = color * DIV12;
        let highValues = pow((color + vec3f(0.055)) * DIV1, GAMMA);
        color = condition * lowValues + (1.0 - condition) * highValues;
    }

    if ENCODING_GAMMA != 0.0 {
        let enc_condition = vec3f(f32(color.r < 0.0), f32(color.g < 0.0), f32(color.b < 0.0));
        let enc_lowValues = color;
        let enc_highValues = pow(color, vec3f(ENCODING_GAMMA));
        color = enc_condition * enc_lowValues + (1.0 - enc_condition) * enc_highValues;
    }

    var alpha = pc.blend_alpha; // Default to Blend passthrough mode
    if pc.passthrough_mode != 0 { // Chroma key
        var current = color;
        if pc.passthrough_mode == 2 { // HSV mode
            current = rgb_to_hsv(color);
        }
        let mask = chroma_key_mask(current);

        // Note: because of this calculation, we require premultiplied alpha option in the XR layer
        color = max(color * mask, vec3f(0.0));
        alpha = mask;
    }

    // Optional gaze debug marker overlay
    if (ENABLE_GAZE_DEBUG) {
        var gaze_uv = dbg.gaze_pos.left;
        if (pc.view_idx == 1u) {
            gaze_uv = dbg.gaze_pos.right;
        }
        gaze_uv.y = 1.0 - gaze_uv.y; // Flip Y for texture UV space

        let dist = distance(uv, gaze_uv);
        let radius = GAZE_MARKER_RADIUS;
        let feather = max(GAZE_MARKER_FEATHER, 0.0001);
        let t = smoothstep(radius + feather, radius, dist);

        let marker_col = vec3f(GAZE_MARKER_R, GAZE_MARKER_G, GAZE_MARKER_B);
        color = color * (1.0 - t) + marker_col * t;
        alpha = max(alpha, t);
    }

    // Optional center-shift debug marker overlay (blue)
    if (ENABLE_CENTER_SHIFT_DEBUG) {
        var cs_uv = dbg.center_shift_pos.left;
        if (pc.view_idx == 1u) {
            cs_uv = dbg.center_shift_pos.right;
        }
        cs_uv.y = 1.0 - cs_uv.y; // Flip Y for texture UV space

        let dist = distance(uv, cs_uv);
        let radius = CENTER_SHIFT_MARKER_RADIUS;
        let feather = max(CENTER_SHIFT_MARKER_FEATHER, 0.0001);
        let t = smoothstep(radius + feather, radius, dist);

        let marker_col = vec3f(
            CENTER_SHIFT_MARKER_R,
            CENTER_SHIFT_MARKER_G,
            CENTER_SHIFT_MARKER_B,
        );
        color = color * (1.0 - t) + marker_col * t;
        alpha = max(alpha, t);
    }

    if (ENABLE_FOVEAL_BOX_DEBUG) {
        // Optional foveal box debug marker overlay (yellow outline, no fill)
        // Uses the server-provided per-frame center size when available.
        var cs_uv = dbg.center_shift_pos.left;
        if (pc.view_idx == 1u) {
            cs_uv = dbg.center_shift_pos.right;
        }
        cs_uv.y = 1.0 - cs_uv.y; // Flip Y for texture UV space

        var box_size = dbg.center_size;
        if (box_size.x <= 0.0 && box_size.y <= 0.0) {
            box_size = vec2f(CENTER_SIZE_X, CENTER_SIZE_Y);
        }

        let half_size = 0.5 * box_size;
        let thickness = max(CENTER_SHIFT_MARKER_RADIUS, 0.0001);
        let feather = max(CENTER_SHIFT_MARKER_FEATHER, 0.0001);
        let inner_half_size = max(half_size - vec2f(thickness, thickness), vec2f(0.0));

        // Box SDFs in UV space: negative inside, positive outside.
        let p = abs(uv - cs_uv);
        let d_outer = p - half_size;
        let d_inner = p - inner_half_size;
        let sdf_outer = max(d_outer.x, d_outer.y);
        let sdf_inner = max(d_inner.x, d_inner.y);

        let outer_mask = smoothstep(0.0, feather, -sdf_outer);
        let inner_mask = smoothstep(0.0, feather, -sdf_inner);
        let box_outline = clamp(outer_mask - inner_mask, 0.0, 1.0);

        let box_col = vec3f(1.0, 1.0, 0.0);
        color = color * (1.0 - box_outline) + box_col * box_outline;
        alpha = max(alpha, box_outline);
    }

    return vec4f(color, alpha);
}


fn chroma_key_mask(color: vec3f) -> f32 {
    let start_max = vec3f(pc.ck_channel0.x, pc.ck_channel1.x, pc.ck_channel2.x);
    let start_min = vec3f(pc.ck_channel0.y, pc.ck_channel1.y, pc.ck_channel2.y);
    let end_min = vec3f(pc.ck_channel0.z, pc.ck_channel1.z, pc.ck_channel2.z);
    let end_max = vec3f(pc.ck_channel0.w, pc.ck_channel1.w, pc.ck_channel2.w);

    let start_mask = smoothstep(start_min.yz, start_max.yz, color.yz);
    let end_mask = smoothstep(end_min.yz, end_max.yz, color.yz);
    let sv_mask = max(start_mask, end_mask);
    // create t1 < t2 < t3, based on the rotated hue starting with start_max.x representing 0.
    let t3 = end_max.x - start_max.x;
    // check hue range
    if t3 > 1.0 {
        return max(sv_mask.x, sv_mask.y);
    }
    let x = fract(color.x - start_max.x);
    var t1 = fract(start_min.x - start_max.x);
    var t2 = fract(end_min.x - start_max.x);

    return max(max(smoothstep(t1, 0.0, x), smoothstep(t2, t3, x)), max(sv_mask.x, sv_mask.y));
}

fn rgb_to_hsv(rgb: vec3f) -> vec3f {
    let cmax = max(rgb.r, max(rgb.g, rgb.b));
    let cmin = min(rgb.r, min(rgb.g, rgb.b));
    let delta = cmax - cmin;

    var h = 0.0;
    var s = 0.0;
    let v = cmax;

    if cmax > cmin {
        s = delta / cmax;

        if rgb.r == cmax {
            h = (rgb.g - rgb.b) / delta;
        } else if rgb.g == cmax {
            h = 2.0 + (rgb.b - rgb.r) / delta;
        } else {
            h = 4.0 + (rgb.r - rgb.g) / delta;
        }
        h = fract(h / 6.0);
    }

    return vec3f(h, s, v);
}

//============================================================================================================
//
//
//                  Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================

fn fastLanczos2(x: f32) -> f32
{
    var wA: f32 = x - 4.0;
    let wB: f32 = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

fn weightY(dx: f32, dy: f32, c: f32, data: vec3f) -> vec2f {
    let stdA: f32 = data.x;
    let dir: vec2f = data.yz;
    let edgeDis: f32 = ((dx * dir.y) + (dy * dir.x));
    let x: f32 = (((dx * dx) + (dy * dy)) + ((edgeDis * edgeDis) * ((clamp(((c * c) * stdA), 0.0, 1.0) * 0.7) + -1.0)));
    let w: f32 = fastLanczos2(x);
    return vec2f(w, w * c);
}

fn weightYned(dx: f32, dy: f32, c: f32, data: f32) -> vec2f {
    let stdA: f32 = data;
    let x: f32 = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * stdA, 0.0, 1.0);
    let w: f32 = fastLanczos2(x);
    return vec2f(w, w * c);
}

fn edgeDirection(left: vec4f, right: vec4f) -> vec2f
{
    var dir: vec2f;
    let RxLz: f32 = (right.x + (-left.z));
    let RwLy: f32 = (right.w + (-left.y));
    var delta: vec2f;
    delta.x = (RxLz + RwLy);
    delta.y = (RxLz + (-RwLy));
    let lengthInv: f32 = inverseSqrt((delta.x * delta.x + 3.075740e-05) + (delta.y * delta.y));
    dir.x = (delta.x * lengthInv);
    dir.y = (delta.y * lengthInv);
    return dir;
}

fn sgsr(in_TEXCOORD0: vec4f, source_resolution_multiplier: f32) -> vec4f {
    // https://github.com/SnapdragonStudios/snapdragon-gsr/issues/2
    let dim = vec2f(textureDimensions(stream_texture)) * source_resolution_multiplier;
    let viewport_info = vec4f(1/dim.x, 1/dim.y, dim.x, dim.y);

    var color: vec4f;
    let texSample = textureSampleLevel(stream_texture, stream_sampler, in_TEXCOORD0.xy, 0.0);
    color.x = texSample.x;
    color.y = texSample.y;
    color.z = texSample.z;

    // all of these 1 values are the OperationMode
    // see https://github.com/SnapdragonStudios/snapdragon-gsr/tree/main/sgsr/v1#operation-mode
    let imgCoord: vec2f = (in_TEXCOORD0.xy * viewport_info.zw) + vec2f(-0.5, 0.5);
    let imgCoordPixel: vec2f = floor(imgCoord);
    var coord: vec2f = (imgCoordPixel * viewport_info.xy);
    let pl: vec2f = (imgCoord + (-imgCoordPixel));
    var left: vec4f = textureGather(1, stream_texture, stream_sampler, coord);

    let edgeVote: f32 = abs(left.z - left.y) + abs(color[1] - left.y) + abs(color[1] - left.z);
    if edgeVote > UPSCALE_EDGE_THRESHOLD {
        coord.x += viewport_info.x;

        var right: vec4f = textureGather(1, stream_texture, stream_sampler, coord + vec2f(viewport_info.x, 0.0));
        var upDown: vec4f;
        let texGatherA = textureGather(1, stream_texture, stream_sampler, coord + vec2f(0.0, -viewport_info.y));
        upDown.x = texGatherA.w;
        upDown.y = texGatherA.z;
        let texGatherB = textureGather(1, stream_texture, stream_sampler, coord + vec2f(0.0, viewport_info.y));
        upDown.z = texGatherB.y;
        upDown.w = texGatherB.x;

        let mean: f32 = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color[1] - mean;

        let sum: f32 = (((((abs(left.x) + abs(left.y)) + abs(left.z)) + abs(left.w)) + (((abs(right.x) + abs(right.y)) + abs(right.z)) + abs(right.w))) + (((abs(upDown.x) + abs(upDown.y)) + abs(upDown.z)) + abs(upDown.w)));
        let sumMean: f32 = 1.014185e+01 / sum;
        let stdA: f32 = (sumMean * sumMean);

        var aWY: vec2f;
        if UPSCALE_USE_EDGE_DIRECTION {
            let data = vec3f(stdA, edgeDirection(left, right));
            aWY = weightY(pl.x, pl.y + 1.0, upDown.x, data);
            aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
            aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
            aWY += weightY(pl.x, pl.y - 2.0, upDown.w, data);
            aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, data);
            aWY += weightY(pl.x, pl.y - 1.0, left.y, data);
            aWY += weightY(pl.x, pl.y, left.z, data);
            aWY += weightY(pl.x + 1.0, pl.y, left.w, data);
            aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, data);
            aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, data);
            aWY += weightY(pl.x - 2.0, pl.y, right.z, data);
            aWY += weightY(pl.x - 1.0, pl.y, right.w, data);
        } else {
            let data: f32 = stdA;
            aWY = weightYned(pl.x, pl.y + 1.0, upDown.x, data);
            aWY += weightYned(pl.x - 1.0, pl.y + 1.0, upDown.y, data);
            aWY += weightYned(pl.x - 1.0, pl.y - 2.0, upDown.z, data);
            aWY += weightYned(pl.x, pl.y - 2.0, upDown.w, data);
            aWY += weightYned(pl.x + 1.0, pl.y - 1.0, left.x, data);
            aWY += weightYned(pl.x, pl.y - 1.0, left.y, data);
            aWY += weightYned(pl.x, pl.y, left.z, data);
            aWY += weightYned(pl.x + 1.0, pl.y, left.w, data);
            aWY += weightYned(pl.x - 1.0, pl.y - 1.0, right.x, data);
            aWY += weightYned(pl.x - 2.0, pl.y - 1.0, right.y, data);
            aWY += weightYned(pl.x - 2.0, pl.y, right.z, data);
            aWY += weightYned(pl.x - 1.0, pl.y, right.w, data);
        }

        let finalY: f32 = aWY.y / aWY.x;
        let maxY: f32 = max(max(left.y, left.z), max(right.x, right.w));
        let minY: f32 = min(min(left.y, left.z), min(right.x, right.w));
        var deltaY: f32 = clamp(UPSCALE_EDGE_SHARPNESS * finalY, minY, maxY) - color.w;

        //smooth high contrast input
        deltaY = clamp(deltaY, -23.0 / 255.0, 23.0 / 255.0);

        color.x = clamp((color.x + deltaY), 0.0, 1.0);
        color.y = clamp((color.y + deltaY), 0.0, 1.0);
        color.z = clamp((color.z + deltaY), 0.0, 1.0);
    }

    color.w = 1.0; //assume alpha channel is not used

    return color;
}
