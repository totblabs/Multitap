import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../models/session_guide.dart';
import '../data/remote_session.dart';
import 'app_dialog.dart';
import 'app_menu.dart' show AnchoredPanelRoute, PopupMotion;
import 'app_theme.dart';
import 'korean_text.dart';
import 'primary_region.dart';
import 'session_menu.dart' show SessionPopups;
import '../l10n/l10n.dart';

/// Live local presentation only; no remote feature is enabled by this state.
class SessionGuidePresentation {
  const SessionGuidePresentation({
    required this.canRemember,
    this.hidden = false,
    this.preferenceNote,
    this.releasePending = false,
  });
  final bool canRemember, hidden, releasePending;
  final String? preferenceNote;
}

DialogRoute<bool> createSessionGuideRoute(
  BuildContext context, {
  required String machine,
  required SessionGuideContent content,
  required bool canRemember,
  bool hidden = false,
  bool showPreference = true,
  String? preferenceNote,
  RemoteSession? session,
  ValueListenable<SessionGuidePresentation>? presentation,
  SessionPopups? popups,
}) {
  final navigator = Navigator.of(context, rootNavigator: true);
  final track = popups?.tracker();
  Widget body() => _liveGuide(
    machine: machine,
    content: content,
    session: session,
    canRemember: canRemember,
    hidden: hidden,
    showPreference: showPreference,
    preferenceNote: preferenceNote,
    presentation: presentation,
  );
  return DialogRoute<bool>(
    context: context,
    animationStyle: PopupMotion.style(context),
    themes: InheritedTheme.capture(from: context, to: navigator.context),
    anchorPoint: PrimaryRegionScope.maybeOf(context)?.value?.center,
    builder: (_) => track == null ? body() : track(body()),
  );
}

/// Explicit toolbar help uses the same live content as the first-connect
/// notice and belongs to the session's popup lifecycle.
PopupRoute<bool> createSessionGuidePanelRoute(
  BuildContext context, {
  required Rect? anchor,
  required String machine,
  required RemoteSession session,
  ValueListenable<SessionGuidePresentation>? presentation,
  required SessionPopups popups,
  bool showPreference = true,
}) {
  final navigator = Navigator.of(context, rootNavigator: true);
  final media = MediaQuery.of(navigator.overlay!.context);
  const width = 440.0;
  final area = anchor == null
      ? null
      : AnchoredPanelRoute.areaFor(media, anchor);
  if (anchor == null ||
      area == null ||
      area.width <= 2 * AnchoredPanelRoute.margin ||
      area.bottom - anchor.bottom - AnchoredPanelRoute.margin < 240) {
    return createSessionGuideRoute(
      context,
      machine: machine,
      content: SessionGuideContent.fromSession(session),
      canRemember: false,
      showPreference: showPreference,
      session: session,
      presentation: presentation,
      popups: popups,
    );
  }
  final track = popups.tracker();
  return AnchoredPanelRoute<bool>(
    anchor: anchor,
    width: width,
    label: l10n.guide,
    reduceMotion: MediaQuery.disableAnimationsOf(context),
    theme: buildAppTheme(brightness: Brightness.dark),
    builder: (_) => track(
      _liveGuide(
        machine: machine,
        content: SessionGuideContent.fromSession(session),
        session: session,
        canRemember: false,
        showPreference: showPreference,
        presentation: presentation,
        panel: true,
      ),
    ),
  );
}

