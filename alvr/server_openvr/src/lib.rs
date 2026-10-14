mod graphics;
mod props;
mod tracking;

#[allow(
    non_camel_case_types,
    non_upper_case_globals,
    dead_code,
    non_snake_case,
    clippy::unseparated_literal_suffix
)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}
use bindings::*;

unsafe extern "C" {
    fn SetGazeRuntimeFlags(
        enablePrediction: u8,
        enableHysteresis: u8,
        enableDynamicSizing: u8,
    );

    fn SetGazePredictorErrorXY(errorUvX: f32, errorUvY: f32);
}

use alvr_common::{
    BUTTON_INFO, HAND_LEFT_ID, HAND_RIGHT_ID, HAND_TRACKER_LEFT_ID, HAND_TRACKER_RIGHT_ID, HEAD_ID,
    Pose, ViewParams, error,
    glam::Vec2,
    parking_lot::{Mutex, RwLock},
    settings_schema::Switch,
    warn,
};
use alvr_filesystem as afs;
use alvr_packets::{ButtonValue, Haptics};
use alvr_server_core::{HandType, ServerCoreContext, ServerCoreEvent};
use alvr_session::{CodecType, ControllersConfig};
use std::{
    collections::VecDeque,
    ffi::{CString, OsStr, c_char, c_void},
    ptr,
    sync::{Once, mpsc},
    thread,
    time::{Duration, Instant},
};

static SERVER_CORE_CONTEXT: RwLock<Option<ServerCoreContext>> = RwLock::new(None);
static LOCAL_VIEW_PARAMS: RwLock<[ViewParams; 2]> = RwLock::new([ViewParams::DUMMY; 2]);
static HEAD_POSE_QUEUE: Mutex<VecDeque<(Duration, Pose)>> = Mutex::new(VecDeque::new());

