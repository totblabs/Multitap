import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../data/clipboard_session.dart';
import '../data/paste_target.dart';
import '../data/transfer_notice_window.dart';
import 'clipboard_transfer_notice.dart';
import 'korean_text.dart';
import 'session_menu.dart';
import '../l10n/l10n.dart';

enum TransferPhase { hidden, running, done, failed, cancelled }

/// Where a running transfer is: nothing known yet, data moving, or every byte
/// handed over while Explorer is still writing (not done yet).
enum TransferStage { preparing, moving, finishing }

/// What the transfer card shows right now; the in-session card and the card window outside the
/// session window are both drawn from it.
@immutable
class TransferCardView {
  const TransferCardView({
    this.phase = TransferPhase.hidden,
    this.stage = TransferStage.moving,
    this.sending = false,
    this.title = '',
    this.subtitle = '',
    this.name,
    this.detail = '',
    this.trailing = '',
    this.message = '',
    this.hint = '',
    this.fraction,
    this.destination,
    this.details = const [],
    this.cancelling = false,
    this.suppressed = false,
  });
  final TransferPhase phase;
  final TransferStage stage;

  /// This PC → VM.
  final bool sending;

  /// "WorkPC → PC" while it runs; "옮겼어요", "옮기지 못했어요", "전송을 취소했어요" after.
  final String title;

  /// Under the title: what moves ("1,702개 중 120개", one file's name, "이미지"),
  /// after the destination folder's name when that is known; the outcome after it ends.
  final String subtitle;

  /// The file in progress, whole (the card shortens it for display).
  final String? name;

  /// "113.3 MB / 1.8 GB · 8.0 MB/s"; empty while the size is unknown.
  final String detail;

  /// The time left once it is steady, or "마무리하는 중…".
  final String trailing;

  /// Why a transfer failed, and the one thing to do next.
  final String message, hint;

  /// 0..1, null while the size is unknown (an indeterminate bar).
  final double? fraction;

  /// The folder on this PC the paste is known to fill (null when not certain).
  final PasteDestination? destination;

  /// 자세히: label and value rows.
  final List<(String, String)> details;
  final bool cancelling;

  /// Windows reports the progress itself (Explorer's own copy dialog): no card of ours, but the
  /// taskbar button still shows it.
  final bool suppressed;

  int? get percent =>
      fraction == null ? null : (fraction! * 100).floor().clamp(0, 100);

  /// One sentence for a screen reader.
  String get spoken => [
    title,
    if (subtitle.isNotEmpty) subtitle,
    if (phase == TransferPhase.running && percent != null) '$percent%',
    if (trailing.isNotEmpty) trailing,
    if (message.isNotEmpty) message,
    if (hint.isNotEmpty) hint,
  ].join(', ');

  @override
  bool operator ==(Object other) =>
      other is TransferCardView &&
      other.phase == phase &&
      other.stage == stage &&
      other.sending == sending &&
      other.title == title &&
      other.subtitle == subtitle &&
      other.name == name &&
      other.detail == detail &&
      other.trailing == trailing &&
      other.message == message &&
      other.hint == hint &&
      other.fraction == fraction &&
      other.destination == destination &&
      _sameRows(other.details, details) &&
      other.cancelling == cancelling &&
      other.suppressed == suppressed;

  static bool _sameRows(List<(String, String)> a, List<(String, String)> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  @override
  int get hashCode => Object.hash(
    phase,
    stage,
    sending,
    title,
    subtitle,
    name,
    detail,
    trailing,
    message,
    hint,
    fraction,
    destination,
    Object.hashAll(details),
    cancelling,
    suppressed,
  );
}

/// The one clipboard transfer card of a session: decides when a transfer is worth a card (still
/// running after [showAfter]; plain and rich text only after [textShowAfter]), follows its
/// progress a few times a second, ends it with a short 옮겼어요 (and 폴더 열기 when the folder is
/// known), a short 취소했어요, or a failure that stays until it is closed, and mirrors it to the
/// card window and the taskbar button outside the session window ([window]). 취소 goes through
/// [ClipboardSession.cancelTransfer]. A newer copy replaces the older card.
class ClipboardTransferController extends ChangeNotifier {
  ClipboardTransferController({
    required this.clipboard,
    this.window,
    this.onFailureWithoutCard,
    this.remoteName = 'VM',
    this.pasteTargets,
    this.showAfter = const Duration(milliseconds: 700),
    this.textShowAfter = const Duration(milliseconds: 1500),
    this.doneFor = const Duration(seconds: 4),
    this.openFor = const Duration(seconds: 8),
    this.cancelledFor = const Duration(seconds: 2),
    this.fade = const Duration(milliseconds: 250),
    this.interval = const Duration(milliseconds: 250),
    this.failedTaskbarFor = const Duration(seconds: 3),
    this.dark = true,
    this.clock,
    this.wallClock,
  }) {
    if (window != null) {
      final previous = _windowOwners[window!];
      _windowOwners[window!] = this;
      if (previous != null) {
        unawaited(window!.hide());
        unawaited(window!.taskbar(TaskbarProgressMode.none, 0));
      }
    }
    window
      ?..onCancel = cancel
      ..onClose = _windowClosed;
    final opener = window;
    if (opener is TransferNoticeOpenAction) {
      (opener as TransferNoticeOpenAction).onOpen = _windowOpen;
    }
    final presentation = window;
    if (presentation is TransferNoticePresentation) {
      (presentation as TransferNoticePresentation).addListener(_presentationChanged);
    }
    clipboard.addListener(_onSession);
    if (clipboard.transferring) _begin();
    _syncInteraction();
  }

  final ClipboardSession clipboard;
  final TransferNoticeWindow? window;

  /// A transfer failed that never had a card (it ended before [showAfter]): the session shows
  /// the reason as a short toast instead.
  final void Function(String message)? onFailureWithoutCard;

