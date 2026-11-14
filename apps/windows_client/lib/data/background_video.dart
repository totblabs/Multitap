/// background-video-v1 (docs/remote-session-protocol.md "Background video"):
/// while the remote picture cannot be seen (the Android app in the background,
/// the Windows app minimised) the client tells the Host
/// (`{"type":"stream.background","on":true}`), and the Host sends one picture
/// a second instead of every frame, as for an idle screen. Sound and the
/// session go on; `on:false` brings every frame back, starting with an IDR.
/// Ordinary stream sessions only.
const backgroundVideoProtocol = 'background-video-v1';

/// Released brokers reject this connect field and strip this capability.
bool backgroundVideoAdvertised(Object? capabilities) =>
    capabilities is Map &&
    capabilities['backgroundVideoProtocol'] == backgroundVideoProtocol;

/// The client request: the view is hidden (on) or shown again (off).
Map<String, Object?> backgroundVideoMessage(bool hidden) => {
  'type': 'stream.background',
  'on': hidden,
};
