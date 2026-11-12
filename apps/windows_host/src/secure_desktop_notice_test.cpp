// Secure desktop notice (secure_desktop_notice.hpp): the tick decisions and the picture.
// Optional argument: a directory that receives notice-<w>x<h>.bmp for a visual check.
#include "secure_desktop_notice.hpp"
#include "stream_prepare_diagnostics.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>

namespace {
int failures = 0;
void expect(bool value, const char* what) { if (!value) { ++failures; std::cerr << "FAIL: " << what << '\n'; } }
using Clock = host::SecureDesktopNotice::Clock;
using Step = host::SecureDesktopStep;

void decisions() {
  const auto t0 = Clock::now();
  {
    host::SecureDesktopNotice n;
    expect(n.step(true, true, t0) == Step::capture, "available desktop captures");
    expect(!n.active(), "capture leaves no notice");
    expect(n.step(false, true, t0) == Step::notice, "UAC of an unlocked session shows the notice");
    expect(n.active(), "notice active");
    expect(n.step(false, true, t0 + std::chrono::minutes(9)) == Step::notice, "notice continues within the limit");
    expect(n.step(true, true, t0 + std::chrono::seconds(75)) == Step::resume, "returned desktop resumes");
    expect(!n.active() && n.resumedRecord() == (2u | (75u << 8)), "resume record carries seconds");
    expect(n.step(true, true, t0 + std::chrono::seconds(76)) == Step::capture, "after resume it captures");
  }
  {
    host::SecureDesktopNotice n;
    expect(n.step(false, false, t0) == Step::end, "a locked session ends as before");
    expect(n.endedRecord() == 3, "locked record");
  }
  {
    host::SecureDesktopNotice n;
    (void)n.step(false, true, t0);
    expect(n.step(false, false, t0 + std::chrono::seconds(5)) == Step::end, "locking during the notice ends it");
    expect(!n.active() && n.endedRecord() == 3, "locked during notice record");
  }
  {
    host::SecureDesktopNotice n;
    (void)n.step(false, true, t0);
    expect(n.step(false, true, t0 + host::secureDesktopNoticeLimit) == Step::end, "the limit ends the notice");
    expect(n.endedRecord() == 4, "limit record");
  }
  {
    // While the SYSTEM helper's real prompt shows it can be answered: the limit does not run. It runs again
    // from a fallback to the notice, and the returned-desktop record still counts the whole secure desktop.
    host::SecureDesktopNotice n;
    n.answerable(t0);
    expect(n.step(false, true, t0) == Step::notice, "answerable before any notice changes nothing");
    for (int minute = 1; minute <= 25; ++minute) { n.answerable(t0 + std::chrono::minutes(minute)); expect(n.step(false, true, t0 + std::chrono::minutes(minute)) == Step::notice, "a prompt that streams is not ended by the limit"); }
    expect(n.step(false, true, t0 + std::chrono::minutes(34)) == Step::notice, "nine minutes after the fallback the notice still shows");
    expect(n.step(false, true, t0 + std::chrono::minutes(35)) == Step::end && n.endedRecord() == 4, "ten minutes on the notice end the stream");
    host::SecureDesktopNotice back;
    (void)back.step(false, true, t0); back.answerable(t0 + std::chrono::seconds(40));
    expect(back.step(true, true, t0 + std::chrono::seconds(50)) == Step::resume && back.resumedRecord() == (2u | (50u << 8)), "the resumed record counts from the first sight of the secure desktop");
    expect(back.step(false, false, t0 + std::chrono::seconds(60)) == Step::end && back.endedRecord() == 3, "a lock still ends at once");
  }
  expect(host::streamObservationOk(44, 1) && host::streamObservationOk(44, 2 | (30u << 8)), "shown/resumed are observations");
  for (uint32_t kept : {5u, 6u, 7u, 8u}) expect(host::streamObservationOk(44, kept), "helper records 5..8 are observations (the stream continues)");
  expect(!host::streamObservationOk(44, 0) && !host::streamObservationOk(44, 9), "unknown 244 values are not ok records");
  expect(host::streamObservationOk(45, 1) && host::streamObservationOk(45, 2) && !host::streamObservationOk(45, 3), "stage 45 kept from main");
  expect(!host::streamObservationOk(44, 3) && !host::streamObservationOk(44, 4), "ends are failures");
  expect(std::string(host::secureDesktopUnavailable) == "Interactive desktop is locked, secure or unavailable", "matches media.cpp");
}

struct Counts { size_t canvas = 0, amber = 0, ink = 0, other = 0; };
Counts count(const std::vector<uint8_t>& bgra) {
  Counts c;
  for (size_t i = 0; i < bgra.size(); i += 4) {
    const int b = bgra[i], g = bgra[i + 1], r = bgra[i + 2];
    if (r == 0x0F && g == 0x12 && b == 0x15) ++c.canvas;
    else if (r == 0xF5 && g == 0xA6 && b == 0x23) ++c.amber;
    else if (r > 0xC0 && g > 0xC0 && b > 0xC0) ++c.ink;
    else ++c.other;
  }
  return c;
}
void writeBmp(const std::string& path, const std::vector<uint8_t>& bgra, int width, int height) {
  BITMAPFILEHEADER file{}; BITMAPINFOHEADER info{};
  info.biSize = sizeof(info); info.biWidth = width; info.biHeight = -height; info.biPlanes = 1; info.biBitCount = 32; info.biCompression = BI_RGB;
  file.bfType = 0x4D42; file.bfOffBits = sizeof(file) + sizeof(info); file.bfSize = DWORD(file.bfOffBits + bgra.size());
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(&file), sizeof(file)); out.write(reinterpret_cast<const char*>(&info), sizeof(info));
  out.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
}

