#include "ubv_thumbnail.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ubv {

// ---------------------------------------------------------------------------
// Record type and codec constants (logical, host-byte-order values)
// ---------------------------------------------------------------------------
static constexpr uint32_t TYPE_FILE_HEADER = 0xa00009a9u;
static constexpr uint32_t TYPE_META        = 0xa0da7e04u;
static constexpr uint32_t TYPE_JPEG        = 0xa04a709au;

static constexpr uint32_t CODEC_META       = 0xfd020000u;
static constexpr uint32_t CODEC_JPEG       = 0xfd020001u;

// Byte size of the fixed record header fields (type+codec+timestamp+len).
static constexpr uint32_t RECORD_HEADER_SIZE = 20u;

// ---------------------------------------------------------------------------
// Big-endian I/O helpers
// ---------------------------------------------------------------------------
static uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
       | (static_cast<uint32_t>(p[2]) <<  8) |  static_cast<uint32_t>(p[3]);
}

static uint64_t be64(const uint8_t* p) {
  return (static_cast<uint64_t>(be32(p)) << 32) | static_cast<uint64_t>(be32(p + 4));
}

static void put_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint32_t>(v >> 24); p[1] = static_cast<uint32_t>(v >> 16);
  p[2] = static_cast<uint32_t>(v >>  8); p[3] = static_cast<uint32_t>(v);
}

static void put_be64(uint8_t* p, uint64_t v) {
  put_be32(p,     static_cast<uint32_t>(v >> 32));
  put_be32(p + 4, static_cast<uint32_t>(v));
}

// ---------------------------------------------------------------------------
// Read one complete record from @p in.
// Returns false (without throwing) at clean EOF; throws on partial data.
// ---------------------------------------------------------------------------
struct Record {
  uint32_t             type;
  uint32_t             codec;
  uint64_t             timestamp_ms;
  std::vector<uint8_t> payload;
};

static bool read_record(std::ifstream& in, Record& out) {
  uint8_t hdr[RECORD_HEADER_SIZE];
  if (!in.read(reinterpret_cast<char*>(hdr), RECORD_HEADER_SIZE)) {
    if (in.eof() && in.gcount() == 0) return false;  // clean EOF
    throw std::runtime_error("ubv: truncated record header");
  }

  out.type         = be32(hdr + 0);
  out.codec        = be32(hdr + 4);
  out.timestamp_ms = be64(hdr + 8);
  const uint32_t n = be32(hdr + 16);

  out.payload.resize(n);
  if (n > 0) {
    in.read(reinterpret_cast<char*>(out.payload.data()), n);
    if (in.gcount() < static_cast<std::streamsize>(n))
      return false;  // truncated at end of file -- stop cleanly
  }

  uint8_t trailer[4];
  if (!in.read(reinterpret_cast<char*>(trailer), 4))
    throw std::runtime_error("ubv: truncated record trailer");

  const uint32_t back_ref = be32(trailer);
  if (back_ref == RECORD_HEADER_SIZE + n) {
    // Normal case: back_ref correctly encodes 20 + payload_len.
  } else if (back_ref == 0) {
    // Some JPEG records use back_ref=0 and are followed by a small number
    // of alignment/padding bytes before the next record's 0xa0 type marker.
    // Scan forward byte-by-byte until we find the next record start.
    uint8_t b;
    for (int limit = 16; limit > 0; --limit) {
      if (!in.read(reinterpret_cast<char*>(&b), 1)) break;
      if (b == 0xa0u) {
        in.seekg(-1, std::ios::cur);
        break;
      }
    }
  } else {
    throw std::runtime_error(
      "ubv: unexpected back_ref " + std::to_string(back_ref)
      + " (expected " + std::to_string(RECORD_HEADER_SIZE + n) + ')');
  }

  return true;
}

// ---------------------------------------------------------------------------
// Write one record to @p out.
// ---------------------------------------------------------------------------
static void write_record(std::ofstream& out,
                         uint32_t type, uint32_t codec, uint64_t ts,
                         const uint8_t* payload, uint32_t n) {
  uint8_t hdr[RECORD_HEADER_SIZE];
  put_be32(hdr + 0,  type);
  put_be32(hdr + 4,  codec);
  put_be64(hdr + 8,  ts);
  put_be32(hdr + 16, n);

  uint8_t trailer[4];
  put_be32(trailer, RECORD_HEADER_SIZE + n);

  out.write(reinterpret_cast<const char*>(hdr), RECORD_HEADER_SIZE);
  if (n > 0)
    out.write(reinterpret_cast<const char*>(payload), n);
  out.write(reinterpret_cast<const char*>(trailer), 4);
}

