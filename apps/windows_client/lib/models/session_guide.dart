import '../data/clipboard_policy.dart';
import '../data/remote_session.dart';
import '../l10n/l10n.dart';

enum SessionGuideFeature { printer, usb, folders, clipboard }

enum SessionGuideAvailability { ready, preparing, unavailable }

class SessionGuideTip {
  const SessionGuideTip(
    this.feature,
    this.title,
    this.message,
    this.availability,
  );
  final SessionGuideFeature feature;
  final String title, message;
  final SessionGuideAvailability availability;
}

class SessionGuideContent {
  const SessionGuideContent(this.tips);
  final List<SessionGuideTip> tips;

  factory SessionGuideContent.fromSession(RemoteSession session) {
    final connection = session.connection;
    final printer = session.printer, usb = session.usb, drive = session.drive;
    final clipboard = session.clipboard;
    final clipboardReady =
        clipboard != null && clipboard.ready && !clipboard.closed;
    return SessionGuideContent.fromFacts(
      printerGranted: connection.printWebSocketUrl != null,
      printerReady: printer != null && printer.queueReady && !printer.closed,
      printerFailed: printer?.closed == true || printer?.queueReason != null,
      usbGranted: connection.usbWebSocketUrl != null,
      usbReady: usb?.ready == true,
      usbFailed: usb?.closed == true,
      foldersGranted: connection.driveWebSocketUrl != null,
      foldersReady: drive != null && drive.ready && !drive.closed,
      foldersFailed: drive?.closed == true,
      clipboardGranted:
          connection.clipboardSync && connection.clipboardWebSocketUrl != null,
      clipboardReady: clipboardReady,
      clipboardFailed: clipboard?.closed == true,
      policy: clipboardReady
          ? clipboard.effectivePolicy
          : connection.clipboardPolicy,
    );
  }

  /// What an Android device can use in this connection: printing (through
  /// the device's own print screen), copy and paste of text, and, in a
  /// connection that has them, folders of the device shared with the desktop.
  factory SessionGuideContent.forAndroid(RemoteSession session) {
    final connection = session.connection;
    final printer = session.printer, clipboard = session.clipboard;
    final drive = session.drive;
    final clipboardReady =
        clipboard != null && clipboard.ready && !clipboard.closed;
    final policy = clipboardReady
        ? clipboard.effectivePolicy
        : connection.clipboardPolicy;
    SessionGuideAvailability state(bool granted, bool ready, bool failed) =>
        !granted || failed
        ? SessionGuideAvailability.unavailable
        : ready
        ? SessionGuideAvailability.ready
        : SessionGuideAvailability.preparing;
    final printState = state(
      connection.printWebSocketUrl != null,
      printer != null && printer.queueReady && !printer.closed,
      printer?.closed == true || printer?.queueReason != null,
    );
    final text = policy.textToRemote || policy.textToLocal;
    final clipboardState = state(
      connection.clipboardSync &&
          connection.clipboardWebSocketUrl != null &&
          text,
      clipboardReady,
      clipboard?.closed == true,
    );
    return SessionGuideContent([
      SessionGuideTip(
        SessionGuideFeature.printer,
        l10n.printer,
        printState == SessionGuideAvailability.unavailable
            ? l10n.askAdministrator
            : l10n.selectMultitapPrinterDesktopPrint,
        printState,
      ),
      // Listed only in a connection the broker granted it (as its menu row).
      if (connection.driveWebSocketUrl != null)
        SessionGuideTip(
          SessionGuideFeature.folders,
          l10n.sharedFolder,
          l10n.chooseFolderDeviceUnderShared,
          state(
            true,
            drive != null && drive.ready && !drive.closed,
            drive?.closed == true,
          ),
        ),
      SessionGuideTip(
        SessionGuideFeature.clipboard,
        l10n.copyPaste,
        clipboardState == SessionGuideAvailability.unavailable
            ? l10n.askAdministrator
            : policy.textToRemote && policy.textToLocal
            ? l10n.youCanPasteCopiedText
            : policy.textToRemote
            ? l10n.youCanPasteTextCopied
            : l10n.youCanPasteTextCopied2,
        clipboardState,
      ),
    ]);
  }

  /// A negotiated relay URL already requires assignment permission and peer
  /// support. Its absence cannot distinguish denial from unsupported hardware.
  /// An unavailable tip says only what to do: its badge already says it is
  /// unavailable (saying so again would be the same fact twice per row).
  factory SessionGuideContent.fromFacts({
    bool printerGranted = false,
    bool printerReady = false,
    bool printerFailed = false,
    bool usbGranted = false,
    bool usbReady = false,
    bool usbFailed = false,
    bool foldersGranted = false,
    bool foldersReady = false,
    bool foldersFailed = false,
    bool clipboardGranted = false,
    bool clipboardReady = false,
    bool clipboardFailed = false,
    ClipboardPolicy policy = const ClipboardPolicy(),
  }) {
    SessionGuideAvailability state(bool granted, bool ready, bool failed) =>
        !granted || failed
        ? SessionGuideAvailability.unavailable
        : ready
        ? SessionGuideAvailability.ready
        : SessionGuideAvailability.preparing;
    String prefix(SessionGuideAvailability value) =>
        value == SessionGuideAvailability.preparing ? l10n.onceConnectionReady : '';
    final printState = state(printerGranted, printerReady, printerFailed);
    final usbState = state(usbGranted, usbReady, usbFailed);
    final folderState = state(foldersGranted, foldersReady, foldersFailed);
    final clipboardState = state(
      clipboardGranted && policy.any,
      clipboardReady,
      clipboardFailed,
    );
    String direction(String label, bool toRemote, bool toLocal) =>
        '$label: ${toRemote && toLocal
            ? l10n.pcDesktop
            : toRemote
            ? l10n.pcDesktop2
            : l10n.desktopPc}';
    final directions = <String>[
      if (policy.textToRemote || policy.textToLocal)
        direction(l10n.text12, policy.textToRemote, policy.textToLocal),
      if (policy.filesToRemote || policy.filesToLocal)
        direction(l10n.files, policy.filesToRemote, policy.filesToLocal),
    ];
    return SessionGuideContent([
      SessionGuideTip(
        SessionGuideFeature.printer,
        l10n.printer,
        printState == SessionGuideAvailability.unavailable
            ? l10n.askAdministrator
            : l10n.selectMultitapPrinterDesktopPreview(prefix(printState)),
        printState,
      ),
      SessionGuideTip(
        SessionGuideFeature.usb,
        l10n.usbDevices,
        usbState == SessionGuideAvailability.unavailable
            ? l10n.askAdministrator
            : l10n.selectUsbDeviceToolbarConnected(prefix(usbState)),
        usbState,
      ),
      SessionGuideTip(
        SessionGuideFeature.folders,
        l10n.sharedFolder,
        folderState == SessionGuideAvailability.unavailable
            ? l10n.askAdministrator
            : l10n.selectPcFolderToolbarSharing(prefix(folderState)),
        folderState,
      ),
      SessionGuideTip(
        SessionGuideFeature.clipboard,
        l10n.copyPaste,
        clipboardState == SessionGuideAvailability.unavailable
            ? l10n.askAdministrator
            : l10n.useCtrlCCtrlV(prefix(clipboardState), directions.join('\n'), policy.filesToRemote || policy.filesToLocal ? l10n.filesTransferredWhenYouPaste : ''),
        clipboardState,
      ),
    ]);
  }
}
