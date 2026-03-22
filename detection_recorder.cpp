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

#include "detection_recorder.hpp"

#include <curl/curl.h>
#include <libpq-fe.h>
#include <sqlite3.h>
#include <sys/stat.h>

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "ubv_thumbnail.hpp"

namespace onvif {

// ============================================================
// Event classifier (file-local)
// ============================================================
namespace {

struct Detection {
  std::string type;    // "human" or "vehicle"
  bool        started;  // true = detection began, false = detection ended
  std::string time;    // ISO-8601 UTC event_time from the OnvifEvent
};

std::optional<Detection> classify(const OnvifEvent& ev) {
  // --- Camera 108 style: FieldDetector ObjectsInside ---
  if (ev.topic == "tns1:RuleEngine/FieldDetector/ObjectsInside") {
    auto rule_it   = ev.source.find("Rule");
    auto inside_it = ev.data.find("IsInside");
    if (rule_it == ev.source.end() || inside_it == ev.data.end())
      return {};

    std::string type;
    if      (rule_it->second == "Human")   type = "human";
    else if (rule_it->second == "Vehicle") type = "vehicle";
    else return {};

    return Detection{type, inside_it->second == "true", ev.event_time};
  }

  // --- Camera 109 style: HumanShapeDetect ---
  if (ev.topic == "tns1:UserAlarm/IVA/HumanShapeDetect") {
    auto it = ev.data.find("State");
    if (it == ev.data.end()) return {};
    return Detection{"human", it->second == "true", ev.event_time};
  }

  // --- Generic CellMotionDetector/Motion (Amcrest, Lorex, Dahua, etc.) ---
  // Basic pixel-change motion; no object class available so treated as person.
  if (ev.topic == "tns1:RuleEngine/CellMotionDetector/Motion") {
    auto it = ev.data.find("IsMotion");
    if (it == ev.data.end()) return {};
    return Detection{"human", it->second == "true", ev.event_time};
  }

  return {};
}

}  // anonymous namespace

// ============================================================
// Timestamp and ID helpers (file-local)
// ============================================================
namespace {

// Current time as milliseconds since Unix epoch.
static uint64_t now_ms() {
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

// Current time as ISO-8601 UTC string: "YYYY-MM-DDTHH:MM:SS.mmmZ"
static std::string utc_now_iso8601() {
  auto   now = std::chrono::system_clock::now();
  auto   t   = std::chrono::system_clock::to_time_t(now);
  auto   ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
  struct tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  char out[48];
  std::snprintf(out, sizeof(out), "%s.%03dZ", buf, static_cast<int>(ms.count()));
  return out;
}

// Generate a UUID v4 string.
static std::string generate_uuid() {
  static std::random_device rd;
  thread_local std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  uint64_t hi = dis(gen);
  uint64_t lo = dis(gen);

  hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
  lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

  char buf[37];
  std::snprintf(buf, sizeof(buf),
    "%08x-%04x-%04x-%04x-%012" PRIx64,
    static_cast<unsigned>(hi >> 32),
    static_cast<unsigned>((hi >> 16) & 0xFFFF),
    static_cast<unsigned>(hi & 0xFFFF),
    static_cast<unsigned>((lo >> 48) & 0xFFFF),
    static_cast<uint64_t>(lo & 0x0000FFFFFFFFFFFFull));
  return buf;
}

// Generate a 24-char lowercase hex ID (12 random bytes).
// Protect routes thumbnailIds with length == 24 to its local DB (thumbnails
// table) rather than to the msp media server, so this format lets Protect
// serve thumbnails we insert directly.
static std::string generate_24hex_id() {
  static std::random_device rd2;
  thread_local std::mt19937_64 gen(rd2());
  std::uniform_int_distribution<uint64_t> dis;
  uint64_t a = dis(gen);
  uint64_t b = dis(gen);
  char buf[25];
  std::snprintf(buf, sizeof(buf), "%016" PRIx64 "%08" PRIx64,
                static_cast<uint64_t>(a),
                static_cast<uint64_t>(b & 0xFFFFFFFFull));
  return buf;
}

// Map our internal detection type to the Ubiquiti smartDetect object type.
static const char* sdo_type(const std::string& det_type) {
  if (det_type == "vehicle") return "vehicle";
  return "person";  // "human" -> "person"
}

// Build the smartDetectTypes JSON array from our detection type.
static std::string smart_detect_types_json(const std::string& det_type) {
  if (det_type == "vehicle") return "[\"vehicle\"]";
  return "[\"person\"]";
}

}  // anonymous namespace

// ============================================================
// Snapshot fetch helpers (file-local)
// ============================================================
namespace {

static size_t curl_write_cb(void* data, size_t size, size_t nmemb, void* userp) {
  auto* buf = static_cast<std::vector<unsigned char>*>(userp);
  const size_t total = size * nmemb;
  const auto*  bytes = static_cast<unsigned char*>(data);
  buf->insert(buf->end(), bytes, bytes + total);
  return total;
}

static std::vector<unsigned char> fetch_snapshot(const std::string& url,
                                                  const std::string& user,
                                                  const std::string& password) {
  std::vector<unsigned char> buf;
  CURL* curl = curl_easy_init();
  if (!curl) return buf;

  curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  curl_write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &buf);

