import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/services.dart';

import 'clipboard_limits.dart';
import 'clipboard_progress.dart';
import 'clipboard_pull_observation.dart';
import 'clipboard_window.dart';
import 'native_clipboard.dart';

/// A pull request id as the relay accepts it: `pullc-<n>` from the client and
/// `pullh-<n>` from the Host, n rising per requester within one clipboard pair.
final clipboardPullIdPattern = RegExp(r'^pull[ch]-[1-9][0-9]{0,8}$');

/// The reasons a pull may end with on the wire (the relay's pull reasons).
const _pullWireReasons = {
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
  'clipboard_limits_exceeded',
  'clipboard_disk_full',
  'clipboard_file_locked',
  'clipboard_selected_file_changed',
  'clipboard_offer_expired',
  // A deferred list request may end with the limit its walk met (an on-paste pair is a scale pair).
  'clipboard_path_too_long',
  'clipboard_entry_count_limit',
  'clipboard_depth_limit',
};

/// A list this side asked the peer for (deferred manifest): the counts once announced and the entries so far.
class _ManifestIn {
  _ManifestIn(this.requestId, this.offerId);
  final String requestId, offerId;
  bool begun = false;
  int entries = 0, files = 0, bytes = 0, tops = 0, walked = 0;
  final items = <ClipboardItem>[];
}

/// A list this side walks for the peer's paste of its deferred copy.
class _ManifestServe {
  _ManifestServe(this.requestId, this.offerId);
  final String requestId, offerId;
  // `done`: the walk returned; a progress event that arrives after it (another platform message) is not sent any more: the relay
  // accepts progress only before the counts.
  bool cancelled = false, done = false;
  int walked = 0, observation = 0;
}

class ClipboardPullViolation implements Exception {
  const ClipboardPullViolation();
  @override
  String toString() => 'clipboard_invalid_message';
}

/// One file range being pulled from the peer's offer.
class _Pull {
  _Pull(
    this.rangeId,
    this.offerId,
    this.index,
    this.offset,
    this.requestId,
    this.progressManifest,
  )
    : next = offset;
  final int rangeId, index, offset;
  final String offerId, requestId;

  // Display-only reference for this accepted range. Offer retirement must not
  // erase a live/native-finalizing range's known size/name/counters. Receiving
  // any more data still checks the live _offers map, so this grants no access
  // to a withdrawn/expired offer and cannot describe a newer range.
  final _Received progressManifest;

  /// The offset the next chunk must have.
  int next;
  int observation = 0;

  /// Asked for at the relay (a pull queue asks before this pull's turn).
  bool sent = false;
}

/// A manifest the peer offered and this side published as virtual files.
class _Received {
  _Received(
    this.id,
    this.sizes,
    this.folders, [
    this.names = const [],
    this.format = 'tree',
  ]) : total = sizes.fold(0, (sum, size) => sum + size);
  final String id;

  /// `tree` (files and folders), or `text`, `richtext`, `image` (on-paste offers of content).
  final String format;
  final List<int> sizes;

  /// Each file's name (relative path), in file order.
  final List<String> names;
  final int folders, total;

  /// Bytes delivered by the pulls of the paste in progress, and the files it has finished.
  int pulled = 0, finished = 0;

  /// The pasted items themselves: each file's first path segment, the first few distinct ones.
  late final List<String> topLevel = () {
    final seen = <String>{};
    for (final name in names) {
      final top = name.split(RegExp(r'[\\/]')).first;
      if (top.isEmpty || top == '.' || top == '..' || top.contains(':')) continue;
      if (seen.add(top) && seen.length == 4) break;
    }
    return List<String>.unmodifiable(seen);
  }();
}

/// What this side serves while the peer pastes: one file range read from the
/// offer the platform kept, streamed with the sliding window.
class _Serve {
  _Serve(this.requestId, this.index, this.size, this.offset, this.window)
    : sent = offset;
  final String requestId;
  final int index, size, offset;
  final ClipboardSendWindow window;
  int sent;
  int observation = 0;
  bool pumping = false, pumpAgain = false, ended = false;
  // Bytes read from the platform and not sent yet: one platform call reads several chunks (clipboardServeReadChunks).
  Uint8List? read;
  int readAt = 0;
}

/// The paste-pull profile of a clipboard channel. Copying announced only a
/// manifest ([ClipboardSession] sends `clipboard.offer`); this object does what
/// happens when the user pastes:
///  * receiving: the platform's virtual files ask for ranges; each becomes a
///    `clipboard.pull`, the peer's chunks are handed to the platform, and the
///    platform's consumption drives the acknowledgements (so the sender's window
///    follows what Explorer has actually taken);
///  * offering: `clipboard.pull` requests from the peer are answered by reading
///    the file the platform kept and sending the windowed chunk stream.
/// One pull is in flight per direction; the relay enforces it.
class ClipboardPulls {
  ClipboardPulls({
    required this.owner,
    required this.bridge,
    required this.send,
    required this.limits,
    required this.idPrefix,
    required this.changed,
    required this.started,
    required this.failed,
    this.failedServing,
    required this.mayServe,
    this.linger = const Duration(milliseconds: 800),
    this.onPaste,
    this.observed,
    this.depth = 1,
    this.deferred,
    this.validateTree,
    this.wireReason,
  });

  /// The deferred manifest (manifest-on-paste-v1) once the relay confirmed it for the pair.
  final ClipboardDeferredBridge? deferred;

  /// The session's checks of a folder tree against the pair's limits (throws PlatformException clipboard_limits_exceeded with the
  /// detail, or a protocol violation), and its reason for the relay of a failure code and detail.
  final void Function(List<ClipboardItem> entries, int tops)? validateTree;
  final String Function(String code, String? detail)? wireReason;

  /// Pulls this side may have outstanding: 1, or the relay's pull queue (clipboardPullQueueDepth). With a pull queue the relay also
  /// checks every chunk's base64 is canonical before it forwards it, and each pull's window is the queue's.
  final int depth;
  bool get _queued => depth > 1;

  /// The on-paste revision: our card is the only progress (the platform shows no Explorer dialog, so its operation notices are not
  /// counted), a cancel also ends what is blocked in the platform, and the Host says when a paste in the VM waits.
  final ClipboardOnPasteBridge? onPaste;
  final void Function(ClipboardPullObservation)? observed;
  late final _receivingObservation = ClipboardPullObserver(
    sending: false,
    emit: _observed,
  );
  late final _sendingObservation = ClipboardPullObserver(
    sending: true,
    emit: _observed,
  );
  void _observed(ClipboardPullObservation value) {
    if (!_closed) observed?.call(value);
  }

  final String owner;
  final ClipboardPullBridge bridge;
  final void Function(Map<String, Object?> message) send;
  final ClipboardLimits Function() limits;

  /// `pullc` for the client, `pullh` for the Host.
  final String idPrefix;
  final void Function() changed;

  /// A pull begins (the session clears its previous error).
  final void Function() started;

  /// A pull ended in failure with this code (a [ClipboardSession] failure code).
  final void Function(String code) failed;

