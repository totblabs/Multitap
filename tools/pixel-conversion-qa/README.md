# Pixel conversion QA

Checks and times the Host's BGRA → NV12 (BT.601 limited range) conversion in
`native/windows_video/bgra_nv12_sse2.hpp`: the paired-row kernel (AVX2 where
the CPU and Windows support it) and its SSE2 kernel. It captures no display,
opens no session and changes no Windows setting.

- **Verification.** Both kernels are compared byte for byte against an
  independent scalar BT.601 limited-range reference: sizes from 2×2 to
  3840×2160, unaligned input and output, two-pixel tails,
  black/white/structured/random pixels, ignored alpha, destination sentinels
  and real guard pages after the buffers.
- **Timing.** `paired_rows_conversion_ab` (an older separate-pass conversion
  against the current kernel) and `avx2_kernel_ab` (SSE2 against AVX2) at
  1280×720 up to 5120×1440: non-inlined entry points, alternating A/B order,
  ten rounds of eighty frames per implementation, thread CPU time and a full
  output comparison outside the timed intervals. The reported p95 is the p95
  of **round means**, not a per-frame tail. Windows thread CPU time is
  quantised; wall times are reported separately.

The older conversion is not in this repository. Configure with
`-DPIXEL_CONVERSION_BASELINE=<dir>`, where `<dir>/baseline.hpp` defines
`baseline::bgraToNv12SeparatePasses(const uint8_t*, uint8_t*, int, int)`;
CMake refuses to configure without it, and the harness checks it against the
same scalar reference.

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/pixel-conversion-qa -B <build> -G 'Visual Studio 17 2022' -A x64 "-DPIXEL_CONVERSION_BASELINE=<dir>"
& $cmake --build <build> --config Release
& <build>/Release/pixel-conversion-qa.exe                # verification + timing
& <build>/Release/pixel-conversion-qa.exe --verify-only  # byte and bounds checks only
```

Codec-level colour checks with the production converter, encoder and decoder
are in `tools/media-quality-qa`. Neither tool measures capture, network,
presentation or end-to-end latency.