// ---------------------------------------------------------------------------
// decode -- extract all JPEG frames
// ---------------------------------------------------------------------------
std::vector<Frame> decode(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    throw std::runtime_error("ubv::decode: cannot open " + path);

  std::vector<Frame> frames;
  Record rec;
  while (read_record(in, rec)) {
    if (rec.type != TYPE_JPEG) continue;
    if (rec.payload.size() < 2
        || rec.payload[0] != 0xff || rec.payload[1] != 0xd8)
      continue;  // not a real JPEG, skip
    frames.push_back({rec.timestamp_ms, std::move(rec.payload)});
  }
  return frames;
}

// ---------------------------------------------------------------------------
// decode_one -- find a single frame by timestamp
// ---------------------------------------------------------------------------
Frame decode_one(const std::string& path, uint64_t timestamp_ms) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open())
    throw std::runtime_error("ubv::decode_one: cannot open " + path);

  Record rec;
  while (read_record(in, rec)) {
    if (rec.type != TYPE_JPEG)             continue;
    if (rec.timestamp_ms != timestamp_ms)  continue;
    if (rec.payload.size() < 2
        || rec.payload[0] != 0xff || rec.payload[1] != 0xd8)
      continue;
    return {rec.timestamp_ms, std::move(rec.payload)};
  }
  throw std::runtime_error(
    "ubv::decode_one: no frame with timestamp "
    + std::to_string(timestamp_ms) + " in " + path);
}

// ---------------------------------------------------------------------------
// encode -- write frames into a new UBV file
// ---------------------------------------------------------------------------
void encode(const std::string& path, const std::vector<Frame>& frames) {
  if (frames.empty())
    throw std::runtime_error("ubv::encode: no frames provided");

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open())
    throw std::runtime_error("ubv::encode: cannot open " + path);

  const uint64_t first_ts = frames.front().timestamp_ms;

  // File-header record: fixed 32-byte zeroed payload.
  static const uint8_t file_hdr_payload[32]{};
  write_record(out, TYPE_FILE_HEADER, CODEC_META, first_ts,
               file_hdr_payload, sizeof(file_hdr_payload));

  // One [meta + JPEG] pair per frame.
  static const uint8_t meta_payload[8]{};
  for (const auto& f : frames) {
    write_record(out, TYPE_META, CODEC_META, f.timestamp_ms,
                 meta_payload, sizeof(meta_payload));
    write_record(out, TYPE_JPEG, CODEC_JPEG, f.timestamp_ms,
                 f.jpeg.data(), static_cast<uint32_t>(f.jpeg.size()));
  }

  if (!out.flush())
    throw std::runtime_error("ubv::encode: write error on " + path);
}

// ---------------------------------------------------------------------------
// append -- add one frame to an existing UBV file (create if new)
// ---------------------------------------------------------------------------
void append(const std::string& path, const Frame& frame) {
  // Determine whether the file already exists and has content.
  bool is_new = true;
  {
    std::ifstream probe(path, std::ios::binary | std::ios::ate);
    if (probe.is_open() && probe.tellg() > 0)
      is_new = false;
  }

  static const uint8_t file_hdr_payload[32]{};
  static const uint8_t meta_payload[8]{};

  if (is_new) {
    // Create file and write the file-header record first.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
      throw std::runtime_error("ubv::append: cannot create " + path);

    write_record(out, TYPE_FILE_HEADER, CODEC_META, frame.timestamp_ms,
                 file_hdr_payload, sizeof(file_hdr_payload));
    write_record(out, TYPE_META, CODEC_META, frame.timestamp_ms,
                 meta_payload, sizeof(meta_payload));
    write_record(out, TYPE_JPEG, CODEC_JPEG, frame.timestamp_ms,
                 frame.jpeg.data(), static_cast<uint32_t>(frame.jpeg.size()));

    if (!out.flush())
      throw std::runtime_error("ubv::append: write error on " + path);
  } else {
    // Append a meta + JPEG pair to the existing file.
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out.is_open())
      throw std::runtime_error("ubv::append: cannot open for append " + path);

    write_record(out, TYPE_META, CODEC_META, frame.timestamp_ms,
                 meta_payload, sizeof(meta_payload));
    write_record(out, TYPE_JPEG, CODEC_JPEG, frame.timestamp_ms,
                 frame.jpeg.data(), static_cast<uint32_t>(frame.jpeg.size()));

    if (!out.flush())
      throw std::runtime_error("ubv::append: write error on " + path);
  }
}

}  // namespace ubv
