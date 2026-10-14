#pragma once

#include <stdint.h>

#include "d3d-render-utils/RenderPipeline.h"

#include "gaze_prediction.h"

struct FoveationVars {
    uint32_t targetEyeWidth;
    uint32_t targetEyeHeight;
    uint32_t optimizedEyeWidth;
    uint32_t optimizedEyeHeight;

    float eyeWidthRatio;
    float eyeHeightRatio;

    float centerSizeX;
    float centerSizeY;
    float centerShiftX;
    float centerShiftY;
    float centerShiftXLeft;
    float centerShiftYLeft;
    float centerShiftXRight;
    float centerShiftYRight;
    float edgeRatioX;
    float edgeRatioY;
};

struct GazeCsvTelemetry {
    float predictorErrorX = 0.0f;
    float predictorErrorY = 0.0f;
    float dynamicErrorX = 0.0f;
    float dynamicErrorY = 0.0f;
    int predictorTuningLatencyMs = 0;

    uint8_t predictionEnabled = 0;
    uint8_t hysteresisEnabled = 0;
    uint8_t dynamicSizingEnabled = 0;

    float measuredShiftXLeft = 0.5f;
    float measuredShiftYLeft = 0.5f;
    float measuredShiftXRight = 0.5f;
    float measuredShiftYRight = 0.5f;

    float anchorShiftXLeft = 0.5f;
    float anchorShiftYLeft = 0.5f;
    float anchorShiftXRight = 0.5f;
    float anchorShiftYRight = 0.5f;

    float finalShiftXLeft = 0.5f;
    float finalShiftYLeft = 0.5f;
    float finalShiftXRight = 0.5f;
    float finalShiftYRight = 0.5f;

    float feedbackBiasXLeft = 0.0f;
    float feedbackBiasYLeft = 0.0f;
    float feedbackBiasXRight = 0.0f;
    float feedbackBiasYRight = 0.0f;
};

class FFR {
public:
    FFR(ID3D11Device* device);
    void Initialize(
        ID3D11Texture2D* compositionTexture,
        uint32_t renderEyeWidth,
        uint32_t renderEyeHeight,
        uint32_t encoderEyeWidth,
        uint32_t encoderEyeHeight
    );
    void Render(uint64_t targetTimestampNs);
    void GetOptimizedResolution(uint32_t* width, uint32_t* height);
    ID3D11Texture2D* GetOutputTexture();

private:
    FoveationVars CalculateFoveationVars(uint64_t targetTimestampNs);

    Microsoft::WRL::ComPtr<ID3D11Device> mDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> mImmediateContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> mOptimizedTexture;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> mQuadVertexShader;

    Microsoft::WRL::ComPtr<ID3D11Buffer> mFoveatedRenderingBuffer;

    std::vector<d3d_render_utils::RenderPipeline> mPipelines;

    // Emulated render eye resolution (recommended target). Used for logging/telemetry only.
    uint32_t mRenderEyeWidth = 0;
    uint32_t mRenderEyeHeight = 0;

    // Encoder/transcoding eye resolution (output sizing).
    uint32_t mTargetEyeWidth = 0;
    uint32_t mTargetEyeHeight = 0;

    // Encoder-aligned output dimensions + ratios (stable across frames).
    uint32_t mOptimizedEyeWidthAligned = 0;
    uint32_t mOptimizedEyeHeightAligned = 0;

    // --- Gaze prediction state (per-eye) ---
    uint64_t mLastGazePredictTimestampNs = 0;
    alvr::gaze_prediction::GazeState mGazeStateLeft;
    alvr::gaze_prediction::GazeState mGazeStateRight;
    alvr::gaze_prediction::DynamicFoveaBoxState mDynFoveaLeft;
    alvr::gaze_prediction::DynamicFoveaBoxState mDynFoveaRight;
    bool mDynFoveaInitialized = false;

    GazeCsvTelemetry mLastGazeCsvTelemetry;
};
