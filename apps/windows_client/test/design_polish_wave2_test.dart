// Design polish wave 2: what the shared form parts and their surfaces
// promise. Widgets only; nothing outside the test
// process is touched.
//
//   F-23 one field and one select: the label above, 42 px (48 for a finger),
//        a chevron, the label still read out as the control's name
//   F-11 one fold: a chevron on the left and a label that is never amber
//   F-06 the sign-in card is a product surface: the server as a line, no
//        key drawn on the button, guidance under the card, version aside
//   F-39 a sign-in failure is an icon and a line; the card keeps its height
//   F-27 the state of a device or a folder is a dot and ordinary text
//   F-25 what a dialog's buttons do stays in view above them
//   F-01 the connecting card lists the stages: a check on the stages above
//        the one it is at, the mark and the time on the stage it is at
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/connection_feedback.dart';
import 'package:multitap_client/ui/app_dialog.dart';
import 'package:multitap_client/ui/connection_wait_status.dart';
import 'package:multitap_client/ui/session_status_panel.dart';
import 'package:multitap_client/ui/sign_in_screen.dart';

import 'auth_flow_test.dart' show TestLoginMemory;

import 'package:multitap_client/ui/app_form.dart';
import 'package:multitap_client/ui/app_theme.dart';

Future<void> _pump(
  WidgetTester tester,
  Widget home, {
  Brightness brightness = Brightness.dark,
  TargetPlatform platform = TargetPlatform.windows,
  double scale = 1,
}) async {
  tester.view.devicePixelRatio = 1;
  tester.view.physicalSize = const Size(900, 700);
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildAppTheme(brightness: brightness).copyWith(platform: platform),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context)
            .copyWith(textScaler: TextScaler.linear(scale)),
        child: child!,
      ),
      home: Scaffold(
        body: Center(child: SizedBox(width: 360, child: home)),
      ),
    ),
  );
  await tester.pump();
}

