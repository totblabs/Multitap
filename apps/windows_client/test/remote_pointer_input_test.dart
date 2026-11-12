// Real Flutter pointer dispatch and RemoteSession ordering; media/socket are
// fixtures, so this is neither physical-key nor real-VM latency acceptance.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/ui/remote_view.dart';

import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket, SessionMedia, packet;
import 'viewport_session_test.dart' show startInfo;

class _PointerMedia extends SessionMedia {
  Completer<bool>? repeatedClaim;
  final claimReplies = <bool>[];
  Completer<void>? releaseReply;
  bool deferReleaseEvents = false;
  bool nativeHeld = false;
  int nativeGeneration = 0;
  @override
  Future<bool> capture(bool enabled) {
    final generation = ++nativeGeneration;
    if (!enabled) {
      captures.add(false);
      final held = nativeHeld;
      void emitRelease() {
        if (!held) return;
        nativeHeld = false;
        events.add({'type': 'input.reset'});
        events.add({'type': 'capture.released'});
      }
      final reply = releaseReply;
      if (deferReleaseEvents && reply != null) {
        return reply.future.then((_) {
          emitRelease();
          return false;
        });
      }
      emitRelease();
      return reply == null
          ? Future.value(false)
          : reply.future.then((_) => false);
    }
    final held = repeatedClaim;
    if (enabled && held != null) {
      captures.add(enabled);
      return held.future.then((accepted) {
        if (generation == nativeGeneration) nativeHeld = accepted;
        return accepted;
      });
    }
    if (enabled && claimReplies.isNotEmpty) {
      captures.add(enabled);
      nativeHeld = claimReplies.removeAt(0);
      return Future.value(nativeHeld);
    }
    nativeHeld = enabled;
    return super.capture(enabled);
  }

  @override
  Future<int> start(int width, int height) async {
    starts++;
    return 42;
  }
}

class _PointerFixture {
  final socket = SessionSocket();
  final media = _PointerMedia();
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'pointer-fixture',
      webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/pointer'),
      epochMedia: true,
      viewportProvisioning: true,
    ),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
  );
  Duration now = Duration.zero;
  late TestGesture mouse;

  List<Map<String, dynamic>> get inputs =>
      socket.sent.where((event) => event['type'] == 'input').toList();
  Rect bounds(WidgetTester tester) =>
      tester.getRect(find.byKey(const Key('remote-image-bounds')));
  Offset point(WidgetTester tester, double x, double y) {
    final rect = bounds(tester);
    return rect.topLeft + Offset(rect.width * x, rect.height * y);
  }

  Future<void> mount(WidgetTester tester, {
    String key = 'pointer-view',
    bool claim = true,
    bool childWindow = false,
    bool batchedFirstFrame = false,
  }) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1280, 720);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const window = MethodChannel('multitap/window');
    const events = MethodChannel('multitap/window/events');
    messenger.setMockMethodCallHandler(window, (_) async => false);
    messenger.setMockMethodCallHandler(events, (_) async => null);
    await tester.pumpWidget(
      MaterialApp(
        home: RemoteView(
          key: ValueKey(key),
          machine: machine(1),
          load: () async => session,
          pointerElapsed: () => now,
          windowActions: childWindow
              ? SessionWindowActions(closeWindow: () async {})
              : null,
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    socket.incoming.add(jsonEncode(startInfo));
    if (!batchedFirstFrame) {
      await tester.pump();
      await tester.pump();
    }
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    if (childWindow) {
      // The production child claims input after its automatic viewport settle,
      // rather than losing that claim to the pending 250ms resize release.
      expect(media.captures.where((enabled) => enabled), isEmpty);
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump();
      await tester.pump();
    }
    mouse = await tester.createGesture(kind: PointerDeviceKind.mouse);
    await mouse.addPointer(location: point(tester, .1, .6));
    if (!claim) return;
    await mouse.down(point(tester, .1, .6));
    await tester.pump();
    await tester.pump();
    await mouse.up();
    await tester.pump();
    expect(session.captured, isTrue);
    socket.sent.clear();
  }

  Future<void> advance(WidgetTester tester, int milliseconds) async {
    final duration = Duration(milliseconds: milliseconds);
    now += duration;
    await tester.pump(duration);
  }

  Future<void> queue(WidgetTester tester) async {
    await mouse.moveTo(point(tester, .2, .6));
    await mouse.moveTo(point(tester, .7, .6));
  }

  Future<void> close(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink());
    unawaited(session.close());
    await tester.pump(const Duration(seconds: 3));
    await tester.pump();
    unawaited(media.events.close());
  }
}