Widget _liveGuide({
  required String machine,
  required SessionGuideContent content,
  required bool canRemember,
  bool hidden = false,
  bool showPreference = true,
  String? preferenceNote,
  RemoteSession? session,
  ValueListenable<SessionGuidePresentation>? presentation,
  bool panel = false,
}) {
  Widget dialog(SessionGuidePresentation? value) => SessionGuideDialog(
    machine: machine,
    content: session == null
        ? content
        : SessionGuideContent.fromSession(session),
    canRemember: value?.canRemember ?? canRemember,
    hidden: value?.hidden ?? hidden,
    showPreference: showPreference,
    preferenceNote: value == null ? preferenceNote : value.preferenceNote,
    releasePending: value?.releasePending ?? false,
    panel: panel,
  );
  Widget liveDialog(SessionGuidePresentation? value) => session == null
      ? dialog(value)
      : ListenableBuilder(
          listenable: session,
          builder: (_, _) => ListenableBuilder(
            listenable: Listenable.merge([
              session.clipboard,
              session.drive,
              session.usb,
              session.printer,
            ]),
            builder: (_, _) => dialog(value),
          ),
        );
  return presentation == null
      ? liveDialog(null)
      : ValueListenableBuilder<SessionGuidePresentation>(
          valueListenable: presentation,
          builder: (_, value, _) => liveDialog(value),
        );
}

class SessionGuideDialog extends StatefulWidget {
  const SessionGuideDialog({
    super.key,
    required this.machine,
    required this.content,
    required this.canRemember,
    this.hidden = false,
    this.showPreference = true,
    this.preferenceNote,
    this.releasePending = false,
    this.panel = false,
  });
  final String machine;
  final SessionGuideContent content;
  final bool canRemember, hidden, releasePending;
  final bool panel, showPreference;
  final String? preferenceNote;
  @override
  State<SessionGuideDialog> createState() => _SessionGuideDialogState();
}

class _SessionGuideDialogState extends State<SessionGuideDialog> {
  late bool _hidden = widget.hidden;
  bool _edited = false;

  @override
  void didUpdateWidget(covariant SessionGuideDialog oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (!_edited) _hidden = widget.hidden;
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    void cancel() => Navigator.of(context).pop();
    final body = <Widget>[
      KeepAllText(
        l10n.featuresAvailableConnection,
        style: uiText(size: 13, color: p.muted),
      ),
      const SizedBox(height: 16),
      for (final tip in widget.content.tips) ...[
        _Tip(tip),
        const SizedBox(height: 14),
      ],
      if (widget.showPreference)
        CheckboxListTile(
          key: const Key('session-guide-hide'),
          contentPadding: EdgeInsets.zero,
          controlAffinity: ListTileControlAffinity.leading,
          title: KeepAllText(
            l10n.dontShowAgainDesktop,
            softWrap: true,
            maxLines: 3,
            style: uiText(size: 14, color: p.ink),
          ),
          subtitle: KeepAllText(
            widget.preferenceNote ?? l10n.youCanSeeAgainUnder,
            softWrap: true,
            maxLines: 6,
            style: uiText(size: 12, color: p.muted),
          ),
          value: widget.canRemember && _hidden,
          onChanged: widget.canRemember && !widget.releasePending
              ? (value) => setState(() {
                  _edited = true;
                  _hidden = value == true;
                })
              : null,
        ),
    ];
    // 확인 only closes the guide: the grey button, not the amber one.
    final confirm = TextButton(
      key: const Key('confirm-session-guide'),
      autofocus: true,
      style: AppButtons.quiet(p),
      onPressed: widget.releasePending
          ? null
          : () => Navigator.of(
              context,
            ).pop(widget.showPreference && widget.canRemember ? _hidden : null),
      child: KeepAllText(l10n.ok),
    );
    final surface = widget.panel
        ? ConstrainedBox(
            key: const Key('session-guide-dialog'),
            constraints: const BoxConstraints(maxHeight: 600),
            child: Column(
              key: const Key('session-guide-panel'),
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 12, 8, 12),
                  child: Row(
                    children: [
                      AppToneIcon(
                        icon: Icons.tips_and_updates_outlined,
                        tone: AppTone.neutral,
                        busy: widget.releasePending,
                        size: 36,
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            KeepAllText(
                              l10n.guide,
                              style: uiText(
                                size: 15,
                                weight: FontWeight.w600,
                                color: p.ink,
                              ),
                            ),
                            const SizedBox(height: 3),
                            KeepAllText(
                              widget.machine,
                              style: uiText(size: 12, color: p.muted),
                            ),
                          ],
                        ),
                      ),
                      IconButton(
                        key: const Key('close-session-guide'),
                        tooltip: l10n.closeGuideEsc,
                        onPressed: cancel,
                        icon: const Icon(Icons.close_rounded, size: 20),
                      ),
                    ],
                  ),
                ),
                Divider(height: 1, color: p.tileLine),
                Flexible(
                  child: SingleChildScrollView(
                    padding: const EdgeInsets.fromLTRB(16, 14, 16, 8),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: body,
                    ),
                  ),
                ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 8, 16, 16),
                  child: Align(
                    alignment: Alignment.centerRight,
                    child: ConstrainedBox(
                      constraints: const BoxConstraints(
                        minWidth: AppDialog.loneActionWidth,
                      ),
                      child: confirm,
                    ),
                  ),
                ),
              ],
            ),
          )
        : AppDialog(
            key: const Key('session-guide-dialog'),
            title: l10n.guide,
            subtitle: widget.machine,
            icon: Icons.tips_and_updates_outlined,
            busy: widget.releasePending,
            onClose: cancel,
            closeKey: const Key('close-session-guide'),
            maxWidth: 720,
            body: body,
            actions: [confirm],
          );
    return CallbackShortcuts(
      bindings: {const SingleActivator(LogicalKeyboardKey.escape): cancel},
      child: surface,
    );
  }
}

