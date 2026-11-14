#include "flutter_window.h"
#include "stream_size_native.h"
#include "console_browser_target.h"
#include <shellapi.h>

#include <optional>
#include <climits>
#include <limits>
#include <algorithm>
#include <flutter/standard_method_codec.h>
#include <flutter/standard_message_codec.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_result_functions.h>

#include "credential_store.h"
#include "remote_track_configuration.h"
#include <cmath>
#include "local_monitor_layout.h"
#include "product_update.hpp"
#include "resource.h"
#include "window_geometry.hpp"
#include "flutter/generated_plugin_registrant.h"
#include "ui_language.h"

namespace {

bool AuthenticationOwnerValid(const std::string& value) {
  return value.size()==32 && std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}

constexpr UINT kSessionWindowsChanged = WM_APP + 184;
constexpr UINT kSessionWindowClose = WM_APP + 185;
constexpr UINT_PTR kSessionCloseTimer = 19040;
constexpr UINT_PTR kAuthenticationIdleTimer = 19042;
// A streaming window also refreshes its launcher picture about once a minute,
// so a crash or forced exit still leaves a recent one.
constexpr UINT_PTR kThumbnailTimer = 19041;
constexpr UINT kThumbnailInterval = 60000;

using NoticeValue=flutter::EncodableValue;
using NoticeMap=flutter::EncodableMap;
std::optional<int64_t> NoticeNumber(const NoticeMap* map,const char* key) {
  if(!map)return std::nullopt;const auto found=map->find(NoticeValue(key));if(found==map->end())return std::nullopt;
  if(const auto* value=std::get_if<int32_t>(&found->second))return int64_t(*value);
  if(const auto* value=std::get_if<int64_t>(&found->second))return *value;
  return std::nullopt;
}
std::string NoticeText(const NoticeMap* map,const char* key) {
  if(!map)return {};const auto found=map->find(NoticeValue(key));
  const auto* value=found==map->end()?nullptr:std::get_if<std::string>(&found->second);return value?*value:std::string{};
}
std::wstring NoticeWide(const std::string& value) {
  if(value.empty()||value.size()>4096)return {};
  const int size=MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),nullptr,0);
  std::wstring wide(size_t(size>0?size:0),L'\0');
  if(size>0)MultiByteToWideChar(CP_UTF8,0,value.data(),int(value.size()),wide.data(),size);
  return wide;
}
NoticeMap NoticePacket(const multitap::NoticeStamp& state,bool accepted) {
  using V=NoticeValue;
  return {{V("protocol"),V(multitap::kNoticePresentationProtocol)},{V("accepted"),V(accepted)},
    {V("owner"),V(state.identity.owner)},{V("generation"),V(int32_t(state.identity.generation))},
    {V("lease"),V(state.identity.lease)},{V("revision"),V(state.revision)},
    {V("visible"),V(state.visible)},{V("phase"),V(multitap::NoticePhaseName(state.phase))}};
}
NoticeMap NoticeSurfacePacket(const multitap::NoticeSurfaceStamp& state) {
  using V=NoticeValue;
  return {{V("protocol"),V(multitap::kNoticePresentationProtocol)},{V("surfaceRevision"),V(state.revision)},
    {V("inlineAllowed"),V(state.inlineAllowed)},{V("blockedByNative"),V(state.blockedByNative)}};
}
bool NoticePrepared(const NoticeValue* value,const multitap::NoticeStamp& state) {
  const auto* map=value?std::get_if<NoticeMap>(value):nullptr;
  if(!map||NoticeText(map,"owner")!=state.identity.owner||NoticeText(map,"lease")!=state.identity.lease||
      NoticeNumber(map,"generation")!=state.identity.generation||NoticeNumber(map,"revision")!=state.revision)return false;
  const auto found=map->find(NoticeValue("prepared"));
  const auto* prepared=found==map->end()?nullptr:std::get_if<bool>(&found->second);return prepared&&*prepared;
}

bool SessionCloseReason(const std::string& reason) {
  return reason == "logout" || reason == "session_expired" ||
      reason == "update" || reason == "application_close";
}

std::wstring SessionWindowTitle(const std::string& value) {
  if (value.empty() || value.size() > 384 ||
      std::any_of(value.begin(), value.end(), [](unsigned char ch) { return ch < 32 || ch == 127; })) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (count < 1 || count > 128) return {};
  std::wstring title(static_cast<size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), title.data(), count) != count) return {};
  return title;
}

const std::string* CredentialArgument(const flutter::EncodableMap& arguments,
                                      const char* name) {
  const auto value = arguments.find(flutter::EncodableValue(name));
  if (value == arguments.end()) return nullptr;
  return std::get_if<std::string>(&value->second);
}

void HandleCredentials(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const auto* arguments =
      std::get_if<flutter::EncodableMap>(call.arguments());
  if (!arguments) {
    result->Error("invalid_arguments", "A broker origin is required.");
    return;
  }
  const auto* origin = CredentialArgument(*arguments, "origin");
  if (!origin || !multitap::ClientTransportSyntaxValid(*origin)) {
    result->Error("invalid_arguments", "A valid broker origin is required.");
    return;
  }
  DWORD error = ERROR_SUCCESS;
  if (call.method_name() == "load") {
    std::optional<multitap::SavedCredential> credential;
    if (!multitap::ReadCredential(*origin,&credential,&error)) {
      result->Error("credential_read_failed", "Windows credential storage is unavailable.");
      return;
    }
    if (!credential) {
      result->Success();
      return;
    }
    result->Success(flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("username"),
         flutter::EncodableValue(credential->username)},
        {flutter::EncodableValue("password"),
         flutter::EncodableValue(credential->password)}}));
    SecureZeroMemory(credential->password.data(), credential->password.size());
    return;
  }
  if (call.method_name() == "save") {
    const auto* username = CredentialArgument(*arguments, "username");
    const auto* password = CredentialArgument(*arguments, "password");
    if (!username || !password ||
        !multitap::SaveCredential(*origin, *username, *password, &error)) {
      result->Error("credential_write_failed", "Windows could not remember this password.");
      return;
    }
    result->Success();
    return;
  }
  if (call.method_name() == "delete") {
    if (!multitap::DeleteCredential(*origin, &error)) {
      result->Error("credential_delete_failed", "Windows could not remove the remembered password.");
      return;
    }
    result->Success();
    return;
  }
  result->NotImplemented();
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project,
                             bool allow_session_windows)
    : allow_session_windows_(allow_session_windows), project_(project) {}

FlutterWindow::FlutterWindow(const flutter::DartProject& project,
                             FlutterWindow* owner, std::string key,
                             flutter::EncodableValue payload)
    : allow_session_windows_(false), session_owner_(owner),
      session_key_(std::move(key)), session_payload_(std::move(payload)),
      project_(project) {
  // The native caption is protected even before the first Dart frame/handler.
  close_guard_.Update(false, true);
  // The account key is a hash.
  const auto* launch = std::get_if<flutter::EncodableMap>(&session_payload_);
  const auto* account = launch ? CredentialArgument(*launch, "thumbnailKey") : nullptr;
  const auto* desktop = launch ? CredentialArgument(*launch, "desktopId") : nullptr;
  const auto* epoch=launch?CredentialArgument(*launch,"thumbnailEpoch"):nullptr;
  if (owner && owner->thumbnails_ && account && desktop && epoch && epoch->size()==32) {
    thumbnail_target_ = multitap::ThumbnailWriter::Resolve(*account, *desktop,{},*epoch);
    if (thumbnail_target_) thumbnails_ = owner->thumbnails_;
  }
}

FlutterWindow::~FlutterWindow() {
  Destroy();
  for (auto& child : session_windows_) {
    child->session_owner_ = nullptr;
    child->Destroy();
  }
  Win32Window::Destroy();
}

multitap::relaunch::Product FlutterWindow::UpdateProduct() const {
  return host_settings_ ? multitap::relaunch::Product::HostSettings : multitap::relaunch::Product::Client;
}

std::string FlutterWindow::RunningUpdateVersion() const {
  if (host_settings_) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (!length || length >= 32768) return {};
    return multitap::relaunch::InstalledHostVersion(std::filesystem::path(path));
  }
  // FLUTTER_VERSION is "x.y.z+build", the value of this EXE's version resource.
  std::string version = FLUTTER_VERSION;
  const auto plus = version.find('+');
  if (plus != std::string::npos) version.resize(plus);
  return multitap::relaunch::ValidVersion(version) ? version : std::string();
}

void FlutterWindow::RestoreUpdatePlacement() {
  const multitap::relaunch::Store store;
  const auto record = store.Read(UpdateProduct());
  const auto now = multitap::relaunch::NowFileTime();
  if (!record || !multitap::relaunch::PlacementUsable(*record, now) ||
      !multitap::relaunch::PlacementSane(record->placement))
    return;
  // Only the version the installer itself relaunched: the new one after
  // success, the previous one after a failure.
  const auto decision = multitap::relaunch::Decide(*record, RunningUpdateVersion(), now);
  if (decision != multitap::relaunch::Decision::Updated && decision != multitap::relaunch::Decision::Failed) return;
  ApplyUpdatePlacement(record->placement, multitap::relaunch::ShowCommandFor(record->placement));
}

bool FlutterWindow::ApplyUpdatePlacement(const WINDOWPLACEMENT& placement, int show_command) {
  SetInitialPlacement(placement, show_command);
  // Create() can deliver the one-shot frame callback before main restores the
  // update placement. That restoration hides the HWND while moving it; show
  // the saved placement now if the first frame has already arrived. A pending
  // first frame still owns its normal Show(), and retired authentication wins.
  if (!GetHandle() || !first_frame_completed_ ||
      !(session_owner_ ? session_owner_ : this)->CheckAuthenticationDeadline() ||
      authentication_retired_) return false;
#ifdef MULTITAP_SESSION_WINDOWS_QA
  // Exercise the same guard without exposing the isolated QA window.
  ++qa_update_reveal_requests_;
#else
  Show();
#endif
  return true;
}

bool FlutterWindow::HasSessionWindows() const {
  return (console_webviews_&&console_webviews_->Count()!=0) || session_window_opening_ || std::any_of(session_windows_.begin(), session_windows_.end(),
      [](const auto& child) {
        return child->GetHandle() != nullptr || child->flutter_controller_ || child->session_teardown_in_progress_;
      });
}

bool FlutterWindow::SessionKeyClosing(const std::string& key) const {
  return std::any_of(session_windows_.begin(), session_windows_.end(), [&](const auto& child) {
    return child->session_key_ == key && (child->session_teardown_in_progress_ ||
        (!child->GetHandle() && child->flutter_controller_));
  });
}

void FlutterWindow::CollectClosedSessionWindows() {
  session_windows_.erase(std::remove_if(session_windows_.begin(), session_windows_.end(),
      [](const auto& child) {
        return child->session_destroyed_ && !child->GetHandle() &&
            !child->flutter_controller_ && !child->session_teardown_in_progress_;
      }), session_windows_.end());
}

FlutterWindow* FlutterWindow::visibility_owner_ = nullptr;

void FlutterWindow::UpdateSessionVisibility() {
  if (session_owner_ || updating_visibility_ || session_windows_.empty()) return;
  updating_visibility_ = true;
  std::vector<HWND> edge_to_edge;
  if (GetHandle() && IsZoomed(GetHandle())) edge_to_edge.push_back(GetHandle());
  for (const auto& child : session_windows_) {
    const HWND window = child->GetHandle();
    if (window && (child->presentation_.fullscreen() || IsZoomed(window))) edge_to_edge.push_back(window);
  }
  for (const auto& child : session_windows_) {
    if (!child->GetHandle() || !child->remote_media_ || child->session_teardown_in_progress_) continue;
    const bool hidden = multitap::SessionWindowHidden(child->GetHandle(), edge_to_edge);
    if (hidden == child->media_hidden_) continue;
    child->media_hidden_ = hidden;
    child->remote_media_->SetHidden(hidden);
  }
  updating_visibility_ = false;
}

// Another virtual desktop cloaks a window without moving it.
void CALLBACK FlutterWindow::CloakChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD) {
  DWORD process = 0;
  if (object != OBJID_WINDOW || child != CHILDID_SELF || !visibility_owner_ || !window ||
      !GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId()) return;
  visibility_owner_->UpdateSessionVisibility();
}

void FlutterWindow::DeliverSessionClose() {
  if (session_owner_ && session_ready_ && !session_close_reason_.empty() && sessions_channel_)
    sessions_channel_->InvokeMethod("closeRequested", std::make_unique<flutter::EncodableValue>(
        flutter::EncodableMap{{flutter::EncodableValue("reason"), flutter::EncodableValue(session_close_reason_)}}));
}

void FlutterWindow::RequestSessionWindowsClose(const std::string& reason, bool close_parent) {
  sessions_closing_ = true;
  sessions_close_reason_ = reason;
  close_parent_when_sessions_closed_ = close_parent;
  sessions_close_deadline_ = GetTickCount64() + 15000;
  if(console_webviews_)console_webviews_->CloseAll();
  // Retain C++ child objects until the outer message pump has unwound. Sending
  // the request only posts work; no child window is synchronously destroyed.
  for (const auto& child : session_windows_) {
    if (!child->GetHandle()) continue;
    child->session_close_reason_ = reason;
    PostMessageW(child->GetHandle(), kSessionWindowClose, 0, 0);
  }
  SetTimer(GetHandle(), kSessionCloseTimer, 100, nullptr);
  PostMessageW(GetHandle(), kSessionWindowsChanged, 0, 0);
}

void FlutterWindow::CheckSessionWindowsClosed() {
  if (!sessions_closing_) return;
  // Closed windows hand their last frame to the writer; logout deletes the
  // account's pictures only after this resolves, so wait for those files too.
  if (!HasSessionWindows() && (!thumbnails_ || thumbnails_->Idle())) {
    KillTimer(GetHandle(), kSessionCloseTimer);
    sessions_closing_ = false;
    sessions_close_reason_.clear();
    const bool close_parent = close_parent_when_sessions_closed_;
    close_parent_when_sessions_closed_ = false;
    auto result = std::move(sessions_close_result_);
    if (result) result->Success(flutter::EncodableValue(true));
    if (close_for_update_) {
      // The update asked this app to close: every remote window has finished its cleanup.
      close_for_update_ = false;
      if (!session_owner_ && update_worker_ && update_worker_->Busy()) { ShowWindow(GetHandle(), SW_HIDE); DestroyWindow(GetHandle()); return; }
    }
    if (close_parent && close_guard_.RequestClose()) PostMessageW(GetHandle(), WM_CLOSE, 0, 0);
  } else if (sessions_close_deadline_ && GetTickCount64() >= sessions_close_deadline_) {
    sessions_close_deadline_ = 0;
    const bool report = close_parent_when_sessions_closed_;
    close_parent_when_sessions_closed_ = false;
    auto result = std::move(sessions_close_result_);
    if (result) result->Error("session_close_pending", "Remote session cleanup is still pending.");
    // Keep observing and block new opens. Do not tear down a child whose host
    // cleanup has not completed just to make the dashboard close faster.
    if (report) MessageBoxW(GetHandle(),
        multitap::Tr(L"아직 연결을 끊고 있어요. 원격 창이 닫힌 뒤 다시 종료하세요.", L"Still disconnecting. Quit again after the remote windows close."),
        L"Multitap", MB_OK | MB_ICONINFORMATION);
  }
}

