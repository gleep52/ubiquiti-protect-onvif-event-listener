// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * test_detection_recorder.cpp
 *
 * End-to-end test for DetectionRecorder.
 *
 * Two SnapshotSyntheticEmulator instances stand in for real cameras.  Each
 * emulator:
 *   - serves scripted SOAP PullMessages responses (ONVIF events)
 *   - serves a real JPEG snapshot at GET /snapshot
 *
 * After the listener collects all events the following are verified:
 *
 *   SQLite -- events table
 *     * 3 rows total (Human x2 + Vehicle x1)
 *     * all type = 'smartDetectZone'
 *     * all "end" IS NOT NULL (no open/orphaned rows)
 *     * smartDetectTypes: 2 x '["person"]', 1 x '["vehicle"]'
 *     * all thumbnailId IS NOT NULL
 *     * per-camera counts correct
 *
 *   SQLite -- smartDetectObjects table
 *     * 3 rows total
 *     * 2 x type='person', 1 x type='vehicle'
 *     * all detectedAt > 0
 *     * all eventId references exist in events
 *
 *   UBV thumbnail files
 *     * cam108 file: 2 frames (human start + vehicle start)
 *     * cam109 file: 1 frame  (human start)
 *     * every frame contains valid JPEG bytes (FF D8 magic)
 */

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../detection_recorder.hpp"
#include "../onvif_listener.hpp"
#include "../ubv_thumbnail.hpp"
#include "onvif_camera_emulator.hpp"

// ============================================================
// Tiny test framework
// ============================================================
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg, const char* file, int line) {
  if (cond) {
    ++g_pass;
  } else {
    std::cerr << "  FAIL [" << file << ":" << line << "] " << msg << "\n";
    ++g_fail;
  }
}
#define CHECK(cond, msg) check((cond), (msg), __FILE__, __LINE__)

static bool run_test(const std::string& name, const std::function<void()>& fn) {
  int before = g_fail;
  std::cout << "[ RUN ] " << name << "\n";
  fn();
  bool ok = (g_fail == before);
  std::cout << (ok ? "[  OK ] " : "[FAIL ] ") << name << "\n\n";
  return ok;
}

