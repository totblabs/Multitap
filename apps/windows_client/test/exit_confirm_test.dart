import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:multitap_client/data/appearance_settings.dart';
import 'package:multitap_client/data/window_close_guard.dart';
import 'package:multitap_client/ui/themed_app.dart';

class _Guard implements WindowCloseGuard {
  Future<void> Function()? onRequest;
  bool dirty = false;
  int closes = 0;

  @override
  void attach(Future<void> Function() callback) => onRequest = callback;

  @override
  Future<bool> update({required bool busy, required bool dirty}) async {
    this.dirty = dirty;
    return true;
  }

  @override
  Future<bool> close() async {
    closes++;
    return true;
  }

  @override
  void dispose() => onRequest = null;
}

void main() {
  testWidgets('Closing the client asks first; only 종료 closes it', (t) async {
    final guard = _Guard();
    final appearance = AppAppearance();
    addTearDown(appearance.dispose);
    await t.pumpWidget(
      ThemedApp(
        appearance: appearance,
        confirmExit: true,
        exitGuard: guard,
        home: const Scaffold(body: Text('home')),
      ),
    );
    await t.pumpAndSettle();
    // The window holds its close until the app answers.
    expect(guard.dirty, isTrue);

    final asked = guard.onRequest!();
    await t.pumpAndSettle();
    expect(find.byKey(const Key('exit-confirm-dialog')), findsOneWidget);
    expect(find.text('Multitap을 종료할까요?'), findsOneWidget);
    await t.tap(find.byKey(const Key('exit-cancel')));
    await t.pumpAndSettle();
    await asked;
    expect(guard.closes, 0);

    final again = guard.onRequest!();
    await t.pumpAndSettle();
    await t.tap(find.byKey(const Key('exit-confirm')));
    await t.pumpAndSettle();
    await again;
    expect(guard.closes, 1);
  });

  testWidgets('Without confirmExit the window closes as before', (t) async {
    final guard = _Guard();
    final appearance = AppAppearance();
    addTearDown(appearance.dispose);
    await t.pumpWidget(
      ThemedApp(
        appearance: appearance,
        exitGuard: guard,
        home: const Scaffold(body: Text('home')),
      ),
    );
    await t.pumpAndSettle();
    expect(guard.onRequest, isNull);
    expect(guard.dirty, isFalse);
  });
}