fn event_loop(events_receiver: mpsc::Receiver<ServerCoreEvent>) {
    thread::spawn(move || {
        if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
            context.start_connection();
        }

        let mut last_resync = Instant::now();
        loop {
            let event = match events_receiver.recv_timeout(Duration::from_millis(5)) {
                Ok(event) => event,
                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            };

            match event {
                ServerCoreEvent::SetOpenvrProperty { device_id, prop } => {
                    props::set_openvr_prop(None, device_id, prop)
                }
                ServerCoreEvent::ClientConnected => {
                    let video_config = alvr_server_core::settings().video;

                    unsafe {
                        if InitializeStreaming() {
                            // Server-only tunables for dynamic foveal sizing. Applied on stream init.
                            if let Switch::Enabled(config) = &video_config.dynamic_foveal_sizing {
                                SetDynamicFovealSizingParams(
                                    config.dyn_min_size,
                                    config.dyn_max_size,
                                    config.grow_rate,
                                    config.shrink_rate,
                                );
                            }

                            RequestDriverResync();
                        } else {
                            SERVER_CORE_CONTEXT.write().take();

                            ShutdownSteamvr();
                        }
                    }
                }

                ServerCoreEvent::ClientDisconnected => unsafe { DeinitializeStreaming() },
                ServerCoreEvent::Battery(info) => unsafe {
                    SetBattery(info.device_id, info.gauge_value, info.is_plugged)
                },
                ServerCoreEvent::PlayspaceSync(bounds) => unsafe {
                    SetChaperoneArea(bounds.x, bounds.y)
                },
                ServerCoreEvent::LocalViewParams(params) => unsafe {
                    *LOCAL_VIEW_PARAMS.write() = params;

                    let ffi_params = [
                        tracking::to_ffi_view_params(params[0]),
                        tracking::to_ffi_view_params(params[1]),
                    ];
                    SetLocalViewParams(ffi_params.as_ptr());
                },
                ServerCoreEvent::Tracking { poll_timestamp } => {
                    let settings = alvr_server_core::settings();
                    let headset_config = &settings.headset;

                    unsafe {
                        if let Switch::Enabled(config) = &settings.video.dynamic_foveal_sizing {
                            SetDynamicFovealSizingParams(
                                config.dyn_min_size,
                                config.dyn_max_size,
                                config.grow_rate,
                                config.shrink_rate,
                            );
                        }

                        SetGazeRuntimeFlags(
                            settings.video.enable_gaze_prediction as u8,
                            settings.video.enable_gaze_hysteresis as u8,
                            settings.video.dynamic_foveal_sizing.enabled() as u8,
                        );
                    }

                    let controllers_config = headset_config.controllers.clone().into_option();
                    let track_body = headset_config.body_tracking.enabled();

                    let tracked = controllers_config.as_ref().is_some_and(|c| c.tracked);
                    let detached_controllers = headset_config
                        .multimodal_tracking
                        .as_option()
                        .is_some_and(|c| c.detached_controllers_steamvr_sink);

                    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
                        if let Some([shift_uv_left, shift_uv_right]) =
                            context.get_last_received_foveation_center_shift()
                        {
                            // Normalized [0,1] (u:right, v:down)
                            unsafe {
                                SetFoveationCenterShift(
                                    shift_uv_left.x,
                                    shift_uv_left.y,
                                    shift_uv_right.x,
                                    shift_uv_right.y,
                                )
                            };
                        }

                        if let Some([mag_uv_x, mag_uv_y]) =
                            context.get_last_received_gaze_magnitude_uv_xy()
                        {
                            unsafe { SetGazeMagnitudeXY(mag_uv_x, mag_uv_y) };
                        } else if let Some(mag_uv) =
                            context.get_last_received_gaze_magnitude_uv()
                        {
                            // Fallback: treat scalar as isotropic overflow.
                            unsafe { SetGazeMagnitudeXY(mag_uv, mag_uv) };
                        }

                        if let Some([err_uv_x, err_uv_y]) =
                            context.get_last_received_gaze_predictor_error_uv_xy()
                        {
                            unsafe { SetGazePredictorErrorXY(err_uv_x, err_uv_y) };
                        }

                        // Feed a horizon estimate (ms) to the C++ gaze predictor.
                        // Use ALVR's motion-to-photon (MTP) latency estimate from server statistics.
                        let mtp_latency_ms = context.get_motion_to_photon_latency().as_secs_f32() * 1000.0;
                        unsafe { SetGazeNetworkLatencyMs(mtp_latency_ms) };

                        let target_timestamp =
                            poll_timestamp + context.get_motion_to_photon_latency();
                        let controllers_pose_time_offset = context.get_tracker_pose_time_offset();
                        // We need to remove the additional offset that SteamVR adds
                        let target_controller_timestamp =
                            target_timestamp.saturating_sub(controllers_pose_time_offset);

                        let ffi_head_motion = if let Some(motion) =
                            context.get_device_motion(*HEAD_ID, poll_timestamp)
                        {
                            let motion = motion.predict(poll_timestamp, target_timestamp);

                            let mut head_pose_queue_lock = HEAD_POSE_QUEUE.lock();
                            head_pose_queue_lock.push_back((poll_timestamp, motion.pose));
                            while head_pose_queue_lock.len() > 360 {
                                head_pose_queue_lock.pop_front();
                            }

                            tracking::to_ffi_motion(*HEAD_ID, motion)
                        } else {
                            FfiDeviceMotion::default()
                        };

                        let (ffi_left_controller_motion, ffi_right_controller_motion) =
                            if tracked && let Some(config) = &controllers_config {
                                let ffi_left_controller_motion = context
                                    .get_device_motion(*HAND_LEFT_ID, poll_timestamp)
                                    .map(|motion| {
                                        let motion = motion
                                            .predict(poll_timestamp, target_controller_timestamp);
                                        let motion = tracking::offset_controller_motion(
                                            config,
                                            *HAND_LEFT_ID,
                                            motion,
                                        );
                                        tracking::to_ffi_motion(*HAND_LEFT_ID, motion)
                                    });
                                let ffi_right_controller_motion = context
                                    .get_device_motion(*HAND_RIGHT_ID, poll_timestamp)
                                    .map(|motion| {
                                        let motion = motion
                                            .predict(poll_timestamp, target_controller_timestamp);
                                        let motion = tracking::offset_controller_motion(
                                            config,
                                            *HAND_RIGHT_ID,
                                            motion,
                                        );
                                        tracking::to_ffi_motion(*HAND_RIGHT_ID, motion)
                                    });

                                (ffi_left_controller_motion, ffi_right_controller_motion)
                            } else {
                                (None, None)
                            };

                        let (
                            ffi_left_hand_skeleton,
                            ffi_right_hand_skeleton,
                            use_separate_hand_trackers,
                            predict_hand_skeleton,
                        ) = if let Some(ControllersConfig {
                            hand_skeleton: Switch::Enabled(hand_skeleton_config),
                            ..
                        }) = controllers_config
                        {
                            let left_hand_skeleton = context
                                .get_hand_skeleton(HandType::Left, poll_timestamp)
                                .map(|s| {
                                    tracking::to_openvr_ffi_hand_skeleton(
                                        headset_config,
                                        *HAND_LEFT_ID,
                                        &s,
                                    )
                                });
                            let right_hand_skeleton = context
                                .get_hand_skeleton(HandType::Right, poll_timestamp)
                                .map(|s| {
                                    tracking::to_openvr_ffi_hand_skeleton(
                                        headset_config,
                                        *HAND_RIGHT_ID,
                                        &s,
                                    )
                                });

                            (
                                tracked.then_some(left_hand_skeleton).flatten(),
                                tracked.then_some(right_hand_skeleton).flatten(),
                                hand_skeleton_config.steamvr_input_2_0,
                                hand_skeleton_config.predict,
                            )
                        } else {
                            (None, None, false, false)
                        };

                        let ffi_left_hand_data = FfiHandData {
                            controllerMotion: if let Some(motion) = &ffi_left_controller_motion {
                                motion
                            } else {
                                ptr::null()
                            },
                            handSkeleton: if let Some(skeleton) = &ffi_left_hand_skeleton {
                                skeleton
                            } else {
                                ptr::null()
                            },
                            isHandTracker: use_separate_hand_trackers
                                && ffi_left_controller_motion.is_none()
                                && ffi_left_hand_skeleton.is_some(),
                            predictHandSkeleton: predict_hand_skeleton,
                        };
                        let ffi_right_hand_data = FfiHandData {
                            controllerMotion: if let Some(motion) = &ffi_right_controller_motion {
                                motion
                            } else {
                                ptr::null()
                            },
                            handSkeleton: if let Some(skeleton) = &ffi_right_hand_skeleton {
                                skeleton
                            } else {
                                ptr::null()
                            },
                            isHandTracker: use_separate_hand_trackers
                                && ffi_right_controller_motion.is_none()
                                && ffi_right_hand_skeleton.is_some(),
                            predictHandSkeleton: predict_hand_skeleton,
                        };

                        let ffi_body_tracker_motions = if track_body || detached_controllers {
                            tracking::BODY_TRACKER_IDS
                                .iter()
                                .filter_map(|id| {
                                    Some(tracking::to_ffi_motion(
                                        *id,
                                        context.get_device_motion(*id, poll_timestamp)?,
                                    ))
                                })
                                .collect::<Vec<_>>()
                        } else {
                            vec![]
                        };

                        // There are two pairs of controllers/hand tracking devices registered in
                        // OpenVR, two lefts and two rights. If enabled with use_separate_hand_trackers,
                        // we select at runtime which device to use (selected for left and right hand
                        // independently. Selection is done by setting deviceIsConnected.
                        unsafe {
                            SetTracking(
                                poll_timestamp.as_nanos() as _,
                                controllers_pose_time_offset.as_secs_f32(),
                                ffi_head_motion,
                                ffi_left_hand_data,
                                ffi_right_hand_data,
                                ffi_body_tracker_motions.as_ptr(),
                                ffi_body_tracker_motions.len() as i32,
                            )
                        };
                    }
                }
                ServerCoreEvent::Buttons(entries) => {
                    for entry in entries {
                        let value = match entry.value {
                            ButtonValue::Binary(value) => FfiButtonValue {
                                type_: FfiButtonType_BUTTON_TYPE_BINARY,
                                __bindgen_anon_1: FfiButtonValue__bindgen_ty_1 {
                                    binary: value.into(),
                                },
                            },

                            ButtonValue::Scalar(value) => FfiButtonValue {
                                type_: FfiButtonType_BUTTON_TYPE_SCALAR,
                                __bindgen_anon_1: FfiButtonValue__bindgen_ty_1 { scalar: value },
                            },
                        };
                        unsafe { SetButton(entry.path_id, value) };
                    }
                }
                ServerCoreEvent::RequestIDR => unsafe { RequestIDR() },
                ServerCoreEvent::CaptureFrame => unsafe { CaptureFrame() },
                ServerCoreEvent::GtRecordingStart {
                    mode,
                    duration_minutes,
                    filename_suffix,
                } => {
                    let suffix = CString::new(filename_suffix).unwrap_or_else(|_| CString::new("").unwrap());
                    unsafe { StartGtRecording(suffix.as_ptr(), mode, duration_minutes) };
                }
                ServerCoreEvent::GtRecordingStop => unsafe { StopGtRecording() },
                ServerCoreEvent::GameRenderLatencyFeedback(game_latency) => {
                    if cfg!(target_os = "linux") && game_latency.as_secs_f32() > 0.25 {
                        let now = Instant::now();
                        if now.saturating_duration_since(last_resync).as_secs_f32() > 0.1 {
                            last_resync = now;
                            warn!("Desync detected. Attempting recovery.");
                            unsafe {
                                RequestDriverResync();
                            }
                        }
                    }
                }
                ServerCoreEvent::ShutdownPending => {
                    SERVER_CORE_CONTEXT.write().take();

                    unsafe { ShutdownSteamvr() };
                }
                ServerCoreEvent::RestartPending => {
                    if let Some(context) = SERVER_CORE_CONTEXT.write().take() {
                        context.restart();
                    }

                    unsafe { ShutdownSteamvr() };
                }
                ServerCoreEvent::ProximityState(headset_is_worn) => unsafe {
                    SetProximityState(headset_is_worn)
                },
            }
        }

        unsafe { ShutdownOpenvrClient() };
    });
}

