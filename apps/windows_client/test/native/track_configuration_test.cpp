// audio-opus-tracks-v1 on the client runner: the independent-output start
// arguments (TrackConfiguration) name Opus only as "audioFormat":"opus", and
// the shared TVT1 wire and gate take an Opus audio payload (a u16 sequence +
// one packet of any length) only in an Opus configuration. No audio device,
// display or network.
#include "remote_track_configuration.h"
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
namespace wire = multitap::tracks;
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
using V = flutter::EncodableValue;
V configuration(const std::optional<V>& format) {
  flutter::EncodableList tracks;
  for (const std::string id : {"0", "1"}) {
    tracks.emplace_back(flutter::EncodableMap{{V("id"), V(id)}, {V("epoch"), V(1)}, {V("width"), V(640)}, {V("height"), V(360)},
      {V("sourceWidth"), V(640)}, {V("sourceHeight"), V(360)}, {V("left"), V(id == "0" ? 0 : 640)}, {V("top"), V(0)}, {V("primary"), V(id == "0")}});
  }
  flutter::EncodableMap map{{V("topologyEpoch"), V(1)}, {V("audioEnabled"), V(true)}, {V("tracks"), V(tracks)}};
  if (format) map.emplace(V("audioFormat"), *format);
  return V(map);
}
// Both outputs' first IDR (SPS, PPS, IDR NAL units), so shared audio may pass.
void keyed(wire::Gate& gate) {
  static const std::vector<uint8_t> key{0, 0, 0, 1, 0x67, 1, 0, 0, 1, 0x68, 1, 0, 0, 1, 0x65, 1};
  for (uint8_t id = 0; id < 2; ++id) require(gate.accept({wire::video, 1, id, 1, 1, 1, key.data(), key.size()}) == wire::Decision::accepted, "Fixture IDR refused");
}
}

int main() {
  try {
    wire::Configuration pcm, opus, refused;
    const auto pcmValue = configuration(std::nullopt), opusValue = configuration(V("opus"));
    require(multitap::TrackConfiguration(&pcmValue, pcm) && pcm.audioEnabled && !pcm.audioOpus, "The released start arguments changed");
    require(multitap::TrackConfiguration(&opusValue, opus) && opus.audioEnabled && opus.audioOpus, "audioFormat opus did not reach the engine");
    for (const auto& format : {V("s16le"), V("OPUS"), V(true), V(1)}) {
      const auto value = configuration(format);
      require(!multitap::TrackConfiguration(&value, refused), "An audio format other than opus was accepted");
    }
    // The wire: an Opus payload is not whole 4-byte PCM frames.
    const std::vector<uint8_t> payload{7, 0, 0xfc, 0xff, 0xfe, 0x01, 0x02};
    const wire::Packet audio{wire::audio, 0, wire::audioTrack, 1, 0, 5, payload.data(), payload.size()};
    std::vector<uint8_t> bytes, pcmBytes;
    require(wire::pack(audio, bytes, true) && !wire::pack(audio, pcmBytes), "TVT1 audio framing did not follow the codec");
    wire::Packet parsed;
    require(wire::parse(bytes.data(), bytes.size(), parsed, true) && parsed.size == payload.size() && !wire::parse(bytes.data(), bytes.size(), parsed), "Opus TVT1 audio parsed outside an Opus stream");
    // The gate: as the configuration says.
    wire::Gate opusGate, pcmGate;
    require(opusGate.configure(opus) == wire::Configured::changed && pcmGate.configure(pcm) == wire::Configured::changed, "Gate fixture");
    keyed(opusGate); keyed(pcmGate);
    require(opusGate.accept(audio) == wire::Decision::accepted, "An Opus gate refused an Opus packet");
    require(pcmGate.accept(audio) == wire::Decision::invalid, "A PCM gate accepted partial frames");
    // The same topology with another audio format is a change, not a duplicate.
    require(opusGate.configure(pcm) == wire::Configured::invalid, "A format change kept the topology");
    std::cout << "{\"kind\":\"track-configuration\",\"opusArguments\":true,\"wire\":true,\"gate\":true,\"audioDevice\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