  /// A pull this side served ended in failure. Every one updates what the
  /// user sees; [record] is true only for the first of a serving episode, so
  /// the ranges that fail together with it are one failed transfer.
  final void Function(String code, {required bool record})? failedServing;

  void _servingFailed(int observation, String reason) {
    final record = _sendingObservation.claimFailureReport(observation);
    final serving = failedServing;
    if (serving != null) {
      serving(reason, record: record);
    } else {
      failed(reason);
    }
  }

  /// Whether the administrator allows this side to hand an offer of this format (tree, text, richtext, image) to the peer.
  final bool Function(String format) mayServe;
  final Duration linger;

  final _offers = <String, _Received>{};
  final _receivedOffers = <String>{};
  final _operations = <String, int>{};
  final _operationFloors = <String, int>{};
  final _operationPulled = <String>{};
  final _lastObservation = <String, int>{};
  final _operationRevisions = <String, int>{};
  int _operationRevision = 0, _operationOwnerRevision = 0;

  int _touchOperation(String offerId) =>
      _operationRevisions[offerId] = ++_operationRevision;
  final _queue = <_Pull>[];
  // Deferred manifest: offers published without a list, the list asked for (one at a time) and the list being walked for the peer.
  final _deferredOffers = <String>{};
  _ManifestIn? _manifestIn;
  _ManifestServe? _manifestServe;
  // Bounded observation correlation for a platform failure following its cancel
  // notice. These local range IDs never enter diagnostics.
  final _rangeObservations = <int, int>{};
  _Pull? _active;
  // One native terminal callback owns finalization until its Future settles.
  // Other lifecycle paths may retire _active, but cannot admit a new range yet.
  _Pull? _finishing;
  _Serve? _serve;
  // A pull queue: the Host's pulls waiting for the one being served.
  final _serveQueue = <Map<String, dynamic>>[];
  String? _lastServedOfferId;
  int _counter = 0;
  bool _closed = false, _lingering = false, _serving = false;
  Timer? _lingerTimer;
  StreamSubscription<ClipboardPullEvent>? _events;

  /// The Host reported that an application in the VM waits for data of the PC's offer (on-paste revision).
  bool _pasteWaiting = false;

  /// Somebody is actually waiting for the data: a paste on this PC that pulls (queued, running or just ended), or a paste in the VM that
  /// the Host reports as waiting. Without the on-paste revision a served pull always counts (the Host reports nothing).
  bool get userWaiting =>
      !_closed &&
      (_operationWaiting ||
          _active != null ||
          _queue.isNotEmpty ||
          _lingering ||
          // A paste here waits for the list of a deferred copy; the VM's paste waits for this side's list.
          _manifestIn != null ||
          (_manifestServe != null && (onPaste == null || _pasteWaiting)) ||
          (_serve != null && (onPaste == null || _pasteWaiting)) ||
          (onPaste != null && _pasteWaiting));

  bool get _operationWaiting => _operations.keys.any(_operationPulled.contains);

  /// clipboard.paste.waiting from the Host.
  void pasteWaiting(bool active) {
    if (_closed) return;
    _pasteWaiting = active;
    _sendingObservation.waiting(active);
    changed();
  }

  /// The peer's offers this side published (a snapshot).
  List<String> get receivedOfferIds => List.of(_receivedOffers);

  /// When a range of each received offer last ended (for [pasting]).
  final _rangeEndedAt = <String, DateTime>{};

  /// A paste on this PC is reading [offerId] now: a range running or queued,
  /// Explorer's copy operation open (its "replace or skip" question may wait
  /// between two files), or a file finished moments ago (the next one is asked
  /// for right away). A newer copy in the VM does not end such a paste; the
  /// Host keeps serving it (as RDP locks the clipboard data of a paste). An
  /// explicit withdraw still ends it.
  bool pasting(String offerId) {
    if (_closed) return false;
    if (_active?.offerId == offerId ||
        _queue.any((pull) => pull.offerId == offerId) ||
        _operations.containsKey(offerId)) {
      return true;
    }
    final ended = _rangeEndedAt[offerId];
    return ended != null &&
        DateTime.now().difference(ended) < const Duration(seconds: 5);
  }

  /// The peer withdrew [offerId] (its clipboard changed): it is no longer answerable here.
  void offerWithdrawn(String offerId) {
    _operationRevisions.remove(offerId);
    _deferredOffers.remove(offerId);
    // The relay has already failed a list request for it; the platform's wait for that list ends with the withdrawal (pullRetire).
    if (_manifestIn?.offerId == offerId) _manifestIn = null;
    _offers.remove(offerId);
    _receivedOffers.remove(offerId);
    _retireOperation(offerId);
    _operationFloors.remove(offerId);
    _lastObservation.remove(offerId);
    if (_current == offerId) _current = null;
    if (_active == null && _queue.isEmpty && !_operationWaiting) {
      _lingering = false;
      _lingerTimer?.cancel();
    }
    changed();
  }

  void start() {
    _events ??= bridge.pullEvents.listen((event) {
      if (!_closed) unawaited(_onEvent(event));
    });
  }

  /// A pull is in flight, queued, being served or has just ended.
  bool get active =>
      !_closed &&
      (_operationWaiting ||
          _active != null ||
          _queue.isNotEmpty ||
          _serve != null ||
          _manifestIn != null ||
          _manifestServe != null ||
          _lingering);

  String? get _progressOfferId {
    final pull = _active ?? (_queue.isEmpty ? null : _queue.first);
    if (pull != null) return pull.offerId;
    final operating = _operations.keys.where(_operationPulled.contains);
    return operating.isEmpty ? _current : operating.last;
  }

  ClipboardProgress? get progress {
    if (!active) return null;
    // The list of a deferred copy is walked (by the peer for a paste here, or here for the peer's paste): how many items so far.
    final listing = _active == null && _serve == null ? (_manifestIn ?? _manifestServe) : null;
    if (listing != null) {
      final sending = listing is _ManifestServe;
      return ClipboardProgress(
        sending: sending,
        format: 'tree',
        fileCount: 0,
        fileIndex: 0,
        transferred: 0,
        total: 0,
        listed: listing is _ManifestIn ? listing.walked : (listing as _ManifestServe).walked,
      );
    }
    final serve = _serve;
    final pull = _active ?? (_queue.isEmpty ? null : _queue.first);
    final received =
        _servedOffer ?? pull?.progressManifest ?? _offers[_progressOfferId ?? ''];
    if (received == null) return null;
    final index = pull?.index ?? serve?.index ?? max(0, received.finished - 1);
    return ClipboardProgress(
      sending: _serving,
      format: received.format,
      fileCount: received.sizes.length,
      folderCount: received.folders,
      fileIndex: received.sizes.isEmpty
          ? 0
          : min(index, received.sizes.length - 1) + 1,
      transferred: min(received.pulled, received.total),
      total: received.total,
      fileName: received.names.isEmpty
          ? null
          : clipboardLeaf(
              received.names[min(index, received.names.length - 1)],
            ),
      asyncConsumer:
          !_serving &&
          _operations.containsKey(_progressOfferId) &&
          onPaste == null,
      topLevel: _serving ? const [] : received.topLevel,
    );
  }

