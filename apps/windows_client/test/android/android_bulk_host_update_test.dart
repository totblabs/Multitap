// Host 일괄 업데이트 on Android: the update panel's button opens the bulk
// dialog for the desktops assigned to the signed-in account, nothing is
// requested before its button, every request goes through the per-desktop
// broker route (which authorizes it), and the rows follow the desktop list.
// A fake broker and catalog; no network, Host or device.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_update_panel.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_updates.dart' show ProductRelease;
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/ui/app_theme.dart';

import '../helpers/pump_work.dart';
import 'android_launcher_test.dart' show FakeBroker, MemoryStore;
import 'android_updates_test.dart' show FakePlatform, apk, catalog;

const _current = '0.2.130', _latest = '0.2.140';

final _release = ProductRelease(
  product: 'host',
  version: _latest,
  filename: 'Multitap-Host-$_latest-windows-x64.exe',
  size: 10,
  sha256: 'a' * 64,
  url: Uri.parse(
    'https://vdi.example/downloads/Multitap-Host-$_latest-windows-x64.exe',
  ),
  changelog: '- USB 연결을 다듬었어요',
);

HostUpdateState _host({
  String version = _current,
  bool sessionActive = false,
  String? operation,
}) => HostUpdateState(
  present: true,
  connected: true,
  supported: true,
  version: version,
  latestVersion: _latest,
  newer: version != _latest,
  available: version != _latest && operation == null,
  sessionActive: sessionActive,
  operation: operation == null
      ? null
      : HostUpdateOperation(
          status: operation,
          targetVersion: _latest,
          checkedAt: DateTime.utc(2026, 11, 15, 9, 0, _tick++),
        ),
);
int _tick = 0;

BrokerDesktop _desktop(
  String id, {
  String kind = 'vm',
  HostUpdateState host = HostUpdateState.absent,
}) => BrokerDesktop(
  id: id,
  name: id.toUpperCase(),
  kind: kind,
  description: '',
  os: 'Windows 11',
  status: 'online',
  canConnect: true,
  canStream: true,
  hostConnected: true,
  hostUpdate: host,
);

typedef _Request = ({String id, String version, bool disconnect});

/// The per-desktop route of the broker: it records each request and, like
/// the broker, lists the desktop with its accepted operation afterwards.
class _HostBroker extends FakeBroker implements HostUpdateBrokerApi {
  _HostBroker(super.inventory);
  final requests = <_Request>[];

  /// Desktops another account is connected to: an ordinary account's
  /// disconnecting request is refused for them (desktop_busy, other_user).
  final otherUsers = <String>{};

  /// The list read fails with this (a sign-in that is over).
  BrokerException? listFailure;

  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (listFailure case final failure?) throw failure;
    return super.desktops(token);
  }

  void set(String id, HostUpdateState host) => inventory = [
    for (final desktop in inventory)
      desktop.id == id ? _desktop(id, kind: desktop.kind, host: host) : desktop,
  ];

  @override
  Future<HostUpdateState> requestHostUpdate(
    String token,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  }) async {
    requests.add((
      id: desktopId,
      version: version,
      disconnect: disconnectSessions,
    ));
    if (disconnectSessions && otherUsers.contains(desktopId)) {
      throw const BrokerException('desktop_busy', 409, 'other_user');
    }
    final waits = otherUsers.contains(desktopId);
    final state = _host(
      sessionActive: waits,
      operation: waits ? 'waiting' : 'accepted',
    );
    set(desktopId, state);
    return state;
  }
}

/// Signed in with this app current, so only the Hosts have updates.
Future<void> _open(
  WidgetTester tester,
  FakeBroker broker, {
  Size dp = const Size(412, 915),
}) async {
  tester.view.physicalSize = dp;
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
  await tester.pumpWidget(
    RepaintBoundary(
      key: const Key('android-bulk-render'),
      child: AndroidVdiApp(
        apiFactory: (_) => broker,
        updatesFactory: (o) => AndroidUpdates(
          o,
          transport: catalog([apk('0.1.0-preview.4', 'arm64-v8a')]),
        ),
        updatePlatform: FakePlatform(),
        hostUpdateReleaseLoader: (_) async => _release,
      ),
    ),
  );
  await tester.pumpAndSettle();
  await tester.enterText(
    find.byKey(const Key('sign-in-password')),
    'secret-pass',
  );
  await tester.tap(find.byKey(const Key('login-submit')));
  await pumpWork(tester);
  await tester.runAsync(
    () => Future<void>.delayed(const Duration(milliseconds: 100)),
  );
  await pumpWork(tester);
}

