// Test-only public/synthetic data. No account, clipboard or network service.
import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/mfa_account.dart';
import 'package:multitap_client/data/mfa_account_controller.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/mfa_account_dialog.dart';
import 'package:multitap_client/ui/mfa_enrollment_qr.dart';

import 'helpers/mfa_account_fake.dart';

const _publicUri = 'otpauth://totp/Multitap%3A%ED%85%8C%EC%8A%A4%ED%8A%B8?secret=JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP&issuer=Multitap&algorithm=SHA1&digits=6&period=30';
const _paintKey = Key('mfa-enrollment-qr-paint');

class _Owner {
  final api = MfaTestApi(), inbox = MfaCompletionInbox();
  bool presentation = true, authenticated = true, disposed = false;
  int authenticationReads = 0;
  late final owner = MfaCompletionOwner(origin: api.origin,
    accountId: '11111111-1111-4111-8111-111111111111', username: 'synthetic',
    generation: 1, current: () => presentation);
  late final controller = MfaAccountController(api: api, owner: owner,
    token: 'synthetic-only', completed: inbox,
    currentAuthentication: () { authenticationReads++; return authenticated; });
  Future<MfaEnrollment> setup() async {
    await controller.refresh(); controller.choose(MfaAccountAction.enable);
    await controller.submitProof('synthetic-password');
    return controller.enrollment!;
  }
  void dispose() {
    if (disposed) { return; }
    disposed = true; controller.dispose(); inbox.dispose();
  }
}

MfaEnrollment _publicSetup({Duration ttl = const Duration(minutes: 2)}) => MfaEnrollment.parse({
  'protocol': 'mfa-v1', 'setupToken': 'a' * 43,
  'expiresAt': DateTime.now().add(ttl).toUtc().toIso8601String(),
  'secret': 'JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP', 'uri': _publicUri,
});

Future<void> _mount(WidgetTester tester, Widget child, {
  Brightness brightness = Brightness.dark, double textScale = 1,
}) async {
  await tester.pumpWidget(MaterialApp(theme: buildAppTheme(brightness: brightness),
    home: MediaQuery(data: MediaQueryData(size: tester.view.physicalSize / tester.view.devicePixelRatio,
      devicePixelRatio: tester.view.devicePixelRatio, textScaler: TextScaler.linear(textScale)),
      child: Scaffold(body: Center(child: child)))));
  await tester.pumpAndSettle();
}

Future<MfaEnrollment> _reveal(WidgetTester tester, _Owner f, {
  Brightness brightness = Brightness.dark, double textScale = 1,
}) async {
  await _mount(tester, MfaAccountDialog(controller: f.controller, onClose: () {}),
    brightness: brightness, textScale: textScale);
  f.controller.choose(MfaAccountAction.enable);
  await f.controller.submitProof('synthetic-password'); await tester.pumpAndSettle();
  final setup = f.controller.enrollment!;
  expect(find.byType(MfaEnrollmentQr), findsNothing);
  expect(find.byKey(const Key('mfa-account-secret')), findsNothing);
  final authenticationReads = f.authenticationReads;
  await tester.ensureVisible(find.byKey(const Key('mfa-account-reveal')));
  await tester.tap(find.byKey(const Key('mfa-account-reveal'))); await tester.pumpAndSettle();
  expect(f.authenticationReads, authenticationReads);
  expect(find.byType(MfaEnrollmentQr), findsOneWidget);
  expect(find.byKey(const Key('mfa-account-secret')), findsOneWidget);
  return setup;
}

({CustomPainter painter, Size size}) _painting(WidgetTester tester) =>
    (painter: tester.widget<CustomPaint>(find.byKey(_paintKey)).painter!,
     size: tester.getSize(find.byKey(_paintKey)));