void FlutterWindow::ConfigureSessionWindows() {
  using V = flutter::EncodableValue;
  using M = flutter::EncodableMap;
  if (allow_session_windows_ && !session_owner_ && !visibility_owner_) {
    visibility_owner_ = this;
    // Out of context: delivered through this thread's message queue.
    cloak_hook_ = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr, CloakChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
  }
  sessions_channel_ = std::make_unique<flutter::MethodChannel<V>>(
      flutter_controller_->engine()->messenger(), "multitap/session-windows",
      &flutter::StandardMethodCodec::GetInstance());
  sessions_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (call.method_name() == "bootstrap") {
      if (!session_owner_) { result->Success(); return; }
      if (session_bootstrapped_) { result->Error("already_bootstrapped", "Session bootstrap was already consumed."); return; }
      session_bootstrapped_ = true;
      result->Success(session_payload_);
      session_payload_ = V();
      return;
    }
    if (call.method_name() == "ready") {
      if (!session_owner_ || !session_bootstrapped_) { result->Error("not_session_window", "A native session window is required."); return; }
      session_ready_ = true;
      result->Success();
      if (!session_close_reason_.empty()) PostMessageW(GetHandle(), kSessionWindowClose, 0, 0);
      return;
    }
    if (call.method_name() == "log") {
      const auto* args = std::get_if<M>(call.arguments());
      if (!session_owner_ || !session_owner_->GetHandle() || !session_owner_->sessions_channel_) {
        result->Error("not_session_window", "A live session owner is required."); return;
      }
      if (!args) { result->Error("invalid_arguments", "A bounded log event is required."); return; }
      const auto encoded = flutter::StandardMessageCodec::GetInstance().EncodeMessage(V(*args));
      if (!encoded || encoded->size() > 4096) { result->Error("invalid_arguments", "Log event exceeds its limit."); return; }
      // Only the dashboard writes product logs. Dart validates the fixed event
      // and boolean/numeric fields before recording; this transport never logs
      // the launch payload, broker bearer, or arbitrary exception strings.
      session_owner_->sessions_channel_->InvokeMethod("log", std::make_unique<V>(*args));
      result->Success();
      return;
    }
    if (call.method_name() == "openConsoleInLauncher") {
      // A session window cannot open the Console WebView itself (it belongs to the
      // launcher and its sign-in); it asks the launcher, which opens it for this VM
      // after its own checks (a signed out or locked VM).
      const auto* args = std::get_if<M>(call.arguments());
      if (!session_owner_ || !session_owner_->GetHandle() || !session_owner_->sessions_channel_ ||
          session_owner_->authentication_retired_) {
        result->Error("not_session_window", "A live session owner is required."); return;
      }
      const auto* desktop = args ? CredentialArgument(*args, "desktopId") : nullptr;
      if (!desktop || desktop->empty() || desktop->size() > 128 ||
          !std::all_of(desktop->begin(), desktop->end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
          })) {
        result->Error("invalid_arguments", "A desktop id is required."); return;
      }
      session_owner_->sessions_channel_->InvokeMethod("consoleRequested", std::make_unique<V>(M{{V("desktopId"), V(*desktop)}}));
      result->Success(V(true));
      return;
    }
    if (call.method_name()=="humanActivity" || call.method_name()=="authenticationEnded") {
      const auto* args=std::get_if<M>(call.arguments());
      const auto* owner=args?CredentialArgument(*args,"owner"):nullptr;
      if(!args || args->size()!=(call.method_name()=="humanActivity"?1u:2u)) {result->Error("invalid_arguments","Exact authentication event required.");return;}
      if(!session_owner_ || !owner || *owner!=authentication_owner_ || authentication_retired_ ||
          !AuthenticationOwnerValid(*owner) || session_owner_->authentication_owner_!=*owner ||
          session_owner_->authentication_retired_) {result->Error("authentication_owner_changed","Authentication owner retired.");return;}
      if(call.method_name()=="humanActivity") {AuthenticationHuman();result->Success();return;}
      const auto* reason=CredentialArgument(*args,"reason");
      if(!reason || (*reason!="pc_login_replaced"&&*reason!="session_idle_timeout"&&*reason!="session_reauthentication_required")) {result->Error("invalid_arguments","Fixed authentication reason required.");return;}
      // Hide every video synchronously, before any Dart cleanup future runs.
      session_owner_->HideAuthentication();
      session_owner_->sessions_channel_->InvokeMethod("authenticationEvent",std::make_unique<V>(M{
        {V("owner"),V(*owner)},{V("kind"),V("terminal")},{V("reason"),V(*reason)}}));
      result->Success();return;
    }
    if (session_owner_ || !allow_session_windows_) {
      result->Error("not_dashboard", "Only the client dashboard manages session windows."); return;
    }
#ifdef MULTITAP_SESSION_WINDOWS_QA
    // Compiled solely into the isolated no-network QA executable.
    if (call.method_name() == "qaSnapshot") {
      int32_t windows = 0, engines = flutter_controller_ ? 1 : 0;
      int32_t retired = 0, frames = 0, show_eligible = 0;
      for (const auto& child : session_windows_) {
        if (child->GetHandle()) ++windows;
        if (child->flutter_controller_) ++engines;
        if (!child->GetHandle()) continue;
        if (child->authentication_retired_) ++retired;
        if (child->first_frame_completed_) ++frames;
        if (child->first_frame_completed_ && !child->authentication_retired_ &&
            !authentication_retired_ && child->authentication_owner_ == authentication_owner_)
          ++show_eligible;
      }
      result->Success(V(M{{V("childWindows"), V(windows)}, {V("engineCount"), V(engines)},
          {V("processId"), V(static_cast<int64_t>(GetCurrentProcessId()))},
          {V("dashboardAlive"), V(GetHandle() && IsWindow(GetHandle()))},
          {V("closing"), V(sessions_closing_)}, {V("opening"), V(session_window_opening_)},
          {V("teardownChecks"), V(qa_teardown_checks_)},
          {V("teardownDrainChecks"), V(qa_teardown_drain_checks_)},
          {V("teardownFailures"), V(qa_teardown_failures_)},
          {V("parentFirstFrameCompleted"), V(first_frame_completed_)},
          {V("updateRevealRequests"), V(qa_update_reveal_requests_)},
          {V("childAuthenticationRetired"), V(retired)},
          {V("childFirstFrames"), V(frames)},
          {V("childShowEligible"), V(show_eligible)}}));
      return;
    }
    if (call.method_name() == "qaRestoreUpdatePlacement") {
      WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
      if (!GetHandle() || !GetWindowPlacement(GetHandle(), &placement)) {
        result->Success(V(false)); return;
      }
      result->Success(V(ApplyUpdatePlacement(placement, SW_SHOWNORMAL)));
      return;
    }
#endif
    const auto* args = std::get_if<M>(call.arguments());
    if (!args) { result->Error("invalid_arguments", "Session window arguments are required."); return; }
    if(call.method_name()=="bindAuthentication") {
      const auto* owner=CredentialArgument(*args,"owner");
      const auto remaining=NoticeNumber(args,"idleRemainingMs");
      const auto remainingField=args->find(V("idleRemainingMs"));
      const bool disabled=remainingField==args->end() || std::holds_alternative<std::monostate>(remainingField->second);
      if(args->size()>(remainingField==args->end()?1u:2u) || (!disabled&&(!remaining||*remaining<0||*remaining>86400000)) || !owner || !AuthenticationOwnerValid(*owner) || session_window_opening_ || sessions_closing_ ||
          (OpenSessionWindowCount()>0 && authentication_owner_!=*owner) ||
          (console_webviews_ && console_webviews_->Count()>0 && authentication_owner_!=*owner)) {
        result->Error("authentication_owner_changed","Previous authentication windows must close.");return;}
      authentication_owner_=*owner;authentication_retired_=false;authentication_activity_tick_=0;
      if(!SetAuthenticationDeadline(disabled?std::nullopt:remaining)){result->Error("authentication_clock_unverified","Authentication clock unavailable.");return;}
      result->Success();return;
    }
    if(call.method_name()=="authenticationDeadline" || call.method_name()=="hideAuthentication" || call.method_name()=="retireAuthentication") {
      const auto* owner=CredentialArgument(*args,"owner");
      if(!owner || !AuthenticationOwnerValid(*owner) || *owner!=authentication_owner_) {result->Error("authentication_owner_changed","Authentication owner changed.");return;}
      if(call.method_name()=="authenticationDeadline") {
        if(!CheckAuthenticationDeadline()){result->Error("authentication_owner_changed","Authentication owner expired.");return;}
        const auto found=args->find(V("sessionAccess"));
        const auto* policy=found==args->end()?nullptr:std::get_if<M>(&found->second);
        const auto elapsed=NoticeNumber(args,"elapsedMs");
        const auto remaining=NoticeNumber(args,"remainingMs");
        const auto remainingField=args->find(V("remainingMs"));
        const auto enabledField=policy?policy->find(V("idleEnabled")):M::const_iterator{};
        const bool* enabled=policy&&enabledField!=policy->end()?std::get_if<bool>(&enabledField->second):nullptr;
        const bool disabled=remainingField!=args->end()&&std::holds_alternative<std::monostate>(remainingField->second);
        if(authentication_retired_ || args->size()!=4 || !policy || policy->size()!=7 || NoticeText(policy,"protocol")!="session-access-v1" || !enabled ||
            (*enabled?(!remaining||*remaining<0||*remaining>86400000):!disabled) ||
            !elapsed || *elapsed<0 || *elapsed>86400000) {result->Error("invalid_arguments","Bounded session policy required.");return;}
        if(!SetAuthenticationDeadline(*enabled?remaining:std::nullopt)){result->Error("authentication_clock_unverified","Authentication clock unavailable.");return;}
        for(const auto& child:session_windows_) if(child->authentication_owner_==*owner && !child->authentication_retired_ && child->sessions_channel_)
          child->sessions_channel_->InvokeMethod("authenticationEvent",std::make_unique<V>(M{{V("owner"),V(*owner)},{V("kind"),V("deadline")},{V("sessionAccess"),V(*policy)},{V("elapsedMs"),V(*elapsed)}}));
        result->Success();return;
      }
      if(args->size()!=1) {result->Error("invalid_arguments","Exact authentication owner required.");return;}
      HideAuthentication();
      if(call.method_name()=="hideAuthentication") {result->Success();return;}
      if(sessions_close_result_) {result->Error("session_close_busy","Session cleanup is already pending.");return;}
      sessions_close_result_=std::move(result);RequestSessionWindowsClose("session_expired",false);return;
    }
    if(call.method_name()=="thumbnailInvalidate"){
      const auto* key=CredentialArgument(*args,"key");const auto* desktop=CredentialArgument(*args,"desktopId");const auto* epoch=CredentialArgument(*args,"epoch");
      if(!key||!desktop||!epoch||epoch->size()!=32){result->Error("invalid_arguments","Thumbnail identity required.");return;}
      const auto target=multitap::ThumbnailWriter::Resolve(*key,*desktop,{},*epoch);
      if(!target){result->Success(V(false));return;}
      result->Success(V(!thumbnails_||thumbnails_->InvalidateTarget(*target)));return;
    }
#ifdef MULTITAP_SESSION_WINDOWS_QA
    if (call.method_name() == "qaRequestClose") {
      const auto* key = CredentialArgument(*args, "key");
      for (const auto& child : session_windows_) {
        if (key && child->session_key_ == *key && child->GetHandle()) {
          result->Success(V(PostMessageW(child->GetHandle(), WM_CLOSE, 0, 0) != FALSE)); return;
        }
      }
      result->Success(V(false)); return;
    }
