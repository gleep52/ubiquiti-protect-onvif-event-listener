# CLAUDE.md — Claude Code guide for this project

## Build commands

```bash
# Host (x86_64) binary
bazel build //:onvif_recorder

# ARM64 cross-compiled binary (for Dream Machine)
bazel build --config=arm64 //:onvif_recorder

# All tests
bazel test //test:all

# Individual tests
bazel run //test:test_detection_recorder
bazel run //test:test_onvif_listener       # testdata JSONL passed automatically
bazel run //test:test_ubv_thumbnail        # snapshot JPEGs passed automatically

# Manual inspection with a custom file
bazel run //test:test_onvif_listener -- /path/to/other.jsonl
bazel run //test:test_ubv_thumbnail  -- /path/to/file.ubv

# Throughput benchmark (single core, 50 000 events default)
bazel run //test:bench_onvif_listener
bazel run //test:bench_onvif_listener -- 100000   # custom event count

# PGO + ThinLTO optimised build (x86)
make pgo-bench-x86                   # baseline → instrument → profile → optimised
make pgo-bench-x86 PGO_EVENTS=100000

# ARM64 benchmark under QEMU (cross-PGO reuses x86 profile)
# Prerequisite: sudo apt-get install qemu-user-static
# Prerequisite: run pgo-bench-x86 first to generate pgo.profdata
make pgo-bench-arm64
```

Bazelisk is at `~/.local/bin/bazel` (auto-downloads Bazel 7.4.1 per `.bazelversion`).

## Code style

Google C++ Style Guide enforced via cpplint:

```bash
python3 -m cpplint <file>
```

Key rules in practice:
- K&R braces — `{` at end of previous line, never on its own line
- Access specifiers indented +1 space: ` public:`, ` protected:`, ` private:`
- Include order: matching `.h` → C system (`<foo.h>`) → C++ system → other
- `// NOLINT(runtime/int)` on libcurl lines that use `long` (required by the API)
- Line length limit: 100 (set in `CPPLINT.cfg`)

## Project structure

```
onvif_listener.hpp/.cpp       — WS-PullPoint ONVIF subscription library
detection_recorder.hpp/.cpp   — Detection → SQLite/PostgreSQL recorder
ubv_thumbnail.hpp/.cpp        — UBV container encode/decode (thumbnail storage)
unifi_camera_config.hpp/.cpp  — Load camera credentials from UniFi Protect DB
main.cpp                      — Binary entry point

test/
  onvif_camera_emulator.hpp/.cpp  — libmicrohttpd fake camera base class
  camera_emulators.hpp/.cpp       — Camera 108 / 109 concrete emulators
  test_onvif_listener.cpp         — Listener integration test
  test_detection_recorder.cpp     — Detection recorder e2e test
  test_ubv_thumbnail.cpp          — UBV round-trip test
  testdata/
    snapshot_108.jpg                  — Real JPEG from cam 192.168.1.108
    snapshot_109.jpg                  — Real JPEG from cam 192.168.1.109
    onvif_raw_20260219_085752.jsonl   — Canonical raw SOAP log for listener tests
```

## Database backends

`DetectionRecorder` supports two backends selected at runtime via `ONVIF_DB_BACKEND`:

- **SQLite** (default): writes to a local `.db` file; thumbnails stored in per-camera
  `.ubv` files under `ONVIF_UBV_DIR` (default: `thumbnails/`).
- **PostgreSQL**: writes to the UniFi Protect database; thumbnails inserted directly
  into the `thumbnails` table as `bytea` with a 24-char hex `id` so UniFi Protect's
  UI routes them correctly (IDs of length ≠ 24 go to msp TCP and fail).

## Key architectural notes

- One thread per camera in `OnvifListener`; `EventCallback` is called from camera
  threads and must be thread-safe.
- `CameraConfig::max_consecutive_failures` (default 0 = unlimited) controls give-up
  behaviour; `main.cpp` sets it to 5 for production. Tests leave it at 0 because
  some emulated cameras replay startup-failure responses before succeeding.
- `CameraConfig::id` and `mac` are populated from the UniFi Protect DB and used by
  the PostgreSQL backend to set `cameraId` and generate thumbnail IDs.
- Detection mapping: ONVIF `"Human"` → `"person"`, `"Vehicle"` → `"vehicle"`.
- `"end"` must be double-quoted in SQL (reserved word).

## Environment variables (runtime)

| Variable | Default | Description |
|---|---|---|
| `ONVIF_DB_BACKEND` | `sqlite` | `sqlite` or `postgres` |
| `ONVIF_DB_CONN` | `onvif_detections.db` | SQLite path or libpq conninfo |
| `ONVIF_UBV_DIR` | `thumbnails` | Directory for UBV thumbnail files (SQLite only) |
| `ONVIF_PRE_BUFFER_SEC` | `2` | Seconds before first detection event |
| `ONVIF_POST_BUFFER_SEC` | `2` | Seconds after last detection event |
| `ONVIF_VERBOSE` | _(unset)_ | Set to `1` to enable verbose logging (lifecycle, events, renewals) |
