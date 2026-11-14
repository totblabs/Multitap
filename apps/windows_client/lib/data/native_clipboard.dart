import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart';

class ClipboardItem {
  const ClipboardItem({
    required this.size,
    this.name,
    this.units,
    this.kind,
    this.width,
    this.height,
    this.attributes = 0,
    this.mtime,
  });
  final int size;

  /// A file name, or in a folder tree the relative path with '/' separators.
  final String? name;

  /// Text only: the length in UTF-16 units (a limit of released peers).
  final int? units;

  /// Formats profile: "text", "html", "rtf" (rich text), "png" (image), "file"
  /// or "dir" (folder tree entry). Null for plain text and flat file lists.
  final String? kind;

  /// Image only.
  final int? width, height;

  /// Folder tree entries: the Windows attribute bits that may travel
  /// (read-only 1, hidden 2, system 4, archive 32).
  final int attributes;

  /// Paste-pull offers: the last write time in milliseconds since 1970.
  final int? mtime;

  /// A folder: no data of its own.
  bool get isDirectory => kind == 'dir';
}

/// Local native snapshot identity. This is not a permission or a wire token;
/// never persist it or include it in relay messages or product logs.
class ClipboardSourceReceipt {
  const ClipboardSourceReceipt(this.owner, this.captureSerial);
  final String owner;
  final int captureSerial;

  bool get _valid =>
      owner.isNotEmpty &&
      owner.length <= 80 &&
      !RegExp(r'[^A-Za-z0-9_-]').hasMatch(owner) &&
      captureSerial > 0 &&
      captureSerial <= 0x7fffffffffffffff;
}

class ClipboardCapture {
  const ClipboardCapture({
    required this.format,
    required this.items,
    this.sequence,
    this.own = false,
    this.tops = 0,
    this.skipped = 0,
    this.offer = false,
    this.sourceReceipt,
    this.deferred = false,
  });
  final String format;

  /// A deferred offer (manifest-on-paste-v1): files were copied, and their list is walked only when the peer pastes. [items] is empty.
  final bool deferred;
  final List<ClipboardItem> items;
  final int? sequence;

  /// Folder trees: the number of top-level items, and how many links
  /// (symbolic links, junctions) were left out and not followed.
  final int tops, skipped;

  /// Only for a capture with skipOwn: the clipboard still holds the content
  /// this session received from its peer and published (our window owns it).
  /// No source was opened; format and items are empty.
  final bool own;

  /// The capture is a paste-pull offer: only the manifest is sent now, the native
  /// side keeps the files (by identity) and serves pulls until the offer is retired.
  final bool offer;

  /// Absent on legacy native adapters. Presence identifies the snapshot only;
  /// an own/offer capture still need not have an ordinary readable source.
  final ClipboardSourceReceipt? sourceReceipt;
}

/// Optional native support only. Session scheduling and cleanup remain on
/// their existing paths until an exact-receipt lifecycle is integrated.
/// The interface alone is not a capability check: use a receipt returned by
/// that exact capture; a legacy native adapter returns no receipt.
abstract interface class ClipboardSourceBridge {
  Future<bool> retireSource(ClipboardSourceReceipt source);
  Future<Uint8List> sourceChunkScoped(
    ClipboardSourceReceipt source,
    int index,
    int offset,
    int length,
  );
  Future<String> sourceDigestScoped(ClipboardSourceReceipt source, int index);
}

class ClipboardChange {
  const ClipboardChange(this.owner, this.sequence);
  final String owner;
  final int sequence;
}

/// Observation exposes only a sequence number until the authorized session
/// decides to capture. Existing non-observing fixtures keep the manual contract.
abstract interface class ClipboardObservationBridge {
  Stream<ClipboardChange> get changes;
  Future<int> watch(String owner, bool enabled);
  int? get lastCommittedSequence;
}

/// [ClipboardObservationBridge.watch] switched on, plus whether the clipboard
/// then held this client's own publication (the VM's copy, or a placeholder
/// for it). Change notifications already leave those out; the sequence a
/// watch starts from would otherwise be taken for a copy made on this PC.
abstract interface class ClipboardWatchOwnership {
  Future<({int sequence, bool own})> watchOwned(String owner);
}

/// Diagnostics after publishing received files: fixed fields, never paths or
/// content. [lastPublishCheck] describes the last files commit: whether the
/// desktop (Explorer) user could open every published file, the first Windows
/// error, and flags (1 elevated client, 2 desktop user is another account,
/// 4 no desktop token to test with, 8 a path of MAX_PATH or longer).
abstract interface class ClipboardPublishDiagnostics {
  ({bool readable, int error, int flags})? get lastPublishCheck;

  /// Whether this owner's published content still owns the clipboard.
  Future<({bool owned, int sequence})> published(String owner);
}

/// A shared publication getter may be consumed only for this exact result.
/// Callers check this and read the getters in one synchronous block.
abstract interface class ClipboardPublicationBridge {
  bool publicationMatches(
    String owner,
    String requestId, {
    ClipboardPendingLease? lease,
  });
}

/// A bridge that can capture and publish rich text, images and folder trees
/// (the formats profile). The Windows adapter does; the Android one announces
/// it for the on-paste pair and keeps only the plain text of rich text. The
/// session announces the profile to the relay only for such a bridge.
abstract interface class ClipboardFormatsBridge {
  bool get supportsFormats;

  /// Whether the negotiated pair allows the formats: capture then returns
  /// them; otherwise plain text and flat file lists only.
  set formats(bool value);
}

/// A bridge that can take part in the scale revision (no size, total or
/// top-level-count limits, 64 MiB of text and HTML, 259-character pasteable
/// paths): it reads the rich-text companion up to the pair's limit and walks
/// paths of any length. Only the Windows adapter does.
abstract interface class ClipboardScaleBridge {
  bool get supportsScale;

  /// Whether the negotiated pair is of the scale revision.
  set scale(bool value);

  /// The largest HTML companion of a text that is read (8 MiB for released
  /// pairs, 64 MiB with the scale revision).
  set richBytes(int value);
}

