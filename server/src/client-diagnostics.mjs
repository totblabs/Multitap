import { observabilityDiagnosticBounds } from './stream-observability.mjs';
import { HttpError } from './errors.mjs';
import { maxUdpRelays } from './udp-media.mjs';

const events = new Set(['appStarted', 'signIn', 'inventory', 'streamStart', 'streamReady', 'streamEnd', 'viewportRequest', 'viewportResult', 'displayConfigure', 'brokerRequest', 'updateCheck', 'updateResult', 'updateDownload', 'streamStats',
  // Clipboard channel and transfer outcomes (fixed codes, no content).
  'clipboard',
  // drive-v1 clients (recorded only in a session this broker granted drives):
  // share/mapping/channel outcomes, fixed codes and counts, never names or paths.
  'drive',
  // Client with usb-v1: device sharing outcomes (fixed codes and counts; never a VID/PID, name or data).
  'usb',
  // stream-stall-v1 clients (only after this broker acknowledged it): one row per
  // displayed-video gap of 250 ms or more, fixed numbers only.
  'streamStall']);
// stream-stats-v1: one streaming statistics window (fps x10, times in us).
// Acknowledged to the client so released brokers are never sent these rows.
export const streamStatsProtocol = 'stream-stats-v1';
// stream-stall-v1: acknowledged like stream-stats-v1, so released brokers never get streamStall rows.
export const streamStallProtocol = 'stream-stall-v1';
// udp-media-v1 probe result. udpProbe: 0 no grant, 1 reachable, 2 socket error,
// 3 name lookup failed, 4 BIND timeout, 5 no PONG. A value past its bound is
// dropped, not clamped: a wrong number must not read as a measurement.
const udpProbeBounds = Object.freeze({ udpProbe: 5, udpRttP50Ms: 60000, udpRttP95Ms: 60000, udpLossPct: 100, udpRelayIndex: maxUdpRelays - 1, udpKeepSent: 0xffffffff, udpKeepLost: 0xffffffff, udpActivePath: 1, udpDirectPath: 2, udpDirectRttMs: 60000, udpRelayRttMs: 60000, udpFallbacks: 0xffffffff, udpGaps: 0xffffffff, udpLegFailed: 1,
  // Android stall watch: key-frame requests, stalls counted once each, and the receiver's state at the last one (1 decoder queue
  // full, 2 refused, 3 loss given up, 4 another epoch, 5 next message waiting for repair, 6 nothing of it arrived).
  udpKeyRequests: 0xffffffff, udpStalls: 0xffffffff, udpStallCause: 6,
  // Windows leg (lifetime): video that waited for a full decode queue, skips to the next key frame, NACK datagrams.
  udpQueueWaits: 0xffffffff, udpQueueSkips: 0xffffffff, udpNacks: 0xffffffff,
  // stream-stall-v1 rows: 1 no frame was published (decode side), 2 a frame waited for the client's
  // presentation; the frame that ended the gap was a key frame (1).
  stallKind: 2, stallKey: 1 });
