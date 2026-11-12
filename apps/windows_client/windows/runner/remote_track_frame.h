#pragma once
#include "remote_decoder.h"
#include <memory>
namespace multitap {
struct RemoteTrackFrame {
  uint8_t track=0;
  uint32_t topology=0,epoch=0;
  uint64_t serial=0;
  std::shared_ptr<const DecodedFrame> pixels;
};
}
