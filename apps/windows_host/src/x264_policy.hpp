#pragma once
#include "support.hpp"

namespace host::x264policy {
inline constexpr size_t maximumConfigBytes = 4096;

// Invalid or absent policy keeps the released affinity-based automatic choice.
inline X264Settings fromConfig(const std::string& bytes) {
  auto settings = X264Settings::production();
  if (bytes.size() > maximumConfigBytes) return settings;
  try {
    const auto value = JsonParser::parse(bytes);
    const auto* count = std::get_if<double>(&value.get("x264Threads").value);
    if (count && std::isfinite(*count) && std::floor(*count) == *count && *count >= 1 && *count <= 8)
      settings.threads = int(*count);
  } catch (...) {}
  return settings;
}

template<class Read> X264Settings readProtected(bool trusted, Read read) {
  if (trusted) try { return fromConfig(read()); } catch (...) {}
  return X264Settings::production();
}
}