const diagnosticBounds = { ...observabilityDiagnosticBounds, ...udpProbeBounds };
const streamStatsNumeric = [...Object.keys(observabilityDiagnosticBounds), 'samples', 'windowMs', 'srcWidth', 'srcHeight', 'decodeFps10', 'decodeFpsMin10', 'textureFps10', 'captureFps10', 'encodeFps10',
  // tracks-v1 clients: the slowest output's rates (the others are summed over outputs).
  'decodeFpsLow10', 'captureFpsLow10', 'encodeFpsLow10', 'receiveKbps', 'bitrateKbps', 'targetKbps', 'networkDelayMs', 'rttMs', 'rttMaxMs', 'hostWorkUs', 'hostP95Us', 'hostMaxUs', 'captureUs', 'convertUs', 'encodeUs', 'decodeUs', 'decodeMaxUs', 'queueMaxMs', 'skipped', 'losslessTiles', 'losslessKbps', 'inputHostP50Us', 'inputHostP95Us', 'inputFrameP50Us', 'inputFrameP95Us',
  // texture-pacing-v1 clients: decoded -> taken by Flutter wait and
  // Flutter's texture upload (us), frames queued/skipped in the window, the
  // texture path (0 = CPU pixel buffer, 1 = GPU surface) and the composition
  // rate (Hz) the client's Flutter paces frames at.
  'textureWaitUs', 'textureUploadUs', 'queuedFrames', 'skippedFrames', 'texturePath', 'displayHz',
  // Per-stage maxima of the host pipeline (which stage a slow frame spent its time in).
  'topologyMaxUs', 'captureMaxUs', 'convertMaxUs', 'encodeMaxUs',
  // Backpressure that does not end the stream (video packets
  // that waited for a full decode queue; ms of audio the playback queue dropped).
  'mediaWaits', 'audioDropMs',
  // Where input -> frame goes (latest rolling
  // p50/p95 in us). Host clock: injection -> capture tick of the changed frame,
  // that tick -> encoder output. Client clock: tagged packet arrival -> native
  // engine accepted it, accepted -> handed to the texture.
  'hostInputCaptureP50Us', 'hostInputCaptureP95Us', 'hostCaptureEncodedP50Us', 'hostCaptureEncodedP95Us', 'inputPushP50Us', 'inputDecodeP50Us',
  // The tagged packet after the encoder:
  // host clock, encoder output -> its WebSocket write start
  // (worker pipe, service send queue) and that write's duration; relay clock,
  // the host's complete message -> written toward the client; client clock, its
  // tag's arrival -> the packet's arrival (the bytes after the first) and the
  // packet's size (bytes, p50).
  'hostSendQueueP50Us', 'hostSendQueueP95Us', 'hostSendWriteP50Us', 'hostSendWriteP95Us', 'relayForwardP50Us', 'relayForwardP95Us',
  'inputTransferP50Us', 'inputTransferP95Us', 'inputFrameBytes',
  // Per input (frame-timing-v1 tags): the host's tag write -> the packet's
  // arrival at the client, the uplink estimate and the host's durations removed.
  'inputDownlinkP50Us', 'inputDownlinkP95Us',
  // Android client input in the window (counts only, never content): touch
  // mode (0 direct, 1 trackpad), taps, right clicks, drags started, wheel
  // steps, pinches, keyboard opens, text commits sent as text-input-v1 and
  // their UTF-16 units, characters typed as key presses (Hosts without
  // text-input-v1), characters that could not be sent, PC keys pressed,
  // hardware mouse button changes.
  'touchMode', 'taps', 'rightClicks', 'dragStarts', 'wheelSteps', 'pinches', 'keyboardOpens', 'textCommits', 'textUnits', 'textKeyChars', 'textDropped', 'keyPresses', 'mouseButtons',
  // Android picture crispness: the Windows display scale the Host applied to
  // the streamed output (display-scale-v1, percent; 0 = none), the one this
  // device asked for, and device pixels per remote pixel (1 = the device's
  // own pixels, 2 = exact 2x for Hosts without display-scale-v1).
  'displayScale', 'displayScaleWanted', 'pixelScale',
  // Android direct touch (touch-input-v1): touch frames sent in the window
  // and the most contacts held at once (counts only, never positions).
  'touchFrames', 'touchContacts',
  // stream-size-v1 sessions: the encoded picture's scale of the source (per
  // mille), why it is below 1000 (0 none, 1 decoder size, 2 decoder rate,
  // 3 measured client decode load, 4 measured Host encoder load, 5
  // administrator cap) and this client's decoder busy share (per mille).
  'encodeScale', 'encodeLimit', 'decoderBusy',
  // client-load-v1 (tenths of a percent): app CPU, PC CPU, app GPU video-decode / 3D engine.
  'appCpuPct10', 'systemCpuPct10', 'gpuDecodePct10', 'gpu3dPct10',
  // Where H.264 decodes (0 CPU, 1 hardware on the render adapter, 2 another
  // adapter) and that adapter's PCI vendor id (a company, never a device).
  'decodeTier', 'decodeVendor',
  // usb-v1 (counts only): devices shared at the end of the window, requests
  // served, KiB moved, and the USB class byte of a refused device. No
  // vendor/product ID, serial number or name is ever sent: they identify the
  // user's own hardware and no decision here needs them.
  'usbDevices', 'usbUrbs', 'usbKib', 'usbClass',
  // usb-v1 stage 15: one listing's counts (listed, this PC's own,
  // refused by the policy, descriptors unreadable).
  'usbFound', 'usbHidden', 'usbDenied', 'usbUnreadable',
  // usb-v1: requests that stalled / failed / timed out / ran longer than 5 s, the class of the first
  // request that was not a plain success (bmRequestType<<8 | bRequest, or 0x10000 | type<<8 | endpoint address;
  // never data) and whether Windows took the device back after it was given back (0 unknown, 1 yes, 2 no).
  'usbStalls', 'usbFailed', 'usbTimeouts', 'usbSlow', 'usbReq', 'usbBack',
  // usb-v1: the raw Windows USBD_STATUS of that first request (a fixed
  // status number from usb.h, e.g. 0xC0000004 stall; never data).
  'usbUsbd',
  // The longest gap between video frames in the window (ms).
  'videoGapMaxMs', ...Object.keys(udpProbeBounds),
  // stream-stall-v1: gaps of 250 ms or more in the window and the longest (ms); the rate the Host's capture loop paces at.
  'stalls', 'stallMaxMs', 'fpsTarget',
  // audio-stats-v1 (client lib/data/audio_stats.dart): the client's jitter buffer depth, playback latency
  // estimate (ms, while sound played) and target, underruns, overruns, frames repeated / dropped by the
  // drift correction, the mean A/V offset (sound behind or ahead of the picture, ms); the Host's capture ->
  // packet mean/max (us), audio packets handed on / dropped and their send-queue wait mean/max (us).
  'audioDepthMs', 'audioTargetMs', 'audioLatencyMs', 'audioUnderruns', 'audioOverruns', 'audioInserted', 'audioTrimmed', 'audioAvLagMs', 'audioAvLeadMs',
  // Opus 10 ms frames the client concealed (audio-opus-v1 only), the stream's audio
  // format (1 PCM, 2 Opus; also on streamReady) and the client player's last error as an unsigned 32-bit status
  // (0 none; a Windows HRESULT, or 0xffffffff on Android).
  'audioConcealed', 'audioFormat', 'audioError',
  'hostAudioCaptureUs', 'hostAudioCaptureMaxUs', 'hostAudioSent', 'hostAudioDropped', 'hostAudioQueueUs', 'hostAudioQueueMaxUs',
  // audio-path-v1 (Android client, lib/data/audio_stats.dart audioPathFields): which decoder plays
  // the sound (0 none, 1 platform software Opus, 2 another Opus decoder, 3 none usable), the output format it stated
  // (Hz, channels), the level the track waits for and its buffer (ms), the track's own underrun count, tracks made
  // again (lost by the platform / written to and not playing), audio focus (0 none, 1 held, 2 waiting, 3 paused,
  // 4 lost) and the Android API level. Flags (audioOutFloat, audioBufferFollows, audioFastTrack, audioMuted,
  // audioOpusUnusable) are in `boolean` below. Fixed numbers only, never audio content.
  'audioDecoder', 'audioOutRate', 'audioOutChannels', 'audioStartMs', 'audioBufferMs', 'audioTrackUnderruns', 'audioTrackRestarts', 'audioTrackStuck', 'audioFocus', 'androidApi',
  // Android client: the low-latency configuration its H.264 decoder accepted
  // (bits: 1 KEY_LOW_LATENCY, 2 real-time priority, 4 operating rate, 8 vendor
  // keys, 16 the decoder declares FEATURE_LowLatency) and the vendor key set
  // its name matched (0 none, 1 Qualcomm, 2 Exynos, 3 Kirin, 4 Amlogic, 5 MediaTek).
  'decoderLowLatency', 'decoderVendorKeys'];