#endif
    if (call.method_name() == "closeAll") {
      const auto* reason = CredentialArgument(*args, "reason");
      if (!reason || !SessionCloseReason(*reason)) { result->Error("invalid_arguments", "A supported close reason is required."); return; }
      if (sessions_close_result_) { result->Error("session_close_busy", "Session cleanup is already pending."); return; }
      sessions_close_result_ = std::move(result);
      RequestSessionWindowsClose(*reason, false);
      return;
    }
    if (call.method_name() != "open") { result->NotImplemented(); return; }
    if (sessions_closing_ || session_window_opening_ || (update_worker_ && update_worker_->Busy())) {
      result->Error("session_window_busy", "A session window or product update is being prepared."); return;
    }
    const auto* key = CredentialArgument(*args, "key");
    const auto* title = CredentialArgument(*args, "title");
    const auto payload = args->find(V("payload"));
    const auto* launch_payload = payload == args->end() ? nullptr : std::get_if<M>(&payload->second);
    const auto wide_title = title ? SessionWindowTitle(*title) : std::wstring();
    if (!key || key->empty() || key->size() > 512 || key->find('\0') != std::string::npos ||
        wide_title.empty() || !launch_payload) {
      result->Error("invalid_arguments", "A bounded key, title and session payload are required."); return;
    }
    const auto* launch_origin=CredentialArgument(*launch_payload,"origin");
    const auto* launch_owner=CredentialArgument(*launch_payload,"authenticationOwner");
    if(!authentication_owner_.empty() && (authentication_retired_ || !launch_owner || *launch_owner!=authentication_owner_)) {
      result->Error("authentication_owner_changed","Authentication owner changed.");return;
    }
    const auto* launch_consent=CredentialArgument(*launch_payload,"httpConsentOrigin");
    if (!launch_origin || !multitap::ClientLaunchTransportAllowed(*launch_origin,
        launch_consent ? *launch_consent : std::string(),
        launch_origin->rfind("http://",0)==0 ? multitap::ReadClientTransportSettings() : std::nullopt)) {
      result->Error("invalid_arguments", "The remote window needs the saved broker transport consent."); return;
    }
    if (SessionKeyClosing(*key)) {
      result->Error("session_window_busy", "The previous session window is still closing."); return;
    }
    for (const auto& child : session_windows_) {
      if (child->session_key_ != *key) continue;
      if (!child->GetHandle()) continue;
      if (child->authentication_retired_ || child->authentication_owner_ != authentication_owner_ ||
          !CheckAuthenticationDeadline()) {
        result->Error("authentication_owner_changed", "Authentication owner retired."); return;
      }
#ifndef MULTITAP_SESSION_WINDOWS_QA
      ShowWindow(child->GetHandle(), IsIconic(child->GetHandle()) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(child->GetHandle());
#endif
      result->Success(V(M{{V("opened"), V(false)}})); return;
    }
    const auto encoded = flutter::StandardMessageCodec::GetInstance().EncodeMessage(payload->second);
    if (!encoded || encoded->size() > 128 * 1024) { result->Error("invalid_arguments", "Session payload exceeds its limit."); return; }
    const auto count = std::count_if(session_windows_.begin(), session_windows_.end(),
        [](const auto& child) { return child->GetHandle() != nullptr; });
    if (count >= 8) { result->Error("session_window_limit", "Close a session window before opening another."); return; }
    // Engine creation may pump messages. A global close must continue waiting
    // until this pending child is either owned by the registry or rolled back.
    struct Opening { bool& flag; explicit Opening(bool& value) : flag(value) { flag = true; } ~Opening() { flag = false; } };
    try {
      Opening opening(session_window_opening_);
      auto project = project_;
      project.set_dart_entrypoint_arguments({"--remote-session-window"});
      auto child = std::unique_ptr<FlutterWindow>(new FlutterWindow(project, this, *key, payload->second));
      child->authentication_owner_=authentication_owner_;
      child->SetQuitOnClose(false);
      child->SetMinimumSize(Size(640, 420));
      if (!child->Create(wide_title, Point(90, 70), Size(1280, 800)) || !child->GetHandle()) {
        result->Error("session_window_failed", "The remote session window could not be created."); return;
      }
      // Open over the launcher (on its monitor), not at a fixed screen corner.
      // The child is still hidden; Flutter shows it after its first frame.
      if (!IsIconic(GetHandle())) {
        RECT owner{}, frame{}; MONITORINFO monitor{sizeof(monitor)};
        if (GetWindowRect(GetHandle(), &owner) && GetWindowRect(child->GetHandle(), &frame) &&
            GetMonitorInfoW(MonitorFromWindow(GetHandle(), MONITOR_DEFAULTTONEAREST), &monitor)) {
          if (const auto centered = multitap::centeredWindowFrame(owner, monitor.rcWork, frame.right - frame.left, frame.bottom - frame.top))
            SetWindowPos(child->GetHandle(), nullptr, centered->left, centered->top, centered->right - centered->left,
                centered->bottom - centered->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
      if (sessions_closing_) {
        child->session_close_reason_ = sessions_close_reason_;
        PostMessageW(child->GetHandle(), kSessionWindowClose, 0, 0);
      }
      session_windows_.push_back(std::move(child));
      UpdateTrayTooltip();
      result->Success(V(M{{V("opened"), V(true)}}));
    } catch (...) {
      result->Error("session_window_failed", "The remote session window could not be created.");
    }
  });
}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  if (allow_session_windows_ && !session_owner_)
    thumbnails_ = std::make_shared<multitap::ThumbnailWriter>(GetHandle());
  ConfigureSessionWindows();
  ConfigureTray();
  credential_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "multitap/credentials",
          &flutter::StandardMethodCodec::GetInstance());
  credential_channel_->SetMethodCallHandler(HandleCredentials);
  if (session_owner_) credential_channel_->SetMethodCallHandler([](const auto&, auto result) {
    result->Error("not_dashboard", "Saved credentials are available only to the dashboard.");
  });
  clipboard_worker_ = std::make_unique<multitap::ClientClipboardChannel>(GetHandle());
  clipboard_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/clipboard",
      &flutter::StandardMethodCodec::GetInstance());
  clipboard_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if(call.method_name()=="watch") {
      using V=flutter::EncodableValue;using M=flutter::EncodableMap;
      const auto* args=std::get_if<M>(call.arguments());
      if(!args){result->Error("invalid_arguments","Clipboard watch arguments required.");return;}
      const auto owner=args->find(V("owner")),enabled=args->find(V("enabled"));
      const auto* ownerValue=owner==args->end()?nullptr:std::get_if<std::string>(&owner->second);
      const auto* enabledValue=enabled==args->end()?nullptr:std::get_if<bool>(&enabled->second);
      if(!ownerValue||!enabledValue){result->Error("invalid_arguments","Clipboard watch owner and state required.");return;}
      try {const auto sequence=clipboard_worker_->Watch(*ownerValue,*enabledValue);
        M reply{{V("sequence"),V(int64_t(sequence))}};if(*enabledValue)reply[V("own")]=V(clipboard_worker_->WatchFoundOwn());
        result->Success(V(std::move(reply)));}
      catch(...){result->Error("clipboard_not_active","Clipboard observation requires the foreground session.");}
      return;
    }
    clipboard_worker_->Enqueue(call, std::move(result));
  });
  drive_worker_ = std::make_unique<multitap::ClientDriveChannel>(GetHandle());
  drive_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/drive",
      &flutter::StandardMethodCodec::GetInstance());
  drive_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (drive_worker_) drive_worker_->Handle(call, std::move(result));
    else result->Error("drive_unavailable", "The window is closing.");
  });
  // usb-v1: capture of this PC's USB devices (UsbDk), only used by a session.
  usb_worker_ = std::make_unique<multitap::UsbCaptureChannel>(GetHandle());
  usb_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/usb",
      &flutter::StandardMethodCodec::GetInstance());
  usb_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (usb_worker_) usb_worker_->Enqueue(call, std::move(result));
    else result->Error("usb_capture_failed", "USB capture is not available.");
  });
  usb_worker_->SetGone([this](int64_t handle) {
    if (usb_channel_) usb_channel_->InvokeMethod("gone", std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
        {flutter::EncodableValue("handle"), flutter::EncodableValue(handle)}}));
  });
  usb_worker_->SetChanged([this] {
    if (usb_channel_) usb_channel_->InvokeMethod("devices_changed", std::make_unique<flutter::EncodableValue>());
  });
  usb_worker_->SetHandOver([this](const std::string& id, const std::string& window) {
    if (usb_channel_) usb_channel_->InvokeMethod("hand_over", std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
        {flutter::EncodableValue("id"), flutter::EncodableValue(id)}, {flutter::EncodableValue("window"), flutter::EncodableValue(window)}}));
  });
  clipboard_worker_->SetChanged([this](const std::string& owner,DWORD sequence) {
    if(clipboard_channel_)clipboard_channel_->InvokeMethod("changed",std::make_unique<flutter::EncodableValue>(flutter::EncodableMap{
      {flutter::EncodableValue("owner"),flutter::EncodableValue(owner)},
      {flutter::EncodableValue("sequence"),flutter::EncodableValue(int64_t(sequence))}}));
  });
  // Paste-pull: an Explorer paste of the peer's offered files (virtual files on this PC's clipboard) asks the session for bytes. The
  // clipboard thread posts pull_message; the events are handed to Dart here, on the platform thread.
  clipboard_worker_->SetPullHandler([this](const multitap::PullEvent& event) {
    if(!clipboard_channel_)return;
    using V=flutter::EncodableValue;
    clipboard_channel_->InvokeMethod("pull",std::make_unique<V>(flutter::EncodableMap{
      {V("kind"),V(event.kind)},{V("rangeId"),V(int64_t(event.rangeId))},{V("offerId"),V(event.offerId)},{V("reason"),V(event.reason)},
      {V("index"),V(int64_t(event.index))},{V("offset"),V(int64_t(event.offset))},{V("end"),V(int64_t(event.end))}}));
  });
  // The transfer card (a small always-on-top window while this window is in the background) and this window's
  // taskbar progress. Updates use an explicit per-transfer lease, never an implicit claim.
  transfer_notice_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/transfer-notice",
      &flutter::StandardMethodCodec::GetInstance());
  transfer_notice_alive_=std::make_shared<std::atomic_bool>(true);
  transfer_notice_observer_=multitap::TransferNotice::Shared().Observe(GetHandle(),
    [this,weak=std::weak_ptr<std::atomic_bool>(transfer_notice_alive_)](const multitap::NoticeSurfaceStamp& proof){
      const auto alive=weak.lock();if(!alive||!alive->load()||!transfer_notice_channel_)return;
      transfer_notice_channel_->InvokeMethod("surface",std::make_unique<NoticeValue>(NoticeSurfacePacket(proof)));
    });
  transfer_notice_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    using V=flutter::EncodableValue;using M=flutter::EncodableMap;
    const auto* args=std::get_if<M>(call.arguments());
    const auto text=[&](const char* key)->std::wstring{
      if(!args)return {};const auto found=args->find(V(key));
      const auto* value=found==args->end()?nullptr:std::get_if<std::string>(&found->second);
      if(!value||value->empty())return {};
      const int size=MultiByteToWideChar(CP_UTF8,0,value->data(),int(value->size()),nullptr,0);
      std::wstring wide(size_t(size>0?size:0),L'\0');
      if(size>0)MultiByteToWideChar(CP_UTF8,0,value->data(),int(value->size()),wide.data(),size);
      return wide;
    };
    const auto number=[&](const char* key,int fallback)->int{
      if(!args)return fallback;const auto found=args->find(V(key));
      if(found==args->end())return fallback;
      if(const auto* i=std::get_if<int32_t>(&found->second))return int(*i);
      if(const auto* l=std::get_if<int64_t>(&found->second))return int(*l);
      return fallback;
    };
    const auto flag=[&](const char* key,bool fallback)->bool{
      if(!args)return fallback;const auto found=args->find(V(key));
      const auto* value=found==args->end()?nullptr:std::get_if<bool>(&found->second);
      return value?*value:fallback;
    };
    const auto percent=[&]()->std::optional<int64_t>{
      if(!args)return std::nullopt;const auto found=args->find(V("percent"));
      if(found==args->end())return std::nullopt;
      if(const auto* i=std::get_if<int32_t>(&found->second))return int64_t(*i);
      if(const auto* l=std::get_if<int64_t>(&found->second))return *l;
      return std::nullopt;
    };
    auto& notice=multitap::TransferNotice::Shared();
    const HWND window=GetHandle();
    const auto generation=NoticeNumber(args,"generation");
    multitap::NoticeIdentity identity{reinterpret_cast<std::uintptr_t>(window),NoticeText(args,"owner"),
      NoticeText(args,"lease"),generation&&*generation>=0&&*generation<=INT_MAX?int(*generation):-1};
    const auto respond=[&](const multitap::NoticeReply& reply) {
      // A rejected old request must never receive the newer owner's identity.
      auto state=reply.state;if(!reply.accepted){state={};state.identity=identity;state.revision=1;}
      auto packet=NoticePacket(state,reply.accepted);
      if(call.method_name()=="acquire"){
        for(const auto& field:NoticeSurfacePacket(notice.Surface()))packet[field.first]=field.second;
      }
      result->Success(V(std::move(packet)));
    };
    if(call.method_name()=="surfaceStatus"){result->Success(V(NoticeSurfacePacket(notice.SurfaceStatus())));return;}
    if(call.method_name()=="acquire") {
      if(!notice.Observes(window,transfer_notice_observer_)){respond({});return;}
      const std::weak_ptr<std::atomic_bool> weak=transfer_notice_alive_;
      const auto send=[this,weak](const char* method,const multitap::NoticeStamp& state,bool accepted) {
        const auto alive=weak.lock();if(!alive||!alive->load()||!transfer_notice_channel_)return;
        transfer_notice_channel_->InvokeMethod(method,std::make_unique<V>(NoticePacket(state,accepted)));
      };
      multitap::TransferNoticeCallbacks callbacks;
      callbacks.presentation=[send](const auto& state,bool accepted){send("presentation",state,accepted);};
      callbacks.cancel=[send](const auto& state){send("cancel",state,true);};
      callbacks.close=[send](const auto& state){send("close",state,true);};
      callbacks.open=[send](const auto& state){send("open",state,true);};
      callbacks.prepare=[this,weak](const multitap::NoticeStamp& state,std::function<void(bool)> ready) {
        const auto alive=weak.lock();if(!alive||!alive->load()||!transfer_notice_channel_){ready(false);return;}
        const auto used=std::make_shared<std::atomic_bool>(false);
        const auto finish=[ready=std::move(ready),used](bool prepared){if(used->exchange(true))return;ready(prepared);};
        auto packet=NoticePacket(state,true);packet[V("requiresFrame")]=V(state.requiresFrame);
        transfer_notice_channel_->InvokeMethod("prepare",std::make_unique<V>(std::move(packet)),
          std::make_unique<flutter::MethodResultFunctions<V>>(
            [state,finish](const V* reply){finish(NoticePrepared(reply,state));},
            [finish](const std::string&,const std::string&,const V*){finish(false);},
            [finish]{finish(false);}));
      };
      respond(notice.Acquire(window,identity.owner,identity.generation,std::move(callbacks)));return;
    }
    if(!multitap::NoticeOpaque(identity.owner)||!multitap::NoticeOpaque(identity.lease)||identity.generation<0){respond({});return;}
    if(call.method_name()=="update") {
      multitap::TransferNoticeContent content;
      const int kind=number("kind",0);
      using Kind=multitap::TransferNoticeContent::Kind;
      content.kind=kind==1?Kind::done:kind==2?Kind::failed:kind==3?Kind::cancelled:Kind::running;
      content.title=text("title");content.name=text("name");content.detail=text("detail");content.message=text("message");
      content.subtitle=text("subtitle");content.trailing=text("trailing");content.hint=text("hint");
      if(args){
        // 자세히: label, value, label, value... (at most a few rows).
        const auto found=args->find(V("details"));
        const auto* list=found==args->end()?nullptr:std::get_if<flutter::EncodableList>(&found->second);
        if(list)for(size_t i=0;i+1<list->size()&&content.details.size()<6;i+=2){
          const auto* label=std::get_if<std::string>(&(*list)[i]);const auto* value=std::get_if<std::string>(&(*list)[i+1]);
          if(label&&value)content.details.push_back({NoticeWide(*label),NoticeWide(*value)});
        }
      }
      content.percent=multitap::DecodeNoticePercent(percent());content.dark=flag("dark",true);
      content.sending=flag("sending",false);content.folder=flag("folder",false);content.openable=flag("openable",false);
      content.cancellable=flag("cancellable",true);content.cancelling=flag("cancelling",false);content.generation=identity.generation;
      respond(notice.Update(identity,content));return;
    }
    if(call.method_name()=="hide"){respond(notice.Hide(identity));return;}
    if(call.method_name()=="taskbar") {
      const auto mode=text("mode");
      respond(notice.SetTaskbar(identity,mode==L"normal"?multitap::TaskbarMode::normal:mode==L"indeterminate"?multitap::TaskbarMode::indeterminate:mode==L"error"?multitap::TaskbarMode::error:multitap::TaskbarMode::none,number("percent",0)));return;
    }
    result->NotImplemented();
  });
  paste_target_worker_ = std::make_unique<multitap::PasteTargetChannel>(GetHandle());
  paste_target_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/paste-target",
      &flutter::StandardMethodCodec::GetInstance());
  paste_target_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (paste_target_worker_) paste_target_worker_->Handle(call, std::move(result));
    else result->Success(flutter::EncodableValue());
  });
  // print-v1: print or save what the VM printed (client_print.h).
  print_worker_ = std::make_unique<multitap::ClientPrintChannel>(GetHandle());
  print_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/print",
      &flutter::StandardMethodCodec::GetInstance());
  print_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (print_worker_) print_worker_->Handle(call, std::move(result));
    else result->Error("print_unavailable", "Printing is unavailable.");
  });
  remote_registrar_ = std::make_unique<flutter::PluginRegistrarWindows>(
      flutter_controller_->engine()->GetRegistrarForPlugin("MultitapRemoteEngine"));
  // The adapter Flutter renders this window on: hardware decoding and GPU
  // presentation (shared textures) use exactly this one, so frames do not cross
  // adapters. A separate remote window on another GPU decides on its own.
  Microsoft::WRL::ComPtr<IDXGIAdapter> render_adapter;
  {
    IDXGIAdapter* adapter = nullptr;
    if (remote_registrar_->GetGraphicsAdapter(&adapter) && adapter) {
      render_environment_ = multitap::InspectRenderAdapter(adapter);
      render_adapter.Attach(adapter);
    }
  }
  remote_media_ = std::make_unique<multitap::RemoteMediaSession>(remote_registrar_->texture_registrar(), multitap::RemoteMediaSession::FrameNotification{}, render_adapter);
  udp_media_ = std::make_unique<multitap::UdpMediaChannel>(flutter_controller_->engine()->messenger(), [this](const multitap::udp::TrackBinding& binding){return remote_media_?remote_media_->TrackPathGate(binding):nullptr;});
  remote_media_->UsePathGate(udp_media_->gate());
  if (thumbnails_ && thumbnail_target_) {
    thumbnail_ticket_=std::make_shared<multitap::ThumbnailWriter::Ticket>(thumbnails_->Own(*thumbnail_target_));
    // Ordinary ends keep their last frame. A privacy revoke retires its exact
    // ticket before this callback; stale/failure frames then cannot publish.
    remote_media_->OnRetiredFrame([writer = thumbnails_, target = *thumbnail_target_,ticket=thumbnail_ticket_](auto frame) {
      writer->Submit(target, std::move(frame),*ticket);
    });
    SetTimer(GetHandle(), kThumbnailTimer, kThumbnailInterval, nullptr);
  }
  remote_input_ = std::make_unique<multitap::RemoteInput>([this](flutter::EncodableMap event) {
    if(NoticeText(&event,"type")=="input" &&
        (NoticeText(&event,"event")=="key" || NoticeText(&event,"event")=="text")) AuthenticationHuman();
    if(authentication_retired_ && NoticeText(&event,"type")=="input")return;
    if(NoticeText(&event,"type")=="capture.released")relative_mouse_.End("focus");
    const auto lease=remote_media_?remote_media_->Lease():0;
    if (remote_sink_ && lease > 0) {
      event[flutter::EncodableValue("lease")] = flutter::EncodableValue(lease);
      remote_sink_->Success(flutter::EncodableValue(std::move(event)));
    }
  });
  // cursor-v1: presents only while keyboard capture is active, over the
  // picture whose Flutter cursor is `none` (cursor_presenter.h).
  cursor_presenter_ = std::make_unique<multitap::CursorPresenter>(
      [this] { return remote_input_ && remote_input_->captured(); });
  cursor_presenter_->Attach(flutter_controller_->view()->GetNativeWindow());
  // cursor-echo-v1: a pinned 게임 마우스 pointer stands on each echoed VM position.
  cursor_presenter_->SetPin([this](POINT screen) { return relative_mouse_.Pin(screen); });
  // cursor-compose-v1: no cursor of ours while the VM's is in the video.
  cursor_presenter_->SetConceal([this] { return relative_mouse_.concealed(); });
  remote_events_ = std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/remote/input",
      &flutter::StandardMethodCodec::GetInstance());
  remote_events_->SetStreamHandler(std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [this](const auto*, auto sink) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
        remote_sink_ = std::move(sink); return nullptr;
      }, [this](const auto*) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
        remote_input_->Capture(nullptr, false); remote_sink_.reset(); return nullptr;
      }));
  remote_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/remote",
      &flutter::StandardMethodCodec::GetInstance());
  stream_size_queries_=std::make_unique<multitap::StreamSizeQueryChannel>(GetHandle());
  remote_channel_->SetMethodCallHandler([this,render_adapter](const auto& call, auto result) {
    using V=flutter::EncodableValue;
    // Compatibility tuple is ABI-only, never a decoder-supported rectangle.
    // stream-size-v2 clients must validate their actual encoded shape(s).
    if (call.method_name() == "decoderLimits") {
      result->Success(V(flutter::EncodableMap{{V("maxWidth"), V(int64_t(multitap::kMaximumVideoWidth))}, {V("maxHeight"), V(int64_t(multitap::kMaximumVideoHeight))}, {V("maxPixels"), V(int64_t(multitap::kMaximumVideoPixels))}, {V("maxPixelRate"), V(0)}, {V("exactValidation"),V("validateStreamSize")},{V("limitsKind"),V("abi-only")}}));
      return;
    }
    const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
    if (!args) { result->Error("invalid_arguments", "Remote engine arguments required."); return; }
    if(call.method_name()=="validateStreamSize"){
      int64_t schema=0;
      const auto* hardwareField=multitap::MediaField(*args,"hardware");const auto* hardware=hardwareField?std::get_if<bool>(hardwareField):nullptr;
      const auto* tracksField=multitap::MediaField(*args,"tracks");const auto* tracks=tracksField?std::get_if<flutter::EncodableList>(tracksField):nullptr;
      if(!multitap::MediaInteger(multitap::MediaField(*args,"schema"),schema)||schema!=1||!hardware||!tracks||tracks->empty()||tracks->size()>2){result->Error("invalid_arguments","Exact stream-size candidate required.");return;}
      std::array<multitap::StreamSize,2> shapes{};
      for(size_t i=0;i<tracks->size();++i){
        const auto* shape=std::get_if<flutter::EncodableMap>(&(*tracks)[i]);int64_t width=0,height=0,fps=0;
        if(!shape||!multitap::MediaInteger(multitap::MediaField(*shape,"width"),width)||!multitap::MediaInteger(multitap::MediaField(*shape,"height"),height)||!multitap::MediaInteger(multitap::MediaField(*shape,"fps"),fps)||width<1||height<1||fps<1||uint64_t(width)>UINT32_MAX||uint64_t(height)>UINT32_MAX||uint64_t(fps)>UINT32_MAX){result->Error("invalid_arguments","Exact stream-size candidate required.");return;}
        shapes[i]={uint32_t(width),uint32_t(height),uint32_t(fps)};
        const auto* sw=multitap::MediaField(*shape,"sourceWidth");const auto* sh=multitap::MediaField(*shape,"sourceHeight");
        if(sw||sh){int64_t sourceWidth=0,sourceHeight=0;if(!sw||!sh||!multitap::MediaInteger(sw,sourceWidth)||!multitap::MediaInteger(sh,sourceHeight)||sourceWidth<1||sourceHeight<1||uint64_t(sourceWidth)>UINT32_MAX||uint64_t(sourceHeight)>UINT32_MAX){result->Error("invalid_arguments","Workspace geometry required.");return;}shapes[i].sourceWidth=uint32_t(sourceWidth);shapes[i].sourceHeight=uint32_t(sourceHeight);}
      }
      if(!stream_size_queries_){result->Error("session_replaced","Decoder query observer retired.");return;}
      multitap::StreamSizeQueryChannel::Request request;request.shapes=shapes;request.count=tracks->size();request.hardware=*hardware;request.render=render_adapter;
      stream_size_queries_->Submit(std::move(request),std::move(result));return;
    }
    // decoder-prestart-v1: a session waiting for its stream info starts its CPU
    // decoder(s) now (RemoteMediaSession::Prepare); not tied to a media lease.
    if(call.method_name()=="prepareDecoder"){
      int64_t width=0,height=0,fps=0,outputs=0;
      if(!multitap::MediaInteger(multitap::MediaField(*args,"width"),width)||!multitap::MediaInteger(multitap::MediaField(*args,"height"),height)||
         !multitap::MediaInteger(multitap::MediaField(*args,"fps"),fps)||!multitap::MediaInteger(multitap::MediaField(*args,"outputs"),outputs)||
         width<1||height<1||uint64_t(width)>UINT32_MAX||uint64_t(height)>UINT32_MAX||!multitap::ValidVideoSize(uint32_t(width),uint32_t(height))||fps<1||fps>240||outputs<1||outputs>2){
        result->Error("invalid_arguments","Expected stream size, frame rate and output count required.");return;
      }
      remote_media_->Prepare(uint32_t(width),uint32_t(height),uint32_t(fps),size_t(outputs));result->Success();return;
    }
    int64_t lease=0;
    if(!multitap::MediaInteger(multitap::MediaField(*args,"lease"),lease)||lease<1){result->Error("invalid_arguments","Native media lease required.");return;}
    const auto streamFps=[&](uint32_t& fps){const auto* field=multitap::MediaField(*args,"fps");if(!field){fps=60;return true;}int64_t value=0;if(!multitap::MediaInteger(field,value)||value<1||uint64_t(value)>UINT32_MAX)return false;fps=uint32_t(value);return true;};
    const auto applyThumbnailStartPrivacy=[&]{
      const auto* field=multitap::MediaField(*args,"thumbnailAllowed");
      const auto* allowed=field?std::get_if<bool>(field):nullptr;
      if(field&&!allowed){result->Error("invalid_arguments","Thumbnail permission must be boolean.");return false;}
      if(!allowed||*allowed)return true;
      thumbnail_privacy_locked_=true;
      // Lock before Start can retire a held frame or start a decoder. The
      // shared generation latch also denies late timer/retired-frame writes
      // and every later Change(ticket,true) in this native window.
      if(thumbnails_&&thumbnail_target_&&thumbnail_ticket_&&
          (!multitap::ThumbnailGeneration::Lock(*thumbnail_ticket_)||
           !thumbnails_->Invalidate(*thumbnail_target_,*thumbnail_ticket_))){
        result->Error("thumbnail_privacy_unknown","Thumbnail publication could not be locked.");return false;
      }
      return true;
    };
    if(call.method_name()=="thumbnailPrivacy"||call.method_name()=="thumbnailPrivacyStatus"||call.method_name()=="thumbnailPrivacyLocked"){
      if(!remote_media_->Current(lease)){result->Error("session_replaced","Native media owner retired.");return;}
      if(call.method_name()=="thumbnailPrivacyLocked"){
        const bool noTarget=!thumbnails_||!thumbnail_target_;
        result->Success(V(thumbnail_privacy_locked_&&(noTarget||(thumbnail_ticket_&&
          multitap::ThumbnailGeneration::Locked(*thumbnail_ticket_)&&multitap::ThumbnailGeneration::DurablyRevoked(*thumbnail_ticket_)))));return;
      }
      if(call.method_name()=="thumbnailPrivacyStatus"){
        int64_t generation=0;if(!multitap::MediaInteger(multitap::MediaField(*args,"generation"),generation)||generation<1){result->Error("invalid_arguments","Thumbnail generation required.");return;}
        result->Success(V(thumbnail_ticket_&&uint64_t(generation)==thumbnail_ticket_->stamp&&multitap::ThumbnailGeneration::DurablyRevoked(*thumbnail_ticket_)));return;
      }
      if(thumbnails_&&thumbnail_target_&&thumbnail_ticket_&&!thumbnails_->Invalidate(*thumbnail_target_,*thumbnail_ticket_)){result->Error("thumbnail_privacy_unknown","Thumbnail revoke could not be acknowledged.");return;}
      result->Success(V(thumbnail_ticket_?static_cast<int64_t>(thumbnail_ticket_->stamp):int64_t(0)));return;
    }
    const auto complete=[&](HRESULT hr){
      if(hr==E_ABORT)result->Error("session_replaced","Native media owner retired.");
      else if(hr==HRESULT_FROM_WIN32(ERROR_BUSY))result->Error("engine_busy","Native media change is in progress.");
      else if(FAILED(hr))result->Error("media_backpressure","Native media operation failed.");
      else result->Success();
    };
    if(call.method_name()=="stop"){if(stream_size_queries_)stream_size_queries_->Retire();complete(remote_media_->Stop(lease,[this]{ResetRemoteInput();}));return;}
    // A resize's held picture (start with holdPrevious) is no longer shown.
    if(call.method_name()=="releaseHeld"){
      int64_t texture=-1;if(!multitap::MediaInteger(multitap::MediaField(*args,"texture"),texture)||texture<0){result->Error("invalid_arguments","Held texture required.");return;}
      result->Success(V(remote_media_->ReleaseHeld(texture)));return;
    }
    // cursor-v1 records and picture geometry of the current media lease only.
    if(call.method_name()=="cursor"||call.method_name()=="cursorView"){
      if(!remote_media_->Current(lease)||!cursor_presenter_){result->Error("session_replaced","Native media owner retired.");return;}
      if(call.method_name()=="cursor"){
        int64_t epoch=0;const auto field=multitap::MediaField(*args,"bytes");const auto bytes=field?std::get_if<std::vector<uint8_t>>(field):nullptr;
        if(!bytes||!multitap::MediaInteger(multitap::MediaField(*args,"epoch"),epoch)||epoch<0||epoch>65535){result->Error("invalid_arguments","Cursor record and epoch required.");return;}
        const HRESULT hr=cursor_presenter_->Apply(lease,uint32_t(epoch),bytes->data(),bytes->size());
        if(FAILED(hr))result->Error("invalid_cursor","Cursor record rejected.");else result->Success();return;
      }
      const auto number=[args](const char* key,double& out){const auto value=multitap::MediaField(*args,key);if(!value)return false;
        if(const auto* d=std::get_if<double>(value)){out=*d;return true;}if(const auto* i=std::get_if<int32_t>(value)){out=*i;return true;}if(const auto* l=std::get_if<int64_t>(value)){out=double(*l);return true;}return false;};
      multitap::CursorPicture picture;int64_t sourceWidth=0,sourceHeight=0;
      if(!number("left",picture.left)||!number("top",picture.top)||!number("width",picture.width)||!number("height",picture.height)||
         !multitap::MediaInteger(multitap::MediaField(*args,"sourceWidth"),sourceWidth)||!multitap::MediaInteger(multitap::MediaField(*args,"sourceHeight"),sourceHeight)||
         sourceWidth<1||uint64_t(sourceWidth)>UINT32_MAX||sourceHeight<1||uint64_t(sourceHeight)>UINT32_MAX){result->Error("invalid_arguments","Picture geometry required.");return;}
      picture.sourceWidth=uint32_t(sourceWidth);picture.sourceHeight=uint32_t(sourceHeight);
      if(FAILED(cursor_presenter_->SetPicture(lease,picture)))result->Error("invalid_arguments","Picture geometry rejected.");else result->Success();return;
    }
    if(call.method_name()=="stats"){
      flutter::EncodableMap stats;const HRESULT hr=remote_media_->Stats(lease,stats);
      if(FAILED(hr)){complete(hr);return;}
      // How this client renders (no adapter names): hardware GPU or WARP/basic
      // render driver, the composition rate Flutter paces frames at, and
      // whether the client itself runs inside a Remote Desktop session.
      // gpuHardware: the hardware H.264 decoder is in use (decodeTier 1 on Flutter's
      // adapter, 2 on another one). renderHardware: Flutter itself renders on a
      // hardware GPU rather than WARP / the basic render driver (what gpuHardware
      // meant before hardware decoding existed).
      {const auto hw=stats.find(V("hardwareDecode"));stats[V("gpuHardware")]=V(hw!=stats.end()&&std::get_if<bool>(&hw->second)&&std::get<bool>(hw->second));}
      if(render_environment_.known)stats[V("renderHardware")]=V(render_environment_.hardware);
      if(const auto hz=multitap::CompositionRefreshHz())stats[V("displayHz")]=V(hz);
      stats[V("remoteSession")]=V(multitap::RemoteDesktopSession());
      result->Success(V(stats));return;
    }
    if(call.method_name()=="push"){
      const auto field=multitap::MediaField(*args,"bytes");const auto bytes=field?std::get_if<std::vector<uint8_t>>(field):nullptr;
      if(!bytes){result->Error("invalid_arguments","Media bytes required.");return;}
      // Optional input-latency-v1 frame watch; absent keeps the released call.
      const auto watchField=multitap::MediaField(*args,"watch");const auto watch=watchField?std::get_if<bool>(watchField):nullptr;
      if(watchField&&!watch){result->Error("invalid_arguments","Media watch flag must be boolean.");return;}
      const auto pathField=multitap::MediaField(*args,"path");const auto path=pathField?std::get_if<bool>(pathField):nullptr;
      const HRESULT hr=remote_media_->Push(lease,*bytes,watch&&*watch,path&&*path);
      // A full decode queue is backpressure: the relay reader waits and offers
      // the same packet again. Only the other outcomes end the stream.
      if(hr==multitap::kMediaQueueFull)result->Error("media_queue_full","Native media queue is full.");
      else if(hr==multitap::kMediaDecoderStopped)result->Error("decoder_failed","Native decoder stopped.");
      else if(hr==multitap::kMediaPacketInvalid)result->Error("invalid_stream_message","Native media packet rejected.");
      else complete(hr);
      return;
    }
    if (call.method_name() == "capture") {
      auto it = args->find(flutter::EncodableValue("enabled"));
      const bool* enabled = it == args->end() ? nullptr : std::get_if<bool>(&it->second);
      if (!enabled) { result->Error("invalid_arguments", "Capture state required."); return; }
      const auto* text_field = multitap::MediaField(*args, "textInput");
      const auto* text_enabled = text_field ? std::get_if<bool>(text_field) : nullptr;
      if (text_field && !text_enabled) { result->Error("invalid_arguments", "Text input flag must be boolean."); return; }
      bool accepted=false;const HRESULT hr=remote_media_->Capture(lease,[&]{
        // Configure only the lease admitted by the media owner. A stale call
        // must never change another session's Unicode capture policy.
        remote_input_->SetTextInputEnabled(*enabled && text_enabled && *text_enabled);
        const bool captured=remote_input_->Capture(flutter_controller_->view()->GetNativeWindow(),*enabled);
        if(!captured)remote_input_->SetTextInputEnabled(false);
        return captured;
      },accepted);
      if(cursor_presenter_)cursor_presenter_->Refresh();
      if(FAILED(hr))complete(hr);else result->Success(V(accepted));return;
    }
    if(call.method_name()=="startTracks"){
      if(stream_size_queries_)stream_size_queries_->Retire();
      multitap::tracks::Configuration configuration;
      if(!multitap::TrackConfiguration(multitap::MediaField(*args,"configuration"),configuration)){result->Error("invalid_stream","Invalid independent output configuration.");return;}
      // The "hardware decoding" setting (default on); false keeps the CPU path.
      const auto hardwareField=multitap::MediaField(*args,"hardwareDecode");const auto hardwareValue=hardwareField?std::get_if<bool>(hardwareField):nullptr;
      if(hardwareField&&!hardwareValue){result->Error("invalid_arguments","Hardware decoding flag must be boolean.");return;}
      uint32_t fps=0;if(!streamFps(fps)){result->Error("invalid_arguments","Frame rate required.");return;}
      // Optional "outputBounds": {"0": [left, top, right, bottom], ...} - the physical
      // desktop rectangle of the native window that will show each output. The
      // output then decodes and converts on the adapter driving that monitor when
      // it has an H.264 decoder (zero-copy swap chain); absent = Flutter's adapter.
      multitap::RemoteMediaSession::OutputAdapters outputAdapters;
      if(const auto* boundsField=multitap::MediaField(*args,"outputBounds")){
        const auto* boundsMap=std::get_if<flutter::EncodableMap>(boundsField);
        if(!boundsMap){result->Error("invalid_arguments","Output bounds must be a map.");return;}
        for(const auto& [key,value]:*boundsMap){
          const auto* id=std::get_if<std::string>(&key);const auto* list=std::get_if<flutter::EncodableList>(&value);
          if(!id||(*id!="0"&&*id!="1")||!list||list->size()!=4){result->Error("invalid_arguments","Output bounds are invalid.");return;}
          int64_t n[4]{};for(size_t i=0;i<4;++i){if(!multitap::MediaInteger(&(*list)[i],n[i])||n[i]<std::numeric_limits<LONG>::min()||n[i]>std::numeric_limits<LONG>::max()){result->Error("invalid_arguments","Output bounds are invalid.");return;}}
          const RECT rect{LONG(n[0]),LONG(n[1]),LONG(n[2]),LONG(n[3])};
          if(rect.right<=rect.left||rect.bottom<=rect.top){result->Error("invalid_arguments","Output bounds are invalid.");return;}
          outputAdapters[(*id)[0]-'0']=multitap::gpu::AdapterForRect(rect);
        }
      }
      std::map<uint8_t,int64_t> textures;
      std::array<multitap::StreamSize,2> shapes{};size_t count=0;
      for(const auto& t:configuration.tracks)shapes[count++]={t.width,t.height,fps,t.sourceWidth,t.sourceHeight};
      multitap::StreamMemory memory{};if(!multitap::StreamMemoryNeeded(shapes.data(),count,memory)){result->Error("stream_size_abi","Invalid stream buffer layout.");return;}
      const auto available=multitap::AvailableVideoSystemMemory();if(!available||memory.systemBytes>available){result->Error(available?"stream_memory_budget":"stream_memory_unknown","Stream memory admission failed.");return;}
      if(!applyThumbnailStartPrivacy())return;
      const HRESULT hr=remote_media_->StartTracks(lease,configuration,textures,[this]{ResetRemoteInput();},!hardwareValue||*hardwareValue,outputAdapters,fps);
      if(remote_media_->LatestAttempt(lease)&&thumbnails_&&thumbnail_target_&&thumbnail_ticket_){if(SUCCEEDED(hr))*thumbnail_ticket_=multitap::ThumbnailGeneration::Change(*thumbnail_ticket_,true);else thumbnails_->Invalidate(*thumbnail_target_,*thumbnail_ticket_);}
      if(FAILED(hr)){complete(hr);return;}
      flutter::EncodableMap value;for(const auto& [id,texture]:textures)value.emplace(V(std::to_string(id)),V(texture));
      result->Success(V(value));return;
    }
    if (call.method_name() == "start") {
      if(stream_size_queries_)stream_size_queries_->Retire();
      auto integer = [args](const char* key) -> int64_t {
        auto it = args->find(flutter::EncodableValue(key)); if (it == args->end()) return 0;
        if (auto* value = std::get_if<int32_t>(&it->second)) return *value;
        if (auto* value = std::get_if<int64_t>(&it->second)) return *value;
        return 0;
      };
      const int64_t width = integer("width"), height = integer("height");
      if (lease < 1 || width<1||height<1||uint64_t(width)>UINT32_MAX||uint64_t(height)>UINT32_MAX||!multitap::ValidVideoSize(uint32_t(width),uint32_t(height))) { result->Error("invalid_stream", "Unsupported remote dimensions."); return; }
      const auto audio = args->find(flutter::EncodableValue("audioEnabled"));
      if (audio != args->end() && !std::get_if<bool>(&audio->second)) { result->Error("invalid_stream", "Audio enablement must be boolean."); return; }
      const bool audio_enabled = audio == args->end() || std::get<bool>(audio->second);
      // audio-opus-v1: stream.info's audio format; absent is the released PCM.
      const auto format = args->find(flutter::EncodableValue("audioFormat"));
      const auto* format_name = format == args->end() ? nullptr : std::get_if<std::string>(&format->second);
      if (format != args->end() && (!format_name || (*format_name != "s16le" && *format_name != "opus"))) { result->Error("invalid_stream", "Unsupported audio format."); return; }
      const bool audio_opus = format_name && *format_name == "opus";
      // stream-size-v1: the Host's picture change within a session keeps the
      // input capture (same source); only cursor shapes start over.
      const auto keep = args->find(flutter::EncodableValue("keepCapture"));
      if (keep != args->end() && !std::get_if<bool>(&keep->second)) { result->Error("invalid_stream", "Capture retention must be boolean."); return; }
      const bool keep_capture = keep != args->end() && std::get<bool>(keep->second);
      // A viewport resize keeps the previous picture on screen until the new
      // decoder's first frame (RemoteMediaSession::Start `hold`).
      const auto held = args->find(flutter::EncodableValue("holdPrevious"));
      if (held != args->end() && !std::get_if<bool>(&held->second)) { result->Error("invalid_stream", "Picture hold must be boolean."); return; }
      const bool hold_previous = held != args->end() && std::get<bool>(held->second);
      const auto hardware = args->find(flutter::EncodableValue("hardwareDecode"));
      if (hardware != args->end() && !std::get_if<bool>(&hardware->second)) { result->Error("invalid_stream", "Hardware decoding flag must be boolean."); return; }
      const bool hardware_decode = hardware == args->end() || std::get<bool>(hardware->second);
      uint32_t fps=0;if(!streamFps(fps)){result->Error("invalid_arguments","Frame rate required.");return;}
      multitap::StreamSize shape{uint32_t(width),uint32_t(height),fps};
      const auto* sw=multitap::MediaField(*args,"sourceWidth");const auto* sh=multitap::MediaField(*args,"sourceHeight");
      if(sw||sh){int64_t sourceWidth=0,sourceHeight=0;if(!sw||!sh||!multitap::MediaInteger(sw,sourceWidth)||!multitap::MediaInteger(sh,sourceHeight)||sourceWidth<1||sourceHeight<1||uint64_t(sourceWidth)>UINT32_MAX||uint64_t(sourceHeight)>UINT32_MAX){result->Error("invalid_arguments","Workspace geometry required.");return;}shape.sourceWidth=uint32_t(sourceWidth);shape.sourceHeight=uint32_t(sourceHeight);}
      multitap::StreamMemory memory{};if(!multitap::StreamMemoryNeeded(&shape,1,memory)){result->Error("stream_size_abi","Invalid stream buffer layout.");return;}
      const auto available=multitap::AvailableVideoSystemMemory();if(!available||memory.systemBytes>available){result->Error(available?"stream_memory_budget":"stream_memory_unknown","Stream memory admission failed.");return;}
      if(!applyThumbnailStartPrivacy())return;
      int64_t texture=-1;const HRESULT hr=remote_media_->Start(lease,static_cast<uint32_t>(width),static_cast<uint32_t>(height),audio_enabled,texture,[this,keep_capture]{
        if(keep_capture){
          // A decoder restart owns a new media lease even when ordinary keys
          // stay captured. Retire partial Unicode before the new lease can
          // stamp an event; its current capture RPC restores the text grant.
          remote_input_->SetTextInputEnabled(false);
          if(cursor_presenter_)cursor_presenter_->Reset();
        }else ResetRemoteInput();
      },hardware_decode,fps,shape.sourceWidth,shape.sourceHeight,hold_previous,audio_opus);
      if(remote_media_->LatestAttempt(lease)&&thumbnails_&&thumbnail_target_&&thumbnail_ticket_){if(SUCCEEDED(hr))*thumbnail_ticket_=multitap::ThumbnailGeneration::Change(*thumbnail_ticket_,true);else thumbnails_->Invalidate(*thumbnail_target_,*thumbnail_ticket_);}
      if(FAILED(hr))complete(hr);else result->Success(flutter::EncodableValue(texture));
      return;
    }
    result->NotImplemented();
  });
  window_events_ = std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/window/events", &flutter::StandardMethodCodec::GetInstance());
  window_events_->SetStreamHandler(std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [this](const auto*, auto sink) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
        window_sink_ = std::move(sink); return nullptr;
      }, [this](const auto*) -> std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>> {
        window_sink_.reset(); return nullptr;
      }));
  close_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/window/close", &flutter::StandardMethodCodec::GetInstance());
  close_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    using V = flutter::EncodableValue;
    if (call.method_name() == "update") {
      const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
      if (!args) { result->Error("invalid_arguments", "Expected close guard state."); return; }
      const auto busy = args->find(V("busy")), dirty = args->find(V("dirty"));
      if (busy == args->end() || dirty == args->end() || !std::get_if<bool>(&busy->second) || !std::get_if<bool>(&dirty->second)) {
        result->Error("invalid_arguments", "Expected boolean close guard state."); return;
      }
      close_guard_.Update(std::get<bool>(busy->second), std::get<bool>(dirty->second));
      result->Success(V(true)); return;
    }
    if (call.method_name() == "close") {
      const bool allowed = close_guard_.RequestClose();
      if (allowed) PostMessageW(GetHandle(), WM_CLOSE, 0, 0);
      result->Success(V(allowed)); return;
    }
    result->NotImplemented();
  });
  window_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/window", &flutter::StandardMethodCodec::GetInstance());
  update_worker_=std::make_unique<multitap::ProductUpdateTask>(GetHandle());
  window_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if(call.method_name()=="monitorLayout") {
      using V=flutter::EncodableValue;
      const auto monitors=multitap::LocalMonitorLayout();
      RECT client{};POINT origin{};
      if(monitors.size()!=2||!GetClientRect(GetHandle(),&client)||!ClientToScreen(GetHandle(),&origin)) {
        result->Error("local_display_layout_unavailable","Exactly two local monitors are required.");return;
      }
      RECT bounds=monitors.front().bounds;
      for(const auto& monitor:monitors){bounds.left=std::min(bounds.left,monitor.bounds.left);bounds.top=std::min(bounds.top,monitor.bounds.top);bounds.right=std::max(bounds.right,monitor.bounds.right);bounds.bottom=std::max(bounds.bottom,monitor.bounds.bottom);}
      // {"requireCover":false}: measure only (before the window is spread over
      // both monitors); the connection re-reads with the cover check after it.
      bool requireCover=true;
      if(const auto* args=std::get_if<flutter::EncodableMap>(call.arguments())){const auto found=args->find(V("requireCover"));if(found!=args->end())if(const auto* flag=std::get_if<bool>(&found->second))requireCover=*flag;}
      if(requireCover&&(origin.x!=bounds.left||origin.y!=bounds.top||client.right-client.left!=bounds.right-bounds.left||client.bottom-client.top!=bounds.bottom-bounds.top)) {
        result->Error("local_display_layout_unavailable","The window does not cover the measured monitor layout.");return;
      }
      flutter::EncodableList surfaces;
      for(size_t i=0;i<monitors.size();++i){const auto& monitor=monitors[i];surfaces.emplace_back(flutter::EncodableMap{
        {V("id"),V(std::to_string(i))},{V("x"),V(int32_t(monitor.bounds.left))},{V("y"),V(int32_t(monitor.bounds.top))},
        {V("width"),V(int32_t(monitor.bounds.right-monitor.bounds.left))},{V("height"),V(int32_t(monitor.bounds.bottom-monitor.bounds.top))},
        {V("primary"),V(monitor.primary)},{V("dpi"),V(int32_t(monitor.dpi))}});}
      result->Success(V(flutter::EncodableMap{{V("x"),V(int32_t(bounds.left))},{V("y"),V(int32_t(bounds.top))},{V("width"),V(int32_t(bounds.right-bounds.left))},{V("height"),V(int32_t(bounds.bottom-bounds.top))},{V("monitors"),V(surfaces)}}));return;
    }
    if(call.method_name()=="localMonitorCount") {
      // This PC's monitor count (모든 모니터 사용 needs two).
      result->Success(flutter::EncodableValue(GetSystemMetrics(SM_CMONITORS)));return;
    }
    if (call.method_name() == "productVersion") {
      result->Success(flutter::EncodableValue(FLUTTER_VERSION)); return;
    }
    if (call.method_name() == "openConsoleWebView") {
      using V=flutter::EncodableValue;
      if(host_settings_||session_owner_){result->Error("console_webview_unavailable","Open the console from the launcher.");return;}
      if(authentication_retired_){result->Error("authentication_owner_changed","Authentication owner retired.");return;}
      const auto* args=std::get_if<flutter::EncodableMap>(call.arguments());
      auto target=multitap::ConsoleWebViewDestination(NoticeText(args,"origin"),NoticeText(args,"desktopId"),NoticeText(args,"attachPath"),NoticeText(args,"code"));
      if(!target){result->Error("invalid_console_webview","Invalid console destination.");return;}
      if(!console_webviews_)console_webviews_=std::make_unique<multitap::ConsoleWebViews>(GetHandle(),[this](const std::string& origin,const std::string& desktop,const std::string& owner){
        if(window_channel_)window_channel_->InvokeMethod("consoleWebViewClosed",std::make_unique<V>(flutter::EncodableMap{{V("origin"),V(origin)},{V("desktopId"),V(desktop)},{V("ownerId"),V(owner)}}));
        UpdateTrayTooltip();PostMessageW(GetHandle(),kSessionWindowsChanged,0,0);
      });
      console_webviews_->SetHuman([this]{AuthenticationHuman();});
      const auto error=console_webviews_->Open(std::move(*target),dark_appearance());
      if(!error.empty()){result->Error(error,"Console WebView unavailable.");return;}
      UpdateTrayTooltip();result->Success(V(true));return;
    }
    if (call.method_name() == "hideConsoleWebViews") {
      if(console_webviews_)console_webviews_->HideAll();result->Success();return;
    }
    if (call.method_name() == "closeConsoleWebViews") {
      const auto* args=std::get_if<flutter::EncodableMap>(call.arguments());
      if(!args){result->Error("invalid_console_webview","Invalid console close scope.");return;}
      if(args->empty()){if(console_webviews_)console_webviews_->CloseAll();}
      else{
        const auto origin=NoticeText(args,"origin"),desktop=NoticeText(args,"desktopId");
        const auto page=multitap::ConsoleBrowserTarget(origin,desktop);
        if(args->size()!=2||!page){result->Error("invalid_console_webview","Invalid console close scope.");return;}
        const auto normalized=page->substr(0,page->size()-std::string("/console/").size()-desktop.size());
        if(console_webviews_)console_webviews_->Close(normalized,desktop);
      }
      result->Success(flutter::EncodableValue(true));return;
    }
    if (call.method_name() == "consoleWebViewCount") {
      result->Success(flutter::EncodableValue(static_cast<int32_t>(console_webviews_?console_webviews_->Count():0)));return;
    }
    if (call.method_name() == "openConsoleBrowser") {
      using V=flutter::EncodableValue;
      const auto* args=std::get_if<flutter::EncodableMap>(call.arguments());
      const auto target=multitap::ConsoleBrowserTarget(NoticeText(args,"origin"),NoticeText(args,"desktopId"));
      if(!target){result->Error("invalid_console_browser","Invalid console destination.");return;}
      const std::wstring url(target->begin(),target->end());
      SHELLEXECUTEINFOW info{sizeof(info)};
      info.fMask=SEE_MASK_FLAG_NO_UI|SEE_MASK_ASYNCOK;
      info.hwnd=GetHandle();info.lpVerb=L"open";info.lpFile=url.c_str();info.nShow=SW_SHOWNORMAL;
      result->Success(V(ShellExecuteExW(&info)!=FALSE));return;
    }
    if (call.method_name() == "appearance") {
      const auto* dark = std::get_if<bool>(call.arguments());
      if (!dark) { result->Error("invalid_arguments", "Expected appearance state."); return; }
      SetDarkAppearance(*dark);
      if (tray_) tray_->SetDark(*dark);
      if (console_webviews_) console_webviews_->SetDark(*dark);
      result->Success(); return;
    }
    if (call.method_name() == "hasSessionWindows") {
      result->Success(flutter::EncodableValue(HasSessionWindows() || sessions_closing_)); return;
    }
    if (call.method_name() == "takeUpdateHandoff") {
      // update-handoff-v1: what the installer that relaunched this app did.
      using V = flutter::EncodableValue;
      if (session_owner_) { result->Success(); return; }
      const multitap::relaunch::Store store;
      const auto product = UpdateProduct();
      const auto record = store.Read(product);
      if (!record) { result->Success(); return; }
      const auto decision = multitap::relaunch::Decide(*record, RunningUpdateVersion(), multitap::relaunch::NowFileTime());
      if (decision == multitap::relaunch::Decision::Stale) store.Clear(product);
      if (decision != multitap::relaunch::Decision::Updated && decision != multitap::relaunch::Decision::Failed) {
        result->Success(); return;
      }
      store.Clear(product);
      result->Success(V(flutter::EncodableMap{
          {V("result"), V(decision == multitap::relaunch::Decision::Updated ? "updated" : "failed")},
          {V("product"), V(host_settings_ ? "host" : "client")},
          {V("fromVersion"), V(record->fromVersion)}, {V("toVersion"), V(record->toVersion)},
          {V("code"), V(record->code)}, {V("stage"), V(int32_t(record->stage))},
          {V("attention"), V(record->attention)}, {V("restartRequired"), V(record->restartRequired)}}));
      return;
    }
    if (call.method_name() == "verifyUpdate" || call.method_name() == "installUpdate") {
      const bool install = call.method_name() == "installUpdate";
      if (session_owner_) { result->Error("invalid_update", "Only the main window installs updates."); return; }
      if (install && (HasSessionWindows() || sessions_closing_)) {
        result->Error("session_windows_open", "Close remote sessions before installing an update."); return;
      }
      const auto* args=std::get_if<flutter::EncodableMap>(call.arguments());
      if(!args){result->Error("invalid_update","Update information is required.");return;}
      const auto field=[args](const char* key){const auto it=args->find(flutter::EncodableValue(key));if(it==args->end())return std::string();const auto* value=std::get_if<std::string>(&it->second);return value?*value:std::string();};
      const auto path=field("path"),filename=field("filename");
      const auto toWide=[](const std::string& value){const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);if(length<=0)return std::wstring();std::wstring out(size_t(length),L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),length);return out;};
      multitap::ProductUpdateSpec spec;spec.source=toWide(path);spec.filename=toWide(filename);spec.product=field("product");spec.version=field("version");spec.digest=field("sha256");spec.signature=field("signature");
      const auto it=args->find(flutter::EncodableValue("size"));
      if(it!=args->end()){if(const auto* v=std::get_if<int32_t>(&it->second);v&&*v>0)spec.size=uint64_t(*v);else if(const auto* n=std::get_if<int64_t>(&it->second);n&&*n>0)spec.size=uint64_t(*n);}
      // Each window updates only its own product.
      if(spec.product!=(host_settings_?"host":"client")||!multitap::ValidProductUpdate(spec)){result->Error("invalid_update","Update information is invalid.");return;}
      // The project's own release signature, before the restart is offered
      // and again before the installer starts (pinned key, no Windows trust).
      if(!multitap::ProductUpdateSignatureValid(spec)){result->Error("signature_invalid","The release signature is missing or does not match.");return;}
      if(!install){
        // The downloaded installer's own version resource, before the restart
        // is offered. The staged copy is checked again before it starts.
        if(multitap::InstallerVersionMatches(spec.source,spec.version))result->Success(flutter::EncodableValue(true));
        else result->Error("package_version_mismatch","The installer version does not match this release.");
        return;
      }
      const auto mode_name=field("mode");
      if(mode_name!="apply"&&mode_name!="manual"){result->Error("invalid_update","An update mode is required.");return;}
      const auto mode=mode_name=="manual"?multitap::InstallerMode::Manual:multitap::InstallerMode::Update;
      std::optional<multitap::relaunch::Record> handoff;
      if(mode==multitap::InstallerMode::Update){
        // A Host settings EXE outside its installation reports the Host
        // version from the service status instead of its path.
        auto running=RunningUpdateVersion();
        if(running.empty()&&host_settings_)running=field("currentVersion");
        if(!multitap::relaunch::ValidVersion(running)){result->Error("version_unknown","The running version is unknown.");return;}
        // Client and Host alike: a genuine older release is never an "update".
        if(multitap::relaunch::VersionLess(spec.version,running)){result->Error("update_not_newer","The release is older than the running client.");return;}
        multitap::relaunch::Record record;
        record.fromVersion=running;record.toVersion=spec.version;record.token=multitap::relaunch::NewToken();
        // The window reopens where it is now; a fullscreen view first returns
        // to the window it came from.
        RestorePresentation();
        record.hasPlacement=GetWindowPlacement(GetHandle(),&record.placement)!=FALSE;
        record.hasBounds=!IsIconic(GetHandle())&&GetWindowRect(GetHandle(),&record.bounds);
        if(!record.hasBounds&&record.hasPlacement){record.bounds=record.placement.rcNormalPosition;record.hasBounds=true;}
        record.dark=dark_appearance();record.processId=GetCurrentProcessId();record.created=multitap::relaunch::NowFileTime();
        if(!multitap::relaunch::ValidToken(record.token)){result->Error("handoff_unavailable","The update handoff could not be prepared.");return;}
        handoff=std::move(record);
      }
      // A Host update waits for the installer's own pre-checks (package,
      // display driver and publisher trust) before this window closes.
      update_worker_->Start(std::move(spec),mode,std::move(handoff),UpdateProduct(),host_settings_?180000:60000,std::move(result));return;
    }
    if (call.method_name() == "restore") { RestorePresentation(); result->Success(); return; }
    if (call.method_name() == "maximizeToFullscreen") {
      const auto* enabled = std::get_if<bool>(call.arguments());
      if (!enabled) { result->Error("invalid_arguments", "Expected a boolean."); return; }
      maximize_to_fullscreen_ = *enabled;
      result->Success(); return;
    }
    if (call.method_name() == "flash") {
      // Something wants the person (a document from the VM's printer): the
      // taskbar button flashes until the window is in front. Nothing is done
      // when it already is, and the window is never moved or activated.
      const HWND window = GetHandle();
      if (window && GetForegroundWindow() != window) {
        FLASHWINFO info{sizeof(info), window, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&info);
      }
      result->Success(); return;
    }
    if (call.method_name() == "minimize") {
      remote_input_->Capture(nullptr, false);
      ShowWindow(GetHandle(), SW_MINIMIZE);
      result->Success(); return;
    }
    const bool all_monitors = call.method_name() == "allMonitors";
    if (call.method_name() != "fullscreen" && !all_monitors) { result->NotImplemented(); return; }
    const auto* enabled = std::get_if<bool>(call.arguments());
    if (!enabled) { result->Error("invalid_arguments", "Expected fullscreen state."); return; }
    if (*enabled) {
      if (all_monitors && GetSystemMetrics(SM_CMONITORS) < 2) {
        result->Error("multiple_monitors_required", "Two local monitors are required."); return;
      }
      if (!presentation_.Set(GetHandle(), true, all_monitors)) {
        result->Error("presentation_failed", "Could not apply display mode."); return;
      }
      if (GetAncestor(GetForegroundWindow(), GA_ROOT) == GetHandle())
        RegisterHotKey(GetHandle(), 4312, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
    } else RestorePresentation();
    result->Success(flutter::EncodableValue(presentation_.fullscreen()));
  });
  input_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/input",
      &flutter::StandardMethodCodec::GetInstance());
  input_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (call.method_name() != "passwordMode") { result->NotImplemented(); return; }
    const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
    if (!args) { result->Error("invalid_arguments", "Expected password focus state."); return; }
    auto value = args->find(flutter::EncodableValue("enabled"));
    const bool* enabled = value == args->end() ? nullptr : std::get_if<bool>(&value->second);
    if (!enabled) { result->Error("invalid_arguments", "Expected password focus state."); return; }
    auto scope_it = args->find(flutter::EncodableValue("scope"));
    int64_t scope = 0;
    if (scope_it != args->end()) {
      if (auto* number = std::get_if<int32_t>(&scope_it->second)) scope = *number;
      else if (auto* number64 = std::get_if<int64_t>(&scope_it->second)) scope = *number64;
    }
    if (*enabled) password_scope_ = scope;
    if (*enabled || scope == password_scope_)
      password_ime_.Set(flutter_controller_->view()->GetNativeWindow(), *enabled);
    result->Success();
  });
  ConfigureRelativeMouse();
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    first_frame_completed_ = true;
#ifndef MULTITAP_SESSION_WINDOWS_QA
    if((session_owner_?session_owner_:this)->CheckAuthenticationDeadline() && !authentication_retired_) this->Show();
