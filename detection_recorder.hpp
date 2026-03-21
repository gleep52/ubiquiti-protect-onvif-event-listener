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

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "onvif_listener.hpp"

namespace onvif {

enum class DbBackend { SQLite, PostgreSQL };

/**
 * DetectionRecorder
 *
 * Translates raw ONVIF events into human/vehicle detections and persists them
 * to a SQLite or PostgreSQL database using a schema that mirrors the UniFi
 * Protect event tables, making third-party camera data structurally identical
 * to data produced by native Ubiquiti cameras.
 *
 * Supported ONVIF event formats
 * ------------------------------
 * Camera 108 style  (tns1:RuleEngine/FieldDetector/ObjectsInside)
 *   source["Rule"]     = "Human" | "Vehicle"
 *   data["IsInside"]   = "true"  | "false"
 *
 * Camera 109 style  (tns1:UserAlarm/IVA/HumanShapeDetect)
 *   data["State"]      = "true"  | "false"   (always maps to "person")
 *
 * Detection type mapping
 * ----------------------
 *   ONVIF "human"   -> smartDetectTypes ["person"], smartDetectObjects.type "person"
 *   ONVIF "vehicle" -> smartDetectTypes ["vehicle"], smartDetectObjects.type "vehicle"
 *
 * Backend selection
 * -----------------
 *   DbBackend::SQLite     -- conn is a file path (e.g. "onvif_detections.db")
 *   DbBackend::PostgreSQL -- conn is a libpq conninfo string
 *                           (e.g. "host=localhost dbname=unifi user=protect")
 *
 * Database schema (mirrors UniFi Protect, auto-created on construction)
 * -----------------------------------------------------------------------
 *
 *   events (
 *     id TEXT PK,                            -- UUID v4
 *     type TEXT,                             -- 'smartDetectZone'
 *     start INTEGER/BIGINT,                  -- ms since Unix epoch
 *     end INTEGER/BIGINT,                    -- ms since Unix epoch; NULL while active
 *     cameraId TEXT,                         -- camera IP address
 *     score INTEGER DEFAULT 0,
 *     smartDetectTypes TEXT DEFAULT '[]',    -- JSON array, e.g. '["person"]'
 *     metadata TEXT DEFAULT '{}',            -- JSON object
 *     locked INTEGER DEFAULT 0,
 *     thumbnailId TEXT,                      -- '<cameraIP>-<start_ms>'
 *     thumbnailFullfovId TEXT,
 *     packageThumbnailId TEXT,
 *     packageThumbnailFullfovId TEXT,
 *     deletedAt TEXT,
 *     deletionType TEXT,
 *     userId TEXT,
 *     partitionId TEXT,
 *     createdAt TEXT NOT NULL,               -- ISO-8601 UTC
 *     updatedAt TEXT NOT NULL                -- ISO-8601 UTC
 *   )
 *
 *   smartDetectObjects (
 *     id TEXT PK,                            -- UUID v4
 *     eventId TEXT NOT NULL,                 -- -> events.id
 *     thumbnailId TEXT,                      -- '<cameraIP>-<start_ms>'
 *     cameraId TEXT NOT NULL,                -- camera IP address
 *     type TEXT NOT NULL,                    -- 'person' or 'vehicle'
 *     attributes TEXT DEFAULT '{}',          -- JSON: {"confidence":0}
 *     smartDetectObjectGroupId TEXT,
 *     detectedAt INTEGER/BIGINT NOT NULL,    -- ms since Unix epoch
 *     metadata TEXT DEFAULT '{}',            -- JSON object
 *     createdAt TEXT NOT NULL,
 *     updatedAt TEXT NOT NULL
 *   )
 *
 * Thread safety
 * -------------
 * on_event() is fully thread-safe; the OnvifListener may call it from
 * multiple camera threads simultaneously.
 * set_snapshot() and set_ubv_dir() must be called before the listener starts.
 */
class DetectionRecorder {
 public:
  DetectionRecorder(DbBackend backend, const std::string& conn);
  ~DetectionRecorder();

