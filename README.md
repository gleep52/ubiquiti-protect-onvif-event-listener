# ONVIF Event Recorder

Bridges **third-party ONVIF cameras** into UniFi Protect. It listens to ONVIF
WS-PullPoint event streams and writes human/vehicle detection intervals to the
UniFi Protect PostgreSQL database so they appear natively in the Protect UI.

> **Note:** This software is only needed for third-party cameras adopted into
> UniFi Protect via the ONVIF integration. Native UniFi cameras have built-in
> smart detection and do not require this tool.

---

## Installation on Dream Router / Dream Machine (third-party cameras only)

### Step 1 — Enable SSH on your UniFi device

Go to **UniFi OS → System → Advanced** and enable SSH.
Full instructions: https://help.ui.com/hc/en-us/articles/204909374

### Step 2 — Download the latest release

From your local machine, download the two files from the
[latest release](https://github.com/danielwoz/ubiquiti-protect-onvif-event-listener/releases/latest):

```bash
# Copy binary and service file to the router
scp onvif_recorder_arm64 root@<router-ip>:/root/onvif_recorder
scp onvif-recorder.service root@<router-ip>:/etc/systemd/system/
```

### Step 3 — Enable and start the service

```bash
chmod +x /root/onvif_recorder
systemctl enable onvif-recorder
systemctl start onvif-recorder
systemctl status onvif-recorder
```

Detections will appear in UniFi Protect within seconds of the first motion event.
Logs are written to `/var/log/onvif-recorder.log`.

---

## Performance (Dream Router)

The recorder uses negligible CPU at normal camera workloads:

| Load | CPU (single core) | Share of total CPU (4 cores) |
|------|-------------------|------------------------------|
| 60 ev/min (typical) | 0.036% of 1 core | **< 0.01%** |
| 2,714 ev/s (benchmark max) | 97.5% of 1 core | 24.4% |

---

## Troubleshooting

**Check the log:**
```bash
tail -f /var/log/onvif-recorder.log
```

**Enable verbose output** to see per-camera lifecycle events:
```bash
systemctl stop onvif-recorder
ONVIF_VERBOSE=1 /root/onvif_recorder
```

**Camera not working?** Capture a diagnostic log and open a GitHub issue:
```bash
ONVIF_VERBOSE=1 /root/onvif_recorder
# Let it run 60+ seconds (one full subscribe → pull → renew cycle), then Ctrl+C
# Attach the onvif_raw_<timestamp>.jsonl file to your issue
```

---

## Building from source

### Prerequisites

- Ubuntu 22.04 x86_64 build host
- Clang 14 + LLD: `sudo apt install clang-14 lld`
- [Bazelisk](https://github.com/bazelbuild/bazelisk) at `~/.local/bin/bazel`
- Host library packages (x86 native build only):

```bash
sudo apt install \
  libxml2-dev libcurl4-openssl-dev libssl-dev libsqlite3-dev \
  libmicrohttpd-dev libpq-dev libicu-dev liblzma-dev libzstd-dev \
  libnghttp2-dev libidn2-dev librtmp-dev libssh-dev libpsl-dev \
  libbrotli-dev libldap-dev libgnutls28-dev libgmp-dev nettle-dev \
  libtasn1-6-dev libunistring-dev libgcrypt20-dev libgpg-error-dev
```

### Build

```bash
# ARM64 (for Dream Router / Dream Machine)
bazel build --config=arm64 //:onvif_recorder

# x86_64 (native)
bazel build //:onvif_recorder

# Run all tests
bazel test //test:all

# Throughput benchmark
bazel run //test:bench_onvif_listener

# PGO + ThinLTO optimised build
make pgo-bench-x86
```

The ARM64 build downloads its own sysroot automatically — no manual cross-toolchain
setup required.

### Runtime dependencies (ARM64)

The binary is almost entirely statically linked:

```
libm.so.6  libc.so.6  ld-linux-aarch64.so.1  libgcc_s.so.1
```

All other libraries (libcurl, OpenSSL, libxml2, libpq, libsqlite3, GnuTLS, ICU,
etc.) are compiled in.

---

## Project structure

```
onvif_listener.hpp/.cpp       — WS-PullPoint ONVIF subscription library
detection_recorder.hpp/.cpp   — Detection → SQLite/PostgreSQL recorder
ubv_thumbnail.hpp/.cpp        — UBV container encode/decode (thumbnail storage)
unifi_camera_config.hpp/.cpp  — Load camera credentials from UniFi Protect DB
main.cpp                      — Binary entry point
bazel/
  pkg_config.bzl              — Repository rule: wraps host libs via pkg-config
  arm64_sysroot.bzl           — Repository rule: ARM64 sysroot + Clang toolchain
test/
  onvif_camera_emulator.hpp/.cpp  — libmicrohttpd fake camera base class
  camera_emulators.hpp/.cpp       — Camera 108/109 concrete emulators
  test_onvif_listener.cpp         — Listener integration test
  test_detection_recorder.cpp     — Detection recorder e2e test
  test_ubv_thumbnail.cpp          — UBV round-trip test
  bench_onvif_listener.cpp        — Single-core throughput benchmark
```
