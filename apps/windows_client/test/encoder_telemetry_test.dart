import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/encoder_telemetry.dart';
import 'package:multitap_client/data/stream_telemetry.dart';

Map<String, dynamic> encoder({
  String backend = 'nvenc',
  bool initialized = true,
  bool verified = false,
}) => {
  'protocol': 'h264-encoder-v1',
  'backend': backend,
  'initialized': initialized,
  'hardwareVerified': verified,
  'adapterLuid': initialized ? 'fedcba98:76543210' : null,
  'vendorId': initialized ? 0x10de : null,
  'deviceId': initialized ? 0x2684 : null,
  'adapterName': initialized ? 'Fixture NVIDIA adapter' : null,
  'gpuFallbackCode': 0,
  'softwareFallbackCode': 0,
};
Map<String, dynamic> stats() => {
  'capturedFps': 60,
  'encodedFps': 60,
  'bitrateKbps': 8000,
};
void main() {
  test(
    'initialization and actual first output have distinct Host encoder labels',
    () {
      final pending = HostEncoderInfo.parse(encoder())!;
      expect(hostEncoderLabel(pending), contains('출력 확인 전'));
      final active = HostEncoderInfo.parse(encoder(verified: true))!;
      expect(hostEncoderLabel(active), 'GPU · NVIDIA NVENC');
      expect(active.adapterLuid, 'fedcba98:76543210');
      expect(
        hostEncoderLabel(HostEncoderInfo.parse(encoder(initialized: false))),
        '초기화 확인 중',
      );
    },
  );
  test('CPU fallback clears old GPU identity and preserves fixed reason', () {
    final cpu = {
      ...encoder(),
      'backend': 'x264',
      'hardwareVerified': false,
      'adapterLuid': null,
      'vendorId': null,
      'deviceId': null,
      'adapterName': null,
      'gpuFallbackCode': 43,
    };
    final value = HostEncoderInfo.parse(cpu)!;
    expect(hostEncoderLabel(value), 'CPU · x264');
    expect(hostEncoderFallback(value), 'GPU 장치 연결이 끊김');
    expect(HostEncoderInfo.parse({...cpu, 'hardwareVerified': true}), isNull);
    expect(
      HostEncoderInfo.parse({...cpu, 'adapterLuid': '00000001:00000001'}),
      isNull,
    );
  });
  test('AMD and Intel are separate actual backends, not inferred from client decoder', () {
    final amd = HostEncoderInfo.parse({
      ...encoder(backend: 'amf', verified: true),
      'vendorId': 0x1002,
    })!;
    final intel = HostEncoderInfo.parse({
      ...encoder(backend: 'qsv-mft', verified: true),
      'vendorId': 0x8086,
    })!;
    expect(hostEncoderLabel(amd), 'GPU · AMD AMF');
    expect(hostEncoderLabel(intel), 'GPU · Intel · Media Foundation');
    expect(
      HostEncoderInfo.parse({...encoder(verified: true), 'vendorId': 0x1002}),
      isNull,
    );
  });
  test('LUID is full64bit canonical text and never a JSON number or unsafe identity', () {
    expect(
      HostEncoderInfo.parse({
        ...encoder(verified: true),
        'adapterLuid': 'ffffffff:ffffffff',
      })?.adapterLuid,
      'ffffffff:ffffffff',
    );
    for (final invalid in [
      0xffffffffffffffff,
      'ffffffffffffffff',
      'FEDCBA98:76543210',
      '00000000:00000000',
      '1:2',
      '',
      true,
    ]) {
      expect(
        HostEncoderInfo.parse({...encoder(), 'adapterLuid': invalid}),
        isNull,
      );
    }
  });
  test('malformed optional encoder data never discards valid legacy FPS', () {
    for (final invalid in [
      null,
      true,
      'gpu',
      {...encoder(), 'backend': 'automatic'},
      {...encoder(), 'gpuFallbackCode': 31},
      {...encoder(), 'gpuFallbackCode': 45},
      {...encoder(), 'softwareFallbackCode': 8},
      {...encoder(), 'vendorId': -1},
      {...encoder(), 'deviceId': 0x100000000},
      {...encoder(), 'hardwareVerified': 1},
      {...encoder(), 'extra': 'untrusted'},
      {...encoder(), 'adapterName': 'bad\nname'},
      {...encoder(), 'adapterName': 'x' * 129},
    ]) {
      final value = HostStreamTelemetry.parse({
        ...stats(),
        'encoderInfo': invalid,
      })!;
      expect(value.encoder, isNull);
      expect(value.encodedFps, 60);
    }
  });
  test('legacy encoder has no fabricated GPU or device claim', () {
    final legacy = HostStreamTelemetry.parse({...stats(), 'encoder': 'mft'})!;
    expect(legacy.encoder, isNull);
    expect(
      hostEncoderLabel(legacy.encoder, legacy.legacyEncoder),
      'Media Foundation · 장치 미확인',
    );
    expect(hostEncoderLabel(null, 'x264'), 'CPU · x264');
    expect(hostEncoderLabel(null, 'nvenc'), '—');
  });
}
