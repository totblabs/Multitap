import 'broker_transport.dart';

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'client_diagnostic_codes.dart';
import 'native_clipboard.dart';
import 'clipboard_limits.dart';
import 'clipboard_progress.dart';
import 'clipboard_pull.dart';
import 'clipboard_pull_observation.dart';
import 'clipboard_policy.dart';
import 'clipboard_sync.dart';
import 'clipboard_pending.dart';
import 'clipboard_window.dart';
import 'product_log.dart';
import 'relay_socket.dart';
import '../l10n/l10n.dart';

export 'clipboard_progress.dart';

typedef ClipboardDialer = Future<WebSocket> Function(Uri url, String token);

/// A received or sent clipboard file name that is one plain Windows file name
/// leaf: the same rule as the native receiver's safeLeaf
/// (native/windows_clipboard/clipboard.cpp), so a name is never accepted here
/// and refused (or treated differently) there. Device names count with the
/// spaces and dots Windows strips from the stem ("CON .txt" is CON); only
/// ASCII letters are folded, as natively.
bool clipboardSafeLeaf(String name, {int max = 240}) {
  if (name.isEmpty ||
      name.length > max ||
      name.endsWith('.') ||
      name.endsWith(' ') ||
      RegExp(r'[<>:"/\\|?*]').hasMatch(name)) {
    return false;
  }
  for (var i = 0; i < name.length; i++) {
    final unit = name.codeUnitAt(i);
    if (unit < 32) return false;
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++i == name.length) return false;
      final low = name.codeUnitAt(i);
      if (low < 0xdc00 || low > 0xdfff) return false;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return false;
    }
  }
  final dot = name.indexOf('.');
  var stem = dot < 0 ? name : name.substring(0, dot);
  while (stem.endsWith(' ') || stem.endsWith('.')) {
    stem = stem.substring(0, stem.length - 1);
  }
  stem = String.fromCharCodes(
    stem.codeUnits.map(
      (unit) => unit >= 0x61 && unit <= 0x7a ? unit - 32 : unit,
    ),
  );
  if (const {
    'CON',
    'PRN',
    'AUX',
    'NUL',
    r'CONIN$',
    r'CONOUT$',
  }.contains(stem)) {
    return false;
  }
  if (stem.length == 4 && (stem.startsWith('COM') || stem.startsWith('LPT'))) {
    final digit = stem.codeUnitAt(3);
    // 1-9, and the superscript digits Windows also treats as device aliases.
    if ((digit >= 0x31 && digit <= 0x39) ||
        digit == 0xb9 ||
        digit == 0xb2 ||
        digit == 0xb3) {
      return false;
    }
  }
  return true;
}

class ClipboardTransferException implements Exception {
  const ClipboardTransferException(this.code);
  final String code;
  @override
  String toString() => code;
}

/// A transfer that was refused before anything was read or sent (busy,
/// unavailable, not permitted): the sync may try the same announcement again.
class ClipboardNotStartedException extends ClipboardTransferException
    implements ClipboardNotStarted {
  const ClipboardNotStartedException(super.code);
}

enum _CapturePhase { preparing, invoked, settled }

class _CaptureOutcome {
  const _CaptureOutcome.notStarted() : capture = null, invoked = false;
  const _CaptureOutcome.returned(this.capture) : invoked = true;
  final ClipboardCapture? capture;
  final bool invoked;
}

class _Transfer {
  _Transfer(
    this.id,
    this.sending,
    this.started, {
    this.automatic = false,
    required this.window,
    required this.ackPolicy,
  }) : at = started,
       stage = sending ? 'capture' : 'begin';
  final String id;
  final bool sending;
  final bool automatic;
  int? sourceSequence;
  ClipboardPendingAdmission? pendingAdmission;
  ClipboardPendingBinding? pendingBinding;
  bool pendingFastAdmission = false;
  bool get pendingBound => pendingBinding != null;
  // Created before reserve's synchronous listener notification.
  final captureOutcome = Completer<_CaptureOutcome>();
  _CapturePhase capturePhase = _CapturePhase.preparing;
  ClipboardSourceReceipt? sourceReceipt;
  Future<void>? finishing, retirement;
  final DateTime started;
  final Stopwatch elapsed = Stopwatch()..start();
  DateTime at;
  String stage;
  String format = '';

  /// The data items (what streams as bytes): the one text / file items, the
  /// rich text parts, the image, or the files of a folder tree (by ordinal).
  List<ClipboardItem> items = const [];

  /// Folder trees: every entry (folders and files), parents first.
  List<ClipboardItem> entries = const [];
  int treeTops = 0,
      treeSkipped = 0,
      treeFileCount = 0,
      treeBytes = 0,
      treeEntryCount = 0;
  final entryNames = <String>{};
  final entryDirs = <String>{};
  // Sending: the sliding window; receiving: when to acknowledge.
  final ClipboardSendWindow window;
  final ClipboardAckPolicy ackPolicy;
  bool pumping = false, pumpAgain = false;
  // Sending: each item's native SHA-256, in order.
  final digests = <String>[];
  // Receiving: what this session last sent, while the incoming manifest and
  // every item digest so far equal it (the peer sending our copy back).
  _Sent? echoOf;
  bool keptOwnCopy = false;
  // Receiving files: the native self-check of the published copy, and the
  // clipboard sequence it was published at.
  ({bool readable, int error, int flags})? publishCheck;
  int? publishedSequence;
  int index = 0, offset = 0;

  /// A paste-pull offer: only the manifest crosses; the bytes follow when pasted.
  bool offer = false;

  /// A deferred offer (manifest-on-paste-v1): published without a list, which the paste asks for.
  bool deferredOffer = false;

  /// A received offer has been handed to the platform (its data object is on the clipboard).
  bool published = false;
  bool cancelled = false;
  String? terminalRequest;

  /// This PC's clipboard was claimed for the content arriving (see [ClipboardSession._armClaim]).
  bool claimed = false;
  Timer? claimTimer;

  /// The native call this transfer was in (see [ClipboardSession.failureOrigin]):
  /// 1 capture, 2 read a chunk, 3 digest a file, 4 begin receiving, 5 append,
  /// 6 finish an item, 7 commit; 0 none yet.
  int step = 0;
  String previousStatus = '';
  String? previousError;
  final done = Completer<void>();
}

/// The content of this session's last completed send: format, item names and
/// sizes, every SHA-256 and the local clipboard sequence it was captured at.
class _Sent {
  _Sent(this.format, this.items, this.digests, this.sequence);
  final String format;
  final List<ClipboardItem> items;
  final List<String> digests;
  final int sequence;
  bool matches(String format, List<ClipboardItem> other) {
    if (format != this.format || other.length != items.length) return false;
    for (var i = 0; i < items.length; i++) {
      if (other[i].name != items[i].name || other[i].size != items[i].size) {
        return false;
      }
    }
    return true;
  }
}

/// Explicit, session-scoped clipboard actions. Connecting never reads either
/// clipboard; native access starts only in sendLocal or a requested receive.
/// This channel can fail independently without interrupting video or input.
class ClipboardSession extends ChangeNotifier {
  ClipboardSession({
    required this.url,
    required this._token,
    required this.sessionId,
    this.policy = const ClipboardPolicy(),
    this.syncSupported = false,
    ClipboardBridge? bridge,
    ClipboardDialer? dialer,
    this.readyTimeout = const Duration(seconds: 15),
    this.idleTimeout = const Duration(seconds: 30),
    this.totalTimeout = const Duration(hours: 6),
    this.publishCheckDelay = const Duration(seconds: 3),
    this.pullLinger = const Duration(milliseconds: 800),
    this.claimAfter = const Duration(milliseconds: 500),
    bool? sharedFolders,
  }) : sharedFolders = sharedFolders ?? !Platform.isAndroid,
       _bridge =
           bridge ??
           NativeClipboardBridge(
             allowedText: policy.textToRemote,
             allowedFiles: policy.filesToRemote,
           ),
       _customDialer = dialer {
    // Closing before the dial completes must not create an unhandled future.
    _ready.future.ignore();
  }

  /// This client has 공유 폴더 to suggest for what the clipboard cannot
  /// carry; Android has none.
  final bool sharedFolders;

  /// Only the Windows adapter can capture and publish rich text, images and
  /// folder trees; any other bridge never announces the formats profile.
  bool get _formatsSupported {
    final bridge = _bridge;
    return bridge is ClipboardFormatsBridge &&
        (bridge as ClipboardFormatsBridge).supportsFormats;
  }

  /// Only the Windows adapter can offer files for pulling and pull them on paste.
  bool get _pullSupported {
    final bridge = _bridge;
    return _formatsSupported &&
        bridge is ClipboardPullBridge &&
        (bridge as ClipboardPullBridge).supportsPull;
  }

  /// The scale revision builds on paste-pull (big copies are offered, not
  /// pushed); only a bridge that says so announces it.
  bool get _scaleSupported {
    final bridge = _bridge;
    return _pullSupported &&
        bridge is ClipboardScaleBridge &&
        (bridge as ClipboardScaleBridge).supportsScale;
  }

  /// The on-paste revision builds on the scale revision (a copy is silent, data moves when pasted); only a bridge that says so announces it.
  bool get _onPasteSupported {
    final bridge = _bridge;
    return _scaleSupported &&
        bridge is ClipboardOnPasteBridge &&
        (bridge as ClipboardOnPasteBridge).supportsOnPaste;
  }

  // The Windows and Android adapters; other bridges keep the old wire. Android
  // then reads a VM copy only once the Host announced it settled, and only as
  // text or an image: the Host refuses files before reading them.
  bool get _settledSupported =>
      _onPasteSupported &&
      _bridge is NativeClipboardBridge &&
      !kIsWeb &&
      (defaultTargetPlatform == TargetPlatform.windows || _bridge.android);
  bool _settledProtocol = false;
  // The complete Windows pending renderer and lazy-offer delegate must both be
  // available. Other adapters and older peers retain their negotiated profile.
  static const _enablePendingIntent = true;
  bool get _intentSupported =>
      _enablePendingIntent &&
      _settledSupported &&
      _bridge is ClipboardPendingBridge &&
      (_bridge as ClipboardPendingBridge).supportsPending;
  bool _intentProtocol = false;
  ClipboardPending? _pendingClipboard;

  /// The relay's pull queue builds on the on-paste revision; only a bridge whose platform asks for files ahead announces it.
  bool get _pullQueueSupported {
    final bridge = _bridge;
    return _onPasteSupported &&
        bridge is ClipboardPullQueueBridge &&
        (bridge as ClipboardPullQueueBridge).supportsPullQueue;
  }

  bool _pullQueue = false;

  /// Deferred manifest builds on the on-paste revision; only a bridge that can walk a copy when it is pasted announces it.
  bool get _deferredSupported {
    final bridge = _bridge;
    return _onPasteSupported && bridge is ClipboardDeferredBridge && (bridge as ClipboardDeferredBridge).supportsDeferred;
  }

  bool _deferredProtocol = false;

  /// Whether the relay confirmed the deferred manifest for this pair (tests, diagnostics).
  bool get deferredManifest => _deferredProtocol;

  /// The on-paste bridge once the relay confirmed the revision for this pair.
  ClipboardOnPasteBridge? get _onPasteBridge =>
      _limits.onPaste && _bridge is ClipboardOnPasteBridge
      ? _bridge as ClipboardOnPasteBridge
      : null;

  /// The capability headers of the clipboard channel: the extended limits, the
  /// formats profile for a bridge that supports it, on top of that the
  /// paste-pull profile and, on top of that, the scale revision.
  @visibleForTesting
  Map<String, String> get dialHeaders => {
    clipboardLimitsHeader: clipboardLimitsHeaderValue,
    if (_formatsSupported) clipboardFormatsHeader: clipboardFormatsHeaderValue,
    if (_pullSupported) clipboardPullHeader: clipboardPullHeaderValue,
    if (_scaleSupported) clipboardScaleHeader: clipboardScaleHeaderValue,
    if (_onPasteSupported) clipboardOnPasteHeader: clipboardOnPasteHeaderValue,
    if (_settledSupported) clipboardSettledHeader: clipboardSettledHeaderValue,
    if (_intentSupported) clipboardIntentHeader: clipboardIntentHeaderValue,
    if (_pullQueueSupported)
      clipboardPullQueueHeader: clipboardPullQueueHeaderValue,
    if (_deferredSupported) clipboardDeferredHeader: clipboardDeferredHeaderValue,
  };

  Future<WebSocket> _dial(Uri url, String token) => connectRelaySocket(
    url,
    token,
    maxPayloadLength: 256 * 1024,
    headers: dialHeaders,
  );
  static final _random = Random.secure();
  static String _id() => base64Url
      .encode(List<int>.generate(18, (_) => _random.nextInt(256)))
      .replaceAll('=', '');
  final Uri url;
  final String sessionId;
  final ClipboardPolicy policy;
  final bool syncSupported;
  ClipboardPolicy _allowed = const ClipboardPolicy();
  ClipboardSync? _sync;
  bool _sharingRequested = false;
  Object? _watchErrorToken;
  String? _watchErrorStatus;
  bool get _recoverWindowsInitialization =>
      !kIsWeb && defaultTargetPlatform == TargetPlatform.windows &&
      _bridge is NativeClipboardBridge && !_bridge.android;
  bool get canSend => _allowed.toRemote;
  bool get canReceive => _allowed.toLocal;
  bool get automatic => _sync?.active == true;

  /// The limits of this channel's peer pair, as the relay advertised them.
  /// Released relays and peers keep the smaller v1 profile.
  ClipboardLimits get limits => _limits;
  ClipboardLimits _limits = ClipboardLimits.v1;
  bool get supportsAutomatic => _sync != null;
  ClipboardPolicy get effectivePolicy => _allowed;

  Future<void> setSharingActive(bool active) async {
    if (_closed) return;
    _sharingRequested = active;
    await _sync?.setActive(active && ready && _allowed.any);
  }

  /// Only a still-current accepted native recapture requests this. Listener
  /// updates retain the old idempotent setSharingActive(bool) contract.
  Future<void> retrySharingInitialization() async {
    if (_closed || !ready || !_sharingRequested ||
        !_recoverWindowsInitialization) {
      return;
    }
    await _sync?.retryInitialization();
  }

