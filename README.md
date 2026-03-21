# ONVIF Event Recorder

Listens to ONVIF WS-PullPoint event streams from IP cameras, records every raw
event as JSON Lines, and persists human/vehicle detection intervals to a SQLite
database (schema mirrors UniFi Protect).

## Binaries

| Target | Description |
|--------|-------------|
| `//:onvif_recorder` | Main recorder binary |
| `//test:test_onvif_listener` | Listener unit tests |
| `//test:test_detection_recorder` | Detection → SQLite integration test |

---

## System prerequisites

### Build tools

| Tool | Package / install |
|------|-------------------|
| Clang ≥ 14 | `apt install clang-14` |
| LLD linker | `apt install lld` |
| CMake / pkg-config | `apt install pkg-config` |
| Bazelisk | See below |

**Install Bazelisk** (manages the exact Bazel version in `.bazelversion`):

```bash
curl -Lo ~/.local/bin/bazel \
  https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
chmod +x ~/.local/bin/bazel
```

### Host (x86_64) library packages

These are required to build the x86_64 binary.  All must be present for both
compilation headers and `pkg-config` metadata.

```bash
sudo apt install \
  libxml2-dev \
  libcurl4-openssl-dev \
  libssl-dev \
  libsqlite3-dev \
  libmicrohttpd-dev \
  libpq-dev \
  libicu-dev \
  liblzma-dev \
  libzstd-dev \
  libnghttp2-dev \
  libidn2-dev \
  librtmp-dev \
  libssh-dev \
  libpsl-dev \
  libbrotli-dev \
  libldap-dev \
  libgnutls28-dev \
  libgmp-dev \
  nettle-dev \
  libtasn1-6-dev \
  libunistring-dev \
  libgcrypt20-dev \
  libgpg-error-dev
```

