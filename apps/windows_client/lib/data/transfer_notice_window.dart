import 'dart:async';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

/// What the transfer card says; the same words the in-session card shows.
class TransferNoticeContent {
  const TransferNoticeContent({
    required this.kind,
    this.title = '',
    this.subtitle = '',
    this.subtitleTip = '',
    this.name,
    this.detail = '',
    this.trailing = '',
    this.message = '',
    this.hint = '',
    this.details = const [],
    this.percent,
    this.dark = true,
    this.sending = false,
    this.folder = false,
    this.openable = false,
    this.cancellable = true,
    this.cancelling = false,
    this.generation = 0,
  });

  /// 0 running, 1 done, 2 failed, 3 cancelled.
  final int kind;

  /// The first line ("WorkPC → PC", "옮겼어요") and the one under it
  /// ("다운로드 · 1,702개 중 120개"); [subtitleTip] is its full text (the path).
  final String title, subtitle, subtitleTip;

  /// Sizes and rate on the left of the line under the bar, [trailing] (the
  /// time left, "마무리하는 중…") on its right.
  final String detail, trailing;

  /// A failure: its cause, and the one thing to do next.
  final String message, hint;
  final String? name;

  /// 자세히: label and full value pairs.
  final List<(String, String)> details;

  /// 0..100 or null while the size is unknown.
  final int? percent;
  final bool dark, cancellable, cancelling;

  /// This PC → VM (the icon points up); [folder]: [subtitle] starts with the
  /// destination folder's name; [openable]: a finished card offers 폴더 열기.
  final bool sending, folder, openable;

  /// One value per transfer: a new value brings back a card the user closed.
  final int generation;

  Map<String, Object?> toMap() => {
    'kind': kind,
    'title': title,
    'subtitle': subtitle,
    'subtitleTip': subtitleTip,
    'name': name ?? '',
    'detail': detail,
    'trailing': trailing,
    'message': message,
    'hint': hint,
    'details': [for (final (label, value) in details) ...[label, value]],
    'percent': percent ?? -1,
    'dark': dark,
    'sending': sending,
    'folder': folder,
    'openable': openable,
    'cancellable': cancellable,
    'cancelling': cancelling,
    'generation': generation,
  };
}

/// The taskbar button's progress: nothing, a bar, a bar that only moves, a red paused bar.
enum TaskbarProgressMode { none, normal, indeterminate, error }

/// The part of a transfer's notice that lives outside the session window (Windows only): a small
/// always-on-top card while the session window is in the background, and the taskbar button's
/// progress. The native side decides when the card is on screen (never while the session window is
/// in front); this side only describes the transfer and hears 취소 / 닫기.
abstract interface class TransferNoticeWindow {
  Future<void> update(TransferNoticeContent content);
  Future<void> hide();
  Future<void> taskbar(TaskbarProgressMode mode, int percent);

  /// 취소 was pressed on the card.
  set onCancel(VoidCallback? callback);

  /// 닫기 was pressed on the card.
  set onClose(VoidCallback? callback);
}

/// Optional: the card window's 폴더 열기 on a finished paste.
abstract interface class TransferNoticeOpenAction {
  set onOpen(VoidCallback? callback);
}

/// Optional runner capability. Suppression includes the prepare/frame handoff;
/// the input barrier remains owned by the session throughout.
abstract interface class TransferNoticePresentation implements Listenable {
  bool isPresented(int generation);
}

const _presentationProtocol = 'presentation-lease-v1';
final _opaqueNoticeId = RegExp(r'^[0-9a-f]{32}$');
String _newNoticeOwner() {
  final random = Random.secure();
  return List.generate(16,
      (_) => random.nextInt(256).toRadixString(16).padLeft(2, '0')).join();
}

class _NoticeLease {
  const _NoticeLease(this.owner, this.generation, this.token);
  final String owner, token;
  final int generation;
  Map<String, Object> get identity => {
    'owner': owner, 'generation': generation, 'lease': token,
  };
  bool matches(Map value) => value['generation'] is int && value['owner'] == owner &&
      value['generation'] == generation && value['lease'] == token;
}