  Future<bool> _retryRemoteInitialization() async {
    final pending = _pendingClipboard, sync = _sync;
    final ticket = pending?.failedInitialization ?? pending?.virginInitialization;
    if (_closed || !ready || !_sharingRequested || !_allowed.toLocal ||
        (pending != null && ticket == null) || pulling ||
        _retireFailures.isNotEmpty || _remoteGrant != null) {
      return false;
    }
    final transfer = _transfer;
    final native = _nativeTail, delivery = _pendingDeliveryTail;
    final owned = _ownedWork.toList();
    try {
      await Future.wait<void>([
        native, delivery, ...owned,
        if (transfer != null) transfer.done.future.catchError((Object _) {}),
      ]);
    } catch (_) { return false; }
    if (_closed || !ready || !_sharingRequested || !identical(sync, _sync) ||
        !identical(pending, _pendingClipboard) || busy || pulling ||
        _retireFailures.isNotEmpty || _remoteGrant != null ||
        !identical(native, _nativeTail) ||
        !identical(delivery, _pendingDeliveryTail) || _ownedWork.isNotEmpty) {
      return false;
    }
    if (pending == null) return true;
    if (ticket == null || !pending.initializationCurrent(ticket) ||
        pending.arriving || pending.publicationSequence != null) {
      return false;
    }
    final recovered = await pending.retryInitialization(ticket);
    return recovered && pending.initializationRecoveredFrom(ticket) &&
        !_closed && ready && _sharingRequested && identical(sync, _sync) &&
        identical(pending, _pendingClipboard) && !busy && !pulling &&
        _retireFailures.isEmpty && _remoteGrant == null &&
        identical(native, _nativeTail) &&
        identical(delivery, _pendingDeliveryTail) && _ownedWork.isEmpty;
  }

  void _initializationRecovered(Object token) {
    final sync = _sync;
    if (sync == null) return;
    _clearOwnedWatchError(sync, token, sync.initializationGeneration);
  }

  void _initializationReady() {
    final sync = _sync, presentationStamp = _watchErrorToken;
    if (sync == null || presentationStamp == null || !sync.initializationReady) {
      return;
    }
    // Only the old presentation error is identified here. Fresh current
    // readiness/generation authorizes clearing; no old retry/native token arms.
    _clearOwnedWatchError(sync, presentationStamp, sync.initializationGeneration);
  }

  void _clearOwnedWatchError(ClipboardSync sync, Object token, int generation) {
    unawaited(() async {
      await _waitIdle();
      // Never clear/replace a later transfer error or status, or restore an
      // earlier error snapshot while a transfer is still using it.
      if (_closed || !ready || !_sharingRequested || !identical(sync, _sync) ||
          sync.initializationGeneration != generation || !sync.initializationReady ||
          busy || !identical(token, _watchErrorToken) ||
          error != 'clipboard_watch_unavailable' || status != _watchErrorStatus) {
        return;
      }
      error = null; errorDetail = null;
      _watchErrorToken = null; _watchErrorStatus = null;
      status = l10n.clipboardConnected;
      notifyListeners();
    }().catchError((Object _) {}));
  }

  void _watchRemote(bool enabled) {
    final pending = _pendingClipboard;
    if (_intentProtocol && pending != null) {
      final epoch = pending.epoch;
      if (enabled) _require(pending.active && epoch > 0);
      if (!_closed && ready && epoch > 0) {
        _send({
          'type': 'clipboard.watch',
          'sessionId': sessionId,
          'enabled': enabled,
          'watchEpoch': epoch,
        });
      }
      if (!enabled) unawaited(pending.end().catchError((Object _) {}));
    } else if (!_closed && ready) {
      _send({
        'type': 'clipboard.watch',
        'sessionId': sessionId,
        'enabled': enabled,
      });
    }
  }

  Future<void> _bindPending(_Transfer transfer) async {
    if (!_intentProtocol ||
        transfer.sending ||
        !transfer.automatic ||
        transfer.sourceSequence == null) {
      return;
    }
    if (!{'text', 'richtext', 'image'}.contains(transfer.format) ||
        !_current(transfer)) {
      if (transfer.pendingFastAdmission) {
        throw const ClipboardTransferException('clipboard_changed');
      }
      return;
    }
    final pending = _pendingClipboard;
    final admission =
        transfer.pendingAdmission ??
        pending?.admissionFor(transfer.sourceSequence!);
    transfer.pendingAdmission = admission;
    if (pending == null || admission == null) {
      throw const ClipboardTransferException('clipboard_changed');
    }
    final binding = await pending.bindAdmission(
      admission,
      transfer.id,
      transfer.format,
    );
    if (binding == null) {
      throw const ClipboardTransferException('clipboard_changed');
    }
    if (!_current(transfer)) {
      await pending.failedBinding(binding);
      return;
    }
    transfer.pendingBinding = binding;
  }

  /// Pastes in the VM that are waiting for this PC's copy to arrive (see [preparePaste]).
  int _pasteWaiters = 0;

  /// Somebody is actually waiting for a transfer: the user asked for it (a manual send or receive), a
  /// paste in the VM is held until the copy has arrived, or a paste on this PC is pulling files. A
  /// copy that is only being shared in the background (automatic sync) has nobody waiting: it shows
  /// no card, no window, no taskbar progress and, if it fails, no notice - it is served again when a
  /// paste asks for it.
  bool get userWaiting {
    final transfer = _transfer;
    return _pasteWaiters > 0 ||
        (_pulls?.userWaiting ?? false) ||
        (transfer != null && !transfer.automatic);
  }

  Future<bool> preparePaste() async {
    _pasteWaiters++;
    if (!_closed) notifyListeners();
    try {
      return await (_sync?.preparePaste() ?? Future.value(true));
    } finally {
      _pasteWaiters--;
      if (!_closed) notifyListeners();
    }
  }

  void pasteBlocked() {
    if (_closed) return;
    // The automatic copy of what is being pasted failed quietly (a folder, a
    // locked file...): say why now that the user asks for it.
    final quiet = _quietFailure;
    if (!busy && quiet != null && quiet != 'clipboard_empty') {
      _quietFailure = null;
      error = quiet;
      errorDetail = _quietFailureDetail;
      failureQuiet = false;
      status = _failureStatus(quiet, _quietFailureDetail);
      pasteNotice = status;
      notifyListeners();
      return;
    }
    status = busy
        ? l10n.clipboardBeingTransferredPasteAgain
        : l10n.couldntPrepareContentPasteCopy;
    notifyListeners();
  }

  Future<void> _waitIdle() async {
    final transfer = _transfer;
    if (transfer != null) {
      try {
        await transfer.done.future;
      } catch (_) {}
    }
    await _nativeTail;
  }

  ClipboardPendingAdmission? _remoteGrant;
  bool get _nativePendingLane =>
      _bridge is NativeClipboardBridge &&
      _intentProtocol &&
      _pendingClipboard != null;

  Future<void> _waitRemoteIdle(ClipboardRemoteCandidate candidate) async {
    _remoteGrant = null;
    final transfer = _transfer;
    if (transfer != null) {
      try {
        await transfer.done.future;
      } catch (_) {}
    }
    final pending = _pendingClipboard;
    if (_nativePendingLane &&
        ready &&
        !_closed &&
        !_cleaning &&
        _transfer == null &&
        _sync?.acceptsRemoteCandidate(candidate) == true &&
        pending != null) {
      final admission = await pending.settledAdmission(candidate.sequence);
      if (admission != null &&
          pending.acceptsAdmission(admission, requireLease: true) &&
          _sync?.acceptsRemoteCandidate(candidate) == true &&
          !_closed &&
          !_cleaning &&
          _transfer == null) {
        _remoteGrant = admission;
        return;
      }
    }
    await _waitIdle();
  }

  final String _token;
  final ClipboardBridge _bridge;
  final ClipboardDialer? _customDialer;
  ClipboardDialer get _dialer => _customDialer ?? _dial;
  final Duration readyTimeout, idleTimeout, totalTimeout;

  /// After received files are published, how long until the session checks
  /// (diagnostics only) that they are still what the clipboard holds.
  final Duration publishCheckDelay;

  /// After a pulled file ends, how long the progress notice waits for the next one.
  final Duration pullLinger;

  /// How long a copy from the VM may take before this PC's clipboard is claimed for it.
  final Duration claimAfter;
  Timer? _publishCheck;
  final String _owner = 'clipboard-${_id()}';
  final String _requestPrefix = List.generate(
    16,
    (_) => _random.nextInt(256).toRadixString(16).padLeft(2, '0'),
  ).join();
  int _requestCounter = 0;
  bool _sequenceIds = false;
  String _nextRequestId() {
    if (!_sequenceIds) return _id();
    if (_requestCounter == 0x7fffffffffffffff) {
      throw const ClipboardTransferException('clipboard_session_limit');
    }
    return '$_requestPrefix-${(++_requestCounter).toRadixString(16).padLeft(16, '0')}';
  }

  bool _retiredRequest(String id) {
    if (!_sequenceIds ||
        id.length != 49 ||
        !id.startsWith('$_requestPrefix-') ||
        !RegExp(r'^[0-9a-f]{16}$').hasMatch(id.substring(33))) {
      return false;
    }
    final counter = int.tryParse(id.substring(33), radix: 16);
    return counter != null && counter > 0 && counter <= _requestCounter;
  }

  final _ready = Completer<void>();
  final _retired = <String>{};
  WebSocket? _socket;
  StreamSubscription<dynamic>? _subscription;
  // The socket's two message lanes (see _start).
  Future<void> _controlLane = Future<void>.value(),
      _pullLane = Future<void>.value();
  int _controlBacklog = 0;
  bool _backlogPaused = false;
  static const _controlBacklogLimit = 64;
  Future<void> _nativeTail = Future<void>.value();
  Future<void>? _releasedNativeTail;
  Future<void> _pendingDeliveryTail = Future<void>.value();
  final _ownedWork = <Future<void>>{};
  final _retireFailures = <(String, int), ClipboardSourceReceipt>{};
  Future<void>? _starting, _closing;
  _Transfer? _transfer;
  _Sent? _lastSent;
  Timer? _deadline;
  bool _closed = false, _cleaning = false;

  /// Why the channel ended, recorded in the action 3 diagnostics (error): 1 a
  /// protocol check of this session failed, 2 a pull violation, 3 the handler
  /// threw something unexpected, 4 the socket ended or broke, 6 the relay sent
  /// the end (its reason is the record's code); 0 not attributed.
  int _closeKind = 0;
  bool ready = false;
  bool get busy => _transfer != null || _cleaning;

  /// Files are being pulled (or served to a paste on the other side): a
  /// transfer in the user's eyes, though it never blocks a new copy.
  bool get pulling => _pulls?.active ?? false;

  /// What the transfer notice follows: a transfer or a pull in progress.
  bool get transferring => busy || pulling;
  ClipboardPulls? _pulls;
  bool get closed => _closed;
  bool _reconnectable = false;

  /// A fresh authenticated pair may replace this closed channel once [close]
  /// completes. Never reuse native ownership or resume an interrupted transfer.
  /// Protocol, policy and rejected handshakes require an explicit new session.
  bool get canReconnect => _closed && _reconnectable;
  bool _uncertainRecovery = false;
  int? _inheritedRemoteSequence;

  /// Call before starting a replacement pair, after the old owner has finished
  /// cleanup. This carries metadata only; native owners, requests and content
  /// are never resumed. An uncertain commit requires a fresh observed copy.
  void inheritRecoveryFrom(ClipboardSession previous) {
    if (_starting != null || _closed) {
      throw StateError('Clipboard already started');
    }
    _uncertainRecovery |= previous._uncertainRecovery;
    if (_uncertainRecovery) {
      _inheritedRemoteSequence =
          previous._sync?.receivedRemoteSequence ??
          previous._inheritedRemoteSequence;
    }
  }

  String status = l10n.preparingClipboardConnection;
  String? error;

  /// More about [error] where the code alone is ambiguous (which limit).
  String? errorDetail;

  /// A message for a paste the user attempted that could not be applied; the
  /// session UI shows it once and clears it with [takePasteNotice].
  String? pasteNotice;
  String? takePasteNotice() {
    final notice = pasteNotice;
    pasteNotice = null;
    return notice;
  }

  /// The last failure came from an automatic copy and is expected and harmless:
  /// it is logged but the session UI shows nothing.
  bool failureQuiet = false;
  static const _quietWhenAutomatic = {
    'clipboard_empty',
    'unsupported_clipboard_format',
    'clipboard_policy_denied',
    'clipboard_changed',
    'clipboard_busy',
    'clipboard_cancelled',
  };
  // Not failures: nothing this client can send on the clipboard (no text,
  // image or files), a format it does not share, the administrator's policy,
  // a newer copy, or a cancel.
  static const _normalTransferEnds = {
    'clipboard_empty',
    'unsupported_clipboard_format',
    'clipboard_policy_denied',
    'clipboard_changed',
    'clipboard_cancelled',
  };
  String? _quietFailure, _quietFailureDetail;
  int transferredBytes = 0, totalBytes = 0;

  /// The running transfer as the UI shows it (null while idle). [format] is
  /// empty until the manifest is known; [total] is 0 until then.
  ClipboardProgress? get progress {
    final transfer = _transfer;
    if (transfer == null || transfer.offer) return _pulls?.progress;
    final count = transfer.items.length;
    final tree = transfer.format == 'tree';
    return ClipboardProgress(
      sending: transfer.sending,
      format: transfer.format,
      fileCount: transfer.format == 'files' || tree ? count : 0,
      folderCount: tree ? transfer.entries.length - count : 0,
      fileIndex: count == 0 ? 0 : min(transfer.index, count - 1) + 1,
      transferred: min(transferredBytes, totalBytes),
      total: totalBytes,
      fileName: transfer.format == 'files' || tree
          ? clipboardLeaf(
              count == 0
                  ? null
                  : transfer.items[min(transfer.index, count - 1)].name,
            )
          : null,
    );
  }