// ============================================================
// SOAP building helpers
// ============================================================
static std::string make_create_response(const std::string& real_ip) {
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
    "<s:Body>"
    "<tev:CreatePullPointSubscriptionResponse>"
    "<tev:SubscriptionReference>"
    "<wsa5:Address>http://" + real_ip + "/onvif/subscription</wsa5:Address>"
    "</tev:SubscriptionReference>"
    "</tev:CreatePullPointSubscriptionResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

static std::string make_empty_pull_response() {
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    "            xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\">"
    "<s:Body><tev:PullMessagesResponse/></s:Body>"
    "</s:Envelope>";
}

// Camera 108 style: tns1:RuleEngine/FieldDetector/ObjectsInside
static std::string make_field_detector_response(
  const std::string& rule,
  bool               inside,
  const std::string& utc_time) {
  const std::string val = inside ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:RuleEngine/FieldDetector/ObjectsInside</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"Rule\" Value=\"" + rule + "\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"IsInside\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// CellMotionDetector style: tns1:RuleEngine/CellMotionDetector/Motion
static std::string make_cell_motion_response(bool is_motion, const std::string& utc_time) {
  const std::string val = is_motion ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:RuleEngine/CellMotionDetector/Motion</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"00000\"/>"
    "<tt:SimpleItem Name=\"VideoAnalyticsConfigurationToken\" Value=\"00000\"/>"
    "<tt:SimpleItem Name=\"Rule\" Value=\"00000\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"IsMotion\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// Fallback: tns1:VideoSource/MotionAlarm
static std::string make_motion_alarm_response(bool state, const std::string& utc_time) {
  const std::string val = state ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:VideoSource/MotionAlarm</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"VideoSourceMain\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"State\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// Camera 109 style: tns1:UserAlarm/IVA/HumanShapeDetect
static std::string make_human_shape_response(bool state, const std::string& utc_time) {
  const std::string val = state ? "true" : "false";
  return
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope"
    " xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
    " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>"
    "<tev:PullMessagesResponse>"
    "<wsnt:NotificationMessage>"
    "<wsnt:Topic>tns1:UserAlarm/IVA/HumanShapeDetect</wsnt:Topic>"
    "<wsnt:Message>"
    "<tt:Message UtcTime=\"" + utc_time + "\" PropertyOperation=\"Changed\">"
    "<tt:Source>"
    "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"VideoSourceMain\"/>"
    "</tt:Source>"
    "<tt:Data>"
    "<tt:SimpleItem Name=\"State\" Value=\"" + val + "\"/>"
    "</tt:Data>"
    "</tt:Message>"
    "</wsnt:Message>"
    "</wsnt:NotificationMessage>"
    "</tev:PullMessagesResponse>"
    "</s:Body>"
    "</s:Envelope>";
}

// ============================================================
// SnapshotSyntheticEmulator
//
// Serves scripted SOAP PullMessages responses AND a JPEG snapshot at
// GET /snapshot.  The snapshot URL is http://127.0.0.1:<port>/snapshot.
// ============================================================
class SnapshotSyntheticEmulator : public OnvifCameraEmulator {
 public:
  SnapshotSyntheticEmulator(const std::string&         real_ip,
                             std::vector<std::string>   pull_responses,
                             std::vector<unsigned char> snapshot_jpeg)
    : OnvifCameraEmulator(real_ip)
    , create_resp_(make_create_response(real_ip))
    , pull_responses_(std::move(pull_responses))
    , empty_pull_(make_empty_pull_response())
    , snapshot_jpeg_(std::move(snapshot_jpeg))
  {}

  /// Full URL at which this emulator serves JPEG snapshots.
  std::string snapshot_url() const {
    return "http://127.0.0.1:" + std::to_string(port()) + "/snapshot";
  }

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& /*body*/) override {
    // GET /snapshot -- no SOAPAction header, just return the JPEG bytes.
    if (soap_action.empty() && path == "/snapshot") {
      return {200, std::string(
        reinterpret_cast<const char*>(snapshot_jpeg_.data()),
        snapshot_jpeg_.size())};
    }

    // SOAP dispatch: extract the last segment of the action URI.
    auto   p    = soap_action.rfind('/');
    auto   tail = (p != std::string::npos) ? soap_action.substr(p + 1) : soap_action;

    if (tail == "CreatePullPointSubscriptionRequest")
      return {200, rewrite_urls(create_resp_)};

    if (tail == "PullMessagesRequest" || tail == "RenewRequest") {
      std::lock_guard<std::mutex> lk(pull_mu_);
      if (pull_idx_ < pull_responses_.size())
        return {200, rewrite_urls(pull_responses_[pull_idx_++])};
      return {200, empty_pull_};
    }

    return {400, ""};
  }

 private:
  std::string              create_resp_;
  std::vector<std::string> pull_responses_;
  std::string              empty_pull_;
  std::vector<unsigned char> snapshot_jpeg_;
  std::size_t              pull_idx_{0};
  std::mutex               pull_mu_;
};

// ============================================================
// Helpers
// ============================================================

// Load a binary file into a byte vector.
static std::vector<unsigned char> load_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    std::fprintf(stderr, "Fatal: Cannot open: %s\n", path.c_str());
    std::abort();
  }
  auto sz = static_cast<std::size_t>(f.tellg());
  f.seekg(0);
  std::vector<unsigned char> buf(sz);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
  return buf;
}

// Return the directory containing this source file (compile-time path).
static std::string source_dir() {
  std::string f = __FILE__;
  auto slash = f.rfind('/');
  return (slash != std::string::npos) ? f.substr(0, slash + 1) : "./";
}

// Run a SQLite query returning the integer value of the first column of the
// first row, or -1 on error.
static int db_count(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
  int val = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return val;
}

// ============================================================
// Shared emulator / listener helper
// ============================================================

struct TestContext {
  std::string                      db_path;
  std::string                      ubv_dir;
  onvif::CameraConfig              cfg108;
  onvif::CameraConfig              cfg109;
  std::unique_ptr<SnapshotSyntheticEmulator> emu108;
  std::unique_ptr<SnapshotSyntheticEmulator> emu109;
};

// Build and run emulators + listener for a standard 3-detection script.
// Returns after all 6 events (4 from cam108, 2 from cam109) have been seen.
// The recorder is expected to be fully configured by the caller before calling this.
static bool run_standard_script(TestContext& ctx,
                                onvif::DetectionRecorder& recorder) {
  const std::string dir = source_dir();

  std::vector<unsigned char> jpeg108 = load_file(dir + "testdata/snapshot_108.jpg");
  std::vector<unsigned char> jpeg109 = load_file(dir + "testdata/snapshot_109.jpg");

  std::vector<std::string> pulls_108 = {
    make_field_detector_response("Human",   true,  "2026-02-19T10:00:00Z"),
    make_field_detector_response("Vehicle", true,  "2026-02-19T10:00:01Z"),
    make_field_detector_response("Human",   false, "2026-02-19T10:00:10Z"),
    make_field_detector_response("Vehicle", false, "2026-02-19T10:00:11Z"),
  };
  std::vector<std::string> pulls_109 = {
    make_human_shape_response(true,  "2026-02-19T10:00:02Z"),
    make_human_shape_response(false, "2026-02-19T10:00:12Z"),
  };

  ctx.emu108 = std::make_unique<SnapshotSyntheticEmulator>(
    "192.168.1.108", std::move(pulls_108), jpeg108);
  ctx.emu109 = std::make_unique<SnapshotSyntheticEmulator>(
    "192.168.1.109", std::move(pulls_109), jpeg109);
  ctx.emu108->start();
  ctx.emu109->start();

  ctx.cfg108.ip                 = ctx.emu108->local_address();
  ctx.cfg108.user               = "admin";
  ctx.cfg108.password           = "test";
  ctx.cfg108.snapshot_url       = ctx.emu108->snapshot_url();
  ctx.cfg108.retry_interval_sec = 1;

  ctx.cfg109.ip                 = ctx.emu109->local_address();
  ctx.cfg109.user               = "user";
  ctx.cfg109.password           = "test";
  ctx.cfg109.snapshot_url       = ctx.emu109->snapshot_url();
  ctx.cfg109.retry_interval_sec = 1;

  recorder.set_ubv_dir(ctx.ubv_dir);
  recorder.set_snapshot(ctx.cfg108);
  recorder.set_snapshot(ctx.cfg109);

  std::mutex              mu;
  std::condition_variable cv;
  std::atomic<int>        events_seen{0};
  const int               needed = 6;

  onvif::OnvifListener listener;
  listener.add_camera(ctx.cfg108);
  listener.add_camera(ctx.cfg109);

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      recorder.on_event(ev);
      if (!ev.topic.empty())
        if (++events_seen >= needed) cv.notify_one();
    });
  });

  bool timed_out;
  {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(30),
                              [&] { return events_seen.load() >= needed; });
  }
  listener.stop();
  t.join();
  return !timed_out;
}

