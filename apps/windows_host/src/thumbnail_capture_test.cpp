// thumbnail-capture-v1 (thumbnail_capture.hpp). Substituted session reads,
// helper runs and privacy epochs; synthetic pixels through the real WIC JPEG
// encoder; the actual Agent request -> thumbnail thread -> ordered queue path.
// Nothing here reads this PC's screen or starts the capture helper.
#include "agent.hpp"
#include <cstring>
#include <iostream>
namespace {
int checks = 0;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); ++checks; }
const std::string testId = "0123456789abcdef0123456789abcdef";
const std::string testEpoch = "1234567890abcdef0000000000000002";
std::vector<uint8_t> gradient(uint32_t width, uint32_t height) {
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) { auto* p = pixels.data() + (size_t(y) * width + x) * 4; p[0] = uint8_t(x * 255 / width); p[1] = uint8_t(y * 255 / height); p[2] = uint8_t((x + y) & 255); p[3] = 255; }
  return pixels;
}
bool decodes(const std::vector<uint8_t>& jpeg, uint32_t width, uint32_t height) {
  IWICImagingFactory* factory = nullptr; IWICStream* stream = nullptr; IWICBitmapDecoder* decoder = nullptr; IWICBitmapFrameDecode* frame = nullptr; UINT w = 0, h = 0;
  bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->CreateStream(&stream)) &&
    SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), DWORD(jpeg.size()))) && SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
    SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h));
  if (frame) frame->Release(); if (decoder) decoder->Release(); if (stream) stream->Release(); if (factory) factory->Release();
  return ok && w == width && h == height;
}
host::Json parse(const std::string& text) { return host::JsonParser::parse(text); }

void pure() {
  using host::thumbnailScaledSize;
  require(thumbnailScaledSize(1920, 1080) == std::pair<uint32_t, uint32_t>{480, 270}, "16:9 scale");
  require(thumbnailScaledSize(2560, 1600) == std::pair<uint32_t, uint32_t>{432, 270}, "16:10 scale");
  require(thumbnailScaledSize(1080, 1920) == std::pair<uint32_t, uint32_t>{152, 270}, "portrait scale");
  require(thumbnailScaledSize(320, 200) == std::pair<uint32_t, uint32_t>{320, 200}, "small screens are never enlarged");
  require(thumbnailScaledSize(0, 1080).first == 0, "no size, no picture");
  std::vector<uint8_t> black(64 * 4, 0); require(host::thumbnailBlank(black.data(), 64), "black reads as blank");
  black[33 * 4 + 2] = 13; require(!host::thumbnailBlank(black.data(), 64), "one lit pixel is content");
  require(host::validThumbnailRequestId(testId) && !host::validThumbnailRequestId(testId.substr(1)) && !host::validThumbnailRequestId("0123456789ABCDEF0123456789abcdef"), "request id shape");
  const std::vector<uint8_t> jpeg{0xff, 0xd8, 1, 2, 0xff, 0xd9};
  const auto packet = host::thumbnailPacket(testId, jpeg);
  require(packet.size() == 4 + 32 + jpeg.size() && std::memcmp(packet.data(), "TVP1", 4) == 0 && std::string(packet.begin() + 4, packet.begin() + 36) == testId && std::equal(jpeg.begin(), jpeg.end(), packet.begin() + 36), "binary record layout");
  require(host::outgoingClass({false, packet}) == host::OutgoingClass::control, "the picture is never dropped as media");
  host::ThumbnailShot shot{host::ThumbnailState::captured, testEpoch, jpeg, 480, 270};
  const auto captured = parse(host::thumbnailResultJson(testId, shot));
  require(captured.get("type").string() == "host.thumbnail.result" && captured.get("requestId").string() == testId && captured.get("state").string() == "captured" && captured.get("epoch").string() == testEpoch && captured.get("bytes").number() == 6 && captured.get("width").number() == 480, "captured result");
  const auto locked = parse(host::thumbnailResultJson(testId, host::ThumbnailShot{host::ThumbnailState::locked, testEpoch}));
  require(locked.get("state").string() == "locked" && locked.get("epoch").string().empty() && locked.get("bytes").number(-1) == -1, "a locked answer carries no epoch or picture");
  require(host::thumbnailCaptureCapabilityFields() == ",\"thumbnailCaptureProtocol\":\"thumbnail-capture-v1\"", "capability field");
  // Base64 and the helper answer are bounded and strict.
  require(host::thumbnailUnbase64(host::thumbnailBase64(jpeg)).value() == jpeg, "base64 round trip");
  require(!host::thumbnailUnbase64("AAAA\"BBB") && !host::thumbnailUnbase64(std::string(60000, 'A')) && !host::thumbnailUnbase64(""), "base64 rejects text and oversize");
  for (const auto* bad : {R"({"state":"captured","width":481,"height":270,"jpeg":"/9j/2Q=="})", R"({"state":"captured","width":2,"height":2,"jpeg":"AAAA"})", R"({"state":"captured","width":2,"height":2})", R"({"state":"oops"})"})
    require(host::parseThumbnailHelperAnswer(parse(bad)).state == host::ThumbnailState::failed, "malformed helper answer is a failure, never a picture");
}

