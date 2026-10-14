use crate::dashboard::{GtRecordingMode, GtRecordingStartConfig, ServerRequest};
use eframe::egui::{ComboBox, RichText, TextEdit, Ui};
use std::time::{Duration, Instant};

const START_DELAY: Duration = Duration::from_secs(5);

struct PendingStart {
    deadline: Instant,
    config: GtRecordingStartConfig,
}

pub struct DebugTab {
    filename_suffix: String,
    mode: GtRecordingMode,
    duration_minutes: u32,

    pending_start: Option<PendingStart>,
    expected_end: Option<Instant>,

    last_notification: Option<(Instant, String)>,
}

impl DebugTab {
    pub fn new() -> Self {
        Self {
            filename_suffix: String::new(),
            mode: GtRecordingMode::LogsOnly,
            duration_minutes: 1,
            pending_start: None,
            expected_end: None,
            last_notification: None,
        }
    }

    pub fn ui(&mut self, ui: &mut Ui) -> Option<ServerRequest> {
        let mut request = None;
        let now = Instant::now();

        // Auto-trigger start when countdown expires.
        if request.is_none() {
            if let Some(pending) = &self.pending_start {
                if now >= pending.deadline {
                    request = Some(ServerRequest::StartGtRecording(pending.config.clone()));
                    self.expected_end = Some(now + Duration::from_secs(pending.config.duration_minutes as u64 * 60));
                    self.pending_start = None;

                    self.last_notification = Some((
                        now,
                        "Recording started. Outputs will be written to the logFiles folder.".to_string(),
                    ));
                }
            }
        }

        ui.label(
            "Recording from ALVR using these tools is intended for debugging/analysis (raw bitstreams, logs), not for capturing gameplay.",
        );

        ui.separator();

        ui.group(|ui| {
            ui.heading("GT Recording");

            ui.horizontal(|ui| {
                ui.label("Filename suffix:");
                ui.add(
                    TextEdit::singleline(&mut self.filename_suffix)
                        .hint_text("e.g. test01")
                        .desired_width(240.0),
                );
            });

            ui.horizontal(|ui| {
                ui.label("Mode:");
                ComboBox::from_id_salt("gt_recording_mode")
                    .selected_text(match self.mode {
                        GtRecordingMode::LogsOnly => "Logs-only (FFR CSV)",
                        GtRecordingMode::Lossless => "Lossless video + FFR CSV",
                        GtRecordingMode::Compressed => "Compressed video + FFR CSV",
                    })
                    .show_ui(ui, |ui| {
                        ui.selectable_value(
                            &mut self.mode,
                            GtRecordingMode::LogsOnly,
                            "Logs-only (FFR CSV)",
                        );
                        ui.selectable_value(
                            &mut self.mode,
                            GtRecordingMode::Lossless,
                            "Lossless video + FFR CSV",
                        );
                        ui.selectable_value(
                            &mut self.mode,
                            GtRecordingMode::Compressed,
                            "Compressed video + FFR CSV",
                        );
                    });
            });

            ui.horizontal(|ui| {
                ui.label("Duration:");
                ComboBox::from_id_salt("gt_recording_duration")
                    .selected_text(format!("{} min", self.duration_minutes))
                    .show_ui(ui, |ui| {
                        ui.selectable_value(&mut self.duration_minutes, 1, "1 min");
                        ui.selectable_value(&mut self.duration_minutes, 2, "2 min");
                        ui.selectable_value(&mut self.duration_minutes, 5, "5 min");
                    });
            });

            ui.horizontal(|ui| {
                let start_clicked = ui.button("Start recording").clicked();
                let stop_clicked = ui.button("Stop recording").clicked();

                if start_clicked {
                    let config = GtRecordingStartConfig {
                        filename_suffix: self.filename_suffix.clone(),
                        mode: self.mode,
                        duration_minutes: self.duration_minutes,
                    };
                    self.pending_start = Some(PendingStart {
                        deadline: now + START_DELAY,
                        config,
                    });
                    self.expected_end = None;

                    self.last_notification = Some((
                        now,
                        "Recording scheduled. It will start after the countdown.".to_string(),
                    ));
                }

                if stop_clicked {
                    self.pending_start = None;
                    self.expected_end = None;
                    request = Some(ServerRequest::StopGtRecording);

                    self.last_notification = Some((
                        now,
                        "Recording stopped. Video and/or data log should now be saved in the logFiles folder.".to_string(),
                    ));
                }
            });

            // Notification label
            if let Some(pending) = &self.pending_start {
                let remaining = pending.deadline.saturating_duration_since(now);
                let secs = remaining.as_secs();
                let mm = secs / 60;
                let ss = secs % 60;
                ui.label(
                    RichText::new(format!(
                        "Recording scheduled. Starting in {mm:02}:{ss:02} (5s delay)."
                    ))
                    .strong(),
                );
            } else if let Some(end) = self.expected_end {
                if now < end {
                    let remaining = end.saturating_duration_since(now);
                    let secs = remaining.as_secs();
                    let mm = secs / 60;
                    let ss = secs % 60;
                    ui.label(RichText::new(format!("Recording running. Auto-stop in {mm:02}:{ss:02}."))
                        .strong());
                } else {
                    ui.label("Recording duration elapsed. Stopping...");
                    // Send an explicit stop so the streamer closes/flushed files promptly.
                    request = Some(ServerRequest::StopGtRecording);
                    self.expected_end = None;

                    self.last_notification = Some((
                        now,
                        "Recording finished. Video and/or data log should now be saved in the logFiles folder.".to_string(),
                    ));
                }
            } else {
                ui.label("Idle.");
            }

            if let Some((ts, msg)) = &self.last_notification {
                if now.saturating_duration_since(*ts) <= Duration::from_secs(15) {
                    ui.label(RichText::new(msg).strong());
                }
            }
        });

        ui.separator();

        ui.columns(2, |ui| {
            if ui[0].button("Capture frame").clicked() {
                request = Some(ServerRequest::CaptureFrame);
            }

            if ui[1].button("Insert IDR").clicked() {
                request = Some(ServerRequest::InsertIdr);
            }
        });

        request
    }
}
