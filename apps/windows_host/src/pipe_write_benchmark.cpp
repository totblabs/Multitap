// Worker -> service pipe write benchmark (pipe_packet.hpp).
// The interactive worker's capture/encode thread writes every encoded frame to
// the service through this pipe synchronously, so the time of that write is part
// of the "encode" stage the Host reports. This replays a bimodal frame size mix
// (many small P frames, some 100-300 KB, a few 0.6-1.3 MB scene-change/IDR
// frames) at 60 Hz into a pipe whose reader plays the service (normal priority,
// like the real service; the writer is one step above, like the worker's
// ABOVE_NORMAL class), optionally while other processes keep the CPU busy.
// Synthetic payloads only: no desktop, network or product state is touched.
//
//   MultitapHostPipeWriteBenchmark [frames=900] [variants=legacy64,new64,new4m]
//   legacy64: the previous code (event per partial transfer, 3 writes per packet) and a 64 KiB buffer
//   new64:    current packet code, 64 KiB buffer      new4m: current code, workerPipeInboundBytes\n//   new64rd / new4mrd: the same with the reader (service) thread one priority step higher
#include "pipe_packet.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <thread>

namespace {
using host::Clock;
double percentile(std::vector<double> v, double f) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); size_t i = size_t(std::ceil(f * double(v.size()))); return v[std::max<size_t>(i, 1) - 1]; }

// The code as it was before pipe_packet.hpp (service.hpp), kept for the A/B.
bool legacyBytes(HANDLE pipe, bool writing, void* buffer, DWORD size, const std::atomic<bool>& stopped) {
  auto* at = static_cast<BYTE*>(buffer);
  while (size && !stopped) {
    host::WinHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr)); OVERLAPPED operation{}; operation.hEvent = event.value; DWORD transferred = 0;
    BOOL result = writing ? WriteFile(pipe, at, size, &transferred, &operation) : ReadFile(pipe, at, size, &transferred, &operation);
    if (!result && GetLastError() == ERROR_IO_PENDING) {
      while (WaitForSingleObject(event.value, 100) == WAIT_TIMEOUT) if (stopped) { CancelIoEx(pipe, &operation); GetOverlappedResult(pipe, &operation, &transferred, TRUE); return false; }
      result = GetOverlappedResult(pipe, &operation, &transferred, FALSE);
    }
    if (!result || !transferred) return false; at += transferred; size -= transferred;
  }
  return size == 0;
}
bool legacyPacket(HANDLE pipe, bool text, std::vector<uint8_t> bytes, const std::atomic<bool>& stopped, std::mutex& mutex) {
  std::lock_guard lock(mutex); uint32_t length = uint32_t(bytes.size() + 1); BYTE kind = text ? 0 : 1;
  return legacyBytes(pipe, true, &length, 4, stopped) && legacyBytes(pipe, true, &kind, 1, stopped) && legacyBytes(pipe, true, bytes.data(), DWORD(bytes.size()), stopped);
}

struct Variant { std::string name; bool legacy; DWORD buffer; bool readerHigh = false; };
struct Result { std::string name; size_t frames = 0, large = 0; std::vector<double> all, big; };

Result run(const Variant& variant, int frames) {
  const std::wstring name = L"\\\\.\\pipe\\MultitapPipeBenchmark." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
  host::WinHandle server(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, variant.buffer ? 65536 : 65536, variant.buffer, 0, nullptr));
  if (server.value == INVALID_HANDLE_VALUE) throw std::runtime_error("pipe");
  host::WinHandle client(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
  if (client.value == INVALID_HANDLE_VALUE) throw std::runtime_error("pipe client");
  std::atomic<bool> stopped{false}, readerStop{false}; std::mutex writing;
  std::thread reader([&] {
    SetThreadPriority(GetCurrentThread(), variant.readerHigh ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL);
    bool text = false; std::vector<uint8_t> bytes;
    while (!readerStop && host::readPipePacket(server.value, text, bytes, readerStop)) { std::vector<uint8_t> forwarded(bytes); (void)forwarded; }
  });
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
  uint32_t seed = 20261027; const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  Result result; result.name = variant.name;
  const auto start = Clock::now();
  for (int i = 0; i < frames; ++i) {
    const uint32_t pick = next() % 100;
    const size_t size = pick < 85 ? 3000 + next() % 37000 : pick < 97 ? 80000 + next() % 220000 : 600000 + next() % 700000;
    std::vector<uint8_t> packet(size, uint8_t(i)); packet[0] = 1;
    const auto deadline = start + std::chrono::microseconds(int64_t(i) * 1000000 / 60); const auto now = Clock::now();
    if (deadline > now) { LARGE_INTEGER due; due.QuadPart = -int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count() / 100); SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE); WaitForSingleObject(timer, INFINITE); }
    LARGE_INTEGER a{}, b{}; QueryPerformanceCounter(&a);
    const bool ok = variant.legacy ? legacyPacket(client.value, false, std::move(packet), stopped, writing) : host::pipePacket(client.value, false, std::move(packet), stopped, writing);
    QueryPerformanceCounter(&b);
    if (!ok) throw std::runtime_error("write failed");
    const double ms = double(b.QuadPart - a.QuadPart) * 1000.0 / double(frequency.QuadPart);
    result.all.push_back(ms); ++result.frames; if (size > 65536) { result.big.push_back(ms); ++result.large; }
  }
  CloseHandle(timer);
  readerStop = true; CancelIoEx(server.value, nullptr); client = host::WinHandle(); reader.join();
  return result;
}
}

int wmain(int argc, wchar_t** argv) {
  try {
    int frames = 900; std::vector<std::string> names{"legacy64", "new64", "new4m"};
    for (int a = 1; a < argc; ++a) {
      std::string n; for (const wchar_t* c = argv[a]; *c; ++c) n.push_back(char(*c));
      if (n.rfind("frames=", 0) == 0) frames = std::stoi(n.substr(7));
      else if (n.rfind("variants=", 0) == 0) { names.clear(); std::stringstream s(n.substr(9)); std::string item; while (std::getline(s, item, ',')) names.push_back(item); }
      else throw std::runtime_error("Unknown option " + n);
    }
    for (const auto& name : names) {
      const Variant variant = name == "legacy64" ? Variant{name, true, 65536} : name == "new64" ? Variant{name, false, 65536} : name == "new64rd" ? Variant{name, false, 65536, true} : name == "new4mrd" ? Variant{name, false, host::workerPipeInboundBytes, true} : Variant{name, false, host::workerPipeInboundBytes};
      const auto r = run(variant, frames);
      std::cout << std::fixed << std::setprecision(2) << "{\"variant\":\"" << r.name << "\",\"frames\":" << r.frames << ",\"largeFrames\":" << r.large
                << ",\"writeP50Ms\":" << percentile(r.all, .5) << ",\"writeP90Ms\":" << percentile(r.all, .9) << ",\"writeP99Ms\":" << percentile(r.all, .99) << ",\"writeMaxMs\":" << percentile(r.all, 1)
                << ",\"largeP50Ms\":" << percentile(r.big, .5) << ",\"largeP90Ms\":" << percentile(r.big, .9) << ",\"largeMaxMs\":" << percentile(r.big, 1) << "}" << std::endl;
    }
    return 0;
  } catch (const std::exception& e) { std::cerr << "fatal: " << e.what() << "\n"; return 1; }
}
