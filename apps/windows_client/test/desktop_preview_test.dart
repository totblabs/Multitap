// desktop-preview-v1: the broker's recent picture of each assigned VM on the
// launcher cards, without connecting first. Injected sources and a recorded
// HTTP client: no broker, Host, file or screen is involved.
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/desktop_previews.dart';
import 'package:multitap_client/data/desktop_thumbnails.dart';
import 'package:multitap_client/data/launcher_memory.dart';
import 'package:multitap_client/models/desktop_machine.dart';
import 'package:multitap_client/models/stream_options.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/desktop_art.dart';
import 'package:multitap_client/ui/workspace_shell.dart';

import 'desktop_thumbnail_tile_test.dart' show FakeThumbnails, picture;

const unlocked = '1234567890abcdef0000000000000001';
const relocked = '1234567890abcdef0000000000000002';

/// The smallest byte run that passes the JPEG shape check (SOI ... EOI).
final jpeg = Uint8List.fromList([0xff, 0xd8, 0xff, 0xe0, 1, 2, 3, 0xff, 0xd9]);

Map<String, Object?> captured({int ageMs = 60000, List<int>? bytes}) => {
  'protocol': desktopPreviewProtocol,
  'state': 'captured',
  'takenAt': DateTime.now()
      .toUtc()
      .subtract(Duration(milliseconds: ageMs))
      .toIso8601String(),
  'ageMs': ageMs,
  'image': base64Encode(bytes ?? jpeg),
};

DesktopMachine machine(
  String id, {
  String? epoch = unlocked,
  bool allowed = true,
  MachineKind kind = MachineKind.vm,
}) =>
    DesktopMachine(
      id: id,
      name: 'TESTER',
      description: '',
      kind: kind,
      localPcApproved: kind == MachineKind.pc,
      status: MachineStatus.ready,
      specification: 'Windows 11 Pro',
      accent: Colors.blue,
      hostOnline: true,
      interactiveSession: true,
      virtualDisplayReady: true,
      powerState: 'running',
      virtualMonitorControl: true,
      display: VirtualDisplayState(
        supported: true,
        activeCount: 1,
        checkedAt: DateTime.utc(2026, 11, 2),
        stale: false,
      ),
      monitors: const [
        RemoteMonitor(id: '0', width: 1920, height: 1080, primary: true),
      ],
      thumbnailPrivacy: epoch == null
          ? null
          : ThumbnailPrivacy(epoch: epoch, allowed: allowed),
    );

class FakePreviews implements DesktopPreviewSource {
  final answers = <String, Map<String, Object?>>{};
  final offeredIds = <String>{};
  final reads = <String>[];
  @override
  bool offered(String desktopId) => offeredIds.contains(desktopId);
  @override
  Future<DesktopPreview> read(String desktopId) async {
    reads.add(desktopId);
    return DesktopPreview.parse(answers[desktopId]);
  }
}

Widget shell(
  List<DesktopMachine> machines,
  DesktopPreviewSource? previews, {
  ThumbnailSource? thumbnails,
  bool demo = false,
}) => MaterialApp(
  theme: buildAppTheme(brightness: Brightness.dark),
  home: WorkspaceShell(
    onSignOut: () {},
    onRefresh: () {},
    displayName: 'fixture',
    serverOrigin: 'https://vdi.example.com',
    machines: machines,
    isDemo: demo,
    launcherMemory: SessionLauncherMemory(),
    thumbnails: thumbnails,
    previews: previews,
    onConnect: (_, {options = const StreamOptions()}) async =>
        throw StateError('fixture only'),
  ),
);

DesktopArt art(WidgetTester t, String id) => t.widget<DesktopArt>(
  find.descendant(
    of: find.byKey(Key('card-$id')),
    matching: find.byType(DesktopArt),
  ),
);

String? chip(WidgetTester t, Key key) => t
    .widget<Text>(
      find.descendant(
        of: find.byKey(key),
        matching: find.byWidgetPredicate((widget) => widget is Text),
      ),
    )
    .data;

