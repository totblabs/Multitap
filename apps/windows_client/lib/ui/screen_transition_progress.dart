import 'package:flutter/material.dart';

import 'app_theme.dart';
import 'korean_text.dart';
import '../l10n/l10n.dart';

/// Stages report the operation currently awaited, never an invented percentage.
enum ScreenTransitionStage {
  localCheck,
  windowMode,
  retiring,
  hostReady,
  outputs,
  connecting,
  firstPicture,
  restoring;

  const ScreenTransitionStage();
  String get message => switch (this) {
    ScreenTransitionStage.localCheck => l10n.checkingPcsMonitors,
    ScreenTransitionStage.windowMode => l10n.applyingWindowMode,
    ScreenTransitionStage.retiring => l10n.closingPreviousConnection,
    ScreenTransitionStage.hostReady => l10n.waitingHostScreen2,
    ScreenTransitionStage.outputs => l10n.settingUpRemoteMonitors,
    ScreenTransitionStage.connecting => l10n.connectingNewScreen,
    ScreenTransitionStage.firstPicture => l10n.waitingFirstFrame3,
    ScreenTransitionStage.restoring => l10n.restoringPreviousWindowMode,
  };

}

class ScreenTransitionProgress extends StatelessWidget {
  const ScreenTransitionProgress({
    super.key,
    required this.target,
    required this.stage,
    required this.onClose,
    this.onCancel,
    this.cancelling = false,
    this.status,
  });

  final String target;
  final ScreenTransitionStage stage;
  final VoidCallback onClose;
  final VoidCallback? onCancel;
  final bool cancelling;

  /// Optional measured connection detail; absent callers keep their layout.
  final Widget? status;

  @override
  Widget build(BuildContext context) => Theme(
    data: buildAppTheme(brightness: Brightness.dark),
    child: Builder(
      builder: (context) {
        final palette = AppPalette.of(context);
        return Center(
          child: SingleChildScrollView(
            padding: const EdgeInsets.all(20),
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 400),
              child: Material(
                color: palette.tile,
                borderRadius: BorderRadius.circular(18),
                child: Padding(
                  padding: const EdgeInsets.all(22),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      KeepAllText(
                        l10n.switching(target),
                        style: TextStyle(
                          color: palette.ink,
                          fontSize: 18,
                          fontWeight: FontWeight.w600,
                        ),
                      ),
                      const SizedBox(height: 18),
                      const LinearProgressIndicator(
                        key: Key('screen-transition-progress'),
                        minHeight: 4,
                        color: Color(0xFFF5A623),
                      ),
                      const SizedBox(height: 14),
                      Semantics(
                        liveRegion: true,
                        child: KeepAllText(
                          cancelling ? l10n.cancellingSwitch : stage.message,
                          key: const Key('screen-transition-stage'),
                          style: TextStyle(color: palette.ink, height: 1.5),
                        ),
                      ),
                      if (status != null) ...[
                        const SizedBox(height: 12),
                        status!,
                      ],
                      const SizedBox(height: 6),
                      KeepAllText(
                        l10n.youCanTypeOnceNew,
                        style: TextStyle(color: palette.muted, height: 1.5),
                      ),
                      const SizedBox(height: 14),
                      Align(
                        alignment: Alignment.centerRight,
                        child: TextButton(
                          key: const Key('screen-transition-cancel'),
                          onPressed: cancelling ? null : onCancel ?? onClose,
                          style: TextButton.styleFrom(
                            minimumSize: const Size(88, 44),
                          ),
                          child: KeepAllText(
                            onCancel != null || cancelling
                                ? l10n.cancelSwitch
                                : l10n.closeRemoteWindow,
                          ),
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        );
      },
    ),
  );
}
