# Dyna-ALVR: Gaze-Adaptive Foveation and Latency-Driven Optimization

ALVR (Air Light VR) is a VR streaming application that streams PC-rendered VR content to a VR headset over a network connection, either wirelessly via Wi-Fi or through a wired connection.

**Dyna-ALVR** is a research prototype built on [ALVR](https://github.com/alvr-org/ALVR), implementing gaze-adaptive foveation and latency-driven optimization (LDO) for remotely rendered VR. It is maintained independently of the upstream ALVR project. **SteamVR is required to use this project.**

The public [Dyna-ALVR repository](https://github.com/adhi-w/Dyna-ALVR) is intended for research source releases and downloadable application builds. Ongoing development is maintained separately on GitLab. This project retains the original ALVR commit history up to the version on which our implementation is based; it does not automatically incorporate newer upstream ALVR changes.

The original ALVR landing page is preserved in the [wiki](wiki/ALVR-README.md). See the [wiki folder](wiki) for ALVR documentation, compatibility information, and troubleshooting.

## Overview

This project is a **research prototype / experimental release** implementing the methods presented in our ACM Multimedia papers:

- **2026:** *Latency-Driven Optimization for Foveation in Remote Rendered VR* (paper link forthcoming).
- **2025:** [*Gaze-Adaptive Foveation for Remote Rendered VR*](https://doi.org/10.1145/3746027.3755620).

Remote rendering brings high-fidelity VR to standalone headsets by offloading graphics workloads to a PC or remote server. Streaming those images, however, introduces bandwidth and latency challenges. Our work addresses these challenges through gaze-adaptive spatial compression and explicit compensation for latency.

For the full methodology and evaluation, please read the papers. If you find this work useful, please consider [citing it](#citation). We sincerely appreciate your interest and support.

### Gaze-adaptive foveation (ACM Multimedia 2025)

Foveation exploits the human visual system's varying acuity: it preserves detail near the user's gaze while reducing resolution in the periphery. We integrate three dynamic spatial compression methods into ALVR, alongside their static counterparts:

- **D-AADT2:** Dynamic Axis-Aligned Distortion Transmission with two distinct quality regions: one high-resolution foveal region and one lower-resolution peripheral region.
- **D-AADT3:** Dynamic Axis-Aligned Distortion Transmission with three distinct quality regions: one high-resolution foveal region and two peripheral regions.
- **D-FRW:** Dynamic Foveated Radial Warp, which applies gaze-centered radial distortion with continuously varying spatial resolution rather than discrete quality regions.

These methods adapt compression to gaze-tracking input and support native 72 FPS operation in the evaluated setup. Across the VR games evaluated in the paper, dynamic methods improved encoding efficiency and perceptual quality over static approaches. A complementary user study found improvements in visual quality, immersion, and interaction experience.

### Latency-driven optimization (ACM Multimedia 2026)

When gaze changes while a frame is being rendered and transmitted, its high-quality region can become misaligned with the user's gaze. Our LDO framework treats latency as a design parameter and combines **gaze prediction, hysteresis-based stabilization, and adaptive foveal region sizing** to reduce this misalignment and temporal instability.

In the paper's evaluation, LDO reduced mean gaze error by **24.04%** and 95th-percentile error by **28.17%**, while reducing outside-fovea misses from **7.27% to 0.21%**. Perceptual quality improved by **1–4 dB in EWPSNR** and **0.2–0.8 in FovVideoVDP** over the baseline, with larger gains at higher latencies. A subjective study also found statistically significant improvements in perceived visual quality and overall quality over the previous state of the art.

With the current default parameters, disable gaze prediction when motion-to-photon (MTP) latency is below **90 ms**, as prediction may overshoot and reduce visual quality.

These results describe the papers' evaluation conditions; performance depends on the headset, content, network, and configuration.


## Requirements

- A VR-capable PC and **SteamVR**.
- An ALVR-compatible standalone headset. The gaze-adaptive methods require gaze-tracking input. (We use Meta Quest Pro)
- A network connection between the PC and headset, via Wi-Fi or a [wired connection](wiki/ALVR-wired-setup-(ALVR-over-USB).md).
- The streamer and client built from this research repository to use the integrated methods.

See the [original ALVR documentation](wiki/ALVR-README.md#compatibility) for general hardware compatibility. Upstream ALVR compatibility does not establish support for every research feature on every headset.

## Download the application

**[Download Dyna-ALVR research builds from GitHub Releases](https://github.com/adhi-w/Dyna-ALVR/releases)**

The public repository and release assets are being prepared. Until they are published, the links above may be unavailable. Once published, use the application assets described below.

In a research release's **Assets** section, download both:

- **`alvr_streamer_windows.zip`** — extract it on your Windows PC and run `ALVR Dashboard.exe`. Keep the extracted directory structure intact.
- **`alvr_client_android.apk`** — install it on your headset, for example with `adb install -r alvr_client_android.apk`.

Use the streamer and client from the same research release. **SteamVR is required** on the PC; gaze-adaptive features require gaze tracking (our setup uses Meta Quest Pro).

GitHub also lists **Source code (zip)** and **Source code (tar.gz)** under Assets. These downloads contain the project's programming files and must be compiled before use. To install and run the application, download **`alvr_streamer_windows.zip`** and **`alvr_client_android.apk`** listed above.

The Windows bundle also requires the [Microsoft Visual C++ x64 Redistributable](https://aka.ms/vc14/vc_redist.x64.exe) for its libvpl runtime. Install it if it is not already present.

These are experimental research builds. If no research release assets are listed yet, build from source below. 

For local builds, the upload-ready Windows ZIP and matching headset APK are kept directly in `release-files/`, alongside checksums and build information. The headset application is named **Dyna-ALVR**.

## Software configuration (application dashboard)

Configure the following settings in the ALVR dashboard on your PC.

### Settings > Presets

| Setting | Purpose |
| --- | --- |
| **Transcoding resolution** | Sets the image resolution used for video encoding and streaming from the server to the headset. |
| **Emulated (render) resolution** | Sets the render resolution before encoding and the target resolution for presenting the reconstructed image on the headset. |

For the intended foveated streaming configuration, set the **emulated resolution higher than the transcoding resolution**. This lets the server render at a higher resolution while transmitting fewer pixels. Foveation prioritizes detail near the gaze position and reduces peripheral detail to fit the smaller streamed image. This can reduce bandwidth requirements; actual network usage also depends on the bitrate settings. Restart SteamVR after changing these resolution presets.

### Settings > Video

| Setting | Purpose |
| --- | --- |
| **Show gaze/center-shift debug markers** | Displays the gaze position in red and the foveation center position in blue. Intended only for debugging. |
| **Show foveal ROI box debug marker** | Displays a yellow rectangle outlining the foveal region of interest (ROI), useful for inspecting the D-AADT2 foveal region. Intended only for debugging. |
| **Gaze Prediction** | Enables the LDO component that predicts gaze position to compensate for latency. |
| **Hysteresis** | Enables the LDO stabilization component to reduce rapid changes in the foveal region's position. |
| **Dynamic Foveal Sizing** | Enables adaptive adjustment of the foveal region's size. Applies to **D-AADT2** and **D-AADT3**. |
| **Foveated encoding** | Enables the foveation controls. Turn it on, then select a method from the options below. |

With the current default parameters, **disable Gaze Prediction when MTP latency is below 90 ms**: prediction can overshoot and produce worse results. Restart the stream after changing the debug-marker settings.

Under **Foveated encoding > Method**, choose:

| Method | Description |
| --- | --- |
| **Vanilla** | ALVR's default foveation method. |
| **D-AADT2**, **D-AADT3**, **D-FRW** | Our dynamic, gaze-adaptive foveation methods. |
| **GT (Ground Truth)** | Reference mode with no foveation applied. |

Restart SteamVR after changing the foveation method.

## Build from source

See [Building from source](wiki/Building-From-Source.md) for the full ALVR guide. The following Windows PC / Android headset workflow summarizes the project's development setup, with commands and output paths matched to this checkout. Start from a clone of **this repository**.

### 1. Install the development tools

- [Rust via rustup](https://www.rust-lang.org/tools/install).
- [Chocolatey](https://chocolatey.org/install), used by the dependency preparation scripts.
- [LLVM](https://releases.llvm.org/download.html) (the project's setup used **18.1.8**).
- [Vulkan SDK](https://vulkan.lunarg.com/). Use the paths for your installed SDK version.
- [Android Studio](https://developer.android.com/studio).
- Visual Studio 2022 with these components:
  - MSVC build tools for x64/x86 and C++ ATL for the installed MSVC toolset.
  - C++ CMake tools and the C++ Clang compiler.
  - Windows 11 SDK.

In Android Studio's **SDK Manager**, install:

| Component | Project setup |
| --- | --- |
| Android SDK Platforms | API 32 (target SDK in this checkout); API 29 was also installed in the development setup |
| Android SDK Build-Tools | 36.1.0 and 29.0.3 were installed in the development setup |
| Android NDK | 25.1.8937393 |
| CMake | 4.0.2 was installed in the development setup |
| Android SDK Platform-Tools | Required for `adb` |

The recorded tool versions describe the development environment, rather than a verified minimum-version matrix. Accept the Android SDK licenses before building.

### 2. Set environment variables

Set these Windows environment variables to your installation paths:

| Variable | Example value |
| --- | --- |
| `ANDROID_HOME` | `%LOCALAPPDATA%\Android\Sdk` |
| `ANDROID_NDK_HOME` | `%LOCALAPPDATA%\Android\Sdk\ndk\25.1.8937393` |
| `JAVA_HOME` | `C:\Program Files\Android\Android Studio\jbr` |

Ensure the following directories are on `PATH`, adjusting them for your installation:

```text
%USERPROFILE%\.cargo\bin
C:\ProgramData\chocolatey\bin
C:\Program Files\LLVM\bin
%JAVA_HOME%\bin
%ANDROID_HOME%\platform-tools
%ANDROID_NDK_HOME%\toolchains\llvm\prebuilt\windows-x86_64\bin
C:\VulkanSDK\<installed-version>\Bin
```

If you invoke Android build tools directly, add their versioned directory, such as `%ANDROID_HOME%\build-tools\36.1.0`. Open a new terminal after updating environment variables.

### 3. Prepare dependencies and build

Run these commands in PowerShell from the repository root:

```powershell
rustup update
git submodule update --init --checkout --recursive

cargo xtask prepare-deps --platform windows
cargo xtask prepare-deps --platform android

cargo xtask build-streamer --release
cargo xtask build-client --release
```

The submodule step retrieves dependencies including OpenVR. Dependency preparation installs additional tools and downloads/builds dependencies; the Windows step may request administrator privileges.

Build outputs:

- **PC streamer:** `build/alvr_streamer_windows/`
- **Android client APK:** `build/alvr_client_android/alvr_client_android.apk`

Use `build-streamer` and `build-client` for this checkout. The older command names `build-server` and `build-alxr-android` are not supported. When rebuilding the streamer, add `--keep-config` to preserve its existing `session.json`.

To bundle FFmpeg in the Windows streamer, use `cargo xtask build-streamer --release --gpl`; see the [build guide](wiki/Building-From-Source.md#streamer-building) for the associated redistribution terms.

### 4. Install and run

Enable developer mode / USB debugging on the headset, connect it by USB, and authorize the PC. Install the APK from the repository root:

```powershell
adb install -r .\build\alvr_client_android\alvr_client_android.apk
```

Alternatively, after preparing the Android dependencies, build and run directly on the connected headset:

```powershell
cd alvr/client_openxr
cargo apk run
```

Launch the dashboard from the streamer output directory and follow the [installation guide](wiki/Installation-guide.md) to connect the headset and configure SteamVR. Use the research build's APK and streamer for these steps.

**Windows troubleshooting:** The development setup also required `libvpl.dll` when resolving a SteamVR headset-detection issue. Dependency preparation builds libvpl under `deps/windows/libvpl/alvr_build`. If the driver reports a missing `libvpl.dll`, check that the matching runtime is available to the driver. This is a setup-specific troubleshooting note; see the [troubleshooting guide](wiki/Troubleshooting.md) for other headset-detection issues.

### 5. Create a distributable bundle

For distribution, archive the streamer directory as `release-files/alvr_streamer_windows.zip` and copy the matching client APK to `release-files/alvr_client_android.apk`. Include `libvpl.dll` and license notices in the Windows bundle, and exclude local settings, logs, and debug symbols. Keep APK signing keys private and back them up outside disposable build directories so future client updates can use the same signing identity.

Publish these application files as GitHub Release assets alongside checksums and build information. Build the streamer and client from the same source commit and associate the release with a tag for that commit. The `release-files/` directory is a local staging location; application downloads are distributed through GitHub Releases rather than committed to the source repository.

The upstream packaging command is also available:

```powershell
cargo xtask package-streamer --platform windows
```

This command prepares dependencies again and uses the `distribution` profile. It is not needed to archive existing `--release` builds. Add `--gpl` if bundling FFmpeg. For a Meta Store client package, use `cargo xtask package-client --meta-store`; this flag applies to packaging, not `build-client`.

For dashboard-only development, use `cargo build -p alvr_dashboard`. Use `build-streamer` when you need the complete runnable streamer layout.

## Papers

- **Gaze-Adaptive Foveation for Remote Rendered VR**, ACM Multimedia 2025. [Read the paper](https://doi.org/10.1145/3746027.3755620).
- **Latency-Driven Optimization for Foveation in Remote Rendered VR**, ACM Multimedia 2026. Paper link and citation forthcoming.

## Citation

If this project supports your research, please cite the relevant paper. The available citation is:

```bibtex
@inproceedings{widagdo2025gaze,
  title={Gaze-Adaptive Foveation for Remote Rendered VR},
  author={Widagdo, Adhi and K{\"a}m{\"a}r{\"a}inen, Teemu and Alhilal, Ahmad and Siekkinen, Matti and Hsu, Cheng-Hsin},
  booktitle={Proceedings of the 33rd ACM International Conference on Multimedia},
  pages={11871--11879},
  year={2025},
  doi={10.1145/3746027.3755620}
}
```

## Acknowledgments and license

This research builds on [ALVR](https://github.com/alvr-org/ALVR). We thank the ALVR developers and contributors for their open-source platform.

See the repository's [MIT License](LICENSE). Optional bundled dependencies may have additional licensing requirements, as described in the [build guide](wiki/Building-From-Source.md).
