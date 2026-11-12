// 게임 마우스: what the Host itself adds between a client's mouse input arriving and the VM receiving it, on
// both paths, measured here on fakes (docs/usb-redirection.md). Nothing is installed or injected:
//  - usb: a report frame put into the controller's inbox (the same wait/notify hop UsbRedirection::run uses)
//    -> UsbSessionCore -> the real loopback USB/IP server -> a fake kernel reading RET_SUBMIT from its socket.
//  - sendinput: a moveRelative message given to the worker Agent -> its injector (substituted, no OS input).
//    The service-to-worker pipe before it is not part of this measurement.
// Neither includes the network, the VM's HID stack or a game; it is the Host's own share only.
#include <winsock2.h>
#include "agent.hpp"
#include "usb_redirection.hpp"
#include <algorithm>
#include <iostream>
using namespace host;
namespace {
int64_t nowUs() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return int64_t(c.QuadPart * 1000000 / f.QuadPart); }
const std::string session = "00000000-0000-4000-8000-000000000251";
struct Stats { double p50 = 0, p95 = 0, p99 = 0, max = 0; size_t count = 0; };
Stats stats(std::vector<int64_t> values) {
  Stats out; out.count = values.size();
  if (values.empty()) return out;
  std::sort(values.begin(), values.end());
  const auto at = [&](double q) { return double(values[std::min(values.size() - 1, size_t(q * double(values.size() - 1) + 0.5))]); };
  out.p50 = at(0.5); out.p95 = at(0.95); out.p99 = at(0.99); out.max = double(values.back());
  return out;
}
std::string json(const char* name, const Stats& s) {
  return std::string("\"") + name + "\":{\"samples\":" + std::to_string(s.count) + ",\"p50Us\":" + std::to_string(int64_t(s.p50)) + ",\"p95Us\":" + std::to_string(int64_t(s.p95)) +
         ",\"p99Us\":" + std::to_string(int64_t(s.p99)) + ",\"maxUs\":" + std::to_string(int64_t(s.max)) + '}';
}
bool readAll(SOCKET socket, usb::Bytes& out, size_t size) {
  out.resize(size); size_t at = 0;
  while (at < size) { const int count = recv(socket, reinterpret_cast<char*>(out.data() + at), int(size - at), 0); if (count <= 0) return false; at += size_t(count); }
  return true;
}
bool sendAll(SOCKET socket, const usb::Bytes& bytes) { return send(socket, reinterpret_cast<const char*>(bytes.data()), int(bytes.size()), 0) == int(bytes.size()); }
usb::Bytes readRequest(uint32_t seq, uint32_t devid) {
  usb::Bytes cmd; usb::put32(cmd, usb::cmdSubmit); usb::put32(cmd, seq); usb::put32(cmd, devid); usb::put32(cmd, 1); usb::put32(cmd, 1);
  usb::put32(cmd, 0); usb::put32(cmd, 7); usb::put32(cmd, 0); usb::put32(cmd, 0xffffffffu); usb::put32(cmd, 0); cmd.resize(48, 0);
  return cmd;
}
std::string base64(const usb::Bytes& bytes) {
  DWORD length = 0; CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length);
  std::string out(length, '\0'); CryptBinaryToStringA(bytes.data(), DWORD(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &length); out.resize(length); return out;
}