void main() {
  testWidgets('A ready session window accepts keyboard input without a picture click', (tester) async {
    final fixture = _PointerFixture();
    try {
      await fixture.mount(tester, claim: false, childWindow: true);
      await tester.pump();
      expect(fixture.session.captured, isTrue);
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(1));
      fixture.media.events.add({'type': 'input', 'event': 'key', 'vk': 0x41, 'scan': 30, 'down': true, 'extended': false});
      await tester.pump();
      expect(fixture.inputs.where((event) => event['event'] == 'key').map((event) => event['vk']), [0x41]);
    } finally {
      await fixture.close(tester);
    }
  });

  testWidgets('Batched stream metadata and first frame still claim after viewport discovery in the next build', (tester) async {
    final fixture = _PointerFixture();
    try {
      await fixture.mount(tester, claim: false, childWindow: true, batchedFirstFrame: true);
      await tester.pump();
      expect(fixture.session.captured, isTrue);
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(1));
      expect(fixture.media.nativeHeld, isTrue);
    } finally {
      await fixture.close(tester);
    }
  });

  testWidgets('Initial native foreground refusal retries finitely and explicit focus release retires it', (tester) async {
    final fixture = _PointerFixture();
    fixture.media.claimReplies.addAll([false, false, true]);
    try {
      await fixture.mount(tester, claim: false, childWindow: true);
      await tester.pump();
      expect(fixture.session.captured, isFalse);
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(1));
      await tester.pump(const Duration(milliseconds: 150));
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(2));
      FocusManager.instance.primaryFocus?.unfocus();
      await tester.pump();
      await tester.pump(const Duration(seconds: 1));
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(2));
      expect(fixture.session.captured, isFalse);
    } finally {
      await fixture.close(tester);
    }
  });

  for (final accepted in [true, false]) {
    testWidgets('Initial native activation is bounded to three attempts, final accepted=$accepted', (tester) async {
      final fixture = _PointerFixture();
      fixture.media.claimReplies.addAll([false, false, accepted, true]);
      try {
        await fixture.mount(tester, claim: false, childWindow: true);
        await tester.pump();
        expect(fixture.media.captures.where((enabled) => enabled), hasLength(1));
        await tester.pump(const Duration(milliseconds: 150));
        await tester.pump();
        expect(fixture.media.captures.where((enabled) => enabled), hasLength(2));
        await tester.pump(const Duration(milliseconds: 150));
        await tester.pump();
        expect(fixture.media.captures.where((enabled) => enabled), hasLength(3), reason: 'closed=${fixture.session.closed}, ready=${fixture.session.frameReady}, resizing=${fixture.session.resizing}, focus=${FocusManager.instance.primaryFocus}');
        await tester.pump(const Duration(seconds: 2));
        expect(fixture.media.captures.where((enabled) => enabled), hasLength(3));
        expect(fixture.session.captured, accepted);
        expect(fixture.media.claimReplies, [true]);
      } finally {
        await fixture.close(tester);
      }
    });
  }

  for (final loss in ['background', 'local-dialog']) {
    testWidgets('Initial keyboard retry cannot reclaim after $loss', (tester) async {
      final fixture = _PointerFixture();
      fixture.media.claimReplies.addAll([false, true]);
      try {
        await fixture.mount(tester, claim: false, childWindow: true);
        await tester.pump();
        expect(fixture.media.captures.where((enabled) => enabled), hasLength(1));
        if (loss == 'background') {
          tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        } else {
          final context = tester.element(find.byType(RemoteView));
          unawaited(showDialog<void>(context: context, builder: (_) => const AlertDialog(title: Text('Local settings'), content: TextField(autofocus: true))));
        }
        await tester.pump();
        await tester.pump(const Duration(seconds: 2));
        expect(fixture.media.captures.where((enabled) => enabled), hasLength(1));
        expect(fixture.session.captured, isFalse);
        if (loss == 'background') {
          tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
          await tester.pump();
          await tester.pump(const Duration(seconds: 1));
          expect(fixture.media.captures.where((enabled) => enabled), hasLength(1));
        }
      } finally {
        await fixture.close(tester);
      }
    });
  }

  testWidgets('A first quick Down and Up waits for native ownership and reaches the VM in order', (tester) async {
    final fixture = _PointerFixture();
    final pending = Completer<bool>();
    try {
      await fixture.mount(tester, claim: false);
      fixture.media.repeatedClaim = pending;
      final at = fixture.point(tester, .4, .6);
      await fixture.mouse.down(at);
      await tester.pump();
      await fixture.mouse.up();
      await tester.pump();
      expect(fixture.inputs.where((event) => event['event'] == 'button'), isEmpty);
      pending.complete(true);
      await tester.pump();
      await tester.pump();
      expect(fixture.inputs.where((event) => event['event'] == 'button').map((event) => event['down']), [true, false]);
      expect(fixture.session.captured, isTrue);
    } finally {
      if (!pending.isCompleted) pending.complete(false);
      fixture.media.repeatedClaim = null;
      await fixture.close(tester);
    }
  });

  testWidgets('Picture focus return routes native keys and a later capture release blocks them', (tester) async {
    final fixture = _PointerFixture();
    try {
      await fixture.mount(tester);
      FocusManager.instance.primaryFocus?.unfocus();
      await tester.pump(Duration.zero);
      expect(fixture.session.captured, isFalse);
      await fixture.mouse.down(fixture.point(tester, .4, .6));
      await tester.pump(Duration.zero);
      await fixture.mouse.up();
      await tester.pump(Duration.zero);
      expect(fixture.session.captured, isTrue);
      fixture.socket.sent.clear();
      fixture.media.events.add({'type': 'input', 'event': 'key', 'vk': 0x42, 'scan': 48, 'down': true, 'extended': false});
      fixture.media.events.add({'type': 'input', 'event': 'key', 'vk': 0x42, 'scan': 48, 'down': false, 'extended': false});
      await tester.pump();
      expect(fixture.inputs.where((event) => event['event'] == 'key').map((event) => event['down']), [true, false]);
      fixture.media.events.add({'type': 'capture.released'});
      await tester.pump();
      fixture.socket.sent.clear();
      fixture.media.events.add({'type': 'input', 'event': 'key', 'vk': 0x43, 'scan': 46, 'down': true, 'extended': false});
      await tester.pump();
      expect(fixture.inputs.where((event) => event['event'] == 'key'), isEmpty);
    } finally {
      await fixture.close(tester);
    }
  });

  testWidgets('A new picture claim waits for its held native release and its reset events before claiming again', (tester) async {
    final fixture = _PointerFixture();
    final release = Completer<void>();
    try {
      await fixture.mount(tester);
      expect(fixture.media.nativeHeld, isTrue);
      fixture.media.releaseReply = release;
      FocusManager.instance.primaryFocus?.unfocus();
      await tester.pump();
      expect(fixture.media.nativeHeld, isFalse);
      expect(fixture.session.captured, isFalse);
      fixture.socket.sent.clear();
      final claims = fixture.media.captures.where((enabled) => enabled).length;
      await fixture.mouse.down(fixture.point(tester, .4, .6));
      await tester.pump();
      await fixture.mouse.up();
      await tester.pump();
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(claims));
      expect(fixture.inputs.where((event) => event['event'] == 'button'), isEmpty);
      release.complete();
      // A null pump duration flushes microtasks but does not elapse FakeAsync
      // timers. Explicit zero runs the production event-loop release barrier.
      await tester.pump(Duration.zero);
      await tester.pump(Duration.zero);
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(claims + 1));
      expect(fixture.session.captured, isTrue);
      expect(fixture.media.nativeHeld, isTrue);
      expect(fixture.inputs.where((event) => event['event'] == 'button').map((event) => event['down']), [true, false]);
      fixture.media.events.add({'type': 'input', 'event': 'key', 'vk': 0x44, 'scan': 32, 'down': true, 'extended': false});
      await tester.pump();
      expect(fixture.inputs.where((event) => event['event'] == 'key').map((event) => event['vk']), [0x44]);
    } finally {
      if (!release.isCompleted) release.complete();
      fixture.media.releaseReply = null;
      await fixture.close(tester);
    }
  });

  testWidgets('Old native reset arriving after a new tap discards that uncertain tap but recovers keyboard after the release reply', (tester) async {
    final fixture = _PointerFixture();
    final release = Completer<void>();
    try {
      await fixture.mount(tester);
      fixture.media.releaseReply = release;
      fixture.media.deferReleaseEvents = true;
      FocusManager.instance.primaryFocus?.unfocus();
      await tester.pump();
      final claims = fixture.media.captures.where((enabled) => enabled).length;
      fixture.socket.sent.clear();
      await fixture.mouse.down(fixture.point(tester, .4, .6));
      await tester.pump();
      await fixture.mouse.up();
      await tester.pump();
      expect(fixture.media.captures.where((enabled) => enabled), hasLength(claims));
      release.complete();
      await tester.pump(Duration.zero);
      await tester.pump(Duration.zero);
      expect(fixture.session.captured, isTrue);
      expect(fixture.media.nativeHeld, isTrue);
      expect(fixture.inputs.where((event) => event['event'] == 'button'), isEmpty,
          reason: 'An old ownership reset invalidates uncertain pointer coordinates; it must never produce an orphan Up.');
      fixture.media.events.add({'type': 'input', 'event': 'key', 'vk': 0x45, 'scan': 18, 'down': true, 'extended': false});
      await tester.pump();
      expect(fixture.inputs.where((event) => event['event'] == 'key').map((event) => event['vk']), [0x45]);
    } finally {
      if (!release.isCompleted) release.complete();
      fixture.media.releaseReply = null;
      await fixture.close(tester);
    }
  });

  for (final loss in ['background', 'local-dialog']) {
    testWidgets('A genuine $loss while the old native release waits prevents a new claim', (tester) async {
      final fixture = _PointerFixture();
      final release = Completer<void>();
      try {
        await fixture.mount(tester);
        fixture.media.releaseReply = release;
        FocusManager.instance.primaryFocus?.unfocus();
        await tester.pump();
        final claims = fixture.media.captures.where((enabled) => enabled).length;
        await fixture.mouse.down(fixture.point(tester, .4, .6));
        await tester.pump();
        await fixture.mouse.up();
        if (loss == 'background') {
          tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
        } else {
          final context = tester.element(find.byType(RemoteView));
          unawaited(showDialog<void>(context: context, builder: (_) => const AlertDialog(title: Text('Local settings'), content: TextField(autofocus: true))));
        }
        await tester.pump();
        release.complete();
        await tester.pump();
        await tester.pump();
        expect(fixture.media.captures.where((enabled) => enabled), hasLength(claims));
        expect(fixture.session.captured, isFalse);
        expect(fixture.media.nativeHeld, isFalse);
        expect(fixture.inputs.where((event) => event['event'] == 'button'), isEmpty);
        if (loss == 'background') tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      } finally {
        if (!release.isCompleted) release.complete();
        fixture.media.releaseReply = null;
        await fixture.close(tester);
      }
    });
  }

  testWidgets('A local dialog during viewport resize stays authoritative after it closes and the new frame arrives', (tester) async {
    final fixture = _PointerFixture();
    try {
      await fixture.mount(tester);
      tester.view.physicalSize = const Size(1300, 720);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump();
      final request = fixture.socket.sent.singleWhere((event) => event['type'] == 'stream.resize');
      final ack = {...request, 'type': 'stream.resizing', 'resizeId': 'input-focus', 'epoch': 1, 'timeoutMs': 90000};
      fixture.socket.incoming.add(jsonEncode(ack));
      await tester.pump();
      expect(fixture.session.resizing, isTrue);
      final context = tester.element(find.byType(RemoteView));
      unawaited(showDialog<void>(context: context, builder: (_) => const AlertDialog(title: Text('Local settings'), content: TextField(autofocus: true))));
      await tester.pump();
      await tester.pump();
      Navigator.of(tester.element(find.byType(AlertDialog))).pop();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      fixture.socket.incoming.add(jsonEncode({...startInfo, ...ack, 'type': 'stream.info', 'codec': 'h264', 'sessionId': fixture.session.connection.sessionId, 'sourceWidth': 1300, 'sourceHeight': 720, 'fpsTarget': 60}));
      await tester.pump();
      await tester.pump();
      fixture.socket.incoming.add(packet()..[2] = 1);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 60));
      expect(fixture.session.resizing, isFalse);
      expect(fixture.session.captured, isFalse);
      expect(fixture.media.nativeHeld, isFalse);
    } finally {
      await fixture.close(tester);
    }
  });

  for (final loss in ['focus', 'lifecycle', 'native-reset', 'menu']) {
    testWidgets('A pending first click cannot replay after $loss', (tester) async {
      final fixture = _PointerFixture();
      final pending = Completer<bool>();
      try {
        await fixture.mount(tester, claim: false);
        fixture.media.repeatedClaim = pending;
        await fixture.mouse.down(fixture.point(tester, .4, .6));
        await tester.pump();
        await fixture.mouse.up();
        switch (loss) {
          case 'focus':
            FocusManager.instance.primaryFocus?.unfocus();
          case 'lifecycle':
            tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
          case 'native-reset':
            fixture.media.events.add({'type': 'input.reset'});
          case 'menu':
            final context = tester.element(find.byType(RemoteView));
            unawaited(showDialog<void>(context: context, builder: (_) => const AlertDialog(title: Text('Local menu'))));
        }
        await tester.pump();
        pending.complete(true);
        await tester.pump();
        await tester.pump();
        expect(fixture.inputs.where((event) => event['event'] == 'button'), isEmpty);
        if (loss == 'lifecycle') tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      } finally {
        if (!pending.isCompleted) pending.complete(false);
        fixture.media.repeatedClaim = null;
        await fixture.close(tester);
      }
    });
  }

  testWidgets('A focused captured picture sends a quick Down and Up without awaiting another native claim', (tester) async {
    final fixture = _PointerFixture();
    await fixture.mount(tester);
    final pending = Completer<bool>();
    fixture.media.repeatedClaim = pending;
    try {
      final calls = fixture.media.captures.length;
      final listener = tester.widgetList<Listener>(
        find.descendant(of: find.byType(RemoteView), matching: find.byType(Listener)),
      ).singleWhere((value) => value.onPointerDown != null &&
          value.onPointerUp != null && value.onPointerMove != null &&
          value.onPointerHover != null && value.onPointerSignal != null);
      final at = fixture.point(tester, .4, .6);
      // One platform packet can contain both transitions. No pump/microtask
      // between these production Listener callbacks may lose the Down.
      listener.onPointerDown!(PointerDownEvent(
        position: at,
        kind: PointerDeviceKind.mouse,
        buttons: kPrimaryMouseButton,
      ));
      listener.onPointerUp!(PointerUpEvent(position: at, kind: PointerDeviceKind.mouse));
      expect(
        fixture.inputs.where((event) => event['event'] == 'button').map((event) => event['down']),
        [true, false],
        reason: 'Existing capture must preserve the click before a repeated native RPC can complete.',
      );
      expect(fixture.media.captures.length, calls, reason: 'Already-owned input needs no new native claim.');
      expect(pending.isCompleted, isFalse);
      expect(fixture.session.captured, isTrue);
    } finally {
      if (!pending.isCompleted) pending.complete(true);
      fixture.media.repeatedClaim = null;
      await fixture.close(tester);
    }
  });

  tearDown(() {
    final binding = TestWidgetsFlutterBinding.instance;
    final messenger = binding.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window'),
      null,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window/events'),
      null,
    );
  });

  testWidgets(
    'Hover and drag deliver their final position after motion stops',
    (tester) async {
      final fixture = _PointerFixture();
      await fixture.mount(tester);
      try {
        await fixture.queue(tester);
        expect(fixture.inputs, isEmpty);
        await fixture.advance(tester, 16);
        expect(fixture.inputs.single['x'], closeTo(.7, 1e-9));
        await fixture.mouse.down(fixture.point(tester, .7, .6));
        await tester.pump();
        await fixture.mouse.moveTo(fixture.point(tester, .8, .6));
        await fixture.mouse.moveTo(fixture.point(tester, .9, .6));
        await fixture.advance(tester, 16);
        expect(fixture.inputs.last['x'], closeTo(.9, 1e-9));
        expect(fixture.inputs.last['event'], 'move');
        await fixture.mouse.up();
        await tester.pump();
        expect(
          fixture.inputs
              .where((e) => e['event'] == 'button')
              .map((e) => e['down']),
          [true, false],
        );
        expect(fixture.inputs.last['event'], 'button');
        expect(fixture.inputs.last['x'], closeTo(.9, 1e-9));
      } finally {
        await fixture.close(tester);
      }
    },
  );

  group('A drag across the session toolbar (2026-10-26)', () {
    const handle = Key('expand-session-toolbar');
    const bar = Key('session-command-bar');
    IgnorePointer gate(WidgetTester tester) => tester.widget<IgnorePointer>(
      find.byKey(const Key('session-toolbar-pointer')),
    );
    List<Map<String, dynamic>> buttons(_PointerFixture fixture) =>
        fixture.inputs.where((e) => e['event'] == 'button').toList();
    double y(WidgetTester tester, _PointerFixture fixture, Offset at) {
      final rect = fixture.bounds(tester);
      return ((at.dy - rect.top) / rect.height).clamp(0.0, 1.0);
    }

    testWidgets(
      'keeps moving the remote over the handle and releases there once',
      (tester) async {
        final fixture = _PointerFixture();
        await fixture.mount(tester);
        try {
          final over = tester.getCenter(find.byKey(handle));
          await fixture.mouse.down(fixture.point(tester, .5, .5));
          await tester.pump();
          await tester.pump();
          expect(gate(tester).ignoring, isTrue);
          for (final target in [
            Offset(over.dx, 300),
            over,
            over + const Offset(18, 0),
          ]) {
            await fixture.mouse.moveTo(target);
            await fixture.advance(tester, 16);
          }
          final moves = fixture.inputs.where((e) => e['event'] == 'move');
          expect(moves.length, greaterThanOrEqualTo(3));
          expect(moves.last['y'], closeTo(y(tester, fixture, over), 1e-6));
          await fixture.mouse.up();
          await tester.pump();
          await tester.pump();
          expect(buttons(fixture).map((e) => e['down']), [true, false]);
          expect(
            buttons(fixture).last['y'],
            closeTo(y(tester, fixture, over), 1e-6),
          );
          // Nothing released the remote: the drag was never cut.
          expect(
            fixture.socket.sent.where((e) => e['type'] == 'input.reset'),
            isEmpty,
          );
          expect(fixture.session.captured, isTrue);
          // The handle did not react and takes the pointer again.
          expect(find.byKey(bar), findsNothing);
          expect(gate(tester).ignoring, isFalse);
        } finally {
          await fixture.close(tester);
        }
      },
    );

    testWidgets(
      'releasing over a command of a pinned bar never runs it and sends one up first',
      (tester) async {
        final fixture = _PointerFixture();
        await fixture.mount(tester);
        try {
          await tester.tap(find.byKey(handle));
          await tester.pump(Duration.zero);
          // 바 열어 두기 is a switch of the bar's 설정 panel (2026-11-10): it
          // applies in place and the panel stays, so it is closed here. No
          // time passes, so the panel goes at once. Its row hands its key to
          // its InkWell too: the first match is the row.
          await tester.tap(find.byKey(const Key('remote-settings')));
          await tester.pump(Duration.zero);
          await tester.pump(Duration.zero);
          await tester.tap(find.byKey(const Key('pin-session-toolbar')).first);
          await tester.pump(Duration.zero);
          Navigator.of(
            tester.element(find.byKey(const Key('session-settings-panel'))),
          ).pop();
          await tester.pump(Duration.zero);
          await tester.pump(Duration.zero);
          expect(find.byKey(const Key('session-settings-panel')), findsNothing);
          // A click into the picture: pinned, the bar stays.
          await fixture.mouse.moveTo(fixture.point(tester, .5, .6));
          await fixture.mouse.down(fixture.point(tester, .5, .6));
          await tester.pump(Duration.zero);
          await tester.pump(Duration.zero);
          expect(find.byKey(bar), findsOneWidget);
          expect(fixture.session.captured, isTrue);
          final target = tester.getCenter(
            find.byKey(const Key('stream-menu-화면')),
          );
          final mark = fixture.socket.sent.length;
          await fixture.mouse.moveTo(target);
          await fixture.advance(tester, 16);
          await fixture.mouse.up();
          await tester.pump();
          await tester.pump();
          final after = fixture.socket.sent.skip(mark).toList();
          final up = after.indexWhere(
            (e) => e['event'] == 'button' && e['down'] == false,
          );
          final reset = after.indexWhere((e) => e['type'] == 'input.reset');
          expect(up, greaterThanOrEqualTo(0));
          expect(
            after.where((e) => e['event'] == 'button'),
            hasLength(1),
            reason: 'One release, no extra press from the toolbar.',
          );
          // Once the button is up, resting on the bar may hand input back,
          // but only after the remote got its release.
          expect(reset < 0 || reset > up, isTrue);
          expect(find.byKey(bar), findsOneWidget);
          // The release over the 화면 button did not open its panel.
          await tester.pumpAndSettle();
          expect(find.byKey(const Key('session-screen-panel')), findsNothing);
          expect(find.byKey(const Key('stream-windowed')), findsNothing);
        } finally {
          await fixture.close(tester);
        }
      },
    );

    testWidgets('a release outside the window still reaches the remote', (
      tester,
    ) async {
      final fixture = _PointerFixture();
      await fixture.mount(tester);
      try {
        await fixture.mouse.down(fixture.point(tester, .5, .5));
        await tester.pump();
        await tester.pump();
        await fixture.mouse.moveTo(const Offset(640, -40));
        await fixture.advance(tester, 16);
        await fixture.mouse.up();
        await tester.pump();
        expect(buttons(fixture).map((e) => e['down']), [true, false]);
        expect(buttons(fixture).last['y'], 0);
        expect(gate(tester).ignoring, isFalse);
      } finally {
        await fixture.close(tester);
      }
    });

    testWidgets('a cancelled press resets remote input and frees the toolbar', (
      tester,
    ) async {
      final fixture = _PointerFixture();
      await fixture.mount(tester);
      try {
        await fixture.mouse.down(fixture.point(tester, .5, .5));
        await tester.pump();
        await tester.pump();
        expect(gate(tester).ignoring, isTrue);
        await fixture.mouse.cancel();
        await tester.pump();
        expect(
          fixture.socket.sent.where((e) => e['type'] == 'input.reset'),
          isNotEmpty,
          reason: 'Nothing may stay pressed on the remote.',
        );
        expect(gate(tester).ignoring, isFalse);
      } finally {
        await fixture.close(tester);
      }
    });
  });

  testWidgets(
    'Pending movement precedes each native key without merging keys',
    (tester) async {
      final fixture = _PointerFixture();
      await fixture.mount(tester);
      try {
        await fixture.queue(tester);
        fixture.media.events.add({
          'type': 'input',
          'event': 'key',
          'vk': 65,
          'scan': 30,
          'down': true,
        });
        fixture.media.events.add({
          'type': 'input',
          'event': 'key',
          'vk': 65,
          'scan': 30,
          'down': false,
        });
        await tester.pump();
        expect(fixture.inputs.map((e) => e['event']), ['move', 'key', 'key']);
        expect(fixture.inputs.first['x'], closeTo(.7, 1e-9));
        expect(fixture.inputs.skip(1).map((e) => e['down']), [true, false]);
        await fixture.advance(tester, 32);
        expect(fixture.inputs, hasLength(3));
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'Button and wheel boundaries replace pending movement before dispatch',
    (tester) async {
      final fixture = _PointerFixture();
      await fixture.mount(tester);
      try {
        await fixture.queue(tester);
        await fixture.mouse.down(fixture.point(tester, .8, .6));
        await tester.pump();
        expect(fixture.inputs.map((e) => e['event']), ['move', 'button']);
        expect(fixture.inputs.first['x'], closeTo(.8, 1e-9));
        await fixture.mouse.up();
        await tester.pump();
        fixture.socket.sent.clear();
        await fixture.queue(tester);
        await tester.sendEventToBinding(
          PointerScrollEvent(
            position: fixture.point(tester, .9, .6),
            scrollDelta: const Offset(0, 20),
            kind: PointerDeviceKind.mouse,
          ),
        );
        expect(fixture.inputs.map((e) => e['event']), ['move', 'wheel']);
        expect(fixture.inputs.first['x'], closeTo(.9, 1e-9));
        await fixture.advance(tester, 32);
        expect(fixture.inputs, hasLength(2));
      } finally {
        await fixture.close(tester);
      }
    },
  );

  for (final loss in ['focus', 'native-reset', 'capture', 'lifecycle']) {
    testWidgets('$loss cancels queued movement before capture is reacquired', (
      tester,
    ) async {
      final fixture = _PointerFixture();
      await fixture.mount(tester);
      try {
        await fixture.queue(tester);
        switch (loss) {
          case 'focus':
            FocusManager.instance.primaryFocus?.unfocus();
          case 'native-reset':
            fixture.media.events.add({'type': 'input.reset'});
          case 'capture':
            fixture.media.events.add({'type': 'capture.released'});
          case 'lifecycle':
            tester.binding.handleAppLifecycleStateChanged(
              AppLifecycleState.inactive,
            );
        }
        await tester.pump(Duration.zero);
        expect(fixture.inputs, isEmpty);
        if (loss == 'lifecycle') {
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
        }
        await fixture.mouse.down(fixture.point(tester, .4, .6));
        await tester.pump(Duration.zero);
        await tester.pump(Duration.zero);
        await fixture.mouse.up();
        await tester.pump(Duration.zero);
        fixture.socket.sent.clear();
        await fixture.advance(tester, 32);
        expect(fixture.inputs, isEmpty);
      } finally {
        await fixture.close(tester);
      }
    });
  }

  testWidgets(
    'Epoch replacement discards moves even when texture ID is reused',
    (tester) async {
      final fixture = _PointerFixture();
      await fixture.mount(tester);
      try {
        await fixture.queue(tester);
        final change = fixture.session.resizeViewport(1300, 720);
        await tester.pump();
        final request = fixture.socket.sent.singleWhere(
          (e) => e['type'] == 'stream.resize',
        );
        final ack = {
          ...request,
          'type': 'stream.resizing',
          'resizeId': 'pointer-epoch',
          'epoch': 1,
          'timeoutMs': 90000,
        };
        fixture.socket.incoming.add(jsonEncode(ack));
        await tester.pump();
        await tester.pump();
        fixture.socket.incoming.add(
          jsonEncode({
            ...startInfo,
            ...ack,
            'type': 'stream.info',
            'codec': 'h264',
            'sessionId': fixture.session.connection.sessionId,
            'sourceWidth': 1300,
            'sourceHeight': 720,
            'fpsTarget': 60,
          }),
        );
        await tester.pump();
        await tester.pump();
        expect(fixture.session.resizing, isTrue);
        expect(fixture.session.textureId, 42);
        final frame = packet()..[2] = 1;
        fixture.socket.incoming.add(frame);
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 60));
        expect(await change, isTrue);
        await fixture.session.capture(true);
        await tester.pump();
        fixture.socket.sent.clear();
        await fixture.advance(tester, 32);
        expect(fixture.inputs, isEmpty);
      } finally {
        await fixture.close(tester);
      }
    },
  );

  testWidgets(
    'Disposal and a new session cannot receive the old pending timer',
    (tester) async {
      final old = _PointerFixture();
      await old.mount(tester);
      await old.queue(tester);
      await old.mouse.removePointer();
      final fresh = _PointerFixture();
      await fresh.mount(tester, key: 'replacement-view');
      try {
        old.socket.sent.clear();
        fresh.socket.sent.clear();
        await fresh.advance(tester, 32);
        expect(old.inputs, isEmpty);
        expect(fresh.inputs, isEmpty);
      } finally {
        await fresh.close(tester);
        unawaited(old.media.events.close());
      }
    },
  );

  testWidgets('An old ordering revoker cannot unbind the current view', (
    tester,
  ) async {
    final fixture = _PointerFixture();
    await fixture.mount(tester);
    try {
      var oldFlushes = 0, oldDiscards = 0, newFlushes = 0, newDiscards = 0;
      final old = fixture.session.bindPointerOrdering(
        beforeKey: () => oldFlushes++,
        discard: () => oldDiscards++,
      );
      final current = fixture.session.bindPointerOrdering(
        beforeKey: () => newFlushes++,
        discard: () => newDiscards++,
      );
      expect(oldDiscards, 1);
      old();
      fixture.media.events.add({'type': 'input', 'event': 'key', 'down': true});
      await tester.pump();
      expect(oldFlushes, 0);
      expect(newFlushes, 1);
      fixture.media.events.add({'type': 'input.reset'});
      await tester.pump();
      expect(newDiscards, 1);
      current();
      fixture.media.events.add({
        'type': 'input',
        'event': 'key',
        'down': false,
      });
      await tester.pump();
      expect(newFlushes, 1);
      expect(newDiscards, 2);
    } finally {
      await fixture.close(tester);
    }
  });
}
