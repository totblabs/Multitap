import { brokerWebSocketOrigin } from './transport-policy.mjs';
import { observabilityProtocol, requestedObservability, observabilityCapability, observabilityStats, withoutObservability, beginObservabilityForward, completeObservabilityForward, clearObservability, observabilityTls, safeObservabilityTls, safeObservabilityVersion } from './stream-observability.mjs';
import { randomUUID, randomBytes, createHash } from 'node:crypto';
import { TakeoverContexts } from './session-context.mjs';
import { isDeepStrictEqual } from 'node:util';
import { SigninStreamPhase, SigninEligibilityOffers, SigninServiceLease, parseSigninBinding, signinStreamProtocol, signinServiceProtocol } from './signin-stream.mjs';
import { WebSocketServer, WebSocket } from 'ws';
import { HttpError } from './errors.mjs';
import { newToken, tokenHash, text } from './security.mjs';
import { localPcCapabilities } from './local-pc.mjs';
import { ClipboardRelay } from './clipboard.mjs';
import { anyClipboardAllowed, clipboardPolicyRestricted } from './clipboard-policy.mjs';
import { TrackStream, requestedTracks, tracksSupported, parseTrackPacket, trackProtocol, trackTransport } from './track-media.mjs';
import { profileTracksProtocol, requestedProfileTracks, profileTracksCapability, profileTracksSupported } from './profile-tracks.mjs';
import { validHostResources, freshHostResources, hostResourcesProtocol } from './host-resources.mjs';
import { validThumbnailPrivacy, observeThumbnailPrivacy, freshThumbnailPrivacy } from './thumbnail-privacy.mjs';
import { diagnosticsProtocol, safeDiagnosticReport, recordHostEventHistory, hostEventHistory, hostEventHistoryLimit, hostEventHistoryMs } from './host-diagnostics.mjs';
import { SessionStop, sessionStopProtocol } from './session-stop.mjs';
import { ConsolePreparation, consoleReasons } from './console-preparation.mjs';
import { QemuConsolePreparation, qemuConsoleProtocol, legacyQemuConsoleProtocol } from './qemu-console-preparation.mjs';
import { consoleWebViewProtocol } from './console-webviews.mjs';
import { ConsoleRelay, consoleRelayProtocol } from './console-relay.mjs';
import { prefetchStreamConsoleIdentity, streamConsoleProof, consoleProofRefusal, streamConsoleProfileProtocol } from './stream-console-profile.mjs';
import { bitrateProtocol, adaptiveMaxBitrate, bitrateCapability, negotiateBitrate, startBitrateFields, validBitrateFeedback, bitrateFeedbackForHost } from './stream-bitrate.mjs';
import { initialViewportProtocol, streamProfileViewportProtocol, initialViewportPrepareMs, requestedInitialViewport, initialViewportSupported, profileViewportSupported, initialViewportSessionSupported, prepareInitialViewport, initialViewportInfoMatches, viewportEncoderLimitsProtocol, requestedViewportEncoderLimits, viewportInfoWithinLimits } from './initial-viewport.mjs';
import { inputLatencyProtocol, inputFrameTimingProtocol, inputLatencyCapability, requestedInputLatency, negotiateInputLatency, inputLatencyState, admitInputSample, holdEarlyDirectInputReport, takeEarlyDirectInputReports, inputLatencyHostMessage, inputLatencyTimedPacket, recordInputLatencyForward, inputLatencyForwardStats } from './input-latency.mjs';
import { cursorProtocol, cursorTracksProtocol, cursorKind, cursorCapability, cursorTracksCapability, requestedCursor, requestedCursorTracks, negotiateCursor, cursorState, cursorPacket, trackCursorPacket, cursorSource, cursorEchoProtocol, cursorEchoCapability, requestedCursorEcho, negotiateCursorEcho, validCursorEcho, cursorComposeProtocol, cursorComposeCapability, requestedCursorCompose, negotiateCursorCompose, validCursorCompose } from './cursor.mjs';
import { textInputProtocol, textInputCapability, requestedTextInput, negotiateTextInput, validTextInput } from './text-input.mjs';
import { displayScaleProtocol, displayScaleOutputsProtocol, displayScaleCapability, displayScaleOutputsCapability, requestedDisplayScale, requestedDisplayScaleOutputs, negotiateDisplayScale, negotiateDisplayScaleControl, validDisplayScaleRequest, displayScaleResult } from './display-scale.mjs';
import { touchInputProtocol, touchInputCapability, requestedTouchInput, negotiateTouchInput, validTouchInput } from './touch-input.mjs';
import { relativeMouseProtocol, relativeMouseTracksProtocol, relativeMouseCapability, relativeMouseTracksCapability, requestedRelativeMouse, negotiateRelativeMouse, relativeMouseInput, validRelativeMouse, validRelativeTrackMouse } from './relative-mouse.mjs';
import { caretProtocol, caretCapability, requestedCaret, negotiateCaret, validCaretWatch, caretReport, caretAllowed } from './caret.mjs';
import { DriveRelay, driveProtocol, driveCapability, driveUnavailableCapability, requestedDrive, negotiateDrive, driveFsCapability, requestedDriveFs } from './drive.mjs';
import { streamSizeProtocol, streamSizeProtocolV2, streamSizeCapability, streamSizeAsked, requestedStreamSize, negotiateStreamSize, ownedOutputSize, encodedSize, withinDecoderLimits, rescaledInfoValid, validDecoderReport, validDecoderLimits } from './stream-size.mjs';
import { videoGeometry, boundedInteger, maximumGeometrySide } from './video-geometry.mjs';
import { PrintRelay, printProtocol, printCapability, printUnavailableCapability, requestedPrint, negotiatePrint } from './print.mjs';
import { UsbRelay, usbProtocol, usbTracksProtocol, usbCapability, requestedUsb, negotiateUsb } from './usb.mjs';
import { UdpRelay } from './udp-relay.mjs';
import { audioOpusProtocol, audioOpusTracksProtocol, audioOpusCodec, audioOpusCapability, audioOpusTracksCapability, requestedAudioOpus, negotiateAudioOpus, validStreamAudio, validAudioPayload } from './audio-codec.mjs';
import { udpDirectProtocol, directCapability, udpPeerDirectAllowed } from './udp-direct.mjs';
import { udpTracksProtocol, udpTracksCapability, requestedUdpTracks, negotiateUdpTracks, udpTrackScope, udpTrackKey, udpTrackLocal, udpTrackSignature, udpTrackFeedback } from './udp-tracks.mjs';
import { udpMediaProtocol, udpMediaCapability, requestedUdpMedia, negotiateUdpMedia, udpMediaAllowed, udpGrantMessage, udpKeyMessage } from './udp-media.mjs';
import { DesktopPreviews, desktopPreviewProtocol, thumbnailCaptureProtocol, thumbnailCaptureCapability } from './desktop-previews.mjs';
import { SessionResume, sessionResumeProtocol, hostResumeProtocol, trackResumeProtocol, resumeHeader, sessionResumeCapability, requestedSessionResume, negotiateSessionResume, hostResumeCapability, trackResumeCapability } from './session-resume.mjs';
import { ChannelResume, channelResumeProtocol, channelResumeHeader, hostChannelResumeCapability } from './channel-resume.mjs';
import { HostReadyWaits, hostReadyProtocol } from './host-ready-wait.mjs';
import { ImeState, imeStateProtocol, imeStateCapability } from './ime-state.mjs';

const MAX_MESSAGE = 4 * 1024 * 1024;
const MAX_QUEUE = 8 * 1024 * 1024;
const MAX_CLIENT_BYTES_PER_SECOND = 32 * 1024 * 1024;
const streamFailures = new Set(['no_interactive_session', 'virtual_display_required', 'worker_failed', 'capture_failed', 'encoder_failed', 'host_maintenance']);
// Exact, negotiated v2 failures only. Do not widen legacy admission or
// forward arbitrary SDK/encoder error text from the reusable Host channel.
const streamSizeFailuresV2 = new Set(['frame_layout_unsupported', 'frame_memory_unavailable', 'frame_memory_insufficient', 'encoder_format_unsupported', 'unsupported_display_mode']);
// The fixed reasons a Host's sign-in session may end with (signin_bridge.cpp).
const signinStreamFailures = new Set(['signin_stream_unavailable', 'signin_owned_input_release_pending', 'signin_console_changed', 'signin_phase_changed', 'signin_capture_failed', 'signin_restore_pending', 'signin_preparer_exit_pending', 'signin_admission_expired', 'signin_authority_lost', 'signin_display_unavailable']);
function allowedStreamFailure(session, reason) {
  // A Host that refuses a sign-in start says so; ending that one session is
  // enough (an unlisted reason drops the Host's whole channel instead).
  return streamFailures.has(reason) || (!!session.signinPolicy && signinStreamFailures.has(reason)) || (session.mode === 'stream' &&
    session.streamSizeVersion === streamSizeProtocolV2 && streamSizeFailuresV2.has(reason));
}
const defaultStreamOptions = Object.freeze({ maxWidth: 1920, maxHeight: 1080, fps: 60, bitrate: 12000000, audio: true, monitor: '0' });
const uuidPattern = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i;
const sessionPattern = /^[0-9a-f-]{36}$/;
const validToken = value => /^[A-Za-z0-9_-]{43}$/.test(value ?? '');
const displayFailures = new Set(['invalid_display_request','display_control_unavailable','no_interactive_session','operation_cancelled','display_configuration_failed','display_count_mismatch','worker_failed','display_timeout']);
// Optional self-reported diagnostics, independent of display/mode readiness.
// Keep this finite contract aligned with native service safeViewportFailure.
// Unknown/malformed values are omitted, like other unrecognized metadata;
// they must neither leak arbitrary host text nor disable valid capabilities.
const viewportFailures = new Set(['', 'no_interactive_session', 'interactive_console_required', 'administrator_required', 'viewport_probe_pending', 'viewport_probe_failed', 'viewport_probe_timeout', 'viewport_support_unavailable', 'viewport_recovery_required', 'viewport_settings_unavailable', 'viewport_settings_untrusted', 'viewport_settings_read_failed', 'viewport_original_settings_invalid', 'viewport_original_settings_modified', 'viewport_settings_modified_externally', 'viewport_package_path_invalid', 'viewport_package_path_untrusted', 'viewport_package_not_administrator_controlled', 'viewport_installed_driver_unavailable', 'viewport_installed_driver_untrusted', 'viewport_installed_driver_mismatch', 'viewport_sources_unavailable', 'viewport_connector_identity_unavailable', 'viewport_connector_identity_ambiguous', 'viewport_ccd_query_failed', 'viewport_ccd_source_invalid', 'viewport_ccd_snapshot_limit', 'viewport_topology_changed', 'viewport_ccd_validate_failed', 'monitor_not_available', 'driver_hash_mismatch', 'driver_signature_untrusted', 'driver_catalog_membership_invalid']);
for (const reason of ['host_maintenance', 'mode_catalog_update_required', 'mode_catalog_failed', 'mode_catalog_timeout', 'mode_catalog_cancelled', 'mode_catalog_registration_timeout', 'mode_catalog_not_verified', 'viewport_rollback_incomplete', 'reboot_required', 'existing_virtual_display_requires_review', 'shared_vendor_path_conflict', 'owned_virtual_output_inactive', 'owned_virtual_display_not_ready', 'viewport_busy', 'owned_device_restart_failed', 'owned_device_restart_status_unavailable']) viewportFailures.add(reason);
for (const reason of ['native_display_not_ready', 'native_display_provider_unavailable', 'native_display_snapshot_invalid', 'native_display_configuration_untrusted', 'native_display_topology_mismatch', 'native_display_change_pending', 'native_topology_not_ready', 'native_topology_recovery_required']) viewportFailures.add(reason);
// Why a Host connection ended, beyond its released reason code: the `detail`
// of session.ended (released clients read only `reason`) and of the
// host_channel_closed audit row. ws names the bytes it refused (error.code);
// a framing error is what WinHTTP produces when it writes its PONG into a
// large frame another thread is sending (websocket_send.hpp on the Host).
const hostErrorDetails = { WS_ERR_UNSUPPORTED_MESSAGE_LENGTH: 'host_message_too_large', WS_ERR_UNSUPPORTED_DATA_PAYLOAD_LENGTH: 'host_message_too_large', WS_ERR_INVALID_UTF8: 'host_text_invalid', WS_ERR_TOO_MANY_BUFFERED_PARTS: 'host_message_fragmented' };
export function hostErrorDetail(error) {
  const code = typeof error?.code === 'string' ? error.code : '';
  return hostErrorDetails[code] ?? (code.startsWith('WS_ERR_') ? 'host_frame_invalid' : 'host_transport_error');
}
// host-closing-v1: a Host that is about to close its channel deliberately
// says why (offered in host.capabilities.ack; released Hosts never send it).
export const hostClosingProtocol = 'host-closing-v1';
// keyframe-v1 (Host capability keyFrameProtocol): {type:'stream.keyframe'}
// makes the Host code its next frame as an IDR. A client whose socket backlog
// passes half the bound then gets no more video (and no audio) until that
// IDR instead of being disconnected as client_backpressure; released Hosts
// keep the released behaviour.
export const keyFrameProtocol = 'keyframe-v1';
const MEDIA_SKIP = MAX_QUEUE / 2;
const hostClosingDetails = { service_stopping: 'host_service_stopping', protocol_rejected: 'host_protocol_rejected' };
const signinId = value => typeof value === 'string' && /^[a-f0-9]{32}$/.test(value);
const signinExact = (value, fields) => value && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length === fields.length && fields.every(key => Object.hasOwn(value, key));
const signinU64 = value => typeof value === 'string' && /^[1-9][0-9]{0,19}$/.test(value) && BigInt(value) <= 0xffffffffffffffffn;
const signinIdr = bytes => {
  let mask = 0;
  for (let n = 0; n + 3 < bytes.length; n++) {
    const length = bytes[n] === 0 && bytes[n+1] === 0 ? bytes[n+2] === 1 ? 3 : bytes[n+2] === 0 && bytes[n+3] === 1 ? 4 : 0 : 0;
    if (!length || n + length >= bytes.length) continue;
    const header = bytes[n+length]; if (header & 128) return false;
    if ((header & 31) === 5) mask |= 1; if ((header & 31) === 7) mask |= 2; if ((header & 31) === 8) mask |= 4;
    n += length;
  }
  return mask === 7;
};
function signinInput(value, sessionId, textAllowed) {
  if (value.sessionId !== undefined && value.sessionId !== sessionId) return false;
  const { sessionId: _id, ...input } = value;
  if (input.type === 'input.reset' || input.type === 'input.fence') return Object.keys(input).length === 1;
  if (input.event === 'text') return textAllowed && validTextInput(input);
  const fields = input.event === 'key' ? ['type','event','vk','scan','down','extended'] : input.event === 'move' ? ['type','event','x','y'] : input.event === 'button' ? ['type','event','x','y','button','down'] : input.event === 'wheel' ? ['type','event','x','y','delta'] : [];
  return signinExact(input, fields) && validClientControl(input);
}
// Peers that sent anything since the previous heartbeat are alive and are not
// pinged: WinHTTP answers a ping from the Host's receiving thread and could
// write the pong inside a large frame the sending thread was writing, which
// ended every session of the Host as host_error (as with 4K video).
// Only a silent peer is pinged, so released Hosts are protected as well.