> **Note:** `libgssapi_krb5.a` and `libp11-kit.a` are not shipped by their
> respective Ubuntu dev packages.  The x86_64 binary therefore has a small
> number of remaining dynamic dependencies (see [Runtime dependencies](#runtime-dependencies)).
> A minimal `libsasl2.a` stub is compiled from source at workspace-setup time
> (via the `pkg_config_library` repository rule), so `libsasl2-dev` is not required.

### ARM64 cross-compilation prerequisites

Only the **build-host** packages below are needed; all ARM64 target libraries
are downloaded automatically by the Bazel `arm64_sysroot` repository rule.

```bash
sudo apt install clang lld
```

The `arm64_sysroot` rule fetches ~36 Ubuntu `.deb` packages on first use and
caches them in Bazel's output base.  No manual sysroot setup is required.

---

## Building

### x86_64 (native)

```bash
bazel build //:onvif_recorder
# output: bazel-bin/onvif_recorder
```

### ARM64 (cross-compile from x86_64)

```bash
bazel build --config=arm64 //:onvif_recorder
# output: bazel-bin/onvif_recorder  (ELF aarch64)
```

Packages are downloaded on first use and cached; subsequent builds are fast.

### Tests

```bash
# Detection → SQLite integration test (no arguments)
bazel run //test:test_detection_recorder

# Listener tests (testdata JSONL is passed automatically)
bazel run //test:test_onvif_listener

# Pass a different JSONL for manual inspection
bazel run //test:test_onvif_listener -- /path/to/other.jsonl
```

---

## Runtime dependencies

Both binaries are mostly statically linked.

### x86_64

```
libm.so.6          (glibc)
libgssapi_krb5.so.2  (MIT Kerberos — no static .a on Ubuntu)
libp11-kit.so.0      (p11-kit — no static .a on Ubuntu)
libstdc++.so.6
libgcc_s.so.1
libc.so.6
```

Install on the target machine:

```bash
sudo apt install libgssapi-krb5-2 libp11-kit0
```

### ARM64

```
libm.so.6
libc.so.6
ld-linux-aarch64.so.1
libgcc_s.so.1
```

All other dependencies (libxml2, libcurl, OpenSSL, libpq, libsqlite3,
libmicrohttpd, ICU, GnuTLS, etc.) are statically linked into the binary.
GSSAPI, p11-kit, and sasl2 are provided by lightweight stub libraries compiled
into the sysroot.

---

## Performance

### Throughput benchmark

The benchmark (`//test:bench_onvif_listener`) drives a local ONVIF camera emulator
flat-out on a single CPU core and measures end-to-end HTTP → XML-parse → callback
throughput.  Both the emulator and the listener are pinned to core 0, so the number
reflects single-core capacity under maximum load.

| Platform | CPU | Build | Throughput | Per event | CPU (1 core) |
|----------|-----|-------|-----------|-----------|--------------|
| x86_64 (dev host) | — | clang -O2 | 24,490 ev/s | 40.8 µs | 98.1% |
| x86_64 (dev host) | — | PGO + ThinLTO | 24,635 ev/s | 40.6 µs | 97.8% |
| ARM64 (Dream Router 60) | Cortex-A57 × 4 @ ~1.7 GHz | PGO + ThinLTO | 2,714 ev/s | 368 µs | 97.5% |

### CPU cost in production (Dream Router 60)

A typical installation with a handful of cameras generates well under 60 events per
minute (motion/person/vehicle detections).  At that rate the recorder's CPU footprint
is negligible:

| Load | Core usage | Share of total CPU (4 cores) |
|------|-----------|------------------------------|
| 2,714 ev/s (benchmark max) | 97.5% of 1 core | 24.4% |
| 1 ev/s (60 ev/min) | **0.036% of 1 core** | **< 0.01%** |

**Calculation:** 97.5% ÷ 2,714 ev/s = 0.036% per ev/s per core.  At 60 ev/min
(1 ev/s) that is 0.036% of one Cortex-A57 core, or 0.009% of the router's total
four-core capacity.

> Note: the benchmark is CPU-bound because the emulator runs in the same process on
> the same pinned core.  In production the listener thread spends most of its time
> waiting for camera network responses (I/O-bound), so real-world CPU usage is even
> lower than the extrapolation above.

---

## Capturing ONVIF logs for unsupported cameras

If your camera does not work with this software, you can capture a full log of
all ONVIF messages and submit it as a bug report. The log contains every SOAP
request and response exchanged with the camera.

### Step 1 — run the recorder with verbose logging and raw recording enabled

Set `ONVIF_VERBOSE=1` to see per-camera lifecycle messages in the terminal, and
ensure raw recording is active (it is always enabled in the default binary — the
raw file is printed at startup as `Raw file : onvif_raw_<timestamp>.jsonl`).

```bash
ONVIF_VERBOSE=1 ./onvif_recorder
```

Leave it running for at least 60 seconds so that at least one full subscription
cycle (subscribe → pull → renew) is captured, then press Ctrl+C.

### Step 2 — locate the raw log file

The raw JSONL file is printed at startup:

```
Raw file    : onvif_raw_20260219_085752.jsonl
```

Each line is one SOAP exchange (request + response), for example:

```json
{"timestamp":"...","camera_ip":"192.168.1.108","url":"...","soap_action":"...","request":"...","response_status":200,"response":"..."}
```

### Step 3 — submit the log

Open a GitHub issue and attach the `.jsonl` file. If the file is large, compress
it first (`gzip onvif_raw_*.jsonl`). Please also include:

- Camera make and model
- Firmware version (if known)
- The terminal output from step 1 (copy the lines printed for your camera's IP)

---

## Project structure

```
onvif/
├── main.cpp                    # recorder binary entry point
├── onvif_listener.hpp/.cpp     # ONVIF WS-PullPoint listener
├── detection_recorder.hpp/.cpp # SQLite detection recorder
├── unifi_camera_config.hpp     # camera definitions
├── bazel/
│   ├── pkg_config.bzl          # repository rule: wraps system libs via pkg-config
│   └── arm64_sysroot.bzl       # repository rule: downloads arm64 sysroot + toolchain
├── platforms/
│   └── BUILD.bazel             # linux_arm64 platform definition
├── test/
│   ├── onvif_camera_emulator.hpp/.cpp  # HTTP emulator base (libmicrohttpd)
│   ├── camera_emulators.hpp/.cpp       # Camera 108/109 emulators
│   ├── test_onvif_listener.cpp
│   └── test_detection_recorder.cpp
├── WORKSPACE                   # external deps declarations
├── BUILD.bazel                 # build targets
└── .bazelrc                    # compiler flags + arm64 config
```
