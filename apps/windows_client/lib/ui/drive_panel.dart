import 'dart:async';

import 'package:flutter/material.dart';

import '../data/drive_rules.dart';
import '../data/drive_session.dart';
import 'app_dialog.dart';
import 'app_form.dart';
import 'app_menu.dart';
import 'app_theme.dart';
import 'korean_text.dart';
import 'session_menu.dart' show SessionPopups;
import '../l10n/l10n.dart';

/// What the user reads for a fixed drive code.
String driveMessage(String? code) => switch (code) {
  'drive_exists' => l10n.folderAlreadyShared,
  'drive_limit' => l10n.youCanShareUpFolders(driveMaxShares),
  'drive_user_unavailable' =>
    l10n.couldNotFindUserSigned,
  'drive_denied' => l10n.pcUserCannotOpenFolder,
  'drive_not_found' =>
    l10n.couldNotFindFolderPc,
  'drive_not_directory' => l10n.youCanOnlyShareFolders,
  'drive_invalid_path' => l10n.pathCannotShared,
  'drive_open_unavailable' => l10n.cannotOpenExplorerDesktopCheck,
  'drive_open_failed' => l10n.couldNotOpenFolderExplorer,
  'drive_open_unknown' =>
    l10n.couldNotCheckWhetherFolder,
  'drive_share_unknown' => l10n.sharingFolderHasEndedReconnecting,
  'drive_busy' => l10n.tryAgainAfterOtherTask,
  'drive_unavailable' => l10n.sharedFoldersNotAvailableWindow,
  'drive_webclient_unavailable' =>
    l10n.webclientServiceDesktopOffMissing,
  'drive_letter_unavailable' =>
    l10n.desktopHasNoDriveLetters,
  'no_interactive_session' => l10n.youNotSignedDesktopSign,
  'drive_map_failed' => l10n.couldNotConnectDriveDesktop,
  'drive_policy_changed' => l10n.administratorTurnedOffSharedFolder,
  _ => l10n.couldNotShareFolder,
};

/// The sentence of the confirmation for a risky folder (null: none needed).
String? driveRiskMessage(DriveRisk risk) => switch (risk) {
  DriveRisk.driveRoot => l10n.youAboutShareWholeDrive,
  DriveRisk.system =>
    l10n.windowsProgramFolderChangingDeleting,
  DriveRisk.profile =>
    l10n.youAboutShareWholeAccount,
  DriveRisk.network =>
    l10n.networkFolderIfConnectionDrops,
  DriveRisk.none => null,
};

/// Said once a folder listing was cut at the protocol's entry limit.
String get driveTruncatedText =>
    l10n.foldersMoreThan20000;

/// The Host's one-time hints (the WebDAV drive backend only).
String get driveOldLimitText => l10n.untilDesktopRestartsFilesOver;
String get driveTooLargeText =>
    l10n.singleFileOver4Gb;

/// What a share row says while its folder cannot be reached.
String get driveOfflineText => l10n.cannotOpenFolderPcCheck;

/// The toast when sharing ends while the stream goes on (null: say nothing).
String? driveEndedMessage(DriveSession drive) =>
    !drive.endedUnexpectedly || drive.recovering
    ? null
    : drive.error == 'drive_policy_changed'
    ? l10n.administratorTurnedOffSharedFolder
    : l10n.sharedFolderWasDisconnectedConnect;

/// Said when a shared folder was deleted on this PC and its share ended.
String driveGoneMessage(String name) => l10n.wasDeletedSoSharingEnded(name);

String _samePath(String path) => path;

/// What the panel says about the device the folders are on. The defaults are
/// the Windows PC's; Android passes its own (lib/android/android_drive.dart:
/// "이 기기", a readable place instead of a document URI, its own reasons).
class DriveWording {
  const DriveWording({
    String? device,
    this.message = driveMessage,
    this.risk,
    this.path = _samePath,
    this.gone = driveGoneMessage,
    String? offline,
    this.fix,
  }) : _device = device, // ignore: prefer_initializing_formals
       _offline = offline; // ignore: prefer_initializing_formals

  /// What the person can do themselves about a folder that could not be
  /// added, as one button under the reason (null: nothing to offer).
  final ({String label, VoidCallback run})? Function(String code)? fix;

  /// The word for where the folders are ("PC", "이 기기"), as it reads at the
  /// start of a sentence or as a label (English: "Your PC", "This device").
  final String? _device;
  String get device => _device ?? l10n.pcDeviceWord;

  /// What the user reads for a fixed drive code.
  final String Function(String? code) message;