  String? _current;
  _Received? _servedOffer;

  /// Asynchronous operations the platform reported on our data object (Explorer's paste calls
  /// StartOperation and shows its own progress dialog). While one runs, the app's card stays away.
  bool get asyncConsumer => onPaste == null && _operations.isNotEmpty;

  void _retireOperation(String offerId) {
    final operation = _operations.remove(offerId);
    if (operation != null) _operationFloors[offerId] = operation;
    _operationPulled.remove(offerId);
    _receivingObservation.waiting(_operations.isNotEmpty);
  }

  void _trimOffers() {
    while (_offers.length > 2) {
      // The oldest offer no paste is reading goes first (the newest stays).
      final keys = _offers.keys.toList();
      final id = keys
              .take(keys.length - 1)
              .where((key) => !pasting(key))
              .firstOrNull ??
          keys.first;
      _rangeEndedAt.remove(id);
      _offers.remove(id);
      _deferredOffers.remove(id);
      _receivedOffers.remove(id);
      _retireOperation(id);
      _operationFloors.remove(id);
      _lastObservation.remove(id);
      _operationRevisions.remove(id);
    }
  }

  Future<void> _operation(ClipboardPullOperation event) async {
    final id = event.offerId, epoch = event.rangeId;
    if (_closed || !_receivedOffers.contains(id) || epoch <= 0) return;
    if (event.started) {
      final previous = _operations[id] ?? 0;
      if (epoch <= previous || epoch <= (_operationFloors[id] ?? 0)) return;
      _touchOperation(id);
      _operations[id] = epoch;
      _operationPulled.remove(id);
      if (_active?.offerId == id || _queue.any((pull) => pull.offerId == id)) {
        _operationPulled.add(id);
      }
      _receivingObservation.waiting(true);
    } else {
      if (_operations[id] != epoch) return;
      final revision = _touchOperation(id);
      final ownerRevision = _operationOwnerRevision;
      final hadPull = _operationPulled.contains(id);
      final displayed = _progressOfferId == id;
      _retireOperation(id);
      final reason = event.failureCode == null
          ? null
          : _safe(event.failureCode);
      if (reason != null && hadPull) {
        final observation = _lastObservation[id];
        if (observation != null) {
          _receivingObservation.failed(observation, reason);
        }
        final active = _active;
        final queued = _queue.where((pull) => pull.offerId == id).toList();
        _queue.removeWhere((pull) => pull.offerId == id);
        final finalizing = active != null && identical(_finishing, active);
        if (active != null && active.offerId == id) {
          if (!finalizing) _active = null;
          try {
            send({'type': 'clipboard.cancel', 'requestId': active.requestId});
          } catch (_) {}
        }
        // Keep already queued requests live before yielding, as _cancelRange
        // does. An obsolete End continuation must not schedule newer work.
        _startNext();
        if (active != null && active.offerId == id && !finalizing) {
          try {
            await bridge.pullFail(active.rangeId, reason);
          } catch (_) {}
        }
        for (final pull in queued) {
          // These are detached old ranges, not current queue entries. Finish
          // their cleanup even if cancel/close has invalidated our UI work.
          if (pull.sent) {
            try {
              send({'type': 'clipboard.cancel', 'requestId': pull.requestId});
            } catch (_) {}
          }
          try {
            await bridge.pullFail(pull.rangeId, reason);
          } catch (_) {}
        }
        // Same offer IDs survive successive pastes. Recheck the exact local
        // lifecycle after native cleanup; neither a newer epoch/request nor
        // cancel/withdraw/close may inherit this old failure or notification.
        if (_closed ||
            ownerRevision != _operationOwnerRevision ||
            _operationRevisions[id] != revision) {
          return;
        }
        if (displayed &&
            !_closed &&
            (_progressOfferId == id || _progressOfferId == null)) {
          failed(reason);
        }
      }
      if ((_progressOfferId == id || _progressOfferId == null) &&
          _active == null &&
          _queue.isEmpty &&
          !_operationWaiting) {
        _lingering = false;
        _lingerTimer?.cancel();
      }
    }
    changed();
  }

  /// The platform published [entries] (a tree manifest, parents first) as virtual
  /// files. Older offers stay answerable: an Explorer paste may still be using one.
  void offerPublished(
    String offerId,
    List<ClipboardItem> entries, {
    String format = 'tree',
  }) {
    final files = [
      for (final entry in entries)
        if (!entry.isDirectory) entry.size,
    ];
    _offers[offerId] = _Received(
      offerId,
      files,
      entries.length - files.length,
      [
        for (final entry in entries)
          if (!entry.isDirectory) entry.name ?? '',
      ],
      format,
    );
    _receivedOffers.add(offerId);
    _touchOperation(offerId);
    _current = offerId;
    _trimOffers();
  }

  /// The platform published the peer's deferred offer (no list yet): a paste asks for the list ([ClipboardPullManifest]).
  void offerPublishedDeferred(String offerId) {
    _offers[offerId] = _Received(offerId, const [], 0, const [], 'tree');
    _deferredOffers.add(offerId);
    _receivedOffers.add(offerId);
    _touchOperation(offerId);
    _current = offerId;
    _trimOffers();
  }

  /// Declares what this side offered so progress can describe pulls served for it.
  void offerSent(
    String offerId,
    List<ClipboardItem> entries, {
    String format = 'tree',
  }) {
    final files = [
      for (final entry in entries)
        if (!entry.isDirectory) entry.size,
    ];
    _offers[offerId] = _Received(
      offerId,
      files,
      entries.length - files.length,
      [
        for (final entry in entries)
          if (!entry.isDirectory) entry.name ?? '',
      ],
      format,
    );
    _trimOffers();
  }

  // ---- Receiving ----------------------------------------------------------------------------------

  Future<void> _onEvent(ClipboardPullEvent event) async {
    try {
      switch (event) {
        case ClipboardPullRequest():
          await _request(event);
        case ClipboardPullOperation():
          await _operation(event);
        case ClipboardPullCancel():
          await _cancelRange(event.rangeId);
        case ClipboardPullAck():
          final pull = _active;
          if (pull != null && pull.rangeId == event.rangeId) {
            send({
              'type': 'clipboard.chunk.ack',
              'requestId': pull.requestId,
              'index': event.index,
              'offset': event.end,
            });
          }
        case ClipboardPullFailed():
          // The platform ended the range itself (timeout, hash mismatch...). Its
          // cancel notice already freed the slot; the user hears why.
          _receivingObservation.failed(
            _rangeObservations.remove(event.rangeId) ?? 0,
            _safe(event.reason),
          );
          failed(event.reason);
          changed();
        case ClipboardPullManifest():
          await _requestManifest(event.offerId);
        case ClipboardPullManifestCancel():
          final asked = _manifestIn;
          if (asked != null && asked.offerId == event.offerId) {
            _manifestIn = null;
            send({'type': 'clipboard.cancel', 'requestId': asked.requestId});
            // The peer was silent too long: that paste failed (a paste that gave up is no failure to report).
            if (event.reason == 'clipboard_timeout') failed('clipboard_timeout');
            changed();
          }
        case ClipboardPullManifestProgress():
          final walking = _manifestServe;
          if (walking != null && walking.offerId == event.offerId && !walking.cancelled && !walking.done) {
            walking.walked = event.entries;
            send({'type': 'clipboard.manifest.progress', 'requestId': walking.requestId, 'entries': event.entries});
            changed();
          }
      }
    } catch (_) {
      // A closed channel: the session reports it.
    }
  }

