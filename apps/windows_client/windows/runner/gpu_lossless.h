#ifndef MULTITAP_GPU_LOSSLESS_H_
#define MULTITAP_GPU_LOSSLESS_H_
// ToTBSeamless lossless tiles over GPU pictures (header only). The legacy
// engine (remote_engine.cpp) and the tracks-v1 decoder (remote_track_decoder.cpp)
// share these: tile pixels live in a gpu::Overlay texture, present tiles are
// copied over a freshly converted frame, and a republish clones the newest GPU
// frame instead of reading it back.
#include "remote_decoder.h"
#include "../../../../native/windows_video/lossless_tiles.hpp"
#include <algorithm>

namespace multitap::gpu {
// The present tiles of a compositor over a GPU frame, one copy per run of
// neighbouring tiles in a row.
inline void CopyPresentTiles(Overlay& overlay, const lossless::Compositor& compositor, const Frame& frame) {
  const uint32_t columns = lossless::tileColumns(compositor.width()), rows = lossless::tileRows(compositor.height());
  for (uint32_t row = 0; row < rows; ++row) {
    const uint32_t y0 = row * lossless::tileSize, y1 = std::min(compositor.height(), y0 + lossless::tileSize);
    for (uint32_t column = 0; column < columns;) {
      if (!compositor.present(row * columns + column)) { ++column; continue; }
      uint32_t end = column + 1;
      while (end < columns && compositor.present(row * columns + end)) ++end;
      const uint32_t x0 = column * lossless::tileSize, x1 = std::min(compositor.width(), end * lossless::tileSize);
      overlay.CopyRun(frame, x0, y0, x1 - x0, y1 - y0);
      column = end;
    }
  }
}

// Due drops first, then every present tile over the decoded GPU frame. A frame
// of another size than the compositor's (tiles describe a different picture) is
// left alone; a scaled source is upscaled to the source size first.
inline HRESULT ComposeDecoded(lossless::Compositor& compositor, Converter& converter, Overlay& overlay, DecodedFrame& frame) {
  if (!frame.gpu) return E_UNEXPECTED;
  if (compositor.scaled()) {
    compositor.ApplyDecodedScaled(frame.pts_us);
    if (frame.width != compositor.width() || frame.height != compositor.height()) {
      // A video-size picture decoded before the source size was known.
      std::shared_ptr<Frame> upscaled;
      const HRESULT hr = converter.Scale(*frame.gpu, compositor.width(), compositor.height(), upscaled);
      if (hr == E_PENDING) return S_OK;
      if (FAILED(hr)) return hr;
      frame.gpu = std::move(upscaled); frame.width = compositor.width(); frame.height = compositor.height();
    }
  } else compositor.ApplyDecoded(frame.pts_us);
  if (!compositor.presentTiles() || frame.width != compositor.width() || frame.height != compositor.height()) return S_OK;
  CopyPresentTiles(overlay, compositor, *frame.gpu);
  return converter.Finish();
}

// The tiles accepted since `added_before` go to the overlay texture once, when
// they arrive. False: the overlay could not be sized (tiles stay unused).
inline bool UploadAddedTiles(lossless::Compositor& compositor, Overlay& overlay, size_t added_before) {
  if (!compositor.width() || FAILED(overlay.Configure(compositor.width(), compositor.height()))) return false;
  const auto& added = compositor.addedTiles();
  for (size_t i = added_before; i < added.size(); ++i) {
    const auto rect = lossless::tileRect(added[i], compositor.width(), compositor.height());
    overlay.UploadTile(rect.x, rect.y, rect.width, rect.height, compositor.tilePixels(added[i]), size_t(compositor.width()) * 4);
  }
  return true;
}

// New tiles appear now instead of at the next video frame: a copy of the newest
// frame (which already has every earlier tile) plus the added tiles. False: the
// newest frame is not a GPU frame of a matching size; `out` is then untouched.
inline bool RepublishLatest(lossless::Compositor& compositor, Converter& converter, Overlay& overlay, const DecodedFrame* latest, DecodedFrame& out) {
  if (!latest || !latest->gpu) return false;
  if (latest->width == compositor.width() && latest->height == compositor.height()) {
    std::shared_ptr<Frame> copy;
    if (FAILED(converter.Clone(*latest->gpu, copy))) return false;
    if (!compositor.poisoned()) for (const uint32_t index : compositor.addedTiles()) if (compositor.present(index)) {
      const auto rect = lossless::tileRect(index, compositor.width(), compositor.height());
      overlay.CopyRun(*copy, rect.x, rect.y, rect.width, rect.height);
    }
    compositor.clearAdded();
    if (FAILED(converter.Finish())) return false;
    out = DecodedFrame{}; out.width = latest->width; out.height = latest->height; out.pts_us = latest->pts_us; out.gpu = std::move(copy);
    return true;
  }
  if (compositor.scaled() && latest->width == compositor.videoWidth() && latest->height == compositor.videoHeight()) {
    // The newest frame is still the video size: scale it once under the tiles.
    std::shared_ptr<Frame> source;
    if (FAILED(converter.Scale(*latest->gpu, compositor.width(), compositor.height(), source))) return false;
    if (!compositor.poisoned()) CopyPresentTiles(overlay, compositor, *source);
    compositor.clearAdded();
    if (FAILED(converter.Finish())) return false;
    out = DecodedFrame{}; out.width = compositor.width(); out.height = compositor.height(); out.pts_us = latest->pts_us; out.gpu = std::move(source);
    return true;
  }
  return false;
}
}  // namespace multitap::gpu
#endif