// ============================================================
// Tests
// ============================================================
static void test_detection_e2e(const std::string& db_path,
                                const std::string& ubv_dir) {
  TestContext ctx;
  ctx.db_path = db_path;
  ctx.ubv_dir = ubv_dir;

  // 2 s pre + 2 s post (defaults)
  auto rec_or = onvif::DetectionRecorder::Create(onvif::DbBackend::SQLite, db_path);
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::Create failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  bool ok = run_standard_script(ctx, recorder);

  CHECK(ok, "timed out before all detection events arrived");

  // =========================================================
  // Verify SQLite -- events table
  // =========================================================
  sqlite3* db = nullptr;
  sqlite3_open(db_path.c_str(), &db);

  int total_events = db_count(db, "SELECT COUNT(*) FROM events;");
  CHECK(total_events == 3,
        "expected 3 event rows, got " + std::to_string(total_events));

  int open_events = db_count(db,
    "SELECT COUNT(*) FROM events WHERE \"end\" IS NULL;");
  CHECK(open_events == 0,
        "expected 0 open events (no NULL end), got " + std::to_string(open_events));

  int zone_events = db_count(db,
    "SELECT COUNT(*) FROM events WHERE type='smartDetectZone';");
  CHECK(zone_events == 3,
        "expected 3 smartDetectZone events, got " + std::to_string(zone_events));

  int person_events = db_count(db,
    "SELECT COUNT(*) FROM events WHERE smartDetectTypes='[\"person\"]';");
  CHECK(person_events == 2,
        "expected 2 person events, got " + std::to_string(person_events));

  int vehicle_events = db_count(db,
    "SELECT COUNT(*) FROM events WHERE smartDetectTypes='[\"vehicle\"]';");
  CHECK(vehicle_events == 1,
        "expected 1 vehicle event, got " + std::to_string(vehicle_events));

  int no_thumb = db_count(db,
    "SELECT COUNT(*) FROM events WHERE thumbnailId IS NULL;");
  CHECK(no_thumb == 0,
        "expected all events to have thumbnailId, got "
        + std::to_string(no_thumb) + " without");

  {
    std::string sql = "SELECT COUNT(*) FROM events WHERE cameraId='"
                      + ctx.cfg108.ip + "';";
    int n = db_count(db, sql.c_str());
    CHECK(n == 2, "expected 2 events for cam108, got " + std::to_string(n));
  }
  {
    std::string sql = "SELECT COUNT(*) FROM events WHERE cameraId='"
                      + ctx.cfg109.ip + "';";
    int n = db_count(db, sql.c_str());
    CHECK(n == 1, "expected 1 event for cam109, got " + std::to_string(n));
  }

  // =========================================================
  // Verify SQLite -- smartDetectObjects table
  // =========================================================
  int total_sdo = db_count(db, "SELECT COUNT(*) FROM smartDetectObjects;");
  CHECK(total_sdo == 3,
        "expected 3 smartDetectObject rows, got " + std::to_string(total_sdo));

  int person_sdo = db_count(db,
    "SELECT COUNT(*) FROM smartDetectObjects WHERE type='person';");
  CHECK(person_sdo == 2,
        "expected 2 person SDOs, got " + std::to_string(person_sdo));

  int vehicle_sdo = db_count(db,
    "SELECT COUNT(*) FROM smartDetectObjects WHERE type='vehicle';");
  CHECK(vehicle_sdo == 1,
        "expected 1 vehicle SDO, got " + std::to_string(vehicle_sdo));

  int zero_detect = db_count(db,
    "SELECT COUNT(*) FROM smartDetectObjects WHERE detectedAt = 0;");
  CHECK(zero_detect == 0,
        "expected all SDOs to have non-zero detectedAt");

  int orphan_sdo = db_count(db,
    "SELECT COUNT(*) FROM smartDetectObjects"
    " WHERE eventId NOT IN (SELECT id FROM events);");
  CHECK(orphan_sdo == 0,
        "expected no orphan SDO rows, got " + std::to_string(orphan_sdo));

  // =========================================================
  // Verify buffer padding: end - start >= pre_buffer + post_buffer (4000 ms)
  // =========================================================
  {
    // With default 2 s pre + 2 s post the stored span must be >= 4000 ms.
    int padded = db_count(db,
      "SELECT COUNT(*) FROM events WHERE (\"end\" - start) >= 4000;");
    CHECK(padded == 3,
          "expected all 3 events to have end-start >= 4000 ms (2s pre + 2s post), got "
          + std::to_string(padded));
  }

  sqlite3_close(db);

  // =========================================================
  // Verify UBV thumbnail files
  // =========================================================
  const std::string ubv108 = ubv_dir + "/" + ctx.cfg108.ip + "_thumbnails.ubv";
  const std::string ubv109 = ubv_dir + "/" + ctx.cfg109.ip + "_thumbnails.ubv";

  // cam108: 2 frames (human start + vehicle start)
  {
    auto frames_or = ubv::decode(ubv108);
    if (!frames_or.ok()) {
      CHECK(false, std::string("ubv::decode failed for cam108: ")
                   + std::string(frames_or.status().message()));
    } else {
      const auto& frames = *frames_or;
      CHECK(frames.size() == 2,
            "expected 2 UBV frames for cam108, got " + std::to_string(frames.size()));
      for (std::size_t i = 0; i < frames.size(); ++i) {
        bool valid = frames[i].jpeg.size() >= 2
                  && frames[i].jpeg[0] == 0xff
                  && frames[i].jpeg[1] == 0xd8;
        CHECK(valid, "cam108 UBV frame " + std::to_string(i) + " is not a valid JPEG");
      }
    }
  }

  // cam109: 1 frame (human start)
  {
    auto frames_or = ubv::decode(ubv109);
    if (!frames_or.ok()) {
      CHECK(false, std::string("ubv::decode failed for cam109: ")
                   + std::string(frames_or.status().message()));
    } else {
      const auto& frames = *frames_or;
      CHECK(frames.size() == 1,
            "expected 1 UBV frame for cam109, got " + std::to_string(frames.size()));
      if (!frames.empty()) {
        bool valid = frames[0].jpeg.size() >= 2
                  && frames[0].jpeg[0] == 0xff
                  && frames[0].jpeg[1] == 0xd8;
        CHECK(valid, "cam109 UBV frame 0 is not a valid JPEG");
      }
    }
  }
}

