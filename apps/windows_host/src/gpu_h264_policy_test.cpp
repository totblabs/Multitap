#include "gpu_h264_policy.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
int checks = 0;
void require(bool value, const char* reason) { ++checks; if (!value) throw std::runtime_error(reason); }
}
int main() {
  try {
    using namespace host::gpu;
    for (const uint64_t luid : {uint64_t(0), uint64_t(0x123456789abcdef0), UINT64_MAX}) {
      const auto text = adapterLuidText(luid);
      require(text.size() == 17 && parseAdapterLuid(text) == luid, "Adapter LUID lost bits or sign across text round trip");
    }
    for (const auto text : {"1", "0x123456789abcdef0", "12345678:1234567", "12345678:123456789", "12345678:1234567g", " 12345678:12345678", "12345678/12345678"})
      require(!parseAdapterLuid(text), "Noncanonical or overflowing adapter identity accepted");
    require(parseAdapterLuid("FFFFFFFF:FFFFFFFF") == UINT64_MAX, "Uppercase adapter identity lost bits");
    const Limits observed{16, 16, 12288, 12288, 400000, 24000000};
    for (const auto& shape : {std::array<uint32_t, 2>{7680, 4320}, {4320, 7680}, {10240, 2160}, {5120, 1440}})
      require(pictureFits(observed, shape[0], shape[1], 60), "Queried >4K/portrait/ultrawide capability was replaced by a product cap");
    require(pictureFits(observed, 3840, 2160, 60) && pictureFits(observed, 2560, 1440, 60), "Independent dual encoded pictures were incorrectly summed");
    require(!pictureFits(observed, 12290, 2160, 60), "Actual queried width limit ignored");
    require(!pictureFits(observed, 3841, 2160, 60), "Odd NV12 width accepted");
    require(!pictureFits(observed, UINT32_MAX, UINT32_MAX, UINT32_MAX), "Capability arithmetic overflow accepted");
    auto tight = observed; tight.maximumMacroblocks = 100;
    require(!pictureFits(tight, 1920, 1080, 60), "Actual queried macroblock limit ignored");
    tight = observed; tight.maximumMacroblocksPerSecond = 100;
    require(!pictureFits(tight, 1920, 1080, 60), "Actual queried throughput limit ignored");
    require(!pictureFits({}, 1920, 1080, 60), "Unobserved capability was treated as supported");
    const std::vector<uint8_t> key{0,0,0,1,0x67,0x42,0x00,0x1e,0,0,1,0x68,0xc0,0,0,0,1,0x65,0x88};
    const auto first = inspectAnnexB(key, 1024);
    require(first.valid && first.idr && first.sps && first.pps && first.spsProfile == 66 && first.spsLevel == 30, "SPS/PPS IDR access unit or actual profile/level rejected");
    require(!inspectAnnexB(key, key.size() - 1).valid, "Oversized packet ignored its negotiated wire bound");
    for (const auto& invalid : std::vector<std::vector<uint8_t>>{{}, {1,2,3,4}, {0,0,1}, {0,0,1,0x65}, {0,0,1,0xe5,0x80}, {0,0,1,0x65,0x88,0,0,1}})
      require(!inspectAnnexB(invalid, 1024).valid, "Malformed AnnexB output accepted");
    const std::vector<uint8_t> predicted{0,0,1,0x41,0x80};
    const auto next = inspectAnnexB(predicted, 1024);
    require(next.valid && !next.idr && !next.sps && !next.pps, "Ordinary predicted access unit misclassified");
    std::cout << "{\"kind\":\"gpu-h264-policy\",\"checks\":" << checks << ",\"actualGpu\":false,\"actualEncoder\":false,\"passed\":true}\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