  /// Something on this PC pastes the peer's deferred offer: ask the peer for its list (one request at a time).
  Future<void> _requestManifest(String offerId) async {
    final bridge = deferred;
    if (bridge == null) return;
    Future<void> refuse(String reason) async {
      try {
        await bridge.manifestFail(owner, offerId, reason);
      } catch (_) {}
    }

    if (!_deferredOffers.contains(offerId) || !_offers.containsKey(offerId)) return refuse('clipboard_offer_expired');
    final asked = _manifestIn;
    if (asked != null) {
      if (asked.offerId == offerId) return;  // already asked (the platform asks once per attempt)
      return refuse('clipboard_busy');
    }
    if (_counter >= 999999999) return refuse('clipboard_session_limit');
    final request = _ManifestIn('$idPrefix-${++_counter}', offerId);
    _manifestIn = request;
    _current = offerId;
    _lingering = false;
    _lingerTimer?.cancel();
    started();
    send({'type': 'clipboard.manifest', 'requestId': request.requestId, 'offerId': offerId});
    changed();
  }

  /// The peer's answer to this side's list request: progress, the counts, the entry pages, or its error.
  Future<void> _manifestMessage(_ManifestIn request, Map<String, dynamic> value) async {
    final bridge = deferred!;
    final type = value['type'];
    Future<void> end(String reason) async {
      if (identical(_manifestIn, request)) _manifestIn = null;
      try {
        await bridge.manifestFail(owner, request.offerId, reason);
      } catch (_) {}
      if (reason != 'clipboard_cancelled') failed(reason);
      changed();
    }

    switch (type) {
      case 'clipboard.manifest.progress':
        _fields(value, ['type', 'requestId', 'entries']);
        final entries = value['entries'];
        if (entries is! int || entries < 0 || entries > limits().maxEntries) throw const ClipboardPullViolation();
        request.walked = entries;
        await bridge.manifestProgress(owner, request.offerId);
        changed();
      case 'clipboard.manifest.begin':
        _fields(value, ['type', 'requestId', 'offerId', 'entries', 'files', 'bytes', 'tops', 'skipped']);
        final counts = [value['entries'], value['files'], value['bytes'], value['tops'], value['skipped']];
        if (request.begun || value['offerId'] != request.offerId || counts.any((count) => count is! int || count < 0)) {
          throw const ClipboardPullViolation();
        }
        request
          ..begun = true
          ..entries = value['entries'] as int
          ..files = value['files'] as int
          ..bytes = value['bytes'] as int
          ..tops = value['tops'] as int
          ..walked = value['entries'] as int;
        if (request.entries < 1 || request.tops < 1 || request.files > request.entries) throw const ClipboardPullViolation();
        await bridge.manifestProgress(owner, request.offerId);
        changed();
      case 'clipboard.entries':
        _fields(value, ['type', 'requestId', 'start', 'entries']);
        final page = value['entries'];
        if (!request.begun || value['start'] != request.items.length || page is! List || page.isEmpty || page.length > 256 ||
            request.items.length + page.length > request.entries) {
          throw const ClipboardPullViolation();
        }
        for (final entry in page) {
          if (entry is! Map) throw const ClipboardPullViolation();
          final kind = entry['kind'], path = entry['path'], attrs = entry['attrs'], mtime = entry['mtime'];
          final file = kind == 'file';
          if ((kind != 'file' && kind != 'dir') || path is! String || attrs is! int || mtime is! int ||
              entry.length != (file ? 5 : 4) || (file && entry['size'] is! int)) {
            throw const ClipboardPullViolation();
          }
          request.items.add(
            ClipboardItem(size: file ? entry['size'] as int : 0, name: path, kind: kind as String, attributes: attrs, mtime: mtime),
          );
        }
        await bridge.manifestProgress(owner, request.offerId);
        if (request.items.length < request.entries) return;
        final files = [for (final item in request.items) if (!item.isDirectory) item];
        final tops = request.items.where((item) => !(item.name ?? '').contains('/')).length;
        if (files.length != request.files || files.fold<int>(0, (sum, item) => sum + item.size) != request.bytes || tops != request.tops) {
          throw const ClipboardPullViolation();
        }
        try {
          validateTree?.call(request.items, request.tops);
        } on PlatformException catch (failure) {
          // Beyond this pair's limits: the paste fails with the limit's own reason (the relay has already ended the request).
          final reason = wireReason?.call(failure.code, failure.message) ?? failure.code;
          return end(_safe(reason));
        }
        if (!identical(_manifestIn, request)) return;
        _manifestIn = null;
        if (!_offers.containsKey(request.offerId)) return;  // withdrawn meanwhile
        // The data object checks the list too: one it refuses fails this paste with its reason, and the copy stays deferred (the
        // next paste asks again) instead of looking listed.
        try {
          await bridge.manifestDeliver(owner, request.offerId, request.items);
        } on PlatformException catch (refused) {
          final reason = wireReason?.call(refused.code, refused.message) ?? refused.code;
          failed(_safe(reason));
          changed();
          return;
        }
        if (!_offers.containsKey(request.offerId)) return;
        _offers[request.offerId] = _Received(
          request.offerId,
          [for (final item in files) item.size],
          request.items.length - files.length,
          [for (final item in files) item.name ?? ''],
        );
        _deferredOffers.remove(request.offerId);
        changed();
      case 'clipboard.error':
        _fields(value, ['type', 'requestId', 'reason']);
        await end(_safe(value['reason']));
      case 'clipboard.cancel':
        // The offering peer answers a list request with the list or an error, never a cancel: a cancel is the relay ending a
        // request nobody answered within its idle bound, so the paste failed.
        _fields(value, ['type', 'requestId']);
        await end('clipboard_timeout');
      default:
        throw const ClipboardPullViolation();
    }
  }

  Future<void> _request(ClipboardPullRequest event) async {
    final offer = _offers[event.offerId];
    if (offer == null ||
        event.index >= offer.sizes.length ||
        event.offset > offer.sizes[event.index]) {
      _receivingObservation.failed(
        _receivingObservation.request(),
        'clipboard_offer_expired',
      );
      await bridge.pullFail(event.rangeId, 'clipboard_offer_expired');
      failed('clipboard_offer_expired');
      return;
    }
    if (_counter >= 999999999) {
      _receivingObservation.failed(
        _receivingObservation.request(),
        'clipboard_session_limit',
      );
      await bridge.pullFail(event.rangeId, 'clipboard_session_limit');
      return;
    }
    _touchOperation(event.offerId);
    _queue.add(
      _Pull(
        event.rangeId,
        event.offerId,
        event.index,
        event.offset,
        '$idPrefix-${++_counter}',
        offer,
      ),
    );
    if (_operations.containsKey(event.offerId)) {
      _operationPulled.add(event.offerId);
    }
    _current = event.offerId;
    _startNext();
  }

