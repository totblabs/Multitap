#pragma once

// These are runtime XML additions, not changes to the signed driver package or
// the pinned original settings. Keep the catalog bounded for GPU-less guests.
namespace common_modes {
constexpr unsigned revision = 1;
static const std::vector<Mode> &all() {
  static const std::vector<Mode> values = {
      {1024, 768, 60},  {1280, 720, 60},  {1280, 800, 60},
      {1280, 1024, 60}, {1366, 768, 60},  {1440, 900, 60},
      {1600, 900, 60},  {1600, 1200, 60}, {1680, 1050, 60},
      {1920, 1080, 60}, {1920, 1200, 60}, {2048, 1152, 60},
      {2560, 1080, 60}, {2560, 1440, 60}, {2560, 1600, 60},
      {3440, 1440, 60}, {3840, 2160, 60}, {1080, 1920, 60}};
  return values;
}
static bool same(const Mode &a, const Mode &b) {
  return a.width == b.width && a.height == b.height &&
         a.refreshRate == b.refreshRate;
}
static bool contains(const Mode &mode) {
  return std::any_of(all().begin(), all().end(),
                     [&](const Mode &item) { return same(mode, item); });
}
static bool original(const Mode &mode) {
  return same(mode, {1920, 1080, 60}) || same(mode, {1280, 720, 60});
}
} // namespace common_modes
