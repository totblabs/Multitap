#pragma once
#include <atomic>
// Test-only switch for the NVENC backend (gpu_h264_interop_test.cpp). The product never
// sets it; the default leaves every encoder untouched. No environment, settings or wire setter.
namespace host::gpu::nvenctesting {
// Non-zero: long-term references begin with this many slots whatever the stream's level
// holds, to meet on a real GPU the recovery frame an encoder does not code from its slot.
extern std::atomic<int> longTermSlots;
}
