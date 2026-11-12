// The session bar's first-connection hint (2026-11-10, session bar v2):
// "도구는 여기 있어요.", once per account on this PC, under the bar it opens
// after the first picture; never together with 사용안내. Real RemoteView and
// RemoteSession; media, socket, the native window and the store that
// remembers it are fixtures (no file is written).
import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/broker_api.dart';
import 'package:multitap_client/data/remote_session.dart';
import 'package:multitap_client/data/session_guide_memory.dart';
import 'package:multitap_client/data/session_windows.dart';
import 'package:multitap_client/models/session_display_mode.dart';
import 'package:multitap_client/ui/app_menu.dart';
import 'package:multitap_client/ui/remote_view.dart';
import 'package:multitap_client/ui/session_connection_panel.dart';
import 'package:multitap_client/ui/session_toolbar.dart';

import 'remote_session_test.dart'
    show SessionSocket, SessionMedia, info, packet;
import 'stream_display_entry_test.dart' show entryMachine;

const _bar = Key('session-command-bar');
const _handle = Key('expand-session-toolbar');
const _coach = Key('session-bar-coach');
const _confirm = Key('session-bar-coach-ok');
const _guide = Key('session-guide-dialog');

/// Where the hint is remembered: under the account, as if it were a desktop.
const _coachId = 'multitap.session-bar-coach.v1';

/// An account key as the launcher derives it: 64 hex characters.
String _account(String id) =>
    sessionGuideAccountKey(Uri.parse('https://fixture.invalid'), id)!;

SessionGuideIdentity _identity(String account) =>
    SessionGuideIdentity(accountKey: account, desktopId: _coachId);

/// Memory only, recording what was read and written.
class _Store extends MemorySessionGuideStore {
  final reads = <String>[];
  final writes = <(String, bool)>[];
  @override
  Future<bool> hidden(SessionGuideIdentity identity) {
    reads.add(identity.storageKey);
    return super.hidden(identity);
  }

  @override
  Future<void> setHidden(SessionGuideIdentity identity, bool hidden) {
    writes.add((identity.storageKey, hidden));
    return super.setHidden(identity, hidden);
  }
}

/// One session window with its connection (as session_toolbar_items_test's).
class _View {
  _View({this.store, this.account, this.sessionWindow = true, this.id = 'a'});

  /// The test's store (null: the view has nowhere to remember the hint).
  final SessionGuideStore? store;
  final String? account;

  /// A session window can hold the pointer and has the window's actions.
  final bool sessionWindow;
  final String id;
  final socket = SessionSocket();
  final media = SessionMedia();
  var _connected = false;
  late final session = RemoteSession(
    connection: BrokerConnection(
      sessionId: 'coach-$id',
      webSocketUrl: Uri.parse(
        'wss://fixture.invalid/api/v1/sessions/coach-$id',
      ),
    ),
    token: 'fixture-only-token',
    media: media,
    dialer: (_, _) async => socket,
  );

