#include "output/frame_writer.h"
#include "output/frame_stride.h"
#include "pipeline/pipeline_types.h"
#include "orchestration/letterbox.h"
#include "metrics/log_global.h"
#include "util/spsc_queue.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <filesystem>
#include <system_error>
#include <chrono>
#include <thread>
#include <atomic>

namespace fs = std::filesystem;

namespace {
// Write a JPEG atomically: encode to a temp file beside the target, then
// rename() over it. rename(2) is atomic on the same filesystem, so a concurrent
// reader (the preview/MJPEG server) always sees a complete JPEG -- either the
// previous frame or the new one, never a half-written file. Writing in place
// with cv::imwrite() left the file truncated mid-write, which the Go monitor
// reported as "invalid JPEG data" and which blanked panels under multi-stream
// load (the writer is slow, so the partial-file window is wide).
bool write_jpeg_atomic(const std::string& final_path, const cv::Mat& img,
                       const std::vector<int>& params) noexcept {
  try {
    const fs::path fp(final_path);
    // cv::imwrite picks the codec from the extension, so the temp file must keep
    // the .jpg suffix; place it beside the target so rename() stays same-fs.
    const fs::path tmp = fp.parent_path() /
                         (fp.stem().string() + ".tmp" + fp.extension().string());
    if (!cv::imwrite(tmp.string(), img, params)) return false;
    std::error_code ec;
    fs::rename(tmp, fp, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
  } catch (...) {
    return false;
  }
}
} // namespace

namespace {

class NullFrameWriter final : public IFrameWriter {
public:
  bool writeFrame(const cv::Mat&, const PipelineResult&) noexcept override {
    return true;
  }
};

class DiskFrameWriter final : public IFrameWriter {
public:
  DiskFrameWriter(const std::string& output_dir, int max_frames, int quality,
                  int stride) noexcept
    : output_dir_(output_dir), max_frames_(max_frames), quality_(quality),
      stride_(stride), frame_stride_(stride),
      writes_count_(0), last_log_time_(std::chrono::steady_clock::now()) {
    try {
      fs::create_directories(output_dir_);
      LG_INFO("frame_writer: created output directory %s", output_dir_.c_str());
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer: failed to create output directory: %s", e.what());
    }
  }

  bool writeFrame(const cv::Mat& img, const PipelineResult& result) noexcept override {
    if (img.empty()) {
      LG_WARN("frame_writer: empty image, skipping");
      return false;
    }

    // Save one frame in every `stride`. Skipped frames cost nothing here; the
    // frames that are saved are as fresh as the worker made them.
    if (!frame_stride_.admit()) {
      return true;
    }

    try {
      // Write to a fixed filename per output_dir (overwrite mode) so an
      // MJPEG server can watch a single stable path per stream, regardless
      // of which directory this writer was configured for.
      const std::string filepath = (fs::path(output_dir_) / "output.jpg").string();

      cv::Mat output_img = img;  // No copy needed

      // Crop letterbox (remove black bars) if original dimensions are available
      if (result.frame_width > 0 && result.frame_height > 0 &&
          output_img.cols > 0 && output_img.rows > 0) {
        // Letterbox region of the camera frame inside this canvas
        const letterbox::Geometry geometry = letterbox::compute(
            result.frame_width, result.frame_height, output_img.cols, output_img.rows);

        // Crop to letterbox region (removes black bars)
        if (geometry.content_w > 0 && geometry.content_h > 0 &&
            geometry.pad_x >= 0 && geometry.pad_y >= 0 &&
            geometry.pad_x + geometry.content_w <= output_img.cols &&
            geometry.pad_y + geometry.content_h <= output_img.rows) {
          cv::Rect crop_region(geometry.pad_x, geometry.pad_y, geometry.content_w, geometry.content_h);
          output_img = output_img(crop_region).clone();
        }
      }

      // Write frame as JPEG (data is already in correct format)
      // The RGA pipeline actually outputs BGR despite the misleading variable names
      std::vector<int> compression_params;
      compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
      compression_params.push_back(quality_);

      if (!write_jpeg_atomic(filepath, output_img, compression_params)) {
        LG_WARN("frame_writer: failed to write frame to %s", filepath.c_str());
        return false;
      }
      #ifdef ENABLE_DEBUG
      // Track write count and log performance metrics every 1 second
      writes_count_++;
      auto now = std::chrono::steady_clock::now();
      auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time_).count();
      
      if (elapsed_ms >= 1000) {
        float write_fps = (elapsed_ms > 0) ? (1000.0f * writes_count_ / elapsed_ms) : 0.0f;
        LG_INFO("frame_writer: performance metrics (writes=%d, fps=%.1f, stride=%d)",
                writes_count_, write_fps, stride_);
        writes_count_ = 0;
        last_log_time_ = now;
      }

      
      LG_DEBUG("frame_writer: wrote %s (seq=%llu tracks=%zu)",
               filepath.c_str(), (unsigned long long)result.seq, 
               (size_t)result.tracks.size());
      #endif

      return true;
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer: exception while writing frame: %s", e.what());
      return false;
    }
  }

private:
  std::string output_dir_;
  int max_frames_;
  int quality_;
  int stride_;
  output::FrameStride frame_stride_;
  int writes_count_;
  std::chrono::steady_clock::time_point last_log_time_;
};

// Phase 2C: Async Frame Writer
// Moves JPEG encoding and disk I/O to background thread for ~1ms per-frame gain
class AsyncDiskFrameWriter final : public IFrameWriter {
public:
  struct FrameData {
    cv::Mat image;           // Preprocessed image (cloned, cropped)
    std::string filepath;    // Target output path
    uint64_t seq;           // Frame sequence number
    int quality;            // JPEG quality
    bool valid;             // Slot in use flag
    