void encoder() {
  const auto pixels = gradient(480, 270);
  const auto answer = parse(host::thumbnailHelperAnswer(pixels, 480, 270));
  const auto shot = host::parseThumbnailHelperAnswer(answer);
  require(shot.state == host::ThumbnailState::captured && shot.width == 480 && shot.height == 270 && shot.jpeg.size() <= host::thumbnailMaxBytes && decodes(shot.jpeg, 480, 270), "synthetic desktop becomes a bounded JPEG of the scaled size");
  std::cout << "gradient 480x270 jpeg bytes " << shot.jpeg.size() << "\n";
  require(parse(host::thumbnailHelperAnswer(std::vector<uint8_t>(480 * 270 * 4, 0), 480, 270)).get("state").string() == "blank", "a dark output is blank, not a picture");
  // Noise defeats JPEG: the ladder either fits the bound or fails, never more.
  std::vector<uint8_t> noise(480 * 270 * 4); uint32_t seed = 7; for (auto& b : noise) { seed = seed * 1664525 + 1013904223; b = uint8_t(seed >> 24); }
  const auto noisy = host::parseThumbnailHelperAnswer(parse(host::thumbnailHelperAnswer(noise, 480, 270)));
  require(noisy.state == host::ThumbnailState::failed || (noisy.state == host::ThumbnailState::captured && noisy.jpeg.size() <= host::thumbnailMaxBytes), "the byte bound holds for any content");
  std::cout << "noise 480x270 state " << host::thumbnailStateName(noisy.state) << " bytes " << noisy.jpeg.size() << "\n";
  require(parse(host::thumbnailHelperAnswer(pixels, 481, 270)).get("state").string() == "failed", "oversized pixels are refused");
  // Cost of one encode (the helper's work after its one StretchBlt).
  const auto began = std::chrono::steady_clock::now();
  for (int i = 0; i < 20; ++i) (void)host::thumbnailHelperAnswer(pixels, 480, 270);
  std::cout << "encode+base64 per picture ms " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() / 20 << "\n";
}