  Future<void> start() => _starting ??= _start();
  Future<void> _start() async {
    try {
      if (!brokerRelayAllowed(url) ||
          url.host.isEmpty ||
          url.userInfo.isNotEmpty ||
          url.hasQuery ||
          url.hasFragment ||
          !RegExp(r'^[A-Za-z0-9-]{1,80}$').hasMatch(sessionId) ||
          url.path != '/api/v1/sessions/$sessionId/clipboard') {
        throw const ClipboardTransferException('clipboard_invalid_url');
      }
      final socket = await _dialer(url, _token);
      if (_closed) {
        await socket.close();
        return;
      }
      _socket = socket;
      socket.pingInterval = const Duration(seconds: 10);
      _subscription = socket.listen(
        (message) {
          // Two ordered lanes. A pull's data, end and failure (pull ids) are
          // handed to the platform at once: a paste reading on the clipboard
          // thread waits for exactly them, and a transfer message may itself
          // wait for that thread (in one lane, a paste of a VM copy that
          // has just been published stalls behind its own
          // clipboard.complete until the user cancels it).
          final decoded = _decode(message), id = decoded?['requestId'];
          // The end of a list this side is walking stops the walk now: the walk itself holds the pull lane.
          if (_pulls?.endWalk(decoded) == true) return;
          if (_pulls != null &&
              id is String &&
              clipboardPullIdPattern.hasMatch(id)) {
            final work = _pullLane.then((_) => _receive(message, decoded));
            _pullLane = work.catchError((Object _) {});
            return;
          }
          // Everything else stays strictly serialized. The socket is read on
          // meanwhile (pull data, pings), and paused only if far more is
          // queued than the protocol's windows allow.
          if (++_controlBacklog >= _controlBacklogLimit && !_backlogPaused) {
            _backlogPaused = true;
            _subscription?.pause();
          }
          final work = _controlLane
              .then((_) => _receive(message, decoded))
              .whenComplete(() {
                if (--_controlBacklog < _controlBacklogLimit &&
                    _backlogPaused) {
                  _backlogPaused = false;
                  if (!_closed) _subscription?.resume();
                }
              });
          _controlLane = work.catchError((Object _) {});
        },
        onError: (_) {
          _closeKind = 4;
          unawaited(_transportFailed('clipboard_transport_closed'));
        },
        onDone: () {
          _closeKind = 4;
          unawaited(_transportFailed('clipboard_transport_closed'));
        },
        cancelOnError: true,
      );
      await _ready.future.timeout(readyTimeout);
      if (!_closed) {
        _deadline = Timer.periodic(const Duration(seconds: 1), (_) {
          final transfer = _transfer;
          if (transfer == null) return;
          final now = DateTime.now();
          if (now.difference(transfer.at) > idleTimeout ||
              now.difference(transfer.started) > totalTimeout) {
            unawaited(_expireTransfer(transfer));
          }
        });
      }
    } catch (failure) {
      final code = failure is ClipboardTransferException
          ? failure.code
          : failure is TimeoutException
          ? 'clipboard_pair_timeout'
          : 'clipboard_transport_closed';
      await _transportFailed(
        code,
        // 503: channel-resume-v1's "not now" (the stream is away), not a refusal.
        transientDial:
            failure is SocketException ||
            failure is TimeoutException ||
            (failure is WebSocketException && failure.httpStatusCode == 503),
      );
      throw ClipboardTransferException(code);
    }
  }

  Future<void> _native(Future<void> Function() action) {
    final waiting = Stopwatch()..start();
    final transfer = _transfer;
    final result = _nativeTail.then((_) async {
      final queueMs = waiting.elapsedMilliseconds;
      final running = Stopwatch()..start();
      try {
        await action();
      } finally {
        _noteClipboardDelay(1, queueMs, transfer);
        _noteClipboardDelay(2, running.elapsedMilliseconds, transfer);
      }
    });
    _nativeTail = result.catchError((Object _) {});
    return result;
  }

  Future<T> _own<T>(Future<T> work) {
    final settled = work.then<void>(
      (_) {},
      onError: (Object _, StackTrace _) {},
    );
    _ownedWork.add(settled);
    settled.whenComplete(() => _ownedWork.remove(settled));
    return work;
  }

  ClipboardSourceReceipt? _exactSource(_Transfer transfer) {
    final receipt = transfer.sourceReceipt;
    return _bridge is NativeClipboardBridge &&
            receipt != null &&
            receipt.owner == _owner &&
            receipt.captureSerial > 0 &&
            receipt.captureSerial <= 0x7fffffffffffffff
        ? receipt
        : null;
  }

  void _captureSettled(_Transfer transfer, ClipboardCapture? capture) {
    if (capture != null) transfer.sourceReceipt = capture.sourceReceipt;
    if (!transfer.captureOutcome.isCompleted) {
      final outcome = transfer.capturePhase == _CapturePhase.preparing
          ? const _CaptureOutcome.notStarted()
          : _CaptureOutcome.returned(capture);
      transfer.capturePhase = _CapturePhase.settled;
      transfer.captureOutcome.complete(outcome);
    }
  }

  void _retireSource(_Transfer transfer, ClipboardSourceReceipt receipt) {
    if (transfer.retirement != null) return;
    final key = (receipt.owner, receipt.captureSerial);
    final work =
        _native(() async {
          await (_bridge as ClipboardSourceBridge).retireSource(receipt);
        }).then<void>(
          (_) {
            _retireFailures.remove(key);
          },
          onError: (Object _, StackTrace _) {
            _retireFailures[key] = receipt;
          },
        );
    transfer.retirement = work;
    _own(work);
  }

  bool _bindingCurrent(_Transfer transfer) {
    final binding = transfer.pendingBinding;
    return binding == null ||
        _pendingClipboard?.acceptsBinding(binding) == true;
  }

  Future<void> _deliver(_Transfer transfer, Future<void> Function() action) {
    final binding = transfer.pendingBinding;
    Future<void> guarded() async {
      if (!_current(transfer)) return;
      if (!_bindingCurrent(transfer)) {
        throw const ClipboardTransferException('clipboard_changed');
      }
      await action();
    }

    if (binding == null || !_nativePendingLane) return _native(guarded);
    final work = _pendingDeliveryTail.then((_) => guarded());
    _pendingDeliveryTail = work.catchError((Object _) {});
    return _own(work);
  }

  bool _publicationCurrent(_Transfer transfer) {
    if (!_current(transfer) || !_bindingCurrent(transfer)) return false;
    final bridge = _bridge;
    if (bridge case ClipboardPublicationBridge publisher) {
      return publisher.publicationMatches(
        _owner,
        transfer.id,
        lease: transfer.pendingBinding?.lease,
      );
    }
    return true;
  }

  final Stopwatch _delayWindow = Stopwatch()..start();
  int _delayNotes = 0;
  void _noteClipboardDelay(int phase, int elapsedMs, _Transfer? transfer) {
    if (_closed || elapsedMs < 200) return;
    if (_delayWindow.elapsedMilliseconds >= 60000) {
      _delayWindow.reset();
      _delayNotes = 0;
    }
    if (_delayNotes >= 8) return;
    _delayNotes++;
    try {
      ProductLog.instance.record(LogEvent.clipboard, {
        // 0: remote announcement -> read; 1: Dart native queue;
        // 2: awaited platform operation (including its native STA queue).
        'action': 12,
        'stage': phase,
        'elapsedMs': elapsedMs.clamp(0, 86400000),
        if (transfer != null) 'remote': transfer.sending,
        if (transfer != null) 'auto': transfer.automatic,
      });
    } catch (_) {
      // Observational logging must not change a clipboard operation's result.
    }
  }

  /// A copy made in the VM that is still arriving: once it has taken [claimAfter], this PC's clipboard
  /// is claimed (emptied, owned by this client) so that a paste made in the meantime, in local Explorer,
  /// finds nothing instead of inserting what was on the clipboard before. The arriving content replaces it
  /// when it is published. Best effort: a refusal changes nothing, and a transfer that ends without
  /// publishing leaves the clipboard empty (the failure or the cancel is reported as before).
  void _armClaim(_Transfer transfer) {
    if (transfer.pendingBound) return;
    final bridge = _bridge;
    // (an on-paste pair announces the copy at once and the clipboard is claimed by the offer itself: nothing older can be pasted)
    if (bridge is! ClipboardClaimBridge ||
        _limits.onPaste ||
        transfer.sending ||
        transfer.offer ||
        transfer.echoOf != null ||
        !(transfer.format == 'files' ||
            transfer.format == 'tree' ||
            transfer.format == 'image')) {
      return;
    }
    transfer.claimTimer?.cancel();
    transfer.claimTimer = Timer(claimAfter, () {
      if (!_current(transfer) || transfer.stage != 'data') return;
      unawaited(
        _native(() async {
          if (!_current(transfer) || transfer.stage != 'data') return;
          try {
            final sequence = await (bridge as ClipboardClaimBridge).claim(
              _owner,
              transfer.id,
            );
            if (sequence != null &&
                _current(transfer) &&
                _bindingCurrent(transfer)) {
              transfer.claimed = true;
              // Our own change, not a copy to send.
              _sync?.appliedLocal(sequence);
            }
          } catch (_) {
            // The claim is a courtesy; the transfer goes on without it.
          }
        }).catchError((Object _) {}),
      );
    });
  }

  bool _current(_Transfer transfer) =>
      !_closed && identical(_transfer, transfer) && !transfer.cancelled;
  void _require(bool value) {
    if (!value) {
      throw const ClipboardTransferException('clipboard_invalid_message');
    }
  }

  void _fields(Map<String, dynamic> value, List<String> fields) => _require(
    value.length == fields.length && fields.every(value.containsKey),
  );
  void _send(Map<String, Object?> value) {
    if (_closed || _socket == null) {
      throw const ClipboardTransferException('clipboard_transport_closed');
    }
    _socket!.add(jsonEncode(value));
  }

  static int _notStartedCode(String code) => switch (code) {
    'clipboard_busy' => 1,
    'clipboard_unavailable' => 2,
    'clipboard_policy_denied' => 3,
    _ => 4,
  };

  DateTime? _ignoredAt;
  int _ignoredKey = 0;

  /// action 9: a change was seen and did not become a transfer. stage = why
  /// (1 the remote change arrived while this window was not capturing, 2 the
  /// direction is not permitted, 3 the same VM clipboard sequence was already
  /// read, 4 an automatic transfer could not start, 5 a change announced
  /// within the 2 s grace after the user left the window was accepted (ok),
  /// 6 gave up after retries, 7 a change on this PC replaced a VM copy that
  /// was still arriving);
  /// error (stage 4) = 1 busy, 2 unavailable, 3 not permitted, 4 other;
  /// error (stage 7) = how the PC change was seen: 1 a change notification,
  /// 2 the clipboard as found when the window became active again, 3 the
  /// same before a paste into the VM;
  /// remote = the VM -> this PC direction. One record per reason per second.
  void _logIgnored(int reason, bool remote, [int error = 0]) {
    final now = DateTime.now();
    final key = reason * 4 + (remote ? 1 : 0) + error * 64;
    final last = _ignoredAt;
    if (key == _ignoredKey &&
        last != null &&
        now.difference(last) < const Duration(seconds: 1)) {
      return;
    }
    _ignoredKey = key;
    _ignoredAt = now;
    ProductLog.instance.record(LogEvent.clipboard, {
      'action': 9,
      'ok': ignoredIsOk(reason, error),
      'remote': remote,
      'auto': true,
      'stage': reason,
      if (error != 0) 'error': error,
    });
  }

  /// Only a copy given up (6), or one that could not start for another reason
  /// than the permission (4), is a failure; the other reasons are by design.
  @visibleForTesting
  static bool ignoredIsOk(int reason, int error) =>
      reason != ClipboardIgnored.gaveUp &&
      (reason != ClipboardIgnored.notStarted || error == 3);

  void _logNotStarted(bool sending, bool automatic, int error) {
    // A transfer the user asked for shows its own error; only the automatic
    // ones would otherwise leave no trace.
    if (automatic) _logIgnored(ClipboardIgnored.notStarted, !sending, error);
  }

  _Transfer _reserve(
    bool sending, {
    bool automatic = false,
    ClipboardPendingAdmission? pendingAdmission,
    bool pendingFastAdmission = false,
  }) {
    if (!ready || _closed) {
      throw const ClipboardNotStartedException('clipboard_unavailable');
    }
    if (busy) throw const ClipboardNotStartedException('clipboard_busy');
    if (sending && _retireFailures.isNotEmpty) {
      // A failed scoped cleanup stays owned until close's final owner barrier.
      throw const ClipboardNotStartedException('clipboard_unavailable');
    }
    if (!_sequenceIds && _retired.length >= 256) {
      throw const ClipboardTransferException('clipboard_session_limit');
    }
    final transfer = _Transfer(
      _nextRequestId(),
      sending,
      DateTime.now(),
      automatic: automatic,
      window: ClipboardSendWindow.forLimits(_limits),
      ackPolicy: ClipboardAckPolicy(windowed: _limits.formats),
    );
    transfer
      ..pendingAdmission = pendingAdmission
      ..pendingFastAdmission = pendingFastAdmission
      ..previousStatus = status
      ..previousError = error;
    _transfer = transfer;
    error = null;
    errorDetail = null;
    failureQuiet = false;
    _quietFailure = null;
    transferredBytes = totalBytes = 0;
    status = sending ? l10n.checkingPcsClipboard : l10n.requestingDesktopsClipboard;
    notifyListeners();
    return transfer;
  }

  Future<void> sendLocal({bool automatic = false}) async {
    if (!canSend) {
      throw const ClipboardNotStartedException('clipboard_policy_denied');
    }
    final _Transfer transfer;
    try {
      transfer = _reserve(true, automatic: automatic);
    } on ClipboardNotStartedException catch (failure) {
      _logNotStarted(true, automatic, _notStartedCode(failure.code));
      rethrow;
    }
    // Attach the completion listener before any asynchronous native operation.
    final operation = transfer.done.future;
    _own(_capture(transfer));
    await operation;
  }

  Future<void> _capture(_Transfer transfer) async {
    ClipboardCapture? capture;
    try {
      transfer.step = 1;
      await _native(() async {
        if (_current(transfer)) {
          if (transfer.automatic && !_sharingRequested) {
            throw PlatformException(code: 'clipboard_cancelled');
          }
          final bridge = _bridge;
          // The clipboard of this PC changed: the offers made for the older copy are no longer served, whatever this one turns out to be.
          await _withdrawOwnOffers();
          if (!_current(transfer) ||
              !canSend ||
              (transfer.automatic && !_sharingRequested)) {
            return;
          }
          // With the paste-pull profile a large copy of files and folders is
          // offered (kept natively under this transfer's id) instead of pushed.
          transfer.capturePhase = _CapturePhase.invoked;
          capture = _limits.pull && bridge is ClipboardPullBridge
              ? await (bridge as ClipboardPullBridge).captureWithOffer(
                  _owner,
                  transfer.id,
                  skipOwn: transfer.automatic,
                )
              : await bridge.capture(_owner, skipOwn: transfer.automatic);
          _captureSettled(transfer, capture);
        }
      });
      if (!_current(transfer) || capture == null) return;
      _sync?.capturedLocal(capture!.sequence);
      transfer.sourceSequence = capture!.sequence;
      // The clipboard still holds what this session received from the peer
      // (whatever its sequence number now reads): sending it back would
      // replace the peer's clipboard with its own content and, with a peer
      // that does the same, ping-pong forever.
      if (transfer.automatic && capture!.own) {
        await _finish(transfer, null, echo: true);
        return;
      }
      if (transfer.automatic &&
          _uncertainRecovery &&
          _sync?.allowsAutomaticLocal(capture!.sequence) != true) {
        await _finish(transfer, null, echo: true);
        return;
      }
      if (!_allowed.allows(capture!.format, toRemote: true)) {
        throw PlatformException(code: 'clipboard_policy_denied');
      }
      // A legacy peer may support automatic sharing without virtual files.
      // Copying files still never starts a byte transfer: only a manifest
      // offer is automatic; explicit manual transfers remain available.
      if (transfer.automatic &&
          (capture!.format == 'files' || capture!.format == 'tree') &&
          !capture!.offer) {
        throw PlatformException(code: 'clipboard_policy_denied');
      }
      // Rich text, images and folder trees exist only for a pair that
      // negotiated the formats profile.
      _require(
        _limits.formats ||
            capture!.format == 'text' ||
            capture!.format == 'files',
      );
      // Deferred manifest: files were copied and that is all that goes out now (no list); the VM's paste asks for the list, which
      // is walked then (ClipboardPulls). The pair's limits are checked on that list.
      if (capture!.deferred) {
        _require(
          _deferredProtocol &&
              capture!.offer &&
              capture!.format == 'tree' &&
              capture!.items.isEmpty,
        );
        transfer
          ..format = 'tree'
          ..offer = true
          ..entries = const []
          ..items = const []
          ..treeTops = capture!.tops
          ..treeSkipped = 0
          ..stage = 'commit';
        status = l10n.preparingPaste;
        _pulls?.offerSent(transfer.id, const []);
        _send({
          'type': 'clipboard.offer',
          'requestId': transfer.id,
          'format': 'tree',
          'deferred': true,
        });
        notifyListeners();
        return;
      }
      _validateManifest(capture!.format, capture!.items, tops: capture!.tops);
      transfer.format = capture!.format;
      if (capture!.offer) {
        // Only the manifest crosses now; the peer pulls files when its user pastes.
        _require(
          _limits.pull && (transfer.format == 'tree' || _limits.onPaste),
        );
        // (text, rich text and images are offered too in an on-paste pair: a manifest of items and no entries)
        _require(transfer.format == 'tree' || _limits.onPaste);
        transfer
          ..offer = true
          ..entries = capture!.items
          ..items = [
            for (final item in capture!.items)
              if (!item.isDirectory) item,
          ]
          ..treeTops = capture!.tops
          ..treeSkipped = capture!.skipped
          ..stage = 'commit';
        status = l10n.preparingPaste;
        _pulls?.offerSent(transfer.id, capture!.items, format: transfer.format);
        _send(_manifest(transfer, offer: true));
        if (transfer.format == 'tree') _sendTreeEntries(transfer, offer: true);
        notifyListeners();
        return;
      }
      if (transfer.format == 'tree') {
        transfer.entries = capture!.items;
        transfer.items = [
          for (final item in capture!.items)
            if (!item.isDirectory) item,
        ];
        transfer.treeTops = capture!.tops;
        transfer.treeSkipped = capture!.skipped;
      } else {
        transfer.items = capture!.items;
      }
      totalBytes = transfer.items.fold(0, (sum, item) => sum + item.size);
      transfer.stage = 'data';
      _send(_manifest(transfer));
      if (transfer.format == 'tree') _sendTreeEntries(transfer);
      await _pump(transfer);
    } catch (failure) {
      _captureSettled(transfer, capture);
      await _nativeFailure(transfer, failure);
    } finally {
      _captureSettled(transfer, capture);
    }
  }