  Future<void> mount(WidgetTester tester, {bool connect = true}) async {
    tester.view.devicePixelRatio = 1;
    tester.view.physicalSize = const Size(1440, 900);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final messenger =
        TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    const window = MethodChannel('multitap/window'),
        events = MethodChannel('multitap/window/events');
    messenger.setMockMethodCallHandler(window, (_) async => false);
    messenger.setMockMethodCallHandler(events, (_) async => null);
    addTearDown(() {
      messenger.setMockMethodCallHandler(window, null);
      messenger.setMockMethodCallHandler(events, null);
    });
    await tester.pumpWidget(
      MaterialApp(
        navigatorObservers: [MenuDismissObserver()],
        home: RemoteView(
          key: ObjectKey(this),
          machine: entryMachine(1),
          prepareDisplaysOnEntry: sessionWindow,
          windowActions: sessionWindow
              ? SessionWindowActions(closeWindow: () async {})
              : null,
          initialDisplayMode: SessionDisplayMode.windowed,
          guideAccountKey: account,
          coachStore: store,
          load: () async => session,
          // The first connection; nothing here reconnects.
          onReconnect: (options) async {
            if (!_connected) {
              _connected = true;
              return session;
            }
            throw const BrokerException('network_error');
          },
          onConfigureDisplays: (count) async => entryMachine(count),
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    if (connect) await firstPicture(tester);
  }

  /// The first picture arrives; then the frames in which the hint, if it is
  /// due, reads its store and shows.
  Future<void> firstPicture(WidgetTester tester) async {
    socket.incoming.add(jsonEncode(info));
    await tester.pump();
    await tester.pump();
    socket.incoming.add(packet());
    await tester.pump();
    await tester.pump();
    expect(session.frameReady, isTrue, reason: session.error);
    await _frames(tester);
  }

  Future<void> close(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink());
    unawaited(session.close());
    await tester.pump(const Duration(seconds: 3));
    await tester.pump();
    unawaited(media.events.close());
  }
}

/// A few frames: a post-frame callback, a store answer and a popup's short
/// transition all fit in them. (Never settling: the view may animate.)
Future<void> _frames(WidgetTester tester) async {
  for (var n = 0; n < 6; n++) {
    await tester.pump(const Duration(milliseconds: 60));
  }
}

/// The tap target of a panel's keyed row. A SessionPanelRow hands its own
/// key on to its InkWell, so the key alone finds both widgets; this is the
/// one InkWell.
Finder _row(String key) {
  final keyed = find.byKey(Key(key));
  final inside = find.descendant(of: keyed, matching: find.byType(InkWell));
  return inside.evaluate().isNotEmpty ? inside : keyed;
}

SessionToolbarState _toolbar(WidgetTester tester) =>
    tester.state<SessionToolbarState>(find.byType(SessionToolbar));

void main() {
  testWidgets('It shows once after the first picture, under the bar it '
      'opened; 확인 hides it, folds the bar and remembers', (tester) async {
    final store = _Store();
    final account = _account('account-A');
    expect(account, hasLength(64));
    final identity = _identity(account);
    final view = _View(store: store, account: account);
    // Nothing before a picture: the bar is folded and nothing was read.
    await view.mount(tester, connect: false);
    try {
      await _frames(tester);
      expect(find.byKey(_coach), findsNothing);
      expect(find.byKey(_bar), findsNothing);
      expect(store.reads, isEmpty);
      await view.firstPicture(tester);
      // The hint, with the bar opened for it.
      expect(find.byKey(_coach), findsOneWidget);
      expect(find.byType(SessionBarCoach), findsOneWidget);
      expect(find.byKey(_bar), findsOneWidget);
      expect(_toolbar(tester).isOpen, isTrue);
      expect(store.reads, [identity.storageKey]);
      expect(store.writes, isEmpty, reason: 'Remembered when it goes.');
      // Three short lines and one button.
      expect(
        find.descendant(
          of: find.byKey(_coach),
          matching: find.text('툴바는 여기 있어요.'),
        ),
        findsOneWidget,
      );
      expect(
        find.descendant(of: find.byKey(_confirm), matching: find.text('확인')),
        findsOneWidget,
      );
      // Under the bar, at its centre, inside the window.
      final bar = tester.getRect(find.byKey(_bar));
      final hint = tester.getRect(find.byKey(_coach));
      expect(hint.top, greaterThan(bar.bottom));
      expect(hint.width, SessionBarCoach.width);
      expect((hint.center.dx - bar.center.dx).abs(), lessThan(1));
      expect(hint.left, greaterThanOrEqualTo(0));
      expect(hint.right, lessThanOrEqualTo(1440));
      // It stays while nothing is done (another frame, another session
      // notification), and is asked about once only.
      view.session.notifyListeners();
      await _frames(tester);
      expect(find.byKey(_coach), findsOneWidget);
      expect(store.reads, hasLength(1));

      await tester.tap(find.byKey(_confirm));
      await _frames(tester);
      expect(find.byKey(_coach), findsNothing);
      expect(find.byKey(_bar), findsNothing, reason: '확인 folds the bar.');
      expect(find.byKey(_handle), findsOneWidget);
      expect(_toolbar(tester).isOpen, isFalse);
      expect(store.writes, [(identity.storageKey, true)]);
      expect(await store.hidden(identity), isTrue);
      // Opening the bar again does not bring it back.
      await tester.tap(find.byKey(_handle));
      await _frames(tester);
      expect(find.byKey(_bar), findsOneWidget);
      expect(find.byKey(_coach), findsNothing);
      expect(store.writes, hasLength(1));
      expect(tester.takeException(), isNull);
    } finally {
      await view.close(tester);
    }
  });

  testWidgets('A later connection of the same account never shows it; '
      'another account gets its own', (tester) async {
    final store = _Store();
    final account = _account('account-A');
    final first = _View(store: store, account: account);
    await first.mount(tester);
    try {
      expect(find.byKey(_coach), findsOneWidget);
      await tester.tap(find.byKey(_confirm));
      await _frames(tester);
      expect(find.byKey(_coach), findsNothing);
      expect(await store.hidden(_identity(account)), isTrue);
    } finally {
      await first.close(tester);
    }
    // The same account again, in a new window with a new connection.
    final second = _View(store: store, account: account, id: 'b');
    await second.mount(tester);
    try {
      expect(find.byKey(_coach), findsNothing);
      expect(
        find.byKey(_bar),
        findsNothing,
        reason: 'The bar is not opened for a hint that is not shown.',
      );
      expect(find.byKey(_handle), findsOneWidget);
      await _frames(tester);
      expect(find.byKey(_coach), findsNothing);
      // Its bar works as ever, without the hint.
      await tester.tap(find.byKey(_handle));
      await _frames(tester);
      expect(find.byKey(_bar), findsOneWidget);
      expect(find.byKey(_coach), findsNothing);
      expect(store.writes, hasLength(1), reason: 'Nothing new to remember.');
    } finally {
      await second.close(tester);
    }
    // Another account on this PC has not seen it yet.
    final other = _account('account-B');
    expect(other, isNot(account));
    final third = _View(store: store, account: other, id: 'c');
    await third.mount(tester);
    try {
      expect(find.byKey(_coach), findsOneWidget);
      expect(find.byKey(_bar), findsOneWidget);
      expect(await store.hidden(_identity(other)), isFalse);
      expect(await store.hidden(_identity(account)), isTrue);
    } finally {
      await third.close(tester);
    }
  });

  testWidgets('Without a store, an account key or a session window it is '
      'never shown', (tester) async {
    final account = _account('account-A');
    for (final (name, view, store) in <(String, _View, _Store?)>[
      // Nowhere to remember "once": never.
      ('no store', _View(account: account, id: 'a'), null),
      // No account to remember it for.
      () {
        final store = _Store();
        return ('no account', _View(store: store, id: 'b'), store);
      }(),
      // Not an account key (64 hex characters).
      () {
        final store = _Store();
        return (
          'malformed account',
          _View(store: store, account: 'account-A', id: 'c'),
          store,
        );
      }(),
      // Not a session window (a preview, an embedded view).
      () {
        final store = _Store();
        return (
          'no session window',
          _View(store: store, account: account, sessionWindow: false, id: 'd'),
          store,
        );
      }(),
    ]) {
      await view.mount(tester);
      try {
        expect(find.byKey(_coach), findsNothing, reason: name);
        expect(find.byKey(_bar), findsNothing, reason: name);
        expect(find.byKey(_handle), findsOneWidget, reason: name);
        await _frames(tester);
        expect(find.byKey(_coach), findsNothing, reason: name);
        expect(find.byKey(_bar), findsNothing, reason: name);
        expect(store?.reads ?? const <String>[], isEmpty, reason: name);
        expect(
          store?.writes ?? const <(String, bool)>[],
          isEmpty,
          reason: name,
        );
        expect(tester.takeException(), isNull, reason: name);
      } finally {
        await view.close(tester);
      }
    }
  });

  testWidgets('Folding the bar ends it for good', (tester) async {
    final store = _Store();
    final account = _account('account-A');
    final view = _View(store: store, account: account);
    await view.mount(tester);
    try {
      expect(find.byKey(_coach), findsOneWidget);
      // The handle under the open bar.
      await tester.tap(find.byKey(const Key('session-toolbar-fold')));
      await _frames(tester);
      expect(find.byKey(_bar), findsNothing);
      expect(find.byKey(_coach), findsNothing);
      expect(store.writes, [(_identity(account).storageKey, true)]);
      expect(await store.hidden(_identity(account)), isTrue);
      await tester.tap(find.byKey(_handle));
      await _frames(tester);
      expect(find.byKey(_bar), findsOneWidget);
      expect(find.byKey(_coach), findsNothing);
      expect(tester.takeException(), isNull);
    } finally {
      await view.close(tester);
    }
  });

  for (final (name, key, panel) in const [
    ('화면', 'stream-menu-화면', 'session-screen-panel'),
    ('설정', 'remote-settings', 'session-settings-panel'),
    ('the quality chip', 'session-quality', 'session-connection-panel'),
  ]) {
    testWidgets('Using the bar ends it: $name opens its panel and the hint '
        'goes', (tester) async {
      final store = _Store();
      final account = _account('account-A');
      final view = _View(store: store, account: account);
      await view.mount(tester);
      try {
        expect(find.byKey(_coach), findsOneWidget);
        await tester.tap(find.byKey(Key(key)));
        await _frames(tester);
        expect(find.byKey(Key(panel)), findsOneWidget);
        expect(find.byKey(_coach), findsNothing);
        expect(find.byKey(_bar), findsOneWidget, reason: 'In use, not folded.');
        expect(store.writes, [(_identity(account).storageKey, true)]);
        await tester.sendKeyEvent(LogicalKeyboardKey.escape);
        await _frames(tester);
        expect(find.byKey(Key(panel)), findsNothing);
        expect(find.byKey(_coach), findsNothing);
        expect(store.writes, hasLength(1));
        expect(tester.takeException(), isNull);
      } finally {
        await view.close(tester);
      }
    });
  }

  testWidgets('사용안내 and the hint are never up together: the hint goes '
      'before the guide appears, and stays gone', (tester) async {
    final store = _Store();
    final account = _account('account-A');
    final view = _View(store: store, account: account);
    await view.mount(tester);
    try {
      expect(find.byKey(_coach), findsOneWidget);
      expect(find.byKey(_guide), findsNothing);
      var sawGuide = false;
      // Every frame from here on: never both.
      Future<void> frames() async {
        for (var n = 0; n < 8; n++) {
          await tester.pump(const Duration(milliseconds: 50));
          final hint = find.byKey(_coach).evaluate().isNotEmpty;
          final guide = find.byKey(_guide).evaluate().isNotEmpty;
          sawGuide = sawGuide || guide;
          expect(hint && guide, isFalse, reason: 'frame $n');
        }
      }

      // 설정 › 사용안내, the only way to it.
      await tester.tap(find.byKey(const Key('remote-settings')));
      await frames();
      expect(find.byKey(const Key('session-settings-panel')), findsOneWidget);
      expect(find.byKey(_guide), findsNothing);
      await tester.tap(_row('remote-session-guide'));
      await frames();
      expect(sawGuide, isTrue);
      expect(find.byKey(_guide), findsOneWidget);
      expect(find.byKey(const Key('session-guide-panel')), findsOneWidget);
      expect(find.byKey(_coach), findsNothing);
      expect(store.writes, [(_identity(account).storageKey, true)]);
      expect(await store.hidden(_identity(account)), isTrue);
      // Closing the guide does not bring the hint back.
      await tester.tap(find.byKey(const Key('confirm-session-guide')));
      await frames();
      expect(find.byKey(_guide), findsNothing);
      expect(find.byKey(_coach), findsNothing);
      expect(store.writes, hasLength(1));
      expect(tester.takeException(), isNull);
    } finally {
      await view.close(tester);
    }
  });
}
