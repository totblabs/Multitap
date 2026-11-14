#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>
#include <flutter/event_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>
#include <atomic>
#include <optional>
#include <vector>

#include "win32_window.h"
#include "thumbnail_writer.h"
#include "password_ime.h"
#include "remote_media_session.h"
#include "udp_media_channel.h"
#include "render_environment.h"
#include "remote_input.h"
#include "window_presentation.h"
#include "window_close_guard.h"
#include "main_window_close.h"
#include "client_tray.h"
#include "client_clipboard.h"
#include "client_print.h"
#include "transfer_notice.h"
#include "paste_target.h"
#include "drive_channel.h"
#include "usb_capture.h"
#include "product_update_task.hpp"
#include "relative_mouse.h"
#include "game_mouse_usb_channel.h"
#include "gamepad_input.h"
#include "cursor_presenter.h"
#include "stream_size_query.h"
#include "console_webview.h"
#include "session_visibility.h"

// A window that does nothing but host a Flutter view.
class FlutterWindow : public Win32Window, private multitap::tray::Delegate {
 public:
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  explicit FlutterWindow(const flutter::DartProject& project,
                         bool allow_session_windows = true);
  virtual ~FlutterWindow();
  // The Host settings role updates the Host product; the dashboard the client.
  void SetHostSettings(bool host_settings) { host_settings_ = host_settings; }
  // Reopens at the placement saved before an update, when the installer
  // relaunched exactly this version (update-handoff-v1). Call before showing.
  void RestoreUpdatePlacement();
  // Call only from the outer application message pump, never from a window or
  // engine callback: a nested message pump can run while a child is closing.
  void CollectClosedSessionWindows();

 protected:
  // Win32Window:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

 private:
  FlutterWindow(const flutter::DartProject& project, FlutterWindow* owner,
                std::string key, flutter::EncodableValue payload);
  void ConfigureSessionWindows();
  void RequestSessionWindowsClose(const std::string& reason, bool close_parent);
  void CheckSessionWindowsClosed();
  void DeliverSessionClose();
  bool HasSessionWindows() const;
  bool SessionKeyClosing(const std::string& key) const;
  // Hidden windows (backlog #62), dashboard only: each session window that
  // shows none of its picture (session_visibility.h) stops presenting it.
  void UpdateSessionVisibility();
  static void CALLBACK CloakChanged(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD);
  static FlutterWindow* visibility_owner_;
  HWINEVENTHOOK cloak_hook_ = nullptr;
  bool media_hidden_ = false, updating_visibility_ = false;
  void ReleaseForegroundOwnership();
  // The notification-area icon of the main client window: X rests
  // the window in the tray; 완전 종료 (tray or account menu) is the way out.
  void ConfigureTray();
  void HideToTray();
  void RestoreFromTray();
  void AskToExit();
  void UpdateTrayTooltip();
  int OpenSessionWindowCount() const;
  multitap::close::State CloseState() const;
  // multitap::tray::Delegate:
  void TrayToggle() override;
  void TrayOpen() override;
  void TrayExit() override;
  multitap::tray::SystemShell tray_shell_;
  std::unique_ptr<multitap::tray::Host> tray_;
  HICON tray_icon_ = nullptr;
  bool tray_enabled_ = false, hidden_to_tray_ = false, session_ending_ = false;
  // When the tray took the foreground from this window (a click on its icon).
  ULONGLONG tray_deactivated_tick_ = 0;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> tray_channel_;
  void CaptureThumbnail();
  void DeliverThumbnails();
  // The dashboard owns the writer; streaming session windows share it and
  // write <LocalAppData>\Multitap\thumbnails\<account key>\<desktop>.jpg.
  std::shared_ptr<multitap::ThumbnailWriter> thumbnails_;
  std::optional<multitap::ThumbnailWriter::Target> thumbnail_target_;
  std::shared_ptr<multitap::ThumbnailWriter::Ticket> thumbnail_ticket_;
  bool thumbnail_privacy_locked_ = false;
  int64_t thumbnail_lease_ = 0;
  uint64_t thumbnail_serial_ = 0;
  bool allow_session_windows_ = true;
  FlutterWindow* session_owner_ = nullptr;
  std::string session_key_;
  std::string authentication_owner_;
  bool authentication_retired_ = false;
  ULONGLONG authentication_activity_tick_ = 0;
  ULONGLONG authentication_deadline_tick_ = 0;
  void AuthenticationHuman();
  void HideAuthentication();
  bool CheckAuthenticationDeadline();
  bool SetAuthenticationDeadline(std::optional<int64_t> remaining);
  flutter::EncodableValue session_payload_;
  bool session_bootstrapped_ = false, session_ready_ = false;
  bool session_started_ = false, session_destroyed_ = false;
  bool session_teardown_in_progress_ = false;
  bool first_frame_completed_ = false;
  std::string session_close_reason_;
  std::vector<std::unique_ptr<FlutterWindow>> session_windows_;
  std::unique_ptr<multitap::ConsoleWebViews> console_webviews_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> sessions_channel_;
  std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> sessions_close_result_;
  bool sessions_closing_ = false, close_parent_when_sessions_closed_ = false;
  // The update installer asked the dashboard to close while remote windows were open.
  bool close_for_update_ = false;
  bool session_window_opening_ = false;
  std::string sessions_close_reason_;
  ULONGLONG sessions_close_deadline_ = 0;
#ifdef MULTITAP_SESSION_WINDOWS_QA
  int32_t qa_teardown_checks_ = 0, qa_teardown_drain_checks_ = 0, qa_teardown_failures_ = 0;
  int32_t qa_update_reveal_requests_ = 0;
#endif