class _Tip extends StatelessWidget {
  const _Tip(this.tip);
  final SessionGuideTip tip;
  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    // Four grey circles: a colour here would mean something, and blue,
    // yellow, amber and green meant nothing. Whether it can be used is the
    // chip beside the name.
    final icon = switch (tip.feature) {
      SessionGuideFeature.printer => Icons.print_outlined,
      SessionGuideFeature.usb => Icons.usb_rounded,
      SessionGuideFeature.folders => Icons.folder_shared_outlined,
      SessionGuideFeature.clipboard => Icons.content_paste_rounded,
    };
    final ready = tip.availability == SessionGuideAvailability.ready;
    final caption = switch (tip.availability) {
      SessionGuideAvailability.ready => l10n.available,
      SessionGuideAvailability.preparing => l10n.checkingConnection,
      SessionGuideAvailability.unavailable => l10n.notAvailableConnection,
    };
    return Row(
      key: Key('session-guide-tip-${tip.feature.name}'),
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        AppToneIcon(icon: icon, tone: AppTone.neutral, size: 36),
        const SizedBox(width: 12),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Wrap(
                spacing: 10,
                runSpacing: 4,
                crossAxisAlignment: WrapCrossAlignment.center,
                children: [
                  KeepAllText(
                    tip.title,
                    style: uiText(
                      size: 15,
                      weight: FontWeight.w600,
                      color: p.ink,
                    ),
                  ),
                  Container(
                    key: Key('session-guide-state-${tip.feature.name}'),
                    padding: const EdgeInsets.symmetric(
                      horizontal: 8,
                      vertical: 2,
                    ),
                    decoration: BoxDecoration(
                      color: ready ? p.okSoft : p.quiet,
                      borderRadius: BorderRadius.circular(999),
                    ),
                    child: Row(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        if (ready) ...[
                          Container(
                            width: 6,
                            height: 6,
                            decoration: BoxDecoration(
                              color: p.green,
                              shape: BoxShape.circle,
                            ),
                          ),
                          const SizedBox(width: 6),
                        ],
                        Flexible(
                          child: KeepAllText(
                            caption,
                            style: uiText(
                              size: 12,
                              weight: FontWeight.w600,
                              height: 1.5,
                              color: ready ? p.green : p.muted,
                            ),
                          ),
                        ),
                      ],
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 4),
              // How to use it, or why it cannot be used: ordinary body text.
              KeepAllText(
                tip.message,
                style: uiText(size: 13, height: 1.5, color: p.muted),
              ),
            ],
          ),
        ),
      ],
    );
  }
}
