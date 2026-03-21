#pragma once

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "onvif_camera_emulator.hpp"

// ============================================================
// RecordedSession -- responses loaded from a raw JSONL log file
// ============================================================

/// One HTTP exchange loaded from the raw recording.
struct RecordedExchange {
  int         status;
  std::string response;  // raw SOAP XML (JSON-unescaped)
};

/// All exchanges for one camera, partitioned by SOAP action.
struct RecordedSession {
  std::vector<RecordedExchange> create_sub;  ///< CreatePullPointSubscription
  std::vector<RecordedExchange> pull;        ///< PullMessages
  std::vector<RecordedExchange> renew;       ///< Renew

  /// Parse @p jsonl_path and return entries matching @p camera_ip.
  static RecordedSession from_jsonl(const std::string& jsonl_path,
                                    const std::string& camera_ip);
};

// ============================================================
// Camera108Emulator -- 192.168.1.108 (always subscribes on first attempt)
// ============================================================
class Camera108Emulator : public OnvifCameraEmulator {
 public:
  explicit Camera108Emulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::size_t     create_idx_{0};
  std::size_t     pull_idx_{0};
  std::size_t     renew_idx_{0};
  std::mutex      mu_;
};

// ============================================================
// Camera109Emulator -- 192.168.1.109 (returns 400 x N then 200 for subscribe)
// ============================================================
class Camera109Emulator : public OnvifCameraEmulator {
 public:
  explicit Camera109Emulator(const std::string& jsonl_path);

 protected:
  std::pair<int, std::string> handle(
    const std::string& path,
    const std::string& soap_action,
    const std::string& body) override;

 private:
  RecordedSession session_;
  std::size_t     create_idx_{0};
  std::size_t     pull_idx_{0};
  std::mutex      mu_;
};
