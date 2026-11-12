// The real dispatcher and engine against the UDP path gate: a receive thread offers
// media through every start and stop, and a stopped engine is never reached again.
// No VM, relay or network; garbage video only reaches the decoder's error path.
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>
#include "remote_media_session.h"

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Registrar final : flutter::TextureRegistrar {
  std::map<int64_t, flutter::TextureVariant*> textures;
  int64_t next = 0;
  int64_t RegisterTexture(flutter::TextureVariant* texture) override { textures.emplace(++next, texture); return next; }
  bool MarkTextureFrameAvailable(int64_t id) override { return textures.count(id) != 0; }
  // Inside RemoteEngine::Stop: the receive thread must already be locked out.
  void UnregisterTexture(int64_t id, std::function<void()> callback) override { attachedWhileStopping += gate && gate->attached() ? 1 : 0; textures.erase(id); if (callback) callback(); }
  bool UnregisterTexture(int64_t id) override { UnregisterTexture(id, {}); return true; }
  multitap::UdpPathGate* gate = nullptr;
  int attachedWhileStopping = 0;
};

std::vector<uint8_t> media(uint8_t kind, bool key, uint16_t epoch, uint64_t pts, size_t payload) {
  std::vector<uint8_t> packet(12 + payload, 0);
  packet[0] = kind; packet[1] = key ? 1 : 0; packet[2] = uint8_t(epoch); packet[3] = uint8_t(epoch >> 8);
  for (unsigned n = 0; n < 8; ++n) packet[4 + n] = uint8_t(pts >> (8 * n));
  return packet;
}
}

int main() {
  try {
    Registrar registrar;
    auto gate = std::make_shared<multitap::UdpPathGate>();
    registrar.gate = gate.get();
    auto session = std::make_unique<multitap::RemoteMediaSession>(&registrar);
    session->UsePathGate(gate);
    gate->Stream(1, 1);
    gate->Engage(false);
    std::atomic<bool> go{true};
    std::atomic<uint64_t> offers{0}, reached{0};
    std::thread receiver([&] {
      uint64_t pts = 1;
      while (go) {
        auto video = media(1, true, 1, pts++, 64), audio = media(2, false, 1, pts, 1920);
        const auto first = gate->OfferUdp(video, true, true, 1, pts);
        gate->OfferUdp(audio, false, false, 1, pts);
        ++offers; reached += first == multitap::UdpPathGate::Delivery::dropped ? 0 : 1;
      }
    });
    int64_t lease = 0, texture = -1;
    for (int cycle = 0; cycle < 40; ++cycle) {
      require(session->Start(++lease, 64, 64, true, texture) == S_OK, "The engine did not start");
      require(gate->attached(), "A running engine is not attached");
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      // A decoder restart replaces the engine in one step.
      if (cycle % 3 == 0) { require(session->Start(++lease, 64, 64, true, texture) == S_OK && gate->attached(), "The replacement is not attached"); }
      require(session->Stop(lease) == S_OK, "The engine did not stop");
      require(!gate->attached(), "A stopped engine stayed attached");
    }
    require(session->Start(++lease, 64, 64, true, texture) == S_OK, "The last engine did not start");
    session.reset();
    require(!gate->attached(), "A closed session left an engine attached");
    go = false; receiver.join();
    // Independent outputs keep two gates. Regranting unchanged epochs still
    // detaches the retired gate before the replacement can accept a socket.
    multitap::RemoteMediaSession dual(&registrar);
    multitap::tracks::Configuration configuration;
    configuration.topology=7;
    for(uint8_t id=0;id<2;++id){multitap::tracks::Track track;track.id=id;track.epoch=3;track.width=track.sourceWidth=64;track.height=track.sourceHeight=64;track.left=id*64;track.primary=id==1;configuration.tracks.push_back(track);}
    std::map<uint8_t,int64_t> textures;
    require(dual.StartTracks(1,configuration,textures)==S_OK,"Dual engines did not start");
    const multitap::udp::TrackBinding first{0,7,3,false},second{1,7,3,true};
    auto a=dual.TrackPathGate(first),b=dual.TrackPathGate(second);
    require(a&&b&&a!=b&&a->attached()&&b->attached(),"Outputs did not own independent receive gates");
    auto replacement=dual.TrackPathGate(first);
    require(replacement&&replacement!=a&&!a->attached()&&b->attached(),"Same-epoch regrant reused a retired receive gate");
    require(!dual.TrackPathGate({0,8,3,false})&&!dual.TrackPathGate({0,7,4,false})&&!dual.TrackPathGate({0,7,3,true}),"A stale/foreign binding acquired a decoder");
    require(dual.StartTracks(2,configuration,textures)==S_OK,"Replacement dual engines did not start");
    require(!replacement->attached()&&!b->attached(),"Decoder replacement left old UDP gates attached");
    auto current=dual.TrackPathGate(first);
    require(current&&current->attached(),"New decoder did not receive a fresh gate");
    require(dual.Stop(2)==S_OK&&!current->attached(),"Track stop did not detach its socket first");
    require(offers.load() > 0 && reached.load() > 0, "The receive thread never reached an engine");
    require(!registrar.attachedWhileStopping, "An engine stopped while the receive thread could still reach it");
    std::cout << "{\"kind\":\"udp-session-smoke\",\"offers\":" << offers.load() << ",\"reached\":" << reached.load() << ",\"realNetwork\":false,\"decodedVideo\":false}" << std::endl;
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAILED: " << error.what() << std::endl;
    return 1;
  }
}
