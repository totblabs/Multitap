import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../ui/session_menu.dart' show SessionChrome;
import '../ui/app_theme.dart' show uiText;
import '../ui/korean_text.dart';

/// How long a notice of [message] stays up: long enough to read slowly,
/// never shorter than [minimum], at most
/// 15 s unless [minimum] asks for more. A newer notice replaces it anyway.
Duration androidNoticeDuration(
  String message, [
  Duration minimum = const Duration(seconds: 3),
]) {
  final characters = message.replaceAll(RegExp(r'\s'), '').length;
  final reading = math.min(15000, 1000 + 150 * characters);
  return Duration(milliseconds: math.max(minimum.inMilliseconds, reading));
}

/// A remote-view message never owns the picture's pointer. Only its explicit
/// action button participates in hit testing, including at enlarged text sizes.
class AndroidRemoteNotice extends StatelessWidget {
  const AndroidRemoteNotice({
    super.key,
    required this.message,
    this.actionLabel,
    this.onAction,
  });
  final String message;
  final String? actionLabel;
  final VoidCallback? onAction;

  @override
  Widget build(BuildContext context) {
    final chrome = SessionChrome.of(context);
    Widget surface(Widget child) => Material(
      color: chrome.surface,
      borderRadius: BorderRadius.circular(8),
      elevation: 3,
      child: child,
    );
    return Align(
      alignment: Alignment.topCenter,
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 320),
        child: Row(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            Flexible(
              child: IgnorePointer(
                key: const Key('android-remote-notice-message'),
                child: surface(
                  Padding(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 10,
                      vertical: 8,
                    ),
                    child: Semantics(
                      liveRegion: true,
                      // Whole, never cut (our own messages are short), by
                      // words; 13 like the session chips.
                      child: KeepAllText(
                        message,
                        style: TextStyle(
                          fontSize: 13,
                          height: 1.35,
                          color: chrome.ink,
                        ),
                      ),
                    ),
                  ),
                ),
              ),
            ),
            if (actionLabel != null && onAction != null) ...[
              const SizedBox(width: 6),
              surface(
                TextButton(
                  key: const Key('android-remote-notice-action'),
                  onPressed: onAction,
                  style: TextButton.styleFrom(
                    foregroundColor: chrome.accent,
                    minimumSize: const Size(0, 36),
                    padding: const EdgeInsets.symmetric(
                      horizontal: 10,
                      vertical: 8,
                    ),
                    tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    textStyle: uiText(size: 12, weight: FontWeight.w600),
                  ),
                  child: Text(actionLabel!, maxLines: 1),
                ),
              ),
            ],
          ],
        ),
      ),
    );
  }
}
