// The session bar, its menus and panels, notices and the pointer after
// session events (a bar or menu left open resets when the resolution
// changes; it does not stay open). Media, socket, monitors and the native
// window are fixtures; nothing here is real-session acceptance.
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/data/track_stream.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_status_panel.dart';
import 'package:multitap_client/ui/session_toolbar.dart';

import 'display_remote_test.dart' show machine;
import 'remote_session_test.dart' show SessionSocket, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'track_stream_test.dart' as tracks show TrackMedia, packet, trackStats;
import 'viewport_session_test.dart' show ResizeMedia, startInfo;

const _window = MethodChannel('multitap/window');
const _events = MethodChannel('multitap/window/events');
const _bar = Key('session-command-bar');
const _handle = Key('expand-session-toolbar');
const _quality = Key('session-quality');

/// The line under the latency, in the quality chip's 연결 panel (RemoteView)
/// and in the bar's own quality menu (the bar on its own).
const _qualityRow = '왕복 지연 · Host 응답';

/// The bar's 설정 button and its panel: 화질·소리 and 바 열어 두기 are
/// rows of it.
const _settings = Key('remote-settings');
const _settingsPanel = Key('session-settings-panel');

Future<void> _openBar(WidgetTester tester) async {
  if (find.byKey(_bar).evaluate().isEmpty) {
    await tester.tap(find.byKey(_handle));
    await tester.pump();
  }
  expect(find.byKey(_bar), findsOneWidget);
}

/// A popup route opened or closed: its short transition, without waiting
/// for the rest of the view to come to rest (a transfer card or a resize
/// veil may be animating).
Future<void> _popup(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 300));
}

/// The tap target of a panel's keyed row. A SessionPanelRow hands its own
/// key on to its InkWell, so the key alone finds both widgets; this is the
/// one InkWell.
Finder _row(String key) {
  final keyed = find.byKey(Key(key));
  final inside = find.descendant(of: keyed, matching: find.byType(InkWell));
  return inside.evaluate().isNotEmpty ? inside : keyed;
}

/// The 설정 panel under its button on the open bar.
Future<void> _openSettings(WidgetTester tester) async {
  await tester.tap(find.byKey(_settings));
  await _popup(tester);
  expect(find.byKey(_settingsPanel), findsOneWidget);
}

/// Escape closes the panel that is up; the bar stays.
Future<void> _closePanel(WidgetTester tester) async {
  await tester.sendKeyEvent(LogicalKeyboardKey.escape);
  await _popup(tester);
}

/// 바 열어 두기 on or off: the switch row of 설정 applies in place and the
/// panel stays, so it is closed again here.
Future<void> _togglePin(WidgetTester tester, {required bool pinned}) async {
  await _openSettings(tester);
  await tester.tap(_row('pin-session-toolbar'));
  await tester.pump();
  await _closePanel(tester);
  expect(find.byKey(_settingsPanel), findsNothing);
  expect(find.byKey(_bar), findsOneWidget);
  expect(
    tester.state<SessionToolbarState>(find.byType(SessionToolbar)).pinned,
    pinned,
  );
}

/// 화질: the row of 설정 that opens the streaming choices in its place.
Future<void> _openStreamSettings(WidgetTester tester) async {
  await _openSettings(tester);
  await tester.tap(_row('remote-stream-settings'));
  await _popup(tester);
  expect(find.byKey(const Key('stream-settings-panel')), findsOneWidget);
}

/// One streaming connection: viewport sessions resize their remote output.
class _Stream {
  _Stream({this.viewport = true, this.id = 'chrome-qa'});
  final bool viewport;
  final String id;
  final socket = SessionSocket();
  final media = ResizeMedia();
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: id,
      webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/$id'),
      epochMedia: viewport,
      viewportProvisioning: viewport,
    ),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
  );
  int epoch = 0;

  List<Map<String, dynamic>> get requests =>
      socket.sent.where((event) => event['type'] == 'stream.resize').toList();

  Future<void> firstFrame(WidgetTester tester) async {
    socket.incoming.add(jsonEncode(startInfo));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue, reason: session.error);
  }

  Future<void> waitFor(WidgetTester tester, int count) async {
    for (var n = 0; n < 25 && requests.length < count; n++) {
      await tester.pump(const Duration(milliseconds: 100));
      await tester.pump();
    }
    expect(requests, hasLength(count));
  }

  Future<Map<String, dynamic>> acknowledge(
    WidgetTester tester,
    Map<String, dynamic> request,
  ) async {
    final ack = <String, dynamic>{
      ...request,
      'type': 'stream.resizing',
      'resizeId': 'chrome-epoch-${++epoch}',
      'epoch': epoch,
      'timeoutMs': 90000,
    };
    socket.incoming.add(jsonEncode(ack));
    await tester.pump();
    await tester.pump();
    expect(session.resizing, isTrue);
    return ack;
  }

  Future<void> apply(WidgetTester tester, Map<String, dynamic> ack) async {
    socket.incoming.add(
      jsonEncode({
        ...startInfo,
        'sessionId': session.connection.sessionId,
        'requestId': ack['requestId'],
        'resizeId': ack['resizeId'],
        'epoch': ack['epoch'],
        'width': ack['width'],
        'height': ack['height'],
        'sourceWidth': ack['width'],
        'sourceHeight': ack['height'],
      }),
    );
    await tester.pump();
    await tester.pump();
    final frame = packet();
    frame[2] = epoch & 255;
    frame[3] = epoch >> 8;
    socket.incoming.add(frame);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 60));
    expect(session.resizing, isFalse);
  }

  Future<void> dispose(WidgetTester tester) async {
    unawaited(session.close());
    await tester.pump(const Duration(seconds: 3));
    await tester.pump();
    unawaited(media.events.close());
  }
}

