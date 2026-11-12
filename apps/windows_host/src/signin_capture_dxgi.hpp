#pragma once
#include "signin_worker_native.hpp"
#include <vector>

namespace host::signin::native {
struct BgraFrame {
  uint32_t width=0,height=0;uint64_t sequence=0;
  std::vector<uint8_t> pixels;
};
// This seam shares the real acquisition/revalidation/release orchestration with
// fake metadata fixtures. The production implementation below uses actual DXGI.
class CaptureAccess {
 public:
  virtual ~CaptureAccess()=default;
  virtual Status acquire(uint32_t timeoutMs)=0;
  virtual Status read(BgraFrame&)=0;
  virtual Status release()noexcept=0;
};
Status acquireValidatedFrame(CaptureAccess&,const std::function<Status()>&,
                            BgraFrame&,uint32_t timeoutMs);
bool validFrameShape(uint32_t width,uint32_t height,uint32_t pitch,size_t bytes);
// One positively owned output per instance. Caller may not substitute another
// display on unsupported IDD/DXGI, desktop switch, or adapter/output mismatch.
// This initial path supports identity rotation; other rotations are explicitly
// unsupported until separately implemented/qualified, never silently rotated.
class CaptureDxgi {
 public:
  CaptureDxgi(const Permit&,const OwnedTarget&);
  ~CaptureDxgi();
  CaptureDxgi(const CaptureDxgi&)=delete;CaptureDxgi& operator=(const CaptureDxgi&)=delete;
  Status next(BgraFrame&,uint32_t timeoutMs);
 private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
