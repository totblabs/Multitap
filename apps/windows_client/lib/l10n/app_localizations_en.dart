// ignore: unused_import
import 'package:intl/intl.dart' as intl;

import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for English (`en`).
class AppLocalizationsEn extends AppLocalizations {
  AppLocalizationsEn([String locale = 'en']) : super(locale);

  @override
  String get couldntSavePreviewSetting => 'Couldn\'t save the preview setting.';

  @override
  String get quitApp => 'Quit the app?';

  @override
  String get appClosesRemovedFromRecent =>
      'The app closes and is removed from recent apps.\nYour desktop stays on.';

  @override
  String get quit => 'Quit';

  @override
  String get disconnectRemoteScreenThenQuit =>
      'Disconnect in the remote screen, then quit.';

  @override
  String get couldntQuitAppTryAgain => 'Couldn\'t quit the app. Try again.';

  @override
  String get desktopNoLongerAssignedYou =>
      'This desktop is no longer assigned to you.';

  @override
  String get couldntChangeFavoritesTryAgain =>
      'Couldn\'t change favorites. Try again in a moment.';

  @override
  String get updateInstalled => 'Update installed.';

  @override
  String get couldntLoadSavedServerSettings =>
      'Couldn\'t load the saved server settings. Enter the address again in server settings.';

  @override
  String get couldntLoadSavedSignDetails =>
      'Couldn\'t load the saved sign-in details. Enter your ID and password.';

  @override
  String get couldntLoadServerSettings => 'Couldn\'t load the server settings.';

  @override
  String get couldntTurnOffRememberPassword =>
      'Couldn\'t turn off remember password.';

  @override
  String get enterIdPassword => 'Enter your ID and password.';

  @override
  String get signedButCouldntSaveRemember =>
      'Signed in, but couldn\'t save the remember ID and password setting.';

  @override
  String get signHasExpiredSignAgain =>
      'Your sign-in has expired. Sign in again.';

  @override
  String get signOut => 'Sign out?';

  @override
  String get server => 'Server';

  @override
  String get desktopsRunningWorkStayAs =>
      'Your desktops and running work stay as they are.';

  @override
  String get signOut2 => 'Sign out';

  @override
  String get notificationsOffSoConnectionStatus =>
      'Notifications are off, so the connection status and the ‘Disconnect’ button don\'t show in the notification shade.';

  @override
  String get notificationSettings => 'Notification settings';

  @override
  String get couldntProcessRequestCheckNetwork =>
      'Couldn\'t process the request. Check your network and try again.';

  @override
  String get anotherConnectionOpening => 'Another connection is opening.';

  @override
  String get desktopTaskProgress => 'A desktop task is in progress.';

  @override
  String get startDesktopThenConnect => 'Start the desktop, then connect.';

  @override
  String get endCurrentConnectionThenConnect =>
      'End the current connection, then connect.';

  @override
  String get disconnectConsoleConnect => 'Disconnect Console and connect?';

  @override
  String get openConsoleConnectionWillEnd =>
      'The open Console connection will end.\nThe desktop and running work stay as they are.';

  @override
  String get disconnectConnect => 'Disconnect and connect';

  @override
  String get couldntEndConsoleConnection =>
      'Couldn\'t end the Console connection.';

  @override
  String get openConsole => 'Open in Console?';

  @override
  String opensDesktopsProxmoxConsoleApp(Object desktopName) {
    return '$desktopName\n\nOpens the desktop\'s Proxmox console in the app.\nNo separate Proxmox sign-in needed.';
  }

  @override
  String get openConsole2 => 'Open in Console';

  @override
  String get couldntOpenConsoleCheckNetwork =>
      'Couldn\'t open Console. Check your network and try again.';

  @override
  String connect(Object assignedName) {
    return 'Connect to $assignedName?';
  }

  @override
  String get connect2 => 'Connect';

  @override
  String get couldntSaveStreamingSettingsThey =>
      'Couldn\'t save the streaming settings. They apply to this connection only.';

  @override
  String get bringOverWindowsScreen => 'Bring over the Windows screen?';

  @override
  String continuesWindowsScreenYouWere(Object desktopName) {
    return '$desktopName\nContinues on the Windows screen you were using in RDP.';
  }

  @override
  String get bringOverConnect => 'Bring over and connect';

  @override
  String waitingPreviousConnectionCleanUp(Object label) {
    return 'Waiting for the previous connection to clean up, so the $label request wasn\'t accepted. Try again in a moment.';
  }

  @override
  String numberMonitorsChangingSoRequest(Object label) {
    return 'The number of monitors is changing, so the $label request wasn\'t accepted. Try again when it\'s done.';
  }

  @override
  String hostUpdateInstallingSoRequest(Object label) {
    return 'A Host update is installing, so the $label request wasn\'t accepted. Try again after it finishes.';
  }

  @override
  String couldntProcessRequestTryAgain(Object label) {
    return 'Couldn\'t process the $label request. Try again.';
  }

  @override
  String get autoConnectStoppedBecauseDesktop =>
      'Auto connect stopped because the desktop wasn\'t ready within 5 minutes. Check the status in the connection readiness check.';

  @override
  String get autoConnectCanceled => 'Auto connect canceled.';

  @override
  String get couldntFindDesktop => 'Couldn\'t find this desktop';

  @override
  String get unknown => 'Unknown';

  @override
  String get passwordChangedSignNewPassword =>
      'Password changed. Sign in with your new password.';

  @override
  String get powerTaskProgress => 'A power task is in progress.';

  @override
  String get serverLocalPcsHostDoesnt =>
      'This server or the local PC\'s Host doesn\'t support remote power.';

  @override
  String get checkingDesktopsPowerStateRefresh =>
      'Checking the desktop\'s power state. Refresh in a moment.';

  @override
  String get numberMonitorsChangingPowerAvailable =>
      'The number of monitors is changing. Power is available when it\'s done.';

  @override
  String get refresh => 'Refresh';

  @override
  String get checkingList => 'Checking the list';

  @override
  String get refreshList => 'Refresh list';

  @override
  String get previews => 'Previews';

  @override
  String get screenPreviewsTapTurnOff => 'Screen previews on · tap to turn off';

  @override
  String get screenPreviewsOffTapTurn => 'Screen previews off · tap to turn on';

  @override
  String get search => 'Search';

  @override
  String sort(Object sortLabel) {
    return 'Sort · $sortLabel';
  }

  @override
  String get clearAllFilters => 'Clear all filters';

  @override
  String get myDesktops => 'My desktops';

  @override
  String get tryAgain => 'Try again';

  @override
  String get couldntLoadList => 'Couldn\'t load the list';

  @override
  String get noDesktopsAssignedYet => 'No desktops assigned yet';

  @override
  String get askAdministratorAssignDesktopPull =>
      'Ask your administrator to assign a desktop.\nPull down to check again.';

  @override
  String listMayOutDate(Object listError) {
    return 'The list may be out of date. $listError';
  }

  @override
  String get clearFilters => 'Clear filters';

  @override
  String get clearSearch => 'Clear search';

  @override
  String connectionOkMultitapMs(Object version, Object elapsedMs) {
    return 'Connection OK · Multitap $version · $elapsedMs ms';
  }

  @override
  String get checkServerAddressPort => 'Check the server address and port.';

  @override
  String get couldntSaveServerAddress => 'Couldn\'t save the server address.';

  @override
  String get signServer => 'Sign-in server';

  @override
  String get httpsSecureConnection => 'HTTPS secure connection';

  @override
  String get httpNotEncrypted => 'HTTP · not encrypted';

  @override
  String get youCanSaveAddressAfter =>
      'You can save this address after a successful test.';

  @override
  String get serverAddress => 'Server address';

  @override
  String get httpsServerAddress => 'https://server-address';

  @override
  String get testConnection => 'Test connection';

  @override
  String get cancel => 'Cancel';

  @override
  String get save => 'Save';

  @override
  String get streamingSettings => 'Streaming settings';

  @override
  String get custom => 'Custom';

  @override
  String qualityMode(Object value) {
    return 'Quality mode · $value';
  }

  @override
  String get useRecommended => 'Use recommended';

  @override
  String get workKeepsTextSharpSaver =>
      'Work keeps text sharp; Saver uses less data.';

  @override
  String get advancedSettings => 'Advanced settings';

  @override
  String get targetFrameRate => 'Target frame rate';

  @override
  String get videoBitrateMbps => 'Video bitrate · Mbps';

  @override
  String get auto => 'Auto';

  @override
  String get fitsOneVirtualDisplayDevices =>
      'Fits one virtual display to this device\'s size.\nOther streaming connections may be disconnected.';

  @override
  String get newPasswordMust10256 =>
      'The new password must be 10–256 characters and match the confirmation.';

  @override
  String get couldntChangePassword => 'Couldn\'t change the password.';

  @override
  String get changePassword => 'Change password';

  @override
  String get changingSignsYouOutAll =>
      'Changing it signs you out on all devices.';

  @override
  String get currentPassword => 'Current password';

  @override
  String get newPassword => 'New password';

  @override
  String get confirmNewPassword => 'Confirm new password';

  @override
  String get least10Characters => 'At least 10 characters';

  @override
  String get change => 'Change';

  @override
  String get disconnect => 'Disconnect';

  @override
  String get keyboard => 'Keyboard';

  @override
  String get screen => 'Screen';

  @override
  String get originalSize => 'Original size';

  @override
  String get windowsKey => 'Windows key';

  @override
  String get reconnect => 'Reconnect';

  @override
  String get close => 'Close';

  @override
  String get console => 'Console';

  @override
  String get noPermissionOpenFolderChoose =>
      'No permission to open this folder. Choose the folder again.';

  @override
  String get cantFindFolderDeviceMay =>
      'Can\'t find the folder on this device. It may have been deleted, or the SD card or USB storage was removed.';

  @override
  String get filesAppTurnedOffSo =>
      'The ‘Files’ app is turned off, so you can\'t choose a folder.\nTap ‘Enable’ in the ‘Files’ app settings, then try again.';

  @override
  String get deviceHasNoFolderPicker =>
      'This device has no folder picker, so shared folders can\'t be used.';

  @override
  String get couldntOpenFolderPickerTry =>
      'Couldn\'t open the folder picker.\nTry again, and if it keeps happening, tell your administrator.';

  @override
  String get device => 'This device';

  @override
  String get openFilesAppSettings => 'Open ‘Files’ app settings';

  @override
  String get youreAboutShareAllStorage =>
      'You\'re about to share all of the storage.\nPrograms on the desktop can read and change every file in it.\nChoosing only the folders you need is recommended.';

  @override
  String get chosenFolder => 'Chosen folder';

  @override
  String stoppedSharingBecausePermissionOpen(Object name) {
    return 'Stopped sharing because permission to open the ‘$name’ folder was lost';
  }

  @override
  String stoppedSharingBecauseFolderCant(Object name) {
    return 'Stopped sharing because the ‘$name’ folder can\'t be found';
  }

  @override
  String get cantOpenFoldersDeviceCheck =>
      'Can\'t open folders on this device. Check the SD card or USB storage and try again.';

  @override
  String get timesAverageMaximumOverLast =>
      'Times are the average and maximum over the last second that had frames.\nThey don\'t include the time until a frame actually appears on screen.';

  @override
  String get notMeasured => 'Not measured';

  @override
  String avg(Object average) {
    return 'Avg $average';
  }

  @override
  String avgMax(Object average, Object highest) {
    return 'Avg $average · max $highest';
  }

  @override
  String get preparing => 'Preparing';

  @override
  String get defaultFallbackAfterError => 'Default (fallback after an error)';

  @override
  String get androidLowLatency => 'Android low latency';

  @override
  String get realTimePriority => 'Real-time priority';

  @override
  String get vendor => 'Vendor';

  @override
  String lowLatencyKeys(Object vendorKeys) {
    return '$vendorKeys low-latency keys';
  }

  @override
  String get offDefault => 'Off (default)';

  @override
  String get onLabel => 'On';

  @override
  String get off => 'Off';

  @override
  String get normal => 'Normal';

  @override
  String get slightlyWarm => 'Slightly warm';

  @override
  String get warm => 'Warm';

  @override
  String get hot => 'Hot';

  @override
  String get veryHot => 'Very hot';

  @override
  String get critical => 'Critical';

  @override
  String get aboutShutDown => 'About to shut down';

  @override
  String highestConnection(Object now, Object top) {
    return '$now (highest this connection: $top)';
  }

  @override
  String get requested => 'Requested';

  @override
  String get requestedHighPerformanceMode =>
      'Requested (high-performance mode)';

  @override
  String get refusedByDevice => 'Refused by device';

  @override
  String get notRequested => 'Not requested';

  @override
  String get decoderLowLatency => 'Decoder low latency';

  @override
  String get decoderFallback => 'Decoder fallback';

  @override
  String get none => 'None';

  @override
  String text(num fallbacks) {
    String _temp0 = intl.Intl.pluralLogic(
      fallbacks,
      locale: localeName,
      other: '$fallbacks times',
      one: 'once',
    );
    return '$_temp0';
  }

  @override
  String get decodeTime => 'Decode time';

  @override
  String get receivedDecoded => 'Received→decoded';

  @override
  String get decodedSentScreen => 'Decoded→sent to screen';

  @override
  String get framesReplacedWaitingDropped =>
      'Frames (replaced · waiting · dropped)';

  @override
  String get audioBuffer => 'Audio buffer';

  @override
  String queuedTargetBuffer(Object queued, Object target, Object buffer) {
    return 'Queued $queued · target $target · buffer $buffer';
  }

  @override
  String get audioOutput => 'Audio output';

  @override
  String get lowLatencyPath => 'Low-latency path';

  @override
  String lowLatencyPathLatency(Object output) {
    return 'Low-latency path · latency $output';
  }

  @override
  String get normalPath => 'Normal path';

  @override
  String normalPathLatency(Object output) {
    return 'Normal path · latency $output';
  }

  @override
  String get displayRefreshRate => 'Display refresh rate';

  @override
  String get sharp => 'Sharp';

  @override
  String get wiFiLowLatency => 'Wi-Fi low latency';

  @override
  String get deviceHeat => 'Device heat';

  @override
  String get allowAccessAllFiles => 'Allow access to all files?';

  @override
  String get chooseFolderMultitapNeedsSee =>
      'To choose a folder, Multitap needs to see this device\'s storage.\nIf you allow it, Multitap can read and write the storage. The desktop sees only the folder you choose.';

  @override
  String get openSettings => 'Open settings';

  @override
  String get cantOpenFolderChooseAnother =>
      'Can\'t open this folder. Choose another folder.';

  @override
  String get upOneFolder => 'Up one folder';

  @override
  String get chooseFolderShare => 'Choose a folder to share';

  @override
  String get storage => 'Storage';

  @override
  String get couldntFindStorage => 'Couldn\'t find the storage.';

  @override
  String get noFoldersInside => 'No folders inside.';

  @override
  String get shareFolder => 'Share this folder';

  @override
  String get lostConnectionHostCouldntConfirm =>
      'Lost the connection to the Host. Couldn\'t confirm the result of the update request.';

  @override
  String get couldntConfirmResultUpdateRequest =>
      'Couldn\'t confirm the result of the update request. Check the list again.';

  @override
  String get couldntConfirmResultUpdateRequest2 =>
      'Couldn\'t confirm the result of the update request.';

  @override
  String currentStatus(Object observed) {
    return 'Current status · $observed';
  }

  @override
  String lastCheckedStatus(Object observed) {
    return 'Last checked status · $observed';
  }

  @override
  String get localPc => 'Local PC';

  @override
  String get desktop => 'Desktop';

  @override
  String serverSignStatusChangedCancel(Object kind) {
    return 'The server, sign-in or $kind status changed. Cancel and open this again.';
  }

  @override
  String get targetListDiffersFromLatest =>
      'The target in the list differs from the latest Host release. Refresh the list and check again.';

  @override
  String get couldntFindLatestHostRelease =>
      'Couldn\'t find the latest Host release information.';

  @override
  String get couldntCheckHostReleaseInformation =>
      'Couldn\'t check the Host release information and change log. Check again.';

  @override
  String get hostUpdate => 'Host update';

  @override
  String currentNewVersion(Object currentVersion, Object targetVersion) {
    return 'Current $currentVersion\nNew version $targetVersion';
  }

  @override
  String get updateStartsWhenConnectionUse =>
      'The update starts when the connection in use ends.';

  @override
  String updatesOnlyHostProgramYou(Object kind) {
    return 'Updates only the Host program on the $kind.\nYou can\'t connect for a moment while it installs.';
  }

  @override
  String get changeLog => 'Change log';

  @override
  String get noChangeLogRegisteredServer =>
      'No change log is registered on this server.';

  @override
  String get checkAgain => 'Check again';

  @override
  String get update => 'Update';

  @override
  String get themeBackground => 'Theme and background';

  @override
  String get theme => 'theme';

  @override
  String get background => 'background';

  @override
  String couldntSaveChooseAgain(Object what) {
    return 'Couldn\'t save the $what. Choose again.';
  }

  @override
  String get dark => 'Dark';

  @override
  String get light => 'Light';

  @override
  String background2(Object sceneLabel) {
    return 'Background · $sceneLabel';
  }

  @override
  String hostUpdates(Object hostUpdates) {
    return 'Host updates: $hostUpdates';
  }

  @override
  String newVersion(Object update) {
    return 'New version $update';
  }

  @override
  String get checkUpdates => 'Check for updates';

  @override
  String get aboutLicenses => 'About and licenses';

  @override
  String get androidClient => 'Android client';

  @override
  String get viewLogs => 'View logs';

  @override
  String get twoStepVerification => 'Two-step verification';

  @override
  String accountMenu(Object account) {
    return 'Account menu · $account';
  }

  @override
  String get idleAutoSignOut => 'Idle auto sign-out';

  @override
  String get serverSetupNeeded => 'Server setup needed';

  @override
  String get changeServer => 'Change server';

  @override
  String get id => 'ID';

  @override
  String get password => 'Password';

  @override
  String get rememberPassword => 'Remember password';

  @override
  String get passwordsRememberedOnlyHttps =>
      'Passwords are remembered only for HTTPS servers.';

  @override
  String get passwordEncryptedKeptDevice =>
      'Your password is encrypted and kept on this device.';

  @override
  String get signing => 'Signing in…';

  @override
  String get sign => 'Sign in';

  @override
  String get serverSettings => 'Server settings';

  @override
  String get askAdministratorAccount => 'Ask your administrator for an account';

  @override
  String get androidStreaming => 'Android · streaming';

  @override
  String newVersion2(Object updateVersion) {
    return 'New version $updateVersion';
  }

  @override
  String maintenanceNotice(Object noticeTitle) {
    return 'Maintenance notice · $noticeTitle';
  }

  @override
  String printerConnectionEnded(num lostDocuments) {
    String _temp0 = intl.Intl.pluralLogic(
      lostDocuments,
      locale: localeName,
      other:
          '$lostDocuments documents not yet processed can no longer be received.',
      one: '1 document not yet processed can no longer be received.',
    );
    return 'The printer connection ended. $_temp0';
  }

  @override
  String get printerConnectionEndedReconnectPrint =>
      'The printer connection ended. Reconnect to print.';

  @override
  String get couldntOpenReceivedPrintDocument =>
      'Couldn\'t open the received print document. Print again from the desktop.';

  @override
  String get androidPrintingIsntAvailableCheck =>
      'Android printing isn\'t available. Check the device\'s print service.';

  @override
  String get couldntPassPrintDocumentPrint =>
      'Couldn\'t pass on the print document. Print again from the desktop.';

  @override
  String get receivingPrintDocument => 'Receiving print document';

  @override
  String receivingPrintDocument2(Object label) {
    return 'Receiving print document · $label';
  }

  @override
  String get couldntKeepConnectionRunningBackground =>
      'Couldn\'t keep the connection running in the background. It may end when you leave the app.';

  @override
  String get stoppedPreparingConnectionWhileOutside =>
      'Stopped preparing the connection while outside the app. It reconnects when you return.';

  @override
  String get hostsVirtualDisplayIsntReady =>
      'The Host\'s virtual display isn\'t ready yet. Reconnect in a moment.';

  @override
  String get desktopWasntReadyWithin3 =>
      'The desktop wasn\'t ready within 3 minutes.\nTap ‘Reconnect’ in a moment.';

  @override
  String get connectionEndedCouldntReconnectCheck =>
      'The connection ended and couldn\'t reconnect. Check your network and reconnect.';

  @override
  String get couldntChangeWindowsScaleScreen =>
      'Couldn\'t change the Windows scale. The screen still looks sharp.';

  @override
  String setLargestScaleWindowsAllows(Object scale) {
    return 'Set to $scale%, the largest scale Windows allows at this screen size.';
  }

  @override
  String get couldntSendInputWhileScreen =>
      'Couldn\'t send input while the screen size is adjusting. Type again when it\'s done.';

  @override
  String get couldntConfirmInputWasDelivered =>
      'Couldn\'t confirm the input was delivered, so the keyboard closed. Open it again to type.';

  @override
  String get hostReceivesKoreanTextAs =>
      'This Host receives Korean text as key presses. It types as is when the desktop is in Korean input mode; if English letters appear, tap Han/Eng in the key row.';

  @override
  String get controlCharactersCantSentSo =>
      'Control characters can\'t be sent, so they were removed.';

  @override
  String get hostCantReceiveSomeCharacters =>
      'This Host can\'t receive some characters, such as emoji. Update the Host to send them.';

  @override
  String get mouseLock => 'Mouse lock';

  @override
  String get tapOkLockMouseInside =>
      'Tap OK to lock the mouse inside the desktop.\nPress Back to release it.';

  @override
  String get mouseLockedInsideDesktopPress =>
      'Mouse locked inside the desktop · press Back to release';

  @override
  String get someGamesSuchAsLeague =>
      'In some games, such as League of Legends, the mouse may not move';

  @override
  String get mouseLockOff => 'Mouse lock off';

  @override
  String get couldntFitScreenSizeTap =>
      'Couldn\'t fit the screen size. Tap ‘Reapply screen size’ under connection quality at the top of the menu.';

  @override
  String get deviceCantReceiveSharperPicture =>
      'This device can\'t receive a sharper picture, so it shows at auto size.';

  @override
  String get couldntApplySharpSoShows =>
      'Couldn\'t apply Sharp, so it shows at auto size.';

  @override
  String get stoppedWaitingHostUpdateTap =>
      'Stopped waiting for the Host update.\nTap ‘Reconnect’ when the update finishes.';

  @override
  String get stoppedWaitingTapReconnectWhen =>
      'Stopped waiting.\nTap ‘Reconnect’ when the desktop is ready.';

  @override
  String get screenSettings => 'Screen settings';

  @override
  String get applyReconnect => 'Apply and reconnect';

  @override
  String get hostCantChangeScreenSize =>
      'This Host can\'t change the screen size during a connection. It applies from the next connection.';

  @override
  String get resolutionWindowsScaleSeparate =>
      'Resolution and Windows scale apply separately.';

  @override
  String resolutionWindowsScaleApplySeparately(
    Object session,
    Object session2,
  ) {
    return '$session\n$session2\nResolution and Windows scale apply separately.';
  }

  @override
  String current(Object width, Object height) {
    return 'Current $width×$height';
  }

  @override
  String requested2(Object width, Object height) {
    return ' · requested $width×$height';
  }

  @override
  String current2(Object currentWidth, Object currentHeight) {
    return 'Current $currentWidth×$currentHeight';
  }

  @override
  String get checkingCurrent => 'Checking current';

  @override
  String recommended(
    Object desired,
    Object desired2,
    Object requested,
    Object current,
  ) {
    return 'Recommended $desired×$desired2$requested · $current';
  }

  @override
  String get supportedRange => 'Supported range';

  @override
  String get deviceSupport => 'Device support';

  @override
  String selected(
    Object desiredWidth,
    Object desiredHeight,
    Object requestNote,
    Object current,
  ) {
    return 'Selected $desiredWidth×$desiredHeight$requestNote · $current';
  }

  @override
  String sharp2(Object width, Object height, Object plan, Object current) {
    return 'Sharp $width×$height$plan · $current';
  }

  @override
  String couldntApplySharpSoShows2(Object sharpRefusedReason, Object current) {
    return 'Couldn\'t apply Sharp, so it shows at auto size ($sharpRefusedReason) · $current';
  }

  @override
  String checkingSharp(Object current) {
    return 'Checking Sharp · $current';
  }

  @override
  String cantReceiveSharperPictureSo(Object planned, Object current) {
    return 'Can\'t receive a sharper picture, so it shows at auto size$planned · $current';
  }

  @override
  String remoteResolution(Object width, Object height) {
    return 'Remote resolution $width × $height';
  }

  @override
  String get remoteResolutionUnknown => 'Remote resolution unknown';

  @override
  String get connected => 'Connected';

  @override
  String windowsScale(Object session, Object session2) {
    return '$session\nWindows scale · $session2';
  }

  @override
  String get remoteScreen => 'Remote screen';

  @override
  String get n1Screen => '1 screen';

  @override
  String get n2Screens => '2 screens';

  @override
  String get screenShow => 'Screen to show';

  @override
  String screen2(Object intValue, Object trackId) {
    return 'Screen $intValue · $trackId';
  }

  @override
  String get view => 'View';

  @override
  String get wholeScreen => 'Whole screen';

  @override
  String get fill => 'Fill';

  @override
  String get zoomOut => 'Zoom out';

  @override
  String zoom(Object scale) {
    return 'Zoom $scale%';
  }

  @override
  String get zoomsDeviceOnlyRemoteResolution =>
      'Zooms on this device only · remote resolution stays the same';

  @override
  String get zoom2 => 'Zoom in';

  @override
  String get pan => 'Pan';

  @override
  String get useWhenZoomedWhenKeyboard =>
      'Use when zoomed in or when the keyboard covers the screen';

  @override
  String get pointerSpeed => 'Pointer speed';

  @override
  String get inputMode => 'Input mode';

  @override
  String get streaming => 'Streaming';

  @override
  String get reapplyScreenSize => 'Reapply screen size';

  @override
  String get pressBackRelease => 'Press Back to release';

  @override
  String get locksMouseInsideDesktop => 'Locks the mouse inside the desktop';

  @override
  String get availableWhenMouseConnected =>
      'Available when a mouse is connected';

  @override
  String get touchControlsPrinterCopy => 'Touch controls · printer · copy';

  @override
  String speed(Object modeLabel, Object speedLabel) {
    return '$modeLabel · speed $speedLabel';
  }

  @override
  String get shortcutsAvailableOnceScreenSize =>
      'Shortcuts are available once the screen size is set';

  @override
  String get switchApps => 'Switch apps';

  @override
  String get taskManager => 'Task Manager';

  @override
  String get edit => 'Edit';

  @override
  String get copy => 'Copy';

  @override
  String get paste => 'Paste';

  @override
  String get selectAll => 'Select all';

  @override
  String get undo => 'Undo';

  @override
  String get cantSend => 'Can\'t send';

  @override
  String get ctrlAltDelCantSent =>
      'Ctrl+Alt+Del can\'t be sent while streaming. Open Task Manager with Ctrl+Shift+Esc.';

  @override
  String get youCanChangeOnceConnected => 'You can change this once connected';

  @override
  String get screenWholeScreenZoomPan => 'Screen · whole screen, zoom, pan';

  @override
  String get screenViewScaleResolution => 'Screen · view, scale, resolution';

  @override
  String text2(num mapped) {
    String _temp0 = intl.Intl.pluralLogic(
      mapped,
      locale: localeName,
      other: '$mapped shared folders connected',
      one: '1 shared folder connected',
    );
    return '$_temp0';
  }

  @override
  String get useDevicesFoldersDesktop =>
      'Use this device\'s folders on the desktop';

  @override
  String get availableOnceConnected => 'Available once connected';

  @override
  String get settingsPictureQualityMouse =>
      'Settings · picture quality, mouse lock, input mode, guide';

  @override
  String get screenSizeAdjustingChooseShortcut =>
      'The screen size is adjusting. Choose the shortcut again when it\'s done.';

  @override
  String get couldntConfirmShortcutWasSent =>
      'Couldn\'t confirm the shortcut was sent. Check the remote screen and choose it again if needed.';

  @override
  String get technology => 'Technology';

  @override
  String get decoder => 'Decoder';

  @override
  String get hardwareDecoding => 'Hardware decoding';

  @override
  String get onLabel2 => 'On';

  @override
  String get off2 => 'Off';

  @override
  String get appCpu => 'App CPU';

  @override
  String get receivedSize => 'Received size';

  @override
  String get remoteOriginal => 'Remote original';

  @override
  String get devicePixels => 'Device pixels';

  @override
  String get half => 'Half';

  @override
  String fixed(Object shortSide) {
    return 'Fixed $shortSide';
  }

  @override
  String selected2(Object width, Object height) {
    return 'Selected $width×$height';
  }

  @override
  String selected3(Object desktopLabel) {
    return 'Selected $desktopLabel';
  }

  @override
  String selected4(Object workspaceLabel) {
    return 'Selected $workspaceLabel';
  }

  @override
  String get windowsScale2 => 'Windows scale';

  @override
  String get pixelScale => 'Pixel scale';

  @override
  String get zoomFitInterpolated => 'Zoom/fit (interpolated)';

  @override
  String devicePixels1RemotePixel(Object viewPixelScale) {
    return '$viewPixelScale device pixels = 1 remote pixel';
  }

  @override
  String get mouseCursor => 'Mouse cursor';

  @override
  String get shownDevice => 'Shown on this device';

  @override
  String get video => 'In the video';

  @override
  String get audio => 'Audio';

  @override
  String get framesNetwork => 'Frames and network';

  @override
  String get sourceCapture => 'Source capture';

  @override
  String get decodingDevice => 'Decoding on this device';

  @override
  String get sentScreen => 'Sent to screen';

  @override
  String get receiveRate => 'Receive rate';

  @override
  String get inputAppliedHostEstP50 => 'Input→applied on Host (est. p50/p95)';

  @override
  String get inputNewFrameDecodedP50 => 'Input→new frame decoded (p50/p95)';

  @override
  String get inputLatency => 'Input latency';

  @override
  String get hostDoesntSupportMeasuring =>
      'This Host doesn\'t support measuring this';

  @override
  String get audioDrops => 'Audio drops';

  @override
  String text3(num frames) {
    String _temp0 = intl.Intl.pluralLogic(
      frames,
      locale: localeName,
      other: '$frames frames',
      one: '1 frame',
    );
    return '$_temp0';
  }

  @override
  String get inputNewFrameDecodedRuns =>
      'Input→new frame decoded runs from sending the input to the MediaCodec output callback of the measured frame. It doesn\'t include later texture handoff or discard, Flutter compositing, or actual display on screen. RTT is not this latency measurement.';

  @override
  String get processingDevice => 'Processing on this device';

  @override
  String get connectionStats => 'Connection stats';

  @override
  String streaming2(Object name) {
    return '$name · streaming';
  }

  @override
  String get detailedDiagnosticsOffByDefault =>
      'Detailed diagnostics are off by default.\nHost and receive-queue measurement settings apply from the next connection.';

  @override
  String get backList => 'Back to list';

  @override
  String get reconnectAutoResolution => 'Reconnect at auto resolution';

  @override
  String get pictureQualitySettings => 'Picture quality settings';

  @override
  String get disconnectConnectHere => 'Disconnect and connect here';

  @override
  String get hostPreparingVirtualDisplayPlease =>
      'The Host is preparing the virtual display. Please wait.';

  @override
  String get hostUpdatingReconnectsAutomaticallyWhen =>
      'The Host is updating.\nIt reconnects automatically when done.';

  @override
  String get waitingDesktopConnectsAutomaticallyWhen =>
      'Waiting for the desktop.\nIt connects automatically when ready.';

  @override
  String get connectionDroppedReconnecting =>
      'The connection dropped. Reconnecting.';

  @override
  String get requestingConnection => 'Requesting a connection.';

  @override
  String get stopWaiting => 'Stop waiting';

  @override
  String get hostUpdating => 'Host updating';

  @override
  String get waitingDesktop => 'Waiting for the desktop';

  @override
  String get desktopRestarting => 'Desktop restarting';

  @override
  String get reconnecting2 => 'Reconnecting';

  @override
  String get fittingScreenSize => 'Fitting screen size';

  @override
  String get nowPointerTrackpadTapTouch =>
      'Now pointer (trackpad) · tap for touch · long-press for pointer speed';

  @override
  String get nowTouchTapPointerTrackpad =>
      'Now touch · tap for pointer (trackpad) · long-press for input mode';

  @override
  String get closeKeyboard => 'Close keyboard';

  @override
  String get openKeyboard => 'Open keyboard';

  @override
  String get phone => 'Phone';

  @override
  String get foldableInnerScreen => 'Foldable inner screen';

  @override
  String get tablet => 'Tablet';

  @override
  String get fitDevice => 'Fit to device';

  @override
  String get n125 => '1.25×';

  @override
  String get n15 => '1.5×';

  @override
  String get n2 => '2×';

  @override
  String get autoRotate => 'Auto-rotate';

  @override
  String get landscape => 'Landscape';

  @override
  String get portrait => 'Portrait';

  @override
  String get autoRecommended => 'Auto (recommended)';

  @override
  String lockedTapRelease(Object mLabel) {
    return '$mLabel locked · tap to release';
  }

  @override
  String pressedCombinesNextKey(Object mLabel) {
    return '$mLabel pressed · combines with the next key';
  }

  @override
  String longPressLock(Object mLabel) {
    return '$mLabel · long-press to lock';
  }

  @override
  String get hanEng => 'Han/Eng';

  @override
  String get ctrlAltDelCantSent2 => 'Ctrl+Alt+Del can\'t be sent';

  @override
  String get basicKeys => 'Basic keys';

  @override
  String get f1F12NavigationKeys => 'F1–F12 · navigation keys';

  @override
  String get actualValueUnknownNotSupported =>
      'Actual value unknown · not supported by Host';

  @override
  String get manual => 'Manual';

  @override
  String actualValueUnknown(Object mode) {
    return '$mode · actual value unknown';
  }

  @override
  String requestedActualValueUnknown(Object mode, Object wanted) {
    return '$mode $wanted% requested · actual value unknown';
  }

  @override
  String requested3(Object wanted) {
    return ' $wanted% requested';
  }

  @override
  String cantChangeActualValueUnknown(Object mode, Object wanted) {
    return '$mode$wanted · can\'t change · actual value unknown';
  }

  @override
  String nowRequested(Object scale, Object mode, Object wanted) {
    return 'Now $scale% · $mode $wanted% requested';
  }

  @override
  String now(Object scale, Object mode) {
    return 'Now $scale% · $mode';
  }

  @override
  String get resolution => 'Resolution';

  @override
  String get recommended2 => 'Recommended';

  @override
  String get fitted => 'Fitted';

  @override
  String get screensRatio => 'This screen\'s ratio';

  @override
  String get usesDevicesOwnPixelsHost =>
      'Uses the device\'s own pixels on a Host that can change scale, otherwise a readable whole-number multiple';

  @override
  String get recommendedSizeKeepsDeviceRatio =>
      'Recommended size that keeps the device ratio · Check the applied size under current resolution';

  @override
  String get sharpest => 'Sharpest';

  @override
  String get quarterPixelsDrawnCrisply2 =>
      'A quarter of the pixels · Drawn crisply at 2:1, easier on the Host and TV';

  @override
  String get fixedSizeScreensRatio => 'Fixed size in this screen\'s ratio';

  @override
  String get sizeKeepsDeviceRatioSmall =>
      'Size that keeps the device ratio · Small screens aren\'t enlarged';

  @override
  String get remoteWorkspaceSizeCheckApplied =>
      'Remote workspace size · Check the applied size under current resolution';

  @override
  String get remoteWorkspaceFittedDevicesScreen =>
      'Remote workspace fitted to the device\'s screen ratio · Check the applied size under current resolution';

  @override
  String windowsScale3(Object deviceClassLabel) {
    return 'Windows scale · $deviceClassLabel';
  }

  @override
  String get calculating => 'Calculating';

  @override
  String get choosesSuitTvsResolution => 'Chooses to suit the TV\'s resolution';

  @override
  String get orientation => 'Orientation';

  @override
  String get reconnectChangedSettings => 'Reconnect with the changed settings';

  @override
  String get noSettingsChanged => 'No settings changed';

  @override
  String get qualityMode2 => 'Quality mode';

  @override
  String get tapSwipe => 'Tap · swipe';

  @override
  String get sentAsWindowsTouchWorks =>
      'Sent as Windows touch · works like a touch screen';

  @override
  String get longPress => 'Long press';

  @override
  String get rightClick => 'Right-click';

  @override
  String get twoFingers => 'Two fingers';

  @override
  String get zoomScrollInsideAppsWhen =>
      'Zoom and scroll inside apps (when the app supports it)';

  @override
  String get slideOneFinger => 'Slide one finger';

  @override
  String get moveCursor => 'Move cursor';

  @override
  String get tap => 'Tap';

  @override
  String get clickCursorDoubleTapDouble =>
      'Click at the cursor · double-tap to double-click';

  @override
  String get doubleTapMoveWhileHolding =>
      'Double-tap and move while holding the second tap';

  @override
  String get dragSelectMoveWindows => 'Drag · select · move windows';

  @override
  String get twoFingersUpDown => 'Two fingers up or down';

  @override
  String get scroll => 'Scroll';

  @override
  String get clickWhereYouTapDouble =>
      'Click where you tap · double-tap to double-click';

  @override
  String get oneFingerUpDown => 'One finger up or down';

  @override
  String get doubleTapMoveWhileHolding2 => 'Double-tap and move while holding';

  @override
  String get longPressTwoFingerTap => 'Long press · two-finger tap';

  @override
  String get spreadTwoFingers => 'Spread two fingers';

  @override
  String get zoomPinchBackSeeWhole =>
      'Zoom in · pinch back to see the whole screen';

  @override
  String get threeFingerTap => 'Three-finger tap';

  @override
  String get openCloseKeyboard => 'Open or close keyboard';

  @override
  String get fourFingerTap => 'Four-finger tap';

  @override
  String get switchInputModePointerTouch =>
      'Switch input mode · pointer ↔ touch';

  @override
  String get ok => 'OK';