  /// The VM's name as the launcher shows it ("WorkPC → PC").
  final String remoteName;

  /// Where a paste on this PC goes (Windows); null shows no folder.
  final PasteTargetProbe? pasteTargets;

  /// How long 옮겼어요 stays: [doneFor], or [openFor] when it offers 폴더 열기.
  final Duration showAfter,
      textShowAfter,
      doneFor,
      openFor,
      cancelledFor,
      fade,
      interval,
      failedTaskbarFor;
  final bool dark;
  final Duration Function()? clock;

  /// The wall clock the file system's creation times are compared with (tests).
  final DateTime Function()? wallClock;

  static final _windowOwners = Expando<ClipboardTransferController>(
    'clipboard-transfer-window-owner',
  );
  static int _noticeSequence = 0;
  bool get _ownsWindow =>
      window != null && identical(_windowOwners[window!], this);
  bool get presentedExternally {
    final presentation = window;
    return _ownsWindow && presentation is TransferNoticePresentation &&
        (presentation as TransferNoticePresentation).isPresented(_generation);
  }
  void _presentationChanged() {
    if (!_disposed) notifyListeners();
  }
  void _nextGeneration() {
    _noticeSequence = _noticeSequence % 0x7ffffffe + 1;
    _generation = _noticeSequence;
  }

  static const quietCodes = {
    'clipboard_cancelled',
    'clipboard_changed',
    'clipboard_empty',
    'clipboard_session_closed',
  };

  TransferCardView _view = const TransferCardView();
  TransferCardView get view => _view;
  TransferPhase get phase => _view.phase;

  /// A file paste is actually in progress. Announcing a copy, silent text and
  /// images do not lock the remote view. This includes the short notice delay.
  bool get blocksInteraction => _blocksInteraction;
  bool _blocksInteraction = false;

  void _syncInteraction() {
    final format = clipboard.progress?.format ?? _format;
    final next =
        !_disposed &&
        !clipboard.closed &&
        clipboard.transferring &&
        clipboard.userWaiting &&
        (format == 'files' || format == 'tree');
    if (next == _blocksInteraction) return;
    _blocksInteraction = next;
    notifyListeners();
  }

  /// The card is the small pill (a chevron folded it; a click opens it again).
  bool get collapsed => _collapsed;
  bool _collapsed = false;

  /// 자세히 is open on the in-session card.
  bool get detailsOpen => _detailsOpen;
  bool _detailsOpen = false;

  /// The 옮겼어요 card is fading out.
  bool get fading => _fading;
  bool _fading = false;

  /// One value per transfer that got a card; the card window brings itself back for a new one.
  int _generation = 0;

  final _watch = Stopwatch()..start();
  Duration _now() => clock?.call() ?? _watch.elapsed;
  DateTime _wall() => wallClock?.call() ?? DateTime.now();
  bool _busy = false, _userCancelled = false, _disposed = false;

  /// Somebody was waiting for this transfer at some point (a paste, a manual send): only then is
  /// there anything to show, or to complain about when it fails.
  bool _everWaited = false;
  String _format = '';
  Duration _startedAt = Duration.zero;
  DateTime _startedWall = DateTime.fromMillisecondsSinceEpoch(0);
  TransferEstimator _estimate = TransferEstimator();
  Timer? _showTimer, _sampleTimer, _doneTimer, _taskbarTimer;
  String? _taskbarKey;

  /// The last progress seen (the finished card still says what moved).
  ClipboardProgress? _last;

  /// The rate and the time left as shown: they change at most every second / two seconds.
  String? _rateText, _etaText;
  Duration? _rateAt, _etaAt;

  /// A paste on this PC: its destination check, the pasted items' names, when it last looked.
  PasteDestinationCheck? _destination;
  List<String> _pasted = const [];
  Duration? _checkedAt;

  void toggleCollapsed() {
    _collapsed = !_collapsed;
    notifyListeners();
  }

  void toggleDetails() {
    _detailsOpen = !_detailsOpen;
    notifyListeners();
  }

  /// 취소: the transfer is cancelled through the clipboard protocol; the card says so until the
  /// transfer ends (and then says 취소했어요 briefly, without any error notice).
  void cancel() {
    if (_disposed ||
        (_view.phase != TransferPhase.running && !blocksInteraction) ||
        _view.cancelling) {
      return;
    }
    _userCancelled = true;
    if (_view.phase == TransferPhase.hidden) {
      _nextGeneration();
      _view = _compose(clipboard.progress);
    }
    _view = _with(_view, cancelling: true);
    notifyListeners();
    _push();
    unawaited(clipboard.cancelTransfer());
  }

  /// 닫기 on a finished card (a failure stays until this).
  void close() {
    if (_view.phase == TransferPhase.failed ||
        _view.phase == TransferPhase.done ||
        _view.phase == TransferPhase.cancelled) {
      _hideNow();
    }
  }

  /// 폴더 열기 on a finished paste whose folder is known: File Explorer opens it with the pasted
  /// item selected, and the card goes.
  Future<bool> openFolder() async {
    final check = _destination, probe = pasteTargets;
    final folder = check?.confirmed;
    if (_view.phase != TransferPhase.done || folder == null || probe == null) {
      return false;
    }
    _hideNow();
    return probe.open(folder.path, select: check!.item);
  }

  void _windowClosed() => close();
  void _windowOpen() => unawaited(openFolder());

  void _onSession() {
    if (_disposed) return;
    final busy = clipboard.transferring;
    if (busy && !_busy) {
      _begin();
    } else if (!busy && _busy) {
      _end();
    } else if (busy) {
      _noteFormat();
      _watchPaste(clipboard.progress);
      if (clipboard.userWaiting) {
        _everWaited = true;
        // A paste began to wait for a transfer that has already run long enough.
        if (_view.phase == TransferPhase.hidden &&
            _now() - _startedAt >= showAfter) {
          _maybeShow();
        }
      }
    }
    _syncInteraction();
  }

