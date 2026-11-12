import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/product_updates.dart';
import 'package:multitap_client/data/client_build_version.dart';
import 'package:multitap_client/data/product_log.dart';
import 'package:multitap_client/ui/product_update_dialog.dart';
import 'package:multitap_client/ui/update_messages.dart';

class Transport implements UpdateTransport {
  Transport(this.reply);
  final UpdateResponse Function(Uri) reply;
  final calls = <Uri>[];
  bool closed = false;
  @override
  Future<UpdateResponse> get(Uri url) async {
    calls.add(url);
    return reply(url);
  }

  @override
  void close() {
    closed = true;
  }
}

final executable = <int>[77, 90, ...List.generate(2048, (i) => i % 256)];
// A well-formed release signature. Dart only carries it; the native side
// verifies it against the pinned key
// (apps/windows_host/src/host_update_test.cpp covers that).
final releaseSignature = base64.encode(List.generate(64, (i) => i * 3 % 256));
Map<String, dynamic> entry({
  String product = 'client',
  String version = '0.4.16',
}) {
  final filename =
      'Multitap${product == 'host' ? '-Host' : ''}-$version-windows-x64.exe';
  return {
    'product': product,
    'version': version,
    'filename': filename,
    'size': executable.length,
    'sha256': sha256.convert(executable).toString(),
    'format': 'installer',
    'url': '/downloads/$filename',
    'changelog': '한글 입력 개선\n툴바와 업데이트 확인',
    'releasedAt': '2026-10-23T05:00:00.000Z',
    'signature': releaseSignature,
  };
}

/// A transport that can continue a download (HTTP Range); [ranged] answers
/// those requests.
class RangeTransport extends Transport implements RangeUpdateTransport {
  RangeTransport(super.reply, this.ranged);
  final UpdateResponse Function(Uri, int, String) ranged;
  final ranges = <(int, String)>[];
  @override
  Future<UpdateResponse> getFrom(Uri uri, int from, String entityTag) async {
    calls.add(uri);
    ranges.add((from, entityTag));
    return ranged(uri, from, entityTag);
  }
}

/// [bytes], then the connection fails with [error].
Stream<List<int>> interrupted(List<int> bytes, Object error) async* {
  yield bytes;
  throw error;
}

UpdateResponse partial(int from) => UpdateResponse(
  206,
  executable.length - from,
  Stream.value(executable.sublist(from)),
  contentRange: 'bytes $from-${executable.length - 1}/${executable.length}',
);

UpdateResponse catalog(List<dynamic> entries) {
  final bytes = utf8.encode(jsonEncode({'releases': entries}));
  return UpdateResponse(200, bytes.length, Stream.value(bytes));
}

/// This test's own parent for the `Multitap-update-*` download folders, so a
/// run never leaves fixture installers in the system temporary folder.
late Directory downloads;

ProductUpdates apiFor(
  List<dynamic> entries, {
  UpdateResponse Function(Uri)? download,
}) => ProductUpdates(
  'https://my-own-server.example:18443',
  temporaryRoot: downloads,
  retryDelay: Duration.zero,
  transport: Transport(
    (url) => url.path == '/api/v1/downloads'
        ? catalog(entries)
        : download?.call(url) ??
              UpdateResponse(
                200,
                executable.length,
                Stream.fromIterable([
                  executable.sublist(0, 1000),
                  executable.sublist(1000),
                ]),
              ),
  ),
);

