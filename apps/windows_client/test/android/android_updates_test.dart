// In-app Android update: the server's version rule, catalog parsing, the
// verified download, the update dialog and the launcher entry points.
// Injected transport and platform; no network, installer or device.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_app.dart';
import 'package:multitap_client/android/android_diagnostics.dart';
import 'package:multitap_client/android/android_store.dart';
import 'package:multitap_client/android/android_update_dialog.dart';
import 'package:multitap_client/android/android_update_panel.dart';
import 'package:multitap_client/android/android_updates.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/product_updates.dart'
    show UpdateFailure, UpdateResponse, UpdateTransport;

import 'android_launcher_test.dart' show FakeBroker, MemoryStore, vm;

final origin = Uri.parse('https://vdi.example');

class FakeTransport implements UpdateTransport {
  FakeTransport(this.routes);
  final Map<String, (int, List<int>, int?)> routes;
  final requested = <Uri>[];
  bool closed = false;
  @override
  Future<UpdateResponse> get(Uri uri) async {
    requested.add(uri);
    final route = routes[uri.toString()];
    if (route == null) return UpdateResponse(404, -1, const Stream.empty());
    final (status, bytes, length) = route;
    final chunks = <List<int>>[
      for (var i = 0; i < bytes.length; i += 4096)
        bytes.sublist(i, i + 4096 > bytes.length ? bytes.length : i + 4096),
    ];
    return UpdateResponse(
      status,
      length ?? bytes.length,
      Stream.fromIterable(chunks),
    );
  }

  @override
  void close() => closed = true;
}

Map<String, Object> apk(
  String version,
  String abi, {
  int size = 10,
  String sha = 'a',
  String? url,
  String changelog = '',
}) {
  final name = 'Multitap-$version-android-$abi.apk';
  return {
    'product': 'android-client',
    'version': version,
    'abi': abi,
    'filename': name,
    'size': size,
    'sha256': sha.length == 64 ? sha : sha * 64,
    'format': 'apk',
    'url': url ?? '/downloads/$name',
    if (changelog.isNotEmpty) 'changelog': changelog,
  };
}

FakeTransport catalog(List<Object> releases, {int status = 200}) =>
    FakeTransport({
      'https://vdi.example/api/v1/downloads': (
        status,
        utf8.encode(jsonEncode({'releases': releases})),
        null,
      ),
    });

class FakePlatform implements AndroidUpdatePlatform {
  FakePlatform({this.allowed = true, this.installError});
  bool allowed;
  String? installError;
  final calls = <String>[];
  final status = StreamController<String>.broadcast();
  Directory? dir;
  @override
  Future<String?> abi() async => 'arm64-v8a';
  @override
  Future<Directory> directory() async {
    calls.add('directory');
    return dir ??= Directory.systemTemp.createTempSync('android-update-test');
  }

  @override
  Future<bool> canInstall() async {
    calls.add('canInstall');
    return allowed;
  }

  @override
  Future<void> openInstallSettings() async => calls.add('settings');
  @override
  Future<void> install(File file, AndroidRelease release) async {
    calls.add('install:${file.uri.pathSegments.last}');
    if (installError != null) throw UpdateFailure(installError!);
  }

  @override
  Future<void> discard(File file) async => calls.add('discard');
  @override
  Stream<String> get statuses => status.stream;
}

/// Download without file IO (widget tests run in fake async).
class InstantUpdates extends AndroidUpdates {
  InstantUpdates(super.origin, {this.failure})
    : super(transport: FakeTransport({}));
  final String? failure;
  bool closedByDialog = false;
  int downloads = 0;
  @override
  Future<File> download(
    AndroidRelease release,
    Directory directory,
    void Function(int, int) progress, {
    void Function()? verifying,
  }) async {
    downloads++;
    progress(release.size ~/ 2, release.size);
    verifying?.call();
    if (failure != null) throw UpdateFailure(failure!);
    return File('${directory.path}/${release.filename}');
  }

  @override
  void close() {
    closedByDialog = true;
    super.close();
  }
}

class DeferredPreparationPlatform extends FakePlatform {
  var permission = Completer<bool>();
  final location = Completer<Directory>();
  @override
  Future<bool> canInstall() {
    calls.add('canInstall');
    return permission.future;
  }

  @override
  Future<Directory> directory() {
    calls.add('directory');
    return location.future;
  }
}

final _release = AndroidRelease(
  version: '0.1.0-preview.5',
  abi: 'arm64-v8a',
  size: 26214400,
  sha256: 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
  url: Uri(),
  changelog: '- 앱 안에서 업데이트해요',
);

AndroidRelease releaseAt(Uri url) => AndroidRelease(
  version: _release.version,
  abi: _release.abi,
  size: 10,
  sha256: _release.sha256,
  url: url,
);

