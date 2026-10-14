#include "FFR.h"

#include "alvr_server/Settings.h"
#include "alvr_server/Utils.h"
#include "alvr_server/bindings.h"
#include "alvr_server/GtRecording.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>

// Some Windows headers define min/max macros that break <algorithm>.
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

using alvr::gaze_prediction::DynamicFoveaBoxState;
using alvr::gaze_prediction::GazeState;
using alvr::gaze_prediction::GetHysteresisKwargs;
using alvr::gaze_prediction::GetPredictorConfig;
using alvr::gaze_prediction::PredictGazeGated;
using alvr::gaze_prediction::UpdateGazePrediction;
using alvr::gaze_prediction::ApplyGazeHysteresis;
using alvr::gaze_prediction::ApplyPredictionErrorFeedback;

using Microsoft::WRL::ComPtr;
using namespace d3d_render_utils;

namespace {

constexpr int kDefaultPredictorLatencyMs = 60;

inline float ToPm1(float v01) {
    return 2.0f * v01 - 1.0f;
}

inline float To01(float vpm1) {
    return 0.5f * (vpm1 + 1.0f);
}

inline bool IsValidUv01(float v) {
    return (std::isfinite)(v) && v >= 0.0f && v <= 1.0f;
}

inline float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

inline float SanitizeGazeUvOrCenter(float v) {
    // Client gaze tracking is expected in normalized UV coordinates [0, 1].
    // If data is missing (often represented by a sentinel like -1), NaN/inf,
    // or otherwise out of range, default to the display center.
    if (!(std::isfinite)(v) || v < 0.0f || v > 1.0f) {
        return 0.5f;
    }
    return v;
}

struct EyeUv01 {
    float x01 = 0.5f;
    float y01 = 0.5f;
    bool valid = false;
};

inline EyeUv01 ReadMeasuredEyeCenterShiftUv(bool right_eye) {
    const float x01 = right_eye ? (float)Settings::Instance().m_foveationCenterShiftXRight
                                : (float)Settings::Instance().m_foveationCenterShiftXLeft;
    const float y01 = right_eye ? (float)Settings::Instance().m_foveationCenterShiftYRight
                                : (float)Settings::Instance().m_foveationCenterShiftYLeft;

    return {
        SanitizeGazeUvOrCenter(x01),
        SanitizeGazeUvOrCenter(y01),
        IsValidUv01(x01) && IsValidUv01(y01),
    };
}

inline float ComputePredictionDtSeconds(uint64_t targetTimestampNs, uint64_t lastTimestampNs) {
    // dt based on the target timestamps (stable cadence).
    if (lastTimestampNs != 0 && targetTimestampNs > lastTimestampNs) {
        const uint64_t dt_ns = targetTimestampNs - lastTimestampNs;
        return (float)dt_ns / 1e9f;
    }

    const int rr = Settings::Instance().m_refreshRate;
    if (rr > 0) {
        return 1.0f / (float)rr;
    }

    return 1.0f / 72.0f;
}

inline float SignedUvFromMetric(float magnitude_uv) {
    if (!(std::isfinite)(magnitude_uv)) {
        return 0.0f;
    }
    return std::clamp(magnitude_uv, -1.0f, 1.0f);
}

inline float ClampSigned01(float v) {
    return std::clamp(v, -1.0f, 1.0f);
}

inline float SanitizeLatencyMs(float latency_ms) {
    if (!(std::isfinite)(latency_ms) || latency_ms < 0.0f) {
        return 0.0f;
    }

    return latency_ms;
}

inline int ResolvePredictorTuningLatencyMs(float total_latency_ms) {
    if (!(std::isfinite)(total_latency_ms) || total_latency_ms <= 0.0f) {
        return kDefaultPredictorLatencyMs;
    }

    const int rounded_latency_ms = (int)std::lround(total_latency_ms);
    const int clamped_latency_ms = std::clamp(rounded_latency_ms, 30, 90);
    return alvr::gaze_prediction::QuantizeAddedLatencyMs(clamped_latency_ms);
}

inline alvr::gaze_prediction::OnlineFeedbackParams ResolveFeedbackParams(int tuning_latency_ms) {
    alvr::gaze_prediction::OnlineFeedbackParams params;

    switch (alvr::gaze_prediction::QuantizeAddedLatencyMs(tuning_latency_ms)) {
    case 30:
        params.error_gain = 0.08f;
        params.decay = 0.84f;
        params.max_bias = 0.050f;
        params.velocity_damp = 0.22f;
        break;
    case 60:
        params.error_gain = 0.06f;
        params.decay = 0.81f;
        params.max_bias = 0.042f;
        params.velocity_damp = 0.26f;
        break;
    case 90:
        // High-latency profile: reduce integral wind-up and clamp corrective bias.
        params.error_gain = 0.04f;
        params.decay = 0.78f;
        params.max_bias = 0.035f;
        params.velocity_damp = 0.30f;
        break;
    default:
        break;
    }

    return params;
}

inline void ResetHysteresisState(GazeState& state) {
    state.locked = false;
    state.lock_frames = 0;
    state.lock_anchor_initialized = false;
}

inline void ResetPredictionFeedbackState(GazeState& state) {
    state.feedback_bias_x = 0.0f;
    state.feedback_bias_y = 0.0f;
    state.last_feedback_error = 0.0f;
}

struct EyeGazeFrameState {
    bool valid = false;
    float measured_x_pm1 = 0.0f;
    float measured_y_pm1 = 0.0f;
    float anchor_x_pm1 = 0.0f;
    float anchor_y_pm1 = 0.0f;
    float final_x_pm1 = 0.0f;
    float final_y_pm1 = 0.0f;
    alvr::gaze_prediction::GazePredictionInfo info;
};

extern "C" void SetDynamicFovealSizingParams(
    float dynMinSize,
    float dynMaxSize,
    float growRate,
    float shrinkRate
) {
    if (!(std::isfinite)(dynMinSize) || !(std::isfinite)(dynMaxSize) || !(std::isfinite)(growRate)
        || !(std::isfinite)(shrinkRate)) {
        return;
    }

    dynMinSize = std::clamp(dynMinSize, 0.0f, 1.0f);
    dynMaxSize = std::clamp(dynMaxSize, dynMinSize, 1.0f);

    growRate = std::clamp(growRate, 0.0f, 1.0f);
    shrinkRate = std::clamp(shrinkRate, 0.0f, 1.0f);

    Settings::Instance().m_dynMinSize = dynMinSize;
    Settings::Instance().m_dynMaxSize = dynMaxSize;
    Settings::Instance().m_dynGrowRate = growRate;
    Settings::Instance().m_dynShrinkRate = shrinkRate;
}

void LogGazeRowCsv(
    const FoveationVars& fovVars,
    [[maybe_unused]] const GazeCsvTelemetry& gazeTelemetry,
    uint32_t renderEyeWidth,
    uint32_t renderEyeHeight
) {
    static std::mutex logMutex;
    static std::ofstream file;
    static uint64_t frameIndex = 0;
    static uint64_t flushCounter = 0;
    static uint64_t openSessionId = 0;

    alvr::gt_recording::Update();

    if (!alvr::gt_recording::IsLogsEnabled()) {
        std::lock_guard<std::mutex> lock(logMutex);
        if (file.is_open()) {
            file.flush();
            file.close();
        }
        return;
    }

    const uint64_t sessionId = alvr::gt_recording::SessionId();

    const auto timestampMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch()
                             )
                                 .count();