// ============================================================
// Buffer padding test -- custom pre/post values
// ============================================================
static void test_buffer_padding(const std::string& db_path,
                                const std::string& ubv_dir) {
  TestContext ctx;
  ctx.db_path = db_path;
  ctx.ubv_dir = ubv_dir;

  const uint32_t pre_sec  = 1;
  const uint32_t post_sec = 3;
  const uint64_t min_span = (pre_sec + post_sec) * 1000;  // 4000 ms

  auto rec_or2 = onvif::DetectionRecorder::Create(onvif::DbBackend::SQLite, db_path);
  if (!rec_or2.ok()) {
    CHECK(false, std::string("DetectionRecorder::Create failed: ")
                 + std::string(rec_or2.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or2;
  recorder.set_buffer(pre_sec, post_sec);

  bool ok = run_standard_script(ctx, recorder);
  CHECK(ok, "buffer test: timed out before all events arrived");

  sqlite3* db = nullptr;
  sqlite3_open(db_path.c_str(), &db);

  // All events must have end-start >= pre+post ms.
  std::string sql = "SELECT COUNT(*) FROM events"
                    " WHERE (\"end\" - start) >= " + std::to_string(min_span) + ";";
  int padded = db_count(db, sql.c_str());
  CHECK(padded == 3,
        "expected all 3 events span >= " + std::to_string(min_span)
        + " ms (1s pre + 3s post), got " + std::to_string(padded));

  // end-start should NOT be >= (pre+post+1)*1000 -- sanity upper-bound
  // (events arrive in rapid succession so the raw interval is well under 1 s).
  std::string sql_upper = "SELECT COUNT(*) FROM events"
                          " WHERE (\"end\" - start) >= "
                          + std::to_string(min_span + 1000) + ";";
  int over = db_count(db, sql_upper.c_str());
  CHECK(over == 0,
        "expected no events with span >= " + std::to_string(min_span + 1000)
        + " ms (raw detection too short to reach that), got "
        + std::to_string(over));

  sqlite3_close(db);
}

// ============================================================
// Run a single-camera scripted listener test.
// Drives the given emulator until `needed` non-empty-topic events arrive.
// Returns false on timeout.
static bool run_single_camera(SnapshotSyntheticEmulator& emu,
                               onvif::DetectionRecorder&  recorder,
                               int                        needed) {
  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.snapshot_url       = emu.snapshot_url();
  cfg.retry_interval_sec = 1;
  recorder.set_snapshot(cfg);

  std::mutex              mu;
  std::condition_variable cv;
  std::atomic<int>        events_seen{0};

  onvif::OnvifListener listener;
  listener.add_camera(cfg);

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      recorder.on_event(ev);
      if (!ev.topic.empty())
        if (++events_seen >= needed) cv.notify_one();
    });
  });

  bool timed_out;
  {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(30),
                              [&] { return events_seen.load() >= needed; });
  }
  listener.stop();
  t.join();
  return !timed_out;
}

