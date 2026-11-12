# Text quality and codec-support probe

A small synthetic check of how H.264 4:2:0 treats coloured desktop text. It
changes no product setting, service, display, VM or account.

`MultitapTextQualityQa` draws a memory-only 1920×1080 DIB with Malgun Gothic
Korean/English text at 12, 14, 16 and 20 px in black, blue and red; three
streams add moving colour content below the text. The product NV12 conversion
and the Host's default encoder (`host::Encoder`, Constrained Baseline) code 12
frames per case: static at 12 Mbps and moving at 4, 12 and 20 Mbps. The process
runs below normal priority on two logical processors.

`measure.mjs` then decodes every frame independently with an FFmpeg build you
provide (`-xerror -err_detect explode -hwaccel none`, two decode threads; FFmpeg
is QA-only and not part of the product), converts the raw NV12 and the decoded
H.264 with the same explicit BT.601 limited-range matrix, and compares both
with the original RGB. `colourInk` samples only visibly coloured glyph pixels
in the text area; the full-region PSNR also includes the white background.
These are relative fixture measurements, not readability scores. On a test VM
with the Microsoft MFT encoder, the coloured-glyph PSNR moved by only ~0.2 dB
between 4 and 20 Mbps and stayed below that of the raw NV12 round trip: the
4:2:0 chroma sampling, not the bitrate, limits coloured text. This is what
lossless tiles address ([lossless-tiles-design.md](../../docs/lossless-tiles-design.md)).

`MultitapTextQualityQa.exe --probe` runs only a type-negotiation probe of the
installed Microsoft H.264 MFT (profiles and input formats accepted by an actual
`SetOutputType`/`SetInputType`, not just `MFT_SET_TYPE_TEST_ONLY`) without
encoding a scene. The product policy rejects High 4:4:4 and has no lossless
H.264 mode; a profile value that the MFT merely accepts in a test-only call is
no reason to enable one.

## Run

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S tools/text-quality-qa -B <build> -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$($PWD.Path -replace '\\','/')/.tools/visualstudio-buildtools"
& $cmake --build <build> --config Release --target MultitapTextQualityQa --parallel 2
node tools/text-quality-qa/measure.mjs <build>/Release/MultitapTextQualityQa.exe <ffmpeg.exe> <ffprobe.exe> <new result directory>
```

The Host subproject is excluded from the default build. The runner refuses to
overwrite an existing result directory.