  Future<void> _cancelRange(int rangeId) async {
    final pull = _active;
    if (pull != null && pull.rangeId == rangeId) {
      if (identical(_finishing, pull)) {
        // The platform cancelled while its terminal callback was still held.
        // Retire old UI intent without reentering native finalization.
        _touchOperation(pull.offerId);
      } else {
        _receivingObservation.failed(pull.observation, 'clipboard_cancelled');
        _active = null;
      }
      send({'type': 'clipboard.cancel', 'requestId': pull.requestId});
      _startNext();
    } else {
      // A pull queue: one already asked for is cancelled at the relay and the Host too.
      for (final queued in _queue.where((queued) => queued.rangeId == rangeId)) {
        if (queued.sent) {
          _receivingObservation.cancelRange(queued.observation);
          send({'type': 'clipboard.cancel', 'requestId': queued.requestId});
        }
      }
      _queue.removeWhere((queued) => queued.rangeId == rangeId);
      if (_queued) _startNext();
    }
    _settled();
    changed();
  }

  /// Pulls outstanding at the relay: the one being received and those of a pull queue asked for ahead of it.
  int get _outstanding =>
      (_active != null ? 1 : 0) + _queue.where((pull) => pull.sent).length;

  void _startNext() {
    if (_closed) return;
    if (_active == null && _finishing == null && _queue.isNotEmpty) {
      final pull = _queue.removeAt(0);
      final offer = _offers[pull.offerId];
      if (offer != null && pull.index == 0 && pull.offset == 0) {
        // The first file of a paste: a new paste of the same offer counts from zero.
        if (offer.finished >= offer.sizes.length) {
          offer.pulled = 0;
          offer.finished = 0;
        }
      }
      _active = pull;
      _lingering = false;
      _lingerTimer?.cancel();
      started();
      if (!pull.sent) _sendPull(pull);
      changed();
    }
    // A pull queue: the next pulls are asked for now; the offering side serves them right after this one.
    for (final pull in _queue) {
      if (_outstanding >= depth) break;
      if (!pull.sent) _sendPull(pull);
    }
  }

  void _sendPull(_Pull pull) {
    pull.sent = true;
    pull.observation = _receivingObservation.request();
    _lastObservation[pull.offerId] = pull.observation;
    _rangeObservations[pull.rangeId] = pull.observation;
    if (_rangeObservations.length > 16) {
      _rangeObservations.remove(_rangeObservations.keys.first);
    }
    send({
      'type': 'clipboard.pull',
      'requestId': pull.requestId,
      'offerId': pull.offerId,
      'index': pull.index,
      'offset': pull.offset,
    });
  }

  void _settled() {
    if (_active != null || _queue.isNotEmpty) return;
    final offer = _offers[_current ?? ''];
    // After the last file nothing more is expected; between files a short wait
    // keeps the progress notice from flickering.
    final complete = offer != null && offer.finished >= offer.sizes.length;
    if (complete || linger == Duration.zero) {
      _lingering = false;
      _lingerTimer?.cancel();
    } else {
      _lingering = true;
      _lingerTimer?.cancel();
      _lingerTimer = Timer(linger, () {
        _lingering = false;
        if (!_closed) changed();
      });
    }
  }

  /// A message of the relay with a pull id. Returns when it is handled.
  Future<void> handle(Map<String, dynamic> value) async {
    try {
      await _handle(value);
    } on PlatformException {
      // The platform could not take the data: this pull ends, the channel goes on.
      final pull = _active;
      if (pull != null && pull.requestId == value['requestId']) {
        _receivingObservation.failed(
          pull.observation,
          'clipboard_transfer_failed',
        );
        _active = null;
        try {
          send({'type': 'clipboard.cancel', 'requestId': pull.requestId});
        } catch (_) {}
        failed('clipboard_transfer_failed');
        _startNext();
        _settled();
        changed();
      }
    }
  }

  Future<void> _finishReceiving(
    _Pull pull,
    Future<void> Function() finishNative, {
    String? reason,
    _Received? completedOffer,
    bool cancelPeer = false,
  }) async {
    if (_closed || !identical(_active, pull) || _finishing != null) return;
    _finishing = pull;
    final ownerRevision = _operationOwnerRevision;
    final revision = _operationRevision;
    final offerRevision = _operationRevisions[pull.offerId];
    bool currentRevision() =>
        !_closed &&
        ownerRevision == _operationOwnerRevision &&
        revision == _operationRevision &&
        offerRevision == _operationRevisions[pull.offerId];
    var outcome = reason;
    var observedTerminal = false;
    try {
      if (cancelPeer) {
        send({'type': 'clipboard.cancel', 'requestId': pull.requestId});
      }
      await finishNative();
      observedTerminal = true;
      // Receiving all bytes is not native finalization. Count completion only
      // after a successful callback, and never mutate a newer paste's progress.
      if (outcome == null &&
          currentRevision() &&
          completedOffer != null &&
          identical(_offers[pull.offerId], completedOffer)) {
        completedOffer.finished++;
      }
    } on PlatformException {
      // The callback failed; this does not prove native cancellation or commit.
      outcome = 'clipboard_transfer_failed';
      observedTerminal = true;
      if (!_closed) {
        try {
          send({'type': 'clipboard.cancel', 'requestId': pull.requestId});
        } catch (_) {}
      }
    } finally {
      if (!_closed && observedTerminal) {
        if (outcome == null) {
          _receivingObservation.rangeEnded(pull.observation);
          _rangeEndedAt[pull.offerId] = DateTime.now();
        } else {
          _receivingObservation.failed(pull.observation, outcome);
        }
      }
      // Do not let a late callback clear somebody else's active operation.
      if (identical(_finishing, pull)) {
        if (identical(_active, pull)) _active = null;
        _finishing = null;
        if (!_closed && observedTerminal) {
          if (outcome != null && currentRevision()) {
            failed(outcome);
          }
          _startNext();
          _settled();
          changed();
        }
      }
    }
  }