#endif
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  session_started_ = true;
  return true;
}

void FlutterWindow::OnDestroy() {
  // Win32Window::Create() first calls Destroy() even on a virgin object. There
  // is no native lifetime to retire yet: closing authentication or console
  // input here would permanently disable the newly created session window.
  // Real/partial creation and reentrant teardown must still run cleanup, and
  // an already retired object's state must never be reset for reuse.
  if(!GetHandle() && !flutter_controller_ && !session_started_ && !session_teardown_in_progress_) return;
  if(GetHandle())KillTimer(GetHandle(),kAuthenticationIdleTimer);
  authentication_deadline_tick_=0;authentication_retired_=true;
  if (cloak_hook_) { UnhookWinEvent(cloak_hook_); cloak_hook_ = nullptr; }
  if (visibility_owner_ == this) visibility_owner_ = nullptr;
  if(console_webviews_){console_webviews_->CloseAll();console_webviews_.reset();}
  if(stream_size_queries_){stream_size_queries_->Close();stream_size_queries_.reset();}
  if(transfer_notice_alive_)transfer_notice_alive_->store(false);
  if(GetHandle()&&transfer_notice_observer_){
    multitap::TransferNotice::Shared().Detach(GetHandle(),transfer_notice_observer_);
    transfer_notice_observer_=0;
  }
  // Before any nested teardown pump: never leave the shared cursor clipped.
  relative_mouse_.Release();
  game_mouse_usb_.reset();
  gamepad_input_.reset();
  // The notification-area icon goes first, in every exit path (a normal exit,
  // the installer closing the app, logoff), so none stays behind.
  if (tray_channel_) tray_channel_->SetMethodCallHandler(nullptr);
  tray_channel_.reset();
  if (tray_) {
    tray_->Destroy();
    tray_.reset();
  }
  if (tray_icon_) {
    DestroyIcon(tray_icon_);
    tray_icon_ = nullptr;
  }
  if (session_teardown_in_progress_) return;
  struct Teardown {
    bool& flag;
    explicit Teardown(bool& value) : flag(value) { flag = true; }
    ~Teardown() { flag = false; }
  } teardown(session_teardown_in_progress_);
  const bool was_started = session_started_;
  session_started_ = false;
  if (was_started) {
    session_destroyed_ = true;
    sessions_closing_ = true;
    allow_session_windows_ = false;
  }
  // Teardown can run a nested message pump. Retire all inbound method
  // handlers before releasing any native owner or iterating sibling windows.
  const auto retire = [](const auto& channel) {
    if (channel) channel->SetMethodCallHandler(nullptr);
  };
  retire(sessions_channel_); retire(close_channel_); retire(window_channel_);
  retire(clipboard_channel_); retire(remote_channel_); retire(drive_channel_); retire(usb_channel_);
  retire(input_channel_); retire(credential_channel_); retire(relative_channel_);
  retire(print_channel_); retire(transfer_notice_channel_);
  relative_channel_.reset();
  if (GetHandle()) {
    KillTimer(GetHandle(), kSessionCloseTimer);
    KillTimer(GetHandle(), kThumbnailTimer);
  }
  // Pictures still queued are written by the writer's last owner; only the
  // notifications to this closing window stop.
  if (thumbnails_ && !session_owner_) thumbnails_->Detach();
  for (const auto& child : session_windows_) child->Destroy();
  if (sessions_close_result_) {
    auto result = std::move(sessions_close_result_);
    result->Error("window_closed", "The dashboard window was closed.");
  }
  if (sessions_channel_) sessions_channel_->SetMethodCallHandler(nullptr);
  sessions_channel_.reset();
  if (was_started) session_payload_ = flutter::EncodableValue();
  if (close_channel_) close_channel_->SetMethodCallHandler(nullptr);
  close_channel_.reset();
  if(clipboard_worker_)clipboard_worker_->ForegroundLost();
  clipboard_channel_.reset(); clipboard_worker_.reset();
  // The card and the taskbar progress of this window go with it; no callback into the closed channel runs afterwards.
  transfer_notice_channel_.reset();
  paste_target_channel_.reset(); paste_target_worker_.reset();
  print_channel_.reset(); print_worker_.reset();
  drive_channel_.reset(); drive_worker_.reset();
  // Every captured USB device goes back to Windows with the window.
  if (usb_worker_) usb_worker_->Shutdown();
  usb_channel_.reset(); usb_worker_.reset();
  RestorePresentation();
  if (window_events_) window_events_->SetStreamHandler(nullptr);
  update_worker_.reset();
  window_sink_.reset(); window_events_.reset(); window_channel_.reset();
  if (remote_input_) remote_input_->Capture(nullptr, false);
  // Before the view and the capture probe go away: restore and unsubclass.
  if (cursor_presenter_) cursor_presenter_->Detach();
  cursor_presenter_.reset();
  udp_media_.reset();
  remote_input_.reset(); remote_media_.reset();
  if (remote_events_) remote_events_->SetStreamHandler(nullptr);
  remote_sink_.reset(); remote_events_.reset(); remote_channel_.reset(); remote_registrar_.reset();
  password_ime_.Set(nullptr, false);
  input_channel_.reset();
  credential_channel_.reset();
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }
#ifdef MULTITAP_SESSION_WINDOWS_QA
  // Simulate a nested parent timer at the exact boundary that used to look
  // empty: the child's HWND/engine are gone but OnDestroy has not unwound.
  if (was_started && session_owner_ && session_owner_->GetHandle() && !GetHandle()) {
    auto& owner = *session_owner_;
    ++owner.qa_teardown_checks_;
    const bool draining = owner.sessions_closing_;
    if (draining) ++owner.qa_teardown_drain_checks_;
    const bool guarded = owner.HasSessionWindows() && owner.SessionKeyClosing(session_key_);
    owner.CheckSessionWindowsClosed();
    if (!guarded || (draining && !owner.sessions_closing_)) ++owner.qa_teardown_failures_;
  }