  Future<void> receiveRemote({
    bool automatic = false,
    int? expectedSequence,
    bool textImageOnly = true,
  }) async {
    if (expectedSequence != null) {
      _require(
        automatic &&
            _settledProtocol &&
            expectedSequence >= 0 &&
            expectedSequence <= 0xffffffff,
      );
    }
    if (!canReceive) {
      _logNotStarted(false, automatic, 3);
      throw const ClipboardNotStartedException('clipboard_policy_denied');
    }
    if (automatic && !_limits.onPaste && ready && !busy) {
      // Legacy change notifications have no format. Asking an old Host to read
      // can already push file bytes before its manifest can be refused here.
      // Without on-paste negotiation only an explicit receive may request data.
      _logNotStarted(false, automatic, 3);
      throw const ClipboardTransferException('clipboard_policy_denied');
    }
    final _Transfer transfer;
    try {
      final grant = automatic && expectedSequence != null ? _remoteGrant : null;
      _remoteGrant = null;
      if (grant != null &&
          (expectedSequence != grant.sourceSequence ||
              _pendingClipboard?.acceptsAdmission(grant, requireLease: true) !=
                  true)) {
        throw const ClipboardNotStartedException('clipboard_changed');
      }
      transfer = _reserve(
        false,
        automatic: automatic,
        pendingAdmission:
            grant ??
            (expectedSequence == null
                ? null
                : _pendingClipboard?.admissionFor(expectedSequence)),
        pendingFastAdmission: grant != null,
      );
    } on ClipboardNotStartedException catch (failure) {
      _logNotStarted(false, automatic, _notStartedCode(failure.code));
      rethrow;
    }
    final operation = transfer.done.future;
    transfer.sourceSequence = expectedSequence;
    final manualPending =
        !automatic && _intentProtocol && _pendingClipboard != null;
    try {
      if (manualPending) {
        final epoch = _pendingClipboard!.epoch;
        if (epoch > 0) {
          _send({
            'type': 'clipboard.watch',
            'sessionId': sessionId,
            'enabled': false,
            'watchEpoch': epoch,
          });
        }
        await _pendingClipboard!.end();
        await (_bridge as ClipboardPendingBridge).allowManualRequest(
          _owner,
          transfer.id,
        );
      }
      _send({
        'type': 'clipboard.read',
        'requestId': transfer.id,
        if (expectedSequence != null) ...{
          'expectedSequence': expectedSequence,
          'textImageOnly': textImageOnly,
          if (_intentProtocol) 'watchEpoch': _pendingClipboard!.epoch,
        },
      });
    } catch (_) {
      unawaited(_transportFailed('clipboard_transport_closed'));
    }
    try {
      await operation;
    } finally {
      if (manualPending && !_closed && _sync?.active == true) {
        try {
          await _pendingClipboard!.arm();
          if (!_closed && _sync?.active == true) _watchRemote(true);
        } catch (_) {
          if (!_closed) {
            error = 'clipboard_watch_unavailable';
            status = l10n.checkAutomaticClipboardSharingAgain;
            notifyListeners();
          }
        }
      }
    }
  }

  Map<String, Object?> _manifest(_Transfer transfer, {bool offer = false}) => {
    'type': offer ? 'clipboard.offer' : 'clipboard.begin',
    'requestId': transfer.id,
    'format': transfer.format,
    if (transfer.format == 'text') 'size': transfer.items.single.size,
    if (transfer.format == 'files')
      'files': [
        for (final item in transfer.items)
          {'name': item.name, 'size': item.size},
      ],
    if (transfer.format == 'richtext' || transfer.format == 'image')
      'items': [
        for (final item in transfer.items)
          {
            'kind': item.kind,
            'size': item.size,
            if (transfer.format == 'image') 'width': item.width,
            if (transfer.format == 'image') 'height': item.height,
          },
      ],
    if (transfer.format == 'tree') ...{
      'entries': transfer.entries.length,
      'files': transfer.items.length,
      'bytes': transfer.items.fold<int>(0, (sum, item) => sum + item.size),
      'tops': transfer.treeTops,
      'skipped': transfer.treeSkipped,
    },
  };

  /// A folder tree's entries in pages the relay and the peer accept (at most
  /// 200 entries or about 40 KB each).
  void _sendTreeEntries(_Transfer transfer, {bool offer = false}) {
    var start = 0;
    var page = <Map<String, Object?>>[];
    var size = 0;
    void flush() {
      if (page.isEmpty) return;
      _send({
        'type': 'clipboard.entries',
        'requestId': transfer.id,
        'start': start,
        'entries': page,
      });
      start += page.length;
      page = [];
      size = 0;
    }

    for (final entry in transfer.entries) {
      final map = <String, Object?>{
        'path': entry.name,
        'kind': entry.isDirectory ? 'dir' : 'file',
        if (!entry.isDirectory) 'size': entry.size,
        'attrs': entry.attributes,
        if (offer) 'mtime': entry.mtime ?? 0,
      };
      page.add(map);
      size += (entry.name?.length ?? 0) * 3 + 64;
      if (page.length >= 200 || size > 40000) flush();
    }
    flush();
  }

  /// A size, count or length beyond this pair's limits is a limits failure
  /// with its own message, never a protocol error or a generic read failure.
  Never _limitsExceeded(String detail) => throw PlatformException(
    code: 'clipboard_limits_exceeded',
    message: detail,
  );

  void _validateManifest(
    String format,
    List<ClipboardItem> items, {
    int tops = 0,
  }) {
    final limits = _limits;
    if (format == 'text') {
      _require(items.length == 1 && items.single.name == null);
      _require(items.single.size >= 0);
      final units = items.single.units;
      if (items.single.size > limits.maxTextBytes ||
          (units != null && units > limits.maxTextUnits)) {
        _limitsExceeded('text');
      }
    } else if (format == 'files') {
      _require(items.isNotEmpty);
      if (items.length > limits.maxFiles) _limitsExceeded('count');
      final names = <String>{};
      var sum = 0;
      for (final item in items) {
        final name = item.name;
        _require(name != null && _safeName(name));
        _require(names.add(name!.toUpperCase()));
        _require(item.size >= 0);
        if (item.size > limits.maxFileBytes) _limitsExceeded('file');
        sum += item.size;
        if (sum > limits.maxTotalBytes) _limitsExceeded('total');
      }
    } else if (format == 'richtext') {
      // Plain text always comes first; HTML and RTF follow in that order.
      _require(
        limits.formats &&
            items.isNotEmpty &&
            items.length <= 3 &&
            items.first.kind == 'text',
      );
      const order = ['text', 'html', 'rtf'];
      var previous = -1;
      for (final item in items) {
        final rank = order.indexOf(item.kind ?? '');
        _require(rank > previous && item.size >= (rank == 0 ? 0 : 1));
        previous = rank;
        if (rank == 0) {
          final units = item.units;
          if (item.size > limits.maxTextBytes ||
              (units != null && units > limits.maxTextUnits)) {
            _limitsExceeded('text');
          }
        } else if (item.size > limits.maxRichBytes) {
          _limitsExceeded('text');
        }
      }
    } else if (format == 'image') {
      _require(
        limits.formats && items.length == 1 && items.single.kind == 'png',
      );
      final image = items.single, width = image.width, height = image.height;
      _require(width != null && height != null && width >= 1 && height >= 1);
      _require(width! <= 65535 && height! <= 65535 && image.size >= 1);
      if (width * height! > limits.maxImagePixels ||
          image.size > limits.maxImageBytes) {
        _limitsExceeded('image');
      }
    } else if (format == 'tree') {
      _validateTree(items, tops);
    } else {
      _require(false);
    }
  }

  /// A folder tree: parents before children, every component a plain leaf, no
  /// duplicates (case-insensitively), depth / path / count / size bounds.
  void _validateTree(List<ClipboardItem> entries, int tops) {
    final limits = _limits;
    _require(limits.formats && entries.isNotEmpty);
    if (entries.length > limits.maxEntries) _limitsExceeded('entries');
    final names = <String>{}, dirs = <String>{};
    var counted = 0, sum = 0, longest = 0;
    for (final entry in entries) {
      final path = entry.name;
      _require(path != null && path.isNotEmpty);
      final parts = path!.split('/');
      _require(
        parts.every(
          (part) => clipboardSafeLeaf(part, max: limits.maxComponentChars),
        ),
      );
      if (parts.length > limits.maxDepth) _limitsExceeded('depth');
      // Reported after the whole list, naming the longest path: the profile's
      // limit, and never more than Explorer can create at a paste destination.
      if (path.length > longest) longest = path.length;
      final key = parts.map((part) => part.toUpperCase()).join('/');
      if (parts.length > 1) {
        _require(dirs.contains(key.substring(0, key.lastIndexOf('/'))));
      } else {
        counted++;
      }
      _require(names.add(key));
      _require(entry.attributes >= 0 && (entry.attributes & ~0x27) == 0);
      if (entry.isDirectory) {
        _require(entry.size == 0);
        dirs.add(key);
      } else {
        _require(entry.kind == 'file' && entry.size >= 0);
        if (entry.size > limits.maxFileBytes) _limitsExceeded('file');
        sum += entry.size;
        if (sum > limits.maxTotalBytes) _limitsExceeded('total');
      }
    }
    if (longest > limits.pastePathChars) _limitsExceeded('path:$longest');
    _require(counted == tops && counted >= 1);
    if (counted > limits.maxFiles) _limitsExceeded('count');
  }

  bool _safeName(String name) => clipboardSafeLeaf(name);

  /// Sends as many units (data chunks, item ends) as the window allows, then
  /// the commit once every unit has been acknowledged. Runs again whenever an
  /// acknowledgement frees window; only one run is active at a time.
  Future<void> _pump(_Transfer transfer) async {
    if (transfer.pumping) {
      transfer.pumpAgain = true;
      return;
    }
    transfer.pumping = true;
    try {
      do {
        transfer.pumpAgain = false;
        await _sendUnits(transfer);
      } while (transfer.pumpAgain && _current(transfer));
    } finally {
      transfer.pumping = false;
    }
  }

  Future<void> _sendUnits(_Transfer transfer) async {
    while (_current(transfer) && transfer.stage == 'data') {
      if (transfer.index == transfer.items.length) {
        if (!transfer.window.isEmpty) return;
        transfer.stage = 'commit';
        status = l10n.checkingWasAppliedDesktop;
        _send({'type': 'clipboard.commit', 'requestId': transfer.id});
        notifyListeners();
        return;
      }
      final item = transfer.items[transfer.index];
      if (transfer.offset == item.size) {
        if (!transfer.window.canSendItemEnd) return;
        final index = transfer.index;
        String? digest;
        transfer.step = 3;
        await _native(() async {
          if (_current(transfer)) {
            final receipt = _exactSource(transfer);
            digest = receipt == null
                ? await _bridge.sourceDigest(_owner, index)
                : await (_bridge as ClipboardSourceBridge).sourceDigestScoped(
                    receipt,
                    index,
                  );
          }
        });
        if (!_current(transfer)) return;
        _require(digest != null && RegExp(r'^[a-f0-9]{64}$').hasMatch(digest!));
        transfer.digests.add(digest!);
        _send({
          'type': 'clipboard.item.end',
          'requestId': transfer.id,
          'index': index,
          'sha256': digest,
        });
        transfer.window.sentItemEnd(index);
        transfer.index++;
        transfer.offset = 0;
      } else {
        final count = min(_limits.chunkBytes, item.size - transfer.offset);
        if (!transfer.window.canSendChunk(count)) return;
        final index = transfer.index, offset = transfer.offset;
        Uint8List? bytes;
        transfer.step = 2;
        await _native(() async {
          if (_current(transfer)) {
            final receipt = _exactSource(transfer);
            bytes = receipt == null
                ? await _bridge.sourceChunk(_owner, index, offset, count)
                : await (_bridge as ClipboardSourceBridge).sourceChunkScoped(
                    receipt,
                    index,
                    offset,
                    count,
                  );
          }
        });
        if (!_current(transfer)) return;
        _require(bytes != null && bytes!.length == count);
        _send({
          'type': 'clipboard.chunk',
          'requestId': transfer.id,
          'index': index,
          'offset': offset,
          'data': base64Encode(bytes!),
        });
        transfer.offset += count;
        transfer.window.sentChunk(index, transfer.offset, count);
      }
      status = l10n.sendingClipboard;
      notifyListeners();
    }
  }