extern "C" fn driver_ready_idle(set_default_chap: bool) {
    thread::spawn(move || {
        unsafe { InitOpenvrClient() };

        if set_default_chap {
            // call this when inside a new thread. Calling this on the parent thread will crash SteamVR
            unsafe {
                SetChaperoneArea(2.0, 2.0);
            }
        }
    });
}

/// # Safety
/// `instance_ptr` is a valid pointer to a `TrackedDevice` instance
pub unsafe extern "C" fn register_buttons(instance_ptr: *mut c_void, device_id: u64) {
    let mapped_device_id = if device_id == *HAND_TRACKER_LEFT_ID {
        *HAND_LEFT_ID
    } else if device_id == *HAND_TRACKER_RIGHT_ID {
        *HAND_RIGHT_ID
    } else {
        device_id
    };

    for button_id in alvr_server_core::registered_button_set() {
        if let Some(info) = BUTTON_INFO.get(&button_id) {
            if info.device_id == mapped_device_id {
                unsafe { RegisterButton(instance_ptr, button_id) };
            }
        } else {
            error!("Cannot register unrecognized button ID {button_id}");
        }
    }
}

extern "C" fn send_haptics(device_id: u64, duration_s: f32, frequency: f32, amplitude: f32) {
    if let Ok(duration) = Duration::try_from_secs_f32(duration_s)
        && let Some(context) = &*SERVER_CORE_CONTEXT.read()
    {
        context.send_haptics(Haptics {
            device_id,
            duration,
            frequency,
            amplitude,
        });
    }
}

