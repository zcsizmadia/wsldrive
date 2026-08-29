#pragma once

#include "core/error.hpp"
#include "core/metadata_tree.hpp"
#include "core/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Wire protocol between the two wsldrive agents.
///
/// Every message is a fixed 24-byte little-endian frame header followed by a
/// payload. Payload encoding is hand-rolled: fixed-width integers where the
/// value range is known, LEB128 varints for sizes and counts, and
/// length-prefixed byte strings. Decoding returns views into the input buffer
/// (no copies) wherever the consumer can work with a view.
namespace wsld::proto {

inline constexpr std::uint32_t kMagic = 0x444C5357;  // "WSLD" when read as little-endian bytes
inline constexpr std::uint16_t kVersion = 2;  // 2: Hello carries an auth token
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::uint32_t kMaxPayload = 64u << 20;  // 64 MiB per frame

// FrameHeader::flags bits.
inline constexpr std::uint32_t kFlagMore = 0x1;  // more frames follow for this response (streamed reply)

enum class MsgType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  SnapshotRequest = 3,
  Snapshot = 4,
  Invalidation = 5,
  ReadRequest = 6,
  ReadResponse = 7,
  Error = 8,
  Ping = 9,
  Pong = 10,
  // Write-through mutations (Phase 3).
  WriteRequest = 11,
  WriteResponse = 12,
  CreateRequest = 13,   // create/replace an empty file
  CreateResponse = 14,  // carries the new file's attributes
  MkdirRequest = 15,
  UnlinkRequest = 16,
  RmdirRequest = 17,
  RenameRequest = 18,
  TruncateRequest = 19,
  Ok = 20,  // generic success for mutations with no payload
  // Bulk read for prefetch/read-ahead (Direction B cold-read latency).
  ReadManyRequest = 21,
  ReadManyResponse = 22,
};

struct FrameHeader {
  MsgType type = MsgType::Ping;
  std::uint32_t flags = 0;
  std::uint32_t payload_len = 0;
  std::uint64_t request_id = 0;

  friend bool operator==(const FrameHeader&, const FrameHeader&) = default;
};

void encode_header(const FrameHeader& h, std::span<std::byte, kHeaderSize> out) noexcept;
[[nodiscard]] Result<FrameHeader> decode_header(std::span<const std::byte> in) noexcept;

// --- primitives --------------------------------------------------------------

class Writer {
 public:
  explicit Writer(std::vector<std::byte>& out) noexcept : out_(out) {}

  void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
  void u16(std::uint16_t v);
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
  void varint(std::uint64_t v);
  void raw(std::span<const std::byte> b);
  void string(std::string_view s);         // varint length + bytes
  void blob(std::span<const std::byte> b);  // varint length + bytes

  [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
  void patch_u32(std::size_t pos, std::uint32_t v) noexcept;

 private:
  std::vector<std::byte>& out_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> in) noexcept : in_(in) {}

  [[nodiscard]] Result<std::uint8_t> u8() noexcept;
  [[nodiscard]] Result<std::uint16_t> u16() noexcept;
  [[nodiscard]] Result<std::uint32_t> u32() noexcept;
  [[nodiscard]] Result<std::uint64_t> u64() noexcept;
  [[nodiscard]] Result<std::int64_t> i64() noexcept {
    return u64().transform([](std::uint64_t v) { return static_cast<std::int64_t>(v); });
  }
  [[nodiscard]] Result<std::uint64_t> varint() noexcept;
  [[nodiscard]] Result<std::span<const std::byte>> raw(std::size_t n) noexcept;
  [[nodiscard]] Result<std::string_view> string() noexcept;
  [[nodiscard]] Result<std::span<const std::byte>> blob() noexcept;

  [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }
  [[nodiscard]] bool empty() const noexcept { return pos_ == in_.size(); }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }

 private:
  std::span<const std::byte> in_;
  std::size_t pos_ = 0;
};

// --- messages ----------------------------------------------------------------

struct Hello {
  std::uint32_t protocol_version = kVersion;
  std::uint64_t capabilities = 0;
  std::string_view agent;  // free-form "name/version" for diagnostics
  // Shared secret proving the peer was started by the same launcher. Empty when
  // the agent runs with --insecure-no-auth.
  std::string_view token;
};

struct InvalidationOp {
  InvalidationKind kind;
  std::string path;  // normalised, '/'-separated, relative to the mount root
  Attributes attr;   // meaningful for Upsert only
};

struct InvalidationBatch {
  std::uint64_t generation = 0;
  std::vector<InvalidationOp> ops;
};

struct ReadRequest {
  std::string_view path;
  std::uint64_t offset = 0;
  std::uint32_t length = 0;
};

struct ReadResponse {
  std::uint64_t file_size = 0;
  std::span<const std::byte> data;  // view into the frame payload
};

struct ErrorMessage {
  std::uint32_t code = 0;  // Errc value
  std::string_view detail;
};

