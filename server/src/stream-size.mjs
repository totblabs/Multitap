import { HttpError } from './errors.mjs';
import { videoGeometry, boundedInteger, maximumGeometrySide, maximumWireInteger } from './video-geometry.mjs';

// Negotiated stream sizes (stream-size-v1; see docs/remote-session-protocol.md
// "Stream sizes"). A viewport stream is encoded at its source's own size; the
// Host reduces it only for the client decoder's declared limits, the client's
// measured decode load, its own measured encoder overload or an optional
// administrator cap. Owned outputs and encoded pictures may be portrait up to
// 2160x3840 (the longer side stays at most 3840, at most 8,294,400 pixels).
// Every field exists only in a session whose client requested it, whose Host
// advertised it and whose broker granted it; released peers never see it.
export const streamSizeProtocol = 'stream-size-v1';
export const streamSizeProtocolV2 = 'stream-size-v2';
export const maximumSide = 3840;
export const maximumPixels = 8294400;
// Released single-output bound: 3840x2160.
const releasedHeight = 2160;

// An owned output / requested viewport size. Granted sessions allow portrait.
export function ownedOutputSize(width, height, portrait = false) {
  if (portrait === streamSizeProtocolV2) return videoGeometry(width, height, 320, 180);
  return Number.isInteger(width) && Number.isInteger(height) && width % 2 === 0 && height % 2 === 0 &&
    width >= 320 && height >= 180 && width <= maximumSide && height <= (portrait ? maximumSide : releasedHeight) && width * height <= maximumPixels;
}

// An encoded picture (stream.info width/height).
export function encodedSize(width, height, portrait = false) {
  if (portrait === streamSizeProtocolV2) return videoGeometry(width, height);
  return Number.isInteger(width) && Number.isInteger(height) && width % 2 === 0 && height % 2 === 0 && width >= 2 && height >= 2 &&
    width <= maximumSide && height <= (portrait ? maximumSide : releasedHeight) && width * height <= maximumPixels;
}

// What the client's decoder takes: sides, pixels per frame and pixels per
// second (0 = not declared: a software decoder whose rate is measured live).
export function validDecoderLimits(value, protocol = streamSizeProtocol) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  const keys = Object.keys(value).sort().join(',');
  if (protocol === streamSizeProtocolV2) {
    return keys === 'maxHeight,maxPixelRate,maxPixels,maxWidth' &&
      boundedInteger(value.maxWidth, 16, maximumGeometrySide) && boundedInteger(value.maxHeight, 16, maximumGeometrySide) &&
      boundedInteger(value.maxPixels, 256) &&
      (value.maxPixelRate === 0 || boundedInteger(value.maxPixelRate, 256));
  }
  const integer = (number, min, max) => Number.isInteger(number) && number >= min && number <= max;
  return keys === 'maxHeight,maxPixelRate,maxPixels,maxWidth' &&
    integer(value.maxWidth, 320, maximumSide) && integer(value.maxHeight, 180, maximumSide) &&
    integer(value.maxPixels, 320 * 180, maximumPixels) &&
    (value.maxPixelRate === 0 || integer(value.maxPixelRate, 320 * 180 * 30, maximumPixels * 60));
}

// Host capability: exactly the marker, on a Host that also advertises the
// viewport encoder ceilings (separate-v1) it extends; anything else is omitted.
export function streamSizeCapability(value, protocol = value?.streamSizeProtocol) {
  if (protocol === streamSizeProtocolV2) return (value?.streamSizeProtocol === streamSizeProtocolV2 ||
    (Array.isArray(value?.streamSizeProtocols) && value.streamSizeProtocols.length<=2 && new Set(value.streamSizeProtocols).size===value.streamSizeProtocols.length &&
     value.streamSizeProtocols.every(p=>p===streamSizeProtocol||p===streamSizeProtocolV2) && value.streamSizeProtocols.includes(streamSizeProtocolV2))) &&
    (value.viewportEncoderLimitsProtocol === 'separate-v1' || value.videoTrackProtocol === 'tracks-v1');
  return protocol === streamSizeProtocol && [streamSizeProtocol, streamSizeProtocolV2].includes(value?.streamSizeProtocol) &&
    value.viewportEncoderLimitsProtocol === 'separate-v1';
}

