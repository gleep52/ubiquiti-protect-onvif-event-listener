#pragma once

#include <string>
#include <vector>

#include "onvif_listener.hpp"

namespace unifi {

/// Connection parameters for the UniFi Protect PostgreSQL instance.
struct DbConfig {
  std::string host     = "192.168.1.1";
  int         port     = 5433;
  std::string dbname   = "unifi-protect";
  std::string user     = "postgres";
  std::string password = "";
};

/// Connect to the UniFi Protect database and return a CameraConfig for every
/// adopted third-party (ONVIF) camera.
///
/// Reads the `cameras` table where `isThirdPartyCamera = true` and
/// `isAdopted = true`, extracting the IP from `host` and credentials from
/// the `thirdPartyCameraInfo` JSONB column.
///
/// Throws std::runtime_error on connection or query failure.
std::vector<onvif::CameraConfig> load_cameras(const DbConfig& db = {});

}  // namespace unifi