class _NoticeUnsupported {
  const _NoticeUnsupported();
}
class _NoticeUncertain {
  const _NoticeUncertain();
}
int? _noticeSurfaceRevision(Object? raw) {
  if (raw is! Map || raw['protocol'] != _presentationProtocol ||
      raw['surfaceRevision'] is! int ||
      (raw['surfaceRevision'] as int) <= 0 ||
      (raw['surfaceRevision'] as int) > 0x7fffffffffffffff ||
      raw['inlineAllowed'] is! bool || raw['blockedByNative'] is! bool ||
      raw['inlineAllowed'] == raw['blockedByNative']) {
    return null;
  }
  return raw['surfaceRevision'] as int;
}

/// One handler per messenger/channel, with opaque-owner routing. Separate Dart
/// wrappers must not overwrite each other's MethodChannel callback handler.
class _NoticeRouter {
  _NoticeRouter(this.channel);
  final MethodChannel channel;
  final owners = <String, NativeTransferNoticeWindow>{};
  NativeTransferNoticeWindow? legacyOwner;
  int _surfaceRevision = -1;
  bool? _inlineAllowed;
  bool _supportsProtocol = false;

  // A process-wide physical-surface observation has no transfer owner. Retain
  // its monotonic floor so a late acquire ACK cannot undo a newer hidden proof.
  bool observeSurface(Object? raw) {
    final revision = _noticeSurfaceRevision(raw);
    if (revision == null || raw is! Map) return false;
    _supportsProtocol = true;
    final allowed = raw['inlineAllowed'] as bool;
    if (revision < _surfaceRevision ||
        (revision == _surfaceRevision && _inlineAllowed != allowed)) {
      return false;
    }
    _surfaceRevision = revision;
    _inlineAllowed = allowed;
    for (final window in owners.values.toList(growable: false)) {
      window._surfaceProof(revision, allowed);
    }
    return true;
  }
  void applyLatestSurface(NativeTransferNoticeWindow window) {
    final allowed = _inlineAllowed;
    if (allowed != null) window._surfaceProof(_surfaceRevision, allowed);
  }
  static final _routers = <(BinaryMessenger, String), _NoticeRouter>{};
  static _NoticeRouter attach(NativeTransferNoticeWindow window) {
    final channel = window._channel;
    final key = (channel.binaryMessenger, channel.name);
    final router = _routers.putIfAbsent(key, () {
      final created = _NoticeRouter(channel);
      channel.setMethodCallHandler(created._event);
      return created;
    });
    router.owners[window._owner] = window;
    return router;
  }
  void detach(NativeTransferNoticeWindow window) {
    if (identical(owners[window._owner], window)) owners.remove(window._owner);
    if (identical(legacyOwner, window)) legacyOwner = null;
    if (owners.isEmpty) {
      final key = (channel.binaryMessenger, channel.name);
      if (identical(_routers[key], this)) {
        _routers.remove(key);
        channel.setMethodCallHandler(null);
      }
    }
  }
  Future<Object?> _event(MethodCall call) async {
    final raw = call.arguments;
    if (call.method == 'surface') {
      observeSurface(raw);
      return null;
    }
    if (raw is int) {
      legacyOwner?._legacyAction(call.method, raw);
      return null;
    }
    if (raw is! Map || raw['owner'] is! String) return null;
    return owners[raw['owner']]?._event(call.method, raw);
  }
}

