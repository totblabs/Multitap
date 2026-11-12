// Broker channel send queue (send_queue.hpp): a media backlog degrades the
// stream (video waits for the next IDR) and never closes the channel; control
// messages and cursor records are never dropped.
#include "send_queue.hpp"
#include "audio_stats.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
host::OutgoingPacket media(uint8_t kind, uint64_t pts, size_t size, bool key = false) {
  host::OutgoingPacket packet; packet.bytes.assign(12 + size, 0); packet.bytes[0] = kind; packet.bytes[1] = key ? 1 : 0;
  for (int n = 0; n < 8; ++n) packet.bytes[4 + n] = uint8_t(pts >> (8 * n));
  return packet;
}
host::OutgoingPacket tiles(uint64_t pts, size_t size) { auto packet = media(1, pts, size); packet.bytes[12] = 'T'; packet.bytes[13] = 'S'; packet.bytes[14] = 'L'; packet.bytes[15] = '1'; return packet; }
host::OutgoingPacket text(const std::string& value) { host::OutgoingPacket packet; packet.text = true; packet.bytes.assign(value.begin(), value.end()); return packet; }
uint64_t pts(const host::OutgoingPacket& packet) { uint64_t value = 0; for (int n = 7; n >= 0; --n) value = (value << 8) | packet.bytes[4 + n]; return value; }
}
int main() {
  try {
    int checks = 0;
    using host::OutgoingClass;
    require(host::outgoingClass(media(1, 1, 10)) == OutgoingClass::video && host::outgoingClass(media(1, 1, 10, true)) == OutgoingClass::keyVideo &&
            host::outgoingClass(tiles(1, 30)) == OutgoingClass::tiles && host::outgoingClass(media(2, 1, 16)) == OutgoingClass::audio &&
            host::outgoingClass(media(3, 1, 16)) == OutgoingClass::control && host::outgoingClass(text("{}")) == OutgoingClass::control, "Packet classes");
    ++checks;
    { // The released fixed limit (32): the 33rd frame drops the backlog and waits for an IDR.
      host::HostSendQueue queue; size_t requests = 0;
      for (uint64_t n = 1; n <= 32; ++n) require(queue.admit(media(1, n, 1000), 32).queued, "Frame within the limit was not queued");
      auto overflow = queue.admit(media(1, 33, 1000), 32);
      require(!overflow.queued && !overflow.stalled && overflow.requestKeyFrame && overflow.droppedVideo == 33 && queue.empty() && queue.awaitingKeyFrame(), "Overflow did not drop the stale video and ask for an IDR");
      for (uint64_t n = 34; n <= 40; ++n) { const auto result = queue.admit(media(1, n, 1000), 32); require(!result.queued && result.droppedVideo == 1, "A P frame after the drop was sent"); requests += result.requestKeyFrame; }
      require(requests == 7 && queue.empty(), "Waiting P frames did not keep asking for the IDR");
      require(queue.admit(text("{\"type\":\"stream.stats\"}"), 32).queued && queue.admit(media(2, 41, 3840), 32).queued, "Control/audio stopped while video waited");
      const auto key = queue.admit(media(1, 42, 50000, true), 32);
      require(key.queued && !key.requestKeyFrame && !queue.awaitingKeyFrame(), "The IDR did not end the drop");
      require(queue.admit(media(1, 43, 1000), 32).queued, "Video after the IDR was dropped");
      host::OutgoingPacket out; std::vector<uint64_t> order; while (queue.pop(out)) if (!out.text) order.push_back(pts(out));
      require((order == std::vector<uint64_t>{41, 42, 43}) && queue.bytes() == 0 && queue.packets() == 0, "Queue order or accounting changed");
      ++checks; }
    { // Byte bound: 3 x 3 MiB frames exceed 8 MiB; an IDR then fits again.
      host::HostSendQueue queue;
      require(queue.admit(media(1, 1, 3 << 20, true), 128).queued && queue.admit(media(1, 2, 3 << 20), 128).queued, "Large frames within the byte bound");
      const auto third = queue.admit(media(1, 3, 3 << 20), 128);
      require(!third.queued && third.droppedVideo == 3 && third.requestKeyFrame && queue.bytes() == 0, "Byte overflow did not drop the video backlog");
      require(queue.admit(media(1, 4, 3 << 20, true), 128).queued, "IDR after a byte overflow was refused");
      ++checks; }
    { // Control messages and cursor records never count against media and are never dropped.
      host::HostSendQueue queue;
      for (uint64_t n = 1; n <= 32; ++n) queue.admit(media(1, n, 1000), 32);
      require(queue.admit(text("{\"type\":\"pong\"}"), 32).queued && queue.admit(media(3, 33, 64), 32).queued, "A full media queue refused control/cursor");
      for (int n = 0; n < 1000; ++n) require(queue.admit(media(3, 100 + n, 64), 32).queued, "A cursor record was dropped");
      require(queue.packets() == 32 + 2 + 1000, "Control accounting changed");
      ++checks; }
    { // Only a control backlog beyond its bound reports a stalled sender.
      host::HostSendQueue queue; size_t queued = 0; host::SendQueueAdmission last;
      for (size_t n = 0; n <= host::HostSendQueue::controlPacketsLimit; ++n) { last = queue.admit(text("{}"), 32); queued += last.queued; }
      require(queued == host::HostSendQueue::controlPacketsLimit && last.stalled && !last.queued, "Control bound did not report a stalled sender");
      host::HostSendQueue bytes; const std::string big(1 << 20, 'x');
      for (int n = 0; n < 4; ++n) require(bytes.admit(text(big), 32).queued, "Control bytes within the bound");
      require(bytes.admit(text(big), 32).stalled, "Control byte bound did not report a stall");
      ++checks; }
    { // Tiles and audio are skipped, never awaited; a full queue of audio makes room for the IDR.
      host::HostSendQueue queue;
      for (uint64_t n = 1; n <= 8; ++n) queue.admit(media(2, n, 3840), 8);
      const auto tile = queue.admit(tiles(9, 1000), 8);
      require(!tile.queued && tile.droppedOther == 1 && !tile.requestKeyFrame && !queue.awaitingKeyFrame(), "A tile packet that did not fit changed the video state");
      const auto video = queue.admit(media(1, 10, 1000), 8);
      require(!video.queued && video.droppedVideo == 1 && video.requestKeyFrame && queue.awaitingKeyFrame(), "A P frame that did not fit did not wait for an IDR");
      const auto key = queue.admit(media(1, 11, 1000, true), 8);
      require(key.queued && key.droppedOther == 1 && !queue.awaitingKeyFrame(), "The IDR did not replace the oldest audio");
      ++checks; }
    { // Cursor states coalesce: a stalled uplink with echoes at 240/s for 10 s
      // (2400 states) keeps one waiting state per epoch, never a stalled channel;
      // shapes are kept in order before it, and the newest position survives a
      // later state without one.
      host::HostSendQueue queue;
      const auto state = [](uint16_t epoch, uint8_t flags, uint32_t id, int32_t x, int32_t y) {
        auto packet = media(3, 1, 16); packet.bytes[2] = uint8_t(epoch); packet.bytes[3] = uint8_t(epoch >> 8); packet.bytes[12] = 2; packet.bytes[13] = flags;
        for (int n = 0; n < 4; ++n) { packet.bytes[16 + n] = uint8_t(id >> (8 * n)); packet.bytes[20 + n] = uint8_t(uint32_t(x) >> (8 * n)); packet.bytes[24 + n] = uint8_t(uint32_t(y) >> (8 * n)); }
        return packet;
      };
      const auto shape = [](uint16_t epoch, uint32_t id) { auto packet = media(3, 1, 16 + 64); packet.bytes[2] = uint8_t(epoch); packet.bytes[12] = 1; for (int n = 0; n < 4; ++n) packet.bytes[16 + n] = uint8_t(id >> (8 * n)); return packet; };
      const auto read32 = [](const host::OutgoingPacket& packet, size_t at) { uint32_t value = 0; for (int n = 3; n >= 0; --n) value = (value << 8) | packet.bytes[at + n]; return value; };
      require(host::cursorStateRecord(state(1, 3, 1, 0, 0)) && !host::cursorStateRecord(shape(1, 1)) && !host::cursorStateRecord(media(3, 1, 64)) && !host::cursorStateRecord(text("{\"type\":\"cursor\"}")), "Cursor state classification");
      require(queue.admit(shape(1, 7), 32).queued, "Shape refused");
      for (int n = 0; n < 2400; ++n) { const auto result = queue.admit(state(1, 3, 7, n % 1900, n % 1000), 32); require(result.queued && !result.stalled, "An echo state stalled the channel"); }
      require(queue.packets() == 2, "Waiting cursor states were not coalesced");
      require(queue.admit(shape(1, 8), 32).queued && queue.admit(state(1, 1, 8, 0, 0), 32).queued && queue.packets() == 3, "A state after a new shape");
      require(queue.admit(state(2, 3, 9, 5, 6), 32).queued && queue.packets() == 4, "A state of another epoch was merged");
      host::OutgoingPacket out; std::vector<host::OutgoingPacket> sent; while (queue.pop(out)) sent.push_back(out);
      require(sent.size() == 4 && sent[0].bytes[12] == 1 && read32(sent[0], 16) == 7 && sent[1].bytes[12] == 1 && read32(sent[1], 16) == 8, "Shapes left their order");
      // The newest echoed position (2399 % 1900, 2399 % 1000) rides on the later state without one.
      require(sent[2].bytes[12] == 2 && sent[2].bytes[13] == 3 && read32(sent[2], 16) == 8 && read32(sent[2], 20) == 499 && read32(sent[2], 24) == 399, "The newest position or the newest state was lost");
      require(sent[3].bytes[2] == 2 && read32(sent[3], 20) == 5 && queue.packets() == 0 && queue.bytes() == 0, "Epoch state or accounting");
      ++checks; }
    { // #98: audio leaves before the video and tiles queued ahead of it, never
      // before a control message; everything else keeps its order.
      host::HostSendQueue queue;
      queue.admit(media(1, 1, 50000, true), 128); queue.admit(media(1, 2, 1000), 128); queue.admit(tiles(3, 500), 128);
      queue.admit(media(2, 4, 3840), 128); queue.admit(media(1, 5, 1000), 128); queue.admit(text("{\"type\":\"stream.stats\"}"), 128);
      queue.admit(media(2, 7, 3840), 128); queue.admit(media(1, 8, 1000), 128); queue.admit(media(2, 9, 3840), 128);
      host::OutgoingPacket out; std::vector<uint64_t> order; while (queue.pop(out)) order.push_back(out.text ? 0 : pts(out));
      require((order == std::vector<uint64_t>{4, 1, 2, 3, 5, 0, 7, 9, 8}) && queue.packets() == 0 && queue.bytes() == 0, "Audio waited behind queued video, or overtook a control message");
      ++checks; }
    { // audio-stats-v1: the audio part of what a full queue drops.
      host::HostSendQueue queue;
      for (uint64_t n = 1; n <= 8; ++n) queue.admit(media(2, n, 3840), 8);
      require(queue.admit(tiles(9, 1000), 8).droppedAudio == 0, "A dropped tile counted as audio");
      const auto refused = queue.admit(media(2, 10, 3840), 8);
      require(!refused.queued && refused.droppedOther == 1 && refused.droppedAudio == 1, "A refused audio packet was not counted");
      const auto key = queue.admit(media(1, 11, 1000, true), 8);
      require(key.queued && key.droppedAudio == 1 && key.droppedVideo == 0, "Audio an IDR replaced was not counted");
      ++checks; }
    { // audio-stats-v1 windows (audio_stats.hpp): fixed numbers, one window each.
      require(host::audioCaptureLatencyUs(30000000, 10000000, 25000000) == 500000 && host::audioCaptureLatencyUs(1, 0, 1) == -1 &&
              host::audioCaptureLatencyUs(10, 10, 20000000) == -1 && host::audioCaptureLatencyUs(10, 10, 0) == -1, "Capture latency from QPC");
      // 3 s at a 10 MHz counter is 30,000,000 x 100 ns; a 3,579,545 Hz counter at 3.5 s has a remainder.
      require(std::abs(host::audioCaptureLatencyUs(3 * 3579545 + 3579545 / 2, 3579545, 30000000) - 500000) <= 1, "QPC remainder");
      host::AudioCaptureWindow capture; require(capture.takeJson().empty(), "An empty capture window wrote a member");
      capture.add(9000); capture.add(15000); capture.add(12000);
      require(capture.takeJson() == ",\"audioCapture\":{\"packets\":3,\"avgUs\":12000,\"maxUs\":15000}" && capture.takeJson().empty(), "Capture window JSON or reset");
      host::AudioSendWindow send; require(send.takeJson().empty(), "An empty send window wrote a member");
      send.written(400); send.written(1600); send.sent(5); send.dropped(2);
      require(send.takeJson() == ",\"audioSend\":{\"sent\":7,\"dropped\":2,\"queuePackets\":2,\"queueAvgUs\":1000,\"queueMaxUs\":1600}" && send.takeJson().empty(), "Send window JSON or reset");
      send.sent(3); require(send.takeJson() == ",\"audioSend\":{\"sent\":3,\"dropped\":0}", "UDP-only audio has no WebSocket queue wait");
      ++checks; }
    { // A resize restart (new epoch, new IDR) drops queued media and ends the wait.
      host::HostSendQueue queue; queue.admit(text("{}"), 1); queue.admit(media(1, 1, 10), 1); queue.admit(media(1, 2, 10), 1);
      require(queue.awaitingKeyFrame(), "Fixture did not wait for an IDR");
      queue.dropMedia(); require(!queue.awaitingKeyFrame() && queue.packets() == 1, "dropMedia kept media or the wait");
      queue.clear(); require(queue.empty() && queue.bytes() == 0, "clear left packets");
      ++checks; }
    std::cout << "{\"sendQueueCases\":" << checks << ",\"network\":false}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