Future<void> _mount(
  WidgetTester tester,
  _Stream stream, {
  Size size = const Size(800, 600),
  SessionDisplayMode mode = SessionDisplayMode.fullscreen,
  Future<Object?> Function(MethodCall call)? window,
  Future<RemoteSession> Function(StreamOptions options)? onReconnect,
  SessionWindowActions? windowActions,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = size;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(_window, window ?? (_) async => false);
  messenger.setMockMethodCallHandler(_events, (_) async => null);
  addTearDown(() {
    messenger.setMockMethodCallHandler(_window, null);
    messenger.setMockMethodCallHandler(_events, null);
  });
  await tester.pumpWidget(
    MaterialApp(
      home: RemoteView(
        machine: machine(1),
        initialDisplayMode: mode,
        load: () async => stream.session,
        onReconnect: onReconnect,
        windowActions: windowActions,
      ),
    ),
  );
  await tester.pump();
  await tester.pump();
}

Future<void> _unmount(WidgetTester tester, List<_Stream> streams) async {
  await tester.pumpWidget(const SizedBox.shrink());
  for (final stream in streams) {
    await stream.dispose(tester);
  }
}

/// An event the native window sends (presentation, layout, maximize).
Future<void> _native(WidgetTester tester, Object event) async {
  await tester.binding.defaultBinaryMessenger.handlePlatformMessage(
    _events.name,
    const StandardMethodCodec().encodeSuccessEnvelope(event),
    (_) {},
  );
  await tester.pump();
}

class _BusyClipboard extends ClipboardSession {
  _BusyClipboard()
    : super(
        url: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/chrome-qa/clipboard',
        ),
        token: 'fixture-only',
        sessionId: 'chrome-qa',
      );

  /// A long transfer that is still running.
  @override
  bool get busy => true;
  @override
  bool get userWaiting => true;
}

/// A file transfer the test starts and ends by hand.
class _FileClipboard extends ClipboardSession {
  _FileClipboard()
    : super(
        url: Uri.parse(
          'wss://fixture.invalid/api/v1/sessions/chrome-qa/clipboard',
        ),
        token: 'fixture-only',
        sessionId: 'chrome-qa',
      );
  bool running = false;
  @override
  bool get busy => running;
  @override
  bool get userWaiting => true;
  @override
  ClipboardProgress? get progress => running
      ? const ClipboardProgress(
          sending: true,
          format: 'files',
          fileCount: 2,
          fileIndex: 1,
          transferred: 1048576,
          total: 4194304,
        )
      : null;
}

class _PreparingClipboard extends _FileClipboard {
  final completion = Completer<bool>();
  bool waiting = false;
  int cancels = 0;
  @override
  bool get automatic => true;
  @override
  bool get canSend => true;
  @override
  bool get userWaiting => waiting;
  @override
  Future<bool> preparePaste() async {
    waiting = true;
    notifyListeners();
    try {
      return await completion.future;
    } finally {
      waiting = false;
      notifyListeners();
    }
  }

  void finish([bool accepted = true]) {
    running = false;
    notifyListeners();
    if (!completion.isCompleted) completion.complete(accepted);
  }

  @override
  Future<void> cancelTransfer() async {
    cancels++;
    finish(false);
  }
}

