#pragma once
#include <atomic>
#include <cstdint>
namespace host { enum class EncoderBackend : uint8_t; struct X264Settings; }
// Test-only fault injection for the x264 backend (x264_encoder_test.cpp). The
// product never sets these; defaults leave every encoder untouched.
namespace host::x264testing {
// Test-only automatic backend provider. Null preserves the product's protected
// configuration readers. A fixture may return only x264 or mft; installing it
// also prohibits actual GPU opening. No environment, settings or wire setter.
using ConfigurationProvider = EncoderBackend (*)();
extern std::atomic<ConfigurationProvider> configurationProvider;
// Isolated CPU policy fixtures; no installed settings or GPU APIs are needed.
using SettingsProvider = X264Settings (*)();
using OpenObserver = void (*)(int requestedThreads, int appliedThreads) noexcept;
extern std::atomic<SettingsProvider> settingsProvider;
extern std::atomic<OpenObserver> openObserver;
// Non-zero: the next x264 open throws with this EncoderFallback code.
extern std::atomic<uint32_t> failOpenCode;
// >= 0: an x264 encoder throws failEncodeCode instead of coding its frame with
// this zero-based index (each encoder counts its own frames).
extern std::atomic<int64_t> failEncodeAtFrame;
extern std::atomic<uint32_t> failEncodeCode;
}