/// The header's update button, then the panel once its catalog was read.
Future<void> _openPanel(WidgetTester tester) async {
  await tester.tap(find.byKey(const Key('android-update-check')));
  await tester.pump(const Duration(milliseconds: 300));
  await tester.runAsync(
    () => Future<void>.delayed(const Duration(milliseconds: 100)),
  );
  await pumpWork(tester);
}

Future<void> _openBulk(WidgetTester tester) async {
  await _openPanel(tester);
  final bulk = find.byKey(const Key('open-bulk-host-update'));
  await tester.ensureVisible(bulk);
  await pumpWork(tester);
  await tester.tap(bulk);
  await pumpWork(tester);
}

Future<void> _press(WidgetTester tester, String key) async {
  final target = find.byKey(Key(key));
  await tester.ensureVisible(target);
  await pumpWork(tester);
  await tester.tap(target);
  await pumpWork(tester);
}

String _text(WidgetTester tester, String key) {
  final found = find.byKey(ValueKey(key));
  final widget = tester.widget(found);
  return widget is Text
      ? widget.data!
      : tester
            .widgetList<Text>(
              find.descendant(of: found, matching: find.byType(Text)),
            )
            .map((text) => text.data)
            .join();
}

/// A picture of what is on screen, only where ANDROID_BULK_CAPTURE_DIR names
/// a folder (a look at the layout; the tests do not depend on it).
Future<void> _capture(WidgetTester tester, String name) async {
  final directory = Platform.environment['ANDROID_BULK_CAPTURE_DIR'];
  if (directory == null) return;
  final boundary = tester.renderObject<RenderRepaintBoundary>(
    find.byKey(const Key('android-bulk-render')),
  );
  await tester.runAsync(() async {
    final image = await boundary.toImage(pixelRatio: 2);
    try {
      final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
      await Directory(directory).create(recursive: true);
      await File(
        '$directory/$name.png',
      ).writeAsBytes(bytes!.buffer.asUint8List());
    } finally {
      image.dispose();
    }
  });
}