  @override
  String inputMode2(Object modeLabel) {
    return 'Input mode · $modeLabel';
  }

  @override
  String get zoomScreenItselfSwitchInput =>
      'To zoom the screen itself, switch the input mode to pointer.';

  @override
  String get tapClicksLittleSlowWhile =>
      'Tap clicks are a little slow while checking for a double-tap.';

  @override
  String longPressInputModeButton(Object windowsTouch) {
    return 'Long-press the input mode button to change input mode and pointer speed.\n$windowsTouch\nConnect a mouse to use it right away.';
  }

  @override
  String get featuresConnection => 'Features in this connection';

  @override
  String get available => 'Available';

  @override
  String get checkingConnection => 'Checking connection';

  @override
  String get notAvailableConnection => 'Not available in this connection';

  @override
  String get usesMoreDataBattery => 'Uses more data and battery';

  @override
  String get getsScreensExactPixelsSo =>
      'Gets this screen\'s exact pixels so text is crisp. Uses more data and battery.';

  @override
  String get windowsScale200Limit => 'Windows scale 200% limit';

  @override
  String get desktopLimit => 'Desktop limit';

  @override
  String get decoderSizeLimitDevice => 'Decoder size limit on this device';

  @override
  String get decoderSpeedLimitDevice => 'Decoder speed limit on this device';

  @override
  String get memoryDevice => 'Memory on this device';

  @override
  String get autoSizeAlreadyNativePixels =>
      'Auto size is already native pixels';

  @override
  String get couldntCheck => 'Couldn\'t check';

  @override
  String backAutoSize(num count, Object stats) {
    String _temp0 = intl.Intl.pluralLogic(
      count,
      locale: localeName,
      other: '$count times',
      one: 'once',
    );
    return ' · back to auto size $_temp0 ($stats)';
  }

  @override
  String off3(Object fell) {
    return 'Off$fell';
  }

  @override
  String nativeDevicePixels(Object size) {
    return 'Native device pixels$size';
  }

  @override
  String get lowered => 'Lowered';

  @override
  String onLabel3(Object state, Object fell) {
    return 'On · $state$fell';
  }

  @override
  String get checkHttpConsentAgain => 'Check the HTTP consent again.';

  @override
  String get viewScaleResolution => 'View · scale · resolution';

  @override
  String get qualityInputHelp => 'Quality · input · help';

  @override
  String get textScreenSize => 'Text and screen size';

  @override
  String get frameRateBitrate => 'Frame rate · bitrate';

  @override
  String get useExternalMouse => 'Use an external mouse';

  @override
  String get availableFeatures => 'Available features';

  @override
  String get roundTripLatencyHostResponse =>
      'Round-trip latency · Host response';

  @override
  String get notMeasuredYet => 'Not measured yet';

  @override
  String get menu => 'Menu';

  @override
  String get closeMenu => 'Close menu';

  @override
  String get openMenu => 'Open menu';

  @override
  String openMenu2(Object latencyText) {
    return 'Open menu · $latencyText';
  }

  @override
  String get screenPanHandleDragMove =>
      'Screen pan handle · drag to move the visible area';

  @override
  String get lookLeft => 'Look left';

  @override
  String get lookUp => 'Look up';

  @override
  String get lookDown => 'Look down';

  @override
  String get lookRight => 'Look right';

  @override
  String get back => 'Back';

  @override
  String get touch => 'Touch';

  @override
  String get pointer => 'Pointer';

  @override
  String get precise => 'Precise';

  @override
  String get standard => 'Normal';

  @override
  String get fast => 'Fast';

  @override
  String get couldntOpenTvInputField =>
      'Couldn\'t open the TV input field. Select it again.';

  @override
  String get updateAppUseTvInput => 'Update the app to use TV input.';

  @override
  String get drag => 'Drag';

  @override
  String get scrollUpDown => 'Scroll · up/down';

  @override
  String get draggingOkRelease => 'Dragging · OK to release';

  @override
  String get dragOkGrab => 'Drag · OK to grab';

  @override
  String get remoteScreenMenu => 'Remote screen menu';

  @override
  String get shortcuts => 'Shortcuts';

  @override
  String get backRemoteScreen => 'Back to remote screen';

  @override
  String get installPermissionConfirmedTapDownload =>
      'Install permission confirmed. Tap ‘Download and install’.';

  @override
  String get couldntCheckInstallPermissionDownload =>
      'Couldn\'t check the install permission or download location. Try again.';

  @override
  String get couldntOpenAndroidSettingsAllow =>
      'Couldn\'t open Android settings. Allow it yourself in Settings → Apps → Multitap.';

  @override
  String get appDataSignSettingsKept =>
      'Your app data and sign-in settings are kept.\nThe downloaded file is checked, then the Android installer opens.\nAndroid asks again whether to install.';

  @override
  String get androidSettingsAllowInstallUnknown =>
      'In Android settings, allow\n‘Install unknown apps’ for Multitap.\nThen come back to this screen.\nYou can turn it off again in settings anytime.';

  @override
  String get checkingInstallPermissionDownloadLocation =>
      'Checking the install permission and download location.';

  @override
  String downloadingMb(Object received, Object total, Object total2) {
    return 'Downloading · $received / $total MB ($total2%)';
  }

  @override
  String get checkingDownloadedFilesSizeSha =>
      'Checking the downloaded file\'s size and SHA-256.';

  @override
  String get openingAndroidInstaller => 'Opening the Android installer.';

  @override
  String get tapUpdateAndroidInstallerOpen =>
      'Tap ‘Update’ in the Android installer.\nOpen the app again after installing to start the new version.';

  @override
  String get later => 'Later';

  @override
  String get downloadInstall => 'Download and install';

  @override
  String newVersion3(Object version) {
    return 'New version $version';
  }

  @override
  String currentMb(Object current, Object total) {
    return 'Current $current · $total MB';
  }

  @override
  String released(Object releasedAt) {
    return 'Released $releasedAt';
  }

  @override
  String get current3 => 'Current';

  @override
  String get newVersion4 => 'New version';

  @override
  String get installedVersion => 'Installed version';

  @override
  String get newerThanServer => 'Newer than server';

  @override
  String get upDate => 'Up to date';

  @override
  String text4(num hostsLength) {
    String _temp0 = intl.Intl.pluralLogic(
      hostsLength,
      locale: localeName,
      other: '$hostsLength Host updates available',
      one: '1 Host update available',
    );
    return '$_temp0';
  }

  @override
  String get checkingServersUpdateList => 'Checking the server\'s update list.';

  @override
  String get serverHasNoAndroidInstaller =>
      'This server has no Android installer for this device yet.';

  @override
  String get whatsNewVersion => 'What\'s new in this version';

  @override
  String whatsNewServerVersion(Object version) {
    return 'What\'s new in server version $version';
  }

  @override
  String get whatsNewCurrentVersion => 'What\'s new in the current version';

  @override
  String get appUpdate => 'App update';

  @override
  String get couldntGetUpdateInformationCheck =>
      'Couldn\'t get update information. Check the network and try again.';

  @override
  String get serversUpdateInformationIsntValid =>
      'The server\'s update information isn\'t valid. Tell your administrator.';

  @override
  String get couldntDownloadUpdateFileTry =>
      'Couldn\'t download the update file. Try again.';

  @override
  String get downloadedFilesSizeSha256 =>
      'The downloaded file\'s size or SHA-256 doesn\'t match the server, so it wasn\'t installed.';

  @override
  String get couldntSaveUpdateFileCheck =>
      'Couldn\'t save the update file. Check the device\'s storage.';

  @override
  String get cantCheckUpdatesBecauseApps =>
      'Can\'t check for updates because this app\'s current version is unknown.';

  @override
  String get cantCheckUpdatesFitDevice =>
      'Can\'t check for updates that fit this device.';

  @override
  String get allowInstallUnknownAppsMultitap =>
      'Allow ‘Install unknown apps’ for Multitap in Android settings to install updates.';

  @override
  String get downloadedFileIsntMultitapUpdate =>
      'The downloaded file isn\'t a Multitap update with the same signature, so it wasn\'t installed.';

  @override
  String get downloadedFileIsntNewerThan =>
      'The downloaded file isn\'t newer than the current version, so it wasn\'t installed.';

  @override
  String get anotherUpdateBeingInstalledTry =>
      'Another update is being installed. Try again when it\'s done.';

  @override
  String get couldntCheckInstallStatusRestart =>
      'Couldn\'t check the install status. Restart Multitap, then check for updates.';

  @override
  String get updateCanceled => 'Update canceled.';

  @override
  String get updateInstallCanceledYouKeep =>
      'Update install canceled. You keep using the current version.';

  @override
  String get androidBlockedUpdateInstallCheck =>
      'Android blocked the update install. Check the device management policy.';

  @override
  String get couldntUpdateBecauseDoesntMatch =>
      'Couldn\'t update because it doesn\'t match the installed app.';

  @override
  String get updateIsntCompatibleDevice =>
      'This update isn\'t compatible with this device.';

  @override
  String get notEnoughStorageInstallUpdate =>
      'Not enough storage to install the update.';

  @override
  String get androidFoundUpdateFileInvalid =>
      'Android found the update file invalid.';

  @override
  String get updateInstallTimedOutTry =>
      'The update install timed out. Try again.';

  @override
  String get couldntOpenAndroidInstallerTry =>
      'Couldn\'t open the Android installer. Try again.';

  @override
  String notice(Object bannerTitle) {
    return 'Notice: $bannerTitle';
  }

  @override
  String get couldntOpenConsoleWindow => 'Couldn\'t open the Console window.';

  @override
  String get couldntGetResponseFromServer =>
      'Couldn\'t get a response from the server. Try again in a moment.';

  @override
  String get youWereSignedOutMultitap =>
      'You were signed out of Multitap. Click to see why.';

  @override
  String get couldntSignOutCheckNetwork =>
      'Couldn\'t sign out. Check the network and try again.';

  @override
  String get couldntCloseRemoteWindowClose =>
      'Couldn\'t close the remote window. Close it, then sign out again.';

  @override
  String get passwordWasChangedSignAgain =>
      'Your password was changed. Sign in again with the new password.';

  @override
  String get demo => 'Demo';

  @override
  String get notSet => 'Not set';

  @override
  String get unnamedComponent => 'Unnamed component';

  @override
  String get packageLicense => 'Package license';

  @override
  String get peaks => 'Peaks';

  @override
  String get meadow => 'Meadow';

  @override
  String get desktopConnectedFromAnotherWindow =>
      'This desktop is connected from another window or device. The button below disconnects it there and continues here. The desktop and running work stay as they are.';

  @override
  String get connectionSetupCanceledYouCan =>
      'Connection setup canceled. You can connect again.';

  @override
  String get youNeedAgreeUnencryptedHttp =>
      'You need to agree to an unencrypted HTTP connection first. Press ‘Test connection’ again in server settings.';

  @override
  String get serverDoesntAllowHttpConnections =>
      'This server doesn\'t allow HTTP connections. Use an HTTPS address or tell your administrator.';

  @override
  String get httpConnectionsTurnedOffServer =>
      'HTTP connections are turned off on the server. Use an HTTPS address or ask your administrator.';

  @override
  String get addressIsntRegisteredServerCheck =>
      'This address isn\'t registered on the server. Check the address and port you were given, and if they\'re right, tell your administrator.';

  @override
  String get addressOnlyServersFirstSetup =>
      'This address is only for the server\'s first setup. You can connect after your administrator finishes server access.';

  @override
  String get applyingServerAccessSettingsConnect =>
      'Applying the server access settings.\nConnect again in a moment.';

  @override
  String get addressDoesntMatchServersSettings =>
      'This address doesn\'t match the server\'s settings. Check the address and port you were given, and if they\'re right, tell your administrator.';

  @override
  String get changeInitialUsernamePasswordAdmin =>
      'Change the initial ID and password in the admin console first.';

  @override
  String get checkUsernamePassword => 'Check your ID or password.';

  @override
  String get tooManyAttemptsTryAgain =>
      'Too many attempts. Try again in 1 minute.';

  @override
  String get serverBusyTryAgainMoment =>
      'The server is busy. Try again in a moment.';

  @override
  String get checkVerificationCodeEnterAgain =>
      'Check the verification code and enter it again.';

  @override
  String get verificationTimeUpVerifyAgain =>
      'Verification time is up. Verify again with the next code from your authenticator app.';

  @override
  String get verificationTimeUpStartAgain =>
      'Verification time is up. Start again from sign-in.';

  @override
  String get updateMultitapLatestVersionUse =>
      'Update Multitap to the latest version to use two-step verification.';

  @override
  String get twoStepVerificationWorksHttps =>
      'Two-step verification works with HTTPS server addresses.';

  @override
  String get serverCouldntCheckTwoStep =>
      'The server couldn\'t check two-step verification. Tell your administrator.';

  @override
  String get enterEitherVerificationCodeRecovery =>
      'Enter either a verification code or a recovery code, not both.';

  @override
  String get couldntOpenBrowserCheckDefault =>
      'Couldn\'t open the browser.\nCheck your default browser setting and try again.';

  @override
  String get couldntOpenConsoleCheckNetwork2 =>
      'Couldn\'t open Console.\nCheck the network and try again.';

  @override
  String get consoleNeedsMicrosoftEdgeWebview2 =>
      'Console needs Microsoft Edge WebView2.\nInstall the WebView2 Runtime and try again.';

  @override
  String get consoleCantOpenedServerTell =>
      'Console can\'t be opened on this server.\nTell your administrator.';

  @override
  String get anotherDesktopsConsoleOpenClose =>
      'Another desktop\'s Console is open.\nClose that Console and try again.';

  @override
  String get consoleOpensOnlyHttpsServer =>
      'Console opens only with an HTTPS server address.\nSign in again with an HTTPS address.';

  @override
  String get tooManySignInsUse =>
      'Too many sign-ins in use at once. Sign out on a device you don\'t use, then sign in again.';

  @override
  String get serverNeedsUpdateBeforeYou =>
      'The server needs an update before you can sign in with this app. Tell your administrator.';

  @override
  String get timeRanOutWhileChecking =>
      'Time ran out while checking, or your existing sign-in changed. Sign in again.';

  @override
  String get connectionUseHasChangedConnect =>
      'The connection in use has changed. Connect again to check the current connection.';

  @override
  String get timeConfirmConnectionSwitchUp =>
      'Time to confirm the connection switch is up. Connect again to check the current connection.';

  @override
  String get couldntCheckConnectionSwitchDetails =>
      'Couldn\'t check the connection switch details. Connect again.';

  @override
  String get couldntDisconnectOtherConnectionConnect =>
      'Couldn\'t disconnect the other connection and connect. Try connecting again, and if it keeps happening, tell your administrator.';

  @override
  String get couldntCheckConnectionUseConnect =>
      'Couldn\'t check the connection in use. Connect again in a moment.';

  @override
  String get couldntConnectServerCheckNetwork =>
      'Couldn\'t connect to the server. Check the network.';

  @override
  String get serverIsntRespondingTryAgain =>
      'The server isn\'t responding. Try again in a moment.';

  @override
  String get checkCurrentPassword => 'Check your current password.';

  @override
  String get newPasswordConfirmationDontMatch =>
      'The new password and confirmation don\'t match.';

  @override
  String get passwordsMust10256Characters =>
      'Passwords must be 10 to 256 characters.';

  @override
  String get lostConnectionHostTryAgain =>
      'Lost the connection to the Host. Try again in a moment.';

  @override
  String get desktopHasAnotherConnection =>
      'This desktop has another connection.';

  @override
  String get couldntProcessRequestTryAgain2 =>
      'Couldn\'t process the request. Try again in a moment.';

  @override
  String get couldntConnectSecurelyOverHttps =>
      'Couldn\'t connect securely over HTTPS. Check the server address, and if it\'s right, tell your administrator.';

  @override
  String get couldntFindServerAddressCheck =>
      'Couldn\'t find the server address. Check the address and network.';

  @override
  String get serverDidntRespondTimeCheck =>
      'The server didn\'t respond in time. Check the address and port and try again.';

  @override
  String get cantReachServerAddressPort =>
      'Can\'t reach the server at this address and port. Check the address and port you were given, and if they\'re right, tell your administrator.';

  @override
  String get desktopIsntRespondingConnectAgain =>
      'The desktop isn\'t responding. Connect again in a moment, and if it keeps happening, tell your administrator.';

  @override
  String get hostStillGettingReadyConnect =>
      'The Host is still getting ready. Connect again in a moment.';

  @override
  String get cantPrepareScreenFitsWindow =>
      'Can\'t prepare a screen of this size right now. Connect again, and if it keeps happening, tell your administrator.';

  @override
  String get hostsStateChangedConnectAgain =>
      'The Host\'s state changed. Connect again.';

  @override
  String get pictureQualitySettingsDontMatch =>
      'The picture quality settings don\'t match the server. If it continues after updating Multitap, tell your administrator.';

  @override
  String get cantOpenVirtualDisplayScreen =>
      'Can\'t open a virtual display at this screen size. Change the screen mode or size, then connect again.';

  @override
  String get hostPreparingVirtualDisplayConnect =>
      'The Host is preparing the virtual display. Connect again in a moment.';

  @override
  String get onlyAdministratorAccountsCanUpdate =>
      'Only administrator accounts can update the Host.';

  @override
  String get hostDoesntSupportRemoteUpdates =>
      'This Host doesn\'t support remote updates. Run the Host installer on the desktop yourself.';

  @override
  String get hostUpdateProgressDesktopTry =>
      'A Host update is in progress on this desktop. Try again when it\'s done.';

  @override
  String get hostAlreadyUpDate => 'The Host is already up to date.';

  @override
  String get isntNewerThanCurrentHost =>
      'This isn\'t newer than the current Host.';

  @override
  String get serversLatestHostInstallerHas =>
      'The server\'s latest Host installer has changed. Refresh the list.';

  @override
  String get couldntCheckHostsCurrentVersion =>
      'Couldn\'t check the Host\'s current version. Try again in a moment.';

  @override
  String get tooManyHostUpdateRequests =>
      'Too many Host update requests. Try again in a moment.';

  @override
  String get cleaningUpPreviousConnectionConnect =>
      'Cleaning up the previous connection. Connect again in a moment.';

  @override
  String get virtualDisplayIsntReadyIf =>
      'The virtual display isn\'t ready. If it\'s the same after reconnecting, tell your administrator.';

  @override
  String get windowsSignedOutLockedOpen =>
      'Windows is signed out or locked.\nOpen it with Console and sign in.';

  @override
  String get hostIsntReadyStreamYet =>
      'The Host isn\'t ready to stream yet. Connect again in a moment, and if it keeps happening, tell your administrator.';

  @override
  String get desktopHasAnotherConnectionDisconnect =>
      'This desktop has another connection. Disconnect it, then connect again.';

  @override
  String get anotherUserUsingDesktopConnect =>
      'Another user is using this desktop. Connect again after that connection ends.';

  @override
  String get tooManyConnectionSwitchRequests =>
      'Too many connection switch requests. Try again in a moment.';

  @override
  String get powerActionProgressTryAgain =>
      'A power action is in progress. Try again when it\'s done.';

  @override
  String get cantDoneCurrentPowerState =>
      'This can\'t be done in the current power state. Refresh the list to check the state.';

  @override
  String get couldntCheckVmsPowerState =>
      'Couldn\'t check the VM\'s power state in Proxmox. Refresh in a moment.';

  @override
  String get couldntFinishPowerActionRefresh =>
      'Couldn\'t finish the power action. Refresh the list to check the state, and if it keeps happening, tell your administrator.';

  @override
  String get powerActionsResultIsntConfirmed =>
      'The power action\'s result isn\'t confirmed yet. Don\'t request it again; refresh the list.';

  @override
  String get proxmoxConnectionSettingsChangedRefresh =>
      'The Proxmox connection settings changed. Refresh the state, then try again.';

  @override
  String get youDontHavePermissionDesktop =>
      'You don\'t have permission for this desktop, or its assignment changed. Refresh the list, and ask your administrator if needed.';

  @override
  String get cantCheckPowerActionsResult =>
      'Can\'t check the power action\'s result any further. Refresh the list to check the state.';

  @override
  String get powerActionIsntSupported => 'This power action isn\'t supported.';

  @override
  String get anotherPowerActionEndedStatus =>
      'Another power action ended the status check of the earlier request.';

  @override
  String get hostDoesntSupportChangingNumber =>
      'This Host doesn\'t support changing the number of monitors, or it isn\'t ready yet.';

  @override
  String get changingNumberMonitorsTryAgain =>
      'Changing the number of monitors. Try again when it\'s done.';

  @override
  String get couldntChangeNumberMonitorsCheck =>
      'Couldn\'t change the number of monitors. Check the current state in the desktop list.';

  @override
  String get itsNotConfirmedYetWhether =>
      'It\'s not confirmed yet whether the number of monitors changed. Don\'t request it again; check the desktop list.';

  @override
  String get cantCheckMonitorChangeResult =>
      'Can\'t check the monitor change result any further. Check the current state in the desktop list.';

  @override
  String get youCanSet12 => 'You can set 1 or 2 monitors.';

  @override
  String get allMonitorsCantUsedDesktop =>
      'All monitors can\'t be used on this desktop right now. Use window mode or full screen.';

  @override
  String get couldntCheckLayoutPcs2 =>
      'Couldn\'t check the layout of your PC\'s 2 monitors. Check the monitor connections, then select again.';

  @override
  String get pcsCurrentMonitorSetupCant =>
      'Your PC\'s current monitor setup can\'t use all monitors. It works only with 2 monitors totaling 8.3 million pixels or less.';

  @override
  String get tooManyMonitorChangeRequests =>
      'Too many monitor change requests. Try again in a moment.';

  @override
  String get localPcsHostDoesntSupport =>
      'The local PC\'s Host doesn\'t support remote power. Ask your administrator to update the Host.';

  @override
  String get couldntStartLocalPc => 'Couldn\'t start the local PC.';

  @override
  String get localPcLockedSoWindows =>
      'The local PC is locked, so Windows didn\'t accept the request.';

  @override
  String get anotherUserSignedLocalPc =>
      'Another user is signed in to the local PC, so Windows didn\'t accept the request.';

  @override
  String get localPcAlreadyShuttingDown =>
      'The local PC is already shutting down or restarting.';

  @override
  String get windowsDidntGivePermissionShut =>
      'Windows didn\'t give permission to shut down or restart the local PC. Tell your administrator.';

  @override
  String get windowsCouldntProcessRequestCheck =>
      'Windows couldn\'t process the request. Check on the local PC itself.';

  @override
  String get localPcDidntRespondCheck =>
      'The local PC didn\'t respond. Check its state in the list in a moment.';

  @override
  String get windowsSignedOutLockedSign =>
      'Windows is signed out or locked.\nSign in on the local PC itself.';

  @override
  String get serverIsntRespondingMomentRetrying =>
      'The server isn\'t responding for a moment. Retrying automatically.';

  @override
  String get couldntFindServerAddressCheck2 =>
      'Couldn\'t find the server address. Check the spelling and your internet connection.';

  @override
  String serverCouldntProcessRequestTry(Object status) {
    return 'The server couldn\'t process the request. Try again in a moment, and if it keeps happening, tell your administrator. (HTTP $status)';
  }

  @override
  String get addressIsntMultitapServerCheck =>
      'This address isn\'t a Multitap server. Check the server address.';

  @override
  String get youNeedAgreeUnencryptedHttp2 =>
      'You need to agree to an unencrypted HTTP connection first. Press ‘Test connection’ again.';

  @override
  String get serverDoesntAllowHttpConnections2 =>
      'This server doesn\'t allow HTTP connections.\nUse an HTTPS address or tell your administrator.';

  @override
  String get httpConnectionsTurnedOffServer2 =>
      'HTTP connections are turned off on the server.\nUse an HTTPS address or ask your administrator.';

  @override
  String get addressIsntRegisteredServerCheck2 =>
      'This address isn\'t registered on the server.\nCheck the address and port you were given, and if they\'re right, tell your administrator.';

  @override
  String get httpAddressOnlyServersFirst =>
      'This HTTP address is only for the server\'s first setup.\nYou can connect after your administrator finishes server access.';

  @override
  String get addressDoesntMatchServersSettings2 =>
      'This address doesn\'t match the server\'s settings.\nCheck the address and port you were given, and if they\'re right, tell your administrator.';

  @override
  String get cantReachServerAddressPort2 =>
      'Can\'t reach the server at this address and port.\nCheck the address and port you were given, and if they\'re right, tell your administrator.';

  @override
  String get couldntConnectServerCheckAddress =>
      'Couldn\'t connect to the server. Check the address, port and network.';

  @override
  String get checkYouEnteredServerAddress =>
      'Check that you entered a server address with no spaces or special characters.';

  @override
  String get enterHttpHttpsServerAddress =>
      'Enter an HTTP or HTTPS server address.';

  @override
  String get dontIncludePathServerAddress =>
      'Don\'t include a path in the server address.';

  @override
  String get checkIpv6Address => 'Check the IPv6 address.';

  @override
  String get checkServerPort => 'Check the server port.';

  @override
  String get enterServerPort => 'Enter the server port.';

  @override
  String get ipv6AddressesNeedSquareBrackets =>
      'IPv6 addresses need square brackets.';

  @override
  String get enterIpAddressAsFour =>
      'Enter an IP address as four numbers from 0 to 255 joined by dots.';

  @override
  String get checkServerAddress => 'Check the server address.';

  @override
  String get portNumberFrom165535 => 'The port is a number from 1 to 65535.';

  @override
  String get agreeHttpServerAddressFirst =>
      'Agree to this HTTP server address first, then test the connection.';

  @override
  String get serverSettingsChangedConnectAgain =>
      'The server settings changed. Connect again.';

  @override
  String get httpConsentAddressDifferent =>
      'The HTTP consent address is different.';

  @override
  String get checkServerSettings => 'Check the server settings.';

  @override
  String get testConnectionCurrentHttpAddress =>
      'Test the connection to the current HTTP address first.';

  @override
  String get invalidRelayAddress => 'Invalid relay address.';

  @override
  String get checkServersAccessSettings =>
      'Check the server\'s access settings.';

  @override
  String get waiting => 'Waiting';

  @override
  String get download => 'Download';

  @override
  String get install => 'Install';

  @override
  String get done => 'Done';

  @override
  String get needsCheck => 'Needs check';

  @override
  String get failed => 'Failed';

  @override
  String get stopped => 'Stopped';

  @override
  String get updateAvailable => 'Update available';

  @override
  String get notEligible => 'Not eligible';

  @override
  String get assignmentWasRemovedSoResult =>
      'The assignment was removed, so the result of the accepted request can\'t be checked.';

  @override
  String get desktopIsntAssignedYouNow =>
      'This desktop isn\'t assigned to you now.';

  @override
  String get useRequestingDisconnects => 'In use · requesting disconnects it';

  @override
  String get notUseUpdatesRightAway =>
      'Not in use · updates right away when requested';

  @override
  String get notRequestedYet => 'Not requested yet';

  @override
  String get requestingFromServer => 'Requesting from server…';

  @override
  String get notAcceptedYetRequestingAgain =>
      'Not accepted yet. Requesting again automatically in a moment.';

  @override
  String get acceptedByServerWaitingInstall =>
      'Accepted by server · waiting for the install result';

  @override
  String get updatesWhenOtherUsersConnection =>
      'Updates when the other user\'s connection ends';

  @override
  String get useUpdatesWhenConnectionEnds =>
      'In use · updates when the connection ends';

  @override
  String downloadingInstaller(Object op) {
    return 'Downloading installer$op';
  }

  @override
  String get checkingInstaller => 'Checking installer';

  @override
  String get installingHost => 'Installing Host';

  @override
  String get hostServiceRestartedWaitingConnection =>
      'Host service restarted · waiting for connection';

  @override
  String get serverConfirmedInstallFinished =>
      'The server confirmed the install finished';

  @override
  String get installedWindowsRestartNeeded =>
      'Installed · Windows restart needed';

  @override
  String get installedOpenHostSettingsDesktop =>
      'Installed · open Host settings on the desktop to check';

  @override
  String get runningNewVersionInstallResult =>
      'Running the new version · install result wasn\'t reported';

  @override
  String get checkRequestInstallResultWont =>
      'Check the request and install result · won\'t request again automatically';

  @override
  String get stoppedRemainingRequestsWerentAccepted =>
      'Stopped the remaining requests that weren\'t accepted yet';

  @override
  String get serverCantCheckUpdatePermission =>
      'The server can\'t check update permission or state';

  @override
  String get hostOffline => 'Host offline';

  @override
  String get remoteUpdateNotSupportedInstall =>
      'Remote update not supported · install it on the desktop yourself';

  @override
  String get latestReleaseChangedCloseWindow =>
      'The latest release changed. Close the window and check again.';

  @override
  String get checkCurrentHostVersion => 'Check the current Host version';

  @override
  String get noNewVersionInstall => 'No new version to install';

  @override
  String get anotherHostUpdateProgress => 'Another Host update is in progress';

  @override
  String get serverDoesntAllowUpdatesRight =>
      'The server doesn\'t allow updates right now';

  @override
  String get hostOfflineSoRequestWasnt =>
      'The Host is offline, so the request wasn\'t accepted';

  @override
  String get anotherUpdateAlreadyProgress =>
      'Another update is already in progress';

  @override
  String get serverConfirmedItsAlreadyUp =>
      'The server confirmed it\'s already up to date';

  @override
  String get hostDoesntSupportRemoteUpdates2 =>
      'This Host doesn\'t support remote updates';

  @override
  String get noUpdatePermissionSignExpired =>
      'No update permission, or your sign-in expired';

  @override
  String get tooManyRequestsSoWasnt =>
      'Too many requests, so it wasn\'t accepted. Try again in a moment.';

  @override
  String get serverCouldntAcceptRequestClose =>
      'The server couldn\'t accept the request. Close the window and try again.';

  @override
  String text5(Object stage, Object stage2) {
    return '$stage $stage2';
  }

  @override
  String progress(Object running, Object value, Object value2) {
    return 'In progress $running · $value · $value2';
  }

  @override
  String get allFinished => 'All finished';

  @override
  String get versionUnknown => 'Version unknown';

  @override
  String get youSignedSomewhereElseSo =>
      'You signed in somewhere else, so you were signed out here.';

  @override
  String get youWereSignedOutAfter =>
      'You were signed out after a long time with no input.';

  @override
  String get useAnotherDeviceSignAgain =>
      'In use on another device. Sign in again.';

  @override
  String get youWereSignedOutEnter =>
      'You were signed out. Enter your password to sign in again.';

  @override
  String get multitapKeepsRunningTrayClose =>
      'Multitap keeps running in the tray. To close it completely, choose \'Quit\' from the tray icon or the account menu.';

  @override
  String get signExpiredClickSignAgain =>
      'Your sign-in expired. Click to sign in again.';

  @override
  String text6(num bytes) {
    String _temp0 = intl.Intl.pluralLogic(
      bytes,
      locale: localeName,
      other: '$bytes bytes',
      one: '1 byte',
    );
    return '$_temp0';
  }

  @override
  String get zipThemMoveThemShared =>
      'zip them or move them with a ‘shared folder’';

  @override
  String get zipThemFirst => 'zip them first';

  @override
  String get moveLargeFilesSharedFolder =>
      'Move large files with a ‘shared folder’';

  @override
  String get splitThemIntoSmallerFiles => 'Split them into smaller files';

  @override
  String upPerFileOnce(Object maxFileBytes, Object maxTotalBytes) {
    return 'up to $maxFileBytes per file and $maxTotalBytes at once';
  }

  @override
  String textTooLongYouCan(Object maxTextBytes) {
    return 'The text is too long. You can move up to $maxTextBytes at once.';
  }

  @override
  String tooManyFilesYouCan(Object maxFiles, Object pack) {
    return 'Too many files. You can move up to $maxFiles at once. For more, $pack.';
  }

  @override
  String fileTooLargeClipboardCan(Object files, Object big) {
    return 'The file is too large. The clipboard can move $files. $big.';
  }

  @override
  String imageTooLargeYouCan(Object formats) {
    return 'The image is too large. You can move up to 64 million pixels and $formats. Save the image as a file and move that.';
  }

  @override
  String tooManyItemsFolderYou(Object maxEntries, Object pack) {
    return 'Too many items in the folder. You can move up to $maxEntries at once. For this folder, $pack.';
  }

  @override
  String folderTooDeepYouCan(Object maxDepth, Object pack) {
    return 'The folder is too deep. You can move up to $maxDepth levels inside a folder. For this folder, $pack.';
  }

  @override
  String longestPathCharacters(Object chars) {
    return 'longest path $chars characters, ';
  }

  @override
  String someFilesHavePathsToo(Object chars, Object limit, Object pack) {
    return 'Some files have paths that are too long (${chars}paths inside a folder can be up to $limit characters). For this folder, $pack.';
  }

  @override
  String get overClipboardTransferLimit => 'Over the clipboard transfer limit.';

  @override
  String overClipboardLimitYouCan(
    Object files,
    Object maxFiles,
    Object maxTextBytes,
    Object big,
  ) {
    return 'Over the clipboard limit. You can move $files, up to $maxFiles files, and text up to $maxTextBytes. $big.';
  }

  @override
  String get clipboardConnected => 'Clipboard connected';

  @override
  String get clipboardBeingTransferredPasteAgain =>
      'The clipboard is being transferred. Paste again when it\'s done.';

  @override
  String get couldntPrepareContentPasteCopy =>
      'Couldn\'t prepare the content to paste. Copy it again.';

  @override
  String get preparingClipboardConnection => 'Preparing clipboard connection';

  @override
  String get checkingPcsClipboard => 'Checking your PC\'s clipboard';

  @override
  String get requestingDesktopsClipboard =>
      'Requesting the desktop\'s clipboard';

  @override
  String get preparingPaste => 'Preparing to paste';

  @override
  String get checkAutomaticClipboardSharingAgain =>
      'Check automatic clipboard sharing again.';

  @override
  String get checkingWasAppliedDesktop =>
      'Checking it was applied on the desktop';

  @override
  String get sendingClipboard => 'Sending clipboard';

  @override
  String get gettingClipboardFiles => 'Getting clipboard files';

  @override
  String get couldntPrepareAutomaticClipboardSharing =>
      'Couldn\'t prepare automatic clipboard sharing.';

  @override
  String get gettingClipboard => 'Getting clipboard';

  @override
  String get filesArriveWhenYouPaste => 'Files arrive when you paste';

  @override
  String get arrivesWhenYouPaste => 'Arrives when you paste';

  @override
  String get cancelingClipboardTransfer => 'Canceling clipboard transfer';

  @override
  String copiedFilesArriveWhenYou(num treeSkipped) {
    String _temp0 = intl.Intl.pluralLogic(
      treeSkipped,
      locale: localeName,
      other: '$treeSkipped links or unusable names are',
      one: '1 link or unusable name is',
    );
    return 'Copied. Files arrive when you paste. $_temp0 left out.';
  }

  @override
  String get copiedFilesArriveWhenYou2 =>
      'Copied. Files arrive when you paste.';

  @override
  String get copiedArrivesWhenYouPaste => 'Copied. It arrives when you paste.';

  @override
  String clipboardTransferDoneSkipped(num treeSkipped) {
    String _temp0 = intl.Intl.pluralLogic(
      treeSkipped,
      locale: localeName,
      other: '$treeSkipped links or unusable names',
      one: '1 link or unusable name',
    );
    return 'Clipboard transfer done. Skipped $_temp0.';
  }

  @override
  String get clipboardTransferDone => 'Clipboard transfer done';

  @override
  String get originalFileChangedCopyAgain =>
      'The original file changed. Copy it again, then paste.';

  @override
  String get whatYouCopiedNoLonger =>
      'What you copied is no longer available. Copy it again, then paste.';

  @override
  String get somethingNewWasCopiedSo =>
      'Something new was copied, so nothing was overwritten. Paste again.';

  @override
  String get administratorDoesntAllowClipboardTransfer =>
      'Your administrator doesn\'t allow this clipboard transfer.';

  @override
  String get nothingCopiedNoTextImage =>
      'Nothing copied: no text, image or file.';

  @override
  String get nothingCopiedNoTextFile => 'Nothing copied: no text or file.';

  @override
  String get contentCantMovedOnlyText =>
      'This content can\'t be moved. Only text, images, files and folders can.';

  @override
  String onlyTextFilesCanMoved(Object sharedFolders) {
    return 'Only text and files can be moved right now. For folders, $sharedFolders. Images and folders need the latest app and Host on the desktop.';
  }

  @override
  String get tryAgainAfterOtherClipboard =>
      'Try again after the other clipboard task finishes.';

  @override
  String get clipboardTransferCanceled => 'Clipboard transfer canceled.';

  @override
  String get couldntConfirmWasMovedTry =>
      'Couldn\'t confirm it was moved. Try pasting, and if that fails, copy again.';

  @override
  String get couldntReadWhatYouCopied =>
      'Couldn\'t read what you copied. Copy it again, then paste.';

  @override
  String get couldntPutReceivingClipboardCopy =>
      'Couldn\'t put it on the receiving clipboard. Copy it again, then paste.';

  @override
  String get cantReadCopiedFileCheck =>
      'Can\'t read the copied file. Check that it exists and its name is valid, then copy again.';

  @override
  String get anotherProgramUsingFileSo =>
      'Another program is using the file, so it can\'t be read. Close the file in that program, then copy again.';

  @override
  String get notEnoughDiskSpaceReceiving =>
      'Not enough disk space on the receiving side. Free up space and try again.';

  @override
  String get desktopDidntRespondTime => 'The desktop didn\'t respond in time.';

  @override
  String get clipboardCantUsedBecauseDesktop =>
      'The clipboard can\'t be used because the desktop is locked or a security prompt (such as User Account Control) is open. Unlock it or close the prompt, then try again.';

  @override
  String get clipboardIsntAvailableConnection =>
      'Clipboard isn\'t available in this connection.';

  @override
  String get permissionsDesktopHaveChanged =>
      'Permissions for this desktop have changed.';

  @override
  String get administratorChangedClipboardPermission =>
      'Your administrator changed the clipboard permission.';

  @override
  String get youveReachedClipboardLimitConnection =>
      'You\'ve reached the clipboard limit for this connection. Reconnect to the desktop to use it again.';