// ============================================================
// CellMotionDetector classification test
//
// Verifies that tns1:RuleEngine/CellMotionDetector/Motion events are
// classified as person detections (IsMotion=true → start, false → end).
// ============================================================
static void test_cell_motion_classification(const std::string& db_path,
                                             const std::string& ubv_dir) {
  auto rec_or = onvif::DetectionRecorder::Create(onvif::DbBackend::SQLite, db_path);
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::Create failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  // Single camera emulator with scripted cell-motion responses.
  const std::string real_ip = "192.168.1.200";
  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu(real_ip,
    {make_cell_motion_response(true,  "2026-03-21T10:00:00Z"),
     make_cell_motion_response(false, "2026-03-21T10:00:05Z")},
    jpeg);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "password";
  cfg.snapshot_url       = emu.snapshot_url();
  cfg.retry_interval_sec = 1;

  recorder.set_snapshot(cfg);

  std::mutex              mu;
  std::condition_variable cv;
  std::atomic<int>        events_seen{0};
  const int               needed = 2;

  onvif::OnvifListener listener;
  listener.add_camera(cfg);

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      recorder.on_event(ev);
      if (!ev.topic.empty())
        if (++events_seen >= needed) cv.notify_one();
    });
  });

  bool timed_out;
  {
    std::unique_lock<std::mutex> lk(mu);
    timed_out = !cv.wait_for(lk, std::chrono::seconds(30),
                              [&] { return events_seen.load() >= needed; });
  }

  listener.stop();
  t.join();

  CHECK(!timed_out, "timed out waiting for cell-motion events");
  CHECK(events_seen.load() >= 2, "expected >= 2 cell-motion events");

  sqlite3* db = nullptr;
  sqlite3_open(db_path.c_str(), &db);

  int events = db_count(db, "SELECT COUNT(*) FROM events;");
  CHECK(events == 1, "expected 1 detection interval, got " + std::to_string(events));

  int person_sdo = db_count(db,
    "SELECT COUNT(*) FROM smartDetectObjects WHERE type='person';");
  CHECK(person_sdo == 1,
        "expected 1 person SDO from CellMotionDetector, got "
        + std::to_string(person_sdo));

  sqlite3_close(db);
}

