import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/client_build_version.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('multitap/window');
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  tearDown(() => messenger.setMockMethodCallHandler(channel, null));

  test('display and update comparison use the running EXE build', () async {
    messenger.setMockMethodCallHandler(channel, (call) async {
      expect(call.method, 'productVersion');
      return '0.4.25+39';
    });
    final version = await ClientBuildVersion.load();
    expect(version.version, '0.4.25');
    expect(version.label, '0.4.25');
    expect(version.build, '39');
    // No fixed update version may survive after reading a different EXE.
    messenger.setMockMethodCallHandler(channel, (_) async => '12.34.56+789');
    expect((await ClientBuildVersion.load()).version, '12.34.56');
  });

  test(
    'invalid or missing EXE version stays unknown without an older fallback',
    () async {
      for (final invalid in <Object?>[
        null,
        22,
        '',
        '0.4',
        '0.4.22\n',
        ' 0.4.22',
        '0.4.22+test',
        '1.2.3+1\n',
        '1000000.2.3',
      ]) {
        expect(ClientBuildVersion.parse(invalid).version, isNull);
      }
      messenger.setMockMethodCallHandler(channel, (_) async {
        throw PlatformException(code: 'unavailable');
      });
      expect((await ClientBuildVersion.load()).version, isNull);
      messenger.setMockMethodCallHandler(channel, (_) async => null);
      expect((await ClientBuildVersion.load()).label, '버전 확인 필요');
    },
  );
}