  @override
  String get receivedContentWasInvalidSo =>
      'The received content was invalid, so it wasn\'t transferred. Copy it again, then paste.';

  @override
  String get clipboardConnectionDroppedSoWasnt =>
      'The clipboard connection dropped, so it wasn\'t transferred. Copy it again, then paste.';

  @override
  String get couldntTransferWhatYouCopied =>
      'Couldn\'t transfer what you copied. Copy it again, then paste.';

  @override
  String get clipboardSharingStoppedReconnectDesktop =>
      'Clipboard sharing stopped. Reconnect to the desktop to use it again.';

  @override
  String clipboardSharingStopped(Object status) {
    return 'Clipboard sharing stopped. $status';
  }

  @override
  String get connectionRequest => 'Connection request';

  @override
  String get preparingVirtualDisplay => 'Preparing virtual display';

  @override
  String get serverConnection => 'Server connection';

  @override
  String get waitingFirstFrame => 'Waiting for the first frame';

  @override
  String get waitingHostUpdate => 'Waiting for Host update';

  @override
  String get waitingDesktopRestart => 'Waiting for desktop restart';

  @override
  String get locked => 'Locked';

  @override
  String get signScreen => 'Sign-in screen';

  @override
  String get screenOff => 'Screen is off';

  @override
  String get savedLastScreen => 'Saved last screen';

  @override
  String get lastScreen => 'Last screen';

  @override
  String get justNow => 'Just now';

  @override
  String minAgo(Object ageInMinutes) {
    return '$ageInMinutes min ago';
  }

  @override
  String text7(num ageInHours) {
    String _temp0 = intl.Intl.pluralLogic(
      ageInHours,
      locale: localeName,
      other: '$ageInHours hours ago',
      one: '1 hour ago',
    );
    return '$_temp0';
  }

  @override
  String text8(num ageInDays) {
    String _temp0 = intl.Intl.pluralLogic(
      ageInDays,
      locale: localeName,
      other: '$ageInDays days ago',
      one: '1 day ago',
    );
    return '$_temp0';
  }

  @override
  String get folder => 'Folder';

  @override
  String get mediaFoundationDeviceNotVerified =>
      'Media Foundation · device not verified';

  @override
  String get checkingInitialization => 'Checking initialization';

  @override
  String outputNotVerified(Object label) {
    return '$label · output not verified';
  }

  @override
  String get buildWithoutGpuEncoder => 'Build without GPU encoder';

  @override
  String get selectedGpuNotFound => 'Selected GPU not found';

  @override
  String get vendorNotSupportedCurrentGpu =>
      'Vendor not supported on the current GPU path';

  @override
  String get gpuDriverEncoderUnavailable => 'GPU driver encoder unavailable';

  @override
  String get gpuApiVersionNotSupported => 'GPU API version not supported';

  @override
  String get requestedH264FormatNot => 'Requested H.264 format not supported';

  @override
  String get gpuInitializationFailed => 'GPU initialization failed';

  @override
  String get gpuMemoryAllocationFailed => 'GPU memory allocation failed';

  @override
  String get gpuFrameSubmissionFailed => 'GPU frame submission failed';

  @override
  String get gpuOutputTimedOut => 'GPU output timed out';

  @override
  String get gpuOutputFormatCheckFailed => 'GPU output format check failed';

  @override
  String get gpuDeviceDisconnected => 'GPU device disconnected';

  @override
  String get gpuSettingsChangeFailed => 'GPU settings change failed';

  @override
  String get x264SettingsRejected => 'x264 settings rejected';

  @override
  String get x264InitializationFailed => 'x264 initialization failed';

  @override
  String get x264EncodingFailed => 'x264 encoding failed';

  @override
  String get x264OutputFormatCheckFailed => 'x264 output format check failed';

  @override
  String get x264OutputTooLarge => 'x264 output too large';

  @override
  String get x264MemoryAllocationFailed => 'x264 memory allocation failed';

  @override
  String get x264ProcessingFailed => 'x264 processing failed';

  @override
  String get ready => 'Ready';

  @override
  String get autoInstall => 'Auto-install';

  @override
  String get setupNeeded => 'Setup needed';

  @override
  String get notSupported => 'Not supported';

  @override
  String get unknown2 => 'Unknown';

  @override
  String get installWinfsp => 'Install WinFsp';

  @override
  String get startPrintSpooler => 'Start print spooler';

  @override
  String get switchMultitap => 'Switch to Multitap';

  @override
  String get sharedFolder => 'Shared folder';

  @override
  String get installWinfsp2 => 'Install WinFsp.';

  @override
  String get runHostInstallerAgainInstall =>
      'Run the Host installer again to install WinFsp.';

  @override
  String get refreshStatus => 'Refresh the status.';

  @override
  String get runHostInstallerAgain => 'Run the Host installer again.';

  @override
  String get saveWorkRestartWindowsWont =>
      'Save your work and restart Windows. It won\'t restart automatically.';

  @override
  String get printer => 'Printer';

  @override
  String get maySecuritySettingSoIsnt =>
      'This may be a security setting, so it isn\'t turned on automatically. If needed, turn it on yourself in Windows Services.';

  @override
  String get refreshStatusMoment => 'Refresh the status in a moment.';

  @override
  String get startPrintSpoolerWindowsServices =>
      'Start Print Spooler in Windows Services.';

  @override
  String get turnMicrosoftPrintPdfWindows =>
      'Turn on ‘Microsoft Print to PDF’ in Windows Features.';

  @override
  String get soundCard => 'Sound card';

  @override
  String get administratorNeedsAddAudioDevice =>
      'Your administrator needs to add an audio device in Proxmox.';

  @override
  String get checkAgainFromStreamingProxmox =>
      'Check again from streaming or the Proxmox console (for a local PC, its own screen).';

  @override
  String get deviceFactWinfsp => 'Uses WinFsp';

  @override
  String get deviceFactNoWinfsp => 'No WinFsp, no files over 4 GB';

  @override
  String get deviceFactNoWinfspWebClient => 'No WinFsp, WebClient off';

  @override
  String get deviceFactUsbWindows => 'Windows 10 (1809) or later, not Server';

  @override
  String get deviceFactUsbFilesMissing => 'Driver files missing';

  @override
  String get deviceFactUsbFirstUse => 'Installs on first connection';

  @override
  String get deviceFactUsbRunning => 'Driver running';

  @override
  String get deviceFactUsbStopped => 'Driver not started';

  @override
  String get deviceFactUsbInstalled => 'Driver installed';

  @override
  String get deviceFactUsbForeign => 'In use by another program';

  @override
  String get deviceSwitchUsbMultitap =>
      'Switch the USB connection to Multitap.';

  @override
  String get deviceFactPrinterReady =>
      'Adds a Multitap printer when you connect';

  @override
  String get deviceFactSpoolerOff => 'Print spooler off';

  @override
  String get deviceStartSpooler => 'Start the print spooler.';

  @override
  String get deviceFactSpoolerDisabled => 'Print spooler set to ‘Disabled’';

  @override
  String get deviceFactSpoolerStarting => 'Print spooler starting';

  @override
  String get deviceFactPdfMissing => 'PDF printing missing';

  @override
  String get deviceFactAudioPresent => 'Output device found';

  @override
  String get deviceFactAudioAbsent => 'No sound output device';

  @override
  String get deviceFactAudioRemote => 'Signed in over RDP';

  @override
  String get checkDeviceStatus => 'Check device status';

  @override
  String ready2(Object l) {
    return '$l ready';
  }

  @override
  String notSupported2(Object unsupported) {
    return '$unsupported not supported';
  }

  @override
  String get couldntConfirmInstallResultRefresh =>
      'Couldn\'t confirm the install result.\nRefresh the status before installing again.';

  @override
  String get restartWindowsApplySaveWork =>
      'Restart Windows to apply. Save your work, then restart. It won\'t restart automatically.';

  @override
  String winfspInstalled(Object restartNotice) {
    return 'WinFsp is installed.\n$restartNotice';
  }

  @override
  String get winfspInstalledFromNextConnection =>
      'WinFsp is installed.\nFrom the next connection, large files in shared folders open right away.';

  @override
  String get winfspAlreadyInstalled => 'WinFsp is already installed.';

  @override
  String get couldntInstallBecauseAnotherProgram =>
      'Couldn\'t install because another program is being installed.\nTry again when that install finishes.';

  @override
  String get windowsPolicyBlockedInstallCheck =>
      'A Windows policy blocked the install.\nCheck the Windows install policy.';

  @override
  String get couldntVerifyInstallerFileDownload =>
      'Couldn\'t verify the installer file.\nDownload the Host installer again and run it.';

  @override
  String get couldntInstallRestartWindowsThen =>
      'Couldn\'t install.\nRestart Windows, then try again.';

  @override
  String get couldntVerifyAfterInstallingRestart =>
      'Couldn\'t verify after installing.\nRestart Windows, then check the status.';

  @override
  String get couldntConfirmInstallResultRefresh2 =>
      'Couldn\'t confirm the install result.\nRefresh the status to check.';

  @override
  String errorCode(Object code) {
    return ' (error code: $code)';
  }

  @override
  String meanwhileSharedFoldersUseBasic(
    Object cause,
    Object restart,
    Object safe,
  ) {
    return '$cause$restart\nMeanwhile, shared folders use the basic mode.$safe';
  }

  @override
  String get saveWorkThenRestartWindows =>
      'Save your work, then restart Windows. It won\'t restart automatically.';

  @override
  String get usbConnectionFeatureSwitchedMultitap =>
      'The USB connection feature is switched to Multitap.\nYou can use USB from the next connection.';

  @override
  String restartWindowsSwitchUsbConnection(Object restartNotice) {
    return 'Restart Windows to switch the USB connection feature.\n$restartNotice';
  }

  @override
  String get alreadySetUpMultitap => 'Already set up for Multitap.';

  @override
  String get couldntRemoveOtherProgramsUsb =>
      'Couldn\'t remove the other program\'s USB connection feature.\nClose or uninstall that program first, then try again.';

  @override
  String get multitapUsbDriverCantInstalled =>
      'The Multitap USB driver can\'t be installed.\nRun the Host installer again.';

  @override
  String get couldntInstallMultitapUsbDriver =>
      'Couldn\'t install the Multitap USB driver.\nRefresh the status, then try again.';

  @override
  String get printSpoolerHasStartedYou =>
      'The print spooler has started.\nYou can use the printer from the next connection.';

  @override
  String get printSpoolerSetDisabledIsnt =>
      'The print spooler is set to ‘Disabled’.\nIt isn\'t changed automatically.';

  @override
  String get couldntStartPrintSpoolerCheck =>
      'Couldn\'t start the print spooler.\nCheck Print Spooler in Windows Services.';

  @override
  String get couldntCheckPrintSpoolerStatus =>
      'Couldn\'t check the print spooler status.\nRefresh the status.';

  @override
  String get oldMultitapVirtualDisplayAdapter =>
      'Old Multitap virtual display adapter';

  @override
  String get leftoverAdapterMadeByMultitap =>
      'Leftover adapter made by Multitap';

  @override
  String get sameDriverNoMultitapRecord => 'Same driver · no Multitap record';

  @override
  String get couldntCheckVirtualDisplayAdapters =>
      'Couldn\'t check the virtual display adapters.';

  @override
  String virtualDisplayAdaptersUseNone(Object current) {
    return 'Virtual display adapters: $current in use · none left over';
  }

  @override
  String virtualDisplayAdaptersUseLeft(Object current, Object leftover) {
    return 'Virtual display adapters: $current in use · $leftover left over';
  }

  @override
  String get couldntConfirmCleanupResultRefresh =>
      'Couldn\'t confirm the cleanup result.\nRefresh the status.';

  @override
  String get listChangedMeantimeRefreshStatus =>
      'The list changed in the meantime.\nRefresh the status, then check again.';

  @override
  String get couldntReadVirtualDisplayDevices =>
      'Couldn\'t read the virtual display devices.\nRefresh the status.';

  @override
  String text9(num removed) {
    String _temp0 = intl.Intl.pluralLogic(
      removed,
      locale: localeName,
      other: 'Removed $removed devices.',
      one: 'Removed 1 device.',
    );
    return '$_temp0';
  }

  @override
  String afterYouRestartWindowsWont(num pending) {
    String _temp0 = intl.Intl.pluralLogic(
      pending,
      locale: localeName,
      other: '$pending devices go away',
      one: '1 device goes away',
    );
    return '$_temp0 after you restart Windows. It won\'t restart automatically.';
  }

  @override
  String refreshStatusThenTryAgain(num failed) {
    String _temp0 = intl.Intl.pluralLogic(
      failed,
      locale: localeName,
      other: 'Couldn\'t remove $failed devices.',
      one: 'Couldn\'t remove 1 device.',
    );
    return '$_temp0 Refresh the status, then try again.';
  }

  @override
  String get thereWereNoDevicesRemove => 'There were no devices to remove.';

  @override
  String get noInfo => 'No info';

  @override
  String get multitapHostDiagnostics => 'Multitap Host diagnostics';

  @override
  String hostVersion(Object state) {
    return 'Host version: $state';
  }

  @override
  String desktop2(Object state) {
    return 'Desktop: $state';
  }

  @override
  String server2(Object state) {
    return 'Server: $state';
  }

  @override
  String get registered => 'Registered';

  @override
  String get notRegistered => 'Not registered';

  @override
  String serverRegistration(Object state) {
    return 'Server registration: $state';
  }

  @override
  String get installed => 'Installed';

  @override
  String get notInstalled => 'Not installed';

  @override
  String serviceInstall(Object service) {
    return 'Service install: $service';
  }

  @override
  String get running => 'Running';

  @override
  String serviceState(Object service) {
    return 'Service state: $service';
  }

  @override
  String serviceQueryError(Object service) {
    return 'Service query error: $service';
  }

  @override
  String get serverResponded => 'Server responded';

  @override
  String serverConnection2(Object verifiedOnline) {
    return 'Server connection: $verifiedOnline';
  }

  @override
  String serverConnectionStage(Object brokerStates) {
    return 'Server connection stage: $brokerStates';
  }

  @override
  String s(Object value) {
    return '$value s';
  }

  @override
  String sinceLastServerResponse(Object validAge) {
    return 'Since last server response: $validAge';
  }

  @override
  String serverConnectionFailureReason(Object broker) {
    return 'Server connection failure reason: $broker';
  }

  @override
  String get yes => 'Yes';

  @override
  String windowsSign(Object state) {
    return 'Windows sign-in: $state';
  }

  @override
  String virtualDisplayDriver(Object display) {
    return 'Virtual display driver: $display';
  }

  @override
  String get started => 'Started';

  @override
  String virtualDisplayDevice(Object display) {
    return 'Virtual display device: $display';
  }

  @override
  String virtualDisplay(Object display) {
    return 'Virtual display: $display';
  }

  @override
  String virtualMonitor(Object m, Object m2, Object m3) {
    return 'Virtual monitor $m: $m2 × $m3';
  }

  @override
  String get needed => 'Needed';

  @override
  String get notNeeded => 'Not needed';

  @override
  String displayRecovery(Object display) {
    return 'Display recovery: $display';
  }

  @override
  String get notRequested2 => 'Not requested';

  @override
  String windowsRestart(Object display) {
    return 'Windows restart: $display';
  }

  @override
  String get saved => 'Saved';

  @override
  String windowsAccount(Object account) {
    return 'Windows account: $account';
  }

  @override
  String get unknownVendor => 'Unknown vendor';

  @override
  String get anotherSettingsChangeWasMade =>
      'Another settings change was made. Check the current selection again.';

  @override
  String get selectedGpuChangedCheckList =>
      'The selected GPU changed. Check the list again.';

  @override
  String get anotherEncoderSettingsTaskProgress =>
      'Another encoder settings task is in progress. Check the status again.';

  @override
  String get couldntVerifyEncoderSettingsProtection =>
      'Couldn\'t verify the encoder settings protection. Run the Host installer again to repair it.';

  @override
  String get couldntReadEncoderSettingsFile =>
      'Couldn\'t read the encoder settings file. Refresh the status.';

  @override
  String get couldntConfirmSaveResultCheck =>
      'Couldn\'t confirm the save result. Check the status before applying again.';

  @override
  String get saveResponseDoesntMatchCurrent =>
      'The save response doesn\'t match the current settings. Check the status again.';

  @override
  String get couldntReadGpuListCpu =>
      'Couldn\'t read the GPU list. CPU settings still work.';

  @override
  String get couldntCheckEncoderSettingsUpdate =>
      'Couldn\'t check the encoder settings. Update Host or check the status again.';

  @override
  String get couldntReadStatus => 'Status read failed';

  @override
  String get hostServiceMissing => 'Host service missing';

  @override
  String get hostServiceOff => 'Host service off';

  @override
  String get checkHostService => 'Host service needs a check';

  @override
  String get installationNeedsFinishing => 'Installation unfinished';

  @override
  String get checkVirtualDisplayDriver =>
      'Virtual display driver needs a check';

  @override
  String get restartWindows => 'Windows restart needed';

  @override
  String get virtualDisplayNeedsRecovery => 'Virtual display recovery needed';

  @override
  String get serverConnectionNeeded => 'Server connection needed';

  @override
  String get administratorApprovalNeeded => 'Administrator approval needed';

  @override
  String get notConnectedServer => 'Not connected to the server';

  @override
  String get windowsSignNeeded => 'Windows sign-in needed';

  @override
  String get streamingDisplaySwitchNeeded => 'Streaming display switch needed';

  @override
  String get preparingVirtualDisplay2 => 'Preparing the virtual display';

  @override
  String get checkVirtualDisplay => 'Virtual display needs a check';

  @override
  String get autoSignSetupNeeded => 'Auto sign-in setup needed';

  @override
  String get checkWindowsSign => 'Windows sign-in needs a check';

  @override
  String get checkDevices => 'Devices need a check';

  @override
  String desktopRegisteredServer(Object id) {
    return 'Desktop registered on the server · $id';
  }

  @override
  String get desktopRegisteredServer2 => 'Desktop registered on the server';

  @override
  String get checkedJustNow => 'Checked just now';

  @override
  String checkedSAgo(Object ageSeconds) {
    return 'Checked $ageSeconds s ago';
  }

  @override
  String get existingVirtualDisplaySettingsNeed =>
      'The existing virtual display settings need checking.\nCopy the diagnostics and let your administrator know.';

  @override
  String get virtualDisplayDeviceDidntStart =>
      'The virtual display device didn\'t start.\nRestart Windows, then check the status.';

  @override
  String get couldntVerifyVirtualDisplayDriver =>
      'Couldn\'t verify the virtual display driver files.\nDownload the Host installer again and run it.';

  @override
  String get finishInstallation => 'Finish installation';

  @override
  String get virtualDisplayDriverSharedFolder =>
      'The virtual display driver and the shared folder component (WinFsp) aren\'t installed yet.\nInstall them to connect to this desktop.';

  @override
  String get virtualDisplayDriverIsntInstalled =>
      'The virtual display driver isn\'t installed yet.\nInstall it to connect to this desktop.';

  @override
  String get sharedFolderComponentWinfspIsnt =>
      'The shared folder component (WinFsp) isn\'t installed yet.\nInstall it to open large files right away.';

  @override
  String get trustVirtualDisplayDriversSignature =>
      'Trust the virtual display driver\'s signature?';

  @override
  String get thenDriverSharedFolderComponent =>
      'Then the driver and the shared folder component (WinFsp) are installed.';

  @override
  String get thenDriverInstalled => 'Then the driver is installed.';

  @override
  String virtualDisplayOnlyOriginalSunshine(Object winfsp) {
    return 'For the virtual display only, the original Sunshine-signed driver is used as is.\nOne signing certificate goes into the Windows trust stores (Root·TrustedPublisher).\n$winfsp';
  }

  @override
  String get trustInstall => 'Trust and install';

  @override
  String get installVirtualDisplayDriver =>
      'Install the virtual display driver?';

  @override
  String get sharedFolderComponentWinfspInstalled =>
      '\nThe shared folder component (WinFsp) is installed too.';

  @override
  String installsAlreadyTrustedOriginalSunshine(Object winfsp) {
    return 'Installs the already trusted original Sunshine-signed driver.$winfsp';
  }

  @override
  String get couldntConfirmVirtualDisplayDriver =>
      'Couldn\'t confirm the virtual display driver install result.\nRefresh the status to check.';

  @override
  String get virtualDisplayDriverAlreadyInstalled =>
      'The virtual display driver is already installed.';

  @override
  String get virtualDisplayDriverInstalled =>
      'The virtual display driver is installed.';

  @override
  String get rdpSignYouCantCheck =>
      'In an RDP sign-in, you can\'t check whether the display is on. Check in the Proxmox console (for a local PC, its own screen).';

  @override
  String get couldntRestartHostServiceRestart =>
      'Couldn\'t restart the Host service. Restart the service from the diagnostics report.';

  @override
  String get notInstalledBecauseSignatureVerification =>
      'Not installed because signature verification didn\'t finish.\nSelect Finish installation again.';

  @override
  String get couldntRegisterDriverSignatureWindows =>
      'Couldn\'t register the driver signature in Windows.\nCheck the Windows certificate policy.';

  @override
  String get existingVirtualDisplaySettingsNeed2 =>
      'The existing virtual display settings need checking.\nExisting devices weren\'t changed. Copy the diagnostics and let your administrator know.';

  @override
  String get anotherInstallationProgressTryAgain =>
      'Another installation is in progress.\nTry again in a moment.';

  @override
  String get couldntSaveInstallResultTry =>
      'Couldn\'t save the install result.\nTry again.';

  @override
  String get windowsCouldntInstallVirtualDisplay =>
      'Windows couldn\'t install the virtual display driver.\nRestart Windows, then try again.';

  @override
  String get restart => 'Restart';

  @override
  String get winfspWasAlreadyInstalled => 'WinFsp was already installed.';

  @override
  String get nothingInstall => 'Nothing to install.';

  @override
  String get savedInfoHasChangedGone =>
      'The saved info has changed or is gone. Check the new status, then choose again.';

  @override
  String get couldntConfirmDeletionDontRun =>
      'Couldn\'t confirm the deletion. Don\'t run it again; refresh the status.';

  @override
  String get updateHostClearSavedInfo => 'Update Host to clear the saved info.';

  @override
  String get couldntConfirmServiceRestartedDont =>
      'Couldn\'t confirm the service restarted. Don\'t run it again; refresh the status.';

  @override
  String get connectionRequestChangedCheckCurrent =>
      'The connection request changed. Check the current request, then cancel again.';

  @override
  String get checkServerAddressPort1 =>
      'Check the server address and the port (1–65535).';

  @override
  String get agreeHttpServerAddressFirst2 =>
      'Agree to this HTTP server address first.';

  @override
  String get currentAddressDoesntMatchServers =>
      'The current address doesn\'t match the server\'s access settings. Check the address, port and server settings.';

  @override
  String get cantConfirmHttpSupportServer =>
      'Can\'t confirm HTTP support on the server. Update the server or use an HTTPS address.';

  @override
  String get httpConnectionsOffServerAsk =>
      'HTTP connections are off on the server. Ask your administrator.';

  @override
  String get addressIsntServersAllowedList =>
      'This address isn\'t on the server\'s allowed list. Check the address and port.';

  @override
  String get addressOnlyFirstSetupSet =>
      'This address is only for first setup. Set the access address in the admin console.';

  @override
  String get couldntConnectSecurelyOverHttps2 =>
      'Couldn\'t connect securely over HTTPS. Check the server address and certificate.';

  @override
  String get couldntConnectAddressPortCheck =>
      'Couldn\'t connect to this address and port. Check that the server is running and the port is right.';

  @override
  String get serverDidntRespondTimeCheck2 =>
      'The server didn\'t respond in time. Check the address, port and server status.';

  @override
  String get couldntReachServerBecauseNetwork =>
      'Couldn\'t reach the server because of a network problem. Check your network connection.';

  @override
  String get couldntVerifyMultitapServerResponse =>
      'Couldn\'t verify the Multitap server response. Check the address, port and server status.';

  @override
  String get couldntReadHostSettingsResponse =>
      'Couldn\'t read the Host settings response. Run the Host installer again.';

  @override
  String get couldntFinishSetupRefreshStatus =>
      'Couldn\'t finish the setup. Refresh the status to check.';

  @override
  String get deletionIsntConfirmedYetDont =>
      'The deletion isn\'t confirmed yet. Don\'t run it again; refresh the status.';

  @override
  String get serviceRestartIsntConfirmedYet =>
      'The service restart isn\'t confirmed yet. Don\'t run it again; refresh the status in a moment.';

  @override
  String get requestCancellationIsntConfirmedYet =>
      'The request cancellation isn\'t confirmed yet. Don\'t cancel again; refresh the status.';

  @override
  String get encoderSaveIsntConfirmedYet =>
      'The encoder save isn\'t confirmed yet. Don\'t save again; check the status.';

  @override
  String get winfspInstallIsntConfirmedYet =>
      'The WinFsp install isn\'t confirmed yet. Don\'t install again; refresh the status in a moment.';

  @override
  String get virtualDisplayDriverInstallIsnt =>
      'The virtual display driver install isn\'t confirmed yet. Don\'t install again; refresh the status in a moment.';

  @override
  String get deviceChangeIsntConfirmedYet =>
      'The device change isn\'t confirmed yet. Don\'t run it again; refresh the status in a moment.';

  @override
  String get passwordResultIsntConfirmedYet =>
      'The password result isn\'t confirmed yet.';

  @override
  String get autoSignRequestResultIsnt =>
      'The auto sign-in request result isn\'t confirmed yet. Don\'t apply it again; check the result.';

  @override
  String get settingsResponseTimedOutRefresh =>
      'The settings response timed out. Refresh the status to see what was applied, then try again.';

  @override
  String get couldntFindHostConfigurationFile =>
      'Couldn\'t find the Host configuration file. Run the Host installer again.';

  @override
  String get setupTimeRanOutStart =>
      'Setup time ran out. Start again from identity verification.';

  @override
  String get checkAgainMoment => 'Check again in a moment.';

  @override
  String get couldntFinishIdentityVerificationTry =>
      'Couldn\'t finish identity verification. Try again.';

  @override
  String get checkResultOtherSettingsChange =>
      'Check the result of the other settings change first.';

  @override
  String get settingChangedSignOutThen =>
      'The setting changed. Sign out, then sign in again.';

  @override
  String get couldntConfirmChange => 'Couldn\'t confirm the change.';

  @override
  String get twoStepVerificationRequired =>
      'Two-step verification is required.';

  @override
  String get documentTooLargeOver256 =>
      'The document is too large (over 256 MB), so it wasn\'t received.';

  @override
  String get tooManyPrintJobsStill =>
      'Too many print jobs are still waiting, so the new document wasn\'t received.';

  @override
  String get couldntSaveDocumentTemporarilyDesktop =>
      'Couldn\'t save the document temporarily on the desktop, so it wasn\'t received. Check the desktop\'s disk space.';

  @override
  String get couldntReceiveDocumentPrintAgain =>
      'Couldn\'t receive the document. Print it again from the desktop.';

  @override
  String get microsoftPrintPdfMissingDesktop =>
      'Microsoft Print to PDF is missing on the desktop, so the printer couldn\'t be created. Let your administrator know.';

  @override
  String get printSpoolerServiceOffDesktop =>
      'The print spooler service is off on the desktop, so the printer couldn\'t be created. Let your administrator know.';

  @override
  String get anotherPrinterSameNameExists =>
      'Another printer with the same name exists on the desktop, so it wasn\'t created. Remove that printer on the desktop, then reconnect.';

  @override
  String get couldntCreatePrinterDesktop =>
      'Couldn\'t create the printer on the desktop.';

  @override
  String get administratorTurnedOffPrinterPermission =>
      'Your administrator turned off the printer permission.';

  @override
  String get cantFindSelectedPrinterCheck =>
      'Can\'t find the selected printer. Check that it\'s on and hasn\'t been removed from the list.';

  @override
  String get printerOfflineQueuedDocumentsPrint =>
      'The printer is offline. Queued documents print when it comes back on.';

  @override
  String get printerPausedResumeWindowsPrint =>
      'The printer is paused. Resume it in Windows to print.';

  @override
  String get printerNeedsAttentionPaperToner =>
      'The printer needs attention (paper, toner, cover or similar).';

  @override
  String get printerHasProblemCheckPrinter =>
      'The printer has a problem. Check the printer status.';

  @override
  String get storageFullNotWritableSo =>
      'Storage is full or not writable, so the document wasn\'t received.';

  @override
  String get printingCanceled => 'Printing canceled.';

  @override
  String get couldntOpenDocumentSoWasnt =>
      'Couldn\'t open the document, so it wasn\'t printed. You can save it as a PDF.';

  @override
  String get printingIsntAvailablePc => 'Printing isn\'t available on your PC.';

  @override
  String get couldntSaveFile => 'Couldn\'t save the file.';

  @override
  String get couldntPrint => 'Couldn\'t print.';

  @override
  String get am => 'AM';

  @override
  String get pm => 'PM';

  @override
  String get soon => 'Soon';

  @override
  String min(Object left) {
    return 'In $left min';
  }

  @override
  String get today => 'Today';

  @override
  String get tomorrow => 'Tomorrow';

  @override
  String text10(Object month, Object day) {
    return '$month/$day';
  }

  @override
  String minAgo2(Object passedInMinutes) {
    return '$passedInMinutes min ago';
  }

  @override
  String text11(num passedInHours) {
    String _temp0 = intl.Intl.pluralLogic(
      passedInHours,
      locale: localeName,
      other: '$passedInHours hours ago',
      one: '1 hour ago',
    );
    return '$_temp0';
  }

  @override
  String get myDesktop => 'My desktop';

  @override
  String get samplePcServerRelay => 'Sample PC · server relay';

  @override
  String get windows11PhysicalPc => 'Windows 11 · physical PC';

  @override
  String get proxmoxWorkspace => 'Proxmox workspace';

  @override
  String get lightweightVirtualDesktopStart =>
      'A lightweight virtual desktop to start with';

  @override
  String get devDesktop => 'Dev desktop';

  @override
  String get spaceNewIdeas => 'A space for new ideas';

  @override
  String get multitapPrint => 'Multitap print';

  @override
  String printAgainFromDesktop(num count) {
    String _temp0 = intl.Intl.pluralLogic(
      count,
      locale: localeName,
      other: '$count documents being printed weren\'t received.',
      one: '1 document being printed wasn\'t received.',
    );
    return '$_temp0 Print again from the desktop.';
  }

  @override
  String get couldntChangeScreenSizeSo =>
      'Couldn\'t change the screen size, so the previous size is kept.';

  @override
  String get disconnected => 'Disconnected';

  @override
  String get p2pDirect => 'P2P direct';

  @override
  String get udpViaServer => 'UDP via server';

  @override
  String get tcpViaServer => 'TCP via server';

  @override
  String get connectingServer => 'Connecting to the server…';

  @override
  String get waiting2 => 'Waiting';

  @override
  String get desktopHasNoSoundDevice => 'The desktop has no sound device';

  @override
  String get disconnectedReconnect => 'Disconnected. Reconnect.';

  @override
  String get couldntPrepareKeyboardInputSo =>
      'Couldn\'t prepare keyboard input, so the connection ended. Reconnect.';

  @override
  String get preparingVirtualDisplayResolution =>
      'Preparing the virtual display resolution';

  @override
  String get waitingHostScreen => 'Waiting for the Host screen';

  @override
  String get virtualDisplayTookTooLong =>
      'The virtual display took too long to get ready. Reconnect.';

  @override
  String get couldntConfirmVirtualDisplayChange =>
      'Couldn\'t confirm the virtual display change. Reconnect.';

  @override
  String get preparingRemoteScreen => 'Preparing the remote screen';

  @override
  String get resizingVirtualDisplay => 'Resizing the virtual display';

  @override
  String get restoringPreviousScreenSize =>
      'Restoring the previous screen size';

  @override
  String get couldntResizeVirtualDisplayReconnect =>
      'Couldn\'t resize the virtual display. Reconnect.';

  @override
  String get couldntPrepareVirtualDisplayResolution =>
      'Couldn\'t prepare the virtual display resolution. Reconnect, and if it keeps happening, let your administrator know.';

  @override
  String get virtualDisplayOpenedDifferentResolution =>
      'The virtual display opened at a different resolution. Reconnect.';

  @override
  String get preparingScreenNewResolution =>
      'Preparing the screen at the new resolution';

  @override
  String get preparingFirstFrame => 'Preparing the first frame';

  @override
  String get changingRemoteScreenResolution =>
      'Changing the remote screen resolution';

  @override
  String get couldntResizeRemoteScreenSo =>
      'Couldn\'t resize the remote screen, so the connection ended. Reconnect.';

  @override
  String get hostStoppedRespondingSoConnection =>
      'Host stopped responding, so the connection ended. Reconnect.';

  @override
  String get didntReceiveFirstFrameReconnect =>
      'Didn\'t receive the first frame. Reconnect, and if it keeps happening, let your administrator know.';

  @override
  String get screenStoppedArrivingSoConnection =>
      'The screen stopped arriving, so the connection ended. Reconnect.';

  @override
  String get oneMonitorsScreenStoppedArriving =>
      'One monitor\'s screen stopped arriving, so the connection ended. Reconnect.';

  @override
  String get couldntShowScreenSoConnection =>
      'Couldn\'t show the screen, so the connection ended. Reconnect.';

  @override
  String get couldntApplyRequestedSizeSo =>
      'Couldn\'t apply the requested size, so the previous size was restored.';

  @override
  String get oneMonitorsDisplayFrozeSo =>
      'One monitor\'s display froze, so the connection ended. Reconnect.';

  @override
  String get displayFrozeSoConnectionEnded =>
      'The display froze, so the connection ended. Reconnect.';

  @override
  String get soundOutputDeviceIsntAvailable =>
      'The sound output device isn\'t available';

  @override
  String get playing => 'Playing';

  @override
  String get couldntCheckDisplayStateSo =>
      'Couldn\'t check the display state, so the connection ended. Reconnect.';

  @override
  String get couldntPrepareVideoNewResolution =>
      'Couldn\'t prepare video at the new resolution. Reconnect.';

  @override
  String get remoteResolutionTookTooLong =>
      'The remote resolution took too long to apply. Reconnect.';

  @override
  String get largerResolutionsNeedHostServer =>
      'Larger resolutions need a Host and server update. Let your administrator know.';

  @override
  String get connectionEnded2 => 'Connection ended';

  @override
  String get networkConnectionDropped => 'The network connection dropped.';

  @override
  String get couldntChangeAutoSignOut =>
      'Couldn\'t change the auto sign-out setting. Try again in a moment.';

  @override
  String get couldntConfirmAutoSignOut =>
      'Couldn\'t confirm the auto sign-out setting. The current setting is kept.';

  @override
  String get hostCantHandleScreenSize => 'Host can\'t handle this screen size.';

  @override
  String get couldntCheckHostsAvailableMemory =>
      'Couldn\'t check Host\'s available memory.';

  @override
  String get hostDoesntHaveEnoughFree =>
      'Host doesn\'t have enough free memory for the screen.';

  @override
  String get hostDoesntSupportScreenSize =>
      'Host doesn\'t support this screen size or format.';

  @override
  String get virtualDisplayDoesntSupportResolution =>
      'The virtual display doesn\'t support this resolution and refresh rate.';

  @override
  String keepingPreviousSize(Object message) {
    return '$message Keeping the previous size.';
  }

  @override
  String get reconnectMoment => ' Reconnect in a moment.';

  @override
  String get makeScreenSmallerReconnect =>
      ' Make the screen smaller and reconnect.';

  @override
  String get reconnectDifferentScreenSize =>
      ' Reconnect with a different screen size.';

  @override
  String get console2 => 'in Console';

  @override
  String get fromAnotherWindowDevice => 'from another window or device';

  @override
  String sameAccount(Object where) {
    return 'The same account $where';
  }

  @override
  String account(Object account, Object where) {
    return 'The account $account $where';
  }

  @override
  String anotherAccount(Object where) {
    return 'Another account $where';
  }

  @override
  String connectedDesktopSoConnectionEnded(Object who) {
    return '$who connected to this desktop, so this connection ended.\nReconnect to disconnect that one.';
  }

  @override
  String get desktopWasConnectedFromAnother =>
      'This desktop was connected from another window or device, so this connection ended. Reconnect to disconnect that one.';

  @override
  String get administratorChangedClipboardSharingPermission =>
      'Your administrator changed the clipboard sharing permission. Reconnect to apply it.';

  @override
  String get disconnectedHostUpdateReconnectWhen =>
      'Disconnected for a Host update. Reconnect when the update finishes.';

  @override
  String get signExpiredDesktopsAssignmentChanged =>
      'Your sign-in expired or this desktop\'s assignment changed. Check the desktop list.';

  @override
  String get desktopsHostWasUnregisteredLet =>
      'This desktop\'s Host was unregistered. Let your administrator know.';

  @override
  String get hostCouldntPrepareScreenReconnect =>
      'Host couldn\'t prepare the screen. Reconnect, and if it keeps happening, let your administrator know.';

  @override
  String get hostStoppedSendingScreenReconnect =>
      'Host stopped sending the screen. Reconnect.';

  @override
  String get hostConnectionDroppedReconnectMoment =>
      'The Host connection dropped. Reconnect in a moment, and if it keeps happening, let your administrator know.';

  @override
  String get serverRestartingReconnectMoment =>
      'The server is restarting. Reconnect in a moment.';

  @override
  String get screenCouldntProcessedTimeSo =>
      'The screen couldn\'t be processed in time, so the connection ended. Lower the picture quality or reconnect.';

  @override
  String get virtualDisplaySetupChangedScreen =>
      'The virtual display setup changed or the screen couldn\'t be resized, so the connection ended. Reconnect.';

  @override
  String get couldntConnectAllMonitorsFrom =>
      'Couldn\'t connect on all monitors.\nFrom the list, connect in window mode or full screen.';

  @override
  String get couldntConnectServerCheckNetwork2 =>
      'Couldn\'t connect to the server. Check your network and reconnect.';

  @override
  String get couldntConnectDesktopReconnectIf =>
      'Couldn\'t connect to the desktop. Reconnect, and if it keeps happening, let your administrator know.';

