#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>
#include "frame_layout.hpp"

namespace multitap::tracks {
// Candidate media-tracks-v1. Never infer this format from a legacy packet or
// advertise it until broker, owned-output capture and local presenters agree.
constexpr size_t headerBytes = 28, maximumPacketBytes = 4 * 1024 * 1024;
constexpr uint64_t maximumPts = 9007199254740991ULL;
constexpr uint8_t video = 1, audio = 2, audioTrack = 255;
struct Packet {
  uint8_t kind = 0, flags = 0, track = 0;
  uint32_t topology = 0, epoch = 0;
  uint64_t pts = 0;
  const uint8_t* payload = nullptr;
  size_t size = 0;
};
inline uint32_t read32(const uint8_t* p) {
  uint32_t value = 0; for (unsigned n = 0; n < 4; ++n) value |= uint32_t(p[n]) << (8 * n); return value;
}
inline uint64_t read64(const uint8_t* p) {
  uint64_t value = 0; for (unsigned n = 0; n < 8; ++n) value |= uint64_t(p[n]) << (8 * n); return value;
}
inline void write32(uint8_t* p, uint32_t value) { for (unsigned n = 0; n < 4; ++n) p[n] = uint8_t(value >> (8 * n)); }
inline void write64(uint8_t* p, uint64_t value) { for (unsigned n = 0; n < 8; ++n) p[n] = uint8_t(value >> (8 * n)); }
// `opus` (audio-opus-tracks-v1): an audio payload is a u16 sequence + one Opus
// packet of any length instead of whole 4-byte s16le stereo frames.
inline bool parse(const uint8_t* bytes, size_t size, Packet& result, bool opus = false) {
  if (!bytes || size <= headerBytes || size > maximumPacketBytes || bytes[0] != 'T' || bytes[1] != 'V' ||
      bytes[2] != 'T' || bytes[3] != '1' || bytes[7] || read32(bytes + 24) != size - headerBytes) return false;
  Packet value{bytes[4],bytes[5],bytes[6],read32(bytes+8),read32(bytes+12),read64(bytes+16),bytes+headerBytes,size-headerBytes};
  if (!value.topology || value.pts > maximumPts ||
      (value.kind == video ? (value.track > 1 || !value.epoch || (value.flags & ~1)) :
       value.kind == audio ? (value.track != audioTrack || value.epoch || value.flags || (!opus && value.size % 4)) : true)) return false;
  result = value; return true;
}
inline bool pack(const Packet& packet, std::vector<uint8_t>& result, bool opus = false) {
  if (!packet.payload || !packet.size || packet.size > maximumPacketBytes - headerBytes) return false;
  std::vector<uint8_t> value(headerBytes + packet.size);
  value[0]='T';value[1]='V';value[2]='T';value[3]='1';value[4]=packet.kind;value[5]=packet.flags;value[6]=packet.track;
  write32(value.data()+8,packet.topology);write32(value.data()+12,packet.epoch);write64(value.data()+16,packet.pts);write32(value.data()+24,uint32_t(packet.size));
  for (size_t n=0;n<packet.size;++n) value[headerBytes+n]=packet.payload[n];
  Packet verified; if (!parse(value.data(),value.size(),verified,opus)) return false;
  result.swap(value); return true;
}
struct Track {
  uint8_t id = 0;
  uint32_t epoch = 0, width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
  int32_t left = 0, top = 0;
  bool primary = false;
  bool operator==(const Track& b) const {
    return id==b.id && epoch==b.epoch && width==b.width && height==b.height && sourceWidth==b.sourceWidth &&
      sourceHeight==b.sourceHeight && left==b.left && top==b.top && primary==b.primary;
  }
};
struct Configuration {
  uint32_t topology = 0;
  bool audioEnabled = false;
  std::vector<Track> tracks;
  bool audioOpus = false;  // audio-opus-tracks-v1: the shared audio is Opus (see parse)
};
inline bool valid(const Configuration& value) {
  if (!value.topology || value.tracks.empty() || value.tracks.size() > 2) return false;
  uint64_t bytes=0;unsigned mask=0,primary=0;
  for (const auto& t:value.tracks) {
    multitap::video::FrameLayout decoded{},source{};
    if (t.id>1 || !t.epoch || (mask&(1u<<t.id)) || t.width<16 || t.height<16 ||
        !multitap::video::frameLayout(t.width,t.height,decoded)||!multitap::video::frameLayout(t.sourceWidth,t.sourceHeight,source,false)||
        t.sourceWidth<t.width||t.sourceHeight<t.height||
        int64_t(t.left)+t.sourceWidth>std::numeric_limits<int32_t>::max() ||
        int64_t(t.top)+t.sourceHeight>std::numeric_limits<int32_t>::max()||
        !multitap::video::checkedAdd(bytes,uint64_t(decoded.rgbaBytes)+source.rgbaBytes,bytes)) return false;
    mask|=1u<<t.id;primary+=t.primary?1:0;
  }
  if (bytes>uint64_t(std::numeric_limits<std::ptrdiff_t>::max()) || primary>1) return false;
  if (value.tracks.size()==2) {
    const auto& a=value.tracks[0];const auto& b=value.tracks[1];
    if (int64_t(a.left)<int64_t(b.left)+b.sourceWidth && int64_t(b.left)<int64_t(a.left)+a.sourceWidth &&
        int64_t(a.top)<int64_t(b.top)+b.sourceHeight && int64_t(b.top)<int64_t(a.top)+a.sourceHeight) return false;
  }
  return true;
}
// Inspect actual Annex-B NAL types, not the caller's key flag alone. Decoder
// validity remains an independent check; this is not a full H.264 parser.
inline bool initialIdr(const uint8_t* bytes,size_t size) {
  unsigned mask=0;bool first=true;
  for (size_t n=0;n+3<size;) {
    size_t prefix=0;
    if (bytes[n]==0 && bytes[n+1]==0 && bytes[n+2]==1) prefix=3;
    else if (n+4<size && bytes[n]==0 && bytes[n+1]==0 && bytes[n+2]==0 && bytes[n+3]==1) prefix=4;
    if (!prefix) { if (first && bytes[n]!=0) return false; ++n;continue; }
    first=false;const auto header=bytes[n+prefix];if(header&0x80)return false;
    const unsigned type=header&31;if(type==5)mask|=1;if(type==7)mask|=2;if(type==8)mask|=4;
    n+=prefix+1;
  }
  return mask==7;
}
enum class Decision { accepted, stale, awaitingKey, awaitingAllTracks, invalid };
enum class Configured { changed, unchanged, invalid };
class Gate {
 public:
  Configured configure(Configuration value) {
    if (!valid(value) || value.topology<configuration_.topology) return Configured::invalid;
    if (value.topology==configuration_.topology) {
      if (value.audioEnabled!=configuration_.audioEnabled || value.audioOpus!=configuration_.audioOpus || value.tracks.size()!=configuration_.tracks.size()) return Configured::invalid;
      for (const auto& t:value.tracks) if (!state_[t.id] || !(state_[t.id]->track==t)) return Configured::invalid;
      return Configured::unchanged;
    }
    for (const auto& t:value.tracks) if (state_[t.id] && t.epoch<state_[t.id]->track.epoch) return Configured::invalid;
    std::array<std::optional<State>,2> next;
    for (const auto& t:value.tracks) next[t.id]=State{t,false,{}};
    configuration_=std::move(value);state_=std::move(next);audioPts_.reset();return Configured::changed;
  }
  Decision accept(const Packet& packet) {
    // Require parse() even when callers supply an in-memory Packet view.
    if (!packet.payload || !packet.size || packet.size>maximumPacketBytes-headerBytes || !packet.topology ||
        packet.pts>maximumPts || !configuration_.topology) return Decision::invalid;
    if (packet.topology<configuration_.topology) return Decision::stale;
    if (packet.topology!=configuration_.topology) return Decision::invalid;
    if (packet.kind==audio) {
      if (!configuration_.audioEnabled || packet.track!=audioTrack || packet.epoch || packet.flags || (!configuration_.audioOpus && packet.size%4)) return Decision::invalid;
      if (!ready()) return Decision::awaitingAllTracks;
      if (audioPts_ && packet.pts<=*audioPts_) return Decision::invalid;
      audioPts_=packet.pts;return Decision::accepted;
    }
    if (packet.kind!=video || packet.track>1 || !packet.epoch || (packet.flags&~1) || !state_[packet.track]) return Decision::invalid;
    auto& state=*state_[packet.track];
    if (packet.epoch<state.track.epoch) return Decision::stale;
    if (packet.epoch!=state.track.epoch || (state.pts && packet.pts<=*state.pts)) return Decision::invalid;
    if (!state.key) {
      if (!(packet.flags&1)) return Decision::awaitingKey;
      if (!initialIdr(packet.payload,packet.size)) return Decision::invalid;
    }
    state.key=true;state.pts=packet.pts;return Decision::accepted;
  }
  bool ready() const {
    if (!configuration_.topology) return false;
    for (const auto& s:state_) if(s && !s->key)return false;
    return true;
  }
  const Configuration& configuration() const { return configuration_; }
 private:
  struct State { Track track;bool key=false;std::optional<uint64_t> pts; };
  Configuration configuration_;
  std::array<std::optional<State>,2> state_;
  std::optional<uint64_t> audioPts_;
};
}