void main() {
  group('DesktopPreview.parse', () {
    test('a captured answer becomes an in-memory picture aged by the broker', () {
      final now = DateTime(2026, 11, 2, 12);
      final value = DesktopPreview.parse(captured(ageMs: 90000), now: now);
      expect(value.state, DesktopPreviewState.captured);
      expect(value.picture!.remote, isTrue);
      expect(value.picture!.image, isA<MemoryImage>());
      expect((value.picture!.image as MemoryImage).bytes, jpeg);
      expect(now.difference(value.picture!.modified), const Duration(seconds: 90));
    });

    test('states without a picture, and anything malformed, never carry one', () {
      for (final (state, expected) in [
        ('locked', DesktopPreviewState.locked),
        ('signin', DesktopPreviewState.signin),
        ('asleep', DesktopPreviewState.asleep),
        ('unavailable', DesktopPreviewState.unavailable),
        ('future-state', DesktopPreviewState.unavailable),
      ]) {
        final value = DesktopPreview.parse({
          'protocol': desktopPreviewProtocol,
          'state': state,
          'image': base64Encode(jpeg),
        });
        expect(value.state, expected);
        expect(value.picture, isNull);
      }
      for (final bad in <Object?>[
        null,
        'captured',
        {...captured(), 'protocol': 'other'},
        {...captured(), 'image': 'not base64!'},
        captured(bytes: [1, 2, 3, 4, 5]),
        captured(bytes: [0xff, 0xd8, ...List.filled(desktopPreviewMaxBytes, 0), 0xff, 0xd9]),
        {...captured(), 'ageMs': null, 'takenAt': '2026-11-02T10:00:00'},
        {...captured(), 'image': null},
      ]) {
        final value = DesktopPreview.parse(bad);
        expect(value.picture, isNull, reason: '$bad');
        expect(value.state, DesktopPreviewState.unavailable);
      }
      expect(desktopPreviewNote(DesktopPreviewState.locked), '잠겨 있어요');
      expect(desktopPreviewNote(DesktopPreviewState.signin), '로그인 화면');
      expect(desktopPreviewNote(DesktopPreviewState.asleep), '화면이 꺼져 있어요');
      expect(desktopPreviewNote(DesktopPreviewState.unavailable), isNull);
    });
  });

  group('launcher', () {
    Future<void> show(WidgetTester t, Widget widget) async {
      t.view.devicePixelRatio = 1;
      t.view.physicalSize = const Size(1440, 900);
      addTearDown(t.view.resetPhysicalSize);
      addTearDown(t.view.resetDevicePixelRatio);
      await t.pumpWidget(widget);
      await t.pump();
      await t.pump();
      await t.pump();
    }

    testWidgets(
      'A VM never connected from this PC shows its Host picture with its age; locked says so; an older broker keeps today\'s text',
      (t) async {
        final previews = FakePreviews()
          ..offeredIds.addAll({'pve-149', 'pve-150'})
          ..answers['pve-149'] = captured(ageMs: 60000)
          ..answers['pve-150'] = {
            'protocol': desktopPreviewProtocol,
            'state': 'locked',
          };
        await show(
          t,
          shell([
            machine('pve-149'),
            machine('pve-150', epoch: relocked, allowed: false),
            machine('pve-151'),
          ], previews),
        );
        expect(previews.reads..sort(), ['pve-149', 'pve-150']);
        final image = art(t, 'pve-149').thumbnail;
        expect(image, isA<MemoryImage>());
        expect((image! as MemoryImage).bytes, jpeg);
        expect(chip(t, const Key('thumbnail-caption-pve-149')), '1분 전');
        expect(find.byKey(const Key('thumbnail-empty-pve-149')), findsNothing);
        expect(art(t, 'pve-150').thumbnail, isNull);
        expect(chip(t, const Key('thumbnail-empty-pve-150')), '잠겨 있어요');
        expect(art(t, 'pve-151').thumbnail, isNull);
        expect(chip(t, const Key('thumbnail-empty-pve-151')), '미리보기 없음');
        expect(t.takeException(), isNull);
      },
    );

    testWidgets(
      'A lock or user switch (the list\'s privacy epoch moves) hides the picture at once',
      (t) async {
        final previews = FakePreviews()
          ..offeredIds.add('pve-149')
          ..answers['pve-149'] = captured();
        await show(t, shell([machine('pve-149')], previews));
        expect(art(t, 'pve-149').thumbnail, isNotNull);
        await t.pumpWidget(
          shell([machine('pve-149', epoch: relocked, allowed: false)], previews),
        );
        await t.pump();
        expect(art(t, 'pve-149').thumbnail, isNull);
        expect(find.byKey(const Key('thumbnail-caption-pve-149')), findsNothing);
        // The same epoch turned not allowed hides it too.
        await t.pumpWidget(
          shell([machine('pve-149', allowed: false)], previews),
        );
        await t.pump();
        expect(art(t, 'pve-149').thumbnail, isNull);
        // No privacy observation at all: no Host picture.
        await t.pumpWidget(shell([machine('pve-149', epoch: null)], previews));
        await t.pump();
        expect(art(t, 'pve-149').thumbnail, isNull);
      },
    );

    testWidgets('It asks again about once a minute, and not while minimised', (
      t,
    ) async {
      final previews = FakePreviews()
        ..offeredIds.add('pve-149')
        ..answers['pve-149'] = captured();
      await show(t, shell([machine('pve-149')], previews));
      expect(previews.reads, ['pve-149']);
      await t.pump(const Duration(seconds: 30));
      expect(previews.reads.length, 1);
      await t.pump(const Duration(seconds: 31));
      expect(previews.reads.length, 2);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.hidden);
      await t.pump(const Duration(seconds: 125));
      expect(previews.reads.length, 2, reason: 'nothing is captured while nobody looks');
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.inactive);
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await t.pump();
      expect(previews.reads.length, 3, reason: 'coming back asks at once');
      t.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    });

    testWidgets('The newer of this PC\'s last frame and the Host picture is shown', (
      t,
    ) async {
      final previews = FakePreviews()
        ..offeredIds.addAll({'pve-149', 'pve-150'})
        ..answers['pve-149'] = captured(ageMs: 60000)
        ..answers['pve-150'] = captured(ageMs: 9 * 60000);
      final local = FakeThumbnails()
        ..pictures['pve-149'] = picture(const Duration(minutes: 5))
        ..pictures['pve-150'] = picture(const Duration(minutes: 2), seed: 1);
      await show(
        t,
        shell(
          [machine('pve-149'), machine('pve-150')],
          previews,
          thumbnails: local,
        ),
      );
      expect((art(t, 'pve-149').thumbnail! as MemoryImage).bytes, jpeg);
      expect(chip(t, const Key('thumbnail-caption-pve-149')), '1분 전');
      expect(art(t, 'pve-150').thumbnail, local.pictures['pve-150']!.image);
      expect(chip(t, const Key('thumbnail-caption-pve-150')), '마지막 화면 · 2분 전');
    });

    testWidgets('A physical PC the list offers is asked and shows its picture', (
      t,
    ) async {
      final previews = FakePreviews()
        ..offeredIds.add('pc-1')
        ..answers['pc-1'] = captured();
      await show(t, shell([machine('pc-1', kind: MachineKind.pc)], previews));
      expect(previews.reads, ['pc-1']);
      expect((art(t, 'pc-1').thumbnail! as MemoryImage).bytes, jpeg);
    });

    testWidgets(
      'Local PCs are asked through the broker: a picture when allowed, the lock or sign-in note otherwise',
      (t) async {
        const shown = 'pc-0123abcd-4567-49ef-8123-456789abcde0',
            locked = 'pc-0123abcd-4567-49ef-8123-456789abcde1',
            signin = 'pc-0123abcd-4567-49ef-8123-456789abcde2';
        final client = _ApiClient()
          ..responses.add((
            200,
            {
              'desktops': [
                for (final id in [shown, locked, signin, 'pve-149'])
                  _desktop(
                    id,
                    previewProtocol: desktopPreviewProtocol,
                    kind: id.startsWith('pc-') ? 'pc' : 'vm',
                  ),
              ],
            },
          ))
          ..byPath['/api/v1/desktops/$shown/preview'] = (200, captured())
          ..byPath['/api/v1/desktops/$locked/preview'] = (
            200,
            {'protocol': desktopPreviewProtocol, 'state': 'locked'},
          )
          ..byPath['/api/v1/desktops/$signin/preview'] = (
            200,
            {'protocol': desktopPreviewProtocol, 'state': 'signin'},
          )
          ..byPath['/api/v1/desktops/pve-149/preview'] = (200, captured());
        final api = HttpBrokerApi(
          origin: Uri.parse('https://cloud.example.com'),
          client: client,
        );
        await t.runAsync(() => api.desktops('qa-token'));
        await show(
          t,
          shell([
            machine(shown, kind: MachineKind.pc),
            machine(locked, kind: MachineKind.pc, epoch: relocked, allowed: false),
            machine(signin, kind: MachineKind.pc, epoch: relocked, allowed: false),
            machine('pve-149'),
          ], BrokerDesktopPreviews(api, 'qa-token')),
        );
        for (var i = 0; i < 6; i++) {
          await t.runAsync(() => Future<void>.delayed(Duration.zero));
          await t.pump();
        }
        expect(
          client.requests.map((r) => r.uri.path).skip(1).toSet(),
          client.byPath.keys.toSet(),
        );
        expect((art(t, shown).thumbnail! as MemoryImage).bytes, jpeg);
        expect(art(t, locked).thumbnail, isNull);
        expect(chip(t, Key('thumbnail-empty-$locked')), '잠겨 있어요');
        expect(art(t, signin).thumbnail, isNull);
        expect(chip(t, Key('thumbnail-empty-$signin')), '로그인 화면');
        // VMs are unchanged.
        expect((art(t, 'pve-149').thumbnail! as MemoryImage).bytes, jpeg);
      },
    );

    testWidgets('The design demo and a launcher without a source ask nothing', (
      t,
    ) async {
      final previews = FakePreviews()..offeredIds.add('pve-149');
      await show(t, shell([machine('pve-149')], previews, demo: true));
      await show(t, shell([machine('pve-149')], null));
      expect(previews.reads, isEmpty);
      expect(chip(t, const Key('thumbnail-empty-pve-149')), '미리보기 없음');
    });
  });

  group('HttpBrokerApi', () {
    test('It asks only for desktops the current list offers, with the account token', () async {
      final client = _ApiClient()
        ..responses.add((
          200,
          {
            'desktops': [
              _desktop('pve-149', previewProtocol: desktopPreviewProtocol),
              _desktop('pve-150'),
              _desktop('pve-151', previewProtocol: 'desktop-preview-v9'),
              // A physical PC's Host captures its picture the same way.
              _desktop('pc-1', previewProtocol: desktopPreviewProtocol, kind: 'pc'),
            ],
          },
        ))
        ..responses.add((200, captured()))
        ..responses.add((
          409,
          {
            'error': {'code': 'desktop_previews_disabled', 'message': 'x'},
          },
        ));
      final api = HttpBrokerApi(
        origin: Uri.parse('https://cloud.example.com'),
        client: client,
      );
      expect(api.previewOffered('qa-token', 'pve-149'), isFalse);
      expect(
        (await api.desktopPreview('qa-token', 'pve-149')).state,
        DesktopPreviewState.unavailable,
        reason: 'nothing before the list says so',
      );
      expect(client.requests, isEmpty);
      await api.desktops('qa-token');
      expect(api.previewOffered('qa-token', 'pve-149'), isTrue);
      expect(api.previewOffered('qa-token', 'pve-150'), isFalse);
      expect(api.previewOffered('qa-token', 'pve-151'), isFalse);
      expect(api.previewOffered('qa-token', 'pc-1'), isTrue);
      expect(api.previewOffered('other-token', 'pve-149'), isFalse);
      final value = await api.desktopPreview('qa-token', 'pve-149');
      expect(value.state, DesktopPreviewState.captured);
      final request = client.requests.last;
      expect(request.method, 'GET');
      expect(request.uri.path, '/api/v1/desktops/pve-149/preview');
      expect(request.headers.values[HttpHeaders.authorizationHeader], 'Bearer qa-token');
      expect((await api.desktopPreview('qa-token', 'pve-150')).state, DesktopPreviewState.unavailable);
      expect(client.requests.length, 2, reason: 'an unoffered desktop is never asked');
      await expectLater(
        api.desktopPreview('qa-token', 'pve-149'),
        throwsA(isA<BrokerException>().having((e) => e.status, 'status', 409)),
      );
    });
  });
}

