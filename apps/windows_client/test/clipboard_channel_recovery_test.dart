// The common media/session owner only. Clipboard bytes and native APIs are fake.
import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_policy.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/remote_session.dart';

import 'remote_session_test.dart' show SessionSocket, packet;
import 'session_recovery_test.dart' show RecoveryMedia, connected;
import 'session_resume_test.dart' show message, tokenA;

class _Clipboard extends ClipboardSession {
  _Clipboard()
    : super(
        url: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/qa-session/clipboard',
        ),
        token: 'fixture-only',
        sessionId: 'qa-session',
      );
  bool ended = false, retry = false;
  int starts = 0, sharingCalls = 0;
  ClipboardSession? inheritedFrom, inheritedAtStart;
  Completer<void>? cleanup;
  @override
  void inheritRecoveryFrom(ClipboardSession previous) {
    inheritedFrom = previous;
    super.inheritRecoveryFrom(previous);
  }
  @override
  bool get closed => ended;
  @override
  bool get canReconnect => ended && retry;
  @override
  Future<void> start() async {
    inheritedAtStart = inheritedFrom;
    starts++;
    ready = true;
  }

  @override
  Future<void> setSharingActive(bool active) async {
    sharingCalls++;
  }

  @override
  Future<void> close({String reason = 'clipboard_transport_closed'}) async {
    ended = true;
    ready = false;
    if (reason == 'clipboard_session_closed') retry = false;
    await cleanup?.future;
  }

  void fail({
    bool transient = true,
    String code = 'clipboard_transport_closed',
  }) {
    ended = true;
    retry = transient;
    error = code;
    ready = false;
    notifyListeners();
  }
}

class _Fixture {
  _Fixture({this.resume = false});

  /// session-resume-v1 granted: the stream can be away and come back.
  final bool resume;

  /// The client's own resume dial, held open until a test completes it.
  Completer<WebSocket>? redial;
  final socket = SessionSocket(), media = RecoveryMedia();
  final created = <_Clipboard>[];
  late final current = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'qa-session',
      webSocketUrl: Uri.parse(
        'wss://fixture.invalid/api/v1/sessions/qa-session',
      ),
      clipboardWebSocketUrl: Uri.parse(
        'wss://fixture.invalid/api/v1/sessions/qa-session/clipboard',
      ),
      clipboardPolicy: const ClipboardPolicy(textToRemote: true),
      sessionResume: resume,
      resumeGraceMs: 15000,
    ),
    token: 'fixture-only',
    media: media,
    dialer: (_, _) async => socket,
    resumeDialer: (_, _, _) => (redial = Completer<WebSocket>()).future,
    clipboardFactory: (_, _) {
      final fresh = _Clipboard();
      created.add(fresh);
      return fresh;
    },
  );
  Future<void> start(WidgetTester t) async {
    await connected(t, current, socket);
    await t.pump();
    expect(current.frameReady, isTrue);
    expect(created, hasLength(1));
  }

  Future<void> videoStillWorks(WidgetTester t) async {
    final before = current.receivedBytes;
    socket.incoming.add(packet());
    await t.pump();
    await t.pump();
    expect(current.receivedBytes, greaterThan(before));
    expect(current.closed, isFalse);
    expect(socket.wasClosed, isFalse);
    expect(media.stopped, isFalse);
    expect(
      socket.sent.where((value) => value['type'] == 'session.stop'),
      isEmpty,
    );
  }

  Future<void> close() async {
    for (final item in created) {
      if (item.cleanup != null && !item.cleanup!.isCompleted) {
        item.cleanup!.complete();
      }
    }
    await current.close();
    await media.events.close();
  }
}

