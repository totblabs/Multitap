#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

namespace multitap {
enum class RemoteCursorFormat:uint8_t {ColorBgraAnd=1,MonochromeAndXor=2};
struct RemoteCursorShape {
  RemoteCursorFormat format=RemoteCursorFormat::ColorBgraAnd;
  uint16_t width=0,height=0,hotspotX=0,hotspotY=0;
  std::vector<uint8_t> color,mask;
};
struct RemoteCursorSelection {bool handled=false;HCURSOR cursor=nullptr;};
bool validRemoteCursorShape(const RemoteCursorShape& shape) noexcept;
// Bytes per 1-bpp mask row (DWORD aligned), as GetDIBits/CreateDIBSection use.
size_t RemoteCursorMaskStride(unsigned width) noexcept;
// A new native cursor identical to |shape| (alpha, AND transparency and XOR
// inversion are drawn by Windows), or nullptr for an invalid shape or a failed
// allocation. The caller destroys it with DestroyIcon.
HCURSOR CreateRemoteCursorHandle(const RemoteCursorShape& shape) noexcept;
// One UI thread/output owns and destroys this object and calls Selection in
// WM_SETCURSOR AND after shape/visibility changes while the pointer is still.
// The caller applies the returned handle only when handled is true. No global
// ShowCursor counter, SetCursorPos, files, system cursor replacement or hooks.
class RemoteCursor {
 public:
  RemoteCursor()=default;
  ~RemoteCursor();
  RemoteCursor(const RemoteCursor&)=delete;
  RemoteCursor& operator=(const RemoteCursor&)=delete;
  bool Bind(uint64_t lease,uint16_t epoch);
  HRESULT SetShape(uint64_t lease,uint16_t epoch,const RemoteCursorShape& shape);
  bool SetActive(uint64_t lease,uint16_t epoch,bool active,bool visible);
  bool Unbind(uint64_t lease);
  RemoteCursorSelection Selection(uint64_t lease,uint16_t epoch) const;
 private:
  bool matches(uint64_t lease,uint16_t epoch) const;
  void clear();
  const DWORD owner_=GetCurrentThreadId();
  uint64_t lease_=0,highestLease_=0;
  uint16_t epoch_=0;
  bool bound_=false,active_=false,visible_=false;
  HCURSOR cursor_=nullptr;
};
}