  /// The message as a JSON object within the size bound; null when it is not
  /// one (_receive then reports it as before).
  static Map<String, dynamic>? _decode(dynamic message) {
    if (message is! String || message.length > 256 * 1024) return null;
    try {
      final decoded = jsonDecode(message);
      return decoded is Map<String, dynamic> ? decoded : null;
    } catch (_) {
      return null;
    }
  }

  Future<void> _receive(
    dynamic message, [
    Map<String, dynamic>? decoded,
  ]) async {
    if (_closed) return;
    _Transfer? receivingTransfer;
    try {
      final Map<String, dynamic> value;
      if (decoded != null) {
        value = decoded;
      } else {
        _require(message is String && message.length <= 256 * 1024);
        final parsed = jsonDecode(message as String);
        _require(parsed is Map<String, dynamic>);
        value = parsed as Map<String, dynamic>;
      }
      if (value['type'] == 'clipboard.ready') {
        _fields(value, [
          'type',
          'sessionId',
          'protocol',
          'limits',
          'clipboardPolicy',
          'clipboardSync',
          if (value.containsKey('clipboardSettled')) 'clipboardSettled',
          if (value.containsKey('clipboardIntent')) 'clipboardIntent',
          if (value.containsKey('clipboardPullQueue')) 'clipboardPullQueue',
          if (value.containsKey('clipboardDeferred')) 'clipboardDeferred',
        ]);
        _require(
          !ready &&
              value['sessionId'] == sessionId &&
              value['protocol'] == 'clipboard-v1',
        );
        // The relay's policy of this pair is the administrator's setting now (it checks every message against it): an
        // administrator who allows files while a stream runs makes the relay end the pair, and the next pair's ready
        // carries the new setting. The stream's start (`policy`) is only a snapshot: intersected with it, a widened
        // setting never took effect until the stream was opened again.
        _allowed = ClipboardPolicy.fromJson(value['clipboardPolicy']);
        final native = _bridge;
        if (native is NativeClipboardBridge) {
          native.allowFormats(
            text: _allowed.textToRemote,
            files: _allowed.filesToRemote,
          );
        }
        _require(value['clipboardSync'] is bool);
        _sequenceIds = syncSupported && value['clipboardSync'] == true;
        final negotiated = ClipboardLimits.parse(
          value['limits'],
          chunkBytes: 32768,
        );
        _require(negotiated != null);
        // Only a bridge that announced the formats profile is told about it.
        _require(!negotiated!.formats || _formatsSupported);
        // The paste-pull profile likewise only for a bridge that announced it.
        _require(!negotiated.pull || _pullSupported);
        // And the scale revision for a bridge that announced that.
        _require(!negotiated.scale || _scaleSupported);
        _require(!negotiated.onPaste || _onPasteSupported);
        _settledProtocol = value.containsKey('clipboardSettled');
        _require(
          !_settledProtocol ||
              (_settledSupported &&
                  negotiated.onPaste &&
                  _sequenceIds &&
                  value['clipboardSettled'] == clipboardSettledProtocol),
        );
        _intentProtocol = value.containsKey('clipboardIntent');
        _require(
          !_intentProtocol ||
              (_intentSupported &&
                  _settledProtocol &&
                  value['clipboardIntent'] == clipboardIntentProtocol),
        );
        if (_intentProtocol) {
          _pendingClipboard = ClipboardPending(
            _owner,
            native as ClipboardPendingBridge,
          );
        }
        _pullQueue = value.containsKey('clipboardPullQueue');
        _require(
          !_pullQueue ||
              (_pullQueueSupported &&
                  negotiated.onPaste &&
                  value['clipboardPullQueue'] == clipboardPullQueueProtocol),
        );
        if (native is ClipboardPullQueueBridge) {
          (native as ClipboardPullQueueBridge).pullDepth = _pullQueue
              ? clipboardPullQueueDepth
              : 1;
        }
        _deferredProtocol = value.containsKey('clipboardDeferred');
        _require(
          !_deferredProtocol ||
              (_deferredSupported &&
                  negotiated.onPaste &&
                  value['clipboardDeferred'] == clipboardDeferredProtocol),
        );
        if (native is ClipboardDeferredBridge) {
          (native as ClipboardDeferredBridge).deferred = _deferredProtocol;
        }
        _limits = negotiated;
        if (native is ClipboardFormatsBridge) {
          (native as ClipboardFormatsBridge).formats = negotiated.formats;
        }
        if (native is ClipboardOnPasteBridge) {
          (native as ClipboardOnPasteBridge).onPaste = negotiated.onPaste;
        }
        if (native is ClipboardScaleBridge) {
          (native as ClipboardScaleBridge)
            ..scale = negotiated.scale
            ..richBytes = negotiated.formats
                ? negotiated.maxRichBytes
                : 8 * 1024 * 1024;
        }
        if (negotiated.pull && native is ClipboardPullBridge) {
          final pullBridge = native as ClipboardPullBridge;
          pullBridge.pull = true;
          _pulls = ClipboardPulls(
            owner: _owner,
            bridge: pullBridge,
            send: _send,
            limits: () => _limits,
            idPrefix: 'pullc',
            changed: () {
              if (!_closed) notifyListeners();
            },
            started: () {
              error = null;
              errorDetail = null;
              failureQuiet = false;
              status = l10n.gettingClipboardFiles;
            },
            failed: _pullFailed,
            observed: (value) {
              if (_closed) return;
              final values = <String, Object>{
                'action': value.sending ? 13 : 12,
                'stage': value.stage,
                // Stage 5 (cancelled) is the user's own choice; 6 failed.
                'ok': value.stage < 6,
                'remote': value.sending,
                'requests': value.requests,
                'failedRequests': value.failedRequests,
                'elapsedMs': value.elapsedMs,
                if (value.diagnosticDetail != 0)
                  'error': value.diagnosticDetail,
              };
              if (value.diagnosticCode == null) {
                ProductLog.instance.record(LogEvent.clipboard, values);
              } else {
                ProductLog.instance.recordFailure(
                  LogEvent.clipboard,
                  value.diagnosticCode!,
                  values,
                );
              }
            },
            failedServing: (code, {required record}) =>
                _pullFailed(code, sending: true, record: record),
            mayServe: (format) => format == 'text' || format == 'richtext'
                ? _allowed.textToRemote
                : _allowed.filesToRemote,
            linger: pullLinger,
            onPaste: _onPasteBridge,
            depth: _pullQueue ? clipboardPullQueueDepth : 1,
            deferred: _deferredProtocol ? native as ClipboardDeferredBridge : null,
            validateTree: (entries, tops) => _validateManifest('tree', entries, tops: tops),
            wireReason: (code, detail) => _wireReason(code, detail),
          )..start();
        }
        ready = true;
        final bridge = _bridge;
        // Android shares text only (files never move to or from a phone): a
        // files-only grant neither reads this device's clipboard nor asks for
        // a VM copy. The channel itself keeps the relay's whole policy.
        final shared = bridge is NativeClipboardBridge && bridge.android
            ? ClipboardPolicy(
                textToRemote: _allowed.textToRemote,
                textToLocal: _allowed.textToLocal,
              )
            : _allowed;
        if (syncSupported &&
            value['clipboardSync'] == true &&
            shared.any &&
            bridge is ClipboardObservationBridge) {
          _sync = ClipboardSync(
            owner: _owner,
            bridge: bridge as ClipboardObservationBridge,
            policy: shared,
            observeInitialLocalOnly: _uncertainRecovery,
            initializationBackoffs: _recoverWindowsInitialization
                ? const [Duration(milliseconds: 100), Duration(milliseconds: 250), Duration(milliseconds: 500)]
                : const [],
            retryRemoteInitialization: _recoverWindowsInitialization
                ? _retryRemoteInitialization : null,
            onInitializationRecovered: _recoverWindowsInitialization
                ? _initializationRecovered : null,
            onInitializationReady: _recoverWindowsInitialization
                ? _initializationReady : null,
            receivedRemoteSequence: _inheritedRemoteSequence,
            sendLocal: () => sendLocal(automatic: true),
            receiveRemote: () => receiveRemote(automatic: true),
            // Such a Host announces a copy only after 250 ms of quiet; another
            // 300 ms here only delayed a file copy.
            remoteDebounced: _settledProtocol,
            waitRemoteIdle: _waitRemoteIdle,
            receiveSettledRemote: _settledProtocol
                ? (sequence) =>
                      receiveRemote(automatic: true, expectedSequence: sequence)
                : null,
            receiveObservedRemote: _intentProtocol
                ? (sequence, settled) => receiveRemote(
                    automatic: true,
                    expectedSequence: sequence,
                    textImageOnly: settled,
                  )
                : null,
            armRemote: _pendingClipboard?.arm,
            onLocalChange: (via) {
              final transfer = _transfer;
              if ((transfer != null &&
                      transfer.automatic &&
                      !transfer.sending) ||
                  _pendingClipboard?.arriving == true) {
                _logIgnored(ClipboardIgnored.superseded, true, via);
              }
            },
            onLocalCopy: () {
              unawaited(
                (_pendingClipboard?.localCopy() ?? Future<void>.value())
                    .catchError((Object _) {}),
              );
            },
            waitIdle: _waitIdle,
            watchRemote: _watchRemote,
            onIgnored: (reason, remote) => _logIgnored(reason, remote),
            onRemoteDelay: (elapsedMs) =>
                _noteClipboardDelay(0, elapsedMs, null),
            cancelOlder: _supersede,
            onFailure: () {
              final ownedError = error == 'clipboard_watch_unavailable' &&
                  _watchErrorToken != null && status == _watchErrorStatus;
              if (!_recoverWindowsInitialization || error == null || ownedError) {
                error = 'clipboard_watch_unavailable';
                status = l10n.couldntPrepareAutomaticClipboardSharing;
                _watchErrorToken = _sync?.initializationFailureToken;
                _watchErrorStatus = status;
              }
              ProductLog.instance.recordFailure(
                LogEvent.clipboard,
                'clipboard_watch_unavailable',
                {'action': 4, 'ok': false,
                  if (_recoverWindowsInitialization) ...{
                    'stage': _sync?.initializationFailureStage ?? 0,
                    // Why the step failed (ClipboardSync.failureCause).
                    'error': _sync?.initializationFailureCause ?? 0,
                  }},
              );
              notifyListeners();
            },
          );
          unawaited(setSharingActive(_sharingRequested));
        }
        // action 1: the channel is ready; auto = Ctrl+C/Ctrl+V sharing set
        // up, available = the broker allows some direction, stage = the
        // effective permissions (1 text to VM, 2 text to PC, 4 files to VM,
        // 8 files to PC).
        ProductLog.instance.record(LogEvent.clipboard, {
          'action': 1,
          'ok': true,
          'auto': _sync != null,
          'available': _allowed.any,
          'stage':
              (_allowed.textToRemote ? 1 : 0) |
              (_allowed.textToLocal ? 2 : 0) |
              (_allowed.filesToRemote ? 4 : 0) |
              (_allowed.filesToLocal ? 8 : 0),
        });
        status = l10n.clipboardConnected;
        _ready.complete();
        notifyListeners();
        return;
      }
      if (value['type'] == 'clipboard.intent') {
        _fields(value, ['type', 'sessionId', 'sequence', 'kind', 'watchEpoch']);
        final sequence = value['sequence'],
            epoch = value['watchEpoch'],
            kind = value['kind'];
        _require(
          _intentProtocol &&
              ready &&
              value['sessionId'] == sessionId &&
              sequence is int &&
              sequence >= 0 &&
              sequence <= 0xffffffff &&
              epoch is int &&
              epoch > 0 &&
              epoch <= 0xffffffff &&
              {'text', 'richtext', 'image', 'other'}.contains(kind),
        );
        final pending = _pendingClipboard!;
        final sourceEpoch = epoch as int, sourceSequence = sequence as int;
        _require(sourceEpoch <= pending.epoch);
        if (!pending.accepts(sourceEpoch)) return;
        _sync?.remoteIntent(sourceSequence);
        // Text and images get a placeholder that waits for them. Anything else
        // (files above all) has none: the VM's previous copy is stale from now
        // on, so it ends here and leaves this PC's clipboard now, instead of
        // when the Host withdraws it after its copy settles (250 ms later). A
        // paste made meanwhile never takes the previous copy's files.
        if (kind == 'other') _retirePeerOffers();
        await pending.source(sourceSequence, kind as String, sourceEpoch);
        final published = pending.publicationSequence;
        if (pending.accepts(sourceEpoch) && published != null) {
          _sync?.appliedLocal(published);
        }
        return;
      }
      if (value['type'] == 'clipboard.changed') {
        final settled = value.containsKey('settled');
        _fields(value, [
          'type',
          'sessionId',
          'sequence',
          if (settled) 'settled',
          if (_intentProtocol) 'watchEpoch',
        ]);
        _require(
          !settled ||
              (_settledProtocol &&
                  value['settled'] == clipboardSettledProtocol),
        );
        final sequence = value['sequence'];
        _require(
          ready &&
              value['sessionId'] == sessionId &&
              sequence is int &&
              sequence >= 0 &&
              sequence <= 0xffffffff,
        );
        if (_intentProtocol) {
          final epoch = value['watchEpoch'];
          _require(
            epoch is int && epoch > 0 && epoch <= _pendingClipboard!.epoch,
          );
          if (!_pendingClipboard!.accepts(epoch as int) ||
              _pendingClipboard!.refused(sequence as int)) {
            return;
          }
        }
        _sync?.remoteChanged(sequence as int, settled: settled);
        return;
      }
      if (value['type'] == 'clipboard.error' &&
          !value.containsKey('requestId')) {
        _fields(value, ['type', 'reason']);
        final (code, detail) = specificLimit(_safeReason(value['reason']));
        _closeKind = 6;
        await _transportFailed(code, detail: detail);
        return;
      }
      _require(ready && value['requestId'] is String);
      // On-paste revision, outside any transfer: an application in the VM waits for data of our offer; the peer withdrew an offer.
      if (value['type'] == 'clipboard.paste.waiting') {
        _fields(value, ['type', 'requestId', 'active']);
        _require(_limits.onPaste && value['active'] is bool);
        _pulls?.pasteWaiting(value['active'] as bool);
        if (!_closed) notifyListeners();
        return;
      }
      if (value['type'] == 'clipboard.offer.withdraw') {
        _fields(value, ['type', 'requestId']);
        _require(_limits.onPaste);
        await _withdrawnByPeer(value['requestId'] as String);
        if (!_closed) notifyListeners();
        return;
      }
      // A pull (the peer pasted our offer, or we paste its offer) has its own ids and state,
      // alongside whatever transfer is running.
      if (_pulls != null &&
          clipboardPullIdPattern.hasMatch(value['requestId'] as String)) {
        try {
          await _pulls!.handle(value);
        } on ClipboardPullViolation {
          _closeKind = 2;
          throw const ClipboardTransferException('clipboard_invalid_message');
        }
        if (!_closed) notifyListeners();
        return;
      }
      // Retired callbacks must never apply bytes or finish a newer transfer.
      if (_retired.contains(value['requestId']) ||
          (value['requestId'] != _transfer?.id &&
              _retiredRequest(value['requestId'] as String))) {
        return;
      }
      final transfer = _transfer;
      _require(transfer != null && value['requestId'] == transfer.id);
      final active = transfer!;
      receivingTransfer = active;
      active.at = DateTime.now();
      if (value['type'] == 'clipboard.cancel') {
        _fields(value, ['type', 'requestId']);
        await _finish(active, 'clipboard_cancelled', origin: 3);
        return;
      }
      if (value['type'] == 'clipboard.error') {
        _fields(value, ['type', 'requestId', 'reason']);
        final (code, detail) = specificLimit(_safeReason(value['reason']));
        await _finish(active, code, detail: detail, origin: 3);
        return;
      }
      if (active.cancelled && value['type'] != 'clipboard.complete') return;
      switch (value['type']) {
        case 'clipboard.begin':
          _require(!active.sending && active.stage == 'begin');
          final format = value['format'];
          _require(
            format is String && _allowed.allows(format, toRemote: false),
          );
          if (active.automatic && (format == 'files' || format == 'tree')) {
            // Old Hosts can answer a change notification by eagerly pushing
            // files. Refuse before native staging or receiving any bytes.
            _send({'type': 'clipboard.cancel', 'requestId': active.id});
            await _finish(active, 'clipboard_policy_denied', origin: 3);
            return;
          }
          final items = <ClipboardItem>[];
          if (format == 'text') {
            _fields(value, ['type', 'requestId', 'format', 'size']);
            _require(value['size'] is int);
            items.add(ClipboardItem(size: value['size'] as int));
          } else if (format == 'files') {
            _fields(value, ['type', 'requestId', 'format', 'files']);
            _require(value['files'] is List);
            for (final item in value['files'] as List) {
              _require(item is Map<String, dynamic>);
              final file = item as Map<String, dynamic>;
              _fields(file, ['name', 'size']);
              _require(file['name'] is String && file['size'] is int);
              items.add(
                ClipboardItem(
                  size: file['size'] as int,
                  name: file['name'] as String,
                ),
              );
            }
          } else if (format == 'richtext' || format == 'image') {
            _require(_limits.formats);
            _fields(value, ['type', 'requestId', 'format', 'items']);
            _require(value['items'] is List);
            for (final item in value['items'] as List) {
              _require(item is Map<String, dynamic>);
              final part = item as Map<String, dynamic>;
              _fields(
                part,
                format == 'image'
                    ? ['kind', 'size', 'width', 'height']
                    : ['kind', 'size'],
              );
              _require(part['kind'] is String && part['size'] is int);
              if (format == 'image') {
                _require(part['width'] is int && part['height'] is int);
              }
              items.add(
                ClipboardItem(
                  size: part['size'] as int,
                  kind: part['kind'] as String,
                  width: part['width'] as int?,
                  height: part['height'] as int?,
                ),
              );
            }
          } else if (format == 'tree') {
            // The folder tree is described by clipboard.entries pages next.
            _beginTree(active, value);
            status = l10n.gettingClipboard;
            break;
          } else {
            _require(false);
          }
          _validateManifest(format as String, items);
          active.format = format;
          active.items = items;
          // A newer copy replaces what an application may be blocked on.
          await _onPasteBridge?.pullSupersede(_owner);
          final sent = _lastSent;
          active.echoOf = sent != null && sent.matches(format, items)
              ? sent
              : null;
          totalBytes = items.fold(0, (sum, item) => sum + item.size);
          active.step = 4;
          await _bindPending(active);
          await _deliver(active, () async {
            if (_current(active)) {
              await _bridge.begin(_owner, active.id, format, items);
            }
          });
          if (!_current(active)) return;
          active.stage = 'data';
          status = l10n.gettingClipboard;
          _armClaim(active);
        case 'clipboard.offer':
          // Files and folders announced for pulling: the same manifest as a tree, but
          // no data follows now. The virtual files are published once the entries
          // are in, and the bytes are pulled when the user pastes.
          _require(
            !active.sending &&
                active.stage == 'begin' &&
                _limits.pull &&
                _pulls != null,
          );
          final offerFormat = value['format'];
          _require(
            offerFormat is String &&
                _allowed.allows(offerFormat, toRemote: false),
          );
          // A newer copy replaces what an application may be blocked on (it would hold up the platform thread we publish on).
          await _onPasteBridge?.pullSupersede(_owner);
          // A deferred copy: published at once without a list; the paste asks the Host for it (ClipboardPulls).
          if (value.containsKey('deferred')) {
            _fields(value, ['type', 'requestId', 'format', 'deferred']);
            _require(
              _deferredProtocol &&
                  offerFormat == 'tree' &&
                  value['deferred'] == true,
            );
            active
              ..format = 'tree'
              ..offer = true
              ..deferredOffer = true
              ..items = const []
              ..entries = const [];
            totalBytes = 0;
            await _publishOffer(active);
            break;
          }
          if (offerFormat == 'tree') {
            _beginTree(active, value, offer: true);
            status = l10n.preparingPaste;
            break;
          }
          // Text, rich text and images (on-paste revision): the manifest is the whole offer; the data object is published at once.
          _require(
            _limits.onPaste &&
                (offerFormat == 'text' ||
                    offerFormat == 'richtext' ||
                    offerFormat == 'image'),
          );
          final contentItems = _contentItems(offerFormat as String, value);
          _validateManifest(offerFormat, contentItems);
          active
            ..format = offerFormat
            ..offer = true
            ..items = contentItems
            ..entries = contentItems;
          totalBytes = contentItems.fold(0, (sum, item) => sum + item.size);
          await _publishOffer(active);
        case 'clipboard.entries':
          _fields(value, ['type', 'requestId', 'start', 'entries']);
          _require(
            !active.sending &&
                active.stage == 'entries' &&
                value['start'] == active.entries.length &&
                value['entries'] is List,
          );
          final page = value['entries'] as List;
          _require(
            page.isNotEmpty &&
                page.length <= 256 &&
                active.entries.length + page.length <= active.treeEntryCount,
          );
          for (final raw in page) {
            _require(raw is Map<String, dynamic>);
            final entry = raw as Map<String, dynamic>;
            _require(entry['kind'] == 'file' || entry['kind'] == 'dir');
            final isFile = entry['kind'] == 'file';
            _fields(entry, [
              'path',
              'kind',
              if (isFile) 'size',
              'attrs',
              if (active.offer) 'mtime',
            ]);
            _require(
              entry['path'] is String &&
                  entry['attrs'] is int &&
                  (!isFile || entry['size'] is int) &&
                  (!active.offer ||
                      (entry['mtime'] is int && (entry['mtime'] as int) >= 0)),
            );
            active.entries.add(
              ClipboardItem(
                size: isFile ? entry['size'] as int : 0,
                name: entry['path'] as String,
                kind: entry['kind'] as String,
                attributes: entry['attrs'] as int,
                mtime: active.offer ? entry['mtime'] as int : null,
              ),
            );
          }
          if (active.entries.length < active.treeEntryCount) break;
          // Complete: check the whole tree against the announced counts.
          _validateManifest('tree', active.entries, tops: active.treeTops);
          final files = [
            for (final entry in active.entries)
              if (!entry.isDirectory) entry,
          ];
          _require(
            files.length == active.treeFileCount &&
                files.fold<int>(0, (sum, item) => sum + item.size) ==
                    active.treeBytes,
          );
          active.items = files;
          if (active.offer) {
            // Publish the virtual files; nothing is read from the peer until a paste.
            await _publishOffer(active);
            break;
          }
          active.step = 4;
          await _deliver(active, () async {
            if (_current(active)) {
              await _bridge.begin(_owner, active.id, 'tree', active.entries);
            }
          });
          if (!_current(active)) return;
          active.stage = 'data';
          _armClaim(active);
        case 'clipboard.chunk':
          _fields(value, ['type', 'requestId', 'index', 'offset', 'data']);
          _require(
            !active.sending &&
                active.stage == 'data' &&
                active.index < active.items.length &&
                value['index'] == active.index &&
                value['offset'] == active.offset &&
                value['data'] is String,
          );
          final encoded = value['data'] as String;
          _require(encoded.length <= (_limits.chunkBytes + 2) ~/ 3 * 4);
          final bytes = base64Decode(encoded);
          _require(
            bytes.isNotEmpty &&
                bytes.length <= _limits.chunkBytes &&
                base64Encode(bytes) == encoded &&
                bytes.length <= active.items[active.index].size - active.offset,
          );
          active.step = 5;
          await _deliver(active, () async {
            if (_current(active)) {
              await _bridge.append(
                _owner,
                active.id,
                active.index,
                active.offset,
                bytes,
              );
            }
          });
          if (!_current(active)) return;
          active.offset += bytes.length;
          transferredBytes += bytes.length;
          // A windowed pair is acknowledged cumulatively; stop and wait per chunk.
          if (active.ackPolicy.chunk(bytes.length)) {
            _send({
              'type': 'clipboard.chunk.ack',
              'requestId': active.id,
              'index': active.index,
              'offset': active.offset,
            });
          }
        case 'clipboard.item.end':
          _fields(value, ['type', 'requestId', 'index', 'sha256']);
          _require(
            !active.sending &&
                active.stage == 'data' &&
                active.index < active.items.length &&
                value['index'] == active.index &&
                active.offset == active.items[active.index].size &&
                value['sha256'] is String &&
                RegExp(r'^[a-f0-9]{64}$').hasMatch(value['sha256'] as String),
          );
          active.step = 6;
          await _deliver(active, () async {
            if (_current(active)) {
              await _bridge.finish(
                _owner,
                active.id,
                active.index,
                value['sha256'] as String,
              );
            }
          });
          if (!_current(active)) return;
          if (active.echoOf?.digests[active.index] != value['sha256']) {
            active.echoOf = null;
          }
          active.ackPolicy.reset();
          _send({
            'type': 'clipboard.item.ack',
            'requestId': active.id,
            'index': active.index,
          });
          active.index++;
          active.offset = 0;
        case 'clipboard.commit':
          _fields(value, ['type', 'requestId']);
          _require(
            !active.sending &&
                active.stage == 'data' &&
                active.index == active.items.length,
          );
          active.stage = 'localCommit';
          active.step = 7;
          await _deliver(active, () async {
            if (_current(active)) {
              // Byte-identical to our own last send (an older Host announces
              // its own write): keep the clipboard if it still holds that.
              final unless = active.echoOf?.sequence;
              await _bridge.commit(_owner, active.id, unlessSequence: unless);
              if (!_publicationCurrent(active)) {
                throw const ClipboardTransferException('clipboard_changed');
              }
              final bridge = _bridge;
              if (bridge is ClipboardObservationBridge) {
                final sequence = (bridge as ClipboardObservationBridge)
                    .lastCommittedSequence;
                // A publish always advances the sequence; the unchanged one
                // means the native side kept the clipboard as it was.
                active.keptOwnCopy = unless != null && sequence == unless;
                _sync?.appliedLocal(sequence);
                active.publishedSequence = sequence;
                final binding = active.pendingBinding;
                if (binding != null) {
                  _pendingClipboard?.publishedBinding(binding, sequence);
                } else {
                  _pendingClipboard?.published(sequence);
                }
              }
              if (active.format == 'files' &&
                  !active.keptOwnCopy &&
                  bridge is ClipboardPublishDiagnostics) {
                active.publishCheck =
                    (bridge as ClipboardPublishDiagnostics).lastPublishCheck;
              }
            }
          });
          if (!_current(active)) return;
          active.stage = 'commit';
          _send({'type': 'clipboard.applied', 'requestId': active.id});
        case 'clipboard.chunk.ack':
          _fields(value, ['type', 'requestId', 'index', 'offset']);
          _require(
            active.sending &&
                active.stage == 'data' &&
                value['index'] is int &&
                value['offset'] is int,
          );
          transferredBytes += _released(
            () => active.window.ackChunk(
              value['index'] as int,
              value['offset'] as int,
            ),
          );
          await _pump(active);
        case 'clipboard.item.ack':
          _fields(value, ['type', 'requestId', 'index']);
          _require(
            active.sending && active.stage == 'data' && value['index'] is int,
          );
          transferredBytes += _released(
            () => active.window.ackItem(value['index'] as int),
          );
          await _pump(active);
        case 'clipboard.complete':
          _fields(value, ['type', 'requestId']);
          _require(active.stage == 'commit' || active.cancelled);
          await _finish(active, null);
        default:
          throw const ClipboardTransferException('clipboard_invalid_message');
      }
      if (!_closed) notifyListeners();
    } catch (failure) {
      final active = _transfer;
      if (failure is PlatformException && receivingTransfer != null) {
        // Native RPC failures have the same lifetime as session refusals.
        // A result from retired A must not be attributed to current B.
        final original = receivingTransfer;
        if (_current(original)) {
          await _nativeFailure(original, failure);
        }
      } else if (failure is ClipboardTransferException &&
          failure.code == 'clipboard_changed') {
        // A timer may retire A while its bind RPC waits, then an explicit
        // action may start B. A late refusal belongs only to the matched A.
        final original = receivingTransfer;
        if (original != null && _current(original)) {
          await _nativeFailure(original, failure);
        }
      } else if (failure is PlatformException && active != null) {
        await _nativeFailure(active, failure);
      } else {
        // What ended the channel, for the diagnostics: a protocol check of ours (1), a pull violation (2) or an unexpected error of
        // the handler itself (3: a bug, not the peer).
        if (_closeKind == 0) {
          _closeKind = failure is ClipboardTransferException ? 1 : 3;
        }
        await _transportFailed('clipboard_invalid_message');
      }
    }
  }