void main() {
  setUp(() {
    downloads = Directory.systemTemp.createTempSync('multitap-update-test-');
  });
  tearDown(() {
    // The code under test discards every download (after use, on failure,
    // or when its panel closes); only then is this root empty.
    final left = downloads.existsSync()
        ? downloads.listSync().map((e) => e.path).toList()
        : const <String>[];
    try {
      downloads.deleteSync(recursive: true);
    } on FileSystemException {
      // A file still held open; the expectation below reports it.
    }
    expect(left, isEmpty, reason: 'A downloaded update was left behind.');
  });

  test('The update badge reads only the catalog and reports only a newer version', () async {
    Transport transport(List<dynamic> entries) => Transport(
      (url) => url.path == '/api/v1/downloads'
          ? catalog(entries)
          : throw StateError('The badge never downloads: $url'),
    );
    const server = 'https://my-own-server.example:18443';
    final newer = transport([
      entry(version: '0.4.9'),
      entry(version: '0.4.16'),
    ]);
    expect(
      await newerProductVersion(server, 'client', '0.4.9', transport: newer),
      '0.4.16',
    );
    expect(newer.calls.map((u) => u.path), ['/api/v1/downloads']);
    expect(newer.closed, isTrue);
    expect(
      await newerProductVersion(
        server,
        'client',
        '0.4.16',
        transport: transport([entry(version: '0.4.16')]),
      ),
      isNull,
    );
    // An unknown running version: no badge.
    expect(
      await newerProductVersion(
        server,
        'client',
        '',
        transport: transport([entry()]),
      ),
      isNull,
    );
    // A catalog that could not be read says nothing about the version: it
    // throws, and the caller keeps the state it had (review 2026-11-12: one
    // failed read turned the amber "새 버전" button back into the plain icon).
    final unavailable = Transport(
      (_) => UpdateResponse(503, 0, const Stream.empty()),
    );
    await expectLater(
      newerProductVersion(server, 'client', '0.4.9', transport: unavailable),
      throwsA(isA<UpdateFailure>()),
    );
    expect(unavailable.closed, isTrue);
    await expectLater(
      newerProductVersion(
        server,
        'client',
        '0.4.9',
        transport: Transport((_) => throw const SocketException('offline')),
      ),
      throwsA(anything),
    );
  });

  test('the open-source 1.0.0 is offered over every 0.x (2026-11-03)', () async {
    Transport transport() => Transport(
      (url) => url.path == '/api/v1/downloads'
          ? catalog([
              entry(version: '0.4.179'),
              entry(version: '1.0.0'),
              entry(version: '0.4.180'),
              entry(product: 'host', version: '0.2.155'),
              entry(product: 'host', version: '1.0.0'),
            ])
          : throw StateError('The badge never downloads: $url'),
    );
    const server = 'https://my-own-server.example:18443';
    for (final (product, installed) in [
      ('client', '0.4.179'),
      ('client', '0.4.180'),
      ('host', '0.2.155'),
    ]) {
      expect(
        await newerProductVersion(server, product, installed, transport: transport()),
        '1.0.0',
        reason: '$product $installed',
      );
    }
    expect(await newerProductVersion(server, 'client', '1.0.0', transport: transport()), isNull);
    final api = apiFor([entry(version: '0.4.179'), entry(version: '1.0.0')]);
    final latest = (await api.check('client', '0.4.179'))!;
    expect(latest.version, '1.0.0');
    expect(latest.filename, 'Multitap-1.0.0-windows-x64.exe');
    expect(compareProductVersions('1.0.0', '0.4.179'), greaterThan(0));
    expect(compareProductVersions('1.0.0', '0.99.999'), greaterThan(0));
    api.close();
  });

  test('numeric versions and configured HTTPS origin, ZIP and other products ignored', () async {
    final transport = Transport(
      (_) => catalog([
        entry(version: '0.4.9'),
        entry(version: '0.4.16'),
        entry(product: 'host', version: '9.0.0'),
        {...entry(version: '9.0.0'), 'format': 'archive'},
      ]),
    );
    final api = ProductUpdates(
      'https://my-own-server.example:18443',
      transport: transport,
    );
    final latest = await api.check('client', '0.4.15');
    expect(latest!.version, '0.4.16');
    expect(latest.changelog, contains('한글'));
    expect(latest.url.origin, 'https://my-own-server.example:18443');
    expect(
      transport.calls.single.toString(),
      'https://my-own-server.example:18443/api/v1/downloads',
    );
    expect(compareProductVersions('1.10.0', '1.9.99'), greaterThan(0));
    api.close();
    expect(transport.closed, isTrue);
  });
  test('empty catalog is distinct from a network failure', () async {
    final api = apiFor([]);
    expect(await api.check('host', '0.2.15'), isNull);
    api.close();
  });
  test('old published entries without notes remain supported', () async {
    final old = entry()
      ..remove('changelog')
      ..remove('releasedAt');
    final api = apiFor([old]);
    final release = await api.check('client', '0.4.15');
    expect(release!.changelog, isEmpty);
    expect(release.releasedAt, isNull);
    api.close();
  });
  final bad = <String, Map<String, dynamic>>{
    'external URL': {'url': 'https://other.example/installer.exe'},
    'relative traversal': {'url': '/downloads/../installer.exe'},
    'wrong product filename': {
      'filename': 'Multitap-Host-0.4.16-windows-x64.exe',
    },
    'too large': {'size': 128 * 1024 * 1024 + 1},
    'zero': {'size': 0},
    'bad digest': {'sha256': 'f' * 63},
    'HTML disguised URL': {'url': '/login'},
    'note control': {'changelog': 'hello\u0000world'},
    'unpaired Unicode': {'changelog': String.fromCharCode(0xd800)},
    'oversized UTF8 notes': {'changelog': '한' * 3000},
    'invalid date': {'releasedAt': '2026-02-31T00:00:00.000Z'},
    'non UTC date': {'releasedAt': '2026-10-23T14:00:00+09:00'},
  };
  for (final item in bad.entries) {
    test('reject malformed catalog: ${item.key}', () async {
      final api = apiFor([
        {...entry(), ...item.value},
      ]);
      await expectLater(
        api.check('client', '0.4.15'),
        throwsA(isA<UpdateFailure>()),
      );
      api.close();
    });
  }
  test(
    'duplicate numeric versions and oversized/redirect catalogs are rejected',
    () async {
      final duplicate = apiFor([entry(), entry()]);
      await expectLater(
        duplicate.check('client', '0.4.15'),
        throwsA(isA<UpdateFailure>()),
      );
      duplicate.close();
      for (final response in [
        const UpdateResponse(302, 0, Stream.empty()),
        const UpdateResponse(200, 512 * 1024 + 1, Stream.empty()),
        UpdateResponse(200, -1, Stream.value(List.filled(512 * 1024 + 1, 32))),
      ]) {
        final api = ProductUpdates(
          'https://server.example',
          transport: Transport((_) => response),
        );
        await expectLater(
          api.check('client', '0.4.15'),
          throwsA(isA<UpdateFailure>()),
        );
        api.close();
      }
      expect(
        () => ProductUpdates('http://server.example'),
        throwsFormatException,
      );
    },
  );
  test(
    'real temporary download verifies exact bytes and SHA with actual progress',
    () async {
      final api = apiFor([entry()]);
      final release = (await api.check('client', '0.4.15'))!;
      final progress = <int>[];
      final downloaded = await api.download(release, (n, _) => progress.add(n));
      // Its own Multitap-update-* folder, under the injected root.
      expect(downloaded.directory.parent.path, downloads.path);
      expect(
        downloaded.directory.path.split(Platform.pathSeparator).last,
        startsWith('Multitap-update-'),
      );
      expect(await downloaded.file.readAsBytes(), executable);
      expect(progress, [1000, executable.length]);
      await downloaded.discard();
      expect(await downloaded.directory.exists(), isFalse);
      api.close();
    },
  );
  for (final (failure, code) in [
    ('hash', 'hash_mismatch'),
    ('truncated', 'download_network'),
    ('oversized', 'download_size_mismatch'),
    ('redirect', 'download_http_status'),
    ('length', 'download_size_mismatch'),
  ]) {
    test('download rejects $failure before installation ($code)', () async {
      final api = apiFor(
        [entry()],
        download: (_) => switch (failure) {
          'hash' => UpdateResponse(
            200,
            executable.length,
            Stream.value(List.filled(executable.length, 0)),
          ),
          'truncated' => const UpdateResponse(200, -1, Stream.empty()),
          'oversized' => UpdateResponse(
            200,
            -1,
            Stream.value([...executable, 1]),
          ),
          'redirect' => const UpdateResponse(302, 0, Stream.empty()),
          _ => const UpdateResponse(200, 1, Stream.empty()),
        },
      );
      final release = (await api.check('client', '0.4.15'))!;
      await expectLater(
        api.download(release, (_, _) {}),
        throwsA(isA<UpdateFailure>().having((f) => f.code, 'code', code)),
      );
      api.close();
    });
  }
  test(
    'closing during a download cancels without returning an installer',
    () async {
      final api = apiFor([entry()]);
      final release = (await api.check('client', '0.4.15'))!;
      await expectLater(
        api.download(release, (_, _) => api.close()),
        throwsA(
          isA<UpdateFailure>().having((f) => f.code, 'code', 'cancelled'),
        ),
      );
    },
  );

  test(
    'an interrupted download continues from its last byte and is verified whole',
    () async {
      final transport = RangeTransport(
        (url) => url.path == '/api/v1/downloads'
            ? catalog([entry()])
            : UpdateResponse(
                200,
                executable.length,
                interrupted(
                  executable.sublist(0, 1000),
                  const SocketException('connection reset'),
                ),
              ),
        (_, from, _) => partial(from),
      );
      final api = ProductUpdates(
        'https://my-own-server.example:18443',
        transport: transport,
        temporaryRoot: downloads,
        retryDelay: Duration.zero,
      );
      final release = (await api.check('client', '0.4.15'))!;
      final progress = <int>[], attempts = <int>[];
      final downloaded = await api.download(
        release,
        (n, _) => progress.add(n),
        retrying: attempts.add,
      );
      expect(await downloaded.file.readAsBytes(), executable);
      // Only the missing bytes, and only while the server still has the
      // release's file (its SHA-256 entity tag).
      expect(transport.ranges, [
        (1000, '"sha256-${sha256.convert(executable)}"'),
      ]);
      expect(attempts, [2]);
      expect(progress, [1000, executable.length]);
      await downloaded.discard();
      api.close();
    },
  );

  test('a server that does not continue sends the file again from the start', () async {
    for (final ranges in [true, false]) {
      var requests = 0;
      UpdateResponse whole(Uri url) {
        if (url.path == '/api/v1/downloads') return catalog([entry()]);
        return UpdateResponse(
          200,
          executable.length,
          requests++ == 0
              ? interrupted(
                  executable.sublist(0, 1000),
                  const HttpException('Connection closed while receiving data'),
                )
              : Stream.value(executable),
        );
      }

      final transport = ranges
          ? RangeTransport(whole, (url, _, _) => whole(url))
          : Transport(whole);
      final api = ProductUpdates(
        'https://my-own-server.example:18443',
        transport: transport,
        temporaryRoot: downloads,
        retryDelay: Duration.zero,
      );
      final release = (await api.check('client', '0.4.15'))!;
      final downloaded = await api.download(release, (_, _) {});
      expect(await downloaded.file.readAsBytes(), executable, reason: '$ranges');
      expect(requests, 2);
      await downloaded.discard();
      api.close();
    }
  });

  test(
    'retries are bounded and a continued file passes the same strict checks',
    () async {
      Future<(String, int)> outcome(
        UpdateResponse Function(Uri) first, {
        UpdateResponse Function(Uri, int, String)? ranged,
      }) async {
        var requests = 0;
        final transport = RangeTransport(
          (url) {
            if (url.path == '/api/v1/downloads') return catalog([entry()]);
            requests++;
            return first(url);
          },
          (url, from, tag) {
            requests++;
            return (ranged ?? (url, _, _) => first(url))(url, from, tag);
          },
        );
        final api = ProductUpdates(
          'https://my-own-server.example:18443',
          transport: transport,
          temporaryRoot: downloads,
          retryDelay: Duration.zero,
        );
        final release = (await api.check('client', '0.4.15'))!;
        try {
          await api.download(release, (_, _) {});
          fail('The download must not succeed.');
        } on UpdateFailure catch (failure) {
          return (failure.code, requests);
        } finally {
          api.close();
        }
      }

      UpdateResponse broken(Uri _) => UpdateResponse(
        200,
        executable.length,
        interrupted(executable.sublist(0, 1000), const SocketException('x')),
      );
      // Network, a stall and a busy or restarting server: four requests.
      expect(
        await outcome((_) => throw const SocketException('refused')),
        ('download_network', 4),
      );
      expect(
        await outcome((_) => throw TimeoutException('no reply')),
        ('download_timeout', 4),
      );
      expect(
        await outcome((_) => const UpdateResponse(502, 0, Stream.empty())),
        ('download_server_error', 4),
      );
      // A file the server no longer has, or a refusal: no repeat.
      expect(
        await outcome((_) => const UpdateResponse(404, 0, Stream.empty())),
        ('download_not_found', 1),
      );
      expect(
        await outcome((_) => const UpdateResponse(403, 0, Stream.empty())),
        ('download_http_status', 1),
      );
      // A continued range other than the missing bytes is refused.
      expect(
        await outcome(
          broken,
          ranged: (_, from, _) => UpdateResponse(
            206,
            executable.length - from,
            Stream.value(executable.sublist(from)),
            contentRange: 'bytes 0-${executable.length - 1}/${executable.length}',
          ),
        ),
        ('download_size_mismatch', 2),
      );
      // Bytes that differ after continuing still fail SHA-256.
      expect(
        await outcome(
          broken,
          ranged: (_, from, _) => UpdateResponse(
            206,
            executable.length - from,
            Stream.value(List.filled(executable.length - from, 0)),
            contentRange:
                'bytes $from-${executable.length - 1}/${executable.length}',
          ),
        ),
        ('hash_mismatch', 2),
      );
      // Disk: a full disk while writing, and a temporary folder that
      // cannot be created.
      expect(
        await outcome(
          (_) => throw const FileSystemException(
            'write',
            'update.exe',
            OSError('There is not enough space on the disk.', 112),
          ),
        ),
        ('download_disk_full', 1),
      );
      final api = ProductUpdates(
        'https://my-own-server.example:18443',
        transport: Transport((_) => catalog([entry()])),
        temporaryRoot: Directory('${downloads.path}/missing/folder'),
      );
      final release = (await api.check('client', '0.4.15'))!;
      await expectLater(
        api.download(release, (_, _) {}),
        throwsA(
          isA<UpdateFailure>().having(
            (f) => f.code,
            'code',
            'download_file_error',
          ),
        ),
      );
      api.close();
    },
  );

  test('the remaining time follows the bytes received so far', () {
    const mb = 1024 * 1024;
    expect(
      remainingDownloadText(0, 8 * mb, const Duration(seconds: 5)),
      '남은 시간 계산 중',
    );
    expect(
      remainingDownloadText(mb, 8 * mb, const Duration(milliseconds: 500)),
      '남은 시간 계산 중',
    );
    expect(
      remainingDownloadText(mb, 8 * mb, const Duration(seconds: 2)),
      '약 14초 남음',
    );
    expect(
      remainingDownloadText(mb, 100 * mb, const Duration(seconds: 10)),
      '약 17분 남음',
    );
    expect(
      remainingDownloadText(8 * mb, 8 * mb, const Duration(seconds: 3)),
      '곧 끝나요',
    );
  });
  test('public release metadata is revalidated before filesystem or network access', () async {
    final valid = ProductRelease.parse(
      entry(),
      Uri.parse('https://my-own-server.example:18443'),
      'client',
    )!;
    final cases = <Map<String, Object>>[
      {'product': 'other'},
      {'version': '0.4.16/escape'},
      {
        'filename': '../escaped.exe',
        'url': Uri.parse(
          'https://my-own-server.example:18443/downloads/../escaped.exe',
        ),
      },
      {'filename': 'Multitap-Host-0.4.16-windows-x64.exe'},
      {'size': 0},
      {'size': 128 * 1024 * 1024 + 1},
      {'sha256': 'f' * 63},
      {'changelog': 'bad\u0000notes'},
      {
        'url': Uri.parse(
          'https://user@my-own-server.example:18443/downloads/${valid.filename}',
        ),
      },
      {'url': Uri.parse('https://other.example/downloads/${valid.filename}')},
      {'url': Uri.parse('/downloads/${valid.filename}')},
      {
        'url': Uri.parse(
          'http://my-own-server.example:18443/downloads/${valid.filename}',
        ),
      },
      {'url': Uri.parse('${valid.url}?secret=fixture')},
      {'url': Uri.parse('${valid.url}#fragment')},
    ];
    for (final value in cases) {
      final transport = Transport(
        (_) => throw StateError('Network must not be used.'),
      );
      final api = ProductUpdates(
        'https://my-own-server.example:18443',
        transport: transport,
      );
      final release = ProductRelease(
        product: value['product'] as String? ?? valid.product,
        version: value['version'] as String? ?? valid.version,
        filename: value['filename'] as String? ?? valid.filename,
        size: value['size'] as int? ?? valid.size,
        sha256: value['sha256'] as String? ?? valid.sha256,
        url: value['url'] as Uri? ?? valid.url,
        changelog: value['changelog'] as String? ?? valid.changelog,
        signature: valid.signature,
      );
      var filesystemReads = 0;
      await IOOverrides.runZoned(
        () async => expectLater(
          api.download(release, (_, _) {}),
          throwsA(isA<UpdateFailure>()),
        ),
        getSystemTempDirectory: () {
          filesystemReads++;
          throw StateError('Filesystem must not be used.');
        },
      );
      expect(transport.calls, isEmpty, reason: value.toString());
      expect(filesystemReads, 0, reason: value.toString());
      api.close();
    }
  });
  Future<void> pumpRealUntil(
    WidgetTester tester,
    bool Function() complete,
  ) async {
    // These widgets exercise the real Stream.timeout and temporary-file IO
    // paths. Their completion must run outside the widget fake clock; settling
    // an indeterminate progress animation first can never reach that IO.
    for (var i = 0; i < 200; i++) {
      await tester.pump();
      if (complete()) {
        await tester.pump(const Duration(milliseconds: 300));
        return;
      }
      await tester.runAsync(
        () async => Future<void>.delayed(const Duration(milliseconds: 10)),
      );
    }
    fail(
      'Update operation did not reach its expected UI state within 2 seconds of real async work.',
    );
  }

  /// Unmounts the panel and lets its real file IO finish: closing it
  /// discards its download (dispose), work the widget clock alone never
  /// completes, so it would otherwise stay in the temporary folder.
  Future<void> discardDownloads(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox());
    await pumpRealUntil(tester, () => downloads.listSync().isEmpty);
  }

  String title(WidgetTester tester) =>
      tester.widget<Text>(find.byKey(const Key('update-title'))).data!;

  Future<void> open(
    WidgetTester tester,
    ProductUpdates api, {
    String current = '0.4.15',
    String product = 'client',
    bool manual = false,
    FakeUpdatePlatform? platform,
  }) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Builder(
          builder: (context) => TextButton(
            onPressed: () => showDialog<void>(
              context: context,
              builder: (_) => ProductUpdateDialog(
                server: 'https://my-own-server.example:18443',
                product: product,
                currentVersion: current,
                api: api,
                manual: manual,
                platform: platform ?? FakeUpdatePlatform(),
              ),
            ),
            child: const Text('open'),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await pumpRealUntil(
      tester,
      () =>
          find.byType(ProductUpdateDialog).evaluate().isNotEmpty &&
          find.byType(LinearProgressIndicator).evaluate().isEmpty,
    );
  }

  /// available → download → verify → ready, with the real temporary file.
  Future<void> toReady(WidgetTester tester) async {
    await tester.tap(find.byKey(const Key('install-product-update')));
    await pumpRealUntil(
      tester,
      () => find.byKey(const Key('apply-product-update')).evaluate().isNotEmpty,
    );
  }

  testWidgets(
    'available: version step and notes; one action downloads, verifies, then one restart hands off',
    (tester) async {
      final platform = FakeUpdatePlatform();
      await open(tester, apiFor([entry()]), platform: platform);
      expect(title(tester), '새 버전이 있어요');
      expect(find.text('Multitap · 현재 0.4.15'), findsOneWidget);
      expect(find.text('0.4.16'), findsOneWidget);
      expect(find.textContaining('한글 입력 개선'), findsOneWidget);
      expect(find.text('업데이트 받기'), findsOneWidget);
      expect(platform.calls, isEmpty);

      await toReady(tester);
      expect(title(tester), '업데이트 준비 완료');
      expect(find.byKey(const Key('update-verified')), findsOneWidget);
      expect(
        find.text(
          '크기 ${executable.length.toString().replaceAllMapped(RegExp(r'\B(?=(\d{3})+(?!\d))'), (_) => ',')}바이트 일치',
        ),
        findsOneWidget,
      );
      expect(find.text('설치 파일 버전 0.4.16 확인'), findsOneWidget);
      // The native verification step also checked the release signature.
      expect(find.text('Multitap 배포 서명 확인'), findsOneWidget);
      expect(find.textContaining('앱이 닫혔다가 같은 자리에서 새 버전으로 열려요.'), findsOneWidget);
      expect(find.text('다시 시작해서 업데이트'), findsOneWidget);
      // The installer EXE version was checked before the restart was offered.
      expect(platform.calls, ['verify']);
      expect(
        await tester.runAsync(() => platform.verified!.file.readAsBytes()),
        executable,
      );

      await tester.tap(find.byKey(const Key('apply-product-update')));
      await pumpRealUntil(tester, () => platform.calls.length == 3);
      expect(platform.calls, ['verify', 'sessions', 'install:apply']);
      expect(platform.currentVersion, '0.4.15');
      expect(
        await tester.runAsync(() => platform.installed!.file.readAsBytes()),
        executable,
      );
      // The installer accepted: this window only shows that it is closing.
      expect(title(tester), '업데이트 적용 중');
      expect(find.byKey(const Key('update-applying')), findsOneWidget);
      // Nothing can be pressed again or closed while the installer takes over.
      expect(find.byKey(const Key('apply-product-update')), findsNothing);
      expect(find.byKey(const Key('close-product-update')), findsNothing);
      await discardDownloads(tester);
    },
  );

  testWidgets(
    'remote windows: one confirmation closes them before the handoff',
    (tester) async {
      final platform = FakeUpdatePlatform(sessions: true);
      await open(tester, apiFor([entry()]), platform: platform);
      await toReady(tester);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('연결을 끊고 업데이트할까요?'), findsOneWidget);
      await tester.tap(find.text('취소'));
      await tester.pump(const Duration(milliseconds: 300));
      expect(platform.calls, ['verify', 'sessions']);
      expect(title(tester), '업데이트 준비 완료');

      await tester.tap(find.byKey(const Key('apply-product-update')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const Key('confirm-update-sessions')));
      await pumpRealUntil(tester, () => platform.calls.last == 'install:apply');
      expect(platform.calls, [
        'verify',
        'sessions',
        'sessions',
        'close-sessions',
        'install:apply',
      ]);
      await discardDownloads(tester);
    },
  );

  testWidgets('the restart action runs once however often it is pressed', (
    tester,
  ) async {
    final hold = Completer<void>();
    final platform = FakeUpdatePlatform(hold: hold);
    await open(tester, apiFor([entry()]), platform: platform);
    await toReady(tester);
    final apply = tester.widget<FilledButton>(
      find.byKey(const Key('apply-product-update')),
    );
    apply.onPressed!();
    apply.onPressed!();
    await pumpRealUntil(tester, () => platform.calls.contains('install:apply'));
    apply.onPressed!();
    await tester.pump();
    expect(platform.calls.where((c) => c.startsWith('install')).length, 1);
    hold.complete();
    await tester.pump(const Duration(milliseconds: 300));
    await discardDownloads(tester);
  });

  testWidgets(
    'Host: settings closing and service restart are stated before the restart',
    (tester) async {
      final platform = FakeUpdatePlatform(sessions: true);
      await open(
        tester,
        apiFor([entry(product: 'host', version: '0.2.50')]),
        current: '0.2.49',
        product: 'host',
        platform: platform,
      );
      expect(find.text('Multitap Host · 현재 0.2.49'), findsOneWidget);
      await toReady(tester);
      expect(find.textContaining('Host 설정이 닫히고 연결이 끊긴 뒤 Host 서비스가 다시 시작돼요.'), findsOneWidget);
      expect(find.textContaining('Windows는 자동으로 다시 시작하지 않아요.'), findsOneWidget);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await pumpRealUntil(tester, () => platform.calls.last == 'install:apply');
      // Host settings has no remote windows of its own to close.
      expect(platform.calls, ['verify', 'install:apply']);
      expect(platform.currentVersion, '0.2.49');
      await discardDownloads(tester);
    },
  );

  testWidgets(
    'installer failure keeps this window open with its code and next actions',
    (tester) async {
      final platform = FakeUpdatePlatform(installFailure: 'install_failed');
      await open(tester, apiFor([entry()]), platform: platform);
      await toReady(tester);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
      );
      expect(title(tester), '업데이트하지 못했어요');
      expect(find.text('코드 install_failed'), findsOneWidget);
      expect(find.textContaining('어디서 멈췄는지, 복구됐는지는 확인하지 못했어요.'), findsOneWidget);
      expect(find.textContaining('설치된 앱은 그대로예요'), findsNothing);
      expect(find.byKey(const Key('retry-product-update')), findsOneWidget);
      // The same verified file can go to the ordinary installer window.
      platform.installFailure = null;
      await tester.tap(find.byKey(const Key('manual-product-update')));
      await pumpRealUntil(tester, () => title(tester) == '설치 프로그램을 열었어요');
      expect(platform.calls.last, 'install:manual');
      expect(
        await tester.runAsync(() => platform.installed!.file.readAsBytes()),
        executable,
      );
      await discardDownloads(tester);
    },
  );

  // Review 2026-11-12: 다시 시도 after a failed "설치 프로그램으로 직접 설치"
  // went back to the in-place restart.
  testWidgets(
    '다시 시도 repeats what failed: the installer window, not the in-place restart',
    (tester) async {
      final platform = FakeUpdatePlatform(installFailure: 'install_failed');
      await open(tester, apiFor([entry()]), platform: platform);
      await toReady(tester);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
      );
      await tester.tap(find.byKey(const Key('manual-product-update')));
      await pumpRealUntil(
        tester,
        () =>
            platform.calls.last == 'install:manual' &&
            find.byKey(const Key('retry-product-update')).evaluate().isNotEmpty,
      );
      platform.installFailure = null;
      await tester.tap(find.byKey(const Key('retry-product-update')));
      await pumpRealUntil(tester, () => title(tester) == '설치 프로그램을 열었어요');
      expect(platform.calls.where((call) => call.startsWith('install:')), [
        'install:apply',
        'install:manual',
        'install:manual',
      ]);
      await discardDownloads(tester);
    },
  );

  // Review 2026-11-12: "약 N초 남음" stood still while no bytes arrived.
  testWidgets('a download that stalls does not keep showing its last estimate', (
    tester,
  ) async {
    final body = StreamController<List<int>>();
    await open(
      tester,
      apiFor(
        [entry()],
        download: (_) => UpdateResponse(200, executable.length, body.stream),
      ),
    );
    String remaining() => tester
        .widget<Text>(find.byKey(const Key('update-download-remaining')))
        .data!;
    await tester.tap(find.byKey(const Key('install-product-update')));
    body.add(executable.sublist(0, 500));
    await pumpRealUntil(
      tester,
      () =>
          find
              .byKey(const Key('update-download-remaining'))
              .evaluate()
              .isNotEmpty,
    );
    // An estimate needs a second of the download's own clock.
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 1100)),
    );
    body.add(executable.sublist(500, 1000));
    await pumpRealUntil(tester, () => remaining() != '남은 시간 계산 중');
    expect(remaining(), anyOf(startsWith('약 '), '곧 끝나요'));
    // Three seconds without a byte.
    await tester.pump(const Duration(seconds: 3));
    await tester.pump();
    expect(remaining(), '남은 시간 계산 중');
    body.add(executable.sublist(1000));
    unawaited(body.close());
    await pumpRealUntil(
      tester,
      () => find.byKey(const Key('apply-product-update')).evaluate().isNotEmpty,
    );
    await discardDownloads(tester);
  });

  testWidgets('a Host decision needing a person continues in the installer', (
    tester,
  ) async {
    final platform = FakeUpdatePlatform(
      installFailure: 'driver_install_required',
    );
    await open(
      tester,
      apiFor([entry(product: 'host', version: '0.2.50')]),
      current: '0.2.49',
      product: 'host',
      platform: platform,
    );
    await toReady(tester);
    await tester.tap(find.byKey(const Key('apply-product-update')));
    await pumpRealUntil(
      tester,
      () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
    );
    expect(title(tester), '직접 확인이 필요해요');
    expect(find.textContaining('가상 화면 드라이버 설치나 변경이 필요해요'), findsOneWidget);
    expect(find.text('코드 driver_install_required'), findsOneWidget);
    expect(find.byKey(const Key('retry-product-update')), findsNothing);
    platform.installFailure = null;
    await tester.tap(find.byKey(const Key('manual-product-update')));
    await pumpRealUntil(tester, () => platform.calls.last == 'install:manual');
    expect(platform.calls, ['verify', 'install:apply', 'install:manual']);
    await discardDownloads(tester);
  });

  testWidgets(
    'manual mode ends with the installer window instead of a restart',
    (tester) async {
      final platform = FakeUpdatePlatform();
      await open(tester, apiFor([entry()]), manual: true, platform: platform);
      await toReady(tester);
      expect(find.text('설치 프로그램 열기'), findsOneWidget);
      expect(find.text('다시 시작해서 업데이트'), findsNothing);
      await tester.tap(find.byKey(const Key('apply-product-update')));
      await pumpRealUntil(tester, () => title(tester) == '설치 프로그램을 열었어요');
      expect(platform.calls.last, 'install:manual');
      await tester.tap(find.byKey(const Key('close-product-update')));
      await tester.pumpAndSettle();
      expect(find.byType(ProductUpdateDialog), findsNothing);
      // Closing the panel discards the verified installer.
      await discardDownloads(tester);
    },
  );

  testWidgets(
    'an installer EXE of another version is never offered for restart',
    (tester) async {
      final platform = FakeUpdatePlatform(
        verifyFailure: 'package_version_mismatch',
      );
      await open(tester, apiFor([entry()]), platform: platform);
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
      );
      expect(find.text('코드 package_version_mismatch'), findsOneWidget);
      expect(find.textContaining('파일 버전이 배포 버전(0.4.16)과 달라 설치하지 않았어요.'), findsOneWidget);
      expect(find.byKey(const Key('apply-product-update')), findsNothing);
      expect(platform.calls, ['verify']);
      // A rejected installer is discarded at once, panel still open.
      expect(downloads.listSync(), isEmpty);
      await discardDownloads(tester);
    },
  );

  testWidgets('current or newer local version never offers a downgrade', (
    tester,
  ) async {
    await open(tester, apiFor([entry()]), current: '0.4.17');
    expect(find.byKey(const Key('install-product-update')), findsNothing);
    expect(title(tester), '최신 버전이에요');
    expect(find.text('최신'), findsOneWidget);
    await tester.tap(find.text('닫기'));
    await tester.pump(const Duration(milliseconds: 300));
  });

  testWidgets(
    'the running EXE version prevents the same update being offered again',
    (tester) async {
      const versionChannel = MethodChannel('multitap/window');
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(
        versionChannel,
        (call) async => '0.4.25+39',
      );
      addTearDown(
        () => messenger.setMockMethodCallHandler(versionChannel, null),
      );
      final running = await ClientBuildVersion.load();
      await open(
        tester,
        apiFor([entry(version: '0.4.25')]),
        current: running.version!,
      );
      expect(find.text('Multitap · 현재 0.4.25'), findsOneWidget);
      expect(find.byKey(const Key('install-product-update')), findsNothing);
      expect(title(tester), '최신 버전이에요');
      await tester.tap(find.text('닫기'));
      await tester.pump(const Duration(milliseconds: 300));
    },
  );

  testWidgets('unknown running version does not download or blame the server', (
    tester,
  ) async {
    final api = apiFor([entry(version: '0.4.25')]);
    await open(tester, api, current: '');
    expect(find.textContaining('실행 중인 버전을 확인하지 못했어요'), findsOneWidget);
    expect(find.text('코드 version_unknown'), findsOneWidget);
    expect(find.byKey(const Key('install-product-update')), findsNothing);
    await tester.tap(find.text('닫기'));
    await tester.pump(const Duration(milliseconds: 300));
  });

  testWidgets(
    'corrupt download shows failure, never verifies or launches, and can retry',
    (tester) async {
      final platform = FakeUpdatePlatform();
      await open(
        tester,
        apiFor(
          [entry()],
          download: (_) => UpdateResponse(
            200,
            executable.length,
            Stream.value(List.filled(executable.length, 0)),
          ),
        ),
        platform: platform,
      );
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(
        tester,
        () => find.text('코드 hash_mismatch').evaluate().isNotEmpty,
      );
      expect(find.text('코드 hash_mismatch'), findsOneWidget);
      expect(find.textContaining('받은 파일이 배포 정보의 크기나 SHA-256과 달라 설치하지 않았어요.'), findsOneWidget);
      expect(platform.calls, isEmpty);
      expect(find.byKey(const Key('retry-product-update')), findsOneWidget);
      expect(find.byKey(const Key('manual-product-update')), findsNothing);
      // The corrupt file is already gone.
      expect(downloads.listSync(), isEmpty);
      await tester.tap(find.text('닫기'));
      await tester.pump(const Duration(milliseconds: 300));
      await discardDownloads(tester);
    },
  );

  testWidgets(
    'download reports real progress and cancellation never launches',
    (tester) async {
      final chunks = StreamController<List<int>>();
      var requested = false;
      final platform = FakeUpdatePlatform();
      final api = apiFor(
        [entry()],
        download: (_) {
          requested = true;
          return UpdateResponse(200, executable.length, chunks.stream);
        },
      );
      await open(tester, api, platform: platform);
      await tester.tap(find.byKey(const Key('install-product-update')));
      for (var i = 0; i < 100 && !requested; i++) {
        await tester.pump();
        await tester.runAsync(
          () async => Future<void>.delayed(const Duration(milliseconds: 10)),
        );
      }
      expect(requested, isTrue);
      expect(title(tester), '업데이트 받는 중');
      // Only cancelling remains while the bytes arrive.
      expect(find.byKey(const Key('install-product-update')), findsNothing);
      expect(find.text('취소'), findsOneWidget);
      chunks.add(executable.sublist(0, 1000));
      for (var i = 0; i < 100; i++) {
        await tester.pump();
        final indicators = tester.widgetList<LinearProgressIndicator>(
          find.byType(LinearProgressIndicator),
        );
        if (indicators.length == 1 &&
            indicators.single.value == 1000 / executable.length) {
          break;
        }
        await tester.runAsync(
          () async => Future<void>.delayed(const Duration(milliseconds: 10)),
        );
      }
      expect(
        tester
            .widget<LinearProgressIndicator>(
              find.byType(LinearProgressIndicator),
            )
            .value,
        1000 / executable.length,
      );
      expect(
        find.text('${(1000 / executable.length * 100).floor()}%'),
        findsOneWidget,
      );
      expect(platform.calls, isEmpty);
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const Key('close-product-update')));
      await tester.pumpAndSettle();
      await tester.runAsync(() async {
        await chunks.close();
      });
      await tester.pump();
      expect(find.byType(ProductUpdateDialog), findsNothing);
      expect(platform.calls, isEmpty);
      // The cancelled partial file is discarded.
      await discardDownloads(tester);
      expect(tester.takeException(), isNull);
    },
  );

  /// The updateDownload records this test produces (the panel writes to the
  /// shared ProductLog, which forwards to client diagnostics).
  List<ProductLogEntry> downloadRecords() {
    final records = <ProductLogEntry>[];
    final subscription = ProductLog.instance.localRecords.listen((entry) {
      if (entry.event == LogEvent.updateDownload) records.add(entry);
    });
    addTearDown(subscription.cancel);
    return records;
  }

  testWidgets(
    'a failed download records its fixed code, attempts and bytes',
    (tester) async {
      final records = downloadRecords();
      await open(
        tester,
        apiFor(
          [entry()],
          download: (_) => const UpdateResponse(404, 0, Stream.empty()),
        ),
      );
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
      );
      expect(find.text('코드 download_not_found'), findsOneWidget);
      expect(find.textContaining('서버에 이 버전의 파일이 없어요.'), findsOneWidget);
      expect(records, hasLength(1));
      expect(records.single.code, 'download_not_found');
      expect(records.single.values, containsPair('ok', false));
      expect(records.single.values, containsPair('attempts', 1));
      expect(records.single.values, containsPair('receivedKib', 0));
      expect(records.single.values, containsPair('latest', 4016));
      expect(records.single.values['elapsedMs'], isA<int>());
      await discardDownloads(tester);
    },
  );

  testWidgets(
    'an interrupted download says it continues, then completes verified',
    (tester) async {
      final records = downloadRecords();
      final rest = StreamController<List<int>>();
      final transport = RangeTransport(
        (url) => url.path == '/api/v1/downloads'
            ? catalog([entry()])
            : UpdateResponse(
                200,
                executable.length,
                interrupted(
                  executable.sublist(0, 1000),
                  const SocketException('connection reset'),
                ),
              ),
        (_, from, _) => UpdateResponse(
          206,
          executable.length - from,
          rest.stream,
          contentRange:
              'bytes $from-${executable.length - 1}/${executable.length}',
        ),
      );
      final platform = FakeUpdatePlatform();
      await open(
        tester,
        ProductUpdates(
          'https://my-own-server.example:18443',
          transport: transport,
          temporaryRoot: downloads,
          retryDelay: Duration.zero,
        ),
        platform: platform,
      );
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(tester, () => transport.ranges.isNotEmpty);
      expect(title(tester), '업데이트 받는 중');
      expect(
        tester
            .widget<Text>(find.byKey(const Key('update-download-remaining')))
            .data,
        '네트워크가 끊겨 받은 부분부터 이어받고 있어요.',
      );
      rest.add(executable.sublist(1000));
      unawaited(rest.close());
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('apply-product-update')).evaluate().isNotEmpty,
      );
      expect(platform.calls, ['verify']);
      expect(records.single.code, isNull);
      expect(records.single.values, containsPair('ok', true));
      expect(records.single.values, containsPair('attempts', 2));
      await discardDownloads(tester);
    },
  );

  testWidgets(
    'cancelling records no failure, even while the panel is still closing',
    (tester) async {
      final records = downloadRecords();
      final chunks = StreamController<List<int>>();
      // Like HttpClient.close(force: true): closing ends the response with an
      // error at once, before the closing panel is disposed.
      final transport = _ClosingTransport(chunks);
      await open(
        tester,
        ProductUpdates(
          'https://my-own-server.example:18443',
          transport: transport,
          temporaryRoot: downloads,
          retryDelay: Duration.zero,
        ),
      );
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(tester, () => transport.downloading);
      chunks.add(executable.sublist(0, 1000));
      await pumpRealUntil(
        tester,
        () => find.text('${(1000 / executable.length * 100).floor()}%')
            .evaluate()
            .isNotEmpty,
      );
      expect(
        tester
            .widget<Text>(find.byKey(const Key('update-download-remaining')))
            .data,
        isNotEmpty,
      );
      await tester.tap(find.byKey(const Key('close-product-update')));
      // The error and the partial file's removal finish during the panel's
      // closing animation and must not be recorded as a failed download: no
      // frame is drawn here, so the closing panel is not disposed yet.
      for (var i = 0; i < 30; i++) {
        await tester.idle();
        await tester.runAsync(
          () async => Future<void>.delayed(const Duration(milliseconds: 10)),
        );
      }
      expect(records, isEmpty);
      await tester.pumpAndSettle();
      expect(find.byType(ProductUpdateDialog), findsNothing);
      expect(records, isEmpty);
      await discardDownloads(tester);
    },
  );

  test('native platform: fixed failure codes only, mode and current version passed', () async {
    const channel = MethodChannel('multitap/window');
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    final calls = <MethodCall>[];
    var reply = 'ok';
    messenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (reply != 'ok') {
        throw PlatformException(code: reply, message: 'C:\\secret');
      }
      return call.method == 'hasSessionWindows' ? true : {'closing': true};
    });
    addTearDown(() => messenger.setMockMethodCallHandler(channel, null));
    final api = apiFor([entry()]);
    final release = (await api.check('client', '0.4.15'))!;
    final downloaded = await api.download(release, (_, _) {});
    addTearDown(downloaded.discard);
    const platform = NativeUpdatePlatform();
    expect(await platform.hasRemoteSessions(), isTrue);
    await platform.install(
      downloaded,
      UpdateInstallMode.apply,
      currentVersion: '0.4.15',
    );
    final arguments = calls.last.arguments as Map;
    expect(calls.last.method, 'installUpdate');
    expect(arguments['mode'], 'apply');
    expect(arguments['currentVersion'], '0.4.15');
    expect(arguments['sha256'], release.sha256);
    expect(arguments['path'], downloaded.file.path);
    // The native side verifies the release signature before any installer.
    expect(arguments['signature'], releaseSignature);
    // The server the installer came from, for the record (the runner relies
    // on the pinned signature, not on this).
    expect(arguments['origin'], release.url.origin);
    for (final (code, expected) in [
      ('driver_install_required', 'driver_install_required'),
      ('installer_exited', 'installer_exited'),
      ('signature_invalid', 'signature_invalid'),
      ('update_origin_unverified', 'update_origin_unverified'),
      ('C:\\Users\\secret', 'installer_start_failed'),
    ]) {
      reply = code;
      await expectLater(
        platform.install(
          downloaded,
          UpdateInstallMode.manual,
          currentVersion: '0.4.15',
        ),
        throwsA(isA<UpdateFailure>().having((e) => e.code, 'code', expected)),
      );
    }
    reply = 'package_version_mismatch';
    await expectLater(
      platform.verifyInstaller(downloaded),
      throwsA(
        isA<UpdateFailure>().having(
          (e) => e.code,
          'code',
          'package_version_mismatch',
        ),
      ),
    );
    expect(calls.last.method, 'verifyUpdate');
    expect((calls.last.arguments as Map)['signature'], releaseSignature);
    reply = 'signature_invalid';
    await expectLater(
      platform.verifyInstaller(downloaded),
      throwsA(
        isA<UpdateFailure>().having((e) => e.code, 'code', 'signature_invalid'),
      ),
    );
    api.close();
  });

  test('the release signature is carried as published; a malformed one counts as none', () {
    final origin = Uri.parse('https://my-own-server.example:18443');
    expect(
      ProductRelease.parse(entry(), origin, 'client')!.signature,
      releaseSignature,
    );
    // Older entries (and released servers) have none; old clients ignore the field.
    expect(
      ProductRelease.parse(
        entry()..remove('signature'),
        origin,
        'client',
      )!.signature,
      isNull,
    );
    for (final bad in [
      '',
      'x',
      7,
      releaseSignature.substring(0, 87),
      '${releaseSignature.substring(0, 86)}=A',
      releaseSignature.replaceRange(0, 1, '*'),
    ]) {
      expect(
        ProductRelease.parse(
          {...entry(), 'signature': bad},
          origin,
          'client',
        )!.signature,
        isNull,
        reason: '$bad',
      );
    }
  });

  test('an unsigned release is refused before any download or file', () async {
    final unsigned = entry()..remove('signature');
    final transport = Transport(
      (url) => url.path == '/api/v1/downloads'
          ? catalog([unsigned])
          : throw StateError('An unsigned release is never downloaded: $url'),
    );
    final api = ProductUpdates(
      'https://my-own-server.example:18443',
      transport: transport,
      temporaryRoot: downloads,
    );
    final release = (await api.check('client', '0.4.15'))!;
    expect(release.signature, isNull);
    await expectLater(
      api.download(release, (_, _) {}),
      throwsA(
        isA<UpdateFailure>().having((e) => e.code, 'code', 'signature_invalid'),
      ),
    );
    expect(transport.calls.map((u) => u.path), ['/api/v1/downloads']);
    expect(downloads.listSync(), isEmpty);
    api.close();
  });

  testWidgets(
    'an unsigned release shows the signature failure and offers no installer',
    (tester) async {
      final platform = FakeUpdatePlatform();
      await open(
        tester,
        apiFor([entry()..remove('signature')]),
        platform: platform,
      );
      expect(title(tester), '새 버전이 있어요');
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
      );
      expect(title(tester), '업데이트하지 못했어요');
      expect(find.text('코드 signature_invalid'), findsOneWidget);
      expect(
        find.textContaining('배포 서명이 없거나 맞지 않아 설치하지 않았어요.'),
        findsOneWidget,
      );
      expect(find.textContaining('설치된 앱은 그대로예요'), findsOneWidget);
      expect(find.byKey(const Key('manual-product-update')), findsNothing);
      expect(platform.calls, isEmpty);
      expect(downloads.listSync(), isEmpty);
      await discardDownloads(tester);
    },
  );

  testWidgets(
    'a signature the native side refuses is never offered for restart',
    (tester) async {
      final platform = FakeUpdatePlatform(verifyFailure: 'signature_invalid');
      await open(
        tester,
        apiFor([entry(product: 'host', version: '0.2.70')]),
        current: '0.2.69',
        product: 'host',
        platform: platform,
      );
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
      );
      expect(find.text('코드 signature_invalid'), findsOneWidget);
      expect(find.textContaining('기존 Host는 그대로예요'), findsOneWidget);
      expect(find.byKey(const Key('apply-product-update')), findsNothing);
      expect(find.byKey(const Key('manual-product-update')), findsNothing);
      expect(platform.calls, ['verify']);
      expect(downloads.listSync(), isEmpty);
      await discardDownloads(tester);
    },
  );

  // Tuning #7/#24: a download the updater cancelled writes no record (it is
  // no failure); a refused signature stays a failure with its code.
  for (final (failure, code) in [
    ('cancelled', null),
    ('signature_invalid', 'signature_invalid'),
  ]) {
    testWidgets('a download that ends with $failure records ${code ?? 'nothing'}', (
      tester,
    ) async {
      final records = <ProductLogEntry>[];
      final recording = ProductLog.instance.localRecords.listen(records.add);
      addTearDown(recording.cancel);
      await open(
        tester,
        apiFor([entry(product: 'host', version: '0.2.70')]),
        current: '0.2.69',
        product: 'host',
        platform: FakeUpdatePlatform(verifyFailure: failure),
      );
      await tester.tap(find.byKey(const Key('install-product-update')));
      await pumpRealUntil(
        tester,
        () => find.byKey(const Key('update-failure')).evaluate().isNotEmpty,
      );
      final downloads = records.where(
        (e) => e.event == LogEvent.updateDownload,
      );
      if (code == null) {
        expect(downloads, isEmpty);
      } else {
        expect(downloads.last.code, code);
        expect(downloads.last.values['ok'], false);
      }
      await discardDownloads(tester);
    });
  }
}

