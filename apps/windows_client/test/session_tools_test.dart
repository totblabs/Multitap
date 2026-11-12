// The shared toolbar model (SessionTool) both platforms build from is the
// order and names the parity test checks on screen.
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/session_tools.dart';

import 'session_tools_parity_test.dart' show sessionToolOrder;

void main() {
  test('SessionTool is the toolbar order with one name each', () {
    expect([
      for (final tool in SessionTool.values) (tool.name, tool.label),
    ], sessionToolOrder);
    // Both platforms use the one name.
    for (final tool in SessionTool.values) {
      expect(tool.androidLabel, tool.label, reason: tool.name);
    }
  });

  test('groups follow the order: chip, 화면, 폴더·USB, 설정, window, '
      'disconnect', () {
    final groups = [for (final tool in SessionTool.values) tool.group];
    expect(groups, [...groups]..sort());
    expect(SessionTool.quality.group, 0);
    expect(SessionTool.disconnect.group, groups.last);
    // The bar's three groups and what each holds.
    List<SessionTool> of(int group) => [
      for (final tool in SessionTool.values)
        if (tool.group == group) tool,
    ];
    expect(of(0), [SessionTool.quality]);
    expect(of(1), [SessionTool.screen, SessionTool.scale]);
    expect(of(2), [SessionTool.share, SessionTool.drives, SessionTool.usb]);
    expect(of(3), [
      SessionTool.more,
      SessionTool.settings,
      SessionTool.gameMouse,
      SessionTool.pin,
      SessionTool.guide,
    ]);
    expect(of(4), [SessionTool.minimize, SessionTool.restore]);
    expect(of(5), [SessionTool.disconnect]);
    // Only the three groups have a caption (Android, the narrow 메뉴).
    expect(SessionTool.caption(1), '화면');
    expect(SessionTool.caption(2), '공유');
    expect(SessionTool.caption(3), '설정');
    for (final group in [0, 4, 5, 6]) {
      expect(SessionTool.caption(group), isNull, reason: '$group');
    }
  });

  test('ordered puts known entries in the toolbar order and keeps the rest '
      'after them in their own order', () {
    final ordered = SessionTool.ordered([
      '긴급 콘솔 안내',
      '사용안내',
      '화면',
      'Ctrl+Alt+Delete 보내기',
      '배율',
    ], (label) => label);
    expect(ordered, ['화면', '배율', '사용안내', '긴급 콘솔 안내', 'Ctrl+Alt+Delete 보내기']);
    // The Windows bar's three buttons, however the caller lists them.
    expect(SessionTool.ordered(['설정', 'USB 장치', '화면'], (label) => label), [
      '화면',
      'USB 장치',
      '설정',
    ]);
    expect(SessionTool.ordered(['설정', '폴더·USB', '화면'], (label) => label), [
      '화면',
      '폴더·USB',
      '설정',
    ]);
    expect(SessionTool.named('마우스 잠금'), SessionTool.gameMouse);
    expect(
      SessionTool.named(SessionTool.scale.androidLabel),
      SessionTool.scale,
    );
    // '설정' is the group's button; the streaming choices are
    // 화질, and the pin is 바 열어 두기.
    expect(SessionTool.named('설정'), SessionTool.more);
    expect(SessionTool.named('화질'), SessionTool.settings);
    expect(SessionTool.named('폴더·USB'), SessionTool.share);
    expect(SessionTool.named('툴바 열어 두기'), SessionTool.pin);
    expect(SessionTool.named('창 모드로'), SessionTool.restore);
    expect(SessionTool.named('스트리밍 설정'), isNull);
    // Not names of the vocabulary.
    expect(SessionTool.named('게임 마우스'), isNull);
    expect(SessionTool.named('마우스 가두기'), isNull);
    expect(SessionTool.named('리모컨'), isNull);
    // Every icon is a real glyph.
    for (final tool in SessionTool.values) {
      expect(tool.icon, isA<IconData>());
    }
  });

  test('the measured values keep their ids and order, and nothing is '
      'invented without a session', () {
    final values = sessionQualityValues(null, device: 'PC');
    expect(
      [for (final value in values) value.id],
      ['resolution', 'fps', 'rate', 'path', 'encode', 'decode'],
    );
    expect(
      [for (final value in values) value.label],
      [
        '해상도 · 원격 화면',
        '화면 · PC 디코딩',
        '수신 · 받은 영상',
        '영상 경로',
        '인코딩 · Host 처리',
        '디코딩 · PC 처리',
      ],
    );
    expect(
      [for (final value in values) value.value],
      ['—', '—', '—', '확인 중', '—', '—'],
    );
    // The device's name is the caller's ('PC', '이 기기').
    final android = sessionQualityValues(null, device: '이 기기');
    expect(android[1].label, '화면 · 이 기기 디코딩');
    expect(android[5].label, '디코딩 · 이 기기 처리');
    // The quality menu's rows are built on the same values, then 통계.
    var stats = 0;
    final items = sessionQualityItems(
      null,
      device: 'PC',
      onStats: () => stats++,
    );
    expect(
      [for (final item in items) item.label],
      [for (final value in values) value.label, '연결 통계 보기'],
    );
    expect(
      [for (final item in items.take(values.length)) item.value],
      [for (final value in values) value.value],
    );
    expect(items.last.key, const Key('quality-stats'));
    items.last.onSelected!();
    expect(stats, 1);
  });
}
