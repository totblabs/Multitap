#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <windows.h>
#include <dwmapi.h>
#include <delayimp.h>
#include <cstdio>
#include <cstring>

#include "flutter_window.h"
#include "utils.h"

// Harness only: --hz=N makes Flutter's Windows embedder schedule frames at N Hz
// by answering its (delay-loaded) DwmGetCompositionTimingInfo. This RDP session
// composes at 32 Hz; the hook models a local N Hz display for Flutter's frame
// scheduling (texture takes). It does not change what DWM/RDP actually shows.
static UINT g_hz = 0;
static HRESULT WINAPI TimingHook(HWND hwnd, DWM_TIMING_INFO* info) {
  const HRESULT hr = DwmGetCompositionTimingInfo(hwnd, info);
  if (info && g_hz) { info->rateRefresh.uiNumerator = g_hz; info->rateRefresh.uiDenominator = 1; }
  return hr;
}
static bool PatchDelayImport(HMODULE module, const char* dll, const char* function, void* replacement) {
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
  if (!dir.VirtualAddress) return false;
  for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress); d->DllNameRVA; ++d) {
    if (_stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), dll) != 0) continue;
    auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA);
    auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA);
    for (; names->u1.AddressOfData; ++names, ++slots) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto* name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(name->Name), function) != 0) continue;
      DWORD old = 0;
      VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
      slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
      VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
      return true;
    }
  }
  return false;
}

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  unsigned int window_width = 1280, window_height = 800;
  for (const auto& argument : command_line_arguments) {
    if (argument.rfind("--window=", 0) == 0) sscanf_s(argument.c_str() + 9, "%ux%u", &window_width, &window_height);
    if (argument.rfind("--hz=", 0) == 0) sscanf_s(argument.c_str() + 5, "%u", &g_hz);
  }
  if (g_hz && !PatchDelayImport(GetModuleHandleW(L"flutter_windows.dll"), "dwmapi.dll", "DwmGetCompositionTimingInfo", reinterpret_cast<void*>(&TimingHook)))
    return 3;
  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  FlutterWindow window(project);
  Win32Window::Point origin(10, 10);
  Win32Window::Size size(window_width, window_height);
  if (!window.Create(L"texture_qa", origin, size)) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
