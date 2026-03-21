#pragma once

#include <string>
#include <vector>

#include "onvif_listener.hpp"

namespace unifi {

/// Connection parameters for the UniFi Protect PostgreSQL instance.
struct DbConfig {
  std::string host     = "127.0.0.1";
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

/// For each camera in `ids`, ensure that smart detection is enabled in the
/// Protect database.  Specifically, for any camera whose
/// `featureFlags.smartDetectTypes` or `smartDetectSettings.objectTypes` is
/// empty, sets both to ["person","vehicle"] and updates `updatedAt`.
///
/// This is idempotent — cameras already configured are not touched.
/// Throws std::runtime_error on connection or query failure.
void enable_smart_detect(const std::vector<onvif::CameraConfig>& cameras,
                         const DbConfig& db = {});

}  // namespace unifi
