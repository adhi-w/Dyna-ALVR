#include "GtRecording.h"

#include "bindings.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <mutex>
#include <string>

namespace alvr::gt_recording {
namespace {
namespace FSys = std::filesystem;

struct State {
    bool active = false;
    bool logsEnabled = false;
    bool videoEnabled = false;
    bool lossless = false;

    uint32_t durationMinutes = 1;
    uint32_t mode = MODE_LOGS_ONLY;

    std::string filenameSuffix;

    std::chrono::steady_clock::time_point endTime{};

    uint64_t sessionId = 0;
};

static std::mutex g_mutex;
static State g_state;

static FSys::path FindRepoRootFrom(const FSys::path& start) {
    // In development builds, the streamer is typically under <repo>/build/<...>.
    // Prefer writing to <repo>/logFiles when we can reliably detect the repo root.
    std::error_code ec;
    FSys::path current = start;
    for (int i = 0; i < 8 && !current.empty(); i++) {
        const bool hasCargoToml = FSys::exists(current / "Cargo.toml", ec);
        const bool hasAlvrDir = FSys::exists(current / "alvr", ec);
        if (hasCargoToml && hasAlvrDir) {
            return current;
        }
        auto parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }

    return {};
}

static FSys::path RootPath() {
    // Prefer the filesystem layout root passed from Rust.
    if (g_driverRootDir && g_driverRootDir[0] != '\0') {
        const auto driverRoot = FSys::path(g_driverRootDir);
        if (const auto repoRoot = FindRepoRootFrom(driverRoot); !repoRoot.empty()) {
            return repoRoot;
        }
        return driverRoot;
    }
    // Fallback: derive from session path if available.
    if (g_sessionPath && g_sessionPath[0] != '\0') {
        const auto base = FSys::path(g_sessionPath).parent_path();
        if (const auto repoRoot = FindRepoRootFrom(base); !repoRoot.empty()) {
            return repoRoot;
        }
        return base;
    }

    const auto base = FSys::current_path();
    if (const auto repoRoot = FindRepoRootFrom(base); !repoRoot.empty()) {
        return repoRoot;
    }
    return base;
}

static FSys::path LogPath() { return RootPath() / "logFiles"; }

static std::string SanitizeSuffix(std::string suffix) {
    // Trim whitespace
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    suffix.erase(suffix.begin(), std::find_if(suffix.begin(), suffix.end(), notSpace));
    suffix.erase(std::find_if(suffix.rbegin(), suffix.rend(), notSpace).base(), suffix.end());

    if (suffix.size() > 64) {
        suffix.resize(64);
    }

    for (char& c : suffix) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            c = '_';
        }
    }

    // Avoid leading dots (hidden files)
    while (!suffix.empty() && suffix.front() == '.') {
        suffix.erase(suffix.begin());
    }

    return suffix;
}

static FSys::path WithSuffix(const FSys::path& baseNoExt, const char* extensionWithDot, const std::string& suffix) {
    if (suffix.empty()) {
        return FSys::path(baseNoExt.string() + extensionWithDot);
    }

    return FSys::path(baseNoExt.string() + "_" + suffix + extensionWithDot);
}

static void StopLocked() {
    g_state.active = false;
    g_state.logsEnabled = false;
    g_state.videoEnabled = false;
    g_state.lossless = false;
    g_state.mode = MODE_LOGS_ONLY;
    g_state.durationMinutes = 1;
    g_state.endTime = {};
    g_state.sessionId++;
}
} // namespace

void Start(const char* filenameSuffix, uint32_t mode, uint32_t durationMinutes) {
    std::lock_guard<std::mutex> lock(g_mutex);

    g_state.sessionId++;

    g_state.mode = mode;
    g_state.durationMinutes = std::clamp<uint32_t>(durationMinutes, 1, 60);
    g_state.filenameSuffix = SanitizeSuffix(filenameSuffix ? filenameSuffix : "");

    g_state.logsEnabled = (mode == MODE_LOGS_ONLY) || (mode == MODE_LOSSLESS_VIDEO) || (mode == MODE_COMPRESSED_VIDEO);
    g_state.videoEnabled = (mode == MODE_LOSSLESS_VIDEO) || (mode == MODE_COMPRESSED_VIDEO);
    g_state.lossless = (mode == MODE_LOSSLESS_VIDEO);

    g_state.active = true;
    g_state.endTime = std::chrono::steady_clock::now() + std::chrono::minutes(g_state.durationMinutes);
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    StopLocked();
}

void Update() {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_state.active) {
        return;
    }

    if (g_state.endTime.time_since_epoch().count() != 0
        && std::chrono::steady_clock::now() >= g_state.endTime) {
        StopLocked();
    }
}

bool IsActive() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.active;
}

bool IsLogsEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.active && g_state.logsEnabled;
}

bool IsVideoEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.active && g_state.videoEnabled;
}

bool IsLossless() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.active && g_state.videoEnabled && g_state.lossless;
}

uint64_t SessionId() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state.sessionId;
}

FSys::path LogDir() { return LogPath(); }

FSys::path GetFfrLogFilePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto suffix = g_state.filenameSuffix;
    return WithSuffix(LogPath() / "GazeLog", ".csv", suffix);
}

FSys::path GetVideoFilePath(bool lossless) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto suffix = g_state.filenameSuffix;
    const char* baseName = lossless ? "encodedVideoLossless" : "encodedVideoCompressed";
    return WithSuffix(LogPath() / baseName, ".h265", suffix);
}

} // namespace alvr::gt_recording
