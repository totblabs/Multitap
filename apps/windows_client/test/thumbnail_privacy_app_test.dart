// Actual App/AccountThumbnails/Workspace wiring with memory-only dependencies.
import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/app.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'desktop_thumbnail_tile_test.dart' show AccountBroker, art, picture;
import 'workspace_live_state_test.dart' show LiveUpdates;

const _epoch = '0123456789abcdef0000000000000001';
const _newEpoch = '0123456789abcdef0000000000000002';
const _pcId = 'pc-0123abcd-4567-49ef-8123-456789abcdef';
final _origin = Uri.parse('https://cloud.example.com');

BrokerDesktop _desktop({
  String epoch = _epoch,
  bool allowed = true,
  bool attested = true,
  String id = 'pve-149',
}) {
  final privacy = <String, Object>{
    'protocol': thumbnailPrivacyProtocol,
    'epoch': epoch,
    'allowed': allowed,
  };
  return BrokerDesktop.fromJson({
    'id': id,
    'name': 'Privacy fixture',
    'kind': id.startsWith('pc-') ? 'pc' : 'vm',
    if (id.startsWith('pc-')) 'localPcApproved': true,
    'description': '',
    'os': 'Windows',
    'status': 'ready',
    'canConnect': true,
    'host': {
      'connected': true, 'capabilitiesPending': false,
      // Deliberately preserve the old Broker's blind-forward shape in this
      // fixture. Only the separate fresh Broker projection may authorize it.
      'capabilities': {'thumbnailPrivacy': privacy},
      if (attested) 'thumbnailPrivacy': privacy,
    },
  });
}

class _Broker extends AccountBroker {
  Completer<List<BrokerDesktop>>? delayed;
  @override
  Future<List<BrokerDesktop>> desktops(String token) async {
    if (networkFailure) throw const BrokerException('network_error');
    return delayed?.future ?? assigned;
  }
}

class _Store extends DesktopThumbnails {
  _Store() : super(Directory('unused-memory-thumbnail-app-fixture'));
  @override
  Future<void> retainOnly(String key) async {}
  @override
  Future<void> forget(String key) async {
    suspendAccount(key);
  }

  @override
  Future<DesktopThumbnail?> read(
    String key,
    String desktopId, {
    bool refresh = false,
  }) async => allowedEpoch(key, desktopId) == null
      ? null
      : picture(const Duration(minutes: 1));
}