#endif

  Win32Window::OnDestroy();
  if (was_started && session_owner_ && session_owner_->GetHandle())
    PostMessageW(session_owner_->GetHandle(), kSessionWindowsChanged, 0, 0);
}

void FlutterWindow::AuthenticationHuman() {
  auto* parent=session_owner_?session_owner_:this;
  if(!parent->CheckAuthenticationDeadline())return;
  if(authentication_retired_ || parent->authentication_retired_ || !AuthenticationOwnerValid(authentication_owner_) ||
      authentication_owner_!=parent->authentication_owner_ || !parent->sessions_channel_) return;
  const auto now=GetTickCount64();
  if(parent->authentication_activity_tick_ && now-parent->authentication_activity_tick_<250) return;
  parent->authentication_activity_tick_=now;
  using V=flutter::EncodableValue;using M=flutter::EncodableMap;
  parent->sessions_channel_->InvokeMethod("authenticationEvent",std::make_unique<V>(M{
    {V("owner"),V(authentication_owner_)},{V("kind"),V("human")}}));
}

void FlutterWindow::HideAuthentication() {
  authentication_retired_=true;
  if(GetHandle())KillTimer(GetHandle(),kAuthenticationIdleTimer);authentication_deadline_tick_=0;
  for(const auto& child:session_windows_) if(child->authentication_owner_==authentication_owner_) {
    child->authentication_retired_=true;
    if(child->remote_input_)child->remote_input_->Capture(nullptr,false);
    child->relative_mouse_.End("focus");
    if(child->GetHandle())ShowWindow(child->GetHandle(),SW_HIDE);
    child->session_close_reason_="session_expired";
    if(child->GetHandle())PostMessageW(child->GetHandle(),kSessionWindowClose,0,0);
  }
  if(console_webviews_)console_webviews_->HideAll();
}

