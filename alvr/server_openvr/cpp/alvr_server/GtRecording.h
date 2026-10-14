#pragma once

#include <cstdint>
#include <filesystem>

namespace alvr::gt_recording {

// mode: 0=logs-only, 1=lossless video+logs, 2=compressed video+logs
constexpr uint32_t MODE_LOGS_ONLY = 0;
constexpr uint32_t MODE_LOSSLESS_VIDEO = 1;
constexpr uint32_t MODE_COMPRESSED_VIDEO = 2;

void Start(const char* filenameSuffix, uint32_t mode, uint32_t durationMinutes);
void Stop();

// Called frequently (e.g. per-frame) to enforce auto-stop.
void Update();

bool IsActive();
bool IsLogsEnabled();
bool IsVideoEnabled();
bool IsLossless();

// Increments every time Start/Stop is called and when auto-stop triggers.
uint64_t SessionId();

std::filesystem::path LogDir();
std::filesystem::path GetFfrLogFilePath();
std::filesystem::path GetVideoFilePath(bool lossless);

} // namespace alvr::gt_recording
