import 'dart:async';
import 'dart:convert';

import 'package:fake_async/fake_async.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/app_licenses.dart';

class _Assets extends CachingAssetBundle {
  _Assets(this.files);
  final Map<String, String> files;
  final requested = <String>[];
  final held = <String, Future<String>>{};
  @override
  Future<ByteData> load(String key) async {
    requested.add(key);
    final value = held.containsKey(key) ? await held[key] : files[key];
    if (value == null) throw StateError('missing_synthetic_license_asset');
    return ByteData.sublistView(Uint8List.fromList(utf8.encode(value)));
  }
}

String _manifest(List<Map<String, Object?>> components) =>
    jsonEncode({'schema': 1, 'components': components});

Stream<LicenseEntry> _registered() => Stream<LicenseEntry>.fromIterable(const [
  LicenseEntryWithLineBreaks([
    'fixture-a',
    'fixture-b',
  ], 'FIRST\r\n\r\n  Full original.\r\nLAST'),
  LicenseEntryWithLineBreaks([
    'fixture-a',
  ], 'SECOND LICENSE\nComplete second notice.'),
]);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  test(
    'all registry package notices and exact native originals are retained',
    () async {
      const nativeText = 'NATIVE ORIGINAL\r\n  Preserve spacing.\r\nTAIL';
      final assets = _Assets({
        nativeLicenseManifestAsset: _manifest([
          {
            'name': 'fixture-native',
            'license': 'Fixture-License',
            'version': '1.2',
            'textAsset': 'assets/licenses/fixture.txt',
            'source': 'https://fixture.invalid/source',
          },
        ]),
        'assets/licenses/fixture.txt': nativeText,
      });
      final catalog = await AppLicenseCatalog.load(
        assets: assets,
        registered: _registered,
      );
      expect(catalog.issues, isEmpty);
      expect(catalog.components.map((entry) => entry.name), [
        'fixture-a',
        'fixture-b',
        'fixture-native',
      ]);
      expect(catalog.components.first.notices.length, 2);
      expect(
        catalog.components.first.notices.first.text,
        'FIRST\r\n\r\n  Full original.\r\nLAST',
      );
      expect(
        catalog.components[1].notices.single.text,
        catalog.components.first.notices.first.text,
      );
      final notice = catalog.components.last.notices.single;
      expect(notice.text, nativeText);
      expect(notice.license, 'Fixture-License');
      expect(notice.version, '1.2');
      expect(notice.source, 'https://fixture.invalid/source');
      expect(catalog.components.last.matches('preserve spacing'), isTrue);
      expect(() => catalog.components.clear(), throwsUnsupportedError);
    },
  );

  test(
    'client and Host manifests remain separate target inventories',
    () async {
      final assets = _Assets({
        nativeLicenseManifestAsset: _manifest([
          {
            'name': 'fixture-client-only',
            'license': 'Client-License',
            'textAsset': 'assets/licenses/client.txt',
          },
        ]),
        'assets/licenses/host_native_notices.json': _manifest([
          {
            'name': 'fixture-host-only',
            'license': 'Host-License',
            'textAsset': 'assets/licenses/host.txt',
          },
        ]),
        'assets/licenses/client.txt': 'CLIENT RAW',
        'assets/licenses/host.txt': 'HOST RAW',
      });
      final client = await AppLicenseCatalog.load(
        assets: assets,
        registered: () => const Stream<LicenseEntry>.empty(),
      );
      expect(client.components.single.name, 'fixture-client-only');
      expect(assets.requested, isNot(contains('assets/licenses/host.txt')));
      assets.requested.clear();
      final host = await AppLicenseCatalog.load(
        assets: assets,
        manifestAsset: 'assets/licenses/host_native_notices.json',
        registered: () => const Stream<LicenseEntry>.empty(),
      );
      expect(host.components.single.name, 'fixture-host-only');
      expect(assets.requested, isNot(contains('assets/licenses/client.txt')));
    },
  );

  test('missing native inventory reports incomplete load while keeping registry notices', () async {
    final catalog = await AppLicenseCatalog.load(
      assets: _Assets({}),
      registered: _registered,
    );
    expect(catalog.components.map((entry) => entry.name), [
      'fixture-a',
      'fixture-b',
    ]);
    expect(catalog.issues, [AppLicenseIssue.nativeManifestUnavailable]);
  });

  test(
    'invalid native path is not read and does not erase the next valid notice',
    () async {
      final assets = _Assets({
        nativeLicenseManifestAsset: _manifest([
          {
            'name': 'unsafe-fixture',
            'license': 'Fixture',
            'textAsset': '../outside.txt',
          },
          {
            'name': 'valid-fixture',
            'license': 'Fixture',
            'textAsset': 'assets/licenses/valid.txt',
          },
        ]),
        'assets/licenses/valid.txt': 'VALID ORIGINAL',
      });
      final catalog = await AppLicenseCatalog.load(
        assets: assets,
        registered: () => const Stream<LicenseEntry>.empty(),
      );
      expect(catalog.components.single.name, 'valid-fixture');
      expect(catalog.issues, [AppLicenseIssue.nativeNoticeUnavailable]);
      expect(assets.requested, isNot(contains('../outside.txt')));
    },
  );

  for (final value in <Map<String, Object?>>[
    {'schema': '1', 'components': []},
    {'schema': 1, 'components': 'not-a-list'},
  ]) {
    test(
      'malformed manifest is not an empty successful inventory: ${value['schema']}/${value['components']}',
      () async {
        final catalog = await AppLicenseCatalog.load(
          assets: _Assets({nativeLicenseManifestAsset: jsonEncode(value)}),
          registered: () => const Stream<LicenseEntry>.empty(),
        );
        expect(catalog.components, isEmpty);
        expect(catalog.issues, [AppLicenseIssue.nativeManifestUnavailable]);
      },
    );
  }

  test(
    'registry error preserves completed entries and native inventory',
    () async {
      Stream<LicenseEntry> failing() async* {
        yield const LicenseEntryWithLineBreaks([
          'completed-fixture',
        ], 'COMPLETED');
        throw StateError('synthetic_registry_failure');
      }

      final catalog = await AppLicenseCatalog.load(
        assets: _Assets({nativeLicenseManifestAsset: _manifest([])}),
        registered: failing,
      );
      expect(catalog.components.single.notices.single.text, 'COMPLETED');
      expect(catalog.issues, [AppLicenseIssue.registeredUnavailable]);
    },
  );

  test('timed-out asset stops further batches and a late read cannot alter the catalog', () {
    fakeAsync((clock) {
      final held = Completer<String>();
      final assets = _Assets({
        nativeLicenseManifestAsset: _manifest([
          for (var index = 0; index < 5; index++)
            {
              'name': 'fixture-$index',
              'license': 'Fixture',
              'textAsset': 'assets/licenses/notice-$index.txt',
            },
        ]),
        for (var index = 1; index < 5; index++)
          'assets/licenses/notice-$index.txt': 'ORIGINAL $index',
      })..held['assets/licenses/notice-0.txt'] = held.future;
      AppLicenseCatalog? result;
      unawaited(
        AppLicenseCatalog.load(
          assets: assets,
          registered: () => const Stream<LicenseEntry>.empty(),
        ).then((value) {
          result = value;
        }),
      );
      clock.flushMicrotasks();
      expect(result, isNull);
      clock.elapse(const Duration(seconds: 6));
      clock.flushMicrotasks();
      expect(result, isNotNull);
      expect(result!.components.length, 3);
      expect(result!.issues, contains(AppLicenseIssue.nativeNoticeUnavailable));
      expect(assets.requested, isNot(contains('assets/licenses/notice-4.txt')));
      held.complete('LATE ORIGINAL');
      clock.flushMicrotasks();
      expect(result!.components.length, 3);
    });
  });
}
