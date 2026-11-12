import '../data/product_updates.dart';
import '../l10n/l10n.dart';

/// "0.4.62로", "0.2.60으로": the particle follows how the last number of the
/// version is read aloud (영·삼·육·십·백 take 으로).
String withRo(String version) {
  final last = version.split('.').last;
  final value = int.tryParse(last);
  if (value == null) return l10n.version3(version);
  // English needs no particle: "Update to 1.2.3".
  if (AppLanguageController.instance.code != 'ko') return version;
  final digit = value % 10;
  return '$version${digit == 0 || digit == 3 || digit == 6 ? l10n.text34 : l10n.text35}';
}

/// The short product name in sentences.
String updateProductLabel(String product) =>
    product == 'host' ? 'Multitap Host' : 'Multitap';

/// The time left while an update downloads, from the average rate so far.
String remainingDownloadText(int received, int total, Duration elapsed) {
  if (received <= 0 || elapsed < const Duration(seconds: 1)) {
    return l10n.calculatingTimeLeft;
  }
  final seconds =
      ((total - received) * elapsed.inMilliseconds / received / 1000).ceil();
  if (seconds <= 0) return l10n.almostDone;
  if (seconds < 60) return l10n.aboutSecLeft(seconds);
  return l10n.aboutMinLeft2((seconds / 60).ceil());
}

String updateFailureTitle(String code) =>
    updateNeedsInstaller(code) ? l10n.checkNeeded2 : l10n.couldntUpdate;

/// The text for a fixed update code. [afterRelaunch]: the installer ran after
/// this app closed and opened the previous version again.
String updateFailureMessage(
  String code, {
  required String product,
  String? version,
  bool afterRelaunch = false,
}) {
  final host = product == 'host';
  final kept = host ? l10n.existingHostUnchanged : l10n.installedAppUnchanged;
  switch (code) {
    case 'catalog_unavailable':
    case 'invalid_catalog':
      return l10n.couldntGetUpdateInfoCheck;
    case 'download_failed':
      return l10n.couldntDownloadFileCheckNetwork;
    case 'download_network':
      return l10n.networkDroppedSoFileWasnt;
    case 'download_timeout':
      return l10n.serverDidntRespondWasToo;
    case 'download_server_error':
      return l10n.serverCouldntRespondJustNow;
    case 'download_not_found':
      return l10n.serverDoesntHaveFileVersion;
    case 'download_http_status':
      return l10n.serverRefusedFileRequestCheck;
    case 'download_disk_full':
      return l10n.notEnoughDiskSpaceSave;
    case 'download_file_error':
      return l10n.couldntSaveFileTempFolder;
    case 'download_size_mismatch':
      return l10n.downloadedFileSizeDoesntMatch;
    case 'hash_mismatch':
      return l10n.downloadedFileDoesntMatchRelease;
    case 'package_version_mismatch':
      return l10n.fileVersionDoesntMatchRelease(version == null ? '' : '($version)', kept);
    case 'signature_invalid':
      return l10n.releaseSignatureMissingInvalidSo(kept);
    case 'elevation_cancelled':
      return l10n.administratorApprovalWasCancelledSo(kept);
    case 'update_not_newer':
      return l10n.fileServerIsntNewerThan(kept);
    case 'installer_exited':
      return l10n.installerClosedBeforeUpdateStarted(kept);
    case 'session_windows_open':
      return l10n.couldntCloseOpenConnectionsDisconnect;
    case 'version_unknown':
      return l10n.couldntCheckRunningVersionRestart;
    case 'update_busy':
      return l10n.updateAlreadyBeingPreparedTry;
    case 'installer_missing':
      return l10n.installationFilesNotValidInstall(kept);
    case 'progress_channel_failed':
      return l10n.couldntCommunicateInstallerTryAgain(kept);
    case 'existing_install_required':
      return l10n.desktopHasNoHostInstalled;
    case 'certificate_trust_required':
      return l10n.windowsDoesntTrustVirtualDisplay;
    case 'driver_install_required':
      return l10n.updateNeedsInstallChangeVirtual;
    case 'update_origin_unverified':
      return l10n.fileCameFromSomewhereOther(kept);
    case 'rollback_failed':
      // Only a Host update has a Host service to look at.
      return host
          ? l10n.installationStoppedItsUnknownWhether
          : l10n.installationStoppedReinstallApp;
    case 'install_failed':
      if (afterRelaunch) {
        return host
            ? l10n.problemOccurredDuringInstallationSo
            : l10n.problemOccurredDuringInstallationSo2;
      }
      return l10n.couldntCompleteInstallationItsUnknown;
    default:
      return l10n.couldntStartInstallerTryAgain(kept);
  }
}