extern "C" fn set_video_config_nals(buffer_ptr: *const u8, len: i32, codec: i32) {
    let codec = if codec == 0 {
        CodecType::H264
    } else if codec == 1 {
        CodecType::Hevc
    } else {
        CodecType::AV1
    };

    let mut config_buffer = vec![0; len as usize];

    unsafe { ptr::copy_nonoverlapping(buffer_ptr, config_buffer.as_mut_ptr(), len as usize) };

    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        context.set_video_config_nals(config_buffer, codec);
    }
}

extern "C" fn send_video(timestamp_ns: u64, buffer_ptr: *mut u8, len: i32, is_idr: bool) {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        let timestamp = Duration::from_nanos(timestamp_ns);
        let buffer = unsafe { std::slice::from_raw_parts(buffer_ptr, len as usize) };

        let Some(head_pose) = HEAD_POSE_QUEUE
            .lock()
            .iter()
            .find_map(|(ts, pose)| (*ts == timestamp).then_some(*pose))
        else {
            // We can't submit the frame without its pose
            return;
        };

        let local_views_params = LOCAL_VIEW_PARAMS.read();

        let global_view_params = [
            ViewParams {
                pose: head_pose * local_views_params[0].pose,
                fov: local_views_params[0].fov,
            },
            ViewParams {
                pose: head_pose * local_views_params[1].pose,
                fov: local_views_params[1].fov,
            },
        ];

        context.send_video_nal(timestamp, global_view_params, is_idr, buffer.to_vec());
    }
}

