#pragma once

#include "ALVR-common/packet_types.h"
#include <string>

class Settings {
    static Settings m_Instance;
    bool m_loaded;

    Settings();
    virtual ~Settings();

public:
    void Load();
    static Settings& Instance() { return m_Instance; }

    bool IsLoaded() { return m_loaded; }

    int m_refreshRate;
    uint32_t m_renderWidth;
    uint32_t m_renderHeight;
    int32_t m_recommendedTargetWidth;
    int32_t m_recommendedTargetHeight;
    int32_t m_nAdapterIndex;
    std::string m_captureFrameDir;

    bool m_enableFoveatedEncoding;
    // 0: Vanilla, 1: D-AADT2, 2: D-AADT3, 3: D-FRW
    uint32_t m_foveationMethod;
    // Fine-grained toggles for dynamic ROI features.
    bool m_enableGazePrediction;
    bool m_enableGazeHysteresis;
    bool m_enableDynamicFovealSizing;
    // Dynamic foveal sizing parameters (normalized [0,1] sizes; [0,1] rates).
    float m_dynMinSize = 0.2f;
    float m_dynMaxSize = 0.6f;
    float m_dynGrowRate = 0.20f;
    float m_dynShrinkRate = 0.08f;
    float m_foveationCenterSizeX;
    float m_foveationCenterSizeY;
    float m_foveationCenterShiftX;
    float m_foveationCenterShiftY;
    // Per-eye overrides (normalized [0,1], x:right, y:down). Defaults to the generic values.
    float m_foveationCenterShiftXLeft;
    float m_foveationCenterShiftYLeft;
    float m_foveationCenterShiftXRight;
    float m_foveationCenterShiftYRight;
    float m_foveationEdgeRatioX;
    float m_foveationEdgeRatioY;
    // Used only for method 3 (D-FRW)
    float m_foveationFrwMagnitude;

    bool m_enableColorCorrection;
    float m_brightness;
    float m_contrast;
    float m_saturation;
    float m_gamma;
    float m_sharpening;

    int m_codec;
    int m_h264Profile;
    bool m_use10bitEncoder;
    double m_encodingGamma;
    bool m_enableHdr;
    bool m_forceHdrSrgbCorrection;
    bool m_clampHdrExtendedRange;
    bool m_enableAmfPreAnalysis;
    bool m_enableVbaq;
    bool m_enableAmfHmqb;
    bool m_useAmfPreproc;
    uint32_t m_amfPreProcSigma;
    uint32_t m_amfPreProcTor;
    uint32_t m_encoderQualityPreset;
    bool m_amdBitrateCorruptionFix;
    uint32_t m_nvencQualityPreset;
    uint32_t m_rateControlMode;
    bool m_fillerData;
    uint32_t m_entropyCoding;
    bool m_force_sw_encoding;
    uint32_t m_swThreadCount;

    uint32_t m_nvencTuningPreset;
    uint32_t m_nvencMultiPass;
    uint32_t m_nvencAdaptiveQuantizationMode;
    int64_t m_nvencLowDelayKeyFrameScale;
    int64_t m_nvencRefreshRate;
    bool m_nvencEnableIntraRefresh;
    int64_t m_nvencIntraRefreshPeriod;
    int64_t m_nvencIntraRefreshCount;
    int64_t m_nvencMaxNumRefFrames;
    int64_t m_nvencGopLength;
    int64_t m_nvencPFrameStrategy;
    int64_t m_nvencRateControlMode;
    int64_t m_nvencRcBufferSize;
    int64_t m_nvencRcInitialDelay;
    int64_t m_nvencRcMaxBitrate;
    int64_t m_nvencRcAverageBitrate;
    bool m_nvencEnableWeightedPrediction;

    uint64_t m_minimumIdrIntervalMs;

    bool m_enableViveTrackerProxy = false;
    bool m_TrackingRefOnly = false;
    bool m_enableLinuxVulkanAsyncCompute;
    bool m_enableLinuxAsyncReprojection;

    bool m_enableControllers;
    int m_controllerIsTracker = false;
    int m_enableBodyTrackingFakeVive = false;
    int m_bodyTrackingHasLegs = false;
    bool m_useSeparateHandTrackers = false;
};