  // The project to run.
  flutter::DartProject project_;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      credential_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> input_channel_;
  multitap::PasswordIme password_ime_;
  int64_t password_scope_ = 0;
  std::unique_ptr<flutter::PluginRegistrarWindows> remote_registrar_;
  // Whether this view's Flutter renderer is a hardware GPU (stream statistics).
  multitap::RenderEnvironment render_environment_;
  std::unique_ptr<multitap::RemoteMediaSession> remote_media_;
  std::unique_ptr<multitap::UdpMediaChannel> udp_media_;
  std::unique_ptr<multitap::StreamSizeQueryChannel> stream_size_queries_;
  std::unique_ptr<multitap::RemoteInput> remote_input_;
  // cursor-v1: the host's separated cursor as this window's real cursor.
  std::unique_ptr<multitap::CursorPresenter> cursor_presenter_;
  void ResetRemoteInput();
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> remote_channel_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> remote_events_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> remote_sink_;
  multitap::WindowPresentation presentation_;
  // A remote view opted in: maximizing the window asks Dart for fullscreen
  // instead of a maximized window with its title bar.
  bool maximize_to_fullscreen_ = false;
  multitap::WindowCloseGuard close_guard_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> close_channel_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> window_channel_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> window_events_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> window_sink_;
  std::unique_ptr<multitap::ClientClipboardChannel> clipboard_worker_;
  std::unique_ptr<multitap::ProductUpdateTask> update_worker_;
  bool host_settings_ = false;
  multitap::relaunch::Product UpdateProduct() const;
  // The running product version an update replaces: the client EXE build, or
  // the installed Host version of this settings EXE. Empty when unknown.
  std::string RunningUpdateVersion() const;
  bool ApplyUpdatePlacement(const WINDOWPLACEMENT& placement, int show_command);
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> clipboard_channel_;
  // print-v1: local printing of what a VM printed (client_print.h).
  std::unique_ptr<multitap::ClientPrintChannel> print_worker_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> print_channel_;
  // The clipboard transfer card outside the window and the taskbar progress (transfer_notice.h).
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> transfer_notice_channel_;
  std::shared_ptr<std::atomic_bool> transfer_notice_alive_;
  uint64_t transfer_notice_observer_=0;
  // Where a paste on this PC goes, and 폴더 열기 (paste_target.h).
  std::unique_ptr<multitap::PasteTargetChannel> paste_target_worker_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> paste_target_channel_;
  // drive-v1: folders shared with a streaming VM (drive_channel.h).
  std::unique_ptr<multitap::ClientDriveChannel> drive_worker_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> drive_channel_;
  // usb-v1: this PC's USB devices for the VM (usb_capture.h).
  std::unique_ptr<multitap::UsbCaptureChannel> usb_worker_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> usb_channel_;
  // Session windows only: relative-mouse-v1 capture (relative_mouse.h).
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> relative_channel_;
  multitap::RelativeMouse relative_mouse_;
  // 게임 마우스 over USB: the virtual mouse device (game_mouse_usb_channel.h).
  std::unique_ptr<multitap::GameMouseUsbChannel> game_mouse_usb_;
  // gamepad-v1: this PC's XInput controllers (gamepad_input.h).
  std::unique_ptr<multitap::GamepadInput> gamepad_input_;
  void ConfigureRelativeMouse();
  void RestorePresentation();
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
