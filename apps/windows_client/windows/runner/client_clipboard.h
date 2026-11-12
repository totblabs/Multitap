#pragma once
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <windows.h>
#include <filesystem>
#include <memory>
#include <string>
#include <functional>
#include "clipboard.hpp"
#include "change_watch.hpp"
#include "pull.hpp"
#include "pending_clipboard.hpp"
#include <atomic>
#include <vector>

namespace multitap {
// Diagnostics only (fixed fields, never paths or content): whether the user
// of the desktop shell (the Explorer that pastes) can open files this client
// just published. flags: 1 = this client was started elevated, 2 = the desktop user
// is a different account, 4 = no desktop shell token to test with,
// 8 = a published path is MAX_PATH (260) or longer.
struct PublishCheck { bool readable = false; DWORD error = 0; uint32_t flags = 0; };
PublishCheck CheckDesktopRead(const std::vector<std::filesystem::path>& files);
// Publishes received files in this (unelevated) process.
DWORD PublishReceivedFiles(HWND window, const std::vector<std::filesystem::path>& files, std::optional<DWORD> expected);
// OpenClipboard + EmptyClipboard under window; the unchanged-sequence check and the emptying happen in one open.
DWORD ClaimClipboard(HWND window, std::optional<DWORD> expected);
struct ClientClipboardAccess {
  std::function<clipboard::Snapshot(HWND,bool,bool)> read = [](HWND window,bool text,bool files){return clipboard::read(window,text,files);};
  std::function<DWORD()> sequence = [] { return GetClipboardSequenceNumber(); };
  std::function<DWORD(HWND, const std::wstring&, std::optional<DWORD>)> putText = [](HWND window, const std::wstring& text, std::optional<DWORD> expected) { return clipboard::putText(window,text,expected); };
  std::function<DWORD(HWND, const std::vector<std::filesystem::path>&, std::optional<DWORD>)> putFiles = [](HWND window, const std::vector<std::filesystem::path>& files, std::optional<DWORD> expected) { return PublishReceivedFiles(window,files,expected); };
  std::function<PublishCheck(const std::vector<std::filesystem::path>&)> checkPublished = [](const std::vector<std::filesystem::path>& files) { return CheckDesktopRead(files); };
  std::function<HWND()> owner = [] { return GetClipboardOwner(); };
  // The formats profile: rich text, images and folder trees are read and published as well.
  std::function<clipboard::Snapshot(HWND,const clipboard::ReadOptions&)> readFormats = [](HWND window,const clipboard::ReadOptions& options){return clipboard::read(window,options);};
  clipboard::Publishers publishers;
  PendingClipboardAccess pending;
  // Claims the clipboard for the copy that is arriving: emptied under window (nothing to paste until the content is published), only
  // while it still holds what it held at expected. Returns the new sequence; throws when it changed or cannot be opened.
  std::function<DWORD(HWND,std::optional<DWORD>)> claim = [](HWND window,std::optional<DWORD> expected){ return ClaimClipboard(window,expected); };
  // The paste-pull profile publishes the peer's offer as an OLE data object; QA stand-ins keep it in memory.
  std::function<DWORD(const std::shared_ptr<clipboard::PullOffer>&,std::optional<DWORD>)> publishOffer = [](const std::shared_ptr<clipboard::PullOffer>& offer,std::optional<DWORD> expected){ return clipboard::publishPullOffer(offer,expected); };
  // A received folder tree served as virtual files (paths of MAX_PATH or more) is still the clipboard: our own content, never announced back.
std::function<bool()> localTreeCurrent = []{ return clipboard::localTreeIsCurrent(); };
std::function<bool(const std::shared_ptr<clipboard::PullOffer>&)> offerCurrent = [](const std::shared_ptr<clipboard::PullOffer>& offer){ return clipboard::pullOfferIsCurrent(offer); };
  // Clears the clipboard when (and only when) that offer is still on it; returns the sequence after clearing, 0 when nothing was touched.
  std::function<DWORD(const std::shared_ptr<clipboard::PullOffer>&)> withdrawOffer = [](const std::shared_ptr<clipboard::PullOffer>& offer){ return clipboard::withdrawPullOffer(offer); };
};
// A request of the virtual files (an Explorer paste) or a notice about one, for the session to turn into relay
// messages: kind is request (offerId, index, offset), cancel, ack (index, end) or failed (index, reason).
struct PullEvent { std::string kind; uint64_t rangeId = 0; std::string offerId, reason; uint64_t index = 0, offset = 0, end = 0; };
// Reads/transfers require an authorized-session action. A separate metadata-only
// listener is explicitly leased to the foreground viewer while auto sync is on.
class ClientClipboardState {
 public:
  explicit ClientClipboardState(std::filesystem::path test_parent = {}, ClientClipboardAccess access = {});
  ~ClientClipboardState();
  flutter::EncodableValue Run(const std::string& method,
                             const flutter::EncodableMap& arguments, HWND owner);
  // The normal STA job entry, including its error-cleanup policy. Scoped source
  // failures never fall back to generic owner release. Direct Run gains no
  // job-level cleanup; capture itself has the new exact-attempt failure guard.
  flutter::EncodableValue RunJob(const std::string& method,
                                const flutter::EncodableMap& arguments, HWND owner);
  // Paste-pull. Pull: data (pullData), end (pullEnd) and failure (pullFail) of a range, pullShutdown, and the on-paste revision's pullAbort
  // (the user cancelled: blocked renders and the streams of the paste end), pullSupersede (a newer copy: blocked renders end) and pullRetire
  // (the peer withdrew an offer: its readers fail); any thread.
  void Pull(const std::string& method, const flutter::EncodableMap& arguments);
  // pullOpen / pullRead / pullDigest / pullClose: reading an offered file for the peer's paste; any thread, one at a time.
  flutter::EncodableValue Serve(const std::string& method, const flutter::EncodableMap& arguments);
  std::vector<PullEvent> TakePullEvents();
  void SetPullNotify(std::function<void()> notify);
  void SetOwnSequenceSink(std::atomic<DWORD>* sink);
  void AttachPending(HWND writer);
  void SetPendingRenderQueue(std::function<bool(std::function<void()>)> queue);
  void DetachPending();
  flutter::EncodableValue PendingControl(const std::string&,const flutter::EncodableMap&);
  flutter::EncodableValue PendingTransfer(const std::string&,const flutter::EncodableMap&);
  void FailPending(const flutter::EncodableMap&);
  void StopPending();
  void CloseAllOffers();
 private:
  struct State; std::unique_ptr<State> state_;
};
class ClientClipboardChannel {
 public:
  static constexpr UINT completed_message = WM_APP + 181;
  static constexpr UINT pull_message = WM_APP + 182;
  // state_access and state_parent are for tests (an in-memory clipboard); the application passes neither.
  explicit ClientClipboardChannel(HWND reply_window, clipboard::ChangeWatchAccess access = {},
      std::function<bool()> foreground = {}, ClientClipboardAccess state_access = {},
      std::filesystem::path state_parent = {});
  ~ClientClipboardChannel();
  void Enqueue(const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void Complete();
  DWORD Watch(const std::string& owner, bool enabled);
  // After Watch(owner, true): whether what the clipboard holds is this client's own publication.
  bool WatchFoundOwn();
  void ClipboardChanged();
  void ForegroundLost();
  void SetChanged(std::function<void(const std::string&, DWORD)> changed);
  void SetPullHandler(std::function<void(const PullEvent&)> handler);
  // On pull_message: hands the pending pull events to the handler (platform thread).
  void DeliverPullEvents();
 private:
  struct State; std::unique_ptr<State> state_;
};
}
