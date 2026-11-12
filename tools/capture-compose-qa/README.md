# Capture composition QA

This standalone native benchmark draws only into memory DIBs. It does not read
a desktop, start a service, change a display, send input or use a relay. It
compares a HALFTONE `StretchDIBits` composition with the exact unscaled
composition helper used by `Capture::Modern`.

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/capture-compose-qa -B <build> -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$($PWD.Path -replace '\\','/')/.tools/visualstudio-buildtools"
& $cmake --build <build> --config Release --target MultitapCaptureComposeQa --parallel 4
& $cmake --build <build>/host --config Release --target MultitapStreamMetricsTest --parallel 4
& <build>/Release/MultitapCaptureComposeQa.exe
& <build>/host/Release/MultitapStreamMetricsTest.exe
```

Both targets run without administrator rights and install nothing. The Host
project is included as a subdirectory and excluded from the default build.

Each composition path gets 30 warm-up and 150 measured iterations per geometry,
in alternating order. Timings include `GdiFlush` before direct DIB access. The
program compares every RGB byte and the encoder's NV12 conversion result, and
exercises horizontal and vertical two-surface layouts, cached cursor base
restoration and rejection of invalid geometry and sizes with guarded unaligned
buffers. A nonzero exit means a failed invariant.

`MultitapStreamMetricsTest` uses the product CPU encoder, native decoder and
remote engine with a fake texture registrar and audio disabled. Its
deliberately paused notification callback exercises queue and lifetime
behaviour; do not use it as a latency benchmark.

Composition timings are neither fresh capture FPS nor end-to-end latency.
