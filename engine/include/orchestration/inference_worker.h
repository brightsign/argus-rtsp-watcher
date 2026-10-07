#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>
#include <string>
#include <mutex>

// Full includes instead of forward declarations
#include "models/model_runner.h"
#include "pipeline/shared_frame.h"
#include "orchestration/zone.h"
#include "pipeline/frame_mailbox.h"
#include "pipeline/pipeline_types.h"
#include "tracking/tracker.h"
#include "output/frame_writer.h"
#include "output/face_blur.h"

// Type alias for the fusion output structure (defined in orchestrator.h)
struct FusionResults {
    std::mutex m;
    std::vector<Detection> face_dets;
    std::vector<Landmarks> face_lms;
    uint64_t face_seq{0};
    std::vector<Detection> yolo_dets;
    uint64_t yolo_seq{0};
    // Person tracks with stable IDs and uniform/vest classification results
    std::vector<TrackedBox> tracks;
};

namespace inference_worker {

// Generic inference worker configuration
struct WorkerConfig {
    int skip_frames;           // Process every Nth frame (1 = all, 2 = every other, etc)
    int model_input_width;     // Model input size
    int model_input_height;
    std::string model_name;    // For logging
    output::BlurConfig blur_config;  // Person blur configuration (applied before drawing boxes)
    bool flip_horizontal{false};     // Flip output frame left-right (mirror correction)
    bool enable_frame_stamp{true};   // Burn human+machine-readable frame_seq/frame_ts stamp into output frames
    zone::ZoneCfg zone{};            // Custom detection zone; drawn as a faint green outline when enabled
};

// Run inference loop for a single model
// - Consumes frames from frame_mailbox
// - Runs model inference with frame skipping
// - Stores results in fusion output
// - Draws detection/gaze overlays and writes decorated frames via frame_writer
//   (when non-null); pass a null writer to skip visualization/frame output.
// - Respects stop_flag for clean shutdown
// second_runner: optional second model (e.g. RetinaFace) so a single worker
// (typically YOLOX) can draw both models' detections on one combined frame.
void run_inference_loop(
    IModelRunner* runner,
    std::shared_ptr<FrameMailbox> frame_mailbox,
    FusionResults* fusion_output,
    IFrameWriter* frame_writer,
    const WorkerConfig& config,
    const std::atomic<bool>& stop_flag,
    IModelRunner* second_runner = nullptr) noexcept;

} // namespace inference_worker