Future<void> openDialog(
  WidgetTester tester,
  AndroidUpdates updates,
  FakePlatform platform,
) async {
  await tester.pumpWidget(
    MaterialApp(
      home: Builder(
        builder: (context) => Scaffold(
          body: TextButton(
            onPressed: () => showDialog<void>(
              context: context,
              builder: (_) => AndroidUpdateDialog(
                release: releaseAt(
                  origin.resolve('/downloads/${_release.filename}'),
                ),
                current: '0.1.0-preview.4',
                updates: updates,
                platform: platform,
              ),
            ),
            child: const Text('open'),
          ),
        ),
      ),
    ),
  );
  await tester.tap(find.text('open'));
  await tester.pumpAndSettle();
}

void main() {
  testWidgets(
    'configured server updates open before login without authentication or download',
    (tester) async {
      MemoryStore(origin: 'https://vdi.example').install();
      addTearDown(
        () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
          AndroidStore.channel,
          null,
        ),
      );
      final broker = FakeBroker([vm()]);
      final platform = FakePlatform();
      final transport = catalog([
        apk('0.1.0-preview.32', 'arm64-v8a', changelog: 'TV 리모컨 개선'),
      ]);
      await tester.pumpWidget(
        AndroidVdiApp(
          apiFactory: (_) => broker,
          updatesFactory: (o) => AndroidUpdates(o, transport: transport),
          updatePlatform: platform,
        ),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('sign-in-id')), findsOneWidget);
      await tester.ensureVisible(find.byKey(const Key('login-update-button')));
      await tester.tap(find.byKey(const Key('login-update-button')));
      await tester.pump();
      // The transport's stream is delivered outside the widget fake clock.
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('android-update-panel')), findsOneWidget);
      expect(
        find.byKey(const Key('android-update-panel-install')),
        findsOneWidget,
      );
      expect(find.textContaining('TV 리모컨 개선'), findsOneWidget);
      expect(broker.calls, isEmpty);
      expect(platform.calls, isEmpty);
      expect(transport.requested, isNotEmpty);
      expect(
        transport.requested.every(
          (u) =>
              u.origin == origin.origin &&
              (u.path == '/api/v1/downloads' ||
                  u.path == '/api/v1/releases/history'),
        ),
        true,
      );
      await tester.pumpWidget(const SizedBox.shrink());
      await tester.pumpAndSettle();
    },
  );

  testWidgets('preparation errors release the lock for a permission retry', (
    tester,
  ) async {
    final updates = InstantUpdates(origin);
    final platform = DeferredPreparationPlatform();
    await openDialog(tester, updates, platform);
    await tester.tap(find.byKey(const Key('android-update-install')));
    await tester.pump();
    platform.permission.completeError(StateError('fake permission lookup'));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-update-install')), findsOneWidget);
    expect(find.textContaining('설치 권한이나 다운로드 위치를 확인하지 못했어요'), findsOneWidget);
    expect(updates.downloads, 0);
    platform.permission = Completer<bool>();
    await tester.tap(find.byKey(const Key('android-update-install')));
    await tester.pump();
    platform.permission.complete(false);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-update-permission')), findsOneWidget);
    expect(platform.calls.where((c) => c == 'canInstall'), hasLength(2));
  });
  testWidgets('install locks synchronously before permission and directory '
      'awaits and starts only one download', (tester) async {
    final updates = InstantUpdates(origin);
    final platform = DeferredPreparationPlatform();
    await openDialog(tester, updates, platform);
    final pressed = tester
        .widget<FilledButton>(find.byKey(const Key('android-update-install')))
        .onPressed!;
    // Same rendered callback can be invoked twice before the next frame.
    pressed();
    pressed();
    await tester.pump();
    expect(platform.calls.where((c) => c == 'canInstall'), hasLength(1));
    expect(find.byKey(const Key('android-update-preparing')), findsOneWidget);
    expect(
      tester
          .widget<LinearProgressIndicator>(find.byType(LinearProgressIndicator))
          .value,
      isNull,
    );
    expect(updates.downloads, 0);
    platform.permission.complete(true);
    await tester.pump();
    expect(platform.calls.where((c) => c == 'directory'), hasLength(1));
    expect(find.byKey(const Key('android-update-preparing')), findsOneWidget);
    platform.location.complete(Directory('fake-downloads'));
    await tester.pumpAndSettle();
    expect(updates.downloads, 1);
    expect(platform.calls.where((c) => c.startsWith('install:')), hasLength(1));
  });

  testWidgets('cancelling preparation ignores late permission without '
      'requesting a directory or downloading', (tester) async {
    final updates = InstantUpdates(origin);
    final platform = DeferredPreparationPlatform();
    await openDialog(tester, updates, platform);
    await tester.tap(find.byKey(const Key('android-update-install')));
    await tester.pump();
    await tester.tap(find.byKey(const Key('android-update-cancel')));
    // Complete while the popped route is still mounted for its reverse animation.
    expect(
      tester
          .state(find.byType(AndroidUpdateDialog, skipOffstage: false))
          .mounted,
      isTrue,
    );
    platform.permission.complete(true);
    await tester.pumpAndSettle();
    expect(platform.calls, ['canInstall']);
    expect(updates.downloads, 0);
    expect(updates.closedByDialog, isTrue);
    expect(tester.takeException(), isNull);
  });
  for (final path in ['back', 'barrier', 'cancel']) {
    testWidgets(
      '$path invalidates directory preparation before route disposal',
      (tester) async {
        final updates = InstantUpdates(origin);
        final platform = DeferredPreparationPlatform();
        await openDialog(tester, updates, platform);
        await tester.tap(find.byKey(const Key('android-update-install')));
        await tester.pump();
        platform.permission.complete(true);
        await tester.pump();
        expect(platform.calls, ['canInstall', 'directory']);
        final dialogState = tester.state(find.byType(AndroidUpdateDialog));
        if (path == 'back') {
          await tester.binding.handlePopRoute();
        } else if (path == 'barrier') {
          await tester.tapAt(const Offset(1, 1));
        } else {
          await tester.tap(find.byKey(const Key('android-update-cancel')));
        }
        expect(dialogState.mounted, isTrue);
        expect(updates.closedByDialog, isTrue);
        platform.location.complete(Directory('fake-downloads'));
        await tester.pump();
        expect(updates.downloads, 0);
        expect(platform.calls.where((c) => c.startsWith('install:')), isEmpty);
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
      },
    );
  }
  testWidgets('badge and panel share a pending APK read instead of publishing '
      'competing version results', (tester) async {
    var reads = 0;
    final pending = Completer<String?>();
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      AndroidStore.channel,
      (call) async {
        switch (call.method) {
          case 'appVersion':
            return ++reads == 1 ? null : pending.future;
          case 'origin':
            return origin.origin;
          case 'login':
            return {'username': 'tester', 'password': 'secret-pass'};
          case 'preferences':
            return <String, String>{};
        }
        return null;
      },
    );
    addTearDown(
      () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        AndroidStore.channel,
        null,
      ),
    );
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => FakeBroker([vm()]),
        updatesFactory: (o) => AndroidUpdates(
          o,
          transport: catalog([apk('0.1.0-preview.5', 'arm64-v8a')]),
        ),
        updatePlatform: FakePlatform(),
      ),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    expect(reads, 2);
    await tester.tap(find.byKey(const Key('android-update-check')));
    // The package read is deliberately pending, so the panel's spinner
    // cannot settle yet. Verify that waiting state before completing it.
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const Key('android-update-panel')), findsOneWidget);
    expect(
      find.descendant(
        of: find.byKey(const Key('android-update-panel')),
        matching: find.byType(LinearProgressIndicator),
      ),
      findsOneWidget,
    );
    expect(find.byKey(const Key('android-update-panel-install')), findsNothing);
    expect(reads, 2, reason: 'the panel joins the header check');
    pending.complete('0.1.0-preview.4');
    await tester.pump();
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 100)),
    );
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('client-update-badge')), findsOneWidget);
    expect(
      find.byKey(const Key('android-update-panel-install')),
      findsOneWidget,
    );
    expect(find.text('0.1.0-preview.4'), findsWidgets);
  });

  testWidgets('a version response from an earlier server visit is ignored '
      'even when the displayed origin returns to the same value', (
    tester,
  ) async {
    var serverVisit = 0;
    final visit = serverVisit;
    final pending = Completer<String?>();
    final transport = catalog([apk('0.1.0-preview.5', 'arm64-v8a')]);
    final checked = <AndroidRelease?>[];
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AndroidUpdatePanel(
            server: origin.host,
            current: null,
            readCurrent: () => pending.future,
            isCurrent: () => serverVisit == visit,
            updates: () => AndroidUpdates(origin, transport: transport),
            platform: FakePlatform(),
            onChecked: checked.add,
          ),
        ),
      ),
    );
    await tester.pump();
    serverVisit +=
        2; // A -> B -> A has a new visit, despite equal address text.
    pending.complete('0.1.0-preview.4');
    await tester.pumpAndSettle();
    expect(checked, isEmpty);
    expect(transport.requested, isEmpty);
    expect(find.byKey(const Key('android-update-panel-install')), findsNothing);
  });
  testWidgets('retry rereads the APK version and reports a recovered update '
      'only after the running version is known', (tester) async {
    var reads = 0;
    final checked = <AndroidRelease?>[];
    final transport = catalog([apk('0.1.0-preview.5', 'arm64-v8a')]);
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AndroidUpdatePanel(
            server: origin.host,
            current: null,
            readCurrent: () async => ++reads < 3 ? null : '0.1.0-preview.4',
            updates: () => AndroidUpdates(origin, transport: transport),
            platform: FakePlatform(),
            onChecked: checked.add,
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(reads, 1);
    expect(find.byKey(const Key('android-update-panel-install')), findsNothing);
    expect(transport.requested, isEmpty);
    await tester.tap(find.byKey(const Key('android-update-panel-retry')));
    await tester.pumpAndSettle();
    expect(reads, 2);
    expect(find.byKey(const Key('android-update-panel-install')), findsNothing);
    expect(checked, isEmpty);
    await tester.tap(find.byKey(const Key('android-update-panel-retry')));
    await tester.pump();
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 100)),
    );
    await tester.pumpAndSettle();
    expect(reads, 3);
    expect(
      find.byKey(const Key('android-update-panel-install')),
      findsOneWidget,
    );
    expect(checked.single?.version, '0.1.0-preview.5');
    expect(find.text('0.1.0-preview.4'), findsWidgets);
  });
  TestWidgetsFlutterBinding.ensureInitialized();

  test('versions order exactly as the server compareVersion', () {
    // Output of server/src/downloads.mjs compareReleaseVersions for these
    // pairs (Node 24).
    const server = [
      ['0.1.0-preview', '0.1.0-preview.1', -1],
      ['0.1.0-preview.1', '0.1.0-preview.2', -1],
      ['0.1.0-preview.4', '0.1.0-preview.5', -1],
      ['0.1.0-preview.9', '0.1.0-preview.10', -1],
      ['0.1.0-preview.4', '0.1.0-preview.4', 0],
      ['0.1.0-preview.5', '0.1.0', -1],
      ['0.1.0', '0.1.1-preview.1', -1],
      ['0.1.9', '0.1.10', -1],
      ['0.2.0-preview.1', '0.1.9', 1],
      ['1.0.0', '0.99.99', 1],
      ['0.1.0', '0.1.0', 0],
    ];
    for (final [a, b, expected] in server) {
      expect(
        compareAndroidVersions(a as String, b as String).sign,
        expected,
        reason: '$a vs $b',
      );
      expect(compareAndroidVersions(b, a).sign, -(expected as int));
    }
    for (final invalid in [
      '0.1',
      '0.1.0-beta.1',
      '0.1.0-preview.0',
      '01.0.0',
    ]) {
      expect(
        () => compareAndroidVersions(invalid, '0.1.0'),
        throwsA(isA<UpdateFailure>()),
      );
    }
  });

  test(
    'check reads only the catalog and picks this ABI newer release',
    () async {
      final transport = catalog([
        {'product': 'client', 'version': '0.4.76', 'format': 'installer'},
        apk('0.1.0-preview.4', 'arm64-v8a'),
        apk('0.1.0-preview.5', 'arm64-v8a', changelog: '변경'),
        apk('0.1.0-preview.6', 'armeabi-v7a'),
        {'product': 'android-client', 'abi': 'x86_64', 'version': 'bad'},
      ]);
      final updates = AndroidUpdates(origin, transport: transport);
      final release = await updates.check('0.1.0-preview.4', 'arm64-v8a');
      expect(release?.version, '0.1.0-preview.5');
      expect(release?.changelog, '변경');
      expect(
        release?.url.toString(),
        'https://vdi.example/downloads/Multitap-0.1.0-preview.5-android-arm64-v8a.apk',
      );
      expect(transport.requested.map((u) => u.path), ['/api/v1/downloads']);
      expect(await updates.check('0.1.0-preview.5', 'arm64-v8a'), isNull);
      expect(await updates.check('0.1.0', 'arm64-v8a'), isNull);
    },
  );

  test(
    'the open-source 1.0.0 is offered over the last preview (2026-11-03)',
    () async {
      final transport = catalog([
        apk('0.1.0-preview.66', 'arm64-v8a'),
        apk('1.0.0', 'arm64-v8a', changelog: '1.0.0'),
        apk('0.1.0-preview.65', 'arm64-v8a'),
      ]);
      final updates = AndroidUpdates(origin, transport: transport);
      for (final installed in [
        '0.1.0-preview.65',
        '0.1.0-preview.66',
        '0.1.0-preview.5',
      ]) {
        final release = await updates.check(installed, 'arm64-v8a');
        expect(release?.version, '1.0.0', reason: installed);
        expect(release?.filename, 'Multitap-1.0.0-android-arm64-v8a.apk');
      }
      expect(await updates.check('1.0.0', 'arm64-v8a'), isNull);
      expect(compareAndroidVersions('1.0.0', '0.1.0-preview.999'), 1);
      expect(compareAndroidVersions('1.0.1', '1.0.0'), 1);
      // The diagnostics wire version of a release APK is its plain x.y.z.
      expect(androidDiagnosticVersion('1.0.0'), '1.0.0');
      expect(androidDiagnosticVersion('0.1.0-preview.66'), '0.1.0+66');
    },
  );

  test('a malformed or ambiguous entry of this ABI fails the check', () async {
    for (final releases in [
      [apk('0.1.0-preview.5', 'arm64-v8a', url: '/elsewhere/x.apk')],
      [apk('0.1.0-preview.5', 'arm64-v8a', size: 0)],
      [
        apk('0.1.0-preview.5', 'arm64-v8a'),
        apk('0.1.0-preview.5', 'arm64-v8a', sha: 'b'),
      ],
      [
        {...apk('0.1.0-preview.5', 'arm64-v8a'), 'changelog': 'a\u0001b'},
      ],
      [
        {...apk('0.1.0-preview.5', 'arm64-v8a'), 'format': 'zip'},
      ],
    ]) {
      await expectLater(
        AndroidUpdates(
          origin,
          transport: catalog(releases),
        ).check('0.1.0-preview.4', 'arm64-v8a'),
        throwsA(
          isA<UpdateFailure>().having((e) => e.code, 'code', 'invalid_catalog'),
        ),
      );
    }
    await expectLater(
      AndroidUpdates(
        origin,
        transport: catalog([], status: 500),
      ).check('0.1.0-preview.4', 'arm64-v8a'),
      throwsA(
        isA<UpdateFailure>().having(
          (e) => e.code,
          'code',
          'catalog_unavailable',
        ),
      ),
    );
    final plain = catalog([]);
    expect(
      () => AndroidUpdates(Uri.parse('http://vdi.example'), transport: plain),
      throwsFormatException,
    );
    expect(plain.requested, isEmpty, reason: 'no cleartext request');
    await expectLater(
      AndroidUpdates(origin, transport: plain).check('unknown', 'arm64-v8a'),
      throwsA(
        isA<UpdateFailure>().having((e) => e.code, 'code', 'version_unknown'),
      ),
    );
  });

  group('download', () {
    late Directory dir;
    setUp(() => dir = Directory.systemTemp.createTempSync('android-apk-test'));
    tearDown(() => dir.deleteSync(recursive: true));
    final bytes = List<int>.generate(10000, (i) => i * 7 & 255);
    final digest = sha256.convert(bytes).toString();
    final url = origin.resolve(
      '/downloads/Multitap-0.1.0-preview.5-android-arm64-v8a.apk',
    );
    AndroidRelease release({int? size, String? sha, Uri? at}) => AndroidRelease(
      version: '0.1.0-preview.5',
      abi: 'arm64-v8a',
      size: size ?? bytes.length,
      sha256: sha ?? digest,
      url: at ?? url,
    );
    FakeTransport serving(List<int> body, {int status = 200, int? length}) =>
        FakeTransport({url.toString(): (status, body, length)});

    test('exact size and SHA-256 give the final file', () async {
      final progress = <int>[];
      var verifying = false;
      final file = await AndroidUpdates(origin, transport: serving(bytes))
          .download(
            release(),
            dir,
            (received, total) => progress.add(received),
            verifying: () => verifying = true,
          );
      expect(file.path.endsWith(release().filename), isTrue);
      expect(await file.readAsBytes(), bytes);
      expect(progress.last, bytes.length);
      expect(verifying, isTrue);
      expect(
        dir.listSync().map((e) => e.path.split(Platform.pathSeparator).last),
        [release().filename],
      );
    });

    test('a wrong digest, size or status leaves no file', () async {
      for (final (transport, target, code) in [
        (serving(bytes), release(sha: 'b' * 64), 'hash_mismatch'),
        (serving([...bytes, 1]), release(), 'download_failed'),
        (serving(bytes.sublist(1)), release(), 'download_failed'),
        (
          serving(bytes, length: -1),
          release(size: bytes.length + 1),
          'hash_mismatch',
        ),
        (serving(bytes, status: 302), release(), 'download_failed'),
      ]) {
        await expectLater(
          AndroidUpdates(
            origin,
            transport: transport,
          ).download(target, dir, (_, _) {}),
          throwsA(isA<UpdateFailure>().having((e) => e.code, 'code', code)),
        );
        expect(dir.listSync(), isEmpty);
      }
    });

    test('only https same-origin /downloads/ URLs are requested', () async {
      for (final at in [
        Uri.parse(
          'https://other.example/downloads/Multitap-0.1.0-preview.5-android-arm64-v8a.apk',
        ),
        Uri.parse(
          'http://vdi.example/downloads/Multitap-0.1.0-preview.5-android-arm64-v8a.apk',
        ),
        url.replace(query: 'x=1'),
        origin.resolve('/files/Multitap-0.1.0-preview.5-android-arm64-v8a.apk'),
        origin.resolve(
          '/downloads/Multitap-0.1.0-preview.6-android-arm64-v8a.apk',
        ),
      ]) {
        final transport = serving(bytes);
        await expectLater(
          AndroidUpdates(
            origin,
            transport: transport,
          ).download(release(at: at), dir, (_, _) {}),
          throwsA(
            isA<UpdateFailure>().having(
              (e) => e.code,
              'code',
              'invalid_catalog',
            ),
          ),
        );
        expect(transport.requested, isEmpty, reason: '$at');
      }
    });
  });

  testWidgets('dialog shows version and changelog; install hands off', (
    tester,
  ) async {
    final platform = FakePlatform();
    final updates = InstantUpdates(origin);
    await openDialog(tester, updates, platform);
    expect(find.text('새 버전 0.1.0-preview.5'), findsOneWidget);
    expect(find.textContaining('현재 0.1.0-preview.4'), findsOneWidget);
    expect(platform.calls.where((c) => c.startsWith('install')), isEmpty);
    await tester.tap(find.byKey(const Key('android-update-install')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-update-handed-off')), findsOneWidget);
    expect(
      platform.calls.last,
      'install:Multitap-0.1.0-preview.5-android-arm64-v8a.apk',
    );
    await tester.tap(find.byKey(const Key('android-update-close')));
    await tester.pumpAndSettle();
    expect(updates.closedByDialog, isTrue);
    platform.dir?.deleteSync(recursive: true);
  });

  testWidgets('without the install permission it explains and opens settings', (
    tester,
  ) async {
    final platform = FakePlatform(allowed: false);
    await openDialog(tester, InstantUpdates(origin), platform);
    await tester.tap(find.byKey(const Key('android-update-install')));
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-update-permission')), findsOneWidget);
    expect(
      platform.calls.contains('directory'),
      isFalse,
      reason: 'no download',
    );
    await tester.tap(find.byKey(const Key('android-update-settings')));
    await tester.pumpAndSettle();
    expect(platform.calls.last, 'settings');
    platform.allowed = true;
    tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('android-update-note')), findsOneWidget);
    expect(find.byKey(const Key('android-update-install')), findsOneWidget);
  });

  testWidgets('a refused package shows the fixed reason', (tester) async {
    final platform = FakePlatform(installError: 'package_mismatch');
    await openDialog(tester, InstantUpdates(origin), platform);
    await tester.tap(find.byKey(const Key('android-update-install')));
    await tester.pumpAndSettle();
    expect(find.text(androidUpdateMessage('package_mismatch')), findsOneWidget);
    expect(find.text('다시 시도'), findsOneWidget);
    expect(platform.calls.last, 'discard');
    platform.dir?.deleteSync(recursive: true);
  });

  Future<FakeTransport> signedIn(
    WidgetTester tester,
    List<Object> releases,
  ) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    final transport = catalog(releases);
    await tester.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => FakeBroker([vm()]),
        updatesFactory: (o) => AndroidUpdates(o, transport: transport),
        updatePlatform: FakePlatform(),
      ),
    );
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'secret-pass',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    // The fake catalog body is delivered outside the test's fake clock.
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 100)),
    );
    await tester.pumpAndSettle();
    return transport;
  }

  testWidgets(
    'after sign-in a newer APK badges the header and opens the panel',
    (tester) async {
      final transport = await signedIn(tester, [
        apk('0.1.0-preview.5', 'arm64-v8a', changelog: '새 기능'),
      ]);
      expect(find.byKey(const Key('client-update-badge')), findsOneWidget);
      expect(find.text('새 버전 0.1.0-preview.5'), findsNothing); // compact bar
      expect(transport.requested.map((u) => u.path), ['/api/v1/downloads']);
      await tester.tap(find.byKey(const Key('android-update-check')));
      await tester.pump(const Duration(milliseconds: 300));
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('android-update-panel')), findsOneWidget);
      expect(
        find.descendant(
          of: find.byKey(const Key('android-update-version-card')),
          matching: find.text('0.1.0-preview.4'),
        ),
        findsOneWidget,
      );
      expect(
        find.byKey(const Key('android-update-new-version')),
        findsOneWidget,
      );
      expect(find.text('새 기능'), findsOneWidget);
      await tester.tap(find.byKey(const Key('android-update-panel-install')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('android-update-dialog')), findsOneWidget);
      await tester.tap(find.byKey(const Key('android-update-later')));
      await tester.pumpAndSettle();
      // Nothing was downloaded: only the catalog was read.
      expect(transport.requested.map((u) => u.path).toSet(), {
        '/api/v1/downloads',
      });
    },
  );

  testWidgets('a current app is called 최신 in the panel, with no install', (
    tester,
  ) async {
    await signedIn(tester, [apk('0.1.0-preview.4', 'arm64-v8a')]);
    expect(find.byKey(const Key('client-update-badge')), findsNothing);
    await tester.tap(find.byKey(const Key('android-update-check')));
    await tester.pump(const Duration(milliseconds: 300));
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 100)),
    );
    await tester.pumpAndSettle();
    expect(find.text('설치된 버전'), findsOneWidget);
    expect(find.text('최신'), findsOneWidget);
    expect(find.byKey(const Key('android-update-panel-install')), findsNothing);
  });

  testWidgets(
    'an app newer than the server is not called 최신, and older notes name their version',
    (tester) async {
      // preview.4 installed, the server at preview.3.
      await signedIn(tester, [apk('0.1.0-preview.3', 'arm64-v8a')]);
      await tester.tap(find.byKey(const Key('android-update-check')));
      await tester.pump(const Duration(milliseconds: 300));
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await tester.pumpAndSettle();
      expect(find.text('서버보다 새 버전'), findsOneWidget);
      expect(find.text('최신'), findsNothing);
      expect(find.text('지금 버전에서 달라진 점'), findsNothing);
      expect(find.text('서버 버전 0.1.0-preview.3에서 달라진 점'), findsOneWidget);
      expect(
        find.byKey(const Key('android-update-panel-install')),
        findsNothing,
      );
    },
  );

  testWidgets('the panel lists every version note as a timeline', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1080, 2400);
    tester.view.devicePixelRatio = 3;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
    final transport = FakeTransport({
      'https://vdi.example/api/v1/downloads': (
        200,
        utf8.encode(
          jsonEncode({
            'releases': [apk('0.1.0-preview.5', 'arm64-v8a', changelog: '다섯째')],
          }),
        ),
        null,
      ),
      'https://vdi.example/api/v1/releases/history?product=android-client&limit=40':
          (
            200,
            utf8.encode(
              jsonEncode({
                'releases': [
                  {'version': '0.1.0-preview.5', 'changelog': '다섯째'},
                  {'version': '0.1.0-preview.4', 'changelog': '넷째'},
                  {'version': '0.1.0-preview.3', 'changelog': '셋째'},
                ],
                'total': 3,
              }),
            ),
            null,
          ),
    });
    await tester.pumpWidget(
      AndroidVdiApp(
        apiFactory: (_) => FakeBroker([vm()]),
        updatesFactory: (o) => AndroidUpdates(o, transport: transport),
        updatePlatform: FakePlatform(),
      ),
    );
    await tester.pumpAndSettle();
    await tester.enterText(
      find.byKey(const Key('sign-in-password')),
      'secret-pass',
    );
    await tester.tap(find.byKey(const Key('login-submit')));
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-update-check')));
    await tester.pump(const Duration(milliseconds: 300));
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 100)),
    );
    await tester.pumpAndSettle();
    await tester.tap(find.byKey(const Key('android-open-release-history')));
    await tester.pump(const Duration(milliseconds: 300));
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 100)),
    );
    await tester.pumpAndSettle();
    expect(find.byKey(const Key('release-history-dialog')), findsOneWidget);
    expect(find.textContaining('Android'), findsWidgets);
    for (final v in ['0.1.0-preview.5', '0.1.0-preview.4', '0.1.0-preview.3']) {
      expect(
        find.byKey(Key('release-all-header-$v')),
        findsOneWidget,
        reason: v,
      );
    }
    expect(find.byKey(const Key('release-current')), findsOneWidget);
    expect(
      find.byKey(const Key('release-new-0.1.0-preview.5')),
      findsOneWidget,
    );
  });

  test('latest() ignores the running version and history falls back to the catalog', () async {
    final transport = catalog([
      apk('0.1.0-preview.4', 'arm64-v8a', changelog: '넷째'),
      apk('0.1.0-preview.5', 'arm64-v8a', changelog: '다섯째'),
      apk('0.1.0-preview.9', 'x86_64'),
    ]);
    final updates = AndroidUpdates(origin, transport: transport);
    expect((await updates.latest('arm64-v8a'))?.version, '0.1.0-preview.5');
    final page = await updates.historyPage('arm64-v8a');
    expect(page.releases.map((n) => n.version), [
      '0.1.0-preview.5',
      '0.1.0-preview.4',
    ]);
    expect(page.releases.first.changelog, '다섯째');
  });

  group('assigned Host updates (2026-11-03)', () {
    BrokerDesktop desktop(String id, {String kind = 'vm', bool offer = true}) =>
        BrokerDesktop(
          id: id,
          name: id.toUpperCase(),
          kind: kind,
          description: '',
          os: 'Windows 11',
          status: 'online',
          canConnect: true,
          canStream: true,
          hostConnected: true,
          hostUpdate: offer
              ? const HostUpdateState(
                  present: true,
                  connected: true,
                  supported: true,
                  version: '0.2.130',
                  latestVersion: '0.2.140',
                  newer: true,
                  available: true,
                )
              : HostUpdateState.absent,
        );

    /// Signed in with this app current, so only the Hosts have updates.
    Future<void> open(
      WidgetTester tester,
      FakeBroker broker, {
      Size dp = const Size(1280, 900),
    }) async {
      tester.view.physicalSize = dp;
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.reset);
      MemoryStore(origin: 'https://vdi.example', username: 'tester').install();
      await tester.pumpWidget(
        AndroidVdiApp(
          apiFactory: (_) => broker,
          updatesFactory: (o) => AndroidUpdates(
            o,
            transport: catalog([apk('0.1.0-preview.4', 'arm64-v8a')]),
          ),
          updatePlatform: FakePlatform(),
        ),
      );
      await tester.pumpAndSettle();
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'secret-pass',
      );
      await tester.tap(find.byKey(const Key('login-submit')));
      await tester.pumpAndSettle();
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await tester.pumpAndSettle();
    }

    testWidgets('badge the header; the panel lists them and opens one', (
      tester,
    ) async {
      final broker = _HostBroker([
        desktop('pve-101'),
        desktop('pve-102', offer: false),
        desktop('pc-gaming', kind: 'pc'),
      ]);
      await open(tester, broker);
      final check = find.byKey(const Key('android-update-check'));
      expect(find.byKey(const Key('host-update-badge')), findsOneWidget);
      expect(find.byKey(const Key('client-update-badge')), findsNothing);
      expect(
        find.descendant(
          of: check,
          matching: find.byWidgetPredicate(
            (w) => w is Tooltip && w.message == 'Host 업데이트 2대',
          ),
        ),
        findsOneWidget,
      );
      await tester.tap(check);
      await tester.pump(const Duration(milliseconds: 300));
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 100)),
      );
      await tester.pumpAndSettle();
      expect(find.text('최신'), findsOneWidget);
      expect(find.byKey(const Key('host-update-offer')), findsOneWidget);
      expect(find.text('Host 업데이트 2대 가능'), findsOneWidget);
      expect(find.byKey(const Key('host-update-row-pve-101')), findsOneWidget);
      expect(find.byKey(const Key('host-update-row-pve-102')), findsNothing);
      expect(find.text('로컬 PC · 0.2.130 → 0.2.140'), findsOneWidget);
      // A row opens that desktop's own Host update question in place of
      // the panel; nothing is requested yet.
      await tester.tap(find.byKey(const Key('open-host-update-pc-gaming')));
      for (var i = 0; i < 4; i++) {
        await tester.pump(const Duration(milliseconds: 100));
      }
      expect(find.byKey(const Key('android-update-panel')), findsNothing);
      final question = find.byKey(const Key('android-host-update-dialog'));
      expect(question, findsOneWidget);
      expect(
        find.descendant(of: question, matching: find.text('PC-GAMING')),
        findsOneWidget,
      );
      expect(
        find.descendant(
          of: question,
          matching: find.text(
            '로컬 PC의 Host 프로그램만 업데이트해요.\n설치 중에는 잠시 연결할 수 없어요.',
          ),
        ),
        findsOneWidget,
      );
      expect(broker.requests, isEmpty);
      await tester.tap(find.text('취소'));
      await tester.pumpAndSettle();

      // The list push clears the badge once nothing is installable.
      broker.inventory = [desktop('pve-101', offer: false)];
      await tester.tap(find.byKey(const Key('refresh-desktops')));
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('host-update-badge')), findsNothing);
      expect(find.byKey(const Key('android-update-check')), findsOneWidget);
    });

    testWidgets('a phone says the count in the icon button tooltip', (
      tester,
    ) async {
      await open(
        tester,
        _HostBroker([desktop('pve-101')]),
        dp: const Size(360, 800),
      );
      expect(find.byKey(const Key('host-update-badge')), findsOneWidget);
      expect(
        find.byWidgetPredicate(
          (w) => w is Tooltip && w.message == 'Host 업데이트 1대',
        ),
        findsOneWidget,
      );
      expect(find.text('Host 업데이트 1대'), findsNothing);
    });

    testWidgets('a broker without Host updates badges nothing', (tester) async {
      await open(tester, FakeBroker([desktop('pve-101')]));
      expect(find.byKey(const Key('host-update-badge')), findsNothing);
      expect(find.text('Host 업데이트 1대'), findsNothing);
    });
  });
}

class _HostBroker extends FakeBroker implements HostUpdateBrokerApi {
  _HostBroker(super.inventory);
  final requests = <String>[];

  @override
  Future<HostUpdateState> requestHostUpdate(
    String token,
    String desktopId,
    String version, {
    bool disconnectSessions = false,
  }) async {
    requests.add(desktopId);
    throw const BrokerException('network_error');
  }
}