bool FlutterWindow::SetAuthenticationDeadline(std::optional<int64_t> remaining) {
  if(!GetHandle())return false;
  KillTimer(GetHandle(),kAuthenticationIdleTimer);authentication_deadline_tick_=0;
  if(remaining) {
    authentication_deadline_tick_=GetTickCount64()+static_cast<ULONGLONG>(*remaining);
    if(!SetTimer(GetHandle(),kAuthenticationIdleTimer,static_cast<UINT>(std::max<int64_t>(1,*remaining)),nullptr)) {
      HideAuthentication();
      using V=flutter::EncodableValue;using M=flutter::EncodableMap;
      if(sessions_channel_)sessions_channel_->InvokeMethod("authenticationEvent",std::make_unique<V>(M{
        {V("owner"),V(authentication_owner_)},{V("kind"),V("terminal")},{V("reason"),V("session_reauthentication_required")}}));
      return false;
    }
  }
  return true;
}

bool FlutterWindow::CheckAuthenticationDeadline() {
  if(authentication_retired_)return false;
  if(authentication_deadline_tick_ && GetTickCount64()>=authentication_deadline_tick_) {
    HideAuthentication();
    using V=flutter::EncodableValue;using M=flutter::EncodableMap;
    if(sessions_channel_)sessions_channel_->InvokeMethod("authenticationEvent",std::make_unique<V>(M{
      {V("owner"),V(authentication_owner_)},{V("kind"),V("terminal")},{V("reason"),V("session_idle_timeout")}}));
    return false;
  }
  return true;
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  if(!GetHandle()&&session_destroyed_)return 0;
  if(message==WM_TIMER && wparam==kAuthenticationIdleTimer){CheckAuthenticationDeadline();return 0;}
  if(message==WM_ACTIVATE || message==WM_ACTIVATEAPP || message==WM_SHOWWINDOW)
    (session_owner_?session_owner_:this)->CheckAuthenticationDeadline();
  // Also drain completion on the next window message if PostMessage hit a full
  // queue. No worker thread calls Flutter or keeps a window/engine pointer.
  if(update_worker_)update_worker_->Complete();
  // Logoff and shutdown are never blocked and never ask anything; the tray
  // icon goes quietly (Windows ends the process itself).
  {
    LRESULT end_result = 0;
    if (multitap::close::HandleSessionEnd(message, wparam, session_ending_, end_result,
        [this] { if (tray_) tray_->RemoveIcon(); })) return end_result;
    // A close that arrives while Windows ends the session passes every question.
    if (message == WM_CLOSE && session_ending_) close_guard_.RequestClose();
  }
  // Hidden windows (backlog #62): any move, resize, minimize, show, z-order or
  // activation change of this client's windows can hide or reveal a session.
  if (message == WM_WINDOWPOSCHANGED || message == WM_SIZE || message == WM_SHOWWINDOW ||
      message == WM_ACTIVATE || message == WM_DISPLAYCHANGE) {
    (session_owner_ ? session_owner_ : this)->UpdateSessionVisibility();
  }
  if (tray_enabled_) {
    if (message == WM_ACTIVATE) {
      // Clicking the tray icon moves the foreground to the taskbar: remember
      // that, so the same click can put this window away (TrayToggle).
      wchar_t activated[64]{};
      const bool by_tray = LOWORD(wparam) == WA_INACTIVE && lparam &&
          GetClassNameW(reinterpret_cast<HWND>(lparam), activated, 64) &&
          multitap::tray::IsShellTrayClass(activated);
      tray_deactivated_tick_ = by_tray ? GetTickCount64() : 0;
    }
    if (message == WM_SHOWWINDOW && !lparam) {
      // Shown by anyone (the second launch, a balloon click, the tray): it is
      // out of the tray, and Flutter paints again.
      hidden_to_tray_ = wparam == FALSE;
      if (wparam && flutter_controller_) flutter_controller_->ForceRedraw();
    }
    // X, Alt+F4, the system menu: rest in the tray (or ask, without a tray icon).
    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_CLOSE) {
      switch (multitap::close::Decide(multitap::close::Origin::kUserClose, CloseState())) {
        case multitap::close::Action::kHideToTray: HideToTray(); return 0;
        case multitap::close::Action::kAskApp: AskToExit(); return 0;
        default: break;
      }
    }
    // A plain WM_CLOSE is a request to exit: ?꾩쟾 醫낅즺, the installer, Task
    // Manager. Nothing open: exit now. Connections that would end: confirm.
    if (message == WM_CLOSE && !session_owner_) {
      switch (multitap::close::Decide(multitap::close::Origin::kQuitRequest, CloseState())) {
        case multitap::close::Action::kExit: close_guard_.RequestClose(); break;
        case multitap::close::Action::kAskApp: AskToExit(); return 0;
        default: break;
      }
    }
  }
  // First, so deactivation and modal loops release the clip before anyone else.
  if(message==WM_INPUT && relative_mouse_.engaged() &&
      GetAncestor(GetForegroundWindow(),GA_ROOT)==GetHandle()) {
    RAWINPUT input{};UINT size=sizeof(input);
    if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER))!=UINT(-1) &&
        size>=sizeof(RAWINPUTHEADER)+sizeof(RAWMOUSE) && input.header.dwType==RIM_TYPEMOUSE && input.header.hDevice &&
        (input.data.mouse.lLastX || input.data.mouse.lLastY || input.data.mouse.usButtonFlags)) AuthenticationHuman();
  }
  if (relative_mouse_.HandleMessage(hwnd, message, wparam, lparam)) return 0;
  if (game_mouse_usb_ && game_mouse_usb_->HandleMessage(message)) return 0;
  if (gamepad_input_ && gamepad_input_->HandleMessage(message)) return 0;
  if (message == multitap::kRelativeMouseToggleMessage) {
    // A queued chord belongs to the capture/arming epoch that posted it.
    // Never release or toggle a successor session after focus or ownership
    // changed while the message waited in the native queue.
    if (!remote_input_ || !remote_input_->RelativeTogglePending(wparam) ||
        GetAncestor(GetForegroundWindow(), GA_ROOT) != GetHandle()) return 0;
    // Here first: the release key must work even if Dart does not answer.
    if (relative_mouse_.engaged()) {
      relative_mouse_.Release();
      if (cursor_presenter_) cursor_presenter_->MoveToRemote();
    }
    if (relative_channel_) relative_channel_->InvokeMethod("toggle", nullptr);
    return 0;
  }
  if (message == WM_DISPLAYCHANGE) {
    presentation_.Refresh(hwnd);
    if (window_sink_) window_sink_->Success(flutter::EncodableValue(
      flutter::EncodableMap{{flutter::EncodableValue("type"), flutter::EncodableValue("displayLayoutChanged")}}));
  }
  // A border/caption drag: the remote view asks for the final size once it
  // ends instead of renegotiating on every pause of the drag.
  if ((message == WM_ENTERSIZEMOVE || message == WM_EXITSIZEMOVE) && window_sink_) {
    window_sink_->Success(flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("type"), flutter::EncodableValue("sizeMove")},
      {flutter::EncodableValue("active"), flutter::EncodableValue(message == WM_ENTERSIZEMOVE)}}));
  }
  if (message == kSessionWindowsChanged || (message == WM_TIMER && wparam == kSessionCloseTimer)) {
    UpdateTrayTooltip();
    CheckSessionWindowsClosed(); return 0;
  }
  if (message == kSessionWindowClose) { DeliverSessionClose(); return 0; }
  if (message == WM_TIMER && wparam == kThumbnailTimer) { CaptureThumbnail(); return 0; }
  if (message == multitap::ThumbnailWriter::completed_message) {
    DeliverThumbnails();
    CheckSessionWindowsClosed();
    return 0;
  }
  // The app asks in its own dialog (every exit is confirmed) and
  // is told whether remote windows would close too.
  if (message == WM_CLOSE && !session_owner_ && close_guard_.BlocksClose()) {
    if (close_channel_) {
      flutter::EncodableMap request{{flutter::EncodableValue("sessions"), flutter::EncodableValue(HasSessionWindows())}};
      close_channel_->InvokeMethod("requested", std::make_unique<flutter::EncodableValue>(std::move(request)));
    }
    return 0;
  }
  if (message == WM_CLOSE && !session_owner_ && HasSessionWindows()) {
    if (sessions_closing_) return 0;
    // Confirmed in the app already; the native question only when the app
    // does not ask (for example before it started).
    if (close_guard_.Dirty() || MessageBoxW(hwnd, multitap::Tr(L"열려 있는 연결을 모두 끊고 Multitap을 종료할까요?", L"Disconnect all open connections and quit Multitap?"),
        multitap::Tr(L"Multitap 종료", L"Quit Multitap"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
      RequestSessionWindowsClose("application_close", true);
    return 0;
  }
  if (message == WM_CLOSE && session_owner_ && close_guard_.BlocksClose()) {
    if (close_channel_) close_channel_->InvokeMethod("requested", nullptr);
    return 0;
  }
  if (message == multitap::UsbCaptureChannel::completed_message) {
    if (usb_worker_) usb_worker_->Complete();
    return 0;
  }
  if (message == multitap::ClientDriveChannel::completed_message) {
    if (drive_worker_) drive_worker_->Complete();
    return 0;
  }
  if (message == multitap::ClientClipboardChannel::completed_message) {
    if (clipboard_worker_) clipboard_worker_->Complete();
    return 0;
  }
  if (message == multitap::ClientClipboardChannel::pull_message) {
    if (clipboard_worker_) clipboard_worker_->DeliverPullEvents();
    return 0;
  }
  if (message == multitap::ClientPrintChannel::completed_message) {
    if (print_worker_) print_worker_->Complete();
    return 0;
  }
  if (message == multitap::PasteTargetChannel::completed_message) {
    if (paste_target_worker_) paste_target_worker_->Complete();
    return 0;
  }
  if(message==multitap::StreamSizeQueryChannel::completed_message){if(stream_size_queries_)stream_size_queries_->Complete();return 0;}
  if(message==WM_TIMER&&wparam==multitap::StreamSizeQueryChannel::timeout_timer){if(stream_size_queries_)stream_size_queries_->Expire();return 0;}
  if(message==WM_CLIPBOARDUPDATE) {
    if(clipboard_worker_)clipboard_worker_->ClipboardChanged();
    return 0;
  }
  if(message==multitap::ProductUpdateTask::completed_message){if(update_worker_)update_worker_->Complete();return 0;}
  // The installer accepted the update: close now, without the close
  // questions (no remote session is open; the update was the confirmed step).
  if(message==multitap::ProductUpdateTask::close_message){
    if(!session_owner_&&update_worker_&&update_worker_->Busy()){
      // With a remote window open, staying open would let the installer
      // terminate the app mid-stream (mouse lock, cursor and pending
      // clipboard left behind for the relaunched version). So every remote
      // window closes through its normal cleanup first; the dashboard
      // closes when the last one is gone (CheckSessionWindowsClosed).
      if(HasSessionWindows()){close_for_update_=true;if(!sessions_closing_)RequestSessionWindowsClose("application_close",false);}
      else{ShowWindow(hwnd,SW_HIDE);DestroyWindow(hwnd);}
    }
    return 0;
  }
  if ((message == WM_HOTKEY && wparam == 4312) || message == WM_APP + 180) {
    if (remote_input_) remote_input_->Capture(nullptr, false);
    if (cursor_presenter_) cursor_presenter_->Refresh();
    RestorePresentation();
    return 0;
  }
  if (message == WM_ACTIVATEAPP && flutter_controller_) {
    const bool foreground = wparam && GetAncestor(GetForegroundWindow(), GA_ROOT) == hwnd;
    password_ime_.Activate(flutter_controller_->view()->GetNativeWindow(), foreground);
    if (!foreground) ReleaseForegroundOwnership();
    else if (presentation_.fullscreen())
      RegisterHotKey(hwnd, 4312, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
  }
  if (message == WM_ACTIVATE && flutter_controller_) {
    const bool foreground = LOWORD(wparam) != WA_INACTIVE &&
        GetAncestor(GetForegroundWindow(), GA_ROOT) == hwnd;
    password_ime_.Activate(flutter_controller_->view()->GetNativeWindow(), foreground);
    if (!foreground) ReleaseForegroundOwnership();
    else if (presentation_.fullscreen())
      RegisterHotKey(hwnd, 4312, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
    // Win32Window's default WM_ACTIVATE always focuses its child, including
    // WA_INACTIVE; that would steal focus back from a sibling session window.
    if (!foreground) {
      flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam, lparam);
      return 0;
    }
  }
  // Maximize of a remote view (button, caption double-click) means fullscreen,
  // like a remote desktop client; Dart applies it through its normal display
  // mode path so viewport resolution, confinement and hotkeys follow.
  if (maximize_to_fullscreen_ && !presentation_.fullscreen() && window_sink_) {
    const bool maximize = message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_MAXIMIZE;
    // Win+Up and snapping to the top edge maximize without SC_MAXIMIZE.
    const bool maximized = message == WM_SIZE && wparam == SIZE_MAXIMIZED;
    if (maximize || maximized) {
      using V = flutter::EncodableValue;
      window_sink_->Success(V(flutter::EncodableMap{{V("type"), V("maximizeRequested")}}));
      if (maximize) return 0;
    }
  }
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_DPICHANGED:
      if (presentation_.fullscreen() && presentation_.Refresh(hwnd)) return 0;
      break;
    case WM_FONTCHANGE:
      if (flutter_controller_) flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

// --- the notification-area icon (main client window) ---------------------------

int FlutterWindow::OpenSessionWindowCount() const {
  return static_cast<int>(console_webviews_?console_webviews_->Count():0)+static_cast<int>(std::count_if(session_windows_.begin(), session_windows_.end(),
      [](const auto& child) { return child->GetHandle() != nullptr; }));
}

multitap::close::State FlutterWindow::CloseState() const {
  multitap::close::State state;
  state.tray_enabled = tray_enabled_;
  state.tray_ready = tray_ && tray_->icon_added();
  state.allowed = !close_guard_.BlocksClose();
  state.sessions = HasSessionWindows();
  state.session_ending = session_ending_;
  return state;
}

void FlutterWindow::ConfigureTray() {
#if !defined(MULTITAP_WINDOW_LAYOUT_QA) && !defined(MULTITAP_SESSION_WINDOWS_QA)
  // Only the client's main window: not remote session windows, not Host settings.
  if (session_owner_ || !allow_session_windows_ || host_settings_ || !GetHandle()) return;
  tray_enabled_ = true;
  const UINT dpi = GetDpiForWindow(GetHandle());
  tray_icon_ = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
      GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : 96), GetSystemMetricsForDpi(SM_CYSMICON, dpi ? dpi : 96),
      LR_DEFAULTCOLOR));
  if (!tray_icon_) tray_icon_ = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  // (Delegate is a private base: the conversion has to happen in this member.)
  tray_.reset(new multitap::tray::Host(tray_shell_, *this, tray_icon_, dark_appearance()));
  // No icon (Explorer is not running yet) is not fatal: the window keeps
  // asking before it closes, and the icon is added when the taskbar appears.
  tray_->Create(multitap::tray::Tooltip(0));
  tray_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "multitap/tray", &flutter::StandardMethodCodec::GetInstance());
  tray_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    using V = flutter::EncodableValue;
    if (call.method_name() == "balloon") {
      // Only while the window rests in the tray: a visible window needs no balloon.
      const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
      const auto* title = args ? CredentialArgument(*args, "title") : nullptr;
      const auto* text = args ? CredentialArgument(*args, "text") : nullptr;
      const auto wide_title = title ? SessionWindowTitle(*title) : std::wstring();
      const auto wide_text = text ? SessionWindowTitle(*text) : std::wstring();
      if (wide_title.empty() || wide_text.empty() || wide_title.size() > 63 || wide_text.size() > 127) {
        result->Error("invalid_arguments", "A short title and text are required."); return;
      }
      const bool show = tray_ && hidden_to_tray_ && tray_->icon_added();
      if (show) tray_->Balloon(wide_title, wide_text);
      result->Success(V(show)); return;
    }
    result->NotImplemented();
  });
