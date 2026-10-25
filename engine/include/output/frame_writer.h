#ifndef FRAME_WRITER_H
#define FRAME_WRITER_H

#include <memory>
#include <string>
#include <opencv2/core.hpp>
#include "output/face_blur.h"

// Forward declarations
struct PipelineResult;
struct ImageBuffer;
struct FrameView;
struct InferenceOutputs;

/**
 * Frame writer: Writes decorated/annotated frames to disk or RTSP output.
 * 
 * Usage pattern:
 *   auto writer = make_frame_writer_disk("/storage/sd/decorated");
 *   writer->writeFrame(img, result);  // writes decorated frame
 */
class IFrameWriter {
public:
  virtual ~IFrameWriter() = default;

  // Write a single decorated frame
  // img: cv::Mat with BGR/NV12/RGB data
  // result: inference detections/landmarks
  // Returns false on write failure, true otherwise
  virtual bool writeFrame(const cv::Mat& img, const PipelineResult& result) noexcept = 0;
  
  // Optional: write frame with raw InferenceOutputs (for debugging)
  virtual bool writeFrameRaw(const cv::Mat& img, const InferenceOutputs& outs) noexcept {
    return true;  // default no-op
  }

  // Optional: flush pending writes
  virtual void flush() noexcept {}
};

// Create a frame writer that saves decorated frames to disk (JPEG).
// Always overwrites a single fixed file "<output_dir>/output.jpg" so an
// MJPEG server can watch one stable path per stream/output_dir.
// output_dir: directory to save the frame into (will be created if needed)
// max_frames: unused (kept for ABI/API compatibility with the async variant)
// quality: JPEG quality (0-100)
// blur_config: optional face blur configuration for privacy
// stride: save one frame in every `stride` processed frames (1 = every frame);
//         must be >= 1
std::unique_ptr<IFrameWriter> make_frame_writer_disk(
    const std::string& output_dir,
    int max_frames = 0,
    int quality = 85,
    const output::BlurConfig& blur_config = output::BlurConfig{},
    int stride = 1) noexcept;

// Create a no-op frame writer (useful for disabling frame output)
std::unique_ptr<IFrameWriter> make_frame_writer_null() noexcept;

// Create an async frame writer that encodes/writes in background thread.
// Phase 2C: Moves JPEG encoding and disk I/O to background thread (~1ms gain per frame)
// Same fixed-file overwrite behavior as make_frame_writer_disk (see above).
// output_dir: directory to save the frame into (will be created if needed)
// max_frames: unused (kept for ABI/API compatibility)
// quality: JPEG quality (0-100)
// blur_config: optional face blur configuration for privacy
// queue_size: background queue depth (power of 2, default 8)
// stride: save one frame in every `stride` processed frames (1 = every frame)
std::unique_ptr<IFrameWriter> make_frame_writer_disk_async(
    const std::string& output_dir,
    int max_frames = 0,
    int quality = 85,
    const output::BlurConfig& blur_config = output::BlurConfig{},
    size_t queue_size = 8,
    int stride = 1) noexcept;

#endif // FRAME_WRITER_H
