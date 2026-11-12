import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/clipboard_session.dart';
import 'package:multitap_client/data/paste_target.dart';
import 'package:multitap_client/data/transfer_notice_window.dart';
import 'package:multitap_client/ui/app_theme.dart';
import 'package:multitap_client/ui/clipboard_transfer_card.dart';
import 'package:multitap_client/ui/session_menu.dart';

class _Clipboard extends ClipboardSession {
  _Clipboard()
    : super(
        url: Uri.parse('wss://fixture.invalid/api/v1/sessions/q/clipboard'),
        token: 'fixture-only',
        sessionId: 'q',
      );
  ClipboardProgress? current;
  bool running = false, isClosed = false, waiting = true;
  @override
  bool get userWaiting => waiting;
  int cancels = 0;
  @override
  bool get transferring => running;
  @override
  bool get closed => isClosed;
  @override
  ClipboardProgress? get progress => current;
  @override
  Future<void> cancelTransfer() async => cancels++;

  void begin([ClipboardProgress? progress]) {
    current = progress;
    running = true;
    notifyListeners();
  }

  void update(ClipboardProgress progress) {
    current = progress;
    notifyListeners();
  }

  void end({String? code, String message = '클립보드 전송 완료', bool quiet = false}) {
    running = false;
    current = null;
    error = code;
    status = message;
    failureQuiet = quiet;
    notifyListeners();
  }
}

class _Window implements TransferNoticeWindow, TransferNoticeOpenAction {
  final updates = <TransferNoticeContent>[];
  final taskbars = <(TaskbarProgressMode, int)>[];
  int hides = 0;
  @override
  VoidCallback? onCancel, onClose, onOpen;
  @override
  Future<void> update(TransferNoticeContent content) async =>
      updates.add(content);
  @override
  Future<void> hide() async => hides++;
  @override
  Future<void> taskbar(TaskbarProgressMode mode, int percent) async =>
      taskbars.add((mode, percent));
  TransferNoticeContent get last => updates.last;
}

const _mb = 1048576;
final _epoch = DateTime(2026, 11, 2, 9);

/// Explorer's side, faked: the folder in front, and when items there were created.
class _Probe implements PasteTargetProbe {
  _Probe(this.folder, [Map<String, DateTime>? created])
    : created_ = created ?? {};
  PasteDestination? folder;
  final Map<String, DateTime> created_;
  int asked = 0;
  final opened = <(String, String?)>[];
  @override
  Future<PasteDestination?> foregroundFolder() async {
    asked++;
    return folder;
  }

  @override
  Future<DateTime?> created(String path) async => created_[path];

  @override
  Future<bool> open(String folder, {String? select}) async {
    opened.add((folder, select));
    return true;
  }
}

const _downloads = PasteDestination(
  path: r'C:\Users\<user>\Downloads',
  name: '다운로드',
);

ClipboardProgress _paste({
  required int moved,
  int total = 40 * _mb,
  List<String> top = const ['사진'],
}) => ClipboardProgress(
  sending: false,
  format: 'tree',
  fileCount: 12,
  folderCount: 2,
  fileIndex: 3,
  transferred: moved,
  total: total,
  fileName: 'a.jpg',
  topLevel: top,
);

ClipboardProgress _files({
  required int moved,
  int total = 40 * _mb,
  bool sending = false,
  int count = 3,
  int index = 1,
  String? name = 'report-final.pdf',
  bool async = false,
}) => ClipboardProgress(
  sending: sending,
  format: 'files',
  fileCount: count,
  fileIndex: index,
  transferred: moved,
  total: total,
  fileName: name,
  asyncConsumer: async,
);