Map<String, Object?> _desktop(String id, {String? previewProtocol, String kind = 'vm'}) => {
  'id': id,
  'name': 'VM',
  'kind': kind,
  'description': '',
  'os': 'windows',
  'status': 'ready',
  'canConnect': true,
  'previewProtocol': ?previewProtocol,
};

class _Headers extends Fake implements HttpHeaders {
  final values = <String, Object>{};
  @override
  ContentType? contentType;
  @override
  void set(String name, Object value, {bool preserveHeaderCase = false}) {
    values[name] = value;
  }
}

class _Response extends Fake implements HttpClientResponse {
  _Response(this.statusCode, this.value);
  @override
  final int statusCode;
  final Map<String, Object?> value;
  @override
  HttpHeaders get headers => _Headers();
  @override
  Stream<List<int>> timeout(
    Duration timeLimit, {
    void Function(EventSink<List<int>>)? onTimeout,
  }) => Stream<List<int>>.value(
    utf8.encode(jsonEncode(value)),
  ).timeout(timeLimit, onTimeout: onTimeout);
}

class _Request extends Fake implements HttpClientRequest {
  _Request(this.method, this.uri, this.response);
  @override
  final String method;
  @override
  final Uri uri;
  final _Response response;
  @override
  final _Headers headers = _Headers();
  @override
  bool followRedirects = true;
  @override
  void write(Object? value) {}
  @override
  void abort([Object? exception, StackTrace? stackTrace]) {}
  @override
  Future<HttpClientResponse> close() async => response;
}

class _ApiClient extends Fake implements HttpClient {
  final responses = <(int, Map<String, Object?>)>[];
  // Answers by path (any order); everything else comes from [responses].
  final byPath = <String, (int, Map<String, Object?>)>{};
  final requests = <_Request>[];
  @override
  Duration? connectionTimeout;
  @override
  Future<HttpClientRequest> openUrl(String method, Uri uri) async {
    final response = byPath[uri.path] ?? responses.removeAt(0);
    final request = _Request(method, uri, _Response(response.$1, response.$2));
    requests.add(request);
    return request;
  }

  @override
  void close({bool force = false}) {}
}
