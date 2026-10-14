#include "gaze_prediction.h"

#include <algorithm>
#include <cmath>

namespace alvr {
namespace gaze_prediction {

namespace {

inline float ClampFloat(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

inline bool IsFinite(float v) {
    return (std::isfinite)(v);
}

inline std::tuple<float, float> ClampDelta(float dx, float dy, float max_len) {
    if (max_len <= 0.0f) {
        return { dx, dy };
    }

    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= max_len || d <= 1e-12f) {
        return { dx, dy };
    }

    const float s = max_len / d;
    return { dx * s, dy * s };
}

inline float ClampSigned(float v, float max_abs) {
    max_abs = std::max(0.0f, max_abs);
    return ClampFloat(v, -max_abs, max_abs);
}

struct HysteresisStateResult {
    float out_x = 0.0f;
    float out_y = 0.0f;
    float radius = 0.0f;
    float k_enter_eff = 0.0f;
    float k_exit_eff = 0.0f;
    float R_enter = 0.0f;
    float R_exit = 0.0f;
};

HysteresisStateResult ApplyHysteresisState(
    float candidate_x,
    float candidate_y,
    GazeState& state,
    float fovea_radius,
    float meas_step,
    float speed,
    float k_enter,
    float k_exit,
    float soft_lock,
    float speed_unlock,
    float hysteresis_motion_scale,
    int max_lock_frames
) {
    float k_enter_eff = k_enter;
    float k_exit_eff = k_exit;

    const float scale = std::max(hysteresis_motion_scale, 1e-9f);
    const float motion_norm = ClampFloat(meas_step / scale, 0.0f, 1.0f);
    const float stability = 1.0f - motion_norm;
    const float factor = 0.7f + 0.6f * stability;
    k_enter_eff = k_enter * factor;
    k_exit_eff = k_exit * factor;

    const float fovea_radius_abs = std::abs(fovea_radius);
    const float R_enter = k_enter_eff * fovea_radius_abs;
    const float R_exit = k_exit_eff * fovea_radius_abs;

    if (!state.lock_anchor_initialized) {
        state.lock_anchor_x = candidate_x;
        state.lock_anchor_y = candidate_y;
        state.lock_anchor_initialized = true;
    }

    const bool prev_locked = state.locked;
    const float rx = candidate_x - state.lock_anchor_x;
    const float ry = candidate_y - state.lock_anchor_y;
    const float radius = std::sqrt(rx * rx + ry * ry);

    if (state.locked && radius > R_exit) {
        state.locked = false;
    } else if (!state.locked && radius < R_enter && speed < speed_unlock) {
        state.locked = true;
    } else if (state.locked && speed > speed_unlock && radius > R_enter) {
        state.locked = false;
    }

    if (state.locked) {
        if (!prev_locked) {
            state.lock_frames = 1;
        } else {
            state.lock_frames += 1;
        }

        if (max_lock_frames > 0 && state.lock_frames > max_lock_frames) {
            state.locked = false;
            state.lock_frames = 0;
        }
    } else {
        state.lock_frames = 0;
    }

    if (state.locked) {
        state.vel_x *= 0.5f;
        state.vel_y *= 0.5f;
    }

    float out_x = candidate_x;
    float out_y = candidate_y;

    if (state.locked) {
        out_x = (1.0f - soft_lock) * out_x + soft_lock * state.lock_anchor_x;
        out_y = (1.0f - soft_lock) * out_y + soft_lock * state.lock_anchor_y;
    } else {
        state.lock_anchor_x = candidate_x;
        state.lock_anchor_y = candidate_y;
        state.lock_anchor_initialized = true;
    }

    return { out_x, out_y, radius, k_enter_eff, k_exit_eff, R_enter, R_exit };
}

} // namespace

DynamicFoveaBoxState UpdateDynamicFoveaBox(
    const DynamicFoveaBoxState& state,
    float center_x,
    float center_y,
    float target_x,
    float target_y,
    float prediction_confidence,
    int remaining_latency_ms,
    float border_frac,
    float min_size,
    float max_size,
    float grow_rate,
    float shrink_rate
) {
    const float conf = ClampFloat(prediction_confidence, 0.0f, 1.0f);
    const float lat_norm = ClampFloat((float)std::max(0, remaining_latency_ms) / 120.0f, 0.0f, 1.0f);

    border_frac = ClampFloat(border_frac, 0.0f, 0.25f);
    if (max_size < min_size) {
        std::swap(min_size, max_size);
    }

    float size_x = state.size_x;
    float size_y = state.size_y;

    if (!IsFinite(size_x)) {
        size_x = min_size;
    }
    if (!IsFinite(size_y)) {
        size_y = min_size;
    }

    size_x = ClampFloat(size_x, min_size, max_size);
    size_y = ClampFloat(size_y, min_size, max_size);

    const float dx = std::abs(target_x - center_x);
    const float dy = std::abs(target_y - center_y);

    const bool near_x = dx >= (1.0f - border_frac) * size_x;
    const bool near_y = dy >= (1.0f - border_frac) * size_y;

    float expand_strength = (1.0f - conf) * (0.25f + 0.75f * lat_norm);
    expand_strength = ClampFloat(expand_strength, 0.0f, 1.0f);

    const bool inside_x = dx <= (1.0f - 3.0f * border_frac) * size_x;
    const bool inside_y = dy <= (1.0f - 3.0f * border_frac) * size_y;
    const float shrink_strength = conf;

    const float denom = std::max(1e-9f, 1.0f - border_frac);

    float margin = 1.0f + 0.15f * (1.0f - conf) + 0.15f * lat_norm;
    margin = ClampFloat(margin, 1.0f, 1.5f);

    if (near_x) {
        const float required_x = dx / denom;
        float desired_x = std::max(size_x, required_x * margin);
        desired_x = std::max(desired_x, size_x * (1.0f + grow_rate * expand_strength));
        size_x = desired_x;
    } else if (inside_x) {
        const float shrink_lat = 1.0f - 0.75f * lat_norm;
        size_x = size_x - shrink_rate * shrink_strength * shrink_lat * (size_x - min_size);
    }

    if (near_y) {
        const float required_y = dy / denom;
        float desired_y = std::max(size_y, required_y * margin);
        desired_y = std::max(desired_y, size_y * (1.0f + grow_rate * expand_strength));
        size_y = desired_y;
    } else if (inside_y) {
        const float shrink_lat = 1.0f - 0.75f * lat_norm;
        size_y = size_y - shrink_rate * shrink_strength * shrink_lat * (size_y - min_size);
    }

    size_x = ClampFloat(size_x, min_size, max_size);
    size_y = ClampFloat(size_y, min_size, max_size);

    return { size_x, size_y };
}

GazePredictorParams GetTunedGazeParams(int latency_ms) {
    switch (QuantizeAddedLatencyMs(latency_ms)) {
    case 30:
        return { 0, 0.78f, 0.07f, 0.75f, 0.92f, 0.14f };
    case 60:
        return { 0, 0.75f, 0.06f, 0.82f, 0.90f, 0.13f };
    case 90:
        // High-latency profile: prioritize stability over aggressiveness.
        return { 0, 0.72f, 0.05f, 0.9f, 0.88f, 0.12f };
    default:
        return { 0, 0.7f, 0.08f, 0.6f, 0.95f, 0.25f };
    }
}

int QuantizeAddedLatencyMs(int added_latency_ms) {
    // Keep the tuning buckets aligned with the Python reference implementation.
    // The selected bucket is the nearest supported added-latency value.
    const int buckets[] = { 30, 60, 90 };
    int best_bucket = buckets[0];
    int best_distance = std::abs(added_latency_ms - best_bucket);

    for (int bucket : buckets) {
        const int distance = std::abs(added_latency_ms - bucket);
        if (distance < best_distance) {
            best_distance = distance;
            best_bucket = bucket;
        }
    }

    return best_bucket;
}

PredictorConfig GetPredictorConfig(int latency_ms) {
    const auto p = GetTunedGazeParams(latency_ms);
    PredictorConfig cfg;
    cfg.lead_ms = p.lead_ms;
    cfg.kwargs.alpha = p.alpha;
    cfg.kwargs.beta = p.beta;
    cfg.kwargs.adaptive_gains = true;
    cfg.kwargs.fast_speed = p.fast_speed;
    cfg.kwargs.alpha_fast = p.alpha_fast;
    cfg.kwargs.beta_fast = p.beta_fast;
    return cfg;
}

float GetPredictScale(int latency_ms, float default_value) {
    switch (QuantizeAddedLatencyMs(latency_ms)) {
    case 30:
        return 0.070f;
    case 60:
        return 0.055f;
    case 90:
        return 0.04f;
    default:
        return default_value;
    }
}

float GetMotionStepThreshold(int latency_ms, float default_value) {
    switch (QuantizeAddedLatencyMs(latency_ms)) {
    case 30:
        return 0.070f;
    case 60:
        return 0.055f;
    case 90:
        return 0.04f;
    default:
        return default_value;
    }
}

float GetMaxPredOffsetMult(int latency_ms, float default_value) {
    switch (QuantizeAddedLatencyMs(latency_ms)) {
    case 30:
        return 0.15f;
    case 60:
        return 0.11f;
    case 90:
        return 0.08f;
    default:
        return default_value;
    }
}

float ClampPm1(float v) {
    return ClampFloat(v, -1.0f, 1.0f);
}

GazeHysteresisKwargs GetHysteresisKwargs(
    const PredictorKwargs& predictor_kwargs,
    const GazeHysteresisParams* params
) {
    GazeHysteresisParams use_params;
    if (params) {
        use_params = *params;
    }

    const float tuned_fast_speed = predictor_kwargs.fast_speed;
    const float speed_unlock = use_params.speed_unlock_factor * tuned_fast_speed;

    GazeHysteresisKwargs out;
    out.k_enter = use_params.k_enter;
    out.k_exit = use_params.k_exit;
    out.soft_lock = use_params.soft_lock;
    out.speed_unlock = speed_unlock;
    out.hysteresis_motion_scale = use_params.hysteresis_motion_scale;
    out.max_lock_frames = use_params.max_lock_frames;
    return out;
}

UpdateGazePredictionResult UpdateGazePrediction(
    float meas_x,
    float meas_y,
    GazeState& state,
    float dt,
    float fovea_radius,
    float alpha,
    float beta,
    float k_enter,
    float k_exit,
    float soft_lock,
    float speed_unlock,
    bool adaptive_gains,
    float fast_speed,
    float alpha_fast,
    float beta_fast,
    bool use_hysteresis,
    float hysteresis_motion_scale,
    int max_lock_frames,
    bool predict_ahead
) {
    if (dt <= 0.0f) {
        dt = 1e-6f;
    }

    // 1) Alpha-beta predict (constant-velocity)
    const float pred_x = state.pos_x + state.vel_x * dt;
    const float pred_y = state.pos_y + state.vel_y * dt;

    const float pred_vel_x = state.vel_x;
    const float pred_vel_y = state.vel_y;

    const float res_x = meas_x - pred_x;
    const float res_y = meas_y - pred_y;

    const float speed_pred = std::sqrt(pred_vel_x * pred_vel_x + pred_vel_y * pred_vel_y);

    float use_alpha = alpha;
    float use_beta = beta;
    if (adaptive_gains && speed_pred > fast_speed) {
        use_alpha = alpha_fast;
        use_beta = beta_fast;
    }

    state.pos_x = pred_x + use_alpha * res_x;
    state.pos_y = pred_y + use_alpha * res_y;

    state.vel_x = pred_vel_x + (use_beta / dt) * res_x;
    state.vel_y = pred_vel_y + (use_beta / dt) * res_y;

    // 2) Speed estimation
    const float speed = std::sqrt(state.vel_x * state.vel_x + state.vel_y * state.vel_y);

    float meas_step = 0.0f;
    if (!state.last_meas_initialized) {
        meas_step = 0.0f;
        state.last_meas_x = meas_x;
        state.last_meas_y = meas_y;
        state.last_meas_initialized = true;
    } else {
        const float dx = meas_x - state.last_meas_x;
        const float dy = meas_y - state.last_meas_y;
        meas_step = std::sqrt(dx * dx + dy * dy);
        state.last_meas_x = meas_x;
        state.last_meas_y = meas_y;
    }

    // 3) Final output candidate
    float out_x = 0.0f;
    float out_y = 0.0f;

    if (predict_ahead) {
        out_x = state.pos_x + state.vel_x * dt;
        out_y = state.pos_y + state.vel_y * dt;
    } else {
        out_x = state.pos_x;
        out_y = state.pos_y;
    }

    float r_meas = 0.0f;
    float k_enter_eff = k_enter;
    float k_exit_eff = k_exit;
    float R_enter = k_enter * std::abs(fovea_radius);
    float R_exit = k_exit * std::abs(fovea_radius);

    if (use_hysteresis) {
        const auto hysteresis = ApplyHysteresisState(
            out_x,
            out_y,
            state,
            fovea_radius,
            meas_step,
            speed,
            k_enter,
            k_exit,
            soft_lock,
            speed_unlock,
            hysteresis_motion_scale,
            max_lock_frames
        );
        out_x = hysteresis.out_x;
        out_y = hysteresis.out_y;
        r_meas = hysteresis.radius;
        k_enter_eff = hysteresis.k_enter_eff;
        k_exit_eff = hysteresis.k_exit_eff;
        R_enter = hysteresis.R_enter;
        R_exit = hysteresis.R_exit;
    }

    UpdateGazePredictionResult result;
    result.out_x = out_x;
    result.out_y = out_y;

    result.info.radius = r_meas;
    result.info.meas_step = meas_step;
    result.info.speed = speed;

    result.info.locked = state.locked;
    result.info.lock_frames = state.lock_frames;
    result.info.max_lock_frames = max_lock_frames;
    result.info.use_hysteresis = use_hysteresis;

    result.info.k_enter_eff = k_enter_eff;
    result.info.k_exit_eff = k_exit_eff;
    result.info.R_enter = R_enter;
    result.info.R_exit = R_exit;

    result.info.alpha = use_alpha;
    result.info.beta = use_beta;
    result.info.predict_ahead = predict_ahead;

    return result;
}

std::tuple<float, float> ApplyLeadCompensation(
    float pred_x,
    float pred_y,
    const GazeState& state,
    int lead_ms
) {
    if (lead_ms <= 0) {
        return { pred_x, pred_y };
    }

    const float lead_s = (float)lead_ms / 1000.0f;
    return { pred_x + state.vel_x * lead_s, pred_y + state.vel_y * lead_s };
}

PredictionFeedbackResult ApplyPredictionErrorFeedback(
    GazeState& state,
    float error_x,
    float error_y,
    const OnlineFeedbackParams* params
) {
    OnlineFeedbackParams use_params;
    if (params) {
        use_params = *params;
    }

    const float gain = ClampFloat(use_params.error_gain, 0.0f, 1.0f);
    const float decay = ClampFloat(use_params.decay, 0.0f, 1.0f);
    const float max_bias = std::max(0.0f, use_params.max_bias);
    const float velocity_damp = ClampFloat(use_params.velocity_damp, 0.0f, 1.0f);

    const float err_mag = std::sqrt(error_x * error_x + error_y * error_y);

    state.feedback_bias_x = ClampSigned(decay * state.feedback_bias_x + gain * error_x, max_bias);
    state.feedback_bias_y = ClampSigned(decay * state.feedback_bias_y + gain * error_y, max_bias);
    state.last_feedback_error = err_mag;

    const float damp = ClampFloat(
        velocity_damp * (err_mag / std::max(max_bias, 1e-6f)),
        0.0f,
        1.0f
    );
    state.vel_x *= 1.0f - damp;
    state.vel_y *= 1.0f - damp;

    return { state.feedback_bias_x, state.feedback_bias_y, err_mag, damp };
}

ApplyGazeHysteresisResult ApplyGazeHysteresis(
    float candidate_x,
    float candidate_y,
    GazeState& state,
    float fovea_radius,
    float meas_step,
    float speed,
    float k_enter,
    float k_exit,
    float soft_lock,
    float speed_unlock,
    float hysteresis_motion_scale,
    int max_lock_frames
) {
    ApplyGazeHysteresisResult result;
    const auto hysteresis = ApplyHysteresisState(
        candidate_x,
        candidate_y,
        state,
        fovea_radius,
        meas_step,
        speed,
        k_enter,
        k_exit,
        soft_lock,
        speed_unlock,
        hysteresis_motion_scale,
        max_lock_frames
    );

    result.out_x = hysteresis.out_x;
    result.out_y = hysteresis.out_y;
    result.info.radius = hysteresis.radius;
    result.info.locked = state.locked;
    result.info.lock_frames = state.lock_frames;
    result.info.max_lock_frames = max_lock_frames;
    result.info.use_hysteresis = true;
    result.info.k_enter_eff = hysteresis.k_enter_eff;
    result.info.k_exit_eff = hysteresis.k_exit_eff;
    result.info.R_enter = hysteresis.R_enter;
    result.info.R_exit = hysteresis.R_exit;
    result.info.meas_step = meas_step;
    result.info.speed = speed;

    return result;
}

PredictGazeGatedResult PredictGazeGated(
    float meas_x,
    float meas_y,
    GazeState& state,
    float dt,
    int latency_ms,
    float fovea_radius,
    std::optional<float> predict_scale,
    std::optional<float> motion_step_thresh,
    std::optional<float> max_pred_offset_mult,
    std::optional<int> lead_ms,
    std::optional<int> tuning_latency_ms,
    float vel_smooth,
    bool use_hysteresis,
    float k_enter,
    float k_exit,
    float soft_lock,
    float speed_unlock,
    float hysteresis_motion_scale,
    int max_lock_frames
) {
    if (dt <= 0.0f) {
        dt = 1e-6f;
    }

    const int resolved_tuning_latency_ms
        = QuantizeAddedLatencyMs(tuning_latency_ms.value_or(latency_ms));
    const auto predictor_cfg = GetPredictorConfig(resolved_tuning_latency_ms);

    // Use per-latency defaults unless explicitly overridden.
    if (!lead_ms.has_value()) {
        lead_ms = predictor_cfg.lead_ms;
    }
    if (!predict_scale.has_value()) {
        predict_scale = GetPredictScale(resolved_tuning_latency_ms, 0.10f);
    }
    if (!motion_step_thresh.has_value()) {
        motion_step_thresh = GetMotionStepThreshold(resolved_tuning_latency_ms, 0.02f);
    }
    if (!max_pred_offset_mult.has_value()) {
        max_pred_offset_mult = GetMaxPredOffsetMult(resolved_tuning_latency_ms, 0.25f);
    }

    // Update velocity estimate from measurement differences (online; uses state.last_meas_*).
    if (state.last_meas_initialized && vel_smooth > 0.0f) {
        const float mdx = meas_x - state.last_meas_x;
        const float mdy = meas_y - state.last_meas_y;

        const float v_meas_x = mdx / dt;
        const float v_meas_y = mdy / dt;

        const float a = ClampFloat(vel_smooth, 0.0f, 1.0f);
        state.vel_x = (1.0f - a) * state.vel_x + a * v_meas_x;
        state.vel_y = (1.0f - a) * state.vel_y + a * v_meas_y;
    }

    const bool was_locked = use_hysteresis && state.locked;

    auto base_result = UpdateGazePrediction(
        meas_x,
        meas_y,
        state,
        dt,
        fovea_radius,
        predictor_cfg.kwargs.alpha,
        predictor_cfg.kwargs.beta,
        k_enter,
        k_exit,
        soft_lock,
        speed_unlock,
        predictor_cfg.kwargs.adaptive_gains,
        predictor_cfg.kwargs.fast_speed,
        predictor_cfg.kwargs.alpha_fast,
        predictor_cfg.kwargs.beta_fast,
        /*use_hysteresis*/ false,
        hysteresis_motion_scale,
        max_lock_frames,
        /*predict_ahead*/ false
    );

    const float base_x = base_result.out_x;
    const float base_y = base_result.out_y;

    const float ps = std::max(0.0f, predict_scale.value());
    float horizon_s = 0.0f;
    if (ps > 0.0f) {
        horizon_s = ((float)latency_ms / 1000.0f) * ps
            + ((float)std::max(0, lead_ms.value()) / 1000.0f);
    }

    bool gated = false;
    if (horizon_s <= 0.0f) {
        gated = true;
    }
    if (base_result.info.meas_step > motion_step_thresh.value()) {
        gated = true;
    }
    if (was_locked) {
        gated = true;
    }

    PredictGazeGatedResult out;

    if (gated) {
        out.pred_x = ClampPm1(base_x + state.feedback_bias_x);
        out.pred_y = ClampPm1(base_y + state.feedback_bias_y);
        out.info = base_result.info;
        out.info.gated = true;
        out.info.horizon_s = horizon_s;
        out.info.feedback_bias_x = state.feedback_bias_x;
        out.info.feedback_bias_y = state.feedback_bias_y;
        out.info.tuning_latency_ms = resolved_tuning_latency_ms;

        if (use_hysteresis) {
            const auto hysteresis = ApplyHysteresisState(
                out.pred_x,
                out.pred_y,
                state,
                fovea_radius,
                base_result.info.meas_step,
                base_result.info.speed,
                k_enter,
                k_exit,
                soft_lock,
                speed_unlock,
                hysteresis_motion_scale,
                max_lock_frames
            );
            out.pred_x = hysteresis.out_x;
            out.pred_y = hysteresis.out_y;
            out.info.radius = hysteresis.radius;
            out.info.locked = state.locked;
            out.info.lock_frames = state.lock_frames;
            out.info.max_lock_frames = max_lock_frames;
            out.info.use_hysteresis = true;
            out.info.k_enter_eff = hysteresis.k_enter_eff;
            out.info.k_exit_eff = hysteresis.k_exit_eff;
            out.info.R_enter = hysteresis.R_enter;
            out.info.R_exit = hysteresis.R_exit;
        }

        return out;
    }

    float pred_x = base_x + state.vel_x * horizon_s;
    float pred_y = base_y + state.vel_y * horizon_s;

    // Clamp extrapolation length relative to fovea radius to prevent large overshoots.
    const float max_off = max_pred_offset_mult.value() * std::abs(fovea_radius);
    const float dx = pred_x - base_x;
    const float dy = pred_y - base_y;

    float clamped_dx = dx;
    float clamped_dy = dy;
    std::tie(clamped_dx, clamped_dy) = ClampDelta(dx, dy, max_off);

    pred_x = base_x + clamped_dx;
    pred_y = base_y + clamped_dy;

    pred_x += state.feedback_bias_x;
    pred_y += state.feedback_bias_y;

    pred_x = ClampPm1(pred_x);
    pred_y = ClampPm1(pred_y);

    out.pred_x = pred_x;
    out.pred_y = pred_y;
    out.info = base_result.info;
    out.info.gated = false;
    out.info.horizon_s = horizon_s;
    out.info.feedback_bias_x = state.feedback_bias_x;
    out.info.feedback_bias_y = state.feedback_bias_y;
    out.info.tuning_latency_ms = resolved_tuning_latency_ms;

    if (use_hysteresis) {
        const auto hysteresis = ApplyHysteresisState(
            out.pred_x,
            out.pred_y,
            state,
            fovea_radius,
            base_result.info.meas_step,
            base_result.info.speed,
            k_enter,
            k_exit,
            soft_lock,
            speed_unlock,
            hysteresis_motion_scale,
            max_lock_frames
        );
        out.pred_x = hysteresis.out_x;
        out.pred_y = hysteresis.out_y;
        out.info.radius = hysteresis.radius;
        out.info.locked = state.locked;
        out.info.lock_frames = state.lock_frames;
        out.info.max_lock_frames = max_lock_frames;
        out.info.use_hysteresis = true;
        out.info.k_enter_eff = hysteresis.k_enter_eff;
        out.info.k_exit_eff = hysteresis.k_exit_eff;
        out.info.R_enter = hysteresis.R_enter;
        out.info.R_exit = hysteresis.R_exit;
    }

    return out;
}

} // namespace gaze_prediction
} // namespace alvr