// ============================================================
// MotionAlarm fallback test
//
// A camera that only emits VideoSource/MotionAlarm (no CellMotionDetector,
// no AI events) should still produce a detection.
// ============================================================
static void test_motion_alarm_fallback(const std::string& db_path,
                                        const std::string& ubv_dir) {
  auto rec_or = onvif::DetectionRecorder::Create(onvif::DbBackend::SQLite, db_path);
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::Create failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu("192.168.1.201",
    {make_motion_alarm_response(true,  "2026-03-22T10:00:00Z"),
     make_motion_alarm_response(false, "2026-03-22T10:00:05Z")},
    jpeg);
  emu.start();

  bool ok = run_single_camera(emu, recorder, 2);
  CHECK(ok, "motion_alarm_fallback: timed out");

  sqlite3* db = nullptr;
  sqlite3_open(db_path.c_str(), &db);

  int events = db_count(db, "SELECT COUNT(*) FROM events;");
  CHECK(events == 1,
        "motion_alarm_fallback: expected 1 detection, got " + std::to_string(events));

  int person_sdo = db_count(db,
    "SELECT COUNT(*) FROM smartDetectObjects WHERE type='person';");
  CHECK(person_sdo == 1,
        "motion_alarm_fallback: expected 1 person SDO, got "
        + std::to_string(person_sdo));

  sqlite3_close(db);
}

// ============================================================
// CellMotionDetector suppresses MotionAlarm test
//
// When both CellMotionDetector and MotionAlarm fire for the same camera,
// only CellMotionDetector should produce a detection (MotionAlarm is suppressed).
// ============================================================
static void test_cell_motion_suppresses_alarm(const std::string& db_path,
                                               const std::string& ubv_dir) {
  auto rec_or = onvif::DetectionRecorder::Create(onvif::DbBackend::SQLite, db_path);
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::Create failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  // Script: CellMotion(true) then MotionAlarm(true) then both false.
  // MotionAlarm events should be silently ignored.
  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu("192.168.1.202",
    {make_cell_motion_response(true,  "2026-03-22T10:01:00Z"),
     make_motion_alarm_response(true,  "2026-03-22T10:01:00Z"),
     make_cell_motion_response(false, "2026-03-22T10:01:05Z"),
     make_motion_alarm_response(false, "2026-03-22T10:01:05Z")},
    jpeg);
  emu.start();

  bool ok = run_single_camera(emu, recorder, 4);
  CHECK(ok, "cell_motion_suppresses_alarm: timed out");

  sqlite3* db = nullptr;
  sqlite3_open(db_path.c_str(), &db);

  int events = db_count(db, "SELECT COUNT(*) FROM events;");
  CHECK(events == 1,
        "cell_motion_suppresses_alarm: expected 1 detection (not 2), got "
        + std::to_string(events));

  int open_events = db_count(db,
    "SELECT COUNT(*) FROM events WHERE \"end\" IS NULL;");
  CHECK(open_events == 0,
        "cell_motion_suppresses_alarm: expected 0 open events, got "
        + std::to_string(open_events));

  sqlite3_close(db);
}

