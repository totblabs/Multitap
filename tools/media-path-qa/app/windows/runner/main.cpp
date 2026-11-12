#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "flutter_window.h"
#include "utils.h"

// Media path harness. `--burner=N` (internal) runs N normal-priority busy
// threads and nothing else; the harness starts it as a child process to load
// the PC the way parallel builds do.
static int Burner(int threads) {
  std::vector<std::thread> busy; std::atomic<bool> forever{true};
  for (int i = 0; i < threads; ++i) busy.emplace_back([&] { volatile uint64_t x = 0; while (forever) x = x * 6364136223846793005ull + 1; });
  for (auto& thread : busy) thread.join();
  return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t* command_line, _In_ int show_command) {
  std::vector<std::string> command_line_arguments = GetCommandLineArguments();
  for (const auto& argument : command_line_arguments)
    if (argument.rfind("--burner=", 0) == 0) return Burner(std::atoi(argument.c_str() + 9));
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) CreateAndAttachConsole();
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  flutter::DartProject project(L"data");
  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));
  FlutterWindow window(project);
  Win32Window::Point origin(10, 10);
  Win32Window::Size size(1280, 800);
  if (!window.Create(L"media_qa", origin, size)) return EXIT_FAILURE;
  window.SetQuitOnClose(true);
  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) { ::TranslateMessage(&msg); ::DispatchMessage(&msg); }
  ::CoUninitialize();
  return EXIT_SUCCESS;
}