void main() {
  for (final resetKind in ['input.reset', 'capture.released', 'escape']) {
    testWidgets('a real $resetKind revokes the paste preserved by the modal', (
      tester,
    ) async {
      final stream = _Stream(viewport: false);
      await _mount(tester, stream, mode: SessionDisplayMode.windowed);
      try {
        await stream.firstFrame(tester);
        final clipboard = _PreparingClipboard()..running = true;
        stream.session.clipboard = clipboard;
        stream.session.notifyListeners();
        await tester.pump();
        await tester.tap(find.byKey(const Key('remote-image-bounds')));
        await tester.pump();
        stream.socket.sent.clear();
        stream.media.events.add({
          'type': 'input',
          'event': 'key',
          'vk': 0x11,
          'down': true,
        });
        stream.media.events.add({
          'type': 'input',
          'event': 'key',
          'vk': 0x56,
          'down': true,
        });
        await tester.pump();
        expect(stream.session.preparingClipboardPaste, isTrue);
        expect(stream.session.clipboardTransferBlocked, isTrue);
        stream.media.events.add(
          resetKind == 'escape'
              ? {'type': 'input', 'event': 'key', 'vk': 0x1b, 'down': true}
              : {'type': resetKind},
        );
        await tester.pump();
        clipboard.finish();
        await tester.pump();
        await tester.pump();
        expect(
          stream.socket.sent.any(
            (event) => event['type'] == 'input' && event['vk'] == 0x56,
          ),
          isFalse,
        );
        expect(stream.session.preparingClipboardPaste, isFalse);
        if (resetKind == 'escape') expect(clipboard.cancels, 1);
      } finally {
        await _unmount(tester, [stream]);
      }
    });
  }

  testWidgets(
    'a copy covers an already open close confirmation and a late answer cannot close its session',
    (tester) async {
      final stream = _Stream(viewport: false);
      var closes = 0;
      final actions = SessionWindowActions(closeWindow: () async => closes++);
      await _mount(
        tester,
        stream,
        mode: SessionDisplayMode.windowed,
        windowActions: actions,
      );
      try {
        await stream.firstFrame(tester);
        unawaited(actions.requestClose());
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 300));
        final confirmation = find.text('연결을 끊을까요?');
        expect(confirmation, findsOneWidget);
        final clipboard = _PreparingClipboard()
          ..running = true
          ..waiting = true;
        stream.session.clipboard = clipboard;
        stream.session.notifyListeners();
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 800));
        await tester.pump();
        expect(
          find.byKey(const Key('clipboard-transfer-barrier')),
          findsOneWidget,
        );
        await tester.tap(find.text('연결 끊기'), warnIfMissed: false);
        await tester.pump();
        expect(closes, 0);
        Navigator.of(tester.element(confirmation)).pop(true);
        await tester.pump();
        await tester.pump();
        expect(
          closes,
          0,
          reason:
              'a completion queued before the copy must recheck the blocker',
        );
        expect(stream.session.closed, isFalse);
        await tester.tap(find.byKey(const Key('clipboard-cancel')));
        await tester.pump();
        expect(clipboard.cancels, 1);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'the centered modal preserves its pending Ctrl+V while blocking extra native input',
    (tester) async {
      final stream = _Stream(viewport: false);
      await _mount(tester, stream, mode: SessionDisplayMode.windowed);
      try {
        await stream.firstFrame(tester);
        final clipboard = _PreparingClipboard()..running = true;
        stream.session.clipboard = clipboard;
        stream.session.notifyListeners();
        await tester.pump();
        await tester.tap(find.byKey(const Key('remote-image-bounds')));
        await tester.pump();
        expect(stream.session.captured, isTrue);
        stream.socket.sent.clear();
        stream.media.events.add({
          'type': 'input',
          'event': 'key',
          'vk': 0x11,
          'down': true,
        });
        stream.media.events.add({
          'type': 'input',
          'event': 'key',
          'vk': 0x56,
          'down': true,
        });
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 800));
        await tester.pump();
        expect(
          find.byKey(const Key('clipboard-transfer-barrier')),
          findsOneWidget,
        );
        expect(stream.session.preparingClipboardPaste, isTrue);
        expect(
          stream.session.captured,
          isTrue,
          reason:
              'releasing native ownership now would revoke the waiting paste',
        );
        stream.media.events.add({
          'type': 'input',
          'event': 'key',
          'vk': 0x41,
          'down': true,
        });
        await tester.pump();
        clipboard.finish();
        await tester.pump();
        await tester.pump();
        final keys = stream.socket.sent
            .where((e) => e['type'] == 'input' && e['event'] == 'key')
            .toList();
        expect(
          keys.where((e) => e['vk'] == 0x56 && e['down'] == true),
          hasLength(1),
        );
        expect(keys.any((e) => e['vk'] == 0x41), isFalse);
        expect(stream.session.preparingClipboardPaste, isFalse);
        expect(stream.session.captured, isFalse);
        expect(
          find.byKey(const Key('clipboard-transfer-barrier')),
          findsNothing,
        );
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'A window resize closes an open menu, and the remote resolution change closes it again; a pinned bar stays, an unpinned one folds',
    (tester) async {
      final stream = _Stream();
      await _mount(tester, stream);
      try {
        await stream.firstFrame(tester);
        await stream.waitFor(tester, 1);
        await stream.apply(
          tester,
          await stream.acknowledge(tester, stream.requests.last),
        );
        // The reported case: a pinned bar with the quality chip's panel open
        // (the tile shows the resolution without spaces).
        await _openBar(tester);
        await _togglePin(tester, pinned: true);
        await tester.tap(find.byKey(_quality));
        await tester.pumpAndSettle();
        expect(
          find.byKey(const Key('session-connection-panel')),
          findsOneWidget,
        );
        expect(find.text(_qualityRow), findsOneWidget);
        expect(find.text('800×600'), findsOneWidget);
        tester.view.physicalSize = const Size(1000, 700);
        await tester.pump();
        await tester.pump();
        expect(
          find.text(_qualityRow),
          findsNothing,
          reason: 'Its anchor and its values belong to the old window size.',
        );
        expect(find.byKey(_bar), findsOneWidget, reason: 'Pinned stays open.');
        // Opened again before the remote follows: still the old resolution,
        // so the remote change closes it as well. (Within the 250 ms settle
        // of RemoteView.viewportSettle, so no settling pump here.)
        await tester.tap(find.byKey(_quality));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 50));
        expect(find.text('800×600'), findsOneWidget);
        await stream.waitFor(tester, 2);
        final ack = await stream.acknowledge(tester, stream.requests.last);
        expect(find.text(_qualityRow), findsNothing);
        await stream.apply(tester, ack);
        expect(find.byKey(_bar), findsOneWidget);
        await tester.tap(find.byKey(_quality));
        await tester.pumpAndSettle();
        expect(find.text('1000×700'), findsOneWidget);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        // Unpinned, a resize folds it back to its handle.
        await _togglePin(tester, pinned: false);
        expect(find.byKey(_bar), findsOneWidget);
        tester.view.physicalSize = const Size(900, 640);
        await tester.pump();
        await tester.pump();
        expect(find.byKey(_bar), findsNothing);
        expect(find.byKey(_handle), findsOneWidget);
        expect(tester.takeException(), isNull);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'A screen-mode change from outside the bar (maximize, Win+Up) closes its open menu and folds it',
    (tester) async {
      final stream = _Stream(viewport: false);
      await _mount(
        tester,
        stream,
        mode: SessionDisplayMode.windowed,
        window: (call) async =>
            call.method == 'fullscreen' ? call.arguments : null,
      );
      try {
        await stream.firstFrame(tester);
        await _openBar(tester);
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
        await _native(tester, {'type': 'maximizeRequested'});
        await tester.pump();
        await tester.pump();
        expect(
          find.byKey(const Key('stream-windowed')),
          findsNothing,
          reason: 'Its check mark still named the old mode.',
        );
        expect(find.byKey(_bar), findsNothing);
        await _openBar(tester);
        expect(find.byTooltip('화면 모드 · 전체 화면'), findsOneWidget);
        expect(tester.takeException(), isNull);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'A local monitor change closes an open menu (its 모든 모니터 row may no longer apply)',
    (tester) async {
      final stream = _Stream(viewport: false);
      await _mount(tester, stream);
      try {
        await stream.firstFrame(tester);
        await _openBar(tester);
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-windowed')), findsOneWidget);
        await _native(tester, {'type': 'displayLayoutChanged'});
        await tester.pump();
        expect(find.byKey(const Key('stream-windowed')), findsNothing);
        expect(find.byKey(_bar), findsNothing);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'Losing focus or minimizing closes open menus and folds an unpinned bar; a pinned bar stays',
    (tester) async {
      final stream = _Stream(viewport: false);
      await _mount(tester, stream);
      try {
        await stream.firstFrame(tester);
        for (final states in [
          [AppLifecycleState.inactive],
          [AppLifecycleState.inactive, AppLifecycleState.hidden],
        ]) {
          await _openBar(tester);
          await tester.tap(find.byKey(_quality));
          await tester.pumpAndSettle();
          expect(find.text(_qualityRow), findsOneWidget);
          for (final state in states) {
            tester.binding.handleAppLifecycleStateChanged(state);
          }
          // A hidden (minimized) window draws nothing until restored.
          await tester.pump();
          for (final state in states.reversed.skip(1)) {
            tester.binding.handleAppLifecycleStateChanged(state);
          }
          tester.binding.handleAppLifecycleStateChanged(
            AppLifecycleState.resumed,
          );
          // Back in front, the very first frame shows them closed.
          await tester.pump();
          expect(find.text(_qualityRow), findsNothing, reason: '$states');
          expect(find.byKey(_bar), findsNothing, reason: '$states');
        }
        await _openBar(tester);
        await _togglePin(tester, pinned: true);
        await tester.tap(find.byKey(_quality));
        await tester.pumpAndSettle();
        expect(find.text(_qualityRow), findsOneWidget);
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.inactive,
        );
        await tester.pump();
        await tester.pump();
        expect(find.text(_qualityRow), findsNothing);
        expect(find.byKey(_bar), findsOneWidget, reason: 'Pinned stays open.');
        tester.binding.handleAppLifecycleStateChanged(
          AppLifecycleState.resumed,
        );
        await tester.pump();
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'The end of the connection closes its menus and folds the bar over the status card',
    (tester) async {
      final stream = _Stream(viewport: false);
      await _mount(tester, stream);
      try {
        await stream.firstFrame(tester);
        await _openBar(tester);
        await tester.tap(find.byKey(_quality));
        await tester.pumpAndSettle();
        expect(find.text(_qualityRow), findsOneWidget);
        await stream.socket.incoming.close();
        await tester.pump();
        await tester.pump();
        await tester.pump();
        expect(stream.session.closed, isTrue);
        expect(find.text(_qualityRow), findsNothing);
        expect(find.byKey(_bar), findsNothing);
        expect(find.byType(SessionStatusPanel), findsOneWidget);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'A reconnect drops the clipboard transfer card of the replaced connection',
    (tester) async {
      final first = _Stream(viewport: false);
      final second = _Stream(viewport: false, id: 'chrome-qa-2');
      var reconnects = 0;
      await _mount(
        tester,
        first,
        onReconnect: (_) async {
          reconnects++;
          return second.session;
        },
      );
      try {
        await first.firstFrame(tester);
        first.session.clipboard = _BusyClipboard();
        first.session.notifyListeners();
        await tester.pump(const Duration(milliseconds: 1300));
        await tester.pump(const Duration(milliseconds: 400));
        expect(find.byKey(const Key('clipboard-transfer')), findsOneWidget);
        // New streaming settings (설정 › 화질) replace the connection.
        // The card's indeterminate bar animates for as long as the transfer
        // runs, so never settle. 절약 is a change (sound has no switch).
        await _openBar(tester);
        await _openStreamSettings(tester);
        await tester.pump(const Duration(milliseconds: 300));
        await tester.tap(find.byKey(const Key('stream-preset-saver')));
        await tester.pump();
        await tester.tap(find.byKey(const Key('apply-stream-settings')));
        for (var n = 0; n < 20 && reconnects == 0; n++) {
          await tester.pump(const Duration(milliseconds: 100));
        }
        expect(reconnects, 1);
        await second.firstFrame(tester);
        await tester.pump(const Duration(seconds: 1));
        expect(
          find.byKey(const Key('clipboard-transfer')),
          findsNothing,
          reason: 'Its 취소 belongs to a transfer of the old connection.',
        );
      } finally {
        await _unmount(tester, [first, second]);
      }
    },
  );

  for (final failed in [false, true]) {
    testWidgets(
      failed
          ? 'A failed file transfer keeps its card with the reason until 닫기'
          : 'A finished file transfer ends with a brief transfer-ended notice, then goes',
      (tester) async {
        final stream = _Stream(viewport: false);
        await _mount(tester, stream);
        try {
          await stream.firstFrame(tester);
          final clipboard = _FileClipboard();
          stream.session.clipboard = clipboard;
          clipboard.running = true;
          stream.session.notifyListeners();
          clipboard.notifyListeners();
          // Not at 600 ms: a copy this short never gets a card.
          await tester.pump(const Duration(milliseconds: 600));
          expect(find.byKey(const Key('clipboard-transfer')), findsNothing);
          await tester.pump(const Duration(milliseconds: 200));
          await tester.pump(const Duration(milliseconds: 400));
          expect(find.byKey(const Key('clipboard-transfer')), findsOneWidget);
          expect(find.text('PC → QA VM'), findsOneWidget);
          expect(find.text('2개 중 1개'), findsOneWidget);
          expect(find.text('1.0 MB / 4.0 MB'), findsOneWidget);
          expect(find.text('25%'), findsOneWidget);
          expect(find.text('취소'), findsOneWidget);
          clipboard.running = false;
          if (failed) clipboard.error = 'clipboard_write_failed';
          clipboard.notifyListeners();
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 100));
          if (failed) {
            expect(find.text('옮기지 못했어요'), findsOneWidget);
            await tester.pump(const Duration(seconds: 10));
            expect(find.byKey(const Key('clipboard-transfer')), findsOneWidget);
            await tester.tap(find.byKey(const Key('clipboard-close')));
            await tester.pump();
            expect(find.byKey(const Key('clipboard-transfer')), findsNothing);
          } else {
            expect(find.text('옮겼어요'), findsNothing);
            expect(find.byKey(const Key('clipboard-transfer')), findsNothing);
            await tester.pump(const Duration(milliseconds: 3800));
            await tester.pump(const Duration(milliseconds: 500));
            expect(find.byKey(const Key('clipboard-transfer')), findsNothing);
          }
        } finally {
          await _unmount(tester, [stream]);
        }
      },
    );
  }
  testWidgets(
    'A finished screen-mode change enables 창 모드로 and 화질 on a pinned bar without another event',
    (tester) async {
      final stream = _Stream(viewport: false);
      await _mount(
        tester,
        stream,
        mode: SessionDisplayMode.windowed,
        window: (call) async =>
            call.method == 'fullscreen' ? call.arguments : null,
        onReconnect: (_) async => stream.session,
      );
      try {
        await stream.firstFrame(tester);
        await _openBar(tester);
        await _togglePin(tester, pinned: true);
        // A window has its title bar: no window buttons on the bar.
        expect(find.byKey(const Key('session-restore')), findsNothing);
        await tester.tap(find.byKey(const Key('stream-menu-화면')));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 200));
        await tester.tap(find.byKey(const Key('stream-fullscreen')));
        for (var n = 0; n < 6; n++) {
          await tester.pump();
        }
        expect(find.byKey(_bar), findsOneWidget);
        expect(find.byTooltip('화면 모드 · 전체 화면'), findsOneWidget);
        // What waits for the change is back on the bar by itself: 창 모드로
        // is offered only once the change has finished.
        expect(find.byKey(const Key('session-minimize')), findsOneWidget);
        expect(find.byKey(const Key('session-restore')), findsOneWidget);
        // And 화질 (in 설정) can be opened at once.
        await _openSettings(tester);
        expect(
          tester.widget<InkWell>(_row('remote-stream-settings')).onTap,
          isNotNull,
        );
        expect(
          find.byWidgetPredicate(
            (widget) =>
                widget is Tooltip &&
                (widget.message ?? '').contains('화면 전환이 끝나면'),
          ),
          findsNothing,
        );
        await _closePanel(tester);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'While the remote resolution changes the bar still says connected, and settings wait with the right reason',
    (tester) async {
      final stream = _Stream();
      await _mount(tester, stream, onReconnect: (_) async => stream.session);
      try {
        await stream.firstFrame(tester);
        await stream.waitFor(tester, 1);
        final ack = await stream.acknowledge(tester, stream.requests.last);
        expect(stream.session.textureId, isNull);
        expect(
          find.byWidgetPredicate(
            (widget) =>
                widget is Tooltip && (widget.message ?? '').contains('연결 대기'),
          ),
          findsNothing,
        );
        await _openBar(tester);
        expect(
          find.byKey(const Key('session-waiting-indicator')),
          findsNothing,
        );
        // 화질 is a row of 설정 now: greyed, with the reason of a
        // connection that is up but changing (not "연결된 뒤").
        await _openSettings(tester);
        expect(find.byTooltip('화질 · 화면 전환이 끝나면 바꿀 수 있어요'), findsOneWidget);
        expect(
          tester.widget<InkWell>(_row('remote-stream-settings')).onTap,
          isNull,
        );
        expect(find.byTooltip('화질 · 연결된 뒤 바꿀 수 있어요'), findsNothing);
        await _closePanel(tester);
        await stream.apply(tester, ack);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'A resize closes the settings panel without applying it, and the panel opens again',
    (tester) async {
      final stream = _Stream(viewport: false);
      var reconnects = 0;
      await _mount(
        tester,
        stream,
        size: const Size(1280, 800),
        windowActions: SessionWindowActions(closeWindow: () async {}),
        onReconnect: (_) async {
          reconnects++;
          return stream.session;
        },
      );
      try {
        await stream.firstFrame(tester);
        await tester.pump(const Duration(milliseconds: 100));
        await _openBar(tester);
        // 설정 itself: a resize closes it, and it opens again.
        await _openSettings(tester);
        tester.view.physicalSize = const Size(1200, 780);
        await tester.pump();
        await tester.pump();
        expect(find.byKey(_settingsPanel), findsNothing);
        // 화질, opened from 설정; 절약 is a change that is never applied.
        await _openBar(tester);
        await _openStreamSettings(tester);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-settings-panel')), findsOneWidget);
        await tester.tap(find.byKey(const Key('stream-preset-saver')));
        await tester.pump();
        tester.view.physicalSize = const Size(1100, 760);
        await tester.pump();
        await tester.pump();
        expect(find.byKey(const Key('stream-settings-panel')), findsNothing);
        expect(find.byKey(_settingsPanel), findsNothing);
        await tester.pump(const Duration(milliseconds: 100));
        expect(reconnects, 0, reason: 'A closed panel applies nothing.');
        await _openBar(tester);
        await _openStreamSettings(tester);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-settings-panel')), findsOneWidget);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('stream-settings-panel')), findsNothing);
        expect(find.byKey(_settingsPanel), findsNothing);
        expect(tester.takeException(), isNull);
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  testWidgets(
    'A long session notice wraps inside a narrow window instead of running off it',
    (tester) async {
      final stream = _Stream(viewport: false);
      await _mount(tester, stream, size: const Size(640, 480));
      try {
        stream.socket.incoming.add(
          jsonEncode({
            ...startInfo,
            'audio': {'available': false},
          }),
        );
        await tester.pump();
        await tester.pump();
        stream.socket.incoming.add(packet());
        await tester.pump();
        await tester.pump();
        await tester.pump();
        expect(stream.session.frameReady, isTrue);
        final toast = find.byKey(const Key('pointer-confinement-toast'));
        expect(toast, findsOneWidget);
        expect(
          find.descendant(
            of: toast,
            matching: find.text(
              '이 데스크톱에는 소리 장치가 없어요. 관리자가 Proxmox에서 오디오 장치를 추가해야 해요.',
            ),
          ),
          findsOneWidget,
        );
        // Plain: nothing to press in it.
        expect(
          find.descendant(of: toast, matching: find.byType(TextButton)),
          findsNothing,
        );
        final box = tester.getRect(
          find.descendant(of: toast, matching: find.byType(DecoratedBox)).first,
        );
        expect(box.left, greaterThanOrEqualTo(0));
        expect(box.right, lessThanOrEqualTo(640));
        expect(tester.takeException(), isNull);
        await tester.pump(const Duration(seconds: 11));
      } finally {
        await _unmount(tester, [stream]);
      }
    },
  );

  group('The bar on its own', () {
    Widget bar({
      Object? content,
      Object? layout,
      ValueChanged<Rect>? onBounds,
    }) => MaterialApp(
      home: Scaffold(
        body: SessionToolbar(
          key: const ValueKey('bar'),
          title: 'TESTER',
          connected: true,
          latencyMs: 12,
          contentKey: content,
          layoutKey: layout,
          onBounds: onBounds,
          onInteract: () {},
          onDisconnect: () {},
          actions: [
            SessionToolbarMenu(
              label: '화면',
              icon: Icons.fullscreen_rounded,
              onInteract: () {},
              items: [
                SessionMenuItem(
                  '창 모드',
                  key: const Key('mode-window'),
                  onSelected: () {},
                ),
              ],
            ),
          ],
        ),
      ),
    );

    testWidgets(
      'A change of its own area closes its menu and folds it; pinned it stays where it was moved, inside the new area',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = const Size(1200, 800);
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        Rect? bounds;
        await tester.pumpWidget(bar(onBounds: (value) => bounds = value));
        await _openBar(tester);
        await tester.tap(find.byTooltip('화면'));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('mode-window')), findsOneWidget);
        tester.view.physicalSize = const Size(1000, 700);
        await tester.pump();
        await tester.pump();
        expect(find.byKey(const Key('mode-window')), findsNothing);
        expect(find.byKey(_bar), findsNothing);
        // Pinned (바 열어 두기 is the session's switch; the bar has no pin
        // button of its own) and moved to the right edge, then the window
        // narrows.
        await _openBar(tester);
        expect(find.byKey(const Key('pin-session-toolbar')), findsNothing);
        tester
            .state<SessionToolbarState>(find.byType(SessionToolbar))
            .setPinned(true);
        await tester.pump();
        expect(find.byKey(_bar), findsOneWidget);
        await tester.drag(
          find.byKey(const Key('drag-session-toolbar')),
          const Offset(2000, 0),
        );
        await tester.pump();
        expect(bounds!.right, closeTo(1000, 1));
        await tester.tap(find.byKey(_quality));
        await tester.pumpAndSettle();
        expect(find.text(_qualityRow), findsOneWidget);
        tester.view.physicalSize = const Size(800, 600);
        await tester.pump();
        await tester.pump();
        expect(find.text(_qualityRow), findsNothing);
        expect(find.byKey(_bar), findsOneWidget);
        expect(bounds!.right, closeTo(800, 1));
        // Back to the larger window it stays at the edge it was kept to,
        // and a drag moves it at once (no dead travel past the old edge).
        tester.view.physicalSize = const Size(1000, 700);
        await tester.pump();
        await tester.pump();
        final kept = bounds!;
        await tester.drag(
          find.byKey(const Key('drag-session-toolbar')),
          const Offset(-40, 0),
        );
        await tester.pump();
        expect(bounds!.left, lessThan(kept.left));
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );

    testWidgets(
      'A new content closes its menus but keeps the bar; a new layout also folds it',
      (tester) async {
        tester.view.devicePixelRatio = 1;
        tester.view.physicalSize = const Size(1200, 800);
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);
        await tester.pumpWidget(bar(content: 1, layout: 1));
        await _openBar(tester);
        await tester.tap(find.byKey(_quality));
        await tester.pumpAndSettle();
        expect(find.text(_qualityRow), findsOneWidget);
        await tester.pumpWidget(bar(content: 2, layout: 1));
        await tester.pump();
        expect(find.text(_qualityRow), findsNothing);
        expect(find.byKey(_bar), findsOneWidget);
        await tester.tap(find.byTooltip('화면'));
        await tester.pumpAndSettle();
        expect(find.byKey(const Key('mode-window')), findsOneWidget);
        await tester.pumpWidget(bar(content: 2, layout: 2));
        await tester.pump();
        expect(find.byKey(const Key('mode-window')), findsNothing);
        expect(find.byKey(_bar), findsNothing);
        expect(find.byKey(_handle), findsOneWidget);
        // Its menus work as before afterwards.
        await _openBar(tester);
        await tester.tap(find.byTooltip('화면'));
        await tester.pumpAndSettle();
        await tester.tap(find.byKey(const Key('mode-window')));
        await tester.pumpAndSettle();
        expect(find.byKey(_bar), findsNothing, reason: 'Used, it folds.');
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );
  });

  testWidgets('모든 모니터 사용: the clipboard card stays on the primary monitor', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1920, 720);
    tester.view.devicePixelRatio = 1.5;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final socket = SessionSocket(), media = tracks.TrackMedia();
    RemoteSession? session;
    TrackPlan? request;
    messenger.setMockMethodCallHandler(_window, (call) async {
      if (call.method == 'monitorLayout') {
        return {
          'x': 0,
          'y': 0,
          'width': 1920,
          'height': 720,
          'monitors': [
            {
              'id': '0',
              'x': 0,
              'y': 0,
              'width': 640,
              'height': 360,
              'primary': true,
              'dpi': 144,
            },
            {
              'id': '1',
              'x': 640,
              'y': 0,
              'width': 1280,
              'height': 720,
              'primary': false,
              'dpi': 96,
            },
          ],
        };
      }
      return true;
    });
    messenger.setMockMethodCallHandler(_events, (_) async => null);
    addTearDown(() {
      messenger.setMockMethodCallHandler(_window, null);
      messenger.setMockMethodCallHandler(_events, null);
    });
    await tester.pumpWidget(
      MaterialApp(
        home: RemoteView(
          machine: entryMachine(1),
          prepareDisplaysOnEntry: true,
          initialDisplayMode: SessionDisplayMode.allMonitors,
          initialOptions: const StreamOptions(fps: 60),
          windowActions: SessionWindowActions(closeWindow: () async {}),
          load: () async => fail('All monitors never use one composite.'),
          onConfigureDisplays: (count) async => entryMachine(count),
          onConnectTracks: (plan, options) async {
            request = plan;
            return session = RemoteSession(
              connection: BrokerConnection.fromJson(
                {
                  'sessionId': 'chrome-tracks',
                  'webSocketUrl':
                      'wss://fixture.invalid/api/v1/sessions/chrome-tracks',
                  'mode': 'stream',
                  'transport': 'media-tracks-v1',
                  'mediaProtocol': 'tracks-v1',
                  'trackViewportProvisioning': true,
                  'tracks': plan.toJson(),
                  'options': options.toJson(),
                },
                Uri.parse('https://fixture.invalid'),
                requestedOptions: options,
                requestedTracks: plan,
              ),
              token: 'fixture-token',
              media: media,
              dialer: (_, _) async => socket,
            );
          },
        ),
      ),
    );
    try {
      for (var n = 0; n < 4; n++) {
        await tester.pump();
      }
      await tester.pump(const Duration(milliseconds: 100));
      await tester.pump();
      expect(request, isNotNull);
      socket.incoming.add(
        jsonEncode({
          'type': 'stream.tracks.info',
          'sessionId': 'chrome-tracks',
          'codec': 'h264',
          'topologyEpoch': 1,
          'fpsTarget': 60,
          'bitrateTarget': 12000000,
          'tracks': [
            for (final output in request!.outputs)
              {
                'id': output.id,
                'epoch': 1,
                'width': output.width,
                'height': output.height,
                'sourceWidth': output.width,
                'sourceHeight': output.height,
                'left': output.x,
                'top': output.y,
                'primary': output.id == '0',
                'bitrateTarget': 6000000,
              },
          ],
          'audio': {
            'sampleRate': 48000,
            'channels': 2,
            'format': 's16le',
            'available': false,
          },
        }),
      );
      await tester.pump();
      await tester.pump();
      socket.incoming.add(tracks.packet(0));
      socket.incoming.add(tracks.packet(1));
      await tester.pump();
      await tester.pump();
      media.currentStats = tracks.trackStats();
      await tester.pump(const Duration(seconds: 1));
      await tester.pump();
      expect(session!.frameReady, isTrue, reason: session!.error);
      expect(find.byKey(const Key('local-output-1')), findsOneWidget);
      await tester.pump(const Duration(milliseconds: 100));
      await _openBar(tester);
      // A clipboard transfer's card stays inside the primary monitor's part
      // of the spanning window (physical 640x360 at ratio 1.5), like every popup.
      final clipboard = _FileClipboard();
      session!.clipboard = clipboard;
      clipboard.running = true;
      session!.notifyListeners();
      clipboard.notifyListeners();
      await tester.pump(const Duration(milliseconds: 800));
      await tester.pump(const Duration(milliseconds: 400));
      final card = find.byKey(const Key('clipboard-transfer'));
      expect(card, findsOneWidget);
      final rect = tester.getRect(card);
      expect(rect.left, greaterThanOrEqualTo(0));
      expect(rect.right, lessThanOrEqualTo(640 / 1.5 + .5));
      expect(rect.bottom, lessThanOrEqualTo(360 / 1.5 + .5));
      clipboard.running = false;
      clipboard.notifyListeners();
    } finally {
      await tester.pumpWidget(const SizedBox.shrink());
      unawaited(session?.close());
      await tester.pump(const Duration(seconds: 3));
      await tester.pump();
    }
  });
}