  @override
  String get notEnoughMemoryPrepareScreen =>
      'Not enough memory to prepare this screen size.';

  @override
  String get twoScreensCantShownOnce =>
      'Two screens can\'t be shown at once here.';

  @override
  String get screenSizeFrameRateCant =>
      'This screen size and frame rate can\'t be shown here.';

  @override
  String get videoFormatCantShownHere =>
      'This video format can\'t be shown here.';

  @override
  String get screenSizeCantShownHere =>
      'This screen size can\'t be shown here.';

  @override
  String get couldntCheckWhichScreenSizes =>
      'Couldn\'t check which screen sizes can be shown. Keeping the previous size.';

  @override
  String get pcDecoderSizeLimit => 'PC decoder size limit';

  @override
  String get pcDecoderSpeedLimit => 'PC decoder speed limit';

  @override
  String get pcDecodingLoad => 'PC decoding load';

  @override
  String get desktopEncodingLoad => 'Desktop encoding load';

  @override
  String get limitSetByAdministrator => 'Limit set by administrator';

  @override
  String get desktopConnectedSomewhereElseInformation =>
      'This desktop is connected somewhere else. The information needed to end that connection didn\'t arrive. Try connecting again, and if it keeps happening, tell your administrator.';

  @override
  String connectedAccount(Object username) {
    return 'Connected account $username';
  }

  @override
  String connectedAccountSinceDate(Object username, Object date, Object time) {
    return 'Connected account $username · since $date, $time';
  }

  @override
  String get useAnotherWindow => 'In use in another window';

  @override
  String useWindow(Object name) {
    return 'In use in the $name window';
  }

  @override
  String get deviceAlsoNeedsPermissionIts =>
      'This device also needs the permission for its type (shared folder, printer, smart card, serial (COM) port). Ask your administrator.';

  @override
  String get devicesSuchAsKeyboardsMice =>
      'Devices such as keyboards, mice, hubs, Bluetooth, audio and cameras can\'t be connected.';

  @override
  String get keyboardsMiceArentConnectedDesktop =>
      'Keyboards and mice aren\'t connected to the desktop.';

  @override
  String get couldntReadDeviceInformationSo =>
      'Couldn\'t read the device information, so it can\'t be connected.';

  @override
  String youCanConnectUpDevices(Object usbMaxDevices) {
    return 'You can connect up to $usbMaxDevices devices at a time.';
  }

  @override
  String get desktopDidntReceiveDevice =>
      'The desktop didn\'t receive the device.';

  @override
  String get anotherUsbVirtualDeviceProgram =>
      'Another USB virtual device program is on the desktop.\nYou can fix this in the desktop\'s Host settings → Devices → USB.';

  @override
  String get desktopsHostHasNoUsb =>
      'The desktop\'s Host has no USB connection driver. Tell your administrator.';

  @override
  String get couldntInstallUsbConnectionDriver =>
      'Couldn\'t install the USB connection driver on the desktop. Tell your administrator.';

  @override
  String get restartDesktopUseUsbDevices =>
      'Restart the desktop to use USB devices.';

  @override
  String get desktopsUsbConnectionDriverIsnt =>
      'The desktop\'s USB connection driver isn\'t protected, so USB devices aren\'t connected. Tell your administrator.';

  @override
  String get administratorTurnedOffUsbPermission =>
      'Your administrator turned off USB permission.';

  @override
  String get deviceWasUnpluggedFromPc =>
      'The device was unplugged from your PC.';

  @override
  String get deviceWasRemovedDesktop =>
      'The device was removed on the desktop.';

  @override
  String get anotherProgramUsingDevice =>
      'Another program is using this device.';

  @override
  String get itsUsePcSoCant =>
      'It\'s in use on your PC, so it can\'t be connected. Close any open files or windows and try again.';

  @override
  String get useSomewhereElse => 'In use somewhere else.';

  @override
  String get couldntTakeDeviceFromPc =>
      'Couldn\'t take the device from your PC.';

  @override
  String get anotherWindowProgramAlreadyUsing =>
      'Another window or program is already using this device. Disconnect it there and try again.';

  @override
  String get couldntGetDeviceBackFrom =>
      'Couldn\'t get the device back from the other window. Disconnect it in that window and try again.';

  @override
  String get couldntCommunicateDeviceTryAgain =>
      'Couldn\'t communicate with the device. Try again.';

  @override
  String get takingDeviceFromPcTook =>
      'Taking the device from your PC took too long. Try again.';

  @override
  String get openFileCheckDidntFinish =>
      'The open file check didn\'t finish. Try again in a moment.';

  @override
  String get couldntCheckWhetherPcCan =>
      'Couldn\'t check whether your PC can use the device. Select ‘Return to PC’.';

  @override
  String get deviceDidntReturnPcSelect =>
      'The device didn\'t return to your PC. Select ‘Return to PC’. If that doesn\'t work, plug the USB device in again.';

  @override
  String get noMoreDevicesCanConnected =>
      'No more devices can be connected in this connection. Reconnect to the desktop.';

  @override
  String get couldntFindUsbDriverIf =>
      'Couldn\'t find the USB driver. If restarting your PC doesn\'t help, reinstall the client.';

  @override
  String get couldntVerifyInstallersSignatureSo =>
      'Couldn\'t verify the installer\'s signature, so it wasn\'t installed.';

  @override
  String get couldntInstallUsbDriverTry =>
      'Couldn\'t install the USB driver. Try again.';

  @override
  String get windowsAdministratorApprovalWasCanceled =>
      'The Windows administrator approval was canceled, so the USB driver wasn\'t installed. To install it, select ‘Install USB driver…’ again and choose ‘Yes’.';

  @override
  String get restartPcUseUsbDevices => 'Restart your PC to use USB devices.';

  @override
  String get couldntConnectDevice => 'Couldn\'t connect the device.';

  @override
  String get usbReturnedChannelEnded =>
      'The connection ended, so the device returned to your PC.';

  @override
  String get smartCard => 'Smart card';

  @override
  String get serialComPort => 'Serial (COM) port';

  @override
  String deviceAlsoNeedsPermissionAsk(Object value) {
    return 'This device also needs the $value permission. Ask your administrator.';
  }

  @override
  String get usbDeviceReconnected => 'USB device reconnected';

  @override
  String couldntReconnectUsbDevice(Object failure) {
    return 'Couldn\'t reconnect the USB device. $failure';
  }

  @override
  String get usbConnectionLost => 'USB connection lost';

  @override
  String get multitapHostSettings => 'Multitap Host settings';

  @override
  String get couldntPrepareRemoteWindow =>
      'Couldn\'t prepare the remote window';

  @override
  String get multitapRemoteWindow => 'Multitap remote window';

  @override
  String get closeWindowConnectAgainFrom =>
      'Close the window and connect again from the desktop list.';

  @override
  String get windowsLockedUnlockSameWindows =>
      'Windows is locked. Unlock it with the same Windows account, then connect.';

  @override
  String get theresNoWindowsSignSession =>
      'There\'s no Windows sign-in session to stream. Sign in to Windows.';

  @override
  String get youCanBringWindowsScreen =>
      'You can bring the Windows screen you used over RDP into streaming.';

  @override
  String get checkHostSettingsWindowsSign =>
      'Check Host settings → Windows sign-in → Windows account.';

  @override
  String get anotherWindowsUserSeveralSign =>
      'Another Windows user or several sign-ins are present, so the session can\'t be chosen automatically.';

  @override
  String get windowsSignStateChangedDuring =>
      'The Windows sign-in state changed during the task. Refresh the list.';

  @override
  String get sessionSwitchResultIsntConfirmed =>
      'The session switch result isn\'t confirmed yet. Don\'t repeat the same action; refresh the status.';

  @override
  String get couldntReturnWindowsSessionStreaming =>
      'Couldn\'t return the Windows session to the streaming screen. Check the session status on the Host.';

  @override
  String get windowsSessionVirtualDisplayBeing =>
      'The Windows session or virtual display is being set up. Connect when it\'s done.';

  @override
  String get windowsSessionBackButVirtual =>
      'The Windows session is back, but the virtual display isn\'t ready yet. Check the virtual display status on the Host.';

  @override
  String get returnWindowsSessionConfirmSwitch =>
      'To return the Windows session, confirm in the switch notice.';

  @override
  String get hostCouldntConfirmScreenWas =>
      'The Host couldn\'t confirm the screen was restored after the previous connection.';

  @override
  String get hostReportedScreenWasRestored =>
      'The Host reported the screen was restored, but the new screen state isn\'t confirmed yet.';

  @override
  String get hostCouldntConfirmTimePrevious =>
      'The Host couldn\'t confirm in time that the previous connection ended and the screen was restored.';

  @override
  String get hostReportRestoreUnconfirmed =>
      'Host report · restore unconfirmed';

  @override
  String get restoreReportedWaitingScreenCheck =>
      'Restore reported · waiting for screen check';

  @override
  String get hostCheckTimedOut => 'Host check timed out';

  @override
  String get endCheckDelayed => 'End check delayed';

  @override
  String get endPreviousStreamingIsntConfirmed =>
      'The end of the previous streaming isn\'t confirmed either. Refresh to check again, and if it keeps happening, tell your administrator.';

  @override
  String get refreshCheckAgainIfKeeps =>
      'Refresh to check again, and if it keeps happening, tell your administrator.';

  @override
  String get checkingPreviousConnectionEndedScreen =>
      'Checking that the previous connection ended and the screen was restored.';

  @override
  String get connectingBlockedBecauseScreenRestore =>
      'Connecting is blocked because the screen restore after the previous connection isn\'t confirmed. Refresh, and if it keeps happening, tell your administrator.';

  @override
  String get settingUpVirtualDisplayYou =>
      'Setting up the virtual display. You can connect when it\'s done.';

  @override
  String get cantStartConnectionRightNow =>
      'Can\'t start a connection right now. Try again in a moment.';

  @override
  String get desktopOff => 'The desktop is off.';

  @override
  String get desktopIsntResponding => 'The desktop isn\'t responding.';

  @override
  String get virtualDisplayIsntReadyTell =>
      'The virtual display isn\'t ready. Tell your administrator.';

  @override
  String get checkingWhetherStreamingReady =>
      'Checking whether streaming is ready.';

  @override
  String get waitingHostConnect => 'Waiting for the Host to connect.';

  @override
  String get switchingPower => 'Switching power';

  @override
  String get restoreUnconfirmed => 'Restore unconfirmed';

  @override
  String get restoringScreen => 'Restoring screen';

  @override
  String get settingUpScreen => 'Setting up screen';

  @override
  String get poweredOff => 'Powered off';

  @override
  String get offline => 'Offline';

  @override
  String get screenSetup => 'Screen setup';

  @override
  String get number => 'Number';

  @override
  String get name => 'Name';

  @override
  String get recent => 'Recent';

  @override
  String get runningFirst => 'Running first';

  @override
  String get all => 'All';

  @override
  String get favorites => 'Favorites';

  @override
  String get showAllDesktops => 'Show all desktops';

  @override
  String get showRunningDesktopsOnly => 'Show running desktops only';

  @override
  String get showOffDesktopsOnly => 'Show off desktops only';

  @override
  String get noDesktopsAssigned => 'No desktops assigned';

  @override
  String get noRunningDesktops => 'No running desktops';

  @override
  String get noDesktopsOff => 'No desktops are off';

  @override
  String get showVmsOnly => 'Show VMs only';

  @override
  String get showLocalPcsOnly => 'Show local PCs only';

  @override
  String get noVmsAssigned => 'No VMs assigned';

  @override
  String get noLocalPcsAssigned => 'No local PCs assigned';

  @override
  String get noDesktopsMatch => 'No desktops match';

  @override
  String noDesktopsMatch2(Object short) {
    return 'No desktops match \'$short\'';
  }

  @override
  String get searchByNameNumberGroup => 'Search by name, number or group.';

  @override
  String searchedWithin(Object parts) {
    return 'Searched within $parts.';
  }

  @override
  String get connectedAccount2 => 'Connected with your account';

  @override
  String get anotherUserUsing => 'Another user is using it';

  @override
  String accountUsingIt(Object account) {
    return '$account is using it';
  }

  @override
  String inUseByAccount(Object account) {
    return 'In use by $account';
  }

  @override
  String accountUsingItWhenYou(Object account) {
    return '$account is using it. When you connect, you\'re asked first whether to end that connection.';
  }

  @override
  String get connectionAlreadyOpen => 'A connection is already open';

  @override
  String get connection => 'Your connection';

  @override
  String get useByAnotherUser => 'In use by another user';

  @override
  String get use => 'In use';

  @override
  String get connectedAccountIfItsConnection =>
      'Connected with your account. If it\'s a connection opened on another device, you can bring it here when you connect.';

  @override
  String get anotherUserUsingWhenYou =>
      'Another user is using it. When you connect, you\'re asked first whether to end that connection.';

  @override
  String get desktopAlreadyHasOpenConnection =>
      'This desktop already has an open connection.';

  @override
  String hostUpdate2(Object version, Object latestVersion) {
    return 'Host update · $version → $latestVersion';
  }

  @override
  String get hostUpdateHostOffline => 'Host update · Host offline';

  @override
  String get hostUpdateCheckingVersion => 'Host update · checking version';

  @override
  String hostRemoteUpdateNotSupported(Object version) {
    return 'Host $version · remote update not supported';
  }

  @override
  String hostLatestVersion(Object version) {
    return 'Host $version · latest version';
  }

  @override
  String hostCheckingUpdates(Object version) {
    return 'Host $version · checking for updates';
  }

  @override
  String get checkingHostVersion => 'Checking Host version';

  @override
  String get hostOfflineVersionUnknown => 'Host offline · version unknown';

  @override
  String updating(Object host) {
    return '$host · updating';
  }

  @override
  String offline2(Object host) {
    return '$host · offline';
  }

  @override
  String upDate2(Object host) {
    return '$host · up to date';
  }

  @override
  String remoteUpdateNotSupportedInstall2(Object host) {
    return '$host · remote update not supported · install manually';
  }

  @override
  String newVersionAvailable(Object host, Object latestVersion) {
    return '$host · new version $latestVersion available';
  }

  @override
  String get youCanReconnectWhenIts => 'You can reconnect when it\'s done.';

  @override
  String downloading(Object op, Object after) {
    return 'Downloading$op. $after';
  }

  @override
  String checkingFile(Object after) {
    return 'Checking the file. $after';
  }

  @override
  String installing(Object after) {
    return 'Installing. $after';
  }

  @override
  String updateStarted(Object after) {
    return 'Update started. $after';
  }

  @override
  String get hostUpdateWaitingStartsWhen =>
      'Host update waiting · starts when the connection ends';

  @override
  String updatingHostDownloading(Object op) {
    return 'Updating Host · downloading$op';
  }

  @override
  String get updatingHostCheckingFile => 'Updating Host · checking file';

  @override
  String get updatingHostInstalling => 'Updating Host · installing';

  @override
  String hostUpdatedWindowsRestartNeeded(Object targetVersion) {
    return 'Host $targetVersion updated · Windows restart needed';
  }

  @override
  String hostUpdatedCheckHostSettings(Object targetVersion) {
    return 'Host $targetVersion updated · check Host settings';
  }

  @override
  String hostUpdated(Object targetVersion) {
    return 'Host $targetVersion updated';
  }

  @override
  String checkHostUpdateResult(Object code) {
    return 'Check Host update result · $code';
  }

  @override
  String get hostUpdateWaitExpired => 'Host update wait expired';

  @override
  String hostUpdateFailed(Object code) {
    return 'Host update failed · $code';
  }

  @override
  String get hostDidntReconnectAfterUpdate =>
      'The Host didn\'t reconnect after the update';

  @override
  String get checkConnectionStatusHostDiagnostics =>
      'Check the connection status in Host diagnostics and the server activity log.';

  @override
  String get checkServiceStatusLocalPcs =>
      'Check the service status in the local PC\'s Host settings → Diagnostics.';

  @override
  String get openDesktopConsoleCheckHost =>
      'Open the desktop with Console and check Host settings → Diagnostics.';

  @override
  String couldntConfirmUpdateResultConnection(Object host) {
    return 'Couldn\'t confirm the $host update result and connection.';
  }

  @override
  String couldntGetResponseFromAfter(Object host) {
    return 'Couldn\'t get a response from $host after the update failed.';
  }

  @override
  String couldntGetResponseFromAfter2(Object host) {
    return 'Couldn\'t get a response from $host after it reported the update was done.';
  }

  @override
  String get connectionUse => 'A connection is in use';

  @override
  String get anotherUpdateProgress => 'Another update is in progress';

  @override
  String get notNewerVersion => 'Not a newer version';

  @override
  String get couldntDownloadInstaller => 'Couldn\'t download the installer';

  @override
  String get fileSizeDoesntMatchRelease =>
      'The file size doesn\'t match the release info';

  @override
  String get filesSha256DoesntMatch =>
      'The file\'s SHA-256 doesn\'t match the release info';

  @override
  String get couldntVerifyReleaseSignature =>
      'Couldn\'t verify the release signature';

  @override
  String get downloadedFileIsntInstaller =>
      'The downloaded file isn\'t an installer';

  @override
  String get couldntStartInstaller => 'Couldn\'t start the installer';

  @override
  String get mustInstalledDesktopDirectly =>
      'Must be installed on the desktop directly';

  @override
  String get noExistingHost => 'No existing Host';

  @override
  String get didntReceiveInstallResult => 'Didn\'t receive the install result';

  @override
  String get couldntFinishInstalling => 'Couldn\'t finish installing';

  @override
  String get noResponseFromHost => 'No response from the Host';

  @override
  String get hostConnectionLost => 'Host connection lost';

  @override
  String get noMoreDetails => 'No more details';

  @override
  String get serverHasntReceivedLocalPcs =>
      'The server hasn\'t received this local PC\'s network card info yet.';

  @override
  String get wakeSignalWasSentBut =>
      'The wake signal was sent, but the PC didn\'t start within 4 minutes.';

  @override
  String get serverCouldntSendWakeSignal =>
      'The server couldn\'t send the wake signal.';

  @override
  String get localPcMustConnectedBy =>
      'The local PC must be connected by wired Ethernet.';

  @override
  String get wakeLanMustBiosUefi =>
      'Wake-on-LAN must be on in the BIOS/UEFI settings.';

  @override
  String get deviceManagerNetworkAdapterPower =>
      'In Device Manager → network adapter → Power Management / Advanced, ‘Wake on Magic Packet’ must be on.';

  @override
  String get localPcMustSameNetwork =>
      'The local PC must be on the same network as the server (same router or switch).';

  @override
  String get installLatestHostLocalPc =>
      'Install the latest Host on the local PC and start it once, and the server receives its network card info.';

  @override
  String get windowed => 'Windowed';

  @override
  String get fullScreen => 'Full screen';

  @override
  String get allMonitors => 'All monitors';

  @override
  String get askAdministrator => 'Ask your administrator.';

  @override
  String get selectMultitapPrinterDesktopPrint =>
      'Select the Multitap printer on the desktop.\nPrint or save as PDF from this device\'s print screen.';

  @override
  String get chooseFolderDeviceUnderShared =>
      'Choose a folder on this device under Shared folder in the menu.\nFiles saved on the desktop show the save time as their modified date.';

  @override
  String get copyPaste => 'Copy and paste';

  @override
  String get youCanPasteCopiedText =>
      'You can paste copied text between this device and the desktop.';

  @override
  String get youCanPasteTextCopied =>
      'You can paste text copied on this device into the desktop.';

  @override
  String get youCanPasteTextCopied2 =>
      'You can paste text copied on the desktop into this device.';

  @override
  String get onceConnectionReady => 'Once the connection is ready: ';

  @override
  String get pcDesktop => 'PC ↔ desktop';

  @override
  String get pcDesktop2 => 'PC → desktop';

  @override
  String get desktopPc => 'Desktop → PC';

  @override
  String get text12 => 'Text';

  @override
  String get files => 'Files';

  @override
  String selectMultitapPrinterDesktopPreview(Object printState) {
    return '${printState}Select the Multitap printer on the desktop.\nPreview on your PC, then print or save as PDF.';
  }

  @override
  String get usbDevices => 'USB devices';

  @override
  String selectUsbDeviceToolbarConnected(Object usbState) {
    return '${usbState}Select a USB device in the toolbar.\nA connected device can be used only on the desktop.';
  }

  @override
  String selectPcFolderToolbarSharing(Object folderState) {
    return '${folderState}Select a PC folder in the toolbar.\nSharing ends when you disconnect.';
  }

  @override
  String get filesTransferredWhenYouPaste =>
      '\nFiles are transferred when you paste.';

  @override
  String useCtrlCCtrlV(Object clipboardState, Object n, Object policy) {
    return '${clipboardState}Use Ctrl+C / Ctrl+V.\n$n$policy';
  }

  @override
  String get desktopHasNoSoundDevice2 =>
      'This desktop has no sound device. Your administrator must add an audio device in Proxmox.';

  @override
  String get restartDesktopUseSoundDevice =>
      'Restart the desktop to use the sound device.';

  @override
  String get work => 'Work';

  @override
  String get n60FpsAutoBitrate => '60 fps · auto bitrate';

  @override
  String get saver => 'Saver';

  @override
  String get n30Fps8MbpsUses =>
      '30 fps · 8 Mbps · uses less data and processing';

  @override
  String get restartNeeded => 'Restart needed';

  @override
  String get restartWindowsGetVirtualDisplay =>
      'Restart Windows to get the virtual display ready.';

  @override
  String get consoleNeeded => 'Console needed';

  @override
  String get virtualDisplayCantPreparedRdp =>
      'The virtual display can\'t be prepared on an RDP sign-in. Tell your administrator.';

  @override
  String get signNeeded => 'Sign-in needed';

  @override
  String get signWindowsConnect => 'Sign in to Windows to connect.';

  @override
  String get checkWindowsAccount => 'Check Windows account';

  @override
  String get windowsAccountMustCheckedHost =>
      'The Windows account must be checked in Host settings → Windows sign-in. Tell your administrator.';

  @override
  String get checkWindowsScreen => 'Check Windows screen';

  @override
  String get couldntCheckWindowsScreenIf =>
      'Couldn\'t check the Windows screen. If it keeps happening, tell your administrator.';

  @override
  String get resumeWindows => 'Resume Windows';

  @override
  String get windowsLocked => 'Windows locked';

  @override
  String get windowsLockedUnlockConnect =>
      'Windows is locked. Unlock it to connect.';

  @override
  String get adminAccountNeeded => 'Admin account needed';

  @override
  String get windowsAccountSignedNowIsnt =>
      'The Windows account signed in now isn\'t an administrator. Sign in with an administrator account to connect.';

  @override
  String get repairNeeded => 'Repair needed';

  @override
  String get virtualDisplayNeedsRepairTell =>
      'The virtual display needs repair. Tell your administrator.';

  @override
  String get notReady => 'Not ready';

  @override
  String get virtualDisplayIsntReadyIf2 =>
      'The virtual display isn\'t ready. If it keeps happening, tell your administrator.';

  @override
  String get checkDriver => 'Check driver';

  @override
  String get couldntCheckVirtualDisplayDriver =>
      'Couldn\'t check the virtual display driver. Tell your administrator.';

  @override
  String get checkSettings => 'Check settings';

  @override
  String get virtualDisplaySettingsNeedChecking =>
      'The virtual display settings need checking. Tell your administrator.';

  @override
  String get hostSettingUpVirtualDisplay =>
      'The Host is setting up the virtual display. You can connect when it\'s done.';

  @override
  String get checkStatus => 'Check status';

  @override
  String get couldntCheckVirtualDisplayStatus =>
      'Couldn\'t check the virtual display status. If it keeps happening, tell your administrator.';

  @override
  String get checkingMonitors => 'Checking monitors';

  @override
  String get checkMonitorCount => 'Check monitor count';

  @override
  String get checkAgain2 => ' · check again';

  @override
  String text13(num activeCount, Object stale) {
    String _temp0 = intl.Intl.pluralLogic(
      activeCount,
      locale: localeName,
      other: '$activeCount monitors',
      one: '1 monitor',
    );
    return '$_temp0$stale';
  }

  @override
  String get theme2 => 'Theme';

  @override
  String get background3 => 'Background';

  @override
  String get disconnect2 => 'Disconnect?';

  @override
  String streaming3(Object machine) {
    return '$machine · streaming';
  }

  @override
  String ifOneUseUnsavedWork(num usbShared) {
    String _temp0 = intl.Intl.pluralLogic(
      usbShared,
      locale: localeName,
      other:
          'The $usbShared USB devices connected to the desktop are also disconnected.',
      one: 'The USB device connected to the desktop is also disconnected.',
    );
    return '$_temp0 If one is in use, unsaved work may be lost.';
  }

  @override
  String programsDesktopSavingThemMay(num foldersShared) {
    String _temp0 = intl.Intl.pluralLogic(
      foldersShared,
      locale: localeName,
      other:
          'The $foldersShared folders shared with the desktop are also disconnected.',
      one: 'The folder shared with the desktop is also disconnected.',
    );
    return '$_temp0 Programs on the desktop saving to them may fail to save.';
  }

  @override
  String text14(num printsWaiting) {
    String _temp0 = intl.Intl.pluralLogic(
      printsWaiting,
      locale: localeName,
      other: '$printsWaiting documents not yet printed or saved will be lost.',
      one: '1 document not yet printed or saved will be lost.',
    );
    return '$_temp0';
  }

  @override
  String get clientInfo => 'Client info';

  @override
  String get windowsClient => 'Windows client';

  @override
  String get version => 'Version';

  @override
  String get openSourceLicenses => 'Open source licenses';

  @override
  String get licenses => 'Licenses';

  @override
  String get about => 'About';

  @override
  String get build => 'Build';

  @override
  String get demoMode => 'Demo mode';

  @override
  String get multitapServer => 'Multitap server';

  @override
  String get screenIsntConnectedRealServer =>
      'This screen isn\'t connected to a real server.';

  @override
  String get serverManagesDesktopListConnections =>
      'This server manages the desktop list, connections and updates.';

  @override
  String get address => 'Address';

  @override
  String get serverVersion => 'Server version';

  @override
  String get unavailable => 'Unavailable';

  @override
  String get account2 => 'Account';

  @override
  String get searchComponentsLicenseText => 'Search components or license text';

  @override
  String componentsSearchNameLicenseText(Object componentsLength) {
    return '$componentsLength components · search name or license text';
  }

  @override
  String get someLicensesCouldntLoaded => 'Some licenses couldn\'t be loaded.';

  @override
  String get loading => 'Loading…';

  @override
  String get reload => 'Reload';

  @override
  String get noLicensesShow => 'No licenses to show.';

  @override
  String get noMatchingLicenses => 'No matching licenses.';

  @override
  String text15(num noticesLength) {
    String _temp0 = intl.Intl.pluralLogic(
      noticesLength,
      locale: localeName,
      other: '$noticesLength license texts',
      one: '1 license text',
    );
    return '$_temp0';
  }

  @override
  String version2(Object version) {
    return 'Version $version';
  }

  @override
  String get couldntSaveThemeChooseAgain =>
      'Couldn\'t save the theme. Choose it again.';

  @override
  String get couldntSaveBackgroundChooseAgain =>
      'Couldn\'t save the background. Choose it again.';

  @override
  String get connectWithoutEncryption => 'Connect without encryption?';

  @override
  String get signDetailsScreenInputArent =>
      'Sign-in details, screen and input aren\'t encrypted.\nUse this only on a network you trust.\nYour consent applies only to this address and port.';

  @override
  String get useHttpAddress => 'Use HTTP for this address';

  @override
  String get hostBulkUpdate => 'Host bulk update';

  @override
  String get hostsAssignedDesktops => 'Hosts on assigned desktops';

  @override
  String assignedDesktopsLatest(Object visibleRowsLength, Object version) {
    return 'Assigned desktops: $visibleRowsLength · latest $version';
  }

  @override
  String get checkingServersLatestHostRelease =>
      'Checking this server\'s latest Host release info.';

  @override
  String get couldntLoadLatestHostRelease =>
      'Couldn\'t load the latest Host release info.\nClose the window and check for updates again.';

  @override
  String hostChanges(Object version) {
    return 'Host $version changes';
  }

  @override
  String get theresNoConnectionCheckInstall =>
      'There\'s no connection to check the install result, so the request can\'t be sent.';

  @override
  String get couldntGetLatestStatusFrom =>
      'Couldn\'t get the latest status from the server. Results of accepted requests are still being checked.';

  @override
  String get remainingRequestsStoppedBecauseYou =>
      'Remaining requests stopped because you lack permission or your sign-in expired. Accepted updates continue.';

  @override
  String get latestReleaseChangedSoRemaining =>
      'The latest release changed, so remaining requests stopped. Close this window and check the new version.';

  @override
  String waitingNextRequestLeft(Object pendingCount) {
    return 'Waiting for the next request · $pendingCount left';
  }

  @override
  String get refreshInstallStatus => 'Refresh install status';

  @override
  String get noDesktopsAssigned2 => 'No desktops assigned.';

  @override
  String get checkProgress => 'Check progress';

  @override
  String get requestServerAcceptedIsntFinished =>
      'A request the server accepted isn\'t a finished install.\nCheck the result for each desktop.';

  @override
  String windowsRestartNeededCheckingResults(
    Object rebootCount,
    Object checkingCount,
  ) {
    return 'Windows restart needed: $rebootCount · checking results: $checkingCount';
  }

  @override
  String get connectedDesktopsDisconnectedUpdatedRight =>
      'Connected desktops are disconnected and updated right away.\nWindows doesn\'t restart.';

  @override
  String get connectedDesktopsConnectionEndsUpdate =>
      'On connected desktops, your connection ends and the update starts right away.\nIf another user is connected, the update starts after that connection ends.';

  @override
  String get stopRemainingRequestsClose => 'Stop remaining requests and close';

  @override
  String get stopRemainingRequests => 'Stop remaining requests';

  @override
  String updateSelected(Object selectedCount) {
    return 'Update $selectedCount selected';
  }

  @override
  String get selectAll2 => 'Select all';

  @override
  String get currentVersionUnknown => 'Current version unknown';

  @override
  String get windowsRestartNeeded => 'Windows restart needed';

  @override
  String get enterCurrentPasswordChooseNew =>
      'Enter your current password and choose a new password of 10–256 characters.';

  @override
  String get couldntChangePasswordTryAgain =>
      'Couldn\'t change the password. Try again.';

  @override
  String get newPasswordLeast10Characters =>
      'New password · at least 10 characters';

  @override
  String get changing => 'Changing…';

  @override
  String get transferring => 'Transferring';

  @override
  String get preparing2 => 'Preparing…';

  @override
  String get finishing => 'Finishing…';

  @override
  String get image => 'Image';

  @override
  String get richText => 'Rich text';

  @override
  String ofLabel(Object fileCount, Object progress) {
    return '$progress of $fileCount';
  }

  @override
  String folders(Object folderCount) {
    return 'Folders: $folderCount';
  }

  @override
  String files2(Object fileCount) {
    return 'Files: $fileCount';
  }

  @override
  String get currentFile => 'Current file';

  @override
  String get items => 'Items';

  @override
  String get saved2 => 'Saved to';

  @override
  String get transferred => 'Transferred';

  @override
  String inLabel(Object destinationName) {
    return 'In $destinationName';
  }

  @override
  String get transferCanceled => 'Transfer canceled';

  @override
  String get couldntTransfer => 'Couldn\'t transfer';

  @override
  String get transferringFiles => 'Transferring files';

  @override
  String get openFolder => 'Open folder';

  @override
  String get minimize => 'Minimize';

  @override
  String get transferProgress => 'Transfer progress';

  @override
  String get less => 'Less';

  @override
  String get details => 'Details';

  @override
  String get canceling => 'Canceling';

  @override
  String get cancelTransfer => 'Cancel this transfer';

  @override
  String get canceling2 => 'Canceling…';

  @override
  String get showTransferDetails => 'Show transfer details';

  @override
  String get almostDone => 'Almost done';

  @override
  String aboutSecLeft(Object seconds) {
    return 'About $seconds sec left';
  }

  @override
  String aboutMinLeft(Object minutes) {
    return 'About $minutes min left';
  }

  @override
  String aboutLeft(num minutes) {
    String _temp0 = intl.Intl.pluralLogic(
      minutes,
      locale: localeName,
      other: '$minutes hours',
      one: '1 hour',
    );
    return 'About $_temp0 left';
  }

  @override
  String aboutHMinLeft(Object minutes, Object rest) {
    return 'About $minutes h $rest min left';
  }

  @override
  String elapsed(Object elapsed) {
    return 'Elapsed $elapsed';
  }

  @override
  String elapsed2(Object stepLabel, Object elapsed) {
    return '$stepLabel, elapsed $elapsed';
  }

  @override
  String get couldntConfirmSessionSwitchResult =>
      'Couldn\'t confirm the session switch result. Refresh the list.';

  @override
  String get couldntTakeOverWindowsScreen =>
      'Couldn\'t take over the Windows screen';

  @override
  String get takingOverWindowsScreen => 'Taking over the Windows screen';

  @override
  String get bringingOverWindowsScreenYou =>
      'Bringing over the Windows screen you used in RDP.\nYou\'re connected as soon as it\'s done.\nEven if you cancel, a switch already started may continue.';

  @override
  String get start => 'Start';

  @override
  String get stop => 'Stop';

  @override
  String get shutDown => 'Shut down';

  @override
  String get stopIsntAvailableServerYet =>
      'Stop isn\'t available on this server yet. Tell your administrator.';

  @override
  String get localPc2 => 'local PC';

  @override
  String get desktop3 => 'desktop';

  @override
  String start2(Object noun) {
    return 'Start the $noun?';
  }

  @override
  String the(Object noun, Object label) {
    return '$label the $noun?';
  }

  @override
  String get startsLocalPcOverNetwork =>
      'Starts the local PC over the network. You can connect once it\'s on.';

  @override
  String get youCanConnectOnceIts => 'You can connect once it\'s on.';

  @override
  String get cutsPowerRightAwayUnsaved =>
      'Cuts the power right away. Unsaved work may be lost. Use this only when Windows isn\'t responding.';

  @override
  String get saveAnyUnsavedWorkFirst => 'Save any unsaved work first.';

  @override
  String get asksWindowsShutDownNormally =>
      'Asks Windows to shut down normally. Save any unsaved work first.';

  @override
  String get pcNeeds2Monitors => 'Your PC needs 2 monitors.';

  @override
  String get desktopsHostDoesntSupportAll =>
      'This desktop\'s Host doesn\'t support all monitors.';

  @override
  String get defaultLabel => 'Default';

  @override
  String get streamingSettings2 => 'Streaming settings…';

  @override
  String get removeFromFavorites => 'Remove from favorites';

  @override
  String get addFavorites => 'Add to favorites';

  @override
  String get checkReadiness => 'Check readiness';

  @override
  String get searchCtrlF => 'Search (Ctrl+F)';

  @override
  String get clear => 'Clear';

  @override
  String get closeSearch => 'Close search';

  @override
  String get searchNameNumberGroup => 'Search name, number, group';

  @override
  String get statusFilter => 'Status filter';

  @override
  String get typeFilter => 'Type filter';

  @override
  String text16(Object label, Object count) {
    return '$label ($count)';
  }

  @override
  String text17(Object chipLabel, Object count) {
    return '$chipLabel ($count)';
  }

  @override
  String get selectAgainClear => 'Select again to clear';

  @override
  String showOnly(Object chipLabel) {
    return 'Show $chipLabel only';
  }

  @override
  String get windowsNeedsRestart => 'Windows needs a restart';

  @override
  String get getsVirtualDisplayReadyUnsaved =>
      'That gets the virtual display ready. Unsaved work may be lost.';

  @override
  String get signedOverRdp => 'Signed in over RDP';

  @override
  String get virtualDisplayCantPreparedRdp2 =>
      'The virtual display can\'t be prepared on an RDP sign-in.';

  @override
  String get notSignedWindows => 'Not signed in to Windows';

  @override
  String get windowsAccountNeedsChecking =>
      'The Windows account needs checking';

  @override
  String get couldntCheckWindowsScreen => 'Couldn\'t check the Windows screen';

  @override
  String get refreshCheckAgainIfKeeps2 =>
      'Refresh to check again. If it keeps happening, tell your administrator.';

  @override
  String get windowsScreenFromRdpOpen => 'A Windows screen from RDP is open';

  @override
  String get bringScreenIntoStreamingConnect =>
      'Bring that screen into streaming to connect.';

  @override
  String get windowsLocked2 => 'Windows is locked';

  @override
  String get unlockConnect => 'Unlock it to connect.';

  @override
  String get signWindowsAdministratorAccount =>
      'Sign in with a Windows administrator account';

  @override
  String get virtualDisplayNeedsRepair => 'The virtual display needs repair';

  @override
  String get youCanRepairHostSettings =>
      'You can repair it in Host settings. Tell your administrator.';

  @override
  String get virtualDisplayIsntReady => 'The virtual display isn\'t ready';

  @override
  String get refreshMomentIfKeepsHappening =>
      'Refresh soon, or tell your administrator.';

  @override
  String get virtualDisplayDriverNeedsChecking =>
      'The virtual display driver needs checking';

  @override
  String get youCanCheckHostSettings =>
      'You can check this in Host settings → Diagnostics. Tell your administrator.';

  @override
  String get virtualDisplaySettingsNeedChecking2 =>
      'The virtual display settings need checking';

  @override
  String get hostSettingUpScreen => 'The Host is setting up the screen';

  @override
  String get youCanConnectWhenSetup => 'You can connect when setup is done.';

  @override
  String get couldntCheckVirtualDisplayStatus2 =>
      'Couldn\'t check the virtual display status';

  @override
  String get checkingWindowsSign => 'Checking Windows sign-in';

  @override
  String get checkingWindowsSignStateUpdates =>
      'Checking the Windows sign-in state. This updates as soon as it\'s ready.';

  @override
  String get preparingScreen => 'Preparing screen';

  @override
  String get preparingVirtualDisplayUpdatesAs =>
      'Preparing the virtual display. This updates as soon as it\'s done.';

  @override
  String get preparing3 => 'Preparing';

  @override
  String get hostPreparingConnectionUpdatesAs =>
      'The Host is preparing the connection. This updates as soon as it\'s done.';

