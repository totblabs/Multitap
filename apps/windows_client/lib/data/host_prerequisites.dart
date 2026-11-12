import '../l10n/l10n.dart';

/// host-prerequisites-v1 (apps/windows_host/src/host_prerequisites.hpp): what a
/// Host needs besides its own program files. The Host installer places files
/// and the service only; Host settings finishes a first install: the virtual
/// display driver (after one confirmation that names the certificate it
/// trusts) and then WinFsp. Fixed text only; never a device name or a Windows
/// error.
class HostPrerequisites {
  const HostPrerequisites({
    required this.displayState,
    this.reason = '',
    this.certificateTrusted = false,
    this.remoteSession = false,
  });

  /// `ready`, `install` (nothing is selected and this Host's package can be
  /// installed), `restart`, or `attention` (see [reason]).
  final String displayState;
  final String reason;
  final bool certificateTrusted, remoteSession;

  static final _code = RegExp(r'^[a-z0-9_]{1,64}$');

  /// Null for anything that is not this reply: absence is never "installable".
  static HostPrerequisites? parse(Object? value) {
    if (value is! Map || value['schema'] != 1) return null;
    final display = value['display'];
    if (display is! Map) return null;
    final state = display['state'], reason = display['reason'];
    if (!const {'ready', 'install', 'restart', 'attention'}.contains(state)) {
      return null;
    }
    return HostPrerequisites(
      displayState: state as String,
      reason: reason is String && _code.hasMatch(reason) ? reason : '',
      certificateTrusted: display['certificate'] == 'trusted',
      remoteSession: value['remoteSession'] == true,
    );
  }

  bool get displayInstallable => displayState == 'install';

  /// Why the driver cannot be installed from here, in two lines.
  String? get attention => displayState != 'attention'
      ? null
      : switch (reason) {
          'native_selection_invalid' ||
          'native_selected_device_unverified' =>
            l10n.existingVirtualDisplaySettingsNeed,
          'native_display_device_not_started' =>
            l10n.virtualDisplayDeviceDidntStart,
          _ => l10n.couldntVerifyVirtualDisplayDriver,
        };
}

String get hostFinishTitle => l10n.installationNeedsFinishing;
String get hostFinishAction => l10n.finishInstallation;
String get hostRestartNotice =>
    l10n.restartWindowsApplySaveWork;

/// The overview's banner while something is missing.
String hostFinishCardMessage({required bool display, required bool winfsp}) =>
    display && winfsp
    ? l10n.virtualDisplayDriverSharedFolder
    : display
    ? l10n.virtualDisplayDriverIsntInstalled
    : l10n.sharedFolderComponentWinfspIsnt;

/// The one confirmation before the driver is installed. With [trust] it names
/// the only trust change this product makes: one code-signing certificate in
/// two local-machine stores.
({String title, String message, String accept}) hostFinishConfirmation({
  required bool trust,
  required bool winfsp,
}) => trust
    ? (
        title: l10n.trustVirtualDisplayDriversSignature,
        message:
            l10n.virtualDisplayOnlyOriginalSunshine(winfsp ? l10n.thenDriverSharedFolderComponent : l10n.thenDriverInstalled),
        accept: l10n.trustInstall,
      )
    : (
        title: l10n.installVirtualDisplayDriver,
        message:
            l10n.installsAlreadyTrustedOriginalSunshine(winfsp ? l10n.sharedFolderComponentWinfspInstalled : ''),
        accept: l10n.install,
      );

/// The result of `installNativeDisplay`: installed (with a plain restart or
/// console notice), already there, or the cause and one next action. Windows is
/// never restarted from here.
({bool ok, String message}) hostNativeDisplayInstallResult(
  Map<String, dynamic> reply, {
  bool remoteSession = false,
}) {
  final result = reply['nativeDisplay'];
  if (result is! Map || result['error'] is! String) {
    return (ok: false, message: l10n.couldntConfirmVirtualDisplayDriver);
  }
  final code = result['error'] as String;
  if (code.isEmpty) {
    final kept = result['action'] == 'kept';
    return (
      ok: true,
      message: [
        kept ? l10n.virtualDisplayDriverAlreadyInstalled : l10n.virtualDisplayDriverInstalled,
        if (result['restart'] == true)
          hostRestartNotice
        else if (remoteSession)
          l10n.rdpSignYouCantCheck,
        if (result['service'] == 'unverified')
          l10n.couldntRestartHostServiceRestart,
      ].join('\n'),
    );
  }
  final cause = switch (code) {
    'confirmation_required' ||
    'certificate_trust_required' => l10n.notInstalledBecauseSignatureVerification,
    'native_display_package_missing' ||
    'native_display_package_mismatch' ||
    'native_display_package_unprotected' ||
    'native_display_publisher_mismatch' ||
    'native_display_certificate_expired' =>
      l10n.couldntVerifyVirtualDisplayDriver,
    'certificate_store_failed' ||
    'native_display_catalog_untrusted' =>
      l10n.couldntRegisterDriverSignatureWindows,
    'native_selection_invalid' ||
    'native_selected_device_unverified' =>
      l10n.existingVirtualDisplaySettingsNeed2,
    'native_display_install_busy' => l10n.anotherInstallationProgressTryAgain,
    'native_selection_write_failed' => l10n.couldntSaveInstallResultTry,
    _ => l10n.windowsCouldntInstallVirtualDisplay,
  };
  final safe = RegExp(r'^[a-z0-9_]{1,64}$').hasMatch(code)
      ? l10n.errorCode(code)
      : '';
  return (ok: false, message: '$cause$safe');
}

/// One result for the whole finish: the driver's lines, then what happened with
/// WinFsp: installed in this run (only its first line when it simply worked),
/// failed, or [winfspPresent]: it was there already, so nothing was installed.
({bool ok, String message}) hostFinishSummary({
  ({bool ok, String message})? display,
  ({bool ok, String message})? winfsp,
  bool winfspPresent = false,
}) {
  final lines = <String>[
    if (display != null) display.message,
    if (winfsp != null)
      winfsp.ok &&
              // A restart notice keeps every line (either language's word).
              !winfsp.message.toLowerCase().contains(l10n.restart.toLowerCase())
          ? winfsp.message.split('\n').first
          : winfsp.message
    else if (winfspPresent)
      l10n.winfspWasAlreadyInstalled,
  ];
  return (
    ok: (display?.ok ?? true) && (winfsp?.ok ?? true),
    message: lines.isEmpty ? l10n.nothingInstall : lines.join('\n'),
  );
}
