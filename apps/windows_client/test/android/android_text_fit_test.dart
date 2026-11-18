import 'dart:convert';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

// Texts that have one line on a phone (owner, 2026-11-18: long texts that
// break into two lines). Each is measured in the app's own font against the
// narrowest space it is shown in at 360 dp, in Korean and in English; a
// text with a line break of its own is measured line by line. A text that
// grows past its space fails here instead of wrapping or being cut on a phone.
// The widths came from an audit of every Android screen the tests draw
// (flutter_test_config.dart, WRAP_AUDIT).
class _Fit {
  const _Fit(this.key, this.font, this.limit, {this.bold = false, this.sample});
  final String key;
  final double font, limit;
  final bool bold;

  /// What stands in for a placeholder.
  final String? sample;
}

void main() {
  const fits = <_Fit>[
    // The second line of a desktop card (status detail).
    _Fit('availableViaCheckedAgainWhen', 12.5, 264),
    _Fit('bringsWindowsScreenUsedRdp', 12.5, 264),
    _Fit('changesAutomaticallyWhenRestartFinishes', 12.5, 264),
    _Fit('changesAutomaticallyWhenStartupFinishes', 12.5, 264),
    _Fit('checkConnectionStatusHostDiagnostics', 12.5, 264),
    _Fit('checkingScreenRestoreFromLast', 12.5, 264),
    _Fit('checkingWindowsSignStateUpdates', 12.5, 264),
    _Fit('cleaningUpConnectionJustEnded', 12.5, 264),
    _Fit('couldntConfirmUpdateResultConnection', 12.5, 264),
    _Fit('couldntGetResponseFromAfter', 12.5, 264),
    _Fit('couldntGetResponseFromAfter2', 12.5, 264),
    _Fit('desktopRunningChangesAutomaticallyWhen', 12.5, 264),
    _Fit('filesSha256DoesntMatch', 12.5, 264),
    _Fit('getsVirtualDisplayReadyUnsaved', 12.5, 264),
    _Fit('hostPreparingConnectionUpdatesAs', 12.5, 264),
    _Fit('hostUpdatedWindowsRestartNeeded', 12.5, 264),
    _Fit('hostUpdateWaitingStartsWhen', 12.5, 264),
    _Fit('ifSigningAutomaticallyYouCan', 12.5, 264),
    _Fit('ifWindowsDoesNotRespond', 12.5, 264),
    _Fit('ifYouJustStartedWait', 12.5, 264),
    _Fit('notCheckedCheckedOnceEarlier', 12.5, 264),
    _Fit('notRespondingIfWindowsStarting', 12.5, 264),
    _Fit('preparingVirtualDisplayUpdatesAs', 12.5, 264),
    _Fit('readyOnceScreenUsedRdp', 12.5, 264),
    _Fit('refreshCheckAgainIfKeeps2', 12.5, 264),
    _Fit('refreshMomentIfKeepsHappening', 12.5, 264),
    _Fit('remoteUpdateNotSupportedInstall2', 12.5, 264),
    _Fit('samePcDetail', 12.5, 264),
    _Fit('screenRestoreFromLastConnection', 12.5, 264),
    _Fit('signFromConsoleAfterRestart', 12.5, 264),
    _Fit('virtualDisplayCantPreparedRdp2', 12.5, 264),
    _Fit('windowsHasStartedChangesAs', 12.5, 264),
    _Fit('youCanCheckHostSettings', 12.5, 264),
    _Fit('youCanDisconnectConsoleConnect', 12.5, 264),
    // The first line of a desktop card (status).
    _Fit('checkingStreamingReadiness', 13, 152, bold: true),
    _Fit('cleaningUpLastConnection', 13, 152, bold: true),
    _Fit('couldntCheckVirtualDisplayStatus2', 13, 152, bold: true),
    _Fit('couldntCheckWindowsScreen', 13, 152, bold: true),
    _Fit('hostDidntReconnectAfterUpdate', 13, 152, bold: true),
    _Fit('hostNotResponding', 13, 152, bold: true),
    _Fit('hostSettingUpScreen', 13, 152, bold: true),
    _Fit('notSignedWindows', 13, 152, bold: true),
    _Fit('preparingConnection', 13, 152, bold: true),
    _Fit('screenRestoreNotConfirmed', 13, 152, bold: true),
    _Fit('settingUpVirtualDisplay', 13, 152, bold: true),
    _Fit('signWindowsAdministratorAccount', 13, 152, bold: true),
    _Fit('takeOverWindowsScreenConnect', 13, 152, bold: true),
    _Fit('virtualDisplayDriverNeedsChecking', 13, 152, bold: true),
    _Fit('virtualDisplayIsntReady', 13, 152, bold: true),
    _Fit('virtualDisplayNeedsRepair', 13, 152, bold: true),
    _Fit('virtualDisplaySettingsNeedChecking2', 13, 152, bold: true),
    _Fit('waitingHostConnect2', 13, 152, bold: true),
    _Fit('windowsAccountNeedsChecking', 13, 152, bold: true),
    _Fit('windowsNeedsRestart', 13, 152, bold: true),
    _Fit('windowsScreenFromRdpOpen', 13, 152, bold: true),
    _Fit('windowsStarting2', 13, 152, bold: true),
    _Fit('androidCheckingDesktopAgain', 13, 152, bold: true),
    // Rows, captions, notices, titles: each where it is shown.
    _Fit('locksMouseInsideDesktop', 12, 149),
    _Fit('availableWhenMouseConnected', 12, 149),
    _Fit('useDevicesFoldersDesktop', 12, 190),
    _Fit('webclientServiceDesktopOffMissing', 12, 185, bold: true),
    _Fit('resolutionWindowsScaleSeparate', 12, 256, bold: true),
    _Fit('windowsScale', 12, 276, sample: '수동 100% 요청 · 실제 값 미확인'),
    _Fit('requestedActualValueUnknown', 12, 400),
    _Fit('actualValueUnknown', 12, 400),
    _Fit('actualValueUnknownNotSupported', 12, 200),
    _Fit('checkingSharp', 12, 256, bold: true, sample: 'Current 1052×2104'),
    _Fit('youCanPasteCopiedText', 12.5, 220),
    _Fit('stoppedSharingBecausePermissionOpen', 13, 296, sample: 'Documents'),
    _Fit('deviceCantReceiveSharperPicture', 13, 296, sample: 'Documents'),
    _Fit('setLargestScaleWindowsAllows', 13, 296, sample: '150'),
    _Fit('mouseLockedInsideDesktopPress', 13, 296, sample: 'Documents'),
    _Fit('someGamesSuchAsLeague', 13, 296, sample: 'Documents'),
    _Fit('hostReceivesKoreanTextAs', 13, 296, sample: 'Documents'),
    _Fit('couldntSendInputWhileScreen', 13, 296, sample: 'Documents'),
    _Fit('desktopHasNoSoundDevice2', 12, 256, sample: 'Documents'),
    _Fit('couldntApplySharpSoShows', 13, 296, sample: 'Documents'),
    _Fit('shareFolderDesktopAsDrive', 17, 240, bold: true, sample: 'Documents'),
    _Fit('signOutElsewhereSignHere', 17, 240, bold: true, sample: 'Documents'),
    _Fit('disconnectConsoleConnect', 17, 240, bold: true, sample: 'Documents'),
    _Fit('noDesktopsMatch2', 16, 236, sample: 'no match'),
    _Fit('serverIsntRespondingTryAgain', 13, 188, bold: true),
    _Fit('serverCantCheckUpdatePermission', 12, 214, sample: 'Documents'),
    _Fit('currentVersionUnknown', 12, 137, sample: 'Documents'),
    _Fit('notUseUpdatesRightAway', 12, 214, sample: 'Documents'),
    _Fit('updatesWhenOtherUsersConnection', 12, 214, sample: 'Documents'),
    _Fit('statusServerGotFromHost', 12, 100, bold: true),
    _Fit('bringScreenIntoStreamingConnect', 12.5, 210, sample: 'Documents'),
    _Fit('passwordEncryptedKeptDevice', 12, 290, sample: 'Documents'),
    _Fit('stopIsntAvailableServerYet', 12, 187, sample: 'Documents'),
    _Fit('plannedMaintenance', 12, 109, bold: true, sample: 'Documents'),
    _Fit('recommended2', 12, 58, bold: true),
    _Fit('couldntConfirmShortcutWasSent', 13, 228, bold: true),
    _Fit('onlyFoldersYouChooseShared', 12.5, 280, bold: true),
    _Fit('serverIsntRespondingMomentRetrying', 13, 300, bold: true),
    _Fit('changesAsSoonAsHost', 12.5, 225, bold: true),
    _Fit('statusCheckedAgainWhenFinishes', 12.5, 250, bold: true),
    _Fit('openDesktopConsoleCheckHost', 12.5, 250, bold: true),
    _Fit('checkServiceStatusLocalPcs', 12.5, 250, bold: true),
    _Fit('virtualDisplayNeedsCheckedHost', 12.5, 250, bold: true),
    _Fit('youCanRepairHostSettings', 12.5, 250, bold: true),
  ];

  test('Android texts fit the line they are shown on, in both languages', () async {
    for (final weight in ['Regular', 'Medium', 'SemiBold', 'Bold']) {
      final loader = FontLoader('Pretendard')
        ..addFont(
          Future.value(
            ByteData.sublistView(
              File('assets/fonts/Pretendard-$weight.otf').readAsBytesSync(),
            ),
          ),
        );
      await loader.load();
    }
    final over = <String>[];
    for (final lang in ['ko', 'en']) {
      final arb = jsonDecode(
        File('lib/l10n/app_$lang.arb').readAsStringSync(),
      ) as Map<String, dynamic>;
      for (final fit in fits) {
        final raw = arb[fit.key];
        expect(raw, isA<String>(), reason: '$lang ${fit.key}');
        final text = (raw as String).replaceAllMapped(
          RegExp(r'\{[a-zA-Z0-9_]+\}'),
          (_) => fit.sample ?? 'XXXXXX',
        );
        for (final line in text.split('\n')) {
          final painter = TextPainter(
            text: TextSpan(
              text: line,
              style: TextStyle(
                fontFamily: 'Pretendard',
                fontSize: fit.font,
                fontWeight: fit.bold ? FontWeight.w600 : FontWeight.w400,
              ),
            ),
            textDirection: TextDirection.ltr,
          )..layout();
          if (painter.width > fit.limit) {
            over.add(
              '$lang ${fit.key}: ${painter.width.round()} > ${fit.limit.round()} "$line"',
            );
          }
          painter.dispose();
        }
      }
    }
    expect(over, isEmpty, reason: over.join('\n'));
  });
}