  @override
  String get power => 'Power';

  @override
  String get checkedAfterPowerActionFinishes =>
      'Checked after the power action finishes';

  @override
  String get confirmedByHostResponse => 'On · confirmed by Host response';

  @override
  String get off4 => 'Off';

  @override
  String get pausedTellAdministrator => 'Paused · tell your administrator';

  @override
  String get powerStateNotReceivedYet => 'Power state not received yet';

  @override
  String get hostService => 'Host service';

  @override
  String get checkedWhenDesktopStarts => 'Checked when the desktop starts';

  @override
  String get responding => 'Responding';

  @override
  String get notRespondingIfWindowsStarting =>
      'Not responding · If Windows is starting, wait a moment';

  @override
  String get noResponseYet => 'No response yet';

  @override
  String get virtualDisplay2 => 'Virtual display';

  @override
  String get checkedWhenHostServiceResponds =>
      'Checked when the Host service responds';

  @override
  String get screenRestoreFromLastConnection =>
      'Screen restore from the last connection not confirmed';

  @override
  String get checkingScreenRestoreFromLast =>
      'Checking screen restore from the last connection';

  @override
  String get checkingVirtualDisplaySetup =>
      'Checking the virtual display setup';

  @override
  String get numberScreensNotReceivedYet =>
      'Number of screens not received yet';

  @override
  String ready3(Object count, Object m) {
    return '$count ready$m';
  }

  @override
  String get readyOnceScreenUsedRdp =>
      'Ready once the screen used in RDP is brought over';

  @override
  String get notReadyCheckHostSettings =>
      'Not ready · Check it in Host settings';

  @override
  String get couldNotCheckWindowsScreen =>
      'Could not check the Windows screen to stream';

  @override
  String get checkingStreamingScreen => 'Checking the streaming screen';

  @override
  String get connectionPath => 'Connection path';

  @override
  String availableViaCheckedAgainWhen(Object server) {
    return 'Available via $server · checked again when you connect';
  }

  @override
  String get notCheckedCheckedOnceEarlier =>
      'Unknown · Checked once the earlier steps are ready';

  @override
  String get sample => 'Sample';

  @override
  String get screenPreview => 'Screen preview';

  @override
  String get designSampleNotConnectedServer =>
      'Design sample · not connected to a server';

  @override
  String starting(Object noun) {
    return 'Starting the $noun';
  }

  @override
  String restarting(Object noun) {
    return 'Restarting the $noun';
  }

  @override
  String shuttingDown(Object noun) {
    return 'Shutting down the $noun';
  }

  @override
  String stopping(Object noun) {
    return 'Stopping the $noun';
  }

  @override
  String get powerActionProgress => 'Power action in progress';

  @override
  String get starting2 => 'Starting';

  @override
  String get shuttingDown2 => 'Shutting down';

  @override
  String get stopping2 => 'Stopping';

  @override
  String get powerAction => 'Power action';

  @override
  String get ifWindowsDoesNotRespond =>
      'If Windows does not respond, you can stop it from the power menu.';

  @override
  String get statusCheckedAgainWhenFinishes =>
      'The status is checked again when this finishes.';

  @override
  String get windowsSign2 => 'Windows sign-in';

  @override
  String get takeOverWindowsScreenConnect =>
      'Take over the Windows screen and connect';

  @override
  String get bringsWindowsScreenUsedRdp =>
      'Brings the Windows screen used in RDP into streaming.';

  @override
  String get readyConnect => 'Ready to connect';

  @override
  String get serverSaysYouCanConnect => 'The server says you can connect.';

  @override
  String get pressStartStartDesktop => 'Press ‘Start’ to start the desktop.';

  @override
  String get paused => 'Paused';

  @override
  String get desktopPaused => 'The desktop is paused';

  @override
  String get letAdministratorKnow => 'Let your administrator know.';

  @override
  String get cleaningUpConnectionJustEnded =>
      'Cleaning up the connection that just ended. Press to connect as soon as it is done.';

  @override
  String get screenRestoreNotConfirmed => 'Screen restore not confirmed';

  @override
  String get cleaningUpLastConnection => 'Cleaning up the last connection';

  @override
  String get youCanConnectWhenDone => 'You can connect when it is done.';

  @override
  String get useConsole => 'In use in Console';

  @override
  String get youCanDisconnectConsoleConnect =>
      'You can disconnect Console and connect by streaming.';

  @override
  String get checking => 'Checking';

  @override
  String get settingUpVirtualDisplay => 'Setting up the virtual display';

  @override
  String get updating2 => 'Updating';

  @override
  String get checkHost => 'Check Host';

  @override
  String get windowsStarting => 'Windows starting';

  @override
  String get windowsStarting2 => 'Windows is starting';

  @override
  String get changesAutomaticallyWhenRestartFinishes =>
      'This changes automatically when the restart finishes.';

  @override
  String get changesAutomaticallyWhenStartupFinishes =>
      'This changes automatically when startup finishes.';

  @override
  String get preparingConnection => 'Preparing the connection';

  @override
  String get windowsHasStartedChangesAs =>
      'Windows has started. This changes as soon as the Host connects.';

  @override
  String get hostDisconnected => 'Host disconnected';

  @override
  String get desktopRunningChangesAutomaticallyWhen =>
      'The desktop is running. This changes automatically when the Host is back.';

  @override
  String get hostNotResponding => 'The Host is not responding';

  @override
  String get ifYouJustStartedWait =>
      'If you just started it, wait a moment. If it continues, let your administrator know.';

  @override
  String get unlockConsole => 'Unlock it in Console.';

  @override
  String get ifSigningAutomaticallyYouCan =>
      'If it is signing in automatically, you can connect soon. Otherwise, sign in from Console.';

  @override
  String get signFromConsoleAfterRestart =>
      'Sign in from Console. After a restart it signs in automatically.';

  @override
  String get virtualDisplayNeedsCheckedHost =>
      'The virtual display needs to be checked in Host settings. Let your administrator know.';

  @override
  String get checkingStreamingReadiness => 'Checking streaming readiness';

  @override
  String get changesAsSoonAsHost =>
      'This changes as soon as the Host reports its status.';

  @override
  String get waitingHostConnect2 => 'Waiting for the Host to connect';

  @override
  String get checkingStatus => 'Checking the status';

  @override
  String get hostConnectsOnceWindowsStarts =>
      'The Host connects once Windows starts.';

  @override
  String get checkingAgainAutomaticallyMoment =>
      'Checking again automatically in a moment.';

  @override
  String get power2 => 'Power on';

  @override
  String get preparing4 => 'Preparing';

  @override
  String get connecting => 'Connecting';

  @override
  String get manageLocalPcHostUpdate =>
      'Manage local PC · Host update available';

  @override
  String get manageLocalPc => 'Manage local PC';

  @override
  String get manageDesktopHostUpdateAvailable =>
      'Manage desktop · Host update available';

  @override
  String get manageDesktop => 'Manage desktop';

  @override
  String step3ElapsedDesktopStays(Object progress, Object clock) {
    return 'Step $progress/3 · $clock elapsed · The desktop stays on if you cancel';
  }

  @override
  String defaultStreaming(Object mode) {
    return 'Default: streaming · $mode';
  }

  @override
  String get diagnostics => 'Diagnostics';

  @override
  String get restart2 => 'Restart…';

  @override
  String get disconnectConsoleConnect2 => 'Disconnect Console and connect';

  @override
  String get working => 'Working…';

  @override
  String get preparing5 => 'Preparing…';

  @override
  String get streamingConnection => 'Stream';

  @override
  String get streamingFullScreen => 'Streaming · full screen';

  @override
  String get streamingAllMonitors => 'Streaming · all monitors';

  @override
  String get startsDesktopYouCanConnect =>
      'Starts the desktop. You can connect once it is on.';

  @override
  String streamingConnection2(Object mode) {
    return 'Streaming connection · $mode';
  }

  @override
  String get connectionOptions => 'Connection options';

  @override
  String get otherWaysConnect => 'Other ways to connect';

  @override
  String get connectsByStreamingAsSoon =>
      'Connects by streaming as soon as it is ready';

  @override
  String get qemuAgentOff => 'QEMU Agent off';

  @override
  String get noSound => 'No sound';

  @override
  String screenFromMayDifferNow(Object picture, Object machine) {
    return 'This is the $machine screen from $picture. It may differ now.';
  }

  @override
  String lastSavedScreenScreenMay(Object machine) {
    return 'This is the last saved screen. The $machine screen may differ now.';
  }

  @override
  String get noPreview => 'No preview';

  @override
  String get qemuGuestAgentOffProxmox =>
      'QEMU Guest Agent is off in the Proxmox VM options.\nIf your administrator turns it on, the status is shown more accurately while Windows starts or when the Host is disconnected.';

  @override
  String startedAgo(Object uptime) {
    return 'Started $uptime ago';
  }

  @override
  String startedAgoAccordingProxmox(Object uptime) {
    return 'Started $uptime ago, according to Proxmox';
  }

  @override
  String get statusDetails => 'Status details';

  @override
  String get unknown3 => 'unknown';

  @override
  String monitor(Object o, Object o2) {
    return 'Monitor $o $o2';
  }

  @override
  String get couldNotFindRemoteScreen => 'Could not find the remote screen.';

  @override
  String get couldNotCheckWindowsScale => 'Could not check the Windows scale.';

  @override
  String get windowsDidNotAllowScale =>
      'Windows did not allow the scale change.';

  @override
  String get couldNotSaveOriginalScale =>
      'Could not save the original scale, so the scale was not changed.';

  @override
  String get remoteScreenDoesNotSupport =>
      'This remote screen does not support changing the Windows scale.';

  @override
  String get couldNotCheckResultScale =>
      'Could not check the result of the scale change.';

  @override
  String get disconnected2 => 'Disconnected.';

  @override
  String get youCannotChangeScaleConnection =>
      'You cannot change the scale in this connection.';

  @override
  String autoNow(Object currentScale) {
    return 'Auto · now $currentScale%';
  }

  @override
  String get autoCurrentScaleNotChecked =>
      'Auto · current scale not checked yet';

  @override
  String get apply => 'Apply';

  @override
  String requested4(Object wanted) {
    return ' · requested $wanted%';
  }

  @override
  String applied(Object currentScale, Object wanted) {
    return 'Applied $currentScale%$wanted';
  }

  @override
  String requestedChecking(Object wanted) {
    return '$wanted% requested · checking';
  }

  @override
  String get currentScaleNotCheckedYet => 'Current scale not checked yet';

  @override
  String resolutionAutoNow(Object sourceWidth, Object sourceHeight) {
    return 'Resolution auto · now $sourceWidth × $sourceHeight';
  }

  @override
  String get resolutionAutoCurrentSizeNot =>
      'Resolution auto · current size not checked yet';

  @override
  String get scale => 'Scale';

  @override
  String autoCurrentScaleUnknown(Object scaleStatus) {
    return 'Auto · current scale unknown\n$scaleStatus';
  }

  @override
  String max(Object o) {
    return 'Max · $o';
  }

  @override
  String upScreenSize(Object maximum) {
    return 'Up to $maximum% at this screen size';
  }

  @override
  String get folderAlreadyShared => 'This folder is already shared.';

  @override
  String youCanShareUpFolders(Object driveMaxShares) {
    return 'You can share up to $driveMaxShares folders.';
  }

  @override
  String get couldNotFindUserSigned =>
      'Could not find the user signed in to your PC, so the folder was not shared.\nCheck that Windows Explorer is running.';

  @override
  String get pcUserCannotOpenFolder =>
      'Your PC user cannot open this folder. Choose another folder or check the folder permissions.';

  @override
  String get couldNotFindFolderPc =>
      'Could not find the folder on your PC. It may have been renamed or deleted, or a USB or network drive may not be connected.';

  @override
  String get youCanOnlyShareFolders => 'You can only share folders.';

  @override
  String get pathCannotShared => 'This path cannot be shared.';

  @override
  String get cannotOpenExplorerDesktopCheck =>
      'Cannot open it in Explorer on the desktop.\nCheck that you are signed in to the desktop.';

  @override
  String get couldNotOpenFolderExplorer =>
      'Could not open the folder in Explorer on the desktop.\nPress ‘Open’ again.';

  @override
  String get couldNotCheckWhetherFolder =>
      'Could not check whether the folder opened.\nLook at Explorer on the desktop, and if it did not open, press ‘Open’ again.';

  @override
  String get sharingFolderHasEndedReconnecting =>
      'Sharing of this folder has ended or is reconnecting.';

  @override
  String get tryAgainAfterOtherTask =>
      'Try again after the other task finishes.';

  @override
  String get sharedFoldersNotAvailableWindow =>
      'Shared folders are not available in this window. Close the window and connect again.';

  @override
  String get webclientServiceDesktopOffMissing =>
      'The WebClient service on the desktop is off or missing.\nLet your administrator know.';

  @override
  String get desktopHasNoDriveLetters =>
      'The desktop has no drive letters left. Disconnect unused drives on the desktop, then add the folder again.';

  @override
  String get youNotSignedDesktopSign =>
      'You are not signed in to the desktop. Sign in to Windows, then connect again.';

  @override
  String get couldNotConnectDriveDesktop =>
      'Could not connect the drive on the desktop. Stop sharing and add the folder again.';

  @override
  String get administratorTurnedOffSharedFolder =>
      'Your administrator turned off the shared folder permission.';

  @override
  String get couldNotShareFolder => 'Could not share the folder.';

  @override
  String get youAboutShareWholeDrive =>
      'You are about to share a whole drive.\nPrograms on the desktop can read and change every file on this drive.\nWe recommend choosing only the folders you need.';

  @override
  String get windowsProgramFolderChangingDeleting =>
      'This is a Windows or program folder.\nChanging or deleting its files from the desktop may stop your PC from working properly.';

  @override
  String get youAboutShareWholeAccount =>
      'You are about to share your whole account folder (or AppData).\nSign-in data and program settings may be visible on the desktop.';

  @override
  String get networkFolderIfConnectionDrops =>
      'This is a network folder. If the connection drops, the desktop cannot use it, and files may take a long time to open.';

  @override
  String get foldersMoreThan20000 =>
      'For folders with more than 20,000 files, the desktop shows only the first 20,000. Try sharing smaller folders.';

  @override
  String get untilDesktopRestartsFilesOver =>
      'Until the desktop restarts, files over 50 MB cannot be transferred.';

  @override
  String get singleFileOver4Gb =>
      'A single file over 4 GB cannot be transferred through a shared folder. Use clipboard copy and paste instead.';

  @override
  String get cannotOpenFolderPcCheck =>
      'Cannot open the folder on your PC. Check the USB or network connection and try again.';

  @override
  String get sharedFolderWasDisconnectedConnect =>
      'The shared folder was disconnected. Connect to the desktop again, then choose the folder again.';

  @override
  String get sharedFoldersReconnectingWait =>
      'Reconnecting shared folders. Please wait a moment.';

  @override
  String wasDeletedSoSharingEnded(Object name) {
    return '‘$name’ was deleted, so sharing ended';
  }

  @override
  String get shareFolderDesktopAsDrive =>
      'Share this folder with the desktop as a drive?';

  @override
  String folderCautionYouCanTurn(Object path, Object risk) {
    return 'Folder: $path\n\nCaution: $risk';
  }

  @override
  String get share => 'Share';

  @override
  String get stopSharing => 'Stop sharing?';

  @override
  String programsDesktopHaveFilesFrom(
    Object shownName,
    Object path,
    Object device,
  ) {
    return '‘$shownName’ ($path)\n\nPrograms on the desktop that have files from this folder open may not be able to save. $device keeps the folder and its files.';
  }

  @override
  String get stopSharing2 => 'Stop sharing';

  @override
  String get reconnectingSharedFolders => 'Reconnecting shared folders';

  @override
  String get sharedFoldersDisconnected => 'Shared folders disconnected';

  @override
  String get preparingSharedFolders => 'Preparing shared folders';

  @override
  String get choosingFolder => 'Choosing a folder';

  @override
  String youCanShareUpFolders2(Object driveMaxShares) {
    return 'You can share up to $driveMaxShares folders';
  }

  @override
  String get desktopCanReadChangeFiles =>
      'The desktop can read and change the files in a shared folder.';

  @override
  String shareFoldersFrom(Object device) {
    return 'Folder sharing · $device';
  }

  @override
  String get pcDeviceWord => 'Your PC';

  @override
  String get onlyFoldersYouChooseShared =>
      'Only the folders you choose are shared, for this connection only.\nSharing ends when you disconnect.';

  @override
  String get noSharedFoldersYet => 'No shared folders yet.';

  @override
  String get preparingSharedFolders2 => 'Preparing shared folders…';

  @override
  String get hideDetails => 'Hide details';

  @override
  String get sharingLimits => 'Sharing limits';

  @override
  String get openingFolderTakingWhilePlease =>
      'Opening the folder is taking a while.\nPlease wait a moment.';

  @override
  String chooseFolderShare2(Object device) {
    return '$device: choose a folder to share';
  }

  @override
  String get addFolder => 'Add folder…';

  @override
  String get disconnected3 => 'Disconnected';

  @override
  String get checking2 => 'Checking…';

  @override
  String get connecting2 => 'Connecting…';

  @override
  String connected3(Object letter) {
    return '$letter: connected';
  }

  @override
  String get checkingWhetherOpened => 'Checking whether it opened';

  @override
  String get youCanOpenOnceShared =>
      'You can open it once the shared folder is connected';

  @override
  String get opensSharedFolderExplorerDesktop =>
      'Opens the shared folder in Explorer on the desktop';

  @override
  String get open => 'Open';

  @override
  String get closeRemoteWindow => 'Close remote window';

  @override
  String get encoderApplied => 'Applied';

  @override
  String get couldNotConfirmSaveYet =>
      'Could not confirm the change yet. Check the status again.';

  @override
  String get couldNotStartSavingTry =>
      'Could not start the change. Try again in a moment. Settings were not changed.';

  @override
  String get saveResponseCurrentSettingsNeed =>
      'The reply and the current settings differ. Do not choose again; refresh the status.';

  @override
  String get encoderAuto => 'Automatic (recommended)';

  @override
  String get encoderAutoDetail => 'GPU if there is one, otherwise CPU';

  @override
  String get encoderGpu => 'GPU';

  @override
  String get encoderGpuDetail => 'NVIDIA·AMD·Intel';

  @override
  String get encoderX264 => 'CPU (x264)';

  @override
  String get encoderMft => 'CPU (Windows built-in)';

  @override
  String get encoderWhichGpu => 'GPU to use';

  @override
  String get encoderPickGpu => 'Choose the GPU to use.';

  @override
  String get encoderAdminOnly =>
      'Open Host settings as administrator to change this.';

  @override
  String get encoderReloadedAfterConflict =>
      'It was changed elsewhere, so it was reloaded. Choose again.';

  @override
  String encoderRecentLine(Object label, Object time) {
    return 'Last confirmed: $label ($time)';
  }

  @override
  String get hostEncoder => 'Host encoder';

  @override
  String get chooseMethodNextStreamingConnection =>
      'A choice applies at once and is used from the next streaming connection.\nThe current connection stays and keeps its encoder.\nIf the GPU does not start or its output fails, it switches to CPU.';

  @override
  String get savingChoice => 'Applying';

  @override
  String get checkingCurrentChoiceGpuCandidates =>
      'Checking current choice and GPU candidates';

  @override
  String get hostCannotReportEncoderChoice =>
      'This Host cannot report the encoder choice. Update the Host or refresh.';

  @override
  String savedChoice(Object current) {
    return 'Saved choice: $current';
  }

  @override
  String get triesGpuUsedCaptureEarlier =>
      'Tries the GPU used for capture and earlier confirmed candidates first.\nUses a candidate that is confirmed for the current resolution and H.264 output, and switches to CPU if it fails.';

  @override
  String get couldNotCheckGpuList =>
      'Could not check the GPU list. Check the status again.';

  @override
  String get noConfirmedGpuCandidatesYou =>
      'No confirmed GPU candidates. You can still use CPU.';

  @override
  String get listedCandidateDoesNotMean =>
      'A listed candidate does not mean encoding works. Actual support and output are checked when you connect.';

  @override
  String get discardChanges => 'Discard changes';

  @override
  String get encoderConfirmedRecordedTimeDoes =>
      'The encoder confirmed at the recorded time.\nIt does not show the current connection.';

  @override
  String lastOutputLine(Object screen, Object label, Object time) {
    return 'Last output: screen $screen · $label · $time';
  }

  @override
  String get noOutputRecord => 'Last confirmed: no record yet';

  @override
  String get noOutputRecordMeaning =>
      'No record does not mean the GPU was not used.';

  @override
  String get checkSaveStatus => 'Check save status';

  @override
  String get refreshStatus2 => 'Refresh status';

  @override
  String get gpuChoiceRefersAdapterCurrent =>
      'The GPU choice refers to an adapter in the current Windows. After restarting Windows or changing drivers, you may need to choose again.\nThe CPU default stays on desktops without a GPU.';

  @override
  String get pendingApproval => 'Pending approval';

  @override
  String get cancelingRequest => 'Canceling request';

  @override
  String get available2 => 'Available';

  @override
  String get overview => 'Overview';

  @override
  String get diagnosticReport => 'Diagnostic report';

  @override
  String get devices => 'Devices';

  @override
  String needsAttention(Object titles) {
    return '$titles, needs attention';
  }

  @override
  String get checkingHostStatus => 'Checking Host status';

  @override
  String get waitingWindowsRespond => 'Waiting for Windows to respond.';

  @override
  String get checkEncoderSaveStatusFirst =>
      'Check the encoder save status first.';

  @override
  String get discardEncoderChoice => 'Discard the encoder choice?';

  @override
  String get unsavedEncoderChoiceNotApplied =>
      'An unsaved encoder choice is not applied.';

  @override
  String get discardChanges2 => 'Discard changes';

  @override
  String get savingEncoderChoice => 'Saving encoder choice';

  @override
  String get savingChoiceNextStreamingConnection =>
      'Saving the choice for your next streaming connection.';

  @override
  String get checkEncoderSaveStatusBefore =>
      'Check the encoder save status before closing the window.';

  @override
  String get closeWindowAfterSettingsTask =>
      'Close the window after the settings task finishes.';

  @override
  String get discardUnsavedSettings => 'Discard unsaved settings?';

  @override
  String get serverAddressAccountDetailsEncoder =>
      'The server address, account details and encoder choice you entered are not saved.';

  @override
  String get closeWithoutSaving => 'Close without saving';

  @override
  String get taskStatusChangedSoWindow =>
      'The task status changed, so the window was not closed. Check the status and try again.';

  @override
  String get statusResponseDelayedCheckAgain =>
      'The status response is delayed. Check again in a moment.';

  @override
  String get previousStatusCheckWasCanceled =>
      'The previous status check was canceled.';

  @override
  String get administratorApprovedServerRegistrationComplete =>
      'Your administrator approved it, and server registration is complete. Check the service and virtual display status.';

  @override
  String get checkingWindowsServiceStatus =>
      'Checking Windows and service status.';

  @override
  String get hostServicePreparingVirtualDisplay =>
      'The Host service is preparing the virtual display and server connection.';

  @override
  String get installingVirtualDisplayDriver =>
      'Installing virtual display driver';

  @override
  String get checkingSignatureInstallingDriverScreen =>
      'Checking the signature and installing the driver. The screen may flicker briefly. Do not close this window.';

  @override
  String get installingWinfsp => 'Installing WinFsp';

  @override
  String get installingWinfspMayTake1 =>
      'Installing WinFsp. Do not close this window.';

  @override
  String get couldNotCheckWinfspInstall =>
      'Could not check the WinFsp install result.\nRefresh the status to check.';

  @override
  String get installationFinished => 'Installation finished';

  @override
  String get couldNotFinishInstallation => 'Could not finish the installation';

  @override
  String get continueLabel => 'Continue';

  @override
  String get requestSentWaitingWindowsRespond =>
      'Request sent. Waiting for Windows to respond.';

  @override
  String get couldNotStartTaskOpen =>
      'Could not start the task. Open Host settings again and retry. Settings were not changed.';

  @override
  String get checkingChangedStatus => 'Checking the changed status.';

  @override
  String get statusCheckStoppedRefreshCurrent =>
      'The status check stopped. Refresh the current status.';

  @override
  String get taskRespondedButChangedStatus =>
      'The task responded, but the changed status could not be read. Do not run it again; refresh the status.';

  @override
  String get checkResult => 'Check the result';

  @override
  String get done2 => 'Done';

  @override
  String get refreshStatusCheck => 'Refresh the status to check.';

  @override
  String get passwordsDoNotMatchEnter =>
      'The passwords do not match. Enter them again.';

  @override
  String get checkFormatWindowsAccountPassword =>
      'Check the format of the Windows account or password.';

  @override
  String get checkWindowsAccountPassword =>
      'Check the Windows account and password.';

  @override
  String get windowsAccountLockedItsSign =>
      'The Windows account is locked or its sign-in is restricted. Check the account in Windows.';

  @override
  String get windowsCouldNotCheckAccount =>
      'Windows could not check the account. Try again in a moment.';

  @override
  String get couldNotSaveDeleteWindows =>
      'Could not save or delete the Windows account. Refresh the status to check.';

  @override
  String get couldNotFinishSettingUp =>
      'Could not finish setting up the Windows account. Check the account details and current status.';

  @override
  String get couldNotFinishSettingUp2 =>
      'Could not finish setting up the virtual display.';

  @override
  String get couldNotCheckVirtualDisplay =>
      'Could not check the virtual display cleanup and install result. Check the actual status and the conflict list again.';

  @override
  String get useHttpAddressOpenSettings =>
      'To use an HTTP address, open settings as administrator.';

  @override
  String get testingServerConnection => 'Testing server connection';

  @override
  String get serverChecked => 'Server checked.';

  @override
  String get changeServerConnection => 'Change the server connection?';

  @override
  String serverRequestOnceAdministratorApproves(Object origin) {
    return 'Server to request: $origin\n\nOnce your administrator approves, the current registration moves to the new server. Active connections may drop.';
  }

  @override
  String get requestChange => 'Request change';

  @override
  String get requestingServerConnection => 'Requesting server connection';

  @override
  String get connectionRequestSentApproveAdmin =>
      'Connection request sent. Approve it in the admin console.';

  @override
  String get updateHostCancelRequestSafely =>
      'Update the Host to cancel this request safely.';

  @override
  String get editServerConnect => 'Edit the server to connect to?';

  @override
  String get cancelConnectionRequest => 'Cancel the connection request?';

  @override
  String get cancelsCurrentApprovalRequestSo =>
      'Cancels the current approval request so you can enter the server address again. The existing server connection stays.';

  @override
  String get stopsWaitingAdministratorApprovalExisting =>
      'Stops waiting for administrator approval. The existing server connection stays.';

  @override
  String get cancelEdit => 'Cancel and edit';

  @override
  String get cancelRequest => 'Cancel request';

  @override
  String get cancelingConnectionRequest => 'Canceling connection request';

  @override
  String get connectionRequestWasCanceledCheck =>
      'The connection request was canceled. Check the current status before a new request.';

  @override
  String get hostUpdateNeeded => 'Host update needed';

  @override
  String get startHostService => 'Start the Host service?';

  @override
  String get onceServiceStartsConnectsRegistered =>
      'Once the service starts, it connects to the registered server automatically.';

  @override
  String get startService => 'Start service';

  @override
  String get startingService => 'Starting service';

  @override
  String get serviceHasStartedReconnectsServer =>
      'The service has started. It reconnects to the server in a moment.';

  @override
  String get updateHostRestartSafelyCurrent =>
      'Update the Host to restart it safely. The current service keeps running.';

  @override
  String get restartHostService => 'Restart the Host service?';

  @override
  String get activeConnectionsDropBrieflyYou =>
      'Active connections drop briefly. You can connect again once the service is back.\nWindows and settings stay as they are.';

  @override
  String get restartingService => 'Restarting service';

  @override
  String get serviceHasRestartedReconnectsServer =>
      'The service has restarted. It reconnects to the server in a moment.';

  @override
  String get restoreVirtualDisplay => 'Restore the virtual display?';

  @override
  String get setUpVirtualDisplay => 'Set up the virtual display?';

  @override
  String get restoresInterruptedScreenChangePrevious =>
      'Restores an interrupted screen change to the previous settings. The screen may drop briefly. Run this from the Proxmox console (the PC\'s own screen for a local PC).';

  @override
  String get checksSignedDriverPreparesVirtual =>
      'Checks the signed driver and prepares the virtual display. The screen may drop briefly, or Windows may need a restart. It does not restart automatically.';

  @override
  String get restore => 'Restore';

  @override
  String get settings => 'Settings';

  @override
  String get virtualDisplayRequestDoneCheck =>
      'The virtual display request is done. Check the active monitors and any restart notice.';

  @override
  String get currentSign => 'Current sign-in';

  @override
  String get activeConnections => 'Active connections';

  @override
  String get screenTasksProgress => 'Screen tasks in progress';

  @override
  String get earlierVirtualMonitor => 'Earlier virtual monitor';

  @override
  String get proxmoxConsoleScreen => 'Proxmox console screen';

  @override
  String get logBackup => 'Log backup';

  @override
  String get virtualDisplayService => 'Virtual display service';

  @override
  String get checked => 'Checked';

  @override
  String get needsChecking => 'Needs checking';

  @override
  String get checkConsoleSign => 'Check console sign-in';

  @override
  String get cannotRunFromRdpScreen => 'Cannot run from an RDP screen';

  @override
  String get runFromSignProxmoxConsole =>
      'Run this from a sign-in on the Proxmox console (the PC\'s own screen for a local PC)';

  @override
  String get unlockWindows => 'Unlock Windows';

  @override
  String get serviceNotRunning => 'The service is not running';

  @override
  String get anotherResetProgress => 'Another reset is in progress';

  @override
  String get couldNotCheckServiceResponse =>
      'Could not check the service response';

  @override
  String get noActiveConnections => 'No active connections';

  @override
  String get streamingConnected => 'Streaming connected';

  @override
  String get windowsRdpSignActive => 'A Windows RDP sign-in is active';

  @override
  String get consoleSwitchProgress => 'Console switch in progress';

  @override
  String get changingNumberMonitors => 'Changing the number of monitors';

  @override
  String get automaticRestoreProgress => 'Automatic restore in progress';

  @override
  String get waitingEarlierScreenTaskEnd =>
      'Waiting for the earlier screen task to end';

  @override
  String get resolutionMonitorChangeProgress =>
      'Resolution or monitor change in progress';

  @override
  String get virtualMonitorUse => 'A virtual monitor is in use';

  @override
  String get streamingScreenTaskProgress => 'Streaming screen task in progress';

  @override
  String get noneLeft => 'None left';

  @override
  String get stillPresentDriver => 'Still present in the driver';

  @override
  String get couldNotConnectDriver => 'Could not connect to the driver';

  @override
  String get couldNotReadVirtualDisplay =>
      'Could not read virtual display ownership';

  @override
  String get multitapVirtualDisplayDriverNot =>
      'Multitap virtual display driver not selected';

  @override
  String get onLabel4 => 'On';

  @override
  String get turnedAgain => 'Turned on again';

  @override
  String get anotherScreenNoConsoleId => 'Another screen is on (no console ID)';

  @override
  String get noScreen => 'No screen is on';

  @override
  String get multitapVirtualMonitorStill =>
      'A Multitap virtual monitor is still on';

  @override
  String get windowsRefusedTurnConsoleScreen =>
      'Windows refused to turn on the console screen';

  @override
  String get couldNotConfirmAfterTurning =>
      'Could not confirm after turning on the console screen';

  @override
  String get couldNotReadScreenSetup => 'Could not read the screen setup';

  @override
  String get cannotCheckFromCurrentSign =>
      'Cannot check from the current sign-in';

  @override
  String get movedBackupFolder => 'Moved to the backup folder';

  @override
  String get noLogsCleanUp => 'No logs to clean up';

  @override
  String get couldNotCreateBackupFolder => 'Could not create the backup folder';

  @override
  String get onlyPartlyMoved => 'Only partly moved';

  @override
  String get preparedAgain => 'Prepared again';

  @override
  String get couldNotCheckResetResult =>
      'Could not check the reset result. Refresh the status.';

  @override
  String text18(num moved) {
    String _temp0 = intl.Intl.pluralLogic(
      moved,
      locale: localeName,
      other: '$moved items',
      one: '1 item',
    );
    return ' ($_temp0)';
  }

  @override
  String get text19 => '';

  @override
  String backedUpRecoveryLogsPrepared(Object moved) {
    return 'Backed up the recovery logs$moved and prepared the virtual display service again.\nRefresh to check the virtual display.';
  }

  @override
  String get nothingWasResetLogsScreens =>
      'Nothing was reset.\nLogs and screens are unchanged.\nFix the items below and try again.';

  @override
  String get resetVirtualDisplayRecoveryLogs =>
      'Reset the virtual display recovery logs?';

  @override
  String get movesLogsEarlierStreamingScreens =>
      'Moves logs of earlier streaming screens whose automatic restore did not finish to a backup folder, and prepares the virtual display service again.\n\n• It does not run while a streaming connection or screen change is in progress.\n• It first checks that no earlier Multitap virtual monitor is left in the driver.\n• If the Proxmox console screen is off, it turns only that screen back on. Other screens, drivers and security settings are not changed.\n• Logs are backed up, not deleted. Windows does not restart.\n\nRun this from a Windows sign-in on the Proxmox console (the PC\'s own screen for a local PC).';

  @override
  String get resetLogs => 'Reset logs';

  @override
  String get resettingVirtualDisplayRecoveryLogs =>
      'Resetting virtual display recovery logs';

  @override
  String get virtualDisplayRecoveryLogsWere =>
      'The virtual display recovery logs were reset.';

  @override
  String get saveWorkRestartWindowsThen =>
      'Save your work, restart Windows, then check the screen.';

  @override
  String get anotherUserSignedConsoleTheir =>
      'Another user is signed in on the console. Their sign-in is kept and the switch is put on hold.';

  @override
  String get signStatusChangedRefreshStatus =>
      'The sign-in status changed. Refresh the status and try again.';

  @override
  String get openHostSettingsAsAdministrator =>
      'Open Host settings as administrator.';

  @override
  String get currentSignAlreadyConnectedStreaming =>
      'The current sign-in is already connected to the streaming console. Check whether the virtual display is ready.';

  @override
  String get virtualDisplayDeviceNotReady =>
      'The virtual display device is not ready. Check the device status first.';

  @override
  String get windowsDidNotAllowSign =>
      'Windows did not allow the sign-in switch. Refresh the current status to check.';

  @override
  String get couldNotCheckCurrentSign =>
      'Could not check the current sign-in and console status. Refresh and try again.';

  @override
  String get switchStreamingScreen => 'Switch to the streaming screen?';

  @override
  String get movesCurrentWindowsSignStreaming =>
      'Moves the current Windows sign-in to the streaming screen.\nThe RDP connection may drop.\nAfter switching, connect by streaming from the Multitap client.';

  @override
  String get switchScreen => 'Switch screen';

  @override
  String get switchingWindowsSign => 'Switching Windows sign-in';

  @override
  String get switchRequestedCheckWhetherVirtual =>
      'Switch requested. Check whether the virtual display is ready.';

  @override
  String get windowsSignSwitchConfirmedOnce =>
      'The Windows sign-in switch is confirmed. Once the virtual display is ready, connect by streaming from the client.';

  @override
  String get switchRequestWasReceivedBut =>
      'The switch request was received, but completion could not be confirmed. Refresh the status before switching again.';

  @override
  String get checkingVirtualDisplayConflicts =>
      'Checking virtual display conflicts';

  @override
  String get lookingUpExistingDevicesLeftover =>
      'Looking up existing devices and leftover settings. Nothing is deleted yet.';

  @override
  String get couldNotCheckConflictsRefresh =>
      'Could not check conflicts. Refresh the status and check again.';

  @override
  String get leftoverSetting => 'Leftover setting';

  @override
  String impact(Object kind, Object item, Object location, Object item2) {
    return '$kind · $item\n$location\nImpact: $item2';
  }

  @override
  String get cleanUpCheckedConflictsInstall =>
      'Clean up the checked conflicts and install?';

  @override
  String allRequiredItemsShownCleaned(Object description) {
    return '$description\n\nAll required items shown are cleaned up and the Multitap driver is installed.\nThe virtual display may drop, or Windows may need a restart.\nThe driver store and existing settings files are kept, and Windows does not restart automatically.';
  }

  @override
  String get cleanUpInstall => 'Clean up and install';

  @override
  String get cleaningUpConflictsInstallingMultitap =>
      'Cleaning up conflicts and installing Multitap driver';

  @override
  String get operationTimedOutCheckActual =>
      'The operation timed out. Check the actual state and the conflict list, then try again.';

  @override
  String get devicesSettingsChangedSoCleanup =>
      'Devices or settings changed, so cleanup did not start. Check the conflict list again.';

  @override
  String get deviceRemovalRequestWasSent =>
      'The device removal request was sent, but removal could not be confirmed. Check the device status and the conflict list.';

  @override
  String get onlySomeItemsWereCleaned =>
      'Only some items were cleaned up. Check the conflict list again before acting.';

  @override
  String get conflictsWereCleanedUpBut =>
      'Conflicts were cleaned up, but the driver install did not finish. Check the current status and diagnostics.';

  @override
  String get cleanupInstallDidNotFinish =>
      'Cleanup and install did not finish. Check the current status and the conflict list again.';

  @override
  String get driverInstalledSaveWorkRestart =>
      'Driver installed. Save your work and restart Windows. It does not restart automatically.';

  @override
  String get driverInstalledSignWindowsFrom =>
      'Driver installed. Sign in to Windows from the Proxmox console (or the local PC screen), then finish virtual display setup.';

  @override
  String get driverInstalledCheckNewVirtual =>
      'Driver installed. Check the new virtual display status.';

  @override
  String get enterWindowsAccount => 'Enter the Windows account.';

  @override
  String get enterNewPassword => 'Enter a new password.';

  @override
  String get enterWindowsPassword => 'Enter the Windows password.';

  @override
  String get passwordTooLong => 'The password is too long.';

  @override
  String useLeastCharacters(Object minimum) {
    return 'Use at least $minimum characters.';
  }

