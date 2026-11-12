import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/app_licenses.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'client license inventory loads complete bundled notices offline',
    () async {
      final catalog = await AppLicenseCatalog.load(
        registered: () => const Stream.empty(),
      );
      expect(catalog.issues, isEmpty);
      final byName = {
        for (final entry in catalog.components) entry.name: entry,
      };
      expect(
        byName.keys,
        containsAll([
          'Multitap',
          'Pretendard',
          'Material Icons',
          'USB ID Repository',
          'UsbDk',
          'Microsoft Visual C++ Runtime',
          'libopus',
          'Microsoft WebView2 SDK',
        ]),
      );
      expect(byName.keys, isNot(contains('x264')));
      expect(byName.keys, isNot(contains('WinFsp')));
      expect(
        byName['libopus']!.notices.single.text,
        contains('Redistribution and use in source and binary forms'),
      );
      expect(byName['Microsoft WebView2 SDK']!.notices.length, 2);
      final project = byName['Multitap']!.notices.single.text;
      expect(
        project.trimLeft(),
        startsWith('GNU AFFERO GENERAL PUBLIC LICENSE'),
      );
      expect(project, contains('Version 3, 19 November 2007'));
      expect(project, contains('13. Remote Network Interaction'));
      expect(project.length, greaterThan(30000));
      expect(
        project,
        contains('How to Apply These Terms to Your New Programs'),
      );
      expect(byName['Material Icons']!.notices.length, 2);
      expect(
        byName['UsbDk']!.notices.single.text,
        contains('9. Accepting Warranty or Additional Liability.'),
      );
    },
  );

  test('Host inventory retains driver notices, source offer and full FLOSS exception', () async {
    final catalog = await AppLicenseCatalog.load(
      registered: () => const Stream.empty(),
      manifestAsset: 'assets/licenses/host_native_notices.json',
    );
    expect(catalog.issues, isEmpty);
    final byName = {for (final entry in catalog.components) entry.name: entry};
    expect(
      byName.keys,
      containsAll([
        'Multitap Host',
        'x264',
        'WinFsp',
        'libvirtualdisplay',
        'usbip-win2',
        'Virtual Display Driver',
        'Sunshine Virtual Display Driver',
        'Pretendard',
        'Material Icons',
        'Microsoft Visual C++ Runtime',
        'USB ID Repository',
        'libopus',
        'NVIDIA NVENC API header',
        'AMD AMF headers',
        'Microsoft WebView2 SDK',
      ]),
    );
    expect(byName.keys, isNot(contains('UsbDk')));
    final winFsp = byName['WinFsp']!.notices.single.text;
    expect(winFsp.length, greaterThan(36000));
    expect(winFsp, contains('special exception to GPLv3'));
    expect(winFsp, contains('END OF TERMS AND CONDITIONS'));
    expect(
      byName['Multitap Host']!.notices.any(
        (notice) =>
            notice.text.contains('Corresponding source (written offer)') &&
            notice.text.contains('x264-b35605a-source.zip') &&
            notice.text
                .replaceAll(RegExp(r'\s+'), ' ')
                .contains(
                  'AGPL-3.0 code linked with GPL-3.0 code, which section 13 '
                  'of both licences permits',
                ),
      ),
      isTrue,
    );
    // The Host's own licence is the AGPL text; the GPL version 3 text that
    // x264 and WinFsp are used under stays in the WinFsp notice.
    expect(
      byName['Multitap Host']!.notices.any(
        (notice) => notice.text.trimLeft().startsWith(
          'GNU AFFERO GENERAL PUBLIC LICENSE',
        ),
      ),
      isTrue,
    );
    expect(winFsp, contains('GNU GENERAL PUBLIC LICENSE'));
    expect(winFsp, contains('Version 3, 29 June 2007'));
  });

  test('Android inventory lists only what the APK carries', () async {
    expect(
      licenseManifestFor(nativeLicenseManifestAsset, android: true),
      androidLicenseManifestAsset,
    );
    expect(
      licenseManifestFor(nativeLicenseManifestAsset, android: false),
      nativeLicenseManifestAsset,
    );
    // These tests run on Windows: the default follows the real platform.
    expect(
      licenseManifestFor(nativeLicenseManifestAsset),
      nativeLicenseManifestAsset,
    );
    const host = 'assets/licenses/host_native_notices.json';
    expect(licenseManifestFor(host, android: true), host);
    final catalog = await AppLicenseCatalog.load(
      registered: () => const Stream.empty(),
      manifestAsset: androidLicenseManifestAsset,
    );
    expect(catalog.issues, isEmpty);
    expect(catalog.components.map((entry) => entry.name).toSet(), {
      'Multitap',
      'Pretendard',
      'Material Icons',
    });
  });

  test('bundled notice copies match the pinned in-tree licence files', () {
    // Flutter tests run in apps/windows_client; the sources sit at the root.
    for (final (asset, source) in [
      ('AGPL-3.0.txt', '../../LICENSE'),
      ('libopus-COPYING.txt', '../../native/third_party/opus/COPYING'),
      (
        'nvEncodeAPI-LICENSE.txt',
        '../../apps/windows_host/third_party/nvenc/LICENSE.nvEncodeAPI.txt',
      ),
      (
        'AMF-LICENSE.txt',
        '../../apps/windows_host/third_party/amf/LICENSE.txt',
      ),
      (
        'WinFsp-License.txt',
        '../../apps/windows_host/third_party/winfsp/License.txt',
      ),
    ]) {
      expect(
        File('assets/licenses/$asset').readAsBytesSync(),
        File(source).readAsBytesSync(),
        reason: asset,
      );
    }
    // From the pinned Microsoft.Web.WebView2 1.0.4314.35 package
    // (windows/runner/webview2.cmake), which the build also copies.
    expect(
      sha256
          .convert(
            File('assets/licenses/WebView2-LICENSE.txt').readAsBytesSync(),
          )
          .toString(),
      '0af8f1b807512aae39c2ac1aa4d0cae65cabecb6fd554b8439a5162a0d6eca55',
    );
  });
}