struct Scripted {
  std::vector<host::ThumbnailSession> sessions; size_t reads = 0; int captures = 0; bool throwCapture = false;
  std::vector<bool> privacyAsked; host::ThumbnailPrivacyEpoch::Snapshot privacy{testEpoch, true};
  std::vector<host::ThumbnailState> recorded; std::chrono::steady_clock::time_point now{std::chrono::hours(1)};
  host::ThumbnailShooter shooter{
      [this] { return sessions[std::min(reads++, sessions.size() - 1)]; },
      [this](DWORD session) { if (session != 3) throw std::runtime_error("wrong session"); ++captures; if (throwCapture) throw std::runtime_error("helper failed"); return parse(host::thumbnailHelperAnswer(gradient(160, 90), 160, 90)); },
      [this](const host::ThumbnailShot& shot) { recorded.push_back(shot.state); },
      [this] { return now; }};
  host::ThumbnailShot take() { return shooter.take([this](bool allowed) { privacyAsked.push_back(allowed); return allowed ? privacy : host::ThumbnailPrivacyEpoch::Snapshot{testEpoch, false}; }); }
};
void shooter() {
  const host::ThumbnailSession open{3, true, true};
  { Scripted s; s.sessions = {{}}; require(s.take().state == host::ThumbnailState::signin && !s.captures, "no signed-in session: sign-in screen, nothing started"); }
  { Scripted s; s.sessions = {{3, false, true}}; require(s.take().state == host::ThumbnailState::locked && !s.captures, "locked session: never captured"); }
  { Scripted s; s.sessions = {{3, true, false}}; require(s.take().state == host::ThumbnailState::failed && !s.captures, "not the console session: never captured"); }
  { Scripted s; s.sessions = {open};
    const auto shot = s.take();
    require(shot.state == host::ThumbnailState::captured && shot.epoch == testEpoch && shot.width == 160 && host::thumbnailJpeg(shot.jpeg) && s.captures == 1 && s.privacyAsked == std::vector<bool>{true}, "unlocked console: one capture bound to the privacy testEpoch");
    require(s.take().state == host::ThumbnailState::busy && s.captures == 1, "a second capture within 10 s is refused");
    s.now += std::chrono::seconds(9); require(s.take().state == host::ThumbnailState::busy && s.captures == 1, "still within 10 s");
    s.now += std::chrono::seconds(1); require(s.take().state == host::ThumbnailState::captured && s.captures == 2, "after 10 s it captures again");
    require(s.recorded == std::vector<host::ThumbnailState>{host::ThumbnailState::captured, host::ThumbnailState::busy, host::ThumbnailState::captured}, "diagnostics only on a state change"); }
  { Scripted s; s.sessions = {open, {3, false, true}};
    const auto shot = s.take();
    require(shot.state == host::ThumbnailState::locked && shot.jpeg.empty() && s.privacyAsked == std::vector<bool>{false}, "locked during the capture: the picture is dropped"); }
  { Scripted s; s.sessions = {open, {}};
    require(s.take().state == host::ThumbnailState::signin && s.captures == 1, "signed out during the capture: the picture is dropped"); }
  { Scripted s; s.sessions = {open, {4, true, true}};
    require(s.take().state == host::ThumbnailState::locked, "another session took the console: dropped"); }
  { Scripted s; s.sessions = {open}; s.privacy = {testEpoch, false};
    const auto shot = s.take(); require(shot.state == host::ThumbnailState::failed && shot.jpeg.empty(), "a WTS change during the capture (epoch moved) drops the picture"); }
  { Scripted s; s.sessions = {open}; s.privacy = {"", true};
    require(s.take().state == host::ThumbnailState::failed, "no valid epoch, no picture"); }
  { Scripted s; s.sessions = {open}; s.throwCapture = true;
    require(s.take().state == host::ThumbnailState::failed && s.captures == 1, "a failed helper is a failure"); }
  // The real privacy owner: a rotation between begin and the snapshot denies.
  { host::ThumbnailPrivacyEpoch privacy([] { return std::string("1234567890abcdef1234567890abcdef"); });
    Scripted s; s.sessions = {open};
    const auto ticket = privacy.begin(); privacy.rotate();
    const auto shot = s.shooter.take([&](bool allowed) { return privacy.snapshot(ticket, allowed); });
    require(shot.state == host::ThumbnailState::failed && shot.jpeg.empty(), "real epoch owner: a session event during the capture drops the picture"); }
}