  @override
  String get passwordsDoNotMatch => 'Passwords do not match.';

  @override
  String get createNewPassword => 'Create a new password?';

  @override
  String get saveWindowsAccountTurnAuto =>
      'Save the Windows account and turn on auto sign-in?';

  @override
  String get saveWindowsAccount => 'Save the Windows account?';

  @override
  String windowsAccount2(Object username) {
    return 'Windows account: $username';
  }

  @override
  String get accountGetsWindowsPasswordNeeded =>
      'This account gets a Windows password. It is needed from the next sign-in.';

  @override
  String get thenAccountSavedAutoSign =>
      'Then the account is saved and auto sign-in turns on.';

  @override
  String get streamingConnectsWindowsSignedAccount =>
      'Streaming connects to Windows signed in with this account.';

  @override
  String get whenWindowsStartsSignsAccount =>
      'When Windows starts, it signs in with this account automatically. Unlocking is separate.';

  @override
  String get hostTakesOverExistingAuto =>
      'This Host takes over the existing auto sign-in setting. Any password left in the Windows registry is deleted and cannot be restored.';

  @override
  String get passwordStoredOnlyHostsProtected =>
      'The password is stored only in this Host\'s protected storage. Local administrators can access it.';

  @override
  String get windowsAccountPasswordsSecurityPolicies =>
      'Windows account passwords and security policies are not changed.';

  @override
  String get createPassword => 'Create password';

  @override
  String get saveTurn => 'Save and turn on';

  @override
  String get windowsPasswordCreatedNeededFrom =>
      'Windows password: created. It is needed from the next sign-in.';

  @override
  String get windowsPasswordResultNotConfirmed =>
      'Windows password: result not confirmed. Clear the password field and save again to check.';

  @override
  String get windowsPasswordNotCreatedReopen =>
      'Windows password: not created. Reopen Host settings and try again.';

  @override
  String windowsPasswordNotCreated(Object refused) {
    return 'Windows password: not created. $refused';
  }

  @override
  String get windowsAccountNotSaved => 'Windows account: not saved.';

  @override
  String get autoSignNotChanged => 'Auto sign-in: not changed.';

  @override
  String get checkingSavingWindowsAccount =>
      'Checking and saving Windows account';

  @override
  String get windowsAccountCheckedSaved => 'Windows account checked and saved.';

  @override
  String windowsAccountNotSaved2(Object value) {
    return 'Windows account: not saved. $value';
  }

  @override
  String get settingsChangedDuringCheckCheck =>
      'Settings changed during the check. Check the status, then save the account again.';

  @override
  String get notChangedRefreshStatusCheck =>
      'Not changed. Refresh the status to check.';

  @override
  String get windowsAccountSaved => 'Windows account: saved.';

  @override
  String autoSign(Object autologon) {
    return 'Auto sign-in: $autologon';
  }

  @override
  String get creatingWindowsPassword => 'Creating Windows password';

  @override
  String get checkingWindowsAccount => 'Checking Windows account';

  @override
  String get checkingWhetherAccountHasPassword =>
      'Checking whether this account has a password.';

  @override
  String get couldNotConfirmAccountHas =>
      'Could not confirm that the account has no password. Enter the password.';

  @override
  String get accountAlreadyHasPasswordEnter =>
      'This account already has a password. Enter that password.';

  @override
  String get doesNotMeetWindowsPassword =>
      'This does not meet the Windows password rules. Make it longer and mix upper and lower case, numbers and symbols.';

  @override
  String get couldNotConfirmAccountHas2 =>
      'Could not confirm that the account has no password.';

  @override
  String get windowsRefusedChange => 'Windows refused the change.';

  @override
  String get checkPasswordFormat => 'Check the password format.';

  @override
  String get notLocalAccountWindowsEnter =>
      'This is not a local account on this Windows. Enter the password.';

  @override
  String get accountDoesNotExistWindows =>
      'This account does not exist on this Windows. Check the account name.';

  @override
  String get accountBlockedCheckAccountStatus =>
      'This account is blocked. Check the account status in Windows.';

  @override
  String get accountChangedDuringCheckSave =>
      'The account changed during the check. Save again.';

  @override
  String get couldNotCheckAccountTry =>
      'Could not check the account. Try again in a moment.';

  @override
  String get hostDoesNotSupportAuto =>
      'This Host does not support auto sign-in setup. Only the account is saved.';

  @override
  String get noAdministratorRightsSoAuto =>
      'No administrator rights, so auto sign-in is not changed.';

  @override
  String get windowsPolicyPreventsAutoSign =>
      'Windows policy prevents auto sign-in. The policy is not changed.';

  @override
  String get autoSignSettingAlreadyExists =>
      'An auto sign-in setting already exists. This Host version cannot take it over.';

  @override
  String get previousChangeMustRecoveredFirst =>
      'A previous change must be recovered first. Only the account is saved.';

  @override
  String get autoSignCannotChangedRight =>
      'Auto sign-in cannot be changed right now. Check the status.';

  @override
  String get updateHostClearSavedInfo2 =>
      'Update the Host to clear saved info safely.';

  @override
  String get clearWindowsSignInfoSaved =>
      'Clear the Windows sign-in info saved on the Host?';

  @override
  String onlyHostsSavedInfoCleared(Object username) {
    return 'Only the Host\'s saved info for $username is cleared.\nThe Windows user and existing auto sign-in settings stay.';
  }

  @override
  String get clearSavedInfo => 'Clear saved info';

  @override
  String get savedInfoChanged => 'Saved info changed';

  @override
  String get checkNewStatusThenChoose =>
      'Check the new status, then choose again.';

  @override
  String get clearingSignInfoSavedHost => 'Clearing sign-in info saved on Host';

  @override
  String get windowsSignInfoSavedHost =>
      'Windows sign-in info saved on the Host is cleared.';

  @override
  String get checkingVirtualDisplayStatus => 'Checking virtual display status.';

  @override
  String get noVirtualDisplayDriverYet =>
      'No virtual display driver yet. Select Finish installation to install it.';

  @override
  String get screenRecoveryFromLastStreaming =>
      'Screen recovery from the last streaming session has not finished. If this continues, reset the recovery record below.';

  @override
  String get multitapVirtualDisplayAvailableCurrent =>
      'The Multitap virtual display is available in the current Windows sign-in.';

  @override
  String get youSignedViaRdpSwitch =>
      'You are signed in via RDP. Switch to the streaming screen below.';

  @override
  String get servicePreparingVirtualDisplayPlease =>
      'The service is preparing the virtual display. Please wait.';

  @override
  String get previousScreenChangeWasInterrupted =>
      'A previous screen change was interrupted. Run virtual display recovery from the Proxmox console (or the local PC screen).';

  @override
  String get anotherVirtualDisplayDevicePresent =>
      'Another virtual display device is present. That device is not changed. Check existing devices below.';

  @override
  String get signWindowsFromProxmoxConsole =>
      'Sign in to Windows from the Proxmox console (or the local PC screen), then run virtual display setup.';

  @override
  String get runVirtualDisplaySetupPrepare =>
      'Run virtual display setup to prepare a monitor.';

  @override
  String get virtualDisplayDeviceDidNot =>
      'The virtual display device did not start. Apply the setup again and check the device status in Windows.';

  @override
  String get noVirtualDisplayRunVirtual =>
      'No virtual display is on. Run virtual display setup from the Proxmox console (or the local PC screen).';

  @override
  String get virtualDisplaySetupNeedsAdministrator =>
      'Virtual display setup needs administrator rights.';

  @override
  String get windowsCouldNotFinishPreparing =>
      'Windows could not finish preparing the virtual display driver. Copy the diagnostics and send them to your administrator.';

  @override
  String get existingVirtualDisplaySetupNeeds =>
      'The existing virtual display setup needs checking. Copy the diagnostics and send them to your administrator. Existing devices were not changed.';

  @override
  String get virtualDisplayDeviceOwnershipDuplicate =>
      'Virtual display device ownership or a duplicate install needs checking. Devices from other products were not changed.';

  @override
  String get couldNotVerifyDriverFiles =>
      'Could not verify the driver files. Download the Host installer again and run it. Do not change Windows security settings.';

  @override
  String get couldNotVerifyVirtualDisplay =>
      'Could not verify the virtual display config files. Download the Host installer again and run it.';

  @override
  String get couldNotSaveVirtualDisplay =>
      'Could not save the virtual display device settings. Check administrator rights and the device status in Windows.';

  @override
  String get previousScreenChangeNeedsRecovery =>
      'A previous screen change needs recovery. Run virtual display recovery from the Proxmox console (or the local PC screen).';

  @override
  String get hostServiceManagesVirtualDisplay =>
      'The Host service manages this virtual display. Refresh the status to see the current preparation step.';

  @override
  String get couldNotReadVirtualDisplay2 =>
      'Could not read the virtual display status from the service. Check the service status and diagnostics.';

  @override
  String get couldNotCheckCurrentDisplay =>
      'Could not check the current display layout. Refresh the status; if this continues, check diagnostics.';

  @override
  String get youSignedViaRdpSwitch2 =>
      'You are signed in via RDP. Switch to the streaming screen, or sign in with the same Windows account from the Proxmox console (or the local PC screen).';

  @override
  String get couldNotCheckVirtualDisplay2 =>
      'Could not check the virtual display status. Copy the diagnostics and send them to your administrator.';

  @override
  String get hostInfo => 'Host info';

  @override
  String get hostToolsVersion => 'Host tools version';

  @override
  String get availableWhenOperationFinishes =>
      'Available when the operation finishes.';

  @override
  String get availableAfterRegisteringServer =>
      'Available after registering with the server.';

  @override
  String update2(Object newer) {
    return 'Update to $newer';
  }

  @override
  String get connectSoundOutputDeviceTurn =>
      'Connect a sound output device or turn one on in Windows.';

  @override
  String get proxmoxConsoleLocalPcScreen =>
      'the Proxmox console (or the local PC screen)';

  @override
  String get localPcScreen => 'the local PC screen';

  @override
  String get consoleLocalPcScreen => 'Console (or the local PC screen)';

  @override
  String get proxmoxConsole => 'the Proxmox console';

  @override
  String get checkVirtualDisplay2 => 'Check virtual display';

  @override
  String get notRegistered2 => 'Not registered';

  @override
  String get desktop4 => 'This desktop';

  @override
  String get readyStream => 'Ready to stream';

  @override
  String get checkingStatus2 => 'Checking status';

  @override
  String get checkingHostStatus2 => 'Checking Host status';

  @override
  String get notRegisteredServerYet => 'Not registered with a server yet';

  @override
  String get components => 'Components';

  @override
  String get checking3 => 'Checking';

  @override
  String get signedOutLocked => 'Signed out or locked';

  @override
  String accountAutoSign(Object accountLabel) {
    return '$accountLabel account · auto sign-in on';
  }

  @override
  String get noSavedWindowsAccount => 'No saved Windows account';

  @override
  String get autoSignAccountDiffersFrom =>
      'Auto sign-in account differs from the saved account';

  @override
  String ready4(num monitors) {
    String _temp0 = intl.Intl.pluralLogic(
      monitors,
      locale: localeName,
      other: '$monitors virtual monitors',
      one: '1 virtual monitor',
    );
    return '$_temp0 · ready';
  }

  @override
  String get noDriverYet => 'No driver yet';

  @override
  String get screenRecoveryNeeded => 'Screen recovery needed';

  @override
  String get mustSwitchStreamingScreen => 'Must switch to the streaming screen';

  @override
  String get servicePreparing => 'The service is preparing';

  @override
  String get virtualDisplayNeedsChecking => 'Virtual display needs checking';

  @override
  String get whileStreamingOnlyVirtualDisplay =>
      'While streaming, only the virtual display is on. When it ends, the original screens come back.';

  @override
  String get couldNotReadHostStatus =>
      'Could not read the Host status. Refresh to check again.';

  @override
  String get couldNotReadHostService =>
      'Could not read the Host service status. Refresh to check again.';

  @override
  String get hostServiceNotInstalledRun =>
      'The Host service is not installed. Run the Host installer again.';

  @override
  String get hostServiceChangingStateRefresh =>
      'The Host service is changing state. Refresh in a moment.';

  @override
  String get hostServiceOffSoCannot =>
      'The Host service is off, so it cannot connect to the server. Start the service and it reconnects automatically.';

  @override
  String get screenRecoveryFromLastStreaming2 =>
      'Screen recovery from the last streaming session has not finished. Check it under Virtual display.';

  @override
  String checkVirtualDisplayStatusFrom(Object restart) {
    return 'Check the virtual display status from the Proxmox console (or the local PC screen) and run recovery.$restart';
  }

  @override
  String get viewVirtualDisplay => 'View virtual display';

  @override
  String get notRegisteredServerYetRequest =>
      'Not registered with a server yet. Request a connection using the server address from your administrator.';

  @override
  String get connect4 => 'Connect';

  @override
  String get cancelingConnectionRequestWaitingServer =>
      'Canceling the connection request. Waiting for the server to confirm.';

  @override
  String get requestExpiredRefreshStatusThen =>
      'The request expired. Refresh the status, then request again.';

  @override
  String get checkingRegistrationResultApprovedConnection =>
      'Checking the registration result of the approved connection.';

  @override
  String get adminConsoleUnderPendingApprovals =>
      'In the admin console, under pending approvals, check that the confirmation code matches, then approve.';

  @override
  String get approveConnectionRequestAdminConsole =>
      'Approve this connection request in the admin console, under pending approvals.';

  @override
  String get viewRequest => 'View request';

  @override
  String get connectionDiagnostics => 'Connection diagnostics';

  @override
  String get fromLocalPcScreen => 'from the local PC screen';

  @override
  String get fromConsoleClient => 'from Console in the client';

  @override
  String signRestartDesktopSignAutomatically(Object where) {
    return 'Sign in $where, or restart the desktop to sign in automatically.';
  }

  @override
  String sign2(Object where) {
    return 'Sign in $where.';
  }

  @override
  String windowsSignedOutLocked(Object where) {
    return 'Windows is signed out or locked. $where';
  }

  @override
  String get signSettings => 'Sign-in settings';

  @override
  String get currentWindowsSignNotStreaming =>
      'The current Windows sign-in is not on the streaming screen. Switch to the streaming screen under Virtual display.';

  @override
  String get autoSignOffSaveWindows =>
      'Auto sign-in is off. Save the Windows account so you can connect right after a restart.';

  @override
  String get autoSignNeedsCheckingSave =>
      'Auto sign-in needs checking. Save the account again under Windows sign-in.';

  @override
  String get someDeviceFeaturesNotReady =>
      'Some device features are not ready to use. Check them under Devices.';

  @override
  String get viewDevices => 'View devices';

  @override
  String get openAdministratorSettingsWindow =>
      'Open the administrator settings window?';

  @override
  String get youCanChangeSettingsNew =>
      'You can change settings in a new window approved by a Windows administrator.';

  @override
  String get openNewWindow => 'Open new window';

  @override
  String get waitingWindowsAdministratorApproval =>
      'Waiting for Windows administrator approval';

  @override
  String get administratorSettingsWindowOpenContinue =>
      'The administrator settings window is open. Continue in the new window.';

  @override
  String get settingsAsAdministrator => 'Settings as administrator';

  @override
  String get changingSettingsNeedsAdministratorRights =>
      'Changing settings needs administrator rights.';

  @override
  String get administratorRejectedRequestCheckServer =>
      'The administrator rejected the request. Check the server address, then request again.';

  @override
  String get connectionRequestExpiredCheckServer =>
      'The connection request expired. Check the server, then request again.';

  @override
  String get hostCopiedRegistrationRequestAgain =>
      'This Windows looks like a copy of another registered desktop. Request a new connection for this desktop.';

  @override
  String get connectionRequestCanceled => 'Connection request canceled.';

  @override
  String get checkServerAddress2 => 'Check server address';

  @override
  String get checkApprovalWaitTime => 'Check approval wait time';

  @override
  String get checkingServerRegistration => 'Checking server registration';

  @override
  String get awaitingAdministratorApproval => 'Awaiting administrator approval';

  @override
  String get confirmationCode => 'Confirmation code';

  @override
  String get adminConsoleMustShowSame =>
      'The admin console must show this same code. If the codes differ, do not approve.';

  @override
  String get waitingServerConfirmCancellationAfter =>
      'Waiting for the server to confirm the cancellation. After that, you can request a connection again.';

  @override
  String get adminConsoleGoDesktopsPending =>
      'In the admin console, go to Desktops → pending approvals, check that the confirmation code matches, then approve.';

  @override
  String get adminConsoleApproveConnectionRequest =>
      'In the admin console, approve this connection request under Desktops → pending approvals.';

  @override
  String get evenIfYouCloseWindow =>
      'Even if you close this window, the Windows service keeps checking the connection request.';

  @override
  String get startWindowsServiceKeepChecking =>
      'Start the Windows service to keep checking for approval.';

  @override
  String approvalValid(Object expiryText) {
    return 'Approval valid · $expiryText';
  }

  @override
  String get couldNotCheckApprovalStatus =>
      'Could not check the approval status. Checking again shortly.';

  @override
  String get requestAlreadyApprovedCheckingServer =>
      'This request is already approved. Checking the server registration result.';

  @override
  String get editServer => 'Edit server';

  @override
  String get serverConnection3 => 'Server connection';

  @override
  String get connectServer => 'Connect to server';

  @override
  String get registrationKeptAfterUpdates =>
      'Registration is kept after updates.';

  @override
  String get checkServerAddressRequestConnection =>
      'Check the server address and request a connection. An administrator approves it.';

  @override
  String get desktopId => 'Desktop ID';

  @override
  String get connectionStatus => 'Connection status';

  @override
  String get changeServerReRegister => 'Change server or re-register';

  @override
  String get connectionChecked => 'Connection checked';

  @override
  String get testServerConnection => 'Test server connection';

  @override
  String get nowSendConnectionRequest => 'Now send the connection request.';

  @override
  String get firstFinishServerConnectionTest =>
      'First finish the server connection test.';

  @override
  String text20(num monitorsLength) {
    String _temp0 = intl.Intl.pluralLogic(
      monitorsLength,
      locale: localeName,
      other: '$monitorsLength virtual monitors on',
      one: '1 virtual monitor on',
    );
    return '$_temp0';
  }

  @override
  String get switchStreamingScreen2 => 'Switch to streaming screen';

  @override
  String monitor2(Object index) {
    return 'Monitor $index';
  }

  @override
  String get noMultitapVirtualMonitor => 'No Multitap virtual monitor is on.';

  @override
  String get switchScreenAfterWindowsRestart =>
      'Switch screen after Windows restart';

  @override
  String get refreshVirtualDisplayStatus => 'Refresh virtual display status';

  @override
  String get resetVirtualDisplayRecoveryRecord =>
      'Reset virtual display recovery record';

  @override
  String get checkAfterWindowsRestart => 'Check after Windows restart';

  @override
  String get recoverVirtualDisplay => 'Recover virtual display';

  @override
  String get setUpVirtualDisplayFrom =>
      'Set up virtual display from the Proxmox console';

  @override
  String get setUpVirtualDisplay2 => 'Set up virtual display';

  @override
  String get checkExistingDevicesLeftoverSettings =>
      'Check existing devices and leftover settings';

  @override
  String get onceDisplayReadyResolutionFollows =>
      'Once the display is ready, resolution follows the client window size.';

  @override
  String get setNumberMonitors12 =>
      'Set the number of monitors (1–2) and the resolution in the connected client.\nStreaming uses only the Multitap virtual display.';

  @override
  String get conflictsFound => 'Conflicts found';

  @override
  String devicesLeftoverSettingsNothingDeleted(
    Object devices,
    Object configurations,
  ) {
    return 'Devices: $devices · leftover settings: $configurations. Nothing deleted yet.';
  }

  @override
  String get noConflictsCleanUp => 'No conflicts to clean up.';

  @override
  String get virtualDisplayDeviceRequired =>
      'Virtual display device · required';

  @override
  String get leftoverSettingsRequired => 'Leftover settings · required';

  @override
  String impact2(Object items) {
    return 'Impact: $items';
  }

  @override
  String get requiredItemsAboveCleanedUp =>
      'The required items above are cleaned up together. The Windows driver store and existing settings files are kept.';

  @override
  String get cleanUpCheckedConflictsInstall2 =>
      'Clean up checked conflicts and install';

  @override
  String get couldNotVerifyDriverFiles2 =>
      'Could not verify the driver files. Download the Host installer again, run it, then check.';

  @override
  String get couldNotSafelyVerifyVirtual =>
      'Could not safely verify the virtual display config files. Download the Host installer again, run it, then check.';

  @override
  String get restartWindowsThenCheckConflict =>
      'Restart Windows, then check the conflict list again.';

  @override
  String get signWindowsFromProxmoxConsole2 =>
      'Sign in to Windows from the Proxmox console (or the local PC screen), then check again.';

  @override
  String get firstRecoverPreviousScreenChange =>
      'First recover the previous screen change, then check the conflict list again.';

  @override
  String get automaticCleanupCannotProceedCopy =>
      'Automatic cleanup cannot proceed. Copy the diagnostics and send them to your administrator.';

  @override
  String get lastCleanupInstallResult => 'Last cleanup and install result';

  @override
  String get conflictCleanup => 'Conflict cleanup';

  @override
  String get devicesRequestedRemoval => 'Devices requested for removal';

  @override
  String text21(Object result) {
    return '$result';
  }

  @override
  String get devicesConfirmedRemoved => 'Devices confirmed removed';

  @override
  String get driverInstall => 'Driver install';

  @override
  String get notCompleted => 'Not completed';

  @override
  String get previousSettingsRestored => 'Previous settings restored.';

  @override
  String get someItemsRemainCheckConflict =>
      'Some items remain. Check the conflict list again before retrying.';

  @override
  String get deviceRemovalNotConfirmedYet =>
      'Device removal not confirmed yet.';

  @override
  String get windowsNeedsRestartDoesNot =>
      'Windows needs a restart. It does not restart automatically.';

  @override
  String get offTurnsWhenYouSave => 'Off · turns on when you save the account';

  @override
  String get setAppliesFromNextWindows =>
      'Set · applies from the next Windows start';

  @override
  String get windowsSignSetAccountConfirmed =>
      'Windows sign-in with the set account confirmed';

  @override
  String get accountCheckNeededSaveAccount =>
      'Account check needed · save the account info again';

  @override
  String get windowsHasExistingAutoSign =>
      'Windows has an existing auto sign-in setting';

  @override
  String get windowsPolicyPreventsTurning =>
      'Windows policy prevents turning it on';

  @override
  String get autoSignOffRecoveryNeeded => 'Auto sign-in off · recovery needed';

  @override
  String get checkChangeResultRecoveryStatus =>
      'Check the change result and recovery status';

  @override
  String get couldNotCheckCurrentSetting =>
      'Could not check the current setting';

  @override
  String get hostDoesNotSupportAuto2 =>
      'This Host does not support auto sign-in setup';

  @override
  String get windowsAccountPasswordWrongNot =>
      'The Windows account or password is wrong. It is not retried automatically.';

  @override
  String get enterAccountPasswordPasswordConfirmation =>
      'Enter the account, password and password confirmation again. Empty passwords and PINs are not saved.';

  @override
  String get onlyLocalWindowsAccountsPassword =>
      'Only local Windows accounts with a password are supported.';

  @override
  String get accountChangedRestrictedCheckAccount =>
      'The account changed or is restricted. Check the account status in Windows.';

  @override
  String get windowsPolicyBlocksAutoSign =>
      'Windows policy blocks auto sign-in. The policy is not changed.';

  @override
  String get windowsSettingsChangedDuringCheck =>
      'Windows settings changed during the check. Check the current status again.';

  @override
  String get settingsChangedDuringCheckRefresh =>
      'Settings changed during the check. Refresh, then review the changes again.';

  @override
  String get anotherSettingsTaskRunningCheck =>
      'Another settings task is running. Check the result in a moment.';

  @override
  String get autoSignChangeStoppedMidway =>
      'The auto sign-in change stopped midway. Recover the settings Multitap changed, below.';

  @override
  String get couldNotConfirmResultCheck =>
      'Could not confirm the result. Check it before applying again.';

  @override
  String get recoverSettingsMultitapChanged =>
      'Recover the settings Multitap changed?';

  @override
  String get onlyAutoSignSettingsMultitap =>
      'Only the auto sign-in settings Multitap changed are restored to their original values.\nValues changed elsewhere are not overwritten.\nNo restart, sign-out or unlock happens.';

  @override
  String get recoverSettings => 'Recover settings';

  @override
  String get settingUpWindowsAutoSign => 'Setting up Windows auto sign-in';

  @override
  String get checkingCurrentSettingsAccountPermissions =>
      'Checking the current settings and account permissions.';

  @override
  String get couldNotPrepareWindowProtection =>
      'Could not prepare window protection for the task. Settings were not changed.';

  @override
  String get currentSettingsPermissionsChangedRefresh =>
      'Current settings or permissions changed. Refresh, then check again.';

  @override
  String get couldNotVerifyWindowProtection =>
      'Could not verify window protection for the task. Settings were not changed.';

  @override
  String get couldNotChangeSettingsRefresh =>
      'Could not change the settings. Refresh the status to check.';

  @override
  String get requestResultNotConfirmedYet =>
      'The request result is not confirmed yet. Do not apply again; check the result.';

  @override
  String get changeStoppedMidwayRecoverSettings =>
      'The change stopped midway. Recover the settings Multitap changed under Windows sign-in.';

  @override
  String get couldNotConfirmWhetherRequest =>
      'Could not confirm whether this request finished. Retry only the result check.';

  @override
  String get turnedAppliesFromNextWindows =>
      'Turned on. Applies from the next Windows start.';

  @override
  String get settingsMultitapChangedRecoveredAuto =>
      'The settings Multitap changed are recovered. Auto sign-in is off. Save the account again to turn it on.';

  @override
  String get requestFinishedButCurrentSetting =>
      'The request finished, but the current setting could not be checked. Check the status under Windows sign-in.';

  @override
  String get resultNotConfirmedYetSame =>
      'Result not confirmed yet. The same request is not applied again.';

  @override
  String get whenYouSaveAccountHost =>
      'When you save the account, this Host takes over the existing setting and turns it on.';

  @override
  String get hostVersionCannotTakeOver =>
      'This Host version cannot take over the existing setting.';

  @override
  String get onceWindowsSignsAccountWhen =>
      'Once on, Windows signs in with this account when it starts.';

  @override
  String get windowsSignsAccountAutomaticallyWhen =>
      'Windows signs in with this account automatically when it starts.';

  @override
  String get refreshCheckCurrentSetting =>
      'Refresh to check the current setting.';

  @override
  String get windowsAccount3 => 'Windows account';

  @override
  String get streamingAutoSignUseAccount =>
      'Streaming and auto sign-in use this account.\nIt is separate from your Multitap sign-in account.';

  @override
  String get savedAccount => 'Saved account';

  @override
  String get autoSignAccount => 'Auto sign-in account';

  @override
  String get windowsSetting => 'Windows setting';

  @override
  String get confirmed => 'On (confirmed)';

  @override
  String get offConfirmed => 'Off (confirmed)';

  @override
  String get autoSignAccountDiffersFrom2 =>
      'The auto sign-in account differs from the saved account. Save the account again.';

  @override
  String get updateHostThenCheckAgain => 'Update the Host, then check again.';

  @override
  String get savingAlsoSwitchesAutoSign =>
      'Saving also switches auto sign-in to this account.';

  @override
  String get enterPasswordOnceMoreTurn =>
      'Enter the password once more to turn on auto sign-in.';

  @override
  String get turnAutoSign => 'Turn on auto sign-in';

  @override
  String get changeAccount => 'Change account';

  @override
  String get clearSavedSignInfo => 'Clear saved sign-in info';

  @override
  String get unclearWhetherAutoSignFinished =>
      'It is unclear whether auto sign-in finished. The same change is not run again; only the result is checked.';

  @override
  String get checkRequestResult => 'Check request result';

  @override
  String get recoverSettingsMultitapChanged2 =>
      'Recover settings Multitap changed';

  @override
  String get savedWindowsAccountNeedsChecking =>
      'The saved Windows account needs checking. Review the account info and save it again.';

  @override
  String get currentWindowsSign => 'Current Windows sign-in';

  @override
  String get signedNotLocked => 'Signed in and not locked';

  @override
  String get checkSignLockState => 'Check the sign-in or lock state';

  @override
  String get signChecked => 'Sign-in checked at';

  @override
  String get ifWindowsSignsOutLocks =>
      'If Windows signs out or locks, sign in from Console (or the local PC screen).\nA restart signs in automatically.';

  @override
  String get localUserNameComputerName =>
      'Local user name or computer name\\user name · account password, not PIN';

  @override
  String get createNewPassword2 => 'Create a new password';

  @override
  String leastCharacters(Object minimumLength) {
    return 'at least $minimumLength characters, ';
  }

  @override
  String accountHasNoWindowsPassword(Object setup) {
    return 'This account has no Windows password.\nCreate one: ${setup}matching the Windows password rules.';
  }

  @override
  String get newWindowsPassword => 'New Windows password';

  @override
  String get windowsPassword => 'Windows password';

  @override
  String get leaveBlankSaveIfThere =>
      'Leave blank and save if there is no password.';

  @override
  String get confirmPassword => 'Confirm password';

  @override
  String get createPasswordSave => 'Create password and save';

  @override
  String get saveTurnAutoSign => 'Save and turn on auto sign-in';

  @override
  String get checkSaveAccount => 'Check and save account';

  @override
  String get discardWindowsAccountInfoYou =>
      'Discard the Windows account info you entered?';

  @override
  String get savedWindowsAccountKept => 'The saved Windows account is kept.';

  @override
  String get serviceStopping => 'Service stopping';

  @override
  String get servicePaused => 'Service paused';

  @override
  String get serviceChangingState => 'Service changing state';

  @override
  String get serviceRunning => 'Service running';

  @override
  String get serviceNotInstalled => 'Service not installed';

  @override
  String get serviceStopped => 'Service stopped';

  @override
  String get serviceQueryNeeded => 'Service query needed';

  @override
  String get serviceStartNeeded => 'Service start needed';

  @override
  String get checkServiceStatus => 'Check service status';

  @override
  String get connectionStatusDelayed => 'Connection status delayed';

  @override
  String get serverConnectionConfirmed => 'Server connection confirmed';

  @override
  String get connectingServer2 => 'Connecting to server';

  @override
  String get waitingServerResponse => 'Waiting for server response';

  @override
  String get reconnectingServer => 'Reconnecting to server';

  @override
  String get checkServerResponse => 'Check server response';

  @override
  String get checkServerRegistration => 'Check server registration';

  @override
  String get checkServerConnection => 'Check server connection';

  @override
  String get runHostInstallerAgainInstall2 =>
      'Run the Host installer again to install the Windows service.';

  @override
  String get hostServiceOffStartService =>
      'The Host service is off. Start the service and it connects to the server automatically.';

  @override
  String get couldNotReadServiceStatus =>
      'Could not read the service status. Refresh the status.';

  @override
  String get couldNotReadLatestStatus =>
      'Could not read the latest status. Checking again shortly.';

  @override
  String get connectedServerYouCanConnect =>
      'Connected to the server. You can connect to this desktop from a client.';

  @override
  String get connectedServerCheckVirtualDisplay =>
      'Connected to the server. Check the virtual display status.';

  @override
  String get serverCouldNotConfirmHosts =>
      'The server could not confirm this Host\'s registration. Request a connection again under Server connection.';

  @override
  String get couldNotVerifyServerCertificate =>
      'Could not verify the server certificate. Check the HTTPS address and the certificate.';

  @override
  String get couldNotConnectServerCheck =>
      'Could not connect to the server. Check the Windows network and the server address.';

  @override
  String get serverNotRespondingTellAdministrator =>
      'The server is not responding. Tell your administrator.';

  @override
  String get hostServerVersionsDoNot =>
      'Host and server versions do not match. Update both.';

  @override
  String get couldNotConfirmRecentServer =>
      'Could not confirm a recent server response. The service reconnects automatically.';

  @override
  String get serverConnectionDroppedServiceReconnects =>
      'The server connection dropped. The service reconnects automatically.';

  @override
  String get serviceConnectingServerPleaseWait =>
      'The service is connecting to the server. Please wait.';

  @override
  String get connectedServerWaitingResponse =>
      'Connected to the server and waiting for a response.';

  @override
  String get couldNotConfirmRecentServer2 =>
      'Could not confirm a recent server response. If this continues, restart the service from the diagnostic report.';

  @override
  String get couldNotCheckServerConnection =>
      'Could not check the server connection. If this continues, restart the service from the diagnostic report.';

  @override
  String get noConfirmedResponse => 'No confirmed response';

  @override
  String sAgo(Object age) {
    return '${age}s ago';
  }

  @override
  String get diagnosticsCopied => 'Diagnostics copied.';

  @override
  String get couldNotCopyDiagnosticsTry =>
      'Could not copy diagnostics. Try again.';

  @override
  String get queryNeeded => 'Query needed';

  @override
  String get restartService => 'Restart service';

  @override
  String get summary => 'Summary';

  @override
  String get diagnosticsSentServerAutomaticallyPasswords =>
      'Diagnostics are sent to the server automatically.\nPasswords, credentials and clipboard contents are not sent.';

  @override
  String get copyDiagnostics => 'Copy diagnostics';

  @override
  String get hostVersion2 => 'Host version';

  @override
  String get service => 'Service';

  @override
  String get serverRegistrationNeeded => 'Server registration needed';

  @override
  String get signed => 'Signed in';

  @override
  String get checkSign => 'Check sign-in';

  @override
  String ready5(num outputs) {
    String _temp0 = intl.Intl.pluralLogic(
      outputs,
      locale: localeName,
      other: '$outputs monitors',
      one: '1 monitor',
    );
    return 'Ready · $_temp0';
  }

  @override
  String checkNeeded(num outputs) {
    String _temp0 = intl.Intl.pluralLogic(
      outputs,
      locale: localeName,
      other: '$outputs monitors',
      one: '1 monitor',
    );
    return 'Check needed · $_temp0';
  }

  @override
  String get serviceQueryError2 => 'Service query error';

  @override
  String windowsError(Object s) {
    return 'Windows error $s';
  }

  @override
  String get serviceInstall2 => 'Service install';

  @override
  String get sinceServerResponse => 'Since server response';

  @override
  String get lastConnected => 'Last connected at';

  @override
  String get lastResponse => 'Last response at';

  @override
  String get connectionGuidance => 'Connection guidance';

  @override
  String get virtualDisplayDriver2 => 'Virtual display driver';

  @override
  String get virtualDisplayDevice2 => 'Virtual display device';

  @override
  String get virtualMonitors => 'Virtual monitors on';

  @override
  String text22(Object outputs) {
    return '$outputs';
  }

  @override
  String get screenRecovery => 'Screen recovery';

  @override
  String get windowsRestart2 => 'Windows restart';

  @override
  String get couldNotReadDeviceStatus =>
      'Could not read the device status.\nRefresh the status.';

  @override
  String get permissionsTurnedPerUserDesktop =>
      'Permissions are turned on per user and desktop in the admin console. This screen only shows whether this desktop is ready.\nSmart cards and serial devices connect over USB.\nMicrophones and cameras cannot be connected.';

  @override
  String get installWinfsp3 => 'Install WinFsp?';

  @override
  String get installsSharedFolderDriverWinfsp =>
      'Installs the shared folder driver (WinFsp).\nIf Windows needs a restart, you are told. It does not restart automatically.';

  @override
  String get switchUsbConnectionMultitap =>
      'Switch USB connection to Multitap?';

  @override
  String get usbConnectionFromOtherPrograms =>
      'USB connection from other programs stops working.\nConnected USB devices are also disconnected.\nIf Windows needs a restart, you are told. It does not restart automatically.';

  @override
  String get switchLabel => 'Switch';

  @override
  String get switchingUsbConnection => 'Switching USB connection';

  @override
  String get switchingUsbConnectionMultitapDo =>
      'Switching USB connection to Multitap. Do not close the window.';

  @override
  String get startingPrintSpooler => 'Starting print spooler';

  @override
  String get startingWindowsPrintSpooler =>
      'Starting the Windows print spooler.';

  @override
  String get openSettingsAsAdministrator => 'Open settings as administrator.';

  @override
  String get cleanUp => 'Clean up';

  @override
  String remove(num extrasLength) {
    String _temp0 = intl.Intl.pluralLogic(
      extrasLength,
      locale: localeName,
      other: '$extrasLength leftover virtual display devices',
      one: '1 leftover virtual display device',
    );
    return 'Remove $_temp0?';
  }

  @override
  String get devicesWithoutMultitapRecordMay =>
      '\nDevices without a Multitap record may be used by another program. That program\'s virtual display may disappear.';

  @override
  String adapterUseDevicesFromOther(Object list, Object adapters) {
    return 'The adapter in use and devices from other vendors are left alone.\n$list$adapters\nThe screen may flicker briefly. Windows does not restart automatically.';
  }

  @override
  String get cleaningUpVirtualDisplayDevices =>
      'Cleaning up virtual display devices';

  @override
  String get removingOnlyCheckedDevicesOne =>
      'Removing only the checked devices, one by one.';

  @override
  String get allow => 'Allow';

  @override
  String get checkResult2 => 'Check the result';

  @override
  String get waitExpired => 'Wait expired';

  @override
  String get progress2 => 'In progress';

  @override
  String get installerCouldNotFinishUpdate =>
      'The installer could not finish the update.\nThis error code alone does not tell the exact cause.';

  @override
  String get hostUpdateDiagnostics => 'Host update diagnostics';

  @override
  String get currentVersion => 'Current version';

  @override
  String get unknown4 => 'Unknown';

  @override
  String get requestedVersion => 'Requested version';

  @override
  String get result => 'Result';

  @override
  String get errorCode2 => 'Error code';

  @override
  String get checked2 => 'Checked at';

  @override
  String get localPc3 => 'On the local PC,';

  @override
  String get openDesktopConsole => 'Open the desktop in Console and';

  @override
  String checkInstallStatusHostSettings(Object machine) {
    return '$machine check the install status in Host settings → Diagnostics.\nAdministrators can see the result for the same time in the server activity log.';
  }

