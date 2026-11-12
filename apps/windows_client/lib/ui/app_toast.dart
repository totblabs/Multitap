import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'app_theme.dart';
import 'korean_text.dart';

/// What a toast reports. The icon and the edge say it before the words do.
enum AppToastKind {
  /// Something to know, neither a result nor a problem.
  plain,

  /// The action finished.
  done,

  /// The action did not happen.
  failed,
}

/// The product toast: a small dark card at the bottom centre with the
/// result's icon before the sentence. At most [appToastWidth] wide (wider
/// with large text, as dialogs are) and never wider than the window less a
/// 16 px gutter on each side.
///
/// A failure the user has to act on belongs in a dialog, not here.
SnackBar appToast(
  BuildContext context,
  String message, {
  AppToastKind kind = AppToastKind.plain,
  SnackBarAction? action,
  Duration? duration,
}) {
  final media = MediaQuery.of(context);
  final scale = media.textScaler.scale(1);
  final width = math.min(
    appToastWidth * scale.clamp(1.0, 1.5),
    math.max(120.0, media.size.width - 32),
  );
  // The card is dark in both themes, so the dark theme's status colours.
  const palette = AppPalette(true);
  final (icon, color) = switch (kind) {
    AppToastKind.plain => (null, null),
    AppToastKind.done => (Icons.check_circle_rounded, palette.green),
    AppToastKind.failed => (Icons.error_outline_rounded, palette.red),
  };
  final text = KeepAllText(message);
  return SnackBar(
    behavior: SnackBarBehavior.floating,
    width: width,
    action: action,
    duration: duration ?? const Duration(seconds: 4),
    shape: RoundedRectangleBorder(
      borderRadius: BorderRadius.circular(12),
      side: BorderSide(
        color: color?.withValues(alpha: .45) ?? AppPalette.toastEdge,
      ),
    ),
    content: icon == null
        ? text
        : Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Padding(
                // Centred on the first line (13 px text, 1.5 line height),
                // and as large as the text is.
                padding: EdgeInsets.only(top: (13 * 1.5 - 18) * scale / 2),
                child: Icon(icon, size: 18 * scale, color: color),
              ),
              const SizedBox(width: 10),
              Expanded(child: text),
            ],
          ),
  );
}

/// Shows [message] as the product toast on the nearest [ScaffoldMessenger]
/// (nothing when there is none, as in a bare test host).
void showAppToast(
  BuildContext context,
  String message, {
  AppToastKind kind = AppToastKind.plain,
  SnackBarAction? action,
  Duration? duration,
}) {
  ScaffoldMessenger.maybeOf(context)?.showSnackBar(
    appToast(context, message, kind: kind, action: action, duration: duration),
  );
}