  void _noteFormat() {
    final format = clipboard.progress?.format;
    if (format != null && format.isNotEmpty) _format = format;
  }

  void _begin() {
    _busy = true;
    _userCancelled = false;
    _format = '';
    _estimate = TransferEstimator();
    _startedAt = _now();
    _startedWall = _wall();
    _last = null;
    _rateText = _etaText = null;
    _rateAt = _etaAt = _checkedAt = null;
    _destination = null;
    _pasted = const [];
    _detailsOpen = false;
    _everWaited = clipboard.userWaiting;
    // A newer copy replaces what the older one left (옮겼어요, a failure).
    if (_view.phase != TransferPhase.hidden) _hideNow();
    _watchPaste(clipboard.progress);
    _showTimer?.cancel();
    _showTimer = Timer(showAfter, _maybeShow);
  }

  /// A paste of the VM's files on this PC: ask right away which folder is in front (the paste
  /// started there), then look for the pasted items in it while the copy runs.
  void _watchPaste(ClipboardProgress? progress) {
    final probe = pasteTargets;
    if (probe == null ||
        progress == null ||
        progress.sending ||
        progress.format != 'tree') {
      return;
    }
    if (progress.topLevel.isNotEmpty) _pasted = progress.topLevel;
    _destination ??= PasteDestinationCheck(probe, since: _startedWall)..start();
  }

  void _lookForDestination({bool force = false}) {
    final check = _destination;
    if (check == null || check.confirmed != null || _pasted.isEmpty) return;
    final now = _now();
    if (!force &&
        _checkedAt != null &&
        now - _checkedAt! < const Duration(seconds: 1)) {
      return;
    }
    _checkedAt = now;
    unawaited(
      check.check(_pasted).then((found) {
        if (!found || _disposed || !identical(_destination, check)) return;
        _destinationFound();
      }),
    );
  }

  void _destinationFound() {
    switch (_view.phase) {
      case TransferPhase.running:
        final progress = _last;
        if (progress != null) _view = _running(progress, _now() - _startedAt);
      case TransferPhase.done:
        _view = _doneView();
        // 폴더 열기 appeared: it stays long enough to be pressed.
        if (!_fading) _holdThenHide(openFor);
      default:
        return;
    }
    notifyListeners();
    _push();
  }

  bool get _textual => _format == 'text' || _format == 'richtext';

  void _maybeShow() {
    if (!_busy || _disposed || _view.phase != TransferPhase.hidden) return;
    // A copy that is only shared in the background has nobody waiting: no card.
    if (!clipboard.userWaiting) return;
    _noteFormat();
    final elapsed = _now() - _startedAt;
    if (_textual && elapsed < textShowAfter) {
      _showTimer = Timer(textShowAfter - elapsed, _maybeShow);
      return;
    }
    _nextGeneration();
    _collapsed = false;
    _fading = false;
    _sampleTimer?.cancel();
    _sampleTimer = Timer.periodic(interval, (_) => _sample());
    _sample(first: true);
  }

  void _sample({bool first = false}) {
    if (!_busy || _disposed) return;
    final progress = clipboard.progress;
    if (progress != null && progress.format.isNotEmpty) {
      _format = progress.format;
    }
    final next = _compose(progress);
    if (first || next != _view) {
      _view = next;
      notifyListeners();
      _push();
    }
    _lookForDestination();
  }

  String get _route => '$remoteName → PC';
  String get _routeOut => 'PC → $remoteName';

  TransferCardView _compose(ClipboardProgress? progress) {
    final cancelling = _view.cancelling || _userCancelled;
    if (progress == null) {
      return TransferCardView(
        phase: TransferPhase.running,
        stage: TransferStage.preparing,
        title: l10n.transferring,
        subtitle: l10n.preparing2,
        cancelling: cancelling,
      );
    }
    _last = progress;
    _watchPaste(progress);
    final now = _now() - _startedAt;
    _estimate.add(now, progress.transferred);
    return _running(progress, now, cancelling: cancelling);
  }

  TransferCardView _running(
    ClipboardProgress progress,
    Duration now, {
    bool? cancelling,
  }) {
    final known = progress.total > 0;
    final fraction = known
        ? (progress.transferred / progress.total).clamp(0.0, 1.0)
        : null;
    final textual = _textual;
    final files = progress.format == 'files' || progress.format == 'tree';
    final stage = !known && progress.transferred == 0
        ? TransferStage.preparing
        : known && progress.transferred >= progress.total
        ? TransferStage.finishing
        : TransferStage.moving;
    // The rate changes at most once a second, the time left every two seconds.
    if (_rateAt == null || now - _rateAt! >= const Duration(seconds: 1)) {
      final rate = _estimate.rate;
      _rateText = rate != null && rate > 0 && now >= const Duration(seconds: 1)
          ? '${clipboardBytes(rate.round())}/s'
          : null;
      _rateAt = now;
    }
    if (_etaAt == null || now - _etaAt! >= const Duration(seconds: 2)) {
      _etaText = known
          ? transferEta(
              _estimate.remaining(now, progress.transferred, progress.total),
            )
          : null;
      _etaAt = now;
    }
    final detail = known && !textual
        ? [
            '${clipboardBytes(progress.transferred)} / ${clipboardBytes(progress.total)}',
            // Every byte is over: no rate while Explorer finishes writing.
            if (stage == TransferStage.moving && _rateText != null) _rateText!,
          ].join(' · ')
        : '';
    final destination = _destination?.confirmed;
    final what = stage == TransferStage.preparing
        ? l10n.preparing2
        : _what(progress);
    return TransferCardView(
      phase: TransferPhase.running,
      stage: stage,
      sending: progress.sending,
      title: progress.sending ? _routeOut : _route,
      subtitle: [?destination?.name, if (what.isNotEmpty) what].join(' · '),
      name: textual || !files ? null : progress.fileName,
      detail: detail,
      trailing: stage == TransferStage.finishing
          ? l10n.finishing
          : textual
          ? ''
          : _etaText ?? '',
      fraction: textual || stage == TransferStage.preparing ? null : fraction,
      destination: destination,
      details: _detailRows(progress, destination, running: true),
      cancelling: cancelling ?? _view.cancelling || _userCancelled,
      suppressed: progress.asyncConsumer,
    );
  }

