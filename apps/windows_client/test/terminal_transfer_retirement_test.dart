// PRIVATE diagnostic v4 proposal. All production and canonical v2 test bytes
// remain unchanged. Fixed phase/boolean observations identify the exact await.
// Actual RemoteView/transfer controller/RemoteSession; only clipboard native
// completion and the child-window close boundary are held by this fixture.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'remote_session_test.dart' show SessionSocket;
import 'session_recovery_test.dart'
    show machine, session, connected, RecoveryMedia, mockPresentation;

enum _Case { first, successor, failedDrain }
enum _Phase {
  mount, connected, manualClose, retire, closeCallback, sessionCleanup,
  knownCleanupBudget, releaseClipboard, retirementComplete, bodyDone,
  teardownBegin, removeWidget, closeMedia, closeNextMedia, recheck,
}
enum _Event {
  before, after, observed, completed, beforeBudget, afterBudget, check,
  verifiedReturn,
}

void _mark(_Case c, _Phase p, _Event e,
    {bool done = false, bool failed = false}) => debugPrintSynchronously(
  'PC22_V4 case=${c.name} phase=${p.name} event=${e.name} done=$done failed=$failed',
);

/// Observe one exact existing Future. Check bounded fake-clock progress before
/// awaiting it, so a stuck fixture fails at its stage instead of hanging.
class _Progress {
  _Progress(this.work, this.c, this.phase) {
    _mark(c, phase, _Event.observed);
    unawaited(work.then<void>((_) {
      done = true;
      _mark(c, phase, _Event.completed, done: true);
    }, onError: (Object _, StackTrace _) {
      done = failed = true;
      _mark(c, phase, _Event.completed, done: true, failed: true);
    }));
  }
  final Future<dynamic> work;
  final _Case c;
  final _Phase phase;
  bool done = false, failed = false;

  Future<void> verify(WidgetTester tester,
      {Duration budget = Duration.zero}) async {
    _mark(c, phase, _Event.before, done: done, failed: failed);
    await tester.pump(); // Explicit microtask/frame progress, no clock advance.
    _mark(c, phase, _Event.after, done: done, failed: failed);
    if (!done && budget > Duration.zero) {
      _mark(c, phase, _Event.beforeBudget);
      await tester.pump(budget);
      _mark(c, phase, _Event.afterBudget, done: done, failed: failed);
    }
    _mark(c, phase, _Event.check, done: done, failed: failed);
    expectSync(done, isTrue, reason: 'Bounded progress: ${phase.name}');
    expectSync(failed, isFalse, reason: 'Successful completion: ${phase.name}');
    // The exact original Future has already completed successfully. Return
    // after that proof without re-awaiting its earlier fake-clock zone.
    _mark(c, phase, _Event.verifiedReturn, done: done, failed: failed);
  }
}

class _HeldClipboard extends ClipboardSession {
  _HeldClipboard({this.failDrain = false})
    : super(
        url: Uri.parse('wss://fixture.invalid/clipboard'),
        token: 'fixture-only',
        sessionId: 'retired-session-only',
      );
  final bool failDrain;
  final drain = Completer<void>();
  Future<void>? _closing;
  bool _closed = false, drained = false;
  int closeStarts = 0, cancelRequests = 0;
  @override
  bool get closed => _closed;
  @override
  bool get transferring => !_closed;
  @override
  bool get userWaiting => !_closed;
  @override
  ClipboardProgress? get progress => const ClipboardProgress(
    sending: false,
    format: 'files',
    fileCount: 1,
    fileIndex: 1,
    transferred: 1024,
    total: 8192,
    fileName: 'synthetic.bin',
  );
  @override
  Future<void> cancelTransfer() async {
    // The real API may only send a cancellation request. This is deliberately
    // NOT proof that the native/pending/owned-work lanes have drained.
    cancelRequests++;
  }