  /// The data items of an offer of text, rich text or an image (the manifest of the clipboard.begin it replaces).
  List<ClipboardItem> _contentItems(String format, Map<String, dynamic> value) {
    final items = <ClipboardItem>[];
    if (format == 'text') {
      _fields(value, ['type', 'requestId', 'format', 'size']);
      _require(value['size'] is int);
      items.add(ClipboardItem(size: value['size'] as int, kind: 'text'));
      return items;
    }
    _fields(value, ['type', 'requestId', 'format', 'items']);
    _require(value['items'] is List);
    for (final item in value['items'] as List) {
      _require(item is Map<String, dynamic>);
      final part = item as Map<String, dynamic>;
      _fields(
        part,
        format == 'image'
            ? ['kind', 'size', 'width', 'height']
            : ['kind', 'size'],
      );
      _require(part['kind'] is String && part['size'] is int);
      if (format == 'image') {
        _require(part['width'] is int && part['height'] is int);
      }
      items.add(
        ClipboardItem(
          size: part['size'] as int,
          kind: part['kind'] as String,
          width: part['width'] as int?,
          height: part['height'] as int?,
        ),
      );
    }
    return items;
  }

  /// Hands the peer's offer to the platform as a data object on the clipboard (virtual files, CF_HDROP for a small set, or text / HTML /
  /// an image rendered when asked). Nothing is read from the peer until something pastes.
  Future<void> _publishOffer(_Transfer active) async {
    await _bindPending(active);
    // Publication itself changes the local clipboard. Its synchronous change
    // notification must not cancel this same automatic offer while its native
    // result is pending, just as a pushed clipboard.commit is protected above.
    active.stage = 'localCommit';
    active.step = 7;
    final onPaste = _onPasteBridge;
    final prepared = active.pendingBound;
    void register() => active.deferredOffer
        ? _pulls?.offerPublishedDeferred(active.id)
        : _pulls?.offerPublished(active.id, active.entries, format: active.format);
    if (prepared) {
      // A consumer is already waiting in GetData. Its first pull may race the
      // native publication reply, so validated metadata must exist beforehand.
      // This registers no bytes and reports no publication acknowledgement.
      register();
    }
    try {
      await _deliver(active, () async {
        if (_current(active)) {
          if (active.deferredOffer) {
            // Registered before publishing: the first paste may ask for the list before the reply returns.
            if (!prepared) register();
            await (_bridge as ClipboardDeferredBridge).publishDeferredOffer(_owner, active.id);
          } else if (onPaste != null) {
            await onPaste.publishOfferOnPaste(
              _owner,
              active.id,
              active.format,
              active.entries,
            );
          } else {
            await (_bridge as ClipboardPullBridge).publishOffer(
              _owner,
              active.id,
              active.entries,
            );
          }
          if (!_publicationCurrent(active)) {
            throw const ClipboardTransferException('clipboard_changed');
          }
          final bridge = _bridge;
          if (bridge is ClipboardObservationBridge) {
            // Our own publication must never be sent back to the peer.
            _sync?.appliedLocal(
              (bridge as ClipboardObservationBridge).lastCommittedSequence,
            );
            final sequence =
                (bridge as ClipboardObservationBridge).lastCommittedSequence;
            final binding = active.pendingBinding;
            if (binding != null) {
              _pendingClipboard?.publishedBinding(binding, sequence);
            } else {
              _pendingClipboard?.published(sequence);
            }
          }
          active.published = true;
        }
      });
    } catch (_) {
      if (prepared || active.deferredOffer) {
        _pulls?.offerWithdrawn(active.id);
        try {
          await _onPasteBridge?.pullRetire(_owner, active.id);
        } catch (_) {}
      }
      rethrow;
    }
    if (!_current(active)) return;
    if (!prepared && !active.deferredOffer) register();
    active.stage = 'commit';
    _send({'type': 'clipboard.applied', 'requestId': active.id});
    status = active.format == 'tree' ? l10n.filesArriveWhenYouPaste : l10n.arrivesWhenYouPaste;
    // The peer withdrew this offer before it was published: it goes again at once.
    if (_withdrawnEarly.remove(active.id)) await _withdrawnByPeer(active.id);
  }

