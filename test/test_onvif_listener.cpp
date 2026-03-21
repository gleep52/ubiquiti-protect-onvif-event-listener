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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../onvif_listener.hpp"
#include "camera_emulators.hpp"

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
// Event collector: run OnvifListener until a predicate is satisfied or timeout
// ============================================================
struct CollectedEvents {
  std::vector<onvif::OnvifEvent> events;
  bool timed_out{false};
};

// Collect events until pred(events) returns true or timeout expires.
// pred is called (under the mutex) each time a new event arrives.
template<typename Pred>
static CollectedEvents collect_until(
  std::vector<onvif::CameraConfig> configs,
  Pred pred,
  std::chrono::seconds timeout) {
  onvif::OnvifListener listener;
  for (const auto& cfg : configs)
    listener.add_camera(cfg);

  std::mutex mu;
  std::condition_variable cv;
  std::vector<onvif::OnvifEvent> evs;

  std::thread t([&] {
    listener.run([&](const onvif::OnvifEvent& ev) {
      std::lock_guard<std::mutex> lk(mu);
      evs.push_back(ev);
      if (pred(evs))
        cv.notify_one();
    });
  });

  bool ok;
  {
    std::unique_lock<std::mutex> lk(mu);
    ok = cv.wait_for(lk, timeout, [&] { return pred(evs); });
  }

  listener.stop();
  t.join();

  CollectedEvents result;
  {
    std::lock_guard<std::mutex> lk(mu);
    result.events = std::move(evs);
  }
  result.timed_out = !ok;
  return result;
}

// Convenience: collect until at least N events have arrived.
static CollectedEvents collect(
  std::vector<onvif::CameraConfig> configs,
  std::size_t n,
  std::chrono::seconds timeout) {
  return collect_until(
    std::move(configs),
    [n](const std::vector<onvif::OnvifEvent>& evs) { return evs.size() >= n; },
    timeout);
}

