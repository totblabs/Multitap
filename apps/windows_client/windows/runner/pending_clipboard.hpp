#pragma once
// Per-session delayed rendering. No clipboard payload is read by observation.
#include "clipboard.hpp"
#include "pull.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace multitap {
struct PendingClipboardLease {
  std::string token, owner, kind;
  uint64_t epoch = 0;
  DWORD sourceSequence = 0, sequence = 0;
};
struct PendingClipboardMedium { virtual ~PendingClipboardMedium() = default; };
using PendingClipboardMediumPtr = std::shared_ptr<PendingClipboardMedium>;
// Every desktop operation is replaceable by a memory stand-in. Production
// prepares HGLOBAL blocks on the independent delivery worker, never in render.
struct PendingClipboardAccess {
  bool installWindowHook = true;
  std::function<DWORD()> sequence;
  std::function<HWND()> owner;
  std::function<bool(HWND)> open;
  std::function<void()> close;
  std::function<bool()> empty;
  // CountClipboardFormats while the clipboard is open; negative when it cannot be counted.
  std::function<int()> count;
  std::function<UINT()> pngFormat;
  std::function<UINT()> htmlFormat;
  std::function<bool(UINT)> delayed;
  std::function<PendingClipboardMediumPtr(std::vector<uint8_t>)> prepare;
  std::function<bool(UINT,const PendingClipboardMediumPtr&)> publish;
  std::function<PendingClipboardMediumPtr(HGLOBAL)> adopt;
  std::function<std::vector<uint8_t>(const std::vector<uint8_t>&,uint32_t,uint32_t)> decodeImage;
  std::function<std::string()> token;
};
class PendingClipboard {
 public:
  explicit PendingClipboard(PendingClipboardAccess access = {},
      std::chrono::milliseconds renderTimeout = std::chrono::seconds(5));
  ~PendingClipboard();
  PendingClipboard(const PendingClipboard&) = delete;
  void Attach(HWND writer);  // clipboard STA only; the existing hidden HWND
  void Detach();            // same STA; never waits for a missing payload
  void SetOwnSequenceSink(std::function<void(DWORD)> sink);
  void SetRenderQueue(std::function<bool(std::function<void()>)> queue);
  clipboard::PullRenderPolicy DelegatePolicy(const PendingClipboardLease& lease);
  void BindDelegate(const PendingClipboardLease& lease,const std::string& request,
      std::shared_ptr<clipboard::PullOffer> offer);
  DWORD Observe(const std::string& owner,uint64_t epoch);
  std::optional<PendingClipboardLease> Begin(const std::string& owner,uint64_t epoch,
      DWORD expectedPrior,DWORD sourceSequence,const std::string& kind);
  bool Bind(const PendingClipboardLease& lease,const std::string& request);
  void BeginData(const PendingClipboardLease& lease,const std::string& request,
      const std::string& format,std::vector<clipboard::Item> items);
  void Append(const PendingClipboardLease& lease,const std::string& request,
      size_t index,uint64_t offset,const std::vector<uint8_t>& bytes);
  void Finish(const PendingClipboardLease& lease,const std::string& request,size_t index,const std::string& digest);
  DWORD Commit(const PendingClipboardLease& lease,const std::string& request);
  void Cancel(const PendingClipboardLease& lease);
  // Fast invalidation; the channel delays its RPC ACK until a clipboard-STA
  // barrier follows any already-admitted publication. No UI-thread wait.
  // Epoch tombstones are never evicted/reopened. A higher epoch is a new watch.
  void CancelOwner(const std::string& owner,uint64_t epoch);
  void Shutdown();
  // Production calls these only from WM_RENDERFORMAT / WM_RENDERALLFORMATS.
  // Render must NOT OpenClipboard: the paste requester already holds it open.
  bool Render(UINT format);
  void RenderAll();
  void LostOwnership();
  // The clipboard holds this client's placeholder and Render would refuse it (cancelled, failed, or its watch ended before the data).
  bool Unrenderable();
 private:
  struct State;
  std::shared_ptr<State> state_;
  static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
};
}