// One streamStall row (client lib/data/stream_stall.dart): the client engine's gap record (ms, queue depths,
// packets offered/refused, decode calls and the longest in us, frames waiting/skipped, wait for presentation),
// the longest arrival gap, UDP leg counts during it, the Host windows that cover it (gaps and stage times in ms,
// IDRs, key requests, bitrate changes, lowest target in kbps, UDP pacer/client key frames and refused repairs),
// and the round trip, decoder busy share and stalls not sent (a row of only stallDropped closes a session).
// stall-v2: the Host's spacing of the two frames around the gap (stallPtsGapMs) and the longest
// a changed picture waited on the Host before its encode (hostChangeLateMs); idle-screen gaps send no row.
const streamStallNumeric = ['stallMs', 'stallQueue', 'stallQueueMax', 'stallOffers', 'stallFull', 'stallDecodes', 'stallDecodeMaxUs', 'stallPending', 'stallSkipped', 'stallWaitMs',
  'stallPtsGapMs', 'stallArrivalMs', 'stallUdpLost', 'stallUdpKeys', 'stallUdpWaits', 'stallUdpSkips', 'stallUdpNacks', 'stallUdpFallbacks',
  'hostWindows', 'hostEncodeGapMs', 'hostCaptureGapMs', 'hostWaitMs', 'hostCaptureMs', 'hostConvertMs', 'hostEncodeMs', 'hostKeyFrames', 'hostKeyRequests', 'hostBitrateSteps', 'hostTargetMinKbps',
  'hostUdpBacklogKeys', 'hostUdpClientKeys', 'hostUdpRefused', 'hostChangeLateMs', 'stallDropped'];