  /// The warning for a picked folder (null: none); null here: the Windows
  /// rules ([DrivePanel.riskOf] and [driveRiskMessage]).
  final String? Function(String path)? risk;

  /// How a share's location is shown.
  final String Function(String path) path;

  /// Said when a shared folder's share ended because the folder is gone.
  final String Function(String name) gone;

  /// What a share row says while its folder cannot be reached.
  final String? _offline;
  String get offline => _offline ?? driveOfflineText;
}

/// Opens the shared-folder panel: dropped down from the session bar's
/// button at [anchor], or as a dialog without one (the narrow bar's
/// overflow) or when it cannot fit. Adding a folder and ending a share each
/// ask once first.
Future<void> showDrivePanel(
  BuildContext context, {
  required DriveSession drive,
  Rect? anchor,
  SessionPopups? popups,
}) {
  final navigator = Navigator.of(context);
  // The panel is removed when the native folder picker takes the focus (the
  // session closes its popups on deactivation); its confirmation dialog must
  // not depend on the panel's own, then deactivated, context.
  final dialogContext = navigator.overlay!.context;
  final media = MediaQuery.of(dialogContext);
  if (anchor != null) {
    final area = AnchoredPanelRoute.areaFor(media, anchor);
    const width = 384.0;
    if (area.width >= width + 2 * AnchoredPanelRoute.margin &&
        area.bottom - anchor.bottom - AnchoredPanelRoute.margin >= 240) {
      final track = popups?.tracker();
      return navigator.push(
        AnchoredPanelRoute<void>(
          anchor: anchor,
          reduceMotion: MediaQuery.disableAnimationsOf(context),
          width: width,
          label: l10n.sharedFolder,
          theme: buildAppTheme(brightness: Brightness.dark),
          builder: (context) {
            final panel = DrivePanel(
              drive: drive,
              dialogContext: dialogContext,
            );
            return track == null ? panel : track(panel);
          },
        ),
      );
    }
  }
  final track = popups?.tracker();
  return showDialog<void>(
    context: context,
    builder: (context) {
      final panel = Theme(
        data: buildAppTheme(brightness: Brightness.dark),
        child: Dialog(
          backgroundColor: const AppPalette(true).surface,
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 420, maxHeight: 560),
            child: DrivePanel(drive: drive, dialogContext: dialogContext),
          ),
        ),
      );
      return track == null ? panel : track(panel);
    },
  );
}

/// The folders this PC shares with the VM: each with its VM drive letter
/// (or why it is not there) and 공유 해제, and
/// 폴더 추가…. Follows [drive] live.
class DrivePanel extends StatefulWidget {
  const DrivePanel({
    super.key,
    required this.drive,
    this.dialogContext,
    this.riskOf = driveRiskOf,
    this.wording = const DriveWording(),
    this.embedded = false,
  });
  final DriveSession drive;

  /// Inside another card that has its own title and scrolling (the Android
  /// menu card): no title row, and the rows are not a list of their own.
  final bool embedded;

  /// The device's word and messages (Windows by default).
  final DriveWording wording;

  /// What a picked folder is (system folder, drive root...); replaceable in
  /// tests.
  final DriveRisk Function(String path) riskOf;

  /// Where the confirmations open; defaults to the panel's own context.
  final BuildContext? dialogContext;

  @override
  State<DrivePanel> createState() => _DrivePanelState();
}

class _DrivePanelState extends State<DrivePanel> {
  String? _notice;
  ({String label, VoidCallback run})? _fix;
  bool _details = false;

  Future<void> _open(DriveShare share) async {
    setState(() {
      _notice = null;
      _fix = null;
    });
    await widget.drive.openShare(share.id);
  }

  BuildContext get _dialogContext {
    final outer = widget.dialogContext;
    return outer != null && outer.mounted ? outer : context;
  }