export class Relay {
  constructor({ store, vault, proxmox, publicOrigin, brokerVersion = null, heartbeatMs = 10000, authorizationMs = 250, resizeReadyMs = 15000, viewportReadyMs = 90000, trackMediaMs = 10000, displayReadyMs = 30000, consoleReadyMs = 25000, stopReadyMs = 45000, stopPollMs = 1000, consoleProofReadMs = 10000, clipboardOptions = {}, printOptions = {}, driveOptions = {}, usbOptions = {}, consoleOptions = {}, qemuConsoleOptions = {}, imeOptions = {}, udp = null, hostRoute = null, resumeGraceMs = 15000, hostResumeGraceMs = 30000, previewOptions = {}, hostReadyOptions = {}, signinOptions = null, takeoverContextOptions = {} }) {
    Object.assign(this, { store, vault, proxmox, publicOrigin, heartbeatMs, resizeReadyMs, viewportReadyMs, trackMediaMs, displayReadyMs, hostRoute, consoleProofReadMs });
    // Explicit constructor-injected qualification seam only. No environment,
    // version inference or ordinary readiness relaxation enables this feature.
    this.signinEnabled = signinOptions?.enabled === true && signinOptions?.qualifiedService === true;
    this.signinChallengeMs = Math.max(50, Math.min(2000, signinOptions?.challengeMs ?? 1000));
    this.signinChallenges = new Map();
    this.machineRefusals = new Map(); // host-machine-v1: hostId -> last audited refusal
    this.signinAdmissionRevisions = new WeakMap();
    this.signinOffers = new SigninEligibilityOffers({ isCurrentHost: host => this.hostAuthenticated(host) && host.signinNegotiated === true });
    // host-ready-wait-v1 (host-ready-wait.mjs): clients waiting for a Host after a restart.
    this.hostReadyWaits = new HostReadyWaits({ reported: id => this.hostReported(id), ...hostReadyOptions });
    // session-resume-v1 / host-resume-v1 (session-resume.mjs).
    this.resume = new SessionResume(this, { clientGraceMs: resumeGraceMs, hostGraceMs: hostResumeGraceMs });
    // channel-resume-v1: the side channels of a resumed stream pair again (channel-resume.mjs).
    this.channels = new ChannelResume(this);
    this.brokerVersion = safeObservabilityVersion(brokerVersion);
    this.hosts = new Map(); this.sessions = new Map(); this.hostCloseAudits = new Map();
    this.takeoverContexts = new TakeoverContexts(takeoverContextOptions);
    // Last time a session was created or ended or a display operation began (activity.mjs).
    this.activityAt = 0;
    this.sessionStop = new SessionStop(this,{readyMs:stopReadyMs,pollMs:stopPollMs});
    // Private correlation, deliberately absent from stop DTOs/audit/spreads.
    // A weak key expires with the actual stop record, without a token ledger.
    this.cleanupAuthorization = new WeakMap();
    this.displayOperations = new Map(); this.displayBusy = new Map(); this.displayLatest = new Map();
    this.consolePreparation = new ConsolePreparation(this, consoleReadyMs);
    this.qemuConsole = new QemuConsolePreparation(this, qemuConsoleOptions);
    this.console = new ConsoleRelay(this, consoleOptions);
    // ime-state-v1: the VM's 한/영 mode for the browser Console, asked of the Host per read (ime-state.mjs).
    this.ime = new ImeState(this, imeOptions);
    this.powerBusy = () => false;
    // (desktopId, kind, detail): 'connected' | 'ready' | 'disconnected' | 'session'.
    // The VM state machine (vm-state.mjs) and the live channel listen here.
    this.onHostEvent = () => {};
    this.wss = new WebSocketServer({ noServer: true, maxPayload: MAX_MESSAGE, perMessageDeflate: false, maxFragments: 1024, closeTimeout: 1000 });
    this.clipboard = new ClipboardRelay(this, clipboardOptions);
    this.print = new PrintRelay(this, printOptions);
    this.drive = new DriveRelay(this, driveOptions);
    this.usb = new UsbRelay(this, usbOptions);
    // desktop-preview-v1: launcher pictures on request, in memory only (desktop-previews.mjs).
    this.previews = new DesktopPreviews({ store, send: (host, value) => this.send(host.ws, value), ...previewOptions, allowed: host => ![...this.sessions.values()].some(session => session.host === host && session.signinPolicy) });
    // Null unless a UDP port is configured: no worker, socket or timer exists.
    // A session waiting for its Host (host-resume-v1) keeps its binding.
    this.udp = udp && new UdpRelay({ ...udp, hostRoute, live: id => { const session = this.sessions.get(id); return !!session && (this.current(session) || !!session.hostHeld) && this.authorized(session); } });
    if (this.udp) this.udp.onPeer = (id, role, candidates, scope) => {
      const session = this.sessions.get(id);
      if (!udpPeerDirectAllowed(this.udp) || !session?.udpDirect || !session.udpGranted || !this.authorized(session) || !this.current(session)) return;
      if(scope){
        if(!udpTrackScope(session,{...scope,sessionId:id})||session.trackHeld||!session.trackTunnel)return;
        this.send(role==='host'?session.trackTunnel:session.ws,{type:'udp.tracks.peer',protocol:udpDirectProtocol,sessionId:id,...scope,candidates});return;
      }
      this.send(role === 'host' ? session.host.ws : session.ws, { type: 'udp.peer', protocol: udpDirectProtocol, sessionId: id, candidates });
    };
    this.timer = setInterval(() => this.revalidate(), authorizationMs); this.timer.unref();
    this.heartbeat = setInterval(() => this.checkHeartbeat(), heartbeatMs); this.heartbeat.unref();
  }
  // Fresh, synchronous login admission observation. Never accept these hashes
  // from HTTP input, persist them, return them in an API or prolong their TTL.
  protectedSessionHashes(userId, now = Date.now(), liveClients = []) {
    const protectedHashes = new Set();
    const take = value => {
      const hash = this.consoleParentHash?.(value?.tokenHash ?? value?.hash) ?? (value?.tokenHash ?? value?.hash);
      if (value?.userId !== userId || typeof hash !== 'string' || !/^[a-f0-9]{64}$/.test(hash) || protectedHashes.has(hash)) return;
      const user = this.store.authenticate(hash, now);
      if (!user || user.id !== userId || user.must_change_credentials || (value.desktopId && !this.store.assignedDesktop(userId, value.desktopId))) return;
      protectedHashes.add(hash);
    };
    // Includes unspent/pending tickets and every attached streaming wire.
    for (const value of this.sessions.values()) take(value);
    // A retired console still owns its actual opener/transport until collect.
    for (const value of this.console.grants.values()) take(value);
    for (const value of this.displayBusy.values()) take(value);
    for (const record of this.sessionStop.pending.values()) take(this.cleanupAuthorization.get(record));
    // The caller supplies only its internally owned DesktopEvents entries.
    for (const value of liveClients) if (value.ws?.readyState === WebSocket.OPEN) take(value);
    if (protectedHashes.size > 5) throw Object.assign(new Error('active_session_limit'), { code: 'active_session_limit' });
    return protectedHashes;
  }
  // The signals the VM state machine reads (vm-state.mjs).
  hostAuthenticated(host) {
    return !!host && this.hosts.get(host.identity.desktop_id) === host && host.ws.readyState === WebSocket.OPEN && this.store.authenticateHost(host.tokenHash)?.id === host.identity.id;
  }
  hostResources(desktopId) { const host = this.hosts.get(desktopId); return this.hostAuthenticated(host) ? freshHostResources(host, Date.now()) : null; }
  hostConnected(desktopId) { return this.hostAuthenticated(this.hosts.get(desktopId)); }
  hostReady(desktopId) {
    const host = this.hosts.get(desktopId);
    if (!this.hostAuthenticated(host) || this.displayBusy.has(desktopId)) return false;
    const modes = this.modes(host);
    return modes.stream && !this.sessionStop.blocked(desktopId);
  }
  // host-ready-wait-v1: the desktop's Host is connected here and its
  // capabilities are in, so a fresh desktop read describes it.
  hostReported(desktopId) {
    const host = this.hosts.get(desktopId);
    return this.hostAuthenticated(host) && !host.capabilitiesPending;
  }
  hasSession(desktopId) {
    for (const session of this.sessions.values()) if (session.desktopId === desktopId) return true;
    return false;
  }
  // When the earliest live session of another account on this desktop began
  // (null when there is none): previews of that desktop stop for everyone
  // else while it lasts (desktop-previews.mjs).
  othersSince(desktopId, userId) {
    let since = null;
    for (const session of this.sessions.values()) {
      if (session.desktopId !== desktopId || session.userId === userId) continue;
      const at = Number.isFinite(session.createdAt) ? session.createdAt : 0;
      if (since === null || at < since) since = at;
    }
    return since;
  }
  hostEvent(desktopId, kind, detail) {
    try { this.onHostEvent(desktopId, kind, detail); } catch { /* A listener cannot break the control channel. */ }
    this.hostReadyWaits?.wake(desktopId);
  }
  config() {
    const encrypted = this.store.setting('proxmox');
    if (!encrypted || !this.vault) throw new HttpError(409, 'proxmox_not_configured');
    return this.vault.open(encrypted);
  }
  async createEnrollment(desktop, actor, authorize = () => {}) {
    if (!desktop.pve_available) throw new HttpError(409, 'desktop_unavailable');
    const revision = this.store.setting('proxmox');
    const identity = await this.proxmox.machineIdentity(this.config(), desktop.pve_vmid, { node: desktop.pve_node, expectedUuid: desktop.pve_machine_uuid });
    if (this.store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
    const current = this.store.db.prepare("SELECT pve_vmid,pve_node,pve_machine_uuid FROM desktops WHERE id=? AND kind='vm' AND pve_available=1").get(desktop.id);
    if (!current || current.pve_vmid !== desktop.pve_vmid || current.pve_node !== desktop.pve_node || current.pve_machine_uuid !== desktop.pve_machine_uuid) throw new HttpError(409, 'desktop_changed');
    authorize();
    const code = newToken();
    const expiresAt = this.store.issueEnrollment(desktop.id, identity.machineUuid, tokenHash(code), actor);
    return { code, expiresAt: new Date(expiresAt).toISOString(), serverOrigin: this.publicOrigin };
  }
  async enroll(input) {
    if (!validToken(input.code) || typeof input.machineUuid !== 'string' || !uuidPattern.test(input.machineUuid)) throw new HttpError(400, 'invalid_enrollment');
    let hostname, version;
    try { hostname = text(input.hostname, 128); version = text(input.version, 32); } catch { throw new HttpError(400, 'invalid_host_metadata'); }
    const hash = tokenHash(input.code), enrollment = this.store.pendingEnrollment(hash);
    if (!enrollment) throw new HttpError(403, 'invalid_enrollment');
    const machineUuid = input.machineUuid.toLowerCase();
    if (machineUuid !== enrollment.machine_uuid) throw new HttpError(403, 'host_machine_mismatch');
    const revision = this.store.setting('proxmox');
    const identity = await this.proxmox.machineIdentity(this.config(), enrollment.pve_vmid, { node: enrollment.pve_node, expectedUuid: enrollment.machine_uuid });
    if (revision !== this.store.setting('proxmox') || identity.machineUuid !== machineUuid) throw new HttpError(409, 'host_machine_mismatch');
    const currentEnrollment = this.store.pendingEnrollment(hash);
    if (!currentEnrollment || currentEnrollment.desktop_id !== enrollment.desktop_id || currentEnrollment.pve_node !== enrollment.pve_node || currentEnrollment.pve_vmid !== enrollment.pve_vmid || currentEnrollment.machine_uuid !== enrollment.machine_uuid) throw new HttpError(409, 'host_machine_mismatch');
    const hostToken = newToken();
    let result;
    try { result = this.store.consumeEnrollment(hash, machineUuid, { hostname, version, tokenHash: tokenHash(hostToken) }); }
    catch (error) { if (error.message === 'invalid_enrollment') throw new HttpError(403, 'invalid_enrollment'); throw error; }
    this.revalidate();
    return { ...result, hostToken, serverOrigin: this.publicOrigin };
  }
  decorate(desktops, actor, origin = this.publicOrigin) {
    return desktops.map(desktop => {
      // Each assigned VM carries its own administrator grant.
      const clipboardPolicy = actor ? this.store.clipboardPolicy(actor, desktop.id) : null;
      const host = this.hosts.get(desktop.id);
      const connected = this.hostAuthenticated(host);
      const thumbnailPrivacy = freshThumbnailPrivacy(host, connected, this.heartbeatMs * 3);
      const connectionModes = { stream: connected && !this.displayBusy.has(desktop.id) && this.modes(host).stream && !this.sessionStop.blocked(desktop.id) };
      const activeSession = actor ? this.activeSession(desktop.id, actor) : null;
      return { ...desktop, status: connected ? 'ready' : 'host_offline', canConnect: connectionModes.stream, connectionModes, display: this.displayView(desktop.id, actor), hostReadyProtocol,
        ...(desktop.kind === 'vm' ? { consoleRelayProtocol, ...(typeof origin === 'string' && origin.startsWith('https:') ? { consoleBrowserProtocol: 'console-browser-v1', consoleWebViewProtocol } : {}) } : {}),
        ...(activeSession ? { activeSession } : {}),
        ...((desktop.kind === 'vm' || desktop.kind === 'pc') && this.previews?.offered(host, connected) ? { previewProtocol: desktopPreviewProtocol } : {}),
        ...(clipboardPolicy ? { clipboardPolicy } : {}),
        // A console connection holds this VM (and its display); clients offer
        // 'Console 끊고 연결' instead of a display-preparation wait.
        ...(this.console?.blocked(desktop.id) ? { consoleInUse: true } : {}),
        // The VM's Proxmox option for the QEMU guest agent is off: the tile
        // says so. Unknown (not yet read, read failed): absent.
        ...(this.store.qemuAgentOption?.get(desktop.id) === false ? { qemuAgentOff: true } : {}),
        consolePreparation: this.consolePreparation.view(desktop.id, actor),
        sessionCleanup: this.sessionStop.view(desktop.id),
        host: connected ? { connected: true, hostname: host.identity.hostname, version: host.identity.version, lastSeenAt: new Date(host.lastSeen).toISOString(), capabilitiesPending: host.capabilitiesPending, ...(thumbnailPrivacy ? { thumbnailPrivacy } : {}), ...(host.capabilities ? { capabilities: { ...this.visibleCapabilities(host.capabilities, actor, desktop.id, desktop.kind === 'pc'), ...(clipboardPolicy ? { clipboard: host.capabilities.clipboard === true && anyClipboardAllowed(clipboardPolicy), ...(host.capabilities.clipboardSync === undefined ? {} : { clipboardSync: host.capabilities.clipboardSync === true && anyClipboardAllowed(clipboardPolicy) }) } : {}) } } : {}) } : { connected: false } };
    });
  }
  withSigninAvailability(desktops, hash, expectedUserId) {
    const current = this.store.authenticate(hash);
    if (!current || current.id !== expectedUserId || current.must_change_credentials) throw new HttpError(401, 'session_expired');
    return desktops.filter(desktop => this.store.assignedDesktop(current.id, desktop.id)).map(desktop => {
      const { signinAvailability: _oldOffer, ...ordinary } = desktop;
      if (!this.signinEnabled) return ordinary;
      const host = this.hosts.get(desktop.id), offer = this.signinOffers.current(host), now = Date.now();
      if (!offer || !this.hostAuthenticated(host) || !host.signinNegotiated || !signinId(host.signinServiceInstance) || !(host.signinOfferExpiresAt > now) || [...this.sessions.values()].some(session => session.desktopId === desktop.id)) return ordinary;
      try { this.assertDisplayIdle(desktop.id, 'stream'); } catch { return ordinary; }
      const expiresAtMs = Math.min(offer.expiresAtMs, host.signinOfferExpiresAt);
      return { ...ordinary, signinAvailability: { protocol: signinStreamProtocol, availabilityId: offer.availabilityId, expiresAtMs, windowsState: offer.windowsState } };
    });
  }
  // A connection (or a ticket about to attach) holds this VM: only whether it
  // is the viewer's own account, never whose or from where. Connecting
  // already reports the same through desktop_busy; tiles say it beforehand.
  activeSession(desktopId, actor) {
    let found = null;
    for (const session of this.sessions.values()) if (session.desktopId === desktopId) {
      // Another account: its username too, so the tile says who is using the desktop;
      // the same name the takeover card already shows to the people assigned here.
      if (session.userId !== actor) { const account = this.store.db.prepare('SELECT username FROM users WHERE id=?').get(session.userId)?.username; return typeof account === 'string' ? { mine: false, account } : { mine: false }; }
      found = { mine: true };
    }
    return found;
  }
  // drive-v1 is shown to a user only where their assignment allows `drives`,
  // so a client never offers drive sharing it would not be granted.
  visibleCapabilities(capabilities, actor, desktopId, localPc = false) {
    // No launcher availability/entry is advertised by this adapter slice.
    const { signinServiceProtocol: _privateSigninProtocol, ...withoutSignin } = capabilities;
    capabilities = withoutSignin;
    // Raw Host fields are not broker freshness attestations. Old brokers may
    // blindly forward these; only decorate() stamps host.thumbnailPrivacy.
    // The capture protocol is between this broker and the Host; clients read
    // the desktop's previewProtocol instead.
    const hostDirect = directCapability(capabilities);
    const { thumbnailPrivacy: _rawPrivacy, thumbnailCaptureProtocol: _capture, udpDirectProtocol: _rawDirect, udpDirectHostProtocol: _rawDirectHost, udpTracksProtocol: _rawTracks, ...ordinary } = capabilities;
    capabilities = ordinary;
    // A client that sees this key sends it back, and released brokers refuse
    // that field: show it only while this broker can really grant.
    if (!this.udp?.available) { const { udpMediaProtocol: _unavailable, udpDirectProtocol: _directUnavailable, ...rest } = capabilities; capabilities = rest; }
    // Old brokers may forward unknown Host fields. Only this broker translates
    // the Host marker to the connect marker, so new peers still work with them.
    if (this.udp?.available && udpPeerDirectAllowed(this.udp) && hostDirect) capabilities = { ...capabilities, udpDirectProtocol };
    if(this.udp?.available&&udpMediaAllowed(this.udp,desktopId)&&udpTracksCapability(capabilities))capabilities={...capabilities,udpTracksProtocol};
    // session-resume-v1: this broker resumes where the Host can restart the picture.
    if (sessionResumeCapability(capabilities)) capabilities = { ...capabilities, sessionResumeProtocol };
    // A physical PC uses v2's owned-output path while preserving every foreign
    // display. Never advertise the Proxmox console-identity transaction there.
    if (localPc) {
      const { streamProfileProtocol: _pcProfile, streamProfileTracksProtocol: _pcTracks, streamProfileViewportProtocol: _pcViewport, ...remaining } = capabilities;
      capabilities = remaining;
    }
    if (!actor) return capabilities;
    const policy = this.store.devicePolicy(actor, desktopId);
    let visible = capabilities;
    if (policy.drives !== true && (visible.driveProtocol !== undefined || visible.driveUnavailableReason !== undefined)) {
      const { driveProtocol: _hidden, driveFs: _hiddenFs, driveUnavailableReason: _hiddenReason, ...rest } = visible; visible = rest;
    }
    // The printer's reason is the same kind of hint: only for a user who may print.
    if (policy.printers !== true && visible.printUnavailableReason !== undefined) {
      const { printUnavailableReason: _hiddenPrint, ...rest } = visible; visible = rest;
    }
    return visible;
  }
  modes(host) {
    // Legacy stream-only hosts have no capability advertisement.
    if (host.capabilitiesPending || host.capabilities?.viewportFailureReason === 'host_maintenance') return { stream: false };
    // No signed-in, unlocked Windows session: nothing to stream, so the tile
    // must not say ready (a locked or signed-out desktop would read
    // '연결 준비됨' and the stream would end no_interactive_session).
    return { stream: !host.capabilities || (host.capabilities.streaming && host.capabilities.virtualDisplayReady && host.capabilities.interactiveSession !== false) };
  }
  diagnosticStatus(desktopId) {
    const host = this.hosts.get(desktopId), connected = this.hostAuthenticated(host);
    const row = this.store.db.prepare('SELECT h.id,d.received_at,d.report FROM hosts h LEFT JOIN host_diagnostics d ON d.host_id=h.id WHERE h.desktop_id=? AND h.revoked=0').get(desktopId);
    let report = null;
    try { report = row?.report ? safeDiagnosticReport(JSON.parse(row.report)) : null; } catch { /* Damaged optional telemetry cannot grant or break a session. */ }
    let events = [];
    try { events = row ? hostEventHistory(this.store, row.id) : []; } catch { /* Optional history; the latest report still stands. */ }
    return { supported: !!(connected && host.capabilities?.diagnosticsSupported), connected, receivedAt: report ? new Date(row.received_at).toISOString() : null, stale: !report || !connected || Date.now() - row.received_at > 90000, report,
      history: { retentionDays: hostEventHistoryMs / 86400000, limit: hostEventHistoryLimit, events } };
  }
  requestDiagnostics(desktopId) {
    const host = this.hosts.get(desktopId);
    if (!this.hostAuthenticated(host)) throw new HttpError(409, 'host_offline');
    if (host.capabilities?.diagnosticsSupported !== true) throw new HttpError(409, 'host_diagnostics_update_required');
    const now = Date.now();
    if (host.diagnosticRequestedAt !== undefined && now - host.diagnosticRequestedAt < 5000) throw new HttpError(429, 'diagnostics_rate_limit');
    host.diagnosticRequestedAt = now;
    if (!this.send(host.ws, { type: 'host.diagnostics.request' })) throw new HttpError(503, 'host_diagnostics_unavailable');
    return { requested: true, requestedAt: new Date(now).toISOString() };
  }
  receiveDiagnostics(host, value) {
    try {
    if (this.hosts.get(host.identity.desktop_id) !== host || this.store.authenticateHost(host.tokenHash)?.id !== host.identity.id) return false;
    const report = safeDiagnosticReport(value.report);
    if (!report || host.capabilities?.diagnosticsSupported !== true) return false;
    const now = Date.now(), previous = this.store.db.prepare('SELECT received_at FROM host_diagnostics WHERE host_id=?').get(host.identity.id);
    if (previous && now - previous.received_at < 5000) return false;
    this.store.db.prepare('INSERT INTO host_diagnostics(host_id,received_at,report) VALUES(?,?,?) ON CONFLICT(host_id) DO UPDATE SET received_at=excluded.received_at,report=excluded.report').run(host.identity.id, now, JSON.stringify(report));
    recordHostEventHistory(this.store, host.identity.id, report, now);
    return true;
    } catch { return false; } // Optional telemetry must not crash the control/stream channel on disk errors.
  }
  unavailableReason(host, mode) {
    if (!host || host.ws.readyState !== WebSocket.OPEN) return 'host_offline';
    if (host.capabilitiesPending) return 'capabilities_pending';
    const caps = host.capabilities;
    if (caps?.viewportFailureReason === 'host_maintenance') return 'host_maintenance';
    if (caps && !caps.virtualDisplayReady) return 'virtual_display_required';
    if (caps && !caps.interactiveSession) return 'no_interactive_session';
    return 'streaming_unavailable';
  }
  authorized(session) {
    const user = this.store.authenticate(session.tokenHash);
    return !!user && user.id === session.userId && !user.must_change_credentials && !!this.store.assignedDesktop(user.id, session.desktopId);
  }
  // Permissions belong to one assignment (user x VM). Each live session is
  // compared with its own VM's current grant, so a change ends only the
  // restricted sessions; omit desktopId to recheck all of the user's sessions.
  // A removed assignment is left to the authorization sweep (authorization_revoked).
  policySessions(userId, desktopId) {
    return [...this.sessions.values()].filter(session => session.userId === userId && (desktopId === undefined || session.desktopId === desktopId) && this.authorized(session));
  }
  clipboardPolicyChanged(userId, desktopId) {
    this.channels.policyChanged(userId, desktopId);
    for (const session of this.policySessions(userId, desktopId)) if (session.mode === 'stream' && clipboardPolicyRestricted(session.clipboardPolicy, this.store.clipboardPolicy(userId, session.desktopId))) {
      // Only the clipboard ends, as USB, printers
      // and drives keep the stream: available() refuses a new pair until the
      // next connection takes the narrower grant.
      this.clipboard.closeSession(session, 'clipboard_policy_changed');
    }
  }
  // The stored device permissions (devicePolicy) for streaming.
  devicePolicyChanged(userId, desktopId) {
    // channel-resume-v1: a channel that has no pair now (the stream is away) ends for the session too.
    this.channels.policyChanged(userId, desktopId);
    // Streaming USB (usb-v1) only loses its channel or the affected devices; the stream stays.
    this.usb.policyChanged(userId, desktopId);
    // Streaming printers (print-v1) only lose their channel; the stream stays.
    this.print.policyChanged(userId, desktopId);
    // Streaming drives end with the permission, the stream itself continues.
    for (const session of this.policySessions(userId, desktopId)) if (session.drivePair && !this.drive.allowed(session)) this.drive.closeSession(session, 'drive_policy_changed');
  }
  assertDisplayIdle(desktopId,mode='stream',consoleGrant,reservation) { if (this.vmConfigBusy?.(desktopId)) throw new HttpError(409, 'vm_audio_busy'); if (this.console?.blocked(desktopId,consoleGrant)) throw new HttpError(409,'console_busy'); this.sessionStop.guard(desktopId,mode); const operation=this.displayBusy.get(desktopId); if (operation && operation !== reservation) throw new HttpError(409, 'display_in_progress'); this.hostUpdate?.guard(desktopId); }
  // VM power resets Windows: a stream cleanup still waiting for a display
  // restore, or a display/console operation past its deadline, is no reason to
  // refuse it (a reboot or shutdown drops the Host, which clears both). That
  // barrier would refuse the very reboot that ends it. Only
  // a display change within its deadline and a Host update being installed
  // still refuse power.
  // QEMU preparation additionally retains its lock after timeout/cancellation
  // until the exact worker exits; a missing reply never proves display idle.
  assertPowerAllowed(desktopId) {
    this.assertAccessAdmission?.();
    if (this.vmConfigBusy?.(desktopId)) throw new HttpError(409, 'vm_audio_busy');
    const operation = this.displayBusy.get(desktopId);
    if (operation && (operation.kind === 'qemu-console' || (operation.status === 'pending' && Date.now() < operation.deadline))) throw new HttpError(409, 'display_in_progress');
    this.hostUpdate?.guard(desktopId);
  }
  displaySupported(host) {
    const caps = host?.capabilities;
    return this.hostAuthenticated(host) && !host.capabilitiesPending && Date.now() - (host.capabilitiesAt ?? 0) <= this.heartbeatMs * 3 && caps?.virtualMonitorControl === true && caps.service && caps.interactiveSession && caps.virtualDisplayReady && [1,2].includes(caps.monitors?.length);
  }
  displayPublic(operation) {
    return operation ? { id: operation.id, status: operation.status, requestedCount: operation.count, submittedAt: new Date(operation.submittedAt).toISOString(), checkedAt: new Date(operation.checkedAt).toISOString(), error: operation.error } : null;
  }
  displayView(desktopId, actor, operation) {
    const host = this.hosts.get(desktopId), connected = this.hostAuthenticated(host);
    operation ??= actor === undefined ? null : this.displayOperations.get(this.displayLatest.get(actor + ':' + desktopId));
    const count = connected && host.capabilities?.virtualDisplayReady && [1,2].includes(host.capabilities.monitors?.length) ? host.capabilities.monitors.length : null;
    return { supported: !!this.displaySupported(host), activeCount: count, checkedAt: connected && host.capabilitiesAt ? new Date(host.capabilitiesAt).toISOString() : null,
      stale: !connected || count === null || this.sessionStop.blocked(desktopId) || this.displayBusy.has(desktopId) || Date.now() - (host?.capabilitiesAt ?? 0) > this.heartbeatMs * 3,
      busy: this.displayBusy.has(desktopId), operation: this.displayPublic(operation), error: connected ? null : 'host_offline' };
  }
  displayStatus(user, desktopId, operationId) {
    if (!this.store.assignedDesktop(user.id, desktopId)) throw new HttpError(404, 'desktop_not_found');
    let operation;
    if (operationId !== undefined) {
      operation = this.displayOperations.get(operationId);
      if (!operation || operation.userId !== user.id || operation.desktopId !== desktopId) throw new HttpError(404, 'display_operation_not_found');
    }
    return { display: this.displayView(desktopId, user.id, operation) };
  }
  configureDisplay(user, hash, desktopId, count) {
    this.assertAccessAdmission?.();
    if (![1,2].includes(count)) throw new HttpError(400, 'invalid_display_count');
    const binding = { userId: user.id, tokenHash: hash, desktopId };
    if (!this.authorized(binding)) throw new HttpError(404, 'desktop_not_found');
    this.sessionStop.guard(desktopId);
    const host = this.hosts.get(desktopId);
    if (!this.displaySupported(host)) throw new HttpError(409, 'display_control_unavailable');
    this.assertDisplayIdle(desktopId);
    if (this.powerBusy(desktopId)) throw new HttpError(409, 'power_in_progress');
    const sessions = [...this.sessions.values()].filter(value => value.desktopId === desktopId);
    if (sessions.some(session => session.userId !== user.id || session.mode !== 'stream')) throw new HttpError(409, 'desktop_busy');
    // Negotiated teardown is asynchronous. The caller must retire its session
    // and observe exact Host completion before a new display transaction.
    if(sessions.some(session=>session.hostDispatched&&session.stopProtocol==='stop-v1'))throw new HttpError(409,'desktop_busy');
    if (this.displayBusy.size >= 500 || this.displayOperations.size >= 10000) throw new HttpError(503, 'display_busy');
    const now = Date.now(), operation = { ...binding, host, id: randomUUID(), nonce: newToken(), count, status:'pending', error:null, submittedAt:now, checkedAt:now, deadline:now + this.displayReadyMs };
    this.displayOperations.set(operation.id, operation); this.displayBusy.set(desktopId, operation); this.displayLatest.set(user.id + ':' + desktopId, operation.id);
    for (const session of sessions) this.stop(session, 'display_reconfigure');
    // This section is synchronous: assignment/session cannot change between
    // authorization, reservation and dispatch through this process.
    if (!this.send(host.ws, { type:'host.display.configure', operationId:operation.id, nonce:operation.nonce, count })) this.dropHost(host, 'host_backpressure');
    this.store.audit('display_requested', user.id, desktopId + ':' + count);
    return { display: this.displayView(desktopId, user.id, operation) };
  }
  cancelDisplay(operation, reason) {
    if (operation.kind === 'qemu-console') { this.qemuConsole.cancel(operation, reason); return; }
    if (operation.kind === 'console') { this.consolePreparation.cancel(operation, reason); return; }
    if (this.displayBusy.get(operation.desktopId) !== operation || operation.status !== 'pending') return;
    operation.status = reason === 'authorization_revoked' ? 'failed' : 'unknown'; operation.error = reason; operation.checkedAt = Date.now();
    this.send(operation.host.ws, { type:'host.display.cancel', operationId:operation.id, nonce:operation.nonce, reason });
    // Keep the per-VM lock until the exact worker completion or host loss.
    // A timeout does not establish whether Windows applied the topology.
  }
  displayResult(host, value) {
    const operation = this.displayOperations.get(value.operationId);
    if (!operation || operation.host !== host || this.displayBusy.get(operation.desktopId) !== operation || value.nonce !== operation.nonce || this.hosts.get(operation.desktopId) !== host) return;
    const caps = validCapabilities(value.capabilities);
    const valid = typeof value.ok === 'boolean' && caps && (value.ok ? value.count === operation.count && caps.virtualDisplayReady && caps.monitors?.length === operation.count : displayFailures.has(value.reason));
    if (!valid) { this.cancelDisplay(operation, 'display_result_invalid'); return; }
    if (!this.authorized(operation)) this.cancelDisplay(operation, 'authorization_revoked');
    host.capabilities = caps; host.capabilitiesPending = false; host.capabilitiesAt = Date.now();
    operation.checkedAt = Date.now();
    if (operation.status === 'pending') { operation.status = value.ok ? 'succeeded' : 'failed'; operation.error = value.ok ? null : value.reason; }
    this.displayBusy.delete(operation.desktopId);
    this.store.audit('display_completed', operation.userId, operation.desktopId + ':' + operation.status);
    this.store.audit('display_result_detail', operation.userId, JSON.stringify({ desktopId: operation.desktopId, operationId: operation.id, count: operation.count, status: operation.status, code: operation.error, elapsedMs: operation.checkedAt - operation.submittedAt }));
  }
  connectSignin(user, hash, desktopId, mode, inputOptions, resizeProtocol, clipboardProtocol, request, clientOrigin) {
    this.assertAccessAdmission?.();
    if (!this.signinEnabled) throw new HttpError(409, 'signin_stream_unavailable');
    if (request.signinProtocol !== signinStreamProtocol || !signinId(request.signinAvailabilityId) || mode !== 'stream' || resizeProtocol !== 'epoch-v1' || request.mediaProtocol !== undefined || request.tracks !== undefined) throw new HttpError(400, 'invalid_signin_request');
    const options = streamOptions(inputOptions, request.streamSizeProtocol === streamSizeProtocolV2 ? streamSizeProtocolV2 : false);
    if (options.monitor !== '0') throw new HttpError(400, 'invalid_signin_request');
    const assigned = this.store.assignedDesktop(user.id, desktopId); if (!assigned) throw new HttpError(404, 'desktop_not_found');
    this.assertDisplayIdle(desktopId, mode);
    const host = this.hosts.get(desktopId);
    const offer = this.signinOffers.admit(host, request.signinAvailabilityId);
    if (!this.hostAuthenticated(host) || !host.signinNegotiated || !(host.signinOfferExpiresAt > Date.now()) || !offer) throw new HttpError(409, 'signin_stream_unavailable');
    for (const session of this.sessions.values()) if (session.desktopId === desktopId) throw new HttpError(409, 'desktop_busy');
    if (this.sessions.size >= 500) throw new HttpError(503, 'relay_busy');
    const id = randomUUID(), initial = requestedInitialViewport(mode, request.initialViewportProtocol, request.initialViewport, options, resizeProtocol, null, request.streamSizeProtocol === streamSizeProtocolV2 ? streamSizeProtocolV2 : false);
    const encoderLimits = requestedViewportEncoderLimits(request.viewportEncoderLimitsProtocol, initial);
    const limits = requestedStreamSize(mode, request.streamSizeProtocol, request.decoderLimits, encoderLimits, null);
    const binding = { protocol: signinStreamProtocol, phase: 'signin', workerGeneration: 1, transitionId: randomBytes(16).toString('hex'), mediaScope: { kind: 'composite', epoch: 1 } };
    const clipboardPolicy = this.store.clipboardPolicy(user.id, desktopId);
    const session = { id, desktopId, localPc: assigned.kind === 'pc', userId: user.id, tokenHash: hash, createdAt: Date.now(), host, mode, options, stopProtocol: host.capabilities?.sessionStopProtocol, ws: null, epochMedia: true, epoch: 1, nextEpoch: 1, streamReady: false, expiresAt: Date.now()+30000, clipboardPolicy, clipboardProtocol, clipboardEnabled: false, print: false, driveEnabled: false, usb: false, udpMedia: false, udpDirect: false, audioOpus: false, resume: false, streamSize: limits, streamSizeVersion: limits ? request.streamSizeProtocol : null, initialViewport: initial ? prepareInitialViewport(id, initial) : null, clientOrigin, signinServiceInstance: host.signinServiceInstance };
    session.signinNoRedirects = true;
    session.signinAdmissionOffer = offer;
    session.viewportEncoderLimits = encoderLimits;
    // First slice: no redirected channels or audio for the ENTIRE negotiated
    // session, including its eventual desktop. Never invent future grants.
    session.textInput = negotiateTextInput(requestedTextInput(mode, request.textInputProtocol), host, null);
    session.signinPolicy = new SigninStreamPhase({ sessionId: id, hostBinding: host, binding,
      authorized: () => this.current(session) && this.authorized(session), isCurrentHost: value => value === host && this.hostAuthenticated(value) && host.signinNegotiated && host.signinServiceInstance === session.signinServiceInstance,
      isCurrentPhaseAuthority: (_host, scope, _id, context) => session.signinLease.current(scope, { sameDesktopWorker: context?.sameDesktopWorker === true }),
      validateInfo: (info, phase) => this.signinInfoValid(session, info, phase),
      verifyHostFrame: (_host, value, context) => session.signinVerifiedFrame === value.firstFrameId && (!context.previousWorkerExitRequired || session.signinPreviousExit === value.transitionId),
      validateInput: value => signinInput(value, id, session.textInput) });
    session.signinLease = new SigninServiceLease({ host, sessionId: id, serviceInstance: host.signinServiceInstance, isCurrentHost: value => value === host && this.hostAuthenticated(value) && host.signinNegotiated && host.signinServiceInstance === session.signinServiceInstance, authorized: () => this.current(session) && this.authorized(session), acceptsBinding: value => isDeepStrictEqual(value, session.signinPolicy.binding) || !!session.signinResize && isDeepStrictEqual(value, session.signinResize.previousBinding) });
    this.sessions.set(id, session); this.activityAt = Date.now(); this.previews.forget(desktopId);
    try { this.store.markUsed(user.id, desktopId); } catch {}
    return { sessionId: id, mode, webSocketUrl: brokerWebSocketOrigin(clientOrigin)+'/api/v1/sessions/'+id, transport: 'media-epoch-v1', resizeProtocol: 'epoch-v1', options, clipboardPolicy, signin: binding, sessionCancellationProtocol: 'session-cancel-v1', ...(session.textInput ? { textInputProtocol } : {}), ...(limits ? { streamSizeProtocol: request.streamSizeProtocol } : {}), ...(session.initialViewport ? { initialViewportProtocol, initialViewport: session.initialViewport, viewportProvisioning: true, ...(request.viewportEncoderLimitsProtocol ? { viewportEncoderLimitsProtocol } : {}) } : {}) };
  }
  signinCurrent(session) { return this.signinEnabled && session.host.signinNegotiated && this.current(session) && this.authorized(session) && session.ws?.readyState === WebSocket.OPEN && session.host.signinServiceInstance === session.signinServiceInstance; }
  signinStartAdmission(session) {
    // The HTTP ticket is only a reservation. Issue a new, short service grant
    // on attach, from the SAME still-current eligibility and Host channel.
    // Never copy an admission object supplied by a client.
    if (!this.signinCurrent(session) || session.hostDispatched) return null;
    const host = session.host, now = Date.now();
    const offer = this.signinOffers.admit(host, session.signinAdmissionOffer?.availabilityId);
    if (!offer || offer.windowsState !== session.signinAdmissionOffer.windowsState ||
        session.signinAdmissionOffer.expiresAtMs <= now || offer.revision < session.signinAdmissionOffer.revision ||
        offer.revision !== host.signinOfferRevision) return null;
    const validForMs = Math.min(1000, offer.expiresAtMs - now, host.signinOfferExpiresAt - now);
    const grantRevision = (this.signinAdmissionRevisions.get(host) ?? 0) + 1;
    if (!Number.isInteger(validForMs) || validForMs < 1 || grantRevision > 0xffffffff) return null;
    this.signinAdmissionRevisions.set(host, grantRevision);
    return Object.freeze({ protocol: 'signin-broker-admission-v1', grantRevision,
      eligibilityRevision: offer.revision, serviceInstance: session.signinServiceInstance, validForMs });
  }
  signinWithdraw(host) { this.signinOffers.withdraw(host); for (const session of this.sessions.values()) if (session.host === host && session.signinPolicy) this.signinInvalidate(session); }
  signinChallenge(session, operation, guard = () => true) {
    if (!this.signinCurrent(session) || !session.signinLease.current(session.signinPolicy.binding) || !guard()) return Promise.resolve(false);
    const binding = session.signinPolicy.binding, host = session.host, ws = session.ws, challengeId = randomBytes(16).toString('hex');
    return new Promise(resolve => {
      const entry = { session, host, ws, binding, operation, guard, resolve, timer: setTimeout(() => finish(false), this.signinChallengeMs) };
      const finish = value => { if (this.signinChallenges.get(challengeId) !== entry) return; this.signinChallenges.delete(challengeId); clearTimeout(entry.timer); resolve(value); };
      entry.finish = finish; this.signinChallenges.set(challengeId, entry);
      if (!this.send(host.ws, { type: 'host.signin.authority.query', protocol: signinServiceProtocol, sessionId: session.id, binding, challengeId, operation })) finish(false);
      if (operation === 'first-frame' && session.signinFrame) session.signinFrame.challengeId = challengeId;
    });
  }
  signinInvalidate(session, reason = 'signin_authority_lost') { session.signinLease?.close(); session.signinPolicy?.close(); this.stop(session, reason); }
  receiveSignin(host, value) {
    if (!this.signinEnabled || !host.signinNegotiated || !this.hostAuthenticated(host)) return false;
    if (value.type === 'host.signin.eligibility.withdraw') {
      if (!signinExact(value, ['type','protocol','serviceInstance','revision']) || value.protocol !== signinServiceProtocol ||
          value.serviceInstance !== host.signinServiceInstance || !Number.isInteger(value.revision) ||
          value.revision <= (host.signinOfferRevision ?? 0) || value.revision > 0xffffffff) return false;
      host.signinOfferRevision = value.revision; host.signinOfferExpiresAt = 0;
      this.signinOffers.withdraw(host, value.revision);
      this.send(host.ws, { type: 'host.signin.eligibility.ack', accepted: true });
      // Discovery retirement is independent of an already authorized phase
      // transition. Its capture/input leases and exact old-worker exit fence
      // still enforce the active session; withdrawal must not close its view.
      return true;
    }
    if (value.type === 'host.signin.eligibility') {
      const valid = signinExact(value, ['type','protocol','serviceInstance','revision','windowsState','outputs','validForMs']) && value.protocol === signinServiceProtocol && signinId(value.serviceInstance) && Number.isInteger(value.validForMs) && value.validForMs >= 1 && value.validForMs <= 5000;
      if (!valid || host.signinServiceInstance && host.signinServiceInstance !== value.serviceInstance) return false;
      host.signinServiceInstance = value.serviceInstance;
      const offer = this.signinOffers.observe(host, { revision: value.revision, qualifiedBackend: true, currentService: true, ownedDevice: true, windowsState: value.windowsState, outputs: value.outputs });
      if (offer && value.revision > (host.signinOfferRevision ?? 0)) { host.signinOfferRevision = value.revision; host.signinOfferExpiresAt = Math.min(offer.expiresAtMs, Date.now() + value.validForMs); }
      this.send(host.ws, { type: 'host.signin.eligibility.ack', accepted: !!offer, ...(offer ? { availabilityId: offer.availabilityId } : {}) }); return true;
    }
    const session = this.sessions.get(value.sessionId);
    // An already retired assigned session can still have an ordered service
    // reply in flight. It cannot act for a successor or close that Host.
    if (!session) return typeof value.sessionId === 'string' && sessionPattern.test(value.sessionId);
    if (!session.signinPolicy || session.host !== host || !this.signinCurrent(session)) return false;
    if (value.type === 'host.signin.authority') {
      if (!session.signinLease.observe(host, value)) { const stale = parseSigninBinding(value.binding); return !!stale && session.signinLease.retired(stale); }
      if (value.state === 'withdrawn') this.signinInvalidate(session); return true;
    }
    if (value.type === 'host.signin.authority.result') {
      const entry = this.signinChallenges.get(value.challengeId);
      const good = signinExact(value, ['type','protocol','sessionId','binding','challengeId','serviceInstance','revision','state']) && value.protocol === signinServiceProtocol && entry?.session === session && entry.host === host && entry.ws === session.ws && entry.operation && value.state === 'current' && value.serviceInstance === session.signinServiceInstance && value.revision === session.signinLease.revision && isDeepStrictEqual(value.binding, entry.binding) && isDeepStrictEqual(value.binding, session.signinPolicy.binding) && session.signinLease.current(entry.binding) && entry.guard();
      if (entry) entry.finish(good ? { challengeId: value.challengeId, revision: value.revision } : false); return !!entry;
    }
    if (value.type === 'host.signin.first-frame') {
      const work = session.signinFrame;
      if (!work || !signinExact(value, ['type','protocol','sessionId','binding','challengeId','serviceInstance','revision','firstFrameId','sourceSequence','ptsUs','payloadSHA256','width','height']) || value.protocol !== signinServiceProtocol || value.challengeId !== work.challengeId || value.serviceInstance !== session.signinServiceInstance || !isDeepStrictEqual(value.binding, work.binding) || value.firstFrameId !== work.value.firstFrameId || !signinU64(value.sourceSequence) || !signinU64(value.ptsUs) || !/^[a-f0-9]{64}$/.test(value.payloadSHA256 ?? '') || value.width !== work.value.info.sourceWidth || value.height !== work.value.info.sourceHeight) return false;
      work.proof = value; this.signinFrameReady(session); return true;
    }
    if (value.type === 'stream.phase.info') { void this.signinInfo(session, value); return true; }
    if (value.type === 'stream.phase.prepare') {
      const previous = session.signinPolicy.binding;
      if (!session.signinPolicy.prepare(value, host)) return false;
      session.signinLease.retire(previous); this.signinCloseRedirects(session); this.previews.forget(session.desktopId); session.streamReady = false; session.signinResize = null; session.signinAcceptedInfo = null; session.signinVerifiedFrame = null;
      this.send(session.ws, value); return true;
    }
    // Native exit proof is a separate service receipt, never a Client field.
    if (value.type === 'host.signin.previous-worker-exited' && signinExact(value, ['type','protocol','sessionId','serviceInstance','binding']) && value.protocol === signinServiceProtocol && value.serviceInstance === session.signinServiceInstance && isDeepStrictEqual(value.binding, session.signinPolicy.binding)) { session.signinPreviousExit = value.binding.transitionId; return true; }
    return false;
  }
  signinFrameReady(session) {
    const work = session.signinFrame;
    if (!work?.packet || !work.proof) return;
    const packet = work.packet, proof = work.proof;
    if (packet[0] !== 1 || packet[1] !== 1 || packet.readUInt16LE(2) !== work.binding.mediaScope.epoch || packet.readBigUInt64LE(4).toString() !== proof.ptsUs || !signinIdr(packet.subarray(12)) || createHash('sha256').update(packet.subarray(12)).digest('hex') !== proof.payloadSHA256) return work.finish(false);
    work.finish(true);
  }
  signinInfoValid(session, info, phase) {
    const resize = session.signinResize, options = resize?.nextOptions ?? session.options, limits = resize?.decoderLimits ?? session.streamSize;
    if (!info || typeof info !== 'object' || Array.isArray(info) || !validStreamInfo(info, session.streamSizeVersion ?? !!limits, false) || info.type !== 'stream.info' || info.sessionId !== session.id || info.epoch !== session.signinPolicy.binding.mediaScope.epoch || info.audio?.available !== false || info.fpsTarget !== options.fps || !withinDecoderLimits(info, limits, options.fps) || info.width > options.maxWidth || info.height > options.maxHeight) return false;
    if (resize) return info.requestId === resize.requestId && info.resizeId === resize.resizeId && info.sourceWidth === resize.width && info.sourceHeight === resize.height && (session.viewportEncoderLimits || info.width === resize.width && info.height === resize.height);
    if (session.initialViewport && !session.signinAcceptedInfo && phase === 'signin') return initialViewportInfoMatches(info, session.initialViewport, options);
    return true;
  }
  async signinInfo(session, value) {
    if (session.signinAcceptedInfo && isDeepStrictEqual(session.signinAcceptedInfo, value)) {
      if (!isDeepStrictEqual(session.signinPolicy.binding, { protocol: value.protocol, phase: value.phase, workerGeneration: value.workerGeneration, transitionId: value.transitionId, mediaScope: value.mediaScope })) return;
      if (!this.signinCurrent(session) || !session.signinLease.current(session.signinPolicy.binding) || session.signinPolicy.state === 'await_client_decode' && !session.signinPolicy.hostInfo(value, session.host)) this.signinInvalidate(session, 'invalid_signin_info');
      return;
    }
    if (session.signinFrame || !signinExact(value, ['type','protocol','sessionId','transitionId','workerGeneration','mediaScope','phase','firstFrameId','info']) || !signinId(value.firstFrameId) || !this.signinInfoValid(session, value.info, value.phase)) return this.signinInvalidate(session, 'invalid_signin_info');
    const binding = parseSigninBinding({ protocol: value.protocol, phase: value.phase, workerGeneration: value.workerGeneration, transitionId: value.transitionId, mediaScope: value.mediaScope });
    if (binding && session.signinLease.retired(binding)) return;
    if (!binding || !isDeepStrictEqual(binding, session.signinPolicy.binding)) return this.signinInvalidate(session, 'invalid_signin_info');
    let resolveFrame; const ready = new Promise(resolve => resolveFrame = resolve);
    const work = { binding, value, packet: null, proof: null, finish: accepted => { clearTimeout(work.timer); resolveFrame(accepted); } };
    work.timer = setTimeout(() => work.finish(false), this.signinChallengeMs); session.signinFrame = work;
    try {
      const check = await this.signinChallenge(session, 'first-frame', () => session.signinFrame === work);
      if (!check || !await ready || !this.signinCurrent(session) || session.signinFrame !== work || !isDeepStrictEqual(session.signinPolicy.binding, binding) || !session.signinLease.current(binding) || work.proof.revision !== check.revision) return this.signinInvalidate(session);
      session.signinVerifiedFrame = value.firstFrameId;
      if (!session.signinPolicy.hostInfo(value, session.host)) return this.signinInvalidate(session, 'invalid_signin_info');
      session.signinAcceptedInfo = structuredClone(value);
      if (session.signinResize) { session.options = session.signinResize.nextOptions; if (session.signinResize.decoderLimits) session.streamSize = session.signinResize.decoderLimits; }
      session.epoch = binding.mediaScope.epoch; session.streamReady = true; session.signinResize = null;
      if (!this.send(session.ws, value) || !this.send(session.ws, work.packet, true)) this.stop(session, 'client_backpressure');
    } catch { this.signinInvalidate(session); }
    finally { work.finish(false); if (session.signinFrame === work) session.signinFrame = null; }
  }
  signinMedia(session, data) {
    if (!this.signinCurrent(session) || data.length <= 12 || data.length > MAX_MESSAGE || data[0] !== 1 || (data[1] & ~1) !== 0) return;
    const work = session.signinFrame;
    if (work && !work.packet && data.readUInt16LE(2) === work.binding.mediaScope.epoch) { work.packet = Buffer.from(data); this.signinFrameReady(session); return; }
    if (session.streamReady && data.readUInt16LE(2) === session.epoch && session.signinLease.current(session.signinPolicy.binding) && ['await_client_decode','signin_active','desktop_active'].includes(session.signinPolicy.state)) if (!this.send(session.ws, data, true)) this.stop(session, 'client_backpressure');
  }
  signinCloseRedirects(session) { for (const channel of [this.clipboard, this.drive, this.usb, this.print]) channel.closeSession(session, 'signin_phase_changed'); }
  signinRedirectAvailable(session, owner = null) { return !session.signinPolicy || !session.signinNoRedirects && (owner ? session.signinPolicy.retainsRedirectOwner(owner) : session.signinPolicy.redirectsAllowed()); }
  signinRedirectOwner(session) { return session.signinNoRedirects ? null : session.signinPolicy?.redirectOwner() ?? null; }
  async admitSigninRedirect(session, guard, accept, reject) {
    if (!session.signinPolicy) { if (guard()) accept(); else reject(); return; }
    if (session.signinNoRedirects) return reject();
    const binding = session.signinPolicy.binding, ws = session.ws;
    try {
      if (!session.signinPolicy.redirectsAllowed() || !await this.signinChallenge(session, 'new-redirect', guard) || !this.signinCurrent(session) || session.ws !== ws || !isDeepStrictEqual(session.signinPolicy.binding, binding) || !session.signinPolicy.redirectsAllowed() || !guard()) return reject();
      accept();
    } catch { reject(); }
  }
  async signinClientControl(session, value) {
    if (value.type === 'input.phase') { if (!session.signinPolicy.input(value)) return; if (value.input?.type === 'input' && this.sessionInputAccepted?.(session) === false) return this.stop(session, 'authorization_revoked'); if (!this.send(session.host.ws, value)) this.dropHost(session.host, 'host_backpressure'); return; }
    if (value.type === 'stream.phase.decoded') {
      const binding = session.signinPolicy.binding, ws = session.ws;
      if (!await this.signinChallenge(session, 'decoded', () => session.ws === ws && isDeepStrictEqual(session.signinPolicy.binding, binding)) || !this.signinCurrent(session)) return this.signinInvalidate(session);
      if (!session.signinPolicy.decoded(value)) return this.signinInvalidate(session, 'invalid_signin_ack');
      if (!this.send(session.host.ws, value)) this.dropHost(session.host, 'host_backpressure'); return;
    }
    if (value.type === 'session.stop') return this.stop(session, 'client_stopped');
    if (value.type === 'ping' && validClientControl(value)) { this.send(session.host.ws, { ...value, sessionId: session.id }); return; }
    if (value.type === 'stream.feedback') return; // no alternate input/permission path
    if (value.type === 'stream.resize') return this.signinResize(session, value);
    this.stop(session, 'invalid_input');
  }
  signinResize(session, input) {
    if (session.signinPolicy.state !== 'desktop_active' || !validResize(input, session.streamSizeVersion ?? !!session.streamSize) || input.monitor !== '0') return;
    const previousBinding = session.signinPolicy.binding, epoch = previousBinding.mediaScope.epoch + 1;
    if (!session.signinPolicy.prepareResize({ kind: 'composite', epoch }, { budgetMs: this.resizeReadyMs })) return;
    const resize = { requestId: input.requestId, resizeId: randomUUID(), epoch, width: input.width, height: input.height, fps: input.fps, monitor: input.monitor };
    session.signinResize = { ...resize, previousBinding, nextOptions: session.viewportEncoderLimits ? { ...session.options, fps: input.fps } : { ...session.options, maxWidth: input.width, maxHeight: input.height, fps: input.fps }, ...(input.decoderLimits ? { decoderLimits: { ...input.decoderLimits } } : {}) }; session.streamReady = false;
    this.send(session.ws, { type: 'stream.resizing', ...resize, timeoutMs: this.resizeReadyMs });
    if (!this.send(session.host.ws, { type: 'stream.resize', ...resize, sessionId: session.id, ...(input.decoderLimits ? { decoderLimits: { ...input.decoderLimits } } : {}), signin: session.signinPolicy.binding })) this.dropHost(session.host, 'host_backpressure');
  }
  connect(user, tokenHashValue, desktopId, mode = 'stream', inputOptions, resizeProtocol, clipboardProtocol, trackRequest = {}, clientOrigin = this.publicOrigin, clientTls = 'unknown', takeoverContextRequested = false) {
    const signinRequested = trackRequest.signinProtocol !== undefined || trackRequest.signinAvailabilityId !== undefined;
    this.assertAccessAdmission?.();
    // Streaming is the only connection mode. A released client that still
    // asks for an RDP connection gets the refusal it knows.
    if (mode === 'rdp') throw new HttpError(409, 'rdp_not_supported');
    if (mode !== 'stream') throw new HttpError(400, 'invalid_connection_mode');
    if (resizeProtocol !== undefined && (mode !== 'stream' || resizeProtocol !== 'epoch-v1')) throw new HttpError(400, 'invalid_resize_protocol');
    if (clipboardProtocol !== undefined && (mode !== 'stream' || clipboardProtocol !== 'clipboard-sync-v1')) throw new HttpError(400, 'invalid_clipboard_protocol');
    // stream-size-v1 widens the box and the initial viewport to portrait sizes;
    // the full request is validated below and granted or refused (409) later.
    const portrait = mode === 'stream' && (trackRequest.streamSizeProtocol===streamSizeProtocolV2 ? streamSizeProtocolV2 : streamSizeAsked(trackRequest.streamSizeProtocol));
    const options = mode === 'stream' ? streamOptions(inputOptions, portrait) : undefined;
    const tracks = requestedTracks(mode, trackRequest.mediaProtocol, trackRequest.tracks, options, trackRequest.streamSizeProtocol);
    const profileTracks=requestedProfileTracks(mode,trackRequest.streamProfileTracksProtocol,tracks);
    const requestedViewport = requestedInitialViewport(mode, trackRequest.initialViewportProtocol, trackRequest.initialViewport, options, resizeProtocol, tracks, portrait);
    const viewportEncoderLimits = requestedViewportEncoderLimits(trackRequest.viewportEncoderLimitsProtocol, requestedViewport);
    const streamSizeRequested = requestedStreamSize(mode, trackRequest.streamSizeProtocol, trackRequest.decoderLimits, viewportEncoderLimits, tracks);
    const latencyRequested = requestedInputLatency(mode, trackRequest.inputLatencyProtocol);
    const observationRequested = requestedObservability(mode, trackRequest.observabilityProtocol);
    const cursorRequested = requestedCursor(mode, trackRequest.cursorProtocol);
    const cursorTracksRequested = requestedCursorTracks(mode, trackRequest.cursorProtocol, trackRequest.cursorTracksProtocol, tracks);
    const cursorEchoRequested = requestedCursorEcho(mode, trackRequest.cursorProtocol, trackRequest.cursorEchoProtocol);
    const cursorComposeRequested = requestedCursorCompose(mode, trackRequest.cursorProtocol, trackRequest.cursorComposeProtocol);
    const textInputRequested = requestedTextInput(mode, trackRequest.textInputProtocol);
    const displayScaleRequested = requestedDisplayScale(mode, trackRequest.displayScaleProtocol, trackRequest.displayScale);
    const displayScaleOutputsRequested = requestedDisplayScaleOutputs(mode, trackRequest.displayScaleProtocol, trackRequest.displayScaleOutputsProtocol, tracks);
    const touchInputRequested = requestedTouchInput(mode, trackRequest.touchInputProtocol);
    const relativeMouseRequested = requestedRelativeMouse(mode, trackRequest.relativeMouseProtocol);
    const caretRequested = requestedCaret(mode, trackRequest.caretProtocol);
    const printRequested = requestedPrint(mode, trackRequest.printProtocol);
    const driveRequested = requestedDrive(mode, trackRequest.driveProtocol);
    const driveFsRequested = requestedDriveFs(mode, trackRequest.driveProtocol, trackRequest.driveFs);
    const usbRequested = requestedUsb(mode, trackRequest.usbProtocol);
    if(trackRequest.udpDirectProtocol!==undefined&&(mode!=='stream'||trackRequest.udpDirectProtocol!==udpDirectProtocol))throw new HttpError(400,'invalid_udp_direct_protocol');
    const udpMediaRequested = requestedUdpMedia(mode, trackRequest.udpMediaProtocol);
    const udpTracksRequested=requestedUdpTracks(mode,trackRequest.udpTracksProtocol,tracks);
    const audioOpusRequested = requestedAudioOpus(mode, trackRequest.audioCodecProtocol);
    const resumeRequested = requestedSessionResume(mode, trackRequest.sessionResumeProtocol);
    if (tracks && resizeProtocol !== undefined) throw new HttpError(400, 'invalid_resize_protocol');
    if (signinRequested) return this.connectSignin(user, tokenHashValue, desktopId, mode, inputOptions, resizeProtocol, clipboardProtocol, trackRequest, clientOrigin);
    const assigned = this.store.assignedDesktop(user.id, desktopId);
    if (!assigned) throw new HttpError(404, 'desktop_not_found');
    const localPc = assigned.kind === 'pc';
    if (localPc && profileTracks) throw new HttpError(400, 'invalid_profile_tracks');
    this.assertDisplayIdle(desktopId,mode);
    const host = this.hosts.get(desktopId);
    if (!this.hostAuthenticated(host)) throw new HttpError(409, 'host_not_registered', 'host_not_registered', { reason: 'host_offline' });
    if (!this.modes(host)[mode]) throw new HttpError(409, 'stream_unavailable', 'stream_unavailable', { reason: this.unavailableReason(host, mode) });
    const viewportHost = localPc ? { ...host, capabilities: localPcCapabilities(host.capabilities) } : host;
    if (requestedViewport && !initialViewportSupported(viewportHost,Date.now(),this.heartbeatMs)) throw new HttpError(409, 'initial_viewport_unavailable');
    if (viewportEncoderLimits && host.capabilities?.viewportEncoderLimitsProtocol !== viewportEncoderLimitsProtocol) throw new HttpError(409, 'viewport_encoder_limits_unavailable');
    if (tracks && (!tracksSupported(host,Date.now(),this.heartbeatMs) || tracks.some(t => !host.capabilities.monitors.some(m => m.id === t.id)))) throw new HttpError(409, 'track_streaming_unavailable');
    if(profileTracks&&!profileTracksSupported(host,Date.now(),this.heartbeatMs))throw new HttpError(409,'track_streaming_unavailable');
    if (!localPc && tracks && host.capabilities?.streamProfileProtocol === streamConsoleProfileProtocol && !profileTracks) throw new HttpError(409, 'track_streaming_unavailable');
    // Beyond the released fixed 4-20 Mbps only for a bitrate-adaptive-v1 host.
    const bitrateAdaptive = mode === 'stream' && negotiateBitrate(options, host, tracks);
    // Optional measurement (input-latency-v1): granted only for an ordinary
    // stream session whose host advertises it; never a reason to refuse one.
    const inputLatency = mode === 'stream' && negotiateInputLatency(latencyRequested, host, tracks);
    const observability = observationRequested && observabilityCapability(host.capabilities);
    // Optional cursor separation (cursor-v1): same rule; tracks-v1 keeps the
    // composed cursor unless cursor-tracks-v1 is on both sides.
    const cursor = mode === 'stream' && negotiateCursor(cursorRequested, host, tracks, cursorTracksRequested);
    const cursorTracks = cursor && !!tracks;
    // Optional cursor echo (cursor-echo-v1): only with cursor-v1 on one output.
    const cursorEcho = negotiateCursorEcho(cursorEchoRequested, cursor, host, tracks);
    // Optional cursor in the video while held (cursor-compose-v1): the same rule.
    const cursorCompose = negotiateCursorCompose(cursorComposeRequested, cursor, host, tracks);
    // Optional committed-text input (text-input-v1): same rule; tracks-v1 and
    // released Hosts keep key events only.
    const textInput = mode === 'stream' && negotiateTextInput(textInputRequested, host, tracks);
    // Runtime control can be granted without an initial Windows DPI change.
    // Both still require this request and the Host's advertised capability;
    // independent outputs also display-scale-outputs-v1 on both sides.
    const displayScaleControl = mode === 'stream' && negotiateDisplayScaleControl(trackRequest.displayScaleProtocol, host, tracks, displayScaleOutputsRequested);
    const displayScale = mode === 'stream' ? negotiateDisplayScale(displayScaleRequested, host, tracks, displayScaleOutputsRequested) : null;
    const displayScaleOutputs = displayScaleControl && !!tracks;
    // Optional touch contacts (touch-input-v1): same rule; tracks-v1 and
    // released Hosts keep pointer/key input only.
    const touchInput = mode === 'stream' && negotiateTouchInput(touchInputRequested, host, tracks);
    // Optional relative mouse input (relative-mouse-v1): same rule; one output
    // of two monitors and released Hosts keep positions. tracks-v1 only with a
    // Host that takes it there (relative-mouse-tracks-v1) and the composed cursor.
    const relativeMouse = mode === 'stream' && negotiateRelativeMouse(relativeMouseRequested, host, tracks, cursorCompose);
    // Optional text-caret reports (caret-v1): same rule.
    const caret = mode === 'stream' && negotiateCaret(caretRequested, host, tracks);
    // Optional stream sizes (stream-size-v1): granted only with the Host's
    // capability; a request whose geometry needs it is refused (409) without.
    const streamSize = mode === 'stream' ? negotiateStreamSize(streamSizeRequested, host, tracks, requestedViewport, options, trackRequest.streamSizeProtocol) : null;
    const streamSizeVersion = streamSize ? trackRequest.streamSizeProtocol : null;
    // Optional printer redirection (print-v1): the assignment's `printers`
    // permission AND a capable Host; never a refusal.
    const print = mode === 'stream' && negotiatePrint(printRequested, host, this.store.devicePolicy(user.id, desktopId));
    // Optional USB devices (usb-v1): the assignment's usbDevices permission AND a
    // Host that advertised the channel; otherwise the stream simply has none.
    const usb = mode === 'stream' && negotiateUsb(usbRequested, host, this.store.devicePolicy(user.id, desktopId));
    const udpTracks=negotiateUdpTracks(udpTracksRequested,udpMediaRequested,host,tracks,this.udp,desktopId);
    const udpMedia = negotiateUdpMedia(udpMediaRequested, host, tracks, this.udp)||udpTracks;
    const udpDirect = udpMedia && udpPeerDirectAllowed(this.udp) && trackRequest.udpDirectProtocol === udpDirectProtocol && directCapability(host.capabilities);
    // Optional Opus audio (audio-opus-v1): a capable Host (independent outputs:
    // audio-opus-tracks-v1 too); never a refusal. Resume likewise (tracks-resume-v1).
    const audioOpus = negotiateAudioOpus(audioOpusRequested, host, tracks);
    const resume = negotiateSessionResume(resumeRequested, host, tracks);
    if (options && options.monitor !== '0' && (!host.capabilities?.monitors?.length || (options.monitor !== 'all' && !host.capabilities.monitors.some(monitor => monitor.id === options.monitor)))) throw new HttpError(409, 'monitor_unavailable');
    const occupied = [...this.sessions.values()].filter(session => session.desktopId === desktopId);
    if (occupied.length) {
      const details = this.takeoverDetails(user, tokenHashValue, desktopId, takeoverContextRequested);
      throw new HttpError(409, 'desktop_busy', 'desktop_busy', details);
    }
    if (this.sessions.size >= 500) throw new HttpError(503, 'relay_busy');
    const id = randomUUID();
    const profileViewport = !localPc && !!requestedViewport && profileViewportSupported(host,Date.now(),this.heartbeatMs,true);
    const epochMedia = mode === 'stream' && (localPc || host.capabilities?.streamProfileProtocol !== streamConsoleProfileProtocol || profileViewport) && resizeProtocol === 'epoch-v1' && host.capabilities?.dynamicResolution === true && host.capabilities.resizeProtocol === 'epoch-v1';
    const initialViewport = requestedViewport ? prepareInitialViewport(id,requestedViewport) : null;
    // The grant of this user's assignment to this VM, not of the account.
    const clipboardPolicy = this.store.clipboardPolicy(user.id, desktopId);
    const clipboardEnabled = mode === 'stream' && host.capabilities?.clipboard === true && anyClipboardAllowed(clipboardPolicy);
    const clipboardSync = clipboardProtocol === 'clipboard-sync-v1' && host.capabilities?.clipboardSync === true;
    // Streaming drive redirection (drive-v1): the per-assignment `drives`
    // permission, for any stream session whose Host advertises it.
    const driveEnabled = mode === 'stream' && negotiateDrive(driveRequested, host, this.store.devicePolicy(user.id, desktopId));
    const trackState = tracks ? new TrackStream(id,tracks,options,{prepareMs:this.viewportReadyMs,readyMs:this.resizeReadyMs,mediaMs:this.trackMediaMs,allowRestored:profileTracks,sizeProtocol:streamSizeVersion,decoderLimits:streamSize,audioOpus}) : null;
    this.activityAt = Date.now();
    this.sessions.set(id, { id, desktopId, localPc, userId: user.id, tokenHash: tokenHashValue, createdAt: Date.now(), host, mode, observability, stopProtocol: host.capabilities?.sessionStopProtocol, options, epochMedia, initialViewport, profileViewport, profileTracks, viewportEncoderLimits, clipboardEnabled, clipboardPolicy, clipboardProtocol, clipboardSync, bitrateAdaptive, inputLatency: inputLatency ? inputLatencyState() : null, cursor: cursor ? cursorState() : null, cursorTracks, cursorEcho, cursorCompose, textInput, displayScaleControl, displayScale, displayScaleOutputs, touchInput, relativeMouse, caret, streamSize, streamSizeVersion, print, driveEnabled, driveFs: driveEnabled && driveFsRequested && driveFsCapability(host.capabilities), source: null, usb, udpMedia, udpDirect, udpTracks, audioOpus, resume, epoch: 0, nextEpoch: 0, ws: null, trackState, trackTunnel:null, expiresAt: Date.now() + 30000, streamReady: false });
    // The session grant exists now: remember the use (a convenience that never
    // blocks a connection).
    try { this.store.markUsed(user.id, desktopId); } catch { /* optional */ }
    this.sessions.get(id).clientOrigin = clientOrigin;
    prefetchStreamConsoleIdentity(this, this.sessions.get(id));
    return { ...(udpTracks ? {udpTracksProtocol} : {}), sessionCancellationProtocol: 'session-cancel-v1', ...(resume ? { sessionResumeProtocol, resumeGraceMs: this.resume.clientGraceMs } : {}), ...(audioOpus ? { audioCodecProtocol: audioOpusProtocol } : {}), ...(observability ? { observabilityProtocol, observabilityPath: { clientTls: safeObservabilityTls(clientTls), hostTls: safeObservabilityTls(host.tls) }, observabilityVersions: { broker: this.brokerVersion, host: safeObservabilityVersion(host.identity.version) } } : {}), ...(bitrateAdaptive ? { bitrateProtocol } : {}), ...(inputLatency ? { inputLatencyProtocol } : {}), ...(cursor ? { cursorProtocol, ...(cursorTracks ? { cursorTracksProtocol } : {}), ...(cursorEcho ? { cursorEchoProtocol } : {}), ...(cursorCompose ? { cursorComposeProtocol } : {}) } : {}), ...(textInput ? { textInputProtocol } : {}), ...(displayScaleControl ? { displayScaleProtocol, ...(displayScaleOutputs ? { displayScaleOutputsProtocol } : {}), ...(displayScale !== null ? { displayScale } : {}) } : {}), ...(touchInput ? { touchInputProtocol } : {}), ...(relativeMouse ? { relativeMouseProtocol } : {}), ...(caret ? { caretProtocol } : {}), ...(usb ? { usbProtocol, ...(tracks ? { usbTracksProtocol } : {}), usbWebSocketUrl: brokerWebSocketOrigin(clientOrigin) + '/api/v1/sessions/' + id + '/usb' } : {}), ...(streamSize ? { streamSizeProtocol:streamSizeVersion } : {}), ...(print ? { printProtocol, printWebSocketUrl: brokerWebSocketOrigin(clientOrigin) + '/api/v1/sessions/' + id + '/print' } : {}), ...(profileTracks?{streamProfileTracksProtocol:profileTracksProtocol}:{}), ...(viewportEncoderLimits ? { viewportEncoderLimitsProtocol } : {}), ...(initialViewport ? { initialViewportProtocol, initialViewport } : {}), sessionId: id, webSocketUrl: brokerWebSocketOrigin(clientOrigin) + '/api/v1/sessions/' + id, mode, transport: tracks ? trackTransport : epochMedia ? 'media-epoch-v1' : 'media-v1', ...(tracks ? {mediaProtocol:trackProtocol,tracks,trackViewportProvisioning:true} : {}), ...(mode === 'stream' ? { clipboardPolicy } : {}), ...(clipboardProtocol ? { clipboardProtocol, clipboardSync } : {}), ...(options ? { options } : {}), ...(epochMedia ? { resizeProtocol: 'epoch-v1', viewportProvisioning: host.capabilities.viewportProvisioning === true } : {}), ...(clipboardEnabled ? { clipboardWebSocketUrl: brokerWebSocketOrigin(clientOrigin) + '/api/v1/sessions/' + id + '/clipboard' } : {}), ...(driveEnabled ? { driveProtocol, ...(driveFsRequested && driveFsCapability(host.capabilities) ? { driveFs: true } : {}), driveWebSocketUrl: brokerWebSocketOrigin(clientOrigin) + '/api/v1/sessions/' + id + '/drive' } : {}) };
  }
  driveStartFields(session) {
    return session.driveEnabled ? { driveProtocol, ...(session.driveFs === true ? { driveFs: true } : {}), driveUrl: brokerWebSocketOrigin(session.host.origin ?? this.publicOrigin) + '/api/v1/hosts/sessions/' + session.id + '/drive' } : {};
  }
  send(ws, value, binary = false, written = null, failed = null) {
    // A client away (session-resume-v1) has no socket: what it misses is dropped.
    if (!ws) return true;
    // Serialize a control value once; the bounded size and sent text are the same bytes.
    const payload = binary ? value : JSON.stringify(value);
    const size = binary ? value.length : Buffer.byteLength(payload);
    if (ws.readyState !== WebSocket.OPEN || ws.bufferedAmount + size > MAX_QUEUE) { ws.terminate(); return false; }
    // `written`: the socket accepted the bytes (diagnostic timing only).
    ws.send(payload, { binary }, error => { if (error) { failed?.(); ws.terminate(); } else written?.(); });
    return true;
  }
  // The attached controller of a host channel. connect() admits one session
  // per desktop and a host object serves exactly one desktop, so at most one
  // live session matches. Every media/control packet used to copy and scan all
  // sessions; reuse the previous match only while it is still that live,
  // attached session, otherwise repeat the original insertion-order scan.
  hostSession(host) {
    // A session whose client is away (session-resume-v1) still owns its Host's media.
    const cached = host.attachedSession;
    if (cached && cached.host === host && (cached.ws || cached.detached) && this.sessions.get(cached.id) === cached) return cached;
    host.attachedSession = undefined;
    for (const value of this.sessions.values()) if (value.host === host && (value.ws || value.detached)) return host.attachedSession = value;
    return undefined;
  }
  cancel(user, hash, id) {
    const session = sessionPattern.test(id) ? this.sessions.get(id) : null;
    const current = this.store.authenticate(hash);
    if (!session || !current || current.id !== user.id ||
        session.userId !== user.id || session.tokenHash !== hash ||
        !this.store.assignedDesktop(user.id, session.desktopId)) {
      throw new HttpError(404, 'session_not_found');
    }
    if (session.ws || this.resume.held(session)) this.stop(session, 'client_stopped');
    else this.sessions.delete(id); // Never start a Host merely to cancel a ticket.
    try { this.store.audit('session_cancelled', user.id, JSON.stringify({ sessionId: id, desktopId: session.desktopId, mode: session.mode, attached: !!session.ws })); } catch { /* Optional audit cannot retain a controller. */ }
    // Acknowledges broker ownership release only, not Windows profile restore.
    return { sessionId: id, released: true };
  }
  // desktop_busy recovery: the same account (or an administrator) may end the
  // existing controller, then connect through the ordinary admission path.
  // This only releases; it never admits or skips the Host cleanup barrier.
  // This read-only observation runs before client display-count preparation.
  // It deliberately does not call connect, display/console/power preparation,
  // Host RPC, stop, or session allocation. Both admission and this endpoint use
  // the same offer issuer, exact live-set snapshot, and existing CAS consumer.
  takeoverDetails(user, hash, desktopId, contextual = false) {
    const current = contextual ? this.store.authenticate(hash) : user;
    if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
    const assigned = contextual ? this.store.assignedDesktop(current.id, desktopId) : null;
    if (contextual && (!assigned || !this.store.listDesktops(current.id).some(desktop => desktop.id === desktopId))) throw new HttpError(404, 'desktop_not_found');
    const sessions = [...this.sessions.values()].filter(session => session.desktopId === desktopId);
    if (!sessions.length) return null;
    const takeover = this.takeoverAllowed(current, desktopId), details = { reason: 'controller_in_use', takeover };
    if (contextual && takeover) details.takeoverOffer = this.takeoverContexts.issue({ userId: current.id, tokenHash: hash, desktop: assigned, sessions,
      usernameForSession: session => this.store.db.prepare('SELECT username FROM users WHERE id=?').get(session.userId)?.username });
    return details;
  }
  takeoverContext(user, hash, desktopId) {
    this.assertAccessAdmission?.();
    const details = this.takeoverDetails(user, hash, desktopId, true);
    if (details) throw new HttpError(409, 'desktop_busy', 'desktop_busy', details);
    return { busy: false };
  }
  // Any account assigned to the desktop may end its streams after the
  // explicit confirmation, another account's included (a stream left running
  // at home can be ended from elsewhere), not administrators alone. The
  // assignment itself is checked by every caller.
  takeoverAllowed(user, desktopId) {
    return [...this.sessions.values()].some(session => session.desktopId === desktopId);
  }
  takeover(user, hash, desktopId, confirmation) {
    const current = this.store.authenticate(hash);
    if (!current || current.id !== user.id || current.must_change_credentials || !this.store.assignedDesktop(user.id, desktopId)) throw new HttpError(404, 'desktop_not_found');
    const sessions = [...this.sessions.values()].filter(session => session.desktopId === desktopId);
    if (sessions.length && !this.takeoverAllowed(current, desktopId)) throw new HttpError(409, 'desktop_busy', 'desktop_busy', { reason: 'other_user', takeover: false });
    if (confirmation !== undefined) this.takeoverContexts.consume({ context: confirmation.context, userId: current.id, tokenHash: hash, desktopId, sessions });
    for (const session of sessions) {
      // The ended stream is told whether its own account or another one took
      // it, and another account's name with it (stop() sends it as `by`).
      if (session.userId !== current.id) session.takenBy = current.display_name || current.username;
      if (session.ws || this.resume.held(session)) this.stop(session, 'taken_over', session.userId === current.id ? 'same_account' : 'other_account');
      else this.sessions.delete(session.id); // An unattached ticket never reached the Host.
    }
    if (sessions.length) try { this.store.audit('session_taken_over', user.id, JSON.stringify({ desktopId, sessions: sessions.length, otherUser: sessions.some(session => session.userId !== user.id) })); } catch { /* Optional audit cannot retain a controller. */ }
    return { released: sessions.length > 0, sessionCleanup: this.sessionStop.view(desktopId) };
  }
  stop(session, reason = 'disconnected', detail, observe = true) {
    // session-resume-v1: a client that missed its heartbeat or fell behind its
    // send bound may come back on a new socket.
    if ((reason === 'client_timeout' || reason === 'client_backpressure') && this.resume.detachClient(session, reason)) return;
    clearObservability(session);
    if (!this.sessions.delete(session.id)) return;
    this.usage?.ended(session, reason);
    this.udp?.revoke(session.id);
    this.activityAt = Date.now();
    if (observe) this.hostEvent(session.desktopId, 'session');
    const retirement=this.sessionStop.begin(session);
    if (retirement) this.cleanupAuthorization.set(retirement, { userId: session.userId, tokenHash: session.tokenHash, desktopId: session.desktopId });
    this.clipboard.closeSession(session, 'session_ended');
    this.print.closeSession(session, 'session_ended');
    this.drive.closeSession(session, 'session_ended');
    this.usb.closeSession(session, 'session_ended');
    if (session.signinPolicy) {
      const binding = session.signinPolicy.binding;
      if (session.signinLease.current(binding)) this.send(session.host.ws, { type: 'input.phase', protocol: signinStreamProtocol, sessionId: session.id, transitionId: binding.transitionId, workerGeneration: binding.workerGeneration, mediaScope: binding.mediaScope, input: { type: 'input.reset' } });
      session.signinPolicy.close(); session.signinLease.close(); session.signinFrame?.finish(false);
      for (const entry of this.signinChallenges.values()) if (entry.session === session) entry.finish(false);
      this.previews.forget(session.desktopId);
    } else this.send(session.host.ws, { type: 'input.reset', sessionId: session.id });
    this.send(session.host.ws, { type: 'session.stop', sessionId: session.id, reason, ...this.sessionStop.fields(retirement) });
    if (session.ws?.readyState === WebSocket.OPEN) {
      // taken_over by another account: its username, as the takeover offer shows the taker this one's.
      const by = reason === 'taken_over' && typeof session.takenBy === 'string' ? session.takenBy.slice(0, 64) : '';
      this.send(session.ws, { type: 'session.ended', reason, ...(detail ? { detail } : {}), ...(by ? { by } : {}) }); session.ws.close(1000, reason.slice(0, 100));
    }
    if (session.trackTunnel?.readyState === WebSocket.OPEN) {
      this.send(session.trackTunnel,{type:'input.reset',sessionId:session.id});
      session.trackTunnel.close(1000,reason.slice(0,100));
    }
  }
  unavailable(session, reason) {
    if (this.sessions.get(session.id) !== session) return;
    if (session.ws) this.send(session.ws, { type: 'stream.unavailable', sessionId: session.id, reason });
    this.stop(session, reason);
  }
  // notice-v1 (notices.mjs): these accounts (null: everyone) have notices to
  // read. Contentless, and only for a session whose client asked for it when it
  // opened its control socket (X-ToTB-Live: notices), so a released client
  // never receives it. A client that is behind misses it and reads on its own
  // timer; a notice never ends a stream.
  noticesChanged(userIds = null) {
    const users = userIds ? new Set(userIds) : null, revision = this.noticesRevision = (this.noticesRevision ?? 0) + 1;
    for (const session of this.sessions.values()) {
      const ws = session.ws;
      if (!session.notices || !ws || ws.readyState !== WebSocket.OPEN || ws.bufferedAmount > 65536 || (users && !users.has(session.userId))) continue;
      this.send(ws, { type: 'notices', revision });
    }
  }
  current(session) {
    return this.sessions.get(session.id) === session && this.hosts.get(session.desktopId) === session.host && this.hostAuthenticated(session.host);
  }
  // keyframe-v1: media (kinds 1 and 2) for a client that fell behind. Video
  // stops at the first packet past the skip bound and resumes only at an IDR
  // the Host was asked for; audio waits until the backlog drains.
  mediaSkipped(session, data) {
    if (session.host.capabilities?.keyFrameProtocol !== keyFrameProtocol || session.ws?.readyState !== WebSocket.OPEN) return false;
    const video = data[0] === 1, backlog = session.ws.bufferedAmount + data.length > MEDIA_SKIP;
    if (session.skippingVideo && video) {
      if ((data[1] & 1) && !backlog) { session.skippingVideo = false; return false; }
      this.requestKeyFrame(session); return true;
    }
    if (!backlog) return false;
    if (video) { session.skippingVideo = true; this.requestKeyFrame(session); }
    return true;
  }
  requestKeyFrame(session) {
    const now = Date.now();
    if (now - (session.keyFrameRequestedAt ?? 0) < 500) return;
    session.keyFrameRequestedAt = now;
    if (!this.send(session.host.ws, { type: 'stream.keyframe', sessionId: session.id })) this.dropHost(session.host, 'host_backpressure');
  }
  dropHost(host, reason, detail) {
    if (this.signinEnabled) this.signinOffers.withdraw(host);
    this.previews?.hostLost(host);
    this.qemuConsole.hostLost(host);
    this.ime?.hostLost(host);
    this.sessionStop.hostLost(host);
    const current = this.hosts.get(host.identity.desktop_id) === host;
    if (current) this.hosts.delete(host.identity.desktop_id);
    if (this.signinEnabled) this.signinOffers.forgetDisconnected(host);
    if (current) this.hostEvent(host.identity.desktop_id, 'disconnected', { reason, unexpected: reason !== 'host_revoked' && reason !== 'server_stopping' });
    let ended = 0, displayInterrupted = 0, held = 0;
    // host-resume-v1: a session can wait for this Host to come back.
    for (const session of this.sessions.values()) if (session.host === host) { if (this.resume.hostLost(session, reason, detail)) ++held; else { this.stop(session, reason, detail); ++ended; } }
    for (const operation of this.displayBusy.values()) if (operation.host === host) {
      operation.status = 'unknown'; operation.error = reason; operation.checkedAt = Date.now(); this.displayBusy.delete(operation.desktopId); ++displayInterrupted;
    }
    // Record connections that ended sessions, interrupted display preparation
    // or ended for a specific cause. The Host's own fixed reason also reaches
    // its diagnostics after it reconnects.
    // A Host failing on every reconnect without sessions is recorded once a minute.
    const now = Date.now(), desktopId = host.identity.desktop_id;
    if (current && (ended || held || displayInterrupted || detail) && (ended || held || now - (this.hostCloseAudits.get(desktopId) ?? 0) >= 60000)) {
      this.hostCloseAudits.set(desktopId, now);
      if (this.hostCloseAudits.size > 1000) this.hostCloseAudits.delete(this.hostCloseAudits.keys().next().value);
      try { this.store.audit('host_channel_closed', host.identity.id, [desktopId, reason, detail ?? '', ended].join(':')); } catch { /* Optional audit cannot keep a dead channel. */ }
    }
    // A broker restart (an update) closes the channel with 1012 Service
    // Restart, after the session.stop messages already queued on it, so a Host
    // that knows the code reconnects within a quarter second instead of after
    // its 2 s wait. The relay's 1 s closeTimeout bounds a Host that does not
    // answer; released Hosts read it as an ordinary peer close.
    if (reason === 'server_stopping' && host.ws.readyState === WebSocket.OPEN) host.ws.close(1012, 'server_stopping');
    else host.ws.terminate();
  }
  revalidate() {
    this.consolePrincipalCleanup?.();
    this.sessionStop.tick();
    for (const host of this.hosts.values()) {
      const current = this.store.authenticateHost(host.tokenHash);
      if (!current || current.id !== host.identity.id) this.dropHost(host, 'host_revoked');
    }
    for (const session of this.sessions.values()) {
      if(session.udpTracks&&this.resume.held(session))this.retireUdpTracks(session);
      if (this.resume.expire(session)) continue;
      if (!this.authorized(session) || (!session.ws && !this.resume.held(session) && session.expiresAt < Date.now())) { this.stop(session, this.store.sessionRevocationReason(session.tokenHash, Date.now(), session.userId) ?? 'authorization_revoked'); continue; }
      // A narrower clipboard grant closes only the clipboard; the stream's other checks still run.
      if (session.mode === 'stream' && session.clipboardPair && clipboardPolicyRestricted(session.clipboardPolicy, this.store.clipboardPolicy(session.userId, session.desktopId))) this.clipboard.closeSession(session, 'clipboard_policy_changed');
      if (session.signinPolicy && (session.signinLease.closed || ['invalidated','expired','closed'].includes(session.signinPolicy.state))) this.signinInvalidate(session);
      else if (session.profilePreparing && Date.now() >= session.profileDeadline) this.unavailable(session, 'virtual_display_required');
      else if (session.resize && session.resize.deadline < Date.now()) this.resizeFailed(session, session.resize, 'resize_timeout');
      else if (session.initialFrameDeadline && session.initialFrameDeadline < Date.now()) this.unavailable(session,'capture_failed');
      // tracks-resume-v1: no media and no fresh capabilities while a side is
      // away; the restart after it has its own deadline (TrackStream.restart).
      else if (this.resume.held(session)) continue;
      else if (session.trackState?.expired) this.unavailable(session,session.trackState.pending?'tracks_prepare_timeout':'tracks_ready_timeout');
      else if (session.trackState?.stalled) this.unavailable(session,'tracks_source_stalled');
      // group-v1 binds the current session to exact prepared outputs and live
      // media/decoder epochs. Generic Host readiness can change during SDK work
      // and arrive after completion on the other socket. Its capabilities gate
      // later operations; current source deadlines and authorization stay active.
      else if (session.trackState?.ready && !session.profileTracks && !tracksSupported(session.host,Date.now(),this.heartbeatMs)) this.unavailable(session,'track_streaming_unavailable');
      if(this.sessions.get(session.id)===session&&session.udpTracks)this.tickUdpTracks(session);
    }
    this.udp?.sweep();
    this.previews?.sweep();
    for (const operation of this.displayBusy.values()) {
      if (operation.kind === 'qemu-console') continue; // Exact console principal/cancellation is checked below.
      if (!this.authorized(operation)) this.cancelDisplay(operation, 'authorization_revoked');
      else if (operation.deadline < Date.now()) this.cancelDisplay(operation, 'display_timeout');
    }
    for (const [id, operation] of this.displayOperations) if (this.displayBusy.get(operation.desktopId) !== operation && Date.now() - operation.checkedAt > 600000) {
      this.displayOperations.delete(id);
      if (this.displayLatest.get(operation.userId + ':' + operation.desktopId) === id) this.displayLatest.delete(operation.userId + ':' + operation.desktopId);
    }
    this.consolePreparation.cleanup();
    this.qemuConsole.revalidate();
    this.console.revalidate();
    this.clipboard.revalidate();
    this.print.revalidate();
    this.drive.revalidate();
    this.usb.revalidate();
  }
  checkHeartbeat() {
    this.clipboard.heartbeat();
    this.print.heartbeat();
    this.drive.heartbeat();
    this.usb.heartbeat();
    for (const host of this.hosts.values()) {
      // Traffic since the last check: alive, and never pinged mid-frame.
      if (host.traffic) { host.traffic = false; host.alive = true; continue; }
      if (!host.alive || Date.now() - host.lastSeen > this.heartbeatMs * 3) { this.dropHost(host, 'host_timeout'); continue; }
      host.alive = false; host.ws.ping();
    }
    for (const session of this.sessions.values()) if (session.ws?.readyState === WebSocket.OPEN) {
      // The Host's track tunnel is a WinHTTP socket too (see above).
      const trackTraffic = session.trackTraffic === true;
      session.trackTraffic = false;
      if (session.alive === false) { this.stop(session, 'client_timeout'); continue; }
      session.alive = false; session.ws.ping();
      if (session.trackTunnel?.readyState === WebSocket.OPEN) {
        if (trackTraffic) session.trackAlive = true;
        else {
          if (session.trackAlive === false) { if (!this.resume.trackLost(session,'tracks_source_timeout',undefined,true)) this.stop(session,'tracks_source_timeout');continue; }
          session.trackAlive=false;session.trackTunnel.ping();
        }
      }
    }
  }
  attach(server, { upgrade, ownsLifecycle = true } = {}) {
    server.on('upgrade', (req, socket, head) => {
      const reject = (status, reason, code) => {
        if (socket.destroyed || socket.writableEnded) return;
        // Immediate destroy can discard the encrypted HTTP status before TLS
        // writes it. Flush the bounded rejection, with an absolute close limit
        // so a peer that stops reading cannot retain an unauthenticated socket.
        const deadline = setTimeout(() => socket.destroy(), 2000); deadline.unref();
        socket.once('close', () => clearTimeout(deadline));
        socket.once('error', () => socket.destroy());
        // host-machine-v1: a named refusal the Host can read (X-Multitap-Host-Error).
        socket.end(`HTTP/1.1 ${status} ${reason}\r\n${code ? `X-Multitap-Host-Error: ${code}\r\n` : ''}Connection: close\r\nContent-Length: 0\r\n\r\n`);
      };
      try {
        const url = new URL(req.url, 'http://localhost');
        if (upgrade?.(req, socket, head, url, reject)) return;
        const origin = req.brokerOrigin ?? this.publicOrigin;
        if (url.search || url.hash || (req.headers.origin && req.headers.origin !== origin)) return reject(403, 'Forbidden');
        const scoped = /^\/api\/v1\/(hosts\/)?sessions\/([0-9a-f-]{36})(?:\/|$)/.exec(url.pathname);
        if (scoped && req.brokerOrigin) {
          const current = this.sessions.get(scoped[2]);
          if (current && origin !== (scoped[1] ? current.host.origin ?? this.publicOrigin : current.clientOrigin ?? this.publicOrigin)) return reject(403, 'Forbidden');
        }
        if (this.console.upgrade(req, socket, head, url, reject)) return;
        if (url.pathname === '/api/v1/hosts/channel') {
          const token = /^Host ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization ?? '')?.[1];
          const identity = token ? this.store.authenticateHost(tokenHash(token)) : null;
          if (!identity) return reject(401, 'Unauthorized');
          // host-machine-v1: the Host names the SMBIOS UUID of the Windows it runs
          // on. A full clone of a registered VM carries the same token but runs on
          // another machine: refuse it before it can take the channel the original
          // needs (409 below). A Host that sends nothing (older versions) is unchanged.
          const machine = req.headers['x-multitap-host-machine'];
          if (machine !== undefined) {
            if (typeof machine !== 'string' || !uuidPattern.test(machine)) return reject(400, 'Bad Request');
            const registered = this.store.hostMachineUuid(identity.id);
            if (registered && machine.toLowerCase() !== registered) { this.hostMachineChanged(identity); return reject(403, 'Forbidden', 'host_machine_changed'); }
          }
          if (this.hosts.has(identity.desktop_id)) return reject(409, 'Conflict');
          if (this.hosts.size >= 500) return reject(503, 'Unavailable');
          const protocol = req.headers['x-multitap-host-protocol'];
          if (protocol !== undefined && protocol !== 'capabilities-v1') return reject(400, 'Bad Request');
          this.wss.handleUpgrade(req, socket, head, ws => this.attachHost(ws, identity, tokenHash(token), protocol === 'capabilities-v1', origin, req.brokerTls ?? observabilityTls(req)));
          return;
        }
        if (this.clipboard.upgrade(req, socket, head, url, reject)) return;
        if (this.print.upgrade(req, socket, head, url, reject)) return;
        if (this.drive.upgrade(req, socket, head, url, reject)) return;
        if (this.usb.upgrade(req, socket, head, url, reject)) return;
        const trackId = /^\/api\/v1\/hosts\/sessions\/([0-9a-f-]+)\/tracks$/.exec(url.pathname)?.[1];
        if (trackId) {
          const session=sessionPattern.test(trackId)?this.sessions.get(trackId):null;
          const token=/^Host ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization??'')?.[1];
          const hash=token?tokenHash(token):null,identity=hash?this.store.authenticateHost(hash):null;
          if(!session || !identity || identity.id!==session.host.identity.id || hash!==session.host.tokenHash ||
             !this.current(session) || !this.authorized(session))return reject(401,'Unauthorized');
          // tracks-resume-v1: the Host's next tunnel of a session that waits for it.
          const returning=!!session.trackState && this.resume.trackReturning(session);
          if(!session.trackState || (!returning && (!session.ws || session.trackTunnel)) || session.trackState.expired)return reject(409,'Conflict');
          this.wss.handleUpgrade(req,socket,head,ws=>this.attachTrackTunnel(ws,session,returning));return;
        }
        const id = /^\/api\/v1\/sessions\/([0-9a-f-]+)$/.exec(url.pathname)?.[1];
        const session = id && sessionPattern.test(id) ? this.sessions.get(id) : null;
        const token = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization ?? '')?.[1];
        // session-resume-v1: the same account's client returns to its session,
        // also while that session waits for its Host.
        const resumeToken = req.headers[resumeHeader];
        if (!session || !token || session.tokenHash !== tokenHash(token) || !(this.current(session) || (resumeToken !== undefined && session.hostHeld)) || !this.authorized(session)) return reject(401, 'Unauthorized');
        // notice-v1 (notices.mjs): a client that asks for it hears `notices` on this socket (noticesChanged below).
        session.notices = typeof req.headers['x-totb-live'] === 'string' && req.headers['x-totb-live'].length <= 64 && req.headers['x-totb-live'].split(',').some(item => item.trim().toLowerCase() === 'notices');
        if (resumeToken !== undefined) return this.resume.upgrade(req, socket, head, session, resumeToken, reject);
        if (session.ws || this.resume.held(session) || session.expiresAt < Date.now()) return reject(409, 'Conflict');
        // channel-resume-v1: the client says on its session socket that it dials its side channels again.
        this.channels.negotiate(session, req.headers[channelResumeHeader]);
        this.wss.handleUpgrade(req, socket, head, ws => this.attachClient(ws, session));
      } catch { reject(400, 'Bad Request'); }
    });
    if (ownsLifecycle) {
      const close = server.close.bind(server);
      server.close = (...args) => { this.close(); return close(...args); };
    }
  }
  // host-machine-v1: one audit row per Host per 10 minutes, so an older copy
  // that keeps dialling cannot fill the log.
  hostMachineChanged(identity, now = Date.now()) {
    const last = this.machineRefusals.get(identity.id);
    if (last !== undefined && now - last < 10 * 60 * 1000) return;
    if (this.machineRefusals.size >= 1000) this.machineRefusals.clear();
    this.machineRefusals.set(identity.id, now);
    try { this.store.audit('host_machine_changed', identity.id, identity.desktop_id, now); } catch { /* Optional audit cannot change the refusal. */ }
  }
  attachHost(ws, identity, hash, capabilitiesRequired = false, origin = this.publicOrigin, tls = 'unknown') {
    // Upgrade completion can follow an inventory sync/re-enrollment. Never
    // install a stale authenticated identity over a fresh Host connection.
    if (this.store.authenticateHost(hash)?.id !== identity.id || this.hosts.has(identity.desktop_id)) { ws.close(1008, 'host_revoked'); return; }
    const host = { ws, identity, origin, tls: safeObservabilityTls(tls), tokenHash: hash, capabilitiesPending: capabilitiesRequired, alive: true, lastSeen: Date.now(), windowStart: Date.now(), messages: 0, bytes: 0 };
    this.hosts.set(identity.desktop_id, host); this.store.hostSeen(identity.id);
    this.hostEvent(identity.desktop_id, 'connected');
    const seen = version => {
      if (!this.hostAuthenticated(host)) { this.dropHost(host, 'host_revoked'); return; }
      host.alive = true; host.lastSeen = Date.now();
      const versionChanged = version !== undefined && version !== host.identity.version;
      if (version !== undefined) host.identity.version = version;
      if (versionChanged) this.hostEvent(identity.desktop_id, 'version');
      this.store.hostSeen(identity.id, host.lastSeen, version);
    };
    ws.on('pong', () => seen());
    ws.on('error', error => this.dropHost(host, 'host_error', hostErrorDetail(error)));
    ws.on('close', () => this.dropHost(host, 'host_disconnected'));
    ws.on('message', (data, binary) => {
      // Check before media, capabilities, heartbeat or completion can act for
      // a VM whose authoritative UUID changed since this socket attached.
      if (!this.hostAuthenticated(host)) return this.dropHost(host, 'host_revoked');
      host.traffic = true;
      if (Date.now() - host.windowStart >= 1000) { host.windowStart = Date.now(); host.messages = 0; host.bytes = 0; }
      if (++host.messages > 1000 || (host.bytes += data.length) > 32 * 1024 * 1024) return this.dropHost(host, 'host_rate_limit');
      const session = this.hostSession(host);
      if (binary) {
        if (session?.signinPolicy) { this.signinMedia(session, data); return; }
        // Independent media belongs only to its session-bound socket. Late
        // worker packets on the reusable control channel cannot select a new
        // controller by matching topology epoch1 again.
        if (parseTrackPacket(data, true)) return; // either audio format (audio-opus-tracks-v1)
        // desktop-preview-v1 pictures ("TVP1"): kept by this broker, never forwarded.
        if (this.previews?.binary(host, data)) return;
        // cursor-v1 records (cursor.mjs), checked before the released media
        // envelope so kinds 1/2 keep exactly their released handling.
        if (data[0] === cursorKind) {
          const decision = cursorPacket(session, data);
          if (decision === 'invalid') return this.dropHost(host, 'invalid_media');
          if (decision === 'rate') return this.dropHost(host, 'host_rate_limit');
          if (decision === 'forward' && !session.ws) this.resume.cursor(session, data);
          else if (decision === 'forward' && !this.send(session.ws, data, true)) this.stop(session, 'client_backpressure');
          return;
        }
        if (!validMediaEnvelope(data, audioOpusCapability(host.capabilities))) return this.dropHost(host, 'invalid_media');
        if (session?.trackState) return;
        // Frames already in transit may arrive after reset/stop, or before a
        // fresh stream.info following reconnect. They belong to no decoder.
        // Drop them without taking the reusable host channel offline.
        if (session?.mode !== 'stream' || !session.streamReady) return;
        if (!session.ws) return; // session-resume-v1: the client is away; it restarts at an IDR.
        if (data.readUInt16LE(2) !== session.epoch) return;
        // audio-opus-v1: a Host's Opus packet never reaches a session that hears PCM.
        if (data[0] === 2 && !validAudioPayload(data.length - 12, session.audioOpus)) return;
        if (data[0] === 1 && session.awaitingKeyFrame) {
          if (!(data[1] & 1)) return;
          session.awaitingKeyFrame = false;
          session.initialFrameDeadline = null;
        }
        if (session.initialViewport && session.awaitingKeyFrame) return;
        if (this.mediaSkipped(session, data)) return;
        // input-latency-v1: the tagged video packet's relay share (receipt -> written).
        const timed = inputLatencyTimedPacket(session.inputLatency, data) ? performance.now() : 0;
        const observed = session.observability && data[0] === 1 ? beginObservabilityForward(session, session.epoch) : null;
        const written = timed || observed ? () => {
          if (timed) recordInputLatencyForward(session.inputLatency, performance.now() - timed);
          completeObservabilityForward(session, observed, this.current(session));
        } : null;
        const failed = observed ? () => completeObservabilityForward(session, observed, false) : null;
        if (!this.send(session.ws, data, true, written, failed)) { failed?.(); this.stop(session, 'client_backpressure'); }
        return;
      }
      let value;
      try { if (data.length > 8192) throw new Error(); value = JSON.parse(data.toString('utf8')); } catch { return this.dropHost(host, 'invalid_control'); }
      if (!value || typeof value !== 'object' || Array.isArray(value)) return this.dropHost(host, 'invalid_control');
      if (value.type === 'host.heartbeat') {
        // This is untrusted, self-reported display metadata, never an
        // authorization or binary-integrity claim. Old hosts omit it.
        const version = typeof value.version === 'string' && value.version.length <= 32 && /^\d+\.\d+\.\d+$/.test(value.version) ? value.version : undefined;
        const capabilities = validCapabilities(value.capabilities);
        if (capabilities && host.signinNegotiated && capabilities.signinServiceProtocol !== signinServiceProtocol) { host.signinNegotiated = false; this.signinWithdraw(host); }
        observeThumbnailPrivacy(host, capabilities);
        if (capabilities) { host.capabilities = capabilities; host.capabilitiesPending = !this.qemuConsole.capabilities(host); host.capabilitiesAt = host.capabilitiesPending ? 0 : Date.now(); this.sessionStop.capabilities(host); this.qemuConsole.resume(host); this.hostEvent(identity.desktop_id, 'ready'); }
        const resources = validHostResources(value.resources); // host-resources-v1, optional
        if (resources) host.resources = { ...resources, at: Date.now() };
        seen(version); this.send(ws, { type: 'host.heartbeat.ack' }); return;
      }
      if (value.type === 'host.capabilities') {
        const capabilities = validCapabilities(value.capabilities);
        observeThumbnailPrivacy(host, capabilities);
        if (capabilities) { host.capabilities = capabilities; host.capabilitiesPending = !this.qemuConsole.capabilities(host); host.capabilitiesAt = host.capabilitiesPending ? 0 : Date.now(); this.sessionStop.capabilities(host); this.qemuConsole.resume(host); this.hostEvent(identity.desktop_id, 'ready'); }
        // host-route-v1: released Hosts read only the fields they know.
        const wasSigninNegotiated = host.signinNegotiated === true;
        host.signinNegotiated = this.signinEnabled && capabilities?.signinServiceProtocol === signinServiceProtocol;
        if (wasSigninNegotiated && !host.signinNegotiated) this.signinWithdraw(host);
        this.send(ws, { type: 'host.capabilities.ack', accepted: !!capabilities, ...(host.signinNegotiated ? { signinServiceProtocol } : {}), ...(capabilities?.diagnosticsSupported === true ? { diagnosticsProtocol } : {}), ...(capabilities ? { closingProtocol: hostClosingProtocol, hostResumeProtocol, resourcesProtocol: hostResourcesProtocol } : {}), ...(capabilities && this.hostRoute ? { hostRoute: this.hostRoute } : {}), ...(capabilities && this.store.desktopName?.(host.identity.desktop_id) ? { desktopName: this.store.desktopName(host.identity.desktop_id) } : {}) }); return;
      }
      if (typeof value.type === 'string' && (value.type.startsWith('host.signin.') || value.type.startsWith('stream.phase.'))) {
        if (!this.receiveSignin(host, value)) this.dropHost(host, 'invalid_control');
        return;
      }
      // host-closing-v1: the Host closes next; its sessions end with its reason.
      if (value.type === 'host.closing') return this.dropHost(host, 'host_disconnected', hostClosingDetails[value.reason] ?? 'host_closing');
      if (value.type === 'host.diagnostics') { this.send(ws, { type: 'host.diagnostics.ack', accepted: this.receiveDiagnostics(host, value) }); return; }
      if (value.type === 'host.session.stopped') { this.sessionStop.result(host,value); return; }
      if (value.type === 'host.display.result') { this.displayResult(host, value); return; }
      if (value.type === 'host.console.result') { this.consolePreparation.result(host, value); return; }
      if (value.type === 'host.qemu-console.result') { this.qemuConsole.result(host, value); return; }
      if (value.type === 'host.qemu-console.held') { this.qemuConsole.held(host, value); return; }
      // ime-state-v1: only a Host that declared it answers, with exactly its fields.
      if (value.type === 'host.ime.state') { if (!this.ime.result(host, value)) this.dropHost(host, 'invalid_control'); return; }
      if (value.type === 'host.thumbnail.result') { this.previews?.result(host, value); return; }
      // Host program update (host-update-v1); see host-update.mjs.
      if (value.type === 'host.update.hello' || value.type === 'host.update.status') { this.hostUpdate?.message(host, value); return; }
      // PC power (pc-power-v1); see pc-power.mjs.
      if (value.type === 'host.power.hello' || value.type === 'host.power.result') { this.pcPower?.message(host, value); return; }
      // host-resume-v1: the sessions a returning Host kept (session-resume.mjs).
      if (value.type === 'host.session.resume') { this.resume.hostResume(host, value); return; }
      if (!session) return;
      if (value.type === 'udp.local') {
        if (udpPeerDirectAllowed(this.udp) && !session.udpTracks && session.udpDirect && session.udpGranted && value.sessionId === session.id) this.udp.local(session.id, 'host', value);
        return;
      }
      if (value.type === 'udp.key') {
        const key = !session.udpTracks && session.udpGranted && udpMediaAllowed(this.udp, session.desktopId) && value.sessionId === session.id ? udpKeyMessage(value) : null;
        if (key) this.send(session.ws, key);
        return;
      }
      if (value.type === 'stream.unavailable' || value.type === 'session.unavailable') {
        if (value.sessionId !== session.id) return; // A retired worker cannot end its successor.
        if (!allowedStreamFailure(session, value.reason) || value.type !== 'stream.unavailable') return this.dropHost(host, 'invalid_control');
        this.unavailable(session, value.reason); return;
      }
      // input-latency-v1 reports of the negotiated session only (input-latency.mjs).
      if (value.type === 'input.ack' || value.type === 'input.frame') {
        const early=holdEarlyDirectInputReport(session,value);
        if(early===null)return this.dropHost(host,'invalid_control');
        if(early===true)return;
        const report = inputLatencyHostMessage(session, value);
        if (report === null) return this.dropHost(host, 'invalid_control');
        if (report && !this.send(session.ws, report)) this.stop(session, 'client_backpressure');
        return;
      }
      // display-scale-v1 result of the negotiated session only; one of a
      // retired session is ignored.
      if (value.type === 'display.scale.applied') {
        const result = displayScaleResult(value);
        if (!result) return this.dropHost(host, 'invalid_control');
        if (result.sessionId !== session.id) return;
        // A track session's result comes from its worker on the track tunnel, per output.
        if (!session.displayScaleControl || session.trackState) return this.dropHost(host, 'invalid_control');
        if (session.ws && !this.send(session.ws, result)) this.stop(session, 'client_backpressure');
        return;
      }
      // caret-v1 report of the negotiated session only; one of a retired
      // session is ignored.
      if (value.type === 'caret') {
        const report = caretReport(value);
        if (!report) return this.dropHost(host, 'invalid_control');
        if (report.sessionId !== session.id) return;
        if (!session.caret) return this.dropHost(host, 'invalid_control');
        // A Host that floods reports is not dropped for it; the excess is simply not relayed.
        if (!caretAllowed(session.caretRate ??= { at: 0, count: 0 })) return;
        if (session.ws && !this.send(session.ws, report)) this.stop(session, 'client_backpressure');
        return;
      }
      if (session.trackState && typeof value.type==='string' && value.type.startsWith('stream.')) return;
      if (value.type === 'stream.info') {
        if (session.signinPolicy) return this.signinInvalidate(session, 'invalid_signin_info');
        if (session.mode !== 'stream') return;
        if (!validStreamInfo(value, session.streamSizeVersion ?? !!session.streamSize, session.audioOpus)) return this.dropHost(host, 'invalid_stream_info');
        if (session.resize) {
          const resize = session.resize;
          if (resize.initial && resize.deadline <= Date.now()) return this.resizeFailed(session, resize, 'resize_timeout');
          if (value.sessionId !== session.id || value.resizeId !== resize.resizeId || value.requestId !== resize.requestId || value.epoch !== resize.epoch) return;
          const nextOptions=resize.decoderLimits ? {...session.options,maxWidth:resize.decoderLimits.maxWidth,maxHeight:resize.decoderLimits.maxHeight} : session.options;
          if (resize.initial ? !initialViewportInfoMatches(value,session.initialViewport,session.options) : (session.viewportEncoderLimits ? !viewportInfoWithinLimits(value,resize,nextOptions) : (value.width !== resize.width || value.height !== resize.height || value.sourceWidth !== resize.width || value.sourceHeight !== resize.height || value.fpsTarget !== resize.fps))) return this.resizeFailed(session, resize, 'invalid_resize_info');
          // stream-size-v1: never above what the client's decoder declared.
          if (!withinDecoderLimits(value, resize.decoderLimits ?? session.streamSize, resize.fps)) return this.resizeFailed(session, resize, 'invalid_resize_info');
          if(resize.decoderLimits){session.streamSize=resize.decoderLimits;session.options=nextOptions;}
          session.epoch = resize.epoch; session.resize = null; session.awaitingKeyFrame = true;
          session.source = { width: value.sourceWidth, height: value.sourceHeight };
          if (resize.initial) session.initialFrameDeadline = Date.now() + this.resizeReadyMs;
          else session.options = session.viewportEncoderLimits ? { ...session.options, fps: resize.fps } : { ...session.options, maxWidth: resize.width, maxHeight: resize.height, fps: resize.fps };
        } else if (session.streamSize && session.streamReady && value.epoch === session.epoch && value.sessionId === session.id) {
          // stream-size-v1: the Host restarted its encoder at another scale of
          // the same source within this epoch (its next video is an IDR).
          if (!rescaledInfoValid(value, session)) return this.dropHost(host, 'invalid_stream_info');
        } else if ((value.epoch ?? 0) !== session.epoch || (value.sessionId !== undefined && value.sessionId !== session.id)) return;
        const info = session.epochMedia ? { ...value, epoch: session.epoch } : value;
        cursorSource(session, value);
        this.resume.info(session, info);
        session.streamReady = true; this.send(session.ws, info); this.grantUdpMedia(session); return;
      }
      if (value.type === 'stream.resize.failed') {
        const resize = session.resize;
        if (!resize || value.sessionId !== session.id || value.resizeId !== resize.resizeId || value.epoch !== resize.epoch || value.requestId !== resize.requestId) return;
        const reasons = ['unsupported_display_mode', 'display_change_failed', 'virtual_display_required', 'monitor_not_available', 'resize_failed'];
        this.resizeFailed(session, resize, reasons.includes(value.reason) ? value.reason : 'resize_failed'); return;
      }
      if (value.type === 'stream.stats' && ['capturedFps', 'encodedFps', 'bitrateKbps'].every(key => Number.isFinite(value[key]) && value[key] >= 0 && value[key] <= (key === 'bitrateKbps' ? adaptiveMaxBitrate / 100 : 100000))) {
        // input-latency-v1 sessions: the relay's own share of recent tagged packets.
        const forward = session.inputLatency ? inputLatencyForwardStats(session.inputLatency) : null;
        if (session.mode === 'stream') {
          const stats = observabilityStats(session, value);
          this.send(session.ws, forward ? { ...stats, relayForward: forward } : stats);
        }
        return;
      }
      if (value.type === 'pong' && typeof value.id === 'string' && value.id.length <= 64) { this.send(session.ws, value); return; }
      this.dropHost(host, 'invalid_control');
    });
  }
  // `resumed`: session-resume-v1, the same client's new socket (session-resume.mjs).
  attachClient(ws, session, resumed = false) {
    if (!(this.current(session) || (resumed && session.hostHeld && this.sessions.get(session.id) === session)) || !this.authorized(session)) { ws.close(1008, 'authorization_revoked'); return; }
    if (resumed && !this.resume.replace(session)) { ws.close(1008, 'resume_refused'); return; }
    session.ws = ws; session.alive = true;
    // 모니터링 (usage.mjs): the connection starts here; a resumed socket continues it.
    this.usage?.attached(session);
    let windowStart = Date.now(), count = 0, bytes = 0;
    // Only the session's current socket speaks for it: a replaced one is gone.
    ws.on('pong', () => { if (session.ws === ws) session.alive = true; });
    ws.on('error', error => { if (session.ws === ws && !this.resume.clientError(session, ws, error)) this.stop(session, 'client_error'); });
    ws.on('close', code => { if (session.ws === ws && !this.resume.clientClosed(session, ws, code)) this.stop(session, 'client_disconnected'); });
    ws.on('message', (data, binary) => {
      // A close handshake does not cancel messages already buffered by ws.
      // Never let a retired controller inject input after reset/stop or into
      // a newer controller's stream on the same reusable host channel.
      if (session.ws !== ws || this.sessions.get(session.id) !== session) return;
      if (!this.current(session)) return this.resume.heldClientMessage(session, data, binary);
      if (!this.authorized(session)) return this.stop(session, 'authorization_revoked');
      if (Date.now() - windowStart >= 1000) { windowStart = Date.now(); count = 0; bytes = 0; }
      if (++count > 2000 || (bytes += data.length) > MAX_CLIENT_BYTES_PER_SECOND) return this.stop(session, 'invalid_input');
      if (binary || data.length > 8192) return this.stop(session, 'invalid_input');
      let value;
      try { value = JSON.parse(data.toString('utf8')); } catch { return this.stop(session, 'invalid_input'); }
      if (!value || typeof value !== 'object' || Array.isArray(value)) return this.stop(session, 'invalid_input');
      if (session.signinPolicy) { void this.signinClientControl(session, value).catch(() => this.signinInvalidate(session)); return; }
      let direct=null;
      if(value.type==='input.direct'){
        if(!udpPeerDirectAllowed(this.udp)||!session.udpDirect||(!session.udpTracks&&!session.udpGranted)||value.protocol!==udpDirectProtocol||!Number.isInteger(value.sequence)||value.sequence<1||value.sequence>0xffffffff||!Number.isInteger(value.epoch)||value.epoch<0||value.epoch>65535||Object.keys(value).sort().join(',')!=='epoch,input,protocol,sequence,type'||!value.input||typeof value.input!=='object'||Array.isArray(value.input)||!['input','input.reset','input.fence'].includes(value.input.type))return this.stop(session,'invalid_input');
        direct=value;value=direct.input;
        if(value.type==='input'&&!session.trackState&&direct.epoch!==session.epoch)return;
      }
      const forwardControl=control=>{
        if (control.type === 'input' && this.sessionInputAccepted?.(session) === false) { this.stop(session, 'authorization_revoked'); return true; }
        const result=this.send(session.trackState?session.trackTunnel:session.host.ws,direct?{type:'input.direct',protocol:udpDirectProtocol,sequence:direct.sequence,epoch:direct.epoch,sessionId:session.id,input:control}:control);
        if(result&&control.sample)for(const report of takeEarlyDirectInputReports(session,control.sample))if(!this.send(session.ws,report))this.stop(session,'client_backpressure');
        return result;
      };
      if(direct&&value.type==='input.fence'){
        if(Object.keys(value).length!==1)return this.stop(session,'invalid_input');
        // A fence releases/advances the shared input order without injecting a
        // pointer or key. Keep it ordered behind track resize/reset even while
        // old UDP legs are retired and fresh IDRs are pending.
        if(session.trackState ? session.trackHeld||!session.trackTunnel : session.profilePreparing||session.resize||!session.streamReady)return;
        if(!forwardControl(value))this.dropHost(session.host,'host_backpressure');return;
      }
      if (value.type === 'session.stop') return this.stop(session, 'client_stopped');
      if(value.type==='udp.tracks.feedback'){udpTrackFeedback(session,value);return;}
      if(value.type==='udp.tracks.local'){const local=udpTrackLocal(session,value);if(local&&session.udpDirect&&udpPeerDirectAllowed(this.udp))this.udp.local(session.id,'client',local);return;}
      if(value.type==='udp.tracks.key'){const key=udpTrackKey(session,value);if(key&&session.trackTunnel&&!session.trackHeld)this.send(session.trackTunnel,{...value,publicKey:key.publicKey});return;}
      if (session.profilePreparing) return; // no input/resize before profile authorization
      // display-scale-v1: the Windows scale of the streamed owned output (of
      // every owned output with display-scale-outputs-v1), only in a session
      // that negotiated it; the Host's control channel takes it in both kinds
      // (it keeps it across resizes and applies the latest one once a resize
      // has finished).
      if (value.type === 'display.scale') {
        if (!session.displayScaleControl || session.mode !== 'stream' || !validDisplayScaleRequest(value)) return this.stop(session, 'invalid_input');
        if (!this.send(session.host.ws, { type: 'display.scale', sessionId: session.id, scale: value.scale })) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      if (session.trackState) {
        // tracks-resume-v1: nothing reaches a Host whose track tunnel is away.
        if (session.trackHeld) return;
        if(value.type==='stream.tracks.resize')return this.resizeTracks(session,value);
        if(value.type==='stream.tracks.ready'){
          const result=session.trackState.acknowledge(value);
          if(result==='invalid')return this.stop(session,'invalid_track_ready');
          if(result==='accepted' && session.trackTunnel){this.send(session.trackTunnel,{...value,sessionId:session.id});this.maybeGrantUdpTracks(session);}
          return;
        }
        // cursor-compose-v1 (寃뚯엫 留덉슦??on independent outputs): like a single output's.
        if(value.type==='cursor.compose'){
          if(!session.cursorCompose||!validCursorCompose(value))return this.stop(session,'invalid_input');
          if(!this.send(session.trackTunnel??session.host.ws,{type:'cursor.compose',sessionId:session.id,on:value.on}))this.stop(session,'tracks_host_backpressure');return;
        }
        if(value.type==='input'){
          const result=session.trackState.input(value,validClientControl,session.relativeMouse?validRelativeTrackMouse:null);
          if(result==='invalid')return this.stop(session,'invalid_input');
          if(result!=='forward')return;
        }else if(!validClientControl(value) || !['ping','input.reset'].includes(value.type) ||
                 Object.keys(value).some(k=>!['type',...(value.type==='ping'?['id']:[])].includes(k)))return this.stop(session,'invalid_input');
        const target=session.trackTunnel??session.host.ws;
        if (!direct && value.type === 'input' && this.sessionInputAccepted?.(session) === false) return this.stop(session, 'authorization_revoked');
        if(!(direct?forwardControl(value):this.send(target,{...value,sessionId:session.id})))this.stop(session,'tracks_host_backpressure');return;
      }
      // Arrival feedback exists only in a session that negotiated the adaptive rate.
      if (value.type === 'stream.feedback') {
        if (!session.bitrateAdaptive || !validBitrateFeedback(value)) return this.stop(session, 'invalid_input');
        if (!this.send(session.host.ws, bitrateFeedbackForHost(session, value))) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      if (value.type === 'stream.resize') return this.resize(session, value);
      // stream-size-v1: the client's measured decoder load, only in a session
      // that negotiated it; at most about two per second reach the Host.
      if (value.type === 'stream.decoder') {
        if (!session.streamSize || session.mode !== 'stream' || !validDecoderReport(value)) return this.stop(session, 'invalid_input');
        const now = Date.now();
        if (now - (session.decoderReportAt ?? 0) < 400) return;
        session.decoderReportAt = now;
        if (!this.send(session.host.ws, { type: 'stream.decoder', sessionId: session.id, busy: value.busy, backlogMs: value.backlogMs })) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      if (value.type === 'udp.local') {
        if (udpPeerDirectAllowed(this.udp) && !session.udpTracks && session.udpDirect && session.udpGranted) this.udp.local(session.id, 'client', value);
        return;
      }
      // udp-media-v1: only with a grant; a refused key leaves the session as it is.
      if (value.type === 'udp.key') {
        const key = !session.udpTracks && session.udpGranted && udpMediaAllowed(this.udp, session.desktopId) ? udpKeyMessage(value) : null;
        if (key && !this.send(session.host.ws, { ...key, sessionId: session.id })) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      // caret-v1: start/stop of the Host's caret reports, only in a session
      // that negotiated it.
      if (value.type === 'caret.watch') {
        if (!session.caret || session.mode !== 'stream' || !validCaretWatch(value)) return this.stop(session, 'invalid_input');
        if (!this.send(session.host.ws, { type: 'caret.watch', sessionId: session.id, on: value.on })) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      // cursor-echo-v1: start/stop of the Host's cursor position echo (the
      // client holds the mouse), only in a session that negotiated it.
      if (value.type === 'cursor.echo') {
        if (!session.cursorEcho || session.mode !== 'stream' || !validCursorEcho(value)) return this.stop(session, 'invalid_input');
        if (!this.send(session.host.ws, { type: 'cursor.echo', sessionId: session.id, on: value.on })) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      // cursor-compose-v1: the Host draws the cursor into the video while the
      // client holds the mouse; likewise only in a session that negotiated it.
      if (value.type === 'cursor.compose') {
        if (!session.cursorCompose || session.mode !== 'stream' || !validCursorCompose(value)) return this.stop(session, 'invalid_input');
        if (!this.send(session.host.ws, { type: 'cursor.compose', sessionId: session.id, on: value.on })) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      // text-input-v1: committed text only in a session that negotiated it,
      // exact fields and bounds, never sampled; denied while the output
      // changes like any other injection.
      if (value.type === 'input' && value.event === 'text') {
        if (!session.textInput || session.mode !== 'stream' || !validTextInput(value)) return this.stop(session, 'invalid_input');
        if (session.resize || (session.initialViewport && (!session.streamReady || session.awaitingKeyFrame))) return;
        if (!forwardControl(value)) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      // touch-input-v1: one touch frame (1-10 contacts) only in a session that
      // negotiated it, exact fields and bounds, never sampled; denied while the
      // output changes like any other injection (the Host has released every
      // contact on the input.reset that starts a change).
      if (value.type === 'input' && value.event === 'touch') {
        if (!session.touchInput || session.mode !== 'stream' || !validTouchInput(value)) return this.stop(session, 'invalid_input');
        if (session.resize || (session.initialViewport && (!session.streamReady || session.awaitingKeyFrame))) return;
        if (!forwardControl(value)) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      // relative-mouse-v1: movement, and the buttons and wheel of a captured
      // mouse at the current cursor, only in a session that negotiated it, exact
      // fields and bounds; `sample` as on `move`; denied while the output changes
      // like any other injection.
      if (relativeMouseInput(value)) {
        if (!session.relativeMouse || session.mode !== 'stream' || !validRelativeMouse(value)) return this.stop(session, 'invalid_input');
        const forwarded = admitInputSample(session, value);
        if (!forwarded) return this.stop(session, 'invalid_input');
        if (session.resize || (session.initialViewport && (!session.streamReady || session.awaitingKeyFrame))) return;
        if (!forwardControl(forwarded)) this.dropHost(session.host, 'host_backpressure');
        return;
      }
      if (!validClientControl(value)) return this.stop(session, 'invalid_input');
      // input-latency-v1: `sample` exists only on input of a negotiated session.
      const forwarded = admitInputSample(session, value);
      if (!forwarded) return this.stop(session, 'invalid_input');
      // A changing monitor has no stable input coordinate space. Keep reset,
      // heartbeat and cancellation available while denying pointer/key injection.
      if ((session.resize || (session.initialViewport && (!session.streamReady || session.awaitingKeyFrame))) && value.type === 'input') return;
      if (!forwardControl(forwarded)) this.dropHost(session.host, 'host_backpressure');
    });
    if (session.resume) this.resume.attached(session, resumed);
    if (resumed) return;
    if (session.signinPolicy) {
      if (!this.admitSessionStart(session)) return;
      const signinAdmission = this.signinStartAdmission(session);
      if (!signinAdmission) { this.stop(session, 'signin_authority_lost'); return; }
      this.signinCloseRedirects(session); this.previews.forget(session.desktopId);
      session.hostDispatched = this.send(session.host.ws, { type: 'session.start', sessionId: session.id, mode: 'stream', signin: session.signinPolicy.binding, signinAdmission, width: session.options.maxWidth, height: session.options.maxHeight, fps: session.options.fps, ...startBitrateFields(session.options, false), audio: false, monitor: '0', ...(session.initialViewport ? { initialViewportProtocol, initialViewport: session.initialViewport } : {}) });
      if (!session.hostDispatched) this.dropHost(session.host, 'host_backpressure');
      return;
    }
    if(session.trackState){
      const { bitrateMode: _trackRate, ...trackOptions } = session.options; // tracks-v1 stays fixed-rate
      void this.dispatchSessionStart(session,{type:'session.start',sessionId:session.id,mode:'stream',...trackOptions,...(session.audioOpus?{audioCodec:audioOpusCodec}:{}),...(session.resume?{trackResumeProtocol}:{}),...(session.channelResume?{channelResumeProtocol}:{}),...(session.observability?{observabilityProtocol}:{}),...(session.cursorTracks?{cursorProtocol,cursorTracksProtocol,...(session.cursorCompose?{cursorComposeProtocol}:{})}:{}),...(session.relativeMouse?{relativeMouseProtocol,relativeMouseTracksProtocol}:{}),...(session.displayScaleOutputs?{displayScaleProtocol,displayScaleOutputsProtocol,...(session.displayScale!==null?{displayScale:session.displayScale}:{})}:{}),
        ...session.trackState.start(),...(session.udpTracks?{udpTracksProtocol}:{}),...(session.streamSize?{streamSizeProtocol:session.streamSizeVersion,decoderLimits:session.streamSize}:{}),tunnelUrl:brokerWebSocketOrigin(session.host.origin ?? this.publicOrigin)+'/api/v1/hosts/sessions/'+session.id+'/tracks',
        clipboardPolicy:session.clipboardPolicy,...(session.clipboardProtocol?{clipboardProtocol:session.clipboardProtocol}:{}),
        ...(session.clipboardEnabled?{clipboardUrl:brokerWebSocketOrigin(session.host.origin ?? this.publicOrigin)+'/api/v1/hosts/sessions/'+session.id+'/clipboard'}:{}),
        ...this.printStartFields(session),...this.driveStartFields(session),...this.usbStartFields(session)});
      return;
    }
    if(session.initialViewport){
      if(!initialViewportSessionSupported(session,Date.now(),this.heartbeatMs))return this.unavailable(session,'virtual_display_required');
      session.resize={...session.initialViewport,initial:true,deadline:Date.now()+initialViewportPrepareMs};session.nextEpoch=1;
    }
    void this.dispatchSessionStart(session, { type: 'session.start', sessionId: session.id, mode: 'stream', ...(session.observability ? { observabilityProtocol } : {}), width: session.options.maxWidth, height: session.options.maxHeight, fps: session.options.fps, ...startBitrateFields(session.options, session.bitrateAdaptive), audio: session.options.audio, ...(session.audioOpus ? { audioCodec: audioOpusCodec } : {}), monitor: session.options.monitor, clipboardPolicy: session.clipboardPolicy, ...(session.inputLatency ? { inputLatencyProtocol, inputFrameTimingProtocol } : {}), ...(session.cursor ? { cursorProtocol, ...(session.cursorEcho ? { cursorEchoProtocol } : {}), ...(session.cursorCompose ? { cursorComposeProtocol } : {}) } : {}), ...(session.textInput ? { textInputProtocol } : {}), ...(session.displayScaleControl ? { displayScaleProtocol, ...(session.displayScale !== null ? { displayScale: session.displayScale } : {}) } : {}), ...(session.touchInput ? { touchInputProtocol } : {}), ...(session.relativeMouse ? { relativeMouseProtocol } : {}), ...(session.caret ? { caretProtocol } : {}), ...this.usbStartFields(session), ...(session.channelResume ? { channelResumeProtocol } : {}), ...(session.streamSize ? { streamSizeProtocol:session.streamSizeVersion, decoderLimits: session.streamSize } : {}), ...(session.viewportEncoderLimits ? {viewportEncoderLimitsProtocol} : {}), ...(session.initialViewport ? {initialViewportProtocol,initialViewport:session.initialViewport} : {}), ...(session.clipboardProtocol ? { clipboardProtocol: session.clipboardProtocol } : {}), ...(session.epochMedia ? { resizeProtocol: 'epoch-v1', epoch: 0 } : {}), ...(session.clipboardEnabled ? { clipboardUrl: brokerWebSocketOrigin(session.host.origin ?? this.publicOrigin) + '/api/v1/hosts/sessions/' + session.id + '/clipboard' } : {}) , ...this.printStartFields(session), ...this.driveStartFields(session) });
  }
  // udp-media-v1: once per session, each leg's own ticket and relay list over
  // its own channel. A client never gets the Host's list: it would send proofs
  // to whatever owns those private addresses on its own network. Reconnect and
  // takeover are new sessions, so stop() revoked the old grant and this issues
  // a new one. No ticket means the session stays on WebSocket.
  // Each leg's relay list (udp-relay.mjs relaysFor). Null when one leg has no
  // endpoint: nothing is granted and the session stays on WebSocket.
  udpRelays(session) {
    const client = this.udp.relaysFor('client', session.clientOrigin ?? this.publicOrigin), host = this.udp.relaysFor('host', session.host.origin ?? this.publicOrigin);
    return client.length && host.length ? { client, host } : null;
  }
  grantUdpMedia(session) {
    if(session.udpTracks){this.maybeGrantUdpTracks(session);return;}
    if (!session.udpMedia || session.udpGranted) return;
    session.udpGranted = true;
    const relays = this.udpRelays(session);
    if (!relays) return;
    const allowPeerDirect = udpPeerDirectAllowed(this.udp);
    const grant = this.udp.issue(session.id, { direct: allowPeerDirect && session.udpDirect && udpMediaAllowed(this.udp, session.desktopId) });
    if (!grant) return;
    const keys = udpMediaAllowed(this.udp, session.desktopId);
    this.send(session.ws, udpGrantMessage(grant, relays.client, grant.client, keys, session.udpDirect, this.udp.stun, allowPeerDirect));
    this.send(session.host.ws, udpGrantMessage(grant, relays.host, grant.host, keys, session.udpDirect, this.udp.stun, allowPeerDirect));
  }
  retireUdpTracks(session) {
    if (!session.udpTracks) return;
    this.udp?.revoke(session.id);
    session.udpGranted = false;
    session.udpTrackSignature = null;
    session.udpTrackScopes = null;
    session.udpTrackFeedback?.clear();
  }
  // Each actual output gets its own keys/socket pair. All pairs belong to the
  // authenticated parent session and share its bandwidth/revocation boundary.
  // A declaration or decoder ACK alone cannot grant them: both real WSS IDRs
  // must already have passed the existing TrackStream readiness gates.
  maybeGrantUdpTracks(session) {
    if (!session.udpTracks || !this.current(session) || !this.authorized(session) ||
        !session.ws || !session.trackTunnel || session.trackHeld || session.hostHeld ||
        !session.trackState?.ready || !this.udp?.available || !udpMediaAllowed(this.udp,session.desktopId)) return;
    const info = session.trackState.info, signature = udpTrackSignature(info);
    if (!signature || (session.udpGranted && session.udpTrackSignature === signature && this.udp.grants.has(session.id))) return;
    const generation = (session.udpTrackGeneration ?? 0) + 1;
    if (generation > 0xffffffff) { this.retireUdpTracks(session); return; }
    const scopes = info.tracks.map(row => ({track:row.id,topologyEpoch:info.topologyEpoch,epoch:row.epoch,generation}));
    const relays = this.udpRelays(session);
    if (!relays) return;
    const allowPeerDirect = udpPeerDirectAllowed(this.udp);
    const grant = this.udp.issue(session.id,{direct:allowPeerDirect&&session.udpDirect,tracks:scopes});
    if (!grant) return;
    session.udpTrackGeneration = generation;
    session.udpTrackSignature = signature;
    session.udpTrackScopes = new Map(scopes.map(row => [row.track,row]));
    session.udpTrackFeedback = new Map();
    session.udpGranted = true;
    const message = role => ({type:'udp.tracks.grant',protocol:udpTracksProtocol,sessionId:session.id,
      topologyEpoch:info.topologyEpoch,generation,tracks:grant.tracks.map(row => {
        const output = info.tracks.find(value => value.id === row.track);
        return {track:row.track,epoch:row.epoch,primary:output.primary,bitrateTarget:output.bitrateTarget,
          grant:udpGrantMessage(grant,relays[role],row[role],true,session.udpDirect,this.udp.stun,allowPeerDirect)};
      })});
    // Host-first ordering is on the same tunnel as reliable input and leases.
    // A root Host channel would race the track worker's initialization.
    if (!this.send(session.trackTunnel,message('host')) || !this.send(session.ws,message('client'))) {
      this.retireUdpTracks(session); return;
    }
    session.udpTrackLeaseAt = 0;
  }
  tickUdpTracks(session) {
    if (!session.udpTracks) return;
    if (!this.current(session) || !this.authorized(session) || !session.ws || !session.trackTunnel ||
        session.trackHeld || session.hostHeld || !session.trackState?.ready ||
        !this.udp?.available || !udpMediaAllowed(this.udp,session.desktopId)) {
      if (session.udpGranted) this.retireUdpTracks(session);
      return;
    }
    this.maybeGrantUdpTracks(session);
    const now = Date.now();
    if (session.udpGranted && now - (session.udpTrackLeaseAt ?? 0) >= 1000) {
      session.udpTrackLeaseAt = now;
      if (!this.send(session.trackTunnel,{type:'udp.tracks.lease',protocol:udpTracksProtocol,
          sessionId:session.id,topologyEpoch:session.trackState.info.topologyEpoch,generation:session.udpTrackGeneration})) this.retireUdpTracks(session);
    }
  }
  // usb-v1 in session.start: the marker and the Host's own channel URL.
  usbStartFields(session) {
    if (!session.usb) return {};
    // usbGrant: the class permissions that let a device of that class in as well
    // (the Host re-checks every device's descriptors against them).
    const policy = this.store.devicePolicy(session.userId, session.desktopId);
    return { usbProtocol, usbUrl: brokerWebSocketOrigin(session.host.origin ?? this.publicOrigin) + '/api/v1/hosts/sessions/' + session.id + '/usb', usbGrant: { drives: policy.drives === true, printers: policy.printers === true, smartCards: policy.smartCards === true, serialPorts: policy.serialPorts === true } };
  }
  // print-v1 in session.start: the marker and the Host's own channel URL.
  printStartFields(session) {
    return session.print ? { printProtocol, printUrl: brokerWebSocketOrigin(session.host.origin ?? this.publicOrigin) + '/api/v1/hosts/sessions/' + session.id + '/print' } : {};
  }
  admitSessionStart(session) {
    // A ticket is a reservation, not a durable readiness grant. Revalidate
    // the exact controller and current Host before every start dispatch.
    if (this.sessions.get(session.id) !== session) return false;
    if (!this.current(session)) { this.stop(session, 'host_disconnected'); return false; }
    if (!this.authorized(session)) { this.stop(session, 'authorization_revoked'); return false; }
    try { this.assertDisplayIdle(session.desktopId, session.mode); }
    catch { this.unavailable(session, 'host_maintenance'); return false; }
    if (session.signinPolicy) return this.signinEnabled && session.host.signinNegotiated && session.host.signinServiceInstance === session.signinServiceInstance;
    if (!this.modes(session.host)[session.mode]) {
      const reason = this.unavailableReason(session.host, session.mode);
      const finalReason = allowedStreamFailure(session, reason) ? reason : 'host_maintenance';
      this.unavailable(session, finalReason);
      return false;
    }
    return true;
  }
  abortSessionStart(session) {
    // A failed authority read cannot authorize another lookup or affect a
    // successor. Keep this final rejection handler synchronous and nonthrowing.
    try {
      if (this.sessions.get(session.id) !== session) return;
      const client = session.ws;
      const tunnels = [session.trackTunnel], hostWire = session.host.ws;
      try { this.stop(session, 'host_error', undefined, false); }
      catch {
        // Only an unstarted exact ticket has no Host completion obligation.
        // Never clear a dispatched owner or any existing stop-v1 barrier here.
        if (!session.hostDispatched && this.sessions.get(session.id) === session) {
          this.sessions.delete(session.id); this.activityAt = Date.now();
        }
        for (const channel of [this.clipboard, this.print, this.drive, this.usb]) {
          try { channel.closeSession(session, 'session_ended'); } catch {}
        }
        for (const wire of new Set(tunnels)) {
          if (!wire || wire === hostWire || wire === client) continue;
          try { wire.close(1011, 'host_error'); } catch {}
          try { if (wire.readyState !== WebSocket.CLOSED) wire.terminate(); } catch {}
        }
      }
      try { if (client?.readyState === WebSocket.OPEN) client.close(1011, 'host_error'); }
      catch { try { client?.terminate(); } catch {} }
    } catch { /* Preserve uncertainty; never reject from the last boundary. */ }
  }
  // Administrator records of the console proof (활동 기록): desktopId:code, a
  // fixed code only (stream-console-profile.mjs), never Proxmox text.
  auditConsoleProof(session, event, code) {
    try { this.store.audit(event === 'stream_console_unverified' ? 'stream_console_unverified' : 'stream_console_identity_refused', session.userId, session.desktopId + ':' + code); }
    catch { /* Optional audit cannot stop or hold a stream. */ }
  }
  async dispatchSessionStart(session, command) {
    try { await this.dispatchSessionStartGuarded(session, command); }
    catch { this.abortSessionStart(session); }
  }
  async dispatchSessionStartGuarded(session, command) {
    if (!this.admitSessionStart(session)) return;
    if(session.initialViewport && !initialViewportSessionSupported(session,Date.now(),this.heartbeatMs)){
      if(this.current(session))this.unavailable(session,'virtual_display_required');return;
    }
    if (!session.localPc && session.trackState && session.host.capabilities?.streamProfileProtocol === streamConsoleProfileProtocol && !session.profileTracks) {
      if (this.current(session)) this.unavailable(session, 'track_streaming_unavailable');
      return;
    }
    if(session.profileTracks&&!profileTracksSupported(session.host,Date.now(),this.heartbeatMs)){
      if(this.current(session))this.unavailable(session,'track_streaming_unavailable');return;
    }
    if (session.localPc || session.mode !== 'stream' || session.host.capabilities?.streamProfileProtocol !== streamConsoleProfileProtocol) {
      if (this.current(session) && this.authorized(session)) session.hostDispatched=this.send(session.host.ws, command);
      return;
    }
    if (session.profilePreparing || session.profileDispatched) return;
    session.profilePreparing = true; session.profileDeadline = Date.now() + 20000;
    try {
      let proof, missing;
      try { ({ proof, missing } = await streamConsoleProof(this, session)); }
      catch (error) {
        if (!(error instanceof HttpError)) throw error;
        // Only "not this VM / not this session" ends here, with its own reason.
        if (this.admitSessionStart(session)) {
          const reason = consoleProofRefusal(this, session, error.code);
          if (reason !== 'host_maintenance' && reason !== 'power_in_progress') this.auditConsoleProof(session, 'stream_console_identity_refused', reason);
          this.unavailable(session, reason);
        }
        return;
      }
      if (!this.admitSessionStart(session)) return;
      if (Date.now() >= session.profileDeadline) {
        if (this.current(session)) this.unavailable(session, 'virtual_display_required');
        return;
      }
      if(session.initialViewport && !initialViewportSessionSupported(session,Date.now(),this.heartbeatMs)){
        this.unavailable(session,'virtual_display_required');return;
      }
      if(session.profileTracks&&!profileTracksSupported(session.host,Date.now(),this.heartbeatMs)){
        this.unavailable(session,'track_streaming_unavailable');return;
      }
      session.profileDispatched = true; session.profilePreparing = false;
      // Without a proof the Host gets the start it takes from any VM: no
      // consoleProfile, and for all monitors no group marker (its plain v2
      // tracks start). Every display it does not own is off for the stream.
      if (!proof) this.auditConsoleProof(session, 'stream_console_unverified', missing);
      session.hostDispatched=this.send(session.host.ws, proof ? { ...command, consoleProfile: proof, ...(session.profileTracks?{streamProfileTracksProtocol:profileTracksProtocol}:{}) } : command);
      if (!session.hostDispatched) this.dropHost(session.host, 'host_backpressure');
    } finally { session.profilePreparing = false; }
  }
  resize(session, input) {
    if (session.mode !== 'stream' || !validResize(input, session.streamSizeVersion ?? !!session.streamSize)) return this.stop(session, 'invalid_input');
    const reject = reason => this.send(session.ws, { type: 'stream.resize.failed', requestId: input.requestId, reason, unchanged: true });
    const profile = !session.localPc && (session.profileDispatched || session.profilePreparing || session.host.capabilities?.streamProfileProtocol === streamConsoleProfileProtocol);
    if (profile && (!session.profileViewport || !session.profileDispatched || session.profilePreparing || !profileViewportSupported(session.host,Date.now(),this.heartbeatMs))) return reject('dynamic_resolution_unavailable');
    if (session.viewportEncoderLimits && session.host.capabilities?.viewportEncoderLimitsProtocol !== viewportEncoderLimitsProtocol) return reject('dynamic_resolution_unavailable');
    if (session.initialViewport && (!session.streamReady || session.awaitingKeyFrame)) return reject('resize_in_progress');
    if (!session.epochMedia || session.host.capabilities?.dynamicResolution !== true || session.host.capabilities.resizeProtocol !== 'epoch-v1') return reject('dynamic_resolution_unavailable');
    if (input.monitor === 'all' || input.monitor !== session.options.monitor) return reject('monitor_not_available');
    const monitor = session.host.capabilities.monitors?.find(value => value.id === input.monitor);
    const provisioning = session.host.capabilities.viewportProvisioning === true;
    if (!monitor || (!provisioning && !monitor.modes?.some(mode => mode.width === input.width && mode.height === input.height && mode.refreshRate >= input.fps))) return reject('unsupported_display_mode');
    // Provisioning can involve a real device restart; never supersede an
    // already dispatched hardware transaction with a second change.
    if (provisioning && session.resize) return reject('resize_in_progress');
    const now = Date.now();
    if (!session.resizeWindow || now - session.resizeWindow >= 1000) { session.resizeWindow = now; session.resizeCount = 0; }
    if (++session.resizeCount > 2) return reject('resize_rate_limited');
    if (session.nextEpoch >= 65535) return reject('resize_limit_reached');
    const resize = { requestId: input.requestId, resizeId: randomUUID(), epoch: ++session.nextEpoch, width: input.width, height: input.height, fps: input.fps, monitor: input.monitor };
    const timeoutMs = provisioning ? this.viewportReadyMs : this.resizeReadyMs;
    session.resize = { ...resize, ...(input.decoderLimits ? {decoderLimits:{...input.decoderLimits}} : {}), deadline: now + timeoutMs }; session.streamReady = false;
    this.send(session.host.ws, { type: 'input.reset' });
    this.send(session.ws, { type: 'stream.resizing', ...resize, timeoutMs });
    if (!this.send(session.host.ws, { type: 'stream.resize', sessionId: session.id, ...resize, ...(input.decoderLimits ? {decoderLimits:{...input.decoderLimits}} : {}) })) this.dropHost(session.host, 'host_backpressure');
  }
  resizeFailed(session, resize, reason) {
    if (!this.current(session) || session.resize?.resizeId !== resize.resizeId) return;
    this.send(session.ws, { type: 'stream.resize.failed', requestId: resize.requestId, resizeId: resize.resizeId, epoch: resize.epoch, reason, unchanged: false });
    this.stop(session, 'resize_failed');
  }
  resizeTracks(session,value){
    const state=session.trackState;
    const reject=reason=>this.send(session.ws,{type:'stream.tracks.resize.failed',requestId:typeof value.requestId==='string'?value.requestId.slice(0,64):'',reason,unchanged:true});
    if(!session.trackTunnel || !tracksSupported(session.host,Date.now(),this.heartbeatMs))return reject('track_streaming_unavailable');
    if(session.profileTracks&&(!session.profileDispatched||session.profilePreparing||!profileTracksSupported(session.host,Date.now(),this.heartbeatMs)))return reject('track_streaming_unavailable');
    if(Date.now()-(session.lastTrackResizeAt??0)<500)return reject('resize_rate_limited');
    const result=state.resize(value);if(result.reason)return reject(result.reason);
    if(session.udpTracks)this.retireUdpTracks(session);
    session.lastTrackResizeAt=Date.now();
    this.send(session.trackTunnel,{type:'input.reset',sessionId:session.id});
    const {decoderLimits:_limits,...clientResult}=result;
    this.send(session.ws,{type:'stream.tracks.resizing',...clientResult});
    if(!this.send(session.trackTunnel,{type:'stream.tracks.resize',sessionId:session.id,...result}))this.stop(session,'tracks_host_backpressure');
  }
  // `resumed`: tracks-resume-v1, the Host's next tunnel of this session (session-resume.mjs).
  attachTrackTunnel(ws,session,resumed=false){
    const held=resumed?this.resume.trackReturned(session,ws):null;
    if(resumed&&!held)return;
    session.trackTunnel=ws;session.trackAlive=true;
    let windowStart=Date.now(),count=0,bytes=0;
    // Only the session's current tunnel speaks for it: a replaced one is gone.
    ws.on('pong',()=>{if(session.trackTunnel===ws)session.trackAlive=true;});
    ws.on('error',error=>{if(session.trackTunnel!==ws)return;const detail=hostErrorDetail(error);if(!this.resume.trackLost(session,'tracks_host_error',detail,detail==='host_transport_error'))this.stop(session,'tracks_host_error',detail);});
    ws.on('close',code=>{if(session.trackTunnel===ws&&!this.resume.trackLost(session,'tracks_host_disconnected',undefined,code===1006))this.stop(session,'tracks_host_disconnected');});
    ws.on('message',(data,binary)=>{
      if(!this.current(session) || session.trackTunnel!==ws)return;
      session.trackTraffic=true;
      if(!this.authorized(session))return this.stop(session,'authorization_revoked');
      const identity=this.store.authenticateHost(session.host.tokenHash);
      if(!identity || identity.id!==session.host.identity.id)return this.stop(session,'host_revoked');
      if(Date.now()-windowStart>=1000){windowStart=Date.now();count=0;bytes=0;}
      if(++count>1000 || (bytes+=data.length)>32*1024*1024)return this.stop(session,'tracks_host_rate_limit');
      const state=session.trackState;
      if(binary){
        // cursor-tracks-v1 records (cursor.mjs): kind 3 is never track media,
        // so an ungranted session ends as invalid_track_media as before.
        if(data.length>4 && data[4]===cursorKind){
          const decision=trackCursorPacket(session,data);
          if(decision==='invalid')return this.stop(session,'invalid_track_media');
          if(decision==='rate')return this.stop(session,'tracks_host_rate_limit');
          if(decision==='forward' && !session.ws)this.resume.cursor(session,data);
          else if(decision==='forward' && !this.send(session.ws,data,true))this.stop(session,'client_backpressure');
          return;
        }
        const result=state.media(data);
        if(result==='invalid')return this.stop(session,'invalid_track_media');
        // tracks-resume-v1: an output still without its IDR asks again while it is late.
        if(result==='waiting'&&state.awaitingKeys)this.resume.trackKeyFrame(session);
        if(result==='forward') {
          if(!session.ws)return; // session-resume-v1: the client is away; it restarts at an IDR.
          const observed = session.observability && parseTrackPacket(data)?.kind === 1 ? beginObservabilityForward(session,state.info?.topologyEpoch) : null;
          const written = observed ? () => completeObservabilityForward(session,observed,this.current(session) && session.trackTunnel===ws) : null;
          const failed = observed ? () => completeObservabilityForward(session,observed,false) : null;
          if(!this.send(session.ws,data,true,written,failed)){failed?.();this.stop(session,'client_backpressure');}
          else if(session.udpTracks)this.maybeGrantUdpTracks(session);
        }
        return;
      }
      let value;try{if(data.length>8192)throw Error();value=JSON.parse(data.toString('utf8'));}catch{return this.stop(session,'invalid_track_control');}
      if(!value || typeof value!=='object' || Array.isArray(value))return this.stop(session,'invalid_track_control');
      if(value.type==='udp.tracks.local'){const local=udpTrackLocal(session,value);if(local&&session.udpDirect&&udpPeerDirectAllowed(this.udp))this.udp.local(session.id,'host',local);return;}
      if(value.type==='udp.tracks.key'){const key=udpTrackKey(session,value);if(key)this.send(session.ws,{...value,publicKey:key.publicKey});return;}
      if(value.type==='stream.tracks.info'){
        const result=state.acceptInfo(value);
        if(result==='invalid')return this.stop(session,'invalid_track_info');
        if(result==='accepted'){if(session.udpTracks)this.retireUdpTracks(session);this.send(session.ws,state.info);}return;
      }
      if(value.type==='stream.tracks.stats'){
        const result=state.stats(withoutObservability(value));
        if(result===false)return this.stop(session,'invalid_track_stats');
        if(result)this.send(session.ws,observabilityStats(session,{...result,...(Object.hasOwn(value,'observability')?{observability:value.observability}:{})}));return;
      }
      if(value.type==='stream.tracks.resize.failed'){
        if(!state.failedResize(value))return;
        this.send(session.ws,{...value,reason:['unsupported_display_mode','display_change_failed','virtual_display_required','monitor_not_available','resize_failed'].includes(value.reason)?value.reason:'resize_failed',unchanged:false});
        this.stop(session,'resize_failed');return;
      }
      if(value.type==='stream.tracks.resize.restored'){
        const result=state.restoredResize(value);
        if(result==='invalid')return this.stop(session,'invalid_track_restore');
        if(result==='accepted')this.send(session.ws,state.pending.restored);return;
      }
      // display-scale-outputs-v1: the worker's per-output result of this
      // granted session; anything else about it ends the session.
      if(value.type==='display.scale.applied'){
        const result=displayScaleResult(value,true);
        if(!result || !session.displayScaleOutputs || result.sessionId!==session.id)return this.stop(session,'invalid_track_control');
        if(!this.send(session.ws,result))this.stop(session,'client_backpressure');return;
      }
      if(value.type==='stream.unavailable' && value.sessionId===session.id && allowedStreamFailure(session,value.reason))return this.unavailable(session,value.reason);
      if(value.type==='pong' && typeof value.id==='string' && value.id.length>=1 && value.id.length<=64 && Object.keys(value).every(k=>['type','id'].includes(k)))return this.send(session.ws,value);
      this.stop(session,'invalid_track_control');
    });
    if(held)this.resume.trackResumed(session,held);
  }
  close() {
    clearInterval(this.timer); clearInterval(this.heartbeat);
    this.hostReadyWaits.close();
    for (const session of this.sessions.values()) this.stop(session, 'server_stopping');
    for (const operation of this.displayBusy.values()) this.cancelDisplay(operation, 'server_stopping');
    for (const host of this.hosts.values()) this.dropHost(host, 'server_stopping');
    this.udp?.close();
    this.wss.close();
    this.clipboard.close();
    this.print.close();
    this.drive.close();
    this.usb.close();
    this.console.close();
    this.ime?.close();
  }
}

function validCapabilities(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  // A released Host also sends rdpSupported and rdpEnabled; they are not read.
  const flags = ['service', 'interactiveSession', 'streaming', 'virtualDisplayReady'];
  if (flags.some(key => typeof value[key] !== 'boolean')) return null;
  const privacyValue = validThumbnailPrivacy(value.thumbnailPrivacy);
  // Optional readonly privacy is independently fail-closed. Bad privacy must
  // not reject an otherwise valid Host streaming capability advertisement.
  const thumbnailPrivacy = privacyValue && (!privacyValue.allowed || value.interactiveSession === true) ? privacyValue : null;
  const consoleFields = ['consoleRecoverySupported', 'consoleRecoveryAvailable', 'consoleRecoveryReason'];
  const consoleDeclared = consoleFields.some(key => value[key] !== undefined);
  if (consoleDeclared && (typeof value.consoleRecoverySupported !== 'boolean' || typeof value.consoleRecoveryAvailable !== 'boolean' || !consoleReasons.has(value.consoleRecoveryReason))) return null;
  if (value.consoleRecoveryAvailable === true && (!value.service || value.consoleRecoverySupported !== true || !['rdp_session_active', 'rdp_session_disconnected'].includes(value.consoleRecoveryReason))) return null;
  if (value.qemuConsoleProtocol !== undefined && (![qemuConsoleProtocol, legacyQemuConsoleProtocol].includes(value.qemuConsoleProtocol) || value.service !== true)) return null;
  let monitors;
  let modeCount = 0;
  if (value.monitors !== undefined) {
    if (!Array.isArray(value.monitors) || value.monitors.length > 16 || value.monitors.some(monitor => !monitor || typeof monitor !== 'object' || Array.isArray(monitor) || !/^(?:[0-9]|1[0-5])$/.test(monitor.id) || typeof monitor.id !== 'string' || !boundedInteger(monitor.width,1,maximumGeometrySide) || !boundedInteger(monitor.height,1,maximumGeometrySide) || typeof monitor.primary !== 'boolean') || new Set(value.monitors.map(monitor => monitor.id)).size !== value.monitors.length) return null;
    for (const monitor of value.monitors) {
      // Portrait owned outputs (stream-size-v1) may advertise portrait modes.
      if (monitor.modes !== undefined && (!Array.isArray(monitor.modes) || monitor.modes.length > 64 || monitor.modes.some(mode => !mode || !ownedOutputSize(mode.width, mode.height, streamSizeCapability(value,streamSizeProtocolV2) ? streamSizeProtocolV2 : true) || !Number.isInteger(mode.refreshRate) || mode.refreshRate < 1 || mode.refreshRate > 240))) return null;
      modeCount += monitor.modes?.length ?? 0;
    }
    if (modeCount > 128) return null;
    monitors = value.monitors.map(({ id, width, height, primary, modes }) => ({ id, width, height, primary, ...(modes === undefined ? {} : { modes: modes.map(({ width, height, refreshRate }) => ({ width, height, refreshRate })) }) }));
  }
  if (value.dynamicResolution !== undefined && typeof value.dynamicResolution !== 'boolean') return null;
  if (value.diagnosticsSupported !== undefined && typeof value.diagnosticsSupported !== 'boolean') return null;
  if (value.streamProfileProtocol !== undefined && (value.streamProfileProtocol !== streamConsoleProfileProtocol || !value.service)) return null;
  if (value.videoTrackProtocol !== undefined && value.videoTrackProtocol !== trackProtocol) return null;
  if (value.trackViewportProvisioning !== undefined && typeof value.trackViewportProvisioning !== 'boolean') return null;
  if ((value.videoTrackProtocol !== undefined || value.trackViewportProvisioning === true) &&
      (value.videoTrackProtocol !== trackProtocol || !value.service || !value.interactiveSession || !value.streaming ||
       !value.virtualDisplayReady || ![1,2].includes(monitors?.length) || monitors.some(m=>m.id!=='0' && m.id!=='1'))) return null;
  if (value.viewportProvisioning !== undefined && typeof value.viewportProvisioning !== 'boolean') return null;
  if (value.virtualMonitorControl !== undefined && typeof value.virtualMonitorControl !== 'boolean') return null;
  if (value.clipboard !== undefined && typeof value.clipboard !== 'boolean') return null;
  if (value.clipboardSync !== undefined && typeof value.clipboardSync !== 'boolean') return null;
  if (value.clipboardSync === true && value.clipboard !== true) return null;
  if (value.clipboard === true && (!value.service || !value.interactiveSession)) return null;
  if (value.virtualMonitorControl === true && (!value.service || !value.interactiveSession || !value.virtualDisplayReady || ![1,2].includes(monitors?.length))) return null;
  if (value.resizeProtocol !== undefined && value.resizeProtocol !== 'epoch-v1') return null;
  if (value.dynamicResolution === true && (value.resizeProtocol !== 'epoch-v1' || !modeCount)) return null;
  if (value.viewportProvisioning === true && (value.dynamicResolution !== true || !value.service || !value.interactiveSession || !value.streaming || !value.virtualDisplayReady || ![1,2].includes(monitors?.length))) return null;
  const combinedProfile = value.streamProfileViewportProtocol !== undefined;
  const combinedTracks=value.streamProfileTracksProtocol!==undefined;
  if(combinedTracks&&!profileTracksCapability({...value,monitors}))return null;
  if (combinedProfile && (value.streamProfileViewportProtocol !== streamProfileViewportProtocol || value.streamProfileProtocol !== streamConsoleProfileProtocol || value.initialViewportProtocol !== initialViewportProtocol || value.viewportProvisioning !== true || ![1,2].includes(monitors?.length) || monitors.some(m => m.id !== '0' && m.id !== '1'))) return null;
  if (value.initialViewportProtocol !== undefined && (value.initialViewportProtocol !== initialViewportProtocol || value.viewportProvisioning !== true || (!combinedProfile && (value.virtualMonitorControl !== true || value.streamProfileProtocol !== undefined)) || !monitors?.some(m => m.id === '0' && (combinedProfile || m.primary)))) return null;
  if (value.viewportEncoderLimitsProtocol !== undefined && (value.viewportEncoderLimitsProtocol !== viewportEncoderLimitsProtocol || value.initialViewportProtocol !== initialViewportProtocol || value.viewportProvisioning !== true || value.dynamicResolution !== true || value.resizeProtocol !== 'epoch-v1' || monitors?.length !== 1 || monitors[0].id !== '0')) return null;
  const bitrate = bitrateCapability(value);
  return Object.fromEntries([...(thumbnailPrivacy ? [['thumbnailPrivacy', thumbnailPrivacy]] : []), ...(thumbnailCaptureCapability(value) ? [['thumbnailCaptureProtocol', thumbnailCaptureProtocol]] : []), ...([qemuConsoleProtocol, legacyQemuConsoleProtocol].includes(value.qemuConsoleProtocol) ? [['qemuConsoleProtocol', value.qemuConsoleProtocol]] : []), ...(value.sessionStopProtocol === sessionStopProtocol ? [["sessionStopProtocol", sessionStopProtocol]] : []), ...(value.keyFrameProtocol === keyFrameProtocol ? [['keyFrameProtocol', keyFrameProtocol]] : []), ...(hostResumeCapability(value) ? [['hostResumeProtocol', hostResumeProtocol]] : []), ...(hostChannelResumeCapability(value) ? [['hostChannelResumeProtocol', channelResumeProtocol]] : []), ...(bitrate ? Object.entries(bitrate) : []), ...(inputLatencyCapability(value) ? [['inputLatencyProtocol', inputLatencyProtocol]] : []), ...(observabilityCapability(value) ? [['observabilityProtocol', observabilityProtocol]] : []), ...(cursorCapability(value) ? [['cursorProtocol', cursorProtocol]] : []), ...(cursorTracksCapability(value) ? [['cursorTracksProtocol', cursorTracksProtocol]] : []), ...(cursorEchoCapability(value) ? [['cursorEchoProtocol', cursorEchoProtocol]] : []), ...(cursorComposeCapability(value) ? [['cursorComposeProtocol', cursorComposeProtocol]] : []), ...(textInputCapability(value) ? [['textInputProtocol', textInputProtocol]] : []), ...(displayScaleCapability(value) ? [['displayScaleProtocol', displayScaleProtocol]] : []), ...(displayScaleOutputsCapability(value) ? [['displayScaleOutputsProtocol', displayScaleOutputsProtocol]] : []), ...(touchInputCapability(value) ? [['touchInputProtocol', touchInputProtocol]] : []), ...(relativeMouseCapability(value) ? [['relativeMouseProtocol', relativeMouseProtocol]] : []), ...(relativeMouseTracksCapability(value) ? [['relativeMouseTracksProtocol', relativeMouseTracksProtocol]] : []), ...(caretCapability(value) ? [['caretProtocol', caretProtocol]] : []), ...(usbCapability(value) ? [['usbProtocol', usbProtocol]] : []), ...(udpMediaCapability(value) ? [['udpMediaProtocol', udpMediaProtocol]] : []), ...(directCapability(value) ? [['udpDirectHostProtocol', udpDirectProtocol]] : []), ...(udpTracksCapability(value) ? [['udpTracksHostProtocol', udpTracksProtocol]] : []), ...(audioOpusCapability(value) ? [['audioCodecProtocol', audioOpusProtocol]] : []), ...(audioOpusTracksCapability(value) ? [['audioCodecTracksProtocol', audioOpusTracksProtocol]] : []), ...(trackResumeCapability(value) ? [['trackResumeProtocol', trackResumeProtocol]] : []), ...(driveCapability(value) ? [['driveProtocol', driveProtocol]] : []), ...(driveFsCapability(value) ? [['driveFs', true]] : []), ...(driveUnavailableCapability(value) ? [['driveUnavailableReason', value.driveUnavailableReason]] : []), ...(streamSizeCapability(value) ? [['streamSizeProtocol', value.streamSizeProtocol]] : []), ...(streamSizeCapability(value,streamSizeProtocolV2) ? [['streamSizeProtocols',[streamSizeProtocol,streamSizeProtocolV2]]] : []), ...(printCapability(value) ? [['printProtocol', printProtocol]] : []), ...(printUnavailableCapability(value) ? [['printUnavailableReason', value.printUnavailableReason]] : []), ...(combinedTracks?[["streamProfileTracksProtocol",profileTracksProtocol]]:[]), ...(value.viewportEncoderLimitsProtocol === undefined ? [] : [['viewportEncoderLimitsProtocol', viewportEncoderLimitsProtocol]]), ...(combinedProfile ? [['streamProfileViewportProtocol', streamProfileViewportProtocol]] : []), ...(value.initialViewportProtocol === undefined ? [] : [['initialViewportProtocol', initialViewportProtocol]]), ...(value.streamProfileProtocol === undefined ? [] : [['streamProfileProtocol', value.streamProfileProtocol]]), ...(consoleDeclared ? consoleFields.map(key => [key, value[key]]) : []), ...(value.diagnosticsSupported === undefined ? [] : [['diagnosticsSupported', value.diagnosticsSupported]]), ...(imeStateCapability(value) ? [['imeStateProtocol', imeStateProtocol]] : []), ...(value.service === true && value.signinServiceProtocol === signinServiceProtocol ? [['signinServiceProtocol', signinServiceProtocol]] : []), ...flags.map(key => [key, value[key]]), ...(value.secureDesktop === true && value.service === true && value.interactiveSession === true ? [['secureDesktop', true]] : []), ...(monitors === undefined ? [] : [['monitors', monitors], ['displayCount', monitors.length]]), ...(value.dynamicResolution === undefined ? [] : [['dynamicResolution', value.dynamicResolution]]), ...(value.viewportProvisioning === undefined ? [] : [['viewportProvisioning', value.viewportProvisioning]]), ...(typeof value.viewportFailureReason === 'string' && viewportFailures.has(value.viewportFailureReason) ? [['viewportFailureReason', value.viewportFailureReason]] : []), ...(value.resizeProtocol === undefined ? [] : [['resizeProtocol', value.resizeProtocol]]), ...(value.virtualMonitorControl === undefined ? [] : [['virtualMonitorControl', value.virtualMonitorControl]]), ...(value.clipboard === undefined ? [] : [['clipboard', value.clipboard]]), ...(value.clipboardSync === undefined ? [] : [['clipboardSync', value.clipboardSync]]), ...(value.videoTrackProtocol === undefined ? [] : [['videoTrackProtocol', value.videoTrackProtocol]]), ...(value.trackViewportProvisioning === undefined ? [] : [['trackViewportProvisioning', value.trackViewportProvisioning]])]);
}

// `portrait` (stream-size-v1 asked): the box is two independent per-side
// encoding ceilings up to 3840 each; the pixel limits come from the decoder.
function streamOptions(value, portrait = false) {
  if (value === undefined) return { ...defaultStreamOptions };
  if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).some(key => !Object.hasOwn(defaultStreamOptions, key) && key !== 'bitrateMode')) throw new HttpError(400, 'invalid_stream_options');
  const options = { ...defaultStreamOptions, ...value };
  const dimension = (number, max) => Number.isInteger(number) && number >= 128 && number <= max && number % 2 === 0;
  if (!(portrait===streamSizeProtocolV2 ? videoGeometry(options.maxWidth,options.maxHeight,16,16) : dimension(options.maxWidth,3840) && dimension(options.maxHeight,portrait?3840:2160)) || ![30, 60].includes(options.fps) ||
      !Number.isInteger(options.bitrate) || options.bitrate < 4000000 || options.bitrate > adaptiveMaxBitrate || (options.bitrateMode !== undefined && !['auto', 'fixed'].includes(options.bitrateMode)) || typeof options.audio !== 'boolean' || typeof options.monitor !== 'string' || !/^(?:[0-9]|1[0-5]|all)$/.test(options.monitor)) throw new HttpError(400, 'invalid_stream_options');
  return options;
}

// `opus`: the Host advertises audio-opus-v1, so its audio may be Opus packets of any
// length (a late one of an earlier session included); sessions check their own.
function validMediaEnvelope(data, opus = false) {
  return data.length > 12 && data.length <= MAX_MESSAGE && [1, 2].includes(data[0]) &&
    (data[0] === 1 ? (data[1] & ~1) === 0 : data[1] === 0 && validAudioPayload(data.length - 12, opus));
}
export function validMedia(data, epoch = 0) { return validMediaEnvelope(data) && data.readUInt16LE(2) === epoch; }
// `portrait`: a stream-size-v1 session (portrait sizes up to 2160x3840).
function validResize(value, portrait = false) {
  return Object.keys(value).every(key => ['type', 'width', 'height', 'fps', 'monitor', 'requestId',...(portrait===streamSizeProtocolV2?['decoderLimits']:[])].includes(key)) &&
    (value.decoderLimits===undefined || portrait===streamSizeProtocolV2 && validDecoderLimits(value.decoderLimits,streamSizeProtocolV2)) &&
    ownedOutputSize(value.width, value.height, portrait) && [30,60].includes(value.fps) && typeof value.monitor === 'string' && /^(?:[0-9]|1[0-5]|all)$/.test(value.monitor) && typeof value.requestId === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(value.requestId);
}
function validStreamInfo(v, portrait = false, opus = false) {
  return v.codec === 'h264' && encodedSize(v.width, v.height, portrait) && boundedInteger(v.sourceWidth,v.width,maximumGeometrySide) && boundedInteger(v.sourceHeight,v.height,maximumGeometrySide) && v.sourceWidth<=Math.floor(Number.MAX_SAFE_INTEGER/v.sourceHeight) && Number.isInteger(v.fpsTarget) && v.fpsTarget >= 1 && v.fpsTarget <= 60 && validStreamAudio(v.audio, opus);
}
function validClientControl(v) {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return false;
  if (v.type === 'input.reset') return true;
  if (v.type === 'ping') return typeof v.id === 'string' && v.id.length >= 1 && v.id.length <= 64;
  if (v.type !== 'input') return false;
  if (v.event === 'key') return Number.isInteger(v.vk) && v.vk >= 1 && v.vk <= 255 && Number.isInteger(v.scan) && v.scan >= 0 && v.scan <= 65535 && typeof v.down === 'boolean' && typeof v.extended === 'boolean';
  if (!Number.isFinite(v.x) || !Number.isFinite(v.y) || v.x < 0 || v.x > 1 || v.y < 0 || v.y > 1) return false;
  return v.event === 'move' || (v.event === 'button' && ['left','right','middle','x1','x2'].includes(v.button) && typeof v.down === 'boolean') || (v.event === 'wheel' && Number.isInteger(v.delta) && Math.abs(v.delta) <= 12000);
}
