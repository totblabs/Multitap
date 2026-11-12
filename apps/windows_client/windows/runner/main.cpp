#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <algorithm>

#include "flutter_window.h"
#include "utils.h"
#include "single_instance.hpp"
#include "product_update.hpp"
#include "../../../../native/windows_admin/admin_required.hpp"
#include "ui_language.h"

#if defined(MULTITAP_SESSION_WINDOWS_QA) && !defined(MULTITAP_SESSION_WINDOWS_QA_VALIDATED)
#error Session-window QA must be configured with its validated isolated Dart target.
#endif
#if defined(MULTITAP_SESSION_WINDOWS_QA_VALIDATED) && !defined(MULTITAP_SESSION_WINDOWS_QA)
#error The isolated QA validation marker cannot be used in a product build.
#endif

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
#ifndef MULTITAP_SESSION_WINDOWS_QA_VALIDATED
  {
    // The elevated update instance (product_update.hpp): started by the client
    // itself through UAC to stage and start a verified installer from the
    // administrator-only folder. It is not the user's application: no window,
    // no COM/OLE, no single-instance mutex, and nothing from the command line
    // reaches the Dart entrypoint.
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    const bool staged_update = argv && argc >= 2 && std::wstring(argv[1]) == multitap::kApplyStagedUpdateFlag;
    const int staged_exit = staged_update ? multitap::RunStagedUpdateHelper(argc, argv) : 0;
    if (argv) ::LocalFree(argv);
    if (staged_update) return staged_exit;
  }
#endif

  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();
  wchar_t executable[MAX_PATH]{};
  const DWORD path_length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  const wchar_t* filename = wcsrchr(executable, L'\\');
  bool host_settings = path_length > 0 && path_length < MAX_PATH &&
      _wcsicmp(filename ? filename + 1 : executable, L"MultitapHostSettings.exe") == 0;
  for (const auto& argument : command_line_arguments) if (argument == "--host-settings") host_settings = true;
#ifndef MULTITAP_SESSION_WINDOWS_QA_VALIDATED
  // The client runs unelevated. Host settings change
  // the service, firewall and drivers and need administrator approval: the
  // Host package's MultitapHostSettings.exe carries a requireAdministrator
  // manifest (patched at build time); a plain client EXE asked for Host settings
  // restarts itself through UAC. Declining the prompt ends here.
  if (host_settings && !multitap::administratorToken()) {
    SHELLEXECUTEINFOW elevate{};
    elevate.cbSize = sizeof(elevate);
    elevate.fMask = SEE_MASK_NOASYNC;
    elevate.lpVerb = L"runas";
    elevate.lpFile = executable;
    elevate.lpParameters = command_line;
    elevate.nShow = show_command;
    return ::ShellExecuteExW(&elevate) ? EXIT_SUCCESS : static_cast<int>(::GetLastError());
  }
#endif

#if defined(MULTITAP_WINDOW_LAYOUT_QA) || defined(MULTITAP_SESSION_WINDOWS_QA)
  // This compile-only QA target never competes with an installed client and
  // cannot submit credentials or leave another privileged test window open.
  multitap::SingleInstance single_instance(L"Multitap.LayoutQa."+std::to_wstring(GetCurrentProcessId()),false);
#else
  multitap::SingleInstance single_instance;
#endif
  if (!single_instance.Acquire(host_settings)) return EXIT_SUCCESS;
  // "--restore-existing" (the installer, before it closes the client): a client
  // resting in the tray has no visible window to receive a close request. The
  // single-instance check above has just shown it; with none running there is
  // nothing to show, and no window of this process is created.
  if (std::find(command_line_arguments.begin(), command_line_arguments.end(),
                "--restore-existing") != command_line_arguments.end())
    return EXIT_SUCCESS;
  ::OleInitialize(nullptr);

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  FlutterWindow window(project, !host_settings);
  window.SetHostSettings(host_settings);
  window.SetMinimumSize(Win32Window::Size(host_settings ? 600 : 760,host_settings ? 520 : 560));
  Win32Window::Point origin(60, 50);
  // Host settings opens at the size its overview was designed for (680×760
  // inside the frame): the status, the route, one banner
  // and every area fit without scrolling. A smaller work area still fits it.
  Win32Window::Size size(host_settings ? 696 : 1440, host_settings ? 800 : 900);
  if (!window.Create(host_settings ? L"Multitap Host" : L"Multitap", origin, size)) {
    window.Destroy();
    ::OleUninitialize();
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);
#if !defined(MULTITAP_WINDOW_LAYOUT_QA) && !defined(MULTITAP_SESSION_WINDOWS_QA)
  // After an update the installer relaunched: reopen where the window was.
  window.RestoreUpdatePlacement();
#endif
  single_instance.Register(window.GetHandle());
#ifdef MULTITAP_WINDOW_LAYOUT_QA
  SetWindowTextW(window.GetHandle(),multitap::Tr(L"Multitap · 화면 배치 검사 · 자동 닫힘", L"Multitap · display layout check · closes by itself"));
  EnableWindow(window.GetHandle(),FALSE);
  SetTimer(window.GetHandle(),19026,20000,[](HWND target,UINT,UINT_PTR,DWORD){DestroyWindow(target);});
#endif

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0) > 0) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
    // Never erase a child from its own callback or a nested message pump.
    window.CollectClosedSessionWindows();
  }

  window.Destroy();
  window.CollectClosedSessionWindows();
  ::OleUninitialize();
  return EXIT_SUCCESS;
}