  DetectionRecorder(const DetectionRecorder&)            = delete;
  DetectionRecorder& operator=(const DetectionRecorder&) = delete;

  /// Register snapshot credentials for a camera. Must be called before run().
  void set_snapshot(const CameraConfig& cam);

  /// Set the directory where per-camera UBV thumbnail files are written.
  /// Each camera gets its own file: <dir>/<camera_ip>_thumbnails.ubv
  /// If not called, UBV files are not written (snapshots are still fetched
  /// for the thumbnailId reference if a snapshot URL is configured).
  /// Must be called before run().
  void set_ubv_dir(const std::string& dir);

  /// Set pre/post buffer padding applied to stored timestamps.
  /// start is stored as (detection_time - pre_sec*1000).
  /// end   is stored as (detection_end_time + post_sec*1000).
  /// Defaults: 2 s pre, 2 s post. Must be called before run().
  void set_buffer(uint32_t pre_sec, uint32_t post_sec);

  /// Process one ONVIF event. Ignores events that are not human/vehicle
  /// detections. Thread-safe.
  void on_event(const OnvifEvent& ev);

  // Defined in detection_recorder.cpp -- public so concrete backends in the
  // .cpp translation unit can inherit from it without friendship.
  struct IDbBackend {
    virtual ~IDbBackend() = default;

    virtual void create_schema() = 0;

    /// Register a camera's identifiers before the listener starts.
    /// SQLite: no-op.  PG: stores ip->id and ip->mac for later lookups.
    virtual void register_camera(const std::string& ip,
                                 const std::string& id,
                                 const std::string& mac) = 0;

    /// Compute the thumbnailId string for an event.
    /// SQLite: "<camera_ip>-<ts_ms>"
    /// PG:     "<MAC_UPPERCASE_NOCOLONS>-<ts_ms>"  (media server serves
    ///         the thumbnail directly from the UBV recording on demand)
    virtual std::string make_thumbnail_id(const std::string& camera_ip,
                                          uint64_t           ts_ms) = 0;

    /// True when the backend needs a snapshot fetched on detection.
    /// SQLite: true (written to UBV).  PG: false (media server has UBV).
    virtual bool needs_snapshot() const = 0;

    virtual void insert_event(const std::string& id,
                              uint64_t           ts_ms,
                              const std::string& camera_ip,
                              const std::string& sdt_json,
                              const std::string& thumb_id,
                              const std::string& now_str) = 0;

    virtual void insert_sdo(const std::string& id,
                            const std::string& event_id,
                            const std::string& thumb_id,
                            const std::string& camera_ip,
                            const std::string& obj_type,
                            const std::string& attributes,
                            uint64_t           ts_ms,
                            const std::string& now_str) = 0;

    virtual void update_event_end(const std::string& event_id,
                                  uint64_t           end_ms,
                                  const std::string& now_str) = 0;

    /// Store a JPEG thumbnail.  For PG: INSERT INTO thumbnails.
    /// For SQLite: no-op (UBV appending is handled by DetectionRecorder).
    virtual void write_thumbnail(const std::string&              thumb_id,
                                 const std::string&              event_id,
                                 const std::string&              camera_ip,
                                 uint64_t                        ts_ms,
                                 const std::string&              now_str,
                                 const std::vector<unsigned char>& jpeg) = 0;
  };

 private:
  struct SnapshotInfo {
    std::string url;
    std::string user;
    std::string password;
  };

  std::unique_ptr<IDbBackend> db_;
  std::mutex mu_;

  uint64_t pre_buffer_ms_{2000};   // subtracted from event start timestamp
  uint64_t post_buffer_ms_{2000};  // added to event end timestamp

  // Snapshot info per camera IP -- written before run(), read-only after.
  std::map<std::string, SnapshotInfo> snapshot_info_;

  // Directory for per-camera UBV thumbnail files; empty = disabled.
  std::string ubv_dir_;

  // Tracks the UUID of each open (not-yet-ended) event row in `events`.
  // Key: (camera_ip, detection_type)
  std::map<std::pair<std::string, std::string>, std::string> open_;
};

}  // namespace onvif