// viewportResult, milliseconds: the client's
// decoder check before the request, request -> relay acknowledgement, request
// -> the new size's stream.info, its decoder start, decoder start -> first new
// picture (elapsedMs: request -> picture); then the Host's stages from that
// stream.info (Host resize_stages.hpp): old capture stopped, display change
// with its SDK call, Windows settling and capture proof, and the restart with
// its capture, encoder and audio starts.
export const viewportStageNumeric = ['probeMs', 'ackMs', 'infoMs', 'decoderMs', 'frameMs', 'hostSuspendMs', 'hostDisplayMs', 'hostSdkMs', 'hostSettleMs', 'hostConfirmMs', 'hostRestartMs', 'hostCaptureStartMs', 'hostEncoderStartMs', 'hostAudioStartMs'];
// connect-timing-v1 (client lib/data/connect_timing.dart): one connection's
// stages in ms, on its first streamReady (elapsedMs is the whole session part):
// the session window, its desktop read, the view's preparation steps and the
// connect request; the relay socket, the first stream info, the decoder start,
// the first key frame and its picture; the Host's own stages (its stream.info).
export const connectTimingNumeric = ['windowMs', 'loadMs', 'retireMs', 'refreshMs', 'presentMs', 'outputsMs', 'viewportMs', 'connectMs',
  'dialMs', 'infoMs', 'decoderMs', 'keyFrameMs', 'firstFrameMs', 'hostAdmitMs', 'hostWorkerMs', 'hostDisplayMs', 'hostMediaMs'];
// shares / mapped / requests / failedRequests: drive-v1 counts (folders shared,
// drive letters the Host mapped, file requests served and refused).
const numeric = new Set(['action', 'stage', 'monitors', 'width', 'height', 'error', 'elapsedMs', 'shares', 'mapped', 'requests', 'failedRequests', 'remembered', 'restored', ...streamStatsNumeric, ...streamStallNumeric, ...viewportStageNumeric, ...connectTimingNumeric]);
// gpuHardware: the client renders on a hardware GPU (false: WARP / basic render
// driver); remoteSession: the client itself runs inside a Remote Desktop session.
// textInput / touchInput: the session negotiated text-input-v1 /
// touch-input-v1 (Android client). decoderPrestarted (streamReady, Windows
// client): its decoder started before the stream info (decoder-prestart-v1).
const boolean = new Set(['ok', 'active', 'pending', 'recovery', 'remote', 'running', 'online', 'changed', 'available', 'auto', 'gpuHardware', 'remoteSession', 'textInput', 'touchInput', 'decoderPrestarted',
  // audio-path-v1 flags (see streamStatsNumeric).
  'audioOutFloat', 'audioBufferFollows', 'audioFastTrack', 'audioMuted', 'audioOpusUnusable']);