/// The Windows implementation: the runner's `multitap/transfer-notice` channel. Every call is
/// best effort; a missing runner (tests, another platform) is silently ignored.
class NativeTransferNoticeWindow extends ChangeNotifier
    implements
        TransferNoticeWindow,
        TransferNoticePresentation,
        TransferNoticeOpenAction {
  NativeTransferNoticeWindow([MethodChannel? channel])
    : _channel = channel ?? const MethodChannel('multitap/transfer-notice') {
    _router = _NoticeRouter.attach(this);
  }
  final MethodChannel _channel;
  final String _owner = _newNoticeOwner();
  late _NoticeRouter _router;
  int? _generation;
  int? _actionsGeneration;
  int _epoch = 0, _revision = -1, _invalidThroughRevision = -1;
  bool _disposed = false, _preparing = false, _visible = false;
  bool _legacyFallback = false;
  bool _retiring = false;
  bool _acquisitionPending = false, _blockedBySurface = false;
  int _surfaceRevision = -1, _retirementSerial = 0;
  int? _retirementAttempt;
  _NoticeLease? _lease;
  Future<_NoticeLease?>? _claim;
  @override
  VoidCallback? onCancel;
  @override
  VoidCallback? onClose;
  @override
  VoidCallback? onOpen;

  @override
  bool isPresented(int generation) => !_disposed &&
      _generation == generation && (_acquisitionPending || _blockedBySurface ||
      (_lease != null && (_preparing || _visible)));

  void _claimState({required bool pending, required bool blocked}) {
    if (_disposed || (_acquisitionPending == pending && _blockedBySurface == blocked)) return;
    _acquisitionPending = pending;
    _blockedBySurface = blocked;
    notifyListeners();
  }
  void _surfaceProof(int revision, bool inlineAllowed) {
    if (_disposed || (!_acquisitionPending && !_blockedBySurface) ||
        revision < _surfaceRevision) {
      return;
    }
    _surfaceRevision = revision;
    _claimState(pending: _acquisitionPending, blocked: !inlineAllowed);
    if (inlineAllowed && _retiring && _lease == null) {
      _retiring = false;
      _retirementAttempt = null;
      _claimState(pending: false, blocked: false);
      _router.detach(this);
    }
  }

  void _paintState({required bool preparing, required bool visible}) {
    if (_disposed || (_preparing == preparing && _visible == visible)) return;
    _preparing = preparing;
    _visible = visible;
    notifyListeners();
  }
  Future<Object?> _call(String method, [Object? arguments]) async {
    try {
      return await _channel.invokeMethod<Object?>(method, arguments)
          .timeout(const Duration(seconds: 2));
    } on MissingPluginException {
      return const _NoticeUnsupported();
    } catch (_) {
      // A timeout can follow a native side effect. It is not a hidden proof.
      return const _NoticeUncertain();
    }
  }
  bool _current(_NoticeLease lease, int epoch) => !_disposed &&
      epoch == _epoch && identical(_lease, lease) &&
      _generation == lease.generation;
  bool _validEvent(Map value, _NoticeLease lease) => lease.matches(value) &&
      (value['protocol'] == null || value['protocol'] == _presentationProtocol) &&
      value['revision'] is int && (value['revision'] as int) > 0 &&
      (value['revision'] as int) <= 0x7fffffffffffffff;
  Map? _ack(Object? raw, _NoticeLease lease) {
    if (raw is! Map || raw['protocol'] != _presentationProtocol ||
        raw['accepted'] is! bool || raw['visible'] is! bool ||
        !_validEvent(raw, lease)) {
      return null;
    }
    return raw;
  }

  Future<bool> _hideCaptured(_NoticeLease lease) async {
    // Hide also clears only this lease's taskbar state. A newer owner's native
    // singleton cannot be changed by either of these old requests.
    final hiding = _call('hide', lease.identity);
    final clearing = _call('taskbar', {...lease.identity, 'mode': 'none', 'percent': 0});
    final raw = await hiding;
    await clearing;
    final ack = _ack(raw, lease);
    return ack != null && (ack['accepted'] == false || ack['visible'] == false);
  }

  Future<_NoticeLease?> _acquire(int generation, int epoch,
      _NoticeLease? previous) async {
    // A failed old Hide must retain paint exclusion. Acquire asks native to
    // retire that old surface; only its physical proof may release the card.
    if (previous != null) await _hideCaptured(previous);
    if (_disposed || epoch != _epoch || _generation != generation) return null;
    final raw = await _call('acquire', {'owner': _owner, 'generation': generation});
    final surfaceFieldsPresent = raw is Map &&
        (raw.containsKey('surfaceRevision') || raw.containsKey('inlineAllowed') ||
        raw.containsKey('blockedByNative'));
    final validSurface = !surfaceFieldsPresent || _noticeSurfaceRevision(raw) != null;
    if (raw is! Map || raw['protocol'] != _presentationProtocol ||
        raw['accepted'] != true || raw['owner'] != _owner ||
        raw['generation'] is! int || raw['generation'] != generation || raw['lease'] is! String ||
        !_opaqueNoticeId.hasMatch(raw['lease'] as String) ||
        raw['revision'] is! int || (raw['revision'] as int) <= 0 ||
        (raw['revision'] as int) > 0x7fffffffffffffff ||
        raw['visible'] != false || !validSurface) {
      if (!_disposed && epoch == _epoch) {
        final protocolReply = raw is Map && raw['protocol'] == _presentationProtocol;
        _router._supportsProtocol = _router._supportsProtocol || protocolReply;
        // Null is the explicit legacy no-capability reply, not a caught native
        // timeout. Once v1 is known, even null cannot prove physical absence.
        _legacyFallback = !_router._supportsProtocol && previous == null &&
            (raw is _NoticeUnsupported || raw == null);
        if (_legacyFallback) {
          _router.legacyOwner = this;
        } else if (identical(_router.legacyOwner, this)) {
          _router.legacyOwner = null;
        }
        _paintState(preparing: false, visible: false);
        _claimState(pending: false,
            blocked: _legacyFallback ? false : _blockedBySurface);
        if (!_legacyFallback) {
          _router.observeSurface(raw);
          if (_noticeSurfaceRevision(raw) != null) {
            _router.applyLatestSurface(this);
          }
          unawaited(_reconcileSurface(generation, epoch));
        }
      }
      return null;
    }
    final lease = _NoticeLease(_owner, generation, raw['lease'] as String);
    if (_disposed || epoch != _epoch || _generation != generation) {
      unawaited(_hideCaptured(lease).then<void>((_) {}));
      return null;
    }
    _lease = lease;
    _router._supportsProtocol = true;
    _router.legacyOwner = null;
    _revision = raw['revision'] as int;
    _invalidThroughRevision = -1;
    _legacyFallback = false;
    _retiring = false;
    _paintState(preparing: false, visible: false);
    _router.observeSurface(raw);
    // Earlier explicit v1 success ACKs omit physical fields, but their fresh
    // accepted lease + visible:false already proves Acquire hid the old popup.
    // If any physical field exists, all three were validated above and their
    // latest surface proof remains authoritative (including a blocked proof).
    _claimState(pending: false,
        blocked: surfaceFieldsPresent ? _blockedBySurface : false);
    if (surfaceFieldsPresent) _router.applyLatestSurface(this);
    return lease;
  }

  Future<void> _reconcileSurface(int generation, int epoch) async {
    // Read-only reconciliation is bounded and never claims another lease.
    final raw = await _call('surfaceStatus');
    if (_disposed || epoch != _epoch || _generation != generation) return;
    _router.observeSurface(raw);
    if (_noticeSurfaceRevision(raw) != null) _router.applyLatestSurface(this);
  }

  Future<_NoticeLease?> _forGeneration(int generation) {
    if (_disposed) return Future.value(null);
    if (generation < 0 || generation > 0x7fffffff) {
      unawaited(hide());
      return Future.value(null);
    }
    if (_generation == generation && _claim != null) return _claim!;
    final previous = _lease;
    _generation = generation;
    _lease = null;
    _revision = -1;
    _legacyFallback = false;
    final pendingChanged = !_acquisitionPending || !_blockedBySurface;
    _acquisitionPending = true;
    _blockedBySurface = true;
    final epoch = ++_epoch;
    _router = _NoticeRouter.attach(this);
    // Exactly one acquire per displayed generation. Failed/rejected updates
    // never reacquire and steal the currently valid native owner.
    final claim = _claim = _acquire(generation, epoch, previous);
    if (pendingChanged) notifyListeners();
    return claim;
  }

  void _acceptAck(Object? raw, _NoticeLease lease, int epoch) {
    if (!_current(lease, epoch)) return;
    final ack = _ack(raw, lease);
    if (ack == null) return;
    if (ack['accepted'] != true) {
      _lease = null;
      _retiring = false;
      _paintState(preparing: false, visible: false);
      _router.observeSurface(raw);
      return;
    }
    // Content/taskbar ACKs validate ownership only. Their hidden snapshot can
    // arrive during prepare at the same revision; only presentation callbacks
    // may advance paint ownership. Missing ACKs do not erase a verified Show.
  }

  Future<Object?> _event(String method, Map value) async {
    final lease = _lease;
    if (_disposed || lease == null || !_validEvent(value, lease)) return null;
    final epoch = _epoch, revision = value['revision'] as int;
    if (method == 'cancel' || method == 'close' || method == 'open') {
      if (value['accepted'] != true || _retiring ||
          _actionsGeneration != lease.generation) {
        return null;
      }
      if (method == 'cancel') onCancel?.call();
      if (method == 'close') onClose?.call();
      if (method == 'open') onOpen?.call();
      return null;
    }
    if (revision <= _invalidThroughRevision) {
      return method == 'prepare'
          ? {...lease.identity, 'revision': revision, 'prepared': false} : null;
    }
    if (revision < _revision) return null;
    if (method == 'presentation' && value['accepted'] == false &&
        value['visible'] == false) {
      _lease = null;
      _retiring = false;
      _paintState(preparing: false, visible: false);
      return null;
    }
    if (method == 'presentation' && value['visible'] is bool) {
      if (_preparing && revision == _revision && value['visible'] == false &&
          value['phase'] != 'inline') {
        return null;
      }
      _revision = revision;
      _paintState(preparing: false, visible: value['visible'] == true);
      if (_retiring && value['visible'] == false) {
        _retiring = false;
        _router.detach(this);
      }
      return null;
    }
    if (method != 'prepare' || value['requiresFrame'] is! bool) return null;
    if (_retiring) {
      return {...lease.identity, 'revision': revision, 'prepared': false};
    }
    _revision = revision;
    _paintState(preparing: true, visible: false);
    var prepared = false;
    try {
      if (value['requiresFrame'] == true) {
        await SchedulerBinding.instance.endOfFrame
            .timeout(const Duration(milliseconds: 750));
      }
      prepared = _current(lease, epoch) && _revision == revision && _preparing;
    } catch (_) { /* A stopped frame producer cannot grant native Show. */ }
    if (!prepared && _current(lease, epoch) && _revision == revision) {
      _invalidThroughRevision = revision;
      _paintState(preparing: false, visible: false);
    }
    return {...lease.identity, 'revision': revision, 'prepared': prepared};
  }

  void _legacyAction(String method, int generation) {
    if (_disposed || !_legacyFallback || _lease != null ||
        _generation != generation || _actionsGeneration != generation) {
      return;
    }
    if (method == 'cancel') onCancel?.call();
    if (method == 'close') onClose?.call();
  }

  @override
  Future<void> update(TransferNoticeContent content) async {
    if (_disposed) return;
    // The visual flag may already have been cleared by a Hide callback. Keep
    // retirement ownership separate so its late finally cannot detach a resume.
    if (_retirementAttempt != null) {
      _retirementAttempt = null;
      _retirementSerial++;
      _epoch++;
      _retiring = false;
    }
    _actionsGeneration = content.generation;
    _router = _NoticeRouter.attach(this);
    final pending = _forGeneration(content.generation);
    final epoch = _epoch;
    final lease = await pending;
    if (lease == null || !_current(lease, epoch)) return;
    final ack = await _call('update', {...content.toMap(), ...lease.identity});
    _acceptAck(ack, lease, epoch);
  }

  @override
  Future<void> hide() async {
    final previous = _lease;
    final epoch = ++_epoch;
    _actionsGeneration = null;
    _legacyFallback = false;
    _retiring = true;
    final attempt = ++_retirementSerial;
    _retirementAttempt = attempt;
    var hidden = previous == null && !_blockedBySurface && !_acquisitionPending;
    try {
      if (previous != null) hidden = await _hideCaptured(previous);
      if (previous == null && !hidden) {
        await _reconcileSurface(_generation ?? -1, epoch);
      }
    } finally {
      if (!_disposed && _epoch == epoch && _retirementAttempt == attempt && hidden) {
        _retirementAttempt = null;
        _retiring = false;
        _paintState(preparing: false, visible: false);
        _claimState(pending: false, blocked: false);
        _router.detach(this);
      }
    }
  }

  @override
  Future<void> taskbar(TaskbarProgressMode mode, int percent) async {
    final pending = _claim;
    if (_disposed || _retiring || pending == null || _generation == null) return;
    final epoch = _epoch;
    final lease = await pending;
    if (lease == null || !_current(lease, epoch)) return;
    final ack = await _call('taskbar', {...lease.identity,
      'mode': mode.name, 'percent': percent.clamp(0, 100)});
    _acceptAck(ack, lease, epoch);
  }

  @override
  void dispose() {
    if (_disposed) return;
    final previous = _lease;
    _disposed = true;
    _epoch++;
    _lease = null;
    _generation = null;
    _actionsGeneration = null;
    _claim = null;
    _retirementAttempt = null;
    onCancel = null;
    onClose = null;
    onOpen = null;
    _router.detach(this);
    if (previous != null) unawaited(_hideCaptured(previous).then<void>((_) {}));
    super.dispose();
  }

  /// Whether the card and the taskbar exist on this platform.
  static bool get supported =>
      !kIsWeb && defaultTargetPlatform == TargetPlatform.windows;
}