struct Bridge final : host::AgentBridge {
  void start(const host::Json&, Send) override {}
  void control(const host::Json&) override {}
  void stop() override {}
  std::string capabilities() override { return "{\"service\":true}"; }
  void displayControl(const host::Json&, Send) override {}
  std::atomic<int> calls{0}; HANDLE release = CreateEventW(nullptr, TRUE, TRUE, nullptr); host::ThumbnailShot next{host::ThumbnailState::captured, testEpoch, {0xff, 0xd8, 9, 0xff, 0xd9}, 4, 3};
  host::ThumbnailShot thumbnail() override { ++calls; WaitForSingleObject(release, 5000); return next; }
  ~Bridge() override { CloseHandle(release); }
};
struct Sent { std::vector<host::Json> text; std::vector<std::vector<uint8_t>> binary; std::vector<char> order; };
bool drain(host::Agent& agent, Sent& out) { return agent.publicationTestDrain([&](bool text, const std::vector<uint8_t>& bytes) { if (text) { out.text.push_back(parse(std::string(bytes.begin(), bytes.end()))); out.order.push_back('t'); } else { out.binary.push_back(bytes); out.order.push_back('b'); } return true; }); }
bool await(host::Agent& agent, Sent& out, size_t results) {
  for (int i = 0; i < 300; ++i) { drain(agent, out); size_t n = 0; for (const auto& value : out.text) n += value.get("type").string() == "host.thumbnail.result"; if (n >= results) return true; Sleep(10); }
  return false;
}
void agentPath() {
  const auto request = "{\"type\":\"host.thumbnail.request\",\"requestId\":\"" + testId + "\"}";
  { Bridge bridge; host::Agent agent([](const std::wstring&) {}, &bridge); agent.publicationTestOpen(); Sent sent;
    agent.localControl(request);
    require(await(agent, sent, 1) && bridge.calls == 1, "the request reached the bridge off the receive thread");
    require(sent.order == std::vector<char>{'b', 't'} && sent.binary[0] == host::thumbnailPacket(testId, bridge.next.jpeg) && sent.text[0].get("state").string() == "captured" && sent.text[0].get("epoch").string() == testEpoch && sent.text[0].get("bytes").number() == 5, "the picture leaves right before its result on the ordered queue"); }
  { Bridge bridge; host::Agent agent([](const std::wstring&) {}, &bridge); agent.publicationTestOpen(); Sent sent;
    agent.localControl("{\"type\":\"host.thumbnail.request\",\"requestId\":\"" + testId.substr(2) + "\"}");
    agent.localControl("{\"type\":\"host.thumbnail.request\"}");
    Sleep(100); drain(agent, sent); require(sent.order.empty() && bridge.calls == 0, "a malformed request is ignored"); }
  { Bridge bridge; ResetEvent(bridge.release); bridge.next = {host::ThumbnailState::locked}; host::Agent agent([](const std::wstring&) {}, &bridge); agent.publicationTestOpen(); Sent sent;
    agent.localControl(request);
    for (int i = 0; i < 200 && bridge.calls == 0; ++i) Sleep(5);
    const auto other = "{\"type\":\"host.thumbnail.request\",\"requestId\":\"" + std::string(32, 'a') + "\"}";
    agent.localControl(other);
    require(await(agent, sent, 1) && sent.text[0].get("requestId").string() == std::string(32, 'a') && sent.text[0].get("state").string() == "busy" && bridge.calls == 1, "one capture at a time: a second request answers busy at once");
    SetEvent(bridge.release);
    require(await(agent, sent, 2) && sent.binary.empty() && sent.text[1].get("requestId").string() == testId && sent.text[1].get("state").string() == "locked", "a locked answer carries no picture"); }
  { // The Agent joins its thumbnail thread before it (and its bridge) go away.
    Bridge bridge; ResetEvent(bridge.release);
    { host::Agent agent([](const std::wstring&) {}, &bridge); agent.publicationTestOpen(); agent.localControl(request);
      for (int i = 0; i < 200 && bridge.calls == 0; ++i) Sleep(5);
      std::thread late([&] { Sleep(150); SetEvent(bridge.release); }); late.detach(); }
    require(bridge.calls == 1, "the Agent waited for its capture before going away"); }
  { // No bridge (a worker's own Agent): never answers.
    host::Agent agent([](const std::wstring&) {}); agent.publicationTestOpen(); Sent sent; agent.localControl(request); Sleep(50); drain(agent, sent); require(sent.order.empty(), "a worker Agent never captures"); }
}
}
int main() {
  try {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    pure(); encoder(); shooter(); agentPath();
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << "Thumbnail capture checks passed: " << checks << "\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAILED after " << checks << " checks: " << error.what() << "\n"; return 1; }
}
