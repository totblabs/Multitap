# Host resolution geometry fixture

A standalone, pure fixture for the Host's frame geometry and memory checks:
`src/owned_output_size.hpp`, `src/capture_compose.hpp` and
`src/capture_scale.hpp` (with the shared `native/windows_video/frame_layout.hpp`).
No SDK, driver, display, capture, encoder or OS call is made.

It checks that large owned outputs are representable (3840×2160, 5120×1440,
5120×2880, 4320×7680, 3840×3840, 7680×2160) with exact BGRA and NV12 buffer
sizes; that zero, too-small and misaligned sizes and multiplication overflows
are rejected before allocation; exact, insufficient and overflowing memory
budgets for single and dual buffers; and that encoder geometry is checked
structurally only. "Representable" means the Host's buffers can hold it; the
virtual-display provider and Windows still have to accept the mode, and a
refused mode restores the saved display configuration.

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S apps/windows_host/test/resolution -B <build> -A x64
& $cmake --build <build> --config Release
& <build>/Release/host_resolution_geometry_qa.exe
```

The broader `MultitapHostResolutionV2Test` (Host CMake target, not built by
default; `src/resolution_v2_test.cpp`) runs the production parsers and display
coordinator over memory-only CCD/SDK snapshots: `stream-size-v1`/`v2`
negotiation and decoder limits, resize-candidate binding, compatibility with
v1 clients, large single and dual-output layouts, memory admission forecasts
for one and two outputs, and refusal of a provider mode before any side
effect.