void main() {
  late _Clipboard clipboard;
  late ClipboardTransferController controller;
  // The controller's timers end with the test, before the framework looks for stray ones.
  void cardTest(String name, Future<void> Function(WidgetTester tester) body) {
    testWidgets(name, (tester) async {
      try {
        await body(tester);
      } finally {
        await tester.pumpWidget(const SizedBox());
        controller.dispose();
      }
    });
  }

  late _Window window;
  late Duration now;
  late List<String> toasts;

  Future<void> mount(
    WidgetTester tester, {
    bool dark = true,
    Size size = const Size(1000, 700),
    double textScale = 1.0,
    TransferNoticeWindow? useWindow,
    int interactions = 0,
    VoidCallback? onInteract,
    PasteTargetProbe? probe,
  }) async {
    clipboard = _Clipboard();
    window = _Window();
    now = Duration.zero;
    toasts = [];
    controller = ClipboardTransferController(
      clipboard: clipboard,
      window: useWindow ?? window,
      remoteName: 'WorkPC',
      pasteTargets: probe,
      wallClock: () => _epoch.add(now),
      dark: dark,
      clock: () => now,
      onFailureWithoutCard: toasts.add,
    );
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildAppTheme(brightness: Brightness.dark),
        builder: (context, child) => MediaQuery(
          data: MediaQuery.of(context)
              .copyWith(textScaler: TextScaler.linear(textScale)),
          child: child!,
        ),
        home: Scaffold(
          body: Stack(
            children: [
              Align(
                alignment: Alignment.bottomRight,
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: ClipboardTransferCard(
                    controller: controller,
                    dark: dark,
                    onInteract: onInteract,
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Future<void> advance(WidgetTester tester, int ms) async {
    // Pump in the sampling step so each timer fires with the clock where it should be.
    var left = ms;
    while (left > 0) {
      final step = left > 50 ? 50 : left;
      now += Duration(milliseconds: step);
      await tester.pump(Duration(milliseconds: step));
      left -= step;
    }
  }

  final card = find.byKey(const Key('clipboard-transfer'));
  double? barValue(WidgetTester tester) => tester
      .widget<LinearProgressIndicator>(
        find.byKey(const Key('clipboard-progress-bar')),
      )
      .value;

  cardTest(
    'old controller cleanup cannot hide or clear a replacement cancel button',
    (tester) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb));
      await advance(tester, 800);
      final oldController = controller, oldClipboard = clipboard;
      final oldGeneration = window.last.generation;
      clipboard = _Clipboard();
      controller = ClipboardTransferController(
        clipboard: clipboard,
        window: window,
        clock: () => now,
      );
      clipboard.begin(_files(moved: 2 * _mb));
      await advance(tester, 800);
      final hides = window.hides;
      expect(window.last.generation, greaterThan(oldGeneration));
      oldController.dispose();
      expect(window.hides, hides);
      expect(window.onCancel, isNotNull);
      window.onCancel!();
      expect(clipboard.cancels, 1);
      expect(oldClipboard.cancels, 0);
    },
  );

  group('when the card appears', () {
    cardTest(
      'an on-paste Explorer consumer uses our card after 700 ms and cancels through it',
      (tester) async {
        await mount(tester);
        // In the on-paste revision ClipboardPulls leaves asyncConsumer false even
        // after Explorer StartOperation: FD_PROGRESSUI is absent from the offer.
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 650);
        expect(card, findsNothing);
        expect(window.updates, isEmpty);
        await advance(tester, 100);
        expect(card, findsOneWidget);
        expect(window.last.kind, 0);
        await tester.tap(find.text('취소'));
        await tester.pump();
        expect(clipboard.cancels, 1);
      },
    );
    cardTest('a copy that finishes within 700 ms never shows one', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb));
      await advance(tester, 600);
      expect(card, findsNothing);
      clipboard.end();
      await advance(tester, 1000);
      expect(card, findsNothing);
      expect(window.updates, isEmpty);
      expect(window.taskbars, isEmpty);
      expect(toasts, isEmpty);
    });

    cardTest(
      'one still running after 700 ms shows the route, the count, bar, sizes and 취소',
      (tester) async {
        await mount(tester);
        clipboard.begin(_files(moved: 12 * _mb + _mb ~/ 3));
        await advance(tester, 650);
        expect(card, findsNothing);
        await advance(tester, 100);
        expect(card, findsOneWidget);
        expect(find.text('WorkPC → PC'), findsOneWidget);
        expect(find.text('3개 중 1개'), findsOneWidget);
        expect(find.text('30%'), findsOneWidget);
        expect(find.text('12.3 MB / 40.0 MB'), findsOneWidget);
        expect(barValue(tester), closeTo(0.308, 0.01));
        expect(find.text('취소'), findsOneWidget);
        // The file in progress is one tap away, under 자세히.
        expect(find.text('report-final.pdf'), findsNothing);
        await tester.tap(find.byKey(const Key('clipboard-details-toggle')));
        await tester.pump();
        expect(find.text('report-final.pdf'), findsOneWidget);
        expect(find.text('파일 3개'), findsOneWidget);
        // The card window outside the session window and the taskbar got the same.
        expect(window.last.kind, 0);
        expect(window.last.title, 'WorkPC → PC');
        expect(window.last.subtitle, '3개 중 1개');
        expect(window.last.detail, '12.3 MB / 40.0 MB');
        expect(window.last.name, 'report-final.pdf');
        expect(window.last.details, [
          ('옮기는 파일', 'report-final.pdf'),
          ('항목', '파일 3개'),
        ]);
        expect(window.last.percent, 30);
        expect(window.last.sending, isFalse);
        expect(window.last.cancellable, isTrue);
        expect(window.taskbars.last, (TaskbarProgressMode.normal, 30));
      },
    );

    cardTest('this PC to the VM reads the other way round', (tester) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb, sending: true, count: 1));
      await advance(tester, 800);
      expect(find.text('PC → WorkPC'), findsOneWidget);
      expect(find.text('report-final.pdf'), findsOneWidget,
          reason: 'one file: its name is what moves');
      expect(window.last.sending, isTrue);
      expect(window.last.subtitleTip, 'report-final.pdf');
    });

    cardTest('large counts are grouped: 1,702개 중 120개', (tester) async {
      await mount(tester);
      clipboard.begin(
        const ClipboardProgress(
          sending: false,
          format: 'tree',
          fileCount: 1702,
          folderCount: 297,
          fileIndex: 120,
          transferred: 113 * _mb,
          total: 1843 * _mb,
          fileName: '김정훈_고급2.jpg',
        ),
      );
      await advance(tester, 800);
      expect(find.text('1,702개 중 120개'), findsOneWidget);
      expect(window.last.details, contains(('항목', '폴더 297개 · 파일 1,702개')));
    });

    cardTest('preparing, then moving, then finishing while Explorer writes', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin(
        const ClipboardProgress(
          sending: false,
          format: 'tree',
          fileCount: 0,
          fileIndex: 0,
          transferred: 0,
          total: 0,
        ),
      );
      await advance(tester, 800);
      expect(find.text('준비하는 중…'), findsOneWidget);
      expect(barValue(tester), isNull);
      expect(find.byKey(const Key('clipboard-percent')), findsNothing);
      expect(window.last.percent, isNull);
      clipboard.update(_files(moved: 20 * _mb));
      await advance(tester, 300);
      expect(find.text('준비하는 중…'), findsNothing);
      expect(find.text('50%'), findsOneWidget);
      for (var moved = 21; moved < 40; moved++) {
        clipboard.update(_files(moved: moved * _mb));
        await advance(tester, 100);
      }
      expect(window.last.detail, contains('/s'));
      // Every byte handed over is not done: Explorer may still be writing.
      clipboard.update(_files(moved: 40 * _mb));
      await advance(tester, 300);
      expect(controller.view.stage, TransferStage.finishing);
      expect(find.text('마무리하는 중…'), findsOneWidget);
      expect(window.last.detail, '40.0 MB / 40.0 MB', reason: 'no rate then');
      expect(controller.phase, TransferPhase.running);
      expect(find.text('옮겼어요'), findsNothing);
      expect(window.last.kind, 0);
      expect(window.last.trailing, '마무리하는 중…');
      clipboard.end();
      await tester.pump();
      expect(card, findsNothing);
      expect(find.text('옮겼어요'), findsNothing);
    });

    cardTest('plain text shows a card only when it lasts long', (tester) async {
      await mount(tester);
      const text = ClipboardProgress(
        sending: true,
        format: 'text',
        fileCount: 0,
        fileIndex: 1,
        transferred: 10,
        total: 100,
      );
      clipboard.begin(text);
      await advance(tester, 1400);
      expect(card, findsNothing);
      await advance(tester, 200);
      expect(card, findsOneWidget);
      expect(find.text('PC → WorkPC'), findsOneWidget);
      expect(find.text('텍스트'), findsOneWidget);
      expect(find.byKey(const Key('clipboard-progress-bar')), findsOneWidget);
      expect(barValue(tester), isNull, reason: 'text has no meaningful bar');
    });

    cardTest('a size not known yet is an indeterminate bar', (tester) async {
      await mount(tester);
      clipboard.begin(
        const ClipboardProgress(
          sending: true,
          format: '',
          fileCount: 0,
          fileIndex: 0,
          transferred: 0,
          total: 0,
        ),
      );
      await advance(tester, 800);
      expect(card, findsOneWidget);
      expect(barValue(tester), isNull);
      expect(find.byKey(const Key('clipboard-progress-size')), findsNothing);
      expect(window.last.percent, isNull);
      expect(window.taskbars.last.$1, TaskbarProgressMode.indeterminate);
      clipboard.update(_files(moved: 10 * _mb, total: 100 * _mb));
      await advance(tester, 300);
      expect(barValue(tester), closeTo(0.1, 0.001));
      expect(window.taskbars.last, (TaskbarProgressMode.normal, 10));
    });

    cardTest('says what moves: folders, an image', (tester) async {
      await mount(tester);
      clipboard.begin(
        const ClipboardProgress(
          sending: true,
          format: 'tree',
          fileCount: 41,
          folderCount: 2,
          fileIndex: 1,
          transferred: 1024,
          total: 4096,
        ),
      );
      await advance(tester, 800);
      expect(find.text('PC → WorkPC'), findsOneWidget);
      expect(find.text('41개 중 1개'), findsOneWidget);
      await tester.tap(find.byKey(const Key('clipboard-details-toggle')));
      await tester.pump();
      expect(find.text('폴더 2개 · 파일 41개'), findsOneWidget);
      clipboard.update(
        const ClipboardProgress(
          sending: false,
          format: 'image',
          fileCount: 0,
          fileIndex: 1,
          transferred: 2 * _mb,
          total: 8 * _mb,
        ),
      );
      await advance(tester, 300);
      expect(find.text('WorkPC → PC'), findsOneWidget);
      expect(find.text('이미지'), findsOneWidget);
    });

    cardTest('the rate and the time left appear once they are steady', (
      tester,
    ) async {
      await mount(tester);
      var moved = 0;
      clipboard.begin(_files(moved: moved, total: 100 * _mb));
      await advance(tester, 700);
      final left = find.byKey(const Key('clipboard-progress-left'));
      for (var ms = 0; ms < 6000; ms += 250) {
        moved += 250 * 1000; // 1 MB/s (decimal)
        clipboard.update(_files(moved: moved, total: 100 * _mb));
        await advance(tester, 250);
        // Not before the estimate is warm and steady.
        if (ms < 2000) expect(left, findsNothing, reason: 'at $ms ms');
      }
      final detail = tester
          .widget<Text>(find.byKey(const Key('clipboard-progress-size')))
          .data!;
      expect(detail, contains('/s'));
      final eta = tester.widget<Text>(left).data!;
      expect(eta, startsWith('약 '));
      expect(eta, endsWith('분 남음'));
      expect(window.last.trailing, eta);
    });

    cardTest('the rate holds for a second instead of jumping every sample', (
      tester,
    ) async {
      await mount(tester);
      var moved = 0;
      clipboard.begin(_files(moved: moved, total: 400 * _mb));
      await advance(tester, 700);
      final shown = <String>[];
      const bursts = [100, 900, 300, 1200, 200, 700, 1500, 400];
      for (var ms = 0; ms < 4000; ms += 250) {
        // Bursty arrivals: the measured rate differs at every sample.
        moved += bursts[(ms ~/ 250) % bursts.length] * 1000;
        clipboard.update(_files(moved: moved, total: 400 * _mb));
        await advance(tester, 250);
        final detail = controller.view.detail;
        if (detail.contains('/s')) shown.add(detail.split(' · ').last);
      }
      var changes = 0;
      for (var i = 1; i < shown.length; i++) {
        if (shown[i] != shown[i - 1]) changes++;
      }
      // 13 samples with a rate; at most one change a second.
      expect(shown.length, greaterThanOrEqualTo(10));
      expect(changes, lessThanOrEqualTo(4));
    });
  });

  group('nobody waiting', () {
    cardTest(
      'a background sync of a long copy shows no card, window, taskbar or done, and fails silently',
      (tester) async {
        await mount(tester);
        clipboard.waiting = false;
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 3000);
        expect(card, findsNothing);
        clipboard.end();
        await advance(tester, 2000);
        expect(card, findsNothing);
        expect(window.updates, isEmpty);
        expect(window.taskbars, isEmpty);
        expect(window.hides, 0);
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 3000);
        clipboard.end(code: 'clipboard_write_failed', message: '실패');
        await advance(tester, 100);
        expect(toasts, isEmpty);
        expect(card, findsNothing);
      },
    );

    cardTest(
      'a paste that starts waiting later brings the card of a transfer that has run long',
      (tester) async {
        await mount(tester);
        clipboard.waiting = false;
        clipboard.begin(_files(moved: 5 * _mb));
        await advance(tester, 2000);
        expect(card, findsNothing);
        clipboard.waiting = true;
        clipboard.update(_files(moved: 6 * _mb));
        await tester.pump();
        expect(card, findsOneWidget);
        expect(window.updates, isNotEmpty);
        // It was waited for, so its failure is told.
        clipboard.end(code: 'clipboard_disk_full', message: '공간 부족');
        await tester.pump();
        expect(find.text('공간 부족'), findsOneWidget);
      },
    );

    cardTest(
      'a copy that failed quickly while a paste waited says why in a toast',
      (tester) async {
        await mount(tester);
        clipboard.begin();
        await advance(tester, 100);
        clipboard.end(
          code: 'clipboard_total_size_limit',
          message: '파일이 너무 커요.',
        );
        expect(toasts, ['파일이 너무 커요.']);
      },
    );
  });

  group('the card', () {
    cardTest('collapses to a pill with the percent and opens again', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin(_files(moved: 20 * _mb));
      await advance(tester, 800);
      await tester.tap(find.byKey(const Key('clipboard-collapse')));
      await tester.pump();
      expect(find.byKey(const Key('clipboard-pill')), findsOneWidget);
      expect(find.text('50%'), findsOneWidget);
      expect(find.text('취소'), findsNothing);
      clipboard.update(_files(moved: 30 * _mb));
      await advance(tester, 300);
      expect(find.text('75%'), findsOneWidget);
      await tester.tap(find.byKey(const Key('clipboard-pill')));
      await tester.pump();
      expect(find.byKey(const Key('clipboard-pill')), findsNothing);
      expect(find.text('취소'), findsOneWidget);
    });

    cardTest(
      '취소 cancels once, says so, and a user cancel ends with a brief 취소했어요, no error',
      (tester) async {
        await mount(tester);
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 800);
        await tester.tap(find.byKey(const Key('clipboard-cancel')));
        await tester.pump();
        expect(clipboard.cancels, 1);
        expect(find.text('취소 중…'), findsOneWidget);
        expect(window.last.cancelling, isTrue);
        expect(window.last.cancellable, isFalse);
        // A second press does nothing.
        await tester.tap(
          find.byKey(const Key('clipboard-cancel')),
          warnIfMissed: false,
        );
        expect(clipboard.cancels, 1);
        clipboard.end(code: 'clipboard_cancelled', message: '클립보드 전송을 취소했어요');
        await advance(tester, 300);
        expect(find.text('전송을 취소했어요'), findsOneWidget);
        expect(find.text('옮기지 못했어요'), findsNothing);
        expect(find.byKey(const Key('clipboard-cancel')), findsNothing);
        expect(controller.phase, TransferPhase.cancelled);
        expect(window.last.kind, 3);
        expect(window.taskbars.last.$1, TaskbarProgressMode.none);
        await advance(tester, 2000);
        expect(card, findsNothing);
        expect(toasts, isEmpty);
        expect(window.hides, greaterThan(0));
      },
    );

    cardTest('the card window\'s 취소 cancels the same transfer', (tester) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb));
      await advance(tester, 800);
      window.onCancel!();
      await tester.pump();
      expect(clipboard.cancels, 1);
      expect(find.text('취소 중…'), findsOneWidget);
    });

    for (final format in ['files', 'tree']) {
      for (final sending in [false, true]) {
        for (final asyncConsumer in [false, true]) {
          cardTest('file handoff is not Explorer completion: $format sending=$sending async=$asyncConsumer', (tester) async {
            await mount(tester);
            clipboard.begin(ClipboardProgress(sending: sending, format: format, fileCount: 1,
              fileIndex: 1, transferred: _mb, total: 2 * _mb, fileName: 'existing.xlsx', asyncConsumer: asyncConsumer));
            await advance(tester, 800);
            clipboard.end();
            await tester.pump();
            expect(controller.phase, TransferPhase.hidden,
              reason: 'transport completion does not establish an overwrite choice or a destination write');
            expect(find.text('옮겼어요'), findsNothing);
            expect(window.updates.where((value) => value.kind == 1), isEmpty);
            expect(window.taskbars.last.$1, TaskbarProgressMode.none);
            expect(toasts, isEmpty);
          });
        }
      }
    }

    cardTest('file handoff retires progress without claiming destination success', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb));
      await advance(tester, 800);
      clipboard.end();
      await tester.pump();
      expect(find.text('옮겼어요'), findsNothing);
      expect(card, findsNothing);
      expect(find.byKey(const Key('clipboard-progress-bar')), findsNothing);
      expect(find.byKey(const Key('clipboard-open-folder')), findsNothing,
          reason: 'no folder is known without a probe');
      expect(window.updates.where((value) => value.kind == 1), isEmpty);
      expect(window.taskbars.last.$1, TaskbarProgressMode.none);
      await advance(tester, 3600);
      expect(find.text('옮겼어요'), findsNothing);
      expect(controller.fading, isFalse);
      await advance(tester, 200);
      expect(controller.fading, isFalse);
      await advance(tester, 300);
      expect(card, findsNothing);
      expect(window.hides, 1);
    });

    cardTest('a failure stays with its cause and next step until 닫기', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb));
      await advance(tester, 800);
      clipboard.end(
        code: 'clipboard_disk_full',
        message: 'VM의 디스크 공간이 부족해요. 공간을 확보한 뒤 다시 복사해 주세요.',
      );
      await tester.pump();
      expect(find.text('옮기지 못했어요'), findsOneWidget);
      expect(find.text('VM의 디스크 공간이 부족해요.'), findsOneWidget);
      expect(find.text('공간을 확보한 뒤 다시 복사해 주세요.'), findsOneWidget);
      expect(window.last.message, 'VM의 디스크 공간이 부족해요.');
      expect(window.last.hint, '공간을 확보한 뒤 다시 복사해 주세요.');
      expect(window.last.kind, 2);
      expect(window.taskbars.last, (TaskbarProgressMode.error, 100));
      await advance(tester, 20000);
      expect(card, findsOneWidget, reason: 'a failure does not fade');
      // The taskbar's red bar is brief.
      expect(window.taskbars.last.$1, TaskbarProgressMode.none);
      expect(toasts, isEmpty, reason: 'the card says it; no second notice');
      await tester.tap(find.byKey(const Key('clipboard-close')));
      await tester.pump();
      expect(card, findsNothing);
      expect(window.hides, 1);
    });

    cardTest('a failure the card window closes is closed here too', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb));
      await advance(tester, 800);
      clipboard.end(
        code: 'clipboard_write_failed',
        message: '붙여넣기를 준비하지 못했어요.',
      );
      await tester.pump();
      window.onClose!();
      await tester.pump();
      expect(card, findsNothing);
    });

    cardTest(
      'closing the card window while it runs leaves the transfer and the card',
      (tester) async {
        await mount(tester);
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 800);
        window.onClose!();
        await tester.pump();
        expect(card, findsOneWidget);
        expect(clipboard.cancels, 0);
      },
    );

    cardTest('a copy that failed before any card says why in a toast', (
      tester,
    ) async {
      await mount(tester);
      clipboard.begin();
      await advance(tester, 100);
      clipboard.end(code: 'clipboard_total_size_limit', message: '파일이 너무 커요.');
      await tester.pump();
      expect(toasts, ['파일이 너무 커요.']);
      // Expected, harmless outcomes stay silent.
      clipboard.begin();
      await advance(tester, 100);
      clipboard.end(code: 'clipboard_empty', message: '복사한 내용이 없어요.');
      clipboard.begin();
      await advance(tester, 100);
      clipboard.end(code: 'x', message: 'quiet', quiet: true);
      expect(toasts.length, 1);
    });

    cardTest('a newer copy replaces the older card at once', (tester) async {
      await mount(tester);
      clipboard.begin(_files(moved: _mb));
      await advance(tester, 800);
      clipboard.end(code: 'clipboard_write_failed', message: '첫 번째 실패');
      await tester.pump();
      expect(find.text('첫 번째 실패'), findsOneWidget);
      final generation = window.last.generation;
      clipboard.begin(_files(moved: 0, name: 'second.zip', count: 1));
      await tester.pump();
      expect(find.text('첫 번째 실패'), findsNothing);
      expect(card, findsNothing, reason: 'the new copy waits its 700 ms');
      await advance(tester, 800);
      expect(find.text('second.zip'), findsOneWidget);
      expect(window.last.generation, isNot(generation));
      expect(window.last.kind, 0);
    });

    cardTest('a long name is shortened in the middle, keeping the extension', (
      tester,
    ) async {
      await mount(tester);
      final whole = '${'아주아주' * 20}마지막부분.mp4';
      clipboard.begin(_files(moved: _mb, name: whole, count: 1));
      await advance(tester, 800);
      final shown = find.byKey(const Key('clipboard-transfer-name'));
      final name = tester.widget<Text>(shown).data!;
      expect(name.length, lessThanOrEqualTo(44));
      expect(name, contains('…'));
      expect(name, endsWith('.mp4'));
      // The whole name is a hover away.
      expect(
        find.ancestor(of: shown, matching: find.byType(Tooltip)),
        findsOneWidget,
      );
      expect(
        tester
            .widget<Tooltip>(
              find.ancestor(of: shown, matching: find.byType(Tooltip)),
            )
            .message,
        whole,
      );
      expect(window.last.subtitleTip, whole);
    });

    cardTest(
      'Explorer\'s own copy dialog is the progress: no card, the taskbar stays',
      (tester) async {
        await mount(tester);
        clipboard.begin(_files(moved: 5 * _mb, async: true));
        await advance(tester, 800);
        expect(card, findsNothing);
        expect(window.updates, isEmpty);
        expect(window.hides, greaterThan(0));
        expect(window.taskbars.last, (TaskbarProgressMode.normal, 12));
        // A consumer that reads synchronously gets our card.
        clipboard.update(_files(moved: 6 * _mb));
        await advance(tester, 300);
        expect(card, findsOneWidget);
        expect(window.updates, isNotEmpty);
      },
    );

    cardTest(
      'the pointer going down on the card lets the session release its capture',
      (tester) async {
        var interactions = 0;
        await mount(tester, onInteract: () => interactions++);
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 800);
        await tester.tap(find.byKey(const Key('clipboard-collapse')));
        expect(interactions, 1);
      },
    );

    cardTest(
      'without a card window (Android) the in-session card still works',
      (tester) async {
        // A controller with no window: nothing to push to, nothing throws.
        clipboard = _Clipboard();
        now = Duration.zero;
        controller = ClipboardTransferController(
          clipboard: clipboard,
          clock: () => now,
        );
        await tester.pumpWidget(
          MaterialApp(
            home: Scaffold(body: ClipboardTransferCard(controller: controller)),
          ),
        );
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 800);
        expect(card, findsOneWidget);
        clipboard.end();
        await advance(tester, 4500);
        expect(card, findsNothing);
      },
    );
  });

  group('where a paste on this PC goes', () {
    const photos = r'C:\Users\<user>\Downloads\사진';

    cardTest(
      'the folder in front, where the pasted item was just created, is named and opened',
      (tester) async {
        final probe = _Probe(_downloads, {
          photos: _epoch.add(const Duration(milliseconds: 300)),
        });
        await mount(tester, probe: probe);
        clipboard.begin(_paste(moved: 4 * _mb));
        await advance(tester, 800);
        expect(probe.asked, 1, reason: 'asked once, as the paste starts');
        await advance(tester, 1200);
        expect(find.byKey(const Key('clipboard-destination')), findsOneWidget);
        expect(find.text('다운로드'), findsOneWidget);
        expect(find.text('12개 중 3개'), findsOneWidget);
        expect(window.last.subtitle, '다운로드 · 12개 중 3개');
        expect(window.last.folder, isTrue);
        expect(window.last.subtitleTip, _downloads.path);
        expect(window.last.details, contains(('저장 위치', _downloads.path)));
        expect(
          tester
              .widget<Tooltip>(
                find.ancestor(
                  of: find.byKey(const Key('clipboard-destination')),
                  matching: find.byType(Tooltip),
                ),
              )
              .message,
          _downloads.path,
        );
        clipboard.end();
        await tester.pump();
        expect(find.text('옮겼어요'), findsNothing);
        expect(find.text('다운로드에 있어요'), findsNothing);
        expect(card, findsNothing);
        expect(window.updates.where((value) => value.kind == 1), isEmpty);
        // Observing a created name cannot establish Explorer's final commit.
        await advance(tester, 5000);
        expect(find.byKey(const Key('clipboard-open-folder')), findsNothing);
        expect(await controller.openFolder(), isFalse);
        expect(probe.opened, isEmpty);
        expect(card, findsNothing);
        expect(probe.asked, 1);
      },
    );

    cardTest('a late native open-folder callback cannot turn handoff into completion', (
      tester,
    ) async {
      final probe = _Probe(_downloads, {photos: _epoch});
      await mount(tester, probe: probe);
      clipboard.begin(_paste(moved: 4 * _mb));
      await advance(tester, 2000);
      clipboard.end();
      await tester.pump();
      expect(window.onOpen, isNotNull);
      window.onOpen!();
      await tester.pump();
      expect(probe.opened, isEmpty);
    });

    cardTest('an item appearing at handoff does not prove a completed destination', (
      tester,
    ) async {
      final probe = _Probe(_downloads);
      await mount(tester, probe: probe);
      clipboard.begin(_paste(moved: 4 * _mb));
      await advance(tester, 2000);
      expect(find.byKey(const Key('clipboard-destination')), findsNothing);
      probe.created_[photos] = _epoch.add(const Duration(seconds: 2));
      clipboard.end();
      await tester.pump();
      await tester.pump();
      expect(find.text('다운로드에 있어요'), findsNothing);
      expect(find.byKey(const Key('clipboard-open-folder')), findsNothing);
      expect(window.updates.where((value) => value.kind == 1), isEmpty);
    });

    for (final (what, folder, created) in [
      ('nothing new appeared there', _downloads, <String, DateTime>{}),
      (
        'the item there is older than this paste',
        _downloads,
        {photos: _epoch.subtract(const Duration(seconds: 10))},
      ),
      (
        'the window in front is no folder (another app, a file dialog)',
        null,
        {photos: _epoch},
      ),
    ]) {
      cardTest('no folder is claimed when $what', (tester) async {
        final probe = _Probe(folder, created);
        await mount(tester, probe: probe);
        clipboard.begin(_paste(moved: 4 * _mb));
        await advance(tester, 3000);
        expect(find.byKey(const Key('clipboard-destination')), findsNothing);
        expect(window.last.folder, isFalse);
        expect(window.last.subtitle, '12개 중 3개');
        clipboard.end();
        await tester.pump();
        await tester.pump();
        expect(card, findsNothing);
        expect(find.byKey(const Key('clipboard-open-folder')), findsNothing);
        expect(window.last.openable, isFalse);
        expect(await controller.openFolder(), isFalse);
        expect(probe.opened, isEmpty);
      });
    }

    cardTest('a copy to the VM never asks which folder is in front here', (
      tester,
    ) async {
      final probe = _Probe(_downloads, {photos: _epoch});
      await mount(tester, probe: probe);
      clipboard.begin(_files(moved: _mb, sending: true));
      await advance(tester, 3000);
      expect(probe.asked, 0);
      expect(find.byKey(const Key('clipboard-destination')), findsNothing);
    });
  });

  group('layout', () {
    for (final dark in [true, false]) {
      cardTest('${dark ? 'dark' : 'light'} uses the toolbar\'s colours', (
        tester,
      ) async {
        await mount(tester, dark: dark);
        clipboard.begin(_files(moved: _mb));
        await advance(tester, 800);
        final box = tester.widget<DecoratedBox>(card);
        final decoration = box.decoration as BoxDecoration;
        expect(decoration.color, SessionChrome.forDark(dark).surface);
        expect(window.last.dark, dark);
      });
    }

    cardTest('a narrow window shrinks the card, nothing overflows', (
      tester,
    ) async {
      await mount(tester, size: const Size(300, 520));
      clipboard.begin(_files(moved: _mb, name: '${'가' * 60}.docx'));
      await advance(tester, 800);
      expect(tester.takeException(), isNull);
      expect(tester.getSize(card).width, lessThanOrEqualTo(300));
      // Failure text wraps instead of running out of the window.
      clipboard.end(code: 'clipboard_write_failed', message: '아주 긴 이유 ' * 20);
      await tester.pump();
      expect(tester.takeException(), isNull);
      expect(tester.getRect(card).right, lessThanOrEqualTo(300));
    });

    cardTest('text scale 2.0 keeps every part inside the window', (
      tester,
    ) async {
      await mount(tester, size: const Size(360, 640), textScale: 2.0);
      clipboard.begin(
        _files(moved: 12 * _mb, name: 'a-long-document-name.pdf'),
      );
      await advance(tester, 800);
      expect(tester.takeException(), isNull);
      final rect = tester.getRect(card);
      expect(rect.left, greaterThanOrEqualTo(0));
      expect(rect.right, lessThanOrEqualTo(360));
      expect(rect.bottom, lessThanOrEqualTo(640));
      expect(find.byKey(const Key('clipboard-cancel')), findsOneWidget);
      clipboard.end(
        code: 'clipboard_write_failed',
        message: '붙여넣기를 준비하지 못했어요. 다시 복사해 주세요.',
      );
      await tester.pump();
      expect(tester.takeException(), isNull);
      expect(tester.getRect(card).bottom, lessThanOrEqualTo(640));
    });
  });

  test(
    'the session going away takes the card window and the taskbar with it',
    () async {
      final clipboard = _Clipboard();
      final window = _Window();
      var now = Duration.zero;
      final controller = ClipboardTransferController(
        clipboard: clipboard,
        window: window,
        clock: () => now,
      );
      clipboard.begin(_files(moved: _mb));
      await Future<void>.delayed(const Duration(milliseconds: 900));
      now = const Duration(milliseconds: 900);
      expect(controller.phase, TransferPhase.running);
      controller.dispose();
      expect(window.hides, greaterThan(0));
      expect(window.taskbars.last.$1, TaskbarProgressMode.none);
      expect(window.onCancel, isNull);
      expect(window.onClose, isNull);
    },
  );
}