  Future<void> _handle(Map<String, dynamic> value) async {
    final type = value['type'];
    final id = value['requestId'];
    if (type == 'clipboard.pull' || type == 'clipboard.manifest') {
      await _servePull(value);
      return;
    }
    // Deferred manifest: the answer to this side's list request, or the peer ending the list this side walks.
    final asked = _manifestIn;
    if (asked != null && asked.requestId == id) {
      await _manifestMessage(asked, value);
      return;
    }
    final walking = _manifestServe;
    if (walking != null && walking.requestId == id) {
      if (type != 'clipboard.cancel' && type != 'clipboard.error') throw const ClipboardPullViolation();
      endWalk(value);
      return;
    }
    final serve = _serve;
    if (serve != null && serve.requestId == id) {
      await _serveMessage(serve, value);
      return;
    }
    // A pull queue: the Host's pull waiting for its turn here was cancelled or failed before it started.
    if (_serveQueue.any((queued) => queued['requestId'] == id)) {
      if (type == 'clipboard.cancel' || type == 'clipboard.error') {
        _serveQueue.removeWhere((queued) => queued['requestId'] == id);
      }
      return;
    }
    final head = _active;
    if (head == null || head.requestId != id) {
      final at = _queue.indexWhere(
        (queued) => queued.sent && queued.requestId == id,
      );
      if (at < 0) {
        // What was still in flight when a pull ended (the relay drops it too).
        return;
      }
      final queued = _queue[at];
      if (type == 'clipboard.cancel' || type == 'clipboard.error') {
        // A pull of the queue the relay or the Host ended before its turn (busy, offer withdrawn): its range fails quietly; a reader
        // that asks for that file again pulls it anew.
        _queue.removeAt(at);
        _receivingObservation.cancelRange(queued.observation);
        try {
          await bridge.pullFail(
            queued.rangeId,
            type == 'clipboard.cancel'
                ? 'clipboard_cancelled'
                : _safe(value['reason']),
          );
        } catch (_) {}
        _startNext();
        _settled();
        changed();
        return;
      }
      // Data is served in order: it belongs to the oldest pull, which becomes the one received once the previous one is over.
      if (at != 0 || head != null || _finishing != null) {
        throw const ClipboardPullViolation();
      }
      _startNext();
      if (_active?.requestId != id) throw const ClipboardPullViolation();
    }
    final pull = _active!;
    // Two newer copies retire an older offer (only the latest two are kept) while a paste may still be pulling from it: that pull
    // ends as an expired offer, it must not take the whole clipboard channel down.
    if (_offers[pull.offerId] == null &&
        (type == 'clipboard.chunk' || type == 'clipboard.item.end')) {
      await _finishReceiving(
        pull,
        () => bridge.pullFail(pull.rangeId, 'clipboard_offer_expired'),
        reason: 'clipboard_offer_expired',
        cancelPeer: true,
      );
      return;
    }
    switch (type) {
      case 'clipboard.chunk':
        _fields(value, ['type', 'requestId', 'index', 'offset', 'data']);
        final offer = _offers[pull.offerId]!;
        if (value['index'] != pull.index ||
            value['offset'] != pull.next ||
            value['data'] is! String) {
          throw const ClipboardPullViolation();
        }
        final encoded = value['data'] as String;
        final chunk = limits().chunkBytes;
        if (encoded.length > (chunk + 2) ~/ 3 * 4) {
          throw const ClipboardPullViolation();
        }
        final bytes = base64Decode(encoded);
        if (bytes.isEmpty ||
            bytes.length > chunk ||
            // The pull-queue relay has already refused any chunk that is not canonical base64 (clipboard.mjs receivePull); a pair
            // without it keeps checking here.
            (!_queued && base64Encode(bytes) != encoded) ||
            bytes.length > offer.sizes[pull.index] - pull.next) {
          throw const ClipboardPullViolation();
        }
        final offset = pull.next;
        pull.next += bytes.length;
        offer.pulled += bytes.length;
        await bridge.pullData(pull.rangeId, offset, bytes);
        _receivingObservation.data(pull.observation);
        changed();
      case 'clipboard.item.end':
        _fields(value, ['type', 'requestId', 'index', 'sha256']);
        final offer = _offers[pull.offerId]!;
        final digest = value['sha256'];
        if (value['index'] != pull.index ||
            pull.next != offer.sizes[pull.index] ||
            digest is! String ||
            !RegExp(r'^[a-f0-9]{64}$').hasMatch(digest)) {
          throw const ClipboardPullViolation();
        }
        await _finishReceiving(
          pull,
          () => bridge.pullEnd(pull.rangeId, digest),
          completedOffer: offer,
        );
      case 'clipboard.error':
        _fields(value, ['type', 'requestId', 'reason']);
        final reason = _safe(value['reason']);
        await _finishReceiving(
          pull,
          () => bridge.pullFail(pull.rangeId, reason),
          reason: reason,
        );
      case 'clipboard.cancel':
        _fields(value, ['type', 'requestId']);
        await _finishReceiving(
          pull,
          () => bridge.pullFail(pull.rangeId, 'clipboard_cancelled'),
          reason: 'clipboard_cancelled',
        );
      default:
        throw const ClipboardPullViolation();
    }
  }

  // ---- Offering -----------------------------------------------------------------------------------

  Future<void> _servePull(Map<String, dynamic> value) async {
    final manifest = value['type'] == 'clipboard.manifest';
    if (manifest) {
      _fields(value, ['type', 'requestId', 'offerId']);
    } else {
      _fields(value, ['type', 'requestId', 'offerId', 'index', 'offset']);
    }
    final id = value['requestId'], offerId = value['offerId'];
    final index = value['index'], offset = value['offset'];
    if (id is! String ||
        !clipboardPullIdPattern.hasMatch(id) ||
        offerId is! String ||
        (!manifest && (index is! int || offset is! int || index < 0 || offset < 0)) ||
        (manifest && deferred == null) ||
        !limits().pull) {
      throw const ClipboardPullViolation();
    }
    // A pull queue: the Host asks for several files at once; they are served in order (a list request takes its turn like a pull).
    if ((_serve != null || _manifestServe != null) && _queued && _serveQueue.length < depth) {
      _serveQueue.add(value);
      return;
    }
    await _serveAny(value);
    // A queued pull that was refused (its file could not be opened) must not
    // hold up the ones behind it: they are answered in order, served or
    // refused, instead of waiting for a serve that never comes.
    while (_serve == null && _manifestServe == null && _serveQueue.isNotEmpty && !_closed) {
      await _serveAny(_serveQueue.removeAt(0));
    }
  }

  Future<void> _serveAny(Map<String, dynamic> value) => value['type'] == 'clipboard.manifest'
      ? _serveManifest(value['requestId'] as String, value['offerId'] as String)
      : _serveOne(
          value['requestId'] as String,
          value['offerId'] as String,
          value['index'] as int,
          value['offset'] as int,
        );

  /// The relay or the peer ends the list this side is walking ([value] a clipboard.cancel or clipboard.error of its request). The walk
  /// holds the pull lane until it returns, so the session calls this as the message arrives, ahead of the lane: the walk stops at its
  /// next step. True when [value] was that; anything else (and a walk already answered) goes through the lane as usual.
  bool endWalk(Map<String, dynamic>? value) {
    final walking = _manifestServe, type = value?['type'];
    if (walking == null || walking.cancelled || walking.done || value?['requestId'] != walking.requestId) return false;
    if (type != 'clipboard.cancel' && type != 'clipboard.error') return false;
    walking.cancelled = true;
    _sendingObservation.failed(walking.observation, type == 'clipboard.cancel' ? 'clipboard_cancelled' : _safe(value?['reason']));
    unawaited(deferred?.manifestCancel(owner, walking.requestId).catchError((Object _) {}));
    return true;
  }