  if (!user.empty()) {
    std::string userpwd = user + ":" + password;
    curl_easy_setopt(curl, CURLOPT_USERPWD,  userpwd.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,  // NOLINT(runtime/int)
                     static_cast<long>(CURLAUTH_DIGEST | CURLAUTH_BASIC));  // NOLINT(runtime/int)
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    long http_code = 0;  // NOLINT(runtime/int)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) buf.clear();
  } else {
    buf.clear();
  }

  curl_easy_cleanup(curl);
  return buf;
}

}  // anonymous namespace

// ============================================================
// SQLite backend
// ============================================================
namespace {

struct SqliteBackend final : DetectionRecorder::IDbBackend {
  sqlite3* db_{nullptr};

  static absl::StatusOr<std::unique_ptr<SqliteBackend>> Create(
      const std::string& path) {
    auto b = std::make_unique<SqliteBackend>(path);
    if (!b->db_) {
      return absl::InternalError("SQLite open failed: " + path);
    }
    return b;
  }

  explicit SqliteBackend(const std::string& path) {
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
      // Store error; db_ may still need closing
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      return;
    }
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  }

  ~SqliteBackend() override {
    if (db_) sqlite3_close(db_);
  }

  absl::Status create_schema() override {
    const char* sql =
      "CREATE TABLE IF NOT EXISTS events ("
      "  id                        TEXT PRIMARY KEY,"
      "  type                      TEXT,"
      "  start                     INTEGER,"
      "  \"end\"                   INTEGER,"
      "  cameraId                  TEXT,"
      "  score                     INTEGER NOT NULL DEFAULT 0,"
      "  smartDetectTypes          TEXT    NOT NULL DEFAULT '[]',"
      "  metadata                  TEXT    NOT NULL DEFAULT '{}',"
      "  locked                    INTEGER NOT NULL DEFAULT 0,"
      "  thumbnailId               TEXT,"
      "  thumbnailFullfovId        TEXT,"
      "  packageThumbnailId        TEXT,"
      "  packageThumbnailFullfovId TEXT,"
      "  deletedAt                 TEXT,"
      "  deletionType              TEXT,"
      "  userId                    TEXT,"
      "  partitionId               TEXT,"
      "  createdAt                 TEXT NOT NULL,"
      "  updatedAt                 TEXT NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS events_cameraId_idx"
      "  ON events(cameraId);"
      "CREATE INDEX IF NOT EXISTS events_start_idx"
      "  ON events(start DESC) WHERE deletedAt IS NULL;"
      "CREATE INDEX IF NOT EXISTS events_type_end_idx"
      "  ON events(type, \"end\", deletedAt);"

      "CREATE TABLE IF NOT EXISTS smartDetectObjects ("
      "  id                        TEXT PRIMARY KEY,"
      "  eventId                   TEXT NOT NULL,"
      "  thumbnailId               TEXT,"
      "  cameraId                  TEXT NOT NULL,"
      "  type                      TEXT NOT NULL,"
      "  attributes                TEXT NOT NULL DEFAULT '{}',"
      "  smartDetectObjectGroupId  TEXT,"
      "  detectedAt                INTEGER NOT NULL,"
      "  metadata                  TEXT NOT NULL DEFAULT '{}',"
      "  createdAt                 TEXT NOT NULL,"
      "  updatedAt                 TEXT NOT NULL"
      ");"
      "CREATE INDEX IF NOT EXISTS sdo_eventId_idx"
      "  ON smartDetectObjects(eventId);"
      "CREATE INDEX IF NOT EXISTS sdo_cameraId_idx"
      "  ON smartDetectObjects(cameraId);"
      "CREATE INDEX IF NOT EXISTS sdo_type_idx"
      "  ON smartDetectObjects(type);"
      "CREATE INDEX IF NOT EXISTS sdo_detectedAt_idx"
      "  ON smartDetectObjects(detectedAt);";

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
      std::string msg = errmsg ? errmsg : "unknown error";
      sqlite3_free(errmsg);
      return absl::InternalError("SQLite schema creation failed: " + msg);
    }
    return absl::OkStatus();
  }