// Independent outputs (모든 모니터), media engine and power codes.
const codes = new Set(['unknown_failure', 'network_error', 'invalid_response', 'request_failed', 'authentication_required', 'session_expired', 'desktop_not_found', 'host_unavailable', 'host_disconnected', 'host_offline', 'host_maintenance', 'session_cleanup_pending', 'capabilities_pending', 'virtual_display_required', 'no_interactive_session', 'streaming_unavailable', 'display_control_unavailable', 'display_not_supported', 'display_busy', 'display_in_progress', 'display_operation_failed', 'display_operation_unknown', 'display_operation_timeout', 'display_operation_not_found', 'display_configuration_failed', 'display_count_mismatch', 'display_timeout', 'worker_failed', 'operation_cancelled', 'invalid_display_request', 'invalid_display_count', 'too_many_display_attempts', 'power_in_progress', 'rdp_not_supported', 'desktop_busy', 'session_busy', 'controller_in_use', 'taken_over', 'other_user', 'initial_viewport_unavailable', 'invalid_initial_viewport', 'viewport_encoder_limits_unavailable', 'invalid_viewport_encoder_limits', 'track_streaming_unavailable', 'local_display_layout_unavailable', 'local_display_layout_unsupported', 'console_preparation_failed', 'console_preparation_timeout', 'native_display_not_ready', 'native_topology_recovery_required', 'initial_track_viewport_timeout', 'track_resize_timeout', 'track_resize_failed', 'invalid_stream_message', 'websocket_handshake_or_transport', 'media_platform_error', 'media_timeout', 'independent_outputs_unavailable', 'engine_busy', 'invalid_stream', 'media_backpressure', 'multiple_monitors_required', 'presentation_failed', 'power_status_unavailable', 'invalid_power_state', 'power_task_failed', 'too_many_power_attempts', 'power_busy', 'proxmox_unreachable', 'proxmox_invalid_response', 'power_task_not_found', 'invalid_power_action', 'session_replaced', 'window_closed', 'resize_failed', 'display_change_failed', 'unsupported_display_mode', 'monitor_not_available', 'administrator_required', 'console_binding_changed', 'console_busy', 'console_in_use', 'console_ready', 'console_timeout', 'console_transfer_failed', 'console_transfer_pending', 'console_unavailable', 'native_provider_required', 'rdp_session_active', 'rdp_session_disconnected', 'reboot_required', 'virtual_device_not_ready', 'windows_identity_mismatch', 'windows_identity_required', 'windows_login_required', 'windows_session_ambiguous', 'windows_session_locked', 'windows_session_unknown', 'takeover_context_stale', 'takeover_context_expired', 'invalid_takeover_context', 'invalid_context_protocol', 'session_context_unavailable']);
const id = value => typeof value === 'string' && /^[A-Za-z0-9_-]{1,80}$/.test(value);
codes.add('active_session_limit'); // Bounded login admission; never a token or device identity.
// Fixed first-party signin-stream-v1 outcomes only. No peer error text or
// unrecognized suffix is admitted by this finite list.
for (const code of ['signin_stream_unavailable', 'invalid_signin_request', 'signin_authority_lost', 'invalid_signin_info', 'invalid_signin_ack', 'signin_phase_changed']) codes.add(code);
codes.add('server_restarting'); // 502/503/504 or a reset while the broker itself restarts.
// The relay's connect refusals (relay.mjs, 409), e.g. a Host restarting for its update: not registered yet,
// or not yet able to stream. Kept literally (the client's code test reads this file).
for (const code of ['host_not_registered', 'stream_unavailable', 'desktop_unavailable', 'desktop_changed', 'host_machine_mismatch', 'monitor_unavailable', 'vm_audio_busy', 'proxmox_not_configured', 'proxmox_settings_changed', 'host_diagnostics_update_required']) codes.add(code);
for (const code of ['frame_layout_unsupported','frame_memory_unavailable','frame_memory_insufficient','encoder_format_unsupported']) codes.add(code);
for (const code of ['initial_viewport_timeout', 'initial_viewport_mismatch', 'initial_frame_timeout', 'video_timeout', 'output_video_timeout', 'decoder_failed', 'output_decoder_stalled', 'decoder_stalled', 'media_status_failed', 'resize_timeout', 'resize_frame_timeout', 'capture_failed', 'encoder_failed', 'host_revoked', 'server_stopping', 'connection_overloaded', 'display_changed', 'session_ended', 'authorization_revoked', 'relay_closed', 'host_timeout', 'loopback_error', 'client_backpressure']) codes.add(code);
const uuid = value => typeof value === 'string' && /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/i.test(value);
codes.add('viewport_frame_timeout');codes.add('viewport_apply_timeout');
// Broker-owned emergency console, fixed outcomes only; no upstream URL,
// ticket, VNC challenge, framebuffer or free-form reason is collected.
for (const code of ['console_permission_denied','console_identity_unavailable','console_identity_changed','console_vm_unavailable','console_no_display','console_certificate_error','console_invalid_upstream','console_crypto_unavailable','console_rfb_unsupported','console_protocol_error','console_clipboard_denied','console_limits_exceeded','console_limit_reached','console_backpressure','console_cancelled','console_timeout','console_transport_closed','console_auth_failed','console_unavailable','console_session_not_found']) codes.add(code);
// Clipboard outcomes.
for (const code of ['clipboard_busy', 'clipboard_cancelled', 'clipboard_changed', 'clipboard_commit_uncertain', 'clipboard_disk_full', 'clipboard_empty', 'clipboard_file_locked', 'clipboard_file_unavailable', 'clipboard_hash_mismatch', 'clipboard_invalid_message', 'clipboard_invalid_url', 'clipboard_limits_exceeded', 'clipboard_pair_timeout', 'clipboard_policy_denied', 'clipboard_read_failed', 'clipboard_session_closed', 'clipboard_session_limit', 'clipboard_text_invalid', 'clipboard_timeout', 'clipboard_transfer_failed', 'clipboard_transport_closed', 'clipboard_unavailable', 'clipboard_watch_unavailable', 'clipboard_write_failed', 'unsupported_clipboard_format']) codes.add(code);
// usb-v1: fixed outcomes of sharing a local USB device (docs/usb-redirection.md).
// Listed literally (the client's code test reads this file).
for (const code of ['usb_class_denied', 'usb_hid_boot_denied', 'usb_isochronous_denied', 'usb_descriptor_invalid', 'usb_class_not_granted', 'usb_vhci_unavailable', 'usb_limit_reached', 'usb_attach_failed', 'usb_busy', 'usb_user_stopped', 'usb_device_removed', 'usb_vm_detached', 'usb_policy_changed', 'usb_device_error', 'usb_session_closed', 'usb_driver_missing', 'usb_driver_install_failed', 'usb_vhci_unprotected', 'usb_setup_failed', 'usb_restart_required', 'usb_invalid_message', 'usb_out_of_order', 'usb_limits_exceeded', 'usb_rate_limit', 'usb_backpressure', 'usb_pair_timeout', 'usb_transport_closed', 'usb_unavailable', 'usb_invalid_url', 'usbdk_missing', 'usbdk_install_failed', 'usbdk_signature_invalid', 'usbdk_restart_required', 'usb_capture_failed', 'usb_transfer_failed', 'usb_device_busy', 'usb_limit_session', 'usb_capture_timeout', 'usb_prepare_timeout', 'usb_return_unknown', 'usb_request_stuck', 'usb_device_not_returned']) codes.add(code);
// UsbDk already gives the device to another remote window or
// program, and 이 창으로 옮기기 whose other window did not let the device go in time.
for (const code of ['usb_device_redirected', 'usb_move_timeout']) codes.add(code);
// Release-signature clients: an update refused for a missing or mismatching
// project release signature (multitap-windows-release-signature-v1).
codes.add('signature_invalid');
// Client update downloads: network, server, disk and verification outcomes
// (product_updates.dart downloadFailureCodes) and the installer checks; with
// the versions (logVersion), requests made and KiB on disk when it ended.
for (const code of ['download_network', 'download_timeout', 'download_server_error', 'download_not_found', 'download_http_status', 'download_disk_full', 'download_file_error', 'download_size_mismatch', 'download_failed', 'hash_mismatch', 'package_version_mismatch', 'installer_start_failed', 'invalid_catalog']) codes.add(code);
for (const key of ['current', 'latest', 'attempts', 'receivedKib']) numeric.add(key);
// The relay's own session end reasons, recorded as sent.
for (const code of ['client_disconnected', 'client_error', 'client_stopped', 'client_timeout', 'clipboard_policy_changed', 'display_reconfigure', 'display_result_invalid', 'host_backpressure', 'host_error', 'host_rate_limit', 'invalid_control', 'invalid_input', 'invalid_media', 'invalid_resize_info', 'invalid_stream_info', 'invalid_track_control', 'invalid_track_info', 'invalid_track_media', 'invalid_track_ready', 'invalid_track_restore', 'invalid_track_stats', 'tracks_host_backpressure', 'tracks_host_disconnected', 'tracks_host_error', 'tracks_host_rate_limit', 'tracks_prepare_timeout', 'tracks_source_stalled', 'tracks_source_timeout']) codes.add(code);
// Broker-initiated ends (an update asked to disconnect now;
// a Host dropped for an invalid console-preparation reply).
for (const code of ['host_update', 'invalid_console_result']) codes.add(code);
// drive-v1: file request, Host mapping and pair outcomes (docs/drive-redirection.md).
// Listed literally (the client's code test reads this file); drive.mjs keeps
// the same sets for the relay.
for (const code of ['drive_not_found', 'drive_exists', 'drive_not_empty', 'drive_read_only', 'drive_denied', 'drive_link_refused', 'drive_invalid_path', 'drive_share_unknown', 'drive_busy', 'drive_no_space', 'drive_is_directory', 'drive_not_directory', 'drive_io_failed', 'drive_webclient_unavailable', 'drive_letter_unavailable', 'drive_map_failed', 'drive_invalid_message', 'drive_limits_exceeded', 'drive_out_of_order', 'drive_rate_limit', 'drive_backpressure', 'drive_timeout', 'drive_pair_timeout', 'drive_transport_closed', 'drive_policy_changed', 'drive_unavailable', 'drive_user_unavailable', 'drive_invalid_url', 'drive_session_closed', 'drive_webclient_old_limit', 'drive_file_too_large']) codes.add(code);
// The relay's `detail` of a Host connection that ended
// (relay.mjs hostErrorDetail / host-closing-v1), recorded instead of host_error.
export const hostEndDetails = ['host_frame_invalid', 'host_message_too_large', 'host_text_invalid', 'host_message_fragmented', 'host_transport_error', 'host_service_stopping', 'host_protocol_rejected', 'host_closing'];
for (const code of hostEndDetails) codes.add(code);
const time = value => typeof value === 'string' && value.length <= 32 && /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}(?:\d{3})?Z$/.test(value) && Number.isFinite(Date.parse(value)) && new Date(value).toISOString().slice(0, 23) === value.slice(0, 23);
export const clientDiagnosticRetentionMs = 7 * 86400000;
// Raised with clientDiagnosticNewestKept so older evidence keeps its 512 rows.
export const clientDiagnosticBatchesPerUser = 1024, clientDiagnosticBatchesTotal = 4096;
// Keep the existing hard row bounds. Routine successful inventory polling must
// not displace connection, update, display or failure evidence within them.
// Periodic streaming statistics are routine too: a long session's windows
// must not displace connection, update, display or failure evidence.
// A batch's kind is stored once as `evidence` (1: holds anything but routine
// polling/statistics). Ranking every row through json_each on each upload cost
// ~18 ms of event loop per upload at 4096 rows; this SQL form only fills rows an
// older broker wrote (store.mjs).
export const clientDiagnosticEvidenceSql = `CASE WHEN EXISTS (SELECT 1 FROM json_each(report,'$.events') event WHERE json_type(event.value,'$.code') IS NOT NULL OR NOT (json_extract(event.value,'$.event') IN ('streamStats','streamStall') OR (json_extract(event.value,'$.event')='inventory' AND json_extract(event.value,'$.values.ok') IS 1))) THEN 1 ELSE 0 END`;
// The newest batches stay whatever they hold: ranked by kind alone, a statistics
// window lost to a full account's older evidence the moment it arrived. A PC and a
// phone on one account fill 128 in under an hour (the phone's windows
// are gone before they can be read); 384 is a few hours of both.
export const clientDiagnosticNewestKept = 384;
const routineInventory = event => !Object.hasOwn(event, 'code') && (event.event === 'streamStats' || event.event === 'streamStall' || event.event === 'inventory' && event.values.ok === true);
export const clientDiagnosticEvidence = report => report.events.some(event => !routineInventory(event)) ? 1 : 0;