// ============================================================
// AI events suppress CellMotionDetector test
//
// When a camera emits both AI events (FieldDetector) and CellMotionDetector,
// only the AI events should produce a detection (CellMotionDetector suppressed).
// This exercises the PTZ-patrol false-positive suppression path.
// ============================================================
static void test_ai_suppresses_cell_motion(const std::string& db_path,
                                            const std::string& ubv_dir) {
  auto rec_or = onvif::DetectionRecorder::Create(onvif::DbBackend::SQLite, db_path);
  if (!rec_or.ok()) {
    CHECK(false, std::string("DetectionRecorder::Create failed: ")
                 + std::string(rec_or.status().message()));
    return;
  }
  onvif::DetectionRecorder& recorder = **rec_or;
  recorder.set_ubv_dir(ubv_dir);

  // Script: AI event first, then CellMotion events.
  // CellMotion should be suppressed after the first AI event is seen.
  auto jpeg = load_file(source_dir() + "testdata/snapshot_108.jpg");
  SnapshotSyntheticEmulator emu("192.168.1.203",
    {make_field_detector_response("Human", true,  "2026-03-22T10:02:00Z"),
     make_cell_motion_response(true,              "2026-03-22T10:02:00Z"),
     make_field_detector_response("Human", false, "2026-03-22T10:02:05Z"),
     make_cell_motion_response(false,             "2026-03-22T10:02:05Z")},
    jpeg);
  emu.start();

  bool ok = run_single_camera(emu, recorder, 4);
  CHECK(ok, "ai_suppresses_cell_motion: timed out");

  sqlite3* db = nullptr;
  sqlite3_open(db_path.c_str(), &db);

  int events = db_count(db, "SELECT COUNT(*) FROM events;");
  CHECK(events == 1,
        "ai_suppresses_cell_motion: expected 1 detection (AI only, not 2), got "
        + std::to_string(events));

  int open_events = db_count(db,
    "SELECT COUNT(*) FROM events WHERE \"end\" IS NULL;");
  CHECK(open_events == 0,
        "ai_suppresses_cell_motion: expected 0 open events, got "
        + std::to_string(open_events));

  int person_sdo = db_count(db,
    "SELECT COUNT(*) FROM smartDetectObjects WHERE type='person';");
  CHECK(person_sdo == 1,
        "ai_suppresses_cell_motion: expected 1 person SDO, got "
        + std::to_string(person_sdo));

  sqlite3_close(db);
}

// ============================================================
// main
// ============================================================
int main() {
  const std::string db_path           = "/tmp/test_detections.db";
  const std::string db_path_buf       = "/tmp/test_detections_buf.db";
  const std::string db_path_cell      = "/tmp/test_detections_cell.db";
  const std::string db_path_alarm     = "/tmp/test_detections_alarm.db";
  const std::string db_path_suppress  = "/tmp/test_detections_suppress.db";
  const std::string db_path_ai_sup    = "/tmp/test_detections_ai_sup.db";
  const std::string ubv_dir           = "/tmp/test_dr_thumbs";

  std::remove(db_path.c_str());
  std::remove(db_path_buf.c_str());
  std::remove(db_path_cell.c_str());
  std::remove(db_path_alarm.c_str());
  std::remove(db_path_suppress.c_str());
  std::remove(db_path_ai_sup.c_str());

  onvif::global_init();

  run_test("detection_e2e",             [&] { test_detection_e2e(db_path, ubv_dir); });
  run_test("buffer_padding",            [&] { test_buffer_padding(db_path_buf, ubv_dir); });
  run_test("cell_motion_classification",
           [&] { test_cell_motion_classification(db_path_cell, ubv_dir); });
  run_test("motion_alarm_fallback",
           [&] { test_motion_alarm_fallback(db_path_alarm, ubv_dir); });
  run_test("cell_motion_suppresses_alarm",
           [&] { test_cell_motion_suppresses_alarm(db_path_suppress, ubv_dir); });
  run_test("ai_suppresses_cell_motion",
           [&] { test_ai_suppresses_cell_motion(db_path_ai_sup, ubv_dir); });

  onvif::global_cleanup();

  std::cout << "================================================\n"
            << "Results: " << g_pass << " checks passed, "
            << g_fail    << " checks failed\n"
            << "================================================\n";

  return g_fail > 0 ? 1 : 0;
}