class _ClosingTransport implements UpdateTransport {
  _ClosingTransport(this.chunks);
  final StreamController<List<int>> chunks;
  bool downloading = false;
  @override
  Future<UpdateResponse> get(Uri url) async {
    if (url.path == '/api/v1/downloads') return catalog([entry()]);
    downloading = true;
    return UpdateResponse(200, executable.length, chunks.stream);
  }

  @override
  void close() {
    if (downloading && !chunks.isClosed) {
      chunks.addError(
        const HttpException('Connection closed while receiving data'),
      );
      unawaited(chunks.close());
    }
  }
}

class FakeUpdatePlatform extends UpdatePlatform {
  FakeUpdatePlatform({
    this.sessions = false,
    this.verifyFailure,
    this.installFailure,
    this.hold,
  });
  final bool sessions;
  final String? verifyFailure;
  String? installFailure;
  final Completer<void>? hold;
  final calls = <String>[];
  DownloadedUpdate? verified, installed;
  String? currentVersion;

  @override
  Future<void> verifyInstaller(DownloadedUpdate update) async {
    calls.add('verify');
    verified = update;
    if (verifyFailure != null) throw UpdateFailure(verifyFailure!);
  }

  @override
  Future<bool> hasRemoteSessions() async {
    calls.add('sessions');
    return sessions;
  }

  @override
  Future<void> closeRemoteSessions() async => calls.add('close-sessions');

  @override
  Future<void> install(
    DownloadedUpdate update,
    UpdateInstallMode mode, {
    required String currentVersion,
  }) async {
    calls.add('install:${mode.name}');
    installed = update;
    this.currentVersion = currentVersion;
    // The file still exists while the installer stages it.
    expect(await update.file.exists(), isTrue);
    await hold?.future;
    if (installFailure != null) throw UpdateFailure(installFailure!);
  }
}