/// What the paste-pull profile needs from the platform. Receiving: the offer's
/// virtual files are published on the clipboard, a paste makes the platform ask for
/// ranges ([pullEvents]), and the bytes the peer sends are handed back with
/// [pullData] / [pullEnd] / [pullFail]. Offering: the platform reads a file at an
/// offset on request ([pullOpen] / [pullRead] / [pullDigest] / [pullClose]).
abstract interface class ClipboardPullBridge {
  bool get supportsPull;

  /// Whether the negotiated pair allows pulling: capture then offers large
  /// file copies; otherwise they are pushed.
  set pull(bool value);
  Stream<ClipboardPullEvent> get pullEvents;

  /// Like [ClipboardBridge.capture]; a copy of more than 16 MiB or 256 entries of
  /// files and folders comes back as an offer ([ClipboardCapture.offer]) that the
  /// platform keeps under [offerId]. The result is the same as a capture otherwise.
  Future<ClipboardCapture> captureWithOffer(
    String owner,
    String offerId, {
    bool skipOwn = false,
  });
  Future<void> publishOffer(
    String owner,
    String offerId,
    List<ClipboardItem> entries,
  );
  Future<void> pullData(int rangeId, int offset, Uint8List bytes);
  Future<void> pullEnd(int rangeId, String digest);
  Future<void> pullFail(int rangeId, String reason);

  /// The session ended or lost its permission: everything pending fails.
  Future<void> pullShutdown(String owner, String reason);
  Future<int> pullOpen(String owner, String offerId, int index, int offset);
  Future<Uint8List> pullRead(String owner, int length);
  Future<String> pullDigest(String owner);
  Future<void> pullClose(String owner);

  /// The session ended: the offers this peer kept are retired.
  Future<void> closeOffers(String owner);
}

/// The on-paste revision (a copy is silent, data moves when pasted). Only the Windows adapter has it; the session announces
/// the header only for a bridge that does. [onPaste] is set once the relay confirmed the revision for the pair: [captureWithOffer] then
/// offers every file copy and the big text / rich text / image copies instead of pushing them, and [publishOfferOnPaste] publishes the
/// peer's offer as a data object that shows no Explorer dialog of its own and renders CF_HDROP / text / images on demand.
/// The relay's pull queue (clipboardPullQueueProtocol): the platform asks for up to [pullDepth] files ahead of their reader, and the
/// session sends those pulls at once. Only the Windows adapter (native/windows_clipboard) can.
abstract interface class ClipboardPullQueueBridge {
  bool get supportsPullQueue;
  set pullDepth(int value);
}

/// The list of a deferred copy, walked when the peer pastes it ([ClipboardDeferredBridge.manifestWalk]).
class ClipboardManifest {
  const ClipboardManifest({required this.items, required this.tops, required this.skipped});
  final List<ClipboardItem> items;
  final int tops, skipped;
}

/// Deferred manifest (manifest-on-paste-v1, like RDP): a copy of files that is not small is announced without its list; the list is
/// walked when the peer pastes. Only the Windows adapter can. [deferred] is set once the relay confirmed it for the pair: capture then
/// returns such copies as [ClipboardCapture.deferred].
abstract interface class ClipboardDeferredBridge {
  bool get supportsDeferred;
  set deferred(bool value);

  /// Publishes the peer's deferred offer: the data object asks for the list (a [ClipboardPullManifest] event) when something pastes.
  Future<void> publishDeferredOffer(String owner, String offerId);

  /// The peer pastes this side's deferred copy [offerId]: walks it now (progress arrives as [ClipboardPullManifestProgress] events) and
  /// returns its list. Fails with clipboard_offer_expired when the clipboard no longer holds that copy. [walk] is the peer's request id.
  Future<ClipboardManifest> manifestWalk(String owner, String offerId, String walk);

  /// The walk for request [walk] is no longer wanted (also when it has not started yet).
  Future<void> manifestCancel(String owner, String walk);

  /// The peer's answer to this side's list request for its deferred offer [offerId].
  Future<void> manifestDeliver(String owner, String offerId, List<ClipboardItem> entries);
  Future<void> manifestFail(String owner, String offerId, String reason);
  Future<void> manifestProgress(String owner, String offerId);
}

abstract interface class ClipboardOnPasteBridge {
  bool get supportsOnPaste;
  set onPaste(bool value);

  /// Publishes the peer's offer. [format] is `tree` (entries with names, attributes and write times), `text`, `richtext` or `image` (items).
  Future<void> publishOfferOnPaste(
    String owner,
    String offerId,
    String format,
    List<ClipboardItem> entries,
  );

  /// This side's clipboard changed: the offers it kept are no longer served. Returns their ids (the session withdraws them).
  Future<List<String>> dropOffers(String owner);

  /// The user cancelled the paste: what is blocked waiting for data ends (E_ABORT for the application) and the streams of that paste
  /// are refused for a moment. Not queued behind a reader that waits.
  Future<void> pullAbort(String owner);

  /// A newer copy is about to replace the clipboard: blocked renders of the older offers end. Not queued.
  Future<void> pullSupersede(String owner);

  /// The peer withdrew [offerId]: whatever reads it fails now (not queued); [withdrawOffer] then clears the clipboard if it still holds it.
  Future<void> pullRetire(String owner, String offerId);
  Future<void> withdrawOffer(String owner, String offerId);
}

/// Native-only lease of the exact clipboard source/watch generation.
class ClipboardPendingLease {
  const ClipboardPendingLease({
    required this.token,
    required this.sequence,
    required this.sourceSequence,
    required this.kind,
    required this.epoch,
  });
  final String token, kind;
  final int sequence, sourceSequence, epoch;
}

abstract interface class ClipboardPendingBridge {
  bool get supportsPending;
  Future<int> observePendingSequence(String owner, {required int epoch});
  Future<ClipboardPendingLease?> beginPending(
    String owner, {
    required int expectedLocalSequence,
    required int sourceSequence,
    required String kind,
    required int epoch,
  });
  Future<bool> bindPending(
    String owner,
    ClipboardPendingLease lease,
    String requestId,
  );
  Future<void> cancelPending(String owner, ClipboardPendingLease lease);
  Future<void> cancelPendingOwner(String owner, int epoch);
  Future<void> allowManualRequest(String owner, String requestId);
}