  /// "1,702개 중 120개", one file's name, "이미지", "텍스트", "서식 있는 텍스트".
  static String _what(ClipboardProgress progress) => switch (progress.format) {
    'image' => l10n.image,
    'text' => l10n.text12,
    'richtext' => l10n.richText,
    'files' || 'tree' when progress.fileCount > 1 =>
      l10n.ofLabel(groupDigits(progress.fileCount), groupDigits(progress.fileIndex.clamp(1, progress.fileCount))),
    'files' || 'tree' => progress.fileName ?? '',
    _ => '',
  };

  static String _items(ClipboardProgress progress) => [
    if (progress.folderCount > 0) l10n.folders(groupDigits(progress.folderCount)),
    if (progress.fileCount > 0) l10n.files2(groupDigits(progress.fileCount)),
  ].join(' · ');

  static List<(String, String)> _detailRows(
    ClipboardProgress progress,
    PasteDestination? destination, {
    required bool running,
  }) {
    final files = progress.format == 'files' || progress.format == 'tree';
    final items = files ? _items(progress) : '';
    return [
      if (running && files && progress.fileName != null && progress.fileCount > 1)
        (l10n.currentFile, progress.fileName!),
      if (items.isNotEmpty) (l10n.items, items),
      if (destination != null) (l10n.saved2, destination.path),
    ];
  }

  TransferCardView _doneView() {
    final last = _last;
    final destination = _destination?.confirmed;
    final files = last != null && (last.format == 'files' || last.format == 'tree');
    final what = last == null
        ? ''
        : files
        ? (last.fileCount == 1 ? (last.fileName ?? '') : l10n.files2(groupDigits(last.fileCount)))
        : _what(last);
    return TransferCardView(
      phase: TransferPhase.done,
      sending: last?.sending ?? false,
      title: l10n.transferred,
      subtitle: destination != null
          ? l10n.inLabel(destination.name)
          : [
              if (last != null) last.sending ? _routeOut : _route,
              if (what.isNotEmpty) what,
            ].join(' · '),
      destination: destination,
      details: last == null
          ? const []
          : _detailRows(last, destination, running: false),
    );
  }

  void _end() {
    _busy = false;
    _showTimer?.cancel();
    _sampleTimer?.cancel();
    final code = clipboard.error;
    final shown = _view.phase == TransferPhase.running;
    final quiet =
        _userCancelled ||
        clipboard.closed ||
        clipboard.failureQuiet ||
        (code != null && quietCodes.contains(code));
    if (!shown) {
      if (code != null && !quiet && _everWaited) {
        onFailureWithoutCard?.call(clipboard.status);
      }
      return;
    }
    if (code == null && !clipboard.closed) {
      // File bytes/virtual-file publication can finish while Explorer is still
      // asking about an existing name, writing the destination, or cancelling.
      // Neither a transport ACK nor a recently created path proves that paste
      // succeeded. Let Explorer own the final result in both directions.
      if (_format == 'files' || _format == 'tree') {
        _hideNow();
        return;
      }
      _view = _doneView();
      _collapsed = false;
      _fading = false;
      notifyListeners();
      _push();
      _holdThenHide(_view.destination != null ? openFor : doneFor);
      // The last file may have just been created: one more look for its folder.
      _lookForDestination(force: true);
    } else if (_userCancelled && !clipboard.closed) {
      final last = _last;
      _view = TransferCardView(
        phase: TransferPhase.cancelled,
        sending: last?.sending ?? false,
        title: l10n.transferCanceled,
        subtitle: last == null ? '' : (last.sending ? _routeOut : _route),
      );
      _collapsed = false;
      _fading = false;
      notifyListeners();
      _push();
      _holdThenHide(cancelledFor);
    } else if (quiet) {
      _hideNow();
    } else {
      final (cause, next) = splitReason(clipboard.status);
      _view = TransferCardView(
        phase: TransferPhase.failed,
        sending: _last?.sending ?? false,
        title: l10n.couldntTransfer,
        message: cause,
        hint: next,
      );
      _collapsed = false;
      notifyListeners();
      _push();
      _taskbarTimer?.cancel();
      _taskbarTimer = Timer(failedTaskbarFor, () {
        _taskbarKey = null;
        _sendTaskbar(TaskbarProgressMode.none, 0);
      });
    }
  }

  void _holdThenHide(Duration hold) {
    final phase = _view.phase;
    _doneTimer?.cancel();
    final before = hold > fade ? hold - fade : Duration.zero;
    _doneTimer = Timer(before, () {
      if (_disposed || _view.phase != phase) return;
      _fading = true;
      notifyListeners();
      _doneTimer = Timer(fade, _hideNow);
    });
  }

  void _hideNow() {
    _doneTimer?.cancel();
    _sampleTimer?.cancel();
    _taskbarTimer?.cancel();
    _fading = false;
    final was = _view.phase != TransferPhase.hidden;
    _view = const TransferCardView();
    if (was) {
      if (_ownsWindow) unawaited(window?.hide());
      _sendTaskbar(TaskbarProgressMode.none, 0);
      if (!_disposed) notifyListeners();
    }
  }

  void _sendTaskbar(TaskbarProgressMode mode, int percent) {
    if (!_ownsWindow) return;
    final key = '${mode.name}:$percent';
    if (key == _taskbarKey) return;
    _taskbarKey = key;
    unawaited(window?.taskbar(mode, percent));
  }

