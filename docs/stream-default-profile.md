# New-connection stream defaults

New Windows and Android connections start at **60 fps with automatic
bitrate** ([adaptive-bitrate.md](adaptive-bitrate.md)). Only changed pictures
are encoded, so a still desktop costs little at 60 fps, and automatic bitrate
follows what the path carries up to the Host's ceiling (at most 100 Mbps).

| Where | Default | Code |
|---|---|---|
| Windows | `StreamOptions.recommended`: 60 fps, `bitrateMode: auto` | `apps/windows_client/lib/models/stream_options.dart` |
| Android | `AndroidStreamQuality.recommended`: 60 fps, `bitrate: automatic` | `apps/windows_client/lib/android/android_store.dart` |

- **Windows.** Every new connection starts from `StreamOptions.recommended`.
  A choice made in 스트리밍 설정 before connecting, or in the session's
  settings panel, applies to that connection and its reconnects.
- **Android.** The device remembers its quality choice (frame rate, bitrate,
  sound). A valid saved choice is kept as it is; a missing or invalid one uses
  the recommendation. Android offers 자동 or a fixed 4, 8, 12, 16 or 20 Mbps.
- **Presets.** Both platforms show the same two modes (`StreamPreset`):
  작업 = 60 fps, automatic bitrate (the default) and 절약 = 30 fps, fixed
  8 Mbps for mobile data and weak PCs. Any other combination reads as a custom
  choice.
- **Older Hosts and independent outputs.** `StreamOptions.negotiated()` turns
  automatic bitrate into a fixed 20 Mbps where the Host does not advertise
  `bitrate-adaptive-v1`, and for two independent outputs (tracks-v1), which
  keep a fixed rate. A manual bitrate is clamped to 4 Mbps–the Host's ceiling.
- **Wire defaults.** The plain `StreamOptions()` constructor stays fixed
  20 Mbps at 60 fps, and fixed options keep the original wire shape (no
  `bitrateMode` field). The broker and Host accept 30 or 60 fps.