  void register_camera(const std::string& /*ip*/,
                       const std::string& /*id*/,
                       const std::string& /*mac*/) override {}

  std::string make_thumbnail_id(const std::string& camera_ip,
                                uint64_t           ts_ms) override {
    return camera_ip + "-" + std::to_string(ts_ms);
  }

  bool needs_snapshot() const override { return true; }

  void insert_event(const std::string& id,
                    uint64_t           ts_ms,
                    const std::string& camera_ip,
                    const std::string& sdt_json,
                    const std::string& thumb_id,
                    const std::string& now_str) override {
    const char* sql =
      "INSERT INTO events"
      " (id, type, start, cameraId, score, smartDetectTypes,"
      "  metadata, locked, thumbnailId, createdAt, updatedAt)"
      " VALUES (?, 'smartDetectZone', ?, ?, 0, ?,"
      "         '{}', 0, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,  1, id.c_str(),        -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(ts_ms));
    sqlite3_bind_text(stmt,  3, camera_ip.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  4, sdt_json.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  5, thumb_id.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  6, now_str.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  7, now_str.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void insert_sdo(const std::string& id,
                  const std::string& event_id,
                  const std::string& thumb_id,
                  const std::string& camera_ip,
                  const std::string& obj_type,
                  const std::string& attributes,
                  uint64_t           ts_ms,
                  const std::string& now_str) override {
    const char* sql =
      "INSERT INTO smartDetectObjects"
      " (id, eventId, thumbnailId, cameraId, type, attributes,"
      "  detectedAt, metadata, createdAt, updatedAt)"
      " VALUES (?, ?, ?, ?, ?, ?, ?, '{}', ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,  1, id.c_str(),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  2, event_id.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  3, thumb_id.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  4, camera_ip.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  5, obj_type.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  6, attributes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(ts_ms));
    sqlite3_bind_text(stmt,  8, now_str.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  9, now_str.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void update_event_end(const std::string& event_id,
                        uint64_t           end_ms,
                        const std::string& now_str) override {
    const char* sql =
      "UPDATE events SET \"end\" = ?, updatedAt = ? WHERE id = ?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(end_ms));
    sqlite3_bind_text(stmt,  2, now_str.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  3, event_id.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void write_thumbnail(const std::string& /*thumb_id*/,
                       const std::string& /*event_id*/,
                       const std::string& /*camera_ip*/,
                       uint64_t           /*ts_ms*/,
                       const std::string& /*now_str*/,
                       const std::vector<unsigned char>& /*jpeg*/) override {}
};

// ============================================================
// PostgreSQL backend
// ============================================================
struct PgBackend final : DetectionRecorder::IDbBackend {
  PGconn* conn_{nullptr};

  // Camera IP -> Protect database UUID and MAC.  Populated by register_camera()
  // before the listener starts, then read-only.
  std::map<std::string, std::string> ip_to_id_;
  std::map<std::string, std::string> ip_to_mac_;

  static absl::StatusOr<std::unique_ptr<PgBackend>> Create(
      const std::string& conninfo) {
    auto b = std::make_unique<PgBackend>(conninfo);
    if (!b->conn_) {
      return absl::InternalError("PostgreSQL connect failed");
    }
    return b;
  }

  explicit PgBackend(const std::string& conninfo) {
    conn_ = PQconnectdb(conninfo.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
      PQfinish(conn_);
      conn_ = nullptr;
    }
  }

  ~PgBackend() override {
    if (conn_) PQfinish(conn_);
  }

  // Verify the expected tables are accessible.  The schema already exists
  // in UniFi Protect's database -- we never try to create it.
  absl::Status create_schema() override {
    PGresult* res = PQexec(conn_,
      "SELECT 1 FROM events LIMIT 0;"
      "SELECT 1 FROM \"smartDetectObjects\" LIMIT 0;"
      "SELECT 1 FROM thumbnails LIMIT 0;");
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK) {
      std::string msg = PQresultErrorMessage(res);
      PQclear(res);
      return absl::InternalError("PostgreSQL table check failed: " + msg);
    }
    PQclear(res);
    return absl::OkStatus();
  }

  void register_camera(const std::string& ip,
                       const std::string& id,
                       const std::string& mac) override {
    if (!id.empty())  ip_to_id_[ip]  = id;
    if (!mac.empty()) ip_to_mac_[ip] = mac;
  }

  std::string make_thumbnail_id(const std::string& /*camera_ip*/,
                                uint64_t           /*ts_ms*/) override {
    // 24-char hex IDs are routed by Protect to its thumbnails DB table
    // (not to msp TCP), letting us insert JPEG data directly.
    return generate_24hex_id();
  }

  bool needs_snapshot() const override { return true; }

  // Look up camera UUID; falls back to the IP string if not registered
  // (prevents a crash while still giving a diagnostic clue in the DB).
  const std::string& camera_id(const std::string& ip) const {
    auto it = ip_to_id_.find(ip);
    return (it != ip_to_id_.end()) ? it->second : ip;
  }

  // Execute a parameterized DML statement; logs errors to stderr (non-fatal).
  void exec_params(const char*        sql,
                   int                nparams,
                   const char* const* params,
                   const int*         lengths = nullptr,
                   const int*         formats = nullptr) {
    PGresult* res = PQexecParams(conn_, sql, nparams, nullptr,
                                 params, lengths, formats, 0);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
      std::fprintf(stderr, "[pg] query failed: %s\n", PQresultErrorMessage(res));
    PQclear(res);
  }

  void insert_event(const std::string& id,
                    uint64_t           ts_ms,
                    const std::string& camera_ip,
                    const std::string& sdt_json,
                    const std::string& thumb_id,
                    const std::string& now_str) override {
    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts     = std::to_string(ts_ms);
    const char* params[] = {
      id.c_str(), ts.c_str(), cam_id.c_str(),
      sdt_json.c_str(), thumb_id.c_str(), now_str.c_str(), now_str.c_str()
    };
    // locked = false (boolean literal); metadata/score are literals.
    // $4::json casts the JSON text to the json column type.
    exec_params(
      "INSERT INTO events"
      " (id, type, start, \"cameraId\", score, \"smartDetectTypes\","
      "  metadata, locked, \"thumbnailId\", \"createdAt\", \"updatedAt\")"
      " VALUES ($1, 'smartDetectZone', $2::bigint, $3, 0, $4::json,"
      "         '{}'::json, false, $5, $6, $7)",
      7, params);
  }

  void insert_sdo(const std::string& id,
                  const std::string& event_id,
                  const std::string& thumb_id,
                  const std::string& camera_ip,
                  const std::string& obj_type,
                  const std::string& attributes,
                  uint64_t           ts_ms,
                  const std::string& now_str) override {
    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts     = std::to_string(ts_ms);
    const char* params[] = {
      id.c_str(), event_id.c_str(), thumb_id.c_str(), cam_id.c_str(),
      obj_type.c_str(), attributes.c_str(), ts.c_str(),
      now_str.c_str(), now_str.c_str()
    };
    exec_params(
      "INSERT INTO \"smartDetectObjects\""
      " (id, \"eventId\", \"thumbnailId\", \"cameraId\", type, attributes,"
      "  \"detectedAt\", metadata, \"createdAt\", \"updatedAt\")"
      " VALUES ($1, $2, $3, $4, $5, $6::json, $7::bigint,"
      "         '{}'::jsonb, $8, $9)",
      9, params);
  }

  void update_event_end(const std::string& event_id,
                        uint64_t           end_ms,
                        const std::string& now_str) override {
    const std::string ts = std::to_string(end_ms);
    const char* params[] = { ts.c_str(), now_str.c_str(), event_id.c_str() };
    exec_params(
      "UPDATE events SET \"end\" = $1::bigint, \"updatedAt\" = $2 WHERE id = $3",
      3, params);
  }

  // Insert JPEG thumbnail directly into Protect's thumbnails table.
  // Protect serves 24-char thumbnailIds from this table (not via msp TCP).
  void write_thumbnail(const std::string&              thumb_id,
                       const std::string&              event_id,
                       const std::string&              camera_ip,
                       uint64_t                        ts_ms,
                       const std::string&              now_str,
                       const std::vector<unsigned char>& jpeg) override {
    if (jpeg.empty()) return;

    const std::string& cam_id = camera_id(camera_ip);
    const std::string  ts     = std::to_string(ts_ms);

    // Binary parameters: thumb_id, cam_id, event_id, ts, now_str, now_str
    // + jpeg (binary, format=1)
    const char* params[7] = {
      thumb_id.c_str(),
      cam_id.c_str(),
      event_id.c_str(),
      ts.c_str(),
      now_str.c_str(),
      now_str.c_str(),
      reinterpret_cast<const char*>(jpeg.data())
    };
    const int lengths[7] = {
      0, 0, 0, 0, 0, 0,
      static_cast<int>(jpeg.size())
    };
    const int formats[7] = { 0, 0, 0, 0, 0, 0, 1 };  // $7 is binary

    PGresult* res = PQexecParams(conn_,
      "INSERT INTO thumbnails"
      " (id, \"cameraId\", \"eventId\", timestamp, \"createdAt\","
      "  \"updatedAt\", content, \"isFullfov\")"
      " VALUES ($1, $2, $3, $4::bigint, $5, $6, $7, false)"
      " ON CONFLICT (id) DO NOTHING",
      7, nullptr, params, lengths, formats, 0);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
      std::fprintf(stderr, "[pg] thumbnail insert failed: %s\n",
                   PQresultErrorMessage(res));
    PQclear(res);
  }
};

}  // anonymous namespace

// ============================================================
// DetectionRecorder
// ============================================================

// static factory
absl::StatusOr<std::unique_ptr<DetectionRecorder>> DetectionRecorder::Create(
    DbBackend backend, const std::string& conn) {
  auto dr = std::unique_ptr<DetectionRecorder>(new DetectionRecorder());
  switch (backend) {
    case DbBackend::SQLite: {
      auto b_or = SqliteBackend::Create(conn);
      if (!b_or.ok()) return b_or.status();
      dr->db_ = std::move(*b_or);
      break;
    }
    case DbBackend::PostgreSQL: {
      auto b_or = PgBackend::Create(conn);
      if (!b_or.ok()) return b_or.status();
      dr->db_ = std::move(*b_or);
      break;
    }
  }
  absl::Status s = dr->db_->create_schema();
  if (!s.ok()) return s;
  return dr;
}

DetectionRecorder::~DetectionRecorder() = default;

void DetectionRecorder::set_snapshot(const CameraConfig& cam) {
  std::lock_guard<std::mutex> lk(mu_);
  snapshot_info_[cam.ip] = {cam.snapshot_url, cam.user, cam.password};
  db_->register_camera(cam.ip, cam.id, cam.mac);
}

void DetectionRecorder::set_buffer(uint32_t pre_sec, uint32_t post_sec) {
  std::lock_guard<std::mutex> lk(mu_);
  pre_buffer_ms_  = static_cast<uint64_t>(pre_sec)  * 1000;
  post_buffer_ms_ = static_cast<uint64_t>(post_sec) * 1000;
}

void DetectionRecorder::set_ubv_dir(const std::string& dir) {
  std::lock_guard<std::mutex> lk(mu_);
  ubv_dir_ = dir;
  struct stat st{};
  if (stat(dir.c_str(), &st) != 0)
    mkdir(dir.c_str(), 0755);
}

void DetectionRecorder::on_event(const OnvifEvent& ev) {
  auto det = classify(ev);
  if (!det) return;

  auto key = std::make_pair(ev.camera_ip, det->type);

  if (det->started) {
    // 1. Look up snapshot + UBV config and buffer settings (brief lock, no I/O).
    std::string snap_url, snap_user, snap_pass, ubv_dir;
    uint64_t pre_ms;
    {
      std::lock_guard<std::mutex> lk(mu_);
      auto it = snapshot_info_.find(ev.camera_ip);
      if (it != snapshot_info_.end()) {
        snap_url  = it->second.url;
        snap_user = it->second.user;
        snap_pass = it->second.password;
      }
      ubv_dir = ubv_dir_;
      pre_ms  = pre_buffer_ms_;
    }

    // 2. Compute timestamps and IDs (no lock -- needs ip_to_mac_ which is read-only).
    const uint64_t    ts_ms      = now_ms() - pre_ms;  // padded start
    const std::string now_str    = utc_now_iso8601();
    const std::string event_id   = generate_uuid();
    const std::string sdo_id     = generate_uuid();
    const std::string thumb_id   = db_->make_thumbnail_id(ev.camera_ip, ts_ms);
    const std::string sdt_json   = smart_detect_types_json(det->type);
    const std::string obj_type   = sdo_type(det->type);
    const std::string attributes = "{\"confidence\":0}";

    // 3. Fetch snapshot only if the backend needs it (SQLite UBV; not PG).
    std::vector<unsigned char> snapshot;
    if (db_->needs_snapshot() && !snap_url.empty())
      snapshot = fetch_snapshot(snap_url, snap_user, snap_pass);

    // 4. INSERT into both tables, write thumbnail -- all under lock.
    std::lock_guard<std::mutex> lk(mu_);

    db_->insert_event(event_id, ts_ms, ev.camera_ip, sdt_json, thumb_id, now_str);
    db_->insert_sdo(sdo_id, event_id, thumb_id, ev.camera_ip,
                    obj_type, attributes, ts_ms, now_str);
    open_[key] = event_id;

    // 4d. Thumbnail: UBV file (SQLite). PG write_thumbnail is a no-op.
    if (!snapshot.empty()) {
      if (!ubv_dir.empty()) {
        const std::string ubv_path =
          ubv_dir + "/" + ev.camera_ip + "_thumbnails.ubv";
        auto s = ubv::append(ubv_path, {ts_ms, snapshot});
        if (!s.ok()) {
          std::fprintf(stderr, "[ubv] append failed (non-fatal): %s\n",
                       std::string(s.message()).c_str());
        }
      }
      db_->write_thumbnail(thumb_id, event_id, ev.camera_ip,
                           ts_ms, now_str, snapshot);
    }

  } else {
    // Detection ended -- UPDATE the open events row with end time + updatedAt.
    std::lock_guard<std::mutex> lk(mu_);
    auto it = open_.find(key);
    if (it == open_.end()) return;

    const uint64_t    end_ms  = now_ms() + post_buffer_ms_;  // padded end
    const std::string now_str = utc_now_iso8601();

    db_->update_event_end(it->second, end_ms, now_str);
    open_.erase(it);
  }
}

}  // namespace onvif