    const float gazeMagnitudeX = GetGazeMagnitudeX();
    const float gazeMagnitudeY = GetGazeMagnitudeY();
    const float mtpLatencyMs = GetGazeNetworkLatencyMs();

    std::lock_guard<std::mutex> lock(logMutex);

    if (!file.is_open() || openSessionId != sessionId) {
        if (file.is_open()) {
            file.flush();
            file.close();
        }

        std::error_code ec;
        std::filesystem::create_directories(alvr::gt_recording::LogDir(), ec);

        // Overwrite on every recording start.
        file.open(alvr::gt_recording::GetFfrLogFilePath(), std::ios::out | std::ios::trunc);
        if (!file.is_open()) {
            return;
        }

        openSessionId = sessionId;
        frameIndex = 0;
        flushCounter = 0;

        file.setf(std::ios::fixed);
        file << std::setprecision(6);
        file
            << "frameIndex,timestampMs,targetEyeWidth,targetEyeHeight,"
               "optimizedEyeWidth,optimizedEyeHeight,eyeWidthRatio,eyeHeightRatio,"
               "centerSizeX,centerSizeY,centerShiftX,centerShiftY,"
               "centerShiftXLeft,centerShiftYLeft,centerShiftXRight,centerShiftYRight,"
               "edgeRatioX,edgeRatioY,gazeMagnitudeX,gazeMagnitudeY,mtpLatencyMs"
            // Optional diagnostics: enable together with the corresponding row values below.
            /*
            << ",predictorErrorX,predictorErrorY,dynamicErrorX,dynamicErrorY,"
               "predictorTuningLatencyMs,predictionEnabled,hysteresisEnabled,dynamicSizingEnabled,"
               "measuredShiftXLeft,measuredShiftYLeft,measuredShiftXRight,measuredShiftYRight,"
               "anchorShiftXLeft,anchorShiftYLeft,anchorShiftXRight,anchorShiftYRight,"
               "finalShiftXLeft,finalShiftYLeft,finalShiftXRight,finalShiftYRight,"
               "feedbackBiasXLeft,feedbackBiasYLeft,feedbackBiasXRight,feedbackBiasYRight"
            */
            << '\n';
        file.flush();
    }

    // Note: for historical reasons the CSV columns are named "targetEye*" and
    // "optimizedEye*". Here we log:
    // - targetEye*: emulated render eye resolution (recommended target)
    // - optimizedEye*: encoder-aligned output eye resolution
    file << frameIndex++ << ',' << timestampMs << ','
         << renderEyeWidth << ',' << renderEyeHeight << ','
         << fovVars.optimizedEyeWidth << ',' << fovVars.optimizedEyeHeight << ','
         << fovVars.eyeWidthRatio << ',' << fovVars.eyeHeightRatio << ','
         << fovVars.centerSizeX << ',' << fovVars.centerSizeY << ','
         << fovVars.centerShiftX << ',' << fovVars.centerShiftY << ','
         << fovVars.centerShiftXLeft << ',' << fovVars.centerShiftYLeft << ','
         << fovVars.centerShiftXRight << ',' << fovVars.centerShiftYRight << ','
         << fovVars.edgeRatioX << ',' << fovVars.edgeRatioY << ','
         << gazeMagnitudeX << ',' << gazeMagnitudeY << ',' << mtpLatencyMs
         // Optional diagnostics: enable together with the corresponding header columns above.
         /*
         << ',' << gazeTelemetry.predictorErrorX << ',' << gazeTelemetry.predictorErrorY << ','
         << gazeTelemetry.dynamicErrorX << ',' << gazeTelemetry.dynamicErrorY << ','
         << gazeTelemetry.predictorTuningLatencyMs << ','
         << (int)gazeTelemetry.predictionEnabled << ','
         << (int)gazeTelemetry.hysteresisEnabled << ','
         << (int)gazeTelemetry.dynamicSizingEnabled << ','
         << gazeTelemetry.measuredShiftXLeft << ',' << gazeTelemetry.measuredShiftYLeft << ','
         << gazeTelemetry.measuredShiftXRight << ',' << gazeTelemetry.measuredShiftYRight << ','
         << gazeTelemetry.anchorShiftXLeft << ',' << gazeTelemetry.anchorShiftYLeft << ','
         << gazeTelemetry.anchorShiftXRight << ',' << gazeTelemetry.anchorShiftYRight << ','
         << gazeTelemetry.finalShiftXLeft << ',' << gazeTelemetry.finalShiftYLeft << ','
         << gazeTelemetry.finalShiftXRight << ',' << gazeTelemetry.finalShiftYRight << ','
         << gazeTelemetry.feedbackBiasXLeft << ',' << gazeTelemetry.feedbackBiasYLeft << ','
         << gazeTelemetry.feedbackBiasXRight << ',' << gazeTelemetry.feedbackBiasYRight
         */
         << '\n';