  Future<void> _add() async {
    setState(() {
      _notice = null;
      _fix = null;
    });
    // The read-write consequence is said beside 폴더 추가… before the picker
    // opens (2026-11-02), so an ordinary folder is shared as picked. Only a
    // risky one (a drive root, Windows, the whole profile, a network folder)
    // still asks, with what is at stake.
    final failure = await widget.drive.addFolder(
      confirm: (pick) {
        final risk = widget.wording.risk == null
            ? driveRiskMessage(widget.riskOf(pick.path))
            : widget.wording.risk!(pick.path);
        if (risk == null) return Future.value(true);
        return showAppConfirm(
          _dialogContext,
          icon: Icons.folder_shared_rounded,
          tone: AppTone.warn,
          title: l10n.shareFolderDesktopAsDrive,
          message:
              l10n.folderCautionYouCanTurn(widget.wording.path(pick.path), risk),
          confirmLabel: l10n.share,
          dialogKey: const Key('drive-add-confirm'),
          confirmKey: const Key('drive-add-confirm-yes'),
          cancelKey: const Key('drive-add-confirm-no'),
        );
      },
    );
    if (!mounted) return;
    setState(() {
      _notice = failure == null ? null : widget.wording.message(failure);
      _fix = failure == null ? null : widget.wording.fix?.call(failure);
    });
  }

  Future<void> _remove(DriveShare share) async {
    final confirmed = await showAppConfirm(
      _dialogContext,
      icon: Icons.folder_off_rounded,
      tone: AppTone.warn,
      title: l10n.stopSharing,
      message:
          l10n.programsDesktopHaveFilesFrom(share.shownName, widget.wording.path(share.path), widget.wording.device),
      confirmLabel: l10n.stopSharing2,
      dialogKey: const Key('drive-remove-confirm'),
      confirmKey: const Key('drive-remove-confirm-yes'),
      cancelKey: const Key('drive-remove-confirm-no'),
    );
    if (confirmed) await widget.drive.remove(share.id);
  }

