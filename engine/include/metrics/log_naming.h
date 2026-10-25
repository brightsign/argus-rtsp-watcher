#pragma once

#include <string>

// Names for the per-stream engine log files. Pure and dependency-free so the
// rules (above all: a name taken from config cannot escape the log directory)
// are unit tested on the host, mirroring orchestration/zone.h.
namespace log_naming {

// Used when no stream name is known.
constexpr const char* kFallbackStem = "engine";

// Reduces an arbitrary string (a device id from config, a config file name) to
// one that is safe as a log file name: every character outside [A-Za-z0-9_-]
// becomes '_', so no '/', '.' or other path syntax survives and the file can
// only land inside the log directory. An empty result becomes kFallbackStem.
inline std::string sanitize_log_stem(const std::string& raw) {
  std::string safe;
  safe.reserve(raw.size());
  for (const char c : raw) {
    const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '_' || c == '-';
    safe.push_back(allowed ? c : '_');
  }
  return safe.empty() ? std::string(kFallbackStem) : safe;
}

// A path's file name without its directories or its last extension:
// "/tmp/argus-watcher/reo1.json" -> "reo1". A leading dot is not an extension.
inline std::string path_stem(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name.erase(dot);
  return name;
}

// "<dir>/<stem>.log", without doubling a trailing slash on dir.
inline std::string stream_log_path(const std::string& dir, const std::string& stem) {
  const bool has_slash = !dir.empty() && dir.back() == '/';
  return dir + (has_slash ? "" : "/") + stem + ".log";
}

}  // namespace log_naming
