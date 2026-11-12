import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_watch.dart';
import 'package:multitap_client/models/vm_resources.dart';

import 'remote_session_test.dart' show SessionSocket;
import 'workspace_live_state_test.dart' show LiveBroker, LiveUpdates;

const _gib = 1024 * 1024 * 1024;

VmResources _sample(
  double cpu, {
  Duration age = Duration.zero,
  bool stale = false,
}) {
  final at = DateTime.now().toUtc().subtract(age);
  return VmResources(
    cpuUsagePercent: cpu,
    vcpus: 4,
    memoryUsedBytes: 2 * _gib,
    memoryTotalBytes: 8 * _gib,
    uptimeSeconds: 600,
    checkedAt: at,
    receivedAt: at,
    stale: stale,
  );
}

BrokerDesktop _desktop(VmResources resources) => BrokerDesktop(
  id: 'live-vm',
  name: '실시간 검증 VM',
  kind: 'vm',
  description: '',
  os: 'Windows',
  status: 'ready',
  canConnect: true,
  hostConnected: true,
  power: BrokerPowerSnapshot(
    state: 'running',
    stale: false,
    checkedAt: DateTime.utc(2026, 10, 27),
  ),
  powerCanForceStop: true,
  resources: resources,
);

/// The channel the real watch offers: list invalidations plus the optional
/// resource samples and catalog notices.
class LiveFeeds extends LiveUpdates implements LiveDesktopFeeds {
  final samples = StreamController<List<LiveResourceSample>>.broadcast();
  final catalogNotices = StreamController<void>.broadcast();
  @override
  Stream<List<LiveResourceSample>> get resources => samples.stream;
  @override
  Stream<void> get catalog => catalogNotices.stream;
  void disconnect() {
    connected = false;
    notifyListeners();
  }
}

class _Socket extends SessionSocket {}

String _push(int revision, List<Map<String, Object?>> desktops) => jsonEncode({
  'type': 'resources',
  'revision': revision,
  'desktops': desktops,
});

Map<String, Object?> _wire(String id, double cpu) => {
  'id': id,
  'resources': {
    'source': 'proxmox',
    'checkedAt': DateTime.now().toUtc().toIso8601String(),
    'cpuUsagePercent': cpu,
    'vcpus': 4,
    'memoryUsedBytes': 2 * _gib,
    'memoryTotalBytes': 8 * _gib,
    'uptimeSeconds': 100,
    'stale': false,
  },
};