void pictures(const char* directory) {
  const std::pair<int, int> sizes[] = {{1920, 1080}, {1142, 862}, {2908, 1340}, {800, 1280}, {640, 360}, {320, 200}, {96, 64}, {16, 16}};
  for (const auto& [w, h] : sizes) {
    const auto bgra = host::secureDesktopNoticePicture(w, h);
    expect(bgra.size() == size_t(w) * h * 4, "picture size");
    bool opaque = true; for (size_t i = 3; i < bgra.size(); i += 4) opaque &= bgra[i] == 255;
    expect(opaque, "picture is opaque");
    const auto c = count(bgra);
    expect(c.canvas > bgra.size() / 4 / 4, "mostly charcoal canvas");
    if (w >= 96) expect(c.amber > 0, "amber mark drawn");
    if (w >= 640) expect(c.ink > 200, "title text drawn");
    // Text sits inside the panel: the outer border rows stay canvas.
    bool border = true; for (int x = 0; x < w; ++x) { const size_t top = size_t(x) * 4, bottom = (size_t(h - 1) * w + x) * 4; border &= bgra[top] == 0x15 && bgra[bottom] == 0x15; }
    expect(border, "border rows are canvas");
    std::cout << "{\"size\":\"" << w << 'x' << h << "\",\"amber\":" << c.amber << ",\"ink\":" << c.ink << ",\"other\":" << c.other << "}\n";
    if (directory) writeBmp(std::string(directory) + "\\notice-" + std::to_string(w) + 'x' + std::to_string(h) + ".bmp", bgra, w, h);
  }
  bool threw = false; try { (void)host::secureDesktopNoticePicture(1, 1); } catch (const std::exception&) { threw = true; }
  expect(threw, "a degenerate size is refused");
}
}  // namespace

int main(int argc, char** argv) {
  decisions();
  pictures(argc > 1 ? argv[1] : nullptr);
  if (failures) { std::cerr << failures << " failure(s)\n"; return 1; }
  std::cout << "secure desktop notice: ok\n";
  return 0;
}
