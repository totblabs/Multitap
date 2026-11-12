import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/android/android_print.dart';
import 'package:multitap_client/data/print_session.dart';
import 'package:multitap_client/ui/app_theme.dart';

class _Printer extends PrintSession {
  _Printer()
    : super(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/fixture/print'),
        token: 'not-persisted',
        sessionId: 'fixture',
      );
  bool arriving = false;
  int received = 0, total = 0;
  final finished = <PrintOutcome>[];
  @override
  bool get receiving => arriving;
  @override
  int get receivedBytes => received;
  @override
  int get receivingSize => total;

  void begin({int size = 4096}) {
    total = size;
    received = 0;
    arriving = true;
    notifyListeners();
  }

  void chunk(int bytes) {
    received = bytes;
    notifyListeners();
  }

  void abort({bool endChannel = false}) {
    arriving = false;
    if (endChannel) {
      closed = true;
      error = 'print_transport_closed';
    }
    notifyListeners();
  }

  void complete() {
    arriving = false;
    pending.add(
      PrintDocument(
        jobId: 'complete',
        title: '',
        size: total,
        file: File('unused-print-fixture.pdf'),
      ),
    );
    notifyListeners();
  }

  @override
  Future<void> finish(PrintDocument document, PrintOutcome outcome) async {
    pending.remove(document);
    finished.add(outcome);
    notifyListeners();
  }
}

class _Bridge implements AndroidPrintBridge {
  final shown = <PrintDocument>[];
  final result = Completer<bool>();
  @override
  Future<bool> print(PrintDocument document) {
    shown.add(document);
    return result.future;
  }
}

class _Harness {
  final printer = _Printer();
  final bridge = _Bridge();
  final messenger = GlobalKey<ScaffoldMessengerState>();
  late BuildContext context;
  late AndroidPrintOffers offers;
}