  /// The share rows: a list that scrolls by itself, or plain rows where the
  /// card around them scrolls.
  static Widget _rows({
    required bool embedded,
    required List<Widget> children,
  }) => embedded
      ? Padding(
          padding: const EdgeInsets.symmetric(horizontal: 8),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: children,
          ),
        )
      : ListView(
          shrinkWrap: true,
          padding: const EdgeInsets.symmetric(horizontal: 8),
          children: children,
        );

  String? get _addBlocked {
    final drive = widget.drive;
    if (drive.closed) {
      return drive.recovering ? l10n.reconnectingSharedFolders : l10n.sharedFoldersDisconnected;
    }
    if (!drive.ready) return l10n.preparingSharedFolders;
    if (drive.adding) return l10n.choosingFolder;
    if (drive.shares.length >= driveMaxShares) {
      return l10n.youCanShareUpFolders2(driveMaxShares);
    }
    return null;
  }

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    return ListenableBuilder(
      listenable: widget.drive,
      builder: (context, _) {
        final drive = widget.drive;
        final shares = drive.shares;
        final blocked = _addBlocked;
        // What sharing means, said before the picker opens (just above 폴더
        // 추가…).
        final consequence = KeepAllText(
          l10n.desktopCanReadChangeFiles,
          key: const Key('drive-add-consequence'),
          style: uiText(size: 12, height: 1.45, color: p.muted),
        );
        return Column(
          key: const Key('drive-panel'),
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            if (!widget.embedded)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 14, 16, 0),
                child: Row(
                  children: [
                    Icon(Icons.folder_shared_rounded, size: 18, color: p.amber),
                    const SizedBox(width: 10),
                    Expanded(
                      child: KeepAllText(
                        l10n.shareFoldersFrom(widget.wording.device),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: uiText(
                          size: 14,
                          weight: FontWeight.w600,
                          color: p.ink,
                        ),
                      ),
                    ),
                  ],
                ),
              ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 6, 16, 8),
              child: KeepAllText(
                drive.closed
                    ? drive.error == 'drive_policy_changed'
                          ? l10n.administratorTurnedOffSharedFolder
                          // Coming back by itself with the same folders:
                          // nothing to reconnect or pick again.
                          : drive.recovering
                          ? l10n.sharedFoldersReconnectingWait
                          : l10n.sharedFolderWasDisconnectedConnect
                    : l10n.onlyFoldersYouChooseShared,
                style: uiText(size: 12.5, height: 1.45, color: p.muted),
              ),
            ),
            Flexible(
              child: shares.isEmpty
                  ? Padding(
                      padding: const EdgeInsets.fromLTRB(16, 10, 16, 14),
                      child: Column(
                        mainAxisSize: MainAxisSize.min,
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          KeepAllText(
                            key: const Key('drive-empty'),
                            drive.ready || drive.closed
                                ? l10n.noSharedFoldersYet
                                : l10n.preparingSharedFolders2,
                            style: uiText(size: 13, color: p.muted),
                          ),
                          const SizedBox(height: 8),
                          consequence,
                        ],
                      ),
                    )
                  : _rows(
                      embedded: widget.embedded,
                      children: [
                        for (final share in shares)
                          _ShareRow(
                            key: ValueKey('drive-share-${share.id}'),
                            share: share,
                            wording: widget.wording,
                            closed: drive.closed,
                            recovering: drive.closed && drive.recovering,
                            showOpen: drive.openSupported,
                            onOpen: drive.canOpen(share.id)
                                ? () => unawaited(_open(share))
                                : null,
                            opening: drive.openingShare == share.id,
                            onRemove: () => unawaited(_remove(share)),
                            onRetry: () async {
                              final failure = await drive.retry(share.id);
                              if (mounted && failure != null) {
                                setState(() {
                                  _notice = widget.wording.message(failure);
                                  _fix = null;
                                });
                              }
                            },
                          ),
                        // Scrolls with the rows: enlarged text keeps them in view.
                        Padding(
                          padding: const EdgeInsets.fromLTRB(8, 6, 8, 0),
                          child: consequence,
                        ),
                      ],
                    ),
            ),
            if (drive.lastGone != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                child: AppStatusLine(
                  widget.wording.gone(drive.lastGone!),
                  textKey: const Key('drive-gone'),
                  tone: AppTone.warn,
                ),
              ),
            if (drive.listingTruncated ||
                drive.oldFileLimit ||
                drive.fileTooLarge)
              Flexible(
                flex: _details ? 1 : 0,
                child: SingleChildScrollView(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.stretch,
                    children: [
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 14),
                        child: AppDisclosure(
                          headerKey: const Key('drive-details'),
                          label: _details ? l10n.hideDetails : l10n.sharingLimits,
                          open: _details,
                          onChanged: (open) => setState(() => _details = open),
                        ),
                      ),
                      if (_details && drive.listingTruncated)
                        Padding(
                          padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                          child: AppStatusLine(
                            driveTruncatedText,
                            textKey: const Key('drive-truncated'),
                            tone: AppTone.warn,
                            size: 12,
                          ),
                        ),
                      if (_details && drive.oldFileLimit)
                        Padding(
                          padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                          child: AppStatusLine(
                            driveOldLimitText,
                            textKey: const Key('drive-old-limit'),
                            tone: AppTone.warn,
                            size: 12,
                          ),
                        ),
                      if (_details && drive.fileTooLarge)
                        Padding(
                          padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                          child: AppStatusLine(
                            driveTooLargeText,
                            textKey: const Key('drive-too-large'),
                            tone: AppTone.warn,
                            size: 12,
                          ),
                        ),
                    ],
                  ),
                ),
              ),
            if (_notice != null || drive.openError != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 4, 16, 0),
                // The colour is the icon's; the sentence is ordinary text.
                child: AppStatusLine(
                  _notice ??
                      (drive.openingShare != null
                          ? l10n.openingFolderTakingWhilePlease
                          : widget.wording.message(drive.openError)),
                  textKey: const Key('drive-notice'),
                  // Still opening is a wait, not a failure.
                  tone: _notice == null && drive.openingShare != null
                      ? AppTone.info
                      : AppTone.danger,
                ),
              ),
            if (_notice != null && _fix != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                child: Align(
                  alignment: AlignmentDirectional.centerStart,
                  child: OutlinedButton.icon(
                    key: const Key('drive-notice-fix'),
                    onPressed: _fix!.run,
                    style: AppButtons.outlined(p),
                    icon: const Icon(Icons.settings_outlined, size: 18),
                    label: KeepAllText(_fix!.label),
                  ),
                ),
              ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 10, 16, 14),
              child: Tooltip(
                message: blocked ?? l10n.chooseFolderShare2(widget.wording.device),
                child: OutlinedButton.icon(
                  key: const Key('drive-add'),
                  onPressed: blocked == null ? () => unawaited(_add()) : null,
                  style: AppButtons.outlined(p),
                  icon: const Icon(Icons.create_new_folder_outlined, size: 18),
                  label: KeepAllText(l10n.addFolder),
                ),
              ),
            ),
          ],
        );
      },
    );
  }
}

class _ShareRow extends StatelessWidget {
  const _ShareRow({
    super.key,
    required this.share,
    required this.wording,
    required this.closed,
    this.recovering = false,
    required this.onRemove,
    required this.onRetry,
    required this.showOpen,
    required this.opening,
    this.onOpen,
  });
  final DriveShare share;
  final DriveWording wording;
  final bool closed, showOpen, opening;

