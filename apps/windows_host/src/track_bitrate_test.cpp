// bitrate-adaptive-tracks-v1 (track_bitrate.hpp): the share split, the worker
// command, the session.start rule, and the session on a simulated link (two
// outputs, a FIFO bottleneck, client feedback every 100 ms). Simulated only:
// no relay, no encoder, no client.
#include "track_bitrate.hpp"
#include "track_tunnel.hpp"
#include <iostream>

using namespace host;
namespace {
int checks = 0;
void require(bool ok, const std::string& reason) { ++checks; if (!ok) throw std::runtime_error(reason); }

void split() {
  const std::vector<uint64_t> even{1920ull * 1080, 1920ull * 1080};
  // Both still: each keeps its part (nobody needs more).
  auto shares = splitTrackBitrate(20000000, even, {0, 0});
  require(shares == std::vector<int>{10000000, 10000000}, "still outputs: not their parts");
  // One moving at its part, one still: the moving one gets the other's unused part.
  shares = splitTrackBitrate(20000000, even, {10000000, 0});
  require(shares[0] == 20000000 && shares[1] == 10000000, "one moving: it did not get the still output's part");
  // Both moving: their parts.
  shares = splitTrackBitrate(20000000, even, {10000000, 9000000});
  require(shares == std::vector<int>{10000000, 10000000}, "both moving: not their parts");
  // Light use (typing) is no need: nothing is lent.
  shares = splitTrackBitrate(20000000, even, {2000000, 0});
  require(shares == std::vector<int>{10000000, 10000000}, "light use borrowed the other part");
  // A partly used part lends only what is left beyond 1.25 x its use.
  shares = splitTrackBitrate(40000000, even, {20000000, 8000000});
  require(shares[0] == 30000000 && shares[1] == 20000000, "partly used part: wrong loan");
  // Parts follow the areas.
  shares = splitTrackBitrate(50000000, {1920ull * 1080, 3840ull * 2160}, {0, 0});
  require(shares[0] == 10000000 && shares[1] == 40000000, "parts do not follow the areas");
  // Never below the encoder's minimum nor above the target.
  shares = splitTrackBitrate(2000000, {640ull * 360, 3840ull * 2160}, {0, 0});
  require(shares[0] == encoderMinimumBitrate && shares[1] <= 2000000, "a tiny output went below the encoder minimum");
  require(splitTrackBitrate(20000000, {}, {}).empty() && splitTrackBitrate(20000000, {1}, {}).empty(), "a malformed split was not empty");
}

void command() {
  const auto parsed = trackBitrateCommand(JsonParser::parse(R"({"type":"stream.tracks.bitrate","sessionId":"s","topologyEpoch":3,"tracks":[{"id":"0","bitrate":15000000},{"id":"1","bitrate":5000000}]})"));
  require(parsed && parsed->first == 3 && parsed->second.size() == 2 && parsed->second[0] == std::make_pair(uint8_t(0), 15000000) && parsed->second[1] == std::make_pair(uint8_t(1), 5000000), "command was not read");
  for (const char* bad : {R"({"topologyEpoch":0,"tracks":[{"id":"0","bitrate":15000000}]})", R"({"topologyEpoch":1,"tracks":[{"id":"2","bitrate":15000000}]})",
                          R"({"topologyEpoch":1,"tracks":[{"id":"0","bitrate":400000}]})", R"({"topologyEpoch":1,"tracks":[{"id":"0","bitrate":100000001}]})",
                          R"({"topologyEpoch":1,"tracks":[{"id":"0","bitrate":1500000.5}]})", R"({"topologyEpoch":1,"tracks":[{"id":"0","bitrate":1500000},{"id":"0","bitrate":1500000}]})",
                          R"({"topologyEpoch":1,"tracks":[]})", R"({"topologyEpoch":1})"})
    require(!trackBitrateCommand(JsonParser::parse(bad)), std::string("a malformed command was read: ") + bad);
}

void request() {
  const auto start = [](const std::string& extra) { return JsonParser::parse(R"({"type":"session.start","mediaProtocol":"tracks-v1","bitrate":20000000,"bitrateMax":100000000)" + extra + "}"); };
  const auto granted = adaptiveTracksBitrateRequest(start(R"(,"bitrateMode":"auto","bitrateTracksProtocol":"bitrate-adaptive-tracks-v1")"));
  require(granted.adaptive && granted.bitrate == 20000000 && granted.maximum == 100000000, "a granted session.start was not automatic");
  require(!adaptiveTracksBitrateRequest(start(R"(,"bitrateMode":"auto")")).adaptive, "auto without the marker was automatic");
  require(!adaptiveTracksBitrateRequest(start(R"(,"bitrateTracksProtocol":"bitrate-adaptive-tracks-v1")")).adaptive, "the marker without auto was automatic");
  require(!adaptiveTracksBitrateRequest(JsonParser::parse(R"({"bitrateMode":"auto","bitrateTracksProtocol":"bitrate-adaptive-tracks-v1","bitrate":20000000})")).adaptive, "one output took the tracks rule");
  // The one-output rule still leaves independent outputs fixed.
  require(!adaptiveBitrateRequest(start(R"(,"bitrateMode":"auto","bitrateTracksProtocol":"bitrate-adaptive-tracks-v1")")).adaptive, "the one-output controller took independent outputs");
  // The tunnel lets the client's feedback through only in such a session.
  require(trackTunnelControlAllowed("stream.feedback", false, true) && !trackTunnelControlAllowed("stream.feedback", false, false) && !trackTunnelControlAllowed("stream.feedback", true, false), "tunnel feedback rule");
  require(trackTunnelAdaptive(start(R"(,"bitrateMode":"auto","bitrateTracksProtocol":"bitrate-adaptive-tracks-v1")")) && !trackTunnelAdaptive(start(R"(,"bitrateMode":"auto")")), "tunnel adaptive rule");
}

std::vector<uint8_t> packet(uint8_t kind, uint8_t track, uint32_t topology, uint64_t pts, size_t payload, bool tile = false) {
  std::vector<uint8_t> bytes(28 + payload, 0);
  bytes[0] = 'T'; bytes[1] = 'V'; bytes[2] = 'T'; bytes[3] = '1'; bytes[4] = kind; bytes[6] = track;
  for (int i = 0; i < 4; ++i) { bytes[8 + i] = uint8_t(topology >> (8 * i)); bytes[12 + i] = uint8_t(1 >> (8 * i)); bytes[24 + i] = uint8_t(uint32_t(payload) >> (8 * i)); }
  for (int i = 0; i < 8; ++i) bytes[16 + i] = uint8_t(pts >> (8 * i));
  if (tile && payload >= 4) { bytes[28] = 'T'; bytes[29] = 'S'; bytes[30] = 'L'; bytes[31] = '1'; }
  return bytes;
}
std::vector<uint8_t> text(const std::string& value) { return {value.begin(), value.end()}; }
std::string info(uint32_t topology, int w0 = 1920, int h0 = 1080, int w1 = 1920, int h1 = 1080) {
  return R"({"type":"stream.tracks.info","sessionId":"s","codec":"h264","topologyEpoch":)" + std::to_string(topology) + R"(,"fpsTarget":60,"bitrateTarget":20000000,"tracks":[{"id":"0","epoch":1,"width":)" + std::to_string(w0) + R"(,"height":)" + std::to_string(h0) +
         R"(,"primary":true,"bitrateTarget":10000000},{"id":"1","epoch":1,"width":)" + std::to_string(w1) + R"(,"height":)" + std::to_string(h1) + R"(,"primary":false,"bitrateTarget":10000000}]})";
}
std::map<uint8_t, int> rates(const std::string& command) {
  std::map<uint8_t, int> result; const auto parsed = trackBitrateCommand(JsonParser::parse(command));
  require(parsed.has_value(), "the session wrote a command the worker refuses: " + command);
  for (const auto& [id, rate] : parsed->second) result[id] = rate;
  return result;
}

void session() {
  uint64_t now = 1000; int wakes = 0;
  TrackBitrateSession rate; rate.clock([&] { return now; });
  rate.start("s", {}, 60, [&] { ++wakes; });
  require(!rate.active() && !rate.takeCommand(), "a fixed session ran the controller");
  rate.start("s", {true, 20000000, 100000000}, 60, [&] { ++wakes; });
  require(rate.active() && !rate.takeCommand(), "a command before the outputs were known");
  rate.outgoing(true, text(info(1)));
  auto command = rate.takeCommand();
  require(wakes >= 1 && command && rates(*command) == std::map<uint8_t, int>{{uint8_t(0), 10000000}, {uint8_t(1), 10000000}}, "the first topology's parts were not sent");
  require(command->find("\"topologyEpoch\":1") != std::string::npos && command->find("\"sessionId\":\"s\"") != std::string::npos, "the command does not name its session and topology");
  require(!rate.takeCommand(), "the same shares were sent twice");
  // Output 0 moves at its part for half a second, output 1 is still: the next split lends it output 1's part.
  for (int frame = 0; frame < 40; ++frame) { now += 16667; rate.outgoing(false, packet(1, 0, 1, now, 10000000 / 8 / 60)); }
  command = rate.takeCommand();
  require(command && rates(*command)[0] >= 19000000 && rates(*command)[1] == 10000000, "a moving output was not lent the still one's part");
  // Packets of another topology, audio, tiles and malformed packets count for the controller only.
  rate.outgoing(false, packet(2, 255, 1, now, 400));
  rate.outgoing(false, packet(1, 1, 7, now, 400));
  auto broken = packet(1, 0, 1, now, 400); broken.pop_back(); rate.outgoing(false, broken);
  // The statistics carry the session's rate and the shares.
  auto stats = JsonParser::parse(R"({"type":"stream.tracks.stats","sessionId":"s","topologyEpoch":1,"measuredMs":1000,"tracks":[{"id":"0","epoch":1,"capturedFps":60,"encodedFps":60,"bitrateKbps":1},{"id":"1","epoch":1,"capturedFps":0,"encodedFps":0,"bitrateKbps":0}]})");
  rate.annotate(stats);
  require(stats.get("bitrateTarget").number() == 20000000 && stats.get("bitrateMax").number() == 100000000, "the statistics do not carry the rate");
  const auto& rows = std::get<Json::Array>(stats.get("tracks").value);
  require(rows[0].get("bitrateTarget").number() >= 19000000 && rows[1].get("bitrateTarget").number() == 10000000, "the statistics do not carry the shares");
  auto other = JsonParser::parse(R"({"type":"stream.tracks.stats","sessionId":"x","topologyEpoch":1,"measuredMs":1000,"tracks":[]})");
  rate.annotate(other); require(std::holds_alternative<std::nullptr_t>(other.get("bitrateTarget").value), "another session's statistics were annotated");
  // A new topology (a resize): its outputs and a command for its new encoders.
  rate.outgoing(true, text(info(2, 3840, 2160, 1920, 1080)));
  command = rate.takeCommand();
  require(command && command->find("\"topologyEpoch\":2") != std::string::npos && rates(*command)[1] < rates(*command)[0], "a new topology got no command for its new encoders");
  // A 4K output raises the floor to the sum of the outputs' own floors (19.9 + 5.0 Mbps).
  require(rate.target() >= 24800000, "the floor is not the sum of the outputs' floors");
  // UDP (back on two 1080p outputs, floor 10 Mbps): both legs on it, then the leg feedback moves the controller.
  rate.outgoing(true, text(info(3))); require(rate.takeCommand().has_value(), "the third topology got no command");
  require(rate.udpPath("0", true) > 0, "a leg got no share");
  require(rate.udpFeedback("0", 20000, 400, 0) > 0, "leg feedback returned no share");
  rate.udpPath("1", true);
  for (int n = 0; n < 5; ++n) { now += 100000; rate.udpFeedback("0", 20000, 400, 0); rate.udpFeedback("1", 20000, 400, 0); }
  const int before = rate.target();
  now += 100000; rate.udpFeedback("0", 20000, 300, 100); now += 100000; rate.udpFeedback("1", 20000, 300, 100);
  require(rate.target() < before, "losses on both UDP legs did not lower the target");
  rate.stop();
  require(!rate.active() && !rate.takeCommand() && rate.udpFeedback("0", 1, 1, 0) == 0, "a stopped session still answered");
}

// A FIFO bottleneck of `capacity` with 20 ms propagation; each output codes its
// share as 60 frames a second while it moves; the client reports arrivals every
// 100 ms, as RemoteSession does; the worker applies each command at once.
struct Link {
  struct Item { uint64_t pts; size_t bytes; uint64_t leaveUs = 0; };
  uint64_t now = 1000; double capacity; std::deque<Item> queue; double credit = 0;
  std::vector<std::pair<uint64_t, uint64_t>> arrivals; uint64_t delivered = 0, lastReport = 1000;
  std::vector<double> delays;
  TrackBitrateSession rate; std::map<uint8_t, int> encoders{{uint8_t(0), 10000000}, {uint8_t(1), 10000000}};
  explicit Link(double bitsPerSecond) : capacity(bitsPerSecond) {
    rate.clock([this] { return now; });
    rate.start("s", {true, 20000000, 100000000}, 60, [] {});
    rate.outgoing(true, text(info(1)));
  }
  void run(int seconds, std::array<bool, 2> moving) {
    const uint64_t end = now + uint64_t(seconds) * 1000000;
    uint64_t nextFrame = now;
    while (now < end) {
      now += 1000;
      if (auto command = rate.takeCommand()) encoders = rates(*command);
      if (now >= nextFrame) {
        nextFrame += 16667;
        for (uint8_t id = 0; id < 2; ++id) if (moving[id]) {
          const auto bytes = packet(1, id, 1, now + id, size_t(encoders[id] / 8 / 60));
          rate.outgoing(false, bytes); queue.push_back({now + id, bytes.size()}); rate.written(bytes.size());
        }
      }
      credit += capacity / 8 / 1000;
      while (!queue.empty() && credit >= double(queue.front().bytes)) {
        credit -= double(queue.front().bytes);
        const auto arrived = now + 20000;
        arrivals.push_back({queue.front().pts, arrived}); delivered += queue.front().bytes;
        delays.push_back(double(arrived - queue.front().pts) / 1000);
        queue.pop_front();
      }
      if (queue.empty()) credit = std::min(credit, capacity / 8 / 100);
      if (now - lastReport >= 100000) {
        std::string frames = "[";
        for (size_t n = arrivals.size() > 64 ? arrivals.size() - 64 : 0; n < arrivals.size(); ++n) frames += (frames.size() > 1 ? "," : "") + std::string("[") + std::to_string(arrivals[n].first) + "," + std::to_string(arrivals[n].second) + "]";
        frames += "]";
        rate.feedback(JsonParser::parse(R"({"type":"stream.feedback","sessionId":"s","epoch":1,"frames":)" + frames + R"(,"bytes":)" + std::to_string(delivered) + R"(,"intervalUs":)" + std::to_string(now - lastReport) + R"(,"relayQueuedBytes":0})"));
        arrivals.clear(); delivered = 0; lastReport = now;
      }
    }
  }
  double p95() { auto copy = delays; std::sort(copy.begin(), copy.end()); return copy.empty() ? 0 : copy[copy.size() * 95 / 100]; }
};

void simulated() {
  // A fast path, one output moving (a map on one monitor, code on the other):
  // the rate rises well past the start and the moving output gets most of it.
  { Link link(200e6); link.run(30, {true, false});
    require(link.rate.target() >= 60000000, "fast path, one moving: the rate did not rise (" + std::to_string(link.rate.target()) + ")");
    require(link.encoders[0] >= 0.8 * link.rate.target(), "fast path, one moving: the moving output did not get most of the rate");
    std::cout << "{\"kind\":\"track-bitrate-sim\",\"case\":\"fast-one-moving\",\"targetMbps\":" << link.rate.target() / 1e6 << ",\"moving\":" << link.encoders[0] / 1e6 << ",\"still\":" << link.encoders[1] / 1e6 << "}\n"; }
  // A 30 Mbps path, both moving: the rate settles at the path and the queue stays short.
  { Link link(30e6); link.run(20, {true, true}); link.delays.clear(); link.run(20, {true, true});
    require(link.rate.target() <= 34000000, "30 Mbps, both moving: the rate stayed above the path (" + std::to_string(link.rate.target()) + ")");
    require(link.rate.target() >= 18000000, "30 Mbps, both moving: the rate fell far below the path (" + std::to_string(link.rate.target()) + ")");
    require(link.p95() <= 250, "30 Mbps, both moving: the delay stayed long (p95 " + std::to_string(link.p95()) + " ms)");
    require(std::abs(link.encoders[0] - link.encoders[1]) <= link.encoders[0] / 10, "30 Mbps, both moving: the shares are not even");
    std::cout << "{\"kind\":\"track-bitrate-sim\",\"case\":\"30mbps-both-moving\",\"targetMbps\":" << link.rate.target() / 1e6 << ",\"delayP95Ms\":" << link.p95() << "}\n"; }
  // The still output starts moving: the moving one gives back its loan.
  { Link link(200e6); link.run(20, {true, false}); link.run(2, {true, true});
    require(std::abs(link.encoders[0] - link.encoders[1]) <= link.encoders[0] / 10, "a second output started moving and did not get its part back");
    std::cout << "{\"kind\":\"track-bitrate-sim\",\"case\":\"second-starts\",\"targetMbps\":" << link.rate.target() / 1e6 << ",\"first\":" << link.encoders[0] / 1e6 << ",\"second\":" << link.encoders[1] / 1e6 << "}\n"; }
}
}

int main() {
  try {
    split(); command(); request(); session(); simulated();
    std::cout << "{\"kind\":\"track-bitrate\",\"checks\":" << checks << ",\"simulatedLink\":true,\"realRelay\":false,\"realEncoder\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
}