  /// The card window and the taskbar follow the view.
  void _push() {
    final window = this.window;
    if (window == null || !_ownsWindow || _view.phase == TransferPhase.hidden) {
      return;
    }
    final view = _view;
    if (view.suppressed) {
      unawaited(window.hide());
    } else {
      final destination = view.destination;
      final single = view.name != null && view.subtitle.endsWith(view.name!);
      unawaited(
        window.update(
          TransferNoticeContent(
            kind: switch (view.phase) {
              TransferPhase.done => 1,
              TransferPhase.failed => 2,
              TransferPhase.cancelled => 3,
              _ => 0,
            },
            title: view.title,
            subtitle: view.subtitle,
            subtitleTip: destination != null
                ? destination.path
                : single
                ? view.name!
                : '',
            name: view.name == null ? null : middleEllipsize(view.name!, 80),
            detail: view.detail,
            trailing: view.trailing,
            message: view.message,
            hint: view.hint,
            details: view.details,
            percent: view.phase == TransferPhase.running &&
                    view.stage != TransferStage.preparing
                ? view.percent
                : null,
            dark: dark,
            sending: view.sending,
            folder: destination != null,
            openable: view.phase == TransferPhase.done && destination != null,
            cancellable:
                view.phase == TransferPhase.running && !view.cancelling,
            cancelling: view.cancelling,
            generation: _generation,
          ),
        ),
      );
    }
    switch (view.phase) {
      case TransferPhase.running:
        _taskbarTimer?.cancel();
        if (view.percent == null) {
          _sendTaskbar(TaskbarProgressMode.indeterminate, 0);
        } else {
          _sendTaskbar(TaskbarProgressMode.normal, view.percent!);
        }
      case TransferPhase.failed:
        _sendTaskbar(TaskbarProgressMode.error, 100);
      case TransferPhase.done || TransferPhase.cancelled:
        _sendTaskbar(TaskbarProgressMode.none, 0);
      case TransferPhase.hidden:
        break;
    }
  }

  static TransferCardView _with(TransferCardView view, {bool? cancelling}) =>
      TransferCardView(
        phase: view.phase,
        stage: view.stage,
        sending: view.sending,
        title: view.title,
        subtitle: view.subtitle,
        name: view.name,
        detail: view.detail,
        trailing: view.trailing,
        message: view.message,
        hint: view.hint,
        fraction: view.fraction,
        destination: view.destination,
        details: view.details,
        cancelling: cancelling ?? view.cancelling,
        suppressed: view.suppressed,
      );

  @override
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    clipboard.removeListener(_onSession);
    _showTimer?.cancel();
    _sampleTimer?.cancel();
    _doneTimer?.cancel();
    _taskbarTimer?.cancel();
    final window = this.window;
    if (window is TransferNoticePresentation) {
      (window as TransferNoticePresentation).removeListener(_presentationChanged);
    }
    if (window != null && _ownsWindow) {
      window
        ..onCancel = null
        ..onClose = null;
      if (window is TransferNoticeOpenAction) {
        (window as TransferNoticeOpenAction).onOpen = null;
      }
      if (_view.phase != TransferPhase.hidden || _taskbarKey != null) {
        unawaited(window.hide());
        unawaited(window.taskbar(TaskbarProgressMode.none, 0));
      }
      _windowOwners[window] = null;
    }
    super.dispose();
  }
}

/// A centered file-transfer surface. Its barrier covers the whole remote view,
/// while a spanning view keeps the card on its primary monitor.
class ClipboardTransferOverlay extends StatelessWidget {
  const ClipboardTransferOverlay({
    super.key,
    required this.controller,
    this.dark = true,
    this.primaryRegion,
    this.onInteract,
    this.requestFocus = true,
  });

  final ClipboardTransferController controller;
  final bool dark;
  final Rect? primaryRegion;
  final VoidCallback? onInteract;
  final bool requestFocus;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: controller,
    builder: (context, _) {
      final blocking = controller.blocksInteraction;
      final visible =
          controller.phase != TransferPhase.hidden &&
          !controller.view.suppressed;
      if (!blocking && !visible) return const SizedBox.shrink();
      Widget card = Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: FocusScope(
            autofocus: blocking && requestFocus,
            onKeyEvent: (_, event) {
              if (blocking &&
                  event is KeyDownEvent &&
                  event.logicalKey == LogicalKeyboardKey.escape) {
                controller.cancel();
                return KeyEventResult.handled;
              }
              return KeyEventResult.ignored;
            },
            child: SingleChildScrollView(
              child:
                  blocking &&
                      (controller.phase == TransferPhase.hidden ||
                          controller.view.suppressed)
                  ? Listener(
                      onPointerDown: (_) => onInteract?.call(),
                      child: DecoratedBox(
                        key: const Key('clipboard-transfer-pending'),
                        decoration: _surface(SessionChrome.forDark(dark)),
                        child: Padding(
                          padding: const EdgeInsets.symmetric(
                            horizontal: 16,
                            vertical: 12,
                          ),
                          child: Row(
                            mainAxisSize: MainAxisSize.min,
                            children: [
                              Flexible(
                                child: KeepAllText(
                                  l10n.transferringFiles,
                                  style: _text(
                                    SessionChrome.forDark(dark),
                                    14,
                                    SessionChrome.forDark(dark).ink,
                                  ),
                                ),
                              ),
                              const SizedBox(width: 16),
                              TextButton(
                                key: const Key('clipboard-cancel'),
                                onPressed: controller.cancel,
                                child: KeepAllText(l10n.cancel),
                              ),
                            ],
                          ),
                        ),
                      ),
                    )
                  : ClipboardTransferCard(
                      controller: controller,
                      dark: dark,
                      maxWidth: 420,
                      modal: true,
                      onInteract: onInteract,
                    ),
            ),
          ),
        ),
      );
      // Offstage keeps the focused Escape handler and transfer state alive.
      // Only visual/card hit testing moves to Native; the barrier stays below.
      card = Offstage(offstage: controller.presentedExternally, child: card);
      if (primaryRegion case final region?) {
        card = Positioned.fromRect(rect: region, child: card);
      }
      final result = Stack(
        fit: StackFit.expand,
        children: [
          if (blocking)
            ModalBarrier(
              key: const Key('clipboard-transfer-barrier'),
              color: Colors.black.withValues(alpha: dark ? .42 : .22),
              dismissible: false,
              semanticsLabel: l10n.transferringFiles,
            ),
          card,
        ],
      );
      return blocking
          ? MouseRegion(cursor: SystemMouseCursors.basic, child: result)
          : result;
    },
  );
}