// The connect body's own declaration (before full validation), so that the
// options box and the initial viewport may already use the portrait bounds.
export function streamSizeAsked(protocol) {
  return protocol === streamSizeProtocol || protocol === streamSizeProtocolV2;
}

// Connect body fields. Only an ordinary stream session with an initial
// viewport and separate-v1 encoder ceilings may ask; malformed requests are
// refused before any session. Returns the declared limits, or null.
export function requestedStreamSize(mode, protocol, limits, viewportEncoderLimits, tracks) {
  if (protocol === undefined && limits === undefined) return null;
  const supported = streamSizeAsked(protocol) && (protocol === streamSizeProtocolV2 ? (!!tracks || viewportEncoderLimits) : (!tracks && viewportEncoderLimits));
  if (mode !== 'stream' || !supported || !validDecoderLimits(limits, protocol)) throw new HttpError(400, 'invalid_stream_size');
  return Object.freeze({ maxWidth: limits.maxWidth, maxHeight: limits.maxHeight, maxPixels: limits.maxPixels, maxPixelRate: limits.maxPixelRate });
}

// Never a reason to refuse by itself: an incapable Host leaves it ungranted.
// A request whose geometry needs the grant (a portrait viewport or a box
// beyond the released 3840x2160) is refused with 409 when it is not granted.
export function negotiateStreamSize(requested, host, tracks, viewport, options, protocol = streamSizeProtocol) {
  const granted = requested !== null && (!tracks || protocol === streamSizeProtocolV2) && streamSizeCapability(host?.capabilities, protocol) ? requested : null;
  if (requested !== null && protocol === streamSizeProtocolV2 && !granted) throw new HttpError(409, 'stream_size_unavailable');
  if (requested !== null && !granted && ((viewport && !ownedOutputSize(viewport.width, viewport.height)) || (options && (options.maxHeight > releasedHeight || options.maxWidth * options.maxHeight > maximumPixels))))
    throw new HttpError(409, 'stream_size_unavailable');
  return granted;
}

function macroblocks(width, height) { return Math.ceil(width / 16) * Math.ceil(height / 16); }

// Encoded geometry of a granted session within the client's declared decoder
// limits (frame size in whole macroblocks, as decoders count it).
export function withinDecoderLimits(info, limits, fps) {
  if (!limits) return true;
  if (!videoGeometry(info.width, info.height) || !boundedInteger(fps, 1, 60)) return false;
  const blocks = macroblocks(info.width, info.height);
  return blocks <= Math.floor(maximumWireInteger / 256) && info.width <= limits.maxWidth && info.height <= limits.maxHeight &&
    blocks <= Math.floor(limits.maxPixels / 256) && (limits.maxPixelRate === 0 || blocks <= Math.floor(limits.maxPixelRate / 256 / fps));
}

// A Host's mid-epoch picture change (its encoder restarted at another scale):
// same session, epoch, source and frame rate; a picture no larger than the
// source, the box and the decoder limits.
export function rescaledInfoValid(info, session) {
  const source = session.source;
  return !!source && info.sessionId === session.id && info.epoch === session.epoch &&
    info.sourceWidth === source.width && info.sourceHeight === source.height && info.fpsTarget === session.options.fps &&
    info.width <= Math.min(source.width, session.options.maxWidth) && info.height <= Math.min(source.height, session.options.maxHeight) &&
    withinDecoderLimits(info, session.streamSize, session.options.fps);
}

// One client decoder report: exactly {type, busy, backlogMs}. busy: per mille
// of the last second the decoder was busy (-1 = not measured); backlogMs: age
// of the oldest packet still waiting to be decoded.
export function validDecoderReport(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  return Object.keys(value).sort().join(',') === 'backlogMs,busy,type' && value.type === 'stream.decoder' &&
    Number.isInteger(value.busy) && value.busy >= -1 && value.busy <= 1000 &&
    Number.isInteger(value.backlogMs) && value.backlogMs >= 0 && value.backlogMs <= 60000;
}