// Keeps `limit` batches of one account (userId) or of all (null): the newest
// clientDiagnosticNewestKept whatever they hold, then evidence before routine
// rows, newer before older. Deletes at most `most` rows per call, so an excess
// left by anything else is worked off in small steps (retention.mjs).
export function boundClientDiagnostics(store, userId, limit, most = 64) {
  const scope = userId === null ? '' : 'WHERE user_id=?', args = userId === null ? [] : [userId];
  const excess = Math.min(most, store.db.prepare(`SELECT count(*) n FROM client_diagnostics ${scope}`).get(...args).n - limit);
  if (excess <= 0) return 0;
  const boundary = store.db.prepare(`SELECT id FROM client_diagnostics ${scope} ORDER BY id DESC LIMIT 1 OFFSET ${clientDiagnosticNewestKept - 1}`).get(...args).id;
  return Number(store.db.prepare(`DELETE FROM client_diagnostics WHERE id IN (SELECT id FROM client_diagnostics ${scope ? scope + ' AND' : 'WHERE'} id<? ORDER BY evidence,id LIMIT ?)`).run(...args, boundary, excess).changes);
}

// Projection, rather than arbitrary log text. Reports remain client assertions,
// never inputs to authorization, display readiness or host identity decisions.
export function safeClientDiagnosticReport(raw) {
  if (!raw || raw.protocol !== 'client-diagnostics-v1' || typeof raw.runId !== 'string' || !/^[a-f0-9]{32}$/.test(raw.runId) || !Number.isSafeInteger(raw.sequence) || raw.sequence < 1 || raw.sequence > 0xffffffff || !Array.isArray(raw.events) || raw.events.length < 1 || raw.events.length > 16) return null;
  if (raw.version !== null && (typeof raw.version !== 'string' || !/^\d{1,6}\.\d{1,6}\.\d{1,6}(?:\+\d{1,10})?$/.test(raw.version))) return null;
  const rows = [];
  for (const row of raw.events) {
    if (!row || !events.has(row.event) || !time(row.time) || !row.values || typeof row.values !== 'object' || Array.isArray(row.values)) return null;
    const safe = { time: row.time, event: row.event, values: {} };
    for (const [key, value] of Object.entries(row.values)) {
      if (Object.hasOwn(diagnosticBounds, key) && (!Number.isSafeInteger(value) || value < 0 || value > diagnosticBounds[key])) continue;
      if (numeric.has(key) && Number.isInteger(value) && value >= 0 && value <= 0xffffffff || boolean.has(key) && typeof value === 'boolean') safe.values[key] = value;
    }
    if (row.desktopId !== undefined) { if (!id(row.desktopId)) return null; safe.desktopId = row.desktopId; }
    for (const key of ['operationId', 'sessionId', 'requestId']) {
      if (row[key] !== undefined) { if (!uuid(row[key])) return null; safe[key] = row[key]; }
    }
    if (row.mode === 'stream') safe.mode = row.mode;
    if (row.code !== undefined) safe.code = codes.has(row.code) ? row.code : 'unknown_failure';
    rows.push(safe);
  }
  const safe = { protocol: 'client-diagnostics-v1', runId: raw.runId, sequence: raw.sequence, version: raw.version, events: rows };
  return Buffer.byteLength(JSON.stringify(safe)) <= 16384 ? safe : null;
}