    FrameData() : seq(0), quality(85), valid(false) {}
  };
  
  AsyncDiskFrameWriter(const std::string& output_dir, int max_frames, int quality,
                       size_t queue_size, int stride) noexcept
    : output_dir_(output_dir), max_frames_(max_frames), quality_(quality),
      frame_stride_(stride),
      running_(false), dropped_frames_(0), total_enqueued_(0) {
    
    // Validate queue size is power of 2
    if (queue_size == 0 || (queue_size & (queue_size - 1)) != 0) {
      LG_WARN("frame_writer_async: invalid queue_size %zu, using default 8", queue_size);
      queue_size = 8;
    }
    
    try {
      fs::create_directories(output_dir_);
      LG_INFO("frame_writer_async: created output directory %s", output_dir_.c_str());
      LG_INFO("frame_writer_async: queue size=%zu (async mode enabled)", queue_size);
      
      // Start background thread
      running_.store(true, std::memory_order_release);
      writer_thread_ = std::thread(&AsyncDiskFrameWriter::writer_thread_func, this);
      
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer_async: failed to initialize: %s", e.what());
    }
  }
  
  ~AsyncDiskFrameWriter() noexcept {
    // Signal stop and wait for thread
    running_.store(false, std::memory_order_release);
    
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
    
    // Log statistics
    if (dropped_frames_ > 0) {
      int dropped = dropped_frames_.load(std::memory_order_relaxed);
      int total = total_enqueued_.load(std::memory_order_relaxed);
      float drop_rate = (total > 0) ? 
        (100.0f * dropped / (total + dropped)) : 0.0f;
      LG_WARN("frame_writer_async: shutdown - dropped %d frames (%.1f%% of total)",
              dropped, drop_rate);
    }
    int written = total_enqueued_.load(std::memory_order_relaxed);
    LG_INFO("frame_writer_async: shutdown complete (written=%d frames)", written);
  }
  