void main() {
  group('BrokerDesktopWatch live pushes', () {
    Future<(BrokerDesktopWatch, _Socket)> open(WidgetTester t) async {
      final socket = _Socket();
      final watch = BrokerDesktopWatch(
        origin: Uri.parse('https://cloud.example.com'),
        token: 'fixture-memory-only-bearer',
        dialer: (_, _, _) async => socket,
      )..start();
      await t.pump();
      socket.incoming.add('{"type":"desktops.ready"}');
      await t.pump();
      expect(watch.connected, isTrue);
      return (watch, socket);
    }

    testWidgets('resource samples and catalog notices are delivered once', (
      t,
    ) async {
      final (watch, socket) = await open(t);
      final samples = <List<LiveResourceSample>>[];
      var catalogs = 0;
      watch.resources.listen(samples.add);
      watch.catalog.listen((_) => catalogs++);
      socket.incoming.add(_push(1, [_wire('a', 12.5), _wire('b', 40)]));
      await t.pump();
      expect(samples, hasLength(1));
      expect(samples.single.map((s) => s.id), ['a', 'b']);
      expect(samples.single.first.resources!.cpuUsagePercent, 12.5);
      expect(samples.single.first.resources!.stale, isFalse);
      // The same or an older revision is ignored; a VM that left is cleared.
      socket.incoming.add(_push(1, [_wire('a', 99)]));
      socket.incoming.add(
        _push(2, [
          {'id': 'a', 'resources': null},
        ]),
      );
      await t.pump();
      expect(samples, hasLength(2));
      expect(samples.last.single.resources, isNull);
      socket.incoming.add('{"type":"catalog","revision":1}');
      socket.incoming.add('{"type":"catalog","revision":1}');
      socket.incoming.add('{"type":"catalog","revision":2}');
      await t.pump();
      expect(catalogs, 2);
      expect(watch.connected, isTrue);
      await watch.close();
    });

    testWidgets(
      'malformed pushes, unknown types and early pushes drop the channel',
      (t) async {
        for (final message in [
          '{"type":"surprise"}',
          '{"type":"resources","revision":1,"desktops":"x"}',
          '{"type":"resources","revision":1,"desktops":[{"id":7}]}',
          '{"type":"catalog","revision":0}',
          '{"type":"desktops.changed","revision":1,"pad":"${'x' * 3000}"}',
        ]) {
          final (watch, socket) = await open(t);
          socket.incoming.add(message);
          await t.pump();
          expect(watch.connected, isFalse, reason: message);
          await watch.close();
        }
        final early = _Socket();
        final watch = BrokerDesktopWatch(
          origin: Uri.parse('https://cloud.example.com'),
          token: 'fixture-memory-only-bearer',
          dialer: (_, _, _) async => early,
        )..start();
        var heard = 0;
        watch.catalog.listen((_) => heard++);
        await t.pump();
        early.incoming.add('{"type":"catalog","revision":1}');
        await t.pump();
        expect(heard, 0);
        await watch.close();
      },
    );
  });

  group('Windows client applies live pushes', () {
    Future<void> login(
      WidgetTester t,
      LiveBroker api,
      LiveFeeds feeds, {
      Future<String?> Function(String)? updateCheck,
    }) async {
      t.view.physicalSize = const Size(1440, 900);
      t.view.devicePixelRatio = 1;
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await t.pumpWidget(
        MultitapApp(
          api: api,
          desktopWatchFactory: (_, _) => feeds,
          updateCheck: updateCheck,
          serverOrigin: Uri.parse('https://cloud.example.com'),
        ),
      );
      await t.pumpAndSettle();
      await t.enterText(find.byKey(const Key('sign-in-id')), 'qa-user');
      await t.enterText(
        find.byKey(const Key('sign-in-password')),
        'qa-password',
      );
      await t.tap(find.byKey(const Key('login-submit')));
      await t.pumpAndSettle();
      feeds.ready();
      for (var i = 0; i < 4; i++) {
        await t.pump();
      }
    }

    Finder cpu(String text) => find.descendant(
      of: find.byKey(const Key('resources-live-vm')),
      matching: find.text(text),
    );

    testWidgets('a pushed sample updates the tile without reading the list', (
      t,
    ) async {
      final api = LiveBroker()..assigned = [_desktop(_sample(23))];
      final feeds = LiveFeeds();
      await login(t, api, feeds);
      expect(cpu('23%'), findsOneWidget);
      final reads = api.reads;
      feeds.samples.add([LiveResourceSample('live-vm', _sample(61))]);
      await t.pumpAndSettle();
      expect(cpu('61%'), findsOneWidget);
      expect(cpu('23%'), findsNothing);
      expect(api.reads, reads, reason: 'a sample is not a list re-read');
      // A sample for a VM this account does not have changes nothing.
      feeds.samples.add([LiveResourceSample('other', _sample(5))]);
      await t.pumpAndSettle();
      expect(cpu('61%'), findsOneWidget);
      // The broker clearing a VM's sample leaves only its configured size.
      feeds.samples.add([const LiveResourceSample('live-vm', null)]);
      await t.pumpAndSettle();
      expect(
        find.descendant(
          of: find.byKey(const Key('resources-live-vm')),
          matching: find.textContaining('%'),
        ),
        findsNothing,
      );
    });

    testWidgets(
      'a list read older than a pushed sample keeps the newer values',
      (t) async {
        final api = LiveBroker()
          ..assigned = [_desktop(_sample(23, age: const Duration(seconds: 8)))];
        final feeds = LiveFeeds();
        await login(t, api, feeds);
        feeds.samples.add([LiveResourceSample('live-vm', _sample(70))]);
        await t.pumpAndSettle();
        expect(cpu('70%'), findsOneWidget);
        feeds.changed();
        for (var i = 0; i < 4; i++) {
          await t.pump();
        }
        expect(
          cpu('70%'),
          findsOneWidget,
          reason: 'the list entry is 8 s older',
        );
      },
    );

    testWidgets('stale only after 20 s of silence while the channel is up', (
      t,
    ) async {
      final api = LiveBroker()..assigned = [_desktop(_sample(23))];
      final feeds = LiveFeeds();
      await login(t, api, feeds);
      // 15 s old: the old 10 s rule would hide it; the live channel allows 20 s.
      feeds.samples.add([
        LiveResourceSample(
          'live-vm',
          _sample(44, age: const Duration(seconds: 15)),
        ),
      ]);
      await t.pump();
      await t.pump(const Duration(seconds: 3));
      expect(cpu('44%'), findsOneWidget);
      feeds.samples.add([
        LiveResourceSample(
          'live-vm',
          _sample(45, age: const Duration(seconds: 25)),
        ),
      ]);
      await t.pump();
      await t.pump(const Duration(seconds: 3));
      await t.pumpAndSettle();
      expect(cpu('45%'), findsNothing, reason: 'no sample for over 20 s');
      // With the channel down only the fallback list read refreshes it, so the
      // allowance is that read's period (10 s) plus slack, not 20 s.
      feeds.samples.add([LiveResourceSample('live-vm', _sample(46))]);
      await t.pumpAndSettle();
      expect(cpu('46%'), findsOneWidget);
      feeds.disconnect();
      feeds.samples.add([
        LiveResourceSample(
          'live-vm',
          _sample(47, age: const Duration(seconds: 17)),
        ),
      ]);
      await t.pump();
      await t.pump(const Duration(seconds: 3));
      await t.pumpAndSettle();
      expect(cpu('47%'), findsNothing);
    });

    testWidgets(
      'a catalog push re-reads the update catalog at once and badges the header',
      (t) async {
        final api = LiveBroker()..assigned = [_desktop(_sample(23))];
        final feeds = LiveFeeds();
        var checks = 0;
        String? newest;
        await login(
          t,
          api,
          feeds,
          updateCheck: (server) async {
            checks++;
            expect(server, startsWith('https://cloud.example.com'));
            return newest;
          },
        );
        final initial = checks;
        expect(initial, greaterThan(0));
        expect(find.byKey(const Key('client-update-badge')), findsNothing);
        newest = '9.9.9';
        feeds.catalogNotices.add(null);
        await t.pump();
        await t.pump();
        expect(checks, initial + 1);
        expect(find.byKey(const Key('client-update-badge')), findsOneWidget);
        // The timer is only the fallback: with the channel up it does not read.
        await t.pump(const Duration(minutes: 11));
        expect(checks, initial + 1);
        // Down past the interval: the fallback timer reads again.
        feeds.disconnect();
        await t.pump(const Duration(minutes: 10));
        expect(checks, greaterThan(initial + 1));
      },
    );
  });
}