typedef _RectPaint = ({Rect rect, Color color, bool antialias});
List<_RectPaint> _rectangles(({CustomPainter painter, Size size}) drawing) {
  final rects = <_RectPaint>[];
  expect((Canvas canvas) => drawing.painter.paint(canvas, drawing.size), paints..everything((method, args) {
    expect(method, #drawRect);
    final paint = args[1] as Paint;
    rects.add((rect: args[0] as Rect, color: paint.color, antialias: paint.isAntiAlias));
    return true;
  }));
  return rects;
}

void _currentPainting(({CustomPainter painter, Size size}) drawing, double dpr) {
  final rects = _rectangles(drawing);
  expect(rects.first.rect, Offset.zero & drawing.size); expect(rects.first.color, Colors.white);
  final dark = rects.skip(1).toList(); expect(dark, isNotEmpty);
  final module = dark.first.rect.width;
  expect(module * dpr, closeTo((module * dpr).roundToDouble(), 0.000001));
  var left = drawing.size.width, top = drawing.size.height, right = 0.0, bottom = 0.0;
  for (final row in rects) { expect(row.antialias, isFalse); }
  for (final row in dark) {
    expect(row.color, Colors.black); expect(row.rect.width, closeTo(module, 0.000001));
    expect(row.rect.height, closeTo(module, 0.000001));
    if (row.rect.left < left) left = row.rect.left;
    if (row.rect.top < top) top = row.rect.top;
    if (row.rect.right > right) right = row.rect.right;
    if (row.rect.bottom > bottom) bottom = row.rect.bottom;
  }
  expect(left, closeTo(4 * module, 0.000001)); expect(top, closeTo(4 * module, 0.000001));
  expect(drawing.size.width - right, closeTo(4 * module, 0.000001));
  expect(drawing.size.height - bottom, closeTo(4 * module, 0.000001));
}

void _blank(({CustomPainter painter, Size size}) drawing) {
  final rows = _rectangles(drawing); expect(rows, hasLength(1));
  expect(rows.single.color, Colors.white); expect(rows.single.rect, Offset.zero & drawing.size);
}

void _copy(WidgetTester tester, Key key) {
  final finder = find.descendant(of: find.byKey(key),
    matching: find.byWidgetPredicate((widget) => widget is EditableText));
  expect(finder, findsOneWidget);
  final selected = tester.state<EditableTextState>(finder);
  selected.selectAll(SelectionChangedCause.keyboard);
  final inner = find.byElementPredicate((element) => element is RenderObjectElement &&
    identical(element.renderObject, selected.renderEditable));
  expect(inner, findsOneWidget);
  Actions.invoke(tester.element(inner), CopySelectionTextIntent.copy);
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  final writes = <String>[];
  setUp(() {
    writes.clear();
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(SystemChannels.platform, (call) async {
      if (call.method == 'Clipboard.setData') writes.add((call.arguments as Map)['text'] as String);
      if (call.method == 'Clipboard.hasStrings') return {'value': false};
      return null;
    });
    messenger.setMockMethodCallHandler(SystemChannels.processText, (call) async {
      if (call.method == 'ProcessText.queryTextActions') return <String, String>{};
      throw StateError('qr_test_external_process_action_forbidden');
    });
  });
  tearDown(() {
    final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    messenger.setMockMethodCallHandler(SystemChannels.platform, null);
    messenger.setMockMethodCallHandler(SystemChannels.processText, null);
  });
  for (final platform in [TargetPlatform.windows, TargetPlatform.android]) {
    void testPlatform(String name, Future<void> Function(WidgetTester) body) {
      testWidgets('${platform.name} $name', (tester) async {
        final old = debugDefaultTargetPlatformOverride; debugDefaultTargetPlatformOverride = platform;
        tester.view.physicalSize = const Size(800, 1200); tester.view.devicePixelRatio = 2;
        try { await body(tester); }
        finally { debugDefaultTargetPlatformOverride = old; tester.view.reset(); }
      });
    }
    testPlatform('explicit reveal jointly shows QR and manual guarded copy without auth reads', (tester) async {
      final f = _Owner(), semantics = tester.ensureSemantics();
      try {
        await _reveal(tester, f);
        final reads = f.authenticationReads;
        _currentPainting(_painting(tester), 2);
        expect(f.authenticationReads, reads);
        final nodes = find.semantics.byLabel('인증 앱으로 스캔할 설정 QR 코드').evaluate().toList();
        expect(nodes, hasLength(1)); final label = nodes.single.getSemanticsData().label;
        expect(label, '인증 앱으로 스캔할 설정 QR 코드');
        expect(label.contains(f.controller.enrollment!.secret), isFalse);
        await tester.ensureVisible(find.byKey(const Key('mfa-account-secret'))); await tester.pump();
        _copy(tester, const Key('mfa-account-secret')); await tester.idle();
        expect(writes, [f.controller.enrollment!.secret]);
        expect(f.api.calls, ['status', 'setup']);
      } finally { await tester.pumpWidget(const SizedBox.shrink()); f.dispose(); semantics.dispose(); }
    });
    for (final reason in ['hide', 'owner', 'dispose']) {
      testPlatform('$reason invalidates a retained actual painter without replay', (tester) async {
        final f = _Owner();
        try {
          final setup = await _reveal(tester, f), drawing = _painting(tester);
          _currentPainting(drawing, 2);
          if (reason == 'hide') {
            await tester.ensureVisible(find.byKey(const Key('mfa-account-reveal')));
            await tester.tap(find.byKey(const Key('mfa-account-reveal')));
            expect(setup.secret, isNotEmpty);
          } else if (reason == 'owner') {
            f.presentation = false; f.inbox.retire(f.owner); f.controller.authenticationRetired();
          } else {
            await tester.pumpWidget(const SizedBox.shrink());
            expect(identical(f.controller.enrollment, setup), isTrue);
          }
          _blank(drawing); await tester.pump();
          if (reason == 'hide') {
            expect(find.byType(MfaEnrollmentQr), findsNothing);
            expect(find.byKey(const Key('mfa-account-secret')), findsNothing);
          }
          expect(writes, isEmpty);
        } finally { await tester.pumpWidget(const SizedBox.shrink()); f.dispose(); }
      });
    }
    testPlatform('real setup TTL blocks old paint before timer processing with pure model observation', (tester) async {
      final f = _Owner(); f.api.setup = () async => _publicSetup(ttl: const Duration(seconds: 3));
      try {
        final setup = await f.setup();
        await _mount(tester, MfaEnrollmentQr(controller: f.controller, enrollment: setup, visible: () => true));
        final drawing = _painting(tester); _currentPainting(drawing, 2);
        final reads = f.authenticationReads; var notifications = 0;
        f.controller.addListener(() => notifications++);
        final delay = setup.remaining + const Duration(milliseconds: 80);
        expect(setup.remaining, greaterThan(Duration.zero)); expect(delay, lessThan(const Duration(seconds: 4)));
        await tester.runAsync(() => Future<void>.delayed(delay));
        expect(identical(f.controller.enrollment, setup), isTrue);
        expect(f.controller.phase, MfaAccountPhase.enrollment);
        expect(f.controller.canDisplayEnrollment(setup), isFalse);
        _blank(drawing);
        expect(f.authenticationReads, reads); expect(notifications, 0); expect(writes, isEmpty);
      } finally { await tester.pumpWidget(const SizedBox.shrink()); f.dispose(); }
    });
    testPlatform('sameURI successor binding retires old painter and keeps current matrix', (tester) async {
      final old = _Owner(), next = _Owner();
      try {
        final before = await old.setup(), after = await next.setup();
        expect(before.uri, after.uri);
        await _mount(tester, MfaEnrollmentQr(key: const Key('same-qr-leaf'),
          controller: old.controller, enrollment: before, visible: () => true));
        final retired = _painting(tester); _currentPainting(retired, 2);
        await _mount(tester, MfaEnrollmentQr(key: const Key('same-qr-leaf'),
          controller: next.controller, enrollment: after, visible: () => true));
        _blank(retired); _currentPainting(_painting(tester), 2);
        expect(old.owner.current, isTrue); expect(next.owner.current, isTrue); expect(writes, isEmpty);
      } finally { await tester.pumpWidget(const SizedBox.shrink()); old.dispose(); next.dispose(); }
    });
    testPlatform('pending enable survives QR removal and late ownACK still permits current recovery copy', (tester) async {
      final f = _Owner();
      try {
        await _reveal(tester, f); final drawing = _painting(tester);
        final reply = Completer<MfaAccountChange>(); f.api.change = (_) => reply.future;
        final work = f.controller.enable('123456');
        f.authenticated = false; f.controller.authenticationRetired();
        expect(f.inbox.pendingFor(f.owner), isTrue); _blank(drawing);
        reply.complete(mfaChange()); await work;
        expect(f.inbox.completed!.codes, mfaCodes);
        await _mount(tester, MfaCompletedRecoveryDialog(inbox: f.inbox, onFinished: () {}));
        _copy(tester, const Key('mfa-completed-codes')); await tester.idle();
        expect(writes, hasLength(1)); expect(f.authenticated, isFalse);
        f.inbox.retire(f.owner);
        _copy(tester, const Key('mfa-completed-codes')); await tester.idle();
        expect(writes, hasLength(1)); expect(f.api.calls, ['status', 'setup', 'enable']);
      } finally { await tester.pumpWidget(const SizedBox.shrink()); f.dispose(); }
    });
    for (final brightness in [Brightness.light, Brightness.dark]) {
      testPlatform('320px200percent $brightness keeps QR/manual key/factor scrollable and hittable', (tester) async {
        tester.view.physicalSize = const Size(640, 1480);
        final f = _Owner();
        try {
          await _reveal(tester, f, brightness: brightness, textScale: 2);
          await tester.ensureVisible(find.byKey(const Key('mfa-account-enrollment-qr'))); await tester.pumpAndSettle();
          final rect = tester.getRect(find.byKey(_paintKey));
          expect(rect.left, greaterThanOrEqualTo(0)); expect(rect.right, lessThanOrEqualTo(320));
          expect(rect.top, greaterThanOrEqualTo(0)); expect(rect.bottom, lessThanOrEqualTo(740));
          _currentPainting(_painting(tester), 2);
          await tester.ensureVisible(find.byKey(const Key('mfa-account-secret'))); await tester.pump();
          _copy(tester, const Key('mfa-account-secret')); await tester.idle(); expect(writes, hasLength(1));
          // _copy selects the key "by keyboard", and the key's own text then
          // scrolls its selection into view over 100 ms. Let that end before
          // scrolling on: started in the same frame it pulled the dialog
          // back up, which the taller field (its name is above it) does not
          // survive by 3 px.
          await tester.pumpAndSettle();
          await tester.ensureVisible(find.byKey(const Key('mfa-account-factor'))); await tester.pumpAndSettle();
          expect(find.byKey(const Key('mfa-account-factor')).hitTestable(), findsOneWidget);
          // The whole field is in view, not only its middle.
          expect(
            tester.getRect(find.byKey(const Key('mfa-account-factor'))).bottom,
            lessThanOrEqualTo(tester.getRect(find.byType(SingleChildScrollView).first).bottom + .5),
          );
          expect(tester.getSize(find.byKey(const Key('mfa-account-reveal'))).height, greaterThanOrEqualTo(48));
          expect(tester.takeException(), isNull);
        } finally { await tester.pumpWidget(const SizedBox.shrink()); f.dispose(); }
      });
    }
  }
}
