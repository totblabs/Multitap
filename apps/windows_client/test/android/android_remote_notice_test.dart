// Pointer ownership and bounded renders; no device, real clipboard or printer.

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_print.dart';
import 'package:multitap_client/android/android_remote_notice.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/ui/app_theme.dart';

const _message = '단축키 전송 결과를 확인하지 못했어요. 원격 화면을 확인한 뒤 필요하면 다시 선택해 주세요.';

class _Printer extends PrintSession {
  _Printer()
    : super(
        url: Uri.parse('wss://fixture.invalid/sessions/print'),
        token: 'fixture-only',
        sessionId: 'fixture',
      );
  @override
  bool get receiving => true;
  @override
  int get receivedBytes => 1024;
  @override
  int get receivingSize => 4096;
}

void main() {
  setUpAll(() async {
    final font = FontLoader('Pretendard')
      ..addFont(rootBundle.load('assets/fonts/Pretendard-Regular.otf'));
    await font.load();
    final icons = FontLoader('MaterialIcons')
      ..addFont(rootBundle.load('fonts/MaterialIcons-Regular.otf'));
    await icons.load();
  });

  test('a notice stays long enough to read slowly, within its bounds', () {
    // Two sentences (44 letters) used to go after 2 s.
    const twoLines = '백그라운드 연결 유지를 시작하지 못했어요. 앱을 벗어나면 연결이 끊길 수 있어요.';
    expect(androidNoticeDuration(twoLines), greaterThanOrEqualTo(const Duration(seconds: 6)));
    expect(androidNoticeDuration('입력 방식 · 포인터'), const Duration(seconds: 3));
    expect(androidNoticeDuration(_message * 4), const Duration(seconds: 15));
    expect(
      androidNoticeDuration('데스크톱에 소리 장치가 없어요.', const Duration(seconds: 15)),
      const Duration(seconds: 15),
    );
  });

  testWidgets(
    'message, its padding and the gap pass touches; only the action button owns input',
    (tester) async {
      var pointers = 0, actions = 0;
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: Stack(
              children: [
                Positioned.fill(
                  child: Listener(
                    onPointerDown: (_) => pointers++,
                    onPointerUp: (_) => pointers++,
                    behavior: HitTestBehavior.opaque,
                    child: const ColoredBox(color: Colors.blueGrey),
                  ),
                ),
                Positioned(
                  top: 48,
                  left: 12,
                  right: 12,
                  child: AndroidRemoteNotice(
                    message: _message,
                    actionLabel: '붙여넣기',
                    onAction: () => actions++,
                  ),
                ),
              ],
            ),
          ),
        ),
      );
      final message = tester.getRect(
        find.byKey(const Key('android-remote-notice-message')),
      );
      final action = find.byKey(const Key('android-remote-notice-action'));
      await tester.tapAt(message.center);
      await tester.tapAt(message.topLeft + const Offset(2, 2));
      final gap = Offset(
        (message.right + tester.getRect(action).left) / 2,
        message.center.dy,
      );
      await tester.tapAt(gap);
      expect(pointers, 6);
      await tester.tap(action);
      expect(actions, 1);
      expect(
        pointers,
        6,
        reason: 'Only the explicit action consumes a remote tap',
      );
    },
  );

  for (final brightness in Brightness.values) {
    testWidgets(
      '320 dp, 200% text and keyboard keep notices compact and readable ($brightness)',
      (tester) async {
        final originalShadows = debugDisableShadows;
        debugDisableShadows = false;
        AndroidPrintOffers? offers;
        try {
          tester.view.physicalSize = const Size(320, 780);
          tester.view.devicePixelRatio = 1;
          tester.view.viewInsets = const FakeViewPadding(bottom: 250);
          addTearDown(tester.view.reset);
          final picture = GlobalKey();
          late BuildContext host;
          await tester.pumpWidget(
            RepaintBoundary(
              key: picture,
              child: MaterialApp(
                theme: buildAppTheme(brightness: brightness),
                debugShowCheckedModeBanner: false,
                builder: (context, child) => MediaQuery(
                  data: MediaQuery.of(context)
                      .copyWith(textScaler: TextScaler.linear(2)),
                  child: child!,
                ),
                home: Builder(
                  builder: (context) {
                    host = context;
                    return Scaffold(
                      resizeToAvoidBottomInset: false,
                      body: Stack(
                        children: [
                          const Positioned.fill(
                            child: ColoredBox(color: Color(0xff243b50)),
                          ),
                          Positioned(
                            top: 48,
                            left: 12,
                            right: 12,
                            child: AndroidRemoteNotice(
                              key: const Key('notice-render'),
                              message: _message,
                              actionLabel: '붙여넣기',
                              onAction: () {},
                            ),
                          ),
                        ],
                      ),
                    );
                  },
                ),
              ),
            ),
          );
          offers = AndroidPrintOffers(progressContext: () => host)
            ..watch(_Printer());
          await tester.pump(const Duration(milliseconds: 1300));
          final bounds = tester.getRect(find.byKey(const Key('notice-render')));
          final progress = tester.getRect(
            find.byKey(const Key('android-print-receive')),
          );
          expect(bounds.width, lessThanOrEqualTo(296));
          expect(bounds.bottom, lessThan(530));
          expect(progress.left, greaterThanOrEqualTo(12));
          expect(progress.right, lessThanOrEqualTo(308));
          expect(progress.bottom, lessThanOrEqualTo(518));
          expect(progress.height, lessThanOrEqualTo(58));
          // The whole message, even at 200 % text: nothing is cut.
          final text = tester.widget<Text>(find.text(_message));
          expect(text.maxLines, isNull);
          expect(text.overflow, isNull);
          expect(find.text('1 KB / 4 KB · 25%'), findsOneWidget);
          expect(tester.takeException(), isNull);
          offers.dispose();
          offers = null;
          await tester.pumpWidget(const SizedBox());
        } finally {
          offers?.dispose();
          debugDisableShadows = originalShadows;
        }
      },
    );
  }
}
