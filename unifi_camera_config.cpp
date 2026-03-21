#include "unifi_camera_config.hpp"

#include <libpq-fe.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace unifi {

// ---------------------------------------------------------------------------
// Minimal flat-JSON string-value extractor.
//
// Handles the subset produced by PostgreSQL's JSONB output for the
// thirdPartyCameraInfo column: a single-level object with string or null
// values.  Returns an empty string when the key is absent or its value is
// null.
// ---------------------------------------------------------------------------
static std::string json_get(const std::string& json, const std::string& key) {
  std::string needle = "\"" + key + "\":";
  auto pos = json.find(needle);
  if (pos == std::string::npos) return {};
  pos += needle.size();

  // skip optional whitespace
  while (pos < json.size() && json[pos] == ' ') ++pos;
  if (pos >= json.size()) return {};

  if (json[pos] == 'n') return {};  // null

  if (json[pos] != '"') {
    // bare token (number / bool) -- read until delimiter
    std::string val;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}')
      val += json[pos++];
    return val;
  }

  // quoted string with basic escape handling
  ++pos;
  std::string val;
  while (pos < json.size() && json[pos] != '"') {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      ++pos;
      switch (json[pos]) {
        case '"':  val += '"';  break;
        case '\\': val += '\\'; break;
        case '/':  val += '/';  break;
        case 'n':  val += '\n'; break;
        case 'r':  val += '\r'; break;
        case 't':  val += '\t'; break;
        default:   val += json[pos]; break;
      }
    } else {
      val += json[pos];
    }
    ++pos;
  }
  return val;
}

// ---------------------------------------------------------------------------

std::vector<onvif::CameraConfig> load_cameras(const DbConfig& db) {
  std::string connstr =
    "host="   + db.host   +
    " port="  + std::to_string(db.port) +
    " dbname=" + db.dbname +
    " user="  + db.user;
  if (!db.password.empty())
    connstr += " password=" + db.password;

  PGconn* conn = PQconnectdb(connstr.c_str());
  if (PQstatus(conn) != CONNECTION_OK) {
    std::string err = PQerrorMessage(conn);
    PQfinish(conn);
    throw std::runtime_error("unifi::load_cameras: " + err);
  }

  const char* sql =
    "SELECT id, mac, host, \"thirdPartyCameraInfo\" "
    "FROM cameras "
    "WHERE \"isThirdPartyCamera\" = true "
    "  AND \"isAdopted\" = true "
    "  AND host IS NOT NULL";

  PGresult* res = PQexec(conn, sql);
  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    std::string err = PQresultErrorMessage(res);
    PQclear(res);
    PQfinish(conn);
    throw std::runtime_error("unifi::load_cameras query: " + err);
  }

  std::vector<onvif::CameraConfig> cameras;
  int nrows = PQntuples(res);
  for (int i = 0; i < nrows; ++i) {
    const char* id_c   = PQgetvalue(res, i, 0);
    const char* mac_c  = PQgetvalue(res, i, 1);
    const char* host_c = PQgetvalue(res, i, 2);
    const char* info_c = PQgetvalue(res, i, 3);
    if (!id_c || !host_c || !info_c || PQgetisnull(res, i, 2)) continue;

    std::string info(info_c);
    std::string username     = json_get(info, "username");
    std::string password     = json_get(info, "password");
    std::string snapshot_url = json_get(info, "snapshotUrl");
    if (username.empty() || password.empty()) continue;

    cameras.push_back({std::string(id_c),
                       mac_c ? std::string(mac_c) : std::string(),
                       std::string(host_c),
                       username, password, snapshot_url});
  }

  PQclear(res);
  PQfinish(conn);
  return cameras;
}

}  // namespace unifi