export function receiveClientDiagnostics(store, user, input, now = Date.now()) {
  const report = safeClientDiagnosticReport(input);
  if (!report) throw new HttpError(400, 'invalid_client_diagnostics');
  for (const row of report.events) {
    if (row.desktopId && !store.assignedDesktop(user.id, row.desktopId)) throw new HttpError(404, 'desktop_not_found');
  }
  store.transaction(() => {
    store.db.prepare('DELETE FROM client_diagnostics WHERE received_at<?').run(now - clientDiagnosticRetentionMs);
    store.db.prepare('INSERT INTO client_diagnostics(user_id,received_at,run_id,sequence,report,evidence) VALUES(?,?,?,?,?,?) ON CONFLICT(user_id,run_id,sequence) DO NOTHING').run(user.id, now, report.runId, report.sequence, JSON.stringify(report), clientDiagnosticEvidence(report));
    // ≤16 KB per batch: 1024 per account (128 filled in ~12 h of use and pushed
    // out the evidence of earlier failures), 4096 in total (≤64 MB).
    boundClientDiagnostics(store, user.id, clientDiagnosticBatchesPerUser);
    boundClientDiagnostics(store, null, clientDiagnosticBatchesTotal);
  });
  return { accepted: true, protocol: report.protocol, runId: report.runId, sequence: report.sequence, streamStatsProtocol, streamStallProtocol };
}