/// The transfer card uses the established session-chrome surface and type.
/// Place it with [Align] / [Positioned]; it asks for no more than [maxWidth] and shrinks with the
/// window. [onInteract] runs when the pointer goes down on it (the session releases a captured
/// keyboard there, like the toolbar).
class ClipboardTransferCard extends StatelessWidget {
  const ClipboardTransferCard({
    super.key,
    required this.controller,
    this.dark = true,
    this.onInteract,
    this.maxWidth = 360,
    this.modal = false,
  });
  final ClipboardTransferController controller;
  final bool dark;
  final VoidCallback? onInteract;
  final double maxWidth;
  final bool modal;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: controller,
    builder: (context, _) {
      final view = controller.view;
      if (view.phase == TransferPhase.hidden || view.suppressed) {
        return const SizedBox.shrink();
      }
      final chrome = SessionChrome.forDark(dark);
      final collapsed =
          !modal && controller.collapsed && view.phase == TransferPhase.running;
      return Listener(
        onPointerDown: (_) => onInteract?.call(),
        behavior: HitTestBehavior.deferToChild,
        child: AnimatedOpacity(
          opacity: controller.fading ? 0 : 1,
          duration: controller.fade,
          child: collapsed
              ? _Pill(controller: controller, chrome: chrome)
              : _Card(
                  controller: controller,
                  chrome: chrome,
                  maxWidth: maxWidth,
                  collapsible: !modal,
                ),
        ),
      );
    },
  );
}

TextStyle _text(
  SessionChrome chrome,
  double size,
  Color color, {
  bool bold = false,
  List<FontFeature>? features,
}) => TextStyle(
  fontFamily: 'Pretendard',
  fontFamilyFallback: const ['Malgun Gothic', 'Segoe UI'],
  fontSize: size,
  height: 1.35,
  color: color,
  fontWeight: bold ? FontWeight.w600 : FontWeight.w400,
  fontFeatures: features,
  decoration: TextDecoration.none,
);

// Digits of equal width: the numbers do not jiggle as they change.
const _tabular = [FontFeature.tabularFigures()];

BoxDecoration _surface(SessionChrome chrome, {double radius = 14}) =>
    BoxDecoration(
      color: chrome.surface,
      borderRadius: BorderRadius.circular(radius),
      border: Border.all(color: chrome.edge),
      boxShadow: [
        BoxShadow(
          color: chrome.shadow,
          blurRadius: 24,
          spreadRadius: -6,
          offset: const Offset(0, 10),
        ),
      ],
    );

class _Card extends StatelessWidget {
  const _Card({
    required this.controller,
    required this.chrome,
    required this.maxWidth,
    required this.collapsible,
  });
  final ClipboardTransferController controller;
  final SessionChrome chrome;
  final double maxWidth;
  final bool collapsible;