    // Keep overhead reasonable while ensuring data is not lost on crash.
    if ((++flushCounter % 120) == 0) {
        file.flush();
    }
}

constexpr uint32_t kEncoderDimAlignment = 32;

inline uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

inline void ComputeAlignedOptimizedDims(
    uint32_t encoderEyeWidthPx,
    uint32_t encoderEyeHeightPx,
    uint32_t* outOptimizedEyeWidthAligned,
    uint32_t* outOptimizedEyeHeightAligned,
    float* outEyeWidthRatio,
    float* outEyeHeightRatio
) {
    // Optimized output dimensions are fixed to the encoder eye resolution (only padded for
    // encoder alignment) for all foveation methods.
    const uint32_t alignedW = AlignUp(encoderEyeWidthPx, kEncoderDimAlignment);
    const uint32_t alignedH = AlignUp(encoderEyeHeightPx, kEncoderDimAlignment);

    *outOptimizedEyeWidthAligned = alignedW;
    *outOptimizedEyeHeightAligned = alignedH;

    *outEyeWidthRatio = alignedW > 0 ? (float)encoderEyeWidthPx / (float)alignedW : 1.0f;
    *outEyeHeightRatio = alignedH > 0 ? (float)encoderEyeHeightPx / (float)alignedH : 1.0f;
}

struct GazeFoveationOverrides {
    bool enabled = false;

    // Normalized [0,1], x:right, y:down.
    float shift_x_left = 0.5f;
    float shift_y_left = 0.5f;
    float shift_x_right = 0.5f;
    float shift_y_right = 0.5f;

    // Optional: dynamic center size (methods 1/2 only).
    bool override_center_size = false;
    float center_size_x = 0.0f;
    float center_size_y = 0.0f;
};

struct GazeRuntimeState {
    GazeState& left_state;
    GazeState& right_state;
    DynamicFoveaBoxState& left_dyn_state;
    DynamicFoveaBoxState& right_dyn_state;
    bool& dynamic_initialized;
    uint64_t& last_predict_timestamp_ns;
};

constexpr float kNpConfidenceDivisor = 3.5f;
constexpr float kNpConfidenceFloor = 0.25f;

