# Windows CPU colour and conversion checks

This standalone QA executable compiles the Host's `media.cpp` and the client's
`remote_decoder.cpp`. It never captures the desktop, connects to a VM, injects
input, reads a clipboard or changes display or driver settings.

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/media-quality-qa -B <build> -A x64
& $cmake --build <build> --config Release
& <build>/Release/media-quality-qa.exe
```

It checks the exact scalar/SIMD forward conversion, all four inverse
matrix/range combinations (BT.601/BT.709, limited/full) with padded and
unaligned planes, boundary patterns and buffer guards, and a colour-bar round
trip through the production H.264 encoder and decoder. Colour-bar centres may
differ by five RGB levels (lossy codec); conversion comparisons allow none. A
decoder callback is not a Flutter presentation measurement.

The conversion and codec microbenchmarks use generated pixels in this process.
They exclude capture, relay/network delay and presentation, and say nothing
about remote frame rate or input latency. Compare before/after runs under
comparable load.
