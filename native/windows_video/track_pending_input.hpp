#pragma once
#include "track_media.hpp"
#include "lossless_tiles.hpp"

namespace multitap::video {
// Scheduling hint for one already-configured decoder. Another output, old
// ownership, audio and lossless tiles cannot advance this H.264 transform.
// Never consumes a packet or substitutes for the normal parse/IDR/epoch gate.
inline bool queuedTrackH264AccessUnit(const uint8_t* bytes, size_t size,
                                     uint32_t topology, uint8_t track,
                                     uint32_t epoch) {
  tracks::Packet packet;
  return tracks::parse(bytes, size, packet) && packet.kind == tracks::video &&
         packet.topology == topology && packet.track == track &&
         packet.epoch == epoch &&
         !lossless::isTilePayload(packet.payload, packet.size);
}
}
