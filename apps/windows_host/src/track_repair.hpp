#pragma once
#include "json_object.hpp"
#include "../../../native/windows_video/track_media.hpp"

namespace host {
// A service-to-worker request, scoped to the UDP leg that needs a picture.
struct TrackRepair {
  uint8_t track;
  uint32_t topology, epoch, generation;
  bool key;
  std::optional<uint64_t> lastGood;

  void mergeInto(std::optional<TrackRepair>& pending) const {
    if (pending && topology == pending->topology && epoch == pending->epoch && generation == pending->generation) {
      pending->key |= key;
      if (lastGood && (!pending->lastGood || *lastGood < *pending->lastGood)) pending->lastGood = lastGood;
    } else {
      pending = *this;
    }
  }
  std::string command(const std::string& session) const {
    return "{\"type\":\"worker.tracks.repair\",\"sessionId\":" + jsonQuote(session) +
      ",\"track\":" + std::to_string(track) + ",\"topologyEpoch\":" + std::to_string(topology) +
      ",\"epoch\":" + std::to_string(epoch) + ",\"generation\":" + std::to_string(generation) +
      ",\"key\":" + (key ? "true" : "false") + (lastGood ? ",\"pts\":" + std::to_string(*lastGood) : "") + "}";
  }
  static std::optional<TrackRepair> parse(const Json& value) {
    const auto number = [&](const char* field, uint64_t low, uint64_t high) -> std::optional<uint64_t> {
      const double n = value.get(field).number(-1);
      if (!std::isfinite(n) || n < double(low) || n > double(high) || std::floor(n) != n) return {};
      return uint64_t(n);
    };
    const auto track = number("track", 0, 1), topology = number("topologyEpoch", 1, UINT32_MAX),
      epoch = number("epoch", 1, UINT16_MAX), generation = number("generation", 1, UINT32_MAX),
      pts = number("pts", 0, multitap::tracks::maximumPts);
    const auto key = std::get_if<bool>(&value.get("key").value);
    if (!track || !topology || !epoch || !generation || !key || (!*key && !pts)) return {};
    return TrackRepair{uint8_t(*track), uint32_t(*topology), uint32_t(*epoch), uint32_t(*generation), *key, pts};
  }
};

// Only on the worker pipe: carry the leg generation in four fixed header
// bytes. The leading T keeps it distinct from legacy media. Even a full-size
// packet still fits the pipe. Restore the TVT1 header before validation.
inline void markTrackRecovery(std::vector<uint8_t>& bytes, uint32_t generation) {
  bytes[5] |= 2;
  bytes[1]=uint8_t(generation); bytes[2]=uint8_t(generation>>8);
  bytes[3]=uint8_t(generation>>16); bytes[7]=uint8_t(generation>>24);
}
inline uint32_t takeTrackRecovery(std::vector<uint8_t>& bytes) {
  namespace wire = multitap::tracks;
  if (bytes.size()<=wire::headerBytes || bytes[0]!='T' || bytes[4]!=wire::video || !(bytes[5]&2)) return 0;
  const uint32_t generation=uint32_t(bytes[1])|(uint32_t(bytes[2])<<8)|(uint32_t(bytes[3])<<16)|(uint32_t(bytes[7])<<24);
  if (generation) { bytes[1]='V'; bytes[2]='T'; bytes[3]='1'; bytes[7]=0; bytes[5]&=uint8_t(~2); }
  return generation;
}
}