// ============================================================
// Test: Camera 108 -- subscribes on first attempt, receives events
// ============================================================
static void test_camera108_basic(const std::string& jsonl) {
  Camera108Emulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;

  auto r = collect({cfg}, 5, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for events");
  CHECK(r.events.size() >= 5, "expected >= 5 events");

  for (const auto& ev : r.events) {
    CHECK(!ev.topic.empty(),       "event topic must not be empty");
    CHECK(!ev.property_op.empty(), "event property_op must not be empty");
    CHECK(ev.property_op == "Initialized" ||
          ev.property_op == "Changed"     ||
          ev.property_op == "Deleted",
          "unexpected property_op: " + ev.property_op);
    CHECK(ev.camera_ip == emu.local_address(),
          "camera_ip mismatch: expected " + emu.local_address() +
          " got " + ev.camera_ip);
  }

  // First PullMessages response carries Initialized state notifications
  if (!r.events.empty()) {
    CHECK(r.events.front().property_op == "Initialized",
          "first event should be Initialized, got: " +
          r.events.front().property_op);
  }
}

// ============================================================
// Test: Camera 108 -- Changed events appear after Initialized batch
// ============================================================
static void test_camera108_changed_events(const std::string& jsonl) {
  Camera108Emulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "admin";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;

  // Collect enough to get past the initial Initialized batch
  auto r = collect({cfg}, 20, std::chrono::seconds(30));

  CHECK(!r.timed_out, "timed out waiting for events");
  CHECK(r.events.size() >= 20, "expected >= 20 events");

  bool saw_changed = false;
  for (const auto& ev : r.events) {
    if (ev.property_op == "Changed") {
      saw_changed = true;
      CHECK(ev.topic.find("RuleEngine") != std::string::npos ||
            ev.topic.find("VideoAnalytics") != std::string::npos,
            "Changed event topic unexpected: " + ev.topic);
    }
  }
  CHECK(saw_changed, "expected at least one Changed event");
}

// ============================================================
// Test: Camera 109 -- retries subscription 13x before succeeding
// ============================================================
static void test_camera109_retries(const std::string& jsonl) {
  Camera109Emulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "danielwoz";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;  // 13 retries x 1 s ~= 13 s delay

  auto r = collect({cfg}, 3, std::chrono::seconds(60));

  CHECK(!r.timed_out, "timed out waiting for events after subscription retries");
  CHECK(r.events.size() >= 3, "expected >= 3 events after retries");

  for (const auto& ev : r.events) {
    // Some PullMessages responses carry no notification elements; the
    // listener still invokes the callback with an empty-field event.
    // Skip those and only validate events that carry actual data.
    if (ev.topic.empty()) continue;

    CHECK(ev.property_op == "Initialized" || ev.property_op == "Changed",
          "unexpected property_op: " + ev.property_op);
    CHECK(ev.camera_ip == emu.local_address(),
          "camera_ip mismatch: " + ev.camera_ip);
  }
}

// ============================================================
// Test: Camera 109 -- events have motion-related topics
// ============================================================
static void test_camera109_topics(const std::string& jsonl) {
  Camera109Emulator emu(jsonl);
  emu.start();

  onvif::CameraConfig cfg;
  cfg.ip                 = emu.local_address();
  cfg.user               = "danielwoz";
  cfg.password           = "eW6iO01l";
  cfg.retry_interval_sec = 1;

  auto r = collect({cfg}, 5, std::chrono::seconds(60));

  CHECK(!r.timed_out, "timed out");
  CHECK(r.events.size() >= 5, "expected >= 5 events");

  for (const auto& ev : r.events) {
    if (ev.topic.empty()) continue;  // skip empty-payload PullMessages responses

    // Camera 109 produces motion/alarm events under RuleEngine and VideoSource
    CHECK(ev.topic.find("RuleEngine")    != std::string::npos ||
          ev.topic.find("VideoSource")    != std::string::npos ||
          ev.topic.find("VideoAnalytics") != std::string::npos,
          "unexpected topic: " + ev.topic);
  }
}

// ============================================================
// Test: Both cameras concurrently -- events arrive from both
// ============================================================
static void test_both_cameras(const std::string& jsonl) {
  Camera108Emulator emu108(jsonl);
  Camera109Emulator emu109(jsonl);
  emu108.start();
  emu109.start();

  onvif::CameraConfig cfg108;
  cfg108.ip                 = emu108.local_address();
  cfg108.user               = "admin";
  cfg108.password           = "eW6iO01l";
  cfg108.retry_interval_sec = 1;

  onvif::CameraConfig cfg109;
  cfg109.ip                 = emu109.local_address();
  cfg109.user               = "danielwoz";
  cfg109.password           = "eW6iO01l";
  cfg109.retry_interval_sec = 1;

  // Camera 108 delivers immediately; camera 109 needs ~13 s to work through
  // its 13 recorded 400-error responses (retry_interval_sec=1).
  // Stop as soon as we have >=3 events from cam108 AND >=1 from cam109.
  const std::string addr108 = emu108.local_address();
  const std::string addr109 = emu109.local_address();

  auto r = collect_until(
    {cfg108, cfg109},
    [&](const std::vector<onvif::OnvifEvent>& evs) {
      int n108 = 0, n109 = 0;
      for (const auto& e : evs) {
        if (e.camera_ip == addr108) ++n108;
        if (e.camera_ip == addr109) ++n109;
      }
      return n108 >= 3 && n109 >= 1;
    },
    std::chrono::seconds(60));

  CHECK(!r.timed_out, "timed out waiting for events from both cameras");

  int from_108 = 0, from_109 = 0;
  for (const auto& ev : r.events) {
    if (ev.camera_ip == addr108) ++from_108;
    if (ev.camera_ip == addr109) ++from_109;
  }
  CHECK(from_108 >= 3,
        "expected >= 3 events from camera 108, got: " + std::to_string(from_108));
  CHECK(from_109 >= 1,
        "expected >= 1 events from camera 109, got: " + std::to_string(from_109));
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <onvif_raw.jsonl>\n"
              << "  Provide the path to a raw ONVIF recording made with\n"
              << "  OnvifListener::enable_raw_recording().\n";
    return 1;
  }
  const std::string jsonl = argv[1];

  onvif::global_init();

  run_test("camera108_basic",          [&] { test_camera108_basic(jsonl); });
  run_test("camera108_changed_events", [&] { test_camera108_changed_events(jsonl); });
  run_test("camera109_retries",        [&] { test_camera109_retries(jsonl); });
  run_test("camera109_topics",         [&] { test_camera109_topics(jsonl); });
  run_test("both_cameras",             [&] { test_both_cameras(jsonl); });

  onvif::global_cleanup();

  std::cout << "================================================\n"
            << "Results: " << g_pass << " checks passed, "
            << g_fail    << " checks failed\n"
            << "================================================\n";

  return g_fail > 0 ? 1 : 0;
}
