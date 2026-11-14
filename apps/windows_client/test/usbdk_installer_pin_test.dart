import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

// The UsbDk installer every client carries (usb-v1): one pin,
// third_party/usbdk/upstream.json, which the fetch script downloads, the build
// rule puts beside the client and the runner verifies before it runs it.
void main() {
  final pin =
      (jsonDecode(File('third_party/usbdk/upstream.json').readAsStringSync())
              as Map)['file']
          as Map;

  test('The runner verifies exactly the pinned installer', () {
    final header = File('windows/runner/usb_capture.h').readAsStringSync();
    expect(header, contains('sha256 = "${pin['sha256']}";'));
    expect(header, contains('size = ${pin['size']};'));
    expect(header, contains('file = L"${pin['name']}";'));
  });

  test('Every client build carries it, read from the pin; a release build stops without it; Host settings never carry it', () {
    final cmake = File('windows/CMakeLists.txt').readAsStringSync();
    final start = cmake.indexOf('if(FLUTTER_VERSION_BUILD LESS 20000)');
    expect(start, greaterThan(0), reason: 'the Client-only branch');
    final end = cmake.indexOf('\nendif()', start);
    expect(end, greaterThan(start));
    final block = cmake.substring(start, end);
    expect(
      cmake.indexOf('MULTITAP_USBDK'),
      greaterThan(start),
      reason: 'no UsbDk rule outside the Client-only branch',
    );
    expect(
      cmake.lastIndexOf('MULTITAP_USBDK'),
      lessThan(end),
      reason: 'no UsbDk rule outside the Client-only branch',
    );
    expect(
      block,
      contains(
        'file(READ "\${CMAKE_CURRENT_SOURCE_DIR}/../third_party/usbdk/upstream.json"',
      ),
    );
    for (final field in ['release', 'file name', 'file size', 'file sha256']) {
      expect(block, contains('" $field)'), reason: field);
    }
    // The same place the fetch script puts it.
    expect(
      block,
      contains(
        '.tools/usb-drivers/usbdk-\${MULTITAP_USBDK_RELEASE}/\${MULTITAP_USBDK_NAME}"',
      ),
    );
    expect(
      File('../../tools/fetch-usb-drivers.ps1').readAsStringSync(),
      contains("('.tools/usb-drivers/usbdk-' + \$usb.release)"),
    );
    // Checked when the bundle is installed: a Release or Profile build stops,
    // a Debug build warns; never a silent skip.
    expect(
      block,
      contains('[Rr][Ee][Ll][Ee][Aa][Ss][Ee]|[Pp][Rr][Oo][Ff][Ii][Ll][Ee]'),
    );
    expect(block, contains('set(MULTITAP_USBDK_LEVEL FATAL_ERROR)'));
    expect(block, contains('set(MULTITAP_USBDK_LEVEL WARNING)'));
    expect(block, contains('usbdk_installer_missing'));
    expect(block, contains('usbdk_installer_changed'));
    expect(block, isNot(contains('if(EXISTS')));
    expect(
      block,
      contains(
        'install(FILES "\${MULTITAP_USBDK_MSI}" DESTINATION "\${CMAKE_INSTALL_PREFIX}/usbdk"',
      ),
    );
    // Name, size and hash come from the pin, never a copy.
    expect(cmake, isNot(contains(pin['sha256'] as String)));
    expect(cmake, isNot(contains(pin['name'] as String)));
    expect(cmake, isNot(contains('${pin['size']}')));
  });
}