  @override
  Widget build(BuildContext context) {
    final view = controller.view;
    final running = view.phase == TransferPhase.running;
    final failed = view.phase == TransferPhase.failed;
    final done = view.phase == TransferPhase.done;
    final percent = view.percent;
    return ConstrainedBox(
      constraints: BoxConstraints(maxWidth: maxWidth),
      child: Semantics(
        container: true,
        child: DecoratedBox(
          key: const Key('clipboard-transfer'),
          decoration: _surface(chrome),
          child: Padding(
            padding: const EdgeInsets.fromLTRB(16, 14, 12, 12),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Row(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    ExcludeSemantics(
                      child: _Badge(view: view, chrome: chrome),
                    ),
                    const SizedBox(width: 12),
                    Expanded(
                      child: Padding(
                        padding: const EdgeInsets.only(top: 1, right: 4),
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            // The title changes only with the phase: a screen
                            // reader hears each phase once, not every tick.
                            Semantics(
                              liveRegion: true,
                              child: KeepAllText(
                                view.title,
                                key: const Key('clipboard-transfer-title'),
                                maxLines: 2,
                                overflow: TextOverflow.ellipsis,
                                style: _text(
                                  chrome,
                                  14,
                                  failed ? chrome.danger : chrome.ink,
                                  bold: true,
                                ),
                              ),
                            ),
                            if (view.subtitle.isNotEmpty)
                              Padding(
                                padding: const EdgeInsets.only(top: 2),
                                child: _Subtitle(view: view, chrome: chrome),
                              ),
                          ],
                        ),
                      ),
                    ),
                    if (running &&
                        percent != null &&
                        view.stage != TransferStage.preparing)
                      Padding(
                        padding: const EdgeInsets.only(top: 1, left: 8, right: 4),
                        child: Text(
                          '$percent%',
                          key: const Key('clipboard-percent'),
                          style: _text(
                            chrome,
                            14,
                            chrome.accent,
                            bold: true,
                            features: _tabular,
                          ),
                        ),
                      ),
                    if (done && view.destination != null)
                      Padding(
                        padding: const EdgeInsets.only(left: 8, right: 4),
                        child: Tooltip(
                          message: view.destination!.path,
                          child: TextButton.icon(
                            key: const Key('clipboard-open-folder'),
                            onPressed: () => unawaited(controller.openFolder()),
                            style: _buttonStyle(chrome, primary: true),
                            icon: const Icon(
                              Icons.folder_open_rounded,
                              size: 18,
                            ),
                            label: KeepAllText(l10n.openFolder),
                          ),
                        ),
                      ),
                    if (running && collapsible)
                      Tooltip(
                        message: l10n.minimize,
                        child: IconButton(
                          key: const Key('clipboard-collapse'),
                          onPressed: controller.toggleCollapsed,
                          icon: const Icon(Icons.expand_more_rounded, size: 20),
                          color: chrome.muted,
                          visualDensity: VisualDensity.compact,
                          padding: EdgeInsets.zero,
                          constraints: const BoxConstraints.tightFor(
                            width: 28,
                            height: 28,
                          ),
                          mouseCursor: SystemMouseCursors.click,
                        ),
                      ),
                  ],
                ),
                if (running) ...[
                  Padding(
                    padding: const EdgeInsets.only(top: 12, right: 4),
                    child: ClipRRect(
                      borderRadius: BorderRadius.circular(3),
                      child: LinearProgressIndicator(
                        key: const Key('clipboard-progress-bar'),
                        value: view.fraction,
                        minHeight: 6,
                        color: chrome.accent,
                        backgroundColor: chrome.dark
                            ? const Color(0xFF2A3238)
                            : const Color(0xFFE4E8EB),
                        semanticsLabel: l10n.transferProgress,
                      ),
                    ),
                  ),
                  if (view.detail.isNotEmpty || view.trailing.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(top: 8, right: 4),
                      child: Wrap(
                        alignment: WrapAlignment.spaceBetween,
                        spacing: 12,
                        children: [
                          if (view.detail.isNotEmpty)
                            Text(
                              view.detail,
                              key: const Key('clipboard-progress-size'),
                              style: _text(
                                chrome,
                                12,
                                chrome.muted,
                                features: _tabular,
                              ),
                            ),
                          if (view.trailing.isNotEmpty)
                            Text(
                              view.trailing,
                              key: const Key('clipboard-progress-left'),
                              style: _text(
                                chrome,
                                12,
                                chrome.ink,
                                features: _tabular,
                              ),
                            ),
                        ],
                      ),
                    ),
                  Padding(
                    padding: const EdgeInsets.only(top: 6),
                    child: Row(
                      children: [
                        if (view.details.isNotEmpty)
                          TextButton.icon(
                            key: const Key('clipboard-details-toggle'),
                            onPressed: controller.toggleDetails,
                            style: _linkStyle(chrome),
                            iconAlignment: IconAlignment.end,
                            icon: Icon(
                              controller.detailsOpen
                                  ? Icons.expand_less_rounded
                                  : Icons.expand_more_rounded,
                              size: 18,
                            ),
                            label: KeepAllText(
                              controller.detailsOpen ? l10n.less : l10n.details,
                            ),
                          ),
                        const Spacer(),
                        Tooltip(
                          message: view.cancelling
                              ? l10n.canceling
                              : l10n.cancelTransfer,
                          child: TextButton(
                            key: const Key('clipboard-cancel'),
                            onPressed: view.cancelling
                                ? null
                                : controller.cancel,
                            style: _buttonStyle(chrome),
                            child: KeepAllText(
                              view.cancelling ? l10n.canceling2 : l10n.cancel,
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                  if (controller.detailsOpen && view.details.isNotEmpty)
                    _Details(rows: view.details, chrome: chrome),
                ],
                if (failed) ...[
                  Padding(
                    padding: const EdgeInsets.only(top: 8, left: 44, right: 4),
                    child: KeepAllText(
                      view.message,
                      key: const Key('clipboard-transfer-message'),
                      maxLines: 4,
                      overflow: TextOverflow.ellipsis,
                      style: _text(chrome, 13, chrome.ink),
                    ),
                  ),
                  if (view.hint.isNotEmpty)
                    Padding(
                      padding: const EdgeInsets.only(
                        top: 4,
                        left: 44,
                        right: 4,
                      ),
                      child: KeepAllText(
                        view.hint,
                        key: const Key('clipboard-transfer-hint'),
                        maxLines: 4,
                        overflow: TextOverflow.ellipsis,
                        style: _text(chrome, 12, chrome.muted),
                      ),
                    ),
                  Padding(
                    padding: const EdgeInsets.only(top: 10),
                    child: Align(
                      alignment: Alignment.centerRight,
                      child: TextButton(
                        key: const Key('clipboard-close'),
                        onPressed: controller.close,
                        style: _buttonStyle(chrome),
                        child: KeepAllText(l10n.close),
                      ),
                    ),
                  ),
                ],
              ],
            ),
          ),
        ),
      ),
    );
  }

  static ButtonStyle _buttonStyle(SessionChrome chrome, {bool primary = false}) =>
      TextButton.styleFrom(
        foregroundColor: primary ? chrome.accent : chrome.ink,
        disabledForegroundColor: chrome.faint,
        textStyle: _text(chrome, 13, chrome.ink, bold: primary),
        minimumSize: const Size(64, 32),
        padding: const EdgeInsets.symmetric(horizontal: 14),
        side: BorderSide(
          color: primary ? chrome.accent.withValues(alpha: .55) : chrome.popupEdge,
        ),
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
      ).copyWith(
        overlayColor: WidgetStatePropertyAll(chrome.hover),
        mouseCursor: WidgetStateProperty.resolveWith(
          (states) => states.contains(WidgetState.disabled)
              ? SystemMouseCursors.basic
              : SystemMouseCursors.click,
        ),
      );

  static ButtonStyle _linkStyle(SessionChrome chrome) => TextButton.styleFrom(
    foregroundColor: chrome.muted,
    textStyle: _text(chrome, 12, chrome.muted),
    minimumSize: const Size(0, 32),
    padding: const EdgeInsets.symmetric(horizontal: 6),
    shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
  ).copyWith(
    overlayColor: WidgetStatePropertyAll(chrome.hover),
    mouseCursor: const WidgetStatePropertyAll(SystemMouseCursors.click),
  );
}

/// The round icon at the start of the title row: the direction while it runs, then the outcome.
class _Badge extends StatelessWidget {
  const _Badge({required this.view, required this.chrome});
  final TransferCardView view;
  final SessionChrome chrome;

