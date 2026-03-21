/**
 * main.cpp -- ONVIF event recorder binary.
 *
 * Uses onvif::OnvifListener to receive events from cameras and:
 *   - writes every raw event as a JSON Lines entry to a timestamped .jsonl file
 *   - records human/vehicle detection intervals to a SQLite database
 */

#include <csignal>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

#include "detection_recorder.hpp"
#include "onvif_listener.hpp"
#include "unifi_camera_config.hpp"

// ============================================================
// JSON helpers (used only by EventRecorder)
// ============================================================
static std::string utc_now_iso8601_ms() {
  auto now = std::chrono::system_clock::now();
  std::time_t t = std::chrono::system_clock::to_time_t(now);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  std::ostringstream oss;
  oss << buf << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
  return oss.str();
}

static std::string json_str(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 4);
  out += '"';
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  out += '"';
  return out;
}

static std::string json_obj(const std::map<std::string, std::string>& m) {
  std::string out = "{";
  bool first = true;
  for (auto& [k, v] : m) {
    if (!first) out += ',';
    out += json_str(k) + ':' + json_str(v);
    first = false;
  }
  out += '}';
  return out;
}

// ============================================================
// Thread-safe JSON Lines recorder
// ============================================================
class EventRecorder {
 public:
  explicit EventRecorder(const std::string& path) {
    file_.open(path, std::ios::app);
    if (!file_.is_open())
      throw std::runtime_error("Cannot open: " + path);
    std::cerr << "[recorder] output -> " << path << '\n';
  }

  void write(const onvif::OnvifEvent& ev) {
    std::string line;
    line += '{';
    line += json_str("recorded_at") + ':' + json_str(utc_now_iso8601_ms()) + ',';
    line += json_str("camera_ip")   + ':' + json_str(ev.camera_ip)   + ',';
    line += json_str("camera_user") + ':' + json_str(ev.camera_user) + ',';
    line += json_str("event_time")  + ':' + json_str(ev.event_time)  + ',';
    line += json_str("topic")       + ':' + json_str(ev.topic)       + ',';
    line += json_str("property_op") + ':' + json_str(ev.property_op) + ',';
    line += json_str("source")      + ':' + json_obj(ev.source)      + ',';
    line += json_str("data")        + ':' + json_obj(ev.data);
    line += "}\n";

    std::lock_guard<std::mutex> lk(mu_);
    file_ << line;
    file_.flush();
  }

 private:
  std::ofstream file_;
  std::mutex    mu_;
};

// ============================================================
// Signal handling
// ============================================================
static onvif::OnvifListener* g_listener = nullptr;

static void signal_handler(int) {
  if (g_listener) g_listener->stop();
}

// ============================================================
// Entry point
// ============================================================
int main() {
  std::time_t t0 = std::time(nullptr);
  std::tm tm0{};
  gmtime_r(&t0, &tm0);
  char ts_buf[32];
  std::strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm0);
  std::string output_path  = std::string("onvif_events_") + ts_buf + ".jsonl";
  std::string raw_path     = std::string("onvif_raw_")    + ts_buf + ".jsonl";

  // Backend selection via environment variables:
  //   ONVIF_DB_BACKEND = "sqlite" (default) | "postgres"
  //   ONVIF_DB_CONN    = file path for SQLite (default: "onvif_detections.db")
  //                    = libpq conninfo for PostgreSQL
  //                      (default: local Protect socket on port 5433)
  //   ONVIF_UBV_DIR    = directory for UBV thumbnail files (SQLite only)
  //                      (default: "thumbnails"; ignored for postgres)
  const char* env_backend    = std::getenv("ONVIF_DB_BACKEND");
  const char* env_conn       = std::getenv("ONVIF_DB_CONN");
  const char* env_ubv_dir    = std::getenv("ONVIF_UBV_DIR");
  const char* env_pre_buf    = std::getenv("ONVIF_PRE_BUFFER_SEC");
  const char* env_post_buf   = std::getenv("ONVIF_POST_BUFFER_SEC");
  const char* env_verbose    = std::getenv("ONVIF_VERBOSE");

  onvif::DbBackend db_backend  = onvif::DbBackend::SQLite;
  std::string      db_conn     = "onvif_detections.db";
  std::string      thumbs_dir;
  uint32_t         pre_buf_sec  = env_pre_buf  ?
    static_cast<uint32_t>(std::stoul(env_pre_buf))  : 2u;
  uint32_t         post_buf_sec = env_post_buf ?
    static_cast<uint32_t>(std::stoul(env_post_buf)) : 2u;

  if (env_backend && std::string(env_backend) == "postgres") {
    db_backend = onvif::DbBackend::PostgreSQL;
    db_conn    = env_conn ? env_conn
                          : "host=/run/postgresql port=5433 "
                            "dbname=unifi-protect user=postgres";
    // PG stores thumbnails in the database; UBV dir is not used.
    thumbs_dir = env_ubv_dir ? env_ubv_dir : "";
  } else {
    if (env_conn)    db_conn    = env_conn;
    thumbs_dir = env_ubv_dir ? env_ubv_dir : "thumbnails";
  }

  const bool verbose = env_verbose && std::string(env_verbose) != "0";

  std::cerr << "ONVIF Event Recorder\n"
            << "Events file : " << output_path  << '\n'
            << "Raw file    : " << raw_path      << '\n'
            << "DB backend  : " << (db_backend == onvif::DbBackend::PostgreSQL
                                    ? "postgres" : "sqlite") << '\n'
            << "DB conn     : " << db_conn       << '\n'
            << "Pre-buffer  : " << pre_buf_sec   << " s\n"
            << "Post-buffer : " << post_buf_sec  << " s\n"
            << "Verbose     : " << (verbose ? "yes" : "no") << '\n';
  if (!thumbs_dir.empty())
    std::cerr << "UBV dir     : " << thumbs_dir << '\n';
  std::cerr << "Press Ctrl+C to stop\n\n";

  onvif::global_init();

  try {
    EventRecorder            event_rec(output_path);
    onvif::DetectionRecorder det_rec(db_backend, db_conn);
    det_rec.set_buffer(pre_buf_sec, post_buf_sec);

    onvif::OnvifListener listener;
    g_listener = &listener;
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    if (verbose) listener.enable_verbose_logging();

    if (!thumbs_dir.empty())
      det_rec.set_ubv_dir(thumbs_dir);

    // Camera configs are loaded from the same Protect database.
    // When using the postgres backend we're running on the device, so use
    // the local Unix socket rather than the TCP address in DbConfig defaults.
    unifi::DbConfig cam_db;
    if (db_backend == onvif::DbBackend::PostgreSQL)
      cam_db.host = "/run/postgresql";

    auto cameras = unifi::load_cameras(cam_db);
    unifi::enable_smart_detect(cameras, cam_db);
    for (auto cam : cameras) {
      cam.max_consecutive_failures = 5;
      listener.add_camera(cam);
      det_rec.set_snapshot(cam);
    }
    listener.enable_raw_recording(raw_path);

    listener.run([&event_rec, &det_rec](const onvif::OnvifEvent& ev) {
      event_rec.write(ev);
      det_rec.on_event(ev);
    });
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << '\n';
    onvif::global_cleanup();
    return 1;
  }

  g_listener = nullptr;
  onvif::global_cleanup();
  std::cerr << "\nDone.\n"
            << "  Events     : " << output_path << '\n'
            << "  Raw        : " << raw_path    << '\n'
            << "  Database   : " << db_conn     << '\n'
            << "  Thumbnails : " << thumbs_dir  << "/\n";
  return 0;
}
