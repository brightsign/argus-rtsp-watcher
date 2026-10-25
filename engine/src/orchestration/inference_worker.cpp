#include "orchestration/inference_worker.h"
#include "orchestration/visualization.h"
#include "orchestration/frame_stamp.h"
#include "orchestration/letterbox.h"
#include "models/model_runner.h"
#include "models/model_runner_retinaface.h"
#include "models/model_runner_yolox.h"
#include "pipeline/shared_frame.h"
#include "pipeline/frame_mailbox.h"
#include "pipeline/pipeline_types.h"
#include "config/model_spec.h"
#include "image_utils.h"
#include "common.h"
#include "metrics/log_global.h"
#include "metrics/stage_stats.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <rga/rga.h>
#include <rga/im2d.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <array>

namespace inference_worker {

// Local helper: Check if we should process this frame based on skip count
static bool should_process_frame(uint64_t seq, int skip_frames) noexcept {
    if (skip_frames <= 1) return true;  // Process all frames
    return (seq & (skip_frames - 1)) == 0;  // Bitmask for power-of-2 skip
}

// Fixed pixel layout for the machine-readable frame_seq stamp, documented
// here so an external decoder can sample cell centers directly from a
// recorded (possibly JPEG-recompressed) frame with no other context.
//
//   - Scale: the whole stamp scales continuously with frame height H
//     (= canvas.rows) so it reads the same at any resolution. Each base px
//     dimension D below is drawn at scaled(D) = max(D, round(D*H/kStampScaleRefPx))
//     px (kStampScaleRefPx = 600; round via integer (D*H + 300)/600). So
//     cell = scaled(kStampCellPx), margin = scaled(kStampMarginPx),
//     text_band = scaled(kStampTextBandPx). A decoder MUST compute
//     cell/margin/text_band the same way from H before sampling.
//   - Both stamps sit in the lower-left corner, stacked: the human-readable
//     text is pinned to the very bottom of the frame, and the machine-readable
//     bit grid sits directly ABOVE it. Positions are bottom-anchored (depend
//     on H).
//   - Text band: the bottom text_band px are reserved for the human-readable
//     line (its baseline is at H - margin).
//   - Quiet zone: a solid black rectangle, one cell wide on every side, drawn
//     from (margin, H - text_band - quiet_h) to (margin + quiet_w,
//     H - text_band), where quiet_w = (kStampCols + 2) * cell and
//     quiet_h = (kStampRows + 2) * cell.
//   - Bit grid: kStampCols (8) columns x kStampRows (9) rows of solid
//     cell x cell px cells, starting one cell in from the quiet zone's outer
//     edge, i.e. grid cell [0,0]'s top-left corner is at
//     grid_x0 = margin + cell,
//     grid_y0 = H - text_band - quiet_h + cell
//             = H - text_band - (kStampRows + 1) * cell.
//   - Cell color: pure black (0,0,0) = bit 0, pure white (255,255,255) =
//     bit 1. cell is >= 10px (scale >= 1) so cells survive JPEG.
//   - Bit order: row r in [0,7] holds seq byte r MSB-first (row 0 = bits
//     63-56 of frame_seq, row 7 = bits 7-0); row 8 holds an 8-bit XOR
//     checksum of those 8 bytes, letting a decoder validate the read before
//     trusting it. Within a row, column c in [0,7] holds bit (7-c) of that
//     row's byte (MSB first, left to right).
//     See encode_frame_stamp_bytes() (orchestration/frame_stamp.h) for the
//     exact byte/checksum computation, unit tested independently.
//   - A decoder samples the pixel at each cell's center: cell [row,col]'s
//     center is at (grid_x0 + col*cell + cell/2, grid_y0 + row*cell + cell/2).
namespace {
// BASE (1x) sizes; the drawn stamp multiplies these by an integer 'scale'
// derived from frame height (see layout comment above).
constexpr int kStampCellPx = 10;
constexpr int kStampMarginPx = 4;
constexpr int kStampCols = 8;
constexpr int kStampRows = 9;  // 8 seq-byte rows + 1 checksum row
// Reserved height (px, 1x) of the bottom band holding the human-readable line;
// comfortably exceeds the ~19px the text occupies at font_scale 0.5, so the
// bit grid drawn just above it never overlaps.
constexpr int kStampTextBandPx = 28;
// Frame-height reference for the integer scale factor: scale = max(1, round(H/ref)).
constexpr int kStampScaleRefPx = 600;

// Burns both stamps onto 'canvas' in-place, stacked in the lower-left corner:
//  - Human-readable "#<seq>  <ISO-8601 UTC>.<ms>Z" text (white on a filled
//    dark background) pinned to the very bottom, built from wall_ns.
//  - Machine-readable bit grid directly ABOVE the text band (see layout above).
void draw_frame_stamp(cv::Mat& canvas, uint64_t seq, int64_t wall_ns) {
    if (canvas.empty()) return;

    // Scale the whole stamp (grid + text) continuously with frame height so it
    // reads the same at any resolution; each dimension is floored at its base so
    // low-res frames keep >=10px cells (JPEG-survivable). A decoder computes
    // cell/margin/text_band the same way from H.
    const int H = canvas.rows;
    auto scaled = [H](int base) { return std::max(base, (H * base + kStampScaleRefPx / 2) / kStampScaleRefPx); };
    const int cell = scaled(kStampCellPx);
    const int margin = scaled(kStampMarginPx);
    const int text_band = scaled(kStampTextBandPx);

    // --- Machine-readable bit grid (bottom-anchored, above the text band) ---
    const int quiet_w = (kStampCols + 2) * cell;
    const int quiet_h = (kStampRows + 2) * cell;
    const int quiet_x0 = margin;
    const int quiet_y0 = canvas.rows - text_band - quiet_h;
    cv::rectangle(canvas,
                  cv::Point(quiet_x0, quiet_y0),
                  cv::Point(quiet_x0 + quiet_w, quiet_y0 + quiet_h),
                  cv::Scalar(0, 0, 0), cv::FILLED);

    const std::array<uint8_t, 9> stamp_bytes = encode_frame_stamp_bytes(seq);
    const int grid_x0 = quiet_x0 + cell;
    const int grid_y0 = quiet_y0 + cell;

    for (int row = 0; row < kStampRows; ++row) {
        const uint8_t byte_val = stamp_bytes[static_cast<size_t>(row)];
        for (int col = 0; col < kStampCols; ++col) {
            const bool bit = ((byte_val >> (7 - col)) & 0x1) != 0;
            const cv::Scalar color = bit ? cv::Scalar(255, 255, 255) : cv::Scalar(0, 0, 0);
            const int x0 = grid_x0 + col * cell;
            const int y0 = grid_y0 + row * cell;
            cv::rectangle(canvas, cv::Point(x0, y0),
                          cv::Point(x0 + cell, y0 + cell),
                          color, cv::FILLED);
        }
    }

    // --- Human-readable stamp (pinned to the very bottom of the frame) ---
    const time_t wall_secs = static_cast<time_t>(wall_ns / 1000000000LL);
    const int wall_millis = static_cast<int>((wall_ns / 1000000LL) % 1000LL);
    struct tm tm_utc{};
    gmtime_r(&wall_secs, &tm_utc);

    char text[96];
    std::snprintf(text, sizeof(text), "#%llu  %04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  static_cast<unsigned long long>(seq),
                  tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec, wall_millis);

    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double font_scale = std::max(0.5, 0.5 * H / static_cast<double>(kStampScaleRefPx));
    const int thickness = std::max(1, (H + kStampScaleRefPx / 2) / kStampScaleRefPx);
    int baseline = 0;
    const cv::Size text_size = cv::getTextSize(text, font, font_scale, thickness, &baseline);

    const int pad = margin;
    const int text_x = margin;
    const int text_y = canvas.rows - margin;  // baseline pinned to the bottom
    cv::rectangle(canvas,
                  cv::Point(text_x - pad, text_y - text_size.height - pad),
                  cv::Point(text_x + text_size.width + pad, text_y + baseline + pad),
                  cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(canvas, text, cv::Point(text_x, text_y), font, font_scale,
                cv::Scalar(255, 255, 255), thickness, cv::LINE_AA);
}

// Draw the rolling zone-detect count in the upper-right corner, in the zone's
// configured outline color and at the same size as the bottom-left frame stamp,
// on a dark backing box for legibility over video.
void draw_zone_count(cv::Mat& canvas, int count, const zone::ZoneCfg& zone) {
    if (canvas.empty()) return;
    const int H = canvas.rows;
    auto scaled = [H](int base) { return std::max(base, (H * base + kStampScaleRefPx / 2) / kStampScaleRefPx); };
    const int margin = scaled(kStampMarginPx);

    char text[32];
    std::snprintf(text, sizeof(text), "%d", count);

    const int font = cv::FONT_HERSHEY_SIMPLEX;
    const double font_scale = std::max(0.5, 0.5 * H / static_cast<double>(kStampScaleRefPx));
    const int thickness = std::max(1, (H + kStampScaleRefPx / 2) / kStampScaleRefPx);
    int baseline = 0;
    const cv::Size text_size = cv::getTextSize(text, font, font_scale, thickness, &baseline);

    const int pad = margin;
    const int text_x = canvas.cols - text_size.width - margin;  // right-anchored
    const int text_y = margin + text_size.height;               // top-anchored baseline
    cv::rectangle(canvas,
                  cv::Point(text_x - pad, text_y - text_size.height - pad),
                  cv::Point(text_x + text_size.width + pad, text_y + baseline + pad),
                  cv::Scalar(0, 0, 0), cv::FILLED);
    cv::putText(canvas, text, cv::Point(text_x, text_y), font, font_scale,
                cv::Scalar(zone.b, zone.g, zone.r), thickness, cv::LINE_AA);
}

// Draw a red alert border around the whole frame at the given pixel width, shown
// while an in-zone detect is active. Inset by half the width so the full stroke
// is visible (an OpenCV rectangle line is centered on its edge).
void draw_alert_border(cv::Mat& canvas, int thickness) {
    if (canvas.empty()) return;
    const int t = std::max(1, thickness);
    const int off = t / 2;
    cv::rectangle(canvas, cv::Point(off, off),
                  cv::Point(canvas.cols - 1 - off, canvas.rows - 1 - off),
                  cv::Scalar(0, 0, 255), t);  // red (BGR)
}
}  // namespace

// Local helper: Resize frame via RGA hardware acceleration (with OpenCV fallback)
// Input is BGR24 (from USB/RTSP, already in model color space)
// Output is BGR24 (ready for model inference)
static bool resize_frame_rga(
    const std::vector<uint8_t>& src_bgr,
    int src_w,
    int src_h,
    std::vector<uint8_t>& dst_buf,
    int dst_w,
    int dst_h) noexcept
{
    // If already the right size, just copy
    if (src_w == dst_w && src_h == dst_h) {
        size_t bytes = size_t(dst_w) * dst_h * 3;
        if (dst_buf.size() != bytes) dst_buf.resize(bytes);
        std::memcpy(dst_buf.data(), src_bgr.data(), bytes);
        return true;
    }

    // V6.2.3.4: CRITICAL FIX - Do letterbox resize, not stretch!
    // YOLOX expects letterboxed input (maintain aspect ratio + black padding)
    // Previous code was stretching/squashing, causing wrong detections
    
    const letterbox::Geometry geometry = letterbox::compute(src_w, src_h, dst_w, dst_h);

    // Resize destination buffer to target size and fill with black
    if (dst_buf.size() != size_t(dst_w) * dst_h * 3) {
        dst_buf.resize(size_t(dst_w) * dst_h * 3);
    }
    std::fill(dst_buf.begin(), dst_buf.end(), 0);  // Black padding
    
    // Use OpenCV for letterbox resize (RGA doesn't support offset/padding easily)
    cv::Mat src_mat(src_h, src_w, CV_8UC3, const_cast<uint8_t*>(src_bgr.data()));
    cv::Mat dst_full(dst_h, dst_w, CV_8UC3, dst_buf.data());
    
    // Create ROI in destination for the letterboxed image (skip padding area)
    cv::Mat dst_roi = dst_full(cv::Rect(geometry.pad_x, geometry.pad_y, geometry.content_w, geometry.content_h));

    // Resize source to fit in ROI (maintains aspect ratio)
    cv::resize(src_mat, dst_roi, cv::Size(geometry.content_w, geometry.content_h), 0, 0, cv::INTER_LINEAR);
    
    return true;
}

// Local helper: Prepare FrameView from resized BGR buffer
static void prepare_frame_view(
    FrameView& fv_in,
    const std::vector<uint8_t>& bgr_buf,
    int dst_w,
    int dst_h,
    const std::shared_ptr<SharedFrame>& sf) noexcept
{
    fv_in.fmt     = PixelFormat::BGR24;  // BGR24 input (not RGB24)
    fv_in.width   = dst_w;
    fv_in.height  = dst_h;
    fv_in.stride0 = dst_w * 3;
    fv_in.plane0  = const_cast<uint8_t*>(bgr_buf.data());
    fv_in.pts_ns  = sf->pts_ns;
    // V7.1: FIXED - Store ACTUAL original camera dimensions (not preprocessed dims)
    // RTSP: sf->orig_width=1920, sf->orig_height=1080 (NOT sf->width=320)
    // This allows YOLOX to properly de-letterbox detections back to camera space
    fv_in.orig_width  = sf->orig_width;
    fv_in.orig_height = sf->orig_height;
}

// Local helper: Store inference results in fusion output
static void store_inference_results(
    IModelRunner* runner,
    const InferenceOutputs& outs,
    FusionResults* fusion_output,
    uint64_t seq) noexcept 
{
    if (!fusion_output) return;

    std::lock_guard<std::mutex> g(fusion_output->m);

    auto* retinaface_runner = dynamic_cast<RKNNRetinafaceRunner*>(runner);
    auto* yolox_runner = dynamic_cast<RKNNYoloXRunner*>(runner);

    if (retinaface_runner) {
        fusion_output->face_dets.assign(outs.dets, outs.dets + outs.num_dets);
        fusion_output->face_lms.assign(outs.lms, outs.lms + outs.num_lms);
        fusion_output->face_seq = seq;
        
        // Debug: Log face detection counts periodically
        static int face_log_counter = 0;
        if (++face_log_counter % 90 == 0) {  // Every 3 seconds at 30 FPS
            LG_INFO("[RETINAFACE] Detected %d faces (seq=%llu)", 
                    outs.num_dets, (unsigned long long)seq);
            for (int i = 0; i < outs.num_dets && i < 5; ++i) {  // Log first 5 faces
                const auto& det = outs.dets[i];
                LG_INFO("[RETINAFACE] Face %d: bbox=[%.1f,%.1f,%.1f,%.1f] score=%.2f", 
                        i, det.x0, det.y0, det.x1, det.y1, det.score);
            }
        }
        
        #ifdef ENABLE_DEBUG
        if (seq % 30 == 0) {
            LG_INFO("store_inference_results: RetinaFace seq=%llu dets=%d (yolo_seq=%llu)",
                    (unsigned long long)seq, outs.num_dets, 
                    (unsigned long long)fusion_output->yolo_seq);
        }
        #endif
    } else if (yolox_runner) {
        fusion_output->yolo_dets.assign(outs.dets, outs.dets + outs.num_dets);
        fusion_output->yolo_seq = seq;
        
        #ifdef ENABLE_DEBUG
        if (seq % 30 == 0) {
            LG_INFO("store_inference_results: YOLOX seq=%llu dets=%d (face_seq=%llu)",
                    (unsigned long long)seq, outs.num_dets, 
                    (unsigned long long)fusion_output->face_seq);
        }
        #endif
    }
}

void run_inference_loop(
    IModelRunner* runner,
    std::shared_ptr<FrameMailbox> frame_mailbox,
    FusionResults* fusion_output,
    IFrameWriter* frame_writer,
    const WorkerConfig& config,
    const std::atomic<bool>& stop_flag,
    IModelRunner* second_runner) noexcept
{
    try {
        LG_INFO("inference_worker: start (%s, skip_frames=%d%s)",
                config.model_name.c_str(), config.skip_frames,
                second_runner ? " with second_runner" : "");

        if (!runner) {
            LG_ERROR("inference_worker: runner is null");
            return;
        }

        const int dst_w = config.model_input_width;
        const int dst_h = config.model_input_height;

        // Preallocate resized buffer (reused every frame)
        std::vector<uint8_t> rgb_resized_buf(dst_w * dst_h * 3);

        uint64_t last_logged_seq = 0;
        bool logged_first_frame = false;
        uint32_t debug_frame_idx = 0;

        // FPS tracking
        auto fps_start_time = std::chrono::steady_clock::now();
        int fps_frame_count = 0;
        int fps_processed_count = 0;

        // Rolling zone-detect counter drawn on the annotated frame (upper-right).
        zone::DetectCounter zone_counter;
        zone_counter.window_ms = std::max(1, config.zone.window_minutes) * 60000LL;

        // Per-stage cost of each frame, summarized once per window at debug
        // level (log_level=debug). The camera frame is full resolution, so this
        // shows which full-resolution pass dominates before any is optimized.
        // The writer drops two of every three frames, so write's average is a
        // third of one encode and its max is the real per-encode cost.
        constexpr int64_t kTimingWindowNs = 1000000000LL;
        metrics::StageStats resize_stats, infer_stats, clone_stats, overlay_stats,
                            post_stats, write_stats, frame_stats;
        int64_t timing_window_start_ns = metrics::steady_now_ns();

        while (!stop_flag.load(std::memory_order_relaxed)) {
            // Get next frame from mailbox
            auto sf = frame_mailbox->takeFrame();
            if (!sf) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            // Track all frames (for input FPS)
            fps_frame_count++;

            // Skip frames based on configuration
            if (!should_process_frame(sf->seq, config.skip_frames)) {
                #ifdef ENABLE_DEBUG
                if (sf->seq % 100 == 0) {
                    LG_INFO("inference_worker: skipped frame seq=%llu (%s)",
                            (unsigned long long)sf->seq, config.model_name.c_str());
                }
                #endif
                continue;
            }

            // Track processed frames (for inference FPS)
            fps_processed_count++;
            
            #ifdef ENABLE_DEBUG
            if (sf->seq % 30 == 0) {
                LG_INFO("inference_worker: processing frame seq=%llu (%s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
            }
            #endif

            // Log first frame reception
            if (!logged_first_frame) {
                LG_INFO("inference_worker: got first frame seq=%llu %dx%d (%s)",
                        (unsigned long long)sf->seq, sf->width, sf->height,
                        config.model_name.c_str());
                logged_first_frame = true;
            }

            // Validate frame data
            if (sf->bgr.empty()) {
                LG_WARN("inference_worker: frame has empty BGR data (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }

            const int64_t frame_start_ns = metrics::steady_now_ns();

            // Resize frame via RGA hardware accelerator (BGR24 → BGR24)
            if (!resize_frame_rga(sf->bgr, sf->width, sf->height,
                                  rgb_resized_buf, dst_w, dst_h)) {
                LG_WARN("inference_worker: RGA resize failed (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }
            const int64_t resize_end_ns = metrics::steady_now_ns();
            resize_stats.add_ns(resize_end_ns - frame_start_ns);

            // Prepare input frame view
            FrameView fv_in{};
            prepare_frame_view(fv_in, rgb_resized_buf, dst_w, dst_h, sf);

            // Run inference
            InferenceOutputs outs{};
            if (!runner->infer(fv_in, outs)) {
                LG_WARN("inference_worker: inference failed (seq=%llu, %s)",
                        (unsigned long long)sf->seq, config.model_name.c_str());
                continue;
            }
            const int64_t infer_end_ns = metrics::steady_now_ns();
            infer_stats.add_ns(infer_end_ns - resize_end_ns);

            // Store results in fusion output FIRST (before visualization)
            store_inference_results(runner, outs, fusion_output, sf->seq);

            // Draw overlays on full-resolution camera frame (sf->bgr is now at original RTSP res)
            // Detection coords are in camera/orig space; with canvas == camera size, scale=1.0, offset=0
            cv::Mat vis_canvas = cv::Mat(sf->height, sf->width, CV_8UC3,
                                        const_cast<uint8_t*>(sf->bgr.data())).clone();

            // Flip the raw background first so the image pixels are correct, then draw overlays
            // using mirrored coordinates so that text/box labels remain readable (not mirrored).
            if (config.flip_horizontal) {
                cv::flip(vis_canvas, vis_canvas, 1);  // 1 = horizontal flip
            }
            const int64_t clone_end_ns = metrics::steady_now_ns();
            clone_stats.add_ns(clone_end_ns - infer_end_ns);

            visualization::process_inference_results(runner, vis_canvas, debug_frame_idx,
                                                     sf->orig_width, sf->orig_height, second_runner,
                                                     fusion_output, config.blur_config,
                                                     config.flip_horizontal, config.zone);
            const int64_t overlay_end_ns = metrics::steady_now_ns();
            overlay_stats.add_ns(overlay_end_ns - clone_end_ns);

            // (flip already applied above — no post-visualization flip needed)

            // Write frame to disk if writer is available
            if (frame_writer) {
                PipelineResult result{};
                result.seq = sf->seq;
                result.capture_wall_ns = sf->capture_wall_ns;
                result.frame_width = sf->orig_width;    // Original camera dimensions for letterbox crop
                result.frame_height = sf->orig_height;

                // Populate face tracks for blur processing
                // Transform coordinates: RetinaFace model space (320x320) -> canvas space (dst_w x dst_h)
                if (fusion_output && sf->orig_width > 0 && sf->orig_height > 0) {
                    std::lock_guard<std::mutex> g(fusion_output->m);
                    if (!fusion_output->face_dets.empty()) {
                        // Step 1: De-letterbox parameters (RetinaFace model is 320x320)
                        const float model_size = 320.0f;
                        const float s = std::min(model_size / sf->orig_width, model_size / sf->orig_height);
                        const float pad_x = (model_size - sf->orig_width * s) / 2.0f;
                        const float pad_y = (model_size - sf->orig_height * s) / 2.0f;

                        // Step 2: Canvas letterbox parameters
                        const letterbox::Geometry canvas =
                            letterbox::compute(sf->orig_width, sf->orig_height, dst_w, dst_h);

                        result.tracks.reserve(fusion_output->face_dets.size());
                        for (size_t i = 0; i < fusion_output->face_dets.size(); ++i) {
                            const auto& det = fusion_output->face_dets[i];

                            // De-letterbox: model space -> camera space
                            float cx0 = (det.x0 - pad_x) / s;
                            float cy0 = (det.y0 - pad_y) / s;
                            float cx1 = (det.x1 - pad_x) / s;
                            float cy1 = (det.y1 - pad_y) / s;

                            // Scale: camera space -> canvas space
                            Track t;
                            t.box.x0 = canvas.to_dest_x(cx0);
                            t.box.y0 = canvas.to_dest_y(cy0);
                            t.box.x1 = canvas.to_dest_x(cx1);
                            t.box.y1 = canvas.to_dest_y(cy1);
                            t.box.score = det.score;
                            t.box.class_id = det.class_id;

                            if (i < fusion_output->face_lms.size()) {
                                t.lms = fusion_output->face_lms[i];
                            }
                            result.tracks.push_back(t);
                        }
                    }

                    // Populate person tracks from YOLOX detections for person blur.
                    // yolo_dets are already in camera space (de-letterboxed by the
                    // YOLOX runner), so only the camera -> canvas letterbox mapping
                    // is applied here.
                    if (!fusion_output->yolo_dets.empty()) {
                        const letterbox::Geometry person_canvas =
                            letterbox::compute(sf->orig_width, sf->orig_height, dst_w, dst_h);

                        result.person_tracks.reserve(fusion_output->yolo_dets.size());
                        for (const auto& det : fusion_output->yolo_dets) {
                            // Filter: only person class (class_id == 0 in COCO)
                            if (det.class_id != 0) continue;
                            if (det.score < 0.25f) continue;  // Lower threshold to catch distant people

                            // Scale: camera space -> canvas space
                            TrackedBox tb{};
                            tb.id = -1;  // No tracking ID in this context
                            tb.x0 = person_canvas.to_dest_x(det.x0);
                            tb.y0 = person_canvas.to_dest_y(det.y0);
                            tb.x1 = person_canvas.to_dest_x(det.x1);
                            tb.y1 = person_canvas.to_dest_y(det.y1);
                            tb.score = det.score;
                            result.person_tracks.push_back(tb);
                        }
                    }
                }

                // Rolling zone-detect count (upper-right, in the zone color). A
                // frame counts as in-zone when any YOLO person box satisfies the
                // zone's any/base predicate; the counter confirms and de-dupes
                // episodes. fusion_.yolo_dets are already in camera space (the
                // YOLOX runner de-letterboxes them before storing, same coords the
                // published in_zone uses), so they are tested directly against the
                // normalized zone -- no de-letterboxing.
                if (config.zone.enabled) {
                    bool in_zone_now = false;
                    if (fusion_output && sf->orig_width > 0 && sf->orig_height > 0) {
                        std::lock_guard<std::mutex> g(fusion_output->m);
                        for (const auto& det : fusion_output->yolo_dets) {
                            if (det.class_id != 0 || det.score < 0.25f) continue;  // person only
                            if (zone::box_in_zone(config.zone.mode, det.x0, det.y0, det.x1, det.y1,
                                                  sf->orig_width, sf->orig_height,
                                                  config.zone.x0, config.zone.y0,
                                                  config.zone.x1, config.zone.y1)) {
                                in_zone_now = true;
                                break;
                            }
                        }
                    }
                    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    const int detect_count = zone_counter.record(in_zone_now, now_ms);
                    draw_zone_count(vis_canvas, detect_count, config.zone);
                    // Red alert border while the zone is triggered: fires on the
                    // first in-zone frame and clears when the trigger condition
                    // clears. Its width is independent of the zone outline.
                    if (zone_counter.triggered(now_ms)) {
                        draw_alert_border(vis_canvas, config.zone.border_thickness);
                    }
                }

                // Burn the sync stamp after all overlays so it is never
                // painted over, and only when frame output is actually on
                // (we're already inside the `if (frame_writer)` branch).
                if (config.enable_frame_stamp) {
                    draw_frame_stamp(vis_canvas, sf->seq, sf->capture_wall_ns);
                }

                const int64_t write_start_ns = metrics::steady_now_ns();
                post_stats.add_ns(write_start_ns - overlay_end_ns);
                frame_writer->writeFrame(vis_canvas, result);
                write_stats.add_ns(metrics::steady_now_ns() - write_start_ns);
            }

            const int64_t frame_end_ns = metrics::steady_now_ns();
            frame_stats.add_ns(frame_end_ns - frame_start_ns);
            if (frame_end_ns - timing_window_start_ns >= kTimingWindowNs) {
                LG_DEBUG("inference_worker: stage ms avg/max device=%s (%s) frames=%llu "
                         "resize=%.1f/%.1f infer=%.1f/%.1f clone=%.1f/%.1f "
                         "overlay=%.1f/%.1f post=%.1f/%.1f write=%.1f/%.1f total=%.1f/%.1f",
                         config.device_id.c_str(), config.model_name.c_str(),
                         (unsigned long long)frame_stats.count(),
                         resize_stats.average_ms(), resize_stats.max_ms(),
                         infer_stats.average_ms(), infer_stats.max_ms(),
                         clone_stats.average_ms(), clone_stats.max_ms(),
                         overlay_stats.average_ms(), overlay_stats.max_ms(),
                         post_stats.average_ms(), post_stats.max_ms(),
                         write_stats.average_ms(), write_stats.max_ms(),
                         frame_stats.average_ms(), frame_stats.max_ms());
                resize_stats.reset();
                infer_stats.reset();
                clone_stats.reset();
                overlay_stats.reset();
                post_stats.reset();
                write_stats.reset();
                frame_stats.reset();
                timing_window_start_ns = frame_end_ns;
            }

            // Log periodically
            #ifdef ENABLE_DEBUG
            // Log FPS metrics every second
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - fps_start_time).count();
            
            if (elapsed_ms >= 1000) {
                float input_fps = (elapsed_ms > 0) ? (1000.0f * fps_frame_count / elapsed_ms) : 0.0f;
                float process_fps = (elapsed_ms > 0) ? (1000.0f * fps_processed_count / elapsed_ms) : 0.0f;
                LG_INFO("inference_worker: FPS metrics (%s) input=%.1f process=%.1f skip=%d frames=%d",
                        config.model_name.c_str(), input_fps, process_fps, 
                        config.skip_frames, fps_frame_count);
                
                fps_frame_count = 0;
                fps_processed_count = 0;
                fps_start_time = now;
            }

            
            if (sf->seq != last_logged_seq && (sf->seq % 30 == 0)) {
                last_logged_seq = sf->seq;
                LG_INFO("inference_worker: seq=%llu detections=%d (%s)",
                        (unsigned long long)sf->seq, outs.num_dets,
                        config.model_name.c_str());
            }
            #endif
        }

        LG_INFO("inference_worker: stop (%s)", config.model_name.c_str());

    } catch (const std::exception& e) {
        LG_CRIT("inference_worker: exception: %s", e.what());
        std::fflush(nullptr);
        std::abort();
    } catch (...) {
        LG_CRIT("inference_worker: unknown exception");
        std::fflush(nullptr);
        std::abort();
    }
}

} // namespace inference_worker