void main() {
  testWidgets(
    'A transient pair failure waits for old native cleanup, creates a fresh owner and leaves video running',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      final old = f.created.single..cleanup = Completer<void>();
      old.fail();
      expect(f.current.clipboardRecovering, isTrue);
      await t.pump(const Duration(milliseconds: 999));
      expect(f.created, hasLength(1));
      await t.pump(const Duration(milliseconds: 1));
      expect(
        f.created,
        hasLength(1),
        reason: 'Never overlap native clipboard owners.',
      );
      await f.videoStillWorks(t);
      old.cleanup!.complete();
      await t.pump();
      await t.pump();
      expect(f.created, hasLength(2));
      expect(f.created.last.starts, 1);
      expect(f.created.last.inheritedAtStart, same(old));
      expect(identical(f.current.clipboard, old), isFalse);
      expect(f.current.clipboardRecovering, isFalse);
      await f.videoStillWorks(t);
      await f.close();
    },
  );

  testWidgets(
    'A feature has at most three fresh pair attempts, backoff1/2/4, without stopping the main session',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      for (final seconds in [1, 2, 4]) {
        final count = f.created.length;
        f.created.last.fail();
        await t.pump(Duration(milliseconds: seconds * 1000 - 1));
        expect(f.created, hasLength(count));
        await t.pump(const Duration(milliseconds: 1));
        await t.pump();
        expect(f.created, hasLength(count + 1));
      }
      f.created.last.fail();
      await t.pump(const Duration(seconds: 10));
      expect(f.created, hasLength(4));
      expect(f.current.clipboardRecovering, isFalse);
      await f.videoStillWorks(t);
      await f.close();
    },
  );

  for (final code in [
    'authorization_revoked',
    'clipboard_policy_denied',
    'clipboard_invalid_message',
  ]) {
    testWidgets(
      'Policy/security closure stays closed without automatic pair attempts: $code',
      (t) async {
        final f = _Fixture();
        await f.start(t);
        f.created.single.fail(transient: false, code: code);
        await t.pump(const Duration(seconds: 8));
        expect(f.created, hasLength(1));
        expect(f.current.clipboardRecovering, isFalse);
        await f.videoStillWorks(t);
        await f.close();
      },
    );
  }

  testWidgets(
    'Closing video while clipboard cleanup is pending cancels the scheduled replacement',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      final old = f.created.single..cleanup = Completer<void>();
      old.fail();
      await t.pump(const Duration(seconds: 1));
      unawaited(f.current.close());
      await t.pump(const Duration(seconds: 2));
      old.cleanup!.complete();
      await t.pump();
      await t.pump(const Duration(seconds: 8));
      expect(f.created, hasLength(1));
      expect(f.current.closed, isTrue);
      await f.close();
    },
  );

  testWidgets(
    'An old native cleanup that times out never permits an overlapping fresh owner',
    (t) async {
      final f = _Fixture();
      await f.start(t);
      f.created.single.cleanup = Completer<void>();
      f.created.single.fail();
      for (final seconds in [1, 2, 4]) {
        await t.pump(Duration(seconds: seconds));
        await t.pump(const Duration(seconds: 6));
        await t.pump();
        expect(f.created, hasLength(1));
      }
      expect(f.current.clipboardRecovering, isFalse);
      await f.videoStillWorks(t);
      await f.close();
    },
  );
  // session-resume-v1: the clipboard channel is lost with the stream. Its
  // attempts used to be spent in the first seconds of the outage, so a
  // connection that resumed later kept a dead clipboard.
  testWidgets(
    'While the Host is away a lost channel spends no attempt; the resumed stream starts it again with a fresh three',
    (t) async {
      final f = _Fixture(resume: true);
      await f.start(t);
      f.socket.incoming.add(message('session.resuming'));
      await t.pump();
      expect(f.current.resuming, isTrue);
      f.created.single.fail();
      expect(f.current.clipboardRecovering, isFalse);
      await t.pump(const Duration(seconds: 12));
      expect(
        f.created,
        hasLength(1),
        reason: 'A dial while the stream is away',
      );
      f.socket.incoming.add(message('session.resumed'));
      await t.pump();
      expect(f.current.resuming, isFalse);
      expect(f.current.clipboardRecovering, isTrue);
      // The same bound as before, counted from the resume: 1, 2 and 4 s.
      for (final seconds in [1, 2, 4]) {
        final count = f.created.length;
        if (count > 1) f.created.last.fail();
        await t.pump(Duration(milliseconds: seconds * 1000 - 1));
        expect(f.created, hasLength(count));
        await t.pump(const Duration(milliseconds: 1));
        await t.pump();
        expect(f.created, hasLength(count + 1));
      }
      // A channel that is really broken still ends: no fourth attempt.
      f.created.last.fail();
      await t.pump(const Duration(seconds: 10));
      expect(f.created, hasLength(4));
      expect(f.current.clipboardRecovering, isFalse);
      await f.videoStillWorks(t);
      await f.close();
    },
  );

  testWidgets(
    'Attempts spent before the stream went away do not count against the resumed stream, and a delay that ends while it is away is not spent',
    (t) async {
      final f = _Fixture(resume: true);
      await f.start(t);
      for (final seconds in [1, 2]) {
        f.created.last.fail();
        await t.pump(Duration(seconds: seconds));
        await t.pump();
      }
      expect(f.created, hasLength(3));
      // The third delay (4 s) is running when the Host goes away.
      f.created.last.fail();
      expect(f.current.clipboardRecovering, isTrue);
      await t.pump(const Duration(seconds: 1));
      f.socket.incoming.add(message('session.resuming'));
      await t.pump();
      await t.pump(const Duration(seconds: 10));
      expect(
        f.created,
        hasLength(3),
        reason: 'A dial while the stream is away',
      );
      expect(f.current.clipboardRecovering, isFalse);
      f.socket.incoming.add(message('session.resumed'));
      await t.pump();
      for (final seconds in [1, 2, 4]) {
        final count = f.created.length;
        if (count > 3) f.created.last.fail();
        await t.pump(Duration(seconds: seconds));
        await t.pump();
        expect(f.created, hasLength(count + 1));
      }
      f.created.last.fail();
      await t.pump(const Duration(seconds: 10));
      expect(f.created, hasLength(6));
      expect(f.current.clipboardRecovering, isFalse);
      await f.close();
    },
  );

  testWidgets(
    'A refusal while the stream is away stays closed after the resume',
    (t) async {
      final f = _Fixture(resume: true);
      await f.start(t);
      f.socket.incoming.add(message('session.resuming'));
      await t.pump();
      f.created.single.fail(transient: false, code: 'authorization_revoked');
      f.socket.incoming.add(message('session.resumed'));
      await t.pump();
      expect(f.current.resuming, isFalse);
      await t.pump(const Duration(seconds: 8));
      expect(f.created, hasLength(1));
      expect(f.current.clipboardRecovering, isFalse);
      await f.videoStillWorks(t);
      await f.close();
    },
  );

  testWidgets('A resume with a live channel starts nothing', (t) async {
    final f = _Fixture(resume: true);
    await f.start(t);
    f.socket.incoming.add(message('session.resuming'));
    await t.pump();
    f.socket.incoming.add(message('session.resumed'));
    await t.pump();
    await t.pump(const Duration(seconds: 8));
    expect(f.created, hasLength(1));
    expect(f.current.clipboardRecovering, isFalse);
    await f.close();
  });

  testWidgets(
    'When the client socket is replaced the channel waits for the new socket, then starts again',
    (t) async {
      final f = _Fixture(resume: true);
      await f.start(t);
      f.socket.incoming.add(message('session.resume.token', tokenA));
      await t.pump();
      unawaited(f.socket.incoming.close()); // dropped without a close frame
      await t.pump();
      await t.pump();
      expect(f.current.resuming, isTrue, reason: f.current.error);
      f.created.single.fail();
      await t.pump(const Duration(seconds: 12));
      expect(
        f.created,
        hasLength(1),
        reason: 'A dial while the stream is away',
      );
      expect(f.current.clipboardRecovering, isFalse);
      final second = SessionSocket();
      f.redial!.complete(second);
      await t.pump();
      await t.pump();
      expect(f.current.resuming, isFalse);
      expect(f.current.closed, isFalse, reason: f.current.error);
      expect(f.current.clipboardRecovering, isTrue);
      await t.pump(const Duration(seconds: 1));
      await t.pump();
      expect(f.created, hasLength(2));
      expect(f.created.last.starts, 1);
      await f.current.close();
      await f.media.events.close();
    },
  );
}