  /// The peer pastes this side's deferred copy: walk it now and answer with the counts and the entry pages (or the reason it failed).
  Future<void> _serveManifest(String id, String offerId) async {
    final bridge = deferred!;
    final observation = _sendingObservation.request();
    void refuse(String reason) {
      _sendingObservation.failed(observation, reason);
      send({'type': 'clipboard.error', 'requestId': id, 'reason': reason});
    }

    if (!mayServe('tree')) return refuse('clipboard_policy_denied');
    if (_serve != null || _manifestServe != null) return refuse('clipboard_busy');
    final walking = _ManifestServe(id, offerId)..observation = observation;
    _manifestServe = walking;
    _serving = true;
    started();
    changed();
    String? failure;
    ClipboardManifest? manifest;
    try {
      manifest = await bridge.manifestWalk(owner, offerId, id);
    } on PlatformException catch (error) {
      failure = wireReason?.call(error.code, error.message) ?? error.code;
    }
    walking.done = true;
    try {
      if (walking.cancelled || !identical(_manifestServe, walking) || _closed) return;
      if (manifest != null) {
        // A list this side cannot send fails this paste only (as a copy that cannot be offered does), never the channel.
        try {
          validateTree?.call(manifest.items, manifest.tops);
        } on PlatformException catch (error) {
          failure = wireReason?.call(error.code, error.message) ?? error.code;
        } catch (_) {
          failure = 'clipboard_transfer_failed';
        }
      }
      if (failure != null || manifest == null) {
        final reason = _safe(failure);
        refuse(reason);
        _servingFailed(observation, reason);
        return;
      }
      final items = manifest.items;
      final files = [for (final item in items) if (!item.isDirectory) item];
      send({
        'type': 'clipboard.manifest.begin',
        'requestId': id,
        'offerId': offerId,
        'entries': items.length,
        'files': files.length,
        'bytes': files.fold<int>(0, (sum, item) => sum + item.size),
        'tops': manifest.tops,
        'skipped': manifest.skipped,
      });
      // Pages the relay and the peer accept: at most 200 entries or about 40 KB each.
      var start = 0, size = 0;
      var page = <Map<String, Object?>>[];
      void flush() {
        if (page.isEmpty) return;
        send({'type': 'clipboard.entries', 'requestId': id, 'start': start, 'entries': page});
        start += page.length;
        page = [];
        size = 0;
      }

      for (final item in items) {
        final entry = <String, Object?>{
          'path': item.name,
          'kind': item.kind,
          if (!item.isDirectory) 'size': item.size,
          'attrs': item.attributes,
          'mtime': item.mtime ?? 0,
        };
        page.add(entry);
        size += (item.name ?? '').length * 3 + 80;
        if (page.length >= 200 || size > 40000) flush();
      }
      flush();
      _sendingObservation.rangeEnded(observation);
      offerSent(offerId, items);
    } finally {
      if (identical(_manifestServe, walking)) {
        _manifestServe = null;
        _serving = false;
        _settled();
        changed();
      }
    }
  }

  Future<void> _serveOne(
    String id,
    String offerId,
    int index,
    int offset,
  ) async {
    final observation = _sendingObservation.request();
    void refuse(String reason) {
      _sendingObservation.failed(observation, reason);
      send({'type': 'clipboard.error', 'requestId': id, 'reason': reason});
    }

    if (!mayServe(_offers[offerId]?.format ?? 'tree')) {
      return refuse('clipboard_policy_denied');
    }
    if (!_offers.containsKey(offerId)) return refuse('clipboard_offer_expired');
    if (_serve != null) return refuse('clipboard_busy');
    int size;
    try {
      size = await bridge.pullOpen(owner, offerId, index, offset);
    } on PlatformException catch (failure) {
      final reason = _safe(failure.code);
      _servingFailed(observation, reason);
      return refuse(reason);
    }
    if (offset > size) {
      await bridge.pullClose(owner);
      return refuse('clipboard_transfer_failed');
    }
    final serve = _Serve(
      id,
      index,
      size,
      offset,
      _queued
          ? ClipboardSendWindow(
              maxUnits: clipboardPullQueueWindowUnits,
              maxBytes: clipboardPullQueueWindowBytes,
            )
          : ClipboardSendWindow.forLimits(limits()),
    );
    serve.observation = observation;
    _serve = serve;
    _lastServedOfferId = offerId;
    _serving = true;
    _servedOffer = _offers[offerId];
    started();
    changed();
    await _pumpServe(serve);
  }

  Future<void> _serveMessage(_Serve serve, Map<String, dynamic> value) async {
    switch (value['type']) {
      case 'clipboard.chunk.ack':
        _fields(value, ['type', 'requestId', 'index', 'offset']);
        if (value['index'] is! int || value['offset'] is! int) {
          throw const ClipboardPullViolation();
        }
        try {
          serve.window.ackChunk(value['index'] as int, value['offset'] as int);
        } on ClipboardWindowException {
          throw const ClipboardPullViolation();
        }
        await _pumpServe(serve);
      case 'clipboard.item.ack':
        _fields(value, ['type', 'requestId', 'index']);
      case 'clipboard.cancel' || 'clipboard.error':
        _sendingObservation.failed(
          serve.observation,
          value['type'] == 'clipboard.cancel'
              ? 'clipboard_cancelled'
              : _safe(value['reason']),
        );
        await _endServe(serve, closeNative: true);
      default:
        throw const ClipboardPullViolation();
    }
  }

  Future<void> _pumpServe(_Serve serve) async {
    if (serve.pumping) {
      serve.pumpAgain = true;
      return;
    }
    serve.pumping = true;
    try {
      do {
        serve.pumpAgain = false;
        await _sendServed(serve);
      } while (serve.pumpAgain && identical(_serve, serve));
    } finally {
      serve.pumping = false;
    }
  }

  Future<void> _sendServed(_Serve serve) async {
    final chunkBytes = limits().chunkBytes;
    while (identical(_serve, serve) && !_closed && !serve.ended) {
      if (serve.sent == serve.size) {
        if (!serve.window.canSendItemEnd) return;
        String digest;
        try {
          digest = await bridge.pullDigest(owner);
        } on PlatformException catch (failure) {
          await _failServe(serve, failure.code);
          return;
        }
        if (!identical(_serve, serve)) return;
        if (!RegExp(r'^[a-f0-9]{64}$').hasMatch(digest)) {
          await _failServe(serve, 'clipboard_transfer_failed');
          return;
        }
        send({
          'type': 'clipboard.item.end',
          'requestId': serve.requestId,
          'index': serve.index,
          'sha256': digest,
        });
        // The relay ends the pull at the item end; later acknowledgements are dropped.
        serve.ended = true;
        _sendingObservation.rangeEnded(serve.observation);
        await _endServe(serve, closeNative: true);
        return;
      }
      final count = min(chunkBytes, serve.size - serve.sent);
      if (!serve.window.canSendChunk(count)) return;
      if (serve.readAt == (serve.read?.length ?? 0)) {
        // One platform call for up to clipboardServeReadChunks chunks instead of a round trip per 45 KiB chunk.
        // Reading ahead of the window is fine: the window bounds what is sent, the platform hashes what it read.
        final want = min(chunkBytes * clipboardServeReadChunks, serve.size - serve.sent);
        Uint8List block;
        try {
          block = await bridge.pullRead(owner, want);
        } on PlatformException catch (failure) {
          await _failServe(serve, failure.code);
          return;
        }
        if (!identical(_serve, serve)) return;
        if (block.length != want) {
          await _failServe(serve, 'clipboard_selected_file_changed');
          return;
        }
        serve.read = block;
        serve.readAt = 0;
      }
      if (serve.read!.length - serve.readAt < count) {
        // Blocks are whole chunks until the file's last one, so this cannot happen; never send bytes out of place.
        await _failServe(serve, 'clipboard_transfer_failed');
        return;
      }
      final bytes = Uint8List.sublistView(
        serve.read!,
        serve.readAt,
        serve.readAt + count,
      );
      serve.readAt += count;
      final offset = serve.sent;
      send({
        'type': 'clipboard.chunk',
        'requestId': serve.requestId,
        'index': serve.index,
        'offset': offset,
        'data': base64Encode(bytes),
      });
      serve.sent += count;
      _sendingObservation.data(serve.observation);
      serve.window.sentChunk(serve.index, serve.sent, count);
      final offer = _servedOffer;
      if (offer != null) offer.pulled += count;
      changed();
    }
  }

