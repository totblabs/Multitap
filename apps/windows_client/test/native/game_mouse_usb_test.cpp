// Game mouse over USB (game_mouse_usb.h) without a device, a VM or real input:
// the relay frames, Raw Input as reports (ToTB Link's VirtualMouseState
// cases), the device's answers to a Host that reads ahead and to an older
// Host, unlinks, pacing and joining. Then a measurement on a modelled network
// (a fixed round trip, the Host's read-ahead replacing each answered read):
// how long this PC holds each mouse event before it leaves, on the USB path,
// on a Host without read-ahead, and on today's SendInput path (moveRelative
// every 4 ms, flushed by the ~15.6 ms window timer). The network itself, the
// Host and the VM are not in these numbers.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "game_mouse_usb.h"
#include "relative_mouse.h"

namespace {
int checks = 0;
void require(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
  ++checks;
}
using namespace multitap::game_mouse;

Bytes Submit(uint8_t device, uint32_t seq, uint8_t endpoint = kEndpoint, uint8_t transfer = kInterrupt, uint32_t length = 7) {
  Frame f;
  f.type = kSubmit;
  f.device = device;
  f.endpoint = endpoint;
  f.transfer = transfer;
  f.seq = seq;
  f.length = length;
  return Encode(f);
}
Bytes Control(uint8_t device, uint32_t seq) {
  Frame f;
  f.type = kSubmit;
  f.device = device;
  f.endpoint = 0x80;
  f.transfer = 0;
  f.seq = seq;
  f.length = 255;
  const uint8_t setup[8] = {0x80, 6, 2, 3, 9, 4, 255, 0};
  std::memcpy(f.setup, setup, 8);
  return Encode(f);
}
Bytes Unlink(uint8_t device, uint32_t seq, uint32_t target) {
  Frame f;
  f.type = kUnlink;
  f.device = device;
  f.seq = seq;
  f.length = target;
  return Encode(f);
}
struct Sent {
  std::vector<Frame> frames;
  Device::Send send() {
    return [this](const Bytes& bytes) { frames.push_back(*Decode(bytes.data(), bytes.size())); };
  }
};
Report R(int buttons, int dx, int dy, int wheel = 0, int pan = 0) { return MakeReport(buttons, dx, dy, wheel, pan); }
int16_t X(const Report& r) { return int16_t(uint16_t(r[1] | r[2] << 8)); }
int16_t Y(const Report& r) { return int16_t(uint16_t(r[3] | r[4] << 8)); }
Report Of(const Frame& f) {
  Report r{};
  std::copy(f.payload.begin(), f.payload.end(), r.begin());
  return r;
}

void frames() {
  Frame f;
  f.type = kReturn;
  f.device = 3;
  f.endpoint = 0x81;
  f.transfer = 3;
  f.seq = 0x01020304;
  f.length = 7;
  f.status = -104;
  f.payload = {1, 2, 3, 4, 5, 6, 7};
  const auto bytes = Encode(f);
  require(bytes.size() == 31 && bytes[4] == 0x04 && bytes[7] == 0x01 && bytes[12] == 0x98 && bytes[15] == 0xff, "A relay frame is not little-endian");
  const auto back = Decode(bytes.data(), bytes.size());
  require(back && back->seq == f.seq && back->status == -104 && back->payload == f.payload && back->endpoint == 0x81, "A relay frame did not round-trip");
  require(!Decode(bytes.data(), 23), "A short frame decoded");
}

void mouseState() {
  // Down and up per button, every mouse together, Windows' own input (source 0) too.
  MouseState state;
  auto out = state.Mouse(1, 3, -2, 0x0001, 0, 0);
  require(out.size() == 1 && out[0] == R(1, 3, -2), "A left press with motion is wrong");
  require(state.Mouse(2, 0, 0, 0x0004, 0, 0) == std::vector<Report>{R(3, 0, 0)}, "A second mouse's right press did not combine");
  require(state.Mouse(1, 0, 0, 0x0002, 0, 0) == std::vector<Report>{R(2, 0, 0)}, "A release of one mouse let go of the other's button");
  require(state.Mouse(0, 0, 0, 0x0010 | 0x0040 | 0x0100, 0, 0) == std::vector<Report>{R(2 | 4 | 8 | 16, 0, 0)}, "Middle and side buttons are wrong");
  require(state.Mouse(0, 0, 0, 0, 0, 0).empty(), "Nothing changed but a report was made");
  // A release this state never saw pressed (held before capture began) changes nothing.
  MouseState fresh;
  require(fresh.Mouse(5, 0, 0, 0x0002, 0, 0).empty() && !fresh.AnyHeld(), "A release of a button pressed before capture was sent");
  // The wheel: 120 is one notch, finer wheels keep their remainder; the horizontal wheel the same.
  MouseState wheel;
  require(wheel.Mouse(0, 0, 0, 0x0400, 60, 0).empty(), "Half a notch was sent");
  require(wheel.Mouse(0, 0, 0, 0x0400, 60, 0) == std::vector<Report>{R(0, 0, 0, 1)}, "Two halves did not make a notch");
  require(wheel.Mouse(0, 0, 0, 0x0400, -240, 0) == std::vector<Report>{R(0, 0, 0, -2)}, "Wheel down is wrong");
  require(wheel.Mouse(0, 0, 0, 0x0800, 120, 0) == std::vector<Report>{R(0, 0, 0, 0, 1)}, "The horizontal wheel is wrong");
  // Motion beyond a report is split, nothing lost.
  MouseState big;
  out = big.Mouse(0, 40000, -70000, 0, 0, 0);
  int64_t sx = 0, sy = 0;
  for (const auto& r : out) { sx += X(r); sy += Y(r); }
  require(out.size() == 3 && sx == 40000 && sy == -70000, "Large motion was not split exactly");
  // This PC's swapped buttons (left-handed): the physical left is the secondary button in the VM too.
  MouseState swapped(true);
  require(swapped.Mouse(0, 0, 0, 0x0001, 0, 0) == std::vector<Report>{R(2, 0, 0)}, "Swapped buttons were not applied");
  // A button let go unseen goes up once Windows says so and it was held long enough.
  MouseState held;
  held.Mouse(0, 0, 0, 0x0001, 0, 1000);
  require(!held.Check([](int) { return false; }, 1200), "A fresh press was released by the check");
  require(!held.Check([](int) { return true; }, 1400), "A button still down was released");
  const auto up = held.Check([](int) { return false; }, 1400);
  require(up && *up == R(0, 0, 0) && !held.AnyHeld(), "An unseen release was not sent");
  held.Mouse(0, 0, 0, 0x0004, 0, 0);
  require(held.ReleaseAll() == R(0, 0, 0) && !held.ReleaseAll(), "Release all did not let go once");
}

void readAheadHost() {
  Sent sent;
  Device device(7, sent.send());
  require(!device.TakesInput(0) && !device.confirmed(), "A device nobody read takes input");
  // The Host's read-ahead: reads arrive before anything happens.
  for (uint32_t seq = 1; seq <= 8; ++seq) require(device.Frame(Submit(7, seq).data(), 31 - 7, 1000) == Device::Seen::kOk, "A read was refused");
  require(device.confirmed() && device.TakesInput(1000) && device.held() == 8 && sent.frames.empty(), "Reads were answered with nothing");
  // A report answers the oldest read at once.
  device.Report(R(1, 5, -3), 2000);
  require(sent.frames.size() == 1 && sent.frames[0].type == kReturn && sent.frames[0].seq == 1 && sent.frames[0].status == 0 && sent.frames[0].endpoint == 0x81 &&
              sent.frames[0].transfer == 3 && sent.frames[0].length == 7 && Of(sent.frames[0]) == R(1, 5, -3),
          "The report did not answer the oldest read");
  // Within the pace it waits (4 ms until a round trip is known), and motion with the same buttons joins; a
  // button change stays apart.
  device.Report(R(1, 1, 1), 2300);
  device.Report(R(1, 2, 2), 2600);
  device.Report(R(0, 0, 0), 2700);
  require(sent.frames.size() == 1 && device.queued() == 2, "Reports within the pace went out or did not join");
  require(device.Wake() == 6000, "The wake time is not the first pace");
  device.Tick(5999);
  require(sent.frames.size() == 1, "A report went out before its time");
  device.Tick(6000);
  require(sent.frames.size() == 2 && Of(sent.frames[1]) == R(1, 3, 3) && sent.frames[1].seq == 2, "The joined motion did not go next");
  device.Tick(10000);
  require(sent.frames.size() == 3 && Of(sent.frames[2]) == R(0, 0, 0), "The release did not follow in order");
  require(!device.Wake(), "A wake was asked with nothing waiting");
  // The Host cancels a read it no longer wants (its VM stopped reading): both answers, in that order.
  sent.frames.clear();
  require(device.Frame(Unlink(7, 100, 5).data(), 24, 5000) == Device::Seen::kOk, "unlink");
  require(sent.frames.size() == 2 && sent.frames[0].type == kUnlinkReturn && sent.frames[0].seq == 100 && sent.frames[0].status == kReset &&
              sent.frames[1].type == kReturn && sent.frames[1].seq == 5 && sent.frames[1].status == kReset && sent.frames[1].payload.empty(),
          "A held read was not cancelled with both answers");
  require(device.Frame(Unlink(7, 101, 5).data(), 24, 5000) == Device::Seen::kOk && sent.frames.size() == 3 && sent.frames[2].status == 0, "A cancel of a read already gone was not answered 0");
  // Every read gone: input still counts as taken for a second, then not.
  for (uint32_t seq : {4u, 6u, 7u, 8u}) device.Frame(Unlink(7, 200 + seq, seq).data(), 24, 6000);
  require(device.held() == 0 && device.TakesInput(6000 + 900000) && !device.TakesInput(1000 + 1000000 + 1), "Taking input did not end a second after the last read");
  // Reports keep their order without reads; a stale one gives its buttons only.
  Device quiet(8, sent.send());
  sent.frames.clear();
  quiet.Frame(Submit(8, 1).data(), 24, 0);
  quiet.Report(R(0, 1, 0), 1000);  // answered
  quiet.Report(R(2, 9, 9), 2000);
  quiet.Frame(Submit(8, 2).data(), 24, 400000);
  require(sent.frames.size() == 2 && Of(sent.frames[1]) == R(2, 0, 0), "A stale report kept its motion");
  // Malformed or foreign frames.
  require(quiet.Frame(Submit(9, 3).data(), 24, 0) == Device::Seen::kInvalid, "Another device's frame was taken");
  require(quiet.Frame(Submit(8, 3).data(), 10, 0) == Device::Seen::kInvalid, "A short frame was taken");
  const auto tiny = Submit(8, 4, kEndpoint, kInterrupt, 4);
  require(quiet.Frame(tiny.data(), tiny.size(), 0) == Device::Seen::kOk && sent.frames.back().status == kStall && quiet.held() == 0, "A read too small for a report was held");
  const auto bulk = Submit(8, 5, 0x82, 2, 64);
  require(quiet.Frame(bulk.data(), bulk.size(), 0) == Device::Seen::kOk && sent.frames.back().status == kStall, "A request on another endpoint was held");
  require(!quiet.older(), "Stray requests made the Host look older");
}

void olderHost() {
  // A released Host forwards the VM's control requests: the first one says the Host does not know the device.
  Sent sent;
  Device device(3, sent.send());
  const auto control = Control(3, 41);
  require(device.Frame(control.data(), control.size(), 0) == Device::Seen::kOlderHost, "An older Host was not recognised");
  require(sent.frames.size() == 1 && sent.frames[0].type == kReturn && sent.frames[0].seq == 41 && sent.frames[0].status == kStall && sent.frames[0].transfer == 0, "The control request was not stalled");
  require(device.Frame(Submit(3, 42).data(), 24, 0) == Device::Seen::kOlderHost && sent.frames.back().status == kStall, "A read from an older Host was held");
  require(!device.TakesInput(0) && !device.confirmed(), "An older Host's device takes input");
  device.Report(R(1, 1, 1), 0);
  require(sent.frames.size() == 2, "A report went to an older Host");
  // A control request after the reads began (never from a read-ahead Host) is stalled, not a verdict.
  Device later(4, sent.send());
  later.Frame(Submit(4, 1).data(), 24, 0);
  const auto again = Control(4, 2);
  require(later.Frame(again.data(), again.size(), 0) == Device::Seen::kOk && later.TakesInput(0), "A late control request ended a confirmed device");
}

void pacing() {
  // The round trip is learned from the reads coming back after their answers; answers spread over it.
  Sent sent;
  Device device(1, sent.send());
  uint32_t seq = 0;
  for (int i = 0; i < 8; ++i) device.Frame(Submit(1, ++seq).data(), 24, 0);
  require(device.Interval() == Device::kFirstIntervalUs, "The pace before any round trip is not 4 ms");
  int64_t now = 10000;
  for (int round = 0; round < 40; ++round) {
    device.Report(R(0, 1, 0), now);
    // Each answer comes back as a new read 40 ms later.
    device.Frame(Submit(1, ++seq).data(), 24, now + 40000);
    now += 5000;
  }
  require(device.Interval() >= 4500 && device.Interval() <= 5500, "A 40 ms round trip over 8 reads did not pace at about 5 ms");
  // A short round trip paces at the one-millisecond floor, never faster (the relay's message budget).
  Device fast(2, sent.send());
  for (int i = 0; i < 8; ++i) fast.Frame(Submit(2, ++seq).data(), 24, 0);
  for (int64_t t = 10000; t < 30000; t += 125) {
    fast.Report(R(0, 1, 0), t);
    fast.Frame(Submit(2, ++seq).data(), 24, t + 2000);
  }
  require(fast.Interval() == Device::kMinIntervalUs, "A 2 ms round trip did not pace at one millisecond");
}

void sinkRouting() {
  // The relative mouse hands raw input to the sink for a routed engagement; the timer and the end reach it.
  WNDCLASSW type{};
  type.lpfnWndProc = DefWindowProcW;
  type.hInstance = GetModuleHandleW(nullptr);
  type.lpszClassName = L"MultitapGameMouseQA";
  require(RegisterClassW(&type) != 0, "QA class");
  HWND root = CreateWindowExW(0, type.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 400, 300, nullptr, nullptr, type.hInstance, nullptr);
  HWND view = CreateWindowExW(0, type.lpszClassName, L"", WS_CHILD, 0, 0, 400, 300, root, nullptr, type.hInstance, nullptr);
  static HWND fg;
  static RECT clip{-10000, -10000, 10000, 10000};
  static HWND under;
  fg = root;
  under = view;
  multitap::RelativeMouseOs os;
  os.clip = [](const RECT* r) -> BOOL { clip = r ? *r : RECT{-10000, -10000, 10000, 10000}; return TRUE; };
  os.clipped = [](LPRECT r) -> BOOL { *r = clip; return TRUE; };
  os.cursor = [](LPPOINT p) -> BOOL { *p = POINT{10, 10}; return TRUE; };
  os.foreground = []() -> HWND { return fg; };
  os.window = [](POINT) -> HWND { return under; };
  os.devices = [](PCRAWINPUTDEVICE, UINT, UINT) -> BOOL { return TRUE; };
  os.raw = [](HRAWINPUT input, UINT, LPVOID data, PUINT, UINT) -> UINT { *static_cast<RAWINPUT*>(data) = *reinterpret_cast<const RAWINPUT*>(input); return sizeof(RAWINPUT); };
  multitap::RelativeMouse mouse(os);
  std::vector<std::pair<int, int>> moves;
  std::vector<std::string> ended;
  mouse.Bind([&](int dx, int dy) { moves.emplace_back(dx, dy); }, [&](const char* reason) { ended.emplace_back(reason); });
  int inputs = 0, released = 0;
  bool taking = true;
  multitap::RelativeMouseSink sink;
  sink.input = [&](const RAWMOUSE& raw, HANDLE) { inputs += int(raw.lLastX); };
  sink.tick = [&] { return taking; };
  sink.released = [&] { ++released; };
  const auto event = [&](LONG x, USHORT flags = MOUSE_MOVE_RELATIVE) {
    RAWINPUT raw{};
    raw.header.dwType = RIM_TYPEMOUSE;
    raw.data.mouse.usFlags = flags;
    raw.data.mouse.lLastX = x;
    mouse.HandleMessage(root, WM_INPUT, RIM_INPUT, reinterpret_cast<LPARAM>(&raw));
  };
  mouse.Route(sink);
  require(mouse.Engage(root, view) && mouse.routed(), "A routed engagement failed");
  event(4);
  event(3);
  require(inputs == 7 && moves.empty(), "Routed input reached the moveRelative path");
  mouse.Route({});
  require(mouse.routed(), "A route changed during an engagement");
  require(mouse.HandleMessage(root, WM_TIMER, multitap::RelativeMouse::kTimer, 0) && mouse.engaged(), "A device taking input ended capture");
  taking = false;
  mouse.HandleMessage(root, WM_TIMER, multitap::RelativeMouse::kTimer, 0);
  require(!mouse.engaged() && ended == std::vector<std::string>{"lost"} && released == 1, "A device that stopped taking input kept capture or a button");
  // The next engagement without a route is the injected path again.
  require(mouse.Engage(root, view) && !mouse.routed(), "A route outlived its engagement");
  event(5);
  require(inputs == 7 && moves.size() == 1, "An unrouted engagement did not send movement");
  mouse.Release();
  require(released == 1, "An unrouted release reached the old sink");
  // Focus loss and absolute pointers end a routed engagement with buttons released.
  taking = true;
  mouse.Route(sink);
  require(mouse.Engage(root, view), "engage");
  mouse.HandleMessage(root, WM_ACTIVATE, WA_INACTIVE, 0);
  require(!mouse.engaged() && released == 2 && ended.back() == "focus", "Focus loss did not release the virtual mouse's buttons");
  mouse.Route(sink);
  require(mouse.Engage(root, view), "engage");
  event(1, MOUSE_MOVE_ABSOLUTE);
  require(!mouse.engaged() && released == 3 && ended.back() == "device" && inputs == 7, "An absolute pointer reached the virtual mouse");
  mouse.Route(sink);
  require(mouse.Engage(root, view), "engage");
  mouse.Release();
  require(released == 4 && !mouse.routed(), "A requested release did not let go");
  DestroyWindow(root);
  UnregisterClassW(type.lpszClassName, type.hInstance);
}

// --- measurement on a modelled network -------------------------------------
struct Measure { double p50 = 0, p95 = 0, max = 0, perSecond = 0, gap = 0, gapP99 = 0; };
double Pct(std::vector<int64_t> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return double(v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]);
}
// A 1000 Hz mouse with jitter, moving in 300 ms strokes 200 ms apart, for 6 s.
std::vector<int64_t> Events() {
  std::vector<int64_t> at;
  uint32_t noise = 12345;
  for (int64_t stroke = 0; stroke < 12; ++stroke)
    for (int64_t i = 0; i < 300; ++i) {
      noise = noise * 1103515245u + 12345u;
      at.push_back(stroke * 500000 + i * 1000 + int64_t(noise >> 16) % 400 - 200 + 1000);
    }
  return at;
}
// When each event's motion left this PC (from the cumulative motion sent).
Measure Score(const std::vector<int64_t>& events, const std::vector<std::pair<int64_t, int64_t>>& sends, int64_t seconds) {
  std::vector<int64_t> wait;
  size_t s = 0;
  int64_t total = 0;
  for (size_t i = 0; i < events.size(); ++i) {
    while (s < sends.size() && total < int64_t(i + 1)) total += sends[s++].second;
    if (total < int64_t(i + 1)) break;
    wait.push_back(sends[s - 1].first - events[i]);
  }
  std::vector<int64_t> gaps;
  for (size_t i = 1; i < sends.size(); ++i)
    if (sends[i].first - sends[i - 1].first < 150000) gaps.push_back(sends[i].first - sends[i - 1].first);  // within a stroke
  require(wait.size() == events.size(), "Motion was lost in the model");
  return {Pct(wait, 0.5) / 1000.0, Pct(wait, 0.95) / 1000.0, Pct(wait, 1.0) / 1000.0, double(sends.size()) / double(seconds), Pct(gaps, 1.0) / 1000.0,
          Pct(gaps, 0.99) / 1000.0};
}
// Video on its own socket shares this PC's downlink with the Host's reads coming back: [mbps] at 60 fps on a
// [linkMbps] link, each frame reaching the link at once (a burst), every 120th frame four times the size (a
// keyframe). A read waits for the video bytes ahead of it (FIFO). Reports go the other way and do not wait.
struct Video { double mbps, linkMbps; };
int64_t Backlog(const Video& video, int64_t t) {
  const double frame = video.mbps * 1e6 / 8 / 60, drain = video.linkMbps / 8;  // bytes; bytes per microsecond
  double bytes = 0;
  int64_t at = 0;
  for (int64_t k = 0; k * 1000000 / 60 <= t; ++k) {
    const int64_t arrival = k * 1000000 / 60;
    bytes = std::max(0.0, bytes - double(arrival - at) * drain) + frame * (k % 120 == 0 ? 4 : 1);
    at = arrival;
  }
  return int64_t(std::max(0.0, bytes - double(t - at) * drain) / drain);
}
// The USB path: [reads] kept waiting by the Host (an older Host's VM keeps about 2), each answered read replaced
// one round trip later, plus the wait behind [video] on the way back. The reads are already here when the mouse
// first moves, as when the Host read ahead at the VM's first read. The wake fires at the asked time.
Measure Usb(int64_t rtt, int reads, const Video* video = nullptr) {
  const auto events = Events();
  std::vector<std::pair<int64_t, int64_t>> sends;
  std::vector<int64_t> arrivals;  // reads due at the device
  Device device(1, [&](const Bytes& bytes) {
    const auto f = *Decode(bytes.data(), bytes.size());
    if (f.type != kReturn || f.payload.size() != 7) return;
    sends.emplace_back(-1, X(Of(f)));
  });
  int64_t clock = 0;
  uint32_t seq = 0;
  const auto stamp = [&] {
    for (auto& item : sends)
      if (item.first < 0) { item.first = clock; arrivals.push_back(clock + rtt + (video ? Backlog(*video, clock + rtt) : 0)); }
  };
  for (int i = 0; i < reads; ++i) arrivals.push_back(0);
  size_t next = 0;
  const int64_t end = 6500000;
  while (clock < end) {
    std::sort(arrivals.begin(), arrivals.end());
    int64_t t = end;
    if (next < events.size()) t = std::min(t, events[next]);
    if (!arrivals.empty()) t = std::min(t, arrivals.front());
    if (const auto wake = device.Wake()) t = std::min(t, std::max(clock, *wake));
    clock = t;
    if (!arrivals.empty() && arrivals.front() == t) {
      arrivals.erase(arrivals.begin());
      device.Frame(Submit(1, ++seq).data(), 24, t);
    } else if (next < events.size() && events[next] == t) {
      device.Report(R(0, 1, 0), t);
      ++next;
    } else {
      device.Tick(t);
    }
    stamp();
    if (t == end) break;
  }
  return Score(events, sends, 6);
}
// Today's path: movement summed and sent at most every 4 ms on input, the rest by the window timer.
Measure Injected() {
  const auto events = Events();
  std::vector<std::pair<int64_t, int64_t>> sends;
  multitap::RelativeDeltas deltas;
  size_t next = 0;
  int64_t timer = 15625;
  while (next < events.size() || deltas.Pending()) {
    const bool input = next < events.size() && events[next] <= timer;
    const int64_t t = input ? events[next] : timer;
    if (input) {
      deltas.Add(1, 0);
      ++next;
    } else {
      timer += 15625;
    }
    if (const auto step = deltas.Take(t)) sends.emplace_back(t, step->x);
  }
  return Score(events, sends, 6);
}
std::string Line(const char* name, const Measure& m) {
  char text[256];
  std::snprintf(text, sizeof(text), "\"%s\":{\"holdP50Ms\":%.2f,\"holdP95Ms\":%.2f,\"holdMaxMs\":%.2f,\"sendsPerSecond\":%.0f,\"gapP99Ms\":%.2f,\"largestGapMs\":%.2f}",
                name, m.p50, m.p95, m.max, m.perSecond, m.gapP99, m.gap);
  return text;
}
// 30 Mbps of video on a 100 Mbps downlink: bursts of 5 ms per frame, 20 ms per keyframe.
const Video kVideo{30, 100};
void readAheadPace() {
  // With a round trip's worth of reads waiting (the Host's 24 at up to 20 ms), reports leave at the 1 ms floor
  // and video bursts delaying the reads on their way back do not hold motion back or bunch it. With 8 (older
  // Hosts) the same link holds reports for the reads.
  for (const int64_t rtt : {10000, 20000})
    for (const Video* video : {static_cast<const Video*>(nullptr), &kVideo}) {
      const auto m = Usb(rtt, 24, video);
      require(m.p95 <= 0.6 && m.gapP99 <= 2.0, "Reports waited although a round trip's worth of reads was waiting");
    }
  require(Usb(20000, 8).p95 > 2.0 && Usb(10000, 8, &kVideo).gapP99 > 4.0, "The model no longer shows 8 reads holding reports at 20 ms");
}
std::string measurement() {
  std::string out = "{";
  // Reads waiting at this PC: 8 (older Hosts), 24 (usb_core.hpp mouseAhead). Idle, and with kVideo.
  for (const int64_t rtt : {2000, 10000, 20000, 40000}) {
    out += "\"rtt" + std::to_string(rtt / 1000) + "ms\":{";
    for (const int reads : {8, 24}) {
      const auto r = "reads" + std::to_string(reads);
      out += Line(r.c_str(), Usb(rtt, reads)) + "," + Line((r + "Video").c_str(), Usb(rtt, reads, &kVideo)) + ",";
    }
    out += Line("olderHost", Usb(rtt, 2)) + "},";
  }
  out += Line("sendInput", Injected());
  // CPU: one Raw Input event to its report frame, wall clock on this PC.
  Device device(1, [](const Bytes&) {});
  MouseState state;
  for (uint32_t seq = 1; seq <= 8; ++seq) device.Frame(Submit(1, seq).data(), 24, 0);
  const auto start = std::chrono::steady_clock::now();
  const int count = 200000;
  int64_t now = 0;
  for (int i = 0; i < count; ++i) {
    now += 125;
    for (const auto& report : state.Mouse(0, 1, -1, 0, 0, now / 1000)) device.Report(report, now);
    if (i % 8 == 0) device.Frame(Submit(1, uint32_t(100 + i)).data(), 24, now);
  }
  const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()) / count;
  char cpu[96];
  std::snprintf(cpu, sizeof(cpu), ",\"cpuPerEventNs\":%.0f}", ns);
  return out + cpu;
}
}  // namespace

int main() {
  try {
    frames();
    mouseState();
    readAheadHost();
    olderHost();
    pacing();
    sinkRouting();
    readAheadPace();
    const auto measured = measurement();
    std::cout << "{\"passed\":true,\"checks\":" << checks << ",\"physicalInput\":false,\"realDevice\":false,\"model\":" << measured << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