GazeFoveationOverrides ComputeGazeOverrides(
    uint64_t targetTimestampNs,
    float baseCenterSizeX,
    float baseCenterSizeY,
    bool predictionEnabled,
    bool hysteresisEnabled,
    bool dynamicSizingEnabled,
    GazeRuntimeState state,
    GazeCsvTelemetry* outTelemetry
) {
    GazeFoveationOverrides overrides;

    if (outTelemetry) {
        *outTelemetry = {};
    }

    const EyeUv01 meas_left = ReadMeasuredEyeCenterShiftUv(false);
    const EyeUv01 meas_right = ReadMeasuredEyeCenterShiftUv(true);

    if (!hysteresisEnabled) {
        ResetHysteresisState(state.left_state);
        ResetHysteresisState(state.right_state);
    }

    if (!predictionEnabled) {
        ResetPredictionFeedbackState(state.left_state);
        ResetPredictionFeedbackState(state.right_state);
    }

    if (!dynamicSizingEnabled) {
        state.dynamic_initialized = false;
    }

    if (!predictionEnabled && !hysteresisEnabled && !dynamicSizingEnabled) {
        state.last_predict_timestamp_ns = 0;
        return overrides;
    }

    const float totalLatencyMs = SanitizeLatencyMs(GetGazeNetworkLatencyMs());
    const int predictorTuningLatencyMs = ResolvePredictorTuningLatencyMs(totalLatencyMs);
    const auto predictorCfg = GetPredictorConfig(predictorTuningLatencyMs);
    const auto hysteresisKwargs = GetHysteresisKwargs(predictorCfg.kwargs, nullptr);

    const int totalLatencyMsRounded = (int)std::lround(totalLatencyMs);
    const float dt_s = ComputePredictionDtSeconds(targetTimestampNs, state.last_predict_timestamp_ns);
    const float foveaRadius = (std::max)(0.01f, 0.5f * (baseCenterSizeX + baseCenterSizeY));
    const float dynamicErrorX = ClampSigned01(SignedUvFromMetric(GetGazeMagnitudeX()));
    const float dynamicErrorY = ClampSigned01(SignedUvFromMetric(GetGazeMagnitudeY()));
    const float predictorErrorX = ClampSigned01(SignedUvFromMetric(GetGazePredictorErrorX()));
    const float predictorErrorY = ClampSigned01(SignedUvFromMetric(GetGazePredictorErrorY()));

    auto resolveEye = [&](const EyeUv01& measurement, GazeState& gaze_state) {
        EyeGazeFrameState eye;
        eye.valid = measurement.valid;
        eye.measured_x_pm1 = ToPm1(measurement.x01);
        eye.measured_y_pm1 = ToPm1(measurement.y01);
        eye.anchor_x_pm1 = eye.measured_x_pm1;
        eye.anchor_y_pm1 = eye.measured_y_pm1;
        eye.final_x_pm1 = eye.measured_x_pm1;
        eye.final_y_pm1 = eye.measured_y_pm1;

        if (!measurement.valid) {
            return eye;
        }

        if (predictionEnabled) {
            const auto predicted = PredictGazeGated(
                eye.measured_x_pm1,
                eye.measured_y_pm1,
                gaze_state,
                dt_s,
                totalLatencyMsRounded,
                foveaRadius,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                predictorTuningLatencyMs,
                /*vel_smooth*/ 0.0f,
                /*use_hysteresis*/ false,
                hysteresisKwargs.k_enter,
                hysteresisKwargs.k_exit,
                hysteresisKwargs.soft_lock,
                hysteresisKwargs.speed_unlock,
                hysteresisKwargs.hysteresis_motion_scale,
                hysteresisKwargs.max_lock_frames
            );

            const float residualPeak = (std::max)(std::abs(predictorErrorX), std::abs(predictorErrorY));
            const float residualBlend = std::clamp((residualPeak - 0.07f) / 0.16f, 0.0f, 0.85f);

            const float dampedPredX
                = predicted.pred_x * (1.0f - residualBlend) + eye.measured_x_pm1 * residualBlend;
            const float dampedPredY
                = predicted.pred_y * (1.0f - residualBlend) + eye.measured_y_pm1 * residualBlend;

            eye.anchor_x_pm1 = dampedPredX;
            eye.anchor_y_pm1 = dampedPredY;
            eye.final_x_pm1 = dampedPredX;
            eye.final_y_pm1 = dampedPredY;
            eye.info = predicted.info;
        } else {
            const auto base = UpdateGazePrediction(
                eye.measured_x_pm1,
                eye.measured_y_pm1,
                gaze_state,
                dt_s,
                foveaRadius,
                predictorCfg.kwargs.alpha,
                predictorCfg.kwargs.beta,
                hysteresisKwargs.k_enter,
                hysteresisKwargs.k_exit,
                hysteresisKwargs.soft_lock,
                hysteresisKwargs.speed_unlock,
                predictorCfg.kwargs.adaptive_gains,
                predictorCfg.kwargs.fast_speed,
                predictorCfg.kwargs.alpha_fast,
                predictorCfg.kwargs.beta_fast,
                /*use_hysteresis*/ false,
                hysteresisKwargs.hysteresis_motion_scale,
                hysteresisKwargs.max_lock_frames,
                /*predict_ahead*/ false
            );

            eye.anchor_x_pm1 = base.out_x;
            eye.anchor_y_pm1 = base.out_y;
            eye.final_x_pm1 = base.out_x;
            eye.final_y_pm1 = base.out_y;
            eye.info = base.info;
            eye.info.tuning_latency_ms = predictorTuningLatencyMs;
        }

        if (hysteresisEnabled) {
            const auto hysteresis = ApplyGazeHysteresis(
                eye.anchor_x_pm1,
                eye.anchor_y_pm1,
                gaze_state,
                foveaRadius,
                eye.info.meas_step,
                eye.info.speed,
                hysteresisKwargs.k_enter,
                hysteresisKwargs.k_exit,
                hysteresisKwargs.soft_lock,
                hysteresisKwargs.speed_unlock,
                hysteresisKwargs.hysteresis_motion_scale,
                hysteresisKwargs.max_lock_frames
            );

            eye.final_x_pm1 = hysteresis.out_x;
            eye.final_y_pm1 = hysteresis.out_y;
            eye.info.locked = hysteresis.info.locked;
            eye.info.lock_frames = hysteresis.info.lock_frames;
            eye.info.max_lock_frames = hysteresis.info.max_lock_frames;
            eye.info.use_hysteresis = true;
            eye.info.radius = hysteresis.info.radius;
            eye.info.k_enter_eff = hysteresis.info.k_enter_eff;
            eye.info.k_exit_eff = hysteresis.info.k_exit_eff;
            eye.info.R_enter = hysteresis.info.R_enter;
            eye.info.R_exit = hysteresis.info.R_exit;
        }

        if (predictionEnabled) {
            const auto feedbackParams = ResolveFeedbackParams(predictorTuningLatencyMs);
            const float residualPeak = (std::max)(std::abs(predictorErrorX), std::abs(predictorErrorY));

            if (residualPeak < 0.08f) {
                ApplyPredictionErrorFeedback(
                    gaze_state,
                    predictorErrorX,
                    predictorErrorY,
                    &feedbackParams
                );
            } else {
                // During saccade-like residual spikes, avoid integrating feedback bias.
                gaze_state.feedback_bias_x *= 0.80f;
                gaze_state.feedback_bias_y *= 0.80f;
            }
        }

        return eye;
    };

    const EyeGazeFrameState leftEye = resolveEye(meas_left, state.left_state);
    const EyeGazeFrameState rightEye = resolveEye(meas_right, state.right_state);

    if (outTelemetry) {
        outTelemetry->predictorErrorX = predictorErrorX;
        outTelemetry->predictorErrorY = predictorErrorY;
        outTelemetry->dynamicErrorX = dynamicErrorX;
        outTelemetry->dynamicErrorY = dynamicErrorY;
        outTelemetry->predictorTuningLatencyMs = predictorTuningLatencyMs;
        outTelemetry->predictionEnabled = predictionEnabled ? 1 : 0;
        outTelemetry->hysteresisEnabled = hysteresisEnabled ? 1 : 0;
        outTelemetry->dynamicSizingEnabled = dynamicSizingEnabled ? 1 : 0;

        outTelemetry->measuredShiftXLeft = meas_left.x01;
        outTelemetry->measuredShiftYLeft = meas_left.y01;
        outTelemetry->measuredShiftXRight = meas_right.x01;
        outTelemetry->measuredShiftYRight = meas_right.y01;

        outTelemetry->anchorShiftXLeft = Clamp01(To01(leftEye.anchor_x_pm1));
        outTelemetry->anchorShiftYLeft = Clamp01(To01(leftEye.anchor_y_pm1));
        outTelemetry->anchorShiftXRight = Clamp01(To01(rightEye.anchor_x_pm1));
        outTelemetry->anchorShiftYRight = Clamp01(To01(rightEye.anchor_y_pm1));

        outTelemetry->finalShiftXLeft = Clamp01(To01(leftEye.final_x_pm1));
        outTelemetry->finalShiftYLeft = Clamp01(To01(leftEye.final_y_pm1));
        outTelemetry->finalShiftXRight = Clamp01(To01(rightEye.final_x_pm1));
        outTelemetry->finalShiftYRight = Clamp01(To01(rightEye.final_y_pm1));

        outTelemetry->feedbackBiasXLeft = state.left_state.feedback_bias_x;
        outTelemetry->feedbackBiasYLeft = state.left_state.feedback_bias_y;
        outTelemetry->feedbackBiasXRight = state.right_state.feedback_bias_x;
        outTelemetry->feedbackBiasYRight = state.right_state.feedback_bias_y;
    }

    if (predictionEnabled || hysteresisEnabled) {
        overrides.enabled = true;
        overrides.shift_x_left = Clamp01(To01(leftEye.final_x_pm1));
        // Keep runtime center-shift Y in the same convention as the non-prediction path.
        // Win32 shaders already perform their own Y handling.
        overrides.shift_y_left = Clamp01(To01(leftEye.final_y_pm1));
        overrides.shift_x_right = Clamp01(To01(rightEye.final_x_pm1));
        overrides.shift_y_right = Clamp01(To01(rightEye.final_y_pm1));
    }

    if (dynamicSizingEnabled) {
        const float dyn_min_size = std::clamp(Settings::Instance().m_dynMinSize, 0.0f, 1.0f);
        const float dyn_max_size = std::clamp(Settings::Instance().m_dynMaxSize, dyn_min_size, 1.0f);
        const int remainingLatencyMs = (int)std::lround(totalLatencyMs);

        auto updateDynamicState = [&](DynamicFoveaBoxState& dynState, const EyeGazeFrameState& eye) {
            if (!eye.valid) {
                return;
            }

            const float center_x = eye.anchor_x_pm1;
            const float center_y = eye.anchor_y_pm1;
            // Use signed gaze error feedback as the target offset driver so
            // dynamic ROI can expand/shrink from runtime overflow signals.
            const float target_x = ClampSigned01(eye.measured_x_pm1 + dynamicErrorX);
            const float target_y = ClampSigned01(eye.measured_y_pm1 + dynamicErrorY);

            const float error_x = target_x - center_x;
            const float error_y = target_y - center_y;
            const float error_mag = std::sqrt(error_x * error_x + error_y * error_y);

            float conf = 1.0f - error_mag / std::max(2.0f * foveaRadius, 1e-6f);
            const float growRate = std::clamp(Settings::Instance().m_dynGrowRate, 0.0f, 1.0f);
            const float shrinkRate = std::clamp(Settings::Instance().m_dynShrinkRate, 0.0f, 1.0f);

            if (!predictionEnabled) {
                conf = 1.0f - error_mag / kNpConfidenceDivisor;
                conf = std::clamp(conf, kNpConfidenceFloor, 1.0f);
            } else {
                conf = std::clamp(conf, 0.0f, 1.0f);
            }

            dynState = alvr::gaze_prediction::UpdateDynamicFoveaBox(
                dynState,
                center_x,
                center_y,
                target_x,
                target_y,
                conf,
                remainingLatencyMs,
                0.05f,
                dyn_min_size,
                dyn_max_size,
                growRate,
                shrinkRate
            );
        };

        if (!state.dynamic_initialized) {
            const float initSizeX = std::clamp(baseCenterSizeX, dyn_min_size, dyn_max_size);
            const float initSizeY = std::clamp(baseCenterSizeY, dyn_min_size, dyn_max_size);
            state.left_dyn_state = { initSizeX, initSizeY };
            state.right_dyn_state = { initSizeX, initSizeY };
            state.dynamic_initialized = true;
        }

        updateDynamicState(state.left_dyn_state, leftEye);
        updateDynamicState(state.right_dyn_state, rightEye);

        overrides.override_center_size = true;
        overrides.center_size_x = std::clamp(
            (std::max)(state.left_dyn_state.size_x, state.right_dyn_state.size_x),
            dyn_min_size,
            dyn_max_size
        );
        overrides.center_size_y = std::clamp(
            (std::max)(state.left_dyn_state.size_y, state.right_dyn_state.size_y),
            dyn_min_size,
            dyn_max_size
        );
    }

    state.last_predict_timestamp_ns = targetTimestampNs;
    return overrides;
}

