import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_engine_stats.dart';
import 'package:multitap_client/data/audio_stats.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/session_stats_dialog.dart';

/// The 연결 통계 dialog's "오디오" section on the phone: what decides a "no
/// sound" report without the admin console (the Host sent sound or not, which
/// decoder plays, audio focus, the device's media volume, underruns and audio
/// thread errors), and the Host's send rate it reads from stream.stats.
void main() {
  Map<String, String> rows(Map<String, dynamic>? stats, {double? host}) => {
    for (final (label, value) in androidAudioMetrics(
      stats,
      hostPerSecond: host,
    ))
      label: value,
  };

  const playing = <String, dynamic>{
    'audioEnabled': true,
    'audioPlaying': true,
    'audioOpus': true,
    'audioDecoder': 1,
    'audioFocus': 1,
    'audioMuted': false,
    'mediaVolume': 9,
    'mediaVolumeMax': 15,
    'audioStreamUnderruns': 2,
    'audioThreadErrors': 0,
  };

  test('a playing Opus stream reads as the owner sees it', () {
    final values = rows(playing, host: 99.6);
    expect(values, {
      'Host가 보낸 오디오': '초당 100개',
      '오디오 디코더': 'Opus (기기 기본)',
      '오디오 포커스': '받음',
      '기기 음량': '9/15',
      '오디오 재생': '재생 중 · 끊김 2회',
    });
  });

  test('each "no sound" cause has its own words', () {
    // The Host sends nothing (a silent desktop or a capture that sends nothing).
    expect(rows(playing, host: 0)['Host가 보낸 오디오'], '보내지 않음');
    // No stream.stats yet (or an older Host).
    expect(rows(playing)['Host가 보낸 오디오'], '미측정');
    // Audio focus: a call, a delayed grant, another player.
    expect(rows({...playing, 'audioFocus': 3})['오디오 포커스'], '잠시 넘겨줌 (음소거)');
    expect(rows({...playing, 'audioFocus': 2})['오디오 포커스'], '기다리는 중 (음소거)');
    expect(rows({...playing, 'audioFocus': 4})['오디오 포커스'], '다른 앱이 가져감 (음소거)');
    expect(rows({...playing, 'audioFocus': 0})['오디오 포커스'], '없음');
    // The phone's media volume at 0.
    expect(rows({...playing, 'mediaVolume': 0})['기기 음량'], '0/15 (소리 꺼짐)');
    // The decoder: resting after failures, or none on this device (the next connection asks for PCM).
    expect(rows({...playing, 'audioDecoder': 3})['오디오 디코더'], '없음 · 다시 시도 중');
    expect(
      rows({
        ...playing,
        'audioDecoder': 3,
        'audioOpusUnusable': true,
      })['오디오 디코더'],
      '없음 · 다음 연결은 PCM',
    );
    expect(rows({...playing, 'audioDecoder': 0, 'audioOpus': false})['오디오 디코더'], 'PCM');
    expect(rows({...playing, 'audioDecoder': 0})['오디오 디코더'], '준비 중');
    expect(rows({...playing, 'audioDecoder': 2})['오디오 디코더'], 'Opus');
    // Silence, and the audio thread's errors.
    expect(
      rows({...playing, 'audioPlaying': false, 'audioStreamUnderruns': 0})['오디오 재생'],
      '조용함 · 끊김 0회',
    );
    expect(
      rows({...playing, 'audioThreadErrors': 3})['오디오 재생'],
      '재생 중 · 끊김 2회 · 오류 3회',
    );
  });

  test('sound off or an engine without the fields', () {
    expect(androidAudioMetrics(null), isEmpty);
    expect(androidAudioMetrics(const {'audioEnabled': false}), isEmpty);
    final old = rows(const {'audioEnabled': true});
    expect(old['오디오 디코더'], '미측정');
    expect(old['오디오 포커스'], '미측정');
    expect(old['기기 음량'], '미측정');
    expect(old['오디오 재생'], '미측정');
    expect(rows({...playing, 'mediaVolume': 20})['기기 음량'], '미측정');
    expect(rows({...playing, 'mediaVolumeMax': 0})['기기 음량'], '미측정');
  });

  group('the Host send rate from stream.stats', () {
    Map<String, dynamic> stats(int sent) => {
      'type': 'stream.stats',
      'audioSend': {'sent': sent, 'dropped': 0},
    };

    test('packets per second over the last 3 s', () {
      final tracker = AudioStatsTracker();
      expect(tracker.hostAudioPerSecond(0), isNull);
      tracker.host(stats(100), nowUs: 1000000);
      expect(tracker.hostAudioPerSecond(1000000), 100);
      tracker.host(stats(100), nowUs: 2000000);
      tracker.host(stats(100), nowUs: 3000000);
      tracker.host(stats(100), nowUs: 4000000);
      expect(tracker.hostAudioPerSecond(4000000), 100);
      // A stream.stats without audioSend: the Host sent none in its window.
      for (var n = 5; n <= 8; n++) {
        tracker.host(const {'type': 'stream.stats'}, nowUs: n * 1000000);
      }
      expect(tracker.hostAudioPerSecond(8000000), 0);
      // None for a while: unknown, not "none sent".
      expect(tracker.hostAudioPerSecond(20000000), isNull);
      // A new stream starts over.
      tracker.host(stats(50), nowUs: 21000000);
      tracker.reset();
      expect(tracker.hostAudioPerSecond(21000000), isNull);
    });

    test('readings without a time keep the old behaviour', () {
      final tracker = AudioStatsTracker();
      tracker.host(stats(100));
      expect(tracker.hostAudioPerSecond(0), isNull);
      expect(tracker.read(const {'audioEnabled': true, 'audioUnderruns': 0})?.hostSent, 100);
    });
  });

  testWidgets('the 오디오 section fits a phone and reads in full', (t) async {
    const size = Size(360, 740);
    t.view.devicePixelRatio = 1;
    t.view.physicalSize = size;
    addTearDown(t.view.resetDevicePixelRatio);
    addTearDown(t.view.resetPhysicalSize);
    List<SessionMetricSection> sections() => [
      SessionMetricSection('오디오', [
        for (final (label, value) in androidAudioMetrics({
          ...playing,
          'audioFocus': 4,
          'mediaVolume': 0,
          'audioThreadErrors': 1,
        }, hostPerSecond: 0))
          SessionMetric(label, value),
      ]),
    ];
    await t.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(),
        builder: (_, child) => MediaQuery(
          data: const MediaQueryData(
            size: size,
            textScaler: TextScaler.linear(1.3),
          ),
          child: child!,
        ),
        home: Builder(
          builder: (context) => Scaffold(
            body: Center(
              child: TextButton(
                onPressed: () => showDialog<void>(
                  context: context,
                  builder: (_) => SessionStatsDialog(
                    title: '연결 통계',
                    machine: '데스크톱',
                    sections: sections,
                  ),
                ),
                child: const Text('열기'),
              ),
            ),
          ),
        ),
      ),
    );
    await t.tap(find.text('열기'));
    await t.pumpAndSettle();
    expect(t.takeException(), isNull);
    for (final text in [
      'Host가 보낸 오디오',
      '보내지 않음',
      '오디오 포커스',
      '다른 앱이 가져감 (음소거)',
      '기기 음량',
      '0/15 (소리 꺼짐)',
      '오디오 재생',
      '재생 중 · 끊김 2회 · 오류 1회',
    ]) {
      await t.scrollUntilVisible(
        find.text(text),
        120,
        scrollable: find.byType(Scrollable).first,
        maxScrolls: 30,
      );
      expect(find.text(text).hitTestable(), findsOneWidget, reason: text);
    }
    expect(t.takeException(), isNull);
  });
}