extern "C" fn get_dynamic_encoder_params() -> FfiDynamicEncoderParams {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read()
        && let Some(params) = context.get_dynamic_encoder_params()
    {
        FfiDynamicEncoderParams {
            updated: 1,
            bitrate_bps: params.bitrate_bps as u64,
            framerate: params.framerate,
        }
    } else {
        FfiDynamicEncoderParams::default()
    }
}

extern "C" fn report_composed(timestamp_ns: u64, offset_ns: u64) {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        context.report_composed(
            Duration::from_nanos(timestamp_ns),
            Duration::from_nanos(offset_ns),
        );
    }
}

extern "C" fn report_present(timestamp_ns: u64, offset_ns: u64) {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        context.report_present(
            Duration::from_nanos(timestamp_ns),
            Duration::from_nanos(offset_ns),
        );
    }
}

extern "C" fn report_foveation_center_shift_used(
    timestamp_ns: u64,
    left_shift_x: f32,
    left_shift_y: f32,
    right_shift_x: f32,
    right_shift_y: f32,
) {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        let shift = [
            Vec2::new(left_shift_x, left_shift_y),
            Vec2::new(right_shift_x, right_shift_y),
        ];

        context.report_foveation_center_shift_used(Duration::from_nanos(timestamp_ns), shift);
    }
}

extern "C" fn report_foveation_center_size_used(
    timestamp_ns: u64,
    center_size_x: f32,
    center_size_y: f32,
) {
    if let Some(context) = &*SERVER_CORE_CONTEXT.read() {
        let size = Vec2::new(center_size_x, center_size_y);

        context.report_foveation_center_size_used(Duration::from_nanos(timestamp_ns), size);
    }
}

extern "C" fn wait_for_vsync() {
    // Default 120Hz-ish wait if StatisticsManager isn't up.
    // We use 120Hz-ish so that SteamVR doesn't accidentally get
    // any weird ideas about our display Hz with its frame pacing.
    static PRE_HEADSET_STATS_WAIT_INTERVAL: Duration = Duration::from_millis(8);

    // NB: don't sleep while locking SERVER_DATA_MANAGER or SERVER_CORE_CONTEXT
    let sleep_duration = SERVER_CORE_CONTEXT
        .read()
        .as_ref()
        .and_then(|ctx| ctx.duration_until_next_vsync());

    if let Some(duration) = sleep_duration {
        if alvr_server_core::settings()
            .video
            .enforce_server_frame_pacing
        {
            thread::sleep(duration);
        } else {
            thread::yield_now();
        }
    } else {
        // StatsManager isn't up because the headset hasn't connected,
        // safety fallback to prevent deadlocking.
        thread::sleep(PRE_HEADSET_STATS_WAIT_INTERVAL);
    }
}