FoveationVars BuildFoveationVars(
    uint32_t encoderEyeWidthPx,
    uint32_t encoderEyeHeightPx,
    const GazeFoveationOverrides* overrides
) {
    const float targetEyeWidth = (float)encoderEyeWidthPx;
    const float targetEyeHeight = (float)encoderEyeHeightPx;

    const uint32_t method = Settings::Instance().m_foveationMethod;

    uint32_t optimizedEyeWidthAligned = 0;
    uint32_t optimizedEyeHeightAligned = 0;
    float eyeWidthRatioAligned = 1.0f;
    float eyeHeightRatioAligned = 1.0f;
    ComputeAlignedOptimizedDims(
        encoderEyeWidthPx,
        encoderEyeHeightPx,
        &optimizedEyeWidthAligned,
        &optimizedEyeHeightAligned,
        &eyeWidthRatioAligned,
        &eyeHeightRatioAligned
    );

    // D-FRW uses a tangent warp controlled by magnitude (centerSize), and does not have the
    // piecewise peripheral regions that require edgeRatio-based alignment. Using the AADT-style
    // alignment math here can introduce noticeable misalignment/offset.
    if (Settings::Instance().m_enableFoveatedEncoding && method == 3) {
        const float edgeRatioX = (float)Settings::Instance().m_foveationEdgeRatioX;
        const float edgeRatioY = (float)Settings::Instance().m_foveationEdgeRatioY;

        const float magnitude = (std::max)((float)Settings::Instance().m_foveationFrwMagnitude, 0.01f);

        float centerShiftX = std::clamp((float)Settings::Instance().m_foveationCenterShiftX, 0.0f, 1.0f);
        float centerShiftY = std::clamp((float)Settings::Instance().m_foveationCenterShiftY, 0.0f, 1.0f);
        float centerShiftXLeft = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftXLeft);
        float centerShiftYLeft = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftYLeft);
        float centerShiftXRight = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftXRight);
        float centerShiftYRight = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftYRight);

        if (overrides && overrides->enabled) {
            centerShiftXLeft = Clamp01(overrides->shift_x_left);
            centerShiftYLeft = Clamp01(overrides->shift_y_left);
            centerShiftXRight = Clamp01(overrides->shift_x_right);
            centerShiftYRight = Clamp01(overrides->shift_y_right);
        }

        return { encoderEyeWidthPx,
                 encoderEyeHeightPx,
                 optimizedEyeWidthAligned,
                 optimizedEyeHeightAligned,
                 eyeWidthRatioAligned,
                 eyeHeightRatioAligned,
                 magnitude,
                 magnitude,
                 centerShiftX,
                 centerShiftY,
                 centerShiftXLeft,
                 centerShiftYLeft,
                 centerShiftXRight,
                 centerShiftYRight,
                 edgeRatioX,
                 edgeRatioY };
    }

    float centerSizeX = (float)Settings::Instance().m_foveationCenterSizeX;
    float centerSizeY = (float)Settings::Instance().m_foveationCenterSizeY;
    float centerShiftX = (float)Settings::Instance().m_foveationCenterShiftX;
    float centerShiftY = (float)Settings::Instance().m_foveationCenterShiftY;
    float centerShiftXLeft = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftXLeft);
    float centerShiftYLeft = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftYLeft);
    float centerShiftXRight = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftXRight);
    float centerShiftYRight = SanitizeGazeUvOrCenter((float)Settings::Instance().m_foveationCenterShiftYRight);
    float edgeRatioX = (float)Settings::Instance().m_foveationEdgeRatioX;
    float edgeRatioY = (float)Settings::Instance().m_foveationEdgeRatioY;

    if (overrides) {
        if (overrides->enabled && (method == 1 || method == 2)) {
            centerShiftXLeft = Clamp01(overrides->shift_x_left);
            centerShiftYLeft = Clamp01(overrides->shift_y_left);
            centerShiftXRight = Clamp01(overrides->shift_x_right);
            centerShiftYRight = Clamp01(overrides->shift_y_right);
        }

        if ((method == 1 || method == 2) && overrides->override_center_size) {
            centerSizeX = std::clamp(overrides->center_size_x, 0.05f, 1.0f);
            centerSizeY = std::clamp(overrides->center_size_y, 0.05f, 1.0f);
        }
    }

    const bool useRuntimeRawCenter
        = overrides && (method == 1 || method == 2)
        && (overrides->enabled || overrides->override_center_size);

    if (useRuntimeRawCenter) {
        return { (uint32_t)encoderEyeWidthPx,
                 (uint32_t)encoderEyeHeightPx,
                 optimizedEyeWidthAligned,
                 optimizedEyeHeightAligned,
                 eyeWidthRatioAligned,
                 eyeHeightRatioAligned,
                 std::clamp(centerSizeX, 0.05f, 1.0f),
                 std::clamp(centerSizeY, 0.05f, 1.0f),
                 std::clamp(centerShiftX, 0.0f, 1.0f),
                 std::clamp(centerShiftY, 0.0f, 1.0f),
                 std::clamp(centerShiftXLeft, 0.0f, 1.0f),
                 std::clamp(centerShiftYLeft, 0.0f, 1.0f),
                 std::clamp(centerShiftXRight, 0.0f, 1.0f),
                 std::clamp(centerShiftYRight, 0.0f, 1.0f),
                 edgeRatioX,
                 edgeRatioY };
    }

    float edgeSizeX = targetEyeWidth - centerSizeX * targetEyeWidth;
    float edgeSizeY = targetEyeHeight - centerSizeY * targetEyeHeight;

    float centerSizeXAligned
        = 1. - ceil(edgeSizeX / (edgeRatioX * 2.)) * (edgeRatioX * 2.) / targetEyeWidth;
    float centerSizeYAligned
        = 1. - ceil(edgeSizeY / (edgeRatioY * 2.)) * (edgeRatioY * 2.) / targetEyeHeight;

    float edgeSizeXAligned = targetEyeWidth - centerSizeXAligned * targetEyeWidth;
    float edgeSizeYAligned = targetEyeHeight - centerSizeYAligned * targetEyeHeight;

    float centerShiftXAligned = ceil(centerShiftX * edgeSizeXAligned / (edgeRatioX * 2.))
        * (edgeRatioX * 2.) / edgeSizeXAligned;
    float centerShiftYAligned = ceil(centerShiftY * edgeSizeYAligned / (edgeRatioY * 2.))
        * (edgeRatioY * 2.) / edgeSizeYAligned;

    float centerShiftXAlignedLeft = ceil(centerShiftXLeft * edgeSizeXAligned / (edgeRatioX * 2.))
        * (edgeRatioX * 2.) / edgeSizeXAligned;
    float centerShiftYAlignedLeft = ceil(centerShiftYLeft * edgeSizeYAligned / (edgeRatioY * 2.))
        * (edgeRatioY * 2.) / edgeSizeYAligned;

    float centerShiftXAlignedRight = ceil(centerShiftXRight * edgeSizeXAligned / (edgeRatioX * 2.))
        * (edgeRatioX * 2.) / edgeSizeXAligned;
    float centerShiftYAlignedRight = ceil(centerShiftYRight * edgeSizeYAligned / (edgeRatioY * 2.))
        * (edgeRatioY * 2.) / edgeSizeYAligned;

    return { (uint32_t)encoderEyeWidthPx,
             (uint32_t)encoderEyeHeightPx,
             optimizedEyeWidthAligned,
             optimizedEyeHeightAligned,
             eyeWidthRatioAligned,
             eyeHeightRatioAligned,
             centerSizeXAligned,
             centerSizeYAligned,
             centerShiftXAligned,
             centerShiftYAligned,
             centerShiftXAlignedLeft,
             centerShiftYAlignedLeft,
             centerShiftXAlignedRight,
             centerShiftYAlignedRight,
             edgeRatioX,
             edgeRatioY
            };
}
}