Stats usbPath(int samples) {
  std::mutex lock; std::condition_variable changed;
  std::deque<uint32_t> ahead;                         // the Host's reads waiting at this fake client
  std::vector<int64_t> sentAt(size_t(samples) + 1, 0), gotAt(size_t(samples) + 1, 0);
  std::atomic<uint16_t> port{0}; std::thread kernel; std::atomic<bool> stop{false}; SOCKET kernelSocket = INVALID_SOCKET;
  UsbHooks hooks;
  hooks.sendText = [](const std::string&) { return true; };
  hooks.sendFrame = [&](const usb::Bytes& bytes) {
    const auto frame = usb::decodeFrame(bytes.data(), bytes.size());
    if (frame && frame->type == usb::frameSubmit) { std::lock_guard g(lock); ahead.push_back(frame->seq); changed.notify_all(); }
    return true;
  };
  hooks.prepare = [] { return UsbPrepared{true, ""}; };
  hooks.attach = [&](const std::string& busId) -> int {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { closesocket(s); return 0; }
    BOOL noDelay = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    auto request = usb::encodeOpCommon(usb::opReqImport, 0); usb::Bytes field(32, 0); std::memcpy(field.data(), busId.data(), busId.size()); request.insert(request.end(), field.begin(), field.end());
    usb::Bytes reply;
    if (!sendAll(s, request) || !readAll(s, reply, 8 + 312)) { closesocket(s); return 0; }
    const uint32_t devid = usb::get32(&reply[8 + 256 + 32]) << 16 | usb::get32(&reply[8 + 256 + 36]);
    kernelSocket = s;
    // The VM's HID class driver: two reads waiting, each answered read posted again at once.
    kernel = std::thread([&, s, devid] {
      uint32_t seq = 0;
      sendAll(s, readRequest(++seq, devid)); sendAll(s, readRequest(++seq, devid));
      usb::Bytes header, payload;
      while (!stop && readAll(s, header, 48)) {
        const auto length = usb::get32(&header[24]);
        if (usb::get32(&header[0]) != usb::retSubmit || !readAll(s, payload, length)) break;
        const auto at = nowUs();
        if (length == 7) { const int index = int(int16_t(uint16_t(payload[1] | payload[2] << 8))); if (index > 0 && index <= samples) gotAt[size_t(index)] = at; }
        if (!sendAll(s, readRequest(++seq, devid))) break;
      }
    });
    return 5;
  };
  hooks.detach = [](int) {};
  UsbSessionCore core(session, {}, std::move(hooks));
  usb::UsbIpServer server; usb::UsbIpServer::Access access; access.trusted = [](SOCKET) { return true; };
  port = server.start(core, access);
  if (!port) throw std::runtime_error("The loopback server did not start");
  // The controller's own hop: frames arrive in an inbox and its thread is woken (UsbRedirection::run).
  std::deque<usb::Bytes> inbox; std::mutex inboxLock; std::condition_variable wake; std::atomic<bool> quit{false};
  std::thread controller([&] {
    for (;;) {
      std::deque<usb::Bytes> messages;
      { std::unique_lock g(inboxLock); wake.wait_for(g, std::chrono::milliseconds(500), [&] { return quit || !inbox.empty(); }); messages.swap(inbox); }
      for (auto& message : messages) core.frame(message.data(), message.size());
      core.tick();
      if (quit) break;
    }
  });
  core.message(JsonParser::parse("{\"type\":\"usb.ready\",\"sessionId\":\"" + session + "\",\"protocol\":\"usb-v1\",\"limits\":{\"maxUrbBytes\":262144,\"windowBytes\":1048576,\"maxInFlight\":32,\"maxDevices\":4,\"maxAttaches\":64}}"));
  Sleep(200);
  core.message(JsonParser::parse("{\"type\":\"usb.attach\",\"device\":1,\"descriptors\":\"" + base64(usb::vmouse::descriptors()) + "\",\"speed\":\"full\"}"));
  for (int i = 1; i <= samples; ++i) {
    uint32_t seq = 0;
    { std::unique_lock g(lock); if (!changed.wait_for(g, std::chrono::seconds(5), [&] { return !ahead.empty(); })) throw std::runtime_error("No read-ahead request arrived"); seq = ahead.front(); ahead.pop_front(); }
    usb::Frame answer; answer.type = usb::frameReturn; answer.device = 1; answer.endpoint = 0x81; answer.transfer = 3; answer.seq = seq; answer.length = 7;
    answer.payload = {0, uint8_t(i), uint8_t(i >> 8), 0, 0, 0, 0};
    const auto bytes = usb::encodeFrame(answer);
    sentAt[size_t(i)] = nowUs();
    { std::lock_guard g(inboxLock); inbox.push_back(bytes); }
    wake.notify_all();
    // One report at a time, a millisecond apart: the latency of each, not a queue.
    const auto until = nowUs() + 1000;
    while (gotAt[size_t(i)] == 0 && nowUs() < sentAt[size_t(i)] + 2000000) Sleep(0);
    while (nowUs() < until) Sleep(0);
  }
  quit = true; wake.notify_all(); controller.join();
  stop = true; core.closed(); if (kernelSocket != INVALID_SOCKET) shutdown(kernelSocket, SD_BOTH); server.stop();
  if (kernel.joinable()) kernel.join();
  if (kernelSocket != INVALID_SOCKET) closesocket(kernelSocket);
  std::vector<int64_t> latency;
  for (int i = 1; i <= samples; ++i) if (gotAt[size_t(i)]) latency.push_back(gotAt[size_t(i)] - sentAt[size_t(i)]);
  if (int(latency.size()) != samples) throw std::runtime_error("A report never reached the kernel");
  return stats(latency);
}

Stats sendInputPath(int samples) {
  const RECT screen{0, 0, 1920, 1080};
  const std::string key = "00000000-0000-4000-8000-000000000252";
  std::vector<int64_t> latency; latency.reserve(size_t(samples));
  int64_t sentAt = 0; std::atomic<int64_t> injectedAt{0};
  {
    Agent agent([](const std::wstring&) {}, [](bool, std::vector<uint8_t>) {}, [&](INPUT&) -> UINT { injectedAt = nowUs(); return 1U; }, [screen] { return screen; });
    agent.inputLatencyTestStream(JsonParser::parse("{\"type\":\"session.start\",\"mode\":\"stream\",\"sessionId\":\"" + key + "\",\"resizeProtocol\":\"epoch-v1\",\"epoch\":0,\"monitor\":\"0\",\"relativeMouseProtocol\":\"relative-mouse-v1\"}"), screen);
    for (int i = 1; i <= samples; ++i) {
      injectedAt = 0;
      const std::string message = "{\"type\":\"input\",\"event\":\"moveRelative\",\"dx\":" + std::to_string(1 + i % 7) + ",\"dy\":1}";
      sentAt = nowUs();
      agent.localControl(message);
      while (injectedAt == 0 && nowUs() < sentAt + 2000000) Sleep(0);
      if (injectedAt) latency.push_back(injectedAt - sentAt);
      const auto until = nowUs() + 1000; while (nowUs() < until) Sleep(0);
    }
    agent.localControl("{\"type\":\"session.stop\",\"sessionId\":\"" + key + "\"}");
  }
  if (int(latency.size()) != samples) throw std::runtime_error("A movement was never injected");
  return stats(latency);
}
}  // namespace

int main(int argc, char** argv) {
  try {
    const int samples = argc > 1 ? std::max(10, std::atoi(argv[1])) : 2000;
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2, 2), &wsa)) throw std::runtime_error("Winsock");
    const auto usb = usbPath(samples);
    const auto injected = sendInputPath(samples);
    WSACleanup();
    std::cout << "{\"kind\":\"game-mouse-host-latency\"," << json("usb", usb) << ',' << json("sendinput", injected)
              << ",\"usbScope\":\"inbox->core->loopback->fake kernel\",\"sendinputScope\":\"worker Agent->injector (pipe hop excluded)\",\"osInputEmitted\":false}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "game mouse latency failed: " << error.what() << '\n';
    return 1;
  }
}