  /// The ended channel is coming back by itself with the same folders.
  final bool recovering;
  final VoidCallback? onOpen;
  final VoidCallback onRemove;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    final p = AppPalette.of(context);
    // The state of the row: its colour is a dot, its sentence ordinary text.
    final (text, tone) = closed
        ? (
            recovering ? l10n.reconnecting2 : l10n.disconnected3,
            recovering ? AppTone.info : AppTone.neutral,
          )
        : share.offline
        ? (wording.offline, AppTone.warn)
        : switch (share.state) {
            DriveShareState.opening => (l10n.checking2, AppTone.neutral),
            DriveShareState.pending => (l10n.connecting2, AppTone.info),
            DriveShareState.mapped => (
              l10n.connected3('${share.letter}'),
              AppTone.success,
            ),
            DriveShareState.failed => (
              wording.message(share.error),
              AppTone.danger,
            ),
            DriveShareState.unavailable => (
              wording.message(share.error),
              AppTone.warn,
            ),
          };
    final unavailable = share.state == DriveShareState.unavailable;
    // The same row as a USB device — kind icon, name, a second
    // line, the status line with its dot, and text buttons on the right
    // (stacked under the text when the panel is narrow).
    final buttons = <Widget>[
      if (showOpen)
        Tooltip(
          message: opening
              ? l10n.checkingWhetherOpened
              : share.offline || share.state != DriveShareState.mapped
              ? l10n.youCanOpenOnceShared
              : l10n.opensSharedFolderExplorerDesktop,
          child: FilledButton(
            key: ValueKey('drive-open-${share.id}'),
            onPressed: onOpen,
            style: AppButtons.tonal(p),
            child: KeepAllText(opening ? l10n.checking2 : l10n.open),
          ),
        ),
      if ((unavailable || share.offline) && !closed)
        FilledButton(
          key: ValueKey('drive-retry-${share.id}'),
          onPressed: onRetry,
          style: AppButtons.tonal(p),
          child: KeepAllText(l10n.tryAgain),
        ),
      OutlinedButton(
        key: ValueKey('drive-remove-${share.id}'),
        onPressed: onRemove,
        style: AppButtons.outlined(p),
        child: KeepAllText(l10n.stopSharing2),
      ),
    ];
    final info = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          share.shownName,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: uiText(size: 13.5, weight: FontWeight.w600, color: p.ink),
        ),
        Padding(
          padding: const EdgeInsets.only(top: 2),
          child: Tooltip(
            message: wording.path(share.path),
            child: Text(
              wording.path(share.path),
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: uiText(size: 12, color: p.muted),
            ),
          ),
        ),
        Padding(
          padding: const EdgeInsets.only(top: 4),
          child: AppStatusLine(
            text,
            textKey: ValueKey('drive-status-${share.id}'),
            tone: tone,
            size: 12,
            dot: true,
            maxLines: 3,
          ),
        ),
      ],
    );
    final icon = Padding(
      padding: const EdgeInsets.only(top: 1),
      child: Icon(
        unavailable ? Icons.folder_off_outlined : Icons.folder_rounded,
        size: 18,
        color: unavailable
            ? p.faint
            // Green only while the desktop can really use it.
            : share.state == DriveShareState.mapped && !closed && !share.offline
            ? p.green
            : p.amber,
      ),
    );
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: DecoratedBox(
        decoration: BoxDecoration(
          color: p.subtle,
          borderRadius: BorderRadius.circular(10),
          border: Border.all(color: p.line),
        ),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(12, 10, 10, 10),
          child: LayoutBuilder(
            builder: (context, box) {
              // Narrow panel, large text, or three buttons (a folder that
              // needs attention: 열기, 다시 시도, 공유 끝내기): the buttons go
              // under the text, which keeps the whole width for the reason.
              final stacked =
                  box.maxWidth < 330 ||
                  buttons.length > 2 ||
                  MediaQuery.textScalerOf(context).scale(14) > 19;
              final actions = Wrap(
                spacing: 6,
                runSpacing: 6,
                alignment: WrapAlignment.end,
                children: buttons,
              );
              if (stacked) {
                return Column(
                  crossAxisAlignment: CrossAxisAlignment.stretch,
                  children: [
                    Row(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [icon, const SizedBox(width: 8), Expanded(child: info)],
                    ),
                    const SizedBox(height: 8),
                    Align(alignment: Alignment.centerRight, child: actions),
                  ],
                );
              }
              return Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  icon,
                  const SizedBox(width: 8),
                  Expanded(child: info),
                  const SizedBox(width: 8),
                  actions,
                ],
              );
            },
          ),
        ),
      ),
    );
  }
}