#endif
}

void FlutterWindow::UpdateTrayTooltip() {
  if (tray_) tray_->SetTooltip(multitap::tray::Tooltip(OpenSessionWindowCount()));
}

void FlutterWindow::HideToTray() {
  if (!GetHandle()) return;
  hidden_to_tray_ = true;
  ShowWindow(GetHandle(), SW_HIDE);
  UpdateTrayTooltip();
  if (tray_channel_) tray_channel_->InvokeMethod("hidden", nullptr);
}

void FlutterWindow::RestoreFromTray() {
  const HWND window = GetHandle();
  if (!window) return;
  ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
  BringWindowToTop(window);
  SetForegroundWindow(window);
}

void FlutterWindow::AskToExit() {
  if (sessions_closing_ || !GetHandle()) return;
  // Before the app is listening there is nothing to ask in: close as the
  // window always did.
  if (!close_guard_.Dirty() || !close_channel_) {
    if (close_guard_.RequestClose()) PostMessageW(GetHandle(), WM_CLOSE, 0, 0);
    return;
  }
  RestoreFromTray();
  flutter::EncodableMap request{{flutter::EncodableValue("sessions"), flutter::EncodableValue(HasSessionWindows())}};
  close_channel_->InvokeMethod("requested", std::make_unique<flutter::EncodableValue>(std::move(request)));
}

