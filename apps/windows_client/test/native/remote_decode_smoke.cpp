#include "remote_decoder.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2) return 1;
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) return 2;
  if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 3; }
  size_t decoded = 0, supplied = 0; uint64_t checksum = 0;
  const uint32_t width = argc > 3 ? static_cast<uint32_t>(std::stoul(argv[3])) : 1280;
  const uint32_t height = argc > 4 ? static_cast<uint32_t>(std::stoul(argv[4])) : 720;
  {
    multitap::RemoteDecoder decoder;
    hr = decoder.Initialize(width, height);
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) hr = E_FAIL;
    auto output = [&](multitap::DecodedFrame&& frame) {
      ++decoded;
      for (size_t i = 0; i < frame.rgba.size(); i += 4) checksum += frame.rgba[i] + frame.rgba[i + 1] + frame.rgba[i + 2];
      if (decoded == 1 && argc > 2) {
        std::ofstream image(argv[2], std::ios::binary); image << "P6\n" << frame.width << " " << frame.height << "\n255\n";
        for (size_t i = 0; i < frame.rgba.size(); i += 4) image.write(reinterpret_cast<const char*>(frame.rgba.data() + i), 3);
      }
    };
    while (SUCCEEDED(hr) && file) {
      uint8_t header[4]{}; file.read(reinterpret_cast<char*>(header), 4);
      if (file.gcount() == 0) break;
      if (file.gcount() != 4) { hr = E_FAIL; break; }
      const uint32_t size = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
      if (!size || size > 4 * 1024 * 1024) { hr = E_INVALIDARG; break; }
      std::vector<uint8_t> bytes(size); file.read(reinterpret_cast<char*>(bytes.data()), size);
      if (static_cast<size_t>(file.gcount()) != size) { hr = E_FAIL; break; }
      hr = decoder.Decode(bytes.data(), bytes.size(), supplied * 1000000 / 60, output); ++supplied;
    }
    if (SUCCEEDED(hr)) hr = decoder.Drain(output);
  }
  MFShutdown(); CoUninitialize();
  std::cout << "software H264 decode: supplied=" << supplied << " decoded=" << decoded << " rgbChecksum=" << checksum << " hr=0x" << std::hex << hr << "\n";
  return SUCCEEDED(hr) && decoded == supplied && decoded >= 60 && checksum > 0 ? 0 : 4;
}