  @override
  String get updateHost => 'Update Host?';

  @override
  String current4(Object current) {
    return 'Current $current';
  }

  @override
  String newVersion5(Object latest) {
    return 'New version $latest';
  }

  @override
  String get earlierChanges => 'Earlier changes';

  @override
  String get couldNotLoadChangeList =>
      'Could not load the change list. The update file is checked against the server\'s release info.';

  @override
  String hostDownloadsFromServerInstalls(Object kind) {
    return 'Host downloads it from this server and installs it automatically.\nThe $kind settings stay as they are, and Windows does not restart.';
  }

  @override
  String fileSizeSha256Checked(Object kind) {
    return 'The file size and SHA-256 are checked before installing.\nIf something must be confirmed on the $kind itself, like a driver, it is not installed and you are told.';
  }

  @override
  String someoneConnectedNowUpdateWhen(Object kind) {
    return 'Someone is connected to this $kind now.\n‘Update when done’ starts after the connection ends.\n‘Update now’ disconnects and starts right away.';
  }

  @override
  String get updateWhenDone => 'Update when done';

  @override
  String get updateNow => 'Update now';

  @override
  String get category => 'Category';

  @override
  String get noSavedScreen => 'No saved screen';

  @override
  String select(Object machineName) {
    return '$machineName · Select';
  }

  @override
  String get screenPreviewOff => 'Screen preview off';

  @override
  String get manageVm => 'Manage VM';

  @override
  String get manageVmHostUpdateAvailable => 'Manage VM · Host update available';

  @override
  String get cards => 'Cards';

  @override
  String get list => 'List';

  @override
  String get mini => 'Mini';

  @override
  String get explorer => 'Explorer';

  @override
  String get monitorWall => 'Monitor wall';

  @override
  String get dock => 'Dock';

  @override
  String get recoveryCode => 'Recovery code';

  @override
  String get n6DigitCodeFromAuthenticator =>
      '6-digit code from your authenticator app';

  @override
  String get checkingSetupStatus => 'Checking the setup status.';

  @override
  String get couldNotCheckSetupStatus => 'Could not check the setup status.';

  @override
  String get twoStepVerification2 => 'Two-step verification is on.';

  @override
  String get twoStepVerificationOff => 'Two-step verification is off.';

  @override
  String text23(num recoveryCodesRemaining) {
    String _temp0 = intl.Intl.pluralLogic(
      recoveryCodesRemaining,
      locale: localeName,
      other: '$recoveryCodesRemaining recovery codes left',
      one: '1 recovery code left',
    );
    return '$_temp0';
  }

  @override
  String get changingSignsYouOutAll2 =>
      'Changing this signs you out on all devices.';

  @override
  String get setUpAuthenticatorApp => 'Set up authenticator app';

  @override
  String get makeNewRecoveryCodes => 'Make new recovery codes';

  @override
  String get turnOffTwoStepVerification => 'Turn off two-step verification';

  @override
  String get n13VerifyItsYou => '1/3 · Verify it\'s you';

  @override
  String get verifyItsYou => 'Verify it\'s you';

  @override
  String get enterNextCodeFromAuthenticator =>
      'Enter the next code from your authenticator app instead of one you already used.';

  @override
  String get useAuthenticatorCode => 'Use authenticator code';

  @override
  String get useRecoveryCode => 'Use recovery code';

  @override
  String get makingNewCodesStopsOld =>
      'Making new codes stops the old recovery codes from working.';

  @override
  String get whenChangeDoneYouSigned =>
      'When the change is done, you are signed out on all devices.';

  @override
  String get n23LinkAuthenticatorApp => '2/3 · Link authenticator app';

  @override
  String get n33SaveRecoveryCodes => '3/3 · Save recovery codes';

  @override
  String get scanQrCodeAuthenticatorApp =>
      'Scan the QR code with the authenticator app on another device, or enter the setup key yourself.';

  @override
  String get hideQrSetupKey => 'Hide QR and setup key';

  @override
  String get showQrSetupKey => 'Show QR and setup key';

  @override
  String timeLeftSYouCan(Object remainingInSeconds) {
    return 'Time left $remainingInSeconds s\nYou can see this key only on this screen.';
  }

  @override
  String get setupKeyDisplayTimeEnded =>
      'The setup key display time ended, so the key was cleared.';

  @override
  String get waitingResultSetupRequestAlready =>
      'Waiting for the result of the setup request already sent. It is not sent again.';

  @override
  String get changeRequestsNotSentAgain =>
      'Change requests are not sent again automatically.';

  @override
  String get signAgainThenCheckTwo =>
      'Sign in again, then check your two-step verification status.';

  @override
  String get settingsChangedSignAgain => 'Settings changed. Sign in again.';

  @override
  String get signChangedSoSetupWas =>
      'Your sign-in changed, so setup was closed.';

  @override
  String get finishSetup => 'Finish setup';

  @override
  String get closeRecoveryCodes => 'Close recovery codes?';

  @override
  String get youCannotSeeTheseCodes =>
      'You cannot see these codes again. If you have not saved them yet, go back and save them.';

  @override
  String get goBack => 'Go back';

  @override
  String get closeWithoutSaving2 => 'Close without saving';

  @override
  String get twoStepVerificationOff2 => 'Two-step verification is off';

  @override
  String get saveRecoveryCodes => 'Save your recovery codes';

  @override
  String get youSignedOutAllDevices =>
      'You are signed out on all devices. Sign in again.';

  @override
  String get eachCodeWorksOnlyOnce =>
      'Each code works only once. Once you close this screen, you cannot see them again.';

  @override
  String get iSavedRecoveryCodesSomewhere =>
      'I saved the recovery codes somewhere safe';

  @override
  String get cannotShowQrCodeEnter =>
      'Cannot show the QR code. Enter the setup key below yourself.';

  @override
  String get setupQrCodeScanAuthenticator =>
      'Setup QR code to scan with your authenticator app';

  @override
  String get enterRecoveryCodeYouSaved => 'Enter a recovery code you saved.';

  @override
  String get enter6DigitCode => 'Enter the 6-digit code.';

  @override
  String get couldNotConfirmResultTry =>
      'Could not confirm the result. Try again.';

  @override
  String get couldNotConfirmResultStart =>
      'Could not confirm the result.\nStart again from sign-in.';

  @override
  String get verificationTimeRanOutStart =>
      'Verification time ran out.\nStart again from sign-in.';

  @override
  String get enterRecoveryCodeEachOne =>
      'Enter a recovery code.\nEach one works only once.';

  @override
  String get enter6DigitCodeFrom =>
      'Enter the 6-digit code from your authenticator app.';

  @override
  String timeLeft(Object seconds, Object seconds2) {
    return 'Time left $seconds:$seconds2';
  }

  @override
  String get authenticatorApp => 'Authenticator app';

  @override
  String get code => 'Code';

  @override
  String get n6Digits => '6 digits';

  @override
  String get backSign => 'Back to sign-in';

  @override
  String get verifySign => 'Verify and sign in';

  @override
  String get plannedMaintenance => 'Planned maintenance';

  @override
  String plannedMaintenance2(Object when) {
    return 'Planned maintenance · $when';
  }

  @override
  String get notice2 => 'Notice';

  @override
  String text24(num waiting) {
    String _temp0 = intl.Intl.pluralLogic(
      waiting,
      locale: localeName,
      other: '$waiting more notices.',
      one: '1 more notice.',
    );
    return '$_temp0';
  }

  @override
  String get closeNotice => 'Close notice';

  @override
  String text25(num unread) {
    String _temp0 = intl.Intl.pluralLogic(
      unread,
      locale: localeName,
      other: '$unread notices',
      one: '1 notice',
    );
    return '$_temp0';
  }

  @override
  String get deleteNotice => 'Delete this notice?';

  @override
  String get deletedEveryDeviceSignedAccount =>
      'It is deleted on every device signed in to this account.';

  @override
  String text26(num noticesLength) {
    String _temp0 = intl.Intl.pluralLogic(
      noticesLength,
      locale: localeName,
      other: 'Delete all $noticesLength notices?',
      one: 'Delete 1 notice?',
    );
    return '$_temp0';
  }

  @override
  String get deleteAll => 'Delete all';

  @override
  String get noNotices => 'No notices.';

  @override
  String get couldNotStartLocalPc => 'Could not start the local PC';

  @override
  String get collapse => 'Collapse';

  @override
  String get whatNeedsStart => 'What it needs to start';

  @override
  String get sendingPrinter => 'Sending to printer…';

  @override
  String get saving => 'Saving…';

  @override
  String printingPage(Object done, Object total) {
    return 'Printing · page $done/$total';
  }

  @override
  String get preparingPrint => 'Preparing to print…';

  @override
  String text27(num pages) {
    String _temp0 = intl.Intl.pluralLogic(
      pages,
      locale: localeName,
      other: '$pages pages',
      one: '1 page',
    );
    return '$_temp0';
  }

  @override
  String get documentSentFromDesktop => 'Document sent from the desktop';

  @override
  String moreWaiting(Object waiting) {
    return ' · $waiting more waiting';
  }

  @override
  String get paperColorTwoSidedPrinting =>
      'Paper, color and two-sided printing follow the printer\'s defaults.';

  @override
  String get noPrinterInstalledPcYou =>
      'No printer is installed on the PC. You can save as PDF.';

  @override
  String defaultLabel2(Object printerName) {
    return '$printerName (default)';
  }

  @override
  String get copies => 'Copies';

  @override
  String get oneFewerCopy => 'One fewer copy';

  @override
  String get n1CopyMinimum => '1 copy is the minimum';

  @override
  String get oneMoreCopy => 'One more copy';

  @override
  String youCanPrintUpCopies(Object maximumCopies) {
    return 'You can print up to $maximumCopies copies at once';
  }

  @override
  String get saveAsPdf => 'Save as PDF';

  @override
  String get noPrinterPrint => 'No printer to print to';

  @override
  String get print => 'Print';

  @override
  String get previousPage => 'Previous page';

  @override
  String get nextPage => 'Next page';

  @override
  String pagePreview(Object preview) {
    return 'Page $preview preview';
  }

  @override
  String get eachPageKeepsItsOriginal =>
      'Each page keeps its original orientation.';

  @override
  String get couldNotOpenPreview => 'Could not open the preview.';

  @override
  String multitapLog(Object loadHost) {
    return 'Multitap $loadHost log';
  }

  @override
  String get recentEntriesPasswordsTokensClipboard =>
      'Recent entries · passwords, tokens and clipboard content left out';

  @override
  String get couldNotReadHostLog =>
      'Could not read the Host log. Showing the app log only.';

  @override
  String get logFileCannotWrittenSo =>
      'The log file cannot be written, so only this run\'s entries are shown.';

  @override
  String multitap(Object loadHost, num entriesLength) {
    String _temp0 = intl.Intl.pluralLogic(
      entriesLength,
      locale: localeName,
      other: '$entriesLength recent entries',
      one: '1 recent entry',
    );
    return 'Multitap $loadHost · $_temp0';
  }

  @override
  String needsAttention2(Object attention) {
    return 'Needs attention $attention';
  }

  @override
  String get loading2 => 'Loading';

  @override
  String get copied => 'Copied';

  @override
  String get copyAgain => 'Copy again';

  @override
  String get noEntriesShow => 'No entries to show.';

  @override
  String get appStarted => 'App started';

  @override
  String get desktopListLoaded => 'Desktop list loaded';

  @override
  String get streamingConnectionStarted => 'Streaming connection started';

  @override
  String get streamingReadyReply => 'Streaming ready reply';

  @override
  String get streamingEnded => 'Streaming ended';

  @override
  String get resolutionChangeRequested => 'Resolution change requested';

  @override
  String get resolutionChangeResult => 'Resolution change result';

  @override
  String get updateCheckResult => 'Update check result';

  @override
  String get updateDownload => 'Update download';

  @override
  String get hostInstallStarted => 'Host install started';

  @override
  String get installStep => 'Install step';

  @override
  String get installResult => 'Install result';

  @override
  String get virtualDisplayStatus => 'Virtual display status';

  @override
  String get existingDriverKept => 'Existing driver kept';

  @override
  String get virtualDisplayDriverInstall => 'Virtual display driver install';

  @override
  String get virtualDisplayTurned => 'Virtual display turned on';

  @override
  String get resolutionListChecked => 'Resolution list checked';

  @override
  String get serviceStatus => 'Service status';

  @override
  String get serverResponseStatus => 'Server response status';

  @override
  String get serverAddressSave => 'Save server address';

  @override
  String get systemErrorCode => 'System error code';

  @override
  String get hostSettingsAction => 'Host settings action';

  @override
  String get updateInstall => 'Update install';

  @override
  String get clipboard => 'Clipboard';

  @override
  String get driveSharing => 'Drive sharing';

  @override
  String get androidPrinting => 'Android printing';

  @override
  String get androidPrintingDocumentRequested =>
      'Android printing · document requested';

  @override
  String get androidPrintingRequestReceived =>
      'Android printing · request received';

  @override
  String get androidPrintingDocumentChecked =>
      'Android printing · document checked';

  @override
  String get androidPrintingSystemPrintRequested =>
      'Android printing · system print requested';

  @override
  String get androidPrintingPrintServiceConnected =>
      'Android printing · print service connected';

  @override
  String get androidPrintingLayoutChecked =>
      'Android printing · layout checked';

  @override
  String get androidPrintingDocumentTransferStarted =>
      'Android printing · document transfer started';

  @override
  String get androidPrintingDocumentTransferFinished =>
      'Android printing · document transfer finished';

  @override
  String get androidPrintingPrintScreenClosed =>
      'Android printing · print screen closed';

  @override
  String get androidPrintingSystemErrorSeen =>
      'Android printing · system error seen';

  @override
  String get androidPrintingJobAccepted => 'Android printing · job accepted';

  @override
  String get androidPrintingCanceled => 'Android printing · canceled';

  @override
  String get androidPrintingRequestFailed =>
      'Android printing · request failed';

  @override
  String get updateCheckFailedCheckRunning =>
      'Update check failed · check the running version';

  @override
  String get updateCheckFailedUpdateList =>
      'Update check failed · update list format error';

  @override
  String get updateCheckFailedCouldNot =>
      'Update check failed · could not load the server list';

  @override
  String get updateCheckFailed => 'Update check failed';

  @override
  String get updateCheckNewVersionAvailable =>
      'Update check · new version available';

  @override
  String updateCheckNewVersionFound(Object latest) {
    return 'Update check · new version $latest found';
  }

  @override
  String get updateCheckNoPublishedVersion =>
      'Update check · no published version';

  @override
  String get updateCheckUpDate => 'Update check · up to date';

  @override
  String updateCheckUpDate2(Object current) {
    return 'Update check · up to date ($current)';
  }

  @override
  String downloadFailed(Object name) {
    return '$name download failed';
  }

  @override
  String downloadFinished(Object name) {
    return '$name download finished';
  }

  @override
  String installFailed(Object step) {
    return '$step install failed';
  }

  @override
  String installerStarted(Object step) {
    return '$step installer started';
  }

  @override
  String restartingAfterUpdate(Object step) {
    return '$step restarting after update';
  }

  @override
  String get clipboardConnectedCopyPasteShared =>
      'Clipboard connected · copy and paste shared';

  @override
  String clipboardTransferFailed(Object direction) {
    return 'Clipboard $direction transfer failed';
  }

  @override
  String clipboardTransfer(Object direction) {
    return 'Clipboard $direction transfer';
  }

  @override
  String get clipboardDisconnected => 'Clipboard disconnected';

  @override
  String get clipboardConnectionEnded => 'Clipboard connection ended';

  @override
  String get couldNotPrepareAutomaticClipboard =>
      'Could not prepare automatic clipboard sharing';

  @override
  String get sharedFolderConnected => 'Shared folder connected';

  @override
  String get sharedFolderChanged => 'Shared folder changed';

  @override
  String get desktopDriveConnectionFailed => 'Desktop drive connection failed';

  @override
  String get desktopDriveConnected => 'Desktop drive connected';

  @override
  String get sharedFolderDisconnected => 'Shared folder disconnected';

  @override
  String get sharedFolderConnectionEnded => 'Shared folder connection ended';

  @override
  String get usbConnectionReady => 'USB connection ready';

  @override
  String get usbDeviceConnectionFailed => 'USB device connection failed';

  @override
  String get usbDeviceConnected => 'USB device connected';

  @override
  String get usbDeviceRemoved => 'USB device removed';

  @override
  String get desktopDidNotAcceptUsb => 'Desktop did not accept the USB device';

  @override
  String get usbDisconnected => 'USB disconnected';

  @override
  String get usbConnectionEnded => 'USB connection ended';

  @override
  String get usbCaptureDriverInstallFailed =>
      'USB capture driver install failed';

  @override
  String get notInstalled2 => 'Not installed';

  @override
  String get deviceNotStarted => 'Device not started';

  @override
  String get notRunning => 'Not running';

  @override
  String get noResponse => 'No response';

  @override
  String get noChange => 'No change';

  @override
  String get consoleSessionOff => 'Console session off';

  @override
  String resolution2(Object width, Object height) {
    return 'Resolution $width×$height';
  }

  @override
  String tookMs(Object value) {
    return 'Took $value ms';
  }

  @override
  String get succeeded => 'Succeeded';

  @override
  String get step => 'Step';

  @override
  String get deviceStarted => 'Device started';

  @override
  String get activeDisplay => 'Active display';

  @override
  String get ownDevices => 'Own devices';

  @override
  String get waitingSetup => 'Waiting for setup';

  @override
  String get rdpSession => 'RDP session';

  @override
  String get monitors => 'Monitors';

  @override
  String get responded => 'Responded';

  @override
  String get changed => 'Changed';

  @override
  String get deviceRestartTried => 'Device restart tried';

  @override
  String get restore2 => 'To restore';

  @override
  String get notRestored => 'Not restored';

  @override
  String get notRestoredVirtualMonitor => 'Not restored (virtual monitor)';

  @override
  String get backConsoleSession => 'Back to console session';

  @override
  String get consoleSessionNeedsRestoring => 'Console session needs restoring';

  @override
  String get consoleSessionActive => 'Console session active';

  @override
  String get adapterBeforeRestart => 'Adapter before restart';

  @override
  String get adapterReconnected => 'Adapter reconnected';

  @override
  String get skippedPackets => 'Skipped packets';

  @override
  String get newVersionAvailable2 => 'New version available';

  @override
  String get desktopDrive => 'Desktop drive';

  @override
  String get fileRequests => 'File requests';

  @override
  String get failedRequests => 'Failed requests';

  @override
  String get rememberedFolders => 'Remembered folders';

  @override
  String get reopenedFolders => 'Reopened folders';

  @override
  String get connectedDevices => 'Connected devices';

  @override
  String get handledRequests => 'Handled requests';

  @override
  String get kibTransferred => 'KiB transferred';

  @override
  String get deviceType => 'Device type';

  @override
  String get disconnectUpdate => 'Disconnect and update?';

  @override
  String get allOpenConnectionsClosedDesktops =>
      'All open connections are closed.\nThe desktops and the work running on them stay as they are.';

  @override
  String get disconnectUpdate2 => 'Disconnect and update';

  @override
  String get couldNotOpenHostUpdate =>
      'Could not open the Host update. Check your sign-in and server connection.';

  @override
  String get download2 => 'Download';

  @override
  String get installer => 'Installer';

  @override
  String get networkDroppedSoDownloadResumes =>
      'The network dropped, so the download resumes from where it stopped.';

  @override
  String get checkingDownloadedFilesSizeSha2 =>
      'Checking the downloaded file\'s size, SHA-256 and installer version.';

  @override
  String get whenInstallerOpensFollowIts =>
      'When the installer opens, follow its steps.';

  @override
  String get onceInstallerFinishesItsChecks =>
      'Once the installer finishes its checks, Host settings closes briefly and reopens as the new version.';

  @override
  String get onceInstallerFinishesItsChecks2 =>
      'Once the installer finishes its checks, the app closes briefly and reopens in the same place as the new version.';

  @override
  String sizeMatchesBytes(Object size) {
    return 'Size matches: $size bytes';
  }

  @override
  String sha256Matches(Object digest) {
    return 'SHA-256 matches · $digest';
  }

  @override
  String installerVersionConfirmed(Object version) {
    return 'Installer version $version confirmed';
  }

  @override
  String get multitapReleaseSignatureConfirmed =>
      'Multitap release signature confirmed';

  @override
  String code3(Object code) {
    return 'Code $code';
  }

  @override
  String hostUpdatesAvailable(Object hostUpdates) {
    return 'Host updates available: $hostUpdates';
  }

  @override
  String get whenInstallerOpensInstallContinues =>
      'When the installer opens, the install continues in its window.';

  @override
  String get hostSettingsClosesConnectionsEnd =>
      'Host settings closes, connections end, and the Host service restarts.\nHost registration and virtual display settings stay as they are.\nWindows does not restart on its own.';

  @override
  String get appClosesReopensSamePlace =>
      'The app closes and reopens in the same place as the new version.\nOpen connections are closed.\nServer address and sign-in settings stay as they are.';

  @override
  String get checkingUpdates => 'Checking for updates';

  @override
  String get youreUpDate => 'You\'re up to date';

  @override
  String get noInstallerPublished => 'No installer is published';

  @override
  String get newVersionAvailable3 => 'A new version is available';

  @override
  String get downloadingUpdate => 'Downloading update';

  @override
  String get checkingFile2 => 'Checking file';

  @override
  String get updateReady => 'Update ready';

  @override
  String get openingInstaller => 'Opening installer';

  @override
  String get applyingUpdate => 'Applying update';

  @override
  String get installerOpened => 'Installer opened';

  @override
  String current5(Object currentVersion) {
    return 'Current $currentVersion';
  }

  @override
  String get checkingReleaseInfoServer =>
      'Checking release info on your server.';

  @override
  String get youCanUpdateOnceAdministrator =>
      'You can update once an administrator uploads an installer to the server.';

  @override
  String get followStepsInstallerWindowYou =>
      'Follow the steps in the installer window. You can close this window.';

  @override
  String get downloadUpdate => 'Download update';

  @override
  String get openInstaller => 'Open installer';

  @override
  String get restartUpdate => 'Restart to update';

  @override
  String get continueInstaller => 'Continue with installer';

  @override
  String get installInstaller => 'Install with the installer';

  @override
  String get minimize2 => 'Minimize';

  @override
  String unknown5(Object detail) {
    return 'Unknown · $detail';
  }

  @override
  String get usageReportedByHost => 'Usage · reported by Host';

  @override
  String get usageReportedByProxmox => 'Usage · reported by Proxmox';

  @override
  String get checking4 => 'Checking…';

  @override
  String get couldNotCheck => 'Could not check';

  @override
  String get lastResult => 'Last result';

  @override
  String get checked3 => 'Checked at';

  @override
  String get statusServerGotFromHost =>
      'Status the server got from Host and Proxmox';

  @override
  String get checkAgain3 => 'Check again';

  @override
  String get noServerAddressSetSo =>
      'No server address is set, so the change list cannot be loaded.';

  @override
  String get couldNotLoadServersChange =>
      'Could not load this server\'s change list. Try again in a moment.';

  @override
  String get noReleaseDate => 'No release date';

  @override
  String get noChangesListedVersion =>
      'No changes are listed for this version.';

  @override
  String text28(num total) {
    String _temp0 = intl.Intl.pluralLogic(
      total,
      locale: localeName,
      other: '$total versions',
      one: '1 version',
    );
    return '$_temp0';
  }

  @override
  String get byVersion => 'By version';

  @override
  String get releaseHistoryShowAll => 'Show all';

  @override
  String get seeVersionListEachVersions =>
      'See the version list and each version\'s changes side by side';

  @override
  String get seeChangesAllVersionsOne =>
      'See the changes of all versions in one list';

  @override
  String get loadingChangeList => 'Loading the change list.';

  @override
  String get serverHasNoChangeList => 'This server has no change list.';

  @override
  String get couldNotLoadEarlierVersions =>
      'Could not load earlier versions · try again';

  @override
  String get youveReachedFirstVersion => 'You\'ve reached the first version';

  @override
  String noChangesListed(Object note) {
    return '$note · no changes listed';
  }

  @override
  String get newerVersion => 'Newer version (↑)';

  @override
  String get olderVersion => 'Older version (↓)';

  @override
  String get administratorApprovalNeeded2 => 'Administrator approval needed';

  @override
  String get couldNotPrepareScreenFit =>
      'Could not prepare a screen to fit the window. Reconnect.';

  @override
  String get pressOkLockMouseInside =>
      'Press OK to lock the mouse inside the desktop.\nPress Ctrl+Shift+Alt+Z to turn it off again.';

  @override
  String get mouseLockedInsideDesktopCtrl =>
      'Mouse locked inside the desktop · Ctrl+Shift+Alt+Z to turn on or off';

  @override
  String get couldNotApplyScreenMode =>
      'Could not apply the screen mode. The current connection stays.';

  @override
  String get couldNotConfirmOriginalScreen =>
      'Could not confirm the original screen mode came back. Choose it again from Screen in the toolbar.';

  @override
  String get couldNotChangeScreenMode =>
      'Could not change the screen mode. Choose it again.';

  @override
  String get filesBeingCopiedCancelCopy =>
      'Files are being copied. Cancel the copy, then disconnect.';

  @override
  String desktopCannotPrintPc(Object reason) {
    return 'This desktop cannot print to your PC. $reason';
  }

  @override
  String printerConnectionWasLostReconnect(num lostDocuments) {
    String _temp0 = intl.Intl.pluralLogic(
      lostDocuments,
      locale: localeName,
      other: '$lostDocuments documents not yet printed cannot be printed.',
      one: '1 document not yet printed cannot be printed.',
    );
    return 'The printer connection was lost. $_temp0 Reconnect to the desktop and print again.';
  }

  @override
  String get printerConnectionWasLostReconnect2 =>
      'The printer connection was lost. Reconnect to the desktop to print.';

  @override
  String get printed => 'Printed';

  @override
  String get savedAsPdf => 'Saved as PDF.';

  @override
  String get printingCanceledPrintAgainFrom =>
      'Printing canceled. Print again from the desktop if needed.';

  @override
  String screenMode(Object shownLabel) {
    return 'Screen mode · $shownLabel';
  }

  @override
  String text29(num mappedFolders) {
    String _temp0 = intl.Intl.pluralLogic(
      mappedFolders,
      locale: localeName,
      other: '$mappedFolders shared folders connected',
      one: '1 shared folder connected',
    );
    return '$_temp0';
  }

  @override
  String text30(num sharedUsb) {
    String _temp0 = intl.Intl.pluralLogic(
      sharedUsb,
      locale: localeName,
      other: '$sharedUsb USB devices connected',
      one: '1 USB device connected',
    );
    return '$_temp0';
  }

  @override
  String get foldersUsbUsePcsFolders =>
      'Folders and USB · use your PC\'s folders and USB devices on the desktop';

  @override
  String get sharedFoldersOpenPcFolders =>
      'Shared folders · open PC folders on the desktop';

  @override
  String get usbDevicesUsePcsUsb =>
      'USB devices · use your PC\'s USB devices on the desktop';

  @override
  String connected4(Object mappedFolders) {
    return '$mappedFolders connected';
  }

  @override
  String connected5(Object sharedUsb) {
    return '$sharedUsb connected';
  }

  @override
  String get settingsPictureQualityGuide =>
      'Settings · picture quality, guide and more';

  @override
  String get youCanChangeWhenScreen =>
      'You can change this when the screen switch is done';

  @override
  String get guideOpen => 'The guide is open';

  @override
  String get availableOnceConnected2 => 'Available once connected';

  @override
  String get filesBeingCopiedTryAgain =>
      'Files are being copied. Try again when done.';

  @override
  String get couldNotReconnectUsbDevice =>
      'Could not reconnect the USB device. USB is not available in this connection.';

  @override
  String get sharedFoldersEndedByReconnect =>
      'Folder sharing ended with the reconnection. Add the folders again in Folders·USB.';

  @override
  String get movedUsbDeviceAnotherWindow =>
      'Moved the USB device to another window';

  @override
  String movedUsbDeviceWindow(Object to) {
    return 'Moved the USB device to the $to window';
  }

  @override
  String get remoteResolutionPreparedAgainFit =>
      'The remote resolution is prepared again to fit the window.\nIf the screen is too large, make the window smaller, then connect.';

  @override
  String get reconnectMode => 'Reconnect in this mode';

  @override
  String get takeOverWindowsScreen => 'Take over the Windows screen?';

  @override
  String get currentWindowsScreenMovesStreaming =>
      'The current Windows screen moves to streaming.\nA Windows Remote Desktop (RDP) connection may be closed. Running work stays as it is.';

  @override
  String get takeOverScreen => 'Take over screen';

  @override
  String get connectionCanceled => 'Connection canceled.';

  @override
  String get couldNotConnectAllMonitors =>
      'Could not connect on all monitors. Connect in window mode or full screen from the list.';

  @override
  String get couldNotApplyScreenMode2 =>
      'Could not apply the screen mode. Reconnect.';

  @override
  String get hostConnectionLostReconnecting =>
      'Host connection lost, reconnecting';

  @override
  String get connectionLostReconnecting => 'Connection lost, reconnecting';

  @override
  String get couldNotChangeScreenSize =>
      'Could not change the screen size, reconnecting';

  @override
  String get monitorSetupChangedDisconnectThen =>
      'The monitor setup changed. Disconnect, then choose the screen mode again.';

  @override
  String get cantShowScreenOnMonitors => 'Can\'t show the screen';

  @override
  String text31(num activeCount) {
    String _temp0 = intl.Intl.pluralLogic(
      activeCount,
      locale: localeName,
      other: 'Switched to $activeCount monitors',
      one: 'Switched to 1 monitor',
    );
    return '$_temp0';
  }

  @override
  String get disconnected4 => 'Disconnected';

  @override
  String get reconnectPickUpWhereYou =>
      'Reconnect to pick up where you left off.';

  @override
  String get checkNumberMonitorsDesktopList =>
      'Check the number of monitors in the desktop list.';

  @override
  String get reconnect2 => 'Reconnect…';

  @override
  String get hostUpdating2 => 'Host is updating';

  @override
  String get reconnectsAutomaticallyWhenDoneWaits =>
      'Reconnects automatically when done.\nWaits up to 10 minutes.';

  @override
  String get pictureQuality => 'Picture quality';

  @override
  String get connectedSomewhereElse => 'Connected somewhere else';

  @override
  String get ifWindowsRestartingConnectsAs =>
      'If Windows is restarting, connects as soon as it is done.\nWaits up to 3 minutes.';

  @override
  String get connectsAutomaticallyWhenDesktopBack =>
      'Connects automatically when the desktop is back on.\nWaits up to 3 minutes.';

  @override
  String get cancelConnection => 'Cancel connection';

  @override
  String get preparingScreenFitWindow => 'Preparing a screen to fit the window';

  @override
  String get checkingAccountPermissionsHostConnection =>
      'Checking account permissions and the Host connection.';

  @override
  String get fittingScreenSize2 => 'Fitting the screen size.';

  @override
  String get preparingHostConnection => 'Preparing the Host connection.';

  @override
  String get connectionCantMatchResolutionWindow =>
      'This connection can\'t match the resolution to the window size. Tell your administrator.';

  @override
  String get matchesResolutionWindowSizeAgain =>
      'Matches the resolution to the window size again.';

  @override
  String get retryResolution => 'Retry resolution';

  @override
  String get fixedResolution => 'Fixed resolution';

  @override
  String get checkingPcsMonitors => 'Checking your PC\'s monitors';

  @override
  String get applyingWindowMode => 'Applying the display mode';

  @override
  String get updateStepVerify => 'Verify';

  @override
  String get closingPreviousConnection => 'Closing the previous connection';

  @override
  String get waitingHostScreen2 => 'Waiting for the Host screen';

  @override
  String get settingUpRemoteMonitors => 'Setting up remote monitors';

  @override
  String get connectingNewScreen => 'Connecting the new screen';

  @override
  String get waitingFirstFrame3 => 'Waiting for the first frame';

  @override
  String get restoringPreviousWindowMode =>
      'Restoring the previous window mode';

  @override
  String switching(Object target) {
    return 'Switching to $target';
  }

  @override
  String get cancellingSwitch => 'Cancelling the switch';

  @override
  String get youCanTypeOnceNew => 'You can type once the new screen is ready.';

  @override
  String get cancelSwitch => 'Cancel switch';

  @override
  String get tryAgainMoment => 'Try again in a moment.';

  @override
  String get tryAgain2 => 'Try again.';

  @override
  String get saving2 => 'Saving…';

  @override
  String get connectionConfirmed => 'Connection confirmed';

  @override
  String get couldntSaveServerAddress2 => 'Couldn\'t save the server address';

  @override
  String get couldntConfirmConnection => 'Couldn\'t confirm the connection';

  @override
  String get checkingServerResponse => 'Checking the server response';

  @override
  String get checkConnection => 'Check the connection';

  @override
  String multitapResponseTimeMs(Object version, Object elapsedMs) {
    return 'Multitap $version · response time $elapsedMs ms';
  }

  @override
  String get pleaseWait => 'Please wait.';

  @override
  String get addressServerYouSign => 'Address of the server you sign in to';

  @override
  String account3(Object username) {
    return 'Account: $username';
  }

  @override
  String text32(num sessionCount) {
    String _temp0 = intl.Intl.pluralLogic(
      sessionCount,
      locale: localeName,
      other: '$sessionCount existing sign-ins',
      one: '1 existing sign-in',
    );
    return '$_temp0';
  }

  @override
  String signed2(Object createdAt) {
    return 'Signed in $createdAt';
  }

  @override
  String connectedDesktops(Object session) {
    return 'Connected desktops: $session';
  }

  @override
  String get signingHereEndsStreamingConsole =>
      'Signing in here ends the streaming and Console connections of the other sign-in.';

  @override
  String get recoveryCodeHasBeenUsed =>
      'The recovery code has been used. Cancelling won\'t make it usable again.';

  @override
  String get signOutElsewhereSignHere => 'Sign out elsewhere and sign in here?';

  @override
  String get signOutSign => 'Sign out and sign in';

  @override
  String get signedOutMultitap => 'Signed out of Multitap';

  @override
  String get notYetConfirmedAllConnections =>
      'Not yet confirmed that all connections are closed. Select ‘Check again’.';

  @override
  String get couldntConfirmAllConnectionsClosed =>
      'Couldn\'t confirm that all connections are closed. Quit Multitap and open it again.';

  @override
  String get checking5 => 'Checking…';

  @override
  String get checkAgain4 => 'Check again';

  @override
  String get youllSignedOutSoonIf =>
      'You\'ll be signed out soon if there\'s no input. All connections will close too.';

  @override
  String youllSignedOutInSeconds(Object seconds) {
    return 'You\'ll be signed out in $seconds s if there\'s no input. All connections will close too.';
  }

  @override
  String get keepUsing => 'Keep using';

  @override
  String get checkingAutoSignOut => 'Checking auto sign-out';

  @override
  String get autoSignOutOff => 'Auto sign-out off';

  @override
  String autoSignOut(Object left) {
    return 'Auto sign-out in $left';
  }

  @override
  String get youStaySignedWithoutInput =>
      'You stay signed in without input. You\'re signed out if you sign in elsewhere or the sign-in expires.';

  @override
  String get signsYouOutMultitapWhen =>
      'Signs you out of Multitap when there\'s no keyboard, mouse or touch input.';

  @override
  String get cantCheckServerSetting => 'Can\'t check the server setting.';

  @override
  String get changingSetting => 'Changing the setting.';

  @override
  String get youStaySignedWithoutInput2 => 'You stay signed in without input.';

  @override
  String get checkingTimeLeft => 'Checking the time left.';

  @override
  String withoutInputYoureSignedOut(Object left) {
    return 'Without input, you\'re signed out in $left.';
  }

  @override
  String get autoSignOut2 => 'Auto sign-out';

  @override
  String get couldntChangeSettingTryAgain =>
      'Couldn\'t change the setting. Try again.';

  @override
  String get couldntConfirmScreenWasRestored =>
      'Couldn\'t confirm the screen was restored';

  @override
  String connectionsBlockedUntilRestoreConfirmed(Object cleanupCause) {
    return '$cleanupCause\nConnections are blocked until the restore is confirmed.';
  }

  @override
  String statusRecorded(Object cleanupEvidence, Object at) {
    return '$cleanupEvidence · status recorded $at';
  }

  @override
  String get waiting3 => 'Waiting';

  @override
  String get received => 'Received';

  @override
  String get connectionSlowLowerPictureQuality =>
      'The connection is slow. Lower picture quality to ‘Saver’ for fewer stutters.';

  @override
  String get reconnectSaver => 'Reconnect with Saver';

  @override
  String get showConnectionStats => 'Show connection stats';

  @override
  String get toolbarHere => 'The toolbar is here.';

  @override
  String get changeScreenModeScaleSettings =>
      'Change screen mode, scale and settings here.\nClick the handle to fold it, and again to open it.';

  @override
  String get guide => 'Guide';

  @override
  String get featuresAvailableConnection =>
      'Features available on this connection.';

  @override
  String get dontShowAgainDesktop => 'Don\'t show again for this desktop';

  @override
  String get youCanSeeAgainUnder =>
      'You can see it again under Guide in settings.';

  @override
  String get closeGuideEsc => 'Close guide (Esc)';

  @override
  String get good => 'Good';

  @override
  String get fair => 'Fair';

  @override
  String get slow => 'Slow';

  @override
  String get monitors2 => 'Monitors…';

  @override
  String get windowsTextIconSize => 'Windows text and icon size';

  @override
  String cantUseSharedFolders(Object why) {
    return 'Shared folders can\'t be used.$why';
  }

  @override
  String cantUseUsbDevices(Object why) {
    return 'USB devices can\'t be used.$why';
  }

  @override
  String get printerUsbSharedFoldersCopy =>
      'Printer · USB · shared folders · copy';

  @override
  String get turnOffUsbMouse => 'to turn off · USB mouse';

  @override
  String get turnOffNormalInput => 'to turn off · normal input';

  @override
  String get turnOff => 'to turn on or off';

  @override
  String get staysOpenWhenYouClick =>
      'Stays open when you click the desktop screen';

  @override
  String get notConnected => 'Not connected';

