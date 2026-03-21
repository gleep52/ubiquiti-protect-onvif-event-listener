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

// ubv_extract -- decode all frames from a UBV thumbnail file and save as
// labelled JPEGs.  For each frame the tool looks up the nearest matching
// event in the UniFi Protect PostgreSQL database (matching on thumbnailId
// = "<camera_mac>-<timestamp_ms>") and uses the event start-time and type
// as the filename.
//
// Usage:
//   ubv_extract <ubv_file> <camera_mac> <output_dir>
//
// Example:
//   ubv_extract /tmp/ubv_full_data.ubv F4E2C6741E6A /tmp/unifi_thumbs
//
// If no matching event row is found for a frame, the filename falls back to
// the raw timestamp in milliseconds.

#include "ubv_thumbnail.hpp"

#include <libpq-fe.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void mkdir_p(const std::string& dir) {
  struct stat st{};
  if (stat(dir.c_str(), &st) == 0) return;  // already exists
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
    throw std::runtime_error("mkdir " + dir + ": " + strerror(errno));
}

// Format milliseconds-since-epoch as "YYYY-MM-DD_HH-MM-SS"
static std::string format_ts(uint64_t ts_ms) {
  time_t t = static_cast<time_t>(ts_ms / 1000);
  struct tm utc{};
  gmtime_r(&t, &utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &utc);
  return buf;
}

// Replace characters that are awkward in filenames
static std::string sanitize(std::string s) {
  for (char& c : s)
    if (c == ' ' || c == ':' || c == '/')
      c = '-';
  return s;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0]
              << " <ubv_file> <camera_mac> <output_dir>\n";
    return 1;
  }

  const std::string ubv_path   = argv[1];
  const std::string camera_mac = argv[2];
  const std::string out_dir    = argv[3];

  // -- decode all frames from the UBV file ---------------------------------
  std::cout << "Decoding " << ubv_path << " ...\n";
  std::vector<ubv::Frame> frames;
  try {
    frames = ubv::decode(ubv_path);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
  std::cout << "Found " << frames.size() << " JPEG frame(s).\n";
  if (frames.empty()) return 0;

  // -- connect to UniFi Protect PostgreSQL ---------------------------------
  const char* db_host = std::getenv("ONVIF_DB_HOST");
  std::string connstr =
    std::string("host=") + (db_host ? db_host : "127.0.0.1") +
    " port=5433 dbname=unifi-protect user=postgres";
  PGconn* conn = PQconnectdb(connstr.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::cerr << "PostgreSQL connect failed: " << PQerrorMessage(conn)
              << "  (will use raw timestamps)\n";
    PQfinish(conn);
    conn = nullptr;
  } else {
    std::cout << "Connected to UniFi Protect database.\n";
  }

  // -- create output directory ---------------------------------------------
  mkdir_p(out_dir);

  // -- for each frame, look up event and write JPEG -----------------------
  int saved = 0;
  for (const auto& f : frames) {
    const uint64_t ts = f.timestamp_ms;

    // Build the thumbnailId that would be in the events table.
    std::string thumbnail_id = camera_mac + "-" + std::to_string(ts);

    std::string event_time;
    std::string event_type = "unknown";

    if (conn) {
      // Query: look for an event with this exact thumbnailId.
      // Fall back to closest event by start time if not found.
      const char* param = thumbnail_id.c_str();
      PGresult* res = PQexecParams(
        conn,
        "SELECT type, "
        "  to_char(to_timestamp(start/1000.0) AT TIME ZONE 'UTC',"
        "           'YYYY-MM-DD_HH24-MI-SS') AS event_time "
        "FROM events "
        "WHERE \"thumbnailId\" = $1 AND \"deletedAt\" IS NULL "
        "LIMIT 1",
        1, nullptr, &param, nullptr, nullptr, 0);

      if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        event_type = PQgetvalue(res, 0, 0);
        event_time = PQgetvalue(res, 0, 1);
      } else {
        // No exact match -- use the frame's own timestamp formatted.
        event_time = format_ts(ts);
      }
      PQclear(res);
    } else {
      event_time = format_ts(ts);
    }

    // Build filename: <event_time>_<event_type>_<ts_ms>.jpg
    std::ostringstream fname;
    fname << out_dir << "/" << event_time
          << "_" << sanitize(event_type)
          << "_" << ts << ".jpg";

    std::string path = fname.str();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      std::cerr << "Cannot write " << path << "\n";
      continue;
    }
    out.write(reinterpret_cast<const char*>(f.jpeg.data()),
              static_cast<std::streamsize>(f.jpeg.size()));
    if (!out) {
      std::cerr << "Write error: " << path << "\n";
      continue;
    }

    std::cout << "  Saved: " << path
              << "  (" << f.jpeg.size() << " bytes)\n";
    ++saved;
  }

  if (conn) PQfinish(conn);

  std::cout << "\nDone -- " << saved << "/" << frames.size()
            << " frames saved to " << out_dir << "\n";
  return 0;
}