FoveationVars FFR::CalculateFoveationVars(uint64_t targetTimestampNs) {
    const float baseCenterSizeX = (float)Settings::Instance().m_foveationCenterSizeX;
    const float baseCenterSizeY = (float)Settings::Instance().m_foveationCenterSizeY;

    GazeRuntimeState gaze_state {
        mGazeStateLeft,
        mGazeStateRight,
        mDynFoveaLeft,
        mDynFoveaRight,
        mDynFoveaInitialized,
        mLastGazePredictTimestampNs,
    };

    const bool foveatedEncodingEnabled = Settings::Instance().m_enableFoveatedEncoding;
    const uint32_t method = foveatedEncodingEnabled ? Settings::Instance().m_foveationMethod : 0;

    const bool methodSupportsGaze = foveatedEncodingEnabled && (method == 1 || method == 2 || method == 3);
    const bool predictionEnabled = methodSupportsGaze && Settings::Instance().m_enableGazePrediction;
    const bool hysteresisEnabled = methodSupportsGaze && Settings::Instance().m_enableGazeHysteresis;
    const bool dynamicSizingEnabled = foveatedEncodingEnabled && (method == 1 || method == 2)
        && Settings::Instance().m_enableDynamicFovealSizing;

    mLastGazeCsvTelemetry = {};

    const GazeFoveationOverrides overrides = ComputeGazeOverrides(
        targetTimestampNs,
        baseCenterSizeX,
        baseCenterSizeY,
        predictionEnabled,
        hysteresisEnabled,
        dynamicSizingEnabled,
        gaze_state,
        &mLastGazeCsvTelemetry
    );

    return BuildFoveationVars(
        mTargetEyeWidth,
        mTargetEyeHeight,
        (overrides.enabled || overrides.override_center_size) ? &overrides : nullptr
    );
}