/// Claiming the clipboard for a copy that is still arriving (see ClipboardSession).
abstract interface class ClipboardClaimBridge {
  /// Empties the clipboard under this client's ownership if it still holds what it held when
  /// the transfer [requestId] began. The new sequence, or null when nothing was claimed
  /// (someone copied since, the clipboard is busy).
  Future<int?> claim(String owner, String requestId);
}

/// A request from the platform's data object (an Explorer paste reading a file).
sealed class ClipboardPullEvent {
  const ClipboardPullEvent(this.rangeId);
  final int rangeId;
}

class ClipboardPullRequest extends ClipboardPullEvent {
  const ClipboardPullRequest(
    super.rangeId,
    this.offerId,
    this.index,
    this.offset,
  );

  /// Ask for file [index] (ordinal among files) of [offerId] from [offset] on.
  final String offerId;
  final int index, offset;
}

class ClipboardPullCancel extends ClipboardPullEvent {
  const ClipboardPullCancel(super.rangeId);
}

/// The reader consumed whole chunks up to [end] of file [index].
class ClipboardPullAck extends ClipboardPullEvent {
  const ClipboardPullAck(super.rangeId, this.index, this.end);
  final int index, end;
}

/// The consumer of the virtual files started or finished an asynchronous operation
/// (IDataObjectAsyncCapability::StartOperation / EndOperation). Whether Windows
/// shows progress depends on the negotiated on-paste revision.
class ClipboardPullOperation extends ClipboardPullEvent {
  const ClipboardPullOperation(
    this.started, {
    required this.offerId,
    int operationId = 1,
    this.failureCode,
  }) : super(operationId);
  final bool started;
  final String offerId;
  final String? failureCode;
}

/// The platform gave up on a range itself (timeout, hash mismatch, bad data).
class ClipboardPullFailed extends ClipboardPullEvent {
  const ClipboardPullFailed(super.rangeId, this.index, this.reason);
  final int index;
  final String reason;
}

/// Something pastes the peer's deferred offer [offerId]: ask the peer for its list.
class ClipboardPullManifest extends ClipboardPullEvent {
  const ClipboardPullManifest(this.offerId) : super(0);
  final String offerId;
}

/// The list request for [offerId] is no longer wanted: [reason] clipboard_cancelled (the paste gave up, a newer copy) or
/// clipboard_timeout (the peer was silent too long: the paste failed).
class ClipboardPullManifestCancel extends ClipboardPullEvent {
  const ClipboardPullManifestCancel(this.offerId, [this.reason = 'clipboard_cancelled']) : super(0);
  final String offerId;
  final String reason;
}

/// This side walks its deferred copy [offerId] for the peer's paste: [entries] walked so far.
class ClipboardPullManifestProgress extends ClipboardPullEvent {
  const ClipboardPullManifestProgress(this.offerId, this.entries) : super(0);
  final String offerId;
  final int entries;
}

abstract interface class ClipboardBridge {
  /// [skipOwn]: automatic sharing; report [ClipboardCapture.own] instead of
  /// opening sources when the content is this session's own received copy.
  Future<ClipboardCapture> capture(String owner, {bool skipOwn = false});
  Future<Uint8List> sourceChunk(
    String owner,
    int index,
    int offset,
    int length,
  );
  Future<String> sourceDigest(String owner, int index);
  Future<void> begin(
    String owner,
    String requestId,
    String format,
    List<ClipboardItem> items,
  );
  Future<void> append(
    String owner,
    String requestId,
    int index,
    int offset,
    Uint8List bytes,
  );
  Future<void> finish(String owner, String requestId, int index, String digest);

  /// [unlessSequence]: the received bytes equal what this session sent from
  /// the clipboard at that sequence; when the clipboard has not changed since,
  /// it already holds them, so the native side discards the copy instead of
  /// replacing the clipboard.
  Future<void> commit(String owner, String requestId, {int? unlessSequence});
  Future<void> cancel(String owner);
  Future<void> release(String owner);
}

class _PublicationTicket {
  _PublicationTicket(this.owner, this.request, {this.epoch, this.lease});
  final String owner;
  final String? request;
  final int? epoch;
  ClipboardPendingLease? lease;
  bool invalid = false;
}

class _PendingBindTicket {
  _PendingBindTicket(this.request, this.lease);
  final String request;
  final ClipboardPendingLease lease;
}