  @override
  Future<void> close({String reason = 'clipboard_transport_closed'}) =>
      _closing ??= _close();
  Future<void> _close() async {
    closeStarts++;
    _closed = true;
    notifyListeners();
    await drain.future;
    if (failDrain) throw StateError('synthetic_native_cleanup_failed');
    drained = true;
  }

  void finishDrain() {
    if (!drain.isCompleted) drain.complete();
  }
}

class _Notice implements TransferNoticeWindow {
  @override
  set onCancel(VoidCallback? value) {}
  @override
  set onClose(VoidCallback? value) {}
  @override
  Future<void> update(TransferNoticeContent content) async {}
  @override
  Future<void> hide() async {}
  @override
  Future<void> taskbar(TaskbarProgressMode mode, int percent) async {}
}

void main() {
  testWidgets(
    'ordinary close stays blocked; terminal retirement verifies exact transfer drain before child close',
    (tester) async {
      const c = _Case.first;
      mockPresentation();
      final socket = SessionSocket(), media = RecoveryMedia();
      final old = session(socket, media), clipboard = _HeldClipboard();
      old.clipboard = clipboard;
      var childCloses = 0, closeEntries = 0, retirementFinished = false;
      Map<String, bool>? closeState;
      Object? retirementError;
      final actions = SessionWindowActions(
        closeWindow: () async {
          _mark(c, _Phase.closeCallback, _Event.before);
          closeEntries++;
          closeState = {
            'sessionClosed': old.closed,
            'mediaStopped': media.stopped,
            'transportClosed': socket.wasClosed,
            'clipboardDrained': clipboard.drained,
          };
          expectSync(old.closed, isTrue);
          expectSync(media.stopped, isTrue);
          expectSync(socket.wasClosed, isTrue);
          expectSync(clipboard.drained, isTrue,
              reason: 'A cancel request or media timeout is not native drain proof');
          childCloses++;
          _mark(c, _Phase.closeCallback, _Event.after, done: true);
        },
      );
      addTearDown(() async {
        _mark(c, _Phase.teardownBegin, _Event.before);
        clipboard.finishDrain();
        _mark(c, _Phase.removeWidget, _Event.before);
        await tester.pumpWidget(const SizedBox.shrink());
        _mark(c, _Phase.removeWidget, _Event.after);
        _mark(c, _Phase.knownCleanupBudget, _Event.before);
        await tester.pump(const Duration(seconds: 3));
        _mark(c, _Phase.knownCleanupBudget, _Event.after);
        await _Progress(old.close(), c, _Phase.sessionCleanup).verify(tester);
        await _Progress(media.events.close(), c, _Phase.closeMedia).verify(tester);
      });
      _mark(c, _Phase.mount, _Event.before);
      await tester.pumpWidget(
        MaterialApp(
          home: RemoteView(
            machine: machine,
            load: () async => old,
            initialDisplayMode: SessionDisplayMode.windowed,
            windowActions: actions,
            transferNoticeWindow: _Notice(),
            sessionGuideEnabled: false,
          ),
        ),
      );
      _mark(c, _Phase.mount, _Event.after);
      await tester.pump();
      _mark(c, _Phase.connected, _Event.before);
      await connected(tester, old, socket);
      _mark(c, _Phase.connected, _Event.after);
      await tester.pump();
      expect(old.clipboardTransferBlocked, isTrue,
          reason: 'The real view/controller observes the active file transfer');

      await _Progress(actions.requestClose(), c, _Phase.manualClose).verify(tester);
      await tester.pump();
      expect(old.closed, isFalse);
      expect(childCloses, 0);
      expect(clipboard.closeStarts, 0);
      expect(find.byType(AppConfirmDialog), findsNothing,
          reason: 'Ordinary user close still requires finishing/cancelling the transfer');

      _mark(c, _Phase.retire, _Event.before);
      final retirement = actions.retire().then<void>(
        (_) => retirementFinished = true,
        onError: (Object error) { retirementError = error; },
      );
      _mark(c, _Phase.retire, _Event.after);
      final retirementProgress = _Progress(retirement, c, _Phase.retirementComplete);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 20));
      // Expected first RED on immutable20/21: _closeView returned at the
      // transfer guard, so no clipboard/session retirement has started.
      expect(clipboard.closeStarts, 1);
      expect(old.closed, isTrue);
      // close() coalesces _closing. Only join it after the production path's
      // closed assertion; this observation cannot start cleanup in its place.
      final sessionCleanup = _Progress(old.close(), c, _Phase.sessionCleanup);
      expect(clipboard.drained, isFalse);
      expect(childCloses, 0);
      expect(retirementFinished, isFalse);

      // RemoteSession.close currently swallows its clipboard timeout at2s.
      // A terminal callback must separately retain the exact drain Future.
      _mark(c, _Phase.knownCleanupBudget, _Event.before);
      await tester.pump(const Duration(seconds: 3));
      _mark(c, _Phase.knownCleanupBudget, _Event.after);
      // All RemoteSession close wrappers are already bounded at2seconds;
      // the reservation release is empty here and USB is null. The original
      //3second fake-clock advance must complete this best-effort Future while
      // the exact full clipboard drain remains held.
      await sessionCleanup.verify(tester);
      expect(childCloses, 0);
      expect(retirementFinished, isFalse);
      expect(retirementError, isNull);
      _mark(c, _Phase.releaseClipboard, _Event.before);
      clipboard.finishDrain();
      _mark(c, _Phase.releaseClipboard, _Event.after);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 20));
      await retirementProgress.verify(tester);
      expect(clipboard.closeStarts, 1,
          reason: 'Both callers may join one exact old-owner close operation');
      expect(clipboard.drained, isTrue);
      expect(closeEntries, 1,
          reason: 'Exact close callback entry/state: $closeState');
      expect(childCloses, 1,
          reason: 'Close callback entries=$closeEntries state=$closeState');
      expect(retirementFinished, isTrue);
      expect(retirementError, isNull);
      _mark(c, _Phase.bodyDone, _Event.completed, done: true);
    },
  );

  testWidgets(
    'late old transfer drain cannot close or retire a successor view',
    (tester) async {
      const c = _Case.successor;
      mockPresentation();
      final oldSocket = SessionSocket(), oldMedia = RecoveryMedia();
      final old = session(oldSocket, oldMedia), clipboard = _HeldClipboard();
      old.clipboard = clipboard;
      final nextSocket = SessionSocket(), nextMedia = RecoveryMedia();
      final next = session(nextSocket, nextMedia);
      var oldCloses = 0, nextCloses = 0;
      final oldActions = SessionWindowActions(closeWindow: () async { oldCloses++; });
      final nextActions = SessionWindowActions(closeWindow: () async { nextCloses++; });
      addTearDown(() async {
        _mark(c, _Phase.teardownBegin, _Event.before);
        clipboard.finishDrain();
        _mark(c, _Phase.removeWidget, _Event.before);
        await tester.pumpWidget(const SizedBox.shrink());
        _mark(c, _Phase.removeWidget, _Event.after);
        _mark(c, _Phase.knownCleanupBudget, _Event.before);
        await tester.pump(const Duration(seconds: 3));
        _mark(c, _Phase.knownCleanupBudget, _Event.after);
        await _Progress(old.close(), c, _Phase.sessionCleanup).verify(tester);
        await _Progress(next.close(), c, _Phase.sessionCleanup).verify(tester);
        final oldMediaClosed = _Progress(oldMedia.events.close(), c, _Phase.closeMedia);
        final nextMediaClosed = _Progress(nextMedia.events.close(), c, _Phase.closeNextMedia);
        await oldMediaClosed.verify(tester);
        await nextMediaClosed.verify(tester);
      });
      Widget view({required bool successor}) => MaterialApp(
        home: RemoteView(
          key: ValueKey(successor ? 'successor-owner' : 'retired-owner'),
          machine: machine,
          load: () async => successor ? next : old,
          initialDisplayMode: SessionDisplayMode.windowed,
          windowActions: successor ? nextActions : oldActions,
          transferNoticeWindow: _Notice(),
          sessionGuideEnabled: false,
        ),
      );
      await tester.pumpWidget(view(successor: false));
      await tester.pump();
      await connected(tester, old, oldSocket);
      await tester.pump();
      expect(old.clipboardTransferBlocked, isTrue);
      _mark(c, _Phase.retire, _Event.before);
      final retirement = oldActions.retire();
      _mark(c, _Phase.retire, _Event.after);
      final retirementProgress = _Progress(retirement, c, _Phase.retirementComplete);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 20));
      expect(clipboard.closeStarts, 1);
      await tester.pumpWidget(view(successor: true));
      await tester.pump();
      await connected(tester, next, nextSocket);
      clipboard.finishDrain();
      await tester.pump();
      await tester.pump(const Duration(seconds: 3));
      await retirementProgress.verify(tester);
      expect(clipboard.drained, isTrue);
      expect(oldCloses, 0, reason: 'Disposed owner cannot issue a late window close');
      expect(nextCloses, 0);
      expect(next.closed, isFalse);
      expect(next.clipboard, isNull);
      _mark(c, _Phase.bodyDone, _Event.completed, done: true);
    },
  );

  testWidgets(
    'failed exact clipboard drain keeps the child retired and is never replayed',
    (tester) async {
      const c = _Case.failedDrain;
      mockPresentation();
      final socket = SessionSocket(), media = RecoveryMedia();
      final old = session(socket, media), clipboard = _HeldClipboard(failDrain: true);
      old.clipboard = clipboard;
      var childCloses = 0;
      final actions = SessionWindowActions(closeWindow: () async { childCloses++; });
      addTearDown(() async {
        _mark(c, _Phase.teardownBegin, _Event.before);
        clipboard.finishDrain();
        _mark(c, _Phase.removeWidget, _Event.before);
        await tester.pumpWidget(const SizedBox.shrink());
        _mark(c, _Phase.removeWidget, _Event.after);
        _mark(c, _Phase.knownCleanupBudget, _Event.before);
        await tester.pump(const Duration(seconds: 3));
        _mark(c, _Phase.knownCleanupBudget, _Event.after);
        await _Progress(old.close(), c, _Phase.sessionCleanup).verify(tester);
        await _Progress(media.events.close(), c, _Phase.closeMedia).verify(tester);
      });
      await tester.pumpWidget(MaterialApp(home: RemoteView(
        machine: machine, load: () async => old,
        initialDisplayMode: SessionDisplayMode.windowed,
        windowActions: actions, transferNoticeWindow: _Notice(),
        sessionGuideEnabled: false,
      )));
      await tester.pump();
      await connected(tester, old, socket);
      await tester.pump();
      expect(old.clipboardTransferBlocked, isTrue);
      _mark(c, _Phase.retire, _Event.before);
      final retirement = actions.retire();
      _mark(c, _Phase.retire, _Event.after);
      final retirementProgress = _Progress(retirement, c, _Phase.retirementComplete);
      await tester.pump();
      expect(clipboard.closeStarts, 1);
      clipboard.finishDrain();
      await tester.pump();
      // Only this case has not already advanced the original3second cleanup
      // budget. Advance it conditionally if the known close Future is pending.
      await retirementProgress.verify(tester, budget: const Duration(seconds: 3));
      expect(old.closed, isTrue);
      expect(clipboard.drained, isFalse);
      expect(childCloses, 0);
      await _Progress(actions.retire(), c, _Phase.recheck).verify(tester);
      await _Progress(actions.retire(), c, _Phase.recheck).verify(tester);
      await _Progress(actions.requestClose(), c, _Phase.manualClose).verify(tester);
      expect(clipboard.closeStarts, 1);
      expect(clipboard.cancelRequests, 0,
          reason: 'Repeated native rechecks do not replay uncertain file mutations');
      expect(childCloses, 0);
      _mark(c, _Phase.bodyDone, _Event.completed, done: true);
    },
  );
}
