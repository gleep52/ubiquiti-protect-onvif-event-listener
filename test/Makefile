CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2

PKG_CFLAGS  := $(shell pkg-config --cflags libxml-2.0 libcurl openssl libmicrohttpd sqlite3)
PKG_LDFLAGS := $(shell pkg-config --libs   libxml-2.0 libcurl openssl libmicrohttpd sqlite3) -lpthread

# ── test_onvif_listener ─────────────────────────────────────────────────────
SRCS_LISTENER := ../onvif_listener.cpp       \
                 onvif_camera_emulator.cpp    \
                 camera_emulators.cpp         \
                 test_onvif_listener.cpp

TARGET_LISTENER := test_onvif_listener

# Default JSONL path — override with: make run JSONL=/path/to/file.jsonl
JSONL ?= testdata/onvif_raw_20260219_085752.jsonl

# ── test_detection_recorder ─────────────────────────────────────────────────
SRCS_DR := ../onvif_listener.cpp          \
           ../detection_recorder.cpp      \
           ../ubv_thumbnail.cpp           \
           onvif_camera_emulator.cpp      \
           test_detection_recorder.cpp

TARGET_DR := test_detection_recorder

# ── test_ubv_thumbnail ───────────────────────────────────────────────────────
SRCS_UBV := ../ubv_thumbnail.cpp \
            test_ubv_thumbnail.cpp

TARGET_UBV := test_ubv_thumbnail

# Default UBV file — override with: make run_ubv UBV=/path/to/file.ubv
UBV ?= /tmp/ubv_test_slice.ubv

# ── build rules ──────────────────────────────────────────────────────────────
.PHONY: all run run_dr run_ubv clean

all: $(TARGET_LISTENER) $(TARGET_DR) $(TARGET_UBV)

$(TARGET_LISTENER): $(SRCS_LISTENER)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) $^ -o $@ $(PKG_LDFLAGS)

$(TARGET_DR): $(SRCS_DR)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) $^ -o $@ $(PKG_LDFLAGS)

$(TARGET_UBV): $(SRCS_UBV)
	$(CXX) $(CXXFLAGS) $^ -o $@

run: $(TARGET_LISTENER)
	./$(TARGET_LISTENER) $(JSONL)

run_dr: $(TARGET_DR)
	./$(TARGET_DR)

run_ubv: $(TARGET_UBV)
	./$(TARGET_UBV) $(UBV)

clean:
	rm -f $(TARGET_LISTENER) $(TARGET_DR) $(TARGET_UBV)
