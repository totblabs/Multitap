#pragma once
#include "remote_decoder.h"
#include "thumbnail_generation.h"
#include <windows.h>
#include <wincodec.h>
#include <condition_variable>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace multitap {
// Last-frame pictures of streaming sessions for the launcher tiles. They are
// screen content: written only under the current Windows user's LocalAppData,
// never uploaded, logged or put into diagnostics. The UI thread only hands
// over a reference to a frame an engine already holds (no pixel copy); one
// worker thread does all scaling, WIC encoding and file replacement, so the
// decode and texture paths do no additional work.
class ThumbnailWriter {
 public:
  static constexpr UINT completed_message = WM_APP + 186;
  static constexpr uint32_t kMaxWidth = 480, kMaxHeight = 270;
  struct Target {
    std::string key, desktop, epoch;
    std::wstring folder, path;
    // An existing folder this Windows user owns (%LOCALAPPDATA%, or the
    // caller's root); every folder below it up to |folder| is created or
    // checked one by one and must be a plain folder, never a link.
    std::wstring anchor;
  };
  struct Timing {
    double scaleMs = 0, encodeMs = 0, writeMs = 0;
    uint32_t width = 0, height = 0;
    size_t bytes = 0;
  };
  using Ticket=ThumbnailGeneration::Ticket;
  struct Notice {std::string key,desktop,epoch;Ticket ticket;bool invalidated=false;};
  // |notify| receives completed_message after a picture was replaced.
  explicit ThumbnailWriter(HWND notify = nullptr) : notify_(notify) {}
  // Finishes the (bounded) queued pictures, then joins the worker.
  ~ThumbnailWriter();
  ThumbnailWriter(const ThumbnailWriter&) = delete;
  ThumbnailWriter& operator=(const ThumbnailWriter&) = delete;
  // <root>\<key>\<desktop>.jpg; root defaults to %LOCALAPPDATA%\Multitap\thumbnails.
  // |key| is exactly 16 lowercase hex digits and |desktop| is pve-<digits> or
  // pc-<lowercase UUID>, so neither can name another directory. A given |root| must exist.
  // Other programs of the user can change this folder,
  // so the writer follows no link anywhere below the anchor: a junction or
  // symbolic link in the way refuses the picture instead of redirecting it,
  // and the anchor must belong to this user (not an administrator's folder).
  static std::optional<Target> Resolve(const std::string& key, const std::string& desktop,
                                       const std::wstring& root = {},const std::string& epoch = {});
  // UI thread, constant time: a newer picture replaces a queued one for the
  // same file. Returns false only when the writer is shutting down.
  bool Submit(const Target& target, std::shared_ptr<const DecodedFrame> frame);
  bool Submit(const Target& target, std::shared_ptr<const DecodedFrame> frame,Ticket ticket);
  // A native remote window owns this target; its replacement retires it.
  Ticket Own(const Target& target);
  // Immediate process-local revoke; disk cleanup is queued, not acknowledged
  // as complete. A stale owner cannot revoke its replacement.
  bool Invalidate(const Target& target,Ticket& ticket);
  // Dashboard observation of an authenticated Host epoch change. A late old
  // epoch cannot cancel the replacement window's newer publication owner.
  bool InvalidateTarget(const Target& target);
  bool Idle() const;
  // (key, desktop) pairs whose file was replaced since the previous call.
  std::vector<Notice> TakeNotices();
  std::vector<std::pair<std::string,std::string>> TakeCompleted();
  // Stops window notifications; queued pictures are still written.
  void Detach();
  // Area-averaged downscale into packed 24bpp BGR rows (stride width*3
  // rounded up to 4), aspect preserved, at most kMaxWidth x kMaxHeight.
  static bool Scale(const DecodedFrame& frame, uint32_t& width, uint32_t& height,
                    uint32_t& stride, std::vector<uint8_t>& bgr);
  // Scale + JPEG (quality 0.8) + temporary file + rename-by-handle replacement.
  static HRESULT Write(const Target& target, const DecodedFrame& frame,
                       IWICImagingFactory* factory, Timing* timing = nullptr,const Ticket* ticket=nullptr);

 private:
  struct Job {
    Target target;
    std::shared_ptr<const DecodedFrame> frame;
    Ticket ticket;
    bool purge=false;
  };
  static constexpr size_t kQueueLimit = 4, kCompletedLimit = 512,kTargetLimit=512;
  void Run() noexcept;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> queue_;
  std::vector<Notice> completed_;
  struct TargetState {std::weak_ptr<ThumbnailGeneration::State> state;std::string epoch;};
  std::map<std::wstring,TargetState> targets_;
  HWND notify_ = nullptr;
  bool stopping_ = false, working_ = false;
  std::thread worker_;
};
}  // namespace multitap
