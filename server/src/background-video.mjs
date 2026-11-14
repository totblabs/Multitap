import { HttpError } from './errors.mjs';

// Background video (background-video-v1; docs/remote-session-protocol.md
// "Background video"): while the client's picture cannot be seen (the Android
// app in the background, the Windows app minimised) it tells the Host
// ({type:'stream.background', on}), and the Host captures and encodes one
// picture a second instead of every frame, as for an idle screen. Sound, the
// side channels and the session go on; coming back asks for an IDR at once.
// Ordinary stream sessions only (independent outputs keep every frame).
export const backgroundVideoProtocol = 'background-video-v1';

export function backgroundVideoCapability(value) {
  return value?.backgroundVideoProtocol === backgroundVideoProtocol;
}

// Connect body field. A malformed request is refused before any session.
export function requestedBackgroundVideo(mode, value) {
  if (value === undefined) return false;
  if (mode !== 'stream' || value !== backgroundVideoProtocol) throw new HttpError(400, 'invalid_background_video');
  return true;
}

export function negotiateBackgroundVideo(requested, host, tracks) {
  return requested === true && !tracks && backgroundVideoCapability(host?.capabilities);
}

// Exactly {type:'stream.background', on:boolean}.
export function validBackgroundVideo(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value);
  return keys.length === 2 && keys.includes('type') && keys.includes('on') && value.type === 'stream.background' && typeof value.on === 'boolean';
}