  @override
  Widget build(BuildContext context) {
    final (icon, color) = switch (view.phase) {
      TransferPhase.done => (Icons.check_rounded, chrome.good),
      TransferPhase.failed => (Icons.priority_high_rounded, chrome.danger),
      TransferPhase.cancelled => (Icons.close_rounded, chrome.muted),
      _ => (
        view.sending ? Icons.upload_rounded : Icons.download_rounded,
        chrome.accent,
      ),
    };
    return Container(
      key: const Key('clipboard-transfer-badge'),
      width: 32,
      height: 32,
      decoration: BoxDecoration(
        color: color.withValues(alpha: chrome.dark ? .16 : .12),
        shape: BoxShape.circle,
      ),
      child: Icon(icon, size: 19, color: color),
    );
  }
}

/// The line under the title: the destination folder (with its full path as a tooltip) and what
/// moves; a lone file's name is shortened in the middle, with the whole name as a tooltip.
class _Subtitle extends StatelessWidget {
  const _Subtitle({required this.view, required this.chrome});
  final TransferCardView view;
  final SessionChrome chrome;

  @override
  Widget build(BuildContext context) {
    final style = _text(chrome, 12, chrome.muted, features: _tabular);
    final destination = view.destination;
    final name = view.name;
    final running = view.phase == TransferPhase.running;
    var rest = view.subtitle;
    if (running && destination != null && rest.startsWith(destination.name)) {
      rest = rest.substring(destination.name.length);
      if (rest.startsWith(' · ')) rest = rest.substring(3);
    }
    final lone = name != null && rest == name;
    Widget restText = lone
        ? Tooltip(
            message: name,
            child: Text(
              middleEllipsize(name, 44),
              key: const Key('clipboard-transfer-name'),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: style,
            ),
          )
        : KeepAllText(
            rest,
            key: const Key('clipboard-transfer-subtitle'),
            maxLines: 2,
            overflow: TextOverflow.ellipsis,
            style: style,
          );
    if (!running || destination == null) return restText;
    // The folder takes what its name needs (at most about half the line); the count gets the rest.
    return LayoutBuilder(
      builder: (context, constraints) => Row(
        children: [
          ConstrainedBox(
            constraints: BoxConstraints(
              maxWidth: rest.isEmpty
                  ? constraints.maxWidth
                  : constraints.maxWidth * .55,
            ),
            child: Tooltip(
              message: destination.path,
              child: Row(
                key: const Key('clipboard-destination'),
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.folder_rounded, size: 14, color: chrome.accent),
                  const SizedBox(width: 4),
                  Flexible(
                    child: Text(
                      destination.name,
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: _text(chrome, 12, chrome.ink),
                    ),
                  ),
                ],
              ),
            ),
          ),
          if (rest.isNotEmpty) ...[
            Text('  ·  ', style: style),
            Expanded(child: restText),
          ],
        ],
      ),
    );
  }
}

/// 자세히: the current file, the counts, the folder; long values shortened in the middle with the
/// whole value as a tooltip.
class _Details extends StatelessWidget {
  const _Details({required this.rows, required this.chrome});
  final List<(String, String)> rows;
  final SessionChrome chrome;

  @override
  Widget build(BuildContext context) => Container(
    key: const Key('clipboard-details'),
    margin: const EdgeInsets.only(top: 4, right: 4),
    padding: const EdgeInsets.fromLTRB(12, 10, 12, 10),
    decoration: BoxDecoration(
      color: chrome.popupHighlight,
      borderRadius: BorderRadius.circular(10),
    ),
    // The label column is as wide as its longest label at any text size.
    child: Table(
      columnWidths: const {0: IntrinsicColumnWidth(), 1: FlexColumnWidth()},
      defaultVerticalAlignment: TableCellVerticalAlignment.top,
      children: [
        for (final (label, value) in rows)
          TableRow(
            children: [
              Padding(
                padding: const EdgeInsets.only(top: 2, bottom: 2, right: 12),
                child: Text(
                  label,
                  softWrap: false,
                  style: _text(chrome, 12, chrome.muted),
                ),
              ),
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 2),
                child: Tooltip(
                  message: value,
                  child: Text(
                    middleEllipsize(value, 48),
                    key: label == l10n.currentFile
                        ? const Key('clipboard-transfer-name')
                        : null,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: _text(chrome, 12, chrome.ink, features: _tabular),
                  ),
                ),
              ),
            ],
          ),
      ],
    ),
  );
}

/// The folded card: a ring with the percent. A click opens the card again.
class _Pill extends StatelessWidget {
  const _Pill({required this.controller, required this.chrome});
  final ClipboardTransferController controller;
  final SessionChrome chrome;

  @override
  Widget build(BuildContext context) {
    final view = controller.view;
    final percent = view.percent;
    return Tooltip(
      message: l10n.showTransferDetails,
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          key: const Key('clipboard-pill'),
          borderRadius: BorderRadius.circular(20),
          mouseCursor: SystemMouseCursors.click,
          onTap: controller.toggleCollapsed,
          child: Ink(
            decoration: _surface(chrome, radius: 20),
            padding: const EdgeInsets.fromLTRB(10, 7, 14, 7),
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                SizedBox(
                  width: 18,
                  height: 18,
                  child: CircularProgressIndicator(
                    value: view.fraction,
                    strokeWidth: 2.5,
                    color: chrome.accent,
                    backgroundColor: chrome.faint.withValues(alpha: .4),
                  ),
                ),
                const SizedBox(width: 8),
                Text(
                  percent == null ? l10n.transferring : '$percent%',
                  key: const Key('clipboard-pill-text'),
                  style: _text(chrome, 12, chrome.ink, bold: true),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