  @override
  String get hideDetails2 => 'Hide details';

  @override
  String get showDetails => 'Show details';

  @override
  String get remoteScreenOpensHere => 'The remote screen opens here';

  @override
  String get designDemoNoRealConnection => 'Design demo · no real connection';

  @override
  String get notConnected2 => 'Not connected';

  @override
  String get frameRate => 'Frame rate';

  @override
  String get latency => 'Latency';

  @override
  String get input => 'Input';

  @override
  String get inactive => 'Inactive';

  @override
  String get backDesktopList => 'Back to desktop list';

  @override
  String get copyDiagnostics2 => 'Copy diagnostics';

  @override
  String get detailedDiagnostics => 'Detailed diagnostics';

  @override
  String get copyJson => 'Copy JSON';

  @override
  String get connecting3 => 'Connecting';

  @override
  String get notConnected3 => 'Not connected';

  @override
  String get connectingDesktop => 'Connecting to the desktop';

  @override
  String get notConnected4 => 'Not connected';

  @override
  String get latencyUnder1Ms => 'Latency under 1 ms';

  @override
  String latencyAboutMs(Object value) {
    return 'Latency about $value ms';
  }

  @override
  String get connectionQuality => 'Connection quality';

  @override
  String connectionQuality2(Object word) {
    return 'Connection quality · $word';
  }

  @override
  String connectionQuality3(Object summary) {
    return 'Connection quality · $summary';
  }

  @override
  String get menuScreenSettingsMore => 'Menu · screen, settings and more';

  @override
  String get toolbar => 'Toolbar';

  @override
  String get window => 'Window';

  @override
  String get release => 'to release';

  @override
  String slow2(Object latencyText) {
    return '$latencyText · slow';
  }

  @override
  String get n999MsSlow => '999 ms · slow';

  @override
  String clickOpenToolbar(Object summary) {
    return '$summary · click to open toolbar';
  }

  @override
  String get foldToolbarEsc => 'Fold toolbar (Esc)';

  @override
  String get foldersUsb => 'Folders·USB';

  @override
  String get keepToolbarOpen => 'Keep toolbar open';

  @override
  String get windowed2 => 'Windowed';

  @override
  String get resolutionRemoteScreen => 'Resolution · remote screen';

  @override
  String screenDecoding(Object device) {
    return 'Screen · $device decoding';
  }

  @override
  String get receivedIncomingVideo => 'Received · incoming video';

  @override
  String get videoPath => 'Video path';

  @override
  String get encodingHost => 'Encoding · Host';

  @override
  String decoding(Object device) {
    return 'Decoding · $device';
  }

  @override
  String get couldntGetDesktopConnectionInfo =>
      'Couldn\'t get the desktop connection info.';

  @override
  String get disconnectedClickCloseRemoteWindow =>
      'Disconnected. Click ‘Close remote window’ again.';

  @override
  String get checkingDesktopConnectionInfo =>
      'Checking the desktop connection info.';

  @override
  String get product => 'Product';

  @override
  String get couldntLoadSavedSignInfo =>
      'Couldn\'t load the saved sign-in info.';

  @override
  String get couldntRememberUsernameYouCan =>
      'Couldn\'t remember your ID. You can still sign in.';

  @override
  String get couldntTurnOffRememberPassword2 =>
      'Couldn\'t turn off remember password. Try again.';

  @override
  String get couldntConfirmRememberSignSetting =>
      'Couldn\'t confirm the remember sign-in setting was saved.';

  @override
  String get couldntSaveRememberSignSetting =>
      'Couldn\'t save the remember sign-in setting.';

  @override
  String get changeServerAddress => 'Change server address';

  @override
  String get setServerAddress => 'Set server address';

  @override
  String get saveWindowsCredentialManager =>
      'Save in Windows Credential Manager';

  @override
  String get setUpServerStart => 'Set up server and start';

  @override
  String get exploreDemo => 'Explore the demo';

  @override
  String get collectingDetailedDiagnostics => 'Collecting detailed diagnostics';

  @override
  String get hostDetailsApplyFromNext =>
      'Host details apply from the next connection';

  @override
  String viaTcpServerAppRound(
    Object rttMs,
    Object clientDecodeMs,
    Object clientQueueMs,
    Object current,
  ) {
    return 'Via TCP server · app round trip $rttMs ms\nDecode $clientDecodeMs ms · queue $clientQueueMs ms\n$current';
  }

  @override
  String get notMeasured2 => 'Not measured';

  @override
  String p50P95Ms(Object p50Ms, Object p95Ms, num samples) {
    String _temp0 = intl.Intl.pluralLogic(
      samples,
      locale: localeName,
      other: '$samples samples',
      one: '1 sample',
    );
    return 'p50 $p50Ms / p95 $p95Ms ms · $_temp0';
  }

  @override
  String get inputHostApplied => 'Input→Host applied';

  @override
  String get inputScreenShown => 'Input→screen shown';

  @override
  String get hostReceivedApplied => 'Host received→applied';

  @override
  String get inputAckRoundTrip => 'Input ack round trip';

  @override
  String get currentServerHostDoesntSupport =>
      'The current server or Host doesn\'t support input latency measurement.';

  @override
  String get samplesFromLast30Seconds =>
      'Samples from the last 30 seconds. Input→Host applied is half the measured round trip minus Host wait time (estimated), plus the Host-measured time from receipt to SendInput return. Input→screen shown runs from sending the input until the next changed remote frame is decoded on the PC and handed over as a texture; monitor display time is not included.';

  @override
  String get fpsPerScreen => 'fps (per screen)';

  @override
  String get status => 'Status';

  @override
  String get encodingScale => 'Encoding scale';

  @override
  String get autoResolution => 'Auto resolution';

  @override
  String get notSupportedConnection => 'Not supported on this connection';

  @override
  String get applyingRemoteScreen => 'Applying to the remote screen';

  @override
  String get requestedScreen => 'Requested screen';

  @override
  String auto2(Object desired) {
    return 'Auto · $desired';
  }

  @override
  String get targetFps => 'Target FPS';

  @override
  String requestedApplied(Object fps, Object session) {
    return 'Requested $fps / applied $session';
  }

  @override
  String get targetBitrate => 'Target bitrate';

  @override
  String autoNowMaxMbps(Object bitrateTarget, Object bitrateMax) {
    return 'Auto · now $bitrateTarget / max $bitrateMax Mbps';
  }

  @override
  String requestedAppliedMbps(Object options, Object session) {
    return 'Requested $options / applied $session Mbps';
  }

  @override
  String get captureMethod => 'Capture method';

  @override
  String get hostGpuUse => 'Host GPU in use';

  @override
  String get selectedHostGpu => 'Selected Host GPU';

  @override
  String get encoderFallbackReason => 'Encoder fallback reason';

  @override
  String get losslessSharpening => 'Lossless sharpening';

  @override
  String get off5 => 'Off';

  @override
  String get waitingAppliesWhenScreenStill =>
      'Waiting · applies when the screen is still';

  @override
  String active(num losslessTiles, Object session) {
    String _temp0 = intl.Intl.pluralLogic(
      losslessTiles,
      locale: localeName,
      other: '$losslessTiles tiles',
      one: '1 tile',
    );
    return 'Active · $_temp0$session';
  }

  @override
  String get drawnPcCursorV1 => 'Drawn on the PC (cursor-v1)';

  @override
  String get hostEncoding => 'Host encoding';

  @override
  String get pcDecoding => 'PC decoding';

  @override
  String get pcDecoder => 'PC decoder';

  @override
  String get otherAdapter => ' · other adapter';

  @override
  String hardwareGpu(Object session, Object decodeVendor) {
    return 'Hardware (GPU$session$decodeVendor)';
  }

  @override
  String get softwareCpu => 'Software (CPU)';

  @override
  String get frameDelivery => 'Frame delivery';

  @override
  String get gpuSurface => 'GPU surface';

  @override
  String get cpuPixelBuffer => 'CPU pixel buffer';

  @override
  String get newTextureDelivery => 'New texture delivery';

  @override
  String get networkWait => 'Network wait';

  @override
  String get textureDeliveryMeasuredFlutterRender =>
      'Texture delivery is measured at the Flutter render request. It doesn\'t mean the frame was shown or reflect input latency.';

  @override
  String get hostProcessing => 'Host processing';

  @override
  String get displayLayoutCheck => 'Display layout check';

  @override
  String get captureRead => 'Capture·read';

  @override
  String get colorConversion => 'Color conversion';

  @override
  String get encoding => 'Encoding';

  @override
  String get totalAvgP95 => 'Total avg / p95';

  @override
  String get processingMax => 'Processing max';

  @override
  String get waitingRecentHostMeasurements =>
      'Waiting for recent Host measurements.';

  @override
  String get shownOnceHostReportsProcessing =>
      'Shown once the Host reports processing times.';

  @override
  String lastSSampledNetworkWait(Object session, num samples) {
    String _temp0 = intl.Intl.pluralLogic(
      samples,
      locale: localeName,
      other: '$samples frames',
      one: '1 frame',
    );
    return 'Last $session s · $_temp0 sampled. Network wait time is not included.';
  }

  @override
  String get pcProcessing => 'PC processing';

  @override
  String get decodeAvg => 'Decode avg';

  @override
  String get decodeMax => 'Decode max';

  @override
  String get videoQueue => 'Video queue';

  @override
  String ms(num clientQueuePackets, Object clientQueueMs) {
    String _temp0 = intl.Intl.pluralLogic(
      clientQueuePackets,
      locale: localeName,
      other: '$clientQueuePackets packets',
      one: '1 packet',
    );
    return '$_temp0 · $clientQueueMs ms';
  }

  @override
  String get replacedBeforeDelivery => 'Replaced before delivery';

  @override
  String text33(num skippedDecodedFrames) {
    String _temp0 = intl.Intl.pluralLogic(
      skippedDecodedFrames,
      locale: localeName,
      other: '$skippedDecodedFrames frames',
      one: '1 frame',
    );
    return '$_temp0';
  }

  @override
  String get gpuVideoDecoding => 'GPU video decoding';

  @override
  String get averagesCoverRecentPollingWindow =>
      'Averages cover the recent polling window; max and replacement counts cover the current resolution.';

  @override
  String pcTotal(Object app, Object systemTenths) {
    return '$app (PC total $systemTenths)';
  }

  @override
  String get cpuDecoder => 'CPU decoder';

  @override
  String get gpuDecoder => ' · GPU decoder';

  @override
  String unknown6(Object path) {
    return 'Unknown$path';
  }

  @override
  String autoBitrateMaxMbps(Object maxMbps) {
    return 'Auto bitrate · max $maxMbps Mbps';
  }

  @override
  String get videoBitrate => 'Video bitrate';

  @override
  String get auto3 => '  Auto';

  @override
  String get manual2 => 'Manual';

  @override
  String adjustsBitrateConnectionMaxMbps(Object maxMbps) {
    return 'Adjusts the bitrate to the connection. Max $maxMbps Mbps';
  }

  @override
  String get ifNetworkCantKeepUp =>
      'If the network can\'t keep up, latency can build up.';

  @override
  String get decodesGpuFallsBackCpu =>
      'Decodes on the GPU. Falls back to the CPU if that fails.';

  @override
  String get decodesCpuOnlyTryIf =>
      'Decodes on the CPU only. Try it if the picture breaks up.';

  @override
  String get eachMonitorMatchesItsOwn => 'Each monitor matches its own size.';

  @override
  String get remoteResolutionFollowsWindowSize =>
      'The remote resolution follows the window size.';

  @override
  String get backSettings => 'Back to settings';

  @override
  String get quitMultitap => 'Quit Multitap?';

  @override
  String get allOpenConnectionsCloseToo =>
      'All open connections close too. Desktops stay on.';

  @override
  String get desktopsStay => 'Desktops stay on.';

  @override
  String version3(Object version) {
    return 'version $version';
  }

  @override
  String get text34 => '';

  @override
  String get text35 => '';

  @override
  String get calculatingTimeLeft => 'Calculating time left';

  @override
  String aboutMinLeft2(Object seconds) {
    return 'About $seconds min left';
  }

  @override
  String get checkNeeded2 => 'Check needed';

  @override
  String get couldntUpdate => 'Couldn\'t update';

  @override
  String get existingHostUnchanged => 'The existing Host is unchanged.';

  @override
  String get installedAppUnchanged => 'The installed app is unchanged.';

  @override
  String get couldntGetUpdateInfoCheck =>
      'Couldn\'t get update info.\nCheck the network and try again. If it keeps happening, tell your administrator.';

  @override
  String get couldntDownloadFileCheckNetwork =>
      'Couldn\'t download the file.\nCheck the network and try again.';

  @override
  String get networkDroppedSoFileWasnt =>
      'The network dropped, so the file wasn\'t downloaded.\nCheck the network and try again.';

  @override
  String get serverDidntRespondWasToo =>
      'The server didn\'t respond or was too slow, so the file wasn\'t downloaded.\nCheck the network and try again.';

  @override
  String get serverCouldntRespondJustNow =>
      'The server couldn\'t respond just now.\nTry again in a moment.';

  @override
  String get serverDoesntHaveFileVersion =>
      'The server doesn\'t have the file for this version.\nClose the window and check for updates again.';

  @override
  String get serverRefusedFileRequestCheck =>
      'The server refused the file request.\nCheck the server address and try again.';

  @override
  String get notEnoughDiskSpaceSave =>
      'Not enough disk space to save the file.\nFree up space and try again.';

  @override
  String get couldntSaveFileTempFolder =>
      'Couldn\'t save the file to the temp folder.\nCheck your security software or folder permissions.';

  @override
  String get downloadedFileSizeDoesntMatch =>
      'The downloaded file size doesn\'t match the release info, so it wasn\'t installed.\nTry again, and if it keeps happening, tell your administrator.';

  @override
  String get downloadedFileDoesntMatchRelease =>
      'The downloaded file doesn\'t match the release size or SHA-256, so it wasn\'t installed.\nTry again, and if it keeps happening, tell your administrator.';

  @override
  String fileVersionDoesntMatchRelease(Object version, Object kept) {
    return 'The file version doesn\'t match the release version$version, so it wasn\'t installed.\n$kept\nTell your administrator.';
  }

  @override
  String releaseSignatureMissingInvalidSo(Object kept) {
    return 'The release signature is missing or invalid, so it wasn\'t installed.\n$kept\nTell your administrator.';
  }

  @override
  String administratorApprovalWasCancelledSo(Object kept) {
    return 'Administrator approval was cancelled, so nothing was updated.\n$kept\nNext time, choose ‘Yes’ in the approval window.';
  }

  @override
  String fileServerIsntNewerThan(Object kept) {
    return 'The file on this server isn\'t newer than the running version, so it wasn\'t installed. $kept';
  }

  @override
  String installerClosedBeforeUpdateStarted(Object kept) {
    return 'The installer closed before the update started. $kept';
  }

  @override
  String get couldntCloseOpenConnectionsDisconnect =>
      'Couldn\'t close the open connections. Disconnect them and try again.';

  @override
  String get couldntCheckRunningVersionRestart =>
      'Couldn\'t check the running version. Restart the app and check for updates.';

  @override
  String get updateAlreadyBeingPreparedTry =>
      'An update is already being prepared. Try again in a moment.';

  @override
  String installationFilesNotValidInstall(Object kept) {
    return 'The installation files are not valid. Install with the installer. $kept';
  }

  @override
  String couldntCommunicateInstallerTryAgain(Object kept) {
    return 'Couldn\'t communicate with the installer. Try again. $kept';
  }

  @override
  String get desktopHasNoHostInstalled =>
      'This desktop has no Host installed to update. Install with the installer.';

  @override
  String get windowsDoesntTrustVirtualDisplay =>
      'This Windows doesn\'t trust the virtual display driver signature yet. Check the signature in the installer, then install. The existing Host is unchanged.';

  @override
  String get updateNeedsInstallChangeVirtual =>
      'This update needs to install or change the virtual display driver. Check it in the installer, then install. The existing Host is unchanged.';

  @override
  String fileCameFromSomewhereOther(Object kept) {
    return 'The file came from somewhere other than the verified server, so it wasn\'t installed.\n$kept\nSave the server address again and check for updates.';
  }

  @override
  String get installationStoppedItsUnknownWhether =>
      'Installation stopped, and it\'s unknown whether it was rolled back.\nCheck the Host service status, then reinstall with the installer.';

  @override
  String get installationStoppedReinstallApp =>
      'Installation stopped, and it\'s unknown whether it was rolled back.\nReinstall with the installer.';

  @override
  String get problemOccurredDuringInstallationSo =>
      'A problem occurred during installation, so the Host was rolled back. Host registration and virtual display settings are unchanged.';

  @override
  String get problemOccurredDuringInstallationSo2 =>
      'A problem occurred during installation, so the previous version was reopened. Server address and sign-in settings are unchanged.';

  @override
  String get couldntCompleteInstallationItsUnknown =>
      'Couldn\'t complete the installation.\nIt\'s unknown where it stopped or whether it was rolled back.\nTry again or install with the installer.';

  @override
  String couldntStartInstallerTryAgain(Object kept) {
    return 'Couldn\'t start the installer. Try again. $kept';
  }

  @override
  String updated(Object product, Object toVersion) {
    return '$product updated to $toVersion';
  }

  @override
  String get windowsNeedsRestartSaveWork =>
      'Windows needs to restart. Save your work, then restart it yourself.';

  @override
  String get checkDiagnosticsHostSettings =>
      'Check diagnostics in Host settings.';

  @override
  String get installationFinishedButSomeItems =>
      'Installation finished, but some items need checking.';

  @override
  String get hostServiceRunningNewVersion =>
      'The Host service is running the new version.';

  @override
  String get reopenedNewVersionSettingsUnchanged =>
      'Reopened with the new version. Your settings are unchanged.';

  @override
  String get viewChanges => 'View changes';

  @override
  String stoppedAfterStep6(Object stage) {
    return ' · stopped after step $stage/6';
  }

  @override
  String code4(Object code, Object notice) {
    return 'Code $code$notice';
  }

  @override
  String get restartPcNow => 'Restart your PC now?';

  @override
  String get saveOpenWorkFirstUnsaved =>
      'Save your open work first.\nUnsaved work may be lost. The desktop connection also closes.';

  @override
  String get couldntRequestRestartRestartFrom =>
      'Couldn\'t request a restart. Restart from the Windows Start menu.';

  @override
  String get installUsbDriver => 'Install USB driver';

  @override
  String get installsDriverPcNeedsUsb =>
      'Installs the driver your PC needs for USB connections.\nIf Windows asks for administrator approval, choose ‘Yes’.';

  @override
  String get onlyInstallersVerifiedSignatureUsed =>
      'Only installers with a verified signature are used.\nIf your PC needs a restart, you\'re told after installing. It doesn\'t restart automatically.';

  @override
  String get disconnectUsb => 'Disconnect USB';

  @override
  String get ifDesktopUsingDeviceUnsaved =>
      'If the desktop is using this device, unsaved data may be lost.\nOnce disconnected, the device returns to your PC.';

  @override
  String get hideSupportedDevices => 'Hide supported devices';

  @override
  String get supportedDevices => 'Supported devices';

  @override
  String get connectedDeviceCanOnlyUsed =>
      'A connected device can only be used on the desktop.\nDisconnect it to return it to your PC.';

  @override
  String get keyboardsMiceHubsBluetoothAudio =>
      'Keyboards, mice, hubs, Bluetooth, audio and cameras are not listed.';

  @override
  String get restartNow => 'Restart now';

  @override
  String get installsAfterVerifyingInstallersSignature =>
      'Installs after verifying the installer\'s signature';

  @override
  String get installing2 => 'Installing…';

  @override
  String get installUsbDriver2 => 'Install USB driver…';

  @override
  String get lookingDevices => 'Looking for devices…';

  @override
  String get noUsbDevicesConnect => 'No USB devices to connect.';

  @override
  String devicesPcNeedsCantConnected(Object hiddenNeeded) {
    return 'Devices your PC needs can\'t be connected · $hiddenNeeded hidden';
  }

  @override
  String get reconnectingUsb => 'Reconnecting USB.';

  @override
  String get usbDisconnectedReconnectDesktopUse =>
      'USB disconnected. Reconnect to the desktop to use it.';

  @override
  String get desktopGettingReadyReceiveUsb =>
      'The desktop is getting ready to receive USB devices…';

  @override
  String get pcCantConnectUsbDevices =>
      'Your PC can\'t connect USB devices to the desktop.';

  @override
  String get restartRequestedWindowsAsksIf =>
      'Restart requested. Windows asks if there\'s unsaved work.';

  @override
  String get usbDriverNeedsInstalled => 'The USB driver needs to be installed.';

  @override
  String text36(num count) {
    String _temp0 = intl.Intl.pluralLogic(
      count,
      locale: localeName,
      other: '$count other USB devices',
      one: '1 other USB device',
    );
    return '$_temp0';
  }

  @override
  String otherUsbDevices(Object count) {
    return 'Other USB devices ($count)';
  }

  @override
  String get justPlugged => 'Just plugged in';

  @override
  String get checkingOpenFiles => 'Checking for open files…';

  @override
  String get connectingDesktop2 => 'Connecting to the desktop…';

  @override
  String get connectedDesktopNotAvailablePc =>
      'Connected to the desktop · not available on your PC now';

  @override
  String get openFileCheckTimedOut =>
      'Open file check timed out. Waiting for work on your PC to finish…';

  @override
  String get deviceConnectionTimedOutChecking =>
      'Device connection timed out. Checking whether it returned to your PC…';

  @override
  String get checkingDevicePc => 'Checking the device on your PC…';

  @override
  String get returningDevicePc => 'Returning the device to your PC…';

  @override
  String programUsing(Object code, Object blocker) {
    return '$code Program using it: $blocker';
  }

  @override
  String get movingFromAnotherWindow => 'Moving from another window…';

  @override
  String movingFromWindow(Object from) {
    return 'Moving from the $from window…';
  }

  @override
  String get reconnectingUsb2 => 'Reconnecting USB';

  @override
  String get youCanConnectOnceDesktop =>
      'You can connect once the desktop is ready';

  @override
  String get deviceUseElsewhere => 'This device is in use elsewhere';

  @override
  String get anotherProgramUsingDevice2 =>
      'Another program is using this device';

  @override
  String get movesWindowsDesktop => 'Moves it to this window\'s desktop';

  @override
  String get moveWindow => 'Move to this window';

  @override
  String get usbMovingHere => 'Moving';

  @override
  String get moveUsbHereTitle => 'Move to this window?';

  @override
  String get moveUsbHereMessage =>
      'If the other window\'s desktop is using this device, unsaved data may be lost.\nOnce moved, this window\'s desktop can use it.';

  @override
  String get moveUsbHereConfirm => 'Move';

  @override
  String usbSuperSpeedDevices(Object count) {
    return '$count USB 3.0 device(s) in a USB 3.0 port can\'t be connected. Plug into a USB 2.0 port.';
  }

  @override
  String get returnPc => 'Return to PC';

  @override
  String get returning => 'Returning';

  @override
  String get connectsUseDesktop => 'Connects it for use on the desktop';

  @override
  String get securityTokenSmartCard => 'Security token / smart card';

  @override
  String get ifDriveDoesntAppearDesktop =>
      'If the drive doesn\'t appear on the desktop, check ‘Disk Management’.';

  @override
  String get pcHasNoReadableDrive =>
      'Your PC has no readable drive. You may also need to check ‘Disk Management’ on the desktop.';

  @override
  String get useSafelyRemoveHardwareDesktop =>
      'Use ‘Safely Remove Hardware’ on the desktop before disconnecting. Disconnecting while it\'s in use can damage files.';

  @override
  String get couldntConfirmNumberMonitorsChanged =>
      'Couldn\'t confirm the number of monitors changed. Check in the desktop list.';

  @override
  String get connectionClosesReconnectAfterNumber =>
      'The connection closes. Reconnect after the number of monitors changes.';

  @override
  String get disconnectApply => 'Disconnect and apply';

  @override
  String get n1Monitor => '1 monitor';

  @override
  String get n2Monitors => '2 monitors';

  @override
  String get numberMonitorsKeptDesktopWhich =>
      'Number of monitors kept on in the desktop. Which screen you view and picture quality are set separately.';

  @override
  String get changingNumberMonitors2 => 'Changing the number of monitors…';

  @override
  String switchLabel2(num count) {
    String _temp0 = intl.Intl.pluralLogic(
      count,
      locale: localeName,
      other: '$count monitors',
      one: '1 monitor',
    );
    return 'Switch to $_temp0?';
  }

  @override
  String get measuredLocalPc => 'Measured on the local PC';

  @override
  String get measuredInsideDesktop => 'Measured inside the desktop';

  @override
  String get fromProxmox => 'From Proxmox';

  @override
  String cpu(num vcpus) {
    String _temp0 = intl.Intl.pluralLogic(
      vcpus,
      locale: localeName,
      other: '$vcpus cores',
      one: '1 core',
    );
    return 'CPU $_temp0';
  }

  @override
  String memory(Object memoryTotalBytes) {
    return 'Memory $memoryTotalBytes';
  }

  @override
  String get justStarted => 'Just started';

  @override
  String min2(Object dInMinutes) {
    return '$dInMinutes min';
  }

  @override
  String hr(Object dInHours) {
    return '$dInHours hr';
  }

  @override
  String text37(num dInDays) {
    String _temp0 = intl.Intl.pluralLogic(
      dInDays,
      locale: localeName,
      other: '$dInDays days',
      one: '1 day',
    );
    return '$_temp0';
  }

  @override
  String get under1Min => 'Under 1 min';

  @override
  String hrMin(Object dInHours, Object minutes) {
    return '$dInHours hr $minutes min';
  }

  @override
  String hr2(num dInDays, Object hours) {
    String _temp0 = intl.Intl.pluralLogic(
      dInDays,
      locale: localeName,
      other: '$dInDays days',
      one: '1 day',
    );
    return '$_temp0 $hours hr';
  }

  @override
  String get memoryValueProxmoxReportsSo =>
      'Memory is the value Proxmox reports, so it may differ from Windows Task Manager.';

  @override
  String text38(num vcpus) {
    String _temp0 = intl.Intl.pluralLogic(
      vcpus,
      locale: localeName,
      other: '$vcpus cores',
      one: '1 core',
    );
    return ' · $_temp0';
  }

  @override
  String get previousValue => 'Previous value';

  @override
  String get couldntGetLatestStatus => '\nCouldn\'t get the latest status.';

  @override
  String get memory2 => 'Memory';

  @override
  String onFor(Object uptime) {
    return 'On for $uptime';
  }

  @override
  String get sampleValuesDesignNotMeasured =>
      'Sample values for the design. Not measured on a real desktop.';

  @override
  String get gettingCpuMemoryUsage => 'Getting CPU and memory usage.';

  @override
  String text39(num vcpus) {
    String _temp0 = intl.Intl.pluralLogic(
      vcpus,
      locale: localeName,
      other: '$vcpus cores',
      one: '1 core',
    );
    return ' ($_temp0)';
  }

  @override
  String memory3(Object used, Object total) {
    return 'Memory $used$total';
  }

  @override
  String get couldntGetCpuMemoryInfo =>
      'Couldn\'t get CPU and memory info from the local PC.';

  @override
  String get usageShowsWhileIts => 'Usage shows while it\'s on.';

  @override
  String get couldntGetCpuMemoryInfo2 =>
      'Couldn\'t get CPU and memory info from Proxmox.';

  @override
  String configuredSpecsUsageShowsWhile(Object spec) {
    return 'Configured specs · $spec\nUsage shows while it\'s on.';
  }

  @override
  String get specs => 'Specs';

  @override
  String get configuredSpecs => 'Configured specs';

  @override
  String couldntGetLatestUsageShows(Object pc, Object spec) {
    return '$pc · $spec\nCouldn\'t get the latest usage. It shows again once available.';
  }

  @override
  String used(Object cpu) {
    return ' $cpu used';
  }

  @override
  String used2(Object used) {
    return ' $used used';
  }

  @override
  String measuring(Object cpuLabel, Object memoryLabel) {
    return 'Measuring $cpuLabel, $memoryLabel';
  }

  @override
  String get measuring2 => 'Measuring';

  @override
  String newV(Object update) {
    return 'New v$update';
  }

  @override
  String newAppVersionV(Object update) {
    return 'New app version v$update';
  }

  @override
  String get updatesCantCheckedDemo => 'Updates can\'t be checked in the demo.';

  @override
  String get checkAppUpdates => 'Check for app updates';

  @override
  String get closeDemo => 'Close demo';

  @override
  String get quitsMultitapCompletelyInsteadKeeping =>
      'Quits Multitap completely instead of keeping it in the tray.';

  @override
  String get autoConnectStoppedBecauseConnection =>
      'Auto-connect stopped because the connection wasn\'t ready within 5 minutes. Check Details for the status.';

  @override
  String get opensDesktopsOwnScreenApp =>
      'Opens this desktop\'s own screen in the app.\nUse it when Windows is signed out or locked.';

  @override
  String get broughtDesktopsOpenWindowFront =>
      'Brought this desktop\'s open window to the front. To open it another way, close that window first.';

  @override
  String get connectionReadinessChangedCheckList =>
      'Connection readiness changed. Check the list again.';

  @override
  String connect5(Object currentName) {
    return 'Connect to $currentName?';
  }

  @override
  String get takeOverConnect => 'Take over and connect';

  @override
  String get couldntOpenRemoteWindowTry =>
      'Couldn\'t open the remote window. Try again.';

  @override
  String get openRemoteWindowsCloseToo =>
      'Open remote windows close too.\nThe desktop and running work stay as they are.';

  @override
  String localPc4(Object machineName) {
    return '$machineName: The local PC is on.';
  }

  @override
  String windowsReceivedRequest(Object machineName, Object label) {
    return '$machineName: Windows received the $label request.';
  }

  @override
  String finished(Object machineName, Object label) {
    return '$machineName: $label finished.';
  }

  @override
  String couldntProcessRequestTryAgain3(Object label) {
    return 'Couldn\'t process the $label request. Try again in a moment.';
  }

  @override
  String hostUpdateStartsWhenConnection(
    Object machineName,
    Object latestVersion,
  ) {
    return '$machineName: The Host $latestVersion update starts when the connection ends.';
  }

  @override
  String hostUpdateRequested(Object machineName, Object latestVersion) {
    return '$machineName: Host $latestVersion update requested.';
  }

  @override
  String get couldntRequestHostUpdateTry =>
      'Couldn\'t request the Host update. Try again in a moment.';

  @override
  String get allMonitorsWorksOnlyWhen =>
      '‘All monitors’ works only when your PC has 2 monitors and this desktop\'s Host supports it.';

  @override
  String get allMonitorsWorksOnlyWhen2 =>
      '‘All monitors’ works only when your PC has 2 monitors.';

  @override
  String get checkingPowerStatusRefreshMoment =>
      'Checking power status. Refresh in a moment.';

  @override
  String get refreshingList => 'Refreshing the list.';

  @override
  String get demoNoServer => 'Demo · no server';

  @override
  String get refreshingListTryAgainMoment =>
      'Refreshing the list. Try again in a moment.';

  @override
  String get refreshingList2 => 'Refreshing the list';

  @override
  String get askAdministratorAssignYouDesktop =>
      'Ask your administrator to assign you a desktop.';

  @override
  String get sampleAssignmentsNotConnectedYet =>
      'Sample assignments · not connected yet';

  @override
  String get couldntLoadDesktopList => 'Couldn\'t load the desktop list.';

  @override
  String get language => 'Language';

  @override
  String get languageSaveFailed => 'Couldn\'t save the language. Choose again.';

  @override
  String get localPcInSentence => 'local PC';

  @override
  String get desktopInSentence => 'desktop';

  @override
  String get setUpLabel => 'Set up';

  @override
  String get restoringVirtualDisplayShort => 'Restoring virtual display';

  @override
  String get settingUpVirtualDisplayShort => 'Setting up virtual display';

  @override
  String get appliedStatus => 'Applied';

  @override
  String get quitLabel => 'Quit';

  @override
  String logCode(Object code) {
    return 'Code $code';
  }

  @override
  String logErrorCode(Object code) {
    return 'Error code $code';
  }

  @override
  String get listSeparator => ', ';

  @override
  String untilDate(Object date) {
    return 'until $date';
  }

  @override
  String get languageNameKorean => '한국어';

  @override
  String get languageNameEnglish => 'English';

  @override
  String get languageHint => 'Auto follows the system language.';

  @override
  String get mfaSettingHint =>
      'Also asks for an authenticator app code when you sign in.';

  @override
  String get passwordSettingHint => 'Changes your Multitap sign-in password.';

  @override
  String get hostDoesntMatchRegisteredVm =>
      'This desktop\'s Host doesn\'t match its registered VM. Let your administrator know.';

  @override
  String get desktopRegistrationChangedRefresh =>
      'This desktop\'s registration changed. Refresh the desktop list and connect again.';

  @override
  String get proxmoxNoLongerListsVm =>
      'Proxmox no longer lists this desktop\'s VM. Let your administrator know.';

  @override
  String progressStepOf(Object index, Object count) {
    return 'Step $index of $count';
  }

  @override
  String get androidSigningOut => 'Signing out';

  @override
  String get androidHostUpdateRequesting => 'Requesting the update';

  @override
  String get androidHostUpdateReading =>
      'Checking the Host release information';

  @override
  String get androidCheckingOtherConnection =>
      'Checking for another connection';

  @override
  String get androidCheckingDesktopAgain => 'Checking the desktop again';

  @override
  String get androidOpeningConsole => 'Opening the Console';

  @override
  String get androidEndingConsole => 'Ending the Console connection';

  @override
  String get androidUpdateCheckingPackage =>
      'Checking the setup file\'s signature';

  @override
  String get androidUpdatePreparingInstall => 'Preparing the installation';

  @override
  String get androidUpdateWaitingConfirm =>
      'Waiting for your confirmation in the Android installer';

  @override
  String get androidUpdateInstallingNow =>
      'Android is installing the update.\nThe app closes when it finishes. Open it again to start the new version.';

  @override
  String get androidReadingFolder => 'Reading the folder';

  @override
  String get androidChangingPassword => 'Changing the password';

  @override
  String get androidTestingServer => 'Testing the server connection';

  @override
  String get androidMouseLockConnecting =>
      'Connecting the mouse to the desktop';

  @override
  String get tileOpeningConsole => 'Opening the Console';

  @override
  String get tileOpeningWindow => 'Opening the remote window';

  @override
  String get tileRequestingHostUpdate => 'Requesting the Host update';

  @override
  String get signingOutNow => 'Signing out';

  @override
  String get loadingDesktopList => 'Loading your desktops';

  @override
  String get updateClosingRemoteWindows => 'Closing open remote windows';

  @override
  String get sessionEndClosingConnections => 'Closing open connections';

  @override
  String get hostStepDisplayPackage =>
      'Checking that the driver files are the originals';

  @override
  String get hostStepDisplayTrust => 'Checking the driver\'s signature';

  @override
  String get hostStepDisplayDevice =>
      'Installing the virtual display driver. The screen may flicker briefly.';

  @override
  String get hostStepDisplayService =>
      'Waiting for the Host service to open the virtual display';

  @override
  String get hostStepDisplayRestart => 'Restarting the Host service';

  @override
  String get hostStepWinfspPackage => 'Checking the WinFsp setup file';

  @override
  String get hostStepWinfspInstall => 'Installing WinFsp';

  @override
  String get hostStepWinfspRollback => 'Undoing the WinFsp installation';

  @override
  String get hostStepWinfspWaiting =>
      'Waiting for another installation to finish';

  @override
  String get hostStepWinfspCheck => 'Checking the installed WinFsp';

  @override
  String get hostStepUsbRemove => 'Removing the other USB connection feature';

  @override
  String get hostStepUsbInstall =>
      'Installing the Multitap USB connection feature';

  @override
  String get hostPhaseTestingServer =>
      'Connecting to the server and waiting for its answer.';

  @override
  String get hostPhaseRequestingServer =>
      'Sending the connection request to the server and waiting for its answer.';

  @override
  String get hostPhaseRestartingService =>
      'Stopping the Host service and starting it again. Do not close this window.';

  @override
  String get hostCheckingInstallResult => 'Checking the installation';

  @override
  String get hostPhaseCheckingInstallResult =>
      'Checking in Windows what was installed.';

  @override
  String get hostCheckingRequestResult => 'Checking the request\'s result';

  @override
  String get hostPhaseCheckingRequestResult =>
      'Reading how the Windows auto sign-in request ended.';

  @override
  String get hostApprovalWaitingLine =>
      'Waiting for approval. This window moves on as soon as it is approved.';

  @override
  String hostProgressPercent(Object percent) {
    return '$percent%';
  }

  @override
  String usbUnreadableDevices(Object count) {
    return '$count USB device(s) could not be read. Unplug and plug in again.';
  }

  @override
  String get audioFromHost => 'Audio sent by Host';

  @override
  String audioPacketsPerSecond(Object count) {
    return '$count/s';
  }

  @override
  String get audioNoneSent => 'None sent';

  @override
  String get audioDecoder => 'Audio decoder';

  @override
  String get audioOpusBuiltIn => 'Opus (built in)';

  @override
  String get audioDecoderRetrying => 'None · retrying';

  @override
  String get audioDecoderPcmNext => 'None · PCM from next connection';

  @override
  String get audioFocus => 'Audio focus';

  @override
  String get audioFocusHeld => 'Held';

  @override
  String get audioFocusWaiting => 'Waiting (muted)';

  @override
  String get audioFocusPaused => 'Paused (muted)';

  @override
  String get audioFocusLost => 'Taken by another app (muted)';

  @override
  String get deviceVolume => 'Device volume';

  @override
  String deviceVolumeSilent(Object volume) {
    return '$volume (silent)';
  }

  @override
  String get audioPlayback => 'Audio playback';

  @override
  String audioPlayingUnderruns(Object count) {
    return 'Playing · $count underruns';
  }

  @override
  String audioSilentUnderruns(Object count) {
    return 'Silent · $count underruns';
  }

  @override
  String audioThreadErrorsSuffix(Object text, Object count) {
    return '$text · $count errors';
  }

  @override
  String get otherAppUsingSound =>
      'Another app is using sound. It comes back when that app is done.';
}
