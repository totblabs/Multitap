import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_engine_stats.dart';

/// The 연결 통계 dialog's "이 기기 처리" rows as the Android engine's `stats`
/// answer fills them (AndroidMedia.kt), and what an older engine or an
/// Android version without an API leaves unmeasured.
void main() {
  Map<String, String> rows(Map<String, dynamic>? stats) => {
    for (final (label, value) in androidEngineMetrics(stats)) label: value,
  };

  test('a full answer reads as the owner sees it', () {
    final values = rows({
      'decoderLowLatency': 1 | 2 | 4 | 8 | 16,
      'decoderVendorKeys': 1,
      'decoderPlainFallback': false,
      'decoderFallbacks': 0,
      'decodeRecentMs': 4.24,
      'decodeRecentMaxMs': 9.06,
      'frameArrivalMs': 5.5,
      'frameArrivalMaxMs': 130.4,
      'presentWaitMs': 7.0,
      'presentWaitMaxMs': 16.0,
      'skippedDecodedFrames': 12,
      'textureQueued': 34,
      'droppedPackets': 0,
      'audioEnabled': true,
      'audioDepthMs': 42.0,
      'audioTargetMs': 40.0,
      'audioBufferMs': 120.0,
      'audioFastTrack': true,
      'audioLatencyMs': 35.2,
      'displayHz': 120,
      'wifiLock': 1,
      'wifiLockRefused': 0,
      'thermalStatus': 0,
      'thermalStatusMax': 2,
    });
    expect(values['디코더 저지연'], '켜짐 (Android 저지연 · 실시간 우선 · Qualcomm 저지연 키)');
    expect(values['디코더 대체'], '없음');
    expect(values['디코딩 시간'], '평균 4.2 ms · 최대 9.1 ms');
    expect(values['수신→디코딩 완료'], '평균 5.5 ms · 최대 130 ms');
    expect(values['디코딩 완료→화면 전달'], '평균 7.0 ms · 최대 16.0 ms');
    expect(values['프레임 (교체 · 대기 · 버림)'], '12 · 34 · 0');
    expect(values['오디오 버퍼'], '대기 42.0 ms · 목표 40.0 ms · 버퍼 120 ms');
    expect(values['오디오 출력'], '저지연 경로 · 지연 35.2 ms');
    expect(values['화면 주사율'], '120 Hz');
    expect(values['Wi-Fi 저지연'], '요청함');
    expect(values['기기 발열'], '정상 (이번 연결 최고: 따뜻함)');
    // The answer of an engine without the sharp* fields: 미측정, as any other
    // value it did not report (the row itself: android_sharp_picture_test).
    expect(values.length, 12);
  });

  test('an engine that reports none of it leaves every row unmeasured', () {
    final values = rows(const {'decoder': 'c2.android.avc.decoder'});
    expect(values['디코더 저지연'], '준비 중');
    for (final label in [
      '디코더 대체',
      '디코딩 시간',
      '수신→디코딩 완료',
      '디코딩 완료→화면 전달',
      '오디오 버퍼',
      '오디오 출력',
      '화면 주사율',
      'Wi-Fi 저지연',
      '기기 발열',
    ]) {
      expect(values[label], '미측정', reason: label);
    }
    expect(values['프레임 (교체 · 대기 · 버림)'], '— · — · —');
    expect(androidEngineMetrics(null), isEmpty);
  });

  test('the decoder row says what was accepted', () {
    expect(androidDecoderTuningLabel(null, null, null), '준비 중');
    expect(androidDecoderTuningLabel(0, 0, false), '꺼짐 (기본 설정)');
    expect(androidDecoderTuningLabel(16, 0, false), '꺼짐 (기본 설정)');
    expect(androidDecoderTuningLabel(2, 0, false), '꺼짐 (실시간 우선)');
    expect(
      androidDecoderTuningLabel(1 | 2 | 4, 5, false),
      '켜짐 (Android 저지연 · 실시간 우선)',
    );
    expect(
      androidDecoderTuningLabel(2 | 4 | 8, 2, false),
      '켜짐 (실시간 우선 · Exynos 저지연 키)',
    );
    expect(androidDecoderTuningLabel(8, 9, false), '켜짐 (제조사 저지연 키)');
    // After a failure the decoder runs the platform's defaults.
    expect(androidDecoderTuningLabel(16, 1, true), '기본 설정 (오류 뒤 대체)');
    expect(androidDecoderTuningLabel(64, 0, false), '준비 중');
    expect(rows({'decoderFallbacks': 2})['디코더 대체'], '2회');
  });

  test('Wi-Fi lock and heat', () {
    expect(androidWifiLockLabel(1, 0), '요청함');
    expect(androidWifiLockLabel(2, 0), '요청함 (고성능 모드)');
    expect(androidWifiLockLabel(0, 0), '요청 안 함');
    expect(androidWifiLockLabel(0, 1), '기기가 거절함');
    expect(androidWifiLockLabel(null, null), '미측정');
    expect(androidThermalLabel(0, 0), '정상');
    expect(androidThermalLabel(3, 3), '뜨거움');
    expect(androidThermalLabel(1, 4), '약간 따뜻함 (이번 연결 최고: 매우 뜨거움)');
    expect(androidThermalLabel(null, null), '미측정');
    expect(androidThermalLabel(9, 2), '미측정');
  });

  test('sound off, a normal track and values out of range', () {
    final off = rows({'audioEnabled': false, 'audioBufferMs': null});
    expect(off['오디오 버퍼'], '미측정');
    final normal = rows({
      'audioEnabled': true,
      'audioDepthMs': 61.5,
      'audioTargetMs': 60.0,
      'audioBufferMs': 80.0,
      'audioFastTrack': false,
      'audioLatencyMs': null,
      'decodeRecentMs': -1.0,
      'frameArrivalMs': 3.0,
      'frameArrivalMaxMs': null,
      'displayHz': 0,
    });
    expect(normal['오디오 버퍼'], '대기 61.5 ms · 목표 60.0 ms · 버퍼 80.0 ms');
    expect(normal['오디오 출력'], '일반 경로');
    expect(normal['디코딩 시간'], '미측정');
    expect(normal['수신→디코딩 완료'], '평균 3.0 ms');
    expect(normal['화면 주사율'], '미측정');
  });

  test('the note is short', () {
    expect('\n'.allMatches(androidEngineMetricsNote).length, 1);
  });
}