void main() {
  group('F-01 the stages of a connection', () {
    String now(WidgetTester tester) => tester
        .widget<Text>(find.byKey(const Key('connection-wait-step')))
        .data!;
    List<String> done(WidgetTester tester) => [
      for (final step in ConnectionWaitStep.values)
        if (find
            .byKey(ValueKey('connection-wait-done-${step.name}'))
            .evaluate()
            .isNotEmpty)
          step.label,
    ];

    testWidgets(
      'every stage is listed; the stages above the one it is at are checked',
      (tester) async {
        var step = ConnectionWaitStep.request;
        Object attempt = 1;
        Widget panel() => ConnectionWaitStatus(
          step: step,
          attempt: attempt,
          elapsed: () => const Duration(seconds: 4),
        );
        await _pump(tester, panel());
        for (final stage in ConnectionWaitStatus.stages) {
          expect(find.text(stage.label), findsOneWidget);
        }
        expect(now(tester), '연결 요청');
        expect(done(tester), isEmpty);
        expect(find.byKey(const Key('connection-wait-now')), findsOneWidget);
        // The time is on the row of the stage it is at, to the right.
        final row = tester.getRect(
          find.byKey(const Key('connection-wait-step')),
        );
        final time = tester.getRect(
          find.byKey(const Key('connection-wait-elapsed')),
        );
        expect(find.text('경과 00:04'), findsOneWidget);
        expect(time.left, greaterThan(row.left));
        expect((time.center.dy - row.center.dy).abs(), lessThan(4));

        // The mark moves down the list; what is above it is checked.
        step = ConnectionWaitStep.relay;
        await _pump(tester, panel());
        expect(now(tester), '서버 연결');
        expect(done(tester), ['연결 요청']);
        step = ConnectionWaitStep.display;
        await _pump(tester, panel());
        expect(now(tester), '가상 화면 준비');
        expect(done(tester), ['연결 요청', '서버 연결']);
        step = ConnectionWaitStep.firstFrame;
        await _pump(tester, panel());
        expect(now(tester), '첫 화면 수신 대기');
        expect(done(tester), ['연결 요청', '가상 화면 준비', '서버 연결']);
        // Nothing below the mark keeps a check when a connection starts over.
        step = ConnectionWaitStep.relay;
        await _pump(tester, panel());
        expect(done(tester), ['연결 요청']);
        step = ConnectionWaitStep.firstFrame;
        await _pump(tester, panel());

        // A new attempt starts with nothing checked.
        attempt = 2;
        step = ConnectionWaitStep.request;
        await _pump(tester, panel());
        expect(done(tester), isEmpty);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      },
    );

    testWidgets('a stage passed too fast to be drawn is over, not left empty', (
      tester,
    ) async {
      var step = ConnectionWaitStep.request;
      Widget panel() => ConnectionWaitStatus(step: step, attempt: 1);
      await _pump(tester, panel());
      step = ConnectionWaitStep.display;
      await _pump(tester, panel());
      expect(done(tester), ['연결 요청', '서버 연결']);
      expect(
        find.byKey(const ValueKey('connection-wait-done-firstFrame')),
        findsNothing,
      );
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('a wait outside the four stages is a row above them', (
      tester,
    ) async {
      for (final wait in [
        ConnectionWaitStep.reconnect,
        ConnectionWaitStep.hostUpdate,
        ConnectionWaitStep.vmRestart,
      ]) {
        await _pump(tester, ConnectionWaitStatus(step: wait, attempt: wait));
        expect(now(tester), wait.label);
        expect(
          tester.getRect(find.text(wait.label)).bottom,
          lessThanOrEqualTo(tester.getRect(find.text('연결 요청')).top),
        );
        expect(done(tester), isEmpty);
        await tester.pumpWidget(const SizedBox.shrink());
      }
    });

    testWidgets('the chip form stays one line', (tester) async {
      await _pump(
        tester,
        const ConnectionWaitStatus(
          step: ConnectionWaitStep.reconnect,
          compact: true,
        ),
      );
      // A state, not the words of the 다시 연결 button beside it.
      expect(find.text('다시 연결 중'), findsOneWidget);
      expect(find.text('다시 연결'), findsNothing);
      expect(find.text('연결 요청'), findsNothing);
      expect(find.byKey(const Key('connection-wait-elapsed')), findsOneWidget);
      await tester.pumpWidget(const SizedBox.shrink());
    });

    testWidgets('the card: the list under the message, 연결 취소 at the right', (
      tester,
    ) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(900, 700);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.light),
          home: Scaffold(
            body: SessionStatusPanel(
              busy: true,
              machine: 'TESTER',
              message: 'Host 연결을 준비하고 있어요.',
              backLabel: '연결 취소',
              onBack: () {},
              status: const ConnectionWaitStatus(
                step: ConnectionWaitStep.relay,
              ),
            ),
          ),
        ),
      );
      final message = tester.getRect(find.text('Host 연결을 준비하고 있어요.'));
      final list = tester.getRect(
        find.byKey(const Key('connection-wait-status')),
      );
      final cancel = tester.getRect(
        find.byKey(const Key('leave-remote-connection')),
      );
      expect(list.top, greaterThan(message.bottom));
      expect(cancel.top, greaterThan(list.bottom));
      // Everything starts at one left edge; the lone button sits right.
      expect(list.left, message.left);
      expect(cancel.right, list.right);
      expect(cancel.width, lessThan(list.width / 2));
      expect(cancel.width, greaterThanOrEqualTo(AppDialog.loneActionWidth));
      expect(tester.takeException(), isNull);
      await tester.pumpWidget(const SizedBox.shrink());
    });
  });

  group('F-06, F-39 the sign-in card', () {
    Future<void> signIn(
      WidgetTester tester, {
      String? error,
      Uri? origin,
      Brightness brightness = Brightness.dark,
      Size size = const Size(1280, 800),
    }) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = size;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: brightness)
              .copyWith(platform: TargetPlatform.windows),
          home: SignInScreen(
            onLogin: (_, _) async => SignInResult.rejected,
            serverOrigin: origin,
            onServerChanged: (_) async {},
            loginMemory: TestLoginMemory(),
            error: error,
          ),
        ),
      );
      await tester.pumpAndSettle();
    }

    final server = Uri.parse('https://vdi.example.com');

    testWidgets('the product surface: the tile, 18 px corners, 12 px fields', (
      tester,
    ) async {
      for (final brightness in Brightness.values) {
        await signIn(tester, origin: server, brightness: brightness);
        final p = AppPalette(brightness == Brightness.dark);
        final card =
            tester
                    .widget<Container>(find.byKey(const Key('login-card')))
                    .decoration!
                as BoxDecoration;
        expect(card.color, p.tile);
        expect(card.borderRadius, BorderRadius.circular(18));
        expect((card.border! as Border).top.color, p.tileLine);
        for (final key in ['sign-in-id', 'sign-in-password']) {
          final field = tester.widget<TextField>(find.byKey(Key(key)));
          expect(field.decoration!.labelText, isNull, reason: key);
          expect(tester.getSize(find.byKey(Key(key))).height, 42, reason: key);
        }
        expect(find.text('아이디'), findsOneWidget);
        expect(find.text('비밀번호'), findsOneWidget);
        expect(tester.takeException(), isNull);
      }
    });

    testWidgets('the server is a line at the head of the card, not a field', (
      tester,
    ) async {
      await signIn(tester, origin: server);
      final line = find.byKey(const Key('open-server-settings'));
      expect(find.text('vdi.example.com'), findsOneWidget);
      expect(find.text('서버 변경'), findsOneWidget);
      expect(
        find.descendant(of: line, matching: find.byType(Ink)),
        findsNothing,
        reason: 'no filled box of its own',
      );
      expect(
        tester.getRect(line).bottom,
        lessThanOrEqualTo(
          tester.getRect(find.byKey(const Key('sign-in-id'))).top,
        ),
      );
      // It still opens the address form in place.
      await tester.tap(line);
      await tester.pumpAndSettle();
      expect(find.byKey(const Key('login-server-form')), findsOneWidget);
      // Without a server nothing is said twice.
      await tester.pumpWidget(const SizedBox.shrink());
      await signIn(tester);
      expect(find.text('서버 설정 필요'), findsOneWidget);
      expect(find.text('서버 변경'), findsNothing);
      // The address form starts open; closed, the button names the way back.
      expect(find.byKey(const Key('login-server-form')), findsOneWidget);
      await tester.tap(find.byKey(const Key('cancel-server-settings')));
      await tester.pumpAndSettle();
      expect(find.text('서버 설정하고 시작'), findsOneWidget);
    });

    testWidgets('the button has no key drawn on it; Enter still signs in', (
      tester,
    ) async {
      var logins = 0;
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 800);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: SignInScreen(
            onLogin: (_, _) async {
              logins++;
              return SignInResult.rejected;
            },
            serverOrigin: server,
            onServerChanged: (_) async {},
            loginMemory: TestLoginMemory(),
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('Enter'), findsNothing);
      expect(
        find.descendant(
          of: find.byKey(const Key('login-submit')),
          matching: find.byWidgetPredicate((widget) => widget is Text),
        ),
        findsOneWidget,
      );
      await tester.enterText(find.byKey(const Key('sign-in-id')), 'tester');
      await tester.enterText(
        find.byKey(const Key('sign-in-password')),
        'fixture-password',
      );
      await tester.testTextInput.receiveAction(TextInputAction.done);
      await tester.pumpAndSettle();
      expect(logins, 1);
    });

    testWidgets('guidance under the card, the version in the corner', (
      tester,
    ) async {
      await signIn(tester, origin: server);
      final card = tester.getRect(find.byKey(const Key('login-card')));
      final guidance = tester.getRect(find.byKey(const Key('login-footer')));
      final version = tester.getRect(find.byKey(const Key('login-app-info')));
      expect(
        tester.widget<Text>(find.byKey(const Key('login-footer'))).data,
        '계정은 관리자에게 요청하세요',
      );
      expect(guidance.top, greaterThan(card.bottom));
      expect(version.left, lessThan(card.left));
      expect(version.bottom, greaterThan(800 - 60));
      // A window with no room beside the card keeps the version under it.
      await signIn(tester, origin: server, size: const Size(420, 760));
      final narrow = tester.getRect(find.byKey(const Key('login-app-info')));
      expect(
        narrow.top,
        greaterThanOrEqualTo(
          tester.getRect(find.byKey(const Key('login-footer'))).bottom,
        ),
      );
      expect(tester.takeException(), isNull);
    });

    testWidgets('a failure is an icon and a 13 px line above the button, and '
        'the card keeps its height', (tester) async {
      await signIn(tester, origin: server);
      final before = tester.getRect(find.byKey(const Key('login-card')));
      await signIn(tester, origin: server, error: '아이디 또는 비밀번호를 확인해 주세요.');
      final after = tester.getRect(find.byKey(const Key('login-card')));
      expect(after, before);
      final line = find.byKey(const Key('login-error'));
      final text = tester.widget<Text>(line);
      expect(text.style!.fontSize, 13);
      expect(text.style!.color, AppPalette(true).ink);
      expect(find.byIcon(Icons.error_outline_rounded), findsOneWidget);
      expect(
        tester.getRect(line).bottom,
        lessThanOrEqualTo(
          tester.getRect(find.byKey(const Key('login-submit'))).top,
        ),
      );
      expect(
        tester.getRect(line).top,
        greaterThanOrEqualTo(
          tester.getRect(find.byKey(const Key('remember-password'))).top,
        ),
      );
    });
  });

  group('F-27, F-25 states and notices', () {
    testWidgets(
      'the state of a row: a dot in its colour, an ordinary sentence',
      (tester) async {
        for (final brightness in Brightness.values) {
          final p = AppPalette(brightness == Brightness.dark);
          for (final (tone, ink) in [
            (AppTone.success, p.muted),
            (AppTone.info, p.muted),
            (AppTone.neutral, p.muted),
            (AppTone.warn, p.ink),
            (AppTone.danger, p.ink),
          ]) {
            await _pump(
              tester,
              AppStatusLine(
                '데스크톱에 연결됨',
                textKey: const Key('state'),
                tone: tone,
                size: 12,
                dot: true,
              ),
              brightness: brightness,
            );
            // (A change of theme is animated.)
            await tester.pumpAndSettle();
            expect(find.byType(Icon), findsNothing, reason: '$tone');
            final dot = tester.widget<Container>(
              find.descendant(
                of: find.byType(AppStatusLine),
                matching: find.byType(Container),
              ),
            );
            expect((dot.decoration! as BoxDecoration).color, p.tone(tone));
            expect(
              tester.widget<Text>(find.byKey(const Key('state'))).style!.color,
              ink,
              reason: '$tone',
            );
          }
        }
      },
    );

    testWidgets('a dialog footer stays in view while the body scrolls', (
      tester,
    ) async {
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(700, 420);
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildAppTheme(brightness: Brightness.dark),
          home: Scaffold(
            body: AppDialog(
              title: '일괄 업데이트',
              body: [for (var i = 0; i < 40; i++) Text('줄 $i')],
              footer: const Text('연결을 끊고 바로 업데이트해요.', key: Key('note')),
              actions: [
                TextButton(onPressed: () {}, child: const Text('닫기')),
                FilledButton(
                  key: const Key('go'),
                  onPressed: () {},
                  child: const Text('업데이트'),
                ),
              ],
            ),
          ),
        ),
      );
      final note = find.byKey(const Key('note'));
      expect(note.hitTestable(), findsOneWidget);
      expect(
        find.ancestor(of: note, matching: find.byType(SingleChildScrollView)),
        findsNothing,
      );
      expect(
        tester.getRect(note).bottom,
        lessThanOrEqualTo(tester.getRect(find.byKey(const Key('go'))).top),
      );
      expect(tester.getRect(find.byKey(const Key('go'))).bottom, lessThan(420));
      expect(tester.takeException(), isNull);
    });
  });

  group('F-23 fields and selects', () {
    testWidgets('a field has its label above it and is 42 px high', (
      tester,
    ) async {
      for (final brightness in Brightness.values) {
        await _pump(
          tester,
          Builder(
            builder: (context) => AppField(
              label: '현재 비밀번호',
              help: '10자 이상',
              child: TextField(
                key: const Key('field'),
                decoration: AppField.decoration(context),
              ),
            ),
          ),
          brightness: brightness,
        );
        final label = tester.getRect(find.text('현재 비밀번호'));
        final field = tester.getRect(find.byKey(const Key('field')));
        expect(label.bottom, lessThanOrEqualTo(field.top));
        expect(field.height, 42);
        expect(
          tester.getRect(find.text('10자 이상')).top,
          greaterThan(field.bottom),
        );
        // Nothing floats into the border: the decoration has no label.
        expect(
          tester
              .widget<TextField>(find.byKey(const Key('field')))
              .decoration!
              .labelText,
          isNull,
        );
        expect(tester.takeException(), isNull);
      }
    });

    testWidgets('a finger gets 48 px', (tester) async {
      await _pump(
        tester,
        Builder(
          builder: (context) => AppField(
            label: '아이디',
            child: TextField(
              key: const Key('field'),
              decoration: AppField.decoration(context),
            ),
          ),
        ),
        platform: TargetPlatform.android,
      );
      expect(
        tester.getSize(find.byKey(const Key('field'))).height,
        greaterThanOrEqualTo(48),
      );
    });

    testWidgets('the label is the name the field is read out with, once', (
      tester,
    ) async {
      final handle = tester.ensureSemantics();
      await _pump(
        tester,
        Builder(
          builder: (context) => AppField(
            label: '인증 코드',
            child: TextField(
              key: const Key('field'),
              decoration: AppField.decoration(context),
            ),
          ),
        ),
      );
      expect(find.bySemanticsLabel('인증 코드'), findsOneWidget);
      final node = tester.getSemantics(find.byKey(const Key('field')));
      expect(node.label, '인증 코드');
      handle.dispose();
    });

    testWidgets('an error takes the place of the help line', (tester) async {
      await _pump(
        tester,
        Builder(
          builder: (context) => AppField(
            label: '주소',
            help: '서버 주소를 입력하세요',
            error: '주소를 확인하세요',
            errorKey: const Key('error'),
            child: TextField(decoration: AppField.decoration(context)),
          ),
        ),
      );
      expect(find.byKey(const Key('error')), findsOneWidget);
      expect(find.text('서버 주소를 입력하세요'), findsNothing);
      // The colour is the icon's, not the sentence's.
      final p = AppPalette(true);
      expect(
        tester.widget<Text>(find.byKey(const Key('error'))).style!.color,
        p.ink,
      );
    });

    testWidgets('a select is the same well with a chevron, and still chooses', (
      tester,
    ) async {
      String? chosen = 'a';
      await _pump(
        tester,
        StatefulBuilder(
          builder: (context, setState) => AppSelect<String>(
            fieldKey: const Key('select'),
            label: '프린터',
            value: chosen,
            items: const [
              DropdownMenuItem(value: 'a', child: Text('첫째')),
              DropdownMenuItem(value: 'b', child: Text('둘째')),
            ],
            onChanged: (value) => setState(() => chosen = value),
          ),
        ),
      );
      final label = tester.getRect(find.text('프린터'));
      final field = tester.getRect(find.byKey(const Key('select')));
      expect(label.bottom, lessThanOrEqualTo(field.top));
      expect(field.height, 42);
      expect(find.byIcon(Icons.keyboard_arrow_down_rounded), findsOneWidget);
      expect(find.byIcon(Icons.arrow_drop_down), findsNothing);
      expect(find.byType(DropdownButtonFormField<String>), findsOneWidget);
      await tester.tap(find.byKey(const Key('select')));
      await tester.pumpAndSettle();
      await tester.tap(find.text('둘째').last);
      await tester.pumpAndSettle();
      expect(chosen, 'b');
      expect(tester.takeException(), isNull);
    });
  });

  group('F-11 one fold', () {
    testWidgets('it keeps its own state and shows what is folded', (
      tester,
    ) async {
      final handle = tester.ensureSemantics();
      for (final brightness in Brightness.values) {
        await _pump(
          tester,
          const AppDisclosure(
            headerKey: Key('fold'),
            label: '자세히',
            children: [Text('접힌 내용')],
          ),
          brightness: brightness,
        );
        final p = AppPalette(brightness == Brightness.dark);
        expect(find.text('접힌 내용'), findsNothing);
        expect(find.byIcon(Icons.chevron_right_rounded), findsOneWidget);
        // The chevron is on the left of the label.
        expect(
          tester.getRect(find.byIcon(Icons.chevron_right_rounded)).right,
          lessThanOrEqualTo(tester.getRect(find.text('자세히')).left),
        );
        // Secondary ink, 13/600: never the amber of a main action.
        final style = tester
            .widget<DefaultTextStyle>(
              find
                  .ancestor(
                    of: find.text('자세히'),
                    matching: find.byType(DefaultTextStyle),
                  )
                  .first,
            )
            .style;
        expect(style.color, p.ink2);
        expect(style.fontSize, 13);
        expect(style.fontWeight, FontWeight.w600);
        expect(
          tester.getSemantics(find.byKey(const Key('fold'))),
          isSemantics(isButton: true, hasExpandedState: true),
        );
        await tester.tap(find.byKey(const Key('fold')));
        await tester.pumpAndSettle();
        expect(find.text('접힌 내용'), findsOneWidget);
        expect(
          tester.widget<AnimatedRotation>(find.byType(AnimatedRotation)).turns,
          .25,
        );
        await tester.tap(find.byKey(const Key('fold')));
        await tester.pumpAndSettle();
        expect(find.text('접힌 내용'), findsNothing);
        expect(tester.takeException(), isNull);
        await tester.pumpWidget(const SizedBox.shrink());
      }
      handle.dispose();
    });

    testWidgets('a caller that owns the state is told, and decides', (
      tester,
    ) async {
      var open = false;
      final asked = <bool>[];
      await _pump(
        tester,
        StatefulBuilder(
          builder: (context, setState) => AppDisclosure(
            headerKey: const Key('fold'),
            label: open ? '접기' : '고급 설정',
            open: open,
            onChanged: (value) {
              asked.add(value);
              setState(() => open = value);
            },
          ),
        ),
      );
      await tester.tap(find.byKey(const Key('fold')));
      await tester.pumpAndSettle();
      expect(asked, [true]);
      expect(find.text('접기'), findsOneWidget);
      await tester.tap(find.byKey(const Key('fold')));
      await tester.pumpAndSettle();
      expect(asked, [true, false]);
    });

    testWidgets('maintainState keeps what is folded alive, unseen', (
      tester,
    ) async {
      await _pump(
        tester,
        const AppDisclosure(
          headerKey: Key('fold'),
          label: '고급 설정',
          maintainState: true,
          children: [TextField(key: Key('kept'))],
        ),
      );
      expect(find.byKey(const Key('kept')), findsNothing);
      expect(
        find.byKey(const Key('kept'), skipOffstage: false),
        findsOneWidget,
      );
      await tester.tap(find.byKey(const Key('fold')));
      await tester.pumpAndSettle();
      await tester.enterText(find.byKey(const Key('kept')), '남아 있음');
      await tester.tap(find.byKey(const Key('fold')));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const Key('fold')));
      await tester.pumpAndSettle();
      expect(find.text('남아 있음'), findsOneWidget);
    });
  });
}