void FlutterWindow::TrayToggle() {
  const HWND window = GetHandle();
  if (!window) return;
  const bool recent = tray_deactivated_tick_ != 0 && GetTickCount64() - tray_deactivated_tick_ < 1500;
  if (multitap::tray::DecideToggle(IsWindowVisible(window) != FALSE, IsIconic(window) != FALSE, recent) ==
      multitap::tray::Toggle::kHide)
    HideToTray();
  else
    RestoreFromTray();
}

void FlutterWindow::TrayOpen() { RestoreFromTray(); }

// ?꾩쟾 醫낅즺 from the tray menu: a quit request like any other (exits at once
// when nothing is open, asks first when remote connections would end).
void FlutterWindow::TrayExit() {
  if (GetHandle()) PostMessageW(GetHandle(), WM_CLOSE, 0, 0);
}

void FlutterWindow::CaptureThumbnail() {
  if (!thumbnails_ || !thumbnail_target_ || !remote_media_) return;
  int64_t lease = 0;
  uint64_t serial = 0;
  auto frame = remote_media_->Latest(lease, serial);
  // An idle stream that published nothing new is not encoded again.
  if (!frame || (lease == thumbnail_lease_ && serial == thumbnail_serial_)) return;
  if (thumbnail_ticket_&&thumbnails_->Submit(*thumbnail_target_, std::move(frame),*thumbnail_ticket_)) {
    thumbnail_lease_ = lease;
    thumbnail_serial_ = serial;
  }
}

void FlutterWindow::DeliverThumbnails() {
  if (session_owner_ || !thumbnails_) return;
  using V = flutter::EncodableValue;
  // Only identifiers cross to Dart; the pictures stay in the files.
  for (auto& notice : thumbnails_->TakeNotices()) {
    if (sessions_channel_)
      sessions_channel_->InvokeMethod("thumbnailUpdated", std::make_unique<V>(flutter::EncodableMap{
          {V("key"), V(std::move(notice.key))}, {V("desktopId"), V(std::move(notice.desktop))},
          {V("epoch"),V(std::move(notice.epoch))},
          {V("generation"),V(static_cast<int64_t>(notice.ticket.stamp))},{V("invalidated"),V(notice.invalidated)}}));
  }
}

void FlutterWindow::ReleaseForegroundOwnership() {
  if (clipboard_worker_) clipboard_worker_->ForegroundLost();
  if (remote_input_) remote_input_->Capture(nullptr, false);
  if (cursor_presenter_) cursor_presenter_->Refresh();
  UnregisterHotKey(GetHandle(), 4312);
}

// A media start/stop releases input capture and retires every cursor shape.
void FlutterWindow::ResetRemoteInput() {
  relative_mouse_.End("focus");
  if (remote_input_) remote_input_->Capture(nullptr, false);
  if (cursor_presenter_) cursor_presenter_->Reset();
}

void FlutterWindow::RestorePresentation() {
  if (!GetHandle()) return;
  UnregisterHotKey(GetHandle(), 4312);
  presentation_.Set(GetHandle(), false);
  if (window_sink_) window_sink_->Success(flutter::EncodableValue(false));
}

// "engage": capture now, or false when this window cannot be sure of it.
// "release": Dart ended it. "armKey" {bool}: Ctrl+Alt+Shift+Z stays local.
// Native "released" {reason}: capture ended here. Native "toggle": the key.
void FlutterWindow::ConfigureRelativeMouse() {
  using V = flutter::EncodableValue;
  relative_channel_ = std::make_unique<flutter::MethodChannel<V>>(
      flutter_controller_->engine()->messenger(), "multitap/relative-mouse",
      &flutter::StandardMethodCodec::GetInstance());
  relative_mouse_.Bind(
      [this](int dx, int dy) {
        const auto lease = remote_media_ ? remote_media_->Lease() : 0;
        if (!remote_sink_ || lease <= 0) return;
        remote_sink_->Success(V(flutter::EncodableMap{
            {V("type"), V("input")}, {V("event"), V("moveRelative")},
            {V("dx"), V(dx)}, {V("dy"), V(dy)}, {V("lease"), V(lease)}}));
      },
      [this](const char* reason) {
        if (cursor_presenter_) cursor_presenter_->MoveToRemote();
        if (relative_channel_) relative_channel_->InvokeMethod("released", std::make_unique<V>(reason));
      });
  relative_channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (call.method_name() == "release") {
      if (relative_mouse_.engaged()) {
        relative_mouse_.Release();
        if (cursor_presenter_) cursor_presenter_->MoveToRemote();
      }
      result->Success();
      return;
    }
    if (call.method_name() == "armKey") {
      const auto* armed = std::get_if<bool>(call.arguments());
      if (!armed) { result->Error("invalid_arguments", "Expected a boolean."); return; }
      if (remote_input_) remote_input_->SetRelativeToggle(*armed);
      result->Success();
      return;
    }
    if (call.method_name() != "engage") { result->NotImplemented(); return; }
    // 0: refused; 1: movement as moveRelative (SendInput on the Host); 2: the
    // virtual USB mouse takes movement, buttons and wheel (game_mouse_usb.h).
    // Arguments: the picture {left, top, width, height} in logical pixels of
    // this window's client area, where the pointer is held; none: parked.
    // "pin": true (cursor-echo-v1 granted): parked, then moved only to each
    // VM cursor position the Host echoes (relative_mouse.h Pin).
    // "conceal": true (cursor-compose-v1 granted, preferred): parked and shown
    // as nothing; the Host draws the VM's cursor into the video.
    int32_t path = 0;
    if (session_owner_ && GetHandle() && remote_input_ && remote_input_->captured() &&
        remote_media_ && remote_media_->Lease() > 0 &&
        !multitap::RemoteDesktopSession()) {
      std::optional<RECT> area;
      bool pin = false, conceal = false;
      if (const auto* args = std::get_if<flutter::EncodableMap>(call.arguments())) {
        if (const auto found = args->find(V("pin")); found != args->end())
          if (const auto* value = std::get_if<bool>(&found->second)) pin = *value;
        if (const auto found = args->find(V("conceal")); found != args->end())
          if (const auto* value = std::get_if<bool>(&found->second)) conceal = *value;
        const auto number = [args](const char* key, double& out) {
          const auto found = args->find(V(key));
          if (found == args->end()) return false;
          if (const auto* value = std::get_if<double>(&found->second)) { out = *value; return true; }
          if (const auto* value = std::get_if<int32_t>(&found->second)) { out = *value; return true; }
          return false;
        };
        multitap::LogicalRect rect;
        RECT client{};
        POINT origin{};
        if (number("left", rect.left) && number("top", rect.top) && number("width", rect.width) &&
            number("height", rect.height) && GetClientRect(GetHandle(), &client) && ClientToScreen(GetHandle(), &origin)) {
          area = multitap::ConfinementRect(rect, GetDpiForWindow(GetHandle()), origin,
                                          SIZE{client.right - client.left, client.bottom - client.top});
        }
      }
      if (!relative_mouse_.engaged() && game_mouse_usb_) {
        if (auto sink = game_mouse_usb_->Engage()) relative_mouse_.Route(std::move(*sink));
      }
      if (relative_mouse_.Engage(GetHandle(), flutter_controller_->view()->GetNativeWindow(), area ? &*area : nullptr, pin, conceal)) {
        path = relative_mouse_.routed() ? 2 : 1;
        // cursor-compose-v1: hide this PC's cursor now, not at the next move.
        if (conceal && cursor_presenter_) cursor_presenter_->Refresh();
      } else relative_mouse_.Route({});
    }
    result->Success(V(path));
  });
  game_mouse_usb_ = std::make_unique<multitap::GameMouseUsbChannel>(flutter_controller_->engine()->messenger(), GetHandle());
  gamepad_input_ = std::make_unique<multitap::GamepadInput>(flutter_controller_->engine()->messenger(), GetHandle());
}