void FFR::GetOptimizedResolution(uint32_t* width, uint32_t* height) {
    if (mOptimizedEyeWidthAligned > 0 && mOptimizedEyeHeightAligned > 0) {
        *width = mOptimizedEyeWidthAligned * 2;
        *height = mOptimizedEyeHeightAligned;
        return;
    }

    // Fallback for unexpected call order.
    *width = (uint32_t)Settings::Instance().m_renderWidth;
    *height = (uint32_t)Settings::Instance().m_renderHeight;
}

FFR::FFR(ID3D11Device* device)
    : mDevice(device) {
    // Match the reference Python behavior: start unlocked and let the predictor lock only when
    // confidence is high enough.
    mGazeStateLeft.locked = false;
    mGazeStateRight.locked = false;
}

void FFR::Initialize(
    ID3D11Texture2D* compositionTexture,
    uint32_t renderEyeWidth,
    uint32_t renderEyeHeight,
    uint32_t encoderEyeWidth,
    uint32_t encoderEyeHeight
) {
    // The foveation shader samples from the full-resolution composition texture.
    // renderEye* describes that input resolution.
    // encoderEye* describes the final transmit / encoding eye resolution.
    mRenderEyeWidth = renderEyeWidth;
    mRenderEyeHeight = renderEyeHeight;
    mTargetEyeWidth = encoderEyeWidth;
    mTargetEyeHeight = encoderEyeHeight;

    auto initFovVars = BuildFoveationVars(mTargetEyeWidth, mTargetEyeHeight, nullptr);
    mOptimizedEyeWidthAligned = initFovVars.optimizedEyeWidth;
    mOptimizedEyeHeightAligned = initFovVars.optimizedEyeHeight;

    mDevice->GetImmediateContext(&mImmediateContext);
    mFoveatedRenderingBuffer = CreateBuffer(mDevice.Get(), initFovVars, D3D11_USAGE_DEFAULT);

    std::vector<uint8_t> quadShaderCSO(
        QUAD_SHADER_CSO_PTR, QUAD_SHADER_CSO_PTR + QUAD_SHADER_CSO_LEN
    );
    mQuadVertexShader = CreateVertexShader(mDevice.Get(), quadShaderCSO);

    const bool foveatedEncodingEnabled = Settings::Instance().m_enableFoveatedEncoding;
    const uint32_t method = foveatedEncodingEnabled ? Settings::Instance().m_foveationMethod : 0;

    // Always allocate the output texture when foveated encoding is enabled. The output may differ
    // from the composition size due to encoder alignment requirements.
    if (foveatedEncodingEnabled) {
        mOptimizedTexture = CreateTexture(
            mDevice.Get(),
            mOptimizedEyeWidthAligned * 2,
            mOptimizedEyeHeightAligned,
            Settings::Instance().m_enableHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                             : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        );
    }

    if (!foveatedEncodingEnabled) {
        mOptimizedTexture = compositionTexture;
        return;
    }

    // 0: Vanilla (compressAxisAlignedPipeline)
    // 1: D-AADT2
    // 2: D-AADT3
    // 3: D-FRW
    // 4: GT (GroundTruth/no foveation)
    if (method == 0) {
        // --- Vanilla Shaders ---
        std::vector<uint8_t> compressAxisAlignedShaderCSO(
            COMPRESS_AXIS_ALIGNED_CSO_PTR,
            COMPRESS_AXIS_ALIGNED_CSO_PTR + COMPRESS_AXIS_ALIGNED_CSO_LEN
        );
        auto compressAxisAlignedPipeline = RenderPipeline(mDevice.Get());
        compressAxisAlignedPipeline.Initialize(
            { compositionTexture },
            mQuadVertexShader.Get(),
            compressAxisAlignedShaderCSO,
            mOptimizedTexture.Get(),
            mFoveatedRenderingBuffer.Get()
        );
        mPipelines.push_back(compressAxisAlignedPipeline);
    } else if (method == 1) {
        // --- D-AADT2 Shaders ---
        std::vector<uint8_t> aadt2CSO(AADT2_CSO_PTR, AADT2_CSO_PTR + AADT2_CSO_LEN);
        auto aadt2Pipeline = RenderPipeline(mDevice.Get());
        aadt2Pipeline.Initialize(
            { compositionTexture },
            mQuadVertexShader.Get(),
            aadt2CSO,
            mOptimizedTexture.Get(),
            mFoveatedRenderingBuffer.Get()
        );
        mPipelines.push_back(aadt2Pipeline);
    } else if (method == 2) {
        // --- D-AADT3 Shaders ---
        std::vector<uint8_t> aadt3CSO(AADT3_CSO_PTR, AADT3_CSO_PTR + AADT3_CSO_LEN);
        auto aadt3Pipeline = RenderPipeline(mDevice.Get());
        aadt3Pipeline.Initialize(
            { compositionTexture },
            mQuadVertexShader.Get(),
            aadt3CSO,
            mOptimizedTexture.Get(),
            mFoveatedRenderingBuffer.Get()
        );
        mPipelines.push_back(aadt3Pipeline);
    } else if (method == 3) {
        // --- D-FRW Shaders ---
        std::vector<uint8_t> frwCSO(FRW_CSO_PTR, FRW_CSO_PTR + FRW_CSO_LEN);
        auto frwPipeline = RenderPipeline(mDevice.Get());
        frwPipeline.Initialize(
            { compositionTexture },
            mQuadVertexShader.Get(),
            frwCSO,
            mOptimizedTexture.Get(),
            mFoveatedRenderingBuffer.Get()
        );
        mPipelines.push_back(frwPipeline);
    } else if (method == 4) {
        // ----GroundTruth / No Foveation -----
        // Copy/scale to the encoder-aligned output texture (the aligned size can differ from the
        // composition size even when rendering at encoder resolution).
        std::vector<uint8_t> colorCorrectionShaderCSO(
            COLOR_CORRECTION_CSO_PTR, COLOR_CORRECTION_CSO_PTR + COLOR_CORRECTION_CSO_LEN
        );

        struct CopyParams {
            float renderWidth;
            float renderHeight;
            float brightness;
            float contrast;
            float saturation;
            float gamma;
            float sharpening;
            float _align;
        };

        CopyParams copyParams = {
            (float)(mOptimizedEyeWidthAligned * 2),
            (float)mOptimizedEyeHeightAligned,
            0.0f,
            1.0f,
            1.0f,
            1.0f,
            0.0f,
            0.0f,
        };

        Microsoft::WRL::ComPtr<ID3D11Buffer> copyBuffer
            = CreateBuffer(mDevice.Get(), copyParams, D3D11_USAGE_DEFAULT);

        auto copyPipeline = RenderPipeline(mDevice.Get());
        copyPipeline.Initialize(
            { compositionTexture },
            mQuadVertexShader.Get(),
            colorCorrectionShaderCSO,
            mOptimizedTexture.Get(),
            copyBuffer.Get()
        );
        mPipelines.push_back(copyPipeline);
    }
}

void FFR::Render(uint64_t targetTimestampNs) {
    auto fovVars = CalculateFoveationVars(targetTimestampNs);
    LogGazeRowCsv(fovVars, mLastGazeCsvTelemetry, mRenderEyeWidth, mRenderEyeHeight);

    if (mFoveatedRenderingBuffer) {
        UpdateBuffer(mImmediateContext.Get(), mFoveatedRenderingBuffer.Get(), &fovVars);

        if (ReportFoveationCenterShiftUsed) {
            ReportFoveationCenterShiftUsed(
                targetTimestampNs,
                fovVars.centerShiftXLeft,
                fovVars.centerShiftYLeft,
                fovVars.centerShiftXRight,
                fovVars.centerShiftYRight
            );
        }

        if (ReportFoveationCenterSizeUsed) {
            ReportFoveationCenterSizeUsed(
                targetTimestampNs,
                fovVars.centerSizeX,
                fovVars.centerSizeY
            );
        }
    }
    for (auto& p : mPipelines) {
        p.Render();
    }
}

ID3D11Texture2D* FFR::GetOutputTexture() { return mOptimizedTexture.Get(); }