Future<_Harness> _open(
  WidgetTester tester, {
  Brightness brightness = Brightness.dark,
  double textScale = 1,
}) async {
  final h = _Harness();
  await tester.pumpWidget(
    MaterialApp(
      debugShowCheckedModeBanner: false,
      scaffoldMessengerKey: h.messenger,
      theme: buildAppTheme(brightness: brightness),
      builder: (context, child) => MediaQuery(
        data: MediaQuery.of(context)
            .copyWith(textScaler: TextScaler.linear(textScale)),
        child: child!,
      ),
      home: Builder(
        builder: (context) {
          h.context = context;
          return const Scaffold(body: Text('remote session'));
        },
      ),
    ),
  );
  h.offers = AndroidPrintOffers(
    bridge: h.bridge,
    progressContext: () => h.context.mounted ? h.context : null,
  )..watch(h.printer);
  addTearDown(h.offers.dispose);
  return h;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  setUpAll(() async {
    final font = FontLoader('Pretendard');
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      font.addFont(rootBundle.load('assets/fonts/Pretendard-$weight.otf'));
    }
    await font.load();
  });
  const notice = Key('android-print-receive');

  test(
    'a ready queue is silent but a later queue failure is still reported once',
    () {
      final printer = _Printer();
      final notices = <String>[];
      final offers = AndroidPrintOffers(onNotice: notices.add)..watch(printer);
      printer.queueReady = true;
      printer.abort();
      expect(notices, isEmpty);
      printer.queueReason = 'print_queue_failed';
      printer.abort();
      expect(notices, hasLength(1));
      printer.abort();
      expect(notices, hasLength(1));
      offers.dispose();
    },
  );

  testWidgets('a long actual receive shows bytes after the delay; only the '
      'verified completed document opens the native print screen', (
    tester,
  ) async {
    final h = await _open(tester);
    // A ready VM queue alone says nothing about document rendering progress.
    h.printer.queueReady = true;
    await tester.pump(const Duration(seconds: 2));
    expect(find.byKey(notice), findsNothing);
    h.printer.begin();
    await tester.pump(const Duration(milliseconds: 1199));
    expect(find.byKey(notice), findsNothing);
    expect(h.bridge.shown, isEmpty);
    await tester.pump(const Duration(milliseconds: 1));
    expect(find.byKey(notice), findsOneWidget);
    h.printer.chunk(1024);
    await tester.pump(const Duration(milliseconds: 250));
    expect(find.text('1 KB / 4 KB · 25%'), findsOneWidget);
    expect(
      tester
          .widget<LinearProgressIndicator>(
            find.byKey(const Key('print-receive-bar')),
          )
          .value,
      0.25,
    );
    expect(h.bridge.shown, isEmpty);
    h.printer.complete();
    await tester.pump();
    expect(find.byKey(notice), findsNothing);
    expect(h.bridge.shown, hasLength(1));
    expect(
      h.printer.finished,
      isEmpty,
      reason: 'the native choice is still open',
    );
    h.bridge.result.complete(true);
    await tester.pump();
    expect(h.printer.finished, [PrintOutcome.printed]);
  });

  testWidgets('a fast document never flashes a receive notice', (tester) async {
    final h = await _open(tester);
    h.printer.begin();
    await tester.pump(const Duration(milliseconds: 500));
    h.printer.complete();
    await tester.pump(const Duration(seconds: 3));
    expect(find.byKey(notice), findsNothing);
    expect(h.bridge.shown, hasLength(1));
    h.bridge.result.complete(false);
    await tester.pump();
    expect(h.printer.finished, [PrintOutcome.cancelled]);
  });

  for (final endChannel in [false, true]) {
    testWidgets('receive failure (closed=$endChannel) removes only its own '
        'notice and leaves other messages open', (tester) async {
      final h = await _open(tester);
      h.printer.begin();
      await tester.pump(const Duration(milliseconds: 1300));
      expect(find.byKey(notice), findsOneWidget);
      h.messenger.currentState!.showSnackBar(
        const SnackBar(
          content: Text('another notice'),
          duration: Duration(minutes: 1),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));
      h.printer.abort(endChannel: endChannel);
      await tester.pump();
      expect(find.byKey(notice), findsNothing);
      expect(find.text('another notice'), findsOneWidget);
      expect(h.bridge.shown, isEmpty);
      await tester.pump(const Duration(seconds: 2));
      expect(find.byKey(notice), findsNothing);
    });
  }

  testWidgets('replacing the session cancels its delayed receive notice and '
      'late updates cannot remove the new session notice', (tester) async {
    final h = await _open(tester);
    h.printer.begin();
    await tester.pump(const Duration(milliseconds: 600));
    final next = _Printer();
    h.offers.watch(next);
    next.begin(size: 8192);
    await tester.pump(const Duration(milliseconds: 700));
    expect(find.byKey(notice), findsNothing);
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.byKey(notice), findsOneWidget);
    next.chunk(4096);
    h.printer.abort(endChannel: true);
    await tester.pump(const Duration(milliseconds: 250));
    expect(find.text('4 KB / 8 KB · 50%'), findsOneWidget);
    next.abort();
    await tester.pump();
    expect(find.byKey(notice), findsNothing);
  });

  testWidgets('disposing removes the owned notice without dismissing a '
      'confirmation route or scheduling a late notice', (tester) async {
    final h = await _open(tester);
    h.printer.begin();
    await tester.pump(const Duration(milliseconds: 1300));
    unawaited(
      showDialog<void>(
        context: h.context,
        builder: (_) => const AlertDialog(content: Text('keep confirmation')),
      ),
    );
    await tester.pump(const Duration(milliseconds: 300));
    h.offers.dispose();
    await tester.pump();
    expect(find.byKey(notice), findsNothing);
    expect(find.text('keep confirmation'), findsOneWidget);
    h.printer.chunk(1024);
    await tester.pump(const Duration(seconds: 3));
    expect(find.byKey(notice), findsNothing);
    Navigator.of(h.context).pop();
    await tester.pumpAndSettle();
  });

  for (final brightness in Brightness.values) {
    testWidgets('narrow phone with 200 percent text and the keyboard keeps '
        'the receive bar visible ($brightness)', (tester) async {
      tester.view.physicalSize = const Size(320, 780);
      tester.view.devicePixelRatio = 1;
      tester.view.viewInsets = const FakeViewPadding(bottom: 250);
      addTearDown(tester.view.reset);
      final h = await _open(tester, brightness: brightness, textScale: 2);
      h.printer.begin();
      await tester.pump(const Duration(milliseconds: 1300));
      h.printer.chunk(1024);
      await tester.pump(const Duration(milliseconds: 250));
      expect(tester.takeException(), isNull);
      final bounds = tester.getRect(find.byKey(notice));
      expect(bounds.left, greaterThanOrEqualTo(12));
      expect(bounds.right, lessThanOrEqualTo(308));
      expect(bounds.bottom, lessThanOrEqualTo(518));
      h.printer.abort();
      await tester.pump();
    });
  }
}
