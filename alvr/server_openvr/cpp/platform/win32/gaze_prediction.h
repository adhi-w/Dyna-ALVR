#pragma once

#include <cstdint>
#include <optional>
#include <tuple>

namespace alvr {
namespace gaze_prediction {

struct GazePredictorParams {
    int lead_ms = 0;
    float alpha = 0.7f;
    float beta = 0.08f;
    float fast_speed = 0.6f;
    float alpha_fast = 0.95f;
    float beta_fast = 0.25f;
};

struct OnlineFeedbackParams {
    float error_gain = 0.20f;
    float decay = 0.90f;
    float max_bias = 0.15f;
    float velocity_damp = 0.10f;
};

struct PredictorKwargs {
    float alpha = 0.7f;
    float beta = 0.08f;
    bool adaptive_gains = true;
    float fast_speed = 0.6f;
    float alpha_fast = 0.95f;
    float beta_fast = 0.25f;
};

struct PredictorConfig {
    int lead_ms = 0;
    PredictorKwargs kwargs;
};

struct DynamicFoveaBoxState {
    // Half-sizes (radii) in predictor space [-1, 1].
    float size_x = 0.2f;
    float size_y = 0.2f;
};

DynamicFoveaBoxState UpdateDynamicFoveaBox(
    const DynamicFoveaBoxState& state,
    float center_x,
    float center_y,
    float target_x,
    float target_y,
    float prediction_confidence,
    int remaining_latency_ms,
    float border_frac = 0.05f,
    float min_size = 0.2f,
    float max_size = 0.7f,
    float grow_rate = 0.20f,
    float shrink_rate = 0.02f
);

GazePredictorParams GetTunedGazeParams(int latency_ms);
PredictorConfig GetPredictorConfig(int latency_ms);

int QuantizeAddedLatencyMs(int added_latency_ms);

float GetPredictScale(int latency_ms, float default_value = 0.0f);
float GetMotionStepThreshold(int latency_ms, float default_value = 0.02f);
float GetMaxPredOffsetMult(int latency_ms, float default_value = 0.25f);

float ClampPm1(float v);

struct GazeState {
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float vel_x = 0.0f;
    float vel_y = 0.0f;

    bool locked = true;

    float lock_anchor_x = 0.0f;
    float lock_anchor_y = 0.0f;
    bool lock_anchor_initialized = false;
    int lock_frames = 0;

    float last_meas_x = 0.0f;
    float last_meas_y = 0.0f;
    bool last_meas_initialized = false;

    float feedback_bias_x = 0.0f;
    float feedback_bias_y = 0.0f;
    float last_feedback_error = 0.0f;
};

struct GazeHysteresisParams {
    float k_enter = 0.85f;
    float k_exit = 1.15f;
    float soft_lock = 0.7f;
    float hysteresis_motion_scale = 0.5f;
    int max_lock_frames = 5;
    float speed_unlock_factor = 2.0f;
};

struct GazeHysteresisKwargs {
    float k_enter = 0.85f;
    float k_exit = 1.15f;
    float soft_lock = 0.7f;
    float speed_unlock = 2.0f;
    float hysteresis_motion_scale = 0.5f;
    int max_lock_frames = 5;
};

GazeHysteresisKwargs GetHysteresisKwargs(
    const PredictorKwargs& predictor_kwargs,
    const GazeHysteresisParams* params = nullptr
);

struct GazePredictionInfo {
    float radius = 0.0f;
    float meas_step = 0.0f;
    float speed = 0.0f;

    bool locked = false;
    int lock_frames = 0;
    int max_lock_frames = 0;
    bool use_hysteresis = true;

    float k_enter_eff = 0.0f;
    float k_exit_eff = 0.0f;
    float R_enter = 0.0f;
    float R_exit = 0.0f;

    float alpha = 0.0f;
    float beta = 0.0f;
    bool predict_ahead = true;

    // Added by PredictGazeGated().
    bool gated = false;
    float horizon_s = 0.0f;
    float feedback_bias_x = 0.0f;
    float feedback_bias_y = 0.0f;
    int tuning_latency_ms = 0;
};

struct PredictionFeedbackResult {
    float feedback_bias_x = 0.0f;
    float feedback_bias_y = 0.0f;
    float feedback_error = 0.0f;
    float velocity_damp = 0.0f;
};

struct ApplyGazeHysteresisResult {
    float out_x = 0.0f;
    float out_y = 0.0f;
    GazePredictionInfo info;
};

struct UpdateGazePredictionResult {
    float out_x = 0.0f;
    float out_y = 0.0f;
    GazePredictionInfo info;
};

UpdateGazePredictionResult UpdateGazePrediction(
    float meas_x,
    float meas_y,
    GazeState& state,
    float dt,
    float fovea_radius,
    float alpha = 1.0f,
    float beta = 0.0f,
    float k_enter = 0.85f,
    float k_exit = 1.15f,
    float soft_lock = 0.7f,
    float speed_unlock = 2.0f,
    bool adaptive_gains = true,
    float fast_speed = 0.6f,
    float alpha_fast = 0.95f,
    float beta_fast = 0.25f,
    bool use_hysteresis = true,
    float hysteresis_motion_scale = 0.05f,
    int max_lock_frames = 5,
    bool predict_ahead = true
);

std::tuple<float, float> ApplyLeadCompensation(
    float pred_x,
    float pred_y,
    const GazeState& state,
    int lead_ms
);

PredictionFeedbackResult ApplyPredictionErrorFeedback(
    GazeState& state,
    float error_x,
    float error_y,
    const OnlineFeedbackParams* params = nullptr
);

ApplyGazeHysteresisResult ApplyGazeHysteresis(
    float candidate_x,
    float candidate_y,
    GazeState& state,
    float fovea_radius,
    float meas_step,
    float speed,
    float k_enter = 0.85f,
    float k_exit = 1.15f,
    float soft_lock = 0.7f,
    float speed_unlock = 2.0f,
    float hysteresis_motion_scale = 0.5f,
    int max_lock_frames = 5
);

struct PredictGazeGatedResult {
    float pred_x = 0.0f;
    float pred_y = 0.0f;
    GazePredictionInfo info;
};

PredictGazeGatedResult PredictGazeGated(
    float meas_x,
    float meas_y,
    GazeState& state,
    float dt,
    int latency_ms,
    float fovea_radius,
    std::optional<float> predict_scale = std::nullopt,
    std::optional<float> motion_step_thresh = std::nullopt,
    std::optional<float> max_pred_offset_mult = std::nullopt,
    std::optional<int> lead_ms = std::nullopt,
    std::optional<int> tuning_latency_ms = std::nullopt,
    float vel_smooth = 1.0f,
    bool use_hysteresis = false,
    float k_enter = 0.85f,
    float k_exit = 1.15f,
    float soft_lock = 0.7f,
    float speed_unlock = 2.0f,
    float hysteresis_motion_scale = 0.5f,
    int max_lock_frames = 5
);

} // namespace gaze_prediction
} // namespace alvr