  /// Offers withdrawn by the peer while they were still being published.
  final _withdrawnEarly = <String>{};

  /// The peer's clipboard changed: its offer is no longer served. Whatever reads it fails now, and it leaves this PC's clipboard if it is
  /// still what the clipboard holds (never replacing anything the user copied since).
  Future<void> _withdrawnByPeer(String offerId) async {
    _pulls?.offerWithdrawn(offerId);
    final bridge = _onPasteBridge;
    if (bridge == null) return;
    try {
      await bridge.pullRetire(_owner, offerId);
    } catch (_) {}
    final transfer = _transfer;
    if (transfer != null &&
        transfer.id == offerId &&
        transfer.offer &&
        !transfer.published) {
      _withdrawnEarly.add(offerId);
      return;
    }
    await _native(() async {
      try {
        await bridge.withdrawOffer(_owner, offerId);
      } catch (_) {}
    });
  }

  /// The VM announced a new copy: every offer of its earlier copies is retired as if the Host had withdrawn it. What reads one ends at
  /// once (that part does not queue behind other native work) and the current one leaves the clipboard unless something replaced it.
  /// One a paste on this PC is still reading stays (ClipboardPulls.pasting): the Host keeps serving it after its new copy, and
  /// withdraws it itself once that paste is quiet. A Host that does not keep it withdraws it at its copy, which ends the paste as before.
  void _retirePeerOffers() {
    final pulls = _pulls;
    if (pulls == null || _onPasteBridge == null) return;
    for (final id in pulls.receivedOfferIds) {
      if (pulls.pasting(id)) continue;
      unawaited(_own(_withdrawnByPeer(id)));
    }
  }

  /// This PC's clipboard changed: the offers it kept are no longer served; the peer is told so and clears what it published for them.
  Future<void> _withdrawOwnOffers() async {
    final bridge = _onPasteBridge;
    if (bridge == null) return;
    try {
      for (final id in await bridge.dropOffers(_owner)) {
        try {
          _send({'type': 'clipboard.offer.withdraw', 'requestId': id});
        } catch (_) {}
      }
    } catch (_) {}
  }

  /// The manifest header of a folder tree or of a paste-pull offer (same fields);
  /// clipboard.entries pages describe the entries next.
  void _beginTree(
    _Transfer active,
    Map<String, dynamic> value, {
    bool offer = false,
  }) {
    _require(_limits.formats);
    _fields(value, [
      'type',
      'requestId',
      'format',
      'entries',
      'files',
      'bytes',
      'tops',
      'skipped',
    ]);
    _require(value['format'] == 'tree');
    for (final key in ['entries', 'files', 'bytes', 'tops', 'skipped']) {
      _require(value[key] is int && (value[key] as int) >= 0);
    }
    _require(
      (value['entries'] as int) >= 1 &&
          (value['entries'] as int) <= _limits.maxEntries &&
          (value['tops'] as int) >= 1 &&
          (value['tops'] as int) <= _limits.maxFiles &&
          (value['files'] as int) <= (value['entries'] as int) &&
          (value['bytes'] as int) <= _limits.maxTotalBytes,
    );
    active
      ..format = 'tree'
      ..offer = offer
      ..treeTops = value['tops'] as int
      ..treeSkipped = value['skipped'] as int
      ..treeFileCount = value['files'] as int
      ..treeBytes = value['bytes'] as int
      ..treeEntryCount = value['entries'] as int
      ..entries = <ClipboardItem>[]
      ..stage = 'entries';
    totalBytes = value['bytes'] as int;
  }

  /// Applies an acknowledgement to the window; one that names no pending
  /// unit is a protocol violation.
  int _released(int Function() release) {
    try {
      return release();
    } on ClipboardWindowException {
      throw const ClipboardTransferException('clipboard_invalid_message');
    }
  }

  String _safeReason(dynamic reason) =>
      reason is String && RegExp(r'^[a-z_]{1,64}$').hasMatch(reason)
      ? reason
      : 'clipboard_transfer_failed';

  /// The scale revision's specific-limit reasons as the code the rest of the
  /// client knows (clipboard_limits_exceeded) and the detail that names the
  /// limit; any other reason is itself.
  static (String, String?) specificLimit(String reason) => switch (reason) {
    'clipboard_path_too_long' => ('clipboard_limits_exceeded', 'path'),
    'clipboard_text_too_large' => ('clipboard_limits_exceeded', 'text'),
    'clipboard_entry_count_limit' => ('clipboard_limits_exceeded', 'entries'),
    'clipboard_depth_limit' => ('clipboard_limits_exceeded', 'depth'),
    'clipboard_image_too_large' => ('clipboard_limits_exceeded', 'image'),
    _ => (reason, null),
  };

  Future<void> _nativeFailure(_Transfer transfer, Object failure) async {
    if (!_current(transfer)) return;
    final peerCode = failure is PlatformException
        ? failure.code
        : failure is ClipboardTransferException &&
              failure.code == 'clipboard_changed'
        ? failure.code
        : '';
    final code =
        const {
          'clipboard_empty',
          'clipboard_busy',
          'unsupported_clipboard_format',
          'clipboard_read_failed',
          'clipboard_write_failed',
          'clipboard_file_unavailable',
          'clipboard_transfer_failed',
          'no_interactive_session',
          'clipboard_cancelled',
          'clipboard_policy_denied',
          'clipboard_changed',
          ..._extendedReasons,
        }.contains(peerCode)
        ? peerCode
        : 'clipboard_transfer_failed';
    final detail =
        code == 'clipboard_limits_exceeded' && failure is PlatformException
        ? failure.message
        : null;
    // Where it came from: the native adapter (1) or this session's own check or send (2, 6).
    final origin = failure is PlatformException
        ? 1
        : failure is ClipboardTransferException
        ? 2
        : 6;
    // Before begin was sent there is no server-side transfer to cancel/error.
    if (transfer.stage != 'capture') {
      try {
        _send({
          'type': 'clipboard.error',
          'requestId': transfer.id,
          'reason': _wireReason(code, detail),
        });
      } catch (_) {
        await _transportFailed('clipboard_transport_closed');
        return;
      }
    }
    await _finish(
      transfer,
      code,
      detail: detail,
      origin: origin,
      cause: failure is PlatformException ? win32Detail(failure.message) : 0,
    );
  }

  /// The native adapter's `win32:<detail>` message of a failed write or open
  /// of this PC's clipboard (clipboard.hpp writeFailureDetail: Win32 error in
  /// the low 16 bits, who held the clipboard in bits 16-17, bit 20 = retried
  /// after releasing what this process held itself); 0 when there is none.
  static int win32Detail(String? message) {
    final match = RegExp(r'^win32:(\d{1,9})$').firstMatch(message ?? '');
    return match == null ? 0 : int.parse(match[1]!);
  }

  /// Reasons only a relay and peers of the extended-limits profile know.
  static const _extendedReasons = {
    'clipboard_limits_exceeded',
    'clipboard_disk_full',
    'clipboard_file_locked',
  };

  /// The reason sent to the relay: a released relay rejects unknown reasons, so
  /// for such a pair the closest released one is sent instead.
  String _wireReason(String code, [String? detail]) {
    // The scale revision names the limit that was met; every other pair gets
    // clipboard_limits_exceeded (or, below, the closest released reason).
    if (_limits.scale && code == 'clipboard_limits_exceeded') {
      switch (detail?.split(':').first) {
        case 'path':
          return 'clipboard_path_too_long';
        case 'text':
          return 'clipboard_text_too_large';
        case 'entries':
          return 'clipboard_entry_count_limit';
        case 'depth':
          return 'clipboard_depth_limit';
        case 'image':
          return 'clipboard_image_too_large';
      }
    }
    if (_limits.extended || !_extendedReasons.contains(code)) return code;
    return switch (code) {
      'clipboard_disk_full' => 'clipboard_write_failed',
      'clipboard_file_locked' => 'clipboard_file_unavailable',
      _ => 'clipboard_transfer_failed',
    };
  }

  /// A pull ended in failure (the platform gave up, the peer reported it, the
  /// offer is gone): the user hears why; the channel and the video go on.
  /// [record] is false for a served range that failed together with an
  /// earlier one of the same paste: the user still sees it, the log has the
  /// first one only.
  void _pullFailed(String reason, {bool sending = false, bool record = true}) {
    if (_closed) return;
    // A deferred list request ends with the limit its walk met (entries, path, depth): the limits message names it.
    final (code, detail) = specificLimit(reason);
    error = code;
    errorDetail = detail;
    failureQuiet = false;
    status = _failureStatus(code, detail);
    if (record) {
      // action 8: a pulled file did not arrive. Pull-wire reasons the
      // diagnostic code set lacks (clipboard_offer_expired,
      // clipboard_selected_file_changed) are mapped as for actions 12 and 13
      // instead of becoming unknown_failure.
      final detail = clipboardPullDiagnosticDetail(code);
      ProductLog.instance.recordFailure(
        LogEvent.clipboard,
        clipboardPullDiagnosticCode(code),
        {
          'action': 8,
          'ok': code == 'clipboard_cancelled',
          'remote': sending,
          'auto': false,
          'stage': 5,
          if (detail != 0) 'error': detail,
        },
      );
    }
    notifyListeners();
  }

  /// A newer copy exists: the automatic transfer still crossing is cancelled
  /// (before its commit; after it the result is already being published) so
  /// the newer copy is not queued behind it. A transfer the user started is
  /// left alone, and so is one that was cancelled already.
  void _supersede() {
    final transfer = _transfer;
    if (_closed ||
        transfer == null ||
        !transfer.automatic ||
        transfer.cancelled ||
        transfer.stage == 'commit' ||
        transfer.stage == 'localCommit') {
      return;
    }
    unawaited(cancelTransfer());
  }

  Future<void> cancelTransfer() async {
    final transfer = _transfer;
    if (transfer == null &&
        ((_pulls?.userWaiting ?? false) || (_pulls?.active ?? false))) {
      await _pulls!.cancel();
      return;
    }
    if (transfer == null || transfer.cancelled) return;
    if (transfer.stage == 'capture') {
      // Reserve the terminal result before notifying any reentrant listener.
      await _finish(transfer, 'clipboard_cancelled');
      return;
    }
    transfer.cancelled = true;
    transfer.terminalRequest = 'clipboard_cancelled';
    status = l10n.cancelingClipboardTransfer;
    notifyListeners();
    try {
      _send({'type': 'clipboard.cancel', 'requestId': transfer.id});
    } catch (_) {
      await _transportFailed('clipboard_transport_closed');
    }
  }

  /// [echo]: an automatic capture found this session's own received content;
  /// nothing was sent and nothing changed, so the previous status stays.
  /// How a transfer's failure is attributed in the diagnostics (action 2, field
  /// `error` = cause * 100 + origin * 10 + native step; origin 0 = not attributed;
  /// cause = win32Detail of a failed write of this PC's clipboard, 0 when none):
  /// origin 1 the native adapter threw, 2 this session's own check or send
  /// failed, 3 the peer reported it through the relay, 4 the channel closed,
  /// 5 a timeout, 6 an unexpected exception; step 1 capture, 2 read a chunk,
  /// 3 digest a file, 4 begin receiving, 5 append, 6 finish an item, 7 commit.
  /// Example: 12 = the native adapter threw while reading a chunk of a file.
  Future<void> _finish(
    _Transfer transfer,
    String? code, {
    bool echo = false,
    String? detail,
    int origin = 0,
    int cause = 0,
  }) {
    final finishing = transfer.finishing;
    if (finishing != null) return finishing;
    if (!identical(_transfer, transfer)) return Future<void>.value();
    final terminal = Completer<void>();
    if (code != 'clipboard_commit_uncertain') {
      code = transfer.terminalRequest ?? code;
    }
    transfer.finishing = terminal.future;
    transfer.cancelled = true;
    transfer.claimTimer?.cancel();
    _retired.add(transfer.id);
    if (_sequenceIds && _retired.length > 256) _retired.remove(_retired.first);
    if (transfer.sending && transfer.capturePhase == _CapturePhase.preparing) {
      _captureSettled(transfer, null);
    }
    final work = _finishOnce(
      transfer,
      code,
      echo: echo,
      detail: detail,
      origin: origin,
      cause: cause,
    );
    _own(work);
    work.then(
      (_) => terminal.complete(),
      onError: (Object error, StackTrace stack) =>
          terminal.completeError(error, stack),
    );
    return terminal.future;
  }