  bool writeFrame(const cv::Mat& img, const PipelineResult& result) noexcept override {
    if (img.empty()) {
      return false;
    }
    
    // Save one frame in every `stride` (see DiskFrameWriter).
    if (!frame_stride_.admit()) {
      return true;
    }
    
    try {
      // Preprocess frame in main thread (minimize time in background):
      // letterbox cropping below.

      // Fixed filename per output_dir (overwrite mode) — see DiskFrameWriter.
      const std::string filepath = (fs::path(output_dir_) / "output.jpg").string();

      cv::Mat output_img = img.clone();  // Clone for async processing
      
      // Crop letterbox (remove black bars) if original dimensions are available
      if (result.frame_width > 0 && result.frame_height > 0 &&
          output_img.cols > 0 && output_img.rows > 0) {
        const letterbox::Geometry geometry = letterbox::compute(
            result.frame_width, result.frame_height, output_img.cols, output_img.rows);

        if (geometry.content_w > 0 && geometry.content_h > 0 &&
            geometry.pad_x >= 0 && geometry.pad_y >= 0 &&
            geometry.pad_x + geometry.content_w <= output_img.cols &&
            geometry.pad_y + geometry.content_h <= output_img.rows) {
          cv::Rect crop_region(geometry.pad_x, geometry.pad_y, geometry.content_w, geometry.content_h);
          output_img = output_img(crop_region).clone();
        }
      }
      
      // Create frame data and enqueue
      FrameData frame_data;
      frame_data.image = std::move(output_img);
      frame_data.filepath = std::move(filepath);
      frame_data.seq = result.seq;
      frame_data.quality = quality_;
      frame_data.valid = true;
      
      // Try to enqueue (lock-free operation)
      if (!queue_.try_enqueue(std::move(frame_data))) {
        // Queue full - drop frame and log warning
        int dropped = dropped_frames_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (dropped % 10 == 1) {  // Log every 10th drop
          LG_WARN("frame_writer_async: queue full, dropped %d frames so far", dropped);
        }
        return false;
      }
      
      total_enqueued_.fetch_add(1, std::memory_order_relaxed);
      return true;
      
    } catch (const std::exception& e) {
      LG_ERROR("frame_writer_async: exception in writeFrame: %s", e.what());
      return false;
    }
  }
  
  void flush() noexcept override {
    // Wait for queue to drain (with timeout)
    const int timeout_ms = 5000;
    const int poll_ms = 100;
    int elapsed_ms = 0;
    
    while (!queue_.empty() && elapsed_ms < timeout_ms) {
      std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
      elapsed_ms += poll_ms;
    }
    
    if (!queue_.empty()) {
      LG_WARN("frame_writer_async: flush timeout - %zu frames remain in queue", queue_.size());
    }
  }

private:
  void writer_thread_func() noexcept {
    LG_INFO("frame_writer_async: background writer thread started");
    
    int writes_count = 0;
    auto last_log = std::chrono::steady_clock::now();
    
    while (running_.load(std::memory_order_acquire) || !queue_.empty()) {
      FrameData frame_data;
      
      // Try to dequeue frame
      if (!queue_.try_dequeue(frame_data)) {
        // Queue empty - sleep briefly and retry
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      
      if (!frame_data.valid) {
        continue;
      }
      
      try {
        // JPEG encode and write (CPU + I/O intensive - done in background)
        std::vector<int> compression_params;
        compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
        compression_params.push_back(frame_data.quality);
        
        if (!write_jpeg_atomic(frame_data.filepath, frame_data.image, compression_params)) {
          LG_WARN("frame_writer_async: failed to write %s", frame_data.filepath.c_str());
          continue;
        }
        
        writes_count++;
        
        // Periodic logging (every 1 second)
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log).count();
        if (elapsed_ms >= 1000) {
          float write_fps = (elapsed_ms > 0) ? (1000.0f * writes_count / elapsed_ms) : 0.0f;
          LG_INFO("frame_writer_async: performance - writes=%d fps=%.1f queue_depth=%zu",
                  writes_count, write_fps, queue_.size());
          writes_count = 0;
          last_log = now;
        }
        
      } catch (const std::exception& e) {
        LG_ERROR("frame_writer_async: exception in writer thread: %s", e.what());
      }
    }

    LG_INFO("frame_writer_async: background writer thread stopped");
  }

  std::string output_dir_;
  int max_frames_;
  int quality_;
  output::FrameStride frame_stride_;

  // Async components
  SPSCQueue<FrameData, 16> queue_;  // Lock-free queue (power of 2 capacity)
  std::thread writer_thread_;
  std::atomic<bool> running_;
  std::atomic<int> dropped_frames_;
  std::atomic<int> total_enqueued_;
};

}  // namespace

std::unique_ptr<IFrameWriter> make_frame_writer_null() noexcept {
  return std::make_unique<NullFrameWriter>();
}

std::unique_ptr<IFrameWriter> make_frame_writer_disk(
    const std::string& output_dir,
    int max_frames,
    int quality,
    int stride) noexcept {
  return std::make_unique<DiskFrameWriter>(output_dir, max_frames, quality, stride);
}

std::unique_ptr<IFrameWriter> make_frame_writer_disk_async(
    const std::string& output_dir,
    int max_frames,
    int quality,
    size_t queue_size,
    int stride) noexcept {
  return std::make_unique<AsyncDiskFrameWriter>(output_dir, max_frames, quality, queue_size, stride);
}