void main() {
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

  testWidgets(
    'the update panel opens it; requests go one desktop at a time and the rows follow the list',
    (tester) async {
      final broker = _HostBroker([
        _desktop('pve-101', host: _host()),
        _desktop('pve-102', host: _host(sessionActive: true)),
        _desktop('pve-103'),
        _desktop('pc-gaming', kind: 'pc', host: _host()),
      ]);
      await _open(tester, broker);
      expect(find.byKey(const Key('host-update-badge')), findsOneWidget);
      await _openPanel(tester);
      expect(find.text('Host 업데이트 3대 가능'), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('open-bulk-host-update')),
          matching: find.text('Host 일괄 업데이트'),
        ),
        findsOneWidget,
      );
      await _capture(tester, 'panel');
      await _press(tester, 'open-bulk-host-update');

      // In place of the panel: this account's desktops, nothing chosen and
      // nothing requested.
      expect(find.byKey(const Key('android-update-panel')), findsNothing);
      expect(find.byKey(const Key('bulk-host-update-dialog')), findsOneWidget);
      expect(find.text('배정된 데스크톱 4대 · 최신 $_latest'), findsOneWidget);
      expect(_text(tester, 'bulk-host-version-pc-gaming'),
          '로컬 PC · $_current → $_latest');
      expect(_text(tester, 'bulk-host-status-pve-102'), '사용 중 · 요청하면 연결을 끊어요');
      expect(_text(tester, 'bulk-host-status-pve-103'),
          '서버에서 업데이트 권한·상태를 확인할 수 없어요');
      expect(
        find.descendant(
          of: find.byKey(const Key('bulk-host-disconnect-note')),
          matching: find.text(
            '연결된 데스크톱은 내 연결을 끊고 바로 업데이트해요.\n'
            '다른 사용자가 연결 중이면 그 연결이 끝난 뒤 업데이트해요.',
          ),
        ),
        findsOneWidget,
      );
      FilledButton submit() => tester.widget<FilledButton>(
        find.byKey(const Key('bulk-host-submit')),
      );
      expect(submit().onPressed, isNull);
      expect(find.text('선택한 0대 업데이트'), findsOneWidget);
      expect(broker.requests, isEmpty);
      await _capture(tester, 'dialog');

      // 접속 안 한 N대만 leaves the desktop in use out.
      expect(find.text('접속 안 한 2대만'), findsOneWidget);
      await _press(tester, 'bulk-host-select-idle');
      expect(find.text('선택한 2대 업데이트'), findsOneWidget);
      bool chosen(String id) => tester
          .widget<Checkbox>(find.byKey(ValueKey('bulk-host-select-$id')))
          .value!;
      expect(chosen('pve-101'), isTrue);
      expect(chosen('pve-102'), isFalse);
      expect(chosen('pc-gaming'), isTrue);
      expect(broker.requests, isEmpty);
      await _capture(tester, 'idle-only');

      // 모두 선택 takes the one in use too; the button then requests them,
      // one at a time, each ending that desktop's connections.
      await _press(tester, 'bulk-host-select-all');
      expect(find.text('선택한 3대 업데이트'), findsOneWidget);
      await _press(tester, 'bulk-host-submit');
      expect(broker.requests, [
        (id: 'pve-101', version: _latest, disconnect: true),
      ]);
      await tester.pump(const Duration(seconds: 4));
      await tester.pump(const Duration(seconds: 4));
      await pumpWork(tester);
      expect(broker.requests.map((request) => request.id), [
        'pve-101',
        'pve-102',
        'pc-gaming',
      ]);
      expect(broker.requests.every((request) => request.disconnect), isTrue);
      expect(_text(tester, 'bulk-host-stage-pve-101'), '대기');
      expect(find.byKey(const Key('bulk-host-summary')), findsOneWidget);
      await _capture(tester, 'requested');

      // The Hosts report through the desktop list; the dialog reads it.
      broker
        ..set('pve-101', _host(version: _latest, operation: 'completed'))
        ..set('pve-102', _host(version: _latest, operation: 'completed'))
        ..set('pc-gaming', _host(operation: 'failed'));
      await tester.pump(const Duration(seconds: 4));
      await pumpWork(tester);
      expect(_text(tester, 'bulk-host-stage-pve-101'), '완료');
      expect(_text(tester, 'bulk-host-stage-pc-gaming'), '실패');
      expect(
        _text(tester, 'bulk-host-summary'),
        '모두 끝났어요 · 완료 2대 · 실패 1대',
      );
      await _capture(tester, 'finished');

      await _press(tester, 'bulk-host-close');
      expect(find.byKey(const Key('bulk-host-update-dialog')), findsNothing);
      expect(broker.requests, hasLength(3));
    },
  );

  testWidgets(
    'another account is connected: the request waits for that connection instead',
    (tester) async {
      final broker = _HostBroker([
        _desktop('pve-101', host: _host(sessionActive: true)),
      ])..otherUsers.add('pve-101');
      await _open(tester, broker);
      await _openBulk(tester);
      // Every desktop is in use: no "접속 안 한 N대만".
      expect(find.byKey(const Key('bulk-host-select-idle')), findsNothing);
      await _press(tester, 'bulk-host-select-all');
      await _press(tester, 'bulk-host-submit');
      await tester.pump(const Duration(seconds: 4));
      await pumpWork(tester);
      // The broker refused to end the other account's connection; the same
      // request without it is stored and waits.
      expect(broker.requests, [
        (id: 'pve-101', version: _latest, disconnect: true),
        (id: 'pve-101', version: _latest, disconnect: false),
      ]);
      expect(
        _text(tester, 'bulk-host-status-pve-101'),
        '다른 사용자의 연결이 끝나면 업데이트해요',
      );
      await _press(tester, 'bulk-host-close');
    },
  );

  testWidgets('a sign-in that is over closes it and requests nothing', (
    tester,
  ) async {
    final broker = _HostBroker([
      _desktop('pve-101', host: _host()),
      _desktop('pve-102', host: _host()),
    ]);
    await _open(tester, broker);
    await _openBulk(tester);
    await _press(tester, 'bulk-host-select-all');
    broker.listFailure = const BrokerException('session_expired', 401);
    await _press(tester, 'bulk-host-refresh');
    await tester.pump(const Duration(seconds: 1));
    await pumpWork(tester);
    expect(find.byKey(const Key('bulk-host-update-dialog')), findsNothing);
    expect(find.text('로그인이 만료됐어요. 다시 로그인해 주세요.'), findsOneWidget);
    expect(broker.requests, isEmpty);
  });

  testWidgets('a failed list read says so and the next read clears it', (
    tester,
  ) async {
    final broker = _HostBroker([_desktop('pve-101', host: _host())]);
    await _open(tester, broker);
    await _openBulk(tester);
    broker.listFailure = const BrokerException('network_error');
    await _press(tester, 'bulk-host-refresh');
    expect(
      find.byKey(const Key('bulk-host-observation-failed')),
      findsOneWidget,
    );
    broker.listFailure = null;
    await _press(tester, 'bulk-host-refresh');
    expect(find.byKey(const Key('bulk-host-observation-failed')), findsNothing);
    await _press(tester, 'bulk-host-close');
  });

  testWidgets('without a Host to update the panel offers nothing', (
    tester,
  ) async {
    final broker = _HostBroker([
      _desktop('pve-101', host: _host(version: _latest)),
    ]);
    await _open(tester, broker);
    expect(find.byKey(const Key('host-update-badge')), findsNothing);
    await _openPanel(tester);
    expect(find.byKey(const Key('android-update-panel')), findsOneWidget);
    expect(find.byKey(const Key('host-update-offer')), findsNothing);
    expect(find.byKey(const Key('open-bulk-host-update')), findsNothing);
  });

  testWidgets('a broker without Host updates offers nothing', (tester) async {
    await _open(tester, FakeBroker([_desktop('pve-101', host: _host())]));
    expect(find.byKey(const Key('host-update-badge')), findsNothing);
    await _openPanel(tester);
    expect(find.byKey(const Key('open-bulk-host-update')), findsNothing);
  });

  testWidgets('the panel alone: the button only where the app gives one', (
    tester,
  ) async {
    final hosts = [
      DesktopMachine(
        id: 'pve-101',
        name: 'PVE-101',
        description: '',
        kind: MachineKind.vm,
        status: MachineStatus.ready,
        specification: 'Windows',
        accent: Colors.amber,
        hostUpdate: _host(),
      ),
    ];
    var opened = 0;
    Future<void> show({required bool bulk}) async {
      await tester.pumpWidget(
        MaterialApp(
          key: UniqueKey(),
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: AndroidUpdatePanel(
              server: 'vdi.example',
              current: '0.1.0-preview.4',
              updates: () => AndroidUpdates(
                Uri.parse('https://vdi.example'),
                transport: catalog([apk('0.1.0-preview.4', 'arm64-v8a')]),
              ),
              platform: FakePlatform(),
              hostUpdates: () => hosts,
              onHostUpdate: (_) {},
              onHostBulkUpdate: bulk ? () => opened++ : null,
            ),
          ),
        ),
      );
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await pumpWork(tester);
    }

    await show(bulk: false);
    expect(find.byKey(const Key('host-update-row-pve-101')), findsOneWidget);
    expect(find.byKey(const Key('open-bulk-host-update')), findsNothing);
    await show(bulk: true);
    expect(find.byKey(const Key('host-update-row-pve-101')), findsOneWidget);
    await _press(tester, 'open-bulk-host-update');
    expect(opened, 1);
  });
}
