#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ubv {

// ---------------------------------------------------------------------------
// UBV thumbnail container format
//
// Ubiquiti's proprietary container used by UniFi Protect to store per-event
// JPEG thumbnail images.  A single .ubv file holds an ordered sequence of
// frames; each frame's timestamp_ms matches the value embedded in the
// UniFi Protect database's events.thumbnailId  ("<MAC>-<timestamp_ms>").
//
// Binary record layout (all multi-byte integers are big-endian):
//
//   [4]  type        — 0xa00009a9 file-header | 0xa0da7e04 frame-meta
//                    |  0xa04a709a JPEG frame
//   [4]  codec       — 0xfd020000 meta/header | 0xfd020001 JPEG
//   [8]  timestamp   — milliseconds since Unix epoch
//   [4]  payload_len — byte length N of the following payload
//   [N]  payload     — raw JPEG bytes (for JPEG records)
//   [4]  back_ref    — 20 + N; enables backward scanning
//
// File structure:
//   [file-header record]
//   ( [frame-meta record] [JPEG record] ) × frame count
// ---------------------------------------------------------------------------

/// One decoded JPEG thumbnail.
struct Frame {
    uint64_t             timestamp_ms;  ///< milliseconds since Unix epoch
    std::vector<uint8_t> jpeg;          ///< raw JPEG bytes
};

/// Decode all JPEG thumbnail frames from a UBV file.
/// Throws std::runtime_error on I/O or format errors.
std::vector<Frame> decode(const std::string& path);

/// Decode the single JPEG frame whose timestamp matches @p timestamp_ms.
/// Scans sequentially and returns as soon as the frame is found.
/// Throws std::runtime_error if not found or on I/O error.
Frame decode_one(const std::string& path, uint64_t timestamp_ms);

/// Encode @p frames into a UBV file at @p path (overwritten if it exists).
/// Throws std::runtime_error if @p frames is empty or on I/O error.
void encode(const std::string& path, const std::vector<Frame>& frames);

/// Append a single frame to a UBV file at @p path.
/// If the file does not exist (or is empty) it is created with a fresh
/// file-header record first.  Subsequent calls append only the meta+JPEG
/// record pair, making this suitable for a continuously-growing thumbnail log.
/// Throws std::runtime_error on I/O error.
void append(const std::string& path, const Frame& frame);

}  // namespace ubv
