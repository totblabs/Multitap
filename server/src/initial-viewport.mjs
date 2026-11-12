import { randomUUID } from 'node:crypto';
import { HttpError } from './errors.mjs';
import { ownedOutputSize } from './stream-size.mjs';

export const initialViewportProtocol = 'epoch-v1';
export const streamProfileViewportProtocol = 'epoch-v1';
export const viewportEncoderLimitsProtocol = 'separate-v1';
export const initialViewportPrepareMs = 90000;
const uuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i;
const object = value => value && typeof value === 'object' && !Array.isArray(value);

// Physical owned-display geometry is an explicit request. Encoder maximums
// remain independent and cannot implicitly authorize a display transaction.
// `portrait`: the request also asks for stream-size-v1, whose owned outputs
// may be portrait up to 2160x3840 (granted or refused later in connect).
export function requestedInitialViewport(mode, protocol, value, options, resizeProtocol, tracks, portrait = false) {
  if (protocol === undefined && value === undefined) return null;
  if (mode !== 'stream' || protocol !== initialViewportProtocol || resizeProtocol !== 'epoch-v1' || tracks ||
      !object(value) || Object.keys(value).sort().join(',') !== 'fps,height,monitor,width' ||
      value.monitor !== '0' || options?.monitor !== '0' || value.fps !== options.fps || ![30,60].includes(value.fps) ||
      !ownedOutputSize(value.width, value.height, portrait)) throw new HttpError(400, 'invalid_initial_viewport');
  return Object.freeze({ monitor:'0', width:value.width, height:value.height, fps:value.fps });
}

export function initialViewportSupported(host, now, heartbeatMs) {
  const caps = host?.capabilities;
  return !!(host && !host.capabilitiesPending && Number.isFinite(host.capabilitiesAt) &&
    now >= host.capabilitiesAt && now - host.capabilitiesAt <= heartbeatMs * 3 &&
    caps?.initialViewportProtocol === initialViewportProtocol && caps.resizeProtocol === 'epoch-v1' &&
    caps.dynamicResolution === true && caps.viewportProvisioning === true && caps.virtualMonitorControl === true &&
    caps.service === true && caps.interactiveSession === true && caps.streaming === true && caps.virtualDisplayReady === true &&
    caps.monitors?.length === 1 && caps.monitors[0].id === '0' &&
    (caps.streamProfileProtocol === undefined
      ? caps.streamProfileViewportProtocol === undefined && caps.monitors[0].primary === true
      : caps.streamProfileProtocol === 'console-profile-v1' && caps.streamProfileViewportProtocol === streamProfileViewportProtocol));
}

export function profileViewportSupported(host, now, heartbeatMs, entry = false) {
  const caps=host?.capabilities;
  return !!(host && !host.capabilitiesPending && Number.isFinite(host.capabilitiesAt) && now>=host.capabilitiesAt && now-host.capabilitiesAt<=heartbeatMs*3 &&
    caps?.streamProfileProtocol === 'console-profile-v1' && caps.streamProfileViewportProtocol === streamProfileViewportProtocol &&
    caps.initialViewportProtocol === initialViewportProtocol && caps.resizeProtocol === 'epoch-v1' && caps.dynamicResolution === true && caps.viewportProvisioning === true &&
    caps.service === true && caps.interactiveSession === true && caps.streaming === true && caps.virtualDisplayReady === true &&
    caps.monitors?.length === 1 && caps.monitors[0].id === '0' &&
    (entry ? caps.virtualMonitorControl === true : caps.monitors[0].primary === true));
}

export function initialViewportSessionSupported(session, now, heartbeatMs) {
  if (session.localPc) {
    const { streamProfileProtocol: _console, streamProfileTracksProtocol: _tracks, streamProfileViewportProtocol: _viewport, ...capabilities } = session.host.capabilities;
    return initialViewportSupported({ ...session.host, capabilities }, now, heartbeatMs) && (!session.viewportEncoderLimits || capabilities.viewportEncoderLimitsProtocol === viewportEncoderLimitsProtocol);
  }
  return initialViewportSupported(session.host,now,heartbeatMs) && (!session.viewportEncoderLimits || session.host.capabilities.viewportEncoderLimitsProtocol === viewportEncoderLimitsProtocol) && (session.profileViewport
    ? profileViewportSupported(session.host,now,heartbeatMs,true)
    : session.host.capabilities.streamProfileProtocol === undefined && session.host.capabilities.streamProfileViewportProtocol === undefined);
}

// Only an explicitly negotiated initial, single-output viewport may retain
// quality ceilings during later physical resizes. tracks-v1 is unchanged.
export function requestedViewportEncoderLimits(protocol, initial) {
  if (protocol === undefined) return false;
  if (protocol !== viewportEncoderLimitsProtocol || !initial) throw new HttpError(400, 'invalid_viewport_encoder_limits');
  return true;
}

export function viewportInfoWithinLimits(info, viewport, options) {
  return info.sourceWidth === viewport.width && info.sourceHeight === viewport.height && info.fpsTarget === viewport.fps &&
    info.width <= Math.min(viewport.width, options.maxWidth) && info.height <= Math.min(viewport.height, options.maxHeight);
}

export function prepareInitialViewport(sessionId, viewport, newId = randomUUID) {
  const requestId = newId(), resizeId = newId();
  if (!uuid.test(sessionId) || !uuid.test(requestId) || !uuid.test(resizeId) || requestId === resizeId) throw new Error('invalid_initial_viewport_identity');
  return Object.freeze({ protocol:initialViewportProtocol, sessionId, requestId, resizeId,
    monitor:'0', width:viewport.width, height:viewport.height, fps:viewport.fps, epoch:1 });
}

export function initialViewportInfoMatches(info, initial, options) {
  return info.sessionId === initial.sessionId && info.requestId === initial.requestId && info.resizeId === initial.resizeId && info.epoch === 1 &&
    viewportInfoWithinLimits(info, initial, options);
}
