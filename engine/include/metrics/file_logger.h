#pragma once
#include <cstdio>
#include <mutex>
#include <string>
#include "metrics/logging.h"

// Each engine process (one per stream) logs to its own file, named for its
// stream (see metrics/log_naming.h), so streams never share a file or collide
// when rotating. This default is used only when no stream name is given.
constexpr const char* kDefaultLogPath = "/tmp/engine.log";

class FileRotatingLogger final : public ILogger {
public:
  struct Config {
    std::string path   = kDefaultLogPath;         // re-opened at the stream's own path after config load
    size_t      max_mb = 5;                           // 5 MB per file
    int         max_files = 5;                        // <path>.1 .. .5
    LogLevel    min_level = LogLevel::Info;           // default threshold
    bool        also_stderr = true;                   // mirror to stderr (the engine sets this only when stderr is a terminal)
    bool        utc_time = false;                     // localtime by default (the engine sets true; lines then end the time with Z)
  };

  FileRotatingLogger() noexcept;                        // uses default Config{}
  explicit FileRotatingLogger(const Config& cfg) noexcept;
  ~FileRotatingLogger() override;

  void set_level(LogLevel lvl) noexcept;
  void log(LogLevel lvl, const char* fmt, ...) noexcept override;
  void vlog(LogLevel lvl, const char* fmt, va_list ap) noexcept override;

  // Returns the actual file path in use (after fallback)
  std::string path()    const noexcept { return path_; }
  LogLevel    level()   const noexcept { return min_level_; }
  bool        is_open() const noexcept { return fp_ != nullptr; }

private:
  void vwrite(LogLevel lvl, const char* fmt, va_list ap) noexcept;
  void rotate_if_needed_unlocked() noexcept;
  bool open_file_unlocked(const std::string& prefer) noexcept;
  static const char* level_str(LogLevel l) noexcept;

  mutable std::mutex mu_;
  std::FILE* fp_{nullptr};
  std::string path_;
  size_t max_bytes_{5u * 1024u * 1024u};
  int max_files_{5};
  LogLevel min_level_{LogLevel::Info};
  bool also_stderr_{true};
  bool utc_time_{false};
};