export function clientDiagnostics(store, desktopId = null, now = Date.now()) {
  // Generic application records are visible only in the global administrator
  // view, never attributed to whichever VM happens to be open. A stored batch
  // names its VM as "desktopId":"<id>" (JSON.stringify of the projection), so
  // instr() skips other VMs' batches before they are parsed.
  const shown = 128, vm = desktopId === null ? '' : ' AND instr(d.report,?)>0';
  const args = [now - clientDiagnosticRetentionMs, ...(desktopId === null ? [] : [`"desktopId":${JSON.stringify(desktopId)}`])];
  // Events newest first (batches newest first, each batch's events last first);
  // read lazily, so a poll parses only the batches it shows.
  function* events(evidenceOnly) {
    for (const batch of store.db.prepare(`SELECT d.id,d.received_at,d.report,u.username FROM client_diagnostics d JOIN users u ON u.id=d.user_id WHERE d.received_at>=?${evidenceOnly ? ' AND d.evidence=1' : ''}${vm} ORDER BY d.id DESC`).iterate(...args)) {
      const report = safeClientDiagnosticReport(JSON.parse(batch.report));
      if (!report) continue;
      for (let index = report.events.length - 1; index >= 0; index--) {
        const event = report.events[index];
        if (desktopId !== null && event.desktopId !== desktopId) continue;
        yield { key: batch.id * 16 + index, row: { ...event, account: batch.username, version: report.version, runId: report.runId, sequence: report.sequence, receivedAt: new Date(batch.received_at).toISOString(), source: 'client' } };
      }
    }
  }
  // Select useful events (only evidence batches hold them) before filling the
  // remaining slots with the newest polling, then show them newest first. The
  // fill never reaches past the newest 128 events.
  const selected = new Map();
  for (const entry of events(true)) { if (selected.size === shown) break; if (!routineInventory(entry.row)) selected.set(entry.key, entry.row); }
  for (const entry of events(false)) { if (selected.size === shown) break; if (!selected.has(entry.key)) selected.set(entry.key, entry.row); }
  return { protocol: 'client-diagnostics-v1', retentionDays: 7, events: [...selected].sort((a, b) => b[0] - a[0]).map(([, row]) => row) };
}
