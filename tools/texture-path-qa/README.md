# Texture path QA

A real Flutter Windows view that shows the client's `RemoteEngine` texture
(Microsoft CPU H.264 decoder, pixel-buffer texture) fed with synthetic H.264
from the Microsoft encoder MFT. It measures decoded versus Flutter-taken
frames, Flutter's per-take upload (pixel-buffer callback → release, i.e. the
`glTexImage2D` call), the publish → take wait (engines with texture pacing)
and process CPU. No broker, relay, VM or network is used; the toolbar is a
stand-in widget, not the product `RemoteView`.

```powershell
$new  = tools/texture-path-qa/build.ps1 -OutDir C:\tq
$base = tools/texture-path-qa/build.ps1 -OutDir C:\tq -Ref <commit before the change>
tools/texture-path-qa/compare.ps1 -Base $base -New $new -Out C:\tq\compare.jsonl
tools/texture-path-qa/summarize.ps1 -In C:\tq\compare.jsonl
```

`build.ps1` creates a Windows app with `flutter create`, adds this folder's
sources and links the client engine of the working tree (or of git ref `-Ref`,
for a before/after pair); it prints the path of the built `texture_qa.exe`.
Keep `-OutDir` short (MAX_PATH). `compare.ps1` runs each case with both builds,
interleaved (`-Repeat`, `-Seconds`, `-Cases`; a case is
`label|width|height|fps|pattern|hz|jitterMs|driftPpm`, see the script), and
appends one JSON line per run; `summarize.ps1` averages them per case and
engine.

`--hz=N` answers Flutter's (delay-loaded) `DwmGetCompositionTimingInfo` with
N Hz so that Flutter schedules frames as on a local N Hz display. Inside a
remote desktop session DWM composes at about 32 Hz, which caps Flutter there;
the hook changes Flutter's frame scheduling only, not what DWM shows. Results
describe the machine they ran on: on a GPU-less VM Flutter renders with WARP /
the Basic Render Driver, so they are not hardware-GPU numbers.