  Future<void> _failServe(_Serve serve, String code) async {
    final reason = _safe(code);
    _sendingObservation.failed(serve.observation, reason);
    if (identical(_serve, serve)) {
      send({
        'type': 'clipboard.error',
        'requestId': serve.requestId,
        'reason': reason,
      });
    }
    _servingFailed(serve.observation, reason);
    await _endServe(serve, closeNative: true);
  }

  Future<void> _endServe(_Serve serve, {required bool closeNative}) async {
    if (!identical(_serve, serve)) return;
    serve.ended = true;
    _serve = null;
    _serving = false;
    if (closeNative) {
      try {
        await bridge.pullClose(owner);
      } catch (_) {}
    }
    final offer = _servedOffer;
    if (offer != null && serve.sent == serve.size) offer.finished++;
    _servedOffer = null;
    _settled();
    changed();
    // A pull queue: the Host's next pull, which waited for this one.
    if (_serveQueue.isNotEmpty && !_closed) {
      await _servePull(_serveQueue.removeAt(0));
    }
  }

  // ---- The user, the session ------------------------------------------------------------------------

  /// The user cancelled the transfer notice: the running pull and the queued ones end.
  Future<void> cancel() async {
    ++_operationOwnerRevision;
    final pull = _active;
    final queuedPulls = List<_Pull>.of(_queue);
    _queue.clear();
    final wasPasteWaiting = _pasteWaiting;
    _receivingObservation.cancel();
    _sendingObservation.cancel();
    for (final id in _operations.keys.toList()) {
      _retireOperation(id);
    }
    _pasteWaiting = false;
    _sendingObservation.waiting(false);
    final serve = _serve;
    final waitingOffer = _lastServedOfferId;
    // Between files there is no range id to cancel. Retire the source offer
    // through the existing on-paste withdrawal protocol so the Host ends its
    // blocked data object before it can ask for the next file. Recopy is needed
    // for another paste after this gap cancellation.
    if (onPaste != null &&
        serve == null &&
        wasPasteWaiting &&
        waitingOffer != null) {
      final retired = <String>{waitingOffer};
      try {
        retired.addAll(await onPaste!.dropOffers(owner));
      } catch (_) {}
      for (final id in retired) {
        _offers.remove(id);
        try {
          send({'type': 'clipboard.offer.withdraw', 'requestId': id});
        } catch (_) {}
      }
      _lastServedOfferId = null;
    }
    // A list asked for or being walked ends too.
    final asked = _manifestIn, walking = _manifestServe;
    _manifestIn = null;
    if (asked != null) {
      try {
        send({'type': 'clipboard.cancel', 'requestId': asked.requestId});
      } catch (_) {}
      try {
        await deferred?.manifestFail(owner, asked.offerId, 'clipboard_cancelled');
      } catch (_) {}
    }
    if (walking != null && !walking.cancelled) {
      walking.cancelled = true;
      try {
        send({'type': 'clipboard.error', 'requestId': walking.requestId, 'reason': 'clipboard_cancelled'});
      } catch (_) {}
      try {
        await deferred?.manifestCancel(owner, walking.requestId);
      } catch (_) {}
    }
    // What is blocked waiting for data in the platform ends first (a render between two pulls would otherwise start the next one).
    try {
      await onPaste?.pullAbort(owner);
    } catch (_) {}
    if (pull != null) {
      try {
        send({'type': 'clipboard.cancel', 'requestId': pull.requestId});
      } catch (_) {}
      if (identical(_active, pull) && !identical(_finishing, pull)) {
        _active = null;
        await bridge.pullFail(pull.rangeId, 'clipboard_cancelled');
      }
    }
    for (final queued in queuedPulls) {
      // A pull queue: one already asked for is cancelled at the relay and the Host as well.
      if (queued.sent) {
        try {
          send({'type': 'clipboard.cancel', 'requestId': queued.requestId});
        } catch (_) {}
      }
      await bridge.pullFail(queued.rangeId, 'clipboard_cancelled');
    }
    for (final queued in _serveQueue) {
      try {
        send({
          'type': 'clipboard.error',
          'requestId': queued['requestId'],
          'reason': 'clipboard_cancelled',
        });
      } catch (_) {}
    }
    _serveQueue.clear();
    if (serve != null) {
      try {
        send({
          'type': 'clipboard.error',
          'requestId': serve.requestId,
          'reason': 'clipboard_cancelled',
        });
      } catch (_) {}
      await _endServe(serve, closeNative: true);
    }
    _lingering = false;
    _lingerTimer?.cancel();
    changed();
  }

  /// The channel ended (the session closed, the permission was withdrawn).
  Future<void> close(String reason) async {
    if (_closed) return;
    _receivingObservation.close(_safe(reason));
    _sendingObservation.close(_safe(reason));
    _closed = true;
    ++_operationOwnerRevision;
    _operationRevisions.clear();
    _operations.clear();
    _operationPulled.clear();
    _operationFloors.clear();
    _receivedOffers.clear();
    _lastObservation.clear();
    _pasteWaiting = false;
    _rangeObservations.clear();
    _lingerTimer?.cancel();
    await _events?.cancel();
    _active = null;
    _queue.clear();
    _manifestIn = null;
    final walking = _manifestServe;
    walking?.cancelled = true;
    _manifestServe = null;
    _deferredOffers.clear();
    if (walking != null) {
      try {
        await deferred?.manifestCancel(owner, walking.requestId);
      } catch (_) {}
    }
    _serve = null;
    _serving = false;
    _lingering = false;
    try {
      await bridge.pullShutdown(owner, reason);
    } catch (_) {}
    try {
      await bridge.pullClose(owner);
      await bridge.closeOffers(owner);
    } catch (_) {}
  }

  String _safe(Object? reason) {
    if (reason is String && _pullWireReasons.contains(reason)) return reason;
    return 'clipboard_transfer_failed';
  }

  void _fields(Map<String, dynamic> value, List<String> fields) {
    if (value.length != fields.length || !fields.every(value.containsKey)) {
      throw const ClipboardPullViolation();
    }
  }
}