class NativeClipboardBridge
    implements
        ClipboardBridge,
        ClipboardSourceBridge,
        ClipboardObservationBridge,
        ClipboardPublishDiagnostics,
        ClipboardPublicationBridge,
        ClipboardFormatsBridge,
        ClipboardScaleBridge,
        ClipboardPullBridge,
        ClipboardOnPasteBridge,
        ClipboardPullQueueBridge,
        ClipboardDeferredBridge,
        ClipboardClaimBridge,
        ClipboardPendingBridge,
        ClipboardWatchOwnership {
  NativeClipboardBridge({
    this.allowedText = false,
    this.allowedFiles = false,
    this.announceOnPaste = true,
    bool? android,
  }) : android = android ?? Platform.isAndroid {
    if (!_installed) {
      _installed = true;
      _channel.setMethodCallHandler((call) async {
        if (call.method == 'pull') {
          final event = _pullEvent(call.arguments);
          if (event != null) _pulls.add(event);
          return;
        }
        if (call.method != 'changed') return;
        final args = call.arguments;
        if (args is Map &&
            args['owner'] is String &&
            _validSequence(args['sequence'])) {
          _changes.add(
            ClipboardChange(args['owner'] as String, args['sequence'] as int),
          );
        }
      });
    }
  }
  // One exact binding per owner; keeping its terminal token prevents late
  // old chunks from falling back to the ordinary clipboard publisher.
  final _pendingEpochs = <String, int>{},
      _closedPendingEpochs = <String, int>{};
  final _pendingBindings =
      <String, ({String request, ClipboardPendingLease lease, Object stamp})>{};
  final _pendingObservations = <String, ({int epoch, Object stamp})>{};
  final _pendingBindHeads = <String, _PendingBindTicket>{};
  _PublicationTicket? _publicationHead, _lastPublication;
  final _ordinaryRequests = <String, String>{},
      _manualRequests = <String, String>{};

  /// The Android adapter (AndroidClipboard.kt, text only). It negotiates the
  /// same on-paste pair as Windows so that a VM copy can reach the phone: on
  /// that pair a VM copy is read only after the Host announced it settled,
  /// as text or an image, never files. It publishes no offers
  /// and has no pending placeholder or pull queue.
  final bool android;
  bool get _windows => !android && Platform.isWindows;
  @override
  bool get supportsPending => _windows;
  bool _pendingLive(String owner, int epoch) =>
      _pendingEpochs[owner] == epoch &&
      (_pendingObservations[owner]?.epoch ?? epoch) == epoch &&
      epoch > (_closedPendingEpochs[owner] ?? 0);

  static bool _sameLease(ClipboardPendingLease? a, ClipboardPendingLease? b) =>
      identical(a, b) ||
      (a != null &&
          b != null &&
          a.token == b.token &&
          a.epoch == b.epoch &&
          a.sourceSequence == b.sourceSequence &&
          a.sequence == b.sequence &&
          a.kind == b.kind);

  bool _bindingMatches(
    String owner,
    String request,
    ClipboardPendingLease lease,
  ) {
    final binding = _pendingBindings[owner];
    return binding != null &&
        binding.request == request &&
        identical(binding.stamp, _pendingBindHeads[owner]) &&
        _sameLease(binding.lease, lease) &&
        _pendingLive(owner, lease.epoch);
  }

  _PublicationTicket _startPublication(
    String owner,
    String? request, {
    int? epoch,
    ClipboardPendingLease? lease,
  }) => _publicationHead = _PublicationTicket(
    owner,
    request,
    epoch: epoch,
    lease: lease,
  );

  bool _publicationCurrent(_PublicationTicket ticket) =>
      identical(ticket, _publicationHead) &&
      !ticket.invalid &&
      (ticket.epoch == null || _pendingLive(ticket.owner, ticket.epoch!)) &&
      (ticket.request == null ||
          ticket.lease == null ||
          _bindingMatches(ticket.owner, ticket.request!, ticket.lease!));

  @override
  bool publicationMatches(
    String owner,
    String requestId, {
    ClipboardPendingLease? lease,
  }) {
    final result = _lastPublication;
    return result != null &&
        _publicationCurrent(result) &&
        result.owner == owner &&
        result.request == requestId &&
        _sameLease(result.lease, lease);
  }

  void _invalidatePendingLease(String owner, ClipboardPendingLease lease) {
    final binding = _pendingBindHeads[owner];
    if (binding != null && _sameLease(binding.lease, lease)) {
      _pendingBindHeads.remove(owner);
    }
    final publication = _publicationHead;
    if (publication != null &&
        publication.owner == owner &&
        _sameLease(publication.lease, lease)) {
      publication.invalid = true;
    }
  }

  void _invalidatePendingEpoch(String owner, int epoch) {
    final binding = _pendingBindHeads[owner];
    if (binding != null && binding.lease.epoch <= epoch) {
      _pendingBindHeads.remove(owner);
    }
    final publication = _publicationHead;
    if (publication != null &&
        publication.owner == owner &&
        publication.epoch != null &&
        publication.epoch! <= epoch) {
      publication.invalid = true;
    }
  }

  Map<String, dynamic> _leaseFields(ClipboardPendingLease lease) => {
    'pendingToken': lease.token,
    'pendingSequence': lease.sequence,
    'pendingSourceSequence': lease.sourceSequence,
    'pendingEpoch': lease.epoch,
    'pendingKind': lease.kind,
  };
  Map<String, dynamic> _pendingFields(
    String owner,
    String request, {
    String? format,
  }) {
    final binding = _pendingBindings[owner];
    if (binding != null && binding.request == request) {
      if (!_bindingMatches(owner, request, binding.lease)) {
        throw PlatformException(code: 'clipboard_changed');
      }
      return _leaseFields(binding.lease);
    }
    if (format == 'files' || format == 'tree') {
      _ordinaryRequests[owner] = request;
      return const {};
    }
    if (format != null && _manualRequests[owner] == request) {
      _manualRequests.remove(owner);
      _ordinaryRequests[owner] = request;
      return const {};
    }
    if (format == null && _ordinaryRequests[owner] == request) return const {};
    if (_pendingEpochs.containsKey(owner)) {
      throw PlatformException(code: 'clipboard_changed');
    }
    return const {};
  }

  @override
  Future<int> observePendingSequence(String owner, {required int epoch}) async {
    if (epoch <= 0 ||
        epoch <= (_closedPendingEpochs[owner] ?? 0) ||
        epoch < (_pendingEpochs[owner] ?? 0) ||
        epoch < (_pendingObservations[owner]?.epoch ?? 0) ||
        (!_pendingEpochs.containsKey(owner) && _pendingEpochs.length >= 64)) {
      throw PlatformException(code: 'clipboard_changed');
    }
    final observation = Object();
    _pendingObservations[owner] = (epoch: epoch, stamp: observation);
    _invalidatePendingEpoch(owner, epoch);
    final result = await _channel.invokeMethod<int>('observePendingSequence', {
      'owner': owner,
      'epoch': epoch,
    });
    if (epoch <= (_closedPendingEpochs[owner] ?? 0) ||
        epoch < (_pendingEpochs[owner] ?? 0) ||
        !identical(_pendingObservations[owner]?.stamp, observation)) {
      throw PlatformException(code: 'clipboard_changed');
    }
    if (!_validSequence(result) || result == 0) {
      throw PlatformException(code: 'clipboard_busy', message: 'clipboard_sequence_zero');
    }
    _pendingEpochs[owner] = epoch;
    return result!;
  }

  @override
  Future<ClipboardPendingLease?> beginPending(
    String owner, {
    required int expectedLocalSequence,
    required int sourceSequence,
    required String kind,
    required int epoch,
  }) async {
    if (!_pendingLive(owner, epoch) ||
        !_validSequence(expectedLocalSequence) ||
        !_validSequence(sourceSequence) ||
        !const {'text', 'richtext', 'image'}.contains(kind)) {
      throw PlatformException(code: 'clipboard_changed');
    }
    _pendingBindHeads.remove(owner);
    final publication = _startPublication(owner, null, epoch: epoch);
    final value = await _channel.invokeMapMethod<String, dynamic>(
      'beginPending',
      {
        'owner': owner,
        'epoch': epoch,
        'expectedLocalSequence': expectedLocalSequence,
        'sourceSequence': sourceSequence,
        'kind': kind,
      },
    );
    if (value == null) return null;
    final token = value['token'], sequence = value['sequence'];
    if (token is! String ||
        !RegExp(r'^[A-Za-z0-9_-]{1,80}$').hasMatch(token) ||
        !_validSequence(sequence) ||
        sequence == 0 ||
        value['epoch'] != epoch ||
        value['sourceSequence'] != sourceSequence ||
        value['kind'] != kind) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    final lease = ClipboardPendingLease(
      token: token,
      sequence: sequence as int,
      sourceSequence: sourceSequence,
      kind: kind,
      epoch: epoch,
    );
    if (!_pendingLive(owner, epoch) || !_publicationCurrent(publication)) {
      await cancelPending(owner, lease);
      return null;
    }
    publication.lease = lease;
    _lastCommittedSequence = lease.sequence;
    _lastPublishCheck = null;
    _lastPublication = publication;
    return lease;
  }

  @override
  Future<bool> bindPending(
    String owner,
    ClipboardPendingLease lease,
    String requestId,
  ) async {
    if (!_pendingLive(owner, lease.epoch)) return false;
    // Native PendingClipboard binds a lease only once. A repeated attempt
    // must not replace the head of an accepted or still-pending first bind.
    final previous = _pendingBindHeads[owner];
    if (previous != null && _sameLease(previous.lease, lease)) return false;
    final binding = _PendingBindTicket(requestId, lease);
    _pendingBindHeads[owner] = binding;
    final bound = await _channel.invokeMethod<bool>('bindPending', {
      'owner': owner,
      'requestId': requestId,
      ..._leaseFields(lease),
    });
    if (bound != true) return false;
    if (!_pendingLive(owner, lease.epoch) ||
        !identical(_pendingBindHeads[owner], binding)) {
      await cancelPending(owner, lease);
      return false;
    }
    _pendingBindings[owner] = (
      request: requestId,
      lease: lease,
      stamp: binding,
    );
    return true;
  }

  @override
  Future<void> cancelPending(String owner, ClipboardPendingLease lease) {
    _invalidatePendingLease(owner, lease);
    return _channel.invokeMethod<void>('cancelPending', {
      'owner': owner,
      ..._leaseFields(lease),
    });
  }

  @override
  Future<void> cancelPendingOwner(String owner, int epoch) async {
    if (epoch <= 0) throw PlatformException(code: 'clipboard_changed');
    final previous = _closedPendingEpochs[owner] ?? 0;
    if (epoch > previous) _closedPendingEpochs[owner] = epoch;
    _invalidatePendingEpoch(owner, epoch);
    await _channel.invokeMethod<void>('cancelPendingOwner', {
      'owner': owner,
      'epoch': epoch,
    });
  }

  @override
  Future<void> allowManualRequest(String owner, String requestId) async {
    if (!RegExp(r'^[A-Za-z0-9_-]{1,80}$').hasMatch(requestId) ||
        (_pendingEpochs.containsKey(owner) &&
            (_closedPendingEpochs[owner] ?? 0) <
                (_pendingEpochs[owner] ?? 0)) ||
        _pendingBindings[owner]?.request == requestId) {
      throw PlatformException(code: 'clipboard_changed');
    }
    // A local explicit receive action owns this exact new request only. This
    // is never inferred from unknown/late automatic metadata or a new ID.
    _manualRequests[owner] = requestId;
  }

  bool allowedText, allowedFiles;

  /// Compatibility harnesses may exercise a peer that predates the on-paste revision.
  final bool announceOnPaste;
  @override
  bool get supportsFormats => _windows || android;
  @override
  bool formats = false;
  @override
  bool get supportsPull => _windows || android;
  @override
  bool pull = false;
  @override
  bool get supportsScale => _windows || android;
  @override
  bool scale = false;
  @override
  int richBytes = 8 * 1024 * 1024;
  @override
  bool get supportsOnPaste => (_windows || android) && announceOnPaste;
  @override
  bool onPaste = false;
  @override
  bool get supportsPullQueue => _windows && supportsOnPaste;
  @override
  bool get supportsDeferred => _windows && supportsOnPaste;
  @override
  bool deferred = false;

  @override
  Future<void> publishDeferredOffer(String owner, String offerId) async {
    final publication = _startPublication(owner, offerId);
    final result = await _channel.invokeMapMethod<String, dynamic>('publishOffer', {
      'owner': owner,
      'offerId': offerId,
      'format': 'tree',
      'deferred': true,
      'ownProgress': true,
      if (pullDepth > 1) 'prefetchDepth': pullDepth,
    });
    if (!_publicationCurrent(publication)) return;
    final sequence = result?['sequence'];
    _lastCommittedSequence = _validSequence(sequence) ? sequence as int : null;
    _lastPublishCheck = null;
    _lastPublication = publication;
  }

  @override
  Future<ClipboardManifest> manifestWalk(String owner, String offerId, String walk) async {
    final result = await _channel.invokeMapMethod<String, dynamic>('manifestWalk', {'owner': owner, 'offerId': offerId, 'walk': walk});
    final items = result?['items'], tops = result?['tops'], skipped = result?['skipped'];
    if (items is! List || tops is! int || skipped is! int) throw PlatformException(code: 'clipboard_transfer_failed');
    return ClipboardManifest(
      tops: tops,
      skipped: skipped,
      items: [
        for (final e in items)
          ClipboardItem(
            size: (e as Map)['size'] as int,
            name: e['name'] as String?,
            kind: e['kind'] as String?,
            attributes: e['attrs'] is int ? e['attrs'] as int : 0,
            mtime: e['mtime'] is int ? e['mtime'] as int : null,
          ),
      ],
    );
  }

  @override
  Future<void> manifestCancel(String owner, String walk) =>
      _channel.invokeMethod<void>('manifestCancel', {'owner': owner, 'walk': walk});

  @override
  Future<void> manifestDeliver(String owner, String offerId, List<ClipboardItem> entries) =>
      _channel.invokeMethod<void>('manifestDeliver', {
        'owner': owner,
        'offerId': offerId,
        'items': [
          for (final item in entries)
            {'size': item.size, 'name': item.name, 'kind': item.kind, 'attrs': item.attributes, 'mtime': item.mtime ?? 0},
        ],
      });

  @override
  Future<void> manifestFail(String owner, String offerId, String reason) =>
      _channel.invokeMethod<void>('manifestFail', {'owner': owner, 'offerId': offerId, 'reason': reason});

  @override
  Future<void> manifestProgress(String owner, String offerId) =>
      _channel.invokeMethod<void>('manifestProgress', {'owner': owner, 'offerId': offerId});

  /// Files asked for ahead of their reader in the offers published from now on: 1, or the relay's pull queue depth.
  @override
  int pullDepth = 1;
  static final _pulls = StreamController<ClipboardPullEvent>.broadcast(
    sync: true,
  );
  @override
  Stream<ClipboardPullEvent> get pullEvents => _pulls.stream;
  static ClipboardPullEvent? _pullEvent(Object? arguments) {
    if (arguments is! Map) return null;
    final kind = arguments['kind'], range = arguments['rangeId'];
    if (kind is! String || range is! int) return null;
    int? number(String key) {
      final value = arguments[key];
      return value is int && value >= 0 ? value : null;
    }

    switch (kind) {
      case 'operation':
        final phase = arguments['reason'];
        final offer = arguments['offerId'];
        if (offer is! String || offer.isEmpty || range <= 0) return null;
        final failure = switch (phase) {
          'end-cancelled' => 'clipboard_cancelled',
          'end-failed' => 'clipboard_transfer_failed',
          _ => null,
        };
        if (phase != 'start' && phase != 'end' && failure == null) return null;
        return ClipboardPullOperation(
          phase == 'start',
          offerId: offer,
          operationId: range,
          failureCode: failure,
        );
      case 'request':
        final offer = arguments['offerId'], index = number('index');
        final offset = number('offset');
        if (offer is! String || index == null || offset == null) return null;
        return ClipboardPullRequest(range, offer, index, offset);
      case 'cancel':
        return ClipboardPullCancel(range);
      case 'ack':
        final index = number('index'), end = number('end');
        if (index == null || end == null) return null;
        return ClipboardPullAck(range, index, end);
      case 'failed':
        final index = number('index'), reason = arguments['reason'];
        if (index == null || reason is! String) return null;
        return ClipboardPullFailed(range, index, reason);
      case 'manifest' || 'manifestCancel' || 'manifestProgress':
        final offer = arguments['offerId'];
        if (offer is! String || offer.isEmpty) return null;
        if (kind == 'manifest') return ClipboardPullManifest(offer);
        if (kind == 'manifestCancel') {
          return ClipboardPullManifestCancel(
            offer,
            arguments['reason'] == 'clipboard_timeout' ? 'clipboard_timeout' : 'clipboard_cancelled',
          );
        }
        final entries = number('index');
        if (entries == null) return null;
        return ClipboardPullManifestProgress(offer, entries);
    }
    return null;
  }

  /// What this PC may send now, as the relay's ready says (set, not narrowed: an administrator may widen it).
  void allowFormats({required bool text, required bool files}) {
    allowedText = text;
    allowedFiles = files;
  }

  static bool _installed = false;
  static final _changes = StreamController<ClipboardChange>.broadcast(
    sync: true,
  );
  static bool _validSequence(Object? value) =>
      value is int && value >= 0 && value <= 0xffffffff;
  static const _channel = MethodChannel('multitap/clipboard');
  @override
  Stream<ClipboardChange> get changes => _changes.stream;
  int? _lastCommittedSequence;
  @override
  int? get lastCommittedSequence => _lastCommittedSequence;
  @override
  Future<int> watch(String owner, bool enabled) async {
    final result = await _channel.invokeMapMethod<String, dynamic>('watch', {
      'owner': owner,
      'enabled': enabled,
    });
    final sequence = result?['sequence'];
    if (!_validSequence(sequence)) {
      throw PlatformException(code: 'clipboard_watch_unavailable');
    }
    return sequence as int;
  }

  @override
  Future<({int sequence, bool own})> watchOwned(String owner) async {
    final result = await _channel.invokeMapMethod<String, dynamic>('watch', {
      'owner': owner,
      'enabled': true,
    });
    final sequence = result?['sequence'];
    if (!_validSequence(sequence)) {
      throw PlatformException(code: 'clipboard_watch_unavailable');
    }
    return (sequence: sequence as int, own: result?['own'] == true);
  }

  @override
  Future<ClipboardCapture> capture(String owner, {bool skipOwn = false}) =>
      _capture(owner, skipOwn: skipOwn);

  @override
  Future<ClipboardCapture> captureWithOffer(
    String owner,
    String offerId, {
    bool skipOwn = false,
  }) => _capture(owner, skipOwn: skipOwn, offerId: offerId);

  Future<ClipboardCapture> _capture(
    String owner, {
    bool skipOwn = false,
    String? offerId,
  }) async {
    final result = (await _channel.invokeMapMethod<String, dynamic>('capture', {
      'owner': owner,
      'allowedText': allowedText,
      'allowedFiles': allowedFiles,
      if (skipOwn) 'skipOwn': true,
      if (formats) 'formats': true,
      if (pull) 'pull': true,
      if (onPaste) 'onPaste': true,
      if (deferred && offerId != null) 'deferred': true,
      if (formats) 'richBytes': richBytes,
      'offerId': ?offerId,
    }))!;
    final sourceReceipt = _sourceReceipt(result, owner);
    if (skipOwn && result['own'] == true) {
      return ClipboardCapture(
        format: '',
        items: const [],
        sequence: _validSequence(result['sequence'])
            ? result['sequence'] as int
            : null,
        own: true,
        sourceReceipt: sourceReceipt,
      );
    }
    return ClipboardCapture(
      format: result['format'] as String,
      sourceReceipt: sourceReceipt,
      sequence: _validSequence(result['sequence'])
          ? result['sequence'] as int
          : null,
      tops: result['tops'] is int ? result['tops'] as int : 0,
      skipped: result['skipped'] is int ? result['skipped'] as int : 0,
      offer: result['offer'] == true,
      deferred: result['deferred'] == true,
      items: (result['items'] as List)
          .map(
            (e) => ClipboardItem(
              size: (e as Map)['size'] as int,
              name: e['name'] as String?,
              units: e['units'] is int ? e['units'] as int : null,
              kind: e['kind'] as String?,
              width: e['width'] is int ? e['width'] as int : null,
              height: e['height'] is int ? e['height'] as int : null,
              attributes: e['attrs'] is int ? e['attrs'] as int : 0,
              mtime: e['mtime'] is int ? e['mtime'] as int : null,
            ),
          )
          .toList(),
    );
  }

  static ClipboardSourceReceipt? _sourceReceipt(
    Map<String, dynamic> result,
    String owner,
  ) {
    if (!result.containsKey('sourceReceipt')) return null;
    final fields = result['sourceReceipt'];
    if (fields is! Map ||
        fields.length != 2 ||
        fields['owner'] is! String ||
        fields['captureSerial'] is! int) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    final source = ClipboardSourceReceipt(
      fields['owner'] as String,
      fields['captureSerial'] as int,
    );
    if (!source._valid || source.owner != owner) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    return source;
  }

  static Map<String, Object> _sourceFields(ClipboardSourceReceipt source) {
    if (!source._valid) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    return {'owner': source.owner, 'captureSerial': source.captureSerial};
  }

  @override
  Future<bool> retireSource(ClipboardSourceReceipt source) async {
    final result = await _channel.invokeMethod<Object?>(
      'retireSource',
      _sourceFields(source),
    );
    if (result is! Map || result.length != 1 || result['retired'] is! bool) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    return result['retired'] as bool;
  }

  @override
  Future<Uint8List> sourceChunkScoped(
    ClipboardSourceReceipt source,
    int index,
    int offset,
    int length,
  ) async {
    final result = await _channel.invokeMethod<Object?>('sourceChunkScoped', {
      ..._sourceFields(source),
      'index': index,
      'offset': offset,
      'length': length,
    });
    if (result is! Uint8List) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    return result;
  }

  @override
  Future<String> sourceDigestScoped(
    ClipboardSourceReceipt source,
    int index,
  ) async {
    final result = await _channel.invokeMethod<Object?>('sourceDigestScoped', {
      ..._sourceFields(source),
      'index': index,
    });
    if (result is! String ||
        result.length != 64 ||
        !RegExp(r'^[0-9a-f]{64}$').hasMatch(result)) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    return result;
  }

  @override
  Future<Uint8List> sourceChunk(
    String owner,
    int index,
    int offset,
    int length,
  ) async => (await _channel.invokeMethod<Uint8List>('sourceChunk', {
    'owner': owner,
    'index': index,
    'offset': offset,
    'length': length,
  }))!;
  @override
  Future<String> sourceDigest(String owner, int index) async => (await _channel
      .invokeMethod<String>('sourceDigest', {'owner': owner, 'index': index}))!;
  @override
  Future<void> begin(
    String owner,
    String requestId,
    String format,
    List<ClipboardItem> items,
  ) => _channel.invokeMethod<void>('begin', {
    'owner': owner,
    'requestId': requestId,
    'format': format,
    ..._pendingFields(owner, requestId, format: format),
    'items': [
      for (final item in items)
        {
          'size': item.size,
          if (item.name != null) 'name': item.name,
          if (item.kind != null) 'kind': item.kind,
          if (item.width != null) 'width': item.width,
          if (item.height != null) 'height': item.height,
          if (item.attributes != 0) 'attrs': item.attributes,
        },
    ],
  });
  @override
  Future<void> append(
    String owner,
    String requestId,
    int index,
    int offset,
    Uint8List bytes,
  ) => _channel.invokeMethod<void>('append', {
    'owner': owner,
    'requestId': requestId,
    ..._pendingFields(owner, requestId),
    'index': index,
    'offset': offset,
    'bytes': bytes,
  });
  @override
  Future<void> finish(
    String owner,
    String requestId,
    int index,
    String digest,
  ) => _channel.invokeMethod<void>('finish', {
    'owner': owner,
    'requestId': requestId,
    ..._pendingFields(owner, requestId),
    'index': index,
    'digest': digest,
  });
  @override
  Future<void> commit(
    String owner,
    String requestId, {
    int? unlessSequence,
  }) async {
    final pending = _pendingFields(owner, requestId);
    final lease = pending.isEmpty ? null : _pendingBindings[owner]!.lease;
    final publication = _startPublication(
      owner,
      requestId,
      epoch: lease?.epoch,
      lease: lease,
    );
    final result = await _channel.invokeMapMethod<String, dynamic>('commit', {
      'owner': owner,
      'requestId': requestId,
      ...pending,
      if (unlessSequence != null && _validSequence(unlessSequence))
        'unlessSequence': unlessSequence,
    });
    if (!_publicationCurrent(publication)) return;
    final sequence = result?['sequence'];
    _lastCommittedSequence = _validSequence(sequence) ? sequence as int : null;
    final readable = result?['readable'], error = result?['readError'];
    final flags = result?['readFlags'];
    _lastPublishCheck =
        readable is bool && _validSequence(error) && _validSequence(flags)
        ? (readable: readable, error: error as int, flags: flags as int)
        : null;
    _lastPublication = publication;
  }

  ({bool readable, int error, int flags})? _lastPublishCheck;
  @override
  ({bool readable, int error, int flags})? get lastPublishCheck =>
      _lastPublishCheck;

  @override
  Future<({bool owned, int sequence})> published(String owner) async {
    final result = await _channel.invokeMapMethod<String, dynamic>(
      'published',
      {'owner': owner},
    );
    final owned = result?['owned'], sequence = result?['sequence'];
    if (owned is! bool || !_validSequence(sequence)) {
      throw PlatformException(code: 'clipboard_transfer_failed');
    }
    return (owned: owned, sequence: sequence as int);
  }

  @override
  Future<void> publishOffer(
    String owner,
    String offerId,
    List<ClipboardItem> entries,
  ) async {
    final publication = _startPublication(owner, offerId);
    final result = await _channel.invokeMapMethod<String, dynamic>(
      'publishOffer',
      {
        'owner': owner,
        'offerId': offerId,
        'items': [
          for (final item in entries)
            {
              'size': item.size,
              'name': item.name,
              'kind': item.kind,
              'attrs': item.attributes,
              'mtime': item.mtime ?? 0,
            },
        ],
      },
    );
    if (!_publicationCurrent(publication)) return;
    final sequence = result?['sequence'];
    _lastCommittedSequence = _validSequence(sequence) ? sequence as int : null;
    _lastPublishCheck = null;
    _lastPublication = publication;
  }

  @override
  Future<void> publishOfferOnPaste(
    String owner,
    String offerId,
    String format,
    List<ClipboardItem> entries,
  ) async {
    final pendingFields = format == 'tree'
        ? const <String, dynamic>{}
        : _pendingFields(owner, offerId, format: format);
    final lease = pendingFields.isEmpty ? null : _pendingBindings[owner]!.lease;
    final publication = _startPublication(
      owner,
      offerId,
      epoch: lease?.epoch,
      lease: lease,
    );
    final result = await _channel.invokeMapMethod<String, dynamic>(
      pendingFields.isEmpty ? 'publishOffer' : 'publishPendingOffer',
      {
        'owner': owner,
        if (pendingFields.isNotEmpty) ...{
          ...pendingFields,
          'requestId': offerId,
        },
        'offerId': offerId,
        'format': format,
        'ownProgress': true,
        if (format == 'tree' && pullDepth > 1) 'prefetchDepth': pullDepth,
        'items': [
          for (final item in entries)
            {
              'size': item.size,
              if (format == 'tree') ...{
                'name': item.name,
                'kind': item.kind,
                'attrs': item.attributes,
                'mtime': item.mtime ?? 0,
              } else ...{
                'kind': item.kind,
                if (item.width != null) 'width': item.width,
                if (item.height != null) 'height': item.height,
              },
            },
        ],
      },
    );
    if (!_publicationCurrent(publication)) return;
    final sequence = result?['sequence'];
    _lastCommittedSequence = _validSequence(sequence) ? sequence as int : null;
    _lastPublishCheck = null;
    _lastPublication = publication;
  }

  @override
  Future<List<String>> dropOffers(String owner) async {
    final result = await _channel.invokeListMethod<String>('dropOffers', {
      'owner': owner,
    });
    return result ?? const [];
  }

  @override
  Future<void> pullAbort(String owner) =>
      _channel.invokeMethod<void>('pullAbort', {'owner': owner});
  @override
  Future<void> pullSupersede(String owner) =>
      _channel.invokeMethod<void>('pullSupersede', {'owner': owner});
  @override
  Future<void> pullRetire(String owner, String offerId) => _channel
      .invokeMethod<void>('pullRetire', {'owner': owner, 'offerId': offerId});
  @override
  Future<void> withdrawOffer(String owner, String offerId) =>
      _channel.invokeMethod<void>('withdrawOffer', {
        'owner': owner,
        'offerId': offerId,
      });

  // Data and end-of-range notices go straight to the platform thread (they must
  // never queue behind a reader that is waiting for them).
  @override
  Future<void> pullData(int rangeId, int offset, Uint8List bytes) =>
      _channel.invokeMethod<void>('pullData', {
        'rangeId': rangeId,
        'offset': offset,
        'bytes': bytes,
      });
  @override
  Future<void> pullEnd(int rangeId, String digest) => _channel
      .invokeMethod<void>('pullEnd', {'rangeId': rangeId, 'digest': digest});
  @override
  Future<void> pullFail(int rangeId, String reason) => _channel
      .invokeMethod<void>('pullFail', {'rangeId': rangeId, 'reason': reason});
  @override
  Future<void> pullShutdown(String owner, String reason) => _channel
      .invokeMethod<void>('pullShutdown', {'owner': owner, 'reason': reason});
  @override
  Future<int> pullOpen(
    String owner,
    String offerId,
    int index,
    int offset,
  ) async => (await _channel.invokeMethod<int>('pullOpen', {
    'owner': owner,
    'offerId': offerId,
    'index': index,
    'offset': offset,
  }))!;
  @override
  Future<Uint8List> pullRead(String owner, int length) async =>
      (await _channel.invokeMethod<Uint8List>('pullRead', {
        'owner': owner,
        'length': length,
      }))!;
  @override
  Future<String> pullDigest(String owner) async =>
      (await _channel.invokeMethod<String>('pullDigest', {'owner': owner}))!;
  @override
  Future<void> pullClose(String owner) =>
      _channel.invokeMethod<void>('pullClose', {'owner': owner});
  @override
  Future<void> closeOffers(String owner) =>
      _channel.invokeMethod<void>('closeOffers', {'owner': owner});

  @override
  Future<int?> claim(String owner, String requestId) async {
    final publication = _startPublication(owner, requestId);
    final result = await _channel.invokeMapMethod<String, dynamic>('claim', {
      'owner': owner,
      'requestId': requestId,
    });
    if (!_publicationCurrent(publication)) return null;
    final sequence = result?['sequence'];
    return result?['claimed'] == true && _validSequence(sequence)
        ? sequence as int
        : null;
  }

  @override
  Future<void> cancel(String owner) async {
    final binding = _pendingBindings[owner];
    if (binding != null) await cancelPending(owner, binding.lease);
    await _channel.invokeMethod<void>('cancel', {'owner': owner});
  }

  @override
  Future<void> release(String owner) =>
      _channel.invokeMethod<void>('release', {'owner': owner});
}
