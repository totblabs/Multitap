#pragma once
#include <cstdint>
#include <cstddef>
#include "lossless_tiles.hpp"

namespace multitap::video {
// The legacy queue already admitted this media envelope. Lossless tiles are
// video-kind records too, but cannot make the H.264 transform advance.
inline bool queuedH264AccessUnit(const uint8_t* bytes, size_t size) {
  return bytes&&size>12&&bytes[0]==1&&(bytes[1]&~1u)==0&&!lossless::isTilePayload(bytes+12,size-12);
}
enum class DelayedOutputResult { ready, nextInput, timedOut, failed };
// An accepted access unit has not produced output yet. If another COMPLETE
// access unit already waits, give it to the decoder instead of polling while
// holding up that input. This drops no encoded packet and assigns no timestamp:
// readOutput delivers the MFT sample's own PTS through its existing callback.
// With no further input (a still screen), retain the original bounded output
// drain. A scheduling hint never resets that whole wait's original deadline.
// All callbacks run synchronously on the decoder worker; no detached jobs.
template<class Now, class Ready, class NextInput, class Pause, class ReadOutput>
DelayedOutputResult pumpDelayedOutput(uint64_t budgetMs, Now now, Ready ready,
                                     NextInput nextInput, Pause pause, ReadOutput readOutput) {
  const uint64_t start=now();
  while(!ready()) {
    if(nextInput())return DelayedOutputResult::nextInput;
    const uint64_t current=now();
    if(current<start || current-start>=budgetMs)return DelayedOutputResult::timedOut;
    pause();
    if(!readOutput())return DelayedOutputResult::failed;
  }
  return DelayedOutputResult::ready;
}
}