  Future<void> _finishOnce(
    _Transfer transfer,
    String? code, {
    required bool echo,
    String? detail,
    required int origin,
    required int cause,
  }) async {
    if (code == 'clipboard_commit_uncertain') _uncertainRecovery = true;
    var earlySource = false;
    if (transfer.sending) {
      // This metadata response precedes the queued STA retirement. An absent
      // receipt after an invoked legacy capture still requires strict cleanup.
      final outcome = await transfer.captureOutcome.future;
      final receipt = _exactSource(transfer);
      earlySource = !outcome.invoked || receipt != null;
      if (receipt != null) _retireSource(transfer, receipt);
    }
    if (!transfer.sending && code != null) {
      // A platform teardown failure must not strand transfer completion behind
      // this best-effort exact-lease cancellation. Native waits are bounded.
      try {
        final binding = transfer.pendingBinding;
        final admission = transfer.pendingAdmission;
        if (binding != null) {
          await _pendingClipboard?.failedBinding(binding);
        } else if (admission != null) {
          await _pendingClipboard?.failedAdmission(admission);
        }
      } catch (_) {}
    }
    if (!identical(_transfer, transfer)) return;
    _transfer = null;
    if (!earlySource) {
      _cleaning = true;
      var released = false;
      final release = _native(() async {
        // Legacy cleanup remains serialized. Committed CF_HDROP files persist.
        try {
          await _bridge.release(_owner);
          released = true;
        } catch (_) {}
      });
      final releaseTail = _nativeTail;
      await release;
      if (released) _releasedNativeTail = releaseTail;
      _cleaning = false;
    }
    if (echo) {
      error = transfer.previousError;
      status = transfer.previousStatus;
    } else {
      error = code;
      errorDetail = detail;
      // An automatic copy that meets something expected and harmless (nothing
      // to share, a format that is not supported, content the policy does not
      // allow) is recorded only; the user did not start a transfer.
      failureQuiet =
          code != null &&
          transfer.automatic &&
          _quietWhenAutomatic.contains(code);
      if (failureQuiet) {
        _quietFailure = code;
        _quietFailureDetail = detail;
      }
      status = code == null
          ? (transfer.offer
                ? (transfer.format == 'tree'
                      ? (transfer.treeSkipped > 0
                            ? l10n.copiedFilesArriveWhenYou(transfer.treeSkipped)
                            : l10n.copiedFilesArriveWhenYou2)
                      : l10n.copiedArrivesWhenYouPaste)
                : transfer.treeSkipped > 0
                // Links and names Windows cannot create (AUX.txt, "name.")
                // are left out by the native walk, not the whole copy refused.
                ? l10n.clipboardTransferDoneSkipped(transfer.treeSkipped)
                : l10n.clipboardTransferDone)
          : _failureStatus(code, detail);
      // action 2: one transfer; remote = this PC → the VM; stage = the
      // format (1 text, 2 files, 3 rich text, 4 image, 5 folder tree; absent
      // before a format was known).
      final outcome = {
        'action': 2,
        'ok': code == null || _normalTransferEnds.contains(code),
        // Monotonic local request-to-terminal time; no clipboard contents or
        // Windows sequence values. This does not include Host observation or
        // the client's debounce before reserving this transfer.
        'elapsedMs': transfer.elapsed.elapsedMilliseconds.clamp(0, 86400000),
        'remote': transfer.sending,
        'auto': transfer.automatic,
        if (transfer.format == 'text') 'stage': 1,
        if (transfer.format == 'files') 'stage': 2,
        if (transfer.format == 'richtext') 'stage': 3,
        if (transfer.format == 'image') 'stage': 4,
        if (transfer.format == 'tree') 'stage': 5,
        if (code != null && origin != 0)
          'error': cause * 100 + origin * 10 + transfer.step,
      };
      if (code == null) {
        ProductLog.instance.record(LogEvent.clipboard, outcome);
      } else {
        ProductLog.instance.recordFailure(LogEvent.clipboard, code, outcome);
      }
    }
    // action 5: the peer's own content was recognised and not passed back;
    // remote = the direction not taken (true: not sent to the VM, false:
    // received bytes equal to our own copy were not published locally).
    if (echo || (code == null && transfer.keptOwnCopy)) {
      ProductLog.instance.record(LogEvent.clipboard, {
        'action': 5,
        'ok': true,
        'remote': transfer.sending,
        'auto': transfer.automatic,
      });
    }
    // action 6: received files were published on this PC. ok = the desktop
    // (Explorer) user could open every one; error = the first Windows error;
    // stage = flags (1 elevated client, 2 desktop user is another account,
    // 4 untestable, 8 a path of MAX_PATH or longer, 16 published by the
    // desktop-user helper, 32 the helper failed and the client published in
    // process). No paths or names.
    final check = transfer.publishCheck;
    if (check != null) {
      ProductLog.instance.record(LogEvent.clipboard, {
        'action': 6,
        'ok': check.readable,
        'error': check.error,
        'stage': check.flags,
        'remote': false,
        'auto': transfer.automatic,
      });
      _schedulePublishCheck(transfer.publishedSequence, transfer.automatic);
    }
    if (code == null) {
      if (transfer.sending &&
          !echo &&
          (transfer.format == 'text' || transfer.format == 'files') &&
          transfer.sourceSequence != null &&
          transfer.digests.length == transfer.items.length) {
        _lastSent = _Sent(
          transfer.format,
          transfer.items,
          List.unmodifiable(transfer.digests),
          transfer.sourceSequence!,
        );
      }
      if (transfer.sending) _sync?.confirmedLocal(transfer.sourceSequence);
      transferredBytes = totalBytes;
      if (!transfer.done.isCompleted) transfer.done.complete();
    } else if (!transfer.done.isCompleted) {
      transfer.done.completeError(ClipboardTransferException(code));
    }
    notifyListeners();
  }

  void _schedulePublishCheck(int? sequence, bool automatic) {
    final bridge = _bridge;
    if (_closed || bridge is! ClipboardPublishDiagnostics) return;
    _publishCheck?.cancel();
    _publishCheck = Timer(publishCheckDelay, () {
      if (_closed) return;
      unawaited(
        _native(() async {
          if (_closed) return;
          final state = await (bridge as ClipboardPublishDiagnostics).published(
            _owner,
          );
          // action 7: a few seconds after publishing received files. ok =
          // that copy still owns the clipboard; changed = the clipboard
          // sequence moved on since (another program replaced or added to it).
          ProductLog.instance.record(LogEvent.clipboard, {
            'action': 7,
            'ok': state.owned,
            'changed': sequence == null || state.sequence != sequence,
            'remote': false,
            'auto': automatic,
          });
        }).catchError((Object _) {}),
      );
    });
  }

  Future<void> _transportFailed(
    String code, {
    String? detail,
    bool transientDial = false,
  }) async {
    if (_closed) return;
    _reconnectable =
        (code == 'clipboard_transport_closed' ||
            code == 'clipboard_pair_timeout' ||
            // channel-resume-v1: the pair ended because the stream is away.
            code == 'clipboard_stream_away' ||
            code == 'clipboard_timeout') &&
        (_socket != null || transientDial);
    final transfer = _transfer;
    if (transfer?.stage == 'commit' || transfer?.stage == 'localCommit') {
      code = 'clipboard_commit_uncertain';
      detail = null;
    }
    if (code == 'clipboard_commit_uncertain') _uncertainRecovery = true;
    error = code;
    errorDetail = detail;
    if (!_ready.isCompleted) {
      _ready.completeError(ClipboardTransferException(code));
    }
    await close(reason: code);
  }

  Future<void> _expireTransfer(_Transfer transfer) async {
    if (!_current(transfer)) return;
    final uncertain =
        transfer.stage == 'commit' || transfer.stage == 'localCommit';
    if (transfer.stage != 'capture') {
      try {
        _send({'type': 'clipboard.cancel', 'requestId': transfer.id});
      } catch (_) {
        await _transportFailed('clipboard_transport_closed');
        return;
      }
    }
    // A slow or abandoned operation ends alone. Native cleanup still precedes
    // the next operation; a commit in progress keeps its uncertain outcome.
    await _finish(
      transfer,
      uncertain ? 'clipboard_commit_uncertain' : 'clipboard_timeout',
      origin: 5,
    );
  }

  Future<void> close({String reason = 'clipboard_transport_closed'}) =>
      _closing ??= _close(reason);
  Future<void> _close(String reason) async {
    if (_closed) return;
    _closed = true;
    ready = false;
    error ??= reason;
    status = _failureStatus(error!, errorDetail);
    // The coordinator must observe closure even while an old native operation
    // is finishing. It still waits for close() before creating a new owner.
    notifyListeners();
    // Straight to native epoch invalidation; never wait behind a blocked STA.
    try {
      await _pendingClipboard?.close();
    } catch (_) {}
    try {
      await _sync?.close();
    } catch (_) {}
    _deadline?.cancel();
    _publishCheck?.cancel();
    // Pulls end with the channel: an Explorer paste in progress fails, offers are retired.
    try {
      await _pulls?.close(reason);
    } catch (_) {}
    if (!_ready.isCompleted) {
      _ready.completeError(ClipboardTransferException(reason));
    }
    final transfer = _transfer;
    if (transfer != null) {
      final uncertain =
          transfer.stage == 'commit' || transfer.stage == 'localCommit';
      await _finish(
        transfer,
        uncertain ? 'clipboard_commit_uncertain' : reason,
        origin: reason == 'clipboard_timeout' ? 5 : 4,
      );
    }
    // Capture replies can register exact retirements after close began. Join
    // until neither lane nor the owner ledger can add another cleanup task.
    while (true) {
      final native = _nativeTail, delivery = _pendingDeliveryTail;
      await Future.wait([native, delivery, ..._ownedWork]);
      if (identical(native, _nativeTail) &&
          identical(delivery, _pendingDeliveryTail) &&
          _ownedWork.isEmpty) {
        break;
      }
    }
    var sourceCleanupFailed = false;
    if (_starting != null &&
        (_nativePendingLane ||
            _retireFailures.isNotEmpty ||
            !identical(_releasedNativeTail, _nativeTail))) {
      await _native(() async {
        try {
          await _bridge.release(_owner);
          _retireFailures.clear();
        } catch (_) {
          sourceCleanupFailed = _retireFailures.isNotEmpty;
        }
      });
    }
    try {
      await _subscription?.cancel();
    } catch (_) {}
    try {
      await _socket?.close().timeout(const Duration(seconds: 1));
    } catch (_) {}
    error ??= reason;
    if (transfer == null) status = _failureStatus(error!, errorDetail);
    // action 3: the channel ended; a session ending normally is not a failure.
    if (reason == 'clipboard_session_closed') {
      ProductLog.instance.record(LogEvent.clipboard, {'action': 3, 'ok': true});
    } else {
      // error: what ended the channel (see [_closeKind]); absent when it was an ordinary close.
      ProductLog.instance.recordFailure(LogEvent.clipboard, reason, {
        'action': 3,
        'ok': normalChannelEnds.contains(reason),
        if (_closeKind != 0) 'error': _closeKind,
      });
    }
    notifyListeners();
    if (sourceCleanupFailed) {
      _reconnectable = false;
      throw const ClipboardTransferException('clipboard_cleanup_failed');
    }
  }

  String _failureStatus(String code, [String? detail]) => switch (code) {
    'clipboard_selected_file_changed' => l10n.originalFileChangedCopyAgain,
    'clipboard_offer_expired' => l10n.whatYouCopiedNoLonger,
    'clipboard_changed' => l10n.somethingNewWasCopiedSo,
    'clipboard_policy_denied' => l10n.administratorDoesntAllowClipboardTransfer,
    'clipboard_empty' =>
      _limits.formats ? l10n.nothingCopiedNoTextImage : l10n.nothingCopiedNoTextFile,
    // A pair without the formats profile (an older Host or client) moves only
    // text and files; with it, "unsupported" means something else was copied.
    'unsupported_clipboard_format' =>
      _limits.formats ? l10n.contentCantMovedOnlyText : l10n.onlyTextFilesCanMoved(sharedFolders ? l10n.zipThemMoveThemShared : l10n.zipThemFirst),
    'clipboard_busy' => l10n.tryAgainAfterOtherClipboard,
    'clipboard_cancelled' => l10n.clipboardTransferCanceled,
    'clipboard_commit_uncertain' => l10n.couldntConfirmWasMovedTry,
    'clipboard_read_failed' => l10n.couldntReadWhatYouCopied,
    'clipboard_write_failed' => l10n.couldntPutReceivingClipboardCopy,
    'clipboard_file_unavailable' =>
      l10n.cantReadCopiedFileCheck,
    'clipboard_file_locked' =>
      l10n.anotherProgramUsingFileSo,
    'clipboard_disk_full' => l10n.notEnoughDiskSpaceReceiving,
    'clipboard_timeout' || 'clipboard_pair_timeout' => l10n.desktopDidntRespondTime,
    'no_interactive_session' =>
      l10n.clipboardCantUsedBecauseDesktop,
    'clipboard_unavailable' => l10n.clipboardIsntAvailableConnection,
    'authorization_revoked' => l10n.permissionsDesktopHaveChanged,
    'clipboard_policy_changed' => l10n.administratorChangedClipboardPermission,
    'clipboard_limits_exceeded' => _limits.message(detail, sharedFolders),
    'clipboard_session_limit' =>
      l10n.youveReachedClipboardLimitConnection,
    'clipboard_hash_mismatch' ||
    'clipboard_text_invalid' ||
    'clipboard_invalid_message' => l10n.receivedContentWasInvalidSo,
    'clipboard_transport_closed' => l10n.clipboardConnectionDroppedSoWasnt,
    _ => l10n.couldntTransferWhatYouCopied,
  };
}

/// The one notice a view shows when clipboard sharing stopped in this
/// connection: the channel ended and the automatic attempts (three per
/// connection, RemoteSession._scheduleClipboardReconnect) are over, so it does
/// not come back by itself and only a new connection starts a fresh one. A
/// cause the person should know (a permission, a locked screen) is [status];
/// a lost or broken channel is not named, only what brings sharing back.
String clipboardStopNotice(String? error, String status) => switch (error) {
  'clipboard_transport_closed' ||
  'clipboard_stream_away' ||
  'clipboard_timeout' ||
  'clipboard_pair_timeout' ||
  'clipboard_hash_mismatch' ||
  'clipboard_text_invalid' ||
  'clipboard_invalid_message' ||
  'clipboard_session_limit' => l10n.clipboardSharingStoppedReconnectDesktop,
  _ => l10n.clipboardSharingStopped(status),
};