struct WriteRequest {
  std::string_view path;
  std::uint64_t offset = 0;
  std::span<const std::byte> data;
};
struct WriteResponse {
  std::uint64_t written = 0;
};
struct CreateRequest {
  std::string_view path;
  std::uint32_t mode = 0644;
};
struct MkdirRequest {
  std::string_view path;
  std::uint32_t mode = 0755;
};
struct PathRequest {  // UnlinkRequest / RmdirRequest
  std::string_view path;
};
struct TruncateRequest {
  std::string_view path;
  std::uint64_t size = 0;
};
struct RenameRequest {
  std::string_view from;
  std::string_view to;
};

// Bulk read: request several files, get each one's whole contents (or a
// not-found marker) back in a single frame.
struct ReadManyRequest {
  std::vector<std::string_view> paths;
};
struct ReadManyItem {
  bool ok = false;
  std::span<const std::byte> data;  // valid iff ok; view into the frame payload
};
struct ReadManyResponse {
  std::vector<ReadManyItem> items;
};

void write_attributes(Writer& w, const Attributes& a);
[[nodiscard]] Result<Attributes> read_attributes(Reader& r) noexcept;

void write_hello(Writer& w, const Hello& h);
[[nodiscard]] Result<Hello> read_hello(Reader& r) noexcept;

/// Snapshot payload: u64 generation, u32 count, then `count` entries.
void write_snapshot_header(Writer& w, std::uint64_t generation, std::uint32_t count);
void write_snapshot_entry(Writer& w, const SnapshotEntry& e);

struct SnapshotHeader {
  std::uint64_t generation;
  std::uint32_t count;
};
[[nodiscard]] Result<SnapshotHeader> read_snapshot_header(Reader& r) noexcept;
[[nodiscard]] Result<SnapshotEntry> read_snapshot_entry(Reader& r) noexcept;

/// Streams a full snapshot payload to `on_entry(const SnapshotEntry&)` without
/// materialising a vector. Returns the generation.
template <class F>
[[nodiscard]] Result<std::uint64_t> read_snapshot(Reader& r, F&& on_entry) noexcept {
  auto hdr = read_snapshot_header(r);
  if (!hdr) return fail(hdr.error());
  for (std::uint32_t i = 0; i < hdr->count; ++i) {
    auto e = read_snapshot_entry(r);
    if (!e) return fail(e.error());
    on_entry(*e);
  }
  return hdr->generation;
}

void write_invalidation(Writer& w, const InvalidationBatch& b);
[[nodiscard]] Result<InvalidationBatch> read_invalidation(Reader& r) noexcept;

void write_read_request(Writer& w, const ReadRequest& q);
[[nodiscard]] Result<ReadRequest> read_read_request(Reader& r) noexcept;

void write_read_response(Writer& w, const ReadResponse& p);
[[nodiscard]] Result<ReadResponse> read_read_response(Reader& r) noexcept;

void write_error(Writer& w, const ErrorMessage& e);
[[nodiscard]] Result<ErrorMessage> read_error(Reader& r) noexcept;

void write_write_request(Writer& w, const WriteRequest& q);
[[nodiscard]] Result<WriteRequest> read_write_request(Reader& r) noexcept;
void write_write_response(Writer& w, const WriteResponse& p);
[[nodiscard]] Result<WriteResponse> read_write_response(Reader& r) noexcept;

void write_create_request(Writer& w, const CreateRequest& q);
[[nodiscard]] Result<CreateRequest> read_create_request(Reader& r) noexcept;

void write_mkdir_request(Writer& w, const MkdirRequest& q);
[[nodiscard]] Result<MkdirRequest> read_mkdir_request(Reader& r) noexcept;

void write_path_request(Writer& w, const PathRequest& q);
[[nodiscard]] Result<PathRequest> read_path_request(Reader& r) noexcept;

void write_truncate_request(Writer& w, const TruncateRequest& q);
[[nodiscard]] Result<TruncateRequest> read_truncate_request(Reader& r) noexcept;

void write_rename_request(Writer& w, const RenameRequest& q);
[[nodiscard]] Result<RenameRequest> read_rename_request(Reader& r) noexcept;

void write_read_many_request(Writer& w, const ReadManyRequest& q);
[[nodiscard]] Result<ReadManyRequest> read_read_many_request(Reader& r);
void write_read_many_response(Writer& w, const ReadManyResponse& p);
[[nodiscard]] Result<ReadManyResponse> read_read_many_response(Reader& r);

/// Reserves a frame header at the end of `buf`, lets `body(Writer&)` append the
/// payload, then fills in the header with the actual payload length.
template <class F>
void write_frame(std::vector<std::byte>& buf, MsgType type, std::uint64_t request_id, F&& body,
                 std::uint32_t flags = 0) {
  const std::size_t start = buf.size();
  buf.resize(start + kHeaderSize);
  Writer w(buf);
  body(w);
  FrameHeader h;
  h.type = type;
  h.flags = flags;
  h.payload_len = static_cast<std::uint32_t>(buf.size() - start - kHeaderSize);
  h.request_id = request_id;
  encode_header(h, std::span<std::byte, kHeaderSize>(buf.data() + start, kHeaderSize));
}

}  // namespace wsld::proto