void main() {
  Future<void> login(
    WidgetTester tester,
    _Broker api,
    _Store store,
    LiveUpdates updates,
  ) async {
    tester.view.physicalSize = const Size(1440, 900);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MultitapApp(
        api: api,
        serverOrigin: _origin,
        thumbnails: store,
        desktopWatchFactory: (_, _) => updates,
      ),
    );
    await tester.pumpAndSettle();
    await tester.enterText(find.byKey(const Key('sign-in-id')), 'fixture');
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'memory-only',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    updates.ready();
    for (var i = 0; i < 6; i++) {
      await tester.pump();
    }
  }

  testWidgets(
    'Host notice withdraws visible pixels while its fresh list is pending, then normal same-epoch snapshot resumes',
    (tester) async {
      final api = _Broker()..assigned = [_desktop()];
      final store = _Store(), updates = LiveUpdates();
      await login(tester, api, store, updates);
      expect(art(tester, 'pve-149').thumbnail, isNotNull);
      api.delayed = Completer<List<BrokerDesktop>>();
      updates.changed();
      // The notice runs as a microtask; the frame it schedules is the next one.
      await tester.pump();
      await tester.pump();
      expect(art(tester, 'pve-149').thumbnail, isNull);
      final held = api.delayed!;
      api.delayed = null;
      held.complete([_desktop()]);
      for (var i = 0; i < 6; i++) {
        await tester.pump();
      }
      expect(art(tester, 'pve-149').thumbnail, isNotNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'live loss fences a pending old allowed response; reconnect requires a newer authorized list',
    (tester) async {
      final acceptance = Completer<bool>(), replied = Completer<void>();
      final invalidations = <Map>[];
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method != 'thumbnailInvalidate') {
          throw StateError('Unexpected native method in the memory fixture');
        }
        invalidations.add(Map.of(call.arguments as Map));
        final accepted = await acceptance.future;
        replied.complete();
        return accepted;
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(SessionWindows.channel, null);
      });
      final api = _Broker()..assigned = [_desktop()];
      final store = _Store(), updates = LiveUpdates();
      await login(tester, api, store, updates);
      api.delayed = Completer<List<BrokerDesktop>>();
      updates.changed();
      await tester.pump();
      updates.connected = false;
      updates.notifyListeners();
      await tester.pump();
      final held = api.delayed!;
      api.delayed = null;
      held.complete([_desktop()]);
      for (var i = 0; i < 6; i++) {
        await tester.pump();
      }
      expect(
        art(tester, 'pve-149').thumbnail,
        isNull,
        reason: 'a request launched before observation loss cannot restore cached permission',
      );
      api.assigned = [_desktop(epoch: _newEpoch)];
      updates.ready();
      for (var i = 0; i < 8; i++) {
        await tester.pump();
      }
      expect(art(tester, 'pve-149').thumbnail, isNotNull);
      final key = thumbnailAccountKey(_origin, 'account-one');
      expect(store.allowedEpoch(key, 'pve-149'), _newEpoch);
      expect(invalidations, [
        {'key': key, 'desktopId': 'pve-149', 'epoch': _epoch},
      ]);
      // The picture policy was already checked while the exact native cleanup
      // acceptance was pending. Drain that issued request before disposing.
      acceptance.complete(true);
      await replied.future;
      await tester.pump();
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'fresh false invalidates own native epoch once; late old notices cannot reauthorize the tile',
    (tester) async {
      final api = _Broker()..assigned = [_desktop()];
      final store = _Store(), updates = LiveUpdates(), invalidations = <Map>[];
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method == 'thumbnailInvalidate') {
          invalidations.add(Map.of(call.arguments as Map));
          return true;
        }
        return null;
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(SessionWindows.channel, null);
      });
      await login(tester, api, store, updates);
      api.assigned = [_desktop(allowed: false)];
      updates.changed();
      for (var i = 0; i < 8; i++) {
        await tester.pump();
      }
      final key = thumbnailAccountKey(_origin, 'account-one');
      expect(art(tester, 'pve-149').thumbnail, isNull);
      expect(invalidations, [
        {'key': key, 'desktopId': 'pve-149', 'epoch': _epoch},
      ]);
      expect(
        store.acceptNotice(
          ThumbnailUpdate(key, 'pve-149', generation: 999, epoch: _epoch),
        ),
        isFalse,
      );
      expect(art(tester, 'pve-149').thumbnail, isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'list failure hides retained memory without changing workspace layout or account',
    (tester) async {
      final api = _Broker()..assigned = [_desktop()];
      final store = _Store(), updates = LiveUpdates();
      await login(tester, api, store, updates);
      api.networkFailure = true;
      updates.changed();
      for (var i = 0; i < 8; i++) {
        await tester.pump();
      }
      expect(art(tester, 'pve-149').thumbnail, isNull);
      expect(find.byType(WorkspaceShell), findsOneWidget);
      expect(find.byKey(const Key('card-pve-149')), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
  testWidgets(
    'new Host raw capability behind an old Broker stays neutral until fresh Broker attestation',
    (tester) async {
      final api = _Broker()..assigned = [_desktop(attested: false)];
      final store = _Store(), updates = LiveUpdates();
      await login(tester, api, store, updates);
      final key = thumbnailAccountKey(_origin, 'account-one');
      expect(store.allowedEpoch(key, 'pve-149'), isNull);
      expect(art(tester, 'pve-149').thumbnail, isNull);
      api.assigned = [_desktop()];
      updates.changed();
      for (var i = 0; i < 8; i++) {
        await tester.pump();
      }
      expect(store.allowedEpoch(key, 'pve-149'), _epoch);
      expect(art(tester, 'pve-149').thumbnail, isNotNull);
      api.assigned = [_desktop(attested: false)];
      updates.changed();
      for (var i = 0; i < 8; i++) {
        await tester.pump();
      }
      expect(store.allowedEpoch(key, 'pve-149'), isNull);
      expect(
        art(tester, 'pve-149').thumbnail,
        isNull,
        reason: 'losing the attestation withdraws memory even if raw capability remains',
      );
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );

  testWidgets(
    'A local PC keeps its last streamed picture under the same privacy rules as a VM',
    (tester) async {
      final invalidations = <Map>[];
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(SessionWindows.channel, (call) async {
        if (call.method == 'thumbnailInvalidate') {
          invalidations.add(Map.of(call.arguments as Map));
          return true;
        }
        return null;
      });
      addTearDown(() {
        messenger.setMockMethodCallHandler(SessionWindows.channel, null);
      });
      final api = _Broker()..assigned = [_desktop(id: _pcId), _desktop()];
      final store = _Store(), updates = LiveUpdates();
      await login(tester, api, store, updates);
      final key = thumbnailAccountKey(_origin, 'account-one');
      expect(store.allowedEpoch(key, _pcId), _epoch);
      expect(art(tester, _pcId).thumbnail, isNotNull);
      expect(art(tester, 'pve-149').thumbnail, isNotNull);
      // Locked: the PC's picture leaves and its own epoch is purged natively.
      api.assigned = [_desktop(id: _pcId, allowed: false), _desktop()];
      updates.changed();
      for (var i = 0; i < 8; i++) {
        await tester.pump();
      }
      expect(store.allowedEpoch(key, _pcId), isNull);
      expect(art(tester, _pcId).thumbnail, isNull);
      expect(art(tester, 'pve-149').thumbnail, isNotNull);
      expect(invalidations, [
        {'key': key, 'desktopId': _pcId, 'epoch': _epoch},
      ]);
      await tester.pumpWidget(const SizedBox.shrink());
    },
  );
}
