import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/host_encoder_settings.dart';

Map<String, dynamic> encoderFixture({
  String backend = 'x264',
  String? adapter,
  String? operation,
  bool known = true,
}) => {
  'schema': 1,
  'supported': true,
  'administrator': true,
  'revision': List.filled(64, 'a').join(),
  'backend': backend,
  'adapterId': adapter,
  'operationId': operation,
  'inventoryKnown': known,
  'inventoryIsEncoderSupport': false,
  'autoSupported': true,
  'adapters': [
    {
      'id': 'fedcba98:76543210',
      'vendorId': 0x10de,
      'deviceId': 0x2684,
      'name': 'Synthetic GPU',
      'candidate': true,
    },
  ],
};
void main() {
  test('inventory remains a candidate and full64 identity stays text', () {
    final value = HostEncoderSettings.parse(encoderFixture());
    expect(value.canSave, isTrue);
    expect(value.adapters.single.id, 'fedcba98:76543210');
    expect(value.backend, 'x264');
  });
  test('unsupported old Host is quiet and cannot save', () {
    expect(HostEncoderSettings.parse(null).supported, isFalse);
    expect(HostEncoderSettings.parse({'schema': 0}).canSave, isFalse);
  });
  test('malformed metadata cannot turn into selected GPU', () {
    for (final bad in [
      '00000000:00000000',
      'FEDCBA98:76543210',
      '0xfedcba9876543210',
      jsonDecode('18446744073709551615'),
    ]) {
      final raw = encoderFixture(backend: 'gpu')..['adapterId'] = bad;
      expect(HostEncoderSettings.parse(raw).supported, isFalse);
    }
  });
  test(
    'duplicate inventory and unsafe names refuse rather than fabricate ready',
    () {
      final raw = encoderFixture();
      (raw['adapters'] as List).add(
        Map<String, Object>.from((raw['adapters'] as List).single as Map),
      );
      expect(HostEncoderSettings.parse(raw).supported, isFalse);
      final unsafe = encoderFixture();
      (unsafe['adapters'] as List).single['name'] = 'Synthetic\nGPU';
      expect(HostEncoderSettings.parse(unsafe).supported, isFalse);
    },
  );
  test('CPU selection has no adapter and unavailable inventory does not disable CPU', () {
    expect(
      HostEncoderSettings.parse(encoderFixture(known: false)).canSave,
      isTrue,
    );
    expect(
      HostEncoderSettings.parse(encoderFixture(adapter: 'fedcba98:76543210'))
          .supported,
      isFalse,
    );
  });
  test('operation identity is canonical and independently generated', () {
    final first = newEncoderSettingsOperationId(),
        second = newEncoderSettingsOperationId();
    expect(
      first,
      matches(
        RegExp(
          r'^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}$',
        ),
      ),
    );
    expect(first, isNot(second));
    expect(
      HostEncoderSettings.parse(encoderFixture(operation: first)).operationId,
      first,
    );
  });
  test('automatic selection requires explicit Host support and has no manual adapter', () {
    final automatic = encoderFixture(backend: 'auto', known: false);
    automatic['adapters'] = <Object>[];
    final value = HostEncoderSettings.parse(automatic);
    expect(value.canSave, isTrue);
    expect(value.autoSupported, isTrue);
    expect(value.adapterId, isNull);
    expect(
      HostEncoderSettings.parse({...automatic, 'autoSupported': false})
          .supported,
      isFalse,
    );
    expect(
      HostEncoderSettings.parse({
        ...automatic,
        'adapterId': 'fedcba98:76543210',
      }).supported,
      isFalse,
    );
    expect(
      HostEncoderSettings.parse(encoderFixture()..remove('autoSupported'))
          .autoSupported,
      isFalse,
    );
  });
  test('historical output survives absent live snapshot without inventing GPU nonuse', () {
    final raw = encoderFixture()
      ..['recentUsage'] = {
        'protocol': 'host-encoder-usage-v1',
        'trackId': '1',
        'time': '2026-10-30T01:02:03.004Z',
        'backend': 'nvenc',
        'hardwareVerified': true,
        'gpuFallbackCode': 0,
        'softwareFallbackCode': 0,
      };
    final usage = HostEncoderSettings.parse(raw).recentUsage!;
    expect(usage.hardwareVerified, isTrue);
    expect(usage.backend, 'nvenc');
    expect(usage.trackId, '1');
    expect(HostEncoderSettings.parse(encoderFixture()).recentUsage, isNull);
    final bad = {...raw['recentUsage'] as Map, 'hardwareVerified': false};
    expect(
      HostEncoderSettings.parse({...raw, 'recentUsage': bad}).recentUsage,
      isNull,
    );
    expect(
      HostEncoderSettings.parse({
        ...raw,
        'recentUsage': {...bad, 'backend': 'x264', 'gpuFallbackCode': 43},
      }).recentUsage!.gpuFallbackCode,
      43,
    );
  });
}
