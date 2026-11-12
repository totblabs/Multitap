import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/ui/korean_text.dart';

void main() {
  group('sentenceLines', () {
    test('puts each Korean sentence on its own line', () {
      expect(
        sentenceLines(
          'Host에서 화면을 준비하지 못했어요. Windows 로그인·잠금과 가상 화면 상태를 확인한 뒤 다시 연결하세요.',
        ),
        'Host에서 화면을 준비하지 못했어요.\nWindows 로그인·잠금과 가상 화면 상태를 확인한 뒤 다시 연결하세요.',
      );
      expect(sentenceLines('끊겼어요. 다시 연결하세요. 계속되면 알려 주세요.'), '끊겼어요.\n다시 연결하세요.\n계속되면 알려 주세요.');
    });

    test('leaves single sentences, versions and existing breaks alone', () {
      expect(sentenceLines('다시 연결하세요.'), '다시 연결하세요.');
      expect(sentenceLines('버전 1.0.41 설치 파일이에요'), '버전 1.0.41 설치 파일이에요');
      expect(sentenceLines('끊겼어요.\n다시 연결하세요.'), '끊겼어요.\n다시 연결하세요.');
      expect(sentenceLines('연결이 끊겼어요 · 다시 연결하세요'), '연결이 끊겼어요 · 다시 연결하세요');
    });
  });

  testWidgets('KeepAllText renders one sentence per line and keeps its data', (
    tester,
  ) async {
    const message = '화면을 준비하지 못했어요. 다시 연결하세요.';
    await tester.pumpWidget(
      const MaterialApp(
        home: Scaffold(body: Center(child: KeepAllText(message))),
      ),
    );
    final first = tester.getTopLeft(find.byType(RichText).first);
    expect(find.text(message), findsOneWidget);
    final box = tester.getSize(find.byType(KeepAllText));
    // Two sentences: at least two text lines tall.
    expect(box.height, greaterThan(24));
    expect(first.dy, greaterThanOrEqualTo(0));
  });

  testWidgets('a single-line KeepAllText keeps both sentences on one line', (
    tester,
  ) async {
    const message = '미리보기 없음. 잠겨 있어요.';
    await tester.pumpWidget(
      const MaterialApp(
        home: Scaffold(
          body: Center(child: KeepAllText(message, maxLines: 1)),
        ),
      ),
    );
    expect(tester.getSize(find.byType(KeepAllText)).height, lessThan(24));
  });
}