pub extern "C" fn shutdown_driver() {
    SERVER_CORE_CONTEXT.write().take();
}

/// This is the SteamVR/OpenVR entry point
/// # Safety
#[unsafe(no_mangle)]
pub unsafe extern "C" fn HmdDriverFactory(
    interface_name: *const c_char,
    return_code: *mut i32,
) -> *mut c_void {
    let Ok(driver_dir) = alvr_server_io::get_driver_dir_from_registered() else {
        return ptr::null_mut();
    };
    let Some(filesystem_layout) =
        alvr_filesystem::filesystem_layout_from_openvr_driver_root_dir(&driver_dir)
    else {
        return ptr::null_mut();
    };

    let dashboard_process_paths = sysinfo::System::new_all()
        .processes_by_name(OsStr::new(&afs::dashboard_fname()))
        .filter_map(|proc| Some(proc.exe()?.to_owned()))
        .collect::<Vec<_>>();

    // Check that there is no active dashboard instance not part of this driver installation
    // Note: if the iterator is empty, `all()` returns true
    if !dashboard_process_paths
        .iter()
        .all(|path| *path == filesystem_layout.dashboard_exe())
    {
        return ptr::null_mut();
    }

    static ONCE: Once = Once::new();
    ONCE.call_once(move || {
        alvr_server_core::initialize_environment(filesystem_layout.clone());

        let log_to_disk = alvr_server_core::settings().extra.logging.log_to_disk;

        alvr_server_core::init_logging(
            log_to_disk.then(|| filesystem_layout.session_log()),
            Some(filesystem_layout.crash_log()),
        );

        unsafe {
            g_sessionPath = CString::new(filesystem_layout.session().to_string_lossy().to_string())
                .unwrap()
                .into_raw();
            g_driverRootDir = CString::new(
                filesystem_layout
                    .openvr_driver_root_dir
                    .to_string_lossy()
                    .to_string(),
            )
            .unwrap()
            .into_raw();
        };

        graphics::initialize_shaders();

        unsafe {
            LogError = Some(alvr_server_core::alvr_error);
            LogWarn = Some(alvr_server_core::alvr_warn);
            LogInfo = Some(alvr_server_core::alvr_info);
            LogDebug = Some(alvr_server_core::alvr_dbg_server_impl);
            LogEncoder = Some(alvr_server_core::alvr_dbg_encoder);
            LogPeriodically = Some(alvr_server_core::alvr_log_periodically);
            PathStringToHash = Some(alvr_server_core::alvr_path_to_id);
            GetSerialNumber = Some(props::get_serial_number);
            SetOpenvrProps = Some(props::set_device_openvr_props);
            RegisterButtons = Some(register_buttons);
            DriverReadyIdle = Some(driver_ready_idle);
            HapticsSend = Some(send_haptics);
            SetVideoConfigNals = Some(set_video_config_nals);
            VideoSend = Some(send_video);
            GetDynamicEncoderParams = Some(get_dynamic_encoder_params);
            ReportComposed = Some(report_composed);
            ReportPresent = Some(report_present);
            ReportFoveationCenterShiftUsed = Some(report_foveation_center_shift_used);
            ReportFoveationCenterSizeUsed = Some(report_foveation_center_size_used);
            WaitForVSync = Some(wait_for_vsync);
            ShutdownRuntime = Some(shutdown_driver);

            // When there is already a ALVR dashboard running, initialize the HMD device early to
            // avoid buggy SteamVR behavior
            // NB: we already bail out before if the dashboards don't belong to this streamer
            let early_hmd_initialization = !dashboard_process_paths.is_empty();

            CppInit(early_hmd_initialization);
        }

        let (context, events_receiver) = ServerCoreContext::new();

        *SERVER_CORE_CONTEXT.write() = Some(context);

        event_loop(events_receiver);
    });

    unsafe { CppOpenvrEntryPoint(interface_name, return_code) }
}
