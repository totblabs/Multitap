// Production Windows UI with fake broker/media/window boundaries only.
import 'dart:convert';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/console_preparation.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/session_cleanup.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/console_resume_dialog.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/stream_settings_dialog.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;
import 'helpers/stream_connect_consent.dart';

const _captureRoot = String.fromEnvironment('MULTITAP_WINDOWS_4UX_CAPTURES');
const _boundary = Key('windows-four-ux-render');
const _custom = StreamOptions(fps: 30, bitrate: 12000000, audio: false);

Future<void> _shot(WidgetTester t, String name) async {
  if (_captureRoot.isEmpty) return;
  await t.runAsync(() async {
    final boundary = t.renderObject<RenderRepaintBoundary>(
      find.byKey(_boundary),
    );
    final image = await boundary.toImage(pixelRatio: 1);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(_captureRoot).create(recursive: true);
      await File('$_captureRoot/$name.png')
          .writeAsBytes(bytes!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

Widget _app(Widget home, {Brightness brightness = Brightness.dark}) =>
    RepaintBoundary(
      key: _boundary,
      child: MaterialApp(
        theme: buildAppTheme(brightness: brightness)
            .copyWith(platform: TargetPlatform.windows),
        home: home,
      ),
    );

Future<void> _tap(WidgetTester t, String key) async {
  final target = find.byKey(Key(key));
  await t.ensureVisible(target);
  await t.pump();
  await t.tap(target);
  await t.pumpAndSettle();
}

Future<void> _finish(WidgetTester t) async {
  await t.pumpWidget(const SizedBox.shrink());
  await t.pump(const Duration(seconds: 3));
}

/// [settling]: the previous connection is still being cleaned up, so a press
/// queues the connection instead of opening it.
DesktopMachine _handover({bool settling = false}) => DesktopMachine(
  id: 'handover',
  name: 'Handover fixture',
  description: '',
  kind: MachineKind.vm,
  status: MachineStatus.awaitingHost,
  specification: 'Windows',
  accent: Colors.blue,
  hostOnline: true,
  powerState: 'running',
  powerStale: false,
  sessionCleanup: settling
      ? SessionCleanupState(
          pending: true,
          phase: 'pending',
          checkedAt: DateTime.now(),
        )
      : const SessionCleanupState(),
  consolePreparation: const ConsolePreparationState(
    supported: true,
    available: true,
    reason: 'rdp_session_active',
  ),
);

/// The launcher with a held-by-RDP capable shell: counts what a question
/// would have to precede.
class _Launcher {
  _Launcher(List<DesktopMachine> initial) : machines = ValueNotifier(initial);
  final ValueNotifier<List<DesktopMachine>> machines;
  int prepared = 0, opened = 0;

  // The same callbacks across list updates, as in the app: the shell retires
  // an open question and its queue when a connection callback is replaced.
  Future<DesktopMachine> _prepare(String id) async {
    prepared++;
    return entryMachine(1, id: 'handover');
  }

  Future<SessionWindowOpenResult?> _open(
    DesktopMachine machine,
    SessionDisplayMode mode,
    StreamOptions options,
  ) async {
    opened++;
    return const SessionWindowOpenResult(opened: true);
  }

  Future<void> mount(WidgetTester t) async {
    t.view.physicalSize = const Size(1280, 900);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    addTearDown(machines.dispose);
    await t.pumpWidget(
      _app(
        ValueListenableBuilder<List<DesktopMachine>>(
          valueListenable: machines,
          builder: (_, values, _) => WorkspaceShell(
            displayName: 'fixture',
            guideAccountKey: 'fixture-account',
            serverOrigin: 'https://fixture.invalid',
            machines: values,
            launcherMemory: SessionLauncherMemory(),
            onSignOut: () {},
            onRefresh: () {},
            onPrepareConsole: _prepare,
            onOpenWindow: _open,
          ),
        ),
      ),
    );
    await t.pumpAndSettle();
  }
}

class _RemoteFixture {
  final socket = SessionSocket();
  final media = SessionMedia();
  int loads = 0, takeovers = 0, legacyTakeovers = 0;
  final reconnects = <StreamOptions>[];
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'four-ux-fixture',
      webSocketUrl: Uri.parse('wss://fixture.invalid/api/v1/sessions/four-ux'),
      options: _custom,
    ),
    token: 'fixture-only',
    media: media,
    dialer: (_, _) async => socket,
  );

  Future<void> mount(WidgetTester t, {BrokerException? failure}) async {
    t.view.physicalSize = const Size(1280, 800);
    t.view.devicePixelRatio = 1;
    addTearDown(t.view.reset);
    await t.pumpWidget(
      _app(
        RemoteView(
          machine: entryMachine(1),
          sessionGuideEnabled: false,
          initialOptions: _custom,
          load: () async {
            loads++;
            if (failure != null) throw failure;
            return session;
          },
          onReconnect: (options) async {
            reconnects.add(options);
            throw const BrokerException('session_expired', 401);
          },
          onTakeover: () async {
            legacyTakeovers++;
          },
          onConfirmTakeover: failure?.takeoverOffer == null
              ? null
              : (offer, authorized) async {
                  expect(identical(offer, failure!.takeoverOffer), isTrue);
                  expect(authorized(), isTrue);
                  takeovers++;
                },
        ),
      ),
    );
    await t.pump();
    await t.pump();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  if (_captureRoot.isNotEmpty) {
    setUpAll(() async {
      final font = FontLoader('Pretendard');
      for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
        font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
      }
      await font.load();
      await (FontLoader(
        'MaterialIcons',
      )..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'))).load();
    });
  }
  setUp(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window'),
      (call) async => call.method == 'fullscreen' ? false : null,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window/events'),
      (_) async => null,
    );
  });
  tearDown(() {
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window'),
      null,
    );
    messenger.setMockMethodCallHandler(
      const MethodChannel('multitap/window/events'),
      null,
    );
  });

  for (final handover in [false, true]) {
    testWidgets(
      handover
          ? 'handover cancel performs no preparation or connect'
          : 'ready desktop opens once after ordinary connection consent',
      (t) async {
        t.view.physicalSize = const Size(1280, 900);
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        var opened = 0, prepared = 0;
        final machine = handover ? _handover() : entryMachine(1);
        await t.pumpWidget(
          _app(
            WorkspaceShell(
              displayName: 'fixture',
              guideAccountKey: 'fixture-account',
              serverOrigin: 'https://fixture.invalid',
              machines: [machine],
              launcherMemory: SessionLauncherMemory(),
              onSignOut: () {},
              onRefresh: () {},
              onPrepareConsole: (_) async {
                prepared++;
                return entryMachine(1);
              },
              onOpenWindow: (_, mode, options) async {
                opened++;
                return const SessionWindowOpenResult(opened: true);
              },
            ),
          ),
        );
        await t.pumpAndSettle();
        await _tap(t, 'open-${machine.id}');
        if (handover) {
          // The takeover question is the launcher's one question.
          await expectWindowsHandoverQuestion(t);
          expect(find.byType(AppConfirmDialog), findsOneWidget);
          expect(prepared, 0);
          expect(opened, 0);
          await _shot(t, 'windows-handover-consent');
          await _tap(t, 'rdp-handover-cancel');
          expect(find.byType(AppConfirmDialog), findsNothing);
          expect(prepared, 0);
          expect(opened, 0);
        } else {
          // A ready desktop keeps the plain connect question, and only it.
          expect(find.byKey(const Key('rdp-handover-confirm')), findsNothing);
          expect(find.byType(AppConfirmDialog), findsOneWidget);
          expect(opened, 0);
          await acceptStreamConnect(t, machine.id);
          await t.pumpAndSettle();
          expect(find.byType(AppConfirmDialog), findsNothing);
          expect(opened, 1);
          expect(prepared, 0);
          await _shot(t, 'windows-launcher-one-click');
        }
        expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
        await _finish(t);
      },
    );
  }

  testWidgets('removed assignment during handover consent performs no API', (
    t,
  ) async {
    // The callbacks stay the same across the list update, so only the removed
    // assignment can be what stops the agreed connection.
    final f = _Launcher([_handover()]);
    await f.mount(t);
    await _tap(t, 'open-handover');
    await expectWindowsHandoverQuestion(t);
    f.machines.value = [];
    await t.pumpAndSettle();
    await _tap(t, 'rdp-handover-accept');
    expect(find.byType(AppConfirmDialog), findsNothing);
    expect(f.prepared, 0);
    expect(f.opened, 0);
    await _finish(t);
  });

  for (final entry in ['mode', 'settings']) {
    testWidgets('$entry entry preserves handover consent before any API', (
      t,
    ) async {
      t.view.physicalSize = const Size(1280, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.reset);
      var prepared = 0, opened = 0;
      await t.pumpWidget(
        _app(
          WorkspaceShell(
            displayName: 'fixture',
            serverOrigin: 'https://fixture.invalid',
            machines: [_handover()],
            launcherMemory: SessionLauncherMemory(),
            onSignOut: () {},
            onRefresh: () {},
            onPrepareConsole: (_) async {
              prepared++;
              return entryMachine(1, id: 'handover');
            },
            onOpenWindow: (_, mode, options) async {
              opened++;
              return const SessionWindowOpenResult(opened: true);
            },
          ),
        ),
      );
      await t.pumpAndSettle();
      await _tap(t, 'stream-modes-handover');
      if (entry == 'mode') {
        await _tap(t, 'stream-mode-fullscreen');
      } else {
        await _tap(t, 'stream-settings-handover');
        await _tap(t, 'apply-stream-settings');
      }
      await expectWindowsHandoverQuestion(t);
      expect(prepared, 0);
      expect(opened, 0);
      await _tap(t, 'rdp-handover-cancel');
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect(prepared, 0);
      expect(opened, 0);
      await _finish(t);
    });
  }

  // One question (not "연결할까요?" and then the takeover question). The
  // takeover question is the launcher's question.
  testWidgets(
    'held by RDP: the takeover question alone, and accepting it connects with no second question',
    (t) async {
      final f = _Launcher([_handover()]);
      await f.mount(t);
      await _tap(t, 'open-handover');
      await expectWindowsHandoverQuestion(t);
      expect(find.byType(AppConfirmDialog), findsOneWidget);
      expect(find.text('Windows 화면을 넘겨받을까요?'), findsOneWidget);
      // The one button says both things it does.
      expect(find.text('넘겨받아 연결'), findsOneWidget);
      expect((f.prepared, f.opened), (0, 0));
      await _tap(t, 'rdp-handover-accept');
      // A second question would still be open here, and nothing opened.
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect(find.byType(ConsoleResumeDialog), findsNothing);
      expect((f.prepared, f.opened), (1, 1));
      await _finish(t);
    },
  );

  testWidgets(
    'a queued connection asks the takeover question once when it starts',
    (t) async {
      final f = _Launcher([_handover(settling: true)]);
      await f.mount(t);
      await t.tap(find.byKey(const Key('open-handover')));
      // The cleanup progress never settles; pump the modal only.
      await t.pump(const Duration(milliseconds: 350));
      // Nothing is taken over yet: the queue gets the plain question.
      expect(find.byKey(const Key('rdp-handover-confirm')), findsNothing);
      await acceptStreamConnect(t, 'handover');
      expect(find.byKey(const Key('cancel-start-handover')), findsOneWidget);
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect((f.prepared, f.opened), (0, 0));
      // The cleanup clears: the queued start reaches the open path, which
      // asks before the Windows screen is taken over.
      f.machines.value = [_handover()];
      await t.pump();
      await t.pump(const Duration(milliseconds: 350));
      expect(find.byKey(const Key('connect-confirm-dialog')), findsNothing);
      expect(find.byKey(const Key('rdp-handover-confirm')), findsOneWidget);
      expect(find.byType(AppConfirmDialog), findsOneWidget);
      expect((f.prepared, f.opened), (0, 0));
      await t.tap(find.byKey(const Key('rdp-handover-accept')));
      for (var n = 0; n < 20 && f.opened == 0; n++) {
        await t.pump(const Duration(milliseconds: 50));
      }
      await t.pump(const Duration(milliseconds: 350));
      expect((f.prepared, f.opened), (1, 1));
      expect(find.byType(AppConfirmDialog), findsNothing);
      await _finish(t);
    },
  );

  testWidgets(
    'agreeing to an ordinary connection is not agreeing to a takeover',
    (t) async {
      final f = _Launcher([entryMachine(1, id: 'handover')]);
      await f.mount(t);
      await _tap(t, 'open-handover');
      expect(find.byKey(const Key('connect-confirm-dialog')), findsOneWidget);
      // RDP takes the Windows screen while the plain question is open.
      f.machines.value = [_handover()];
      await t.pump();
      await acceptStreamConnect(t, 'handover');
      await t.pumpAndSettle();
      expect(find.byKey(const Key('rdp-handover-confirm')), findsOneWidget);
      expect((f.prepared, f.opened), (0, 0));
      await _tap(t, 'rdp-handover-cancel');
      expect(find.byType(AppConfirmDialog), findsNothing);
      expect((f.prepared, f.opened), (0, 0));
      await _finish(t);
    },
  );

  testWidgets(
    'a takeover agreement for a screen that came back connects without another question',
    (t) async {
      final f = _Launcher([_handover()]);
      await f.mount(t);
      await _tap(t, 'open-handover');
      await expectWindowsHandoverQuestion(t);
      // Windows returns to the console by itself while the question is open.
      f.machines.value = [entryMachine(1, id: 'handover')];
      await t.pump();
      await _tap(t, 'rdp-handover-accept');
      expect(find.byType(AppConfirmDialog), findsNothing);
      // Nothing was left to take over: no preparation, one connection.
      expect((f.prepared, f.opened), (0, 1));
      await _finish(t);
    },
  );

  testWidgets('remote retry cancellation never starts Windows handover', (
    t,
  ) async {
    var prepared = 0, loads = 0;
    await t.pumpWidget(
      _app(
        RemoteView(
          machine: _handover(),
          sessionGuideEnabled: false,
          onPrepareConsole: () async {
            prepared++;
            return entryMachine(1, id: 'handover');
          },
          load: () async {
            loads++;
            throw const BrokerException('network_error');
          },
        ),
      ),
    );
    await t.pumpAndSettle();
    expect(find.byKey(const Key('rdp-handover-confirm')), findsOneWidget);
    expect(prepared, 0);
    expect(loads, 0);
    await _tap(t, 'rdp-handover-cancel');
    expect(find.text('연결을 취소했어요.'), findsOneWidget);
    expect(prepared, 0);
    expect(loads, 0);
    await _finish(t);
  });

  for (final brightness in Brightness.values) {
    testWidgets(
      'compact settings preserve custom draft while disclosing details in $brightness',
      (t) async {
        t.view.physicalSize = const Size(900, 800);
        t.view.devicePixelRatio = 1;
        addTearDown(t.view.reset);
        final changes = <StreamOptions>[];
        await t.pumpWidget(
          _app(
            Scaffold(
              body: Center(
                child: SizedBox(
                  width: 420,
                  child: StreamSettingsBody(
                    machine: entryMachine(1),
                    initialOptions: _custom,
                    onChanged: changes.add,
                  ),
                ),
              ),
            ),
            brightness: brightness,
          ),
        );
        await t.pumpAndSettle();
        expect(find.byKey(const Key('stream-fps-label')), findsNothing);
        expect(find.byKey(const Key('stream-hardware-decode')), findsNothing);
        // Sound is always on: no 소리 switch, and a draft's
        // earlier 소리 꺼짐 is not shown or carried on.
        expect(find.text('소리'), findsNothing);
        expect(find.byKey(const Key('stream-audio')), findsNothing);
        expect(find.text('30 fps · 12 Mbps'), findsOneWidget);
        expect(changes, isEmpty);
        await _shot(t, 'windows-settings-basic-${brightness.name}');
        await _tap(t, 'stream-advanced');
        expect(find.byKey(const Key('stream-fps-label')), findsOneWidget);
        expect(find.byKey(const Key('stream-hardware-decode')), findsOneWidget);
        expect(find.byKey(const Key('stream-audio')), findsNothing);
        expect(changes, isEmpty);
        await _shot(t, 'windows-settings-advanced-${brightness.name}');
        await _tap(t, 'stream-advanced');
        expect(changes, isEmpty, reason: 'Disclosure is not a settings change');
        await _finish(t);
      },
    );
  }

  testWidgets(
    'normal wait advances only from stream observations to real frame',
    (t) async {
      final f = _RemoteFixture();
      try {
        await f.mount(t);
        expect(find.text('서버 연결'), findsOneWidget);
        await _shot(t, 'windows-wait-relay');
        await t.pump(const Duration(seconds: 2));
        expect(find.text('서버 연결'), findsOneWidget);
        expect(f.loads, 1);
        f.socket.incoming.add(jsonEncode(info));
        await t.pump();
        await t.pump();
        expect(find.text('첫 화면 수신 대기'), findsOneWidget);
        expect(
          find.byKey(const Key('connection-wait-elapsed')),
          findsOneWidget,
        );
        await _shot(t, 'windows-wait-first-frame');
        f.socket.incoming.add(packet());
        await t.pump();
        await t.pump();
        expect(f.session.frameReady, isTrue);
        expect(find.byKey(const Key('connection-wait-status')), findsNothing);
        expect(f.reconnects, isEmpty);
      } finally {
        await _finish(t);
      }
    },
  );

  for (final code in ['client_backpressure', 'initial_viewport_mismatch']) {
    testWidgets('$code recovery is draft-only until explicit apply', (t) async {
      final f = _RemoteFixture();
      try {
        await f.mount(t, failure: BrokerException(code));
        await t.pumpAndSettle();
        expect(
          find.byKey(const Key('recover-remote-settings')),
          findsOneWidget,
        );
        await _shot(t, 'windows-error-$code');
        await _tap(t, 'recover-remote-settings');
        expect(f.loads, 1);
        expect(f.reconnects, isEmpty);
        if (code == 'client_backpressure') {
          await _shot(t, 'windows-quality-recovery-dialog');
          expect(find.text('30 fps · 12 Mbps'), findsOneWidget);
          expect(
            t
                .widget<FilledButton>(
                  find.byKey(const Key('apply-stream-settings')),
                )
                .onPressed,
            isNull,
          );
          await t.tap(find.widgetWithText(TextButton, '취소'));
          await t.pumpAndSettle();
          expect(f.reconnects, isEmpty);
          await _tap(t, 'recover-remote-settings');
          await _tap(t, 'stream-preset-saver');
          await _tap(t, 'apply-stream-settings');
          expect(f.reconnects, hasLength(1));
          // The settings ask for sound, whatever the draft had.
          expect(f.reconnects.single.audio, isTrue);
          expect(f.reconnects.single.bitrate, 8000000);
        } else {
          await _shot(t, 'windows-display-recovery-dialog');
          await _tap(t, 'cancel-recovery-display');
          expect(f.reconnects, isEmpty);
          await _tap(t, 'recover-remote-settings');
          await _tap(t, 'apply-recovery-display');
          expect(f.reconnects, hasLength(1));
          expect(f.reconnects.single.audio, isFalse);
        }
        expect(
          find.byKey(const Key('recover-remote-settings')),
          findsNothing,
          reason: 'Auth expiry from the explicit retry cannot offer settings recovery',
        );
      } finally {
        await _finish(t);
      }
    });
  }

  testWidgets('auth and unknown failures never offer settings recovery', (
    t,
  ) async {
    for (final error in [
      const BrokerException('session_expired', 401),
      const BrokerException('unrecognized_payload'),
    ]) {
      final f = _RemoteFixture();
      try {
        await f.mount(t, failure: error);
        await t.pumpAndSettle();
        expect(find.byKey(const Key('recover-remote-settings')), findsNothing);
        expect(f.reconnects, isEmpty);
      } finally {
        await _finish(t);
      }
    }
  });

  testWidgets('desktop takeover remains an explicit destructive action', (
    t,
  ) async {
    final f = _RemoteFixture();
    try {
      final offer = StreamTakeoverOffer.parse({
        'protocol': streamTakeoverProtocol,
        'context': 'C' * 43,
        'expiresInSeconds': 120,
        'desktop': {'id': 'pve-149', 'name': 'TESTER'},
        'sessions': [
          {'username': 'alice', 'createdAt': null, 'deviceLabel': null},
        ],
      });
      await f.mount(
        t,
        failure: BrokerException(
          'desktop_busy',
          409,
          'controller_in_use',
          true,
          offer,
        ),
      );
      await t.pumpAndSettle();
      expect(f.takeovers, 0);
      expect(f.reconnects, isEmpty);
      expect(find.text('끊고 여기서 연결'), findsOneWidget);
      expect(find.byKey(const Key('recover-remote-settings')), findsNothing);
      // The card is the one question: it says whose connection ends, and
      // nothing is released until its button is pressed.
      expect(find.text('연결 중인 계정 alice'), findsOneWidget);
      await _shot(t, 'windows-error-takeover');
      await t.pump(const Duration(seconds: 2));
      expect(f.takeovers, 0);
      expect(f.legacyTakeovers, 0);
      expect(f.reconnects, isEmpty);
      await _tap(t, 'takeover-remote-connection');
      expect(find.byType(Dialog), findsNothing);
      expect(f.takeovers, 1);
      expect(f.legacyTakeovers, 0);
      expect(f.reconnects, hasLength(1));
    } finally {
      await _finish(t);
    }
  });
}
