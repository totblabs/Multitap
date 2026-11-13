import 'package:flutter/material.dart';

import '../ui/app_progress.dart';
import '../ui/app_theme.dart';

/// What the desktop list is doing after a press whose own screen is not there
/// yet (연결 준비, Console 열기, Console 끊기, 로그아웃): a card over the list
/// with a turning mark, the sentence and, once the wait is long, how long it
/// has run. It never shows a measure it does not have.
///
/// A child of the list's [Stack]. Not [blocking]: a line at the bottom that
/// lets every touch through. [blocking] (로그아웃): the list is dimmed and
/// takes no touch until the screen changes.
class AndroidLauncherActivity extends StatelessWidget {
  const AndroidLauncherActivity(
    this.text, {
    super.key,
    this.blocking = false,
  });

  final String text;
  final bool blocking;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    final card = Semantics(
      container: true,
      liveRegion: true,
      child: Container(
        constraints: const BoxConstraints(maxWidth: 440),
        padding: const EdgeInsets.fromLTRB(16, 12, 16, 12),
        decoration: BoxDecoration(
          color: p.surface,
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: p.line),
          boxShadow: const [
            BoxShadow(
              color: Color(0x33000000),
              blurRadius: 16,
              offset: Offset(0, 6),
            ),
          ],
        ),
        child: AppBusyLine(
          text,
          textKey: const Key('android-launcher-activity-text'),
          color: p.ink,
          // A wait that ends quickly shows no clock.
          trailing: const AppElapsedText(after: Duration(seconds: 3)),
        ),
      ),
    );
    if (blocking) {
      return Positioned.fill(
        child: Stack(
          children: [
            const ModalBarrier(dismissible: false, color: Color(0x73000000)),
            Center(
              child: Padding(padding: const EdgeInsets.all(16), child: card),
            ),
          ],
        ),
      );
    }
    return Positioned(
      left: 16,
      right: 16,
      bottom: 16,
      child: IgnorePointer(child: Center(child: card)),
    );
  }
}
