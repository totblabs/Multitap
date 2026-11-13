import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:intl/intl.dart' as intl;

import 'app_localizations_en.dart';
import 'app_localizations_ko.dart';

// ignore_for_file: type=lint

/// Callers can lookup localized strings with an instance of AppLocalizations
/// returned by `AppLocalizations.of(context)`.
///
/// Applications need to include `AppLocalizations.delegate()` in their app's
/// `localizationDelegates` list, and the locales they support in the app's
/// `supportedLocales` list. For example:
///
/// ```dart
/// import 'l10n/app_localizations.dart';
///
/// return MaterialApp(
///   localizationsDelegates: AppLocalizations.localizationsDelegates,
///   supportedLocales: AppLocalizations.supportedLocales,
///   home: MyApplicationHome(),
/// );
/// ```
///
/// ## Update pubspec.yaml
///
/// Please make sure to update your pubspec.yaml to include the following
/// packages:
///
/// ```yaml
/// dependencies:
///   # Internationalization support.
///   flutter_localizations:
///     sdk: flutter
///   intl: any # Use the pinned version from flutter_localizations
///
///   # Rest of dependencies
/// ```
///
/// ## iOS Applications
///
/// iOS applications define key application metadata, including supported
/// locales, in an Info.plist file that is built into the application bundle.
/// To configure the locales supported by your app, you’ll need to edit this
/// file.
///
/// First, open your project’s ios/Runner.xcworkspace Xcode workspace file.
/// Then, in the Project Navigator, open the Info.plist file under the Runner
/// project’s Runner folder.
///
/// Next, select the Information Property List item, select Add Item from the
/// Editor menu, then select Localizations from the pop-up menu.
///
/// Select and expand the newly-created Localizations item then, for each
/// locale your application supports, add a new item and select the locale
/// you wish to add from the pop-up menu in the Value field. This list should
/// be consistent with the languages listed in the AppLocalizations.supportedLocales
/// property.
abstract class AppLocalizations {
  AppLocalizations(String locale)
    : localeName = intl.Intl.canonicalizedLocale(locale.toString());

  final String localeName;

  static AppLocalizations of(BuildContext context) {
    return Localizations.of<AppLocalizations>(context, AppLocalizations)!;
  }

  static const LocalizationsDelegate<AppLocalizations> delegate =
      _AppLocalizationsDelegate();

  /// A list of this localizations delegate along with the default localizations
  /// delegates.
  ///
  /// Returns a list of localizations delegates containing this delegate along with
  /// GlobalMaterialLocalizations.delegate, GlobalCupertinoLocalizations.delegate,
  /// and GlobalWidgetsLocalizations.delegate.
  ///
  /// Additional delegates can be added by appending to this list in
  /// MaterialApp. This list does not have to be used at all if a custom list
  /// of delegates is preferred or required.
  static const List<LocalizationsDelegate<dynamic>> localizationsDelegates =
      <LocalizationsDelegate<dynamic>>[
        delegate,
        GlobalMaterialLocalizations.delegate,
        GlobalCupertinoLocalizations.delegate,
        GlobalWidgetsLocalizations.delegate,
      ];

  /// A list of this localizations delegate's supported locales.
  static const List<Locale> supportedLocales = <Locale>[
    Locale('en'),
    Locale('ko'),
  ];

  /// No description provided for @couldntSavePreviewSetting.
  ///
  /// In ko, this message translates to:
  /// **'미리보기 설정을 저장하지 못했어요.'**
  String get couldntSavePreviewSetting;

  /// No description provided for @quitApp.
  ///
  /// In ko, this message translates to:
  /// **'앱을 완전히 종료할까요?'**
  String get quitApp;

  /// No description provided for @appClosesRemovedFromRecent.
  ///
  /// In ko, this message translates to:
  /// **'앱을 닫고 최근 앱 목록에서 지워요.\n데스크톱은 계속 켜져 있어요.'**
  String get appClosesRemovedFromRecent;

  /// No description provided for @quit.
  ///
  /// In ko, this message translates to:
  /// **'완전 종료'**
  String get quit;

  /// No description provided for @disconnectRemoteScreenThenQuit.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면에서 연결을 끊은 뒤 종료해 주세요.'**
  String get disconnectRemoteScreenThenQuit;

  /// No description provided for @couldntQuitAppTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'앱을 종료하지 못했어요. 다시 시도하세요.'**
  String get couldntQuitAppTryAgain;

  /// No description provided for @desktopNoLongerAssignedYou.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱은 더 이상 내게 배정돼 있지 않아요.'**
  String get desktopNoLongerAssignedYou;

  /// No description provided for @couldntChangeFavoritesTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'즐겨찾기를 바꾸지 못했어요. 잠시 후 다시 시도하세요.'**
  String get couldntChangeFavoritesTryAgain;

  /// No description provided for @updateInstalled.
  ///
  /// In ko, this message translates to:
  /// **'업데이트를 설치했어요.'**
  String get updateInstalled;

  /// No description provided for @couldntLoadSavedServerSettings.
  ///
  /// In ko, this message translates to:
  /// **'저장된 서버 설정을 불러오지 못했어요. 서버 설정에서 주소를 다시 입력하세요.'**
  String get couldntLoadSavedServerSettings;

  /// No description provided for @couldntLoadSavedSignDetails.
  ///
  /// In ko, this message translates to:
  /// **'저장된 로그인 정보를 불러오지 못했어요. 아이디와 비밀번호를 입력하세요.'**
  String get couldntLoadSavedSignDetails;

  /// No description provided for @couldntLoadServerSettings.
  ///
  /// In ko, this message translates to:
  /// **'서버 설정을 불러오지 못했어요.'**
  String get couldntLoadServerSettings;

  /// No description provided for @couldntTurnOffRememberPassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 기억을 해제하지 못했어요.'**
  String get couldntTurnOffRememberPassword;

  /// No description provided for @enterIdPassword.
  ///
  /// In ko, this message translates to:
  /// **'아이디와 비밀번호를 입력해 주세요.'**
  String get enterIdPassword;

  /// No description provided for @signedButCouldntSaveRemember.
  ///
  /// In ko, this message translates to:
  /// **'로그인했지만 아이디·비밀번호 기억 설정을 저장하지 못했어요.'**
  String get signedButCouldntSaveRemember;

  /// No description provided for @signHasExpiredSignAgain.
  ///
  /// In ko, this message translates to:
  /// **'로그인이 만료됐어요. 다시 로그인해 주세요.'**
  String get signHasExpiredSignAgain;

  /// No description provided for @signOut.
  ///
  /// In ko, this message translates to:
  /// **'로그아웃할까요?'**
  String get signOut;

  /// No description provided for @server.
  ///
  /// In ko, this message translates to:
  /// **'서버'**
  String get server;

  /// No description provided for @desktopsRunningWorkStayAs.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱과 실행 중인 작업은 그대로 유지돼요.'**
  String get desktopsRunningWorkStayAs;

  /// No description provided for @signOut2.
  ///
  /// In ko, this message translates to:
  /// **'로그아웃'**
  String get signOut2;

  /// No description provided for @notificationsOffSoConnectionStatus.
  ///
  /// In ko, this message translates to:
  /// **'알림이 꺼져 있어 알림창에 연결 상태와 ‘연결 끊기’ 버튼이 보이지 않아요.'**
  String get notificationsOffSoConnectionStatus;

  /// No description provided for @notificationSettings.
  ///
  /// In ko, this message translates to:
  /// **'알림 설정'**
  String get notificationSettings;

  /// No description provided for @couldntProcessRequestCheckNetwork.
  ///
  /// In ko, this message translates to:
  /// **'요청을 처리하지 못했어요. 네트워크를 확인하고 다시 시도하세요.'**
  String get couldntProcessRequestCheckNetwork;

  /// No description provided for @anotherConnectionOpening.
  ///
  /// In ko, this message translates to:
  /// **'다른 연결을 여는 중이에요.'**
  String get anotherConnectionOpening;

  /// No description provided for @desktopTaskProgress.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 작업이 진행 중이에요.'**
  String get desktopTaskProgress;

  /// No description provided for @startDesktopThenConnect.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱을 켠 뒤 연결하세요.'**
  String get startDesktopThenConnect;

  /// No description provided for @endCurrentConnectionThenConnect.
  ///
  /// In ko, this message translates to:
  /// **'기존 연결을 끊은 뒤 연결하세요.'**
  String get endCurrentConnectionThenConnect;

  /// No description provided for @disconnectConsoleConnect.
  ///
  /// In ko, this message translates to:
  /// **'Console을 끊고 연결할까요?'**
  String get disconnectConsoleConnect;

  /// No description provided for @openConsoleConnectionWillEnd.
  ///
  /// In ko, this message translates to:
  /// **'지금 열려 있는 Console 연결이 끊겨요.\n데스크톱과 실행 중인 작업은 그대로예요.'**
  String get openConsoleConnectionWillEnd;

  /// No description provided for @disconnectConnect.
  ///
  /// In ko, this message translates to:
  /// **'끊고 연결'**
  String get disconnectConnect;

  /// No description provided for @couldntEndConsoleConnection.
  ///
  /// In ko, this message translates to:
  /// **'Console 연결을 끊지 못했어요.'**
  String get couldntEndConsoleConnection;

  /// No description provided for @openConsole.
  ///
  /// In ko, this message translates to:
  /// **'Console로 열까요?'**
  String get openConsole;

  /// No description provided for @opensDesktopsProxmoxConsoleApp.
  ///
  /// In ko, this message translates to:
  /// **'{desktopName}\n\n앱 안에서 데스크톱의 Proxmox 콘솔 화면을 열어요.\nProxmox에 따로 로그인하지 않아도 돼요.'**
  String opensDesktopsProxmoxConsoleApp(Object desktopName);

  /// No description provided for @openConsole2.
  ///
  /// In ko, this message translates to:
  /// **'Console로 열기'**
  String get openConsole2;

  /// No description provided for @couldntOpenConsoleCheckNetwork.
  ///
  /// In ko, this message translates to:
  /// **'Console을 열지 못했어요. 네트워크를 확인하고 다시 시도하세요.'**
  String get couldntOpenConsoleCheckNetwork;

  /// No description provided for @connect.
  ///
  /// In ko, this message translates to:
  /// **'{assignedName}에 연결할까요?'**
  String connect(Object assignedName);

  /// No description provided for @connect2.
  ///
  /// In ko, this message translates to:
  /// **'연결'**
  String get connect2;

  /// No description provided for @couldntSaveStreamingSettingsThey.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 설정을 저장하지 못했어요. 이번 연결에는 적용해요.'**
  String get couldntSaveStreamingSettingsThey;

  /// No description provided for @bringOverWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면을 가져올까요?'**
  String get bringOverWindowsScreen;

  /// No description provided for @continuesWindowsScreenYouWere.
  ///
  /// In ko, this message translates to:
  /// **'{desktopName}\nRDP에서 쓰던 Windows 화면으로 이어서 연결해요.'**
  String continuesWindowsScreenYouWere(Object desktopName);

  /// No description provided for @bringOverConnect.
  ///
  /// In ko, this message translates to:
  /// **'가져와 연결'**
  String get bringOverConnect;

  /// No description provided for @waitingPreviousConnectionCleanUp.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결 정리를 기다리는 중이라 {label} 요청을 받지 않았어요. 잠시 후 다시 시도하세요.'**
  String waitingPreviousConnectionCleanUp(Object label);

  /// No description provided for @numberMonitorsChangingSoRequest.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수를 바꾸는 중이라 {label} 요청을 받지 않았어요. 끝난 뒤 다시 시도하세요.'**
  String numberMonitorsChangingSoRequest(Object label);

  /// No description provided for @hostUpdateInstallingSoRequest.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트를 설치하는 중이라 {label} 요청을 받지 않았어요. 설치가 끝난 뒤 다시 시도하세요.'**
  String hostUpdateInstallingSoRequest(Object label);

  /// No description provided for @couldntProcessRequestTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'{label} 요청을 처리하지 못했어요. 다시 시도하세요.'**
  String couldntProcessRequestTryAgain(Object label);

  /// No description provided for @autoConnectStoppedBecauseDesktop.
  ///
  /// In ko, this message translates to:
  /// **'5분 안에 연결 준비가 되지 않아 자동 연결을 멈췄어요. 연결 준비 점검에서 상태를 확인하세요.'**
  String get autoConnectStoppedBecauseDesktop;

  /// No description provided for @autoConnectCanceled.
  ///
  /// In ko, this message translates to:
  /// **'자동 연결을 취소했어요.'**
  String get autoConnectCanceled;

  /// No description provided for @couldntFindDesktop.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱을 찾지 못했어요'**
  String get couldntFindDesktop;

  /// No description provided for @unknown.
  ///
  /// In ko, this message translates to:
  /// **'미확인'**
  String get unknown;

  /// No description provided for @passwordChangedSignNewPassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호를 바꿨어요. 새 비밀번호로 로그인해 주세요.'**
  String get passwordChangedSignNewPassword;

  /// No description provided for @powerTaskProgress.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업이 진행 중이에요.'**
  String get powerTaskProgress;

  /// No description provided for @serverLocalPcsHostDoesnt.
  ///
  /// In ko, this message translates to:
  /// **'이 서버나 로컬 PC의 Host는 원격 전원을 지원하지 않아요.'**
  String get serverLocalPcsHostDoesnt;

  /// No description provided for @checkingDesktopsPowerStateRefresh.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 전원 상태를 확인하는 중이에요. 잠시 후 새로고침하세요.'**
  String get checkingDesktopsPowerStateRefresh;

  /// No description provided for @numberMonitorsChangingPowerAvailable.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수를 바꾸는 중이에요. 끝나면 전원을 쓸 수 있어요.'**
  String get numberMonitorsChangingPowerAvailable;

  /// No description provided for @refresh.
  ///
  /// In ko, this message translates to:
  /// **'새로고침'**
  String get refresh;

  /// No description provided for @checkingList.
  ///
  /// In ko, this message translates to:
  /// **'목록을 확인하는 중이에요'**
  String get checkingList;

  /// No description provided for @refreshList.
  ///
  /// In ko, this message translates to:
  /// **'목록 새로고침'**
  String get refreshList;

  /// No description provided for @previews.
  ///
  /// In ko, this message translates to:
  /// **'미리보기'**
  String get previews;

  /// No description provided for @screenPreviewsTapTurnOff.
  ///
  /// In ko, this message translates to:
  /// **'화면 미리보기 켜짐 · 누르면 꺼요'**
  String get screenPreviewsTapTurnOff;

  /// No description provided for @screenPreviewsOffTapTurn.
  ///
  /// In ko, this message translates to:
  /// **'화면 미리보기 꺼짐 · 누르면 켜요'**
  String get screenPreviewsOffTapTurn;

  /// No description provided for @search.
  ///
  /// In ko, this message translates to:
  /// **'검색'**
  String get search;

  /// No description provided for @sort.
  ///
  /// In ko, this message translates to:
  /// **'정렬 · {sortLabel}'**
  String sort(Object sortLabel);

  /// No description provided for @clearAllFilters.
  ///
  /// In ko, this message translates to:
  /// **'필터를 모두 해제'**
  String get clearAllFilters;

  /// No description provided for @myDesktops.
  ///
  /// In ko, this message translates to:
  /// **'내 데스크톱'**
  String get myDesktops;

  /// No description provided for @tryAgain.
  ///
  /// In ko, this message translates to:
  /// **'다시 시도'**
  String get tryAgain;

  /// No description provided for @couldntLoadList.
  ///
  /// In ko, this message translates to:
  /// **'목록을 불러오지 못했어요'**
  String get couldntLoadList;

  /// No description provided for @noDesktopsAssignedYet.
  ///
  /// In ko, this message translates to:
  /// **'아직 배정된 데스크톱이 없어요'**
  String get noDesktopsAssignedYet;

  /// No description provided for @askAdministratorAssignDesktopPull.
  ///
  /// In ko, this message translates to:
  /// **'관리자에게 데스크톱 배정을 요청하세요.\n아래로 당기면 다시 확인해요.'**
  String get askAdministratorAssignDesktopPull;

  /// No description provided for @listMayOutDate.
  ///
  /// In ko, this message translates to:
  /// **'목록이 최신이 아닐 수 있어요. {listError}'**
  String listMayOutDate(Object listError);

  /// No description provided for @clearFilters.
  ///
  /// In ko, this message translates to:
  /// **'필터 해제'**
  String get clearFilters;

  /// No description provided for @clearSearch.
  ///
  /// In ko, this message translates to:
  /// **'검색 지우기'**
  String get clearSearch;

  /// No description provided for @connectionOkMultitapMs.
  ///
  /// In ko, this message translates to:
  /// **'연결 확인 · Multitap {version} · {elapsedMs} ms'**
  String connectionOkMultitapMs(Object version, Object elapsedMs);

  /// No description provided for @checkServerAddressPort.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소와 포트를 확인해 주세요.'**
  String get checkServerAddressPort;

  /// No description provided for @couldntSaveServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소를 저장하지 못했어요.'**
  String get couldntSaveServerAddress;

  /// No description provided for @signServer.
  ///
  /// In ko, this message translates to:
  /// **'로그인 서버'**
  String get signServer;

  /// No description provided for @httpsSecureConnection.
  ///
  /// In ko, this message translates to:
  /// **'HTTPS 보안 연결'**
  String get httpsSecureConnection;

  /// No description provided for @httpNotEncrypted.
  ///
  /// In ko, this message translates to:
  /// **'HTTP · 암호화 없음'**
  String get httpNotEncrypted;

  /// No description provided for @youCanSaveAddressAfter.
  ///
  /// In ko, this message translates to:
  /// **'테스트에 성공하면 이 주소를 저장할 수 있어요.'**
  String get youCanSaveAddressAfter;

  /// No description provided for @serverAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소'**
  String get serverAddress;

  /// No description provided for @httpsServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'https://서버주소'**
  String get httpsServerAddress;

  /// No description provided for @testConnection.
  ///
  /// In ko, this message translates to:
  /// **'연결 테스트'**
  String get testConnection;

  /// No description provided for @cancel.
  ///
  /// In ko, this message translates to:
  /// **'취소'**
  String get cancel;

  /// No description provided for @save.
  ///
  /// In ko, this message translates to:
  /// **'저장'**
  String get save;

  /// No description provided for @streamingSettings.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 설정'**
  String get streamingSettings;

  /// No description provided for @custom.
  ///
  /// In ko, this message translates to:
  /// **'사용자 설정'**
  String get custom;

  /// No description provided for @qualityMode.
  ///
  /// In ko, this message translates to:
  /// **'화질 모드 · {value}'**
  String qualityMode(Object value);

  /// No description provided for @useRecommended.
  ///
  /// In ko, this message translates to:
  /// **'권장값 사용'**
  String get useRecommended;

  /// No description provided for @workKeepsTextSharpSaver.
  ///
  /// In ko, this message translates to:
  /// **'작업은 글자를 선명하게, 절약은 전송량을 줄여요.'**
  String get workKeepsTextSharpSaver;

  /// No description provided for @advancedSettings.
  ///
  /// In ko, this message translates to:
  /// **'고급 설정'**
  String get advancedSettings;

  /// No description provided for @targetFrameRate.
  ///
  /// In ko, this message translates to:
  /// **'목표 프레임'**
  String get targetFrameRate;

  /// No description provided for @videoBitrateMbps.
  ///
  /// In ko, this message translates to:
  /// **'영상 전송률 · Mbps'**
  String get videoBitrateMbps;

  /// No description provided for @auto.
  ///
  /// In ko, this message translates to:
  /// **'자동'**
  String get auto;

  /// No description provided for @fitsOneVirtualDisplayDevices.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 1개를 이 기기 크기에 맞춰요.\n다른 스트리밍 연결은 끊어질 수 있어요.'**
  String get fitsOneVirtualDisplayDevices;

  /// No description provided for @newPasswordMust10256.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호는 10~256자이고 확인 입력과 같아야 해요.'**
  String get newPasswordMust10256;

  /// No description provided for @couldntChangePassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호를 바꾸지 못했어요.'**
  String get couldntChangePassword;

  /// No description provided for @changePassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 변경'**
  String get changePassword;

  /// No description provided for @changingSignsYouOutAll.
  ///
  /// In ko, this message translates to:
  /// **'변경하면 모든 기기에서 로그아웃돼요.'**
  String get changingSignsYouOutAll;

  /// No description provided for @currentPassword.
  ///
  /// In ko, this message translates to:
  /// **'현재 비밀번호'**
  String get currentPassword;

  /// No description provided for @newPassword.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호'**
  String get newPassword;

  /// No description provided for @confirmNewPassword.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호 확인'**
  String get confirmNewPassword;

  /// No description provided for @least10Characters.
  ///
  /// In ko, this message translates to:
  /// **'10자 이상'**
  String get least10Characters;

  /// No description provided for @change.
  ///
  /// In ko, this message translates to:
  /// **'변경'**
  String get change;

  /// No description provided for @disconnect.
  ///
  /// In ko, this message translates to:
  /// **'연결 끊기'**
  String get disconnect;

  /// No description provided for @keyboard.
  ///
  /// In ko, this message translates to:
  /// **'키보드'**
  String get keyboard;

  /// No description provided for @screen.
  ///
  /// In ko, this message translates to:
  /// **'화면'**
  String get screen;

  /// No description provided for @originalSize.
  ///
  /// In ko, this message translates to:
  /// **'원본 크기'**
  String get originalSize;

  /// No description provided for @windowsKey.
  ///
  /// In ko, this message translates to:
  /// **'Windows 키'**
  String get windowsKey;

  /// No description provided for @reconnect.
  ///
  /// In ko, this message translates to:
  /// **'다시 연결'**
  String get reconnect;

  /// No description provided for @close.
  ///
  /// In ko, this message translates to:
  /// **'닫기'**
  String get close;

  /// No description provided for @console.
  ///
  /// In ko, this message translates to:
  /// **'콘솔'**
  String get console;

  /// No description provided for @noPermissionOpenFolderChoose.
  ///
  /// In ko, this message translates to:
  /// **'이 폴더를 열 권한이 없어요. 폴더를 다시 골라 주세요.'**
  String get noPermissionOpenFolderChoose;

  /// No description provided for @cantFindFolderDeviceMay.
  ///
  /// In ko, this message translates to:
  /// **'이 기기에서 폴더를 찾을 수 없어요. 지웠거나 SD 카드·USB 저장장치가 빠졌을 수 있어요.'**
  String get cantFindFolderDeviceMay;

  /// No description provided for @filesAppTurnedOffSo.
  ///
  /// In ko, this message translates to:
  /// **'‘파일’ 앱이 꺼져 있어서 폴더를 고를 수 없어요.\n‘파일’ 앱 설정에서 ‘사용’을 누른 뒤 다시 시도하세요.'**
  String get filesAppTurnedOffSo;

  /// No description provided for @deviceHasNoFolderPicker.
  ///
  /// In ko, this message translates to:
  /// **'이 기기에는 폴더를 고르는 화면이 없어서 공유 폴더를 쓸 수 없어요.'**
  String get deviceHasNoFolderPicker;

  /// No description provided for @couldntOpenFolderPickerTry.
  ///
  /// In ko, this message translates to:
  /// **'폴더를 고르는 화면을 열지 못했어요.\n다시 시도하고, 계속되면 관리자에게 알려 주세요.'**
  String get couldntOpenFolderPickerTry;

  /// No description provided for @device.
  ///
  /// In ko, this message translates to:
  /// **'이 기기'**
  String get device;

  /// No description provided for @openFilesAppSettings.
  ///
  /// In ko, this message translates to:
  /// **'‘파일’ 앱 설정 열기'**
  String get openFilesAppSettings;

  /// No description provided for @youreAboutShareAllStorage.
  ///
  /// In ko, this message translates to:
  /// **'저장공간 전체를 공유하려고 해요.\n데스크톱의 프로그램이 이 저장공간의 모든 파일을 읽고 바꿀 수 있어요.\n꼭 필요한 폴더만 고르는 걸 권해요.'**
  String get youreAboutShareAllStorage;

  /// No description provided for @chosenFolder.
  ///
  /// In ko, this message translates to:
  /// **'고른 폴더'**
  String get chosenFolder;

  /// No description provided for @stoppedSharingBecausePermissionOpen.
  ///
  /// In ko, this message translates to:
  /// **'‘{name}’ 폴더를 열 권한이 없어져서 공유를 끝냈어요'**
  String stoppedSharingBecausePermissionOpen(Object name);

  /// No description provided for @stoppedSharingBecauseFolderCant.
  ///
  /// In ko, this message translates to:
  /// **'‘{name}’ 폴더를 찾을 수 없어서 공유를 끝냈어요'**
  String stoppedSharingBecauseFolderCant(Object name);

  /// No description provided for @cantOpenFoldersDeviceCheck.
  ///
  /// In ko, this message translates to:
  /// **'이 기기에서 폴더를 열 수 없어요. SD 카드·USB 저장장치를 확인하고 다시 시도하세요.'**
  String get cantOpenFoldersDeviceCheck;

  /// No description provided for @timesAverageMaximumOverLast.
  ///
  /// In ko, this message translates to:
  /// **'시간은 프레임이 있었던 최근 1초의 평균과 최대예요.\n화면에 실제로 보이기까지의 시간은 포함하지 않아요.'**
  String get timesAverageMaximumOverLast;

  /// No description provided for @notMeasured.
  ///
  /// In ko, this message translates to:
  /// **'미측정'**
  String get notMeasured;

  /// No description provided for @avg.
  ///
  /// In ko, this message translates to:
  /// **'평균 {average}'**
  String avg(Object average);

  /// No description provided for @avgMax.
  ///
  /// In ko, this message translates to:
  /// **'평균 {average} · 최대 {highest}'**
  String avgMax(Object average, Object highest);

  /// No description provided for @preparing.
  ///
  /// In ko, this message translates to:
  /// **'준비 중'**
  String get preparing;

  /// No description provided for @defaultFallbackAfterError.
  ///
  /// In ko, this message translates to:
  /// **'기본 설정 (오류 뒤 대체)'**
  String get defaultFallbackAfterError;

  /// No description provided for @androidLowLatency.
  ///
  /// In ko, this message translates to:
  /// **'Android 저지연'**
  String get androidLowLatency;

  /// No description provided for @realTimePriority.
  ///
  /// In ko, this message translates to:
  /// **'실시간 우선'**
  String get realTimePriority;

  /// No description provided for @vendor.
  ///
  /// In ko, this message translates to:
  /// **'제조사'**
  String get vendor;

  /// No description provided for @lowLatencyKeys.
  ///
  /// In ko, this message translates to:
  /// **'{vendorKeys} 저지연 키'**
  String lowLatencyKeys(Object vendorKeys);

  /// No description provided for @offDefault.
  ///
  /// In ko, this message translates to:
  /// **'꺼짐 (기본 설정)'**
  String get offDefault;

  /// No description provided for @onLabel.
  ///
  /// In ko, this message translates to:
  /// **'켜짐'**
  String get onLabel;

  /// No description provided for @off.
  ///
  /// In ko, this message translates to:
  /// **'꺼짐'**
  String get off;

  /// No description provided for @normal.
  ///
  /// In ko, this message translates to:
  /// **'정상'**
  String get normal;

  /// No description provided for @slightlyWarm.
  ///
  /// In ko, this message translates to:
  /// **'약간 따뜻함'**
  String get slightlyWarm;

  /// No description provided for @warm.
  ///
  /// In ko, this message translates to:
  /// **'따뜻함'**
  String get warm;

  /// No description provided for @hot.
  ///
  /// In ko, this message translates to:
  /// **'뜨거움'**
  String get hot;

  /// No description provided for @veryHot.
  ///
  /// In ko, this message translates to:
  /// **'매우 뜨거움'**
  String get veryHot;

  /// No description provided for @critical.
  ///
  /// In ko, this message translates to:
  /// **'위험'**
  String get critical;

  /// No description provided for @aboutShutDown.
  ///
  /// In ko, this message translates to:
  /// **'종료 직전'**
  String get aboutShutDown;

  /// No description provided for @highestConnection.
  ///
  /// In ko, this message translates to:
  /// **'{now} (이번 연결 최고: {top})'**
  String highestConnection(Object now, Object top);

  /// No description provided for @requested.
  ///
  /// In ko, this message translates to:
  /// **'요청함'**
  String get requested;

  /// No description provided for @requestedHighPerformanceMode.
  ///
  /// In ko, this message translates to:
  /// **'요청함 (고성능 모드)'**
  String get requestedHighPerformanceMode;

  /// No description provided for @refusedByDevice.
  ///
  /// In ko, this message translates to:
  /// **'기기가 거절함'**
  String get refusedByDevice;

  /// No description provided for @notRequested.
  ///
  /// In ko, this message translates to:
  /// **'요청 안 함'**
  String get notRequested;

  /// No description provided for @decoderLowLatency.
  ///
  /// In ko, this message translates to:
  /// **'디코더 저지연'**
  String get decoderLowLatency;

  /// No description provided for @decoderFallback.
  ///
  /// In ko, this message translates to:
  /// **'디코더 대체'**
  String get decoderFallback;

  /// No description provided for @none.
  ///
  /// In ko, this message translates to:
  /// **'없음'**
  String get none;

  /// No description provided for @text.
  ///
  /// In ko, this message translates to:
  /// **'{fallbacks}회'**
  String text(num fallbacks);

  /// No description provided for @decodeTime.
  ///
  /// In ko, this message translates to:
  /// **'디코딩 시간'**
  String get decodeTime;

  /// No description provided for @receivedDecoded.
  ///
  /// In ko, this message translates to:
  /// **'수신→디코딩 완료'**
  String get receivedDecoded;

  /// No description provided for @decodedSentScreen.
  ///
  /// In ko, this message translates to:
  /// **'디코딩 완료→화면 전달'**
  String get decodedSentScreen;

  /// No description provided for @framesReplacedWaitingDropped.
  ///
  /// In ko, this message translates to:
  /// **'프레임 (교체 · 대기 · 버림)'**
  String get framesReplacedWaitingDropped;

  /// No description provided for @audioBuffer.
  ///
  /// In ko, this message translates to:
  /// **'오디오 버퍼'**
  String get audioBuffer;

  /// No description provided for @queuedTargetBuffer.
  ///
  /// In ko, this message translates to:
  /// **'대기 {queued} · 목표 {target} · 버퍼 {buffer}'**
  String queuedTargetBuffer(Object queued, Object target, Object buffer);

  /// No description provided for @audioOutput.
  ///
  /// In ko, this message translates to:
  /// **'오디오 출력'**
  String get audioOutput;

  /// No description provided for @lowLatencyPath.
  ///
  /// In ko, this message translates to:
  /// **'저지연 경로'**
  String get lowLatencyPath;

  /// No description provided for @lowLatencyPathLatency.
  ///
  /// In ko, this message translates to:
  /// **'저지연 경로 · 지연 {output}'**
  String lowLatencyPathLatency(Object output);

  /// No description provided for @normalPath.
  ///
  /// In ko, this message translates to:
  /// **'일반 경로'**
  String get normalPath;

  /// No description provided for @normalPathLatency.
  ///
  /// In ko, this message translates to:
  /// **'일반 경로 · 지연 {output}'**
  String normalPathLatency(Object output);

  /// No description provided for @displayRefreshRate.
  ///
  /// In ko, this message translates to:
  /// **'화면 주사율'**
  String get displayRefreshRate;

  /// No description provided for @sharp.
  ///
  /// In ko, this message translates to:
  /// **'선명하게'**
  String get sharp;

  /// No description provided for @wiFiLowLatency.
  ///
  /// In ko, this message translates to:
  /// **'Wi-Fi 저지연'**
  String get wiFiLowLatency;

  /// No description provided for @deviceHeat.
  ///
  /// In ko, this message translates to:
  /// **'기기 발열'**
  String get deviceHeat;

  /// No description provided for @allowAccessAllFiles.
  ///
  /// In ko, this message translates to:
  /// **'모든 파일 접근을 허용할까요?'**
  String get allowAccessAllFiles;

  /// No description provided for @chooseFolderMultitapNeedsSee.
  ///
  /// In ko, this message translates to:
  /// **'폴더를 고르려면 Multitap이 이 기기의 저장공간을 볼 수 있어야 해요.\n허용하면 Multitap이 저장공간을 읽고 쓸 수 있어요. 데스크톱에는 고른 폴더만 보여요.'**
  String get chooseFolderMultitapNeedsSee;

  /// No description provided for @openSettings.
  ///
  /// In ko, this message translates to:
  /// **'설정 열기'**
  String get openSettings;

  /// No description provided for @cantOpenFolderChooseAnother.
  ///
  /// In ko, this message translates to:
  /// **'이 폴더는 열 수 없어요. 다른 폴더를 골라 주세요.'**
  String get cantOpenFolderChooseAnother;

  /// No description provided for @upOneFolder.
  ///
  /// In ko, this message translates to:
  /// **'위 폴더로'**
  String get upOneFolder;

  /// No description provided for @chooseFolderShare.
  ///
  /// In ko, this message translates to:
  /// **'공유할 폴더 고르기'**
  String get chooseFolderShare;

  /// No description provided for @storage.
  ///
  /// In ko, this message translates to:
  /// **'저장공간'**
  String get storage;

  /// No description provided for @couldntFindStorage.
  ///
  /// In ko, this message translates to:
  /// **'저장공간을 찾지 못했어요.'**
  String get couldntFindStorage;

  /// No description provided for @noFoldersInside.
  ///
  /// In ko, this message translates to:
  /// **'안에 폴더가 없어요.'**
  String get noFoldersInside;

  /// No description provided for @shareFolder.
  ///
  /// In ko, this message translates to:
  /// **'이 폴더 공유'**
  String get shareFolder;

  /// No description provided for @lostConnectionHostCouldntConfirm.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결이 끊겼어요. 업데이트 요청 결과는 확인하지 못했어요.'**
  String get lostConnectionHostCouldntConfirm;

  /// No description provided for @couldntConfirmResultUpdateRequest.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 요청 결과를 확인하지 못했어요. 목록을 다시 확인해 주세요.'**
  String get couldntConfirmResultUpdateRequest;

  /// No description provided for @couldntConfirmResultUpdateRequest2.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 요청 결과를 확인하지 못했어요.'**
  String get couldntConfirmResultUpdateRequest2;

  /// No description provided for @currentStatus.
  ///
  /// In ko, this message translates to:
  /// **'현재 상태 · {observed}'**
  String currentStatus(Object observed);

  /// No description provided for @lastCheckedStatus.
  ///
  /// In ko, this message translates to:
  /// **'마지막으로 확인한 상태 · {observed}'**
  String lastCheckedStatus(Object observed);

  /// No description provided for @localPc.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC'**
  String get localPc;

  /// No description provided for @desktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱'**
  String get desktop;

  /// No description provided for @serverSignStatusChangedCancel.
  ///
  /// In ko, this message translates to:
  /// **'서버·로그인 또는 {kind} 상태가 바뀌었어요. 취소한 뒤 다시 열어 주세요.'**
  String serverSignStatusChangedCancel(Object kind);

  /// No description provided for @targetListDiffersFromLatest.
  ///
  /// In ko, this message translates to:
  /// **'목록의 대상과 최신 Host 배포 버전이 달라요. 목록을 새로고침한 뒤 다시 확인해 주세요.'**
  String get targetListDiffersFromLatest;

  /// No description provided for @couldntFindLatestHostRelease.
  ///
  /// In ko, this message translates to:
  /// **'최신 Host 배포 정보를 찾지 못했어요.'**
  String get couldntFindLatestHostRelease;

  /// No description provided for @couldntCheckHostReleaseInformation.
  ///
  /// In ko, this message translates to:
  /// **'Host 배포 정보와 변경 내역을 확인하지 못했어요. 다시 확인해 주세요.'**
  String get couldntCheckHostReleaseInformation;

  /// No description provided for @hostUpdate.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트'**
  String get hostUpdate;

  /// No description provided for @currentNewVersion.
  ///
  /// In ko, this message translates to:
  /// **'현재 {currentVersion}\n새 버전 {targetVersion}'**
  String currentNewVersion(Object currentVersion, Object targetVersion);

  /// No description provided for @updateStartsWhenConnectionUse.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 연결이 끝나면 업데이트를 시작해요.'**
  String get updateStartsWhenConnectionUse;

  /// No description provided for @updatesOnlyHostProgramYou.
  ///
  /// In ko, this message translates to:
  /// **'{kind}의 Host 프로그램만 업데이트해요.\n설치 중에는 잠시 연결할 수 없어요.'**
  String updatesOnlyHostProgramYou(Object kind);

  /// No description provided for @changeLog.
  ///
  /// In ko, this message translates to:
  /// **'변경 내역'**
  String get changeLog;

  /// No description provided for @noChangeLogRegisteredServer.
  ///
  /// In ko, this message translates to:
  /// **'이 서버에 변경 내역이 등록되지 않았어요.'**
  String get noChangeLogRegisteredServer;

  /// No description provided for @checkAgain.
  ///
  /// In ko, this message translates to:
  /// **'다시 확인'**
  String get checkAgain;

  /// No description provided for @update.
  ///
  /// In ko, this message translates to:
  /// **'업데이트'**
  String get update;

  /// No description provided for @themeBackground.
  ///
  /// In ko, this message translates to:
  /// **'테마와 배경'**
  String get themeBackground;

  /// No description provided for @theme.
  ///
  /// In ko, this message translates to:
  /// **'테마를'**
  String get theme;

  /// No description provided for @background.
  ///
  /// In ko, this message translates to:
  /// **'배경을'**
  String get background;

  /// No description provided for @couldntSaveChooseAgain.
  ///
  /// In ko, this message translates to:
  /// **'{what} 저장하지 못했어요. 다시 선택해 주세요.'**
  String couldntSaveChooseAgain(Object what);

  /// No description provided for @dark.
  ///
  /// In ko, this message translates to:
  /// **'다크'**
  String get dark;

  /// No description provided for @light.
  ///
  /// In ko, this message translates to:
  /// **'라이트'**
  String get light;

  /// No description provided for @background2.
  ///
  /// In ko, this message translates to:
  /// **'배경 · {sceneLabel}'**
  String background2(Object sceneLabel);

  /// No description provided for @hostUpdates.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 {hostUpdates}대'**
  String hostUpdates(Object hostUpdates);

  /// No description provided for @newVersion.
  ///
  /// In ko, this message translates to:
  /// **'새 버전 {update}'**
  String newVersion(Object update);

  /// No description provided for @checkUpdates.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인'**
  String get checkUpdates;

  /// No description provided for @aboutLicenses.
  ///
  /// In ko, this message translates to:
  /// **'정보·라이선스'**
  String get aboutLicenses;

  /// No description provided for @androidClient.
  ///
  /// In ko, this message translates to:
  /// **'Android 클라이언트'**
  String get androidClient;

  /// No description provided for @viewLogs.
  ///
  /// In ko, this message translates to:
  /// **'로그 보기'**
  String get viewLogs;

  /// No description provided for @twoStepVerification.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증'**
  String get twoStepVerification;

  /// No description provided for @accountMenu.
  ///
  /// In ko, this message translates to:
  /// **'계정 메뉴 · {account}'**
  String accountMenu(Object account);

  /// No description provided for @idleAutoSignOut.
  ///
  /// In ko, this message translates to:
  /// **'유휴 자동 로그아웃'**
  String get idleAutoSignOut;

  /// No description provided for @serverSetupNeeded.
  ///
  /// In ko, this message translates to:
  /// **'서버 설정 필요'**
  String get serverSetupNeeded;

  /// No description provided for @changeServer.
  ///
  /// In ko, this message translates to:
  /// **'서버 변경'**
  String get changeServer;

  /// No description provided for @id.
  ///
  /// In ko, this message translates to:
  /// **'아이디'**
  String get id;

  /// No description provided for @password.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호'**
  String get password;

  /// No description provided for @rememberPassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 기억'**
  String get rememberPassword;

  /// No description provided for @passwordsRememberedOnlyHttps.
  ///
  /// In ko, this message translates to:
  /// **'HTTPS 서버에서만 비밀번호를 기억해요.'**
  String get passwordsRememberedOnlyHttps;

  /// No description provided for @passwordEncryptedKeptDevice.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호를 암호화해 이 기기에 보관해요.'**
  String get passwordEncryptedKeptDevice;

  /// No description provided for @signing.
  ///
  /// In ko, this message translates to:
  /// **'로그인 중…'**
  String get signing;

  /// No description provided for @sign.
  ///
  /// In ko, this message translates to:
  /// **'로그인'**
  String get sign;

  /// No description provided for @serverSettings.
  ///
  /// In ko, this message translates to:
  /// **'서버 설정'**
  String get serverSettings;

  /// No description provided for @askAdministratorAccount.
  ///
  /// In ko, this message translates to:
  /// **'계정은 관리자에게 요청하세요'**
  String get askAdministratorAccount;

  /// No description provided for @androidStreaming.
  ///
  /// In ko, this message translates to:
  /// **'Android · 스트리밍'**
  String get androidStreaming;

  /// No description provided for @newVersion2.
  ///
  /// In ko, this message translates to:
  /// **'새 버전 {updateVersion}'**
  String newVersion2(Object updateVersion);

  /// No description provided for @maintenanceNotice.
  ///
  /// In ko, this message translates to:
  /// **'점검 예고 · {noticeTitle}'**
  String maintenanceNotice(Object noticeTitle);

  /// No description provided for @printerConnectionEnded.
  ///
  /// In ko, this message translates to:
  /// **'프린터 연결이 끊겼어요. 아직 처리하지 않은 문서 {lostDocuments}개는 받을 수 없게 됐어요.'**
  String printerConnectionEnded(num lostDocuments);

  /// No description provided for @printerConnectionEndedReconnectPrint.
  ///
  /// In ko, this message translates to:
  /// **'프린터 연결이 끊겼어요. 다시 연결하면 인쇄할 수 있어요.'**
  String get printerConnectionEndedReconnectPrint;

  /// No description provided for @couldntOpenReceivedPrintDocument.
  ///
  /// In ko, this message translates to:
  /// **'받은 인쇄 문서를 열지 못했어요. 데스크톱에서 다시 인쇄하세요.'**
  String get couldntOpenReceivedPrintDocument;

  /// No description provided for @androidPrintingIsntAvailableCheck.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄를 사용할 수 없어요. 기기의 인쇄 서비스를 확인해 주세요.'**
  String get androidPrintingIsntAvailableCheck;

  /// No description provided for @couldntPassPrintDocumentPrint.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 문서를 전달하지 못했어요. 데스크톱에서 다시 인쇄하세요.'**
  String get couldntPassPrintDocumentPrint;

  /// No description provided for @receivingPrintDocument.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 문서를 받는 중'**
  String get receivingPrintDocument;

  /// No description provided for @receivingPrintDocument2.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 문서 수신 · {label}'**
  String receivingPrintDocument2(Object label);

  /// No description provided for @couldntKeepConnectionRunningBackground.
  ///
  /// In ko, this message translates to:
  /// **'백그라운드 연결 유지를 시작하지 못했어요. 앱을 벗어나면 연결이 끊길 수 있어요.'**
  String get couldntKeepConnectionRunningBackground;

  /// No description provided for @stoppedPreparingConnectionWhileOutside.
  ///
  /// In ko, this message translates to:
  /// **'앱 밖에서 연결 준비를 중단했어요. 앱으로 돌아오면 다시 연결해요.'**
  String get stoppedPreparingConnectionWhileOutside;

  /// No description provided for @hostsVirtualDisplayIsntReady.
  ///
  /// In ko, this message translates to:
  /// **'Host의 가상 화면 준비가 아직 끝나지 않았어요. 잠시 후 다시 연결하세요.'**
  String get hostsVirtualDisplayIsntReady;

  /// No description provided for @desktopWasntReadyWithin3.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 3분 안에 준비되지 않았어요.\n잠시 후 ‘다시 연결’을 누르세요.'**
  String get desktopWasntReadyWithin3;

  /// No description provided for @connectionEndedCouldntReconnectCheck.
  ///
  /// In ko, this message translates to:
  /// **'연결이 끊겼고 다시 연결하지 못했어요. 네트워크를 확인하고 다시 연결하세요.'**
  String get connectionEndedCouldntReconnectCheck;

  /// No description provided for @couldntChangeWindowsScaleScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 배율을 바꾸지 못했어요. 화면은 그대로 선명하게 보여요.'**
  String get couldntChangeWindowsScaleScreen;

  /// No description provided for @setLargestScaleWindowsAllows.
  ///
  /// In ko, this message translates to:
  /// **'이 화면 크기에서 Windows가 허용하는 최대 배율 {scale}%로 맞췄어요.'**
  String setLargestScaleWindowsAllows(Object scale);

  /// No description provided for @couldntSendInputWhileScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기를 맞추는 중이라 입력을 보내지 못했어요. 끝난 뒤 다시 입력하세요.'**
  String get couldntSendInputWhileScreen;

  /// No description provided for @couldntConfirmInputWasDelivered.
  ///
  /// In ko, this message translates to:
  /// **'입력 전달을 확인하지 못해 키보드를 닫았어요. 다시 열어 입력해 주세요.'**
  String get couldntConfirmInputWasDelivered;

  /// No description provided for @hostReceivesKoreanTextAs.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 한글을 키 입력으로 받아요. 데스크톱이 한글 입력 상태일 때 그대로 입력되고, 영문이 나오면 키 줄의 한/영을 누르세요.'**
  String get hostReceivesKoreanTextAs;

  /// No description provided for @controlCharactersCantSentSo.
  ///
  /// In ko, this message translates to:
  /// **'제어 문자는 보낼 수 없어서 뺐어요.'**
  String get controlCharactersCantSentSo;

  /// No description provided for @hostCantReceiveSomeCharacters.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 이모지 같은 일부 문자를 받을 수 없어요. Host를 업데이트하면 보낼 수 있어요.'**
  String get hostCantReceiveSomeCharacters;

  /// No description provided for @mouseLock.
  ///
  /// In ko, this message translates to:
  /// **'마우스 잠금'**
  String get mouseLock;

  /// No description provided for @tapOkLockMouseInside.
  ///
  /// In ko, this message translates to:
  /// **'확인을 누르면 마우스를 데스크톱 안에 잠가요.\n뒤로 가기를 누르면 풀려요.'**
  String get tapOkLockMouseInside;

  /// No description provided for @mouseLockedInsideDesktopPress.
  ///
  /// In ko, this message translates to:
  /// **'마우스를 데스크톱 안에 잠갔어요 · 뒤로 가기로 풀기'**
  String get mouseLockedInsideDesktopPress;

  /// No description provided for @someGamesSuchAsLeague.
  ///
  /// In ko, this message translates to:
  /// **'롤 같은 일부 게임에서는 마우스가 움직이지 않을 수 있어요'**
  String get someGamesSuchAsLeague;

  /// No description provided for @mouseLockOff.
  ///
  /// In ko, this message translates to:
  /// **'마우스 잠금 꺼짐'**
  String get mouseLockOff;

  /// No description provided for @couldntFitScreenSizeTap.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기를 맞추지 못했어요. 메뉴 맨 위 연결 품질에서 ‘화면 크기 다시 적용’을 누르세요.'**
  String get couldntFitScreenSizeTap;

  /// No description provided for @deviceCantReceiveSharperPicture.
  ///
  /// In ko, this message translates to:
  /// **'이 기기에서는 더 선명하게 받을 수 없어 자동 크기로 보여요.'**
  String get deviceCantReceiveSharperPicture;

  /// No description provided for @couldntApplySharpSoShows.
  ///
  /// In ko, this message translates to:
  /// **'선명하게를 적용하지 못해 자동 크기로 보여요.'**
  String get couldntApplySharpSoShows;

  /// No description provided for @stoppedWaitingHostUpdateTap.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 기다리기를 취소했어요.\n업데이트가 끝나면 ‘다시 연결’을 누르세요.'**
  String get stoppedWaitingHostUpdateTap;

  /// No description provided for @stoppedWaitingTapReconnectWhen.
  ///
  /// In ko, this message translates to:
  /// **'기다리기를 취소했어요.\n데스크톱이 준비되면 ‘다시 연결’을 누르세요.'**
  String get stoppedWaitingTapReconnectWhen;

  /// No description provided for @screenSettings.
  ///
  /// In ko, this message translates to:
  /// **'화면 설정'**
  String get screenSettings;

  /// No description provided for @applyReconnect.
  ///
  /// In ko, this message translates to:
  /// **'적용 후 다시 연결'**
  String get applyReconnect;

  /// No description provided for @hostCantChangeScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 연결 중 화면 크기 변경을 지원하지 않아요. 다음 연결부터 적용돼요.'**
  String get hostCantChangeScreenSize;

  /// No description provided for @resolutionWindowsScaleSeparate.
  ///
  /// In ko, this message translates to:
  /// **'해상도와 Windows 배율은 따로 적용돼요.'**
  String get resolutionWindowsScaleSeparate;

  /// No description provided for @resolutionWindowsScaleApplySeparately.
  ///
  /// In ko, this message translates to:
  /// **'{session}\n{session2}\n해상도와 Windows 배율은 따로 적용돼요.'**
  String resolutionWindowsScaleApplySeparately(Object session, Object session2);

  /// No description provided for @current.
  ///
  /// In ko, this message translates to:
  /// **'현재 {width}×{height}'**
  String current(Object width, Object height);

  /// No description provided for @requested2.
  ///
  /// In ko, this message translates to:
  /// **' · 요청 {width}×{height}'**
  String requested2(Object width, Object height);

  /// No description provided for @current2.
  ///
  /// In ko, this message translates to:
  /// **'현재 {currentWidth}×{currentHeight}'**
  String current2(Object currentWidth, Object currentHeight);

  /// No description provided for @checkingCurrent.
  ///
  /// In ko, this message translates to:
  /// **'현재 확인 중'**
  String get checkingCurrent;

  /// No description provided for @recommended.
  ///
  /// In ko, this message translates to:
  /// **'권장 {desired}×{desired2}{requested} · {current}'**
  String recommended(
    Object desired,
    Object desired2,
    Object requested,
    Object current,
  );

  /// No description provided for @supportedRange.
  ///
  /// In ko, this message translates to:
  /// **'지원 범위'**
  String get supportedRange;

  /// No description provided for @deviceSupport.
  ///
  /// In ko, this message translates to:
  /// **'기기 지원'**
  String get deviceSupport;

  /// No description provided for @selected.
  ///
  /// In ko, this message translates to:
  /// **'선택 {desiredWidth}×{desiredHeight}{requestNote} · {current}'**
  String selected(
    Object desiredWidth,
    Object desiredHeight,
    Object requestNote,
    Object current,
  );

  /// No description provided for @sharp2.
  ///
  /// In ko, this message translates to:
  /// **'선명하게 {width}×{height}{plan} · {current}'**
  String sharp2(Object width, Object height, Object plan, Object current);

  /// No description provided for @couldntApplySharpSoShows2.
  ///
  /// In ko, this message translates to:
  /// **'선명하게를 적용하지 못해 자동 크기로 보여요 ({sharpRefusedReason}) · {current}'**
  String couldntApplySharpSoShows2(Object sharpRefusedReason, Object current);

  /// No description provided for @checkingSharp.
  ///
  /// In ko, this message translates to:
  /// **'선명하게 확인 중 · {current}'**
  String checkingSharp(Object current);

  /// No description provided for @cantReceiveSharperPictureSo.
  ///
  /// In ko, this message translates to:
  /// **'더 선명하게 받을 수 없어 자동 크기로 보여요{planned} · {current}'**
  String cantReceiveSharperPictureSo(Object planned, Object current);

  /// No description provided for @remoteResolution.
  ///
  /// In ko, this message translates to:
  /// **'원격 해상도 {width} × {height}'**
  String remoteResolution(Object width, Object height);

  /// No description provided for @remoteResolutionUnknown.
  ///
  /// In ko, this message translates to:
  /// **'원격 해상도 미확인'**
  String get remoteResolutionUnknown;

  /// No description provided for @connected.
  ///
  /// In ko, this message translates to:
  /// **'연결됨'**
  String get connected;

  /// No description provided for @windowsScale.
  ///
  /// In ko, this message translates to:
  /// **'{session}\nWindows 배율 · {session2}'**
  String windowsScale(Object session, Object session2);

  /// No description provided for @remoteScreen.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면'**
  String get remoteScreen;

  /// No description provided for @n1Screen.
  ///
  /// In ko, this message translates to:
  /// **'화면 1개'**
  String get n1Screen;

  /// No description provided for @n2Screens.
  ///
  /// In ko, this message translates to:
  /// **'화면 2개'**
  String get n2Screens;

  /// No description provided for @screenShow.
  ///
  /// In ko, this message translates to:
  /// **'표시할 화면'**
  String get screenShow;

  /// No description provided for @screen2.
  ///
  /// In ko, this message translates to:
  /// **'화면 {intValue} · {trackId}'**
  String screen2(Object intValue, Object trackId);

  /// No description provided for @view.
  ///
  /// In ko, this message translates to:
  /// **'보기'**
  String get view;

  /// No description provided for @wholeScreen.
  ///
  /// In ko, this message translates to:
  /// **'전체 보기'**
  String get wholeScreen;

  /// No description provided for @fill.
  ///
  /// In ko, this message translates to:
  /// **'꽉 채우기'**
  String get fill;

  /// No description provided for @zoomOut.
  ///
  /// In ko, this message translates to:
  /// **'축소'**
  String get zoomOut;

  /// No description provided for @zoom.
  ///
  /// In ko, this message translates to:
  /// **'확대 {scale}%'**
  String zoom(Object scale);

  /// No description provided for @zoomsDeviceOnlyRemoteResolution.
  ///
  /// In ko, this message translates to:
  /// **'이 기기에서만 확대 · 원격 해상도는 그대로예요'**
  String get zoomsDeviceOnlyRemoteResolution;

  /// No description provided for @zoom2.
  ///
  /// In ko, this message translates to:
  /// **'확대'**
  String get zoom2;

  /// No description provided for @pan.
  ///
  /// In ko, this message translates to:
  /// **'화면 이동'**
  String get pan;

  /// No description provided for @useWhenZoomedWhenKeyboard.
  ///
  /// In ko, this message translates to:
  /// **'확대했거나 키보드가 가릴 때 써요'**
  String get useWhenZoomedWhenKeyboard;

  /// No description provided for @pointerSpeed.
  ///
  /// In ko, this message translates to:
  /// **'포인터 속도'**
  String get pointerSpeed;

  /// No description provided for @inputMode.
  ///
  /// In ko, this message translates to:
  /// **'입력 방식'**
  String get inputMode;

  /// No description provided for @streaming.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍'**
  String get streaming;

  /// No description provided for @reapplyScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기 다시 적용'**
  String get reapplyScreenSize;

  /// No description provided for @pressBackRelease.
  ///
  /// In ko, this message translates to:
  /// **'뒤로 가기로 풀어요'**
  String get pressBackRelease;

  /// No description provided for @locksMouseInsideDesktop.
  ///
  /// In ko, this message translates to:
  /// **'마우스를 데스크톱 안에 잠가요'**
  String get locksMouseInsideDesktop;

  /// No description provided for @availableWhenMouseConnected.
  ///
  /// In ko, this message translates to:
  /// **'마우스를 연결하면 쓸 수 있어요'**
  String get availableWhenMouseConnected;

  /// No description provided for @touchControlsPrinterCopy.
  ///
  /// In ko, this message translates to:
  /// **'터치 조작 · 프린터 · 복사'**
  String get touchControlsPrinterCopy;

  /// No description provided for @speed.
  ///
  /// In ko, this message translates to:
  /// **'{modeLabel} · 속도 {speedLabel}'**
  String speed(Object modeLabel, Object speedLabel);

  /// No description provided for @shortcutsAvailableOnceScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기를 맞춘 뒤 단축키를 쓸 수 있어요'**
  String get shortcutsAvailableOnceScreenSize;

  /// No description provided for @switchApps.
  ///
  /// In ko, this message translates to:
  /// **'앱 전환'**
  String get switchApps;

  /// No description provided for @taskManager.
  ///
  /// In ko, this message translates to:
  /// **'작업 관리자'**
  String get taskManager;

  /// No description provided for @edit.
  ///
  /// In ko, this message translates to:
  /// **'편집'**
  String get edit;

  /// No description provided for @copy.
  ///
  /// In ko, this message translates to:
  /// **'복사'**
  String get copy;

  /// No description provided for @paste.
  ///
  /// In ko, this message translates to:
  /// **'붙여넣기'**
  String get paste;

  /// No description provided for @selectAll.
  ///
  /// In ko, this message translates to:
  /// **'전체 선택'**
  String get selectAll;

  /// No description provided for @undo.
  ///
  /// In ko, this message translates to:
  /// **'실행 취소'**
  String get undo;

  /// No description provided for @cantSend.
  ///
  /// In ko, this message translates to:
  /// **'보낼 수 없어요'**
  String get cantSend;

  /// No description provided for @ctrlAltDelCantSent.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍에서는 Ctrl+Alt+Del을 보낼 수 없어요. 작업 관리자는 Ctrl+Shift+Esc로 열 수 있어요.'**
  String get ctrlAltDelCantSent;

  /// No description provided for @youCanChangeOnceConnected.
  ///
  /// In ko, this message translates to:
  /// **'연결된 뒤 바꿀 수 있어요'**
  String get youCanChangeOnceConnected;

  /// No description provided for @screenWholeScreenZoomPan.
  ///
  /// In ko, this message translates to:
  /// **'화면 · 전체 보기, 확대, 화면 이동'**
  String get screenWholeScreenZoomPan;

  /// No description provided for @screenViewScaleResolution.
  ///
  /// In ko, this message translates to:
  /// **'화면 · 보기, 배율, 해상도'**
  String get screenViewScaleResolution;

  /// No description provided for @text2.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 {mapped}개 연결됨'**
  String text2(num mapped);

  /// No description provided for @useDevicesFoldersDesktop.
  ///
  /// In ko, this message translates to:
  /// **'이 기기의 폴더를 데스크톱에서 쓰기'**
  String get useDevicesFoldersDesktop;

  /// No description provided for @availableOnceConnected.
  ///
  /// In ko, this message translates to:
  /// **'연결된 뒤 쓸 수 있어요'**
  String get availableOnceConnected;

  /// No description provided for @settingsPictureQualityMouse.
  ///
  /// In ko, this message translates to:
  /// **'설정 · 화질, 마우스 잠금, 입력 방식, 사용안내'**
  String get settingsPictureQualityMouse;

  /// No description provided for @screenSizeAdjustingChooseShortcut.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기를 맞추는 중이에요. 끝난 뒤 단축키를 다시 선택하세요.'**
  String get screenSizeAdjustingChooseShortcut;

  /// No description provided for @couldntConfirmShortcutWasSent.
  ///
  /// In ko, this message translates to:
  /// **'단축키 전송 결과를 확인하지 못했어요. 원격 화면을 확인한 뒤 필요하면 다시 선택해 주세요.'**
  String get couldntConfirmShortcutWasSent;

  /// No description provided for @technology.
  ///
  /// In ko, this message translates to:
  /// **'연결 기술'**
  String get technology;

  /// No description provided for @decoder.
  ///
  /// In ko, this message translates to:
  /// **'디코더'**
  String get decoder;

  /// No description provided for @hardwareDecoding.
  ///
  /// In ko, this message translates to:
  /// **'하드웨어 디코딩'**
  String get hardwareDecoding;

  /// No description provided for @onLabel2.
  ///
  /// In ko, this message translates to:
  /// **'사용'**
  String get onLabel2;

  /// No description provided for @off2.
  ///
  /// In ko, this message translates to:
  /// **'미사용'**
  String get off2;

  /// No description provided for @appCpu.
  ///
  /// In ko, this message translates to:
  /// **'앱 CPU'**
  String get appCpu;

  /// No description provided for @receivedSize.
  ///
  /// In ko, this message translates to:
  /// **'수신 화면'**
  String get receivedSize;

  /// No description provided for @remoteOriginal.
  ///
  /// In ko, this message translates to:
  /// **'원격 원본'**
  String get remoteOriginal;

  /// No description provided for @devicePixels.
  ///
  /// In ko, this message translates to:
  /// **'기기 화소'**
  String get devicePixels;

  /// No description provided for @half.
  ///
  /// In ko, this message translates to:
  /// **'절반'**
  String get half;

  /// No description provided for @fixed.
  ///
  /// In ko, this message translates to:
  /// **'고정 {shortSide}'**
  String fixed(Object shortSide);

  /// No description provided for @selected2.
  ///
  /// In ko, this message translates to:
  /// **'선택 {width}×{height}'**
  String selected2(Object width, Object height);

  /// No description provided for @selected3.
  ///
  /// In ko, this message translates to:
  /// **'선택 {desktopLabel}'**
  String selected3(Object desktopLabel);

  /// No description provided for @selected4.
  ///
  /// In ko, this message translates to:
  /// **'선택 {workspaceLabel}'**
  String selected4(Object workspaceLabel);

  /// No description provided for @windowsScale2.
  ///
  /// In ko, this message translates to:
  /// **'Windows 배율'**
  String get windowsScale2;

  /// No description provided for @pixelScale.
  ///
  /// In ko, this message translates to:
  /// **'화소 배율'**
  String get pixelScale;

  /// No description provided for @zoomFitInterpolated.
  ///
  /// In ko, this message translates to:
  /// **'확대/맞춤 (보간)'**
  String get zoomFitInterpolated;

  /// No description provided for @devicePixels1RemotePixel.
  ///
  /// In ko, this message translates to:
  /// **'기기 화소 {viewPixelScale}개 = 원격 1화소'**
  String devicePixels1RemotePixel(Object viewPixelScale);

  /// No description provided for @mouseCursor.
  ///
  /// In ko, this message translates to:
  /// **'마우스 커서'**
  String get mouseCursor;

  /// No description provided for @shownDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기에서 표시'**
  String get shownDevice;

  /// No description provided for @video.
  ///
  /// In ko, this message translates to:
  /// **'영상에 포함'**
  String get video;

  /// No description provided for @audio.
  ///
  /// In ko, this message translates to:
  /// **'오디오'**
  String get audio;

  /// No description provided for @framesNetwork.
  ///
  /// In ko, this message translates to:
  /// **'프레임과 네트워크'**
  String get framesNetwork;

  /// No description provided for @sourceCapture.
  ///
  /// In ko, this message translates to:
  /// **'소스 캡처'**
  String get sourceCapture;

  /// No description provided for @decodingDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기 디코딩'**
  String get decodingDevice;

  /// No description provided for @sentScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 전달'**
  String get sentScreen;

  /// No description provided for @receiveRate.
  ///
  /// In ko, this message translates to:
  /// **'수신 속도'**
  String get receiveRate;

  /// No description provided for @inputAppliedHostEstP50.
  ///
  /// In ko, this message translates to:
  /// **'입력→Host 적용 (추정 p50/p95)'**
  String get inputAppliedHostEstP50;

  /// No description provided for @inputNewFrameDecodedP50.
  ///
  /// In ko, this message translates to:
  /// **'입력→새 프레임 디코딩 (p50/p95)'**
  String get inputNewFrameDecodedP50;

  /// No description provided for @inputLatency.
  ///
  /// In ko, this message translates to:
  /// **'입력 지연'**
  String get inputLatency;

  /// No description provided for @hostDoesntSupportMeasuring.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 측정을 지원하지 않아요'**
  String get hostDoesntSupportMeasuring;

  /// No description provided for @audioDrops.
  ///
  /// In ko, this message translates to:
  /// **'오디오 드롭'**
  String get audioDrops;

  /// No description provided for @text3.
  ///
  /// In ko, this message translates to:
  /// **'{frames}프레임'**
  String text3(num frames);

  /// No description provided for @inputNewFrameDecodedRuns.
  ///
  /// In ko, this message translates to:
  /// **'입력→새 프레임 디코딩은 입력 송신부터 측정 대상 프레임의 MediaCodec 출력 콜백까지예요. 이후 프레임의 텍스처 전달·폐기, Flutter 합성과 실제 화면 표시는 포함하지 않아요. RTT는 이 지연 측정값이 아니에요.'**
  String get inputNewFrameDecodedRuns;

  /// No description provided for @processingDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기 처리'**
  String get processingDevice;

  /// No description provided for @connectionStats.
  ///
  /// In ko, this message translates to:
  /// **'연결 통계'**
  String get connectionStats;

  /// No description provided for @streaming2.
  ///
  /// In ko, this message translates to:
  /// **'{name} · 스트리밍'**
  String streaming2(Object name);

  /// No description provided for @detailedDiagnosticsOffByDefault.
  ///
  /// In ko, this message translates to:
  /// **'자세한 진단은 기본으로 꺼져 있어요.\nHost·수신 큐의 계측 설정은 다음 연결부터 적용돼요.'**
  String get detailedDiagnosticsOffByDefault;

  /// No description provided for @backList.
  ///
  /// In ko, this message translates to:
  /// **'목록으로 돌아가기'**
  String get backList;

  /// No description provided for @reconnectAutoResolution.
  ///
  /// In ko, this message translates to:
  /// **'자동 해상도로 다시 연결'**
  String get reconnectAutoResolution;

  /// No description provided for @pictureQualitySettings.
  ///
  /// In ko, this message translates to:
  /// **'화질 설정'**
  String get pictureQualitySettings;

  /// No description provided for @disconnectConnectHere.
  ///
  /// In ko, this message translates to:
  /// **'끊고 여기서 연결'**
  String get disconnectConnectHere;

  /// No description provided for @hostPreparingVirtualDisplayPlease.
  ///
  /// In ko, this message translates to:
  /// **'Host가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.'**
  String get hostPreparingVirtualDisplayPlease;

  /// No description provided for @hostUpdatingReconnectsAutomaticallyWhen.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 중이에요.\n끝나면 자동으로 다시 연결해요.'**
  String get hostUpdatingReconnectsAutomaticallyWhen;

  /// No description provided for @waitingDesktopConnectsAutomaticallyWhen.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱을 기다리고 있어요.\n준비되면 자동으로 연결해요.'**
  String get waitingDesktopConnectsAutomaticallyWhen;

  /// No description provided for @connectionDroppedReconnecting.
  ///
  /// In ko, this message translates to:
  /// **'연결이 끊겨 다시 연결하고 있어요.'**
  String get connectionDroppedReconnecting;

  /// No description provided for @requestingConnection.
  ///
  /// In ko, this message translates to:
  /// **'연결을 요청하고 있어요.'**
  String get requestingConnection;

  /// No description provided for @stopWaiting.
  ///
  /// In ko, this message translates to:
  /// **'기다리기 취소'**
  String get stopWaiting;

  /// No description provided for @hostUpdating.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 중'**
  String get hostUpdating;

  /// No description provided for @waitingDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱을 기다리는 중'**
  String get waitingDesktop;

  /// No description provided for @desktopRestarting.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 다시 시작 중'**
  String get desktopRestarting;

  /// No description provided for @reconnecting2.
  ///
  /// In ko, this message translates to:
  /// **'다시 연결 중'**
  String get reconnecting2;

  /// No description provided for @fittingScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기 맞추는 중'**
  String get fittingScreenSize;

  /// No description provided for @nowPointerTrackpadTapTouch.
  ///
  /// In ko, this message translates to:
  /// **'지금 포인터(트랙패드) · 눌러서 터치로 · 길게 누르면 포인터 속도'**
  String get nowPointerTrackpadTapTouch;

  /// No description provided for @nowTouchTapPointerTrackpad.
  ///
  /// In ko, this message translates to:
  /// **'지금 터치 · 눌러서 포인터(트랙패드)로 · 길게 누르면 입력 방식'**
  String get nowTouchTapPointerTrackpad;

  /// No description provided for @closeKeyboard.
  ///
  /// In ko, this message translates to:
  /// **'키보드 닫기'**
  String get closeKeyboard;

  /// No description provided for @openKeyboard.
  ///
  /// In ko, this message translates to:
  /// **'키보드 열기'**
  String get openKeyboard;

  /// No description provided for @phone.
  ///
  /// In ko, this message translates to:
  /// **'휴대폰'**
  String get phone;

  /// No description provided for @foldableInnerScreen.
  ///
  /// In ko, this message translates to:
  /// **'폴더블 안쪽 화면'**
  String get foldableInnerScreen;

  /// No description provided for @tablet.
  ///
  /// In ko, this message translates to:
  /// **'태블릿'**
  String get tablet;

  /// No description provided for @fitDevice.
  ///
  /// In ko, this message translates to:
  /// **'기기에 맞춤'**
  String get fitDevice;

  /// No description provided for @n125.
  ///
  /// In ko, this message translates to:
  /// **'1.25배'**
  String get n125;

  /// No description provided for @n15.
  ///
  /// In ko, this message translates to:
  /// **'1.5배'**
  String get n15;

  /// No description provided for @n2.
  ///
  /// In ko, this message translates to:
  /// **'2배'**
  String get n2;

  /// No description provided for @autoRotate.
  ///
  /// In ko, this message translates to:
  /// **'자동 회전'**
  String get autoRotate;

  /// No description provided for @landscape.
  ///
  /// In ko, this message translates to:
  /// **'가로'**
  String get landscape;

  /// No description provided for @portrait.
  ///
  /// In ko, this message translates to:
  /// **'세로'**
  String get portrait;

  /// No description provided for @autoRecommended.
  ///
  /// In ko, this message translates to:
  /// **'자동 (권장)'**
  String get autoRecommended;

  /// No description provided for @lockedTapRelease.
  ///
  /// In ko, this message translates to:
  /// **'{mLabel} 고정됨 · 눌러서 해제'**
  String lockedTapRelease(Object mLabel);

  /// No description provided for @pressedCombinesNextKey.
  ///
  /// In ko, this message translates to:
  /// **'{mLabel} 눌림 · 다음 키와 함께'**
  String pressedCombinesNextKey(Object mLabel);

  /// No description provided for @longPressLock.
  ///
  /// In ko, this message translates to:
  /// **'{mLabel} · 길게 눌러 고정'**
  String longPressLock(Object mLabel);

  /// No description provided for @hanEng.
  ///
  /// In ko, this message translates to:
  /// **'한/영'**
  String get hanEng;

  /// No description provided for @ctrlAltDelCantSent2.
  ///
  /// In ko, this message translates to:
  /// **'Ctrl+Alt+Del은 보낼 수 없어요'**
  String get ctrlAltDelCantSent2;

  /// No description provided for @basicKeys.
  ///
  /// In ko, this message translates to:
  /// **'기본 키로'**
  String get basicKeys;

  /// No description provided for @f1F12NavigationKeys.
  ///
  /// In ko, this message translates to:
  /// **'F1–F12 · 탐색 키'**
  String get f1F12NavigationKeys;

  /// No description provided for @actualValueUnknownNotSupported.
  ///
  /// In ko, this message translates to:
  /// **'실제 값 미확인 · Host 미지원'**
  String get actualValueUnknownNotSupported;

  /// No description provided for @manual.
  ///
  /// In ko, this message translates to:
  /// **'수동'**
  String get manual;

  /// No description provided for @actualValueUnknown.
  ///
  /// In ko, this message translates to:
  /// **'{mode} · 실제 값 미확인'**
  String actualValueUnknown(Object mode);

  /// No description provided for @requestedActualValueUnknown.
  ///
  /// In ko, this message translates to:
  /// **'{mode} {wanted}% 요청 · 실제 값 미확인'**
  String requestedActualValueUnknown(Object mode, Object wanted);

  /// No description provided for @requested3.
  ///
  /// In ko, this message translates to:
  /// **' {wanted}% 요청'**
  String requested3(Object wanted);

  /// No description provided for @cantChangeActualValueUnknown.
  ///
  /// In ko, this message translates to:
  /// **'{mode}{wanted} · 바꾸지 못함 · 실제 값 미확인'**
  String cantChangeActualValueUnknown(Object mode, Object wanted);

  /// No description provided for @nowRequested.
  ///
  /// In ko, this message translates to:
  /// **'현재 {scale}% · {mode} {wanted}% 요청'**
  String nowRequested(Object scale, Object mode, Object wanted);

  /// No description provided for @now.
  ///
  /// In ko, this message translates to:
  /// **'현재 {scale}% · {mode}'**
  String now(Object scale, Object mode);

  /// No description provided for @resolution.
  ///
  /// In ko, this message translates to:
  /// **'해상도'**
  String get resolution;

  /// No description provided for @recommended2.
  ///
  /// In ko, this message translates to:
  /// **'권장'**
  String get recommended2;

  /// No description provided for @fitted.
  ///
  /// In ko, this message translates to:
  /// **'맞춰 그림'**
  String get fitted;

  /// No description provided for @screensRatio.
  ///
  /// In ko, this message translates to:
  /// **'이 화면 비율'**
  String get screensRatio;

  /// No description provided for @usesDevicesOwnPixelsHost.
  ///
  /// In ko, this message translates to:
  /// **'배율을 바꿀 수 있는 Host에서는 기기 화소 그대로, 아니면 읽기 편한 정수 배로 맞춰요'**
  String get usesDevicesOwnPixelsHost;

  /// No description provided for @recommendedSizeKeepsDeviceRatio.
  ///
  /// In ko, this message translates to:
  /// **'기기 비율을 유지하는 권장 크기 · 실제 적용 크기는 현재 해상도에서 확인하세요'**
  String get recommendedSizeKeepsDeviceRatio;

  /// No description provided for @sharpest.
  ///
  /// In ko, this message translates to:
  /// **'가장 선명해요'**
  String get sharpest;

  /// No description provided for @quarterPixelsDrawnCrisply2.
  ///
  /// In ko, this message translates to:
  /// **'화소 4분의 1 · 2:1로 또렷하게 그려 Host와 TV의 부담이 줄어요'**
  String get quarterPixelsDrawnCrisply2;

  /// No description provided for @fixedSizeScreensRatio.
  ///
  /// In ko, this message translates to:
  /// **'이 화면 비율의 고정 크기'**
  String get fixedSizeScreensRatio;

  /// No description provided for @sizeKeepsDeviceRatioSmall.
  ///
  /// In ko, this message translates to:
  /// **'기기 비율을 유지하는 크기 · 작은 화면을 확대하지 않아요'**
  String get sizeKeepsDeviceRatioSmall;

  /// No description provided for @remoteWorkspaceSizeCheckApplied.
  ///
  /// In ko, this message translates to:
  /// **'원격 작업 공간 크기 · 실제 적용 크기는 현재 해상도에서 확인하세요'**
  String get remoteWorkspaceSizeCheckApplied;

  /// No description provided for @remoteWorkspaceFittedDevicesScreen.
  ///
  /// In ko, this message translates to:
  /// **'기기 화면 비율에 맞춘 원격 작업 공간 · 실제 적용 크기는 현재 해상도에서 확인하세요'**
  String get remoteWorkspaceFittedDevicesScreen;

  /// No description provided for @windowsScale3.
  ///
  /// In ko, this message translates to:
  /// **'Windows 배율 · {deviceClassLabel}'**
  String windowsScale3(Object deviceClassLabel);

  /// No description provided for @calculating.
  ///
  /// In ko, this message translates to:
  /// **'계산 중'**
  String get calculating;

  /// No description provided for @choosesSuitTvsResolution.
  ///
  /// In ko, this message translates to:
  /// **'TV 화면의 해상도에 맞춰 골라요'**
  String get choosesSuitTvsResolution;

  /// No description provided for @orientation.
  ///
  /// In ko, this message translates to:
  /// **'화면 방향'**
  String get orientation;

  /// No description provided for @reconnectChangedSettings.
  ///
  /// In ko, this message translates to:
  /// **'바꾼 설정으로 다시 연결해요'**
  String get reconnectChangedSettings;

  /// No description provided for @noSettingsChanged.
  ///
  /// In ko, this message translates to:
  /// **'바꾼 설정이 없어요'**
  String get noSettingsChanged;

  /// No description provided for @qualityMode2.
  ///
  /// In ko, this message translates to:
  /// **'화질 모드'**
  String get qualityMode2;

  /// No description provided for @tapSwipe.
  ///
  /// In ko, this message translates to:
  /// **'탭 · 밀기'**
  String get tapSwipe;

  /// No description provided for @sentAsWindowsTouchWorks.
  ///
  /// In ko, this message translates to:
  /// **'Windows 터치로 그대로 전달 · 터치 화면처럼 조작'**
  String get sentAsWindowsTouchWorks;

  /// No description provided for @longPress.
  ///
  /// In ko, this message translates to:
  /// **'길게 누르기'**
  String get longPress;

  /// No description provided for @rightClick.
  ///
  /// In ko, this message translates to:
  /// **'오른쪽 클릭'**
  String get rightClick;

  /// No description provided for @twoFingers.
  ///
  /// In ko, this message translates to:
  /// **'두 손가락'**
  String get twoFingers;

  /// No description provided for @zoomScrollInsideAppsWhen.
  ///
  /// In ko, this message translates to:
  /// **'앱 안에서 확대·스크롤 (앱이 지원할 때)'**
  String get zoomScrollInsideAppsWhen;

  /// No description provided for @slideOneFinger.
  ///
  /// In ko, this message translates to:
  /// **'한 손가락으로 밀기'**
  String get slideOneFinger;

  /// No description provided for @moveCursor.
  ///
  /// In ko, this message translates to:
  /// **'커서 이동'**
  String get moveCursor;

  /// No description provided for @tap.
  ///
  /// In ko, this message translates to:
  /// **'탭'**
  String get tap;

  /// No description provided for @clickCursorDoubleTapDouble.
  ///
  /// In ko, this message translates to:
  /// **'커서 위치를 클릭 · 두 번 탭은 더블 클릭'**
  String get clickCursorDoubleTapDouble;

  /// No description provided for @doubleTapMoveWhileHolding.
  ///
  /// In ko, this message translates to:
  /// **'두 번 탭하고 두 번째를 누른 채 움직이기'**
  String get doubleTapMoveWhileHolding;

  /// No description provided for @dragSelectMoveWindows.
  ///
  /// In ko, this message translates to:
  /// **'끌기 · 범위 선택 · 창 이동'**
  String get dragSelectMoveWindows;

  /// No description provided for @twoFingersUpDown.
  ///
  /// In ko, this message translates to:
  /// **'두 손가락으로 위아래'**
  String get twoFingersUpDown;

  /// No description provided for @scroll.
  ///
  /// In ko, this message translates to:
  /// **'스크롤'**
  String get scroll;

  /// No description provided for @clickWhereYouTapDouble.
  ///
  /// In ko, this message translates to:
  /// **'누른 곳을 클릭 · 두 번 탭은 더블 클릭'**
  String get clickWhereYouTapDouble;

  /// No description provided for @oneFingerUpDown.
  ///
  /// In ko, this message translates to:
  /// **'한 손가락으로 위아래'**
  String get oneFingerUpDown;

  /// No description provided for @doubleTapMoveWhileHolding2.
  ///
  /// In ko, this message translates to:
  /// **'두 번 탭하고 누른 채 움직이기'**
  String get doubleTapMoveWhileHolding2;

  /// No description provided for @longPressTwoFingerTap.
  ///
  /// In ko, this message translates to:
  /// **'길게 누르기 · 두 손가락 탭'**
  String get longPressTwoFingerTap;

  /// No description provided for @spreadTwoFingers.
  ///
  /// In ko, this message translates to:
  /// **'두 손가락 벌리기'**
  String get spreadTwoFingers;

  /// No description provided for @zoomPinchBackSeeWhole.
  ///
  /// In ko, this message translates to:
  /// **'확대 · 오므리면 전체 보기까지'**
  String get zoomPinchBackSeeWhole;

  /// No description provided for @threeFingerTap.
  ///
  /// In ko, this message translates to:
  /// **'세 손가락 탭'**
  String get threeFingerTap;

  /// No description provided for @openCloseKeyboard.
  ///
  /// In ko, this message translates to:
  /// **'키보드 열기·닫기'**
  String get openCloseKeyboard;

  /// No description provided for @fourFingerTap.
  ///
  /// In ko, this message translates to:
  /// **'네 손가락 탭'**
  String get fourFingerTap;

  /// No description provided for @switchInputModePointerTouch.
  ///
  /// In ko, this message translates to:
  /// **'입력 방식 전환 · 포인터 ↔ 터치'**
  String get switchInputModePointerTouch;

  /// No description provided for @ok.
  ///
  /// In ko, this message translates to:
  /// **'확인'**
  String get ok;

  /// No description provided for @inputMode2.
  ///
  /// In ko, this message translates to:
  /// **'입력 방식 · {modeLabel}'**
  String inputMode2(Object modeLabel);

  /// No description provided for @zoomScreenItselfSwitchInput.
  ///
  /// In ko, this message translates to:
  /// **'화면 자체를 확대하려면 입력 방식을 포인터로 바꾸세요.'**
  String get zoomScreenItselfSwitchInput;

  /// No description provided for @tapClicksLittleSlowWhile.
  ///
  /// In ko, this message translates to:
  /// **'탭 클릭은 두 번 탭인지 확인하느라 조금 늦어요.'**
  String get tapClicksLittleSlowWhile;

  /// No description provided for @longPressInputModeButton.
  ///
  /// In ko, this message translates to:
  /// **'입력 방식 버튼을 길게 누르면 입력 방식과 포인터 속도를 바꿔요.\n{windowsTouch}\n마우스를 연결하면 바로 마우스로 조작해요.'**
  String longPressInputModeButton(Object windowsTouch);

  /// No description provided for @featuresConnection.
  ///
  /// In ko, this message translates to:
  /// **'이 연결에서 쓸 수 있는 기능'**
  String get featuresConnection;

  /// No description provided for @available.
  ///
  /// In ko, this message translates to:
  /// **'사용 가능'**
  String get available;

  /// No description provided for @checkingConnection.
  ///
  /// In ko, this message translates to:
  /// **'연결 확인 중'**
  String get checkingConnection;

  /// No description provided for @notAvailableConnection.
  ///
  /// In ko, this message translates to:
  /// **'이번 연결에서 사용 불가'**
  String get notAvailableConnection;

  /// No description provided for @usesMoreDataBattery.
  ///
  /// In ko, this message translates to:
  /// **'데이터·배터리 더 씀'**
  String get usesMoreDataBattery;

  /// No description provided for @getsScreensExactPixelsSo.
  ///
  /// In ko, this message translates to:
  /// **'이 화면의 화소 그대로 받아서 글자가 또렷해요. 데이터와 배터리를 더 써요.'**
  String get getsScreensExactPixelsSo;

  /// No description provided for @windowsScale200Limit.
  ///
  /// In ko, this message translates to:
  /// **'Windows 배율 200% 한도'**
  String get windowsScale200Limit;

  /// No description provided for @desktopLimit.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 한도'**
  String get desktopLimit;

  /// No description provided for @decoderSizeLimitDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기 디코더 크기 한도'**
  String get decoderSizeLimitDevice;

  /// No description provided for @decoderSpeedLimitDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기 디코더 속도 한도'**
  String get decoderSpeedLimitDevice;

  /// No description provided for @memoryDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기 메모리'**
  String get memoryDevice;

  /// No description provided for @autoSizeAlreadyNativePixels.
  ///
  /// In ko, this message translates to:
  /// **'자동 크기가 이미 기기 화소'**
  String get autoSizeAlreadyNativePixels;

  /// No description provided for @couldntCheck.
  ///
  /// In ko, this message translates to:
  /// **'확인하지 못함'**
  String get couldntCheck;

  /// No description provided for @backAutoSize.
  ///
  /// In ko, this message translates to:
  /// **' · 자동 크기로 돌아감 {count}회 ({stats})'**
  String backAutoSize(num count, Object stats);

  /// No description provided for @off3.
  ///
  /// In ko, this message translates to:
  /// **'꺼짐{fell}'**
  String off3(Object fell);

  /// No description provided for @nativeDevicePixels.
  ///
  /// In ko, this message translates to:
  /// **'기기 화소 그대로{size}'**
  String nativeDevicePixels(Object size);

  /// No description provided for @lowered.
  ///
  /// In ko, this message translates to:
  /// **'낮춤'**
  String get lowered;

  /// No description provided for @onLabel3.
  ///
  /// In ko, this message translates to:
  /// **'켜짐 · {state}{fell}'**
  String onLabel3(Object state, Object fell);

  /// No description provided for @checkHttpConsentAgain.
  ///
  /// In ko, this message translates to:
  /// **'HTTP 동의를 다시 확인하세요.'**
  String get checkHttpConsentAgain;

  /// No description provided for @viewScaleResolution.
  ///
  /// In ko, this message translates to:
  /// **'보기 · 배율 · 해상도'**
  String get viewScaleResolution;

  /// No description provided for @qualityInputHelp.
  ///
  /// In ko, this message translates to:
  /// **'화질 · 입력 · 사용안내'**
  String get qualityInputHelp;

  /// No description provided for @textScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'글자와 화면 크기'**
  String get textScreenSize;

  /// No description provided for @frameRateBitrate.
  ///
  /// In ko, this message translates to:
  /// **'프레임·전송률'**
  String get frameRateBitrate;

  /// No description provided for @useExternalMouse.
  ///
  /// In ko, this message translates to:
  /// **'외부 마우스 사용'**
  String get useExternalMouse;

  /// No description provided for @availableFeatures.
  ///
  /// In ko, this message translates to:
  /// **'사용할 수 있는 기능'**
  String get availableFeatures;

  /// No description provided for @roundTripLatencyHostResponse.
  ///
  /// In ko, this message translates to:
  /// **'왕복 지연 · Host 응답'**
  String get roundTripLatencyHostResponse;

  /// No description provided for @notMeasuredYet.
  ///
  /// In ko, this message translates to:
  /// **'측정 전'**
  String get notMeasuredYet;

  /// No description provided for @menu.
  ///
  /// In ko, this message translates to:
  /// **'메뉴'**
  String get menu;

  /// No description provided for @closeMenu.
  ///
  /// In ko, this message translates to:
  /// **'메뉴 닫기'**
  String get closeMenu;

  /// No description provided for @openMenu.
  ///
  /// In ko, this message translates to:
  /// **'메뉴 열기'**
  String get openMenu;

  /// No description provided for @openMenu2.
  ///
  /// In ko, this message translates to:
  /// **'메뉴 열기 · {latencyText}'**
  String openMenu2(Object latencyText);

  /// No description provided for @screenPanHandleDragMove.
  ///
  /// In ko, this message translates to:
  /// **'화면 이동 손잡이 · 끌어서 보이는 부분을 옮겨요'**
  String get screenPanHandleDragMove;

  /// No description provided for @lookLeft.
  ///
  /// In ko, this message translates to:
  /// **'왼쪽 보기'**
  String get lookLeft;

  /// No description provided for @lookUp.
  ///
  /// In ko, this message translates to:
  /// **'위쪽 보기'**
  String get lookUp;

  /// No description provided for @lookDown.
  ///
  /// In ko, this message translates to:
  /// **'아래쪽 보기'**
  String get lookDown;

  /// No description provided for @lookRight.
  ///
  /// In ko, this message translates to:
  /// **'오른쪽 보기'**
  String get lookRight;

  /// No description provided for @back.
  ///
  /// In ko, this message translates to:
  /// **'뒤로'**
  String get back;

  /// No description provided for @touch.
  ///
  /// In ko, this message translates to:
  /// **'터치'**
  String get touch;

  /// No description provided for @pointer.
  ///
  /// In ko, this message translates to:
  /// **'포인터'**
  String get pointer;

  /// No description provided for @precise.
  ///
  /// In ko, this message translates to:
  /// **'정밀'**
  String get precise;

  /// No description provided for @standard.
  ///
  /// In ko, this message translates to:
  /// **'표준'**
  String get standard;

  /// No description provided for @fast.
  ///
  /// In ko, this message translates to:
  /// **'빠름'**
  String get fast;

  /// No description provided for @couldntOpenTvInputField.
  ///
  /// In ko, this message translates to:
  /// **'TV 입력창을 열지 못했어요. 다시 선택해 주세요.'**
  String get couldntOpenTvInputField;

  /// No description provided for @updateAppUseTvInput.
  ///
  /// In ko, this message translates to:
  /// **'TV 입력을 사용하려면 앱을 업데이트해 주세요.'**
  String get updateAppUseTvInput;

  /// No description provided for @drag.
  ///
  /// In ko, this message translates to:
  /// **'끌기'**
  String get drag;

  /// No description provided for @scrollUpDown.
  ///
  /// In ko, this message translates to:
  /// **'스크롤 · 위/아래'**
  String get scrollUpDown;

  /// No description provided for @draggingOkRelease.
  ///
  /// In ko, this message translates to:
  /// **'끄는 중 · OK로 놓기'**
  String get draggingOkRelease;

  /// No description provided for @dragOkGrab.
  ///
  /// In ko, this message translates to:
  /// **'끌기 · OK로 잡기'**
  String get dragOkGrab;

  /// No description provided for @remoteScreenMenu.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면 메뉴'**
  String get remoteScreenMenu;

  /// No description provided for @shortcuts.
  ///
  /// In ko, this message translates to:
  /// **'단축키'**
  String get shortcuts;

  /// No description provided for @backRemoteScreen.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면으로'**
  String get backRemoteScreen;

  /// No description provided for @installPermissionConfirmedTapDownload.
  ///
  /// In ko, this message translates to:
  /// **'설치 허용을 확인했어요. ‘받고 설치’를 누르세요.'**
  String get installPermissionConfirmedTapDownload;

  /// No description provided for @couldntCheckInstallPermissionDownload.
  ///
  /// In ko, this message translates to:
  /// **'설치 권한이나 다운로드 위치를 확인하지 못했어요. 다시 시도해 주세요.'**
  String get couldntCheckInstallPermissionDownload;

  /// No description provided for @couldntOpenAndroidSettingsAllow.
  ///
  /// In ko, this message translates to:
  /// **'Android 설정을 열지 못했어요. 설정 → 앱 → Multitap에서 직접 허용해 주세요.'**
  String get couldntOpenAndroidSettingsAllow;

  /// No description provided for @appDataSignSettingsKept.
  ///
  /// In ko, this message translates to:
  /// **'앱 데이터와 로그인 설정은 유지돼요.\n받은 파일을 확인한 뒤 Android 설치 화면을 열어요.\nAndroid가 설치 여부를 다시 물어봐요.'**
  String get appDataSignSettingsKept;

  /// No description provided for @androidSettingsAllowInstallUnknown.
  ///
  /// In ko, this message translates to:
  /// **'Android 설정에서 Multitap의\n‘알 수 없는 앱 설치’를 허용해 주세요.\n허용한 뒤 이 화면으로 돌아오세요.\n설정에서 언제든 다시 끌 수 있어요.'**
  String get androidSettingsAllowInstallUnknown;

  /// No description provided for @checkingInstallPermissionDownloadLocation.
  ///
  /// In ko, this message translates to:
  /// **'설치 권한과 다운로드 위치를 확인하고 있어요.'**
  String get checkingInstallPermissionDownloadLocation;

  /// No description provided for @downloadingMb.
  ///
  /// In ko, this message translates to:
  /// **'받는 중 · {received} / {total} MB ({total2}%)'**
  String downloadingMb(Object received, Object total, Object total2);

  /// No description provided for @checkingDownloadedFilesSizeSha.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일의 크기와 SHA-256을 확인하고 있어요.'**
  String get checkingDownloadedFilesSizeSha;

  /// No description provided for @openingAndroidInstaller.
  ///
  /// In ko, this message translates to:
  /// **'Android 설치 화면을 여는 중이에요.'**
  String get openingAndroidInstaller;

  /// No description provided for @tapUpdateAndroidInstallerOpen.
  ///
  /// In ko, this message translates to:
  /// **'Android 설치 화면에서 ‘업데이트’를 누르세요.\n설치 후 앱을 다시 열면 새 버전으로 시작해요.'**
  String get tapUpdateAndroidInstallerOpen;

  /// No description provided for @later.
  ///
  /// In ko, this message translates to:
  /// **'나중에'**
  String get later;

  /// No description provided for @downloadInstall.
  ///
  /// In ko, this message translates to:
  /// **'받고 설치'**
  String get downloadInstall;

  /// No description provided for @newVersion3.
  ///
  /// In ko, this message translates to:
  /// **'새 버전 {version}'**
  String newVersion3(Object version);

  /// No description provided for @currentMb.
  ///
  /// In ko, this message translates to:
  /// **'현재 {current} · {total} MB'**
  String currentMb(Object current, Object total);

  /// No description provided for @released.
  ///
  /// In ko, this message translates to:
  /// **'{releasedAt} 배포'**
  String released(Object releasedAt);

  /// No description provided for @current3.
  ///
  /// In ko, this message translates to:
  /// **'현재'**
  String get current3;

  /// No description provided for @newVersion4.
  ///
  /// In ko, this message translates to:
  /// **'새 버전'**
  String get newVersion4;

  /// No description provided for @installedVersion.
  ///
  /// In ko, this message translates to:
  /// **'설치된 버전'**
  String get installedVersion;

  /// No description provided for @newerThanServer.
  ///
  /// In ko, this message translates to:
  /// **'서버보다 새 버전'**
  String get newerThanServer;

  /// No description provided for @upDate.
  ///
  /// In ko, this message translates to:
  /// **'최신'**
  String get upDate;

  /// No description provided for @text4.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 {hostsLength}대 가능'**
  String text4(num hostsLength);

  /// No description provided for @checkingServersUpdateList.
  ///
  /// In ko, this message translates to:
  /// **'서버의 업데이트 목록을 확인하고 있어요.'**
  String get checkingServersUpdateList;

  /// No description provided for @serverHasNoAndroidInstaller.
  ///
  /// In ko, this message translates to:
  /// **'이 서버에는 이 기기용 Android 설치 파일이 아직 없어요.'**
  String get serverHasNoAndroidInstaller;

  /// No description provided for @whatsNewVersion.
  ///
  /// In ko, this message translates to:
  /// **'이번 버전에서 달라진 점'**
  String get whatsNewVersion;

  /// No description provided for @whatsNewServerVersion.
  ///
  /// In ko, this message translates to:
  /// **'서버 버전 {version}에서 달라진 점'**
  String whatsNewServerVersion(Object version);

  /// No description provided for @whatsNewCurrentVersion.
  ///
  /// In ko, this message translates to:
  /// **'지금 버전에서 달라진 점'**
  String get whatsNewCurrentVersion;

  /// No description provided for @appUpdate.
  ///
  /// In ko, this message translates to:
  /// **'앱 업데이트'**
  String get appUpdate;

  /// No description provided for @couldntGetUpdateInformationCheck.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 정보를 확인하지 못했어요. 네트워크를 확인하고 다시 시도하세요.'**
  String get couldntGetUpdateInformationCheck;

  /// No description provided for @serversUpdateInformationIsntValid.
  ///
  /// In ko, this message translates to:
  /// **'서버의 업데이트 정보가 올바르지 않아요. 관리자에게 알려 주세요.'**
  String get serversUpdateInformationIsntValid;

  /// No description provided for @couldntDownloadUpdateFileTry.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 파일을 받지 못했어요. 다시 시도하세요.'**
  String get couldntDownloadUpdateFileTry;

  /// No description provided for @downloadedFilesSizeSha256.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일의 크기나 SHA-256이 서버 정보와 달라 설치하지 않았어요.'**
  String get downloadedFilesSizeSha256;

  /// No description provided for @couldntSaveUpdateFileCheck.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 파일을 저장하지 못했어요. 기기 저장 공간을 확인하세요.'**
  String get couldntSaveUpdateFileCheck;

  /// No description provided for @cantCheckUpdatesBecauseApps.
  ///
  /// In ko, this message translates to:
  /// **'이 앱의 현재 버전을 확인하지 못해 업데이트를 확인할 수 없어요.'**
  String get cantCheckUpdatesBecauseApps;

  /// No description provided for @cantCheckUpdatesFitDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기에 맞는 업데이트를 확인할 수 없어요.'**
  String get cantCheckUpdatesFitDevice;

  /// No description provided for @allowInstallUnknownAppsMultitap.
  ///
  /// In ko, this message translates to:
  /// **'Android 설정에서 Multitap의 ‘알 수 없는 앱 설치’를 허용해야 업데이트를 설치할 수 있어요.'**
  String get allowInstallUnknownAppsMultitap;

  /// No description provided for @downloadedFileIsntMultitapUpdate.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일이 같은 서명의 Multitap 업데이트가 아니라 설치하지 않았어요.'**
  String get downloadedFileIsntMultitapUpdate;

  /// No description provided for @downloadedFileIsntNewerThan.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일이 지금 버전보다 새 버전이 아니라 설치하지 않았어요.'**
  String get downloadedFileIsntNewerThan;

  /// No description provided for @anotherUpdateBeingInstalledTry.
  ///
  /// In ko, this message translates to:
  /// **'다른 업데이트 설치가 진행 중이에요. 끝난 뒤 다시 시도하세요.'**
  String get anotherUpdateBeingInstalledTry;

  /// No description provided for @couldntCheckInstallStatusRestart.
  ///
  /// In ko, this message translates to:
  /// **'설치 상태를 확인하지 못했어요. Multitap을 다시 시작한 뒤 업데이트를 확인하세요.'**
  String get couldntCheckInstallStatusRestart;

  /// No description provided for @updateCanceled.
  ///
  /// In ko, this message translates to:
  /// **'업데이트를 취소했어요.'**
  String get updateCanceled;

  /// No description provided for @updateInstallCanceledYouKeep.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 설치를 취소했어요. 지금 버전을 계속 써요.'**
  String get updateInstallCanceledYouKeep;

  /// No description provided for @androidBlockedUpdateInstallCheck.
  ///
  /// In ko, this message translates to:
  /// **'Android가 업데이트 설치를 막았어요. 기기 관리 정책을 확인하세요.'**
  String get androidBlockedUpdateInstallCheck;

  /// No description provided for @couldntUpdateBecauseDoesntMatch.
  ///
  /// In ko, this message translates to:
  /// **'설치된 앱과 맞지 않아 업데이트하지 못했어요.'**
  String get couldntUpdateBecauseDoesntMatch;

  /// No description provided for @updateIsntCompatibleDevice.
  ///
  /// In ko, this message translates to:
  /// **'이 기기와 호환되지 않는 업데이트예요.'**
  String get updateIsntCompatibleDevice;

  /// No description provided for @notEnoughStorageInstallUpdate.
  ///
  /// In ko, this message translates to:
  /// **'저장 공간이 부족해 업데이트를 설치하지 못했어요.'**
  String get notEnoughStorageInstallUpdate;

  /// No description provided for @androidFoundUpdateFileInvalid.
  ///
  /// In ko, this message translates to:
  /// **'Android가 업데이트 파일을 올바르지 않다고 판단했어요.'**
  String get androidFoundUpdateFileInvalid;

  /// No description provided for @updateInstallTimedOutTry.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 설치 시간이 초과됐어요. 다시 시도하세요.'**
  String get updateInstallTimedOutTry;

  /// No description provided for @couldntOpenAndroidInstallerTry.
  ///
  /// In ko, this message translates to:
  /// **'Android 설치 화면을 열지 못했어요. 다시 시도하세요.'**
  String get couldntOpenAndroidInstallerTry;

  /// No description provided for @notice.
  ///
  /// In ko, this message translates to:
  /// **'공지: {bannerTitle}'**
  String notice(Object bannerTitle);

  /// No description provided for @couldntOpenConsoleWindow.
  ///
  /// In ko, this message translates to:
  /// **'Console 창을 열지 못했어요.'**
  String get couldntOpenConsoleWindow;

  /// No description provided for @couldntGetResponseFromServer.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답을 확인하지 못했어요. 잠시 후 다시 시도하세요.'**
  String get couldntGetResponseFromServer;

  /// No description provided for @youWereSignedOutMultitap.
  ///
  /// In ko, this message translates to:
  /// **'Multitap에서 로그아웃됐어요. 클릭해서 이유를 확인하세요.'**
  String get youWereSignedOutMultitap;

  /// No description provided for @couldntSignOutCheckNetwork.
  ///
  /// In ko, this message translates to:
  /// **'로그아웃하지 못했어요. 네트워크를 확인하고 다시 시도하세요.'**
  String get couldntSignOutCheckNetwork;

  /// No description provided for @couldntCloseRemoteWindowClose.
  ///
  /// In ko, this message translates to:
  /// **'원격 창을 닫지 못했어요. 원격 창을 닫은 뒤 다시 로그아웃하세요.'**
  String get couldntCloseRemoteWindowClose;

  /// No description provided for @passwordWasChangedSignAgain.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 변경됐어요. 새 비밀번호로 다시 로그인해 주세요.'**
  String get passwordWasChangedSignAgain;

  /// No description provided for @demo.
  ///
  /// In ko, this message translates to:
  /// **'데모'**
  String get demo;

  /// No description provided for @notSet.
  ///
  /// In ko, this message translates to:
  /// **'미설정'**
  String get notSet;

  /// No description provided for @unnamedComponent.
  ///
  /// In ko, this message translates to:
  /// **'이름이 없는 구성 요소'**
  String get unnamedComponent;

  /// No description provided for @packageLicense.
  ///
  /// In ko, this message translates to:
  /// **'패키지 라이선스'**
  String get packageLicense;

  /// No description provided for @peaks.
  ///
  /// In ko, this message translates to:
  /// **'봉우리'**
  String get peaks;

  /// No description provided for @meadow.
  ///
  /// In ko, this message translates to:
  /// **'초원'**
  String get meadow;

  /// No description provided for @desktopConnectedFromAnotherWindow.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱은 다른 창이나 기기에서 연결 중이에요. 아래 버튼을 누르면 그쪽 연결을 끊고 여기서 이어서 써요. 데스크톱과 실행 중인 작업은 그대로예요.'**
  String get desktopConnectedFromAnotherWindow;

  /// No description provided for @connectionSetupCanceledYouCan.
  ///
  /// In ko, this message translates to:
  /// **'연결 준비를 취소했어요. 다시 연결할 수 있어요.'**
  String get connectionSetupCanceledYouCan;

  /// No description provided for @youNeedAgreeUnencryptedHttp.
  ///
  /// In ko, this message translates to:
  /// **'암호화 없는 HTTP 연결에 먼저 동의해야 해요. 서버 설정에서 ‘연결 테스트’를 다시 누르세요.'**
  String get youNeedAgreeUnencryptedHttp;

  /// No description provided for @serverDoesntAllowHttpConnections.
  ///
  /// In ko, this message translates to:
  /// **'이 서버에서는 HTTP 연결을 쓸 수 없어요. HTTPS 주소를 쓰거나 관리자에게 알려 주세요.'**
  String get serverDoesntAllowHttpConnections;

  /// No description provided for @httpConnectionsTurnedOffServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 HTTP 연결이 꺼져 있어요. HTTPS 주소를 쓰거나 관리자에게 요청하세요.'**
  String get httpConnectionsTurnedOffServer;

  /// No description provided for @addressIsntRegisteredServerCheck.
  ///
  /// In ko, this message translates to:
  /// **'이 주소는 서버에 등록된 주소가 아니에요. 받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.'**
  String get addressIsntRegisteredServerCheck;

  /// No description provided for @addressOnlyServersFirstSetup.
  ///
  /// In ko, this message translates to:
  /// **'이 주소는 서버 초기 설정에만 쓸 수 있어요. 관리자가 서버 접속 설정을 마친 뒤에 연결할 수 있어요.'**
  String get addressOnlyServersFirstSetup;

  /// No description provided for @applyingServerAccessSettingsConnect.
  ///
  /// In ko, this message translates to:
  /// **'서버 접속 설정을 적용하고 있어요.\n잠시 후 다시 연결해 주세요.'**
  String get applyingServerAccessSettingsConnect;

  /// No description provided for @addressDoesntMatchServersSettings.
  ///
  /// In ko, this message translates to:
  /// **'이 주소가 서버의 설정과 맞지 않아요. 받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.'**
  String get addressDoesntMatchServersSettings;

  /// No description provided for @changeInitialUsernamePasswordAdmin.
  ///
  /// In ko, this message translates to:
  /// **'관리자 웹에서 초기 아이디와 비밀번호를 먼저 변경해 주세요.'**
  String get changeInitialUsernamePasswordAdmin;

  /// No description provided for @checkUsernamePassword.
  ///
  /// In ko, this message translates to:
  /// **'아이디 또는 비밀번호를 확인해 주세요.'**
  String get checkUsernamePassword;

  /// No description provided for @tooManyAttemptsTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'시도가 너무 많아요. 1분 후 다시 시도하세요.'**
  String get tooManyAttemptsTryAgain;

  /// No description provided for @serverBusyTryAgainMoment.
  ///
  /// In ko, this message translates to:
  /// **'서버가 처리 중이에요. 잠시 후 다시 시도하세요.'**
  String get serverBusyTryAgainMoment;

  /// No description provided for @checkVerificationCodeEnterAgain.
  ///
  /// In ko, this message translates to:
  /// **'인증 코드를 확인하고 다시 입력하세요.'**
  String get checkVerificationCodeEnterAgain;

  /// No description provided for @verificationTimeUpVerifyAgain.
  ///
  /// In ko, this message translates to:
  /// **'본인 확인 시간이 끝났어요. 인증 앱의 다음 코드로 다시 확인하세요.'**
  String get verificationTimeUpVerifyAgain;

  /// No description provided for @verificationTimeUpStartAgain.
  ///
  /// In ko, this message translates to:
  /// **'인증 시간이 끝났어요. 로그인부터 다시 시작하세요.'**
  String get verificationTimeUpStartAgain;

  /// No description provided for @updateMultitapLatestVersionUse.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증을 쓰려면 Multitap을 최신 버전으로 업데이트하세요.'**
  String get updateMultitapLatestVersionUse;

  /// No description provided for @twoStepVerificationWorksHttps.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증은 HTTPS 서버 주소에서 사용할 수 있어요.'**
  String get twoStepVerificationWorksHttps;

  /// No description provided for @serverCouldntCheckTwoStep.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 2단계 인증을 확인하지 못했어요. 관리자에게 알려 주세요.'**
  String get serverCouldntCheckTwoStep;

  /// No description provided for @enterEitherVerificationCodeRecovery.
  ///
  /// In ko, this message translates to:
  /// **'인증 코드 또는 복구 코드 하나만 입력하세요.'**
  String get enterEitherVerificationCodeRecovery;

  /// No description provided for @couldntOpenBrowserCheckDefault.
  ///
  /// In ko, this message translates to:
  /// **'브라우저를 열지 못했어요.\n기본 브라우저 설정을 확인하고 다시 시도하세요.'**
  String get couldntOpenBrowserCheckDefault;

  /// No description provided for @couldntOpenConsoleCheckNetwork2.
  ///
  /// In ko, this message translates to:
  /// **'Console을 열지 못했어요.\n네트워크를 확인하고 다시 시도하세요.'**
  String get couldntOpenConsoleCheckNetwork2;

  /// No description provided for @consoleNeedsMicrosoftEdgeWebview2.
  ///
  /// In ko, this message translates to:
  /// **'Console을 열려면 Microsoft Edge WebView2가 필요해요.\nWebView2 Runtime을 설치한 뒤 다시 시도하세요.'**
  String get consoleNeedsMicrosoftEdgeWebview2;

  /// No description provided for @consoleCantOpenedServerTell.
  ///
  /// In ko, this message translates to:
  /// **'이 서버에서는 Console을 열 수 없어요.\n관리자에게 알려 주세요.'**
  String get consoleCantOpenedServerTell;

  /// No description provided for @anotherDesktopsConsoleOpenClose.
  ///
  /// In ko, this message translates to:
  /// **'다른 데스크톱의 Console이 열려 있어요.\n그 Console을 닫고 다시 시도하세요.'**
  String get anotherDesktopsConsoleOpenClose;

  /// No description provided for @consoleOpensOnlyHttpsServer.
  ///
  /// In ko, this message translates to:
  /// **'Console은 HTTPS 서버 주소에서만 열 수 있어요.\nHTTPS 주소로 다시 로그인하세요.'**
  String get consoleOpensOnlyHttpsServer;

  /// No description provided for @tooManySignInsUse.
  ///
  /// In ko, this message translates to:
  /// **'동시에 사용 중인 로그인이 많아요. 사용하지 않는 기기에서 로그아웃한 뒤 다시 로그인하세요.'**
  String get tooManySignInsUse;

  /// No description provided for @serverNeedsUpdateBeforeYou.
  ///
  /// In ko, this message translates to:
  /// **'이 앱으로 로그인하려면 서버를 업데이트해야 해요. 관리자에게 알려 주세요.'**
  String get serverNeedsUpdateBeforeYou;

  /// No description provided for @timeRanOutWhileChecking.
  ///
  /// In ko, this message translates to:
  /// **'확인하는 사이에 시간이 지났거나 기존 로그인이 바뀌었어요. 다시 로그인해 주세요.'**
  String get timeRanOutWhileChecking;

  /// No description provided for @connectionUseHasChangedConnect.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 연결이 바뀌었어요. 다시 연결해 현재 연결을 확인해 주세요.'**
  String get connectionUseHasChangedConnect;

  /// No description provided for @timeConfirmConnectionSwitchUp.
  ///
  /// In ko, this message translates to:
  /// **'연결 전환 확인 시간이 끝났어요. 다시 연결해 현재 연결을 확인해 주세요.'**
  String get timeConfirmConnectionSwitchUp;

  /// No description provided for @couldntCheckConnectionSwitchDetails.
  ///
  /// In ko, this message translates to:
  /// **'연결 전환 정보를 확인하지 못했어요. 다시 연결해 주세요.'**
  String get couldntCheckConnectionSwitchDetails;

  /// No description provided for @couldntDisconnectOtherConnectionConnect.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳의 연결을 끊고 연결하지 못했어요. 다시 연결해 보고, 계속되면 관리자에게 알려 주세요.'**
  String get couldntDisconnectOtherConnectionConnect;

  /// No description provided for @couldntCheckConnectionUseConnect.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 연결 정보를 확인하지 못했어요. 잠시 후 다시 연결해 주세요.'**
  String get couldntCheckConnectionUseConnect;

  /// No description provided for @couldntConnectServerCheckNetwork.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결하지 못했어요. 네트워크를 확인해 주세요.'**
  String get couldntConnectServerCheckNetwork;

  /// No description provided for @serverIsntRespondingTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'서버가 응답하지 않아요. 잠시 후 다시 시도하세요.'**
  String get serverIsntRespondingTryAgain;

  /// No description provided for @checkCurrentPassword.
  ///
  /// In ko, this message translates to:
  /// **'현재 비밀번호를 확인해 주세요.'**
  String get checkCurrentPassword;

  /// No description provided for @newPasswordConfirmationDontMatch.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호와 확인 입력이 일치하지 않아요.'**
  String get newPasswordConfirmationDontMatch;

  /// No description provided for @passwordsMust10256Characters.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호는 10자 이상 256자 이하로 입력해 주세요.'**
  String get passwordsMust10256Characters;

  /// No description provided for @lostConnectionHostTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결이 끊겼어요. 잠시 후 다시 시도하세요.'**
  String get lostConnectionHostTryAgain;

  /// No description provided for @desktopHasAnotherConnection.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에 다른 연결이 있어요.'**
  String get desktopHasAnotherConnection;

  /// No description provided for @couldntProcessRequestTryAgain2.
  ///
  /// In ko, this message translates to:
  /// **'요청을 처리하지 못했어요. 잠시 후 다시 시도해 주세요.'**
  String get couldntProcessRequestTryAgain2;

  /// No description provided for @couldntConnectSecurelyOverHttps.
  ///
  /// In ko, this message translates to:
  /// **'HTTPS로 안전하게 연결하지 못했어요. 서버 주소를 확인하고, 맞다면 관리자에게 알려 주세요.'**
  String get couldntConnectSecurelyOverHttps;

  /// No description provided for @couldntFindServerAddressCheck.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소를 찾지 못했어요. 주소와 네트워크를 확인하세요.'**
  String get couldntFindServerAddressCheck;

  /// No description provided for @serverDidntRespondTimeCheck.
  ///
  /// In ko, this message translates to:
  /// **'서버가 시간 안에 응답하지 않았어요. 주소와 포트를 확인하고 다시 시도하세요.'**
  String get serverDidntRespondTimeCheck;

  /// No description provided for @cantReachServerAddressPort.
  ///
  /// In ko, this message translates to:
  /// **'이 주소와 포트로는 서버에 연결되지 않아요. 받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.'**
  String get cantReachServerAddressPort;

  /// No description provided for @desktopIsntRespondingConnectAgain.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 응답하지 않아요. 잠시 후 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get desktopIsntRespondingConnectAgain;

  /// No description provided for @hostStillGettingReadyConnect.
  ///
  /// In ko, this message translates to:
  /// **'Host가 아직 준비 중이에요. 잠시 후 다시 연결하세요.'**
  String get hostStillGettingReadyConnect;

  /// No description provided for @cantPrepareScreenFitsWindow.
  ///
  /// In ko, this message translates to:
  /// **'지금은 이 크기의 화면을 준비할 수 없어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get cantPrepareScreenFitsWindow;

  /// No description provided for @hostsStateChangedConnectAgain.
  ///
  /// In ko, this message translates to:
  /// **'Host 상태가 바뀌었어요. 다시 연결하세요.'**
  String get hostsStateChangedConnectAgain;

  /// No description provided for @pictureQualitySettingsDontMatch.
  ///
  /// In ko, this message translates to:
  /// **'화면 품질 설정이 서버와 맞지 않아요. Multitap을 업데이트해도 계속되면 관리자에게 알려 주세요.'**
  String get pictureQualitySettingsDontMatch;

  /// No description provided for @cantOpenVirtualDisplayScreen.
  ///
  /// In ko, this message translates to:
  /// **'이 화면 크기로 가상 화면을 열 수 없어요. 화면 모드나 크기를 바꿔 다시 연결하세요.'**
  String get cantOpenVirtualDisplayScreen;

  /// No description provided for @hostPreparingVirtualDisplayConnect.
  ///
  /// In ko, this message translates to:
  /// **'Host가 가상 화면을 준비하고 있어요. 잠시 후 다시 연결하세요.'**
  String get hostPreparingVirtualDisplayConnect;

  /// No description provided for @onlyAdministratorAccountsCanUpdate.
  ///
  /// In ko, this message translates to:
  /// **'관리자 계정만 Host를 업데이트할 수 있어요.'**
  String get onlyAdministratorAccountsCanUpdate;

  /// No description provided for @hostDoesntSupportRemoteUpdates.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 원격 업데이트를 지원하지 않아요. 데스크톱에서 Host 설치 파일을 직접 실행하세요.'**
  String get hostDoesntSupportRemoteUpdates;

  /// No description provided for @hostUpdateProgressDesktopTry.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에서 Host 업데이트가 진행 중이에요. 끝난 뒤 다시 시도하세요.'**
  String get hostUpdateProgressDesktopTry;

  /// No description provided for @hostAlreadyUpDate.
  ///
  /// In ko, this message translates to:
  /// **'이미 최신 Host 버전이에요.'**
  String get hostAlreadyUpDate;

  /// No description provided for @isntNewerThanCurrentHost.
  ///
  /// In ko, this message translates to:
  /// **'현재 Host보다 새 버전이 아니에요.'**
  String get isntNewerThanCurrentHost;

  /// No description provided for @serversLatestHostInstallerHas.
  ///
  /// In ko, this message translates to:
  /// **'서버의 최신 Host 설치 파일이 바뀌었어요. 목록을 새로고침하세요.'**
  String get serversLatestHostInstallerHas;

  /// No description provided for @couldntCheckHostsCurrentVersion.
  ///
  /// In ko, this message translates to:
  /// **'Host의 현재 버전을 확인하지 못했어요. 잠시 후 다시 시도하세요.'**
  String get couldntCheckHostsCurrentVersion;

  /// No description provided for @tooManyHostUpdateRequests.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 요청이 많아요. 잠시 후 다시 시도하세요.'**
  String get tooManyHostUpdateRequests;

  /// No description provided for @cleaningUpPreviousConnectionConnect.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결을 정리하고 있어요. 잠시 후 다시 연결하세요.'**
  String get cleaningUpPreviousConnectionConnect;

  /// No description provided for @virtualDisplayIsntReadyIf.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면이 준비되지 않았어요. 다시 연결해도 같으면 관리자에게 알려 주세요.'**
  String get virtualDisplayIsntReadyIf;

  /// No description provided for @windowsSignedOutLockedOpen.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 로그아웃됐거나 잠겨 있어요.\nConsole로 열어 로그인하세요.'**
  String get windowsSignedOutLockedOpen;

  /// No description provided for @hostIsntReadyStreamYet.
  ///
  /// In ko, this message translates to:
  /// **'Host가 아직 스트리밍을 준비하지 못했어요. 잠시 후 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get hostIsntReadyStreamYet;

  /// No description provided for @desktopHasAnotherConnectionDisconnect.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에 다른 연결이 있어요. 그 연결을 끊은 뒤 다시 연결하세요.'**
  String get desktopHasAnotherConnectionDisconnect;

  /// No description provided for @anotherUserUsingDesktopConnect.
  ///
  /// In ko, this message translates to:
  /// **'다른 사용자가 이 데스크톱을 쓰고 있어요. 그 연결이 끝난 뒤 다시 연결하세요.'**
  String get anotherUserUsingDesktopConnect;

  /// No description provided for @tooManyConnectionSwitchRequests.
  ///
  /// In ko, this message translates to:
  /// **'연결 전환 요청이 많아요. 잠시 후 다시 시도해 주세요.'**
  String get tooManyConnectionSwitchRequests;

  /// No description provided for @powerActionProgressTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업이 진행 중이에요. 끝난 뒤 다시 시도하세요.'**
  String get powerActionProgressTryAgain;

  /// No description provided for @cantDoneCurrentPowerState.
  ///
  /// In ko, this message translates to:
  /// **'지금 전원 상태에서는 할 수 없는 작업이에요. 목록을 새로고침해 상태를 확인하세요.'**
  String get cantDoneCurrentPowerState;

  /// No description provided for @couldntCheckVmsPowerState.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox에서 VM 전원 상태를 확인하지 못했어요. 잠시 후 새로고침해 주세요.'**
  String get couldntCheckVmsPowerState;

  /// No description provided for @couldntFinishPowerActionRefresh.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업을 끝내지 못했어요. 목록을 새로고침해 상태를 확인하고, 계속되면 관리자에게 알려 주세요.'**
  String get couldntFinishPowerActionRefresh;

  /// No description provided for @powerActionsResultIsntConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업 결과를 아직 확인하지 못했어요. 다시 요청하지 말고 목록을 새로고침하세요.'**
  String get powerActionsResultIsntConfirmed;

  /// No description provided for @proxmoxConnectionSettingsChangedRefresh.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 연결 설정이 변경됐어요. 상태를 새로고침한 뒤 다시 시도해 주세요.'**
  String get proxmoxConnectionSettingsChangedRefresh;

  /// No description provided for @youDontHavePermissionDesktop.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱을 쓸 권한이 없거나 배정이 바뀌었어요. 목록을 새로고침하고, 필요하면 관리자에게 요청하세요.'**
  String get youDontHavePermissionDesktop;

  /// No description provided for @cantCheckPowerActionsResult.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업 결과를 더 확인할 수 없어요. 목록을 새로고침해 상태를 확인하세요.'**
  String get cantCheckPowerActionsResult;

  /// No description provided for @powerActionIsntSupported.
  ///
  /// In ko, this message translates to:
  /// **'지원하지 않는 전원 작업이에요.'**
  String get powerActionIsntSupported;

  /// No description provided for @anotherPowerActionEndedStatus.
  ///
  /// In ko, this message translates to:
  /// **'다른 전원 작업으로 이전 요청의 상태 확인을 마쳤어요.'**
  String get anotherPowerActionEndedStatus;

  /// No description provided for @hostDoesntSupportChangingNumber.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 모니터 수 변경을 지원하지 않거나 아직 준비되지 않았어요.'**
  String get hostDoesntSupportChangingNumber;

  /// No description provided for @changingNumberMonitorsTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수를 바꾸고 있어요. 끝난 뒤 다시 시도하세요.'**
  String get changingNumberMonitorsTryAgain;

  /// No description provided for @couldntChangeNumberMonitorsCheck.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수를 바꾸지 못했어요. 데스크톱 목록에서 지금 상태를 확인하세요.'**
  String get couldntChangeNumberMonitorsCheck;

  /// No description provided for @itsNotConfirmedYetWhether.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수가 바뀌었는지 아직 확인하지 못했어요. 다시 요청하지 말고 데스크톱 목록에서 확인하세요.'**
  String get itsNotConfirmedYetWhether;

  /// No description provided for @cantCheckMonitorChangeResult.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수 변경 결과를 더 확인할 수 없어요. 데스크톱 목록에서 지금 상태를 확인하세요.'**
  String get cantCheckMonitorChangeResult;

  /// No description provided for @youCanSet12.
  ///
  /// In ko, this message translates to:
  /// **'모니터는 1대 또는 2대로 설정할 수 있어요.'**
  String get youCanSet12;

  /// No description provided for @allMonitorsCantUsedDesktop.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에서는 지금 모든 모니터를 쓸 수 없어요. 창 모드나 전체 화면을 쓰세요.'**
  String get allMonitorsCantUsedDesktop;

  /// No description provided for @couldntCheckLayoutPcs2.
  ///
  /// In ko, this message translates to:
  /// **'PC 모니터 2대의 배치를 확인하지 못했어요. 모니터 연결을 확인한 뒤 다시 선택하세요.'**
  String get couldntCheckLayoutPcs2;

  /// No description provided for @pcsCurrentMonitorSetupCant.
  ///
  /// In ko, this message translates to:
  /// **'지금 PC의 모니터 구성으로는 모든 모니터를 쓸 수 없어요. 모니터 2대, 합계 830만 화소 이하일 때만 쓸 수 있어요.'**
  String get pcsCurrentMonitorSetupCant;

  /// No description provided for @tooManyMonitorChangeRequests.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수 변경 요청이 많아요. 잠시 후 다시 시도하세요.'**
  String get tooManyMonitorChangeRequests;

  /// No description provided for @localPcsHostDoesntSupport.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC의 Host가 원격 전원을 지원하지 않아요. 관리자에게 Host 업데이트를 요청하세요.'**
  String get localPcsHostDoesntSupport;

  /// No description provided for @couldntStartLocalPc.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC를 켜지 못했어요.'**
  String get couldntStartLocalPc;

  /// No description provided for @localPcLockedSoWindows.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC가 잠겨 있어 Windows가 요청을 받지 않았어요.'**
  String get localPcLockedSoWindows;

  /// No description provided for @anotherUserSignedLocalPc.
  ///
  /// In ko, this message translates to:
  /// **'다른 사용자가 로컬 PC에 로그인해 있어 Windows가 요청을 받지 않았어요.'**
  String get anotherUserSignedLocalPc;

  /// No description provided for @localPcAlreadyShuttingDown.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC가 이미 꺼지거나 다시 시작하는 중이에요.'**
  String get localPcAlreadyShuttingDown;

  /// No description provided for @windowsDidntGivePermissionShut.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 로컬 PC를 끄거나 다시 시작할 권한을 주지 않았어요. 관리자에게 알려 주세요.'**
  String get windowsDidntGivePermissionShut;

  /// No description provided for @windowsCouldntProcessRequestCheck.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 요청을 처리하지 못했어요. 로컬 PC에서 직접 확인하세요.'**
  String get windowsCouldntProcessRequestCheck;

  /// No description provided for @localPcDidntRespondCheck.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC가 응답하지 않았어요. 잠시 후 목록에서 상태를 확인하세요.'**
  String get localPcDidntRespondCheck;

  /// No description provided for @windowsSignedOutLockedSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 로그아웃됐거나 잠겨 있어요.\n로컬 PC에서 직접 로그인하세요.'**
  String get windowsSignedOutLockedSign;

  /// No description provided for @serverIsntRespondingMomentRetrying.
  ///
  /// In ko, this message translates to:
  /// **'서버가 잠시 응답하지 않아요. 자동으로 다시 시도해요.'**
  String get serverIsntRespondingMomentRetrying;

  /// No description provided for @couldntFindServerAddressCheck2.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소를 찾지 못했어요. 주소의 철자와 인터넷 연결을 확인하세요.'**
  String get couldntFindServerAddressCheck2;

  /// No description provided for @serverCouldntProcessRequestTry.
  ///
  /// In ko, this message translates to:
  /// **'서버가 요청을 처리하지 못했어요. 잠시 후 다시 시도하고, 계속되면 관리자에게 알려 주세요. (HTTP {status})'**
  String serverCouldntProcessRequestTry(Object status);

  /// No description provided for @addressIsntMultitapServerCheck.
  ///
  /// In ko, this message translates to:
  /// **'이 주소는 Multitap 서버가 아니에요. 서버 주소를 확인하세요.'**
  String get addressIsntMultitapServerCheck;

  /// No description provided for @youNeedAgreeUnencryptedHttp2.
  ///
  /// In ko, this message translates to:
  /// **'암호화 없는 HTTP 연결에 먼저 동의해야 해요. ‘연결 테스트’를 다시 누르세요.'**
  String get youNeedAgreeUnencryptedHttp2;

  /// No description provided for @serverDoesntAllowHttpConnections2.
  ///
  /// In ko, this message translates to:
  /// **'이 서버에서는 HTTP 연결을 쓸 수 없어요.\nHTTPS 주소를 쓰거나 관리자에게 알려 주세요.'**
  String get serverDoesntAllowHttpConnections2;

  /// No description provided for @httpConnectionsTurnedOffServer2.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 HTTP 연결이 꺼져 있어요.\nHTTPS 주소를 쓰거나 관리자에게 요청하세요.'**
  String get httpConnectionsTurnedOffServer2;

  /// No description provided for @addressIsntRegisteredServerCheck2.
  ///
  /// In ko, this message translates to:
  /// **'이 주소는 서버에 등록된 주소가 아니에요.\n받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.'**
  String get addressIsntRegisteredServerCheck2;

  /// No description provided for @httpAddressOnlyServersFirst.
  ///
  /// In ko, this message translates to:
  /// **'이 HTTP 주소는 서버 초기 설정에만 쓸 수 있어요.\n관리자가 서버 접속 설정을 마친 뒤에 연결할 수 있어요.'**
  String get httpAddressOnlyServersFirst;

  /// No description provided for @addressDoesntMatchServersSettings2.
  ///
  /// In ko, this message translates to:
  /// **'이 주소가 서버의 설정과 맞지 않아요.\n받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.'**
  String get addressDoesntMatchServersSettings2;

  /// No description provided for @cantReachServerAddressPort2.
  ///
  /// In ko, this message translates to:
  /// **'이 주소와 포트로는 서버에 연결되지 않아요.\n받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.'**
  String get cantReachServerAddressPort2;

  /// No description provided for @couldntConnectServerCheckAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결하지 못했어요. 주소·포트와 네트워크를 확인하세요.'**
  String get couldntConnectServerCheckAddress;

  /// No description provided for @checkYouEnteredServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소를 입력했는지, 공백이나 특수 문자가 없는지 확인하세요.'**
  String get checkYouEnteredServerAddress;

  /// No description provided for @enterHttpHttpsServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'HTTP 또는 HTTPS 서버 주소를 입력하세요.'**
  String get enterHttpHttpsServerAddress;

  /// No description provided for @dontIncludePathServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소에는 경로를 넣지 마세요.'**
  String get dontIncludePathServerAddress;

  /// No description provided for @checkIpv6Address.
  ///
  /// In ko, this message translates to:
  /// **'IPv6 주소를 확인하세요.'**
  String get checkIpv6Address;

  /// No description provided for @checkServerPort.
  ///
  /// In ko, this message translates to:
  /// **'서버 포트를 확인하세요.'**
  String get checkServerPort;

  /// No description provided for @enterServerPort.
  ///
  /// In ko, this message translates to:
  /// **'서버 포트를 입력하세요.'**
  String get enterServerPort;

  /// No description provided for @ipv6AddressesNeedSquareBrackets.
  ///
  /// In ko, this message translates to:
  /// **'IPv6 주소에는 대괄호가 필요해요.'**
  String get ipv6AddressesNeedSquareBrackets;

  /// No description provided for @enterIpAddressAsFour.
  ///
  /// In ko, this message translates to:
  /// **'IP 주소는 0~255 숫자 네 개를 점으로 이어 입력하세요.'**
  String get enterIpAddressAsFour;

  /// No description provided for @checkServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소를 확인하세요.'**
  String get checkServerAddress;

  /// No description provided for @portNumberFrom165535.
  ///
  /// In ko, this message translates to:
  /// **'포트는 1~65535 숫자예요.'**
  String get portNumberFrom165535;

  /// No description provided for @agreeHttpServerAddressFirst.
  ///
  /// In ko, this message translates to:
  /// **'이 HTTP 서버 주소에 먼저 동의하고 연결을 확인하세요.'**
  String get agreeHttpServerAddressFirst;

  /// No description provided for @serverSettingsChangedConnectAgain.
  ///
  /// In ko, this message translates to:
  /// **'서버 설정이 바뀌었어요. 다시 연결하세요.'**
  String get serverSettingsChangedConnectAgain;

  /// No description provided for @httpConsentAddressDifferent.
  ///
  /// In ko, this message translates to:
  /// **'HTTP 동의 주소가 달라요.'**
  String get httpConsentAddressDifferent;

  /// No description provided for @checkServerSettings.
  ///
  /// In ko, this message translates to:
  /// **'서버 설정을 확인하세요.'**
  String get checkServerSettings;

  /// No description provided for @testConnectionCurrentHttpAddress.
  ///
  /// In ko, this message translates to:
  /// **'현재 HTTP 주소의 연결을 먼저 확인하세요.'**
  String get testConnectionCurrentHttpAddress;

  /// No description provided for @invalidRelayAddress.
  ///
  /// In ko, this message translates to:
  /// **'잘못된 중계 주소예요.'**
  String get invalidRelayAddress;

  /// No description provided for @checkServersAccessSettings.
  ///
  /// In ko, this message translates to:
  /// **'서버의 접속 설정을 확인하세요.'**
  String get checkServersAccessSettings;

  /// No description provided for @waiting.
  ///
  /// In ko, this message translates to:
  /// **'대기'**
  String get waiting;

  /// No description provided for @download.
  ///
  /// In ko, this message translates to:
  /// **'다운로드'**
  String get download;

  /// No description provided for @install.
  ///
  /// In ko, this message translates to:
  /// **'설치'**
  String get install;

  /// No description provided for @done.
  ///
  /// In ko, this message translates to:
  /// **'완료'**
  String get done;

  /// No description provided for @needsCheck.
  ///
  /// In ko, this message translates to:
  /// **'확인 필요'**
  String get needsCheck;

  /// No description provided for @failed.
  ///
  /// In ko, this message translates to:
  /// **'실패'**
  String get failed;

  /// No description provided for @stopped.
  ///
  /// In ko, this message translates to:
  /// **'중지'**
  String get stopped;

  /// No description provided for @updateAvailable.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 가능'**
  String get updateAvailable;

  /// No description provided for @notEligible.
  ///
  /// In ko, this message translates to:
  /// **'대상 아님'**
  String get notEligible;

  /// No description provided for @assignmentWasRemovedSoResult.
  ///
  /// In ko, this message translates to:
  /// **'할당이 해제돼 접수된 요청의 결과를 확인할 수 없어요.'**
  String get assignmentWasRemovedSoResult;

  /// No description provided for @desktopIsntAssignedYouNow.
  ///
  /// In ko, this message translates to:
  /// **'현재 할당된 데스크톱이 아니에요.'**
  String get desktopIsntAssignedYouNow;

  /// No description provided for @useRequestingDisconnects.
  ///
  /// In ko, this message translates to:
  /// **'사용 중 · 요청하면 연결을 끊어요'**
  String get useRequestingDisconnects;

  /// No description provided for @notUseUpdatesRightAway.
  ///
  /// In ko, this message translates to:
  /// **'사용 중 아님 · 요청하면 바로 업데이트해요'**
  String get notUseUpdatesRightAway;

  /// No description provided for @notRequestedYet.
  ///
  /// In ko, this message translates to:
  /// **'아직 요청하지 않았어요'**
  String get notRequestedYet;

  /// No description provided for @requestingFromServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에 요청하는 중…'**
  String get requestingFromServer;

  /// No description provided for @notAcceptedYetRequestingAgain.
  ///
  /// In ko, this message translates to:
  /// **'아직 접수되지 않았어요. 잠시 후 자동으로 다시 요청해요.'**
  String get notAcceptedYetRequestingAgain;

  /// No description provided for @acceptedByServerWaitingInstall.
  ///
  /// In ko, this message translates to:
  /// **'서버 접수됨 · 설치 결과를 기다려요'**
  String get acceptedByServerWaitingInstall;

  /// No description provided for @updatesWhenOtherUsersConnection.
  ///
  /// In ko, this message translates to:
  /// **'다른 사용자의 연결이 끝나면 업데이트해요'**
  String get updatesWhenOtherUsersConnection;

  /// No description provided for @useUpdatesWhenConnectionEnds.
  ///
  /// In ko, this message translates to:
  /// **'사용 중 · 연결이 끝나면 업데이트해요'**
  String get useUpdatesWhenConnectionEnds;

  /// No description provided for @downloadingInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일 다운로드{op}'**
  String downloadingInstaller(Object op);

  /// No description provided for @checkingInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일 확인 중'**
  String get checkingInstaller;

  /// No description provided for @installingHost.
  ///
  /// In ko, this message translates to:
  /// **'Host 설치 중'**
  String get installingHost;

  /// No description provided for @hostServiceRestartedWaitingConnection.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 다시 시작돼 연결을 기다려요'**
  String get hostServiceRestartedWaitingConnection;

  /// No description provided for @serverConfirmedInstallFinished.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 설치 완료를 확인했어요'**
  String get serverConfirmedInstallFinished;

  /// No description provided for @installedWindowsRestartNeeded.
  ///
  /// In ko, this message translates to:
  /// **'설치 완료 · Windows 다시 시작 필요'**
  String get installedWindowsRestartNeeded;

  /// No description provided for @installedOpenHostSettingsDesktop.
  ///
  /// In ko, this message translates to:
  /// **'설치 완료 · 데스크톱에서 Host 설정을 열어 확인하세요'**
  String get installedOpenHostSettingsDesktop;

  /// No description provided for @runningNewVersionInstallResult.
  ///
  /// In ko, this message translates to:
  /// **'새 버전으로 실행 중 · 설치 결과는 보고되지 않았어요'**
  String get runningNewVersionInstallResult;

  /// No description provided for @checkRequestInstallResultWont.
  ///
  /// In ko, this message translates to:
  /// **'요청·설치 결과 확인 필요 · 자동으로 다시 요청하지 않아요'**
  String get checkRequestInstallResultWont;

  /// No description provided for @stoppedRemainingRequestsWerentAccepted.
  ///
  /// In ko, this message translates to:
  /// **'아직 접수되지 않은 남은 요청을 중지했어요'**
  String get stoppedRemainingRequestsWerentAccepted;

  /// No description provided for @serverCantCheckUpdatePermission.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 업데이트 권한·상태를 확인할 수 없어요'**
  String get serverCantCheckUpdatePermission;

  /// No description provided for @hostOffline.
  ///
  /// In ko, this message translates to:
  /// **'Host 오프라인'**
  String get hostOffline;

  /// No description provided for @remoteUpdateNotSupportedInstall.
  ///
  /// In ko, this message translates to:
  /// **'원격 업데이트 미지원 · 데스크톱에서 직접 설치해야 해요'**
  String get remoteUpdateNotSupportedInstall;

  /// No description provided for @latestReleaseChangedCloseWindow.
  ///
  /// In ko, this message translates to:
  /// **'최신 배포가 바뀌었어요. 창을 닫고 다시 확인하세요.'**
  String get latestReleaseChangedCloseWindow;

  /// No description provided for @checkCurrentHostVersion.
  ///
  /// In ko, this message translates to:
  /// **'현재 Host 버전 확인 필요'**
  String get checkCurrentHostVersion;

  /// No description provided for @noNewVersionInstall.
  ///
  /// In ko, this message translates to:
  /// **'설치할 새 버전이 없어요'**
  String get noNewVersionInstall;

  /// No description provided for @anotherHostUpdateProgress.
  ///
  /// In ko, this message translates to:
  /// **'다른 Host 업데이트가 진행 중이에요'**
  String get anotherHostUpdateProgress;

  /// No description provided for @serverDoesntAllowUpdatesRight.
  ///
  /// In ko, this message translates to:
  /// **'현재 서버에서 업데이트를 허용하지 않아요'**
  String get serverDoesntAllowUpdatesRight;

  /// No description provided for @hostOfflineSoRequestWasnt.
  ///
  /// In ko, this message translates to:
  /// **'Host가 오프라인이라 요청을 접수하지 못했어요'**
  String get hostOfflineSoRequestWasnt;

  /// No description provided for @anotherUpdateAlreadyProgress.
  ///
  /// In ko, this message translates to:
  /// **'다른 업데이트가 이미 진행 중이에요'**
  String get anotherUpdateAlreadyProgress;

  /// No description provided for @serverConfirmedItsAlreadyUp.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 이미 최신 버전임을 확인했어요'**
  String get serverConfirmedItsAlreadyUp;

  /// No description provided for @hostDoesntSupportRemoteUpdates2.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 원격 업데이트를 지원하지 않아요'**
  String get hostDoesntSupportRemoteUpdates2;

  /// No description provided for @noUpdatePermissionSignExpired.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 권한이 없거나 로그인이 만료됐어요'**
  String get noUpdatePermissionSignExpired;

  /// No description provided for @tooManyRequestsSoWasnt.
  ///
  /// In ko, this message translates to:
  /// **'요청이 많아 접수되지 않았어요. 잠시 후 다시 시도하세요.'**
  String get tooManyRequestsSoWasnt;

  /// No description provided for @serverCouldntAcceptRequestClose.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 요청을 접수하지 못했어요. 창을 닫고 다시 시도하세요.'**
  String get serverCouldntAcceptRequestClose;

  /// No description provided for @text5.
  ///
  /// In ko, this message translates to:
  /// **'{stage} {stage2}대'**
  String text5(Object stage, Object stage2);

  /// No description provided for @progress.
  ///
  /// In ko, this message translates to:
  /// **'진행 중 {running}대 · {value} · {value2}'**
  String progress(Object running, Object value, Object value2);

  /// No description provided for @allFinished.
  ///
  /// In ko, this message translates to:
  /// **'모두 끝났어요'**
  String get allFinished;

  /// No description provided for @versionUnknown.
  ///
  /// In ko, this message translates to:
  /// **'버전 확인 필요'**
  String get versionUnknown;

  /// No description provided for @youSignedSomewhereElseSo.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳에서 로그인해 여기서는 로그아웃됐어요.'**
  String get youSignedSomewhereElseSo;

  /// No description provided for @youWereSignedOutAfter.
  ///
  /// In ko, this message translates to:
  /// **'오랫동안 입력이 없어 로그아웃됐어요.'**
  String get youWereSignedOutAfter;

  /// No description provided for @useAnotherDeviceSignAgain.
  ///
  /// In ko, this message translates to:
  /// **'다른 기기에서 사용 중이에요. 다시 로그인해 주세요.'**
  String get useAnotherDeviceSignAgain;

  /// No description provided for @youWereSignedOutEnter.
  ///
  /// In ko, this message translates to:
  /// **'로그아웃됐어요. 비밀번호를 입력해 다시 로그인하세요.'**
  String get youWereSignedOutEnter;

  /// No description provided for @multitapKeepsRunningTrayClose.
  ///
  /// In ko, this message translates to:
  /// **'Multitap은 트레이에서 계속 실행 중이에요. 완전히 끝내려면 트레이 아이콘이나 계정 메뉴의 \'완전 종료\'를 누르세요.'**
  String get multitapKeepsRunningTrayClose;

  /// No description provided for @signExpiredClickSignAgain.
  ///
  /// In ko, this message translates to:
  /// **'로그인이 만료됐어요. 클릭해서 다시 로그인하세요.'**
  String get signExpiredClickSignAgain;

  /// No description provided for @text6.
  ///
  /// In ko, this message translates to:
  /// **'{bytes}바이트'**
  String text6(num bytes);

  /// No description provided for @zipThemMoveThemShared.
  ///
  /// In ko, this message translates to:
  /// **'압축하거나 ‘공유 폴더’로 옮기세요'**
  String get zipThemMoveThemShared;

  /// No description provided for @zipThemFirst.
  ///
  /// In ko, this message translates to:
  /// **'압축해서 옮기세요'**
  String get zipThemFirst;

  /// No description provided for @moveLargeFilesSharedFolder.
  ///
  /// In ko, this message translates to:
  /// **'큰 파일은 ‘공유 폴더’로 옮기세요'**
  String get moveLargeFilesSharedFolder;

  /// No description provided for @splitThemIntoSmallerFiles.
  ///
  /// In ko, this message translates to:
  /// **'더 작은 파일로 나눠 옮기세요'**
  String get splitThemIntoSmallerFiles;

  /// No description provided for @upPerFileOnce.
  ///
  /// In ko, this message translates to:
  /// **'파일 하나에 {maxFileBytes}, 한 번에 {maxTotalBytes}까지'**
  String upPerFileOnce(Object maxFileBytes, Object maxTotalBytes);

  /// No description provided for @textTooLongYouCan.
  ///
  /// In ko, this message translates to:
  /// **'텍스트가 너무 길어요. 한 번에 {maxTextBytes}까지 옮길 수 있어요.'**
  String textTooLongYouCan(Object maxTextBytes);

  /// No description provided for @tooManyFilesYouCan.
  ///
  /// In ko, this message translates to:
  /// **'파일이 너무 많아요. 한 번에 {maxFiles}개까지 옮길 수 있어요. 여러 개는 {pack}.'**
  String tooManyFilesYouCan(Object maxFiles, Object pack);

  /// No description provided for @fileTooLargeClipboardCan.
  ///
  /// In ko, this message translates to:
  /// **'파일이 너무 커요. 클립보드로는 {files} 옮길 수 있어요. {big}.'**
  String fileTooLargeClipboardCan(Object files, Object big);

  /// No description provided for @imageTooLargeYouCan.
  ///
  /// In ko, this message translates to:
  /// **'이미지가 너무 커요. 6,400만 화소, {formats}까지 옮길 수 있어요. 이미지를 파일로 저장해서 옮기세요.'**
  String imageTooLargeYouCan(Object formats);

  /// No description provided for @tooManyItemsFolderYou.
  ///
  /// In ko, this message translates to:
  /// **'폴더 안의 항목이 너무 많아요. 한 번에 {maxEntries}개까지 옮길 수 있어요. 폴더를 {pack}.'**
  String tooManyItemsFolderYou(Object maxEntries, Object pack);

  /// No description provided for @folderTooDeepYouCan.
  ///
  /// In ko, this message translates to:
  /// **'폴더가 너무 깊어요. 폴더 안에 {maxDepth}단계까지 옮길 수 있어요. 폴더를 {pack}.'**
  String folderTooDeepYouCan(Object maxDepth, Object pack);

  /// No description provided for @longestPathCharacters.
  ///
  /// In ko, this message translates to:
  /// **'가장 긴 경로 {chars}자, '**
  String longestPathCharacters(Object chars);

  /// No description provided for @someFilesHavePathsToo.
  ///
  /// In ko, this message translates to:
  /// **'경로가 너무 긴 파일이 있어요 ({chars}폴더 안의 경로는 {limit}자까지). 폴더를 {pack}.'**
  String someFilesHavePathsToo(Object chars, Object limit, Object pack);

  /// No description provided for @overClipboardTransferLimit.
  ///
  /// In ko, this message translates to:
  /// **'클립보드로 옮길 수 있는 한도를 넘었어요.'**
  String get overClipboardTransferLimit;

  /// No description provided for @overClipboardLimitYouCan.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 한도를 넘었어요. {files}, 파일 {maxFiles}개까지, 텍스트는 {maxTextBytes}까지 옮길 수 있어요. {big}.'**
  String overClipboardLimitYouCan(
    Object files,
    Object maxFiles,
    Object maxTextBytes,
    Object big,
  );

  /// No description provided for @clipboardConnected.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 연결됨'**
  String get clipboardConnected;

  /// No description provided for @clipboardBeingTransferredPasteAgain.
  ///
  /// In ko, this message translates to:
  /// **'클립보드를 옮기는 중이에요. 끝난 뒤 다시 붙여넣으세요.'**
  String get clipboardBeingTransferredPasteAgain;

  /// No description provided for @couldntPrepareContentPasteCopy.
  ///
  /// In ko, this message translates to:
  /// **'붙여넣을 내용을 준비하지 못했어요. 다시 복사하세요.'**
  String get couldntPrepareContentPasteCopy;

  /// No description provided for @preparingClipboardConnection.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 연결 준비'**
  String get preparingClipboardConnection;

  /// No description provided for @checkingPcsClipboard.
  ///
  /// In ko, this message translates to:
  /// **'PC의 클립보드 확인 중'**
  String get checkingPcsClipboard;

  /// No description provided for @requestingDesktopsClipboard.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 클립보드 요청 중'**
  String get requestingDesktopsClipboard;

  /// No description provided for @preparingPaste.
  ///
  /// In ko, this message translates to:
  /// **'붙여넣기를 준비하는 중'**
  String get preparingPaste;

  /// No description provided for @checkAutomaticClipboardSharingAgain.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 자동 공유를 다시 확인하세요.'**
  String get checkAutomaticClipboardSharingAgain;

  /// No description provided for @checkingWasAppliedDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 적용 확인 중'**
  String get checkingWasAppliedDesktop;

  /// No description provided for @sendingClipboard.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 보내는 중'**
  String get sendingClipboard;

  /// No description provided for @gettingClipboardFiles.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 파일 가져오는 중'**
  String get gettingClipboardFiles;

  /// No description provided for @couldntPrepareAutomaticClipboardSharing.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 자동 공유를 준비하지 못했어요.'**
  String get couldntPrepareAutomaticClipboardSharing;

  /// No description provided for @gettingClipboard.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 가져오는 중'**
  String get gettingClipboard;

  /// No description provided for @filesArriveWhenYouPaste.
  ///
  /// In ko, this message translates to:
  /// **'붙여넣으면 파일을 받아요'**
  String get filesArriveWhenYouPaste;

  /// No description provided for @arrivesWhenYouPaste.
  ///
  /// In ko, this message translates to:
  /// **'붙여넣으면 받아요'**
  String get arrivesWhenYouPaste;

  /// No description provided for @cancelingClipboardTransfer.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 전송 취소 중'**
  String get cancelingClipboardTransfer;

  /// No description provided for @copiedFilesArriveWhenYou.
  ///
  /// In ko, this message translates to:
  /// **'복사했어요. 붙여넣을 때 파일을 받아요. 링크나 쓸 수 없는 이름 {treeSkipped}개는 빠져요.'**
  String copiedFilesArriveWhenYou(num treeSkipped);

  /// No description provided for @copiedFilesArriveWhenYou2.
  ///
  /// In ko, this message translates to:
  /// **'복사했어요. 붙여넣을 때 파일을 받아요.'**
  String get copiedFilesArriveWhenYou2;

  /// No description provided for @copiedArrivesWhenYouPaste.
  ///
  /// In ko, this message translates to:
  /// **'복사했어요. 붙여넣을 때 받아요.'**
  String get copiedArrivesWhenYouPaste;

  /// No description provided for @clipboardTransferDoneSkipped.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 전송 완료. 링크나 쓸 수 없는 이름 {treeSkipped}개는 건너뛰었어요.'**
  String clipboardTransferDoneSkipped(num treeSkipped);

  /// No description provided for @clipboardTransferDone.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 전송 완료'**
  String get clipboardTransferDone;

  /// No description provided for @originalFileChangedCopyAgain.
  ///
  /// In ko, this message translates to:
  /// **'원본 파일이 바뀌었어요. 다시 복사한 뒤 붙여넣으세요.'**
  String get originalFileChangedCopyAgain;

  /// No description provided for @whatYouCopiedNoLonger.
  ///
  /// In ko, this message translates to:
  /// **'복사한 내용이 더 이상 남아 있지 않아요. 다시 복사한 뒤 붙여넣으세요.'**
  String get whatYouCopiedNoLonger;

  /// No description provided for @somethingNewWasCopiedSo.
  ///
  /// In ko, this message translates to:
  /// **'새로 복사한 내용이 있어 덮어쓰지 않았어요. 다시 붙여넣으세요.'**
  String get somethingNewWasCopiedSo;

  /// No description provided for @administratorDoesntAllowClipboardTransfer.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 허용하지 않은 클립보드 전송이에요.'**
  String get administratorDoesntAllowClipboardTransfer;

  /// No description provided for @nothingCopiedNoTextImage.
  ///
  /// In ko, this message translates to:
  /// **'복사한 텍스트, 이미지, 파일이 없어요.'**
  String get nothingCopiedNoTextImage;

  /// No description provided for @nothingCopiedNoTextFile.
  ///
  /// In ko, this message translates to:
  /// **'복사한 텍스트나 파일이 없어요.'**
  String get nothingCopiedNoTextFile;

  /// No description provided for @contentCantMovedOnlyText.
  ///
  /// In ko, this message translates to:
  /// **'이 내용은 옮길 수 없어요. 텍스트, 이미지, 파일, 폴더만 옮길 수 있어요.'**
  String get contentCantMovedOnlyText;

  /// No description provided for @onlyTextFilesCanMoved.
  ///
  /// In ko, this message translates to:
  /// **'지금은 텍스트와 파일만 옮길 수 있어요. 폴더는 {sharedFolders}. 이미지와 폴더는 앱과 데스크톱의 Host가 최신이어야 옮길 수 있어요.'**
  String onlyTextFilesCanMoved(Object sharedFolders);

  /// No description provided for @tryAgainAfterOtherClipboard.
  ///
  /// In ko, this message translates to:
  /// **'다른 클립보드 작업이 끝난 뒤 다시 시도하세요.'**
  String get tryAgainAfterOtherClipboard;

  /// No description provided for @clipboardTransferCanceled.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 전송을 취소했어요.'**
  String get clipboardTransferCanceled;

  /// No description provided for @couldntConfirmWasMovedTry.
  ///
  /// In ko, this message translates to:
  /// **'옮겨졌는지 확인하지 못했어요. 붙여넣어 보고, 안 되면 다시 복사하세요.'**
  String get couldntConfirmWasMovedTry;

  /// No description provided for @couldntReadWhatYouCopied.
  ///
  /// In ko, this message translates to:
  /// **'복사한 내용을 읽지 못했어요. 다시 복사한 뒤 붙여넣으세요.'**
  String get couldntReadWhatYouCopied;

  /// No description provided for @couldntPutReceivingClipboardCopy.
  ///
  /// In ko, this message translates to:
  /// **'받는 쪽 클립보드에 넣지 못했어요. 다시 복사한 뒤 붙여넣으세요.'**
  String get couldntPutReceivingClipboardCopy;

  /// No description provided for @cantReadCopiedFileCheck.
  ///
  /// In ko, this message translates to:
  /// **'복사한 파일을 읽을 수 없어요. 파일이 있는지, 이름이 올바른지 확인한 뒤 다시 복사하세요.'**
  String get cantReadCopiedFileCheck;

  /// No description provided for @anotherProgramUsingFileSo.
  ///
  /// In ko, this message translates to:
  /// **'다른 프로그램이 파일을 사용 중이라 읽을 수 없어요. 그 프로그램에서 파일을 닫은 뒤 다시 복사하세요.'**
  String get anotherProgramUsingFileSo;

  /// No description provided for @notEnoughDiskSpaceReceiving.
  ///
  /// In ko, this message translates to:
  /// **'받는 쪽 디스크 공간이 부족해요. 공간을 확보한 뒤 다시 시도하세요.'**
  String get notEnoughDiskSpaceReceiving;

  /// No description provided for @desktopDidntRespondTime.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 시간 안에 응답하지 않았어요.'**
  String get desktopDidntRespondTime;

  /// No description provided for @clipboardCantUsedBecauseDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 화면이 잠겼거나 보안 확인 창(사용자 계정 컨트롤 등)이 떠 있어 클립보드를 쓸 수 없어요. 잠금을 풀거나 창을 닫은 뒤 다시 시도하세요.'**
  String get clipboardCantUsedBecauseDesktop;

  /// No description provided for @clipboardIsntAvailableConnection.
  ///
  /// In ko, this message translates to:
  /// **'이 연결에서는 클립보드를 쓸 수 없어요.'**
  String get clipboardIsntAvailableConnection;

  /// No description provided for @permissionsDesktopHaveChanged.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱의 권한이 바뀌었어요.'**
  String get permissionsDesktopHaveChanged;

  /// No description provided for @administratorChangedClipboardPermission.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 클립보드 권한을 바꿨어요.'**
  String get administratorChangedClipboardPermission;

  /// No description provided for @youveReachedClipboardLimitConnection.
  ///
  /// In ko, this message translates to:
  /// **'이 연결에서 클립보드를 쓸 수 있는 횟수를 넘었어요. 데스크톱에 다시 연결하면 쓸 수 있어요.'**
  String get youveReachedClipboardLimitConnection;

  /// No description provided for @receivedContentWasInvalidSo.
  ///
  /// In ko, this message translates to:
  /// **'받은 내용이 올바르지 않아 옮기지 않았어요. 다시 복사한 뒤 붙여넣으세요.'**
  String get receivedContentWasInvalidSo;

  /// No description provided for @clipboardConnectionDroppedSoWasnt.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 연결이 끊겨 옮기지 못했어요. 다시 복사한 뒤 붙여넣으세요.'**
  String get clipboardConnectionDroppedSoWasnt;

  /// No description provided for @couldntTransferWhatYouCopied.
  ///
  /// In ko, this message translates to:
  /// **'복사한 내용을 옮기지 못했어요. 다시 복사한 뒤 붙여넣으세요.'**
  String get couldntTransferWhatYouCopied;

  /// No description provided for @clipboardSharingStoppedReconnectDesktop.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 공유가 멈췄어요. 데스크톱에 다시 연결하면 쓸 수 있어요.'**
  String get clipboardSharingStoppedReconnectDesktop;

  /// No description provided for @clipboardSharingStopped.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 공유가 멈췄어요. {status}'**
  String clipboardSharingStopped(Object status);

  /// No description provided for @connectionRequest.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청'**
  String get connectionRequest;

  /// No description provided for @preparingVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 준비'**
  String get preparingVirtualDisplay;

  /// No description provided for @serverConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결'**
  String get serverConnection;

  /// No description provided for @waitingFirstFrame.
  ///
  /// In ko, this message translates to:
  /// **'첫 화면 수신 대기'**
  String get waitingFirstFrame;

  /// No description provided for @waitingHostUpdate.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 대기'**
  String get waitingHostUpdate;

  /// No description provided for @waitingDesktopRestart.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 다시 시작 대기'**
  String get waitingDesktopRestart;

  /// No description provided for @locked.
  ///
  /// In ko, this message translates to:
  /// **'잠겨 있어요'**
  String get locked;

  /// No description provided for @signScreen.
  ///
  /// In ko, this message translates to:
  /// **'로그인 화면'**
  String get signScreen;

  /// No description provided for @screenOff.
  ///
  /// In ko, this message translates to:
  /// **'화면이 꺼져 있어요'**
  String get screenOff;

  /// No description provided for @savedLastScreen.
  ///
  /// In ko, this message translates to:
  /// **'저장된 마지막 화면'**
  String get savedLastScreen;

  /// No description provided for @lastScreen.
  ///
  /// In ko, this message translates to:
  /// **'마지막 화면'**
  String get lastScreen;

  /// No description provided for @justNow.
  ///
  /// In ko, this message translates to:
  /// **'방금'**
  String get justNow;

  /// No description provided for @minAgo.
  ///
  /// In ko, this message translates to:
  /// **'{ageInMinutes}분 전'**
  String minAgo(Object ageInMinutes);

  /// No description provided for @text7.
  ///
  /// In ko, this message translates to:
  /// **'{ageInHours}시간 전'**
  String text7(num ageInHours);

  /// No description provided for @text8.
  ///
  /// In ko, this message translates to:
  /// **'{ageInDays}일 전'**
  String text8(num ageInDays);

  /// No description provided for @folder.
  ///
  /// In ko, this message translates to:
  /// **'폴더'**
  String get folder;

  /// No description provided for @mediaFoundationDeviceNotVerified.
  ///
  /// In ko, this message translates to:
  /// **'Media Foundation · 장치 미확인'**
  String get mediaFoundationDeviceNotVerified;

  /// No description provided for @checkingInitialization.
  ///
  /// In ko, this message translates to:
  /// **'초기화 확인 중'**
  String get checkingInitialization;

  /// No description provided for @outputNotVerified.
  ///
  /// In ko, this message translates to:
  /// **'{label} · 출력 확인 전'**
  String outputNotVerified(Object label);

  /// No description provided for @buildWithoutGpuEncoder.
  ///
  /// In ko, this message translates to:
  /// **'GPU 인코더가 포함되지 않은 빌드'**
  String get buildWithoutGpuEncoder;

  /// No description provided for @selectedGpuNotFound.
  ///
  /// In ko, this message translates to:
  /// **'선택한 GPU를 찾지 못함'**
  String get selectedGpuNotFound;

  /// No description provided for @vendorNotSupportedCurrentGpu.
  ///
  /// In ko, this message translates to:
  /// **'현재 GPU 경로에서 지원하지 않는 제조사'**
  String get vendorNotSupportedCurrentGpu;

  /// No description provided for @gpuDriverEncoderUnavailable.
  ///
  /// In ko, this message translates to:
  /// **'GPU 드라이버 인코더를 사용할 수 없음'**
  String get gpuDriverEncoderUnavailable;

  /// No description provided for @gpuApiVersionNotSupported.
  ///
  /// In ko, this message translates to:
  /// **'GPU API 버전 미지원'**
  String get gpuApiVersionNotSupported;

  /// No description provided for @requestedH264FormatNot.
  ///
  /// In ko, this message translates to:
  /// **'요청한 H.264 형식 미지원'**
  String get requestedH264FormatNot;

  /// No description provided for @gpuInitializationFailed.
  ///
  /// In ko, this message translates to:
  /// **'GPU 초기화 실패'**
  String get gpuInitializationFailed;

  /// No description provided for @gpuMemoryAllocationFailed.
  ///
  /// In ko, this message translates to:
  /// **'GPU 메모리 할당 실패'**
  String get gpuMemoryAllocationFailed;

  /// No description provided for @gpuFrameSubmissionFailed.
  ///
  /// In ko, this message translates to:
  /// **'GPU 프레임 제출 실패'**
  String get gpuFrameSubmissionFailed;

  /// No description provided for @gpuOutputTimedOut.
  ///
  /// In ko, this message translates to:
  /// **'GPU 출력 응답 시간 초과'**
  String get gpuOutputTimedOut;

  /// No description provided for @gpuOutputFormatCheckFailed.
  ///
  /// In ko, this message translates to:
  /// **'GPU 출력 형식 확인 실패'**
  String get gpuOutputFormatCheckFailed;

  /// No description provided for @gpuDeviceDisconnected.
  ///
  /// In ko, this message translates to:
  /// **'GPU 장치 연결이 끊김'**
  String get gpuDeviceDisconnected;

  /// No description provided for @gpuSettingsChangeFailed.
  ///
  /// In ko, this message translates to:
  /// **'GPU 설정 변경 실패'**
  String get gpuSettingsChangeFailed;

  /// No description provided for @x264SettingsRejected.
  ///
  /// In ko, this message translates to:
  /// **'x264 설정 거절'**
  String get x264SettingsRejected;

  /// No description provided for @x264InitializationFailed.
  ///
  /// In ko, this message translates to:
  /// **'x264 초기화 실패'**
  String get x264InitializationFailed;

  /// No description provided for @x264EncodingFailed.
  ///
  /// In ko, this message translates to:
  /// **'x264 인코딩 실패'**
  String get x264EncodingFailed;

  /// No description provided for @x264OutputFormatCheckFailed.
  ///
  /// In ko, this message translates to:
  /// **'x264 출력 형식 확인 실패'**
  String get x264OutputFormatCheckFailed;

  /// No description provided for @x264OutputTooLarge.
  ///
  /// In ko, this message translates to:
  /// **'x264 출력 크기 초과'**
  String get x264OutputTooLarge;

  /// No description provided for @x264MemoryAllocationFailed.
  ///
  /// In ko, this message translates to:
  /// **'x264 메모리 할당 실패'**
  String get x264MemoryAllocationFailed;

  /// No description provided for @x264ProcessingFailed.
  ///
  /// In ko, this message translates to:
  /// **'x264 처리 실패'**
  String get x264ProcessingFailed;

  /// No description provided for @ready.
  ///
  /// In ko, this message translates to:
  /// **'준비됨'**
  String get ready;

  /// No description provided for @autoInstall.
  ///
  /// In ko, this message translates to:
  /// **'자동 설치'**
  String get autoInstall;

  /// No description provided for @setupNeeded.
  ///
  /// In ko, this message translates to:
  /// **'설정 필요'**
  String get setupNeeded;

  /// No description provided for @notSupported.
  ///
  /// In ko, this message translates to:
  /// **'미지원'**
  String get notSupported;

  /// No description provided for @unknown2.
  ///
  /// In ko, this message translates to:
  /// **'확인 불가'**
  String get unknown2;

  /// No description provided for @installWinfsp.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 설치'**
  String get installWinfsp;

  /// No description provided for @startPrintSpooler.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러 시작'**
  String get startPrintSpooler;

  /// No description provided for @switchMultitap.
  ///
  /// In ko, this message translates to:
  /// **'Multitap용으로 바꾸기'**
  String get switchMultitap;

  /// No description provided for @sharedFolder.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더'**
  String get sharedFolder;

  /// No description provided for @installWinfsp2.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp를 설치하세요.'**
  String get installWinfsp2;

  /// No description provided for @runHostInstallerAgainInstall.
  ///
  /// In ko, this message translates to:
  /// **'Host 설치 파일을 다시 실행해 WinFsp를 설치하세요.'**
  String get runHostInstallerAgainInstall;

  /// No description provided for @refreshStatus.
  ///
  /// In ko, this message translates to:
  /// **'상태를 새로고침하세요.'**
  String get refreshStatus;

  /// No description provided for @runHostInstallerAgain.
  ///
  /// In ko, this message translates to:
  /// **'Host 설치 파일을 다시 실행하세요.'**
  String get runHostInstallerAgain;

  /// No description provided for @saveWorkRestartWindowsWont.
  ///
  /// In ko, this message translates to:
  /// **'작업을 저장하고 Windows를 다시 시작하세요. 자동으로 다시 시작하지 않아요.'**
  String get saveWorkRestartWindowsWont;

  /// No description provided for @printer.
  ///
  /// In ko, this message translates to:
  /// **'프린터'**
  String get printer;

  /// No description provided for @maySecuritySettingSoIsnt.
  ///
  /// In ko, this message translates to:
  /// **'보안 설정일 수 있어 자동으로 켜지 않아요. 필요하면 Windows 서비스에서 직접 켜세요.'**
  String get maySecuritySettingSoIsnt;

  /// No description provided for @refreshStatusMoment.
  ///
  /// In ko, this message translates to:
  /// **'잠시 후 상태를 새로고침하세요.'**
  String get refreshStatusMoment;

  /// No description provided for @startPrintSpoolerWindowsServices.
  ///
  /// In ko, this message translates to:
  /// **'Windows 서비스에서 Print Spooler를 시작하세요.'**
  String get startPrintSpoolerWindowsServices;

  /// No description provided for @turnMicrosoftPrintPdfWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows 기능에서 ‘Microsoft Print to PDF’를 켜세요.'**
  String get turnMicrosoftPrintPdfWindows;

  /// No description provided for @soundCard.
  ///
  /// In ko, this message translates to:
  /// **'사운드카드'**
  String get soundCard;

  /// No description provided for @administratorNeedsAddAudioDevice.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 Proxmox에서 오디오 장치를 추가해야 해요.'**
  String get administratorNeedsAddAudioDevice;

  /// No description provided for @checkAgainFromStreamingProxmox.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍이나 Proxmox 콘솔(로컬 PC는 본체 화면)에서 다시 확인하세요.'**
  String get checkAgainFromStreamingProxmox;

  /// No description provided for @deviceFactWinfsp.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 사용'**
  String get deviceFactWinfsp;

  /// No description provided for @deviceFactNoWinfsp.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 없음, 4 GB 넘는 파일 불가'**
  String get deviceFactNoWinfsp;

  /// No description provided for @deviceFactNoWinfspWebClient.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 없음, WebClient 꺼짐'**
  String get deviceFactNoWinfspWebClient;

  /// No description provided for @deviceFactUsbWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows 10(1809) 이상, Server 제외'**
  String get deviceFactUsbWindows;

  /// No description provided for @deviceFactUsbFilesMissing.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 파일 없음'**
  String get deviceFactUsbFilesMissing;

  /// No description provided for @deviceFactUsbFirstUse.
  ///
  /// In ko, this message translates to:
  /// **'처음 연결할 때 설치'**
  String get deviceFactUsbFirstUse;

  /// No description provided for @deviceFactUsbRunning.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 실행 중'**
  String get deviceFactUsbRunning;

  /// No description provided for @deviceFactUsbStopped.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 시작 안 됨'**
  String get deviceFactUsbStopped;

  /// No description provided for @deviceFactUsbInstalled.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 설치됨'**
  String get deviceFactUsbInstalled;

  /// No description provided for @deviceFactUsbForeign.
  ///
  /// In ko, this message translates to:
  /// **'다른 프로그램이 사용 중'**
  String get deviceFactUsbForeign;

  /// No description provided for @deviceSwitchUsbMultitap.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 기능을 Multitap용으로 바꾸세요.'**
  String get deviceSwitchUsbMultitap;

  /// No description provided for @deviceFactPrinterReady.
  ///
  /// In ko, this message translates to:
  /// **'연결하면 Multitap 프린터 추가'**
  String get deviceFactPrinterReady;

  /// No description provided for @deviceFactSpoolerOff.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러 꺼짐'**
  String get deviceFactSpoolerOff;

  /// No description provided for @deviceStartSpooler.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러를 시작하세요.'**
  String get deviceStartSpooler;

  /// No description provided for @deviceFactSpoolerDisabled.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러 ‘사용 안 함’'**
  String get deviceFactSpoolerDisabled;

  /// No description provided for @deviceFactSpoolerStarting.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러 시작 중'**
  String get deviceFactSpoolerStarting;

  /// No description provided for @deviceFactPdfMissing.
  ///
  /// In ko, this message translates to:
  /// **'PDF 인쇄 기능 없음'**
  String get deviceFactPdfMissing;

  /// No description provided for @deviceFactAudioPresent.
  ///
  /// In ko, this message translates to:
  /// **'출력 장치 있음'**
  String get deviceFactAudioPresent;

  /// No description provided for @deviceFactAudioAbsent.
  ///
  /// In ko, this message translates to:
  /// **'소리 출력 장치 없음'**
  String get deviceFactAudioAbsent;

  /// No description provided for @deviceFactAudioRemote.
  ///
  /// In ko, this message translates to:
  /// **'RDP 로그인 중'**
  String get deviceFactAudioRemote;

  /// No description provided for @checkDeviceStatus.
  ///
  /// In ko, this message translates to:
  /// **'장치 상태 확인 필요'**
  String get checkDeviceStatus;

  /// No description provided for @ready2.
  ///
  /// In ko, this message translates to:
  /// **'{l} 준비됨'**
  String ready2(Object l);

  /// No description provided for @notSupported2.
  ///
  /// In ko, this message translates to:
  /// **'{unsupported} 미지원'**
  String notSupported2(Object unsupported);

  /// No description provided for @couldntConfirmInstallResultRefresh.
  ///
  /// In ko, this message translates to:
  /// **'설치 결과를 확인하지 못했어요.\n다시 설치하기 전에 상태를 새로고침하세요.'**
  String get couldntConfirmInstallResultRefresh;

  /// No description provided for @restartWindowsApplySaveWork.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 다시 시작해야 적용돼요. 작업을 저장한 뒤 다시 시작하세요. 자동으로 다시 시작하지 않아요.'**
  String get restartWindowsApplySaveWork;

  /// No description provided for @winfspInstalled.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp를 설치했어요.\n{restartNotice}'**
  String winfspInstalled(Object restartNotice);

  /// No description provided for @winfspInstalledFromNextConnection.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp를 설치했어요.\n다음 연결부터 공유 폴더에서 큰 파일도 바로 열 수 있어요.'**
  String get winfspInstalledFromNextConnection;

  /// No description provided for @winfspAlreadyInstalled.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp가 이미 설치돼 있어요.'**
  String get winfspAlreadyInstalled;

  /// No description provided for @couldntInstallBecauseAnotherProgram.
  ///
  /// In ko, this message translates to:
  /// **'다른 프로그램을 설치하는 중이라 설치하지 못했어요.\n그 설치가 끝나면 다시 시도하세요.'**
  String get couldntInstallBecauseAnotherProgram;

  /// No description provided for @windowsPolicyBlockedInstallCheck.
  ///
  /// In ko, this message translates to:
  /// **'Windows 정책이 설치를 막았어요.\nWindows의 설치 허용 정책을 확인하세요.'**
  String get windowsPolicyBlockedInstallCheck;

  /// No description provided for @couldntVerifyInstallerFileDownload.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일을 확인하지 못했어요.\nHost 설치 파일을 다시 받아 실행하세요.'**
  String get couldntVerifyInstallerFileDownload;

  /// No description provided for @couldntInstallRestartWindowsThen.
  ///
  /// In ko, this message translates to:
  /// **'설치하지 못했어요.\nWindows를 다시 시작한 뒤 다시 시도하세요.'**
  String get couldntInstallRestartWindowsThen;

  /// No description provided for @couldntVerifyAfterInstallingRestart.
  ///
  /// In ko, this message translates to:
  /// **'설치한 뒤 확인하지 못했어요.\nWindows를 다시 시작한 뒤 상태를 확인하세요.'**
  String get couldntVerifyAfterInstallingRestart;

  /// No description provided for @couldntConfirmInstallResultRefresh2.
  ///
  /// In ko, this message translates to:
  /// **'설치 결과를 확인하지 못했어요.\n상태를 새로고침해 확인하세요.'**
  String get couldntConfirmInstallResultRefresh2;

  /// No description provided for @errorCode.
  ///
  /// In ko, this message translates to:
  /// **' (오류 코드: {code})'**
  String errorCode(Object code);

  /// No description provided for @meanwhileSharedFoldersUseBasic.
  ///
  /// In ko, this message translates to:
  /// **'{cause}{restart}\n그동안 공유 폴더는 기본 방식으로 동작해요.{safe}'**
  String meanwhileSharedFoldersUseBasic(
    Object cause,
    Object restart,
    Object safe,
  );

  /// No description provided for @saveWorkThenRestartWindows.
  ///
  /// In ko, this message translates to:
  /// **'작업을 저장한 뒤 Windows를 다시 시작하세요. 자동으로 다시 시작하지 않아요.'**
  String get saveWorkThenRestartWindows;

  /// No description provided for @usbConnectionFeatureSwitchedMultitap.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 기능을 Multitap용으로 바꿨어요.\n다음 연결부터 USB를 쓸 수 있어요.'**
  String get usbConnectionFeatureSwitchedMultitap;

  /// No description provided for @restartWindowsSwitchUsbConnection.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 기능을 바꾸려면 Windows를 다시 시작해야 해요.\n{restartNotice}'**
  String restartWindowsSwitchUsbConnection(Object restartNotice);

  /// No description provided for @alreadySetUpMultitap.
  ///
  /// In ko, this message translates to:
  /// **'이미 Multitap용이에요.'**
  String get alreadySetUpMultitap;

  /// No description provided for @couldntRemoveOtherProgramsUsb.
  ///
  /// In ko, this message translates to:
  /// **'다른 프로그램의 USB 연결 기능을 지우지 못했어요.\n그 프로그램을 먼저 끄거나 제거한 뒤 다시 시도하세요.'**
  String get couldntRemoveOtherProgramsUsb;

  /// No description provided for @multitapUsbDriverCantInstalled.
  ///
  /// In ko, this message translates to:
  /// **'Multitap USB 드라이버를 설치할 수 없어요.\nHost 설치 파일을 다시 실행하세요.'**
  String get multitapUsbDriverCantInstalled;

  /// No description provided for @couldntInstallMultitapUsbDriver.
  ///
  /// In ko, this message translates to:
  /// **'Multitap USB 드라이버를 설치하지 못했어요.\n상태를 새로고침한 뒤 다시 시도하세요.'**
  String get couldntInstallMultitapUsbDriver;

  /// No description provided for @printSpoolerHasStartedYou.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러를 시작했어요.\n다음 연결부터 프린터를 쓸 수 있어요.'**
  String get printSpoolerHasStartedYou;

  /// No description provided for @printSpoolerSetDisabledIsnt.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러가 ‘사용 안 함’으로 설정돼 있어요.\n자동으로 바꾸지 않아요.'**
  String get printSpoolerSetDisabledIsnt;

  /// No description provided for @couldntStartPrintSpoolerCheck.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러를 시작하지 못했어요.\nWindows 서비스에서 Print Spooler 상태를 확인하세요.'**
  String get couldntStartPrintSpoolerCheck;

  /// No description provided for @couldntCheckPrintSpoolerStatus.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러 상태를 확인하지 못했어요.\n상태를 새로고침하세요.'**
  String get couldntCheckPrintSpoolerStatus;

  /// No description provided for @oldMultitapVirtualDisplayAdapter.
  ///
  /// In ko, this message translates to:
  /// **'이전 Multitap 가상 화면 어댑터'**
  String get oldMultitapVirtualDisplayAdapter;

  /// No description provided for @leftoverAdapterMadeByMultitap.
  ///
  /// In ko, this message translates to:
  /// **'Multitap이 만든 남은 어댑터'**
  String get leftoverAdapterMadeByMultitap;

  /// No description provided for @sameDriverNoMultitapRecord.
  ///
  /// In ko, this message translates to:
  /// **'같은 드라이버 · Multitap 기록 없음'**
  String get sameDriverNoMultitapRecord;

  /// No description provided for @couldntCheckVirtualDisplayAdapters.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 어댑터를 확인하지 못했어요.'**
  String get couldntCheckVirtualDisplayAdapters;

  /// No description provided for @virtualDisplayAdaptersUseNone.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 어댑터: 사용 중 {current}개 · 남은 장치 없음'**
  String virtualDisplayAdaptersUseNone(Object current);

  /// No description provided for @virtualDisplayAdaptersUseLeft.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 어댑터: 사용 중 {current}개 · 남은 장치 {leftover}개'**
  String virtualDisplayAdaptersUseLeft(Object current, Object leftover);

  /// No description provided for @couldntConfirmCleanupResultRefresh.
  ///
  /// In ko, this message translates to:
  /// **'정리 결과를 확인하지 못했어요.\n상태를 새로고침하세요.'**
  String get couldntConfirmCleanupResultRefresh;

  /// No description provided for @listChangedMeantimeRefreshStatus.
  ///
  /// In ko, this message translates to:
  /// **'목록이 그사이 바뀌었어요.\n상태를 새로고침한 뒤 다시 확인하세요.'**
  String get listChangedMeantimeRefreshStatus;

  /// No description provided for @couldntReadVirtualDisplayDevices.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치를 읽지 못했어요.\n상태를 새로고침하세요.'**
  String get couldntReadVirtualDisplayDevices;

  /// No description provided for @text9.
  ///
  /// In ko, this message translates to:
  /// **'{removed}개를 지웠어요.'**
  String text9(num removed);

  /// No description provided for @afterYouRestartWindowsWont.
  ///
  /// In ko, this message translates to:
  /// **'{pending}개는 Windows를 다시 시작하면 사라져요. 자동으로 다시 시작하지 않아요.'**
  String afterYouRestartWindowsWont(num pending);

  /// No description provided for @refreshStatusThenTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'{failed}개는 지우지 못했어요. 상태를 새로고침한 뒤 다시 시도하세요.'**
  String refreshStatusThenTryAgain(num failed);

  /// No description provided for @thereWereNoDevicesRemove.
  ///
  /// In ko, this message translates to:
  /// **'지울 장치가 없었어요.'**
  String get thereWereNoDevicesRemove;

  /// No description provided for @noInfo.
  ///
  /// In ko, this message translates to:
  /// **'정보 없음'**
  String get noInfo;

  /// No description provided for @multitapHostDiagnostics.
  ///
  /// In ko, this message translates to:
  /// **'Multitap Host 진단'**
  String get multitapHostDiagnostics;

  /// No description provided for @hostVersion.
  ///
  /// In ko, this message translates to:
  /// **'Host 버전: {state}'**
  String hostVersion(Object state);

  /// No description provided for @desktop2.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱: {state}'**
  String desktop2(Object state);

  /// No description provided for @server2.
  ///
  /// In ko, this message translates to:
  /// **'서버: {state}'**
  String server2(Object state);

  /// No description provided for @registered.
  ///
  /// In ko, this message translates to:
  /// **'등록됨'**
  String get registered;

  /// No description provided for @notRegistered.
  ///
  /// In ko, this message translates to:
  /// **'미등록'**
  String get notRegistered;

  /// No description provided for @serverRegistration.
  ///
  /// In ko, this message translates to:
  /// **'서버 등록: {state}'**
  String serverRegistration(Object state);

  /// No description provided for @installed.
  ///
  /// In ko, this message translates to:
  /// **'설치됨'**
  String get installed;

  /// No description provided for @notInstalled.
  ///
  /// In ko, this message translates to:
  /// **'미설치'**
  String get notInstalled;

  /// No description provided for @serviceInstall.
  ///
  /// In ko, this message translates to:
  /// **'서비스 설치: {service}'**
  String serviceInstall(Object service);

  /// No description provided for @running.
  ///
  /// In ko, this message translates to:
  /// **'실행 중'**
  String get running;

  /// No description provided for @serviceState.
  ///
  /// In ko, this message translates to:
  /// **'서비스 실행: {service}'**
  String serviceState(Object service);

  /// No description provided for @serviceQueryError.
  ///
  /// In ko, this message translates to:
  /// **'서비스 조회 오류: {service}'**
  String serviceQueryError(Object service);

  /// No description provided for @serverResponded.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답 확인'**
  String get serverResponded;

  /// No description provided for @serverConnection2.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결: {verifiedOnline}'**
  String serverConnection2(Object verifiedOnline);

  /// No description provided for @serverConnectionStage.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 단계: {brokerStates}'**
  String serverConnectionStage(Object brokerStates);

  /// No description provided for @s.
  ///
  /// In ko, this message translates to:
  /// **'{value}초'**
  String s(Object value);

  /// No description provided for @sinceLastServerResponse.
  ///
  /// In ko, this message translates to:
  /// **'마지막 서버 응답 경과: {validAge}'**
  String sinceLastServerResponse(Object validAge);

  /// No description provided for @serverConnectionFailureReason.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 실패 사유: {broker}'**
  String serverConnectionFailureReason(Object broker);

  /// No description provided for @yes.
  ///
  /// In ko, this message translates to:
  /// **'있음'**
  String get yes;

  /// No description provided for @windowsSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인: {state}'**
  String windowsSign(Object state);

  /// No description provided for @virtualDisplayDriver.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버: {display}'**
  String virtualDisplayDriver(Object display);

  /// No description provided for @started.
  ///
  /// In ko, this message translates to:
  /// **'시작됨'**
  String get started;

  /// No description provided for @virtualDisplayDevice.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치: {display}'**
  String virtualDisplayDevice(Object display);

  /// No description provided for @virtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면: {display}'**
  String virtualDisplay(Object display);

  /// No description provided for @virtualMonitor.
  ///
  /// In ko, this message translates to:
  /// **'가상 모니터 {m}: {m2} × {m3}'**
  String virtualMonitor(Object m, Object m2, Object m3);

  /// No description provided for @needed.
  ///
  /// In ko, this message translates to:
  /// **'필요'**
  String get needed;

  /// No description provided for @notNeeded.
  ///
  /// In ko, this message translates to:
  /// **'필요 없음'**
  String get notNeeded;

  /// No description provided for @displayRecovery.
  ///
  /// In ko, this message translates to:
  /// **'화면 복구: {display}'**
  String displayRecovery(Object display);

  /// No description provided for @notRequested2.
  ///
  /// In ko, this message translates to:
  /// **'요청 없음'**
  String get notRequested2;

  /// No description provided for @windowsRestart.
  ///
  /// In ko, this message translates to:
  /// **'Windows 다시 시작: {display}'**
  String windowsRestart(Object display);

  /// No description provided for @saved.
  ///
  /// In ko, this message translates to:
  /// **'저장됨'**
  String get saved;

  /// No description provided for @windowsAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정: {account}'**
  String windowsAccount(Object account);

  /// No description provided for @unknownVendor.
  ///
  /// In ko, this message translates to:
  /// **'제조사 미확인'**
  String get unknownVendor;

  /// No description provided for @anotherSettingsChangeWasMade.
  ///
  /// In ko, this message translates to:
  /// **'다른 설정 변경이 있었어요. 현재 선택을 새로 확인하세요.'**
  String get anotherSettingsChangeWasMade;

  /// No description provided for @selectedGpuChangedCheckList.
  ///
  /// In ko, this message translates to:
  /// **'선택한 GPU가 바뀌었어요. 목록을 새로 확인하세요.'**
  String get selectedGpuChangedCheckList;

  /// No description provided for @anotherEncoderSettingsTaskProgress.
  ///
  /// In ko, this message translates to:
  /// **'다른 인코더 설정 작업 중이에요. 상태를 다시 확인하세요.'**
  String get anotherEncoderSettingsTaskProgress;

  /// No description provided for @couldntVerifyEncoderSettingsProtection.
  ///
  /// In ko, this message translates to:
  /// **'인코더 설정의 보호 상태를 확인하지 못했어요. Host 설치 파일을 다시 실행해 복구하세요.'**
  String get couldntVerifyEncoderSettingsProtection;

  /// No description provided for @couldntReadEncoderSettingsFile.
  ///
  /// In ko, this message translates to:
  /// **'인코더 설정 파일을 읽지 못했어요. 상태를 새로고침하세요.'**
  String get couldntReadEncoderSettingsFile;

  /// No description provided for @couldntConfirmSaveResultCheck.
  ///
  /// In ko, this message translates to:
  /// **'저장 결과를 확인하지 못했어요. 다시 적용하기 전에 상태를 확인하세요.'**
  String get couldntConfirmSaveResultCheck;

  /// No description provided for @saveResponseDoesntMatchCurrent.
  ///
  /// In ko, this message translates to:
  /// **'저장 응답과 현재 설정이 일치하지 않아요. 상태를 다시 확인하세요.'**
  String get saveResponseDoesntMatchCurrent;

  /// No description provided for @couldntReadGpuListCpu.
  ///
  /// In ko, this message translates to:
  /// **'GPU 목록을 읽지 못했어요. CPU 설정은 그대로 사용할 수 있어요.'**
  String get couldntReadGpuListCpu;

  /// No description provided for @couldntCheckEncoderSettingsUpdate.
  ///
  /// In ko, this message translates to:
  /// **'인코더 설정을 확인하지 못했어요. Host를 업데이트하거나 상태를 다시 확인하세요.'**
  String get couldntCheckEncoderSettingsUpdate;

  /// No description provided for @couldntReadStatus.
  ///
  /// In ko, this message translates to:
  /// **'상태 읽기 실패'**
  String get couldntReadStatus;

  /// No description provided for @hostServiceMissing.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스 없음'**
  String get hostServiceMissing;

  /// No description provided for @hostServiceOff.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스 꺼짐'**
  String get hostServiceOff;

  /// No description provided for @checkHostService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스 확인 필요'**
  String get checkHostService;

  /// No description provided for @installationNeedsFinishing.
  ///
  /// In ko, this message translates to:
  /// **'설치 마무리 필요'**
  String get installationNeedsFinishing;

  /// No description provided for @checkVirtualDisplayDriver.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버 확인 필요'**
  String get checkVirtualDisplayDriver;

  /// No description provided for @restartWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows 다시 시작 필요'**
  String get restartWindows;

  /// No description provided for @virtualDisplayNeedsRecovery.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 복구 필요'**
  String get virtualDisplayNeedsRecovery;

  /// No description provided for @serverConnectionNeeded.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 필요'**
  String get serverConnectionNeeded;

  /// No description provided for @administratorApprovalNeeded.
  ///
  /// In ko, this message translates to:
  /// **'관리자 승인 필요'**
  String get administratorApprovalNeeded;

  /// No description provided for @notConnectedServer.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 안 됨'**
  String get notConnectedServer;

  /// No description provided for @windowsSignNeeded.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인 필요'**
  String get windowsSignNeeded;

  /// No description provided for @streamingDisplaySwitchNeeded.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 화면 전환 필요'**
  String get streamingDisplaySwitchNeeded;

  /// No description provided for @preparingVirtualDisplay2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 준비 중'**
  String get preparingVirtualDisplay2;

  /// No description provided for @checkVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 확인 필요'**
  String get checkVirtualDisplay;

  /// No description provided for @autoSignSetupNeeded.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 설정 필요'**
  String get autoSignSetupNeeded;

  /// No description provided for @checkWindowsSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인 확인 필요'**
  String get checkWindowsSign;

  /// No description provided for @checkDevices.
  ///
  /// In ko, this message translates to:
  /// **'장치 확인 필요'**
  String get checkDevices;

  /// No description provided for @desktopRegisteredServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에 등록된 데스크톱 · {id}'**
  String desktopRegisteredServer(Object id);

  /// No description provided for @desktopRegisteredServer2.
  ///
  /// In ko, this message translates to:
  /// **'서버에 등록된 데스크톱'**
  String get desktopRegisteredServer2;

  /// No description provided for @checkedJustNow.
  ///
  /// In ko, this message translates to:
  /// **'방금 확인'**
  String get checkedJustNow;

  /// No description provided for @checkedSAgo.
  ///
  /// In ko, this message translates to:
  /// **'{ageSeconds}초 전 확인'**
  String checkedSAgo(Object ageSeconds);

  /// No description provided for @existingVirtualDisplaySettingsNeed.
  ///
  /// In ko, this message translates to:
  /// **'기존 가상 화면 설정을 확인해야 해요.\n진단 정보를 복사해 관리자에게 알려 주세요.'**
  String get existingVirtualDisplaySettingsNeed;

  /// No description provided for @virtualDisplayDeviceDidntStart.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치가 시작되지 않았어요.\nWindows를 다시 시작한 뒤 상태를 확인하세요.'**
  String get virtualDisplayDeviceDidntStart;

  /// No description provided for @couldntVerifyVirtualDisplayDriver.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버 파일을 확인하지 못했어요.\nHost 설치 파일을 다시 받아 실행하세요.'**
  String get couldntVerifyVirtualDisplayDriver;

  /// No description provided for @finishInstallation.
  ///
  /// In ko, this message translates to:
  /// **'설치 마무리'**
  String get finishInstallation;

  /// No description provided for @virtualDisplayDriverSharedFolder.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버와 공유 폴더 구성 요소(WinFsp)가 아직 없어요.\n설치해야 이 데스크톱에 연결할 수 있어요.'**
  String get virtualDisplayDriverSharedFolder;

  /// No description provided for @virtualDisplayDriverIsntInstalled.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버가 아직 없어요.\n설치해야 이 데스크톱에 연결할 수 있어요.'**
  String get virtualDisplayDriverIsntInstalled;

  /// No description provided for @sharedFolderComponentWinfspIsnt.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 구성 요소(WinFsp)가 아직 없어요.\n설치하면 큰 파일도 바로 열 수 있어요.'**
  String get sharedFolderComponentWinfspIsnt;

  /// No description provided for @trustVirtualDisplayDriversSignature.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버의 서명을 신뢰할까요?'**
  String get trustVirtualDisplayDriversSignature;

  /// No description provided for @thenDriverSharedFolderComponent.
  ///
  /// In ko, this message translates to:
  /// **'이어서 드라이버와 공유 폴더 구성 요소(WinFsp)를 설치해요.'**
  String get thenDriverSharedFolderComponent;

  /// No description provided for @thenDriverInstalled.
  ///
  /// In ko, this message translates to:
  /// **'이어서 드라이버를 설치해요.'**
  String get thenDriverInstalled;

  /// No description provided for @virtualDisplayOnlyOriginalSunshine.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면만 Sunshine이 서명한 원본 드라이버를 그대로 써요.\n서명 인증서 하나를 Windows 신뢰 저장소(Root·TrustedPublisher)에 넣어요.\n{winfsp}'**
  String virtualDisplayOnlyOriginalSunshine(Object winfsp);

  /// No description provided for @trustInstall.
  ///
  /// In ko, this message translates to:
  /// **'신뢰하고 설치'**
  String get trustInstall;

  /// No description provided for @installVirtualDisplayDriver.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버를 설치할까요?'**
  String get installVirtualDisplayDriver;

  /// No description provided for @sharedFolderComponentWinfspInstalled.
  ///
  /// In ko, this message translates to:
  /// **'\n공유 폴더 구성 요소(WinFsp)도 함께 설치해요.'**
  String get sharedFolderComponentWinfspInstalled;

  /// No description provided for @installsAlreadyTrustedOriginalSunshine.
  ///
  /// In ko, this message translates to:
  /// **'이미 신뢰한 Sunshine 서명 원본 드라이버를 설치해요.{winfsp}'**
  String installsAlreadyTrustedOriginalSunshine(Object winfsp);

  /// No description provided for @couldntConfirmVirtualDisplayDriver.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버 설치 결과를 확인하지 못했어요.\n상태를 새로고침해 확인하세요.'**
  String get couldntConfirmVirtualDisplayDriver;

  /// No description provided for @virtualDisplayDriverAlreadyInstalled.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버가 이미 설치돼 있어요.'**
  String get virtualDisplayDriverAlreadyInstalled;

  /// No description provided for @virtualDisplayDriverInstalled.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버를 설치했어요.'**
  String get virtualDisplayDriverInstalled;

  /// No description provided for @rdpSignYouCantCheck.
  ///
  /// In ko, this message translates to:
  /// **'RDP 로그인에서는 화면이 켜졌는지 확인할 수 없어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 확인하세요.'**
  String get rdpSignYouCantCheck;

  /// No description provided for @couldntRestartHostServiceRestart.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스를 다시 시작하지 못했어요. 진단 보고서에서 서비스를 다시 시작하세요.'**
  String get couldntRestartHostServiceRestart;

  /// No description provided for @notInstalledBecauseSignatureVerification.
  ///
  /// In ko, this message translates to:
  /// **'서명 확인이 끝나지 않아 설치하지 않았어요.\n설치 마무리를 다시 눌러 주세요.'**
  String get notInstalledBecauseSignatureVerification;

  /// No description provided for @couldntRegisterDriverSignatureWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows에 드라이버 서명을 등록하지 못했어요.\nWindows의 인증서 정책을 확인하세요.'**
  String get couldntRegisterDriverSignatureWindows;

  /// No description provided for @existingVirtualDisplaySettingsNeed2.
  ///
  /// In ko, this message translates to:
  /// **'기존 가상 화면 설정을 확인해야 해요.\n기존 장치는 바꾸지 않았어요. 진단 정보를 복사해 관리자에게 알려 주세요.'**
  String get existingVirtualDisplaySettingsNeed2;

  /// No description provided for @anotherInstallationProgressTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'다른 설치가 진행 중이에요.\n잠시 후 다시 시도하세요.'**
  String get anotherInstallationProgressTryAgain;

  /// No description provided for @couldntSaveInstallResultTry.
  ///
  /// In ko, this message translates to:
  /// **'설치 결과를 저장하지 못했어요.\n다시 시도하세요.'**
  String get couldntSaveInstallResultTry;

  /// No description provided for @windowsCouldntInstallVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 가상 화면 드라이버를 설치하지 못했어요.\nWindows를 다시 시작한 뒤 다시 시도하세요.'**
  String get windowsCouldntInstallVirtualDisplay;

  /// No description provided for @restart.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작'**
  String get restart;

  /// No description provided for @winfspWasAlreadyInstalled.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp는 이미 설치돼 있었어요.'**
  String get winfspWasAlreadyInstalled;

  /// No description provided for @nothingInstall.
  ///
  /// In ko, this message translates to:
  /// **'설치할 것이 없었어요.'**
  String get nothingInstall;

  /// No description provided for @savedInfoHasChangedGone.
  ///
  /// In ko, this message translates to:
  /// **'저장 정보가 바뀌었거나 없어졌어요. 새 상태를 확인한 뒤 다시 선택하세요.'**
  String get savedInfoHasChangedGone;

  /// No description provided for @couldntConfirmDeletionDontRun.
  ///
  /// In ko, this message translates to:
  /// **'삭제 완료를 확인하지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.'**
  String get couldntConfirmDeletionDontRun;

  /// No description provided for @updateHostClearSavedInfo.
  ///
  /// In ko, this message translates to:
  /// **'Host를 업데이트한 뒤 저장 정보를 지울 수 있어요.'**
  String get updateHostClearSavedInfo;

  /// No description provided for @couldntConfirmServiceRestartedDont.
  ///
  /// In ko, this message translates to:
  /// **'서비스를 다시 시작했는지 확인하지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.'**
  String get couldntConfirmServiceRestartedDont;

  /// No description provided for @connectionRequestChangedCheckCurrent.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청이 바뀌었어요. 현재 요청을 확인한 뒤 다시 취소하세요.'**
  String get connectionRequestChangedCheckCurrent;

  /// No description provided for @checkServerAddressPort1.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소와 1~65535 포트를 확인하세요.'**
  String get checkServerAddressPort1;

  /// No description provided for @agreeHttpServerAddressFirst2.
  ///
  /// In ko, this message translates to:
  /// **'이 HTTP 서버 주소에 먼저 동의해 주세요.'**
  String get agreeHttpServerAddressFirst2;

  /// No description provided for @currentAddressDoesntMatchServers.
  ///
  /// In ko, this message translates to:
  /// **'현재 주소와 서버의 접속 설정이 맞지 않아요. 주소·포트와 서버 설정을 확인하세요.'**
  String get currentAddressDoesntMatchServers;

  /// No description provided for @cantConfirmHttpSupportServer.
  ///
  /// In ko, this message translates to:
  /// **'서버의 HTTP 지원을 확인할 수 없어요. 서버를 업데이트하거나 HTTPS 주소를 사용하세요.'**
  String get cantConfirmHttpSupportServer;

  /// No description provided for @httpConnectionsOffServerAsk.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 HTTP 연결이 꺼져 있어요. 관리자에게 요청하세요.'**
  String get httpConnectionsOffServerAsk;

  /// No description provided for @addressIsntServersAllowedList.
  ///
  /// In ko, this message translates to:
  /// **'이 주소는 서버의 허용 주소에 등록되지 않았어요. 주소와 포트를 확인하세요.'**
  String get addressIsntServersAllowedList;

  /// No description provided for @addressOnlyFirstSetupSet.
  ///
  /// In ko, this message translates to:
  /// **'이 주소는 초기 설정에만 사용할 수 있어요. 관리자 웹에서 접속 주소를 설정하세요.'**
  String get addressOnlyFirstSetupSet;

  /// No description provided for @couldntConnectSecurelyOverHttps2.
  ///
  /// In ko, this message translates to:
  /// **'HTTPS로 안전하게 연결하지 못했어요. 서버 주소와 인증서를 확인하세요.'**
  String get couldntConnectSecurelyOverHttps2;

  /// No description provided for @couldntConnectAddressPortCheck.
  ///
  /// In ko, this message translates to:
  /// **'이 주소와 포트로 연결하지 못했어요. 서버 실행 상태와 포트를 확인하세요.'**
  String get couldntConnectAddressPortCheck;

  /// No description provided for @serverDidntRespondTimeCheck2.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답 시간이 초과됐어요. 주소·포트와 서버 상태를 확인하세요.'**
  String get serverDidntRespondTimeCheck2;

  /// No description provided for @couldntReachServerBecauseNetwork.
  ///
  /// In ko, this message translates to:
  /// **'네트워크 문제로 서버에 연결하지 못했어요. 네트워크 연결을 확인하세요.'**
  String get couldntReachServerBecauseNetwork;

  /// No description provided for @couldntVerifyMultitapServerResponse.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 서버 응답을 확인하지 못했어요. 주소·포트와 서버 상태를 확인하세요.'**
  String get couldntVerifyMultitapServerResponse;

  /// No description provided for @couldntReadHostSettingsResponse.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정 응답을 읽지 못했어요. Host 설치 파일을 다시 실행하세요.'**
  String get couldntReadHostSettingsResponse;

  /// No description provided for @couldntFinishSetupRefreshStatus.
  ///
  /// In ko, this message translates to:
  /// **'설정을 완료하지 못했어요. 상태를 새로고침해 확인하세요.'**
  String get couldntFinishSetupRefreshStatus;

  /// No description provided for @deletionIsntConfirmedYetDont.
  ///
  /// In ko, this message translates to:
  /// **'삭제 완료를 아직 확인하지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.'**
  String get deletionIsntConfirmedYetDont;

  /// No description provided for @serviceRestartIsntConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'서비스를 다시 시작했는지 아직 확인하지 못했어요. 다시 실행하지 말고 잠시 후 상태를 새로고침하세요.'**
  String get serviceRestartIsntConfirmedYet;

  /// No description provided for @requestCancellationIsntConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'요청 취소 결과를 아직 확인하지 못했어요. 다시 취소하지 말고 상태를 새로고침하세요.'**
  String get requestCancellationIsntConfirmedYet;

  /// No description provided for @encoderSaveIsntConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'인코더 저장 결과를 아직 확인하지 못했어요. 다시 저장하지 말고 상태를 확인하세요.'**
  String get encoderSaveIsntConfirmedYet;

  /// No description provided for @winfspInstallIsntConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 설치 결과를 아직 확인하지 못했어요. 다시 설치하지 말고 잠시 후 상태를 새로고침하세요.'**
  String get winfspInstallIsntConfirmedYet;

  /// No description provided for @virtualDisplayDriverInstallIsnt.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버 설치 결과를 아직 확인하지 못했어요. 다시 설치하지 말고 잠시 후 상태를 새로고침하세요.'**
  String get virtualDisplayDriverInstallIsnt;

  /// No description provided for @deviceChangeIsntConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'장치 변경 결과를 아직 확인하지 못했어요. 다시 실행하지 말고 잠시 후 상태를 새로고침하세요.'**
  String get deviceChangeIsntConfirmedYet;

  /// No description provided for @passwordResultIsntConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 결과를 아직 확인하지 못했어요.'**
  String get passwordResultIsntConfirmedYet;

  /// No description provided for @autoSignRequestResultIsnt.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 요청 결과를 아직 확인하지 못했어요. 다시 적용하지 말고 결과를 확인하세요.'**
  String get autoSignRequestResultIsnt;

  /// No description provided for @settingsResponseTimedOutRefresh.
  ///
  /// In ko, this message translates to:
  /// **'설정 응답 시간이 초과됐어요. 상태를 새로고침해 적용 결과를 확인한 뒤 다시 시도하세요.'**
  String get settingsResponseTimedOutRefresh;

  /// No description provided for @couldntFindHostConfigurationFile.
  ///
  /// In ko, this message translates to:
  /// **'Host 구성 파일을 찾지 못했어요. Host 설치 파일을 다시 실행하세요.'**
  String get couldntFindHostConfigurationFile;

  /// No description provided for @setupTimeRanOutStart.
  ///
  /// In ko, this message translates to:
  /// **'설정 시간이 끝났어요. 본인 확인부터 다시 시작하세요.'**
  String get setupTimeRanOutStart;

  /// No description provided for @checkAgainMoment.
  ///
  /// In ko, this message translates to:
  /// **'잠시 후 다시 확인하세요.'**
  String get checkAgainMoment;

  /// No description provided for @couldntFinishIdentityVerificationTry.
  ///
  /// In ko, this message translates to:
  /// **'본인 확인을 마치지 못했어요. 다시 시도하세요.'**
  String get couldntFinishIdentityVerificationTry;

  /// No description provided for @checkResultOtherSettingsChange.
  ///
  /// In ko, this message translates to:
  /// **'다른 설정 변경 결과를 먼저 확인하세요.'**
  String get checkResultOtherSettingsChange;

  /// No description provided for @settingChangedSignOutThen.
  ///
  /// In ko, this message translates to:
  /// **'설정은 바뀌었어요. 로그아웃한 뒤 다시 로그인하세요.'**
  String get settingChangedSignOutThen;

  /// No description provided for @couldntConfirmChange.
  ///
  /// In ko, this message translates to:
  /// **'변경 결과를 확인하지 못했어요.'**
  String get couldntConfirmChange;

  /// No description provided for @twoStepVerificationRequired.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증이 필요해요.'**
  String get twoStepVerificationRequired;

  /// No description provided for @documentTooLargeOver256.
  ///
  /// In ko, this message translates to:
  /// **'문서가 너무 커서(256 MB 초과) 받지 못했어요.'**
  String get documentTooLargeOver256;

  /// No description provided for @tooManyPrintJobsStill.
  ///
  /// In ko, this message translates to:
  /// **'아직 처리하지 않은 인쇄가 많아 새 문서를 받지 않았어요.'**
  String get tooManyPrintJobsStill;

  /// No description provided for @couldntSaveDocumentTemporarilyDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 문서를 임시로 저장하지 못해 받지 못했어요. 데스크톱의 디스크 공간을 확인하세요.'**
  String get couldntSaveDocumentTemporarilyDesktop;

  /// No description provided for @couldntReceiveDocumentPrintAgain.
  ///
  /// In ko, this message translates to:
  /// **'문서를 받지 못했어요. 데스크톱에서 다시 인쇄하세요.'**
  String get couldntReceiveDocumentPrintAgain;

  /// No description provided for @microsoftPrintPdfMissingDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 Microsoft Print to PDF가 없어 프린터를 만들지 못했어요. 관리자에게 알려 주세요.'**
  String get microsoftPrintPdfMissingDesktop;

  /// No description provided for @printSpoolerServiceOffDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱의 인쇄 스풀러 서비스가 꺼져 있어 프린터를 만들지 못했어요. 관리자에게 알려 주세요.'**
  String get printSpoolerServiceOffDesktop;

  /// No description provided for @anotherPrinterSameNameExists.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 같은 이름의 다른 프린터가 있어 만들지 않았어요. 데스크톱에서 그 프린터를 지운 뒤 다시 연결하세요.'**
  String get anotherPrinterSameNameExists;

  /// No description provided for @couldntCreatePrinterDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 프린터를 만들지 못했어요.'**
  String get couldntCreatePrinterDesktop;

  /// No description provided for @administratorTurnedOffPrinterPermission.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 프린터 권한을 해제했어요.'**
  String get administratorTurnedOffPrinterPermission;

  /// No description provided for @cantFindSelectedPrinterCheck.
  ///
  /// In ko, this message translates to:
  /// **'선택한 프린터를 찾을 수 없어요. 프린터가 켜져 있는지, 목록에서 지워지지 않았는지 확인해 주세요.'**
  String get cantFindSelectedPrinterCheck;

  /// No description provided for @printerOfflineQueuedDocumentsPrint.
  ///
  /// In ko, this message translates to:
  /// **'프린터가 오프라인이에요. 켜지면 대기열의 문서가 인쇄돼요.'**
  String get printerOfflineQueuedDocumentsPrint;

  /// No description provided for @printerPausedResumeWindowsPrint.
  ///
  /// In ko, this message translates to:
  /// **'프린터가 일시 중지돼 있어요. Windows에서 일시 중지를 풀어야 인쇄돼요.'**
  String get printerPausedResumeWindowsPrint;

  /// No description provided for @printerNeedsAttentionPaperToner.
  ///
  /// In ko, this message translates to:
  /// **'프린터에 용지·토너·덮개 등 확인할 것이 있어요.'**
  String get printerNeedsAttentionPaperToner;

  /// No description provided for @printerHasProblemCheckPrinter.
  ///
  /// In ko, this message translates to:
  /// **'프린터에 문제가 있어요. 프린터 상태를 확인해 주세요.'**
  String get printerHasProblemCheckPrinter;

  /// No description provided for @storageFullNotWritableSo.
  ///
  /// In ko, this message translates to:
  /// **'저장 공간이 부족하거나 쓸 수 없어서 문서를 받지 못했어요.'**
  String get storageFullNotWritableSo;

  /// No description provided for @printingCanceled.
  ///
  /// In ko, this message translates to:
  /// **'인쇄를 취소했어요.'**
  String get printingCanceled;

  /// No description provided for @couldntOpenDocumentSoWasnt.
  ///
  /// In ko, this message translates to:
  /// **'문서를 열지 못해 인쇄하지 못했어요. PDF로 저장할 수 있어요.'**
  String get couldntOpenDocumentSoWasnt;

  /// No description provided for @printingIsntAvailablePc.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 인쇄를 사용할 수 없어요.'**
  String get printingIsntAvailablePc;

  /// No description provided for @couldntSaveFile.
  ///
  /// In ko, this message translates to:
  /// **'파일을 저장하지 못했어요.'**
  String get couldntSaveFile;

  /// No description provided for @couldntPrint.
  ///
  /// In ko, this message translates to:
  /// **'인쇄하지 못했어요.'**
  String get couldntPrint;

  /// No description provided for @am.
  ///
  /// In ko, this message translates to:
  /// **'오전'**
  String get am;

  /// No description provided for @pm.
  ///
  /// In ko, this message translates to:
  /// **'오후'**
  String get pm;

  /// No description provided for @soon.
  ///
  /// In ko, this message translates to:
  /// **'곧'**
  String get soon;

  /// No description provided for @min.
  ///
  /// In ko, this message translates to:
  /// **'{left}분 뒤'**
  String min(Object left);

  /// No description provided for @today.
  ///
  /// In ko, this message translates to:
  /// **'오늘'**
  String get today;

  /// No description provided for @tomorrow.
  ///
  /// In ko, this message translates to:
  /// **'내일'**
  String get tomorrow;

  /// No description provided for @text10.
  ///
  /// In ko, this message translates to:
  /// **'{month}월 {day}일'**
  String text10(Object month, Object day);

  /// No description provided for @minAgo2.
  ///
  /// In ko, this message translates to:
  /// **'{passedInMinutes}분 전'**
  String minAgo2(Object passedInMinutes);

  /// No description provided for @text11.
  ///
  /// In ko, this message translates to:
  /// **'{passedInHours}시간 전'**
  String text11(num passedInHours);

  /// No description provided for @myDesktop.
  ///
  /// In ko, this message translates to:
  /// **'나의 데스크톱'**
  String get myDesktop;

  /// No description provided for @samplePcServerRelay.
  ///
  /// In ko, this message translates to:
  /// **'예시 PC · 서버 중계'**
  String get samplePcServerRelay;

  /// No description provided for @windows11PhysicalPc.
  ///
  /// In ko, this message translates to:
  /// **'Windows 11 · 물리 PC'**
  String get windows11PhysicalPc;

  /// No description provided for @proxmoxWorkspace.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 워크스페이스'**
  String get proxmoxWorkspace;

  /// No description provided for @lightweightVirtualDesktopStart.
  ///
  /// In ko, this message translates to:
  /// **'가볍게 시작하는 가상 데스크톱'**
  String get lightweightVirtualDesktopStart;

  /// No description provided for @devDesktop.
  ///
  /// In ko, this message translates to:
  /// **'개발용 데스크톱'**
  String get devDesktop;

  /// No description provided for @spaceNewIdeas.
  ///
  /// In ko, this message translates to:
  /// **'새로운 아이디어를 위한 공간'**
  String get spaceNewIdeas;

  /// No description provided for @multitapPrint.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 인쇄'**
  String get multitapPrint;

  /// No description provided for @printAgainFromDesktop.
  ///
  /// In ko, this message translates to:
  /// **'인쇄하던 문서 {count}개를 받지 못했어요. 데스크톱에서 다시 인쇄해 주세요.'**
  String printAgainFromDesktop(num count);

  /// No description provided for @couldntChangeScreenSizeSo.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기를 바꾸지 못해 이전 크기를 유지해요.'**
  String get couldntChangeScreenSizeSo;

  /// No description provided for @disconnected.
  ///
  /// In ko, this message translates to:
  /// **'연결 끊김'**
  String get disconnected;

  /// No description provided for @p2pDirect.
  ///
  /// In ko, this message translates to:
  /// **'P2P 직결'**
  String get p2pDirect;

  /// No description provided for @udpViaServer.
  ///
  /// In ko, this message translates to:
  /// **'UDP 서버 경유'**
  String get udpViaServer;

  /// No description provided for @tcpViaServer.
  ///
  /// In ko, this message translates to:
  /// **'TCP 서버 경유'**
  String get tcpViaServer;

  /// No description provided for @connectingServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결하는 중…'**
  String get connectingServer;

  /// No description provided for @waiting2.
  ///
  /// In ko, this message translates to:
  /// **'수신 대기'**
  String get waiting2;

  /// No description provided for @desktopHasNoSoundDevice.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 소리 장치가 없어요'**
  String get desktopHasNoSoundDevice;

  /// No description provided for @disconnectedReconnect.
  ///
  /// In ko, this message translates to:
  /// **'연결이 끊겼어요. 다시 연결하세요.'**
  String get disconnectedReconnect;

  /// No description provided for @couldntPrepareKeyboardInputSo.
  ///
  /// In ko, this message translates to:
  /// **'키보드 입력을 준비하지 못해 연결이 끊겼어요. 다시 연결하세요.'**
  String get couldntPrepareKeyboardInputSo;

  /// No description provided for @preparingVirtualDisplayResolution.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 해상도를 준비하는 중'**
  String get preparingVirtualDisplayResolution;

  /// No description provided for @waitingHostScreen.
  ///
  /// In ko, this message translates to:
  /// **'Host 화면 기다리는 중'**
  String get waitingHostScreen;

  /// No description provided for @virtualDisplayTookTooLong.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 준비 시간이 초과됐어요. 다시 연결해 주세요.'**
  String get virtualDisplayTookTooLong;

  /// No description provided for @couldntConfirmVirtualDisplayChange.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 변경을 확인하지 못했어요. 다시 연결해 주세요.'**
  String get couldntConfirmVirtualDisplayChange;

  /// No description provided for @preparingRemoteScreen.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면 준비 중'**
  String get preparingRemoteScreen;

  /// No description provided for @resizingVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 크기 변경 중'**
  String get resizingVirtualDisplay;

  /// No description provided for @restoringPreviousScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'이전 화면 크기 복구 중'**
  String get restoringPreviousScreenSize;

  /// No description provided for @couldntResizeVirtualDisplayReconnect.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 크기를 변경하지 못했어요. 다시 연결해 주세요.'**
  String get couldntResizeVirtualDisplayReconnect;

  /// No description provided for @couldntPrepareVirtualDisplayResolution.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 해상도를 준비하지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get couldntPrepareVirtualDisplayResolution;

  /// No description provided for @virtualDisplayOpenedDifferentResolution.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면이 다른 해상도로 열렸어요. 다시 연결하세요.'**
  String get virtualDisplayOpenedDifferentResolution;

  /// No description provided for @preparingScreenNewResolution.
  ///
  /// In ko, this message translates to:
  /// **'새 해상도의 화면을 준비하는 중'**
  String get preparingScreenNewResolution;

  /// No description provided for @preparingFirstFrame.
  ///
  /// In ko, this message translates to:
  /// **'첫 화면을 준비하는 중'**
  String get preparingFirstFrame;

  /// No description provided for @changingRemoteScreenResolution.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면 해상도를 변경하는 중'**
  String get changingRemoteScreenResolution;

  /// No description provided for @couldntResizeRemoteScreenSo.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면 크기를 바꾸지 못해 연결이 끊겼어요. 다시 연결해 주세요.'**
  String get couldntResizeRemoteScreenSo;

  /// No description provided for @hostStoppedRespondingSoConnection.
  ///
  /// In ko, this message translates to:
  /// **'Host가 응답하지 않아 연결이 끊겼어요. 다시 연결하세요.'**
  String get hostStoppedRespondingSoConnection;

  /// No description provided for @didntReceiveFirstFrameReconnect.
  ///
  /// In ko, this message translates to:
  /// **'첫 화면을 받지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get didntReceiveFirstFrameReconnect;

  /// No description provided for @screenStoppedArrivingSoConnection.
  ///
  /// In ko, this message translates to:
  /// **'화면을 더 받지 못해 연결이 끊겼어요. 다시 연결하세요.'**
  String get screenStoppedArrivingSoConnection;

  /// No description provided for @oneMonitorsScreenStoppedArriving.
  ///
  /// In ko, this message translates to:
  /// **'모니터 한 대의 화면을 더 받지 못해 연결이 끊겼어요. 다시 연결하세요.'**
  String get oneMonitorsScreenStoppedArriving;

  /// No description provided for @couldntShowScreenSoConnection.
  ///
  /// In ko, this message translates to:
  /// **'화면을 표시하지 못해 연결이 끊겼어요. 다시 연결하세요.'**
  String get couldntShowScreenSoConnection;

  /// No description provided for @couldntApplyRequestedSizeSo.
  ///
  /// In ko, this message translates to:
  /// **'요청한 크기를 적용하지 못해 이전 크기로 복구했어요.'**
  String get couldntApplyRequestedSizeSo;

  /// No description provided for @oneMonitorsDisplayFrozeSo.
  ///
  /// In ko, this message translates to:
  /// **'모니터 한 대의 화면 표시가 멈춰 연결이 끊겼어요. 다시 연결하세요.'**
  String get oneMonitorsDisplayFrozeSo;

  /// No description provided for @displayFrozeSoConnectionEnded.
  ///
  /// In ko, this message translates to:
  /// **'화면 표시가 멈춰 연결이 끊겼어요. 다시 연결하세요.'**
  String get displayFrozeSoConnectionEnded;

  /// No description provided for @soundOutputDeviceIsntAvailable.
  ///
  /// In ko, this message translates to:
  /// **'소리 출력 장치를 쓸 수 없어요'**
  String get soundOutputDeviceIsntAvailable;

  /// No description provided for @playing.
  ///
  /// In ko, this message translates to:
  /// **'재생 중'**
  String get playing;

  /// No description provided for @couldntCheckDisplayStateSo.
  ///
  /// In ko, this message translates to:
  /// **'화면 표시 상태를 확인하지 못해 연결이 끊겼어요. 다시 연결하세요.'**
  String get couldntCheckDisplayStateSo;

  /// No description provided for @couldntPrepareVideoNewResolution.
  ///
  /// In ko, this message translates to:
  /// **'새 해상도의 영상을 준비하지 못했어요. 다시 연결해 주세요.'**
  String get couldntPrepareVideoNewResolution;

  /// No description provided for @remoteResolutionTookTooLong.
  ///
  /// In ko, this message translates to:
  /// **'원격 해상도 적용 시간이 초과됐어요. 다시 연결해 주세요.'**
  String get remoteResolutionTookTooLong;

  /// No description provided for @largerResolutionsNeedHostServer.
  ///
  /// In ko, this message translates to:
  /// **'더 큰 해상도는 Host와 서버를 업데이트해야 쓸 수 있어요. 관리자에게 알려 주세요.'**
  String get largerResolutionsNeedHostServer;

  /// No description provided for @connectionEnded2.
  ///
  /// In ko, this message translates to:
  /// **'연결 종료'**
  String get connectionEnded2;

  /// No description provided for @networkConnectionDropped.
  ///
  /// In ko, this message translates to:
  /// **'네트워크 연결이 끊겼어요.'**
  String get networkConnectionDropped;

  /// No description provided for @couldntChangeAutoSignOut.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그아웃 설정을 변경하지 못했어요. 잠시 후 다시 시도하세요.'**
  String get couldntChangeAutoSignOut;

  /// No description provided for @couldntConfirmAutoSignOut.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그아웃 설정을 확인하지 못했어요. 현재 설정은 유지돼요.'**
  String get couldntConfirmAutoSignOut;

  /// No description provided for @hostCantHandleScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'Host가 이 화면 크기를 처리할 수 없어요.'**
  String get hostCantHandleScreenSize;

  /// No description provided for @couldntCheckHostsAvailableMemory.
  ///
  /// In ko, this message translates to:
  /// **'Host의 사용 가능한 메모리를 확인하지 못했어요.'**
  String get couldntCheckHostsAvailableMemory;

  /// No description provided for @hostDoesntHaveEnoughFree.
  ///
  /// In ko, this message translates to:
  /// **'Host의 사용 가능한 메모리가 화면 처리에 부족해요.'**
  String get hostDoesntHaveEnoughFree;

  /// No description provided for @hostDoesntSupportScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'Host가 이 화면 크기나 형식을 지원하지 않아요.'**
  String get hostDoesntSupportScreenSize;

  /// No description provided for @virtualDisplayDoesntSupportResolution.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면이 이 해상도·주사율을 지원하지 않아요.'**
  String get virtualDisplayDoesntSupportResolution;

  /// No description provided for @keepingPreviousSize.
  ///
  /// In ko, this message translates to:
  /// **'{message} 이전 크기를 유지해요.'**
  String keepingPreviousSize(Object message);

  /// No description provided for @reconnectMoment.
  ///
  /// In ko, this message translates to:
  /// **' 잠시 후 다시 연결해 주세요.'**
  String get reconnectMoment;

  /// No description provided for @makeScreenSmallerReconnect.
  ///
  /// In ko, this message translates to:
  /// **' 화면 크기를 줄여 다시 연결하세요.'**
  String get makeScreenSmallerReconnect;

  /// No description provided for @reconnectDifferentScreenSize.
  ///
  /// In ko, this message translates to:
  /// **' 다른 화면 크기로 다시 연결해 주세요.'**
  String get reconnectDifferentScreenSize;

  /// No description provided for @console2.
  ///
  /// In ko, this message translates to:
  /// **'Console에서'**
  String get console2;

  /// No description provided for @fromAnotherWindowDevice.
  ///
  /// In ko, this message translates to:
  /// **'다른 창이나 기기에서'**
  String get fromAnotherWindowDevice;

  /// No description provided for @sameAccount.
  ///
  /// In ko, this message translates to:
  /// **'같은 계정으로 {where}'**
  String sameAccount(Object where);

  /// No description provided for @account.
  ///
  /// In ko, this message translates to:
  /// **'{account} 계정이 {where}'**
  String account(Object account, Object where);

  /// No description provided for @anotherAccount.
  ///
  /// In ko, this message translates to:
  /// **'다른 계정이 {where}'**
  String anotherAccount(Object where);

  /// No description provided for @connectedDesktopSoConnectionEnded.
  ///
  /// In ko, this message translates to:
  /// **'{who} 이 데스크톱에 연결해 이 연결이 끊겼어요.\n다시 연결하면 그쪽 연결을 끊을 수 있어요.'**
  String connectedDesktopSoConnectionEnded(Object who);

  /// No description provided for @desktopWasConnectedFromAnother.
  ///
  /// In ko, this message translates to:
  /// **'다른 창이나 기기에서 이 데스크톱에 연결해 이 연결이 끊겼어요. 다시 연결하면 그쪽 연결을 끊을 수 있어요.'**
  String get desktopWasConnectedFromAnother;

  /// No description provided for @administratorChangedClipboardSharingPermission.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 클립보드 공유 권한을 변경했어요. 다시 연결하면 새 권한이 적용돼요.'**
  String get administratorChangedClipboardSharingPermission;

  /// No description provided for @disconnectedHostUpdateReconnectWhen.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 때문에 연결이 끊겼어요. 업데이트가 끝나면 다시 연결하세요.'**
  String get disconnectedHostUpdateReconnectWhen;

  /// No description provided for @signExpiredDesktopsAssignmentChanged.
  ///
  /// In ko, this message translates to:
  /// **'로그인이 만료됐거나 이 데스크톱의 배정이 바뀌었어요. 데스크톱 목록에서 확인하세요.'**
  String get signExpiredDesktopsAssignmentChanged;

  /// No description provided for @desktopsHostWasUnregisteredLet.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱의 Host 등록이 해제됐어요. 관리자에게 알려 주세요.'**
  String get desktopsHostWasUnregisteredLet;

  /// No description provided for @hostCouldntPrepareScreenReconnect.
  ///
  /// In ko, this message translates to:
  /// **'Host가 화면을 준비하지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get hostCouldntPrepareScreenReconnect;

  /// No description provided for @hostStoppedSendingScreenReconnect.
  ///
  /// In ko, this message translates to:
  /// **'Host가 화면을 보내다 멈췄어요. 다시 연결하세요.'**
  String get hostStoppedSendingScreenReconnect;

  /// No description provided for @hostConnectionDroppedReconnectMoment.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결이 끊겼어요. 잠시 후 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get hostConnectionDroppedReconnectMoment;

  /// No description provided for @serverRestartingReconnectMoment.
  ///
  /// In ko, this message translates to:
  /// **'서버가 다시 시작되고 있어요. 잠시 후 다시 연결하세요.'**
  String get serverRestartingReconnectMoment;

  /// No description provided for @screenCouldntProcessedTimeSo.
  ///
  /// In ko, this message translates to:
  /// **'화면을 제때 처리하지 못해 연결이 끊겼어요. 화질 설정을 낮추거나 다시 연결하세요.'**
  String get screenCouldntProcessedTimeSo;

  /// No description provided for @virtualDisplaySetupChangedScreen.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 구성이 바뀌었거나 화면 크기를 바꾸지 못해 연결이 끊겼어요. 다시 연결하세요.'**
  String get virtualDisplaySetupChangedScreen;

  /// No description provided for @couldntConnectAllMonitorsFrom.
  ///
  /// In ko, this message translates to:
  /// **'모든 모니터로 연결하지 못했어요.\n목록에서 창 모드나 전체 화면으로 연결하세요.'**
  String get couldntConnectAllMonitorsFrom;

  /// No description provided for @couldntConnectServerCheckNetwork2.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결하지 못했어요. 네트워크를 확인하고 다시 연결하세요.'**
  String get couldntConnectServerCheckNetwork2;

  /// No description provided for @couldntConnectDesktopReconnectIf.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 연결하지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.'**
  String get couldntConnectDesktopReconnectIf;

  /// No description provided for @notEnoughMemoryPrepareScreen.
  ///
  /// In ko, this message translates to:
  /// **'메모리가 부족해 이 화면 크기를 준비하지 못했어요.'**
  String get notEnoughMemoryPrepareScreen;

  /// No description provided for @twoScreensCantShownOnce.
  ///
  /// In ko, this message translates to:
  /// **'여기서는 화면 두 개를 동시에 표시할 수 없어요.'**
  String get twoScreensCantShownOnce;

  /// No description provided for @screenSizeFrameRateCant.
  ///
  /// In ko, this message translates to:
  /// **'여기서는 이 화면 크기와 프레임 속도를 표시할 수 없어요.'**
  String get screenSizeFrameRateCant;

  /// No description provided for @videoFormatCantShownHere.
  ///
  /// In ko, this message translates to:
  /// **'여기서는 이 영상 형식을 표시할 수 없어요.'**
  String get videoFormatCantShownHere;

  /// No description provided for @screenSizeCantShownHere.
  ///
  /// In ko, this message translates to:
  /// **'여기서는 이 화면 크기를 표시할 수 없어요.'**
  String get screenSizeCantShownHere;

  /// No description provided for @couldntCheckWhichScreenSizes.
  ///
  /// In ko, this message translates to:
  /// **'표시할 수 있는 화면 크기를 확인하지 못했어요. 이전 크기를 유지해요.'**
  String get couldntCheckWhichScreenSizes;

  /// No description provided for @pcDecoderSizeLimit.
  ///
  /// In ko, this message translates to:
  /// **'PC 디코더 크기 한도'**
  String get pcDecoderSizeLimit;

  /// No description provided for @pcDecoderSpeedLimit.
  ///
  /// In ko, this message translates to:
  /// **'PC 디코더 속도 한도'**
  String get pcDecoderSpeedLimit;

  /// No description provided for @pcDecodingLoad.
  ///
  /// In ko, this message translates to:
  /// **'PC 디코딩 부하'**
  String get pcDecodingLoad;

  /// No description provided for @desktopEncodingLoad.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 인코딩 부하'**
  String get desktopEncodingLoad;

  /// No description provided for @limitSetByAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'관리자 설정 한도'**
  String get limitSetByAdministrator;

  /// No description provided for @desktopConnectedSomewhereElseInformation.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳에서 연결 중이에요. 그 연결을 끊는 데 필요한 정보를 받지 못했어요. 다시 연결해 보고, 계속되면 관리자에게 알려 주세요.'**
  String get desktopConnectedSomewhereElseInformation;

  /// No description provided for @connectedAccount.
  ///
  /// In ko, this message translates to:
  /// **'연결 중인 계정 {username}'**
  String connectedAccount(Object username);

  /// No description provided for @connectedAccountSinceDate.
  ///
  /// In ko, this message translates to:
  /// **'연결 중인 계정 {username} · {date} {time}부터'**
  String connectedAccountSinceDate(Object username, Object date, Object time);

  /// No description provided for @useAnotherWindow.
  ///
  /// In ko, this message translates to:
  /// **'다른 창에서 쓰는 중이에요'**
  String get useAnotherWindow;

  /// No description provided for @useWindow.
  ///
  /// In ko, this message translates to:
  /// **'{name} 창에서 쓰는 중이에요'**
  String useWindow(Object name);

  /// No description provided for @deviceAlsoNeedsPermissionIts.
  ///
  /// In ko, this message translates to:
  /// **'이 장치를 쓰려면 장치 종류에 맞는 권한(공유 폴더·프린터·스마트 카드·직렬(COM) 포트)도 필요해요. 관리자에게 요청하세요.'**
  String get deviceAlsoNeedsPermissionIts;

  /// No description provided for @devicesSuchAsKeyboardsMice.
  ///
  /// In ko, this message translates to:
  /// **'키보드·마우스·허브·블루투스·오디오·카메라 같은 장치는 연결할 수 없어요.'**
  String get devicesSuchAsKeyboardsMice;

  /// No description provided for @keyboardsMiceArentConnectedDesktop.
  ///
  /// In ko, this message translates to:
  /// **'키보드·마우스는 데스크톱에 연결하지 않아요.'**
  String get keyboardsMiceArentConnectedDesktop;

  /// No description provided for @couldntReadDeviceInformationSo.
  ///
  /// In ko, this message translates to:
  /// **'장치 정보를 읽지 못해 연결할 수 없어요.'**
  String get couldntReadDeviceInformationSo;

  /// No description provided for @youCanConnectUpDevices.
  ///
  /// In ko, this message translates to:
  /// **'장치는 한 번에 {usbMaxDevices}개까지 연결할 수 있어요.'**
  String youCanConnectUpDevices(Object usbMaxDevices);

  /// No description provided for @desktopDidntReceiveDevice.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 장치를 받지 못했어요.'**
  String get desktopDidntReceiveDevice;

  /// No description provided for @anotherUsbVirtualDeviceProgram.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 다른 USB 가상 장치 프로그램이 있어요.\n데스크톱의 Host 설정 → 장치 → USB에서 해결할 수 있어요.'**
  String get anotherUsbVirtualDeviceProgram;

  /// No description provided for @desktopsHostHasNoUsb.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱의 Host에 USB 연결 드라이버가 없어요. 관리자에게 알려 주세요.'**
  String get desktopsHostHasNoUsb;

  /// No description provided for @couldntInstallUsbConnectionDriver.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 USB 연결 드라이버를 설치하지 못했어요. 관리자에게 알려 주세요.'**
  String get couldntInstallUsbConnectionDriver;

  /// No description provided for @restartDesktopUseUsbDevices.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱을 다시 시작해야 USB 장치를 쓸 수 있어요.'**
  String get restartDesktopUseUsbDevices;

  /// No description provided for @desktopsUsbConnectionDriverIsnt.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱의 USB 연결 드라이버가 보호되지 않아 USB 장치를 연결하지 않아요. 관리자에게 알려 주세요.'**
  String get desktopsUsbConnectionDriverIsnt;

  /// No description provided for @administratorTurnedOffUsbPermission.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 USB 권한을 해제했어요.'**
  String get administratorTurnedOffUsbPermission;

  /// No description provided for @deviceWasUnpluggedFromPc.
  ///
  /// In ko, this message translates to:
  /// **'장치가 PC에서 분리됐어요.'**
  String get deviceWasUnpluggedFromPc;

  /// No description provided for @deviceWasRemovedDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 장치를 분리했어요.'**
  String get deviceWasRemovedDesktop;

  /// No description provided for @anotherProgramUsingDevice.
  ///
  /// In ko, this message translates to:
  /// **'다른 프로그램이 이 장치를 쓰고 있어요.'**
  String get anotherProgramUsingDevice;

  /// No description provided for @itsUsePcSoCant.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 사용 중이라 연결할 수 없어요. 열린 파일이나 창을 닫고 다시 시도하세요.'**
  String get itsUsePcSoCant;

  /// No description provided for @useSomewhereElse.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳에서 쓰는 중이에요.'**
  String get useSomewhereElse;

  /// No description provided for @couldntTakeDeviceFromPc.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 장치를 가져오지 못했어요.'**
  String get couldntTakeDeviceFromPc;

  /// No description provided for @anotherWindowProgramAlreadyUsing.
  ///
  /// In ko, this message translates to:
  /// **'다른 창이나 프로그램이 이 장치를 이미 쓰고 있어요. 그쪽에서 연결을 끊고 다시 시도하세요.'**
  String get anotherWindowProgramAlreadyUsing;

  /// No description provided for @couldntGetDeviceBackFrom.
  ///
  /// In ko, this message translates to:
  /// **'다른 창에서 장치를 돌려받지 못했어요. 그 창에서 연결을 끊고 다시 시도하세요.'**
  String get couldntGetDeviceBackFrom;

  /// No description provided for @couldntCommunicateDeviceTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'장치와 통신하지 못했어요. 다시 시도하세요.'**
  String get couldntCommunicateDeviceTryAgain;

  /// No description provided for @takingDeviceFromPcTook.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 장치를 가져오는 데 너무 오래 걸렸어요. 다시 시도하세요.'**
  String get takingDeviceFromPcTook;

  /// No description provided for @openFileCheckDidntFinish.
  ///
  /// In ko, this message translates to:
  /// **'열린 파일 확인이 끝나지 않았어요. 잠시 후 다시 시도하세요.'**
  String get openFileCheckDidntFinish;

  /// No description provided for @couldntCheckWhetherPcCan.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 장치를 쓸 수 있는지 확인하지 못했어요. ‘PC로 되돌리기’를 눌러 주세요.'**
  String get couldntCheckWhetherPcCan;

  /// No description provided for @deviceDidntReturnPcSelect.
  ///
  /// In ko, this message translates to:
  /// **'장치가 PC로 돌아오지 않았어요. ‘PC로 되돌리기’를 눌러 주세요. 안 되면 USB를 다시 꽂아 주세요.'**
  String get deviceDidntReturnPcSelect;

  /// No description provided for @noMoreDevicesCanConnected.
  ///
  /// In ko, this message translates to:
  /// **'이 연결에서는 장치를 더 연결할 수 없어요. 데스크톱에 다시 연결하세요.'**
  String get noMoreDevicesCanConnected;

  /// No description provided for @couldntFindUsbDriverIf.
  ///
  /// In ko, this message translates to:
  /// **'USB 드라이버를 찾지 못했어요. PC를 다시 시작해도 같으면 클라이언트를 다시 설치해 주세요.'**
  String get couldntFindUsbDriverIf;

  /// No description provided for @couldntVerifyInstallersSignatureSo.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일의 서명을 확인하지 못해 설치하지 않았어요.'**
  String get couldntVerifyInstallersSignatureSo;

  /// No description provided for @couldntInstallUsbDriverTry.
  ///
  /// In ko, this message translates to:
  /// **'USB 드라이버를 설치하지 못했어요. 다시 시도하세요.'**
  String get couldntInstallUsbDriverTry;

  /// No description provided for @windowsAdministratorApprovalWasCanceled.
  ///
  /// In ko, this message translates to:
  /// **'Windows 관리자 승인을 취소해서 USB 드라이버를 설치하지 않았어요. 설치하려면 ‘USB 드라이버 설치…’를 다시 누르고 ‘예’를 누르세요.'**
  String get windowsAdministratorApprovalWasCanceled;

  /// No description provided for @restartPcUseUsbDevices.
  ///
  /// In ko, this message translates to:
  /// **'PC를 다시 시작하면 USB 장치를 쓸 수 있어요.'**
  String get restartPcUseUsbDevices;

  /// No description provided for @couldntConnectDevice.
  ///
  /// In ko, this message translates to:
  /// **'장치를 연결하지 못했어요.'**
  String get couldntConnectDevice;

  /// No description provided for @usbReturnedChannelEnded.
  ///
  /// In ko, this message translates to:
  /// **'연결이 끊겨 장치가 PC로 돌아왔어요.'**
  String get usbReturnedChannelEnded;

  /// No description provided for @smartCard.
  ///
  /// In ko, this message translates to:
  /// **'스마트 카드'**
  String get smartCard;

  /// No description provided for @serialComPort.
  ///
  /// In ko, this message translates to:
  /// **'직렬(COM) 포트'**
  String get serialComPort;

  /// No description provided for @deviceAlsoNeedsPermissionAsk.
  ///
  /// In ko, this message translates to:
  /// **'이 장치를 쓰려면 {value} 권한도 필요해요. 관리자에게 요청하세요.'**
  String deviceAlsoNeedsPermissionAsk(Object value);

  /// No description provided for @usbDeviceReconnected.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치를 다시 연결했어요'**
  String get usbDeviceReconnected;

  /// No description provided for @couldntReconnectUsbDevice.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치를 다시 연결하지 못했어요. {failure}'**
  String couldntReconnectUsbDevice(Object failure);

  /// No description provided for @usbConnectionLost.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결이 끊겼어요'**
  String get usbConnectionLost;

  /// No description provided for @multitapHostSettings.
  ///
  /// In ko, this message translates to:
  /// **'Multitap Host 설정'**
  String get multitapHostSettings;

  /// No description provided for @couldntPrepareRemoteWindow.
  ///
  /// In ko, this message translates to:
  /// **'원격 창을 준비하지 못했어요'**
  String get couldntPrepareRemoteWindow;

  /// No description provided for @multitapRemoteWindow.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 원격 창'**
  String get multitapRemoteWindow;

  /// No description provided for @closeWindowConnectAgainFrom.
  ///
  /// In ko, this message translates to:
  /// **'창을 닫고 데스크톱 목록에서 다시 연결하세요.'**
  String get closeWindowConnectAgainFrom;

  /// No description provided for @windowsLockedUnlockSameWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 잠겨 있어요. 같은 Windows 계정으로 잠금을 해제한 뒤 연결하세요.'**
  String get windowsLockedUnlockSameWindows;

  /// No description provided for @theresNoWindowsSignSession.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍할 Windows 로그인 세션이 없어요. Windows에 로그인하세요.'**
  String get theresNoWindowsSignSession;

  /// No description provided for @youCanBringWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'RDP에서 쓰던 Windows 화면을 스트리밍으로 가져올 수 있어요.'**
  String get youCanBringWindowsScreen;

  /// No description provided for @checkHostSettingsWindowsSign.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정 → Windows 로그인 → Windows 계정을 확인하세요.'**
  String get checkHostSettingsWindowsSign;

  /// No description provided for @anotherWindowsUserSeveralSign.
  ///
  /// In ko, this message translates to:
  /// **'다른 Windows 사용자나 여러 로그인이 있어 세션을 자동으로 선택할 수 없어요.'**
  String get anotherWindowsUserSeveralSign;

  /// No description provided for @windowsSignStateChangedDuring.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인 상태가 작업 중 변경됐어요. 목록을 새로고침하세요.'**
  String get windowsSignStateChangedDuring;

  /// No description provided for @sessionSwitchResultIsntConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'세션 전환 결과를 아직 확인하지 못했어요. 같은 작업을 반복하지 말고 상태를 새로고침하세요.'**
  String get sessionSwitchResultIsntConfirmed;

  /// No description provided for @couldntReturnWindowsSessionStreaming.
  ///
  /// In ko, this message translates to:
  /// **'Windows 세션을 스트리밍 화면으로 복귀하지 못했어요. Host의 세션 상태를 확인하세요.'**
  String get couldntReturnWindowsSessionStreaming;

  /// No description provided for @windowsSessionVirtualDisplayBeing.
  ///
  /// In ko, this message translates to:
  /// **'Windows 세션이나 가상 화면을 구성 중이에요. 완료 후 연결하세요.'**
  String get windowsSessionVirtualDisplayBeing;

  /// No description provided for @windowsSessionBackButVirtual.
  ///
  /// In ko, this message translates to:
  /// **'Windows 세션은 복귀했지만 가상 화면 준비가 끝나지 않았어요. Host의 가상 화면 상태를 확인하세요.'**
  String get windowsSessionBackButVirtual;

  /// No description provided for @returnWindowsSessionConfirmSwitch.
  ///
  /// In ko, this message translates to:
  /// **'Windows 세션을 복귀하려면 전환 안내에서 확인하세요.'**
  String get returnWindowsSessionConfirmSwitch;

  /// No description provided for @hostCouldntConfirmScreenWas.
  ///
  /// In ko, this message translates to:
  /// **'Host가 이전 연결의 화면 복원을 확인하지 못했어요.'**
  String get hostCouldntConfirmScreenWas;

  /// No description provided for @hostReportedScreenWasRestored.
  ///
  /// In ko, this message translates to:
  /// **'Host가 화면 복원을 보고했지만 새 화면 상태는 아직 확인되지 않았어요.'**
  String get hostReportedScreenWasRestored;

  /// No description provided for @hostCouldntConfirmTimePrevious.
  ///
  /// In ko, this message translates to:
  /// **'Host가 이전 연결의 종료와 화면 복원을 제한 시간 안에 확인하지 못했어요.'**
  String get hostCouldntConfirmTimePrevious;

  /// No description provided for @hostReportRestoreUnconfirmed.
  ///
  /// In ko, this message translates to:
  /// **'Host 보고 · 복원 미확인'**
  String get hostReportRestoreUnconfirmed;

  /// No description provided for @restoreReportedWaitingScreenCheck.
  ///
  /// In ko, this message translates to:
  /// **'복원 보고 · 화면 상태 확인 대기'**
  String get restoreReportedWaitingScreenCheck;

  /// No description provided for @hostCheckTimedOut.
  ///
  /// In ko, this message translates to:
  /// **'Host 확인 시간 초과'**
  String get hostCheckTimedOut;

  /// No description provided for @endCheckDelayed.
  ///
  /// In ko, this message translates to:
  /// **'종료 확인 지연'**
  String get endCheckDelayed;

  /// No description provided for @endPreviousStreamingIsntConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'이전 스트리밍 종료도 확인되지 않았어요. 새로고침해 다시 확인하고, 계속되면 관리자에게 알려 주세요.'**
  String get endPreviousStreamingIsntConfirmed;

  /// No description provided for @refreshCheckAgainIfKeeps.
  ///
  /// In ko, this message translates to:
  /// **'새로고침해 다시 확인하고, 계속되면 관리자에게 알려 주세요.'**
  String get refreshCheckAgainIfKeeps;

  /// No description provided for @checkingPreviousConnectionEndedScreen.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결의 종료와 화면 복원을 확인하고 있어요.'**
  String get checkingPreviousConnectionEndedScreen;

  /// No description provided for @connectingBlockedBecauseScreenRestore.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결의 화면 복원이 확인되지 않아 연결을 막아 뒀어요. 새로고침하고, 계속되면 관리자에게 알려 주세요.'**
  String get connectingBlockedBecauseScreenRestore;

  /// No description provided for @settingUpVirtualDisplayYou.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 구성하고 있어요. 끝나면 연결할 수 있어요.'**
  String get settingUpVirtualDisplayYou;

  /// No description provided for @cantStartConnectionRightNow.
  ///
  /// In ko, this message translates to:
  /// **'지금은 연결을 시작할 수 없어요. 잠시 후 다시 시도하세요.'**
  String get cantStartConnectionRightNow;

  /// No description provided for @desktopOff.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 꺼져 있어요.'**
  String get desktopOff;

  /// No description provided for @desktopIsntResponding.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 응답하지 않아요.'**
  String get desktopIsntResponding;

  /// No description provided for @virtualDisplayIsntReadyTell.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면이 준비되지 않았어요. 관리자에게 알려 주세요.'**
  String get virtualDisplayIsntReadyTell;

  /// No description provided for @checkingWhetherStreamingReady.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 준비 상태를 확인하고 있어요.'**
  String get checkingWhetherStreamingReady;

  /// No description provided for @waitingHostConnect.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결을 기다리고 있어요.'**
  String get waitingHostConnect;

  /// No description provided for @switchingPower.
  ///
  /// In ko, this message translates to:
  /// **'전원 전환'**
  String get switchingPower;

  /// No description provided for @restoreUnconfirmed.
  ///
  /// In ko, this message translates to:
  /// **'복원 미확인'**
  String get restoreUnconfirmed;

  /// No description provided for @restoringScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 복원 중'**
  String get restoringScreen;

  /// No description provided for @settingUpScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 구성 중'**
  String get settingUpScreen;

  /// No description provided for @poweredOff.
  ///
  /// In ko, this message translates to:
  /// **'전원 꺼짐'**
  String get poweredOff;

  /// No description provided for @offline.
  ///
  /// In ko, this message translates to:
  /// **'오프라인'**
  String get offline;

  /// No description provided for @screenSetup.
  ///
  /// In ko, this message translates to:
  /// **'화면 준비'**
  String get screenSetup;

  /// No description provided for @number.
  ///
  /// In ko, this message translates to:
  /// **'번호순'**
  String get number;

  /// No description provided for @name.
  ///
  /// In ko, this message translates to:
  /// **'이름순'**
  String get name;

  /// No description provided for @recent.
  ///
  /// In ko, this message translates to:
  /// **'최근 사용순'**
  String get recent;

  /// No description provided for @runningFirst.
  ///
  /// In ko, this message translates to:
  /// **'실행 중 먼저'**
  String get runningFirst;

  /// No description provided for @all.
  ///
  /// In ko, this message translates to:
  /// **'전체'**
  String get all;

  /// No description provided for @favorites.
  ///
  /// In ko, this message translates to:
  /// **'즐겨찾기'**
  String get favorites;

  /// No description provided for @showAllDesktops.
  ///
  /// In ko, this message translates to:
  /// **'모든 데스크톱 보기'**
  String get showAllDesktops;

  /// No description provided for @showRunningDesktopsOnly.
  ///
  /// In ko, this message translates to:
  /// **'실행 중인 데스크톱만 보기'**
  String get showRunningDesktopsOnly;

  /// No description provided for @showOffDesktopsOnly.
  ///
  /// In ko, this message translates to:
  /// **'꺼진 데스크톱만 보기'**
  String get showOffDesktopsOnly;

  /// No description provided for @noDesktopsAssigned.
  ///
  /// In ko, this message translates to:
  /// **'배정된 데스크톱이 없어요'**
  String get noDesktopsAssigned;

  /// No description provided for @noRunningDesktops.
  ///
  /// In ko, this message translates to:
  /// **'실행 중인 데스크톱이 없어요'**
  String get noRunningDesktops;

  /// No description provided for @noDesktopsOff.
  ///
  /// In ko, this message translates to:
  /// **'꺼진 데스크톱이 없어요'**
  String get noDesktopsOff;

  /// No description provided for @showVmsOnly.
  ///
  /// In ko, this message translates to:
  /// **'VM만 보기'**
  String get showVmsOnly;

  /// No description provided for @showLocalPcsOnly.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC만 보기'**
  String get showLocalPcsOnly;

  /// No description provided for @noVmsAssigned.
  ///
  /// In ko, this message translates to:
  /// **'배정된 VM이 없어요'**
  String get noVmsAssigned;

  /// No description provided for @noLocalPcsAssigned.
  ///
  /// In ko, this message translates to:
  /// **'배정된 로컬 PC가 없어요'**
  String get noLocalPcsAssigned;

  /// No description provided for @noDesktopsMatch.
  ///
  /// In ko, this message translates to:
  /// **'조건에 맞는 데스크톱이 없어요'**
  String get noDesktopsMatch;

  /// No description provided for @noDesktopsMatch2.
  ///
  /// In ko, this message translates to:
  /// **'\'{short}\'에 맞는 데스크톱이 없어요'**
  String noDesktopsMatch2(Object short);

  /// No description provided for @searchByNameNumberGroup.
  ///
  /// In ko, this message translates to:
  /// **'이름, 번호, 분류로 찾을 수 있어요.'**
  String get searchByNameNumberGroup;

  /// No description provided for @searchedWithin.
  ///
  /// In ko, this message translates to:
  /// **'{parts} 조건 안에서 찾았어요.'**
  String searchedWithin(Object parts);

  /// No description provided for @connectedAccount2.
  ///
  /// In ko, this message translates to:
  /// **'내 계정으로 연결돼 있어요'**
  String get connectedAccount2;

  /// No description provided for @anotherUserUsing.
  ///
  /// In ko, this message translates to:
  /// **'다른 사용자가 사용 중이에요'**
  String get anotherUserUsing;

  /// No description provided for @accountUsingIt.
  ///
  /// In ko, this message translates to:
  /// **'{account} 님이 사용 중이에요'**
  String accountUsingIt(Object account);

  /// No description provided for @inUseByAccount.
  ///
  /// In ko, this message translates to:
  /// **'{account} 사용 중'**
  String inUseByAccount(Object account);

  /// No description provided for @accountUsingItWhenYou.
  ///
  /// In ko, this message translates to:
  /// **'{account} 님이 사용 중이에요. 연결하면 그 연결을 끊을지 먼저 물어봐요.'**
  String accountUsingItWhenYou(Object account);

  /// No description provided for @connectionAlreadyOpen.
  ///
  /// In ko, this message translates to:
  /// **'이미 열린 연결이 있어요'**
  String get connectionAlreadyOpen;

  /// No description provided for @connection.
  ///
  /// In ko, this message translates to:
  /// **'내 연결'**
  String get connection;

  /// No description provided for @useByAnotherUser.
  ///
  /// In ko, this message translates to:
  /// **'다른 사용자 사용 중'**
  String get useByAnotherUser;

  /// No description provided for @use.
  ///
  /// In ko, this message translates to:
  /// **'사용 중'**
  String get use;

  /// No description provided for @connectedAccountIfItsConnection.
  ///
  /// In ko, this message translates to:
  /// **'내 계정으로 연결돼 있어요. 다른 기기에서 연 연결이면 연결할 때 이쪽으로 가져올 수 있어요.'**
  String get connectedAccountIfItsConnection;

  /// No description provided for @anotherUserUsingWhenYou.
  ///
  /// In ko, this message translates to:
  /// **'다른 사용자가 사용 중이에요. 연결하면 그 연결을 끊을지 먼저 물어봐요.'**
  String get anotherUserUsingWhenYou;

  /// No description provided for @desktopAlreadyHasOpenConnection.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에 이미 열린 연결이 있어요.'**
  String get desktopAlreadyHasOpenConnection;

  /// No description provided for @hostUpdate2.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 · {version} → {latestVersion}'**
  String hostUpdate2(Object version, Object latestVersion);

  /// No description provided for @hostUpdateHostOffline.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 · Host 오프라인'**
  String get hostUpdateHostOffline;

  /// No description provided for @hostUpdateCheckingVersion.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 · 버전 확인 중'**
  String get hostUpdateCheckingVersion;

  /// No description provided for @hostRemoteUpdateNotSupported.
  ///
  /// In ko, this message translates to:
  /// **'Host {version} · 원격 업데이트 미지원'**
  String hostRemoteUpdateNotSupported(Object version);

  /// No description provided for @hostLatestVersion.
  ///
  /// In ko, this message translates to:
  /// **'Host {version} · 최신 버전'**
  String hostLatestVersion(Object version);

  /// No description provided for @hostCheckingUpdates.
  ///
  /// In ko, this message translates to:
  /// **'Host {version} · 업데이트 확인 중'**
  String hostCheckingUpdates(Object version);

  /// No description provided for @checkingHostVersion.
  ///
  /// In ko, this message translates to:
  /// **'Host 버전 확인 중'**
  String get checkingHostVersion;

  /// No description provided for @hostOfflineVersionUnknown.
  ///
  /// In ko, this message translates to:
  /// **'Host 오프라인 · 버전 확인 안 됨'**
  String get hostOfflineVersionUnknown;

  /// No description provided for @updating.
  ///
  /// In ko, this message translates to:
  /// **'{host} · 업데이트 중'**
  String updating(Object host);

  /// No description provided for @offline2.
  ///
  /// In ko, this message translates to:
  /// **'{host} · 오프라인'**
  String offline2(Object host);

  /// No description provided for @upDate2.
  ///
  /// In ko, this message translates to:
  /// **'{host} · 최신'**
  String upDate2(Object host);

  /// No description provided for @remoteUpdateNotSupportedInstall2.
  ///
  /// In ko, this message translates to:
  /// **'{host} · 원격 업데이트 미지원 · 직접 설치 필요'**
  String remoteUpdateNotSupportedInstall2(Object host);

  /// No description provided for @newVersionAvailable.
  ///
  /// In ko, this message translates to:
  /// **'{host} · 새 버전 {latestVersion} 설치 가능'**
  String newVersionAvailable(Object host, Object latestVersion);

  /// No description provided for @youCanReconnectWhenIts.
  ///
  /// In ko, this message translates to:
  /// **'끝나면 다시 연결할 수 있어요.'**
  String get youCanReconnectWhenIts;

  /// No description provided for @downloading.
  ///
  /// In ko, this message translates to:
  /// **'다운로드 중이에요{op}. {after}'**
  String downloading(Object op, Object after);

  /// No description provided for @checkingFile.
  ///
  /// In ko, this message translates to:
  /// **'파일을 확인하고 있어요. {after}'**
  String checkingFile(Object after);

  /// No description provided for @installing.
  ///
  /// In ko, this message translates to:
  /// **'설치 중이에요. {after}'**
  String installing(Object after);

  /// No description provided for @updateStarted.
  ///
  /// In ko, this message translates to:
  /// **'업데이트를 시작했어요. {after}'**
  String updateStarted(Object after);

  /// No description provided for @hostUpdateWaitingStartsWhen.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 대기 · 연결이 끝나면 시작해요'**
  String get hostUpdateWaitingStartsWhen;

  /// No description provided for @updatingHostDownloading.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 중 · 다운로드{op}'**
  String updatingHostDownloading(Object op);

  /// No description provided for @updatingHostCheckingFile.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 중 · 파일 확인'**
  String get updatingHostCheckingFile;

  /// No description provided for @updatingHostInstalling.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 중 · 설치'**
  String get updatingHostInstalling;

  /// No description provided for @hostUpdatedWindowsRestartNeeded.
  ///
  /// In ko, this message translates to:
  /// **'Host {targetVersion} 업데이트 완료 · Windows 다시 시작 필요'**
  String hostUpdatedWindowsRestartNeeded(Object targetVersion);

  /// No description provided for @hostUpdatedCheckHostSettings.
  ///
  /// In ko, this message translates to:
  /// **'Host {targetVersion} 업데이트 완료 · Host 설정 확인 필요'**
  String hostUpdatedCheckHostSettings(Object targetVersion);

  /// No description provided for @hostUpdated.
  ///
  /// In ko, this message translates to:
  /// **'Host {targetVersion} 업데이트 완료'**
  String hostUpdated(Object targetVersion);

  /// No description provided for @checkHostUpdateResult.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 결과 확인 필요 · {code}'**
  String checkHostUpdateResult(Object code);

  /// No description provided for @hostUpdateWaitExpired.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 대기 만료'**
  String get hostUpdateWaitExpired;

  /// No description provided for @hostUpdateFailed.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 실패 · {code}'**
  String hostUpdateFailed(Object code);

  /// No description provided for @hostDidntReconnectAfterUpdate.
  ///
  /// In ko, this message translates to:
  /// **'Host가 업데이트 후 다시 연결되지 않았어요'**
  String get hostDidntReconnectAfterUpdate;

  /// No description provided for @checkConnectionStatusHostDiagnostics.
  ///
  /// In ko, this message translates to:
  /// **'Host 진단과 서버 활동 기록에서 연결 상태를 확인하세요.'**
  String get checkConnectionStatusHostDiagnostics;

  /// No description provided for @checkServiceStatusLocalPcs.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC의 Host 설정 → 진단에서 서비스 상태를 확인하세요.'**
  String get checkServiceStatusLocalPcs;

  /// No description provided for @openDesktopConsoleCheckHost.
  ///
  /// In ko, this message translates to:
  /// **'Console로 데스크톱을 열고 Host 설정 → 진단을 확인하세요.'**
  String get openDesktopConsoleCheckHost;

  /// No description provided for @couldntConfirmUpdateResultConnection.
  ///
  /// In ko, this message translates to:
  /// **'{host} 업데이트 결과와 연결을 확인하지 못했어요.'**
  String couldntConfirmUpdateResultConnection(Object host);

  /// No description provided for @couldntGetResponseFromAfter.
  ///
  /// In ko, this message translates to:
  /// **'{host} 업데이트 실패 후 응답을 확인하지 못했어요.'**
  String couldntGetResponseFromAfter(Object host);

  /// No description provided for @couldntGetResponseFromAfter2.
  ///
  /// In ko, this message translates to:
  /// **'{host} 업데이트 완료 보고 뒤 응답을 확인하지 못했어요.'**
  String couldntGetResponseFromAfter2(Object host);

  /// No description provided for @connectionUse.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 연결이 있어요'**
  String get connectionUse;

  /// No description provided for @anotherUpdateProgress.
  ///
  /// In ko, this message translates to:
  /// **'다른 업데이트가 진행 중이에요'**
  String get anotherUpdateProgress;

  /// No description provided for @notNewerVersion.
  ///
  /// In ko, this message translates to:
  /// **'새 버전이 아니에요'**
  String get notNewerVersion;

  /// No description provided for @couldntDownloadInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일을 받지 못했어요'**
  String get couldntDownloadInstaller;

  /// No description provided for @fileSizeDoesntMatchRelease.
  ///
  /// In ko, this message translates to:
  /// **'파일 크기가 배포 정보와 달라요'**
  String get fileSizeDoesntMatchRelease;

  /// No description provided for @filesSha256DoesntMatch.
  ///
  /// In ko, this message translates to:
  /// **'파일의 SHA-256이 배포 정보와 달라요'**
  String get filesSha256DoesntMatch;

  /// No description provided for @couldntVerifyReleaseSignature.
  ///
  /// In ko, this message translates to:
  /// **'배포 서명을 확인하지 못했어요'**
  String get couldntVerifyReleaseSignature;

  /// No description provided for @downloadedFileIsntInstaller.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일이 설치 프로그램이 아니에요'**
  String get downloadedFileIsntInstaller;

  /// No description provided for @couldntStartInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램을 시작하지 못했어요'**
  String get couldntStartInstaller;

  /// No description provided for @mustInstalledDesktopDirectly.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 직접 설치해야 해요'**
  String get mustInstalledDesktopDirectly;

  /// No description provided for @noExistingHost.
  ///
  /// In ko, this message translates to:
  /// **'기존 Host가 없어요'**
  String get noExistingHost;

  /// No description provided for @didntReceiveInstallResult.
  ///
  /// In ko, this message translates to:
  /// **'설치 결과를 받지 못했어요'**
  String get didntReceiveInstallResult;

  /// No description provided for @couldntFinishInstalling.
  ///
  /// In ko, this message translates to:
  /// **'설치를 완료하지 못했어요'**
  String get couldntFinishInstalling;

  /// No description provided for @noResponseFromHost.
  ///
  /// In ko, this message translates to:
  /// **'Host 응답이 없어요'**
  String get noResponseFromHost;

  /// No description provided for @hostConnectionLost.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결이 끊겼어요'**
  String get hostConnectionLost;

  /// No description provided for @noMoreDetails.
  ///
  /// In ko, this message translates to:
  /// **'추가 정보가 없어요'**
  String get noMoreDetails;

  /// No description provided for @serverHasntReceivedLocalPcs.
  ///
  /// In ko, this message translates to:
  /// **'서버가 이 로컬 PC의 랜카드 정보를 아직 받지 못했어요.'**
  String get serverHasntReceivedLocalPcs;

  /// No description provided for @wakeSignalWasSentBut.
  ///
  /// In ko, this message translates to:
  /// **'켜기 신호를 보냈지만 4분 안에 켜지지 않았어요.'**
  String get wakeSignalWasSentBut;

  /// No description provided for @serverCouldntSendWakeSignal.
  ///
  /// In ko, this message translates to:
  /// **'서버가 켜기 신호를 보내지 못했어요.'**
  String get serverCouldntSendWakeSignal;

  /// No description provided for @localPcMustConnectedBy.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC가 유선 랜으로 연결돼 있어야 해요.'**
  String get localPcMustConnectedBy;

  /// No description provided for @wakeLanMustBiosUefi.
  ///
  /// In ko, this message translates to:
  /// **'BIOS/UEFI 설정에서 Wake-on-LAN(랜으로 켜기)이 켜져 있어야 해요.'**
  String get wakeLanMustBiosUefi;

  /// No description provided for @deviceManagerNetworkAdapterPower.
  ///
  /// In ko, this message translates to:
  /// **'장치 관리자 → 랜카드 → 전원 관리·고급에서 ‘매직 패킷으로 깨우기’가 켜져 있어야 해요.'**
  String get deviceManagerNetworkAdapterPower;

  /// No description provided for @localPcMustSameNetwork.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC가 서버와 같은 네트워크(같은 공유기·스위치)에 있어야 해요.'**
  String get localPcMustSameNetwork;

  /// No description provided for @installLatestHostLocalPc.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC에 최신 Host를 설치하고 한 번 켜 두면 서버가 랜카드 정보를 받아요.'**
  String get installLatestHostLocalPc;

  /// No description provided for @windowed.
  ///
  /// In ko, this message translates to:
  /// **'창 모드'**
  String get windowed;

  /// No description provided for @fullScreen.
  ///
  /// In ko, this message translates to:
  /// **'전체 화면'**
  String get fullScreen;

  /// No description provided for @allMonitors.
  ///
  /// In ko, this message translates to:
  /// **'모든 모니터'**
  String get allMonitors;

  /// No description provided for @askAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'관리자에게 요청하세요.'**
  String get askAdministrator;

  /// No description provided for @selectMultitapPrinterDesktopPrint.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 Multitap 프린터를 선택하세요.\n기기의 인쇄 화면에서 인쇄하거나 PDF로 저장해요.'**
  String get selectMultitapPrinterDesktopPrint;

  /// No description provided for @chooseFolderDeviceUnderShared.
  ///
  /// In ko, this message translates to:
  /// **'메뉴의 공유 폴더에서 이 기기의 폴더를 고르세요.\n데스크톱에서 저장한 파일의 수정 날짜는 저장한 시각이에요.'**
  String get chooseFolderDeviceUnderShared;

  /// No description provided for @copyPaste.
  ///
  /// In ko, this message translates to:
  /// **'복사·붙여넣기'**
  String get copyPaste;

  /// No description provided for @youCanPasteCopiedText.
  ///
  /// In ko, this message translates to:
  /// **'복사한 글을 기기와 데스크톱 사이에서 붙여넣을 수 있어요.'**
  String get youCanPasteCopiedText;

  /// No description provided for @youCanPasteTextCopied.
  ///
  /// In ko, this message translates to:
  /// **'기기에서 복사한 글을 데스크톱에 붙여넣을 수 있어요.'**
  String get youCanPasteTextCopied;

  /// No description provided for @youCanPasteTextCopied2.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 복사한 글을 기기에 붙여넣을 수 있어요.'**
  String get youCanPasteTextCopied2;

  /// No description provided for @onceConnectionReady.
  ///
  /// In ko, this message translates to:
  /// **'연결 준비가 끝나면 '**
  String get onceConnectionReady;

  /// No description provided for @pcDesktop.
  ///
  /// In ko, this message translates to:
  /// **'PC ↔ 데스크톱'**
  String get pcDesktop;

  /// No description provided for @pcDesktop2.
  ///
  /// In ko, this message translates to:
  /// **'PC → 데스크톱'**
  String get pcDesktop2;

  /// No description provided for @desktopPc.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 → PC'**
  String get desktopPc;

  /// No description provided for @text12.
  ///
  /// In ko, this message translates to:
  /// **'텍스트'**
  String get text12;

  /// No description provided for @files.
  ///
  /// In ko, this message translates to:
  /// **'파일'**
  String get files;

  /// No description provided for @selectMultitapPrinterDesktopPreview.
  ///
  /// In ko, this message translates to:
  /// **'{printState}데스크톱에서 Multitap 프린터를 선택하세요.\nPC에서 미리보기 후 인쇄하거나 PDF로 저장해요.'**
  String selectMultitapPrinterDesktopPreview(Object printState);

  /// No description provided for @usbDevices.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치'**
  String get usbDevices;

  /// No description provided for @selectUsbDeviceToolbarConnected.
  ///
  /// In ko, this message translates to:
  /// **'{usbState}툴바에서 USB 장치를 선택하세요.\n연결한 장치는 데스크톱에서만 쓸 수 있어요.'**
  String selectUsbDeviceToolbarConnected(Object usbState);

  /// No description provided for @selectPcFolderToolbarSharing.
  ///
  /// In ko, this message translates to:
  /// **'{folderState}툴바에서 PC 폴더를 선택하세요.\n연결을 끊으면 공유도 끝나요.'**
  String selectPcFolderToolbarSharing(Object folderState);

  /// No description provided for @filesTransferredWhenYouPaste.
  ///
  /// In ko, this message translates to:
  /// **'\n파일은 붙여넣을 때 전송해요.'**
  String get filesTransferredWhenYouPaste;

  /// No description provided for @useCtrlCCtrlV.
  ///
  /// In ko, this message translates to:
  /// **'{clipboardState}Ctrl+C / Ctrl+V를 사용하세요.\n{n}{policy}'**
  String useCtrlCCtrlV(Object clipboardState, Object n, Object policy);

  /// No description provided for @desktopHasNoSoundDevice2.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에는 소리 장치가 없어요. 관리자가 Proxmox에서 오디오 장치를 추가해야 해요.'**
  String get desktopHasNoSoundDevice2;

  /// No description provided for @restartDesktopUseSoundDevice.
  ///
  /// In ko, this message translates to:
  /// **'소리 장치를 쓰려면 데스크톱을 다시 시작해야 해요.'**
  String get restartDesktopUseSoundDevice;

  /// No description provided for @work.
  ///
  /// In ko, this message translates to:
  /// **'작업'**
  String get work;

  /// No description provided for @n60FpsAutoBitrate.
  ///
  /// In ko, this message translates to:
  /// **'60 fps · 자동 전송률'**
  String get n60FpsAutoBitrate;

  /// No description provided for @saver.
  ///
  /// In ko, this message translates to:
  /// **'절약'**
  String get saver;

  /// No description provided for @n30Fps8MbpsUses.
  ///
  /// In ko, this message translates to:
  /// **'30 fps · 8 Mbps · 데이터 사용량과 처리 부담을 줄여요'**
  String get n30Fps8MbpsUses;

  /// No description provided for @restartNeeded.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작 필요'**
  String get restartNeeded;

  /// No description provided for @restartWindowsGetVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 다시 시작해야 가상 화면이 준비돼요.'**
  String get restartWindowsGetVirtualDisplay;

  /// No description provided for @consoleNeeded.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 필요'**
  String get consoleNeeded;

  /// No description provided for @virtualDisplayCantPreparedRdp.
  ///
  /// In ko, this message translates to:
  /// **'RDP 로그인에서는 가상 화면을 준비할 수 없어요. 관리자에게 알려 주세요.'**
  String get virtualDisplayCantPreparedRdp;

  /// No description provided for @signNeeded.
  ///
  /// In ko, this message translates to:
  /// **'로그인 필요'**
  String get signNeeded;

  /// No description provided for @signWindowsConnect.
  ///
  /// In ko, this message translates to:
  /// **'Windows에 로그인하면 연결할 수 있어요.'**
  String get signWindowsConnect;

  /// No description provided for @checkWindowsAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 확인'**
  String get checkWindowsAccount;

  /// No description provided for @windowsAccountMustCheckedHost.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정 → Windows 로그인에서 Windows 계정을 확인해야 해요. 관리자에게 알려 주세요.'**
  String get windowsAccountMustCheckedHost;

  /// No description provided for @checkWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면 확인'**
  String get checkWindowsScreen;

  /// No description provided for @couldntCheckWindowsScreenIf.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면을 확인하지 못했어요. 계속되면 관리자에게 알려 주세요.'**
  String get couldntCheckWindowsScreenIf;

  /// No description provided for @resumeWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows 복귀'**
  String get resumeWindows;

  /// No description provided for @windowsLocked.
  ///
  /// In ko, this message translates to:
  /// **'Windows 잠김'**
  String get windowsLocked;

  /// No description provided for @windowsLockedUnlockConnect.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 잠겨 있어요. 잠금을 해제하면 연결할 수 있어요.'**
  String get windowsLockedUnlockConnect;

  /// No description provided for @adminAccountNeeded.
  ///
  /// In ko, this message translates to:
  /// **'관리자 계정 필요'**
  String get adminAccountNeeded;

  /// No description provided for @windowsAccountSignedNowIsnt.
  ///
  /// In ko, this message translates to:
  /// **'지금 로그인한 Windows 계정은 관리자가 아니에요. 관리자 계정으로 로그인하면 연결할 수 있어요.'**
  String get windowsAccountSignedNowIsnt;

  /// No description provided for @repairNeeded.
  ///
  /// In ko, this message translates to:
  /// **'복구 필요'**
  String get repairNeeded;

  /// No description provided for @virtualDisplayNeedsRepairTell.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 복구해야 해요. 관리자에게 알려 주세요.'**
  String get virtualDisplayNeedsRepairTell;

  /// No description provided for @notReady.
  ///
  /// In ko, this message translates to:
  /// **'준비 안 됨'**
  String get notReady;

  /// No description provided for @virtualDisplayIsntReadyIf2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면이 준비되지 않았어요. 계속되면 관리자에게 알려 주세요.'**
  String get virtualDisplayIsntReadyIf2;

  /// No description provided for @checkDriver.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 확인'**
  String get checkDriver;

  /// No description provided for @couldntCheckVirtualDisplayDriver.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버를 확인하지 못했어요. 관리자에게 알려 주세요.'**
  String get couldntCheckVirtualDisplayDriver;

  /// No description provided for @checkSettings.
  ///
  /// In ko, this message translates to:
  /// **'설정 확인'**
  String get checkSettings;

  /// No description provided for @virtualDisplaySettingsNeedChecking.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 설정을 확인해야 해요. 관리자에게 알려 주세요.'**
  String get virtualDisplaySettingsNeedChecking;

  /// No description provided for @hostSettingUpVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'Host가 가상 화면을 구성하고 있어요. 끝나면 연결할 수 있어요.'**
  String get hostSettingUpVirtualDisplay;

  /// No description provided for @checkStatus.
  ///
  /// In ko, this message translates to:
  /// **'상태 확인'**
  String get checkStatus;

  /// No description provided for @couldntCheckVirtualDisplayStatus.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 상태를 확인하지 못했어요. 계속되면 관리자에게 알려 주세요.'**
  String get couldntCheckVirtualDisplayStatus;

  /// No description provided for @checkingMonitors.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수 확인 중'**
  String get checkingMonitors;

  /// No description provided for @checkMonitorCount.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수 확인 필요'**
  String get checkMonitorCount;

  /// No description provided for @checkAgain2.
  ///
  /// In ko, this message translates to:
  /// **' · 다시 확인 필요'**
  String get checkAgain2;

  /// No description provided for @text13.
  ///
  /// In ko, this message translates to:
  /// **'모니터 {activeCount}대{stale}'**
  String text13(num activeCount, Object stale);

  /// No description provided for @theme2.
  ///
  /// In ko, this message translates to:
  /// **'테마'**
  String get theme2;

  /// No description provided for @background3.
  ///
  /// In ko, this message translates to:
  /// **'배경'**
  String get background3;

  /// No description provided for @disconnect2.
  ///
  /// In ko, this message translates to:
  /// **'연결을 끊을까요?'**
  String get disconnect2;

  /// No description provided for @streaming3.
  ///
  /// In ko, this message translates to:
  /// **'{machine} · 스트리밍'**
  String streaming3(Object machine);

  /// No description provided for @ifOneUseUnsavedWork.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 연결한 USB 장치 {usbShared}개도 끊겨요. 쓰는 중이면 저장하지 않은 내용이 사라질 수 있어요.'**
  String ifOneUseUnsavedWork(num usbShared);

  /// No description provided for @programsDesktopSavingThemMay.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 공유한 폴더 {foldersShared}개도 끊겨요. 데스크톱에서 그 폴더에 저장 중인 프로그램은 저장하지 못할 수 있어요.'**
  String programsDesktopSavingThemMay(num foldersShared);

  /// No description provided for @text14.
  ///
  /// In ko, this message translates to:
  /// **'아직 인쇄하거나 저장하지 않은 문서 {printsWaiting}개는 사라져요.'**
  String text14(num printsWaiting);

  /// No description provided for @clientInfo.
  ///
  /// In ko, this message translates to:
  /// **'클라이언트 정보'**
  String get clientInfo;

  /// No description provided for @windowsClient.
  ///
  /// In ko, this message translates to:
  /// **'Windows 클라이언트'**
  String get windowsClient;

  /// No description provided for @version.
  ///
  /// In ko, this message translates to:
  /// **'버전'**
  String get version;

  /// No description provided for @openSourceLicenses.
  ///
  /// In ko, this message translates to:
  /// **'오픈소스 라이선스'**
  String get openSourceLicenses;

  /// No description provided for @licenses.
  ///
  /// In ko, this message translates to:
  /// **'라이선스'**
  String get licenses;

  /// No description provided for @about.
  ///
  /// In ko, this message translates to:
  /// **'정보'**
  String get about;

  /// No description provided for @build.
  ///
  /// In ko, this message translates to:
  /// **'빌드'**
  String get build;

  /// No description provided for @demoMode.
  ///
  /// In ko, this message translates to:
  /// **'데모 모드'**
  String get demoMode;

  /// No description provided for @multitapServer.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 서버'**
  String get multitapServer;

  /// No description provided for @screenIsntConnectedRealServer.
  ///
  /// In ko, this message translates to:
  /// **'실제 서버에 연결하지 않은 화면이에요.'**
  String get screenIsntConnectedRealServer;

  /// No description provided for @serverManagesDesktopListConnections.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 목록·연결·업데이트를 이 서버가 관리해요.'**
  String get serverManagesDesktopListConnections;

  /// No description provided for @address.
  ///
  /// In ko, this message translates to:
  /// **'주소'**
  String get address;

  /// No description provided for @serverVersion.
  ///
  /// In ko, this message translates to:
  /// **'서버 버전'**
  String get serverVersion;

  /// No description provided for @unavailable.
  ///
  /// In ko, this message translates to:
  /// **'확인할 수 없음'**
  String get unavailable;

  /// No description provided for @account2.
  ///
  /// In ko, this message translates to:
  /// **'계정'**
  String get account2;

  /// No description provided for @searchComponentsLicenseText.
  ///
  /// In ko, this message translates to:
  /// **'구성 요소 또는 원문 검색'**
  String get searchComponentsLicenseText;

  /// No description provided for @componentsSearchNameLicenseText.
  ///
  /// In ko, this message translates to:
  /// **'구성 요소 {componentsLength}개 · 이름 또는 원문 검색'**
  String componentsSearchNameLicenseText(Object componentsLength);

  /// No description provided for @someLicensesCouldntLoaded.
  ///
  /// In ko, this message translates to:
  /// **'일부 라이선스를 불러오지 못했어요.'**
  String get someLicensesCouldntLoaded;

  /// No description provided for @loading.
  ///
  /// In ko, this message translates to:
  /// **'불러오는 중…'**
  String get loading;

  /// No description provided for @reload.
  ///
  /// In ko, this message translates to:
  /// **'다시 불러오기'**
  String get reload;

  /// No description provided for @noLicensesShow.
  ///
  /// In ko, this message translates to:
  /// **'표시할 라이선스가 없어요.'**
  String get noLicensesShow;

  /// No description provided for @noMatchingLicenses.
  ///
  /// In ko, this message translates to:
  /// **'일치하는 라이선스가 없어요.'**
  String get noMatchingLicenses;

  /// No description provided for @text15.
  ///
  /// In ko, this message translates to:
  /// **'원문 {noticesLength}개'**
  String text15(num noticesLength);

  /// No description provided for @version2.
  ///
  /// In ko, this message translates to:
  /// **'버전 {version}'**
  String version2(Object version);

  /// No description provided for @couldntSaveThemeChooseAgain.
  ///
  /// In ko, this message translates to:
  /// **'테마를 저장하지 못했어요. 다시 선택해 주세요.'**
  String get couldntSaveThemeChooseAgain;

  /// No description provided for @couldntSaveBackgroundChooseAgain.
  ///
  /// In ko, this message translates to:
  /// **'배경을 저장하지 못했어요. 다시 선택해 주세요.'**
  String get couldntSaveBackgroundChooseAgain;

  /// No description provided for @connectWithoutEncryption.
  ///
  /// In ko, this message translates to:
  /// **'암호화 없이 연결할까요?'**
  String get connectWithoutEncryption;

  /// No description provided for @signDetailsScreenInputArent.
  ///
  /// In ko, this message translates to:
  /// **'로그인 정보·화면·입력이 암호화되지 않아요.\n믿을 수 있는 네트워크에서만 쓰세요.\n동의는 이 주소·포트에만 적용돼요.'**
  String get signDetailsScreenInputArent;

  /// No description provided for @useHttpAddress.
  ///
  /// In ko, this message translates to:
  /// **'이 주소에 HTTP 사용'**
  String get useHttpAddress;

  /// No description provided for @hostBulkUpdate.
  ///
  /// In ko, this message translates to:
  /// **'Host 일괄 업데이트'**
  String get hostBulkUpdate;

  /// No description provided for @hostsAssignedDesktops.
  ///
  /// In ko, this message translates to:
  /// **'배정된 데스크톱의 Host'**
  String get hostsAssignedDesktops;

  /// No description provided for @assignedDesktopsLatest.
  ///
  /// In ko, this message translates to:
  /// **'배정된 데스크톱 {visibleRowsLength}대 · 최신 {version}'**
  String assignedDesktopsLatest(Object visibleRowsLength, Object version);

  /// No description provided for @checkingServersLatestHostRelease.
  ///
  /// In ko, this message translates to:
  /// **'이 서버의 최신 Host 배포 정보를 확인하고 있어요.'**
  String get checkingServersLatestHostRelease;

  /// No description provided for @couldntLoadLatestHostRelease.
  ///
  /// In ko, this message translates to:
  /// **'최신 Host 배포 정보를 불러오지 못했어요.\n창을 닫고 업데이트를 다시 확인하세요.'**
  String get couldntLoadLatestHostRelease;

  /// No description provided for @hostChanges.
  ///
  /// In ko, this message translates to:
  /// **'Host {version} 변경 내역'**
  String hostChanges(Object version);

  /// No description provided for @theresNoConnectionCheckInstall.
  ///
  /// In ko, this message translates to:
  /// **'설치 결과를 확인할 연결이 없어 요청할 수 없어요.'**
  String get theresNoConnectionCheckInstall;

  /// No description provided for @couldntGetLatestStatusFrom.
  ///
  /// In ko, this message translates to:
  /// **'서버에서 최신 상태를 받지 못했어요. 접수된 요청의 결과는 계속 확인하고 있어요.'**
  String get couldntGetLatestStatusFrom;

  /// No description provided for @remainingRequestsStoppedBecauseYou.
  ///
  /// In ko, this message translates to:
  /// **'권한이 없거나 로그인이 만료돼 남은 요청을 중지했어요. 접수된 업데이트는 계속돼요.'**
  String get remainingRequestsStoppedBecauseYou;

  /// No description provided for @latestReleaseChangedSoRemaining.
  ///
  /// In ko, this message translates to:
  /// **'최신 배포가 바뀌어 남은 요청을 중지했어요. 이 창을 닫고 새 버전을 확인하세요.'**
  String get latestReleaseChangedSoRemaining;

  /// No description provided for @waitingNextRequestLeft.
  ///
  /// In ko, this message translates to:
  /// **'다음 요청을 기다리는 중 · 남은 {pendingCount}대'**
  String waitingNextRequestLeft(Object pendingCount);

  /// No description provided for @refreshInstallStatus.
  ///
  /// In ko, this message translates to:
  /// **'설치 상태 새로고침'**
  String get refreshInstallStatus;

  /// No description provided for @noDesktopsAssigned2.
  ///
  /// In ko, this message translates to:
  /// **'배정된 데스크톱이 없어요.'**
  String get noDesktopsAssigned2;

  /// No description provided for @checkProgress.
  ///
  /// In ko, this message translates to:
  /// **'진행 상태 확인 필요'**
  String get checkProgress;

  /// No description provided for @requestServerAcceptedIsntFinished.
  ///
  /// In ko, this message translates to:
  /// **'서버 접수는 설치 완료가 아니에요.\n각 데스크톱의 결과를 확인하세요.'**
  String get requestServerAcceptedIsntFinished;

  /// No description provided for @windowsRestartNeededCheckingResults.
  ///
  /// In ko, this message translates to:
  /// **'Windows 다시 시작 필요 {rebootCount}대 · 결과 확인 {checkingCount}대'**
  String windowsRestartNeededCheckingResults(
    Object rebootCount,
    Object checkingCount,
  );

  /// No description provided for @connectedDesktopsDisconnectedUpdatedRight.
  ///
  /// In ko, this message translates to:
  /// **'연결된 데스크톱은 연결을 끊고 바로 업데이트해요.\nWindows는 다시 시작하지 않아요.'**
  String get connectedDesktopsDisconnectedUpdatedRight;

  /// No description provided for @connectedDesktopsConnectionEndsUpdate.
  ///
  /// In ko, this message translates to:
  /// **'연결된 데스크톱은 내 연결을 끊고 바로 업데이트해요.\n다른 사용자가 연결 중이면 그 연결이 끝난 뒤 업데이트해요.'**
  String get connectedDesktopsConnectionEndsUpdate;

  /// No description provided for @stopRemainingRequestsClose.
  ///
  /// In ko, this message translates to:
  /// **'남은 요청 중지·닫기'**
  String get stopRemainingRequestsClose;

  /// No description provided for @stopRemainingRequests.
  ///
  /// In ko, this message translates to:
  /// **'남은 요청 중지'**
  String get stopRemainingRequests;

  /// No description provided for @updateSelected.
  ///
  /// In ko, this message translates to:
  /// **'선택한 {selectedCount}대 업데이트'**
  String updateSelected(Object selectedCount);

  /// No description provided for @selectAll2.
  ///
  /// In ko, this message translates to:
  /// **'모두 선택'**
  String get selectAll2;

  /// No description provided for @currentVersionUnknown.
  ///
  /// In ko, this message translates to:
  /// **'현재 버전 확인 필요'**
  String get currentVersionUnknown;

  /// No description provided for @windowsRestartNeeded.
  ///
  /// In ko, this message translates to:
  /// **'Windows 다시 시작 필요'**
  String get windowsRestartNeeded;

  /// No description provided for @enterCurrentPasswordChooseNew.
  ///
  /// In ko, this message translates to:
  /// **'현재 비밀번호를 입력하고 새 비밀번호는 10~256자로 정해 주세요.'**
  String get enterCurrentPasswordChooseNew;

  /// No description provided for @couldntChangePasswordTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호를 변경하지 못했어요. 다시 시도해 주세요.'**
  String get couldntChangePasswordTryAgain;

  /// No description provided for @newPasswordLeast10Characters.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호 · 10자 이상'**
  String get newPasswordLeast10Characters;

  /// No description provided for @changing.
  ///
  /// In ko, this message translates to:
  /// **'변경 중…'**
  String get changing;

  /// No description provided for @transferring.
  ///
  /// In ko, this message translates to:
  /// **'옮기는 중'**
  String get transferring;

  /// No description provided for @preparing2.
  ///
  /// In ko, this message translates to:
  /// **'준비하는 중…'**
  String get preparing2;

  /// No description provided for @finishing.
  ///
  /// In ko, this message translates to:
  /// **'마무리하는 중…'**
  String get finishing;

  /// No description provided for @image.
  ///
  /// In ko, this message translates to:
  /// **'이미지'**
  String get image;

  /// No description provided for @richText.
  ///
  /// In ko, this message translates to:
  /// **'서식 있는 텍스트'**
  String get richText;

  /// No description provided for @ofLabel.
  ///
  /// In ko, this message translates to:
  /// **'{fileCount}개 중 {progress}개'**
  String ofLabel(Object fileCount, Object progress);

  /// No description provided for @folders.
  ///
  /// In ko, this message translates to:
  /// **'폴더 {folderCount}개'**
  String folders(Object folderCount);

  /// No description provided for @files2.
  ///
  /// In ko, this message translates to:
  /// **'파일 {fileCount}개'**
  String files2(Object fileCount);

  /// No description provided for @currentFile.
  ///
  /// In ko, this message translates to:
  /// **'옮기는 파일'**
  String get currentFile;

  /// No description provided for @items.
  ///
  /// In ko, this message translates to:
  /// **'항목'**
  String get items;

  /// No description provided for @saved2.
  ///
  /// In ko, this message translates to:
  /// **'저장 위치'**
  String get saved2;

  /// No description provided for @transferred.
  ///
  /// In ko, this message translates to:
  /// **'옮겼어요'**
  String get transferred;

  /// No description provided for @inLabel.
  ///
  /// In ko, this message translates to:
  /// **'{destinationName}에 있어요'**
  String inLabel(Object destinationName);

  /// No description provided for @transferCanceled.
  ///
  /// In ko, this message translates to:
  /// **'전송을 취소했어요'**
  String get transferCanceled;

  /// No description provided for @couldntTransfer.
  ///
  /// In ko, this message translates to:
  /// **'옮기지 못했어요'**
  String get couldntTransfer;

  /// No description provided for @transferringFiles.
  ///
  /// In ko, this message translates to:
  /// **'파일 옮기는 중'**
  String get transferringFiles;

  /// No description provided for @openFolder.
  ///
  /// In ko, this message translates to:
  /// **'폴더 열기'**
  String get openFolder;

  /// No description provided for @minimize.
  ///
  /// In ko, this message translates to:
  /// **'작게 접기'**
  String get minimize;

  /// No description provided for @transferProgress.
  ///
  /// In ko, this message translates to:
  /// **'전송 진행률'**
  String get transferProgress;

  /// No description provided for @less.
  ///
  /// In ko, this message translates to:
  /// **'간단히'**
  String get less;

  /// No description provided for @details.
  ///
  /// In ko, this message translates to:
  /// **'자세히'**
  String get details;

  /// No description provided for @canceling.
  ///
  /// In ko, this message translates to:
  /// **'취소하는 중이에요'**
  String get canceling;

  /// No description provided for @cancelTransfer.
  ///
  /// In ko, this message translates to:
  /// **'이 전송을 취소해요'**
  String get cancelTransfer;

  /// No description provided for @canceling2.
  ///
  /// In ko, this message translates to:
  /// **'취소 중…'**
  String get canceling2;

  /// No description provided for @showTransferDetails.
  ///
  /// In ko, this message translates to:
  /// **'전송 상세 보기'**
  String get showTransferDetails;

  /// No description provided for @almostDone.
  ///
  /// In ko, this message translates to:
  /// **'곧 끝나요'**
  String get almostDone;

  /// No description provided for @aboutSecLeft.
  ///
  /// In ko, this message translates to:
  /// **'약 {seconds}초 남음'**
  String aboutSecLeft(Object seconds);

  /// No description provided for @aboutMinLeft.
  ///
  /// In ko, this message translates to:
  /// **'약 {minutes}분 남음'**
  String aboutMinLeft(Object minutes);

  /// No description provided for @aboutLeft.
  ///
  /// In ko, this message translates to:
  /// **'약 {minutes}시간 남음'**
  String aboutLeft(num minutes);

  /// No description provided for @aboutHMinLeft.
  ///
  /// In ko, this message translates to:
  /// **'약 {minutes}시간 {rest}분 남음'**
  String aboutHMinLeft(Object minutes, Object rest);

  /// No description provided for @elapsed.
  ///
  /// In ko, this message translates to:
  /// **'경과 {elapsed}'**
  String elapsed(Object elapsed);

  /// No description provided for @elapsed2.
  ///
  /// In ko, this message translates to:
  /// **'{stepLabel}, 경과 {elapsed}'**
  String elapsed2(Object stepLabel, Object elapsed);

  /// No description provided for @couldntConfirmSessionSwitchResult.
  ///
  /// In ko, this message translates to:
  /// **'세션 전환 결과를 확인하지 못했어요. 목록을 새로고침하세요.'**
  String get couldntConfirmSessionSwitchResult;

  /// No description provided for @couldntTakeOverWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면을 넘겨받지 못했어요'**
  String get couldntTakeOverWindowsScreen;

  /// No description provided for @takingOverWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면을 넘겨받는 중'**
  String get takingOverWindowsScreen;

  /// No description provided for @bringingOverWindowsScreenYou.
  ///
  /// In ko, this message translates to:
  /// **'RDP에서 쓰던 Windows 화면을 가져오고 있어요.\n끝나면 바로 연결해요.\n취소해도 이미 시작한 전환은 계속될 수 있어요.'**
  String get bringingOverWindowsScreenYou;

  /// No description provided for @start.
  ///
  /// In ko, this message translates to:
  /// **'켜기'**
  String get start;

  /// No description provided for @stop.
  ///
  /// In ko, this message translates to:
  /// **'정지'**
  String get stop;

  /// No description provided for @shutDown.
  ///
  /// In ko, this message translates to:
  /// **'종료'**
  String get shutDown;

  /// No description provided for @stopIsntAvailableServerYet.
  ///
  /// In ko, this message translates to:
  /// **'이 서버에서는 아직 정지를 쓸 수 없어요. 관리자에게 알려 주세요.'**
  String get stopIsntAvailableServerYet;

  /// No description provided for @localPc2.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC를'**
  String get localPc2;

  /// No description provided for @desktop3.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱을'**
  String get desktop3;

  /// No description provided for @start2.
  ///
  /// In ko, this message translates to:
  /// **'{noun} 켤까요?'**
  String start2(Object noun);

  /// No description provided for @the.
  ///
  /// In ko, this message translates to:
  /// **'{noun} {label}할까요?'**
  String the(Object noun, Object label);

  /// No description provided for @startsLocalPcOverNetwork.
  ///
  /// In ko, this message translates to:
  /// **'네트워크로 로컬 PC를 켜요. 켜진 뒤에 연결할 수 있어요.'**
  String get startsLocalPcOverNetwork;

  /// No description provided for @youCanConnectOnceIts.
  ///
  /// In ko, this message translates to:
  /// **'켜진 뒤에 연결할 수 있어요.'**
  String get youCanConnectOnceIts;

  /// No description provided for @cutsPowerRightAwayUnsaved.
  ///
  /// In ko, this message translates to:
  /// **'전원을 바로 끊어요. 저장하지 않은 작업이 사라질 수 있어요. Windows가 응답하지 않을 때만 쓰세요.'**
  String get cutsPowerRightAwayUnsaved;

  /// No description provided for @saveAnyUnsavedWorkFirst.
  ///
  /// In ko, this message translates to:
  /// **'저장하지 않은 작업이 있으면 먼저 저장하세요.'**
  String get saveAnyUnsavedWorkFirst;

  /// No description provided for @asksWindowsShutDownNormally.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 정상적으로 끄도록 요청해요. 저장하지 않은 작업이 있으면 먼저 저장하세요.'**
  String get asksWindowsShutDownNormally;

  /// No description provided for @pcNeeds2Monitors.
  ///
  /// In ko, this message translates to:
  /// **'PC에 모니터가 2대 있어야 해요.'**
  String get pcNeeds2Monitors;

  /// No description provided for @desktopsHostDoesntSupportAll.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱의 Host가 모든 모니터를 지원하지 않아요.'**
  String get desktopsHostDoesntSupportAll;

  /// No description provided for @defaultLabel.
  ///
  /// In ko, this message translates to:
  /// **'기본'**
  String get defaultLabel;

  /// No description provided for @streamingSettings2.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 설정…'**
  String get streamingSettings2;

  /// No description provided for @removeFromFavorites.
  ///
  /// In ko, this message translates to:
  /// **'즐겨찾기에서 빼기'**
  String get removeFromFavorites;

  /// No description provided for @addFavorites.
  ///
  /// In ko, this message translates to:
  /// **'즐겨찾기에 추가'**
  String get addFavorites;

  /// No description provided for @checkReadiness.
  ///
  /// In ko, this message translates to:
  /// **'연결 준비 점검'**
  String get checkReadiness;

  /// No description provided for @searchCtrlF.
  ///
  /// In ko, this message translates to:
  /// **'검색 (Ctrl+F)'**
  String get searchCtrlF;

  /// No description provided for @clear.
  ///
  /// In ko, this message translates to:
  /// **'지우기'**
  String get clear;

  /// No description provided for @closeSearch.
  ///
  /// In ko, this message translates to:
  /// **'검색 닫기'**
  String get closeSearch;

  /// No description provided for @searchNameNumberGroup.
  ///
  /// In ko, this message translates to:
  /// **'이름, 번호, 분류 검색'**
  String get searchNameNumberGroup;

  /// No description provided for @statusFilter.
  ///
  /// In ko, this message translates to:
  /// **'상태 필터'**
  String get statusFilter;

  /// No description provided for @typeFilter.
  ///
  /// In ko, this message translates to:
  /// **'구분 필터'**
  String get typeFilter;

  /// No description provided for @text16.
  ///
  /// In ko, this message translates to:
  /// **'{label} {count}개'**
  String text16(Object label, Object count);

  /// No description provided for @text17.
  ///
  /// In ko, this message translates to:
  /// **'{chipLabel} {count}개'**
  String text17(Object chipLabel, Object count);

  /// No description provided for @selectAgainClear.
  ///
  /// In ko, this message translates to:
  /// **'다시 누르면 해제돼요'**
  String get selectAgainClear;

  /// No description provided for @showOnly.
  ///
  /// In ko, this message translates to:
  /// **'{chipLabel}만 보기'**
  String showOnly(Object chipLabel);

  /// No description provided for @windowsNeedsRestart.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 다시 시작해야 해요'**
  String get windowsNeedsRestart;

  /// No description provided for @getsVirtualDisplayReadyUnsaved.
  ///
  /// In ko, this message translates to:
  /// **'그래야 가상 화면이 준비돼요. 저장하지 않은 작업은 사라질 수 있어요.'**
  String get getsVirtualDisplayReadyUnsaved;

  /// No description provided for @signedOverRdp.
  ///
  /// In ko, this message translates to:
  /// **'RDP로 로그인돼 있어요'**
  String get signedOverRdp;

  /// No description provided for @virtualDisplayCantPreparedRdp2.
  ///
  /// In ko, this message translates to:
  /// **'RDP 로그인에서는 가상 화면을 준비할 수 없어요.'**
  String get virtualDisplayCantPreparedRdp2;

  /// No description provided for @notSignedWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows에 로그인돼 있지 않아요'**
  String get notSignedWindows;

  /// No description provided for @windowsAccountNeedsChecking.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 확인이 필요해요'**
  String get windowsAccountNeedsChecking;

  /// No description provided for @couldntCheckWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면을 확인하지 못했어요'**
  String get couldntCheckWindowsScreen;

  /// No description provided for @refreshCheckAgainIfKeeps2.
  ///
  /// In ko, this message translates to:
  /// **'새로고침해 다시 확인하세요. 계속되면 관리자에게 알려 주세요.'**
  String get refreshCheckAgainIfKeeps2;

  /// No description provided for @windowsScreenFromRdpOpen.
  ///
  /// In ko, this message translates to:
  /// **'RDP에서 쓰던 Windows 화면이 있어요'**
  String get windowsScreenFromRdpOpen;

  /// No description provided for @bringScreenIntoStreamingConnect.
  ///
  /// In ko, this message translates to:
  /// **'그 화면을 스트리밍으로 가져와야 연결할 수 있어요.'**
  String get bringScreenIntoStreamingConnect;

  /// No description provided for @windowsLocked2.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 잠겨 있어요'**
  String get windowsLocked2;

  /// No description provided for @unlockConnect.
  ///
  /// In ko, this message translates to:
  /// **'잠금을 해제하면 연결할 수 있어요.'**
  String get unlockConnect;

  /// No description provided for @signWindowsAdministratorAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows 관리자 계정으로 로그인해야 해요'**
  String get signWindowsAdministratorAccount;

  /// No description provided for @virtualDisplayNeedsRepair.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 복구해야 해요'**
  String get virtualDisplayNeedsRepair;

  /// No description provided for @youCanRepairHostSettings.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정에서 복구할 수 있어요. 관리자에게 알려 주세요.'**
  String get youCanRepairHostSettings;

  /// No description provided for @virtualDisplayIsntReady.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면이 준비되지 않았어요'**
  String get virtualDisplayIsntReady;

  /// No description provided for @refreshMomentIfKeepsHappening.
  ///
  /// In ko, this message translates to:
  /// **'잠시 후 새로고침하세요. 계속되면 관리자에게 알려 주세요.'**
  String get refreshMomentIfKeepsHappening;

  /// No description provided for @virtualDisplayDriverNeedsChecking.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버를 확인해야 해요'**
  String get virtualDisplayDriverNeedsChecking;

  /// No description provided for @youCanCheckHostSettings.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정의 진단에서 확인할 수 있어요. 관리자에게 알려 주세요.'**
  String get youCanCheckHostSettings;

  /// No description provided for @virtualDisplaySettingsNeedChecking2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 설정 확인이 필요해요'**
  String get virtualDisplaySettingsNeedChecking2;

  /// No description provided for @hostSettingUpScreen.
  ///
  /// In ko, this message translates to:
  /// **'Host가 화면을 구성하고 있어요'**
  String get hostSettingUpScreen;

  /// No description provided for @youCanConnectWhenSetup.
  ///
  /// In ko, this message translates to:
  /// **'구성이 끝나면 연결할 수 있어요.'**
  String get youCanConnectWhenSetup;

  /// No description provided for @couldntCheckVirtualDisplayStatus2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 상태를 확인하지 못했어요'**
  String get couldntCheckVirtualDisplayStatus2;

  /// No description provided for @checkingWindowsSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인 확인 중'**
  String get checkingWindowsSign;

  /// No description provided for @checkingWindowsSignStateUpdates.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인 상태를 확인하고 있어요. 준비되면 바로 바뀌어요.'**
  String get checkingWindowsSignStateUpdates;

  /// No description provided for @preparingScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 준비 중'**
  String get preparingScreen;

  /// No description provided for @preparingVirtualDisplayUpdatesAs.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 준비하고 있어요. 끝나면 바로 바뀌어요.'**
  String get preparingVirtualDisplayUpdatesAs;

  /// No description provided for @preparing3.
  ///
  /// In ko, this message translates to:
  /// **'연결 준비 중'**
  String get preparing3;

  /// No description provided for @hostPreparingConnectionUpdatesAs.
  ///
  /// In ko, this message translates to:
  /// **'Host가 연결을 준비하고 있어요. 끝나면 바로 바뀌어요.'**
  String get hostPreparingConnectionUpdatesAs;

  /// No description provided for @power.
  ///
  /// In ko, this message translates to:
  /// **'전원'**
  String get power;

  /// No description provided for @checkedAfterPowerActionFinishes.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업이 끝나면 확인해요'**
  String get checkedAfterPowerActionFinishes;

  /// No description provided for @confirmedByHostResponse.
  ///
  /// In ko, this message translates to:
  /// **'켜짐 · Host 응답으로 확인'**
  String get confirmedByHostResponse;

  /// No description provided for @off4.
  ///
  /// In ko, this message translates to:
  /// **'꺼져 있어요'**
  String get off4;

  /// No description provided for @pausedTellAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'일시 중지됨 · 관리자에게 알려 주세요'**
  String get pausedTellAdministrator;

  /// No description provided for @powerStateNotReceivedYet.
  ///
  /// In ko, this message translates to:
  /// **'전원 상태를 아직 받지 못했어요'**
  String get powerStateNotReceivedYet;

  /// No description provided for @hostService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스'**
  String get hostService;

  /// No description provided for @checkedWhenDesktopStarts.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 켜지면 확인해요'**
  String get checkedWhenDesktopStarts;

  /// No description provided for @responding.
  ///
  /// In ko, this message translates to:
  /// **'응답함'**
  String get responding;

  /// No description provided for @notRespondingIfWindowsStarting.
  ///
  /// In ko, this message translates to:
  /// **'응답하지 않아요 · Windows가 시작 중이면 잠시 기다려 주세요'**
  String get notRespondingIfWindowsStarting;

  /// No description provided for @noResponseYet.
  ///
  /// In ko, this message translates to:
  /// **'응답을 아직 받지 못했어요'**
  String get noResponseYet;

  /// No description provided for @virtualDisplay2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면'**
  String get virtualDisplay2;

  /// No description provided for @checkedWhenHostServiceResponds.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 응답하면 확인해요'**
  String get checkedWhenHostServiceResponds;

  /// No description provided for @screenRestoreFromLastConnection.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결의 화면 복원이 확인되지 않았어요'**
  String get screenRestoreFromLastConnection;

  /// No description provided for @checkingScreenRestoreFromLast.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결의 화면 복원을 확인하는 중'**
  String get checkingScreenRestoreFromLast;

  /// No description provided for @checkingVirtualDisplaySetup.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 구성을 확인하는 중'**
  String get checkingVirtualDisplaySetup;

  /// No description provided for @numberScreensNotReceivedYet.
  ///
  /// In ko, this message translates to:
  /// **'화면 개수를 아직 받지 못했어요'**
  String get numberScreensNotReceivedYet;

  /// No description provided for @ready3.
  ///
  /// In ko, this message translates to:
  /// **'{count}개 준비됨{m}'**
  String ready3(Object count, Object m);

  /// No description provided for @readyOnceScreenUsedRdp.
  ///
  /// In ko, this message translates to:
  /// **'RDP에서 쓰던 화면을 가져오면 준비돼요'**
  String get readyOnceScreenUsedRdp;

  /// No description provided for @notReadyCheckHostSettings.
  ///
  /// In ko, this message translates to:
  /// **'준비 안 됨 · Host 설정에서 확인해야 해요'**
  String get notReadyCheckHostSettings;

  /// No description provided for @couldNotCheckWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍할 Windows 화면을 확인하지 못했어요'**
  String get couldNotCheckWindowsScreen;

  /// No description provided for @checkingStreamingScreen.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 화면 상태를 확인하는 중'**
  String get checkingStreamingScreen;

  /// No description provided for @connectionPath.
  ///
  /// In ko, this message translates to:
  /// **'연결 경로'**
  String get connectionPath;

  /// No description provided for @availableViaCheckedAgainWhen.
  ///
  /// In ko, this message translates to:
  /// **'{server} 경유 연결 가능 · 연결할 때 다시 확인해요'**
  String availableViaCheckedAgainWhen(Object server);

  /// No description provided for @notCheckedCheckedOnceEarlier.
  ///
  /// In ko, this message translates to:
  /// **'확인 안 됨 · 앞 단계가 준비되면 확인해요'**
  String get notCheckedCheckedOnceEarlier;

  /// No description provided for @sample.
  ///
  /// In ko, this message translates to:
  /// **'예시'**
  String get sample;

  /// No description provided for @screenPreview.
  ///
  /// In ko, this message translates to:
  /// **'화면 미리보기'**
  String get screenPreview;

  /// No description provided for @designSampleNotConnectedServer.
  ///
  /// In ko, this message translates to:
  /// **'디자인 예시 · 서버에 연결하지 않아요'**
  String get designSampleNotConnectedServer;

  /// No description provided for @starting.
  ///
  /// In ko, this message translates to:
  /// **'{noun} 켜는 중이에요'**
  String starting(Object noun);

  /// No description provided for @restarting.
  ///
  /// In ko, this message translates to:
  /// **'{noun} 다시 시작하는 중이에요'**
  String restarting(Object noun);

  /// No description provided for @shuttingDown.
  ///
  /// In ko, this message translates to:
  /// **'{noun} 종료하는 중이에요'**
  String shuttingDown(Object noun);

  /// No description provided for @stopping.
  ///
  /// In ko, this message translates to:
  /// **'{noun} 정지하는 중이에요'**
  String stopping(Object noun);

  /// No description provided for @powerActionProgress.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업 중이에요'**
  String get powerActionProgress;

  /// No description provided for @starting2.
  ///
  /// In ko, this message translates to:
  /// **'켜는 중'**
  String get starting2;

  /// No description provided for @shuttingDown2.
  ///
  /// In ko, this message translates to:
  /// **'종료하는 중'**
  String get shuttingDown2;

  /// No description provided for @stopping2.
  ///
  /// In ko, this message translates to:
  /// **'정지하는 중'**
  String get stopping2;

  /// No description provided for @powerAction.
  ///
  /// In ko, this message translates to:
  /// **'전원 작업 중'**
  String get powerAction;

  /// No description provided for @ifWindowsDoesNotRespond.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 응답하지 않으면 전원 메뉴에서 정지할 수 있어요.'**
  String get ifWindowsDoesNotRespond;

  /// No description provided for @statusCheckedAgainWhenFinishes.
  ///
  /// In ko, this message translates to:
  /// **'작업이 끝나면 상태를 다시 확인해요.'**
  String get statusCheckedAgainWhenFinishes;

  /// No description provided for @windowsSign2.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인'**
  String get windowsSign2;

  /// No description provided for @takeOverWindowsScreenConnect.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면을 넘겨받아 연결'**
  String get takeOverWindowsScreenConnect;

  /// No description provided for @bringsWindowsScreenUsedRdp.
  ///
  /// In ko, this message translates to:
  /// **'RDP에서 쓰던 Windows 화면을 스트리밍으로 가져와요.'**
  String get bringsWindowsScreenUsedRdp;

  /// No description provided for @readyConnect.
  ///
  /// In ko, this message translates to:
  /// **'연결 준비됨'**
  String get readyConnect;

  /// No description provided for @serverSaysYouCanConnect.
  ///
  /// In ko, this message translates to:
  /// **'서버가 연결할 수 있다고 알려 줬어요.'**
  String get serverSaysYouCanConnect;

  /// No description provided for @pressStartStartDesktop.
  ///
  /// In ko, this message translates to:
  /// **'‘켜기’를 누르면 데스크톱이 켜져요.'**
  String get pressStartStartDesktop;

  /// No description provided for @paused.
  ///
  /// In ko, this message translates to:
  /// **'일시 중지'**
  String get paused;

  /// No description provided for @desktopPaused.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 일시 중지돼 있어요'**
  String get desktopPaused;

  /// No description provided for @letAdministratorKnow.
  ///
  /// In ko, this message translates to:
  /// **'관리자에게 알려 주세요.'**
  String get letAdministratorKnow;

  /// No description provided for @cleaningUpConnectionJustEnded.
  ///
  /// In ko, this message translates to:
  /// **'방금 끝난 연결을 정리하고 있어요. 누르면 끝나는 대로 연결해요.'**
  String get cleaningUpConnectionJustEnded;

  /// No description provided for @screenRestoreNotConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'화면 복원이 확인되지 않았어요'**
  String get screenRestoreNotConfirmed;

  /// No description provided for @cleaningUpLastConnection.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결을 정리하고 있어요'**
  String get cleaningUpLastConnection;

  /// No description provided for @youCanConnectWhenDone.
  ///
  /// In ko, this message translates to:
  /// **'끝나면 연결할 수 있어요.'**
  String get youCanConnectWhenDone;

  /// No description provided for @useConsole.
  ///
  /// In ko, this message translates to:
  /// **'Console에서 사용 중이에요'**
  String get useConsole;

  /// No description provided for @youCanDisconnectConsoleConnect.
  ///
  /// In ko, this message translates to:
  /// **'Console을 끊고 스트리밍으로 연결할 수 있어요.'**
  String get youCanDisconnectConsoleConnect;

  /// No description provided for @checking.
  ///
  /// In ko, this message translates to:
  /// **'확인 중'**
  String get checking;

  /// No description provided for @settingUpVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 구성하고 있어요'**
  String get settingUpVirtualDisplay;

  /// No description provided for @updating2.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 중'**
  String get updating2;

  /// No description provided for @checkHost.
  ///
  /// In ko, this message translates to:
  /// **'Host 확인 필요'**
  String get checkHost;

  /// No description provided for @windowsStarting.
  ///
  /// In ko, this message translates to:
  /// **'Windows 시작 중'**
  String get windowsStarting;

  /// No description provided for @windowsStarting2.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 시작하고 있어요'**
  String get windowsStarting2;

  /// No description provided for @changesAutomaticallyWhenRestartFinishes.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작이 끝나면 자동으로 바뀌어요.'**
  String get changesAutomaticallyWhenRestartFinishes;

  /// No description provided for @changesAutomaticallyWhenStartupFinishes.
  ///
  /// In ko, this message translates to:
  /// **'시작이 끝나면 자동으로 바뀌어요.'**
  String get changesAutomaticallyWhenStartupFinishes;

  /// No description provided for @preparingConnection.
  ///
  /// In ko, this message translates to:
  /// **'연결을 준비하고 있어요'**
  String get preparingConnection;

  /// No description provided for @windowsHasStartedChangesAs.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 시작됐어요. Host가 연결되면 바로 바뀌어요.'**
  String get windowsHasStartedChangesAs;

  /// No description provided for @hostDisconnected.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결 끊김'**
  String get hostDisconnected;

  /// No description provided for @desktopRunningChangesAutomaticallyWhen.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱은 실행 중이에요. Host가 돌아오면 자동으로 바뀌어요.'**
  String get desktopRunningChangesAutomaticallyWhen;

  /// No description provided for @hostNotResponding.
  ///
  /// In ko, this message translates to:
  /// **'Host가 응답하지 않아요'**
  String get hostNotResponding;

  /// No description provided for @ifYouJustStartedWait.
  ///
  /// In ko, this message translates to:
  /// **'방금 켰다면 잠시 기다려 주세요. 계속되면 관리자에게 알려 주세요.'**
  String get ifYouJustStartedWait;

  /// No description provided for @unlockConsole.
  ///
  /// In ko, this message translates to:
  /// **'Console에서 잠금을 해제하세요.'**
  String get unlockConsole;

  /// No description provided for @ifSigningAutomaticallyYouCan.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 중이면 곧 연결할 수 있어요. 아니면 Console에서 로그인하세요.'**
  String get ifSigningAutomaticallyYouCan;

  /// No description provided for @signFromConsoleAfterRestart.
  ///
  /// In ko, this message translates to:
  /// **'Console에서 로그인하세요. 다시 시작하면 자동으로 로그인돼요.'**
  String get signFromConsoleAfterRestart;

  /// No description provided for @virtualDisplayNeedsCheckedHost.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정에서 가상 화면을 확인해야 해요. 관리자에게 알려 주세요.'**
  String get virtualDisplayNeedsCheckedHost;

  /// No description provided for @checkingStreamingReadiness.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 준비를 확인하고 있어요'**
  String get checkingStreamingReadiness;

  /// No description provided for @changesAsSoonAsHost.
  ///
  /// In ko, this message translates to:
  /// **'Host가 상태를 보고하면 바로 바뀌어요.'**
  String get changesAsSoonAsHost;

  /// No description provided for @waitingHostConnect2.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결을 기다리고 있어요'**
  String get waitingHostConnect2;

  /// No description provided for @checkingStatus.
  ///
  /// In ko, this message translates to:
  /// **'상태를 확인하고 있어요'**
  String get checkingStatus;

  /// No description provided for @hostConnectsOnceWindowsStarts.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 시작되면 Host가 연결돼요.'**
  String get hostConnectsOnceWindowsStarts;

  /// No description provided for @checkingAgainAutomaticallyMoment.
  ///
  /// In ko, this message translates to:
  /// **'잠시 후 자동으로 다시 확인해요.'**
  String get checkingAgainAutomaticallyMoment;

  /// No description provided for @power2.
  ///
  /// In ko, this message translates to:
  /// **'전원 켜기'**
  String get power2;

  /// No description provided for @preparing4.
  ///
  /// In ko, this message translates to:
  /// **'연결 준비'**
  String get preparing4;

  /// No description provided for @connecting.
  ///
  /// In ko, this message translates to:
  /// **'연결하는 중'**
  String get connecting;

  /// No description provided for @manageLocalPcHostUpdate.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC 관리 · Host 업데이트 있음'**
  String get manageLocalPcHostUpdate;

  /// No description provided for @manageLocalPc.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC 관리'**
  String get manageLocalPc;

  /// No description provided for @manageDesktopHostUpdateAvailable.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 관리 · Host 업데이트 있음'**
  String get manageDesktopHostUpdateAvailable;

  /// No description provided for @manageDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 관리'**
  String get manageDesktop;

  /// No description provided for @step3ElapsedDesktopStays.
  ///
  /// In ko, this message translates to:
  /// **'{progress}/3단계 · {clock} 경과 · 취소해도 데스크톱은 켜져 있어요'**
  String step3ElapsedDesktopStays(Object progress, Object clock);

  /// No description provided for @defaultStreaming.
  ///
  /// In ko, this message translates to:
  /// **'기본 연결: 스트리밍 · {mode}'**
  String defaultStreaming(Object mode);

  /// No description provided for @diagnostics.
  ///
  /// In ko, this message translates to:
  /// **'진단'**
  String get diagnostics;

  /// No description provided for @restart2.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작…'**
  String get restart2;

  /// No description provided for @disconnectConsoleConnect2.
  ///
  /// In ko, this message translates to:
  /// **'Console 끊고 연결'**
  String get disconnectConsoleConnect2;

  /// No description provided for @working.
  ///
  /// In ko, this message translates to:
  /// **'처리 중…'**
  String get working;

  /// No description provided for @preparing5.
  ///
  /// In ko, this message translates to:
  /// **'준비 중…'**
  String get preparing5;

  /// No description provided for @streamingConnection.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 연결'**
  String get streamingConnection;

  /// No description provided for @streamingFullScreen.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 · 전체 화면'**
  String get streamingFullScreen;

  /// No description provided for @streamingAllMonitors.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 · 모든 모니터'**
  String get streamingAllMonitors;

  /// No description provided for @startsDesktopYouCanConnect.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱을 켜요. 켜진 뒤에 연결할 수 있어요.'**
  String get startsDesktopYouCanConnect;

  /// No description provided for @streamingConnection2.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 연결 · {mode}'**
  String streamingConnection2(Object mode);

  /// No description provided for @connectionOptions.
  ///
  /// In ko, this message translates to:
  /// **'연결 옵션'**
  String get connectionOptions;

  /// No description provided for @otherWaysConnect.
  ///
  /// In ko, this message translates to:
  /// **'다른 연결 방식'**
  String get otherWaysConnect;

  /// No description provided for @connectsByStreamingAsSoon.
  ///
  /// In ko, this message translates to:
  /// **'준비되면 바로 스트리밍으로 연결해요'**
  String get connectsByStreamingAsSoon;

  /// No description provided for @qemuAgentOff.
  ///
  /// In ko, this message translates to:
  /// **'QEMU Agent 꺼짐'**
  String get qemuAgentOff;

  /// No description provided for @noSound.
  ///
  /// In ko, this message translates to:
  /// **'소리 없음'**
  String get noSound;

  /// No description provided for @screenFromMayDifferNow.
  ///
  /// In ko, this message translates to:
  /// **'{picture} {machine} 화면이에요. 지금과 다를 수 있어요.'**
  String screenFromMayDifferNow(Object picture, Object machine);

  /// No description provided for @lastSavedScreenScreenMay.
  ///
  /// In ko, this message translates to:
  /// **'저장된 마지막 화면이에요. 지금 {machine} 화면과 다를 수 있어요.'**
  String lastSavedScreenScreenMay(Object machine);

  /// No description provided for @noPreview.
  ///
  /// In ko, this message translates to:
  /// **'미리보기 없음'**
  String get noPreview;

  /// No description provided for @qemuGuestAgentOffProxmox.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox VM 옵션에서 QEMU Guest Agent가 꺼져 있어요.\n관리자가 켜면 Windows가 시작 중이거나 Host 연결이 끊겼을 때 상태를 더 정확히 보여 줘요.'**
  String get qemuGuestAgentOffProxmox;

  /// No description provided for @startedAgo.
  ///
  /// In ko, this message translates to:
  /// **'{uptime} 전에 켜졌어요'**
  String startedAgo(Object uptime);

  /// No description provided for @startedAgoAccordingProxmox.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 기준 {uptime} 전에 켜졌어요'**
  String startedAgoAccordingProxmox(Object uptime);

  /// No description provided for @statusDetails.
  ///
  /// In ko, this message translates to:
  /// **'상태 자세히'**
  String get statusDetails;

  /// No description provided for @unknown3.
  ///
  /// In ko, this message translates to:
  /// **'확인 못함'**
  String get unknown3;

  /// No description provided for @monitor.
  ///
  /// In ko, this message translates to:
  /// **'모니터 {o} {o2}'**
  String monitor(Object o, Object o2);

  /// No description provided for @couldNotFindRemoteScreen.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면을 찾지 못했어요.'**
  String get couldNotFindRemoteScreen;

  /// No description provided for @couldNotCheckWindowsScale.
  ///
  /// In ko, this message translates to:
  /// **'Windows 배율을 확인하지 못했어요.'**
  String get couldNotCheckWindowsScale;

  /// No description provided for @windowsDidNotAllowScale.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 배율 변경을 허용하지 않았어요.'**
  String get windowsDidNotAllowScale;

  /// No description provided for @couldNotSaveOriginalScale.
  ///
  /// In ko, this message translates to:
  /// **'원래 배율을 저장하지 못해 배율을 바꾸지 않았어요.'**
  String get couldNotSaveOriginalScale;

  /// No description provided for @remoteScreenDoesNotSupport.
  ///
  /// In ko, this message translates to:
  /// **'이 원격 화면은 Windows 배율 변경을 지원하지 않아요.'**
  String get remoteScreenDoesNotSupport;

  /// No description provided for @couldNotCheckResultScale.
  ///
  /// In ko, this message translates to:
  /// **'배율 변경 결과를 확인하지 못했어요.'**
  String get couldNotCheckResultScale;

  /// No description provided for @disconnected2.
  ///
  /// In ko, this message translates to:
  /// **'연결이 끊겼어요.'**
  String get disconnected2;

  /// No description provided for @youCannotChangeScaleConnection.
  ///
  /// In ko, this message translates to:
  /// **'이 연결에서는 배율을 바꿀 수 없어요.'**
  String get youCannotChangeScaleConnection;

  /// No description provided for @autoNow.
  ///
  /// In ko, this message translates to:
  /// **'자동 · 현재 {currentScale}%'**
  String autoNow(Object currentScale);

  /// No description provided for @autoCurrentScaleNotChecked.
  ///
  /// In ko, this message translates to:
  /// **'자동 · 현재 배율 확인 전'**
  String get autoCurrentScaleNotChecked;

  /// No description provided for @apply.
  ///
  /// In ko, this message translates to:
  /// **'적용'**
  String get apply;

  /// No description provided for @requested4.
  ///
  /// In ko, this message translates to:
  /// **' · 요청 {wanted}%'**
  String requested4(Object wanted);

  /// No description provided for @applied.
  ///
  /// In ko, this message translates to:
  /// **'적용 {currentScale}%{wanted}'**
  String applied(Object currentScale, Object wanted);

  /// No description provided for @requestedChecking.
  ///
  /// In ko, this message translates to:
  /// **'{wanted}% 요청 · 적용 확인 중'**
  String requestedChecking(Object wanted);

  /// No description provided for @currentScaleNotCheckedYet.
  ///
  /// In ko, this message translates to:
  /// **'현재 배율 확인 전'**
  String get currentScaleNotCheckedYet;

  /// No description provided for @resolutionAutoNow.
  ///
  /// In ko, this message translates to:
  /// **'해상도 자동 · 현재 {sourceWidth} × {sourceHeight}'**
  String resolutionAutoNow(Object sourceWidth, Object sourceHeight);

  /// No description provided for @resolutionAutoCurrentSizeNot.
  ///
  /// In ko, this message translates to:
  /// **'해상도 자동 · 현재 크기 확인 전'**
  String get resolutionAutoCurrentSizeNot;

  /// No description provided for @scale.
  ///
  /// In ko, this message translates to:
  /// **'배율'**
  String get scale;

  /// No description provided for @autoCurrentScaleUnknown.
  ///
  /// In ko, this message translates to:
  /// **'자동 · 현재 배율 확인 못함\n{scaleStatus}'**
  String autoCurrentScaleUnknown(Object scaleStatus);

  /// No description provided for @max.
  ///
  /// In ko, this message translates to:
  /// **'최대 · {o}'**
  String max(Object o);

  /// No description provided for @upScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'이 화면 크기에서 최대 {maximum}%'**
  String upScreenSize(Object maximum);

  /// No description provided for @folderAlreadyShared.
  ///
  /// In ko, this message translates to:
  /// **'이미 공유한 폴더예요.'**
  String get folderAlreadyShared;

  /// No description provided for @youCanShareUpFolders.
  ///
  /// In ko, this message translates to:
  /// **'폴더는 {driveMaxShares}개까지 공유할 수 있어요.'**
  String youCanShareUpFolders(Object driveMaxShares);

  /// No description provided for @couldNotFindUserSigned.
  ///
  /// In ko, this message translates to:
  /// **'PC에 로그인한 사용자를 확인하지 못해 공유하지 못했어요.\nWindows 탐색기가 실행 중인지 확인하세요.'**
  String get couldNotFindUserSigned;

  /// No description provided for @pcUserCannotOpenFolder.
  ///
  /// In ko, this message translates to:
  /// **'PC 사용자 권한으로 열 수 없는 폴더예요. 다른 폴더를 고르거나 폴더 권한을 확인해 주세요.'**
  String get pcUserCannotOpenFolder;

  /// No description provided for @couldNotFindFolderPc.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 폴더를 찾을 수 없어요. 이름을 바꿨거나 지웠거나, USB·네트워크 드라이브가 연결돼 있지 않을 수 있어요.'**
  String get couldNotFindFolderPc;

  /// No description provided for @youCanOnlyShareFolders.
  ///
  /// In ko, this message translates to:
  /// **'폴더만 공유할 수 있어요.'**
  String get youCanOnlyShareFolders;

  /// No description provided for @pathCannotShared.
  ///
  /// In ko, this message translates to:
  /// **'공유할 수 없는 경로예요.'**
  String get pathCannotShared;

  /// No description provided for @cannotOpenExplorerDesktopCheck.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱의 탐색기에서 열 수 없어요.\n데스크톱에 로그인돼 있는지 확인하세요.'**
  String get cannotOpenExplorerDesktopCheck;

  /// No description provided for @couldNotOpenFolderExplorer.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱의 탐색기에서 폴더를 열지 못했어요.\n‘열기’를 다시 누르세요.'**
  String get couldNotOpenFolderExplorer;

  /// No description provided for @couldNotCheckWhetherFolder.
  ///
  /// In ko, this message translates to:
  /// **'폴더가 열렸는지 확인하지 못했어요.\n데스크톱의 탐색기를 보고, 안 열렸으면 ‘열기’를 다시 누르세요.'**
  String get couldNotCheckWhetherFolder;

  /// No description provided for @sharingFolderHasEndedReconnecting.
  ///
  /// In ko, this message translates to:
  /// **'이 폴더의 공유가 끝났거나 다시 연결 중이에요.'**
  String get sharingFolderHasEndedReconnecting;

  /// No description provided for @tryAgainAfterOtherTask.
  ///
  /// In ko, this message translates to:
  /// **'다른 작업이 끝난 뒤 다시 시도하세요.'**
  String get tryAgainAfterOtherTask;

  /// No description provided for @sharedFoldersNotAvailableWindow.
  ///
  /// In ko, this message translates to:
  /// **'이 창에서는 공유 폴더를 쓸 수 없어요. 창을 닫고 다시 연결해 보세요.'**
  String get sharedFoldersNotAvailableWindow;

  /// No description provided for @webclientServiceDesktopOffMissing.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱의 WebClient 서비스가 꺼져 있거나 없어요.\n관리자에게 알려 주세요.'**
  String get webclientServiceDesktopOffMissing;

  /// No description provided for @desktopHasNoDriveLetters.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 남은 드라이브 문자가 없어요. 데스크톱에서 안 쓰는 드라이브 연결을 끊은 뒤 폴더를 다시 추가하세요.'**
  String get desktopHasNoDriveLetters;

  /// No description provided for @youNotSignedDesktopSign.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 로그인돼 있지 않아요. Windows에 로그인한 뒤 다시 연결하세요.'**
  String get youNotSignedDesktopSign;

  /// No description provided for @couldNotConnectDriveDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 드라이브를 연결하지 못했어요. 공유를 끝내고 폴더를 다시 추가하세요.'**
  String get couldNotConnectDriveDesktop;

  /// No description provided for @administratorTurnedOffSharedFolder.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 공유 폴더 권한을 해제했어요.'**
  String get administratorTurnedOffSharedFolder;

  /// No description provided for @couldNotShareFolder.
  ///
  /// In ko, this message translates to:
  /// **'폴더를 공유하지 못했어요.'**
  String get couldNotShareFolder;

  /// No description provided for @youAboutShareWholeDrive.
  ///
  /// In ko, this message translates to:
  /// **'드라이브 전체를 공유하려고 해요.\n데스크톱의 프로그램이 이 드라이브의 모든 파일을 읽고 바꿀 수 있어요.\n꼭 필요한 폴더만 고르는 걸 권해요.'**
  String get youAboutShareWholeDrive;

  /// No description provided for @windowsProgramFolderChangingDeleting.
  ///
  /// In ko, this message translates to:
  /// **'Windows·프로그램 폴더예요.\n데스크톱에서 이 폴더의 파일을 바꾸거나 지우면 PC가 제대로 동작하지 않을 수 있어요.'**
  String get windowsProgramFolderChangingDeleting;

  /// No description provided for @youAboutShareWholeAccount.
  ///
  /// In ko, this message translates to:
  /// **'내 계정 폴더 전체(또는 AppData)를 공유하려고 해요.\n로그인 정보와 프로그램 설정까지 데스크톱에서 보일 수 있어요.'**
  String get youAboutShareWholeAccount;

  /// No description provided for @networkFolderIfConnectionDrops.
  ///
  /// In ko, this message translates to:
  /// **'네트워크 폴더예요. 연결이 끊기면 데스크톱에서 쓸 수 없고, 파일을 여는 데 시간이 오래 걸릴 수 있어요.'**
  String get networkFolderIfConnectionDrops;

  /// No description provided for @foldersMoreThan20000.
  ///
  /// In ko, this message translates to:
  /// **'파일이 20,000개를 넘는 폴더는 데스크톱에서 처음 20,000개만 보여요. 폴더를 나눠 공유해 보세요.'**
  String get foldersMoreThan20000;

  /// No description provided for @untilDesktopRestartsFilesOver.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱을 다시 시작하기 전까지는 50 MB가 넘는 파일을 옮길 수 없어요.'**
  String get untilDesktopRestartsFilesOver;

  /// No description provided for @singleFileOver4Gb.
  ///
  /// In ko, this message translates to:
  /// **'파일 하나가 4 GB를 넘으면 공유 폴더로 옮길 수 없어요. 클립보드 복사·붙여넣기로 옮기세요.'**
  String get singleFileOver4Gb;

  /// No description provided for @cannotOpenFolderPcCheck.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 폴더를 열 수 없어요. USB·네트워크 연결을 확인하고 다시 시도하세요.'**
  String get cannotOpenFolderPcCheck;

  /// No description provided for @sharedFolderWasDisconnectedConnect.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 연결이 끊겼어요. 데스크톱에 다시 연결한 뒤 폴더를 다시 고르세요.'**
  String get sharedFolderWasDisconnectedConnect;

  /// No description provided for @sharedFoldersReconnectingWait.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더를 다시 연결하고 있어요. 잠시만 기다려 주세요.'**
  String get sharedFoldersReconnectingWait;

  /// No description provided for @wasDeletedSoSharingEnded.
  ///
  /// In ko, this message translates to:
  /// **'‘{name}’ 폴더가 삭제돼서 공유를 끝냈어요'**
  String wasDeletedSoSharingEnded(Object name);

  /// No description provided for @shareFolderDesktopAsDrive.
  ///
  /// In ko, this message translates to:
  /// **'이 폴더를 데스크톱에 드라이브로 공유할까요?'**
  String get shareFolderDesktopAsDrive;

  /// No description provided for @folderCautionYouCanTurn.
  ///
  /// In ko, this message translates to:
  /// **'폴더: {path}\n\n주의: {risk}'**
  String folderCautionYouCanTurn(Object path, Object risk);

  /// No description provided for @share.
  ///
  /// In ko, this message translates to:
  /// **'공유'**
  String get share;

  /// No description provided for @stopSharing.
  ///
  /// In ko, this message translates to:
  /// **'공유를 끝낼까요?'**
  String get stopSharing;

  /// No description provided for @programsDesktopHaveFilesFrom.
  ///
  /// In ko, this message translates to:
  /// **'‘{shownName}’ ({path})\n\n데스크톱에서 이 폴더의 파일을 열어 둔 프로그램은 저장하지 못할 수 있어요. 폴더와 파일은 {device}에 그대로 남아요.'**
  String programsDesktopHaveFilesFrom(
    Object shownName,
    Object path,
    Object device,
  );

  /// No description provided for @stopSharing2.
  ///
  /// In ko, this message translates to:
  /// **'공유 끝내기'**
  String get stopSharing2;

  /// No description provided for @reconnectingSharedFolders.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더를 다시 연결하고 있어요'**
  String get reconnectingSharedFolders;

  /// No description provided for @sharedFoldersDisconnected.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 연결이 끊겼어요'**
  String get sharedFoldersDisconnected;

  /// No description provided for @preparingSharedFolders.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더를 준비하고 있어요'**
  String get preparingSharedFolders;

  /// No description provided for @choosingFolder.
  ///
  /// In ko, this message translates to:
  /// **'폴더를 고르는 중이에요'**
  String get choosingFolder;

  /// No description provided for @youCanShareUpFolders2.
  ///
  /// In ko, this message translates to:
  /// **'폴더는 {driveMaxShares}개까지 공유할 수 있어요'**
  String youCanShareUpFolders2(Object driveMaxShares);

  /// No description provided for @desktopCanReadChangeFiles.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 공유한 폴더의 파일을 읽고 바꿀 수 있어요.'**
  String get desktopCanReadChangeFiles;

  /// No description provided for @shareFoldersFrom.
  ///
  /// In ko, this message translates to:
  /// **'{device} 폴더 공유'**
  String shareFoldersFrom(Object device);

  /// No description provided for @pcDeviceWord.
  ///
  /// In ko, this message translates to:
  /// **'PC'**
  String get pcDeviceWord;

  /// No description provided for @onlyFoldersYouChooseShared.
  ///
  /// In ko, this message translates to:
  /// **'고른 폴더만 이번 연결에서 공유해요.\n연결을 끊으면 공유도 끝나요.'**
  String get onlyFoldersYouChooseShared;

  /// No description provided for @noSharedFoldersYet.
  ///
  /// In ko, this message translates to:
  /// **'아직 공유한 폴더가 없어요.'**
  String get noSharedFoldersYet;

  /// No description provided for @preparingSharedFolders2.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더를 준비하고 있어요…'**
  String get preparingSharedFolders2;

  /// No description provided for @hideDetails.
  ///
  /// In ko, this message translates to:
  /// **'자세히 접기'**
  String get hideDetails;

  /// No description provided for @sharingLimits.
  ///
  /// In ko, this message translates to:
  /// **'공유 제한 자세히'**
  String get sharingLimits;

  /// No description provided for @openingFolderTakingWhilePlease.
  ///
  /// In ko, this message translates to:
  /// **'폴더를 여는 데 시간이 걸리고 있어요.\n잠시 기다려 주세요.'**
  String get openingFolderTakingWhilePlease;

  /// No description provided for @chooseFolderShare2.
  ///
  /// In ko, this message translates to:
  /// **'공유할 폴더를 {device}에서 골라요'**
  String chooseFolderShare2(Object device);

  /// No description provided for @addFolder.
  ///
  /// In ko, this message translates to:
  /// **'폴더 추가…'**
  String get addFolder;

  /// No description provided for @disconnected3.
  ///
  /// In ko, this message translates to:
  /// **'공유 끊김'**
  String get disconnected3;

  /// No description provided for @checking2.
  ///
  /// In ko, this message translates to:
  /// **'확인 중…'**
  String get checking2;

  /// No description provided for @connecting2.
  ///
  /// In ko, this message translates to:
  /// **'연결 중…'**
  String get connecting2;

  /// No description provided for @connected3.
  ///
  /// In ko, this message translates to:
  /// **'{letter}: 연결됨'**
  String connected3(Object letter);

  /// No description provided for @checkingWhetherOpened.
  ///
  /// In ko, this message translates to:
  /// **'열기 결과를 확인하고 있어요'**
  String get checkingWhetherOpened;

  /// No description provided for @youCanOpenOnceShared.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더가 연결되면 열 수 있어요'**
  String get youCanOpenOnceShared;

  /// No description provided for @opensSharedFolderExplorerDesktop.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더를 데스크톱의 탐색기에서 열어요'**
  String get opensSharedFolderExplorerDesktop;

  /// No description provided for @open.
  ///
  /// In ko, this message translates to:
  /// **'열기'**
  String get open;

  /// No description provided for @closeRemoteWindow.
  ///
  /// In ko, this message translates to:
  /// **'원격 창 닫기'**
  String get closeRemoteWindow;

  /// No description provided for @encoderApplied.
  ///
  /// In ko, this message translates to:
  /// **'적용했어요'**
  String get encoderApplied;

  /// No description provided for @couldNotConfirmSaveYet.
  ///
  /// In ko, this message translates to:
  /// **'적용 결과를 아직 확인하지 못했어요. 상태를 다시 확인하세요.'**
  String get couldNotConfirmSaveYet;

  /// No description provided for @couldNotStartSavingTry.
  ///
  /// In ko, this message translates to:
  /// **'적용을 시작하지 못했어요. 잠시 후 다시 시도하세요. 설정은 바꾸지 않았어요.'**
  String get couldNotStartSavingTry;

  /// No description provided for @saveResponseCurrentSettingsNeed.
  ///
  /// In ko, this message translates to:
  /// **'적용 응답과 현재 설정이 달라요. 다시 고르지 말고 상태를 새로 확인하세요.'**
  String get saveResponseCurrentSettingsNeed;

  /// No description provided for @encoderAuto.
  ///
  /// In ko, this message translates to:
  /// **'자동 (권장)'**
  String get encoderAuto;

  /// No description provided for @encoderAutoDetail.
  ///
  /// In ko, this message translates to:
  /// **'GPU가 있으면 GPU, 없으면 CPU'**
  String get encoderAutoDetail;

  /// No description provided for @encoderGpu.
  ///
  /// In ko, this message translates to:
  /// **'GPU'**
  String get encoderGpu;

  /// No description provided for @encoderGpuDetail.
  ///
  /// In ko, this message translates to:
  /// **'NVIDIA·AMD·Intel'**
  String get encoderGpuDetail;

  /// No description provided for @encoderX264.
  ///
  /// In ko, this message translates to:
  /// **'CPU (x264)'**
  String get encoderX264;

  /// No description provided for @encoderMft.
  ///
  /// In ko, this message translates to:
  /// **'CPU (Windows 기본)'**
  String get encoderMft;

  /// No description provided for @encoderWhichGpu.
  ///
  /// In ko, this message translates to:
  /// **'쓸 GPU'**
  String get encoderWhichGpu;

  /// No description provided for @encoderPickGpu.
  ///
  /// In ko, this message translates to:
  /// **'쓸 GPU를 고르세요.'**
  String get encoderPickGpu;

  /// No description provided for @encoderAdminOnly.
  ///
  /// In ko, this message translates to:
  /// **'관리자 권한으로 Host 설정을 열면 바꿀 수 있어요.'**
  String get encoderAdminOnly;

  /// No description provided for @encoderReloadedAfterConflict.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳에서 설정이 바뀌어 다시 불러왔어요. 다시 골라 주세요.'**
  String get encoderReloadedAfterConflict;

  /// No description provided for @encoderRecentLine.
  ///
  /// In ko, this message translates to:
  /// **'최근 확인: {label} ({time})'**
  String encoderRecentLine(Object label, Object time);

  /// No description provided for @hostEncoder.
  ///
  /// In ko, this message translates to:
  /// **'Host 인코더'**
  String get hostEncoder;

  /// No description provided for @chooseMethodNextStreamingConnection.
  ///
  /// In ko, this message translates to:
  /// **'고르면 바로 적용되고, 다음 스트리밍 연결부터 쓰여요.\n지금 연결은 끊기지 않고 인코더도 그대로예요.\nGPU가 시작되지 않거나 출력이 실패하면 CPU로 바꿔요.'**
  String get chooseMethodNextStreamingConnection;

  /// No description provided for @savingChoice.
  ///
  /// In ko, this message translates to:
  /// **'적용 중'**
  String get savingChoice;

  /// No description provided for @checkingCurrentChoiceGpuCandidates.
  ///
  /// In ko, this message translates to:
  /// **'현재 선택과 GPU 후보 확인 중'**
  String get checkingCurrentChoiceGpuCandidates;

  /// No description provided for @hostCannotReportEncoderChoice.
  ///
  /// In ko, this message translates to:
  /// **'이 Host에서 인코더 선택 상태를 확인할 수 없어요. Host를 업데이트하거나 새로고침하세요.'**
  String get hostCannotReportEncoderChoice;

  /// No description provided for @savedChoice.
  ///
  /// In ko, this message translates to:
  /// **'저장된 선택: {current}'**
  String savedChoice(Object current);

  /// No description provided for @triesGpuUsedCaptureEarlier.
  ///
  /// In ko, this message translates to:
  /// **'캡처에 사용한 GPU와 확인된 이전 후보를 먼저 시도해요.\n현재 해상도·H.264 출력이 확인되는 후보를 사용하고, 실패하면 CPU로 전환해요.'**
  String get triesGpuUsedCaptureEarlier;

  /// No description provided for @couldNotCheckGpuList.
  ///
  /// In ko, this message translates to:
  /// **'GPU 목록을 확인하지 못했어요. 상태를 다시 확인하세요.'**
  String get couldNotCheckGpuList;

  /// No description provided for @noConfirmedGpuCandidatesYou.
  ///
  /// In ko, this message translates to:
  /// **'확인된 GPU 후보가 없어요. CPU 방식은 그대로 사용할 수 있어요.'**
  String get noConfirmedGpuCandidatesYou;

  /// No description provided for @listedCandidateDoesNotMean.
  ///
  /// In ko, this message translates to:
  /// **'후보 표시는 인코딩 성공을 뜻하지 않아요. 연결 시 실제 지원과 출력을 확인해요.'**
  String get listedCandidateDoesNotMean;

  /// No description provided for @discardChanges.
  ///
  /// In ko, this message translates to:
  /// **'변경 취소'**
  String get discardChanges;

  /// No description provided for @encoderConfirmedRecordedTimeDoes.
  ///
  /// In ko, this message translates to:
  /// **'기록 시각에 확인된 인코더예요.\n현재 연결 상태를 뜻하지 않아요.'**
  String get encoderConfirmedRecordedTimeDoes;

  /// No description provided for @lastOutputLine.
  ///
  /// In ko, this message translates to:
  /// **'최근 출력: 화면 {screen} · {label} · {time}'**
  String lastOutputLine(Object screen, Object label, Object time);

  /// No description provided for @noOutputRecord.
  ///
  /// In ko, this message translates to:
  /// **'최근 확인: 아직 기록이 없어요'**
  String get noOutputRecord;

  /// No description provided for @noOutputRecordMeaning.
  ///
  /// In ko, this message translates to:
  /// **'기록이 없다고 GPU를 쓰지 않은 것은 아니에요.'**
  String get noOutputRecordMeaning;

  /// No description provided for @checkSaveStatus.
  ///
  /// In ko, this message translates to:
  /// **'저장 상태 확인'**
  String get checkSaveStatus;

  /// No description provided for @refreshStatus2.
  ///
  /// In ko, this message translates to:
  /// **'상태 새로고침'**
  String get refreshStatus2;

  /// No description provided for @gpuChoiceRefersAdapterCurrent.
  ///
  /// In ko, this message translates to:
  /// **'GPU는 현재 Windows의 어댑터를 선택해요. Windows를 다시 시작하거나 드라이버를 바꾼 뒤에는 다시 선택해야 할 수 있어요.\nCPU 기본값은 GPU가 없는 데스크톱에서도 유지돼요.'**
  String get gpuChoiceRefersAdapterCurrent;

  /// No description provided for @pendingApproval.
  ///
  /// In ko, this message translates to:
  /// **'승인 대기'**
  String get pendingApproval;

  /// No description provided for @cancelingRequest.
  ///
  /// In ko, this message translates to:
  /// **'요청 취소 중'**
  String get cancelingRequest;

  /// No description provided for @available2.
  ///
  /// In ko, this message translates to:
  /// **'확인 가능'**
  String get available2;

  /// No description provided for @overview.
  ///
  /// In ko, this message translates to:
  /// **'개요'**
  String get overview;

  /// No description provided for @diagnosticReport.
  ///
  /// In ko, this message translates to:
  /// **'진단 보고서'**
  String get diagnosticReport;

  /// No description provided for @devices.
  ///
  /// In ko, this message translates to:
  /// **'장치'**
  String get devices;

  /// No description provided for @needsAttention.
  ///
  /// In ko, this message translates to:
  /// **'{titles}, 확인 필요'**
  String needsAttention(Object titles);

  /// No description provided for @checkingHostStatus.
  ///
  /// In ko, this message translates to:
  /// **'Host 상태 확인 중'**
  String get checkingHostStatus;

  /// No description provided for @waitingWindowsRespond.
  ///
  /// In ko, this message translates to:
  /// **'Windows 응답을 기다리고 있어요.'**
  String get waitingWindowsRespond;

  /// No description provided for @checkEncoderSaveStatusFirst.
  ///
  /// In ko, this message translates to:
  /// **'인코더 저장 상태를 먼저 확인하세요.'**
  String get checkEncoderSaveStatusFirst;

  /// No description provided for @discardEncoderChoice.
  ///
  /// In ko, this message translates to:
  /// **'인코더 선택을 버릴까요?'**
  String get discardEncoderChoice;

  /// No description provided for @unsavedEncoderChoiceNotApplied.
  ///
  /// In ko, this message translates to:
  /// **'저장하지 않은 인코더 선택은 적용하지 않아요.'**
  String get unsavedEncoderChoiceNotApplied;

  /// No description provided for @discardChanges2.
  ///
  /// In ko, this message translates to:
  /// **'변경 버리기'**
  String get discardChanges2;

  /// No description provided for @savingEncoderChoice.
  ///
  /// In ko, this message translates to:
  /// **'인코더 선택 저장 중'**
  String get savingEncoderChoice;

  /// No description provided for @savingChoiceNextStreamingConnection.
  ///
  /// In ko, this message translates to:
  /// **'다음 스트리밍 연결부터 쓸 선택을 저장하고 있어요.'**
  String get savingChoiceNextStreamingConnection;

  /// No description provided for @checkEncoderSaveStatusBefore.
  ///
  /// In ko, this message translates to:
  /// **'인코더 저장 상태를 먼저 확인한 뒤 창을 닫아 주세요.'**
  String get checkEncoderSaveStatusBefore;

  /// No description provided for @closeWindowAfterSettingsTask.
  ///
  /// In ko, this message translates to:
  /// **'설정 작업이 끝난 뒤 창을 닫아 주세요.'**
  String get closeWindowAfterSettingsTask;

  /// No description provided for @discardUnsavedSettings.
  ///
  /// In ko, this message translates to:
  /// **'저장하지 않은 설정을 버릴까요?'**
  String get discardUnsavedSettings;

  /// No description provided for @serverAddressAccountDetailsEncoder.
  ///
  /// In ko, this message translates to:
  /// **'입력 중인 서버 주소, 계정 정보와 인코더 선택은 저장하지 않아요.'**
  String get serverAddressAccountDetailsEncoder;

  /// No description provided for @closeWithoutSaving.
  ///
  /// In ko, this message translates to:
  /// **'저장하지 않고 닫기'**
  String get closeWithoutSaving;

  /// No description provided for @taskStatusChangedSoWindow.
  ///
  /// In ko, this message translates to:
  /// **'작업 상태가 바뀌어 창을 닫지 않았어요. 상태를 확인한 뒤 다시 시도하세요.'**
  String get taskStatusChangedSoWindow;

  /// No description provided for @statusResponseDelayedCheckAgain.
  ///
  /// In ko, this message translates to:
  /// **'상태 응답이 지연되고 있어요. 잠시 후 다시 확인하세요.'**
  String get statusResponseDelayedCheckAgain;

  /// No description provided for @previousStatusCheckWasCanceled.
  ///
  /// In ko, this message translates to:
  /// **'이전 상태 조회를 취소했어요.'**
  String get previousStatusCheckWasCanceled;

  /// No description provided for @administratorApprovedServerRegistrationComplete.
  ///
  /// In ko, this message translates to:
  /// **'관리자 승인으로 서버 등록이 완료됐어요. 서비스와 가상 화면 상태를 확인하세요.'**
  String get administratorApprovedServerRegistrationComplete;

  /// No description provided for @checkingWindowsServiceStatus.
  ///
  /// In ko, this message translates to:
  /// **'Windows와 서비스 상태를 조회하고 있어요.'**
  String get checkingWindowsServiceStatus;

  /// No description provided for @hostServicePreparingVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 가상 화면과 서버 연결을 준비하고 있어요.'**
  String get hostServicePreparingVirtualDisplay;

  /// No description provided for @installingVirtualDisplayDriver.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버 설치 중'**
  String get installingVirtualDisplayDriver;

  /// No description provided for @checkingSignatureInstallingDriverScreen.
  ///
  /// In ko, this message translates to:
  /// **'서명을 확인하고 드라이버를 설치하고 있어요. 화면이 잠시 깜빡일 수 있어요. 창을 닫지 마세요.'**
  String get checkingSignatureInstallingDriverScreen;

  /// No description provided for @installingWinfsp.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 설치 중'**
  String get installingWinfsp;

  /// No description provided for @installingWinfspMayTake1.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp를 설치하고 있어요. 창을 닫지 마세요.'**
  String get installingWinfspMayTake1;

  /// No description provided for @couldNotCheckWinfspInstall.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 설치 결과를 확인하지 못했어요.\n상태를 새로고침해 확인하세요.'**
  String get couldNotCheckWinfspInstall;

  /// No description provided for @installationFinished.
  ///
  /// In ko, this message translates to:
  /// **'설치를 마쳤어요'**
  String get installationFinished;

  /// No description provided for @couldNotFinishInstallation.
  ///
  /// In ko, this message translates to:
  /// **'설치 마무리를 끝내지 못했어요'**
  String get couldNotFinishInstallation;

  /// No description provided for @continueLabel.
  ///
  /// In ko, this message translates to:
  /// **'계속'**
  String get continueLabel;

  /// No description provided for @requestSentWaitingWindowsRespond.
  ///
  /// In ko, this message translates to:
  /// **'요청을 전달했어요. Windows 응답을 기다리고 있어요.'**
  String get requestSentWaitingWindowsRespond;

  /// No description provided for @couldNotStartTaskOpen.
  ///
  /// In ko, this message translates to:
  /// **'작업을 시작하지 못했어요. Host 설정을 다시 연 뒤 시도하세요. 설정은 바꾸지 않았어요.'**
  String get couldNotStartTaskOpen;

  /// No description provided for @checkingChangedStatus.
  ///
  /// In ko, this message translates to:
  /// **'바뀐 상태를 확인하고 있어요.'**
  String get checkingChangedStatus;

  /// No description provided for @statusCheckStoppedRefreshCurrent.
  ///
  /// In ko, this message translates to:
  /// **'상태 조회가 중단됐어요. 현재 상태를 새로고침하세요.'**
  String get statusCheckStoppedRefreshCurrent;

  /// No description provided for @taskRespondedButChangedStatus.
  ///
  /// In ko, this message translates to:
  /// **'작업 응답은 받았지만 바뀐 상태를 읽지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.'**
  String get taskRespondedButChangedStatus;

  /// No description provided for @checkResult.
  ///
  /// In ko, this message translates to:
  /// **'결과 확인이 필요해요'**
  String get checkResult;

  /// No description provided for @done2.
  ///
  /// In ko, this message translates to:
  /// **'작업 완료'**
  String get done2;

  /// No description provided for @refreshStatusCheck.
  ///
  /// In ko, this message translates to:
  /// **'상태를 새로고침해 확인하세요.'**
  String get refreshStatusCheck;

  /// No description provided for @passwordsDoNotMatchEnter.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 일치하지 않아요. 다시 입력하세요.'**
  String get passwordsDoNotMatchEnter;

  /// No description provided for @checkFormatWindowsAccountPassword.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 또는 비밀번호 형식을 확인하세요.'**
  String get checkFormatWindowsAccountPassword;

  /// No description provided for @checkWindowsAccountPassword.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정과 비밀번호를 확인하세요.'**
  String get checkWindowsAccountPassword;

  /// No description provided for @windowsAccountLockedItsSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정이 잠겼거나 로그인이 제한돼 있어요. Windows에서 계정 상태를 확인하세요.'**
  String get windowsAccountLockedItsSign;

  /// No description provided for @windowsCouldNotCheckAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows에서 계정을 확인하지 못했어요. 잠시 후 다시 시도하세요.'**
  String get windowsCouldNotCheckAccount;

  /// No description provided for @couldNotSaveDeleteWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정을 저장하거나 삭제하지 못했어요. 상태를 새로고침해 확인하세요.'**
  String get couldNotSaveDeleteWindows;

  /// No description provided for @couldNotFinishSettingUp.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 설정을 완료하지 못했어요. 계정 정보와 현재 상태를 확인하세요.'**
  String get couldNotFinishSettingUp;

  /// No description provided for @couldNotFinishSettingUp2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 설정을 완료하지 못했어요.'**
  String get couldNotFinishSettingUp2;

  /// No description provided for @couldNotCheckVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 정리·설치 결과를 확인하지 못했어요. 실제 상태와 충돌 목록을 새로 확인하세요.'**
  String get couldNotCheckVirtualDisplay;

  /// No description provided for @useHttpAddressOpenSettings.
  ///
  /// In ko, this message translates to:
  /// **'HTTP 주소는 관리자 권한으로 설정을 열어야 쓸 수 있어요.'**
  String get useHttpAddressOpenSettings;

  /// No description provided for @testingServerConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 테스트 중'**
  String get testingServerConnection;

  /// No description provided for @serverChecked.
  ///
  /// In ko, this message translates to:
  /// **'서버를 확인했어요.'**
  String get serverChecked;

  /// No description provided for @changeServerConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결을 변경할까요?'**
  String get changeServerConnection;

  /// No description provided for @serverRequestOnceAdministratorApproves.
  ///
  /// In ko, this message translates to:
  /// **'요청할 서버: {origin}\n\n관리자가 승인하면 지금 등록을 새 서버로 바꿔요. 사용 중인 연결이 끊길 수 있어요.'**
  String serverRequestOnceAdministratorApproves(Object origin);

  /// No description provided for @requestChange.
  ///
  /// In ko, this message translates to:
  /// **'변경 요청'**
  String get requestChange;

  /// No description provided for @requestingServerConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결 요청 중'**
  String get requestingServerConnection;

  /// No description provided for @connectionRequestSentApproveAdmin.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청을 보냈어요. 관리자 웹에서 이 요청을 승인해 주세요.'**
  String get connectionRequestSentApproveAdmin;

  /// No description provided for @updateHostCancelRequestSafely.
  ///
  /// In ko, this message translates to:
  /// **'이 요청을 안전하게 취소하려면 Host를 업데이트하세요.'**
  String get updateHostCancelRequestSafely;

  /// No description provided for @editServerConnect.
  ///
  /// In ko, this message translates to:
  /// **'연결할 서버를 수정할까요?'**
  String get editServerConnect;

  /// No description provided for @cancelConnectionRequest.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청을 취소할까요?'**
  String get cancelConnectionRequest;

  /// No description provided for @cancelsCurrentApprovalRequestSo.
  ///
  /// In ko, this message translates to:
  /// **'현재 승인 요청을 취소한 뒤 서버 주소를 다시 입력해요. 기존에 등록된 서버 연결은 유지돼요.'**
  String get cancelsCurrentApprovalRequestSo;

  /// No description provided for @stopsWaitingAdministratorApprovalExisting.
  ///
  /// In ko, this message translates to:
  /// **'관리자 승인 대기를 중단해요. 기존에 등록된 서버 연결은 유지돼요.'**
  String get stopsWaitingAdministratorApprovalExisting;

  /// No description provided for @cancelEdit.
  ///
  /// In ko, this message translates to:
  /// **'취소 후 수정'**
  String get cancelEdit;

  /// No description provided for @cancelRequest.
  ///
  /// In ko, this message translates to:
  /// **'요청 취소'**
  String get cancelRequest;

  /// No description provided for @cancelingConnectionRequest.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청 취소 중'**
  String get cancelingConnectionRequest;

  /// No description provided for @connectionRequestWasCanceledCheck.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청 취소를 처리했어요. 새 요청 전 현재 상태를 확인하세요.'**
  String get connectionRequestWasCanceledCheck;

  /// No description provided for @hostUpdateNeeded.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트가 필요해요'**
  String get hostUpdateNeeded;

  /// No description provided for @startHostService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스를 시작할까요?'**
  String get startHostService;

  /// No description provided for @onceServiceStartsConnectsRegistered.
  ///
  /// In ko, this message translates to:
  /// **'서비스가 켜지면 등록된 서버에 자동으로 연결해요.'**
  String get onceServiceStartsConnectsRegistered;

  /// No description provided for @startService.
  ///
  /// In ko, this message translates to:
  /// **'서비스 시작'**
  String get startService;

  /// No description provided for @startingService.
  ///
  /// In ko, this message translates to:
  /// **'서비스 시작 중'**
  String get startingService;

  /// No description provided for @serviceHasStartedReconnectsServer.
  ///
  /// In ko, this message translates to:
  /// **'서비스를 시작했어요. 잠시 후 서버에 다시 연결해요.'**
  String get serviceHasStartedReconnectsServer;

  /// No description provided for @updateHostRestartSafelyCurrent.
  ///
  /// In ko, this message translates to:
  /// **'안전하게 다시 시작하려면 Host를 업데이트하세요. 기존 서비스는 계속 실행돼요.'**
  String get updateHostRestartSafelyCurrent;

  /// No description provided for @restartHostService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스를 다시 시작할까요?'**
  String get restartHostService;

  /// No description provided for @activeConnectionsDropBrieflyYou.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 연결이 잠깐 끊겨요. 서비스가 다시 켜지면 다시 연결할 수 있어요.\nWindows와 설정은 그대로예요.'**
  String get activeConnectionsDropBrieflyYou;

  /// No description provided for @restartingService.
  ///
  /// In ko, this message translates to:
  /// **'서비스 다시 시작 중'**
  String get restartingService;

  /// No description provided for @serviceHasRestartedReconnectsServer.
  ///
  /// In ko, this message translates to:
  /// **'서비스를 다시 시작했어요. 잠시 후 서버에 다시 연결해요.'**
  String get serviceHasRestartedReconnectsServer;

  /// No description provided for @restoreVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 복구할까요?'**
  String get restoreVirtualDisplay;

  /// No description provided for @setUpVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 설정할까요?'**
  String get setUpVirtualDisplay;

  /// No description provided for @restoresInterruptedScreenChangePrevious.
  ///
  /// In ko, this message translates to:
  /// **'중단된 화면 변경을 이전 설정으로 복구해요. 화면이 잠시 끊길 수 있어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 실행하세요.'**
  String get restoresInterruptedScreenChangePrevious;

  /// No description provided for @checksSignedDriverPreparesVirtual.
  ///
  /// In ko, this message translates to:
  /// **'서명된 드라이버를 확인하고 가상 화면을 준비해요. 화면이 잠시 끊기거나 Windows를 다시 시작해야 할 수 있어요. 자동으로 다시 시작하지 않아요.'**
  String get checksSignedDriverPreparesVirtual;

  /// No description provided for @restore.
  ///
  /// In ko, this message translates to:
  /// **'복구'**
  String get restore;

  /// No description provided for @settings.
  ///
  /// In ko, this message translates to:
  /// **'설정'**
  String get settings;

  /// No description provided for @virtualDisplayRequestDoneCheck.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 요청을 처리했어요. 켜진 모니터와 다시 시작 안내를 확인하세요.'**
  String get virtualDisplayRequestDoneCheck;

  /// No description provided for @currentSign.
  ///
  /// In ko, this message translates to:
  /// **'현재 로그인'**
  String get currentSign;

  /// No description provided for @activeConnections.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 연결'**
  String get activeConnections;

  /// No description provided for @screenTasksProgress.
  ///
  /// In ko, this message translates to:
  /// **'진행 중인 화면 작업'**
  String get screenTasksProgress;

  /// No description provided for @earlierVirtualMonitor.
  ///
  /// In ko, this message translates to:
  /// **'이전 가상 모니터'**
  String get earlierVirtualMonitor;

  /// No description provided for @proxmoxConsoleScreen.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔 화면'**
  String get proxmoxConsoleScreen;

  /// No description provided for @logBackup.
  ///
  /// In ko, this message translates to:
  /// **'기록 백업'**
  String get logBackup;

  /// No description provided for @virtualDisplayService.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 서비스'**
  String get virtualDisplayService;

  /// No description provided for @checked.
  ///
  /// In ko, this message translates to:
  /// **'확인됨'**
  String get checked;

  /// No description provided for @needsChecking.
  ///
  /// In ko, this message translates to:
  /// **'확인이 필요해요'**
  String get needsChecking;

  /// No description provided for @checkConsoleSign.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 로그인 확인'**
  String get checkConsoleSign;

  /// No description provided for @cannotRunFromRdpScreen.
  ///
  /// In ko, this message translates to:
  /// **'RDP 화면에서는 실행할 수 없어요'**
  String get cannotRunFromRdpScreen;

  /// No description provided for @runFromSignProxmoxConsole.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔(로컬 PC는 본체 화면) 로그인에서 실행하세요'**
  String get runFromSignProxmoxConsole;

  /// No description provided for @unlockWindows.
  ///
  /// In ko, this message translates to:
  /// **'Windows 잠금을 해제하세요'**
  String get unlockWindows;

  /// No description provided for @serviceNotRunning.
  ///
  /// In ko, this message translates to:
  /// **'서비스가 실행 중이 아니에요'**
  String get serviceNotRunning;

  /// No description provided for @anotherResetProgress.
  ///
  /// In ko, this message translates to:
  /// **'다른 초기화가 진행 중이에요'**
  String get anotherResetProgress;

  /// No description provided for @couldNotCheckServiceResponse.
  ///
  /// In ko, this message translates to:
  /// **'서비스 응답을 확인하지 못했어요'**
  String get couldNotCheckServiceResponse;

  /// No description provided for @noActiveConnections.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 연결 없음'**
  String get noActiveConnections;

  /// No description provided for @streamingConnected.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 연결 중'**
  String get streamingConnected;

  /// No description provided for @windowsRdpSignActive.
  ///
  /// In ko, this message translates to:
  /// **'Windows RDP 로그인이 활성 상태'**
  String get windowsRdpSignActive;

  /// No description provided for @consoleSwitchProgress.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 전환 작업 중'**
  String get consoleSwitchProgress;

  /// No description provided for @changingNumberMonitors.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수 변경 중'**
  String get changingNumberMonitors;

  /// No description provided for @automaticRestoreProgress.
  ///
  /// In ko, this message translates to:
  /// **'자동 복구 진행 중'**
  String get automaticRestoreProgress;

  /// No description provided for @waitingEarlierScreenTaskEnd.
  ///
  /// In ko, this message translates to:
  /// **'이전 화면 작업 종료 대기'**
  String get waitingEarlierScreenTaskEnd;

  /// No description provided for @resolutionMonitorChangeProgress.
  ///
  /// In ko, this message translates to:
  /// **'해상도·모니터 변경 진행 중'**
  String get resolutionMonitorChangeProgress;

  /// No description provided for @virtualMonitorUse.
  ///
  /// In ko, this message translates to:
  /// **'가상 모니터가 사용 중'**
  String get virtualMonitorUse;

  /// No description provided for @streamingScreenTaskProgress.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 화면 작업 진행 중'**
  String get streamingScreenTaskProgress;

  /// No description provided for @noneLeft.
  ///
  /// In ko, this message translates to:
  /// **'남아 있지 않음'**
  String get noneLeft;

  /// No description provided for @stillPresentDriver.
  ///
  /// In ko, this message translates to:
  /// **'드라이버에 아직 남아 있음'**
  String get stillPresentDriver;

  /// No description provided for @couldNotConnectDriver.
  ///
  /// In ko, this message translates to:
  /// **'드라이버에 연결하지 못함'**
  String get couldNotConnectDriver;

  /// No description provided for @couldNotReadVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 소유 정보를 읽지 못함'**
  String get couldNotReadVirtualDisplay;

  /// No description provided for @multitapVirtualDisplayDriverNot.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 가상 화면 드라이버가 선택되지 않음'**
  String get multitapVirtualDisplayDriverNot;

  /// No description provided for @onLabel4.
  ///
  /// In ko, this message translates to:
  /// **'켜져 있음'**
  String get onLabel4;

  /// No description provided for @turnedAgain.
  ///
  /// In ko, this message translates to:
  /// **'다시 켰음'**
  String get turnedAgain;

  /// No description provided for @anotherScreenNoConsoleId.
  ///
  /// In ko, this message translates to:
  /// **'다른 화면이 켜져 있음 (콘솔 식별 정보 없음)'**
  String get anotherScreenNoConsoleId;

  /// No description provided for @noScreen.
  ///
  /// In ko, this message translates to:
  /// **'켜진 화면이 없어요'**
  String get noScreen;

  /// No description provided for @multitapVirtualMonitorStill.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 가상 모니터가 아직 켜져 있음'**
  String get multitapVirtualMonitorStill;

  /// No description provided for @windowsRefusedTurnConsoleScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 콘솔 화면 켜기를 거부함'**
  String get windowsRefusedTurnConsoleScreen;

  /// No description provided for @couldNotConfirmAfterTurning.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 화면을 켠 뒤 확인하지 못함'**
  String get couldNotConfirmAfterTurning;

  /// No description provided for @couldNotReadScreenSetup.
  ///
  /// In ko, this message translates to:
  /// **'화면 구성을 읽지 못함'**
  String get couldNotReadScreenSetup;

  /// No description provided for @cannotCheckFromCurrentSign.
  ///
  /// In ko, this message translates to:
  /// **'현재 로그인에서 확인할 수 없음'**
  String get cannotCheckFromCurrentSign;

  /// No description provided for @movedBackupFolder.
  ///
  /// In ko, this message translates to:
  /// **'백업 폴더로 이동함'**
  String get movedBackupFolder;

  /// No description provided for @noLogsCleanUp.
  ///
  /// In ko, this message translates to:
  /// **'정리할 기록 없음'**
  String get noLogsCleanUp;

  /// No description provided for @couldNotCreateBackupFolder.
  ///
  /// In ko, this message translates to:
  /// **'백업 폴더를 만들지 못함'**
  String get couldNotCreateBackupFolder;

  /// No description provided for @onlyPartlyMoved.
  ///
  /// In ko, this message translates to:
  /// **'일부만 이동함'**
  String get onlyPartlyMoved;

  /// No description provided for @preparedAgain.
  ///
  /// In ko, this message translates to:
  /// **'다시 준비함'**
  String get preparedAgain;

  /// No description provided for @couldNotCheckResetResult.
  ///
  /// In ko, this message translates to:
  /// **'초기화 결과를 확인하지 못했어요. 상태를 새로고침하세요.'**
  String get couldNotCheckResetResult;

  /// No description provided for @text18.
  ///
  /// In ko, this message translates to:
  /// **' {moved}개를'**
  String text18(num moved);

  /// No description provided for @text19.
  ///
  /// In ko, this message translates to:
  /// **'을'**
  String get text19;

  /// No description provided for @backedUpRecoveryLogsPrepared.
  ///
  /// In ko, this message translates to:
  /// **'복구 기록{moved} 백업하고 가상 화면 서비스를 다시 준비했어요.\n새로고침해 가상 화면을 확인하세요.'**
  String backedUpRecoveryLogsPrepared(Object moved);

  /// No description provided for @nothingWasResetLogsScreens.
  ///
  /// In ko, this message translates to:
  /// **'초기화하지 않았어요.\n기록과 화면은 그대로예요.\n아래 항목을 해결한 뒤 다시 시도하세요.'**
  String get nothingWasResetLogsScreens;

  /// No description provided for @resetVirtualDisplayRecoveryLogs.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 복구 기록을 초기화할까요?'**
  String get resetVirtualDisplayRecoveryLogs;

  /// No description provided for @movesLogsEarlierStreamingScreens.
  ///
  /// In ko, this message translates to:
  /// **'자동 복구가 끝나지 않은 이전 스트리밍 화면 기록을 백업 폴더로 옮기고 가상 화면 서비스를 다시 준비해요.\n\n• 스트리밍 연결이나 화면 변경이 진행 중이면 실행하지 않아요.\n• 이전 Multitap 가상 모니터가 드라이버에 남아 있지 않은지 먼저 확인해요.\n• Proxmox 콘솔 화면이 꺼져 있으면 그 화면만 다시 켜요. 다른 화면·드라이버·보안 설정은 바꾸지 않아요.\n• 기록은 삭제하지 않고 백업해요. Windows를 다시 시작하지 않아요.\n\nProxmox 콘솔(로컬 PC는 본체 화면)의 Windows 로그인에서 실행하세요.'**
  String get movesLogsEarlierStreamingScreens;

  /// No description provided for @resetLogs.
  ///
  /// In ko, this message translates to:
  /// **'기록 초기화'**
  String get resetLogs;

  /// No description provided for @resettingVirtualDisplayRecoveryLogs.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 복구 기록 초기화 중'**
  String get resettingVirtualDisplayRecoveryLogs;

  /// No description provided for @virtualDisplayRecoveryLogsWere.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 복구 기록 초기화를 처리했어요.'**
  String get virtualDisplayRecoveryLogsWere;

  /// No description provided for @saveWorkRestartWindowsThen.
  ///
  /// In ko, this message translates to:
  /// **'작업을 저장하고 Windows를 다시 시작한 뒤 화면을 확인하세요.'**
  String get saveWorkRestartWindowsThen;

  /// No description provided for @anotherUserSignedConsoleTheir.
  ///
  /// In ko, this message translates to:
  /// **'콘솔에 다른 사용자가 로그인해 있어요. 해당 로그인을 유지하며 전환을 보류해요.'**
  String get anotherUserSignedConsoleTheir;

  /// No description provided for @signStatusChangedRefreshStatus.
  ///
  /// In ko, this message translates to:
  /// **'로그인 상태가 바뀌었어요. 상태를 새로고침한 뒤 다시 시도하세요.'**
  String get signStatusChangedRefreshStatus;

  /// No description provided for @openHostSettingsAsAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'관리자 권한으로 Host 설정을 열어 주세요.'**
  String get openHostSettingsAsAdministrator;

  /// No description provided for @currentSignAlreadyConnectedStreaming.
  ///
  /// In ko, this message translates to:
  /// **'현재 로그인이 스트리밍용 콘솔에 연결돼 있어요. 가상 화면 준비 상태를 확인하세요.'**
  String get currentSignAlreadyConnectedStreaming;

  /// No description provided for @virtualDisplayDeviceNotReady.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치가 준비되지 않았어요. 장치 상태를 먼저 확인하세요.'**
  String get virtualDisplayDeviceNotReady;

  /// No description provided for @windowsDidNotAllowSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 로그인 전환을 허용하지 않았어요. 현재 상태를 새로고침해 확인하세요.'**
  String get windowsDidNotAllowSign;

  /// No description provided for @couldNotCheckCurrentSign.
  ///
  /// In ko, this message translates to:
  /// **'현재 로그인과 콘솔 상태를 확인하지 못했어요. 새로고침한 뒤 다시 시도하세요.'**
  String get couldNotCheckCurrentSign;

  /// No description provided for @switchStreamingScreen.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 화면으로 전환할까요?'**
  String get switchStreamingScreen;

  /// No description provided for @movesCurrentWindowsSignStreaming.
  ///
  /// In ko, this message translates to:
  /// **'현재 Windows 로그인을 스트리밍 화면으로 옮겨요.\nRDP 연결이 끊길 수 있어요.\n전환 후 Multitap 클라이언트에서 스트리밍으로 연결하세요.'**
  String get movesCurrentWindowsSignStreaming;

  /// No description provided for @switchScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 전환'**
  String get switchScreen;

  /// No description provided for @switchingWindowsSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인 전환 중'**
  String get switchingWindowsSign;

  /// No description provided for @switchRequestedCheckWhetherVirtual.
  ///
  /// In ko, this message translates to:
  /// **'전환을 요청했어요. 가상 화면 준비 상태를 확인하세요.'**
  String get switchRequestedCheckWhetherVirtual;

  /// No description provided for @windowsSignSwitchConfirmedOnce.
  ///
  /// In ko, this message translates to:
  /// **'Windows 로그인 전환을 확인했어요. 가상 화면이 준비되면 클라이언트에서 스트리밍으로 연결하세요.'**
  String get windowsSignSwitchConfirmedOnce;

  /// No description provided for @switchRequestWasReceivedBut.
  ///
  /// In ko, this message translates to:
  /// **'전환 요청은 받았지만 완료를 확인하지 못했어요. 다시 전환하기 전에 상태를 새로고침하세요.'**
  String get switchRequestWasReceivedBut;

  /// No description provided for @checkingVirtualDisplayConflicts.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 충돌 항목 확인 중'**
  String get checkingVirtualDisplayConflicts;

  /// No description provided for @lookingUpExistingDevicesLeftover.
  ///
  /// In ko, this message translates to:
  /// **'기존 장치와 남아 있는 설정을 조회해요. 아직 삭제하지 않아요.'**
  String get lookingUpExistingDevicesLeftover;

  /// No description provided for @couldNotCheckConflictsRefresh.
  ///
  /// In ko, this message translates to:
  /// **'충돌 항목을 확인하지 못했어요. 상태를 새로고침하고 다시 확인하세요.'**
  String get couldNotCheckConflictsRefresh;

  /// No description provided for @leftoverSetting.
  ///
  /// In ko, this message translates to:
  /// **'남아 있는 설정'**
  String get leftoverSetting;

  /// No description provided for @impact.
  ///
  /// In ko, this message translates to:
  /// **'{kind} · {item}\n{location}\n영향: {item2}'**
  String impact(Object kind, Object item, Object location, Object item2);

  /// No description provided for @cleanUpCheckedConflictsInstall.
  ///
  /// In ko, this message translates to:
  /// **'확인한 충돌 항목을 정리하고 설치할까요?'**
  String get cleanUpCheckedConflictsInstall;

  /// No description provided for @allRequiredItemsShownCleaned.
  ///
  /// In ko, this message translates to:
  /// **'{description}\n\n표시된 필수 항목을 모두 정리하고 Multitap 드라이버를 설치해요.\n가상 화면이 끊기거나 Windows를 다시 시작해야 할 수 있어요.\n드라이버 보관함·기존 설정 파일은 유지하며, 자동으로 다시 시작하지 않아요.'**
  String allRequiredItemsShownCleaned(Object description);

  /// No description provided for @cleanUpInstall.
  ///
  /// In ko, this message translates to:
  /// **'정리 후 설치'**
  String get cleanUpInstall;

  /// No description provided for @cleaningUpConflictsInstallingMultitap.
  ///
  /// In ko, this message translates to:
  /// **'충돌 항목 정리·Multitap 드라이버 설치 중'**
  String get cleaningUpConflictsInstallingMultitap;

  /// No description provided for @operationTimedOutCheckActual.
  ///
  /// In ko, this message translates to:
  /// **'작업 응답 시간이 지났어요. 실제 상태와 충돌 목록을 확인한 뒤 다시 시도하세요.'**
  String get operationTimedOutCheckActual;

  /// No description provided for @devicesSettingsChangedSoCleanup.
  ///
  /// In ko, this message translates to:
  /// **'장치 또는 설정이 변경되어 정리를 시작하지 않았어요. 충돌 목록을 새로 확인하세요.'**
  String get devicesSettingsChangedSoCleanup;

  /// No description provided for @deviceRemovalRequestWasSent.
  ///
  /// In ko, this message translates to:
  /// **'장치 제거 요청은 전달했지만 실제 제거를 확인하지 못했어요. 장치 상태와 충돌 목록을 확인하세요.'**
  String get deviceRemovalRequestWasSent;

  /// No description provided for @onlySomeItemsWereCleaned.
  ///
  /// In ko, this message translates to:
  /// **'일부 항목만 정리됐어요. 충돌 목록을 다시 확인한 뒤 조치하세요.'**
  String get onlySomeItemsWereCleaned;

  /// No description provided for @conflictsWereCleanedUpBut.
  ///
  /// In ko, this message translates to:
  /// **'충돌 항목은 정리했지만 드라이버 설치를 완료하지 못했어요. 현재 상태와 진단 정보를 확인하세요.'**
  String get conflictsWereCleanedUpBut;

  /// No description provided for @cleanupInstallDidNotFinish.
  ///
  /// In ko, this message translates to:
  /// **'충돌 항목 정리·설치를 완료하지 못했어요. 현재 상태와 충돌 목록을 다시 확인하세요.'**
  String get cleanupInstallDidNotFinish;

  /// No description provided for @driverInstalledSaveWorkRestart.
  ///
  /// In ko, this message translates to:
  /// **'드라이버를 설치했어요. 작업을 저장하고 Windows를 다시 시작하세요. 자동으로 다시 시작하지 않아요.'**
  String get driverInstalledSaveWorkRestart;

  /// No description provided for @driverInstalledSignWindowsFrom.
  ///
  /// In ko, this message translates to:
  /// **'드라이버를 설치했어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 Windows에 로그인한 뒤 가상 화면 설정을 완료하세요.'**
  String get driverInstalledSignWindowsFrom;

  /// No description provided for @driverInstalledCheckNewVirtual.
  ///
  /// In ko, this message translates to:
  /// **'드라이버를 설치했어요. 새로 확인된 가상 화면 상태를 확인하세요.'**
  String get driverInstalledCheckNewVirtual;

  /// No description provided for @enterWindowsAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정을 입력하세요.'**
  String get enterWindowsAccount;

  /// No description provided for @enterNewPassword.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호를 입력하세요.'**
  String get enterNewPassword;

  /// No description provided for @enterWindowsPassword.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호를 입력하세요.'**
  String get enterWindowsPassword;

  /// No description provided for @passwordTooLong.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 너무 길어요.'**
  String get passwordTooLong;

  /// No description provided for @useLeastCharacters.
  ///
  /// In ko, this message translates to:
  /// **'{minimum}자 이상으로 만들어 주세요.'**
  String useLeastCharacters(Object minimum);

  /// No description provided for @passwordsDoNotMatch.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 일치하지 않아요.'**
  String get passwordsDoNotMatch;

  /// No description provided for @createNewPassword.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호를 만들까요?'**
  String get createNewPassword;

  /// No description provided for @saveWindowsAccountTurnAuto.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정을 저장하고 자동 로그인을 켤까요?'**
  String get saveWindowsAccountTurnAuto;

  /// No description provided for @saveWindowsAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정을 저장할까요?'**
  String get saveWindowsAccount;

  /// No description provided for @windowsAccount2.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정: {username}'**
  String windowsAccount2(Object username);

  /// No description provided for @accountGetsWindowsPasswordNeeded.
  ///
  /// In ko, this message translates to:
  /// **'이 계정에 Windows 비밀번호가 생겨요. 다음 로그인부터 이 비밀번호가 필요해요.'**
  String get accountGetsWindowsPasswordNeeded;

  /// No description provided for @thenAccountSavedAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'그다음 계정을 저장하고 자동 로그인을 켜요.'**
  String get thenAccountSavedAutoSign;

  /// No description provided for @streamingConnectsWindowsSignedAccount.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍은 이 계정으로 로그인된 Windows에 연결해요.'**
  String get streamingConnectsWindowsSignedAccount;

  /// No description provided for @whenWindowsStartsSignsAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 시작되면 이 계정으로 자동 로그인해요. 잠금 해제는 별도예요.'**
  String get whenWindowsStartsSignsAccount;

  /// No description provided for @hostTakesOverExistingAuto.
  ///
  /// In ko, this message translates to:
  /// **'기존 자동 로그인 설정은 이 Host가 넘겨받아요. Windows 레지스트리에 남은 비밀번호가 있으면 삭제되며 복원할 수 없어요.'**
  String get hostTakesOverExistingAuto;

  /// No description provided for @passwordStoredOnlyHostsProtected.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호는 이 Host의 보호 저장소에만 저장해요. 로컬 관리자는 접근할 수 있어요.'**
  String get passwordStoredOnlyHostsProtected;

  /// No description provided for @windowsAccountPasswordsSecurityPolicies.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 비밀번호와 보안 정책은 바꾸지 않아요.'**
  String get windowsAccountPasswordsSecurityPolicies;

  /// No description provided for @createPassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 만들기'**
  String get createPassword;

  /// No description provided for @saveTurn.
  ///
  /// In ko, this message translates to:
  /// **'저장하고 켜기'**
  String get saveTurn;

  /// No description provided for @windowsPasswordCreatedNeededFrom.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호: 만들었어요. 다음 로그인부터 필요해요.'**
  String get windowsPasswordCreatedNeededFrom;

  /// No description provided for @windowsPasswordResultNotConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호: 결과를 확인하지 못했어요. 비밀번호 칸을 비우고 다시 저장해 확인하세요.'**
  String get windowsPasswordResultNotConfirmed;

  /// No description provided for @windowsPasswordNotCreatedReopen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호: 만들지 않았어요. Host 설정을 다시 연 뒤 시도하세요.'**
  String get windowsPasswordNotCreatedReopen;

  /// No description provided for @windowsPasswordNotCreated.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호: 만들지 못했어요. {refused}'**
  String windowsPasswordNotCreated(Object refused);

  /// No description provided for @windowsAccountNotSaved.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정: 저장하지 않았어요.'**
  String get windowsAccountNotSaved;

  /// No description provided for @autoSignNotChanged.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인: 바꾸지 않았어요.'**
  String get autoSignNotChanged;

  /// No description provided for @checkingSavingWindowsAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 확인·저장 중'**
  String get checkingSavingWindowsAccount;

  /// No description provided for @windowsAccountCheckedSaved.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정을 확인하고 저장했어요.'**
  String get windowsAccountCheckedSaved;

  /// No description provided for @windowsAccountNotSaved2.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정: 저장하지 못했어요. {value}'**
  String windowsAccountNotSaved2(Object value);

  /// No description provided for @settingsChangedDuringCheckCheck.
  ///
  /// In ko, this message translates to:
  /// **'확인하는 동안 설정이 바뀌었어요. 상태를 확인한 뒤 계정을 다시 저장하세요.'**
  String get settingsChangedDuringCheckCheck;

  /// No description provided for @notChangedRefreshStatusCheck.
  ///
  /// In ko, this message translates to:
  /// **'바뀌지 않았어요. 상태를 새로고침해 확인하세요.'**
  String get notChangedRefreshStatusCheck;

  /// No description provided for @windowsAccountSaved.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정: 저장했어요.'**
  String get windowsAccountSaved;

  /// No description provided for @autoSign.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인: {autologon}'**
  String autoSign(Object autologon);

  /// No description provided for @creatingWindowsPassword.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호 만드는 중'**
  String get creatingWindowsPassword;

  /// No description provided for @checkingWindowsAccount.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 확인 중'**
  String get checkingWindowsAccount;

  /// No description provided for @checkingWhetherAccountHasPassword.
  ///
  /// In ko, this message translates to:
  /// **'이 계정에 비밀번호가 있는지 확인하고 있어요.'**
  String get checkingWhetherAccountHasPassword;

  /// No description provided for @couldNotConfirmAccountHas.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 없는지 확인하지 못했어요. 비밀번호를 입력하세요.'**
  String get couldNotConfirmAccountHas;

  /// No description provided for @accountAlreadyHasPasswordEnter.
  ///
  /// In ko, this message translates to:
  /// **'이 계정에는 이미 비밀번호가 있어요. 그 비밀번호를 입력하세요.'**
  String get accountAlreadyHasPasswordEnter;

  /// No description provided for @doesNotMeetWindowsPassword.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호 규칙에 맞지 않아요. 더 길게, 대·소문자·숫자·기호를 섞어 주세요.'**
  String get doesNotMeetWindowsPassword;

  /// No description provided for @couldNotConfirmAccountHas2.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 없는지 확인하지 못했어요.'**
  String get couldNotConfirmAccountHas2;

  /// No description provided for @windowsRefusedChange.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 변경을 거부했어요.'**
  String get windowsRefusedChange;

  /// No description provided for @checkPasswordFormat.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 형식을 확인하세요.'**
  String get checkPasswordFormat;

  /// No description provided for @notLocalAccountWindowsEnter.
  ///
  /// In ko, this message translates to:
  /// **'이 Windows의 로컬 계정이 아니에요. 비밀번호를 입력하세요.'**
  String get notLocalAccountWindowsEnter;

  /// No description provided for @accountDoesNotExistWindows.
  ///
  /// In ko, this message translates to:
  /// **'이 Windows에 없는 계정이에요. 계정 이름을 확인하세요.'**
  String get accountDoesNotExistWindows;

  /// No description provided for @accountBlockedCheckAccountStatus.
  ///
  /// In ko, this message translates to:
  /// **'사용이 막힌 계정이에요. Windows에서 계정 상태를 확인하세요.'**
  String get accountBlockedCheckAccountStatus;

  /// No description provided for @accountChangedDuringCheckSave.
  ///
  /// In ko, this message translates to:
  /// **'확인 중 계정이 바뀌었어요. 다시 저장하세요.'**
  String get accountChangedDuringCheckSave;

  /// No description provided for @couldNotCheckAccountTry.
  ///
  /// In ko, this message translates to:
  /// **'계정을 확인하지 못했어요. 잠시 후 다시 시도하세요.'**
  String get couldNotCheckAccountTry;

  /// No description provided for @hostDoesNotSupportAuto.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 자동 로그인 설정을 지원하지 않아요. 계정만 저장해요.'**
  String get hostDoesNotSupportAuto;

  /// No description provided for @noAdministratorRightsSoAuto.
  ///
  /// In ko, this message translates to:
  /// **'관리자 권한이 없어 자동 로그인은 바꾸지 않아요.'**
  String get noAdministratorRightsSoAuto;

  /// No description provided for @windowsPolicyPreventsAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 정책으로 자동 로그인을 켤 수 없어요. 정책은 바꾸지 않아요.'**
  String get windowsPolicyPreventsAutoSign;

  /// No description provided for @autoSignSettingAlreadyExists.
  ///
  /// In ko, this message translates to:
  /// **'기존 자동 로그인 설정이 있어요. 이 Host 버전은 넘겨받을 수 없어요.'**
  String get autoSignSettingAlreadyExists;

  /// No description provided for @previousChangeMustRecoveredFirst.
  ///
  /// In ko, this message translates to:
  /// **'이전 변경의 복구가 먼저 필요해요. 계정만 저장해요.'**
  String get previousChangeMustRecoveredFirst;

  /// No description provided for @autoSignCannotChangedRight.
  ///
  /// In ko, this message translates to:
  /// **'지금은 자동 로그인을 바꿀 수 없어요. 상태를 확인하세요.'**
  String get autoSignCannotChangedRight;

  /// No description provided for @updateHostClearSavedInfo2.
  ///
  /// In ko, this message translates to:
  /// **'저장 정보를 안전하게 지우려면 Host를 업데이트하세요.'**
  String get updateHostClearSavedInfo2;

  /// No description provided for @clearWindowsSignInfoSaved.
  ///
  /// In ko, this message translates to:
  /// **'Host에 저장된 Windows 로그인 정보를 지울까요?'**
  String get clearWindowsSignInfoSaved;

  /// No description provided for @onlyHostsSavedInfoCleared.
  ///
  /// In ko, this message translates to:
  /// **'{username}의 Host 저장 정보만 지워요.\nWindows 사용자와 기존 자동 로그인 설정은 유지해요.'**
  String onlyHostsSavedInfoCleared(Object username);

  /// No description provided for @clearSavedInfo.
  ///
  /// In ko, this message translates to:
  /// **'저장 정보 지우기'**
  String get clearSavedInfo;

  /// No description provided for @savedInfoChanged.
  ///
  /// In ko, this message translates to:
  /// **'저장 정보가 바뀌었어요'**
  String get savedInfoChanged;

  /// No description provided for @checkNewStatusThenChoose.
  ///
  /// In ko, this message translates to:
  /// **'새 상태를 확인한 뒤 다시 선택하세요.'**
  String get checkNewStatusThenChoose;

  /// No description provided for @clearingSignInfoSavedHost.
  ///
  /// In ko, this message translates to:
  /// **'Host 저장 로그인 정보 지우는 중'**
  String get clearingSignInfoSavedHost;

  /// No description provided for @windowsSignInfoSavedHost.
  ///
  /// In ko, this message translates to:
  /// **'Host에 저장된 Windows 로그인 정보를 지웠어요.'**
  String get windowsSignInfoSavedHost;

  /// No description provided for @checkingVirtualDisplayStatus.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 상태를 확인하고 있어요.'**
  String get checkingVirtualDisplayStatus;

  /// No description provided for @noVirtualDisplayDriverYet.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버가 아직 없어요. 설치 마무리를 눌러 설치하세요.'**
  String get noVirtualDisplayDriverYet;

  /// No description provided for @screenRecoveryFromLastStreaming.
  ///
  /// In ko, this message translates to:
  /// **'이전 스트리밍의 화면 복구가 끝나지 않았어요. 계속되면 아래에서 복구 기록을 초기화하세요.'**
  String get screenRecoveryFromLastStreaming;

  /// No description provided for @multitapVirtualDisplayAvailableCurrent.
  ///
  /// In ko, this message translates to:
  /// **'현재 Windows 로그인에서 Multitap 가상 화면을 사용할 수 있어요.'**
  String get multitapVirtualDisplayAvailableCurrent;

  /// No description provided for @youSignedViaRdpSwitch.
  ///
  /// In ko, this message translates to:
  /// **'현재 RDP 로그인이에요. 아래에서 스트리밍 화면으로 전환하세요.'**
  String get youSignedViaRdpSwitch;

  /// No description provided for @servicePreparingVirtualDisplayPlease.
  ///
  /// In ko, this message translates to:
  /// **'서비스가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.'**
  String get servicePreparingVirtualDisplayPlease;

  /// No description provided for @previousScreenChangeWasInterrupted.
  ///
  /// In ko, this message translates to:
  /// **'이전 화면 변경이 중단됐어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 복구를 실행하세요.'**
  String get previousScreenChangeWasInterrupted;

  /// No description provided for @anotherVirtualDisplayDevicePresent.
  ///
  /// In ko, this message translates to:
  /// **'다른 가상 화면 장치가 있어요. 그 장치는 바꾸지 않아요. 아래에서 기존 장치를 확인하세요.'**
  String get anotherVirtualDisplayDevicePresent;

  /// No description provided for @signWindowsFromProxmoxConsole.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔(로컬 PC는 본체 화면)에서 Windows에 로그인한 뒤 가상 화면 설정을 실행하세요.'**
  String get signWindowsFromProxmoxConsole;

  /// No description provided for @runVirtualDisplaySetupPrepare.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 설정을 실행해 모니터를 준비하세요.'**
  String get runVirtualDisplaySetupPrepare;

  /// No description provided for @virtualDisplayDeviceDidNot.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치가 시작되지 않았어요. 설정을 다시 적용하고 Windows 장치 상태를 확인하세요.'**
  String get virtualDisplayDeviceDidNot;

  /// No description provided for @noVirtualDisplayRunVirtual.
  ///
  /// In ko, this message translates to:
  /// **'켜진 가상 화면이 없어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 설정을 실행하세요.'**
  String get noVirtualDisplayRunVirtual;

  /// No description provided for @virtualDisplaySetupNeedsAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 설정에 관리자 권한이 필요해요.'**
  String get virtualDisplaySetupNeedsAdministrator;

  /// No description provided for @windowsCouldNotFinishPreparing.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 가상 화면 드라이버 준비를 완료하지 못했어요. 진단 정보를 복사해 관리자에게 알려 주세요.'**
  String get windowsCouldNotFinishPreparing;

  /// No description provided for @existingVirtualDisplaySetupNeeds.
  ///
  /// In ko, this message translates to:
  /// **'기존 가상 화면 설정 확인이 필요해요. 진단 정보를 복사해 관리자에게 알려 주세요. 기존 장치는 변경하지 않았어요.'**
  String get existingVirtualDisplaySetupNeeds;

  /// No description provided for @virtualDisplayDeviceOwnershipDuplicate.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치의 소유권 또는 중복 설치 확인이 필요해요. 다른 제품의 장치는 변경하지 않았어요.'**
  String get virtualDisplayDeviceOwnershipDuplicate;

  /// No description provided for @couldNotVerifyDriverFiles.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 파일을 확인하지 못했어요. Host 설치 파일을 다시 받아 실행하세요. Windows 보안 설정은 바꾸지 마세요.'**
  String get couldNotVerifyDriverFiles;

  /// No description provided for @couldNotVerifyVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 구성 파일을 확인하지 못했어요. Host 설치 파일을 다시 받아 실행하세요.'**
  String get couldNotVerifyVirtualDisplay;

  /// No description provided for @couldNotSaveVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치 설정을 저장하지 못했어요. 관리자 권한과 Windows 장치 상태를 확인하세요.'**
  String get couldNotSaveVirtualDisplay;

  /// No description provided for @previousScreenChangeNeedsRecovery.
  ///
  /// In ko, this message translates to:
  /// **'이전 화면 변경의 복구가 필요해요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 복구를 실행하세요.'**
  String get previousScreenChangeNeedsRecovery;

  /// No description provided for @hostServiceManagesVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'이 가상 화면은 Host 서비스가 관리해요. 상태를 새로고침해 현재 준비 단계를 확인하세요.'**
  String get hostServiceManagesVirtualDisplay;

  /// No description provided for @couldNotReadVirtualDisplay2.
  ///
  /// In ko, this message translates to:
  /// **'현재 서비스의 가상 화면 상태를 읽지 못했어요. 서비스 상태와 진단 정보를 확인하세요.'**
  String get couldNotReadVirtualDisplay2;

  /// No description provided for @couldNotCheckCurrentDisplay.
  ///
  /// In ko, this message translates to:
  /// **'현재 화면 배치를 확인하지 못했어요. 상태를 새로고침하고 계속되면 진단 정보를 확인하세요.'**
  String get couldNotCheckCurrentDisplay;

  /// No description provided for @youSignedViaRdpSwitch2.
  ///
  /// In ko, this message translates to:
  /// **'현재 RDP 로그인이에요. 스트리밍 화면으로 전환하거나 Proxmox 콘솔(로컬 PC는 본체 화면)에서 같은 Windows 계정으로 로그인하세요.'**
  String get youSignedViaRdpSwitch2;

  /// No description provided for @couldNotCheckVirtualDisplay2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 상태를 확인하지 못했어요. 진단 정보를 복사해 관리자에게 알려 주세요.'**
  String get couldNotCheckVirtualDisplay2;

  /// No description provided for @hostInfo.
  ///
  /// In ko, this message translates to:
  /// **'Host 정보'**
  String get hostInfo;

  /// No description provided for @hostToolsVersion.
  ///
  /// In ko, this message translates to:
  /// **'Host 도구 버전'**
  String get hostToolsVersion;

  /// No description provided for @availableWhenOperationFinishes.
  ///
  /// In ko, this message translates to:
  /// **'작업이 끝나면 확인할 수 있어요.'**
  String get availableWhenOperationFinishes;

  /// No description provided for @availableAfterRegisteringServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에 등록하면 확인할 수 있어요.'**
  String get availableAfterRegisteringServer;

  /// No description provided for @update2.
  ///
  /// In ko, this message translates to:
  /// **'{newer} 업데이트'**
  String update2(Object newer);

  /// No description provided for @connectSoundOutputDeviceTurn.
  ///
  /// In ko, this message translates to:
  /// **'소리 출력 장치를 연결하거나 Windows에서 켜세요.'**
  String get connectSoundOutputDeviceTurn;

  /// No description provided for @proxmoxConsoleLocalPcScreen.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔(로컬 PC는 본체 화면)'**
  String get proxmoxConsoleLocalPcScreen;

  /// No description provided for @localPcScreen.
  ///
  /// In ko, this message translates to:
  /// **'본체 화면'**
  String get localPcScreen;

  /// No description provided for @consoleLocalPcScreen.
  ///
  /// In ko, this message translates to:
  /// **'Console(로컬 PC는 본체 화면)'**
  String get consoleLocalPcScreen;

  /// No description provided for @proxmoxConsole.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔'**
  String get proxmoxConsole;

  /// No description provided for @checkVirtualDisplay2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 확인 필요'**
  String get checkVirtualDisplay2;

  /// No description provided for @notRegistered2.
  ///
  /// In ko, this message translates to:
  /// **'등록 안 됨'**
  String get notRegistered2;

  /// No description provided for @desktop4.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱'**
  String get desktop4;

  /// No description provided for @readyStream.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 가능'**
  String get readyStream;

  /// No description provided for @checkingStatus2.
  ///
  /// In ko, this message translates to:
  /// **'상태 확인 중'**
  String get checkingStatus2;

  /// No description provided for @checkingHostStatus2.
  ///
  /// In ko, this message translates to:
  /// **'Host 상태를 확인하고 있어요'**
  String get checkingHostStatus2;

  /// No description provided for @notRegisteredServerYet.
  ///
  /// In ko, this message translates to:
  /// **'아직 서버에 등록하지 않았어요'**
  String get notRegisteredServerYet;

  /// No description provided for @components.
  ///
  /// In ko, this message translates to:
  /// **'구성 요소'**
  String get components;

  /// No description provided for @checking3.
  ///
  /// In ko, this message translates to:
  /// **'확인하고 있어요'**
  String get checking3;

  /// No description provided for @signedOutLocked.
  ///
  /// In ko, this message translates to:
  /// **'로그인돼 있지 않거나 잠겨 있어요'**
  String get signedOutLocked;

  /// No description provided for @accountAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'{accountLabel} 계정 · 자동 로그인 켜짐'**
  String accountAutoSign(Object accountLabel);

  /// No description provided for @noSavedWindowsAccount.
  ///
  /// In ko, this message translates to:
  /// **'저장된 Windows 계정이 없어요'**
  String get noSavedWindowsAccount;

  /// No description provided for @autoSignAccountDiffersFrom.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 계정과 저장된 계정이 달라요'**
  String get autoSignAccountDiffersFrom;

  /// No description provided for @ready4.
  ///
  /// In ko, this message translates to:
  /// **'가상 모니터 {monitors}대 · 준비됨'**
  String ready4(num monitors);

  /// No description provided for @noDriverYet.
  ///
  /// In ko, this message translates to:
  /// **'드라이버가 아직 없어요'**
  String get noDriverYet;

  /// No description provided for @screenRecoveryNeeded.
  ///
  /// In ko, this message translates to:
  /// **'화면 복구가 필요해요'**
  String get screenRecoveryNeeded;

  /// No description provided for @mustSwitchStreamingScreen.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 화면으로 전환해야 해요'**
  String get mustSwitchStreamingScreen;

  /// No description provided for @servicePreparing.
  ///
  /// In ko, this message translates to:
  /// **'서비스가 준비하고 있어요'**
  String get servicePreparing;

  /// No description provided for @virtualDisplayNeedsChecking.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면을 확인해야 해요'**
  String get virtualDisplayNeedsChecking;

  /// No description provided for @whileStreamingOnlyVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍하는 동안에는 가상 화면만 켜지고, 끝나면 원래 화면으로 돌아가요.'**
  String get whileStreamingOnlyVirtualDisplay;

  /// No description provided for @couldNotReadHostStatus.
  ///
  /// In ko, this message translates to:
  /// **'Host 상태를 읽지 못했어요. 새로고침해 다시 확인해 주세요.'**
  String get couldNotReadHostStatus;

  /// No description provided for @couldNotReadHostService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스 상태를 읽지 못했어요. 새로고침해 다시 확인해 주세요.'**
  String get couldNotReadHostService;

  /// No description provided for @hostServiceNotInstalledRun.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 설치돼 있지 않아요. Host 설치 파일을 다시 실행해 주세요.'**
  String get hostServiceNotInstalledRun;

  /// No description provided for @hostServiceChangingStateRefresh.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 상태를 바꾸는 중이에요. 잠시 뒤 새로고침해 주세요.'**
  String get hostServiceChangingStateRefresh;

  /// No description provided for @hostServiceOffSoCannot.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 꺼져 있어 서버에 연결하지 못해요. 서비스를 시작하면 자동으로 다시 연결해요.'**
  String get hostServiceOffSoCannot;

  /// No description provided for @screenRecoveryFromLastStreaming2.
  ///
  /// In ko, this message translates to:
  /// **'이전 스트리밍의 화면 복구가 끝나지 않았어요. 가상 화면에서 확인하세요.'**
  String get screenRecoveryFromLastStreaming2;

  /// No description provided for @checkVirtualDisplayStatusFrom.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 상태를 확인하고 복구를 진행하세요.{restart}'**
  String checkVirtualDisplayStatusFrom(Object restart);

  /// No description provided for @viewVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 보기'**
  String get viewVirtualDisplay;

  /// No description provided for @notRegisteredServerYetRequest.
  ///
  /// In ko, this message translates to:
  /// **'아직 서버에 등록하지 않았어요. 관리자에게 받은 서버 주소로 연결을 요청하세요.'**
  String get notRegisteredServerYetRequest;

  /// No description provided for @connect4.
  ///
  /// In ko, this message translates to:
  /// **'연결하기'**
  String get connect4;

  /// No description provided for @cancelingConnectionRequestWaitingServer.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청을 취소하고 있어요. 서버의 취소 확인을 기다리고 있어요.'**
  String get cancelingConnectionRequestWaitingServer;

  /// No description provided for @requestExpiredRefreshStatusThen.
  ///
  /// In ko, this message translates to:
  /// **'요청 유효 시간이 지났어요. 상태를 새로고침한 뒤 다시 요청하세요.'**
  String get requestExpiredRefreshStatusThen;

  /// No description provided for @checkingRegistrationResultApprovedConnection.
  ///
  /// In ko, this message translates to:
  /// **'승인된 연결의 등록 결과를 확인하고 있어요.'**
  String get checkingRegistrationResultApprovedConnection;

  /// No description provided for @adminConsoleUnderPendingApprovals.
  ///
  /// In ko, this message translates to:
  /// **'관리자 웹의 승인 대기에서 확인 코드가 같은지 본 뒤 승인해 주세요.'**
  String get adminConsoleUnderPendingApprovals;

  /// No description provided for @approveConnectionRequestAdminConsole.
  ///
  /// In ko, this message translates to:
  /// **'관리자 웹의 승인 대기에서 이 연결 요청을 승인해 주세요.'**
  String get approveConnectionRequestAdminConsole;

  /// No description provided for @viewRequest.
  ///
  /// In ko, this message translates to:
  /// **'요청 보기'**
  String get viewRequest;

  /// No description provided for @connectionDiagnostics.
  ///
  /// In ko, this message translates to:
  /// **'연결 진단'**
  String get connectionDiagnostics;

  /// No description provided for @fromLocalPcScreen.
  ///
  /// In ko, this message translates to:
  /// **'본체 화면에서'**
  String get fromLocalPcScreen;

  /// No description provided for @fromConsoleClient.
  ///
  /// In ko, this message translates to:
  /// **'클라이언트의 Console로'**
  String get fromConsoleClient;

  /// No description provided for @signRestartDesktopSignAutomatically.
  ///
  /// In ko, this message translates to:
  /// **'{where} 로그인하거나 데스크톱을 다시 시작하면 자동으로 로그인돼요.'**
  String signRestartDesktopSignAutomatically(Object where);

  /// No description provided for @sign2.
  ///
  /// In ko, this message translates to:
  /// **'{where} 로그인해 주세요.'**
  String sign2(Object where);

  /// No description provided for @windowsSignedOutLocked.
  ///
  /// In ko, this message translates to:
  /// **'Windows에 로그인돼 있지 않거나 잠겨 있어요. {where}'**
  String windowsSignedOutLocked(Object where);

  /// No description provided for @signSettings.
  ///
  /// In ko, this message translates to:
  /// **'로그인 설정'**
  String get signSettings;

  /// No description provided for @currentWindowsSignNotStreaming.
  ///
  /// In ko, this message translates to:
  /// **'지금 Windows 로그인은 스트리밍 화면에 연결돼 있지 않아요. 가상 화면에서 스트리밍 화면으로 전환하세요.'**
  String get currentWindowsSignNotStreaming;

  /// No description provided for @autoSignOffSaveWindows.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인이 꺼져 있어요. Windows 계정을 저장하면 다시 시작한 뒤에도 바로 연결할 수 있어요.'**
  String get autoSignOffSaveWindows;

  /// No description provided for @autoSignNeedsCheckingSave.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 설정을 확인해야 해요. Windows 로그인에서 계정을 다시 저장하세요.'**
  String get autoSignNeedsCheckingSave;

  /// No description provided for @someDeviceFeaturesNotReady.
  ///
  /// In ko, this message translates to:
  /// **'바로 쓸 수 없는 장치 기능이 있어요. 장치에서 확인하세요.'**
  String get someDeviceFeaturesNotReady;

  /// No description provided for @viewDevices.
  ///
  /// In ko, this message translates to:
  /// **'장치 보기'**
  String get viewDevices;

  /// No description provided for @openAdministratorSettingsWindow.
  ///
  /// In ko, this message translates to:
  /// **'관리자 설정 창을 열까요?'**
  String get openAdministratorSettingsWindow;

  /// No description provided for @youCanChangeSettingsNew.
  ///
  /// In ko, this message translates to:
  /// **'Windows 관리자 승인을 받은 새 창에서 설정을 변경할 수 있어요.'**
  String get youCanChangeSettingsNew;

  /// No description provided for @openNewWindow.
  ///
  /// In ko, this message translates to:
  /// **'새 창 열기'**
  String get openNewWindow;

  /// No description provided for @waitingWindowsAdministratorApproval.
  ///
  /// In ko, this message translates to:
  /// **'Windows 관리자 승인 대기 중'**
  String get waitingWindowsAdministratorApproval;

  /// No description provided for @administratorSettingsWindowOpenContinue.
  ///
  /// In ko, this message translates to:
  /// **'관리자 설정 창을 열었어요. 새 창에서 계속하세요.'**
  String get administratorSettingsWindowOpenContinue;

  /// No description provided for @settingsAsAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'관리자 권한으로 설정'**
  String get settingsAsAdministrator;

  /// No description provided for @changingSettingsNeedsAdministratorRights.
  ///
  /// In ko, this message translates to:
  /// **'설정을 변경하려면 관리자 권한이 필요해요.'**
  String get changingSettingsNeedsAdministratorRights;

  /// No description provided for @administratorRejectedRequestCheckServer.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 요청을 거절했어요. 서버 주소를 확인한 뒤 다시 요청하세요.'**
  String get administratorRejectedRequestCheckServer;

  /// No description provided for @connectionRequestExpiredCheckServer.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청이 만료됐어요. 서버를 확인한 뒤 다시 요청하세요.'**
  String get connectionRequestExpiredCheckServer;

  /// Host settings: the registration came with a copied disk (full clone) and was kept aside (host-machine-v1).
  ///
  /// In ko, this message translates to:
  /// **'이 Windows는 등록된 다른 데스크톱을 복사한 것 같아요. 이 데스크톱의 연결을 새로 요청하세요.'**
  String get hostCopiedRegistrationRequestAgain;

  /// No description provided for @connectionRequestCanceled.
  ///
  /// In ko, this message translates to:
  /// **'연결 요청을 취소했어요.'**
  String get connectionRequestCanceled;

  /// No description provided for @checkServerAddress2.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소 확인 필요'**
  String get checkServerAddress2;

  /// No description provided for @checkApprovalWaitTime.
  ///
  /// In ko, this message translates to:
  /// **'승인 대기 시간 확인 필요'**
  String get checkApprovalWaitTime;

  /// No description provided for @checkingServerRegistration.
  ///
  /// In ko, this message translates to:
  /// **'서버 등록 확인 중'**
  String get checkingServerRegistration;

  /// No description provided for @awaitingAdministratorApproval.
  ///
  /// In ko, this message translates to:
  /// **'관리자 승인 대기'**
  String get awaitingAdministratorApproval;

  /// No description provided for @confirmationCode.
  ///
  /// In ko, this message translates to:
  /// **'확인 코드'**
  String get confirmationCode;

  /// No description provided for @adminConsoleMustShowSame.
  ///
  /// In ko, this message translates to:
  /// **'관리자 웹에도 이 코드가 보여야 해요. 코드가 다르면 승인하면 안 돼요.'**
  String get adminConsoleMustShowSame;

  /// No description provided for @waitingServerConfirmCancellationAfter.
  ///
  /// In ko, this message translates to:
  /// **'서버의 취소 확인을 기다리고 있어요. 확인 후 다시 연결을 요청할 수 있어요.'**
  String get waitingServerConfirmCancellationAfter;

  /// No description provided for @adminConsoleGoDesktopsPending.
  ///
  /// In ko, this message translates to:
  /// **'관리자 웹 → 데스크톱의 승인 대기에서 확인 코드가 같은지 확인한 뒤 승인해 주세요.'**
  String get adminConsoleGoDesktopsPending;

  /// No description provided for @adminConsoleApproveConnectionRequest.
  ///
  /// In ko, this message translates to:
  /// **'관리자 웹 → 데스크톱의 승인 대기에서 이 연결 요청을 승인해 주세요.'**
  String get adminConsoleApproveConnectionRequest;

  /// No description provided for @evenIfYouCloseWindow.
  ///
  /// In ko, this message translates to:
  /// **'창을 닫아도 Windows 서비스가 연결 요청을 계속 확인해요.'**
  String get evenIfYouCloseWindow;

  /// No description provided for @startWindowsServiceKeepChecking.
  ///
  /// In ko, this message translates to:
  /// **'승인을 계속 확인하려면 Windows 서비스를 시작하세요.'**
  String get startWindowsServiceKeepChecking;

  /// No description provided for @approvalValid.
  ///
  /// In ko, this message translates to:
  /// **'승인 유효 시간 · {expiryText}'**
  String approvalValid(Object expiryText);

  /// No description provided for @couldNotCheckApprovalStatus.
  ///
  /// In ko, this message translates to:
  /// **'승인 상태를 확인하지 못했어요. 잠시 후 다시 확인해요.'**
  String get couldNotCheckApprovalStatus;

  /// No description provided for @requestAlreadyApprovedCheckingServer.
  ///
  /// In ko, this message translates to:
  /// **'이미 승인된 요청이에요. 서버 등록 결과를 확인하고 있어요.'**
  String get requestAlreadyApprovedCheckingServer;

  /// No description provided for @editServer.
  ///
  /// In ko, this message translates to:
  /// **'서버 수정'**
  String get editServer;

  /// No description provided for @serverConnection3.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 정보'**
  String get serverConnection3;

  /// No description provided for @connectServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결'**
  String get connectServer;

  /// No description provided for @registrationKeptAfterUpdates.
  ///
  /// In ko, this message translates to:
  /// **'등록 정보는 업데이트 후에도 유지돼요.'**
  String get registrationKeptAfterUpdates;

  /// No description provided for @checkServerAddressRequestConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소를 확인하고 연결을 요청하면 관리자가 승인해요.'**
  String get checkServerAddressRequestConnection;

  /// No description provided for @desktopId.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 ID'**
  String get desktopId;

  /// No description provided for @connectionStatus.
  ///
  /// In ko, this message translates to:
  /// **'연결 상태'**
  String get connectionStatus;

  /// No description provided for @changeServerReRegister.
  ///
  /// In ko, this message translates to:
  /// **'서버 변경·다시 등록'**
  String get changeServerReRegister;

  /// No description provided for @connectionChecked.
  ///
  /// In ko, this message translates to:
  /// **'연결 확인 완료'**
  String get connectionChecked;

  /// No description provided for @testServerConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 테스트'**
  String get testServerConnection;

  /// No description provided for @nowSendConnectionRequest.
  ///
  /// In ko, this message translates to:
  /// **'이제 연결 요청을 보내세요.'**
  String get nowSendConnectionRequest;

  /// No description provided for @firstFinishServerConnectionTest.
  ///
  /// In ko, this message translates to:
  /// **'먼저 서버 연결 테스트를 완료하세요.'**
  String get firstFinishServerConnectionTest;

  /// No description provided for @text20.
  ///
  /// In ko, this message translates to:
  /// **'가상 모니터 {monitorsLength}대 켜짐'**
  String text20(num monitorsLength);

  /// No description provided for @switchStreamingScreen2.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 화면으로 전환'**
  String get switchStreamingScreen2;

  /// No description provided for @monitor2.
  ///
  /// In ko, this message translates to:
  /// **'모니터 {index}'**
  String monitor2(Object index);

  /// No description provided for @noMultitapVirtualMonitor.
  ///
  /// In ko, this message translates to:
  /// **'켜진 Multitap 가상 모니터가 없어요.'**
  String get noMultitapVirtualMonitor;

  /// No description provided for @switchScreenAfterWindowsRestart.
  ///
  /// In ko, this message translates to:
  /// **'Windows 다시 시작 후 화면 전환'**
  String get switchScreenAfterWindowsRestart;

  /// No description provided for @refreshVirtualDisplayStatus.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 상태 새로고침'**
  String get refreshVirtualDisplayStatus;

  /// No description provided for @resetVirtualDisplayRecoveryRecord.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 복구 기록 초기화'**
  String get resetVirtualDisplayRecoveryRecord;

  /// No description provided for @checkAfterWindowsRestart.
  ///
  /// In ko, this message translates to:
  /// **'Windows 다시 시작 후 확인'**
  String get checkAfterWindowsRestart;

  /// No description provided for @recoverVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 복구'**
  String get recoverVirtualDisplay;

  /// No description provided for @setUpVirtualDisplayFrom.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔에서 가상 화면 설정'**
  String get setUpVirtualDisplayFrom;

  /// No description provided for @setUpVirtualDisplay2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 설정'**
  String get setUpVirtualDisplay2;

  /// No description provided for @checkExistingDevicesLeftoverSettings.
  ///
  /// In ko, this message translates to:
  /// **'기존 장치·남은 설정 확인'**
  String get checkExistingDevicesLeftoverSettings;

  /// No description provided for @onceDisplayReadyResolutionFollows.
  ///
  /// In ko, this message translates to:
  /// **'화면이 준비되면 클라이언트 창 크기에 맞춰 해상도를 조절해요.'**
  String get onceDisplayReadyResolutionFollows;

  /// No description provided for @setNumberMonitors12.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수(1~2대)와 해상도는 연결한 클라이언트에서 조절해요.\n스트리밍에는 Multitap 가상 화면만 사용해요.'**
  String get setNumberMonitors12;

  /// No description provided for @conflictsFound.
  ///
  /// In ko, this message translates to:
  /// **'확인된 충돌 항목'**
  String get conflictsFound;

  /// No description provided for @devicesLeftoverSettingsNothingDeleted.
  ///
  /// In ko, this message translates to:
  /// **'장치 {devices}개 · 남아 있는 설정 {configurations}개. 아직 삭제하지 않았어요.'**
  String devicesLeftoverSettingsNothingDeleted(
    Object devices,
    Object configurations,
  );

  /// No description provided for @noConflictsCleanUp.
  ///
  /// In ko, this message translates to:
  /// **'정리할 충돌 항목이 없어요.'**
  String get noConflictsCleanUp;

  /// No description provided for @virtualDisplayDeviceRequired.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치 · 필수'**
  String get virtualDisplayDeviceRequired;

  /// No description provided for @leftoverSettingsRequired.
  ///
  /// In ko, this message translates to:
  /// **'남아 있는 설정 · 필수'**
  String get leftoverSettingsRequired;

  /// No description provided for @impact2.
  ///
  /// In ko, this message translates to:
  /// **'영향: {items}'**
  String impact2(Object items);

  /// No description provided for @requiredItemsAboveCleanedUp.
  ///
  /// In ko, this message translates to:
  /// **'위 필수 항목을 함께 정리해요. Windows 드라이버 보관함과 기존 설정 파일은 유지해요.'**
  String get requiredItemsAboveCleanedUp;

  /// No description provided for @cleanUpCheckedConflictsInstall2.
  ///
  /// In ko, this message translates to:
  /// **'확인한 충돌 항목 정리·설치'**
  String get cleanUpCheckedConflictsInstall2;

  /// No description provided for @couldNotVerifyDriverFiles2.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 파일을 확인하지 못했어요. Host 설치 파일을 다시 받아 실행한 뒤 확인하세요.'**
  String get couldNotVerifyDriverFiles2;

  /// No description provided for @couldNotSafelyVerifyVirtual.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 구성 파일을 안전하게 확인하지 못했어요. Host 설치 파일을 다시 받아 실행한 뒤 확인하세요.'**
  String get couldNotSafelyVerifyVirtual;

  /// No description provided for @restartWindowsThenCheckConflict.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 다시 시작한 뒤 충돌 목록을 새로 확인하세요.'**
  String get restartWindowsThenCheckConflict;

  /// No description provided for @signWindowsFromProxmoxConsole2.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 콘솔(로컬 PC는 본체 화면)에서 Windows에 로그인한 뒤 다시 확인하세요.'**
  String get signWindowsFromProxmoxConsole2;

  /// No description provided for @firstRecoverPreviousScreenChange.
  ///
  /// In ko, this message translates to:
  /// **'이전 화면 변경을 먼저 복구한 뒤 충돌 목록을 새로 확인하세요.'**
  String get firstRecoverPreviousScreenChange;

  /// No description provided for @automaticCleanupCannotProceedCopy.
  ///
  /// In ko, this message translates to:
  /// **'자동 정리를 진행할 수 없어요. 진단 정보를 복사해 관리자에게 알려 주세요.'**
  String get automaticCleanupCannotProceedCopy;

  /// No description provided for @lastCleanupInstallResult.
  ///
  /// In ko, this message translates to:
  /// **'직전 정리·설치 결과'**
  String get lastCleanupInstallResult;

  /// No description provided for @conflictCleanup.
  ///
  /// In ko, this message translates to:
  /// **'충돌 항목 정리'**
  String get conflictCleanup;

  /// No description provided for @devicesRequestedRemoval.
  ///
  /// In ko, this message translates to:
  /// **'제거 요청한 장치'**
  String get devicesRequestedRemoval;

  /// No description provided for @text21.
  ///
  /// In ko, this message translates to:
  /// **'{result}개'**
  String text21(Object result);

  /// No description provided for @devicesConfirmedRemoved.
  ///
  /// In ko, this message translates to:
  /// **'제거 확인된 장치'**
  String get devicesConfirmedRemoved;

  /// No description provided for @driverInstall.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 설치'**
  String get driverInstall;

  /// No description provided for @notCompleted.
  ///
  /// In ko, this message translates to:
  /// **'완료하지 못함'**
  String get notCompleted;

  /// No description provided for @previousSettingsRestored.
  ///
  /// In ko, this message translates to:
  /// **'이전 설정을 복원했어요.'**
  String get previousSettingsRestored;

  /// No description provided for @someItemsRemainCheckConflict.
  ///
  /// In ko, this message translates to:
  /// **'일부 항목이 남아 있어요. 다시 시도하기 전에 충돌 목록을 다시 확인하세요.'**
  String get someItemsRemainCheckConflict;

  /// No description provided for @deviceRemovalNotConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'장치 제거 여부를 아직 확인하지 못했어요.'**
  String get deviceRemovalNotConfirmedYet;

  /// No description provided for @windowsNeedsRestartDoesNot.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 다시 시작해야 해요. 자동으로 다시 시작하지 않아요.'**
  String get windowsNeedsRestartDoesNot;

  /// No description provided for @offTurnsWhenYouSave.
  ///
  /// In ko, this message translates to:
  /// **'꺼짐 · 계정을 저장하면 켜져요'**
  String get offTurnsWhenYouSave;

  /// No description provided for @setAppliesFromNextWindows.
  ///
  /// In ko, this message translates to:
  /// **'설정됨 · 다음 Windows 시작부터 적용'**
  String get setAppliesFromNextWindows;

  /// No description provided for @windowsSignSetAccountConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'설정한 계정의 Windows 로그인을 확인했어요'**
  String get windowsSignSetAccountConfirmed;

  /// No description provided for @accountCheckNeededSaveAccount.
  ///
  /// In ko, this message translates to:
  /// **'계정 확인 필요 · 계정 정보를 다시 저장하세요'**
  String get accountCheckNeededSaveAccount;

  /// No description provided for @windowsHasExistingAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows에 기존 자동 로그인 설정이 있어요'**
  String get windowsHasExistingAutoSign;

  /// No description provided for @windowsPolicyPreventsTurning.
  ///
  /// In ko, this message translates to:
  /// **'Windows 정책으로 켤 수 없어요'**
  String get windowsPolicyPreventsTurning;

  /// No description provided for @autoSignOffRecoveryNeeded.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 꺼짐 · 복구가 필요해요'**
  String get autoSignOffRecoveryNeeded;

  /// No description provided for @checkChangeResultRecoveryStatus.
  ///
  /// In ko, this message translates to:
  /// **'변경 결과와 복구 상태를 확인해야 해요'**
  String get checkChangeResultRecoveryStatus;

  /// No description provided for @couldNotCheckCurrentSetting.
  ///
  /// In ko, this message translates to:
  /// **'현재 설정을 확인하지 못했어요'**
  String get couldNotCheckCurrentSetting;

  /// No description provided for @hostDoesNotSupportAuto2.
  ///
  /// In ko, this message translates to:
  /// **'이 Host는 자동 로그인 설정을 지원하지 않아요'**
  String get hostDoesNotSupportAuto2;

  /// No description provided for @windowsAccountPasswordWrongNot.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정 또는 비밀번호가 맞지 않아요. 자동으로 다시 시도하지 않아요.'**
  String get windowsAccountPasswordWrongNot;

  /// No description provided for @enterAccountPasswordPasswordConfirmation.
  ///
  /// In ko, this message translates to:
  /// **'계정과 비밀번호, 비밀번호 확인 칸을 다시 입력하세요. 빈 비밀번호와 PIN은 저장하지 않아요.'**
  String get enterAccountPasswordPasswordConfirmation;

  /// No description provided for @onlyLocalWindowsAccountsPassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 있는 로컬 Windows 계정만 지원해요.'**
  String get onlyLocalWindowsAccountsPassword;

  /// No description provided for @accountChangedRestrictedCheckAccount.
  ///
  /// In ko, this message translates to:
  /// **'계정이 변경됐거나 사용이 제한돼 있어요. Windows에서 계정 상태를 확인하세요.'**
  String get accountChangedRestrictedCheckAccount;

  /// No description provided for @windowsPolicyBlocksAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 정책이 자동 로그인을 막고 있어요. 정책은 바꾸지 않아요.'**
  String get windowsPolicyBlocksAutoSign;

  /// No description provided for @windowsSettingsChangedDuringCheck.
  ///
  /// In ko, this message translates to:
  /// **'확인 중 Windows 설정이 바뀌었어요. 현재 상태를 다시 확인하세요.'**
  String get windowsSettingsChangedDuringCheck;

  /// No description provided for @settingsChangedDuringCheckRefresh.
  ///
  /// In ko, this message translates to:
  /// **'확인 중 설정이 바뀌었어요. 새로고침 후 변경 내용을 다시 확인하세요.'**
  String get settingsChangedDuringCheckRefresh;

  /// No description provided for @anotherSettingsTaskRunningCheck.
  ///
  /// In ko, this message translates to:
  /// **'다른 설정 작업이 진행 중이에요. 잠시 후 결과를 확인하세요.'**
  String get anotherSettingsTaskRunningCheck;

  /// No description provided for @autoSignChangeStoppedMidway.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 변경이 중간에 멈췄어요. 아래에서 Multitap이 바꾼 설정을 복구하세요.'**
  String get autoSignChangeStoppedMidway;

  /// No description provided for @couldNotConfirmResultCheck.
  ///
  /// In ko, this message translates to:
  /// **'설정 결과를 확인하지 못했어요. 다시 적용하기 전에 결과를 확인하세요.'**
  String get couldNotConfirmResultCheck;

  /// No description provided for @recoverSettingsMultitapChanged.
  ///
  /// In ko, this message translates to:
  /// **'Multitap이 바꾼 설정을 복구할까요?'**
  String get recoverSettingsMultitapChanged;

  /// No description provided for @onlyAutoSignSettingsMultitap.
  ///
  /// In ko, this message translates to:
  /// **'Multitap이 바꾼 자동 로그인 설정만 원래 값으로 복구해요.\n다른 곳에서 바뀐 값은 덮어쓰지 않아요.\n다시 시작·로그아웃·잠금 해제는 하지 않아요.'**
  String get onlyAutoSignSettingsMultitap;

  /// No description provided for @recoverSettings.
  ///
  /// In ko, this message translates to:
  /// **'설정 복구'**
  String get recoverSettings;

  /// No description provided for @settingUpWindowsAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'Windows 자동 로그인 설정 중'**
  String get settingUpWindowsAutoSign;

  /// No description provided for @checkingCurrentSettingsAccountPermissions.
  ///
  /// In ko, this message translates to:
  /// **'현재 설정과 계정 권한을 확인하고 있어요.'**
  String get checkingCurrentSettingsAccountPermissions;

  /// No description provided for @couldNotPrepareWindowProtection.
  ///
  /// In ko, this message translates to:
  /// **'작업 중 창 보호를 준비하지 못했어요. 설정은 변경하지 않았어요.'**
  String get couldNotPrepareWindowProtection;

  /// No description provided for @currentSettingsPermissionsChangedRefresh.
  ///
  /// In ko, this message translates to:
  /// **'현재 설정 또는 권한이 바뀌었어요. 새로고침 후 다시 확인하세요.'**
  String get currentSettingsPermissionsChangedRefresh;

  /// No description provided for @couldNotVerifyWindowProtection.
  ///
  /// In ko, this message translates to:
  /// **'작업 중 창 보호를 확인하지 못했어요. 설정은 변경하지 않았어요.'**
  String get couldNotVerifyWindowProtection;

  /// No description provided for @couldNotChangeSettingsRefresh.
  ///
  /// In ko, this message translates to:
  /// **'설정을 변경하지 못했어요. 상태를 새로고침해 확인하세요.'**
  String get couldNotChangeSettingsRefresh;

  /// No description provided for @requestResultNotConfirmedYet.
  ///
  /// In ko, this message translates to:
  /// **'요청 결과를 아직 확인하지 못했어요. 다시 적용하지 말고 결과를 확인하세요.'**
  String get requestResultNotConfirmedYet;

  /// No description provided for @changeStoppedMidwayRecoverSettings.
  ///
  /// In ko, this message translates to:
  /// **'변경이 중간에 멈췄어요. Windows 로그인에서 Multitap이 바꾼 설정을 복구하세요.'**
  String get changeStoppedMidwayRecoverSettings;

  /// No description provided for @couldNotConfirmWhetherRequest.
  ///
  /// In ko, this message translates to:
  /// **'이 요청의 완료 여부를 확인하지 못했어요. 결과 확인만 다시 시도하세요.'**
  String get couldNotConfirmWhetherRequest;

  /// No description provided for @turnedAppliesFromNextWindows.
  ///
  /// In ko, this message translates to:
  /// **'켰어요. 다음 Windows 시작부터 적용돼요.'**
  String get turnedAppliesFromNextWindows;

  /// No description provided for @settingsMultitapChangedRecoveredAuto.
  ///
  /// In ko, this message translates to:
  /// **'Multitap이 바꾼 설정을 복구했어요. 자동 로그인은 꺼져 있어요. 계정을 다시 저장하면 켜져요.'**
  String get settingsMultitapChangedRecoveredAuto;

  /// No description provided for @requestFinishedButCurrentSetting.
  ///
  /// In ko, this message translates to:
  /// **'요청은 끝났지만 현재 설정을 확인하지 못했어요. Windows 로그인에서 상태를 확인하세요.'**
  String get requestFinishedButCurrentSetting;

  /// No description provided for @resultNotConfirmedYetSame.
  ///
  /// In ko, this message translates to:
  /// **'아직 결과를 확인하지 못했어요. 같은 요청을 다시 적용하지 않아요.'**
  String get resultNotConfirmedYetSame;

  /// No description provided for @whenYouSaveAccountHost.
  ///
  /// In ko, this message translates to:
  /// **'계정을 저장하면 이 Host가 기존 설정을 넘겨받아 켜요.'**
  String get whenYouSaveAccountHost;

  /// No description provided for @hostVersionCannotTakeOver.
  ///
  /// In ko, this message translates to:
  /// **'이 Host 버전은 기존 설정을 넘겨받을 수 없어요.'**
  String get hostVersionCannotTakeOver;

  /// No description provided for @onceWindowsSignsAccountWhen.
  ///
  /// In ko, this message translates to:
  /// **'켜지면 Windows가 시작될 때 이 계정으로 로그인해요.'**
  String get onceWindowsSignsAccountWhen;

  /// No description provided for @windowsSignsAccountAutomaticallyWhen.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 시작될 때 이 계정으로 자동 로그인해요.'**
  String get windowsSignsAccountAutomaticallyWhen;

  /// No description provided for @refreshCheckCurrentSetting.
  ///
  /// In ko, this message translates to:
  /// **'새로고침으로 현재 설정을 확인하세요.'**
  String get refreshCheckCurrentSetting;

  /// No description provided for @windowsAccount3.
  ///
  /// In ko, this message translates to:
  /// **'Windows 계정'**
  String get windowsAccount3;

  /// No description provided for @streamingAutoSignUseAccount.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍과 자동 로그인에 이 계정을 써요.\nMultitap 로그인 계정과는 별도예요.'**
  String get streamingAutoSignUseAccount;

  /// No description provided for @savedAccount.
  ///
  /// In ko, this message translates to:
  /// **'저장된 계정'**
  String get savedAccount;

  /// No description provided for @autoSignAccount.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 계정'**
  String get autoSignAccount;

  /// No description provided for @windowsSetting.
  ///
  /// In ko, this message translates to:
  /// **'Windows 설정'**
  String get windowsSetting;

  /// No description provided for @confirmed.
  ///
  /// In ko, this message translates to:
  /// **'켜짐 (확인됨)'**
  String get confirmed;

  /// No description provided for @offConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'꺼짐 (확인됨)'**
  String get offConfirmed;

  /// No description provided for @autoSignAccountDiffersFrom2.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 계정과 저장된 계정이 달라요. 계정을 다시 저장하세요.'**
  String get autoSignAccountDiffersFrom2;

  /// No description provided for @updateHostThenCheckAgain.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 후 다시 확인하세요.'**
  String get updateHostThenCheckAgain;

  /// No description provided for @savingAlsoSwitchesAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'저장하면 자동 로그인도 이 계정으로 바뀌어요.'**
  String get savingAlsoSwitchesAutoSign;

  /// No description provided for @enterPasswordOnceMoreTurn.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호를 한 번 더 입력하면 자동 로그인이 켜져요.'**
  String get enterPasswordOnceMoreTurn;

  /// No description provided for @turnAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 켜기'**
  String get turnAutoSign;

  /// No description provided for @changeAccount.
  ///
  /// In ko, this message translates to:
  /// **'계정 변경'**
  String get changeAccount;

  /// No description provided for @clearSavedSignInfo.
  ///
  /// In ko, this message translates to:
  /// **'저장 로그인 정보 지우기'**
  String get clearSavedSignInfo;

  /// No description provided for @unclearWhetherAutoSignFinished.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그인 완료 여부가 불확실해요. 같은 변경을 다시 실행하지 않고 결과만 확인해요.'**
  String get unclearWhetherAutoSignFinished;

  /// No description provided for @checkRequestResult.
  ///
  /// In ko, this message translates to:
  /// **'요청 결과 확인'**
  String get checkRequestResult;

  /// No description provided for @recoverSettingsMultitapChanged2.
  ///
  /// In ko, this message translates to:
  /// **'Multitap이 바꾼 설정 복구'**
  String get recoverSettingsMultitapChanged2;

  /// No description provided for @savedWindowsAccountNeedsChecking.
  ///
  /// In ko, this message translates to:
  /// **'저장된 Windows 계정을 확인해야 해요. 계정 정보를 다시 확인하고 저장하세요.'**
  String get savedWindowsAccountNeedsChecking;

  /// No description provided for @currentWindowsSign.
  ///
  /// In ko, this message translates to:
  /// **'현재 Windows 로그인'**
  String get currentWindowsSign;

  /// No description provided for @signedNotLocked.
  ///
  /// In ko, this message translates to:
  /// **'로그인돼 있고 잠겨 있지 않아요'**
  String get signedNotLocked;

  /// No description provided for @checkSignLockState.
  ///
  /// In ko, this message translates to:
  /// **'로그인 또는 잠금 상태를 확인하세요'**
  String get checkSignLockState;

  /// No description provided for @signChecked.
  ///
  /// In ko, this message translates to:
  /// **'로그인 확인 시각'**
  String get signChecked;

  /// No description provided for @ifWindowsSignsOutLocks.
  ///
  /// In ko, this message translates to:
  /// **'Windows에서 로그아웃되거나 잠기면 Console(로컬 PC는 본체 화면)에서 로그인하세요.\n다시 시작하면 자동으로 로그인돼요.'**
  String get ifWindowsSignsOutLocks;

  /// No description provided for @localUserNameComputerName.
  ///
  /// In ko, this message translates to:
  /// **'로컬 아이디 또는 컴퓨터 이름\\아이디 · PIN이 아닌 계정 비밀번호'**
  String get localUserNameComputerName;

  /// No description provided for @createNewPassword2.
  ///
  /// In ko, this message translates to:
  /// **'새 비밀번호 만들기'**
  String get createNewPassword2;

  /// No description provided for @leastCharacters.
  ///
  /// In ko, this message translates to:
  /// **'{minimumLength}자 이상, '**
  String leastCharacters(Object minimumLength);

  /// No description provided for @accountHasNoWindowsPassword.
  ///
  /// In ko, this message translates to:
  /// **'이 계정은 Windows 비밀번호가 없어요.\n{setup}Windows 비밀번호 규칙에 맞게 만드세요.'**
  String accountHasNoWindowsPassword(Object setup);

  /// No description provided for @newWindowsPassword.
  ///
  /// In ko, this message translates to:
  /// **'새 Windows 비밀번호'**
  String get newWindowsPassword;

  /// No description provided for @windowsPassword.
  ///
  /// In ko, this message translates to:
  /// **'Windows 비밀번호'**
  String get windowsPassword;

  /// No description provided for @leaveBlankSaveIfThere.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호가 없으면 비워 두고 저장하세요.'**
  String get leaveBlankSaveIfThere;

  /// No description provided for @confirmPassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 확인'**
  String get confirmPassword;

  /// No description provided for @createPasswordSave.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 만들고 저장'**
  String get createPasswordSave;

  /// No description provided for @saveTurnAutoSign.
  ///
  /// In ko, this message translates to:
  /// **'저장하고 자동 로그인 켜기'**
  String get saveTurnAutoSign;

  /// No description provided for @checkSaveAccount.
  ///
  /// In ko, this message translates to:
  /// **'계정 확인·저장'**
  String get checkSaveAccount;

  /// No description provided for @discardWindowsAccountInfoYou.
  ///
  /// In ko, this message translates to:
  /// **'입력한 Windows 계정 정보를 버릴까요?'**
  String get discardWindowsAccountInfoYou;

  /// No description provided for @savedWindowsAccountKept.
  ///
  /// In ko, this message translates to:
  /// **'저장된 Windows 계정은 유지해요.'**
  String get savedWindowsAccountKept;

  /// No description provided for @serviceStopping.
  ///
  /// In ko, this message translates to:
  /// **'서비스 중지 중'**
  String get serviceStopping;

  /// No description provided for @servicePaused.
  ///
  /// In ko, this message translates to:
  /// **'서비스 일시 중지'**
  String get servicePaused;

  /// No description provided for @serviceChangingState.
  ///
  /// In ko, this message translates to:
  /// **'서비스 상태 전환 중'**
  String get serviceChangingState;

  /// No description provided for @serviceRunning.
  ///
  /// In ko, this message translates to:
  /// **'서비스 실행 중'**
  String get serviceRunning;

  /// No description provided for @serviceNotInstalled.
  ///
  /// In ko, this message translates to:
  /// **'서비스 미설치'**
  String get serviceNotInstalled;

  /// No description provided for @serviceStopped.
  ///
  /// In ko, this message translates to:
  /// **'서비스 중지'**
  String get serviceStopped;

  /// No description provided for @serviceQueryNeeded.
  ///
  /// In ko, this message translates to:
  /// **'서비스 조회 필요'**
  String get serviceQueryNeeded;

  /// No description provided for @serviceStartNeeded.
  ///
  /// In ko, this message translates to:
  /// **'서비스 시작 필요'**
  String get serviceStartNeeded;

  /// No description provided for @checkServiceStatus.
  ///
  /// In ko, this message translates to:
  /// **'서비스 상태 확인 필요'**
  String get checkServiceStatus;

  /// No description provided for @connectionStatusDelayed.
  ///
  /// In ko, this message translates to:
  /// **'연결 상태 조회 지연'**
  String get connectionStatusDelayed;

  /// No description provided for @serverConnectionConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 확인됨'**
  String get serverConnectionConfirmed;

  /// No description provided for @connectingServer2.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 중'**
  String get connectingServer2;

  /// No description provided for @waitingServerResponse.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답 대기'**
  String get waitingServerResponse;

  /// No description provided for @reconnectingServer.
  ///
  /// In ko, this message translates to:
  /// **'서버 다시 연결 중'**
  String get reconnectingServer;

  /// No description provided for @checkServerResponse.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답 확인 필요'**
  String get checkServerResponse;

  /// No description provided for @checkServerRegistration.
  ///
  /// In ko, this message translates to:
  /// **'서버 등록 확인 필요'**
  String get checkServerRegistration;

  /// No description provided for @checkServerConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결 확인 필요'**
  String get checkServerConnection;

  /// No description provided for @runHostInstallerAgainInstall2.
  ///
  /// In ko, this message translates to:
  /// **'Host 설치 파일을 다시 실행해 Windows 서비스를 설치하세요.'**
  String get runHostInstallerAgainInstall2;

  /// No description provided for @hostServiceOffStartService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 꺼져 있어요. 서비스를 시작하면 서버에 자동으로 연결해요.'**
  String get hostServiceOffStartService;

  /// No description provided for @couldNotReadServiceStatus.
  ///
  /// In ko, this message translates to:
  /// **'서비스 상태를 읽지 못했어요. 상태를 새로고침하세요.'**
  String get couldNotReadServiceStatus;

  /// No description provided for @couldNotReadLatestStatus.
  ///
  /// In ko, this message translates to:
  /// **'최근 상태를 읽지 못했어요. 잠시 후 다시 확인해요.'**
  String get couldNotReadLatestStatus;

  /// No description provided for @connectedServerYouCanConnect.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결돼 있어요. 클라이언트에서 이 데스크톱에 연결할 수 있어요.'**
  String get connectedServerYouCanConnect;

  /// No description provided for @connectedServerCheckVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결돼 있어요. 가상 화면 상태를 확인하세요.'**
  String get connectedServerCheckVirtualDisplay;

  /// No description provided for @serverCouldNotConfirmHosts.
  ///
  /// In ko, this message translates to:
  /// **'서버가 이 Host의 등록을 확인하지 못했어요. 서버 연결에서 연결을 다시 요청하세요.'**
  String get serverCouldNotConfirmHosts;

  /// No description provided for @couldNotVerifyServerCertificate.
  ///
  /// In ko, this message translates to:
  /// **'서버 인증서를 확인하지 못했어요. HTTPS 주소와 인증서를 확인하세요.'**
  String get couldNotVerifyServerCertificate;

  /// No description provided for @couldNotConnectServerCheck.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결하지 못했어요. Windows의 네트워크와 서버 주소를 확인하세요.'**
  String get couldNotConnectServerCheck;

  /// No description provided for @serverNotRespondingTellAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'서버가 응답하지 않아요. 관리자에게 알려 주세요.'**
  String get serverNotRespondingTellAdministrator;

  /// No description provided for @hostServerVersionsDoNot.
  ///
  /// In ko, this message translates to:
  /// **'Host와 서버 버전이 서로 맞지 않아요. 둘 다 업데이트하세요.'**
  String get hostServerVersionsDoNot;

  /// No description provided for @couldNotConfirmRecentServer.
  ///
  /// In ko, this message translates to:
  /// **'최근 서버 응답을 확인하지 못했어요. 서비스가 자동으로 다시 연결해요.'**
  String get couldNotConfirmRecentServer;

  /// No description provided for @serverConnectionDroppedServiceReconnects.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결이 끊겼어요. 서비스가 자동으로 다시 연결해요.'**
  String get serverConnectionDroppedServiceReconnects;

  /// No description provided for @serviceConnectingServerPleaseWait.
  ///
  /// In ko, this message translates to:
  /// **'서비스가 서버에 연결하고 있어요. 잠시 기다려 주세요.'**
  String get serviceConnectingServerPleaseWait;

  /// No description provided for @connectedServerWaitingResponse.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결했고 응답을 기다리고 있어요.'**
  String get connectedServerWaitingResponse;

  /// No description provided for @couldNotConfirmRecentServer2.
  ///
  /// In ko, this message translates to:
  /// **'최근 서버 응답을 확인하지 못했어요. 계속되면 진단 보고서에서 서비스를 다시 시작하세요.'**
  String get couldNotConfirmRecentServer2;

  /// No description provided for @couldNotCheckServerConnection.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결을 확인하지 못했어요. 계속되면 진단 보고서에서 서비스를 다시 시작하세요.'**
  String get couldNotCheckServerConnection;

  /// No description provided for @noConfirmedResponse.
  ///
  /// In ko, this message translates to:
  /// **'확인된 응답 없음'**
  String get noConfirmedResponse;

  /// No description provided for @sAgo.
  ///
  /// In ko, this message translates to:
  /// **'{age}초 전'**
  String sAgo(Object age);

  /// No description provided for @diagnosticsCopied.
  ///
  /// In ko, this message translates to:
  /// **'진단 정보를 복사했어요.'**
  String get diagnosticsCopied;

  /// No description provided for @couldNotCopyDiagnosticsTry.
  ///
  /// In ko, this message translates to:
  /// **'진단 정보를 복사하지 못했어요. 다시 시도하세요.'**
  String get couldNotCopyDiagnosticsTry;

  /// No description provided for @queryNeeded.
  ///
  /// In ko, this message translates to:
  /// **'조회 필요'**
  String get queryNeeded;

  /// No description provided for @restartService.
  ///
  /// In ko, this message translates to:
  /// **'서비스 다시 시작'**
  String get restartService;

  /// No description provided for @summary.
  ///
  /// In ko, this message translates to:
  /// **'요약'**
  String get summary;

  /// No description provided for @diagnosticsSentServerAutomaticallyPasswords.
  ///
  /// In ko, this message translates to:
  /// **'진단 정보는 서버로 자동으로 보내요.\n비밀번호·인증 정보·클립보드 내용은 보내지 않아요.'**
  String get diagnosticsSentServerAutomaticallyPasswords;

  /// No description provided for @copyDiagnostics.
  ///
  /// In ko, this message translates to:
  /// **'진단 정보 복사'**
  String get copyDiagnostics;

  /// No description provided for @hostVersion2.
  ///
  /// In ko, this message translates to:
  /// **'Host 버전'**
  String get hostVersion2;

  /// No description provided for @service.
  ///
  /// In ko, this message translates to:
  /// **'서비스'**
  String get service;

  /// No description provided for @serverRegistrationNeeded.
  ///
  /// In ko, this message translates to:
  /// **'서버 등록 필요'**
  String get serverRegistrationNeeded;

  /// No description provided for @signed.
  ///
  /// In ko, this message translates to:
  /// **'로그인됨'**
  String get signed;

  /// No description provided for @checkSign.
  ///
  /// In ko, this message translates to:
  /// **'로그인 확인 필요'**
  String get checkSign;

  /// No description provided for @ready5.
  ///
  /// In ko, this message translates to:
  /// **'준비됨 · 모니터 {outputs}대'**
  String ready5(num outputs);

  /// No description provided for @checkNeeded.
  ///
  /// In ko, this message translates to:
  /// **'확인 필요 · 모니터 {outputs}대'**
  String checkNeeded(num outputs);

  /// No description provided for @serviceQueryError2.
  ///
  /// In ko, this message translates to:
  /// **'서비스 조회 오류'**
  String get serviceQueryError2;

  /// No description provided for @windowsError.
  ///
  /// In ko, this message translates to:
  /// **'Windows 오류 {s}'**
  String windowsError(Object s);

  /// No description provided for @serviceInstall2.
  ///
  /// In ko, this message translates to:
  /// **'서비스 설치'**
  String get serviceInstall2;

  /// No description provided for @sinceServerResponse.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답 경과'**
  String get sinceServerResponse;

  /// No description provided for @lastConnected.
  ///
  /// In ko, this message translates to:
  /// **'마지막 연결 시각'**
  String get lastConnected;

  /// No description provided for @lastResponse.
  ///
  /// In ko, this message translates to:
  /// **'마지막 응답 시각'**
  String get lastResponse;

  /// No description provided for @connectionGuidance.
  ///
  /// In ko, this message translates to:
  /// **'연결 안내'**
  String get connectionGuidance;

  /// No description provided for @virtualDisplayDriver2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버'**
  String get virtualDisplayDriver2;

  /// No description provided for @virtualDisplayDevice2.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치'**
  String get virtualDisplayDevice2;

  /// No description provided for @virtualMonitors.
  ///
  /// In ko, this message translates to:
  /// **'켜진 가상 모니터'**
  String get virtualMonitors;

  /// No description provided for @text22.
  ///
  /// In ko, this message translates to:
  /// **'{outputs}대'**
  String text22(Object outputs);

  /// No description provided for @screenRecovery.
  ///
  /// In ko, this message translates to:
  /// **'화면 복구'**
  String get screenRecovery;

  /// No description provided for @windowsRestart2.
  ///
  /// In ko, this message translates to:
  /// **'Windows 다시 시작'**
  String get windowsRestart2;

  /// No description provided for @couldNotReadDeviceStatus.
  ///
  /// In ko, this message translates to:
  /// **'장치 상태를 읽지 못했어요.\n상태를 새로고침하세요.'**
  String get couldNotReadDeviceStatus;

  /// No description provided for @permissionsTurnedPerUserDesktop.
  ///
  /// In ko, this message translates to:
  /// **'사용 권한은 관리자 웹에서 사용자·데스크톱마다 켜요. 이 화면은 이 데스크톱이 준비됐는지만 보여 줘요.\n스마트 카드와 직렬 장치는 USB로 연결해요.\n마이크와 카메라는 연결할 수 없어요.'**
  String get permissionsTurnedPerUserDesktop;

  /// No description provided for @installWinfsp3.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp를 설치할까요?'**
  String get installWinfsp3;

  /// No description provided for @installsSharedFolderDriverWinfsp.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더용 드라이버(WinFsp)를 설치해요.\nWindows를 다시 시작해야 하면 알려 드려요. 자동으로 다시 시작하지 않아요.'**
  String get installsSharedFolderDriverWinfsp;

  /// No description provided for @switchUsbConnectionMultitap.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 기능을 Multitap용으로 바꿀까요?'**
  String get switchUsbConnectionMultitap;

  /// No description provided for @usbConnectionFromOtherPrograms.
  ///
  /// In ko, this message translates to:
  /// **'다른 프로그램의 USB 연결 기능은 더 이상 동작하지 않아요.\n연결된 USB 장치도 끊어져요.\nWindows를 다시 시작해야 하면 알려 드려요. 자동으로 다시 시작하지 않아요.'**
  String get usbConnectionFromOtherPrograms;

  /// No description provided for @switchLabel.
  ///
  /// In ko, this message translates to:
  /// **'바꾸기'**
  String get switchLabel;

  /// No description provided for @switchingUsbConnection.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 기능 바꾸는 중'**
  String get switchingUsbConnection;

  /// No description provided for @switchingUsbConnectionMultitapDo.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 기능을 Multitap용으로 바꾸고 있어요. 창을 닫지 마세요.'**
  String get switchingUsbConnectionMultitapDo;

  /// No description provided for @startingPrintSpooler.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 스풀러 시작 중'**
  String get startingPrintSpooler;

  /// No description provided for @startingWindowsPrintSpooler.
  ///
  /// In ko, this message translates to:
  /// **'Windows 인쇄 스풀러를 시작하고 있어요.'**
  String get startingWindowsPrintSpooler;

  /// No description provided for @openSettingsAsAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'관리자 권한으로 설정을 열어야 해요.'**
  String get openSettingsAsAdministrator;

  /// No description provided for @cleanUp.
  ///
  /// In ko, this message translates to:
  /// **'정리'**
  String get cleanUp;

  /// No description provided for @remove.
  ///
  /// In ko, this message translates to:
  /// **'남은 가상 화면 장치 {extrasLength}개를 지울까요?'**
  String remove(num extrasLength);

  /// No description provided for @devicesWithoutMultitapRecordMay.
  ///
  /// In ko, this message translates to:
  /// **'\nMultitap 기록이 없는 장치는 다른 프로그램이 쓰고 있을 수 있어요. 그 프로그램의 가상 화면이 사라질 수 있어요.'**
  String get devicesWithoutMultitapRecordMay;

  /// No description provided for @adapterUseDevicesFromOther.
  ///
  /// In ko, this message translates to:
  /// **'지금 쓰는 어댑터와 다른 회사 장치는 그대로 둬요.\n{list}{adapters}\n화면이 잠깐 깜빡일 수 있어요. 자동으로 다시 시작하지 않아요.'**
  String adapterUseDevicesFromOther(Object list, Object adapters);

  /// No description provided for @cleaningUpVirtualDisplayDevices.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 장치 정리 중'**
  String get cleaningUpVirtualDisplayDevices;

  /// No description provided for @removingOnlyCheckedDevicesOne.
  ///
  /// In ko, this message translates to:
  /// **'확인한 장치만 하나씩 지우고 있어요.'**
  String get removingOnlyCheckedDevicesOne;

  /// No description provided for @allow.
  ///
  /// In ko, this message translates to:
  /// **'허용'**
  String get allow;

  /// No description provided for @checkResult2.
  ///
  /// In ko, this message translates to:
  /// **'결과 확인 필요'**
  String get checkResult2;

  /// No description provided for @waitExpired.
  ///
  /// In ko, this message translates to:
  /// **'대기 만료'**
  String get waitExpired;

  /// No description provided for @progress2.
  ///
  /// In ko, this message translates to:
  /// **'진행 중'**
  String get progress2;

  /// No description provided for @installerCouldNotFinishUpdate.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램이 업데이트를 완료하지 못했어요.\n이 오류 코드만으로는 자세한 원인을 알 수 없어요.'**
  String get installerCouldNotFinishUpdate;

  /// No description provided for @hostUpdateDiagnostics.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 진단'**
  String get hostUpdateDiagnostics;

  /// No description provided for @currentVersion.
  ///
  /// In ko, this message translates to:
  /// **'현재 버전'**
  String get currentVersion;

  /// No description provided for @unknown4.
  ///
  /// In ko, this message translates to:
  /// **'확인 안 됨'**
  String get unknown4;

  /// No description provided for @requestedVersion.
  ///
  /// In ko, this message translates to:
  /// **'요청 버전'**
  String get requestedVersion;

  /// No description provided for @result.
  ///
  /// In ko, this message translates to:
  /// **'결과'**
  String get result;

  /// No description provided for @errorCode2.
  ///
  /// In ko, this message translates to:
  /// **'오류 코드'**
  String get errorCode2;

  /// No description provided for @checked2.
  ///
  /// In ko, this message translates to:
  /// **'확인 시각'**
  String get checked2;

  /// No description provided for @localPc3.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC의'**
  String get localPc3;

  /// No description provided for @openDesktopConsole.
  ///
  /// In ko, this message translates to:
  /// **'Console로 데스크톱을 열고'**
  String get openDesktopConsole;

  /// No description provided for @checkInstallStatusHostSettings.
  ///
  /// In ko, this message translates to:
  /// **'{machine} Host 설정 → 진단에서 설치 상태를 확인하세요.\n관리자는 서버 활동 기록에서 같은 시각의 결과를 보세요.'**
  String checkInstallStatusHostSettings(Object machine);

  /// No description provided for @updateHost.
  ///
  /// In ko, this message translates to:
  /// **'Host를 업데이트할까요?'**
  String get updateHost;

  /// No description provided for @current4.
  ///
  /// In ko, this message translates to:
  /// **'현재 {current}'**
  String current4(Object current);

  /// No description provided for @newVersion5.
  ///
  /// In ko, this message translates to:
  /// **'새 버전 {latest}'**
  String newVersion5(Object latest);

  /// No description provided for @earlierChanges.
  ///
  /// In ko, this message translates to:
  /// **'지난 변경 내역'**
  String get earlierChanges;

  /// No description provided for @couldNotLoadChangeList.
  ///
  /// In ko, this message translates to:
  /// **'변경 내역을 불러오지 못했어요. 업데이트 파일은 서버의 배포 정보로 확인해요.'**
  String get couldNotLoadChangeList;

  /// No description provided for @hostDownloadsFromServerInstalls.
  ///
  /// In ko, this message translates to:
  /// **'Host가 이 서버에서 받아 자동으로 설치해요.\n{kind} 설정은 그대로이고 Windows는 다시 시작하지 않아요.'**
  String hostDownloadsFromServerInstalls(Object kind);

  /// No description provided for @fileSizeSha256Checked.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일의 크기와 SHA-256을 확인한 뒤 설치해요.\n드라이버처럼 {kind}에서 직접 확인해야 하면 설치하지 않고 알려요.'**
  String fileSizeSha256Checked(Object kind);

  /// No description provided for @someoneConnectedNowUpdateWhen.
  ///
  /// In ko, this message translates to:
  /// **'지금 이 {kind}에 연결된 사용자가 있어요.\n‘끝나면 업데이트’는 연결이 끝난 뒤 시작해요.\n‘지금 업데이트’는 연결을 끊고 바로 시작해요.'**
  String someoneConnectedNowUpdateWhen(Object kind);

  /// No description provided for @updateWhenDone.
  ///
  /// In ko, this message translates to:
  /// **'끝나면 업데이트'**
  String get updateWhenDone;

  /// No description provided for @updateNow.
  ///
  /// In ko, this message translates to:
  /// **'지금 업데이트'**
  String get updateNow;

  /// No description provided for @category.
  ///
  /// In ko, this message translates to:
  /// **'분류'**
  String get category;

  /// No description provided for @noSavedScreen.
  ///
  /// In ko, this message translates to:
  /// **'저장된 화면 없음'**
  String get noSavedScreen;

  /// No description provided for @select.
  ///
  /// In ko, this message translates to:
  /// **'{machineName} · 선택'**
  String select(Object machineName);

  /// No description provided for @screenPreviewOff.
  ///
  /// In ko, this message translates to:
  /// **'화면 미리보기 꺼짐'**
  String get screenPreviewOff;

  /// No description provided for @manageVm.
  ///
  /// In ko, this message translates to:
  /// **'VM 관리'**
  String get manageVm;

  /// No description provided for @manageVmHostUpdateAvailable.
  ///
  /// In ko, this message translates to:
  /// **'VM 관리 · Host 업데이트 있음'**
  String get manageVmHostUpdateAvailable;

  /// No description provided for @cards.
  ///
  /// In ko, this message translates to:
  /// **'카드형'**
  String get cards;

  /// No description provided for @list.
  ///
  /// In ko, this message translates to:
  /// **'리스트형'**
  String get list;

  /// No description provided for @mini.
  ///
  /// In ko, this message translates to:
  /// **'미니'**
  String get mini;

  /// No description provided for @explorer.
  ///
  /// In ko, this message translates to:
  /// **'탐색기형'**
  String get explorer;

  /// No description provided for @monitorWall.
  ///
  /// In ko, this message translates to:
  /// **'모니터월'**
  String get monitorWall;

  /// No description provided for @dock.
  ///
  /// In ko, this message translates to:
  /// **'도킹형'**
  String get dock;

  /// No description provided for @recoveryCode.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드'**
  String get recoveryCode;

  /// No description provided for @n6DigitCodeFromAuthenticator.
  ///
  /// In ko, this message translates to:
  /// **'인증 앱의 6자리 코드'**
  String get n6DigitCodeFromAuthenticator;

  /// No description provided for @checkingSetupStatus.
  ///
  /// In ko, this message translates to:
  /// **'설정 상태를 확인하고 있어요.'**
  String get checkingSetupStatus;

  /// No description provided for @couldNotCheckSetupStatus.
  ///
  /// In ko, this message translates to:
  /// **'설정 상태를 확인하지 못했어요.'**
  String get couldNotCheckSetupStatus;

  /// No description provided for @twoStepVerification2.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증을 사용하고 있어요.'**
  String get twoStepVerification2;

  /// No description provided for @twoStepVerificationOff.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증을 사용하지 않고 있어요.'**
  String get twoStepVerificationOff;

  /// No description provided for @text23.
  ///
  /// In ko, this message translates to:
  /// **'사용할 수 있는 복구 코드 {recoveryCodesRemaining}개'**
  String text23(num recoveryCodesRemaining);

  /// No description provided for @changingSignsYouOutAll2.
  ///
  /// In ko, this message translates to:
  /// **'설정을 변경하면 모든 기기에서 로그아웃돼요.'**
  String get changingSignsYouOutAll2;

  /// No description provided for @setUpAuthenticatorApp.
  ///
  /// In ko, this message translates to:
  /// **'인증 앱 설정'**
  String get setUpAuthenticatorApp;

  /// No description provided for @makeNewRecoveryCodes.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드 다시 만들기'**
  String get makeNewRecoveryCodes;

  /// No description provided for @turnOffTwoStepVerification.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증 끄기'**
  String get turnOffTwoStepVerification;

  /// No description provided for @n13VerifyItsYou.
  ///
  /// In ko, this message translates to:
  /// **'1/3 · 본인 확인'**
  String get n13VerifyItsYou;

  /// No description provided for @verifyItsYou.
  ///
  /// In ko, this message translates to:
  /// **'본인 확인'**
  String get verifyItsYou;

  /// No description provided for @enterNextCodeFromAuthenticator.
  ///
  /// In ko, this message translates to:
  /// **'이미 사용한 코드 대신 인증 앱의 다음 코드를 입력하세요.'**
  String get enterNextCodeFromAuthenticator;

  /// No description provided for @useAuthenticatorCode.
  ///
  /// In ko, this message translates to:
  /// **'인증 앱 코드 사용'**
  String get useAuthenticatorCode;

  /// No description provided for @useRecoveryCode.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드 사용'**
  String get useRecoveryCode;

  /// No description provided for @makingNewCodesStopsOld.
  ///
  /// In ko, this message translates to:
  /// **'새 코드를 만들면 기존 복구 코드는 더 이상 사용할 수 없어요.'**
  String get makingNewCodesStopsOld;

  /// No description provided for @whenChangeDoneYouSigned.
  ///
  /// In ko, this message translates to:
  /// **'변경을 완료하면 모든 기기에서 로그아웃돼요.'**
  String get whenChangeDoneYouSigned;

  /// No description provided for @n23LinkAuthenticatorApp.
  ///
  /// In ko, this message translates to:
  /// **'2/3 · 인증 앱 연결'**
  String get n23LinkAuthenticatorApp;

  /// No description provided for @n33SaveRecoveryCodes.
  ///
  /// In ko, this message translates to:
  /// **'3/3 · 복구 코드 보관'**
  String get n33SaveRecoveryCodes;

  /// No description provided for @scanQrCodeAuthenticatorApp.
  ///
  /// In ko, this message translates to:
  /// **'다른 기기의 인증 앱으로 QR 코드를 스캔하거나 설정 키를 직접 입력하세요.'**
  String get scanQrCodeAuthenticatorApp;

  /// No description provided for @hideQrSetupKey.
  ///
  /// In ko, this message translates to:
  /// **'QR·설정 키 숨기기'**
  String get hideQrSetupKey;

  /// No description provided for @showQrSetupKey.
  ///
  /// In ko, this message translates to:
  /// **'QR·설정 키 보기'**
  String get showQrSetupKey;

  /// No description provided for @timeLeftSYouCan.
  ///
  /// In ko, this message translates to:
  /// **'남은 시간 {remainingInSeconds}초\n이 키는 이 화면에서만 확인할 수 있어요.'**
  String timeLeftSYouCan(Object remainingInSeconds);

  /// No description provided for @setupKeyDisplayTimeEnded.
  ///
  /// In ko, this message translates to:
  /// **'설정 키 표시 시간이 끝나 키를 지웠어요.'**
  String get setupKeyDisplayTimeEnded;

  /// No description provided for @waitingResultSetupRequestAlready.
  ///
  /// In ko, this message translates to:
  /// **'이미 보낸 설정 요청의 결과를 기다리고 있어요. 요청을 다시 보내지 않아요.'**
  String get waitingResultSetupRequestAlready;

  /// No description provided for @changeRequestsNotSentAgain.
  ///
  /// In ko, this message translates to:
  /// **'변경 요청은 자동으로 다시 보내지 않아요.'**
  String get changeRequestsNotSentAgain;

  /// No description provided for @signAgainThenCheckTwo.
  ///
  /// In ko, this message translates to:
  /// **'다시 로그인한 뒤 2단계 인증 설정 상태를 확인하세요.'**
  String get signAgainThenCheckTwo;

  /// No description provided for @settingsChangedSignAgain.
  ///
  /// In ko, this message translates to:
  /// **'설정을 변경했어요. 다시 로그인하세요.'**
  String get settingsChangedSignAgain;

  /// No description provided for @signChangedSoSetupWas.
  ///
  /// In ko, this message translates to:
  /// **'로그인 상태가 바뀌어 설정을 종료했어요.'**
  String get signChangedSoSetupWas;

  /// No description provided for @finishSetup.
  ///
  /// In ko, this message translates to:
  /// **'설정 완료'**
  String get finishSetup;

  /// No description provided for @closeRecoveryCodes.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드를 닫을까요?'**
  String get closeRecoveryCodes;

  /// No description provided for @youCannotSeeTheseCodes.
  ///
  /// In ko, this message translates to:
  /// **'이 코드는 다시 볼 수 없어요. 아직 보관하지 않았다면 돌아가서 보관하세요.'**
  String get youCannotSeeTheseCodes;

  /// No description provided for @goBack.
  ///
  /// In ko, this message translates to:
  /// **'돌아가기'**
  String get goBack;

  /// No description provided for @closeWithoutSaving2.
  ///
  /// In ko, this message translates to:
  /// **'보관하지 않고 닫기'**
  String get closeWithoutSaving2;

  /// No description provided for @twoStepVerificationOff2.
  ///
  /// In ko, this message translates to:
  /// **'2단계 인증을 껐어요'**
  String get twoStepVerificationOff2;

  /// No description provided for @saveRecoveryCodes.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드를 보관하세요'**
  String get saveRecoveryCodes;

  /// No description provided for @youSignedOutAllDevices.
  ///
  /// In ko, this message translates to:
  /// **'모든 기기에서 로그아웃했어요. 다시 로그인해야 해요.'**
  String get youSignedOutAllDevices;

  /// No description provided for @eachCodeWorksOnlyOnce.
  ///
  /// In ko, this message translates to:
  /// **'각 코드는 한 번만 사용할 수 있어요. 이 화면을 닫으면 다시 볼 수 없어요.'**
  String get eachCodeWorksOnlyOnce;

  /// No description provided for @iSavedRecoveryCodesSomewhere.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드를 따로 보관했어요'**
  String get iSavedRecoveryCodesSomewhere;

  /// No description provided for @cannotShowQrCodeEnter.
  ///
  /// In ko, this message translates to:
  /// **'QR 코드를 표시할 수 없어요. 아래 설정 키를 직접 입력해 주세요.'**
  String get cannotShowQrCodeEnter;

  /// No description provided for @setupQrCodeScanAuthenticator.
  ///
  /// In ko, this message translates to:
  /// **'인증 앱으로 스캔할 설정 QR 코드'**
  String get setupQrCodeScanAuthenticator;

  /// No description provided for @enterRecoveryCodeYouSaved.
  ///
  /// In ko, this message translates to:
  /// **'보관한 복구 코드를 입력하세요.'**
  String get enterRecoveryCodeYouSaved;

  /// No description provided for @enter6DigitCode.
  ///
  /// In ko, this message translates to:
  /// **'6자리 인증 코드를 입력하세요.'**
  String get enter6DigitCode;

  /// No description provided for @couldNotConfirmResultTry.
  ///
  /// In ko, this message translates to:
  /// **'인증 결과를 확인하지 못했어요. 다시 시도하세요.'**
  String get couldNotConfirmResultTry;

  /// No description provided for @couldNotConfirmResultStart.
  ///
  /// In ko, this message translates to:
  /// **'인증 결과를 확인하지 못했어요.\n로그인부터 다시 시작하세요.'**
  String get couldNotConfirmResultStart;

  /// No description provided for @verificationTimeRanOutStart.
  ///
  /// In ko, this message translates to:
  /// **'인증 시간이 끝났어요.\n로그인부터 다시 시작하세요.'**
  String get verificationTimeRanOutStart;

  /// No description provided for @enterRecoveryCodeEachOne.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드를 입력하세요.\n한 번만 쓸 수 있어요.'**
  String get enterRecoveryCodeEachOne;

  /// No description provided for @enter6DigitCodeFrom.
  ///
  /// In ko, this message translates to:
  /// **'인증 앱의 6자리 코드를 입력하세요.'**
  String get enter6DigitCodeFrom;

  /// No description provided for @timeLeft.
  ///
  /// In ko, this message translates to:
  /// **'남은 시간 {seconds}:{seconds2}'**
  String timeLeft(Object seconds, Object seconds2);

  /// No description provided for @authenticatorApp.
  ///
  /// In ko, this message translates to:
  /// **'인증 앱'**
  String get authenticatorApp;

  /// No description provided for @code.
  ///
  /// In ko, this message translates to:
  /// **'인증 코드'**
  String get code;

  /// No description provided for @n6Digits.
  ///
  /// In ko, this message translates to:
  /// **'6자리 숫자'**
  String get n6Digits;

  /// No description provided for @backSign.
  ///
  /// In ko, this message translates to:
  /// **'로그인으로 돌아가기'**
  String get backSign;

  /// No description provided for @verifySign.
  ///
  /// In ko, this message translates to:
  /// **'인증하고 로그인'**
  String get verifySign;

  /// No description provided for @plannedMaintenance.
  ///
  /// In ko, this message translates to:
  /// **'점검 예고'**
  String get plannedMaintenance;

  /// No description provided for @plannedMaintenance2.
  ///
  /// In ko, this message translates to:
  /// **'점검 예고 · {when}'**
  String plannedMaintenance2(Object when);

  /// No description provided for @notice2.
  ///
  /// In ko, this message translates to:
  /// **'공지'**
  String get notice2;

  /// No description provided for @text24.
  ///
  /// In ko, this message translates to:
  /// **'공지 {waiting}개 더 있어요.'**
  String text24(num waiting);

  /// No description provided for @closeNotice.
  ///
  /// In ko, this message translates to:
  /// **'공지 닫기'**
  String get closeNotice;

  /// No description provided for @text25.
  ///
  /// In ko, this message translates to:
  /// **'공지 {unread}개'**
  String text25(num unread);

  /// No description provided for @deleteNotice.
  ///
  /// In ko, this message translates to:
  /// **'이 공지를 지울까요?'**
  String get deleteNotice;

  /// No description provided for @deletedEveryDeviceSignedAccount.
  ///
  /// In ko, this message translates to:
  /// **'이 계정으로 로그인한 모든 기기에서 지워져요.'**
  String get deletedEveryDeviceSignedAccount;

  /// No description provided for @text26.
  ///
  /// In ko, this message translates to:
  /// **'공지 {noticesLength}개를 모두 지울까요?'**
  String text26(num noticesLength);

  /// No description provided for @deleteAll.
  ///
  /// In ko, this message translates to:
  /// **'모두 지우기'**
  String get deleteAll;

  /// No description provided for @noNotices.
  ///
  /// In ko, this message translates to:
  /// **'받은 공지가 없어요.'**
  String get noNotices;

  /// No description provided for @couldNotStartLocalPc.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC를 켜지 못했어요'**
  String get couldNotStartLocalPc;

  /// No description provided for @collapse.
  ///
  /// In ko, this message translates to:
  /// **'접기'**
  String get collapse;

  /// No description provided for @whatNeedsStart.
  ///
  /// In ko, this message translates to:
  /// **'켜려면 필요한 것'**
  String get whatNeedsStart;

  /// No description provided for @sendingPrinter.
  ///
  /// In ko, this message translates to:
  /// **'프린터로 보내는 중…'**
  String get sendingPrinter;

  /// No description provided for @saving.
  ///
  /// In ko, this message translates to:
  /// **'저장하는 중…'**
  String get saving;

  /// No description provided for @printingPage.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 중 · 페이지 {done}/{total}'**
  String printingPage(Object done, Object total);

  /// No description provided for @preparingPrint.
  ///
  /// In ko, this message translates to:
  /// **'인쇄 준비 중…'**
  String get preparingPrint;

  /// No description provided for @text27.
  ///
  /// In ko, this message translates to:
  /// **'{pages}페이지'**
  String text27(num pages);

  /// No description provided for @documentSentFromDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 보낸 문서'**
  String get documentSentFromDesktop;

  /// No description provided for @moreWaiting.
  ///
  /// In ko, this message translates to:
  /// **' · 대기 {waiting}개 더'**
  String moreWaiting(Object waiting);

  /// No description provided for @paperColorTwoSidedPrinting.
  ///
  /// In ko, this message translates to:
  /// **'용지·색상·양면은 프린터의 기본 설정을 따라요.'**
  String get paperColorTwoSidedPrinting;

  /// No description provided for @noPrinterInstalledPcYou.
  ///
  /// In ko, this message translates to:
  /// **'PC에 설치된 프린터가 없어요. PDF로 저장할 수 있어요.'**
  String get noPrinterInstalledPcYou;

  /// No description provided for @defaultLabel2.
  ///
  /// In ko, this message translates to:
  /// **'{printerName} (기본)'**
  String defaultLabel2(Object printerName);

  /// No description provided for @copies.
  ///
  /// In ko, this message translates to:
  /// **'매수'**
  String get copies;

  /// No description provided for @oneFewerCopy.
  ///
  /// In ko, this message translates to:
  /// **'한 장 줄이기'**
  String get oneFewerCopy;

  /// No description provided for @n1CopyMinimum.
  ///
  /// In ko, this message translates to:
  /// **'1장이 가장 적어요'**
  String get n1CopyMinimum;

  /// No description provided for @oneMoreCopy.
  ///
  /// In ko, this message translates to:
  /// **'한 장 늘리기'**
  String get oneMoreCopy;

  /// No description provided for @youCanPrintUpCopies.
  ///
  /// In ko, this message translates to:
  /// **'{maximumCopies}장까지 한 번에 인쇄할 수 있어요'**
  String youCanPrintUpCopies(Object maximumCopies);

  /// No description provided for @saveAsPdf.
  ///
  /// In ko, this message translates to:
  /// **'PDF로 저장'**
  String get saveAsPdf;

  /// No description provided for @noPrinterPrint.
  ///
  /// In ko, this message translates to:
  /// **'인쇄할 프린터가 없어요'**
  String get noPrinterPrint;

  /// No description provided for @print.
  ///
  /// In ko, this message translates to:
  /// **'인쇄'**
  String get print;

  /// No description provided for @previousPage.
  ///
  /// In ko, this message translates to:
  /// **'이전 페이지'**
  String get previousPage;

  /// No description provided for @nextPage.
  ///
  /// In ko, this message translates to:
  /// **'다음 페이지'**
  String get nextPage;

  /// No description provided for @pagePreview.
  ///
  /// In ko, this message translates to:
  /// **'{preview}페이지 미리보기'**
  String pagePreview(Object preview);

  /// No description provided for @eachPageKeepsItsOriginal.
  ///
  /// In ko, this message translates to:
  /// **'페이지마다 원본 방향을 유지해요.'**
  String get eachPageKeepsItsOriginal;

  /// No description provided for @couldNotOpenPreview.
  ///
  /// In ko, this message translates to:
  /// **'미리보기를 열지 못했어요.'**
  String get couldNotOpenPreview;

  /// No description provided for @multitapLog.
  ///
  /// In ko, this message translates to:
  /// **'Multitap {loadHost} 로그'**
  String multitapLog(Object loadHost);

  /// No description provided for @recentEntriesPasswordsTokensClipboard.
  ///
  /// In ko, this message translates to:
  /// **'최근 기록 · 비밀번호/토큰/클립보드 내용 제외'**
  String get recentEntriesPasswordsTokensClipboard;

  /// No description provided for @couldNotReadHostLog.
  ///
  /// In ko, this message translates to:
  /// **'Host 기록을 읽지 못했어요. 앱 기록만 표시해요.'**
  String get couldNotReadHostLog;

  /// No description provided for @logFileCannotWrittenSo.
  ///
  /// In ko, this message translates to:
  /// **'기록 파일을 쓸 수 없어 이번 실행의 기록만 표시해요.'**
  String get logFileCannotWrittenSo;

  /// No description provided for @multitap.
  ///
  /// In ko, this message translates to:
  /// **'Multitap {loadHost} · 최근 기록 {entriesLength}개'**
  String multitap(Object loadHost, num entriesLength);

  /// No description provided for @needsAttention2.
  ///
  /// In ko, this message translates to:
  /// **'확인 필요 {attention}'**
  String needsAttention2(Object attention);

  /// No description provided for @loading2.
  ///
  /// In ko, this message translates to:
  /// **'불러오는 중'**
  String get loading2;

  /// No description provided for @copied.
  ///
  /// In ko, this message translates to:
  /// **'복사됨'**
  String get copied;

  /// No description provided for @copyAgain.
  ///
  /// In ko, this message translates to:
  /// **'복사 다시 시도'**
  String get copyAgain;

  /// No description provided for @noEntriesShow.
  ///
  /// In ko, this message translates to:
  /// **'표시할 기록이 없어요.'**
  String get noEntriesShow;

  /// No description provided for @appStarted.
  ///
  /// In ko, this message translates to:
  /// **'앱 시작'**
  String get appStarted;

  /// No description provided for @desktopListLoaded.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 목록 조회'**
  String get desktopListLoaded;

  /// No description provided for @streamingConnectionStarted.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 연결 시작'**
  String get streamingConnectionStarted;

  /// No description provided for @streamingReadyReply.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 준비 응답'**
  String get streamingReadyReply;

  /// No description provided for @streamingEnded.
  ///
  /// In ko, this message translates to:
  /// **'스트리밍 종료'**
  String get streamingEnded;

  /// No description provided for @resolutionChangeRequested.
  ///
  /// In ko, this message translates to:
  /// **'해상도 변경 요청'**
  String get resolutionChangeRequested;

  /// No description provided for @resolutionChangeResult.
  ///
  /// In ko, this message translates to:
  /// **'해상도 적용 결과'**
  String get resolutionChangeResult;

  /// No description provided for @updateCheckResult.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 조회 결과'**
  String get updateCheckResult;

  /// No description provided for @updateDownload.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 다운로드'**
  String get updateDownload;

  /// No description provided for @hostInstallStarted.
  ///
  /// In ko, this message translates to:
  /// **'Host 설치 시작'**
  String get hostInstallStarted;

  /// No description provided for @installStep.
  ///
  /// In ko, this message translates to:
  /// **'설치 단계'**
  String get installStep;

  /// No description provided for @installResult.
  ///
  /// In ko, this message translates to:
  /// **'설치 결과'**
  String get installResult;

  /// No description provided for @virtualDisplayStatus.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 상태'**
  String get virtualDisplayStatus;

  /// No description provided for @existingDriverKept.
  ///
  /// In ko, this message translates to:
  /// **'기존 드라이버 유지'**
  String get existingDriverKept;

  /// No description provided for @virtualDisplayDriverInstall.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버 설치'**
  String get virtualDisplayDriverInstall;

  /// No description provided for @virtualDisplayTurned.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 활성화'**
  String get virtualDisplayTurned;

  /// No description provided for @resolutionListChecked.
  ///
  /// In ko, this message translates to:
  /// **'해상도 목록 확인'**
  String get resolutionListChecked;

  /// No description provided for @serviceStatus.
  ///
  /// In ko, this message translates to:
  /// **'서비스 상태'**
  String get serviceStatus;

  /// No description provided for @serverResponseStatus.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답 상태'**
  String get serverResponseStatus;

  /// No description provided for @serverAddressSave.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소 저장'**
  String get serverAddressSave;

  /// No description provided for @systemErrorCode.
  ///
  /// In ko, this message translates to:
  /// **'시스템 오류 코드'**
  String get systemErrorCode;

  /// No description provided for @hostSettingsAction.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정 작업'**
  String get hostSettingsAction;

  /// No description provided for @updateInstall.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 설치'**
  String get updateInstall;

  /// No description provided for @clipboard.
  ///
  /// In ko, this message translates to:
  /// **'클립보드'**
  String get clipboard;

  /// No description provided for @driveSharing.
  ///
  /// In ko, this message translates to:
  /// **'드라이브 공유'**
  String get driveSharing;

  /// No description provided for @androidPrinting.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄'**
  String get androidPrinting;

  /// No description provided for @androidPrintingDocumentRequested.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 문서 요청'**
  String get androidPrintingDocumentRequested;

  /// No description provided for @androidPrintingRequestReceived.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 요청 받음'**
  String get androidPrintingRequestReceived;

  /// No description provided for @androidPrintingDocumentChecked.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 문서 확인'**
  String get androidPrintingDocumentChecked;

  /// No description provided for @androidPrintingSystemPrintRequested.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 시스템 인쇄 요청'**
  String get androidPrintingSystemPrintRequested;

  /// No description provided for @androidPrintingPrintServiceConnected.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 인쇄 서비스 연결'**
  String get androidPrintingPrintServiceConnected;

  /// No description provided for @androidPrintingLayoutChecked.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 레이아웃 확인'**
  String get androidPrintingLayoutChecked;

  /// No description provided for @androidPrintingDocumentTransferStarted.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 문서 전달 시작'**
  String get androidPrintingDocumentTransferStarted;

  /// No description provided for @androidPrintingDocumentTransferFinished.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 문서 전달 완료'**
  String get androidPrintingDocumentTransferFinished;

  /// No description provided for @androidPrintingPrintScreenClosed.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 인쇄 화면 종료'**
  String get androidPrintingPrintScreenClosed;

  /// No description provided for @androidPrintingSystemErrorSeen.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 시스템 오류 관측'**
  String get androidPrintingSystemErrorSeen;

  /// No description provided for @androidPrintingJobAccepted.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 접수 확인'**
  String get androidPrintingJobAccepted;

  /// No description provided for @androidPrintingCanceled.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 취소'**
  String get androidPrintingCanceled;

  /// No description provided for @androidPrintingRequestFailed.
  ///
  /// In ko, this message translates to:
  /// **'Android 인쇄 · 요청 실패'**
  String get androidPrintingRequestFailed;

  /// No description provided for @updateCheckFailedCheckRunning.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 실패 · 실행 버전 확인 필요'**
  String get updateCheckFailedCheckRunning;

  /// No description provided for @updateCheckFailedUpdateList.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 실패 · 업데이트 목록 형식 오류'**
  String get updateCheckFailedUpdateList;

  /// No description provided for @updateCheckFailedCouldNot.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 실패 · 서버 목록을 불러오지 못함'**
  String get updateCheckFailedCouldNot;

  /// No description provided for @updateCheckFailed.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 실패'**
  String get updateCheckFailed;

  /// No description provided for @updateCheckNewVersionAvailable.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 · 새 버전 있음'**
  String get updateCheckNewVersionAvailable;

  /// No description provided for @updateCheckNewVersionFound.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 · 새 버전 {latest} 발견'**
  String updateCheckNewVersionFound(Object latest);

  /// No description provided for @updateCheckNoPublishedVersion.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 · 게시된 버전 없음'**
  String get updateCheckNoPublishedVersion;

  /// No description provided for @updateCheckUpDate.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 · 최신 버전'**
  String get updateCheckUpDate;

  /// No description provided for @updateCheckUpDate2.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 · 최신 버전 ({current})'**
  String updateCheckUpDate2(Object current);

  /// No description provided for @downloadFailed.
  ///
  /// In ko, this message translates to:
  /// **'{name} 다운로드 실패'**
  String downloadFailed(Object name);

  /// No description provided for @downloadFinished.
  ///
  /// In ko, this message translates to:
  /// **'{name} 다운로드 완료'**
  String downloadFinished(Object name);

  /// No description provided for @installFailed.
  ///
  /// In ko, this message translates to:
  /// **'{step} 설치 실패'**
  String installFailed(Object step);

  /// No description provided for @installerStarted.
  ///
  /// In ko, this message translates to:
  /// **'{step} 설치 프로그램 실행'**
  String installerStarted(Object step);

  /// No description provided for @restartingAfterUpdate.
  ///
  /// In ko, this message translates to:
  /// **'{step} 업데이트 후 다시 시작'**
  String restartingAfterUpdate(Object step);

  /// No description provided for @clipboardConnectedCopyPasteShared.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 연결됨 · 복사·붙여넣기 공유'**
  String get clipboardConnectedCopyPasteShared;

  /// No description provided for @clipboardTransferFailed.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 {direction} 전송 실패'**
  String clipboardTransferFailed(Object direction);

  /// No description provided for @clipboardTransfer.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 {direction} 전송'**
  String clipboardTransfer(Object direction);

  /// No description provided for @clipboardDisconnected.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 연결 끊김'**
  String get clipboardDisconnected;

  /// No description provided for @clipboardConnectionEnded.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 연결 끝남'**
  String get clipboardConnectionEnded;

  /// No description provided for @couldNotPrepareAutomaticClipboard.
  ///
  /// In ko, this message translates to:
  /// **'클립보드 자동 공유 준비 실패'**
  String get couldNotPrepareAutomaticClipboard;

  /// No description provided for @sharedFolderConnected.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 연결됨'**
  String get sharedFolderConnected;

  /// No description provided for @sharedFolderChanged.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 변경'**
  String get sharedFolderChanged;

  /// No description provided for @desktopDriveConnectionFailed.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 드라이브 연결 실패'**
  String get desktopDriveConnectionFailed;

  /// No description provided for @desktopDriveConnected.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 드라이브 연결됨'**
  String get desktopDriveConnected;

  /// No description provided for @sharedFolderDisconnected.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 연결 끊김'**
  String get sharedFolderDisconnected;

  /// No description provided for @sharedFolderConnectionEnded.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 연결 끝남'**
  String get sharedFolderConnectionEnded;

  /// No description provided for @usbConnectionReady.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 준비됨'**
  String get usbConnectionReady;

  /// No description provided for @usbDeviceConnectionFailed.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치 연결 실패'**
  String get usbDeviceConnectionFailed;

  /// No description provided for @usbDeviceConnected.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치 연결됨'**
  String get usbDeviceConnected;

  /// No description provided for @usbDeviceRemoved.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치 분리'**
  String get usbDeviceRemoved;

  /// No description provided for @desktopDidNotAcceptUsb.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치를 데스크톱이 받지 않음'**
  String get desktopDidNotAcceptUsb;

  /// No description provided for @usbDisconnected.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 끊김'**
  String get usbDisconnected;

  /// No description provided for @usbConnectionEnded.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 끝남'**
  String get usbConnectionEnded;

  /// No description provided for @usbCaptureDriverInstallFailed.
  ///
  /// In ko, this message translates to:
  /// **'USB 캡처 드라이버 설치 실패'**
  String get usbCaptureDriverInstallFailed;

  /// No description provided for @notInstalled2.
  ///
  /// In ko, this message translates to:
  /// **'설치 안 됨'**
  String get notInstalled2;

  /// No description provided for @deviceNotStarted.
  ///
  /// In ko, this message translates to:
  /// **'장치 시작 안 됨'**
  String get deviceNotStarted;

  /// No description provided for @notRunning.
  ///
  /// In ko, this message translates to:
  /// **'실행 안 됨'**
  String get notRunning;

  /// No description provided for @noResponse.
  ///
  /// In ko, this message translates to:
  /// **'응답 없음'**
  String get noResponse;

  /// No description provided for @noChange.
  ///
  /// In ko, this message translates to:
  /// **'변경 없음'**
  String get noChange;

  /// No description provided for @consoleSessionOff.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 꺼짐'**
  String get consoleSessionOff;

  /// No description provided for @resolution2.
  ///
  /// In ko, this message translates to:
  /// **'해상도 {width}×{height}'**
  String resolution2(Object width, Object height);

  /// No description provided for @tookMs.
  ///
  /// In ko, this message translates to:
  /// **'소요 {value} ms'**
  String tookMs(Object value);

  /// No description provided for @succeeded.
  ///
  /// In ko, this message translates to:
  /// **'성공'**
  String get succeeded;

  /// No description provided for @step.
  ///
  /// In ko, this message translates to:
  /// **'단계'**
  String get step;

  /// No description provided for @deviceStarted.
  ///
  /// In ko, this message translates to:
  /// **'장치 시작'**
  String get deviceStarted;

  /// No description provided for @activeDisplay.
  ///
  /// In ko, this message translates to:
  /// **'활성 화면'**
  String get activeDisplay;

  /// No description provided for @ownDevices.
  ///
  /// In ko, this message translates to:
  /// **'소유 장치'**
  String get ownDevices;

  /// No description provided for @waitingSetup.
  ///
  /// In ko, this message translates to:
  /// **'설정 대기'**
  String get waitingSetup;

  /// No description provided for @rdpSession.
  ///
  /// In ko, this message translates to:
  /// **'RDP 세션'**
  String get rdpSession;

  /// No description provided for @monitors.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수'**
  String get monitors;

  /// No description provided for @responded.
  ///
  /// In ko, this message translates to:
  /// **'응답 확인'**
  String get responded;

  /// No description provided for @changed.
  ///
  /// In ko, this message translates to:
  /// **'변경됨'**
  String get changed;

  /// No description provided for @deviceRestartTried.
  ///
  /// In ko, this message translates to:
  /// **'장치 다시 시작 시도'**
  String get deviceRestartTried;

  /// No description provided for @restore2.
  ///
  /// In ko, this message translates to:
  /// **'복원 대상'**
  String get restore2;

  /// No description provided for @notRestored.
  ///
  /// In ko, this message translates to:
  /// **'미복원'**
  String get notRestored;

  /// No description provided for @notRestoredVirtualMonitor.
  ///
  /// In ko, this message translates to:
  /// **'미복원(가상 모니터)'**
  String get notRestoredVirtualMonitor;

  /// No description provided for @backConsoleSession.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 복귀'**
  String get backConsoleSession;

  /// No description provided for @consoleSessionNeedsRestoring.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 복원 필요'**
  String get consoleSessionNeedsRestoring;

  /// No description provided for @consoleSessionActive.
  ///
  /// In ko, this message translates to:
  /// **'콘솔 활성'**
  String get consoleSessionActive;

  /// No description provided for @adapterBeforeRestart.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작 전 어댑터'**
  String get adapterBeforeRestart;

  /// No description provided for @adapterReconnected.
  ///
  /// In ko, this message translates to:
  /// **'어댑터 다시 연결'**
  String get adapterReconnected;

  /// No description provided for @skippedPackets.
  ///
  /// In ko, this message translates to:
  /// **'건너뛴 패킷'**
  String get skippedPackets;

  /// No description provided for @newVersionAvailable2.
  ///
  /// In ko, this message translates to:
  /// **'새 버전 있음'**
  String get newVersionAvailable2;

  /// No description provided for @desktopDrive.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 드라이브'**
  String get desktopDrive;

  /// No description provided for @fileRequests.
  ///
  /// In ko, this message translates to:
  /// **'파일 요청'**
  String get fileRequests;

  /// No description provided for @failedRequests.
  ///
  /// In ko, this message translates to:
  /// **'실패한 요청'**
  String get failedRequests;

  /// No description provided for @rememberedFolders.
  ///
  /// In ko, this message translates to:
  /// **'기억한 폴더'**
  String get rememberedFolders;

  /// No description provided for @reopenedFolders.
  ///
  /// In ko, this message translates to:
  /// **'다시 연 폴더'**
  String get reopenedFolders;

  /// No description provided for @connectedDevices.
  ///
  /// In ko, this message translates to:
  /// **'연결된 장치'**
  String get connectedDevices;

  /// No description provided for @handledRequests.
  ///
  /// In ko, this message translates to:
  /// **'처리한 요청'**
  String get handledRequests;

  /// No description provided for @kibTransferred.
  ///
  /// In ko, this message translates to:
  /// **'주고받은 KiB'**
  String get kibTransferred;

  /// No description provided for @deviceType.
  ///
  /// In ko, this message translates to:
  /// **'장치 종류'**
  String get deviceType;

  /// No description provided for @disconnectUpdate.
  ///
  /// In ko, this message translates to:
  /// **'연결을 끊고 업데이트할까요?'**
  String get disconnectUpdate;

  /// No description provided for @allOpenConnectionsClosedDesktops.
  ///
  /// In ko, this message translates to:
  /// **'열린 연결이 모두 끊겨요.\n데스크톱과 실행 중인 작업은 그대로 유지돼요.'**
  String get allOpenConnectionsClosedDesktops;

  /// No description provided for @disconnectUpdate2.
  ///
  /// In ko, this message translates to:
  /// **'연결 끊고 업데이트'**
  String get disconnectUpdate2;

  /// No description provided for @couldNotOpenHostUpdate.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트를 열지 못했어요. 로그인과 서버 연결을 확인하세요.'**
  String get couldNotOpenHostUpdate;

  /// No description provided for @download2.
  ///
  /// In ko, this message translates to:
  /// **'받기'**
  String get download2;

  /// No description provided for @installer.
  ///
  /// In ko, this message translates to:
  /// **'설치 창'**
  String get installer;

  /// No description provided for @networkDroppedSoDownloadResumes.
  ///
  /// In ko, this message translates to:
  /// **'네트워크가 끊겨 받은 부분부터 이어받고 있어요.'**
  String get networkDroppedSoDownloadResumes;

  /// No description provided for @checkingDownloadedFilesSizeSha2.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일의 크기, SHA-256, 설치 파일 버전을 확인하고 있어요.'**
  String get checkingDownloadedFilesSizeSha2;

  /// No description provided for @whenInstallerOpensFollowIts.
  ///
  /// In ko, this message translates to:
  /// **'설치 창이 열리면 안내에 따라 설치하세요.'**
  String get whenInstallerOpensFollowIts;

  /// No description provided for @onceInstallerFinishesItsChecks.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램이 확인을 마치면 Host 설정이 잠시 닫혔다가 새 버전으로 다시 열려요.'**
  String get onceInstallerFinishesItsChecks;

  /// No description provided for @onceInstallerFinishesItsChecks2.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램이 확인을 마치면 앱이 잠시 닫혔다가 같은 자리에서 새 버전으로 다시 열려요.'**
  String get onceInstallerFinishesItsChecks2;

  /// No description provided for @sizeMatchesBytes.
  ///
  /// In ko, this message translates to:
  /// **'크기 {size}바이트 일치'**
  String sizeMatchesBytes(Object size);

  /// No description provided for @sha256Matches.
  ///
  /// In ko, this message translates to:
  /// **'SHA-256 일치 · {digest}'**
  String sha256Matches(Object digest);

  /// No description provided for @installerVersionConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일 버전 {version} 확인'**
  String installerVersionConfirmed(Object version);

  /// No description provided for @multitapReleaseSignatureConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 배포 서명 확인'**
  String get multitapReleaseSignatureConfirmed;

  /// No description provided for @code3.
  ///
  /// In ko, this message translates to:
  /// **'코드 {code}'**
  String code3(Object code);

  /// No description provided for @hostUpdatesAvailable.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 {hostUpdates}대 가능'**
  String hostUpdatesAvailable(Object hostUpdates);

  /// No description provided for @whenInstallerOpensInstallContinues.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램을 열면 그 창에서 설치를 진행해요.'**
  String get whenInstallerOpensInstallContinues;

  /// No description provided for @hostSettingsClosesConnectionsEnd.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정이 닫히고 연결이 끊긴 뒤 Host 서비스가 다시 시작돼요.\nHost 등록·가상 화면 설정은 그대로예요.\nWindows는 자동으로 다시 시작하지 않아요.'**
  String get hostSettingsClosesConnectionsEnd;

  /// No description provided for @appClosesReopensSamePlace.
  ///
  /// In ko, this message translates to:
  /// **'앱이 닫혔다가 같은 자리에서 새 버전으로 열려요.\n열린 연결은 끊겨요.\n서버 주소·로그인 설정은 그대로예요.'**
  String get appClosesReopensSamePlace;

  /// No description provided for @checkingUpdates.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 확인 중'**
  String get checkingUpdates;

  /// No description provided for @youreUpDate.
  ///
  /// In ko, this message translates to:
  /// **'최신 버전이에요'**
  String get youreUpDate;

  /// No description provided for @noInstallerPublished.
  ///
  /// In ko, this message translates to:
  /// **'등록된 설치 파일이 없어요'**
  String get noInstallerPublished;

  /// No description provided for @newVersionAvailable3.
  ///
  /// In ko, this message translates to:
  /// **'새 버전이 있어요'**
  String get newVersionAvailable3;

  /// No description provided for @downloadingUpdate.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 받는 중'**
  String get downloadingUpdate;

  /// No description provided for @checkingFile2.
  ///
  /// In ko, this message translates to:
  /// **'파일 확인 중'**
  String get checkingFile2;

  /// No description provided for @updateReady.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 준비 완료'**
  String get updateReady;

  /// No description provided for @openingInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램 여는 중'**
  String get openingInstaller;

  /// No description provided for @applyingUpdate.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 적용 중'**
  String get applyingUpdate;

  /// No description provided for @installerOpened.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램을 열었어요'**
  String get installerOpened;

  /// No description provided for @current5.
  ///
  /// In ko, this message translates to:
  /// **'현재 {currentVersion}'**
  String current5(Object currentVersion);

  /// No description provided for @checkingReleaseInfoServer.
  ///
  /// In ko, this message translates to:
  /// **'설정한 서버의 배포 정보를 확인하고 있어요.'**
  String get checkingReleaseInfoServer;

  /// No description provided for @youCanUpdateOnceAdministrator.
  ///
  /// In ko, this message translates to:
  /// **'관리자가 서버에 설치 파일을 올리면 업데이트할 수 있어요.'**
  String get youCanUpdateOnceAdministrator;

  /// No description provided for @followStepsInstallerWindowYou.
  ///
  /// In ko, this message translates to:
  /// **'설치 창의 안내에 따라 설치하세요. 이 창은 닫아도 돼요.'**
  String get followStepsInstallerWindowYou;

  /// No description provided for @downloadUpdate.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 받기'**
  String get downloadUpdate;

  /// No description provided for @openInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램 열기'**
  String get openInstaller;

  /// No description provided for @restartUpdate.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작해서 업데이트'**
  String get restartUpdate;

  /// No description provided for @continueInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램으로 계속'**
  String get continueInstaller;

  /// No description provided for @installInstaller.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램으로 직접 설치'**
  String get installInstaller;

  /// No description provided for @minimize2.
  ///
  /// In ko, this message translates to:
  /// **'최소화'**
  String get minimize2;

  /// No description provided for @unknown5.
  ///
  /// In ko, this message translates to:
  /// **'확인 안 됨 · {detail}'**
  String unknown5(Object detail);

  /// No description provided for @usageReportedByHost.
  ///
  /// In ko, this message translates to:
  /// **'사용량 · Host 보고'**
  String get usageReportedByHost;

  /// No description provided for @usageReportedByProxmox.
  ///
  /// In ko, this message translates to:
  /// **'사용량 · Proxmox 보고'**
  String get usageReportedByProxmox;

  /// No description provided for @checking4.
  ///
  /// In ko, this message translates to:
  /// **'점검 중…'**
  String get checking4;

  /// No description provided for @couldNotCheck.
  ///
  /// In ko, this message translates to:
  /// **'점검하지 못했어요'**
  String get couldNotCheck;

  /// No description provided for @lastResult.
  ///
  /// In ko, this message translates to:
  /// **'마지막 결과'**
  String get lastResult;

  /// No description provided for @checked3.
  ///
  /// In ko, this message translates to:
  /// **'점검 시각'**
  String get checked3;

  /// No description provided for @statusServerGotFromHost.
  ///
  /// In ko, this message translates to:
  /// **'서버가 Host와 Proxmox에서 받은 상태'**
  String get statusServerGotFromHost;

  /// No description provided for @checkAgain3.
  ///
  /// In ko, this message translates to:
  /// **'다시 점검'**
  String get checkAgain3;

  /// No description provided for @noServerAddressSetSo.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소가 설정되지 않아 변경 내역을 불러올 수 없어요.'**
  String get noServerAddressSetSo;

  /// No description provided for @couldNotLoadServersChange.
  ///
  /// In ko, this message translates to:
  /// **'이 서버의 변경 내역을 불러오지 못했어요. 잠시 후 다시 시도하세요.'**
  String get couldNotLoadServersChange;

  /// No description provided for @noReleaseDate.
  ///
  /// In ko, this message translates to:
  /// **'배포일 정보 없음'**
  String get noReleaseDate;

  /// No description provided for @noChangesListedVersion.
  ///
  /// In ko, this message translates to:
  /// **'이 버전의 변경 내역이 등록되지 않았어요.'**
  String get noChangesListedVersion;

  /// No description provided for @text28.
  ///
  /// In ko, this message translates to:
  /// **'버전 {total}개'**
  String text28(num total);

  /// No description provided for @byVersion.
  ///
  /// In ko, this message translates to:
  /// **'버전별'**
  String get byVersion;

  /// No description provided for @releaseHistoryShowAll.
  ///
  /// In ko, this message translates to:
  /// **'전체 보기'**
  String get releaseHistoryShowAll;

  /// No description provided for @seeVersionListEachVersions.
  ///
  /// In ko, this message translates to:
  /// **'버전 목록과 그 버전의 내용을 나란히 봐요'**
  String get seeVersionListEachVersions;

  /// No description provided for @seeChangesAllVersionsOne.
  ///
  /// In ko, this message translates to:
  /// **'모든 버전의 내용을 한 번에 이어서 봐요'**
  String get seeChangesAllVersionsOne;

  /// No description provided for @loadingChangeList.
  ///
  /// In ko, this message translates to:
  /// **'변경 내역을 불러오고 있어요.'**
  String get loadingChangeList;

  /// No description provided for @serverHasNoChangeList.
  ///
  /// In ko, this message translates to:
  /// **'이 서버에 등록된 변경 내역이 없어요.'**
  String get serverHasNoChangeList;

  /// No description provided for @couldNotLoadEarlierVersions.
  ///
  /// In ko, this message translates to:
  /// **'이전 버전을 불러오지 못했어요 · 다시 시도'**
  String get couldNotLoadEarlierVersions;

  /// No description provided for @youveReachedFirstVersion.
  ///
  /// In ko, this message translates to:
  /// **'첫 버전까지 모두 봤어요'**
  String get youveReachedFirstVersion;

  /// No description provided for @noChangesListed.
  ///
  /// In ko, this message translates to:
  /// **'{note} · 내역 없음'**
  String noChangesListed(Object note);

  /// No description provided for @newerVersion.
  ///
  /// In ko, this message translates to:
  /// **'더 최신 버전 (↑)'**
  String get newerVersion;

  /// No description provided for @olderVersion.
  ///
  /// In ko, this message translates to:
  /// **'이전 버전 (↓)'**
  String get olderVersion;

  /// No description provided for @administratorApprovalNeeded2.
  ///
  /// In ko, this message translates to:
  /// **'관리자 허용이 필요해요'**
  String get administratorApprovalNeeded2;

  /// No description provided for @couldNotPrepareScreenFit.
  ///
  /// In ko, this message translates to:
  /// **'창 크기에 맞는 화면을 준비하지 못했어요. 다시 연결하세요.'**
  String get couldNotPrepareScreenFit;

  /// No description provided for @pressOkLockMouseInside.
  ///
  /// In ko, this message translates to:
  /// **'확인을 누르면 마우스를 데스크톱 안에 잠가요.\nCtrl+Shift+Alt+Z로 다시 끌 수 있어요.'**
  String get pressOkLockMouseInside;

  /// No description provided for @mouseLockedInsideDesktopCtrl.
  ///
  /// In ko, this message translates to:
  /// **'마우스를 데스크톱 안에 잠갔어요 · Ctrl+Shift+Alt+Z로 켜기·끄기'**
  String get mouseLockedInsideDesktopCtrl;

  /// No description provided for @couldNotApplyScreenMode.
  ///
  /// In ko, this message translates to:
  /// **'화면 모드를 적용하지 못했어요. 현재 연결은 유지돼요.'**
  String get couldNotApplyScreenMode;

  /// No description provided for @couldNotConfirmOriginalScreen.
  ///
  /// In ko, this message translates to:
  /// **'원래 화면 모드로 돌아갔는지 확인하지 못했어요. 툴바의 화면에서 다시 선택하세요.'**
  String get couldNotConfirmOriginalScreen;

  /// No description provided for @couldNotChangeScreenMode.
  ///
  /// In ko, this message translates to:
  /// **'화면 모드를 변경하지 못했어요. 다시 선택하세요.'**
  String get couldNotChangeScreenMode;

  /// No description provided for @filesBeingCopiedCancelCopy.
  ///
  /// In ko, this message translates to:
  /// **'파일 복사 중이에요. 복사를 취소한 뒤 연결을 끊으세요.'**
  String get filesBeingCopiedCancelCopy;

  /// No description provided for @desktopCannotPrintPc.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에서는 PC로 인쇄할 수 없어요. {reason}'**
  String desktopCannotPrintPc(Object reason);

  /// No description provided for @printerConnectionWasLostReconnect.
  ///
  /// In ko, this message translates to:
  /// **'프린터 연결이 끊겼어요. 아직 인쇄하지 않은 문서 {lostDocuments}개는 인쇄할 수 없어요. 데스크톱에 다시 연결한 뒤 다시 인쇄하세요.'**
  String printerConnectionWasLostReconnect(num lostDocuments);

  /// No description provided for @printerConnectionWasLostReconnect2.
  ///
  /// In ko, this message translates to:
  /// **'프린터 연결이 끊겼어요. 데스크톱에 다시 연결하면 인쇄할 수 있어요.'**
  String get printerConnectionWasLostReconnect2;

  /// No description provided for @printed.
  ///
  /// In ko, this message translates to:
  /// **'인쇄했어요'**
  String get printed;

  /// No description provided for @savedAsPdf.
  ///
  /// In ko, this message translates to:
  /// **'PDF로 저장했어요.'**
  String get savedAsPdf;

  /// No description provided for @printingCanceledPrintAgainFrom.
  ///
  /// In ko, this message translates to:
  /// **'인쇄를 취소했어요. 필요하면 데스크톱에서 다시 인쇄하세요.'**
  String get printingCanceledPrintAgainFrom;

  /// No description provided for @screenMode.
  ///
  /// In ko, this message translates to:
  /// **'화면 모드 · {shownLabel}'**
  String screenMode(Object shownLabel);

  /// No description provided for @text29.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 {mappedFolders}개 연결됨'**
  String text29(num mappedFolders);

  /// No description provided for @text30.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치 {sharedUsb}개 연결됨'**
  String text30(num sharedUsb);

  /// No description provided for @foldersUsbUsePcsFolders.
  ///
  /// In ko, this message translates to:
  /// **'폴더·USB · PC의 폴더와 USB 장치를 데스크톱에서 쓰기'**
  String get foldersUsbUsePcsFolders;

  /// No description provided for @sharedFoldersOpenPcFolders.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더 · PC 폴더를 데스크톱에서 열기'**
  String get sharedFoldersOpenPcFolders;

  /// No description provided for @usbDevicesUsePcsUsb.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치 · PC의 USB 장치를 데스크톱에서 쓰기'**
  String get usbDevicesUsePcsUsb;

  /// No description provided for @connected4.
  ///
  /// In ko, this message translates to:
  /// **'{mappedFolders}개 연결됨'**
  String connected4(Object mappedFolders);

  /// No description provided for @connected5.
  ///
  /// In ko, this message translates to:
  /// **'{sharedUsb}개 연결됨'**
  String connected5(Object sharedUsb);

  /// No description provided for @settingsPictureQualityGuide.
  ///
  /// In ko, this message translates to:
  /// **'설정 · 화질, 사용안내 등'**
  String get settingsPictureQualityGuide;

  /// No description provided for @youCanChangeWhenScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 전환이 끝나면 바꿀 수 있어요'**
  String get youCanChangeWhenScreen;

  /// No description provided for @guideOpen.
  ///
  /// In ko, this message translates to:
  /// **'사용안내가 열려 있어요'**
  String get guideOpen;

  /// No description provided for @availableOnceConnected2.
  ///
  /// In ko, this message translates to:
  /// **'연결된 뒤 볼 수 있어요'**
  String get availableOnceConnected2;

  /// No description provided for @filesBeingCopiedTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'파일 복사 중이에요. 끝난 뒤 다시 시도하세요.'**
  String get filesBeingCopiedTryAgain;

  /// No description provided for @couldNotReconnectUsbDevice.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치를 다시 연결하지 못했어요. 이 연결에서는 USB를 쓸 수 없어요.'**
  String get couldNotReconnectUsbDevice;

  /// No description provided for @sharedFoldersEndedByReconnect.
  ///
  /// In ko, this message translates to:
  /// **'다시 연결하면서 폴더 공유를 끝냈어요. 폴더·USB에서 다시 추가하세요.'**
  String get sharedFoldersEndedByReconnect;

  /// No description provided for @movedUsbDeviceAnotherWindow.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치를 다른 창으로 옮겼어요'**
  String get movedUsbDeviceAnotherWindow;

  /// No description provided for @movedUsbDeviceWindow.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치를 {to} 창으로 옮겼어요'**
  String movedUsbDeviceWindow(Object to);

  /// No description provided for @remoteResolutionPreparedAgainFit.
  ///
  /// In ko, this message translates to:
  /// **'창 크기에 맞춰 원격 해상도를 다시 준비해요.\n화면이 너무 크면 창 크기를 줄인 뒤 연결하세요.'**
  String get remoteResolutionPreparedAgainFit;

  /// No description provided for @reconnectMode.
  ///
  /// In ko, this message translates to:
  /// **'이 모드로 다시 연결'**
  String get reconnectMode;

  /// No description provided for @takeOverWindowsScreen.
  ///
  /// In ko, this message translates to:
  /// **'Windows 화면을 넘겨받을까요?'**
  String get takeOverWindowsScreen;

  /// No description provided for @currentWindowsScreenMovesStreaming.
  ///
  /// In ko, this message translates to:
  /// **'지금 Windows 화면을 스트리밍으로 가져와요.\nWindows 원격 데스크톱(RDP) 연결은 끊길 수 있어요. 실행 중인 작업은 그대로예요.'**
  String get currentWindowsScreenMovesStreaming;

  /// No description provided for @takeOverScreen.
  ///
  /// In ko, this message translates to:
  /// **'화면 넘겨받기'**
  String get takeOverScreen;

  /// No description provided for @connectionCanceled.
  ///
  /// In ko, this message translates to:
  /// **'연결을 취소했어요.'**
  String get connectionCanceled;

  /// No description provided for @couldNotConnectAllMonitors.
  ///
  /// In ko, this message translates to:
  /// **'모든 모니터로 연결하지 못했어요. 목록에서 창 모드나 전체 화면으로 연결하세요.'**
  String get couldNotConnectAllMonitors;

  /// No description provided for @couldNotApplyScreenMode2.
  ///
  /// In ko, this message translates to:
  /// **'화면 모드를 적용하지 못했어요. 다시 연결하세요.'**
  String get couldNotApplyScreenMode2;

  /// No description provided for @hostConnectionLostReconnecting.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결이 끊겨 다시 연결하고 있어요'**
  String get hostConnectionLostReconnecting;

  /// No description provided for @connectionLostReconnecting.
  ///
  /// In ko, this message translates to:
  /// **'연결이 끊겨 다시 연결하고 있어요'**
  String get connectionLostReconnecting;

  /// No description provided for @couldNotChangeScreenSize.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기를 바꾸지 못해 다시 연결하고 있어요'**
  String get couldNotChangeScreenSize;

  /// No description provided for @monitorSetupChangedDisconnectThen.
  ///
  /// In ko, this message translates to:
  /// **'모니터 구성이 변경됐어요. 연결을 끊은 뒤 화면 모드를 다시 선택하세요.'**
  String get monitorSetupChangedDisconnectThen;

  /// No description provided for @cantShowScreenOnMonitors.
  ///
  /// In ko, this message translates to:
  /// **'화면을 표시할 수 없어요'**
  String get cantShowScreenOnMonitors;

  /// No description provided for @text31.
  ///
  /// In ko, this message translates to:
  /// **'모니터를 {activeCount}대로 바꿨어요'**
  String text31(num activeCount);

  /// No description provided for @disconnected4.
  ///
  /// In ko, this message translates to:
  /// **'연결을 끊었어요'**
  String get disconnected4;

  /// No description provided for @reconnectPickUpWhereYou.
  ///
  /// In ko, this message translates to:
  /// **'다시 연결하면 이어서 쓸 수 있어요.'**
  String get reconnectPickUpWhereYou;

  /// No description provided for @checkNumberMonitorsDesktopList.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 목록에서 모니터 수를 확인하세요.'**
  String get checkNumberMonitorsDesktopList;

  /// No description provided for @reconnect2.
  ///
  /// In ko, this message translates to:
  /// **'다시 연결…'**
  String get reconnect2;

  /// No description provided for @hostUpdating2.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트 중이에요'**
  String get hostUpdating2;

  /// No description provided for @reconnectsAutomaticallyWhenDoneWaits.
  ///
  /// In ko, this message translates to:
  /// **'끝나면 자동으로 다시 연결해요.\n최대 10분 기다려요.'**
  String get reconnectsAutomaticallyWhenDoneWaits;

  /// No description provided for @pictureQuality.
  ///
  /// In ko, this message translates to:
  /// **'화질'**
  String get pictureQuality;

  /// No description provided for @connectedSomewhereElse.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳에서 연결 중이에요'**
  String get connectedSomewhereElse;

  /// No description provided for @ifWindowsRestartingConnectsAs.
  ///
  /// In ko, this message translates to:
  /// **'Windows가 다시 시작되는 중이면 끝나는 대로 연결해요.\n최대 3분 기다려요.'**
  String get ifWindowsRestartingConnectsAs;

  /// No description provided for @connectsAutomaticallyWhenDesktopBack.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 다시 켜지면 자동으로 연결해요.\n최대 3분 기다려요.'**
  String get connectsAutomaticallyWhenDesktopBack;

  /// No description provided for @cancelConnection.
  ///
  /// In ko, this message translates to:
  /// **'연결 취소'**
  String get cancelConnection;

  /// No description provided for @preparingScreenFitWindow.
  ///
  /// In ko, this message translates to:
  /// **'창 크기에 맞는 화면을 준비하는 중'**
  String get preparingScreenFitWindow;

  /// No description provided for @checkingAccountPermissionsHostConnection.
  ///
  /// In ko, this message translates to:
  /// **'계정 권한과 Host 연결을 확인하고 있어요.'**
  String get checkingAccountPermissionsHostConnection;

  /// No description provided for @fittingScreenSize2.
  ///
  /// In ko, this message translates to:
  /// **'화면 크기를 맞추고 있어요.'**
  String get fittingScreenSize2;

  /// No description provided for @preparingHostConnection.
  ///
  /// In ko, this message translates to:
  /// **'Host 연결을 준비하고 있어요.'**
  String get preparingHostConnection;

  /// No description provided for @connectionCantMatchResolutionWindow.
  ///
  /// In ko, this message translates to:
  /// **'이 연결에서는 해상도를 창 크기에 맞출 수 없어요. 관리자에게 알려 주세요.'**
  String get connectionCantMatchResolutionWindow;

  /// No description provided for @matchesResolutionWindowSizeAgain.
  ///
  /// In ko, this message translates to:
  /// **'창 크기에 맞춰 해상도를 다시 바꿔요.'**
  String get matchesResolutionWindowSizeAgain;

  /// No description provided for @retryResolution.
  ///
  /// In ko, this message translates to:
  /// **'해상도 다시 시도'**
  String get retryResolution;

  /// No description provided for @fixedResolution.
  ///
  /// In ko, this message translates to:
  /// **'해상도 고정'**
  String get fixedResolution;

  /// No description provided for @checkingPcsMonitors.
  ///
  /// In ko, this message translates to:
  /// **'PC의 모니터 확인 중'**
  String get checkingPcsMonitors;

  /// No description provided for @applyingWindowMode.
  ///
  /// In ko, this message translates to:
  /// **'화면 모드 적용 중'**
  String get applyingWindowMode;

  /// No description provided for @updateStepVerify.
  ///
  /// In ko, this message translates to:
  /// **'확인'**
  String get updateStepVerify;

  /// No description provided for @closingPreviousConnection.
  ///
  /// In ko, this message translates to:
  /// **'이전 연결 정리 중'**
  String get closingPreviousConnection;

  /// No description provided for @waitingHostScreen2.
  ///
  /// In ko, this message translates to:
  /// **'Host 화면 준비 기다리는 중'**
  String get waitingHostScreen2;

  /// No description provided for @settingUpRemoteMonitors.
  ///
  /// In ko, this message translates to:
  /// **'원격 모니터 구성 중'**
  String get settingUpRemoteMonitors;

  /// No description provided for @connectingNewScreen.
  ///
  /// In ko, this message translates to:
  /// **'새 화면 연결 중'**
  String get connectingNewScreen;

  /// No description provided for @waitingFirstFrame3.
  ///
  /// In ko, this message translates to:
  /// **'첫 화면 기다리는 중'**
  String get waitingFirstFrame3;

  /// No description provided for @restoringPreviousWindowMode.
  ///
  /// In ko, this message translates to:
  /// **'이전 창 모드 복원 중'**
  String get restoringPreviousWindowMode;

  /// No description provided for @switching.
  ///
  /// In ko, this message translates to:
  /// **'{target} 전환 중'**
  String switching(Object target);

  /// No description provided for @cancellingSwitch.
  ///
  /// In ko, this message translates to:
  /// **'전환 취소 중'**
  String get cancellingSwitch;

  /// No description provided for @youCanTypeOnceNew.
  ///
  /// In ko, this message translates to:
  /// **'새 화면이 준비되면 입력할 수 있어요.'**
  String get youCanTypeOnceNew;

  /// No description provided for @cancelSwitch.
  ///
  /// In ko, this message translates to:
  /// **'전환 취소'**
  String get cancelSwitch;

  /// No description provided for @tryAgainMoment.
  ///
  /// In ko, this message translates to:
  /// **'잠시 후 다시 시도하세요.'**
  String get tryAgainMoment;

  /// No description provided for @tryAgain2.
  ///
  /// In ko, this message translates to:
  /// **'다시 시도하세요.'**
  String get tryAgain2;

  /// No description provided for @saving2.
  ///
  /// In ko, this message translates to:
  /// **'저장 중…'**
  String get saving2;

  /// No description provided for @connectionConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'연결이 확인됐어요'**
  String get connectionConfirmed;

  /// No description provided for @couldntSaveServerAddress2.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소를 저장하지 못했어요'**
  String get couldntSaveServerAddress2;

  /// No description provided for @couldntConfirmConnection.
  ///
  /// In ko, this message translates to:
  /// **'연결을 확인하지 못했어요'**
  String get couldntConfirmConnection;

  /// No description provided for @checkingServerResponse.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답을 확인하고 있어요'**
  String get checkingServerResponse;

  /// No description provided for @checkConnection.
  ///
  /// In ko, this message translates to:
  /// **'연결을 확인해 주세요'**
  String get checkConnection;

  /// No description provided for @multitapResponseTimeMs.
  ///
  /// In ko, this message translates to:
  /// **'Multitap {version} · 응답 시간 {elapsedMs} ms'**
  String multitapResponseTimeMs(Object version, Object elapsedMs);

  /// No description provided for @pleaseWait.
  ///
  /// In ko, this message translates to:
  /// **'잠시만 기다려 주세요.'**
  String get pleaseWait;

  /// No description provided for @addressServerYouSign.
  ///
  /// In ko, this message translates to:
  /// **'로그인할 서버의 주소'**
  String get addressServerYouSign;

  /// No description provided for @account3.
  ///
  /// In ko, this message translates to:
  /// **'계정: {username}'**
  String account3(Object username);

  /// No description provided for @text32.
  ///
  /// In ko, this message translates to:
  /// **'기존 로그인 {sessionCount}개'**
  String text32(num sessionCount);

  /// No description provided for @signed2.
  ///
  /// In ko, this message translates to:
  /// **'로그인 {createdAt}'**
  String signed2(Object createdAt);

  /// No description provided for @connectedDesktops.
  ///
  /// In ko, this message translates to:
  /// **'연결된 데스크톱: {session}'**
  String connectedDesktops(Object session);

  /// No description provided for @signingHereEndsStreamingConsole.
  ///
  /// In ko, this message translates to:
  /// **'로그인하면 기존 로그인의 스트리밍·Console 연결이 끊겨요.'**
  String get signingHereEndsStreamingConsole;

  /// No description provided for @recoveryCodeHasBeenUsed.
  ///
  /// In ko, this message translates to:
  /// **'복구 코드는 사용됐어요. 취소해도 다시 쓸 수 없어요.'**
  String get recoveryCodeHasBeenUsed;

  /// No description provided for @signOutElsewhereSignHere.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳에서 로그아웃하고 여기서 로그인할까요?'**
  String get signOutElsewhereSignHere;

  /// No description provided for @signOutSign.
  ///
  /// In ko, this message translates to:
  /// **'로그아웃하고 로그인'**
  String get signOutSign;

  /// No description provided for @signedOutMultitap.
  ///
  /// In ko, this message translates to:
  /// **'Multitap에서 로그아웃됐어요'**
  String get signedOutMultitap;

  /// No description provided for @notYetConfirmedAllConnections.
  ///
  /// In ko, this message translates to:
  /// **'연결이 모두 끊겼는지 아직 확인하지 못했어요. ‘연결 끊김 다시 확인’을 누르세요.'**
  String get notYetConfirmedAllConnections;

  /// No description provided for @couldntConfirmAllConnectionsClosed.
  ///
  /// In ko, this message translates to:
  /// **'연결이 모두 끊겼는지 확인하지 못했어요. Multitap을 종료한 뒤 다시 여세요.'**
  String get couldntConfirmAllConnectionsClosed;

  /// No description provided for @checking5.
  ///
  /// In ko, this message translates to:
  /// **'확인하는 중…'**
  String get checking5;

  /// No description provided for @checkAgain4.
  ///
  /// In ko, this message translates to:
  /// **'연결 끊김 다시 확인'**
  String get checkAgain4;

  /// No description provided for @youllSignedOutSoonIf.
  ///
  /// In ko, this message translates to:
  /// **'입력이 없으면 곧 로그아웃돼요. 연결도 모두 끊겨요.'**
  String get youllSignedOutSoonIf;

  /// No description provided for @youllSignedOutInSeconds.
  ///
  /// In ko, this message translates to:
  /// **'입력이 없으면 {seconds}초 뒤 로그아웃돼요. 연결도 모두 끊겨요.'**
  String youllSignedOutInSeconds(Object seconds);

  /// No description provided for @keepUsing.
  ///
  /// In ko, this message translates to:
  /// **'계속 사용'**
  String get keepUsing;

  /// No description provided for @checkingAutoSignOut.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그아웃 확인 중'**
  String get checkingAutoSignOut;

  /// No description provided for @autoSignOutOff.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그아웃 꺼짐'**
  String get autoSignOutOff;

  /// No description provided for @autoSignOut.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그아웃 {left}'**
  String autoSignOut(Object left);

  /// No description provided for @youStaySignedWithoutInput.
  ///
  /// In ko, this message translates to:
  /// **'입력이 없어도 로그아웃하지 않아요. 다른 곳에서 로그인하거나 로그인이 만료되면 로그아웃돼요.'**
  String get youStaySignedWithoutInput;

  /// No description provided for @signsYouOutMultitapWhen.
  ///
  /// In ko, this message translates to:
  /// **'키보드·마우스·터치 입력이 없으면 Multitap에서 로그아웃해요.'**
  String get signsYouOutMultitapWhen;

  /// No description provided for @cantCheckServerSetting.
  ///
  /// In ko, this message translates to:
  /// **'서버 설정을 확인할 수 없어요.'**
  String get cantCheckServerSetting;

  /// No description provided for @changingSetting.
  ///
  /// In ko, this message translates to:
  /// **'설정을 바꾸는 중이에요.'**
  String get changingSetting;

  /// No description provided for @youStaySignedWithoutInput2.
  ///
  /// In ko, this message translates to:
  /// **'입력이 없어도 로그아웃하지 않아요.'**
  String get youStaySignedWithoutInput2;

  /// No description provided for @checkingTimeLeft.
  ///
  /// In ko, this message translates to:
  /// **'남은 시간을 확인하고 있어요.'**
  String get checkingTimeLeft;

  /// No description provided for @withoutInputYoureSignedOut.
  ///
  /// In ko, this message translates to:
  /// **'입력이 없으면 {left} 후 로그아웃해요.'**
  String withoutInputYoureSignedOut(Object left);

  /// No description provided for @autoSignOut2.
  ///
  /// In ko, this message translates to:
  /// **'자동 로그아웃'**
  String get autoSignOut2;

  /// No description provided for @couldntChangeSettingTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'설정을 바꾸지 못했어요. 다시 시도해 주세요.'**
  String get couldntChangeSettingTryAgain;

  /// No description provided for @couldntConfirmScreenWasRestored.
  ///
  /// In ko, this message translates to:
  /// **'화면 복원을 확인하지 못했어요'**
  String get couldntConfirmScreenWasRestored;

  /// No description provided for @connectionsBlockedUntilRestoreConfirmed.
  ///
  /// In ko, this message translates to:
  /// **'{cleanupCause}\n복원이 확인될 때까지 연결을 막아 둬요.'**
  String connectionsBlockedUntilRestoreConfirmed(Object cleanupCause);

  /// No description provided for @statusRecorded.
  ///
  /// In ko, this message translates to:
  /// **'{cleanupEvidence} · 상태 기록 {at}'**
  String statusRecorded(Object cleanupEvidence, Object at);

  /// No description provided for @waiting3.
  ///
  /// In ko, this message translates to:
  /// **'연결 대기'**
  String get waiting3;

  /// No description provided for @received.
  ///
  /// In ko, this message translates to:
  /// **'수신'**
  String get received;

  /// No description provided for @connectionSlowLowerPictureQuality.
  ///
  /// In ko, this message translates to:
  /// **'연결이 느려요. 화질을 ‘절약’으로 낮추면 덜 끊겨요.'**
  String get connectionSlowLowerPictureQuality;

  /// No description provided for @reconnectSaver.
  ///
  /// In ko, this message translates to:
  /// **'절약으로 다시 연결'**
  String get reconnectSaver;

  /// No description provided for @showConnectionStats.
  ///
  /// In ko, this message translates to:
  /// **'연결 통계 보기'**
  String get showConnectionStats;

  /// No description provided for @toolbarHere.
  ///
  /// In ko, this message translates to:
  /// **'툴바는 여기 있어요.'**
  String get toolbarHere;

  /// No description provided for @changeScreenModeScaleSettings.
  ///
  /// In ko, this message translates to:
  /// **'화면 모드, 배율, 설정을 여기서 바꿔요.\n손잡이를 누르면 접히고, 다시 누르면 열려요.'**
  String get changeScreenModeScaleSettings;

  /// No description provided for @guide.
  ///
  /// In ko, this message translates to:
  /// **'사용안내'**
  String get guide;

  /// No description provided for @featuresAvailableConnection.
  ///
  /// In ko, this message translates to:
  /// **'이 연결에서 쓸 수 있는 기능이에요.'**
  String get featuresAvailableConnection;

  /// No description provided for @dontShowAgainDesktop.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에서 다시 보지 않기'**
  String get dontShowAgainDesktop;

  /// No description provided for @youCanSeeAgainUnder.
  ///
  /// In ko, this message translates to:
  /// **'설정의 사용안내에서 다시 볼 수 있어요.'**
  String get youCanSeeAgainUnder;

  /// No description provided for @closeGuideEsc.
  ///
  /// In ko, this message translates to:
  /// **'사용안내 닫기 (Esc)'**
  String get closeGuideEsc;

  /// No description provided for @good.
  ///
  /// In ko, this message translates to:
  /// **'좋음'**
  String get good;

  /// No description provided for @fair.
  ///
  /// In ko, this message translates to:
  /// **'보통'**
  String get fair;

  /// No description provided for @slow.
  ///
  /// In ko, this message translates to:
  /// **'느림'**
  String get slow;

  /// No description provided for @monitors2.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수…'**
  String get monitors2;

  /// No description provided for @windowsTextIconSize.
  ///
  /// In ko, this message translates to:
  /// **'Windows 글자·아이콘 크기'**
  String get windowsTextIconSize;

  /// No description provided for @cantUseSharedFolders.
  ///
  /// In ko, this message translates to:
  /// **'공유 폴더를 쓸 수 없어요.{why}'**
  String cantUseSharedFolders(Object why);

  /// No description provided for @cantUseUsbDevices.
  ///
  /// In ko, this message translates to:
  /// **'USB 장치를 쓸 수 없어요.{why}'**
  String cantUseUsbDevices(Object why);

  /// No description provided for @printerUsbSharedFoldersCopy.
  ///
  /// In ko, this message translates to:
  /// **'프린터 · USB · 공유 폴더 · 복사'**
  String get printerUsbSharedFoldersCopy;

  /// No description provided for @turnOffUsbMouse.
  ///
  /// In ko, this message translates to:
  /// **'로 꺼요 · USB 마우스'**
  String get turnOffUsbMouse;

  /// No description provided for @turnOffNormalInput.
  ///
  /// In ko, this message translates to:
  /// **'로 꺼요 · 일반 입력'**
  String get turnOffNormalInput;

  /// No description provided for @turnOff.
  ///
  /// In ko, this message translates to:
  /// **'로 켜고 꺼요'**
  String get turnOff;

  /// No description provided for @staysOpenWhenYouClick.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 화면을 눌러도 접지 않아요'**
  String get staysOpenWhenYouClick;

  /// No description provided for @notConnected.
  ///
  /// In ko, this message translates to:
  /// **'연결되지 않음'**
  String get notConnected;

  /// No description provided for @hideDetails2.
  ///
  /// In ko, this message translates to:
  /// **'상세 정보 숨기기'**
  String get hideDetails2;

  /// No description provided for @showDetails.
  ///
  /// In ko, this message translates to:
  /// **'상세 정보 표시'**
  String get showDetails;

  /// No description provided for @remoteScreenOpensHere.
  ///
  /// In ko, this message translates to:
  /// **'여기에 원격 화면이 열려요'**
  String get remoteScreenOpensHere;

  /// No description provided for @designDemoNoRealConnection.
  ///
  /// In ko, this message translates to:
  /// **'디자인 데모 · 실제 연결 없음'**
  String get designDemoNoRealConnection;

  /// No description provided for @notConnected2.
  ///
  /// In ko, this message translates to:
  /// **'미연결'**
  String get notConnected2;

  /// No description provided for @frameRate.
  ///
  /// In ko, this message translates to:
  /// **'프레임 속도'**
  String get frameRate;

  /// No description provided for @latency.
  ///
  /// In ko, this message translates to:
  /// **'지연'**
  String get latency;

  /// No description provided for @input.
  ///
  /// In ko, this message translates to:
  /// **'입력 전달'**
  String get input;

  /// No description provided for @inactive.
  ///
  /// In ko, this message translates to:
  /// **'비활성'**
  String get inactive;

  /// No description provided for @backDesktopList.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 목록으로'**
  String get backDesktopList;

  /// No description provided for @copyDiagnostics2.
  ///
  /// In ko, this message translates to:
  /// **'진단 복사'**
  String get copyDiagnostics2;

  /// No description provided for @detailedDiagnostics.
  ///
  /// In ko, this message translates to:
  /// **'자세한 진단'**
  String get detailedDiagnostics;

  /// No description provided for @copyJson.
  ///
  /// In ko, this message translates to:
  /// **'JSON 복사'**
  String get copyJson;

  /// No description provided for @connecting3.
  ///
  /// In ko, this message translates to:
  /// **'연결 중'**
  String get connecting3;

  /// No description provided for @notConnected3.
  ///
  /// In ko, this message translates to:
  /// **'연결 안 됨'**
  String get notConnected3;

  /// No description provided for @connectingDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 연결하는 중'**
  String get connectingDesktop;

  /// No description provided for @notConnected4.
  ///
  /// In ko, this message translates to:
  /// **'연결돼 있지 않아요'**
  String get notConnected4;

  /// No description provided for @latencyUnder1Ms.
  ///
  /// In ko, this message translates to:
  /// **'지연 1 ms 미만'**
  String get latencyUnder1Ms;

  /// No description provided for @latencyAboutMs.
  ///
  /// In ko, this message translates to:
  /// **'지연 약 {value} ms'**
  String latencyAboutMs(Object value);

  /// No description provided for @connectionQuality.
  ///
  /// In ko, this message translates to:
  /// **'연결 품질'**
  String get connectionQuality;

  /// No description provided for @connectionQuality2.
  ///
  /// In ko, this message translates to:
  /// **'연결 품질 · {word}'**
  String connectionQuality2(Object word);

  /// No description provided for @connectionQuality3.
  ///
  /// In ko, this message translates to:
  /// **'연결 품질 · {summary}'**
  String connectionQuality3(Object summary);

  /// No description provided for @menuScreenSettingsMore.
  ///
  /// In ko, this message translates to:
  /// **'메뉴 · 화면, 설정 등'**
  String get menuScreenSettingsMore;

  /// No description provided for @toolbar.
  ///
  /// In ko, this message translates to:
  /// **'툴바'**
  String get toolbar;

  /// No description provided for @window.
  ///
  /// In ko, this message translates to:
  /// **'창'**
  String get window;

  /// No description provided for @release.
  ///
  /// In ko, this message translates to:
  /// **'로 풀기'**
  String get release;

  /// No description provided for @slow2.
  ///
  /// In ko, this message translates to:
  /// **'{latencyText} · 느림'**
  String slow2(Object latencyText);

  /// No description provided for @n999MsSlow.
  ///
  /// In ko, this message translates to:
  /// **'999 ms · 느림'**
  String get n999MsSlow;

  /// No description provided for @clickOpenToolbar.
  ///
  /// In ko, this message translates to:
  /// **'{summary} · 눌러서 툴바 열기'**
  String clickOpenToolbar(Object summary);

  /// No description provided for @foldToolbarEsc.
  ///
  /// In ko, this message translates to:
  /// **'툴바 접기 (Esc)'**
  String get foldToolbarEsc;

  /// No description provided for @foldersUsb.
  ///
  /// In ko, this message translates to:
  /// **'폴더·USB'**
  String get foldersUsb;

  /// No description provided for @keepToolbarOpen.
  ///
  /// In ko, this message translates to:
  /// **'툴바 열어 두기'**
  String get keepToolbarOpen;

  /// No description provided for @windowed2.
  ///
  /// In ko, this message translates to:
  /// **'창 모드로'**
  String get windowed2;

  /// No description provided for @resolutionRemoteScreen.
  ///
  /// In ko, this message translates to:
  /// **'해상도 · 원격 화면'**
  String get resolutionRemoteScreen;

  /// No description provided for @screenDecoding.
  ///
  /// In ko, this message translates to:
  /// **'화면 · {device} 디코딩'**
  String screenDecoding(Object device);

  /// No description provided for @receivedIncomingVideo.
  ///
  /// In ko, this message translates to:
  /// **'수신 · 받은 영상'**
  String get receivedIncomingVideo;

  /// No description provided for @videoPath.
  ///
  /// In ko, this message translates to:
  /// **'영상 경로'**
  String get videoPath;

  /// No description provided for @encodingHost.
  ///
  /// In ko, this message translates to:
  /// **'인코딩 · Host 처리'**
  String get encodingHost;

  /// No description provided for @decoding.
  ///
  /// In ko, this message translates to:
  /// **'디코딩 · {device} 처리'**
  String decoding(Object device);

  /// No description provided for @couldntGetDesktopConnectionInfo.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 연결 정보를 확인하지 못했어요.'**
  String get couldntGetDesktopConnectionInfo;

  /// No description provided for @disconnectedClickCloseRemoteWindow.
  ///
  /// In ko, this message translates to:
  /// **'연결을 끊었어요. ‘원격 창 닫기’를 다시 누르세요.'**
  String get disconnectedClickCloseRemoteWindow;

  /// No description provided for @checkingDesktopConnectionInfo.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 연결 정보를 확인하고 있어요.'**
  String get checkingDesktopConnectionInfo;

  /// No description provided for @product.
  ///
  /// In ko, this message translates to:
  /// **'제품'**
  String get product;

  /// No description provided for @couldntLoadSavedSignInfo.
  ///
  /// In ko, this message translates to:
  /// **'저장한 로그인 정보를 불러오지 못했어요.'**
  String get couldntLoadSavedSignInfo;

  /// No description provided for @couldntRememberUsernameYouCan.
  ///
  /// In ko, this message translates to:
  /// **'아이디를 기억하지 못했어요. 로그인은 그대로 할 수 있어요.'**
  String get couldntRememberUsernameYouCan;

  /// No description provided for @couldntTurnOffRememberPassword2.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호 기억을 해제하지 못했어요. 다시 시도해 주세요.'**
  String get couldntTurnOffRememberPassword2;

  /// No description provided for @couldntConfirmRememberSignSetting.
  ///
  /// In ko, this message translates to:
  /// **'로그인 정보 기억 설정의 저장 결과를 확인하지 못했어요.'**
  String get couldntConfirmRememberSignSetting;

  /// No description provided for @couldntSaveRememberSignSetting.
  ///
  /// In ko, this message translates to:
  /// **'로그인 정보 기억 설정을 저장하지 못했어요.'**
  String get couldntSaveRememberSignSetting;

  /// No description provided for @changeServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소 변경'**
  String get changeServerAddress;

  /// No description provided for @setServerAddress.
  ///
  /// In ko, this message translates to:
  /// **'서버 주소 설정'**
  String get setServerAddress;

  /// No description provided for @saveWindowsCredentialManager.
  ///
  /// In ko, this message translates to:
  /// **'Windows 자격 증명에 저장'**
  String get saveWindowsCredentialManager;

  /// No description provided for @setUpServerStart.
  ///
  /// In ko, this message translates to:
  /// **'서버 설정하고 시작'**
  String get setUpServerStart;

  /// No description provided for @exploreDemo.
  ///
  /// In ko, this message translates to:
  /// **'데모 화면 둘러보기'**
  String get exploreDemo;

  /// No description provided for @collectingDetailedDiagnostics.
  ///
  /// In ko, this message translates to:
  /// **'상세 진단 수집 중'**
  String get collectingDetailedDiagnostics;

  /// No description provided for @hostDetailsApplyFromNext.
  ///
  /// In ko, this message translates to:
  /// **'Host 상세 정보는 다음 연결부터 적용'**
  String get hostDetailsApplyFromNext;

  /// No description provided for @viaTcpServerAppRound.
  ///
  /// In ko, this message translates to:
  /// **'TCP 서버 경유 · 앱 왕복 {rttMs} ms\n디코딩 {clientDecodeMs} ms · 큐 {clientQueueMs} ms\n{current}'**
  String viaTcpServerAppRound(
    Object rttMs,
    Object clientDecodeMs,
    Object clientQueueMs,
    Object current,
  );

  /// No description provided for @notMeasured2.
  ///
  /// In ko, this message translates to:
  /// **'측정 안 함'**
  String get notMeasured2;

  /// No description provided for @p50P95Ms.
  ///
  /// In ko, this message translates to:
  /// **'p50 {p50Ms} / p95 {p95Ms} ms · {samples}회'**
  String p50P95Ms(Object p50Ms, Object p95Ms, num samples);

  /// No description provided for @inputHostApplied.
  ///
  /// In ko, this message translates to:
  /// **'입력→Host 적용'**
  String get inputHostApplied;

  /// No description provided for @inputScreenShown.
  ///
  /// In ko, this message translates to:
  /// **'입력→화면 표시'**
  String get inputScreenShown;

  /// No description provided for @hostReceivedApplied.
  ///
  /// In ko, this message translates to:
  /// **'Host 수신→적용'**
  String get hostReceivedApplied;

  /// No description provided for @inputAckRoundTrip.
  ///
  /// In ko, this message translates to:
  /// **'입력 확인 왕복'**
  String get inputAckRoundTrip;

  /// No description provided for @currentServerHostDoesntSupport.
  ///
  /// In ko, this message translates to:
  /// **'현재 서버 또는 Host가 입력 지연 측정을 지원하지 않아요.'**
  String get currentServerHostDoesntSupport;

  /// No description provided for @samplesFromLast30Seconds.
  ///
  /// In ko, this message translates to:
  /// **'최근 30초 표본이에요. 입력→Host 적용은 측정한 왕복 시간에서 Host 대기 시간을 뺀 절반(추정)에 Host가 측정한 수신→SendInput 반환 시간을 더한 값이에요. 입력→화면 표시는 입력 송신부터 그 뒤 처음 바뀐 원격 화면이 PC에서 디코딩돼 텍스처로 전달될 때까지이며, 모니터 표시 시간은 포함하지 않아요.'**
  String get samplesFromLast30Seconds;

  /// No description provided for @fpsPerScreen.
  ///
  /// In ko, this message translates to:
  /// **'fps (화면당)'**
  String get fpsPerScreen;

  /// No description provided for @status.
  ///
  /// In ko, this message translates to:
  /// **'상태'**
  String get status;

  /// No description provided for @encodingScale.
  ///
  /// In ko, this message translates to:
  /// **'인코딩 배율'**
  String get encodingScale;

  /// No description provided for @autoResolution.
  ///
  /// In ko, this message translates to:
  /// **'자동 해상도'**
  String get autoResolution;

  /// No description provided for @notSupportedConnection.
  ///
  /// In ko, this message translates to:
  /// **'현재 연결에서 지원하지 않음'**
  String get notSupportedConnection;

  /// No description provided for @applyingRemoteScreen.
  ///
  /// In ko, this message translates to:
  /// **'원격 화면에 적용 중'**
  String get applyingRemoteScreen;

  /// No description provided for @requestedScreen.
  ///
  /// In ko, this message translates to:
  /// **'요청 화면'**
  String get requestedScreen;

  /// No description provided for @auto2.
  ///
  /// In ko, this message translates to:
  /// **'자동 · {desired}'**
  String auto2(Object desired);

  /// No description provided for @targetFps.
  ///
  /// In ko, this message translates to:
  /// **'목표 FPS'**
  String get targetFps;

  /// No description provided for @requestedApplied.
  ///
  /// In ko, this message translates to:
  /// **'요청 {fps} / 적용 {session}'**
  String requestedApplied(Object fps, Object session);

  /// No description provided for @targetBitrate.
  ///
  /// In ko, this message translates to:
  /// **'목표 비트레이트'**
  String get targetBitrate;

  /// No description provided for @autoNowMaxMbps.
  ///
  /// In ko, this message translates to:
  /// **'자동 · 현재 {bitrateTarget} / 최대 {bitrateMax} Mbps'**
  String autoNowMaxMbps(Object bitrateTarget, Object bitrateMax);

  /// No description provided for @requestedAppliedMbps.
  ///
  /// In ko, this message translates to:
  /// **'요청 {options} / 적용 {session} Mbps'**
  String requestedAppliedMbps(Object options, Object session);

  /// No description provided for @captureMethod.
  ///
  /// In ko, this message translates to:
  /// **'캡처 방식'**
  String get captureMethod;

  /// No description provided for @hostGpuUse.
  ///
  /// In ko, this message translates to:
  /// **'사용 중인 Host GPU'**
  String get hostGpuUse;

  /// No description provided for @selectedHostGpu.
  ///
  /// In ko, this message translates to:
  /// **'선택한 Host GPU'**
  String get selectedHostGpu;

  /// No description provided for @encoderFallbackReason.
  ///
  /// In ko, this message translates to:
  /// **'인코더 전환 이유'**
  String get encoderFallbackReason;

  /// No description provided for @losslessSharpening.
  ///
  /// In ko, this message translates to:
  /// **'무손실 선명화'**
  String get losslessSharpening;

  /// No description provided for @off5.
  ///
  /// In ko, this message translates to:
  /// **'사용 안 함'**
  String get off5;

  /// No description provided for @waitingAppliesWhenScreenStill.
  ///
  /// In ko, this message translates to:
  /// **'대기 · 화면이 멈추면 적용'**
  String get waitingAppliesWhenScreenStill;

  /// No description provided for @active.
  ///
  /// In ko, this message translates to:
  /// **'적용 중 · 타일 {losslessTiles}개{session}'**
  String active(num losslessTiles, Object session);

  /// No description provided for @drawnPcCursorV1.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 표시 (cursor-v1)'**
  String get drawnPcCursorV1;

  /// No description provided for @hostEncoding.
  ///
  /// In ko, this message translates to:
  /// **'Host 인코딩'**
  String get hostEncoding;

  /// No description provided for @pcDecoding.
  ///
  /// In ko, this message translates to:
  /// **'PC 디코딩'**
  String get pcDecoding;

  /// No description provided for @pcDecoder.
  ///
  /// In ko, this message translates to:
  /// **'PC 디코더'**
  String get pcDecoder;

  /// No description provided for @otherAdapter.
  ///
  /// In ko, this message translates to:
  /// **' · 다른 어댑터'**
  String get otherAdapter;

  /// No description provided for @hardwareGpu.
  ///
  /// In ko, this message translates to:
  /// **'하드웨어 (GPU{session}{decodeVendor})'**
  String hardwareGpu(Object session, Object decodeVendor);

  /// No description provided for @softwareCpu.
  ///
  /// In ko, this message translates to:
  /// **'소프트웨어 (CPU)'**
  String get softwareCpu;

  /// No description provided for @frameDelivery.
  ///
  /// In ko, this message translates to:
  /// **'화면 전달 방식'**
  String get frameDelivery;

  /// No description provided for @gpuSurface.
  ///
  /// In ko, this message translates to:
  /// **'GPU 표면'**
  String get gpuSurface;

  /// No description provided for @cpuPixelBuffer.
  ///
  /// In ko, this message translates to:
  /// **'CPU 픽셀 버퍼'**
  String get cpuPixelBuffer;

  /// No description provided for @newTextureDelivery.
  ///
  /// In ko, this message translates to:
  /// **'새 텍스처 전달'**
  String get newTextureDelivery;

  /// No description provided for @networkWait.
  ///
  /// In ko, this message translates to:
  /// **'전송 대기'**
  String get networkWait;

  /// No description provided for @textureDeliveryMeasuredFlutterRender.
  ///
  /// In ko, this message translates to:
  /// **'텍스처 전달은 Flutter 렌더 요청 기준이에요. 화면 표시 완료나 입력 지연을 뜻하지 않아요.'**
  String get textureDeliveryMeasuredFlutterRender;

  /// No description provided for @hostProcessing.
  ///
  /// In ko, this message translates to:
  /// **'Host 처리'**
  String get hostProcessing;

  /// No description provided for @displayLayoutCheck.
  ///
  /// In ko, this message translates to:
  /// **'화면 구성 확인'**
  String get displayLayoutCheck;

  /// No description provided for @captureRead.
  ///
  /// In ko, this message translates to:
  /// **'캡처·읽기'**
  String get captureRead;

  /// No description provided for @colorConversion.
  ///
  /// In ko, this message translates to:
  /// **'색 변환'**
  String get colorConversion;

  /// No description provided for @encoding.
  ///
  /// In ko, this message translates to:
  /// **'인코딩'**
  String get encoding;

  /// No description provided for @totalAvgP95.
  ///
  /// In ko, this message translates to:
  /// **'합계 평균 / p95'**
  String get totalAvgP95;

  /// No description provided for @processingMax.
  ///
  /// In ko, this message translates to:
  /// **'처리 최댓값'**
  String get processingMax;

  /// No description provided for @waitingRecentHostMeasurements.
  ///
  /// In ko, this message translates to:
  /// **'최근 Host 측정값을 기다리고 있어요.'**
  String get waitingRecentHostMeasurements;

  /// No description provided for @shownOnceHostReportsProcessing.
  ///
  /// In ko, this message translates to:
  /// **'Host가 처리 시간을 보고하면 표시돼요.'**
  String get shownOnceHostReportsProcessing;

  /// No description provided for @lastSSampledNetworkWait.
  ///
  /// In ko, this message translates to:
  /// **'최근 {session}초 · {samples}프레임 표본. 네트워크 대기 시간은 포함하지 않아요.'**
  String lastSSampledNetworkWait(Object session, num samples);

  /// No description provided for @pcProcessing.
  ///
  /// In ko, this message translates to:
  /// **'PC 처리'**
  String get pcProcessing;

  /// No description provided for @decodeAvg.
  ///
  /// In ko, this message translates to:
  /// **'디코딩 평균'**
  String get decodeAvg;

  /// No description provided for @decodeMax.
  ///
  /// In ko, this message translates to:
  /// **'디코딩 최댓값'**
  String get decodeMax;

  /// No description provided for @videoQueue.
  ///
  /// In ko, this message translates to:
  /// **'영상 대기'**
  String get videoQueue;

  /// No description provided for @ms.
  ///
  /// In ko, this message translates to:
  /// **'{clientQueuePackets}개 · {clientQueueMs} ms'**
  String ms(num clientQueuePackets, Object clientQueueMs);

  /// No description provided for @replacedBeforeDelivery.
  ///
  /// In ko, this message translates to:
  /// **'전달 전 교체'**
  String get replacedBeforeDelivery;

  /// No description provided for @text33.
  ///
  /// In ko, this message translates to:
  /// **'{skippedDecodedFrames}프레임'**
  String text33(num skippedDecodedFrames);

  /// No description provided for @gpuVideoDecoding.
  ///
  /// In ko, this message translates to:
  /// **'GPU 영상 디코딩'**
  String get gpuVideoDecoding;

  /// No description provided for @averagesCoverRecentPollingWindow.
  ///
  /// In ko, this message translates to:
  /// **'평균은 최근 조회 구간, 최댓값과 교체 수는 현재 해상도 구간이에요.'**
  String get averagesCoverRecentPollingWindow;

  /// No description provided for @pcTotal.
  ///
  /// In ko, this message translates to:
  /// **'{app} (PC 전체 {systemTenths})'**
  String pcTotal(Object app, Object systemTenths);

  /// No description provided for @cpuDecoder.
  ///
  /// In ko, this message translates to:
  /// **'CPU 디코더'**
  String get cpuDecoder;

  /// No description provided for @gpuDecoder.
  ///
  /// In ko, this message translates to:
  /// **' · GPU 디코더'**
  String get gpuDecoder;

  /// No description provided for @unknown6.
  ///
  /// In ko, this message translates to:
  /// **'알 수 없음{path}'**
  String unknown6(Object path);

  /// No description provided for @autoBitrateMaxMbps.
  ///
  /// In ko, this message translates to:
  /// **'자동 전송률 · 최대 {maxMbps} Mbps'**
  String autoBitrateMaxMbps(Object maxMbps);

  /// No description provided for @videoBitrate.
  ///
  /// In ko, this message translates to:
  /// **'영상 전송률'**
  String get videoBitrate;

  /// No description provided for @auto3.
  ///
  /// In ko, this message translates to:
  /// **'  자동'**
  String get auto3;

  /// No description provided for @manual2.
  ///
  /// In ko, this message translates to:
  /// **'직접 설정'**
  String get manual2;

  /// No description provided for @adjustsBitrateConnectionMaxMbps.
  ///
  /// In ko, this message translates to:
  /// **'연결 상태에 맞춰 전송률을 조절해요. 최대 {maxMbps} Mbps'**
  String adjustsBitrateConnectionMaxMbps(Object maxMbps);

  /// No description provided for @ifNetworkCantKeepUp.
  ///
  /// In ko, this message translates to:
  /// **'네트워크가 감당하지 못하면 지연이 쌓일 수 있어요.'**
  String get ifNetworkCantKeepUp;

  /// No description provided for @decodesGpuFallsBackCpu.
  ///
  /// In ko, this message translates to:
  /// **'GPU로 디코딩해요. 안 되면 CPU를 써요.'**
  String get decodesGpuFallsBackCpu;

  /// No description provided for @decodesCpuOnlyTryIf.
  ///
  /// In ko, this message translates to:
  /// **'CPU로만 디코딩해요. 화면이 깨질 때 써 보세요.'**
  String get decodesCpuOnlyTryIf;

  /// No description provided for @eachMonitorMatchesItsOwn.
  ///
  /// In ko, this message translates to:
  /// **'모니터마다 그 모니터의 크기에 맞춰요.'**
  String get eachMonitorMatchesItsOwn;

  /// No description provided for @remoteResolutionFollowsWindowSize.
  ///
  /// In ko, this message translates to:
  /// **'창 크기에 맞춰 원격 해상도가 바뀌어요.'**
  String get remoteResolutionFollowsWindowSize;

  /// No description provided for @backSettings.
  ///
  /// In ko, this message translates to:
  /// **'설정으로 돌아가기'**
  String get backSettings;

  /// No description provided for @quitMultitap.
  ///
  /// In ko, this message translates to:
  /// **'Multitap을 종료할까요?'**
  String get quitMultitap;

  /// No description provided for @allOpenConnectionsCloseToo.
  ///
  /// In ko, this message translates to:
  /// **'열려 있는 연결도 모두 끊겨요. 데스크톱은 계속 켜져 있어요.'**
  String get allOpenConnectionsCloseToo;

  /// No description provided for @desktopsStay.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱은 계속 켜져 있어요.'**
  String get desktopsStay;

  /// No description provided for @version3.
  ///
  /// In ko, this message translates to:
  /// **'{version} 버전으로'**
  String version3(Object version);

  /// No description provided for @text34.
  ///
  /// In ko, this message translates to:
  /// **'으로'**
  String get text34;

  /// No description provided for @text35.
  ///
  /// In ko, this message translates to:
  /// **'로'**
  String get text35;

  /// No description provided for @calculatingTimeLeft.
  ///
  /// In ko, this message translates to:
  /// **'남은 시간 계산 중'**
  String get calculatingTimeLeft;

  /// No description provided for @aboutMinLeft2.
  ///
  /// In ko, this message translates to:
  /// **'약 {seconds}분 남음'**
  String aboutMinLeft2(Object seconds);

  /// No description provided for @checkNeeded2.
  ///
  /// In ko, this message translates to:
  /// **'직접 확인이 필요해요'**
  String get checkNeeded2;

  /// No description provided for @couldntUpdate.
  ///
  /// In ko, this message translates to:
  /// **'업데이트하지 못했어요'**
  String get couldntUpdate;

  /// No description provided for @existingHostUnchanged.
  ///
  /// In ko, this message translates to:
  /// **'기존 Host는 그대로예요.'**
  String get existingHostUnchanged;

  /// No description provided for @installedAppUnchanged.
  ///
  /// In ko, this message translates to:
  /// **'설치된 앱은 그대로예요.'**
  String get installedAppUnchanged;

  /// No description provided for @couldntGetUpdateInfoCheck.
  ///
  /// In ko, this message translates to:
  /// **'업데이트 정보를 확인하지 못했어요.\n네트워크를 확인하고 다시 시도하세요. 계속되면 관리자에게 알려 주세요.'**
  String get couldntGetUpdateInfoCheck;

  /// No description provided for @couldntDownloadFileCheckNetwork.
  ///
  /// In ko, this message translates to:
  /// **'파일을 받지 못했어요.\n네트워크를 확인하고 다시 시도하세요.'**
  String get couldntDownloadFileCheckNetwork;

  /// No description provided for @networkDroppedSoFileWasnt.
  ///
  /// In ko, this message translates to:
  /// **'네트워크가 끊겨 파일을 받지 못했어요.\n네트워크를 확인하고 다시 시도하세요.'**
  String get networkDroppedSoFileWasnt;

  /// No description provided for @serverDidntRespondWasToo.
  ///
  /// In ko, this message translates to:
  /// **'서버 응답이 없거나 너무 느려 파일을 받지 못했어요.\n네트워크를 확인하고 다시 시도하세요.'**
  String get serverDidntRespondWasToo;

  /// No description provided for @serverCouldntRespondJustNow.
  ///
  /// In ko, this message translates to:
  /// **'서버가 잠시 응답하지 못했어요.\n잠시 후 다시 시도하세요.'**
  String get serverCouldntRespondJustNow;

  /// No description provided for @serverDoesntHaveFileVersion.
  ///
  /// In ko, this message translates to:
  /// **'서버에 이 버전의 파일이 없어요.\n창을 닫고 업데이트를 다시 확인하세요.'**
  String get serverDoesntHaveFileVersion;

  /// No description provided for @serverRefusedFileRequestCheck.
  ///
  /// In ko, this message translates to:
  /// **'서버가 파일 요청을 거절했어요.\n서버 주소를 확인하고 다시 시도하세요.'**
  String get serverRefusedFileRequestCheck;

  /// No description provided for @notEnoughDiskSpaceSave.
  ///
  /// In ko, this message translates to:
  /// **'디스크 공간이 부족해 파일을 저장하지 못했어요.\n공간을 확보한 뒤 다시 시도하세요.'**
  String get notEnoughDiskSpaceSave;

  /// No description provided for @couldntSaveFileTempFolder.
  ///
  /// In ko, this message translates to:
  /// **'임시 폴더에 파일을 저장하지 못했어요.\n보안 프로그램이나 폴더 권한을 확인하세요.'**
  String get couldntSaveFileTempFolder;

  /// No description provided for @downloadedFileSizeDoesntMatch.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일 크기가 배포 정보와 달라 설치하지 않았어요.\n다시 시도하고, 계속되면 관리자에게 알려 주세요.'**
  String get downloadedFileSizeDoesntMatch;

  /// No description provided for @downloadedFileDoesntMatchRelease.
  ///
  /// In ko, this message translates to:
  /// **'받은 파일이 배포 정보의 크기나 SHA-256과 달라 설치하지 않았어요.\n다시 시도하고, 계속되면 관리자에게 알려 주세요.'**
  String get downloadedFileDoesntMatchRelease;

  /// No description provided for @fileVersionDoesntMatchRelease.
  ///
  /// In ko, this message translates to:
  /// **'파일 버전이 배포 버전{version}과 달라 설치하지 않았어요.\n{kept}\n관리자에게 알려 주세요.'**
  String fileVersionDoesntMatchRelease(Object version, Object kept);

  /// No description provided for @releaseSignatureMissingInvalidSo.
  ///
  /// In ko, this message translates to:
  /// **'배포 서명이 없거나 맞지 않아 설치하지 않았어요.\n{kept}\n관리자에게 알려 주세요.'**
  String releaseSignatureMissingInvalidSo(Object kept);

  /// No description provided for @administratorApprovalWasCancelledSo.
  ///
  /// In ko, this message translates to:
  /// **'관리자 승인을 취소해 업데이트하지 않았어요.\n{kept}\n다시 시도할 때 승인 창에서 ‘예’를 선택하세요.'**
  String administratorApprovalWasCancelledSo(Object kept);

  /// No description provided for @fileServerIsntNewerThan.
  ///
  /// In ko, this message translates to:
  /// **'이 서버의 파일이 지금 실행 중인 버전보다 새롭지 않아 설치하지 않았어요. {kept}'**
  String fileServerIsntNewerThan(Object kept);

  /// No description provided for @installerClosedBeforeUpdateStarted.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램이 업데이트를 시작하기 전에 종료됐어요. {kept}'**
  String installerClosedBeforeUpdateStarted(Object kept);

  /// No description provided for @couldntCloseOpenConnectionsDisconnect.
  ///
  /// In ko, this message translates to:
  /// **'열린 연결을 끊지 못했어요. 연결을 끊은 뒤 다시 시도하세요.'**
  String get couldntCloseOpenConnectionsDisconnect;

  /// No description provided for @couldntCheckRunningVersionRestart.
  ///
  /// In ko, this message translates to:
  /// **'실행 중인 버전을 확인하지 못했어요. 앱을 다시 시작한 뒤 업데이트를 확인하세요.'**
  String get couldntCheckRunningVersionRestart;

  /// No description provided for @updateAlreadyBeingPreparedTry.
  ///
  /// In ko, this message translates to:
  /// **'이미 업데이트를 준비하고 있어요. 잠시 후 다시 시도하세요.'**
  String get updateAlreadyBeingPreparedTry;

  /// No description provided for @installationFilesNotValidInstall.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일 구성이 올바르지 않아요. 설치 프로그램으로 직접 설치하세요. {kept}'**
  String installationFilesNotValidInstall(Object kept);

  /// No description provided for @couldntCommunicateInstallerTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램과 통신하지 못했어요. 다시 시도하세요. {kept}'**
  String couldntCommunicateInstallerTryAgain(Object kept);

  /// No description provided for @desktopHasNoHostInstalled.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱에는 업데이트할 Host가 설치돼 있지 않아요. 설치 프로그램으로 직접 설치하세요.'**
  String get desktopHasNoHostInstalled;

  /// No description provided for @windowsDoesntTrustVirtualDisplay.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버 서명을 이 Windows가 아직 신뢰하지 않아요. 설치 프로그램에서 서명을 확인한 뒤 설치하세요. 기존 Host는 그대로예요.'**
  String get windowsDoesntTrustVirtualDisplay;

  /// No description provided for @updateNeedsInstallChangeVirtual.
  ///
  /// In ko, this message translates to:
  /// **'이 업데이트는 가상 화면 드라이버 설치나 변경이 필요해요. 설치 프로그램에서 직접 확인한 뒤 설치하세요. 기존 Host는 그대로예요.'**
  String get updateNeedsInstallChangeVirtual;

  /// No description provided for @fileCameFromSomewhereOther.
  ///
  /// In ko, this message translates to:
  /// **'확인된 서버와 다른 곳에서 받은 파일이라 설치하지 않았어요.\n{kept}\n서버 주소를 다시 저장하고 업데이트를 확인하세요.'**
  String fileCameFromSomewhereOther(Object kept);

  /// No description provided for @installationStoppedItsUnknownWhether.
  ///
  /// In ko, this message translates to:
  /// **'설치가 중단됐고 복구 여부도 확인하지 못했어요.\nHost의 서비스 상태를 확인한 뒤 설치 프로그램으로 다시 설치하세요.'**
  String get installationStoppedItsUnknownWhether;

  /// No description provided for @installationStoppedReinstallApp.
  ///
  /// In ko, this message translates to:
  /// **'설치가 중단됐고 복구 여부도 확인하지 못했어요.\n설치 프로그램으로 다시 설치하세요.'**
  String get installationStoppedReinstallApp;

  /// No description provided for @problemOccurredDuringInstallationSo.
  ///
  /// In ko, this message translates to:
  /// **'설치 중 문제가 생겨 Host를 이전 상태로 되돌렸어요. Host 등록과 가상 화면 설정은 그대로예요.'**
  String get problemOccurredDuringInstallationSo;

  /// No description provided for @problemOccurredDuringInstallationSo2.
  ///
  /// In ko, this message translates to:
  /// **'설치 중 문제가 생겨 이전 버전을 다시 열었어요. 서버 주소와 로그인 설정은 그대로예요.'**
  String get problemOccurredDuringInstallationSo2;

  /// No description provided for @couldntCompleteInstallationItsUnknown.
  ///
  /// In ko, this message translates to:
  /// **'설치를 완료하지 못했어요.\n어디서 멈췄는지, 복구됐는지는 확인하지 못했어요.\n다시 시도하거나 설치 프로그램으로 직접 설치하세요.'**
  String get couldntCompleteInstallationItsUnknown;

  /// No description provided for @couldntStartInstallerTryAgain.
  ///
  /// In ko, this message translates to:
  /// **'설치 프로그램을 시작하지 못했어요. 다시 시도하세요. {kept}'**
  String couldntStartInstallerTryAgain(Object kept);

  /// No description provided for @updated.
  ///
  /// In ko, this message translates to:
  /// **'{product} {toVersion} 업데이트했어요'**
  String updated(Object product, Object toVersion);

  /// No description provided for @windowsNeedsRestartSaveWork.
  ///
  /// In ko, this message translates to:
  /// **'Windows를 다시 시작해야 해요. 작업을 저장한 뒤 직접 다시 시작하세요.'**
  String get windowsNeedsRestartSaveWork;

  /// No description provided for @checkDiagnosticsHostSettings.
  ///
  /// In ko, this message translates to:
  /// **'Host 설정에서 진단을 확인해 주세요.'**
  String get checkDiagnosticsHostSettings;

  /// No description provided for @installationFinishedButSomeItems.
  ///
  /// In ko, this message translates to:
  /// **'설치는 끝났지만 확인이 필요한 항목이 남았어요.'**
  String get installationFinishedButSomeItems;

  /// No description provided for @hostServiceRunningNewVersion.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 새 버전으로 실행 중이에요.'**
  String get hostServiceRunningNewVersion;

  /// No description provided for @reopenedNewVersionSettingsUnchanged.
  ///
  /// In ko, this message translates to:
  /// **'새 버전으로 다시 열었어요. 설정은 그대로예요.'**
  String get reopenedNewVersionSettingsUnchanged;

  /// No description provided for @viewChanges.
  ///
  /// In ko, this message translates to:
  /// **'변경 내역 보기'**
  String get viewChanges;

  /// No description provided for @stoppedAfterStep6.
  ///
  /// In ko, this message translates to:
  /// **' · {stage}/6단계 완료 후 중단'**
  String stoppedAfterStep6(Object stage);

  /// No description provided for @code4.
  ///
  /// In ko, this message translates to:
  /// **'코드 {code}{notice}'**
  String code4(Object code, Object notice);

  /// No description provided for @restartPcNow.
  ///
  /// In ko, this message translates to:
  /// **'PC를 지금 다시 시작할까요?'**
  String get restartPcNow;

  /// No description provided for @saveOpenWorkFirstUnsaved.
  ///
  /// In ko, this message translates to:
  /// **'열려 있는 작업을 먼저 저장하세요.\n저장하지 않은 작업은 사라질 수 있어요. 데스크톱 연결도 끊겨요.'**
  String get saveOpenWorkFirstUnsaved;

  /// No description provided for @couldntRequestRestartRestartFrom.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작을 요청하지 못했어요. Windows 시작 메뉴에서 다시 시작해 주세요.'**
  String get couldntRequestRestartRestartFrom;

  /// No description provided for @installUsbDriver.
  ///
  /// In ko, this message translates to:
  /// **'USB 드라이버 설치'**
  String get installUsbDriver;

  /// No description provided for @installsDriverPcNeedsUsb.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결에 필요한 드라이버를 PC에 설치해요.\nWindows가 관리자 승인을 물으면 ‘예’를 누르세요.'**
  String get installsDriverPcNeedsUsb;

  /// No description provided for @onlyInstallersVerifiedSignatureUsed.
  ///
  /// In ko, this message translates to:
  /// **'서명을 확인한 설치 파일만 사용해요.\nPC를 다시 시작해야 하면 설치 후 알려 드려요. 자동으로 다시 시작하지는 않아요.'**
  String get onlyInstallersVerifiedSignatureUsed;

  /// No description provided for @disconnectUsb.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결 끊기'**
  String get disconnectUsb;

  /// No description provided for @ifDesktopUsingDeviceUnsaved.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 이 장치를 쓰는 중이면 저장하지 않은 내용이 사라질 수 있어요.\n끊으면 장치가 PC로 돌아와요.'**
  String get ifDesktopUsingDeviceUnsaved;

  /// No description provided for @hideSupportedDevices.
  ///
  /// In ko, this message translates to:
  /// **'지원 장치 안내 접기'**
  String get hideSupportedDevices;

  /// No description provided for @supportedDevices.
  ///
  /// In ko, this message translates to:
  /// **'지원 장치 안내'**
  String get supportedDevices;

  /// No description provided for @connectedDeviceCanOnlyUsed.
  ///
  /// In ko, this message translates to:
  /// **'연결한 장치는 데스크톱에서만 쓸 수 있어요.\n연결을 끊으면 PC로 돌아와요.'**
  String get connectedDeviceCanOnlyUsed;

  /// No description provided for @keyboardsMiceHubsBluetoothAudio.
  ///
  /// In ko, this message translates to:
  /// **'키보드·마우스·허브·블루투스·오디오·카메라는 목록에 표시하지 않아요.'**
  String get keyboardsMiceHubsBluetoothAudio;

  /// No description provided for @restartNow.
  ///
  /// In ko, this message translates to:
  /// **'지금 다시 시작'**
  String get restartNow;

  /// No description provided for @installsAfterVerifyingInstallersSignature.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일의 서명을 확인한 뒤 설치해요'**
  String get installsAfterVerifyingInstallersSignature;

  /// No description provided for @installing2.
  ///
  /// In ko, this message translates to:
  /// **'설치하는 중…'**
  String get installing2;

  /// No description provided for @installUsbDriver2.
  ///
  /// In ko, this message translates to:
  /// **'USB 드라이버 설치…'**
  String get installUsbDriver2;

  /// No description provided for @lookingDevices.
  ///
  /// In ko, this message translates to:
  /// **'장치를 찾고 있어요…'**
  String get lookingDevices;

  /// No description provided for @noUsbDevicesConnect.
  ///
  /// In ko, this message translates to:
  /// **'연결할 USB 장치가 없어요.'**
  String get noUsbDevicesConnect;

  /// No description provided for @devicesPcNeedsCantConnected.
  ///
  /// In ko, this message translates to:
  /// **'PC를 쓰는 데 필요한 장치는 연결할 수 없어요 · {hiddenNeeded}개 숨김'**
  String devicesPcNeedsCantConnected(Object hiddenNeeded);

  /// No description provided for @reconnectingUsb.
  ///
  /// In ko, this message translates to:
  /// **'USB를 다시 연결하고 있어요.'**
  String get reconnectingUsb;

  /// No description provided for @usbDisconnectedReconnectDesktopUse.
  ///
  /// In ko, this message translates to:
  /// **'USB 연결이 끊겼어요. 데스크톱에 다시 연결하면 쓸 수 있어요.'**
  String get usbDisconnectedReconnectDesktopUse;

  /// No description provided for @desktopGettingReadyReceiveUsb.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 USB 장치를 받을 준비를 하고 있어요…'**
  String get desktopGettingReadyReceiveUsb;

  /// No description provided for @pcCantConnectUsbDevices.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 USB 장치를 데스크톱에 연결할 수 없어요.'**
  String get pcCantConnectUsbDevices;

  /// No description provided for @restartRequestedWindowsAsksIf.
  ///
  /// In ko, this message translates to:
  /// **'다시 시작을 요청했어요. 저장하지 않은 작업이 있으면 Windows가 물어봐요.'**
  String get restartRequestedWindowsAsksIf;

  /// No description provided for @usbDriverNeedsInstalled.
  ///
  /// In ko, this message translates to:
  /// **'USB 드라이버 설치가 필요해요.'**
  String get usbDriverNeedsInstalled;

  /// No description provided for @text36.
  ///
  /// In ko, this message translates to:
  /// **'기타 USB 장치 {count}개'**
  String text36(num count);

  /// No description provided for @otherUsbDevices.
  ///
  /// In ko, this message translates to:
  /// **'기타 USB 장치 ({count})'**
  String otherUsbDevices(Object count);

  /// No description provided for @justPlugged.
  ///
  /// In ko, this message translates to:
  /// **'방금 꽂음'**
  String get justPlugged;

  /// No description provided for @checkingOpenFiles.
  ///
  /// In ko, this message translates to:
  /// **'열린 파일이 없는지 확인하는 중…'**
  String get checkingOpenFiles;

  /// No description provided for @connectingDesktop2.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 연결하는 중…'**
  String get connectingDesktop2;

  /// No description provided for @connectedDesktopNotAvailablePc.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 연결됨 · 지금은 PC에서 쓸 수 없어요'**
  String get connectedDesktopNotAvailablePc;

  /// No description provided for @openFileCheckTimedOut.
  ///
  /// In ko, this message translates to:
  /// **'열린 파일 확인 시간이 초과됐어요. PC 작업이 끝나기를 기다리는 중…'**
  String get openFileCheckTimedOut;

  /// No description provided for @deviceConnectionTimedOutChecking.
  ///
  /// In ko, this message translates to:
  /// **'장치 연결 시간이 초과됐어요. PC로 돌아왔는지 확인하는 중…'**
  String get deviceConnectionTimedOutChecking;

  /// No description provided for @checkingDevicePc.
  ///
  /// In ko, this message translates to:
  /// **'PC의 장치 상태를 확인하는 중…'**
  String get checkingDevicePc;

  /// No description provided for @returningDevicePc.
  ///
  /// In ko, this message translates to:
  /// **'PC로 장치를 돌려주는 중…'**
  String get returningDevicePc;

  /// No description provided for @programUsing.
  ///
  /// In ko, this message translates to:
  /// **'{code} 사용 중인 프로그램: {blocker}'**
  String programUsing(Object code, Object blocker);

  /// No description provided for @movingFromAnotherWindow.
  ///
  /// In ko, this message translates to:
  /// **'다른 창에서 가져오는 중…'**
  String get movingFromAnotherWindow;

  /// No description provided for @movingFromWindow.
  ///
  /// In ko, this message translates to:
  /// **'{from} 창에서 가져오는 중…'**
  String movingFromWindow(Object from);

  /// No description provided for @reconnectingUsb2.
  ///
  /// In ko, this message translates to:
  /// **'USB를 다시 연결하고 있어요'**
  String get reconnectingUsb2;

  /// No description provided for @youCanConnectOnceDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱이 준비되면 연결할 수 있어요'**
  String get youCanConnectOnceDesktop;

  /// No description provided for @deviceUseElsewhere.
  ///
  /// In ko, this message translates to:
  /// **'다른 곳에서 이 장치를 쓰고 있어요'**
  String get deviceUseElsewhere;

  /// No description provided for @anotherProgramUsingDevice2.
  ///
  /// In ko, this message translates to:
  /// **'다른 프로그램이 이 장치를 쓰고 있어요'**
  String get anotherProgramUsingDevice2;

  /// No description provided for @movesWindowsDesktop.
  ///
  /// In ko, this message translates to:
  /// **'이 창의 데스크톱으로 옮겨요'**
  String get movesWindowsDesktop;

  /// No description provided for @moveWindow.
  ///
  /// In ko, this message translates to:
  /// **'이 창으로 옮기기'**
  String get moveWindow;

  /// No description provided for @usbMovingHere.
  ///
  /// In ko, this message translates to:
  /// **'옮기는 중'**
  String get usbMovingHere;

  /// No description provided for @moveUsbHereTitle.
  ///
  /// In ko, this message translates to:
  /// **'이 창으로 옮길까요?'**
  String get moveUsbHereTitle;

  /// No description provided for @moveUsbHereMessage.
  ///
  /// In ko, this message translates to:
  /// **'다른 창의 데스크톱에서 이 장치를 쓰는 중이면 저장하지 않은 내용이 사라질 수 있어요.\n옮기면 이 창의 데스크톱에서 쓸 수 있어요.'**
  String get moveUsbHereMessage;

  /// No description provided for @moveUsbHereConfirm.
  ///
  /// In ko, this message translates to:
  /// **'옮기기'**
  String get moveUsbHereConfirm;

  /// No description provided for @usbSuperSpeedDevices.
  ///
  /// In ko, this message translates to:
  /// **'USB 3.0 포트에 꽂은 USB 3.0 장치 {count}개는 연결할 수 없어요. USB 2.0 포트에 꽂아 주세요.'**
  String usbSuperSpeedDevices(Object count);

  /// No description provided for @returnPc.
  ///
  /// In ko, this message translates to:
  /// **'PC로 되돌리기'**
  String get returnPc;

  /// No description provided for @returning.
  ///
  /// In ko, this message translates to:
  /// **'되돌리는 중'**
  String get returning;

  /// No description provided for @connectsUseDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 쓰도록 연결해요'**
  String get connectsUseDesktop;

  /// No description provided for @securityTokenSmartCard.
  ///
  /// In ko, this message translates to:
  /// **'보안 토큰·스마트 카드'**
  String get securityTokenSmartCard;

  /// No description provided for @ifDriveDoesntAppearDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 드라이브가 보이지 않으면 ‘디스크 관리’를 확인하세요.'**
  String get ifDriveDoesntAppearDesktop;

  /// No description provided for @pcHasNoReadableDrive.
  ///
  /// In ko, this message translates to:
  /// **'PC에서 읽을 수 있는 드라이브가 없어요. 데스크톱에서도 ‘디스크 관리’를 확인해야 할 수 있어요.'**
  String get pcHasNoReadableDrive;

  /// No description provided for @useSafelyRemoveHardwareDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에서 ‘하드웨어 안전하게 제거’를 한 뒤 연결을 끊으세요. 쓰는 도중 끊으면 파일이 손상될 수 있어요.'**
  String get useSafelyRemoveHardwareDesktop;

  /// No description provided for @couldntConfirmNumberMonitorsChanged.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수가 바뀌었는지 확인하지 못했어요. 데스크톱 목록에서 확인하세요.'**
  String get couldntConfirmNumberMonitorsChanged;

  /// No description provided for @connectionClosesReconnectAfterNumber.
  ///
  /// In ko, this message translates to:
  /// **'연결이 끊겨요. 모니터 수가 바뀐 뒤 다시 연결하세요.'**
  String get connectionClosesReconnectAfterNumber;

  /// No description provided for @disconnectApply.
  ///
  /// In ko, this message translates to:
  /// **'연결 끊고 적용'**
  String get disconnectApply;

  /// No description provided for @n1Monitor.
  ///
  /// In ko, this message translates to:
  /// **'1대'**
  String get n1Monitor;

  /// No description provided for @n2Monitors.
  ///
  /// In ko, this message translates to:
  /// **'2대'**
  String get n2Monitors;

  /// No description provided for @numberMonitorsKeptDesktopWhich.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱에 켜 둘 모니터 수예요. 볼 화면과 화질 설정은 따로예요.'**
  String get numberMonitorsKeptDesktopWhich;

  /// No description provided for @changingNumberMonitors2.
  ///
  /// In ko, this message translates to:
  /// **'모니터 수를 바꾸고 있어요…'**
  String get changingNumberMonitors2;

  /// No description provided for @switchLabel2.
  ///
  /// In ko, this message translates to:
  /// **'모니터를 {count}대로 바꿀까요?'**
  String switchLabel2(num count);

  /// No description provided for @measuredLocalPc.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC에서 측정'**
  String get measuredLocalPc;

  /// No description provided for @measuredInsideDesktop.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 안에서 측정'**
  String get measuredInsideDesktop;

  /// No description provided for @fromProxmox.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox 기준'**
  String get fromProxmox;

  /// No description provided for @cpu.
  ///
  /// In ko, this message translates to:
  /// **'CPU {vcpus}코어'**
  String cpu(num vcpus);

  /// No description provided for @memory.
  ///
  /// In ko, this message translates to:
  /// **'메모리 {memoryTotalBytes}'**
  String memory(Object memoryTotalBytes);

  /// No description provided for @justStarted.
  ///
  /// In ko, this message translates to:
  /// **'방금 켜짐'**
  String get justStarted;

  /// No description provided for @min2.
  ///
  /// In ko, this message translates to:
  /// **'{dInMinutes}분'**
  String min2(Object dInMinutes);

  /// No description provided for @hr.
  ///
  /// In ko, this message translates to:
  /// **'{dInHours}시간'**
  String hr(Object dInHours);

  /// No description provided for @text37.
  ///
  /// In ko, this message translates to:
  /// **'{dInDays}일'**
  String text37(num dInDays);

  /// No description provided for @under1Min.
  ///
  /// In ko, this message translates to:
  /// **'1분 미만'**
  String get under1Min;

  /// No description provided for @hrMin.
  ///
  /// In ko, this message translates to:
  /// **'{dInHours}시간 {minutes}분'**
  String hrMin(Object dInHours, Object minutes);

  /// No description provided for @hr2.
  ///
  /// In ko, this message translates to:
  /// **'{dInDays}일 {hours}시간'**
  String hr2(num dInDays, Object hours);

  /// No description provided for @memoryValueProxmoxReportsSo.
  ///
  /// In ko, this message translates to:
  /// **'메모리는 Proxmox가 보고한 값이라 Windows 작업 관리자와 다를 수 있어요.'**
  String get memoryValueProxmoxReportsSo;

  /// No description provided for @text38.
  ///
  /// In ko, this message translates to:
  /// **' · {vcpus}코어'**
  String text38(num vcpus);

  /// No description provided for @previousValue.
  ///
  /// In ko, this message translates to:
  /// **'이전 값'**
  String get previousValue;

  /// No description provided for @couldntGetLatestStatus.
  ///
  /// In ko, this message translates to:
  /// **'\n최신 상태를 확인하지 못했어요.'**
  String get couldntGetLatestStatus;

  /// No description provided for @memory2.
  ///
  /// In ko, this message translates to:
  /// **'메모리'**
  String get memory2;

  /// No description provided for @onFor.
  ///
  /// In ko, this message translates to:
  /// **'켜진 지 {uptime}'**
  String onFor(Object uptime);

  /// No description provided for @sampleValuesDesignNotMeasured.
  ///
  /// In ko, this message translates to:
  /// **'디자인 예시 값이에요. 실제 데스크톱을 측정한 값이 아니에요.'**
  String get sampleValuesDesignNotMeasured;

  /// No description provided for @gettingCpuMemoryUsage.
  ///
  /// In ko, this message translates to:
  /// **'CPU와 메모리 사용량을 받아오는 중이에요.'**
  String get gettingCpuMemoryUsage;

  /// No description provided for @text39.
  ///
  /// In ko, this message translates to:
  /// **' ({vcpus}코어)'**
  String text39(num vcpus);

  /// No description provided for @memory3.
  ///
  /// In ko, this message translates to:
  /// **'메모리 {used}{total}'**
  String memory3(Object used, Object total);

  /// No description provided for @couldntGetCpuMemoryInfo.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC에서 CPU와 메모리 정보를 받지 못했어요.'**
  String get couldntGetCpuMemoryInfo;

  /// No description provided for @usageShowsWhileIts.
  ///
  /// In ko, this message translates to:
  /// **'켜져 있을 때 사용량을 보여줘요.'**
  String get usageShowsWhileIts;

  /// No description provided for @couldntGetCpuMemoryInfo2.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox에서 CPU와 메모리 정보를 받지 못했어요.'**
  String get couldntGetCpuMemoryInfo2;

  /// No description provided for @configuredSpecsUsageShowsWhile.
  ///
  /// In ko, this message translates to:
  /// **'설정된 사양 · {spec}\n켜져 있을 때 사용량을 보여줘요.'**
  String configuredSpecsUsageShowsWhile(Object spec);

  /// No description provided for @specs.
  ///
  /// In ko, this message translates to:
  /// **'사양'**
  String get specs;

  /// No description provided for @configuredSpecs.
  ///
  /// In ko, this message translates to:
  /// **'설정된 사양'**
  String get configuredSpecs;

  /// No description provided for @couldntGetLatestUsageShows.
  ///
  /// In ko, this message translates to:
  /// **'{pc} · {spec}\n최신 사용량을 확인하지 못했어요. 확인되면 다시 보여줘요.'**
  String couldntGetLatestUsageShows(Object pc, Object spec);

  /// No description provided for @used.
  ///
  /// In ko, this message translates to:
  /// **' 사용 {cpu}'**
  String used(Object cpu);

  /// No description provided for @used2.
  ///
  /// In ko, this message translates to:
  /// **' 중 {used} 사용'**
  String used2(Object used);

  /// No description provided for @measuring.
  ///
  /// In ko, this message translates to:
  /// **'{cpuLabel}, {memoryLabel} 측정 중'**
  String measuring(Object cpuLabel, Object memoryLabel);

  /// No description provided for @measuring2.
  ///
  /// In ko, this message translates to:
  /// **'측정 중'**
  String get measuring2;

  /// No description provided for @newV.
  ///
  /// In ko, this message translates to:
  /// **'새 버전 v{update}'**
  String newV(Object update);

  /// No description provided for @newAppVersionV.
  ///
  /// In ko, this message translates to:
  /// **'앱 새 버전 v{update}'**
  String newAppVersionV(Object update);

  /// No description provided for @updatesCantCheckedDemo.
  ///
  /// In ko, this message translates to:
  /// **'데모에서는 업데이트를 확인할 수 없어요.'**
  String get updatesCantCheckedDemo;

  /// No description provided for @checkAppUpdates.
  ///
  /// In ko, this message translates to:
  /// **'앱 업데이트 확인'**
  String get checkAppUpdates;

  /// No description provided for @closeDemo.
  ///
  /// In ko, this message translates to:
  /// **'데모 닫기'**
  String get closeDemo;

  /// No description provided for @quitsMultitapCompletelyInsteadKeeping.
  ///
  /// In ko, this message translates to:
  /// **'트레이에 남기지 않고 Multitap을 완전히 끝내요.'**
  String get quitsMultitapCompletelyInsteadKeeping;

  /// No description provided for @autoConnectStoppedBecauseConnection.
  ///
  /// In ko, this message translates to:
  /// **'5분 안에 연결 준비가 되지 않아 자동 연결을 멈췄어요. 자세히에서 상태를 확인하세요.'**
  String get autoConnectStoppedBecauseConnection;

  /// No description provided for @opensDesktopsOwnScreenApp.
  ///
  /// In ko, this message translates to:
  /// **'앱 안에서 이 데스크톱의 원래 화면을 열어요.\nWindows가 로그아웃됐거나 잠겨 있을 때 쓰세요.'**
  String get opensDesktopsOwnScreenApp;

  /// No description provided for @broughtDesktopsOpenWindowFront.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱의 열려 있는 창을 보여 드렸어요. 다른 방식으로 열려면 그 창을 먼저 닫으세요.'**
  String get broughtDesktopsOpenWindowFront;

  /// No description provided for @connectionReadinessChangedCheckList.
  ///
  /// In ko, this message translates to:
  /// **'연결 준비 상태가 바뀌었어요. 목록을 다시 확인하세요.'**
  String get connectionReadinessChangedCheckList;

  /// No description provided for @connect5.
  ///
  /// In ko, this message translates to:
  /// **'{currentName}에 연결할까요?'**
  String connect5(Object currentName);

  /// No description provided for @takeOverConnect.
  ///
  /// In ko, this message translates to:
  /// **'넘겨받아 연결'**
  String get takeOverConnect;

  /// No description provided for @couldntOpenRemoteWindowTry.
  ///
  /// In ko, this message translates to:
  /// **'원격 창을 열지 못했어요. 다시 시도하세요.'**
  String get couldntOpenRemoteWindowTry;

  /// No description provided for @openRemoteWindowsCloseToo.
  ///
  /// In ko, this message translates to:
  /// **'열린 원격 창도 함께 닫혀요.\n데스크톱과 실행 중인 작업은 그대로 유지돼요.'**
  String get openRemoteWindowsCloseToo;

  /// No description provided for @localPc4.
  ///
  /// In ko, this message translates to:
  /// **'{machineName}: 로컬 PC가 켜졌어요.'**
  String localPc4(Object machineName);

  /// No description provided for @windowsReceivedRequest.
  ///
  /// In ko, this message translates to:
  /// **'{machineName}: Windows가 {label} 요청을 받았어요.'**
  String windowsReceivedRequest(Object machineName, Object label);

  /// No description provided for @finished.
  ///
  /// In ko, this message translates to:
  /// **'{machineName} {label} 작업이 완료됐어요.'**
  String finished(Object machineName, Object label);

  /// No description provided for @couldntProcessRequestTryAgain3.
  ///
  /// In ko, this message translates to:
  /// **'{label} 요청을 처리하지 못했어요. 잠시 후 다시 시도하세요.'**
  String couldntProcessRequestTryAgain3(Object label);

  /// No description provided for @hostUpdateStartsWhenConnection.
  ///
  /// In ko, this message translates to:
  /// **'{machineName}: 연결이 끝나면 Host {latestVersion} 업데이트를 시작해요.'**
  String hostUpdateStartsWhenConnection(
    Object machineName,
    Object latestVersion,
  );

  /// No description provided for @hostUpdateRequested.
  ///
  /// In ko, this message translates to:
  /// **'{machineName}: Host {latestVersion} 업데이트를 요청했어요.'**
  String hostUpdateRequested(Object machineName, Object latestVersion);

  /// No description provided for @couldntRequestHostUpdateTry.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트를 요청하지 못했어요. 잠시 후 다시 시도하세요.'**
  String get couldntRequestHostUpdateTry;

  /// No description provided for @allMonitorsWorksOnlyWhen.
  ///
  /// In ko, this message translates to:
  /// **'모든 모니터는 PC에 모니터가 2대이고 이 데스크톱의 Host가 지원할 때만 쓸 수 있어요.'**
  String get allMonitorsWorksOnlyWhen;

  /// No description provided for @allMonitorsWorksOnlyWhen2.
  ///
  /// In ko, this message translates to:
  /// **'모든 모니터는 PC에 모니터가 2대일 때만 쓸 수 있어요.'**
  String get allMonitorsWorksOnlyWhen2;

  /// No description provided for @checkingPowerStatusRefreshMoment.
  ///
  /// In ko, this message translates to:
  /// **'전원 상태를 확인하는 중이에요. 잠시 후 새로고침하세요.'**
  String get checkingPowerStatusRefreshMoment;

  /// No description provided for @refreshingList.
  ///
  /// In ko, this message translates to:
  /// **'목록을 새로고침하는 중이에요.'**
  String get refreshingList;

  /// No description provided for @demoNoServer.
  ///
  /// In ko, this message translates to:
  /// **'데모 · 서버 미연결'**
  String get demoNoServer;

  /// No description provided for @refreshingListTryAgainMoment.
  ///
  /// In ko, this message translates to:
  /// **'목록을 새로고침하는 중이에요. 잠시 후 다시 시도하세요.'**
  String get refreshingListTryAgainMoment;

  /// No description provided for @refreshingList2.
  ///
  /// In ko, this message translates to:
  /// **'목록을 새로고침하는 중이에요'**
  String get refreshingList2;

  /// No description provided for @askAdministratorAssignYouDesktop.
  ///
  /// In ko, this message translates to:
  /// **'관리자에게 데스크톱 배정을 요청하세요.'**
  String get askAdministratorAssignYouDesktop;

  /// No description provided for @sampleAssignmentsNotConnectedYet.
  ///
  /// In ko, this message translates to:
  /// **'예시 할당 목록 · 실제 연결 전'**
  String get sampleAssignmentsNotConnectedYet;

  /// No description provided for @couldntLoadDesktopList.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 목록을 불러오지 못했어요.'**
  String get couldntLoadDesktopList;

  /// No description provided for @language.
  ///
  /// In ko, this message translates to:
  /// **'언어'**
  String get language;

  /// No description provided for @languageSaveFailed.
  ///
  /// In ko, this message translates to:
  /// **'언어를 저장하지 못했어요. 다시 고르세요.'**
  String get languageSaveFailed;

  /// No description provided for @localPcInSentence.
  ///
  /// In ko, this message translates to:
  /// **'로컬 PC'**
  String get localPcInSentence;

  /// No description provided for @desktopInSentence.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱'**
  String get desktopInSentence;

  /// No description provided for @setUpLabel.
  ///
  /// In ko, this message translates to:
  /// **'설정'**
  String get setUpLabel;

  /// No description provided for @restoringVirtualDisplayShort.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 복구 중'**
  String get restoringVirtualDisplayShort;

  /// No description provided for @settingUpVirtualDisplayShort.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 설정 중'**
  String get settingUpVirtualDisplayShort;

  /// No description provided for @appliedStatus.
  ///
  /// In ko, this message translates to:
  /// **'적용'**
  String get appliedStatus;

  /// No description provided for @quitLabel.
  ///
  /// In ko, this message translates to:
  /// **'종료'**
  String get quitLabel;

  /// No description provided for @logCode.
  ///
  /// In ko, this message translates to:
  /// **'코드 {code}'**
  String logCode(Object code);

  /// No description provided for @logErrorCode.
  ///
  /// In ko, this message translates to:
  /// **'오류 코드 {code}'**
  String logErrorCode(Object code);

  /// No description provided for @listSeparator.
  ///
  /// In ko, this message translates to:
  /// **'·'**
  String get listSeparator;

  /// No description provided for @untilDate.
  ///
  /// In ko, this message translates to:
  /// **'{date}까지'**
  String untilDate(Object date);

  /// No description provided for @languageNameKorean.
  ///
  /// In ko, this message translates to:
  /// **'한국어'**
  String get languageNameKorean;

  /// No description provided for @languageNameEnglish.
  ///
  /// In ko, this message translates to:
  /// **'English'**
  String get languageNameEnglish;

  /// No description provided for @languageHint.
  ///
  /// In ko, this message translates to:
  /// **'자동은 시스템 언어를 따라요.'**
  String get languageHint;

  /// No description provided for @mfaSettingHint.
  ///
  /// In ko, this message translates to:
  /// **'로그인할 때 인증 앱의 코드도 확인해요.'**
  String get mfaSettingHint;

  /// No description provided for @passwordSettingHint.
  ///
  /// In ko, this message translates to:
  /// **'Multitap 로그인 비밀번호를 바꿔요.'**
  String get passwordSettingHint;

  /// No description provided for @hostDoesntMatchRegisteredVm.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱의 Host가 등록된 VM과 맞지 않아요. 관리자에게 알려 주세요.'**
  String get hostDoesntMatchRegisteredVm;

  /// No description provided for @desktopRegistrationChangedRefresh.
  ///
  /// In ko, this message translates to:
  /// **'이 데스크톱의 등록이 바뀌었어요. 데스크톱 목록을 새로고침한 뒤 다시 연결하세요.'**
  String get desktopRegistrationChangedRefresh;

  /// No description provided for @proxmoxNoLongerListsVm.
  ///
  /// In ko, this message translates to:
  /// **'Proxmox에서 이 데스크톱의 VM을 찾지 못했어요. 관리자에게 알려 주세요.'**
  String get proxmoxNoLongerListsVm;

  /// No description provided for @progressStepOf.
  ///
  /// In ko, this message translates to:
  /// **'{index}/{count}단계'**
  String progressStepOf(Object index, Object count);

  /// No description provided for @androidSigningOut.
  ///
  /// In ko, this message translates to:
  /// **'로그아웃하는 중이에요'**
  String get androidSigningOut;

  /// No description provided for @androidHostUpdateRequesting.
  ///
  /// In ko, this message translates to:
  /// **'업데이트를 요청하는 중이에요'**
  String get androidHostUpdateRequesting;

  /// No description provided for @androidHostUpdateReading.
  ///
  /// In ko, this message translates to:
  /// **'Host 배포 정보를 확인하는 중이에요'**
  String get androidHostUpdateReading;

  /// No description provided for @androidCheckingOtherConnection.
  ///
  /// In ko, this message translates to:
  /// **'다른 연결이 있는지 확인하는 중이에요'**
  String get androidCheckingOtherConnection;

  /// No description provided for @androidCheckingDesktopAgain.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 상태를 다시 확인하는 중이에요'**
  String get androidCheckingDesktopAgain;

  /// No description provided for @androidOpeningConsole.
  ///
  /// In ko, this message translates to:
  /// **'Console을 여는 중이에요'**
  String get androidOpeningConsole;

  /// No description provided for @androidEndingConsole.
  ///
  /// In ko, this message translates to:
  /// **'Console 연결을 끊는 중이에요'**
  String get androidEndingConsole;

  /// No description provided for @androidUpdateCheckingPackage.
  ///
  /// In ko, this message translates to:
  /// **'설치 파일의 서명을 확인하는 중이에요'**
  String get androidUpdateCheckingPackage;

  /// No description provided for @androidUpdatePreparingInstall.
  ///
  /// In ko, this message translates to:
  /// **'설치를 준비하는 중이에요'**
  String get androidUpdatePreparingInstall;

  /// No description provided for @androidUpdateWaitingConfirm.
  ///
  /// In ko, this message translates to:
  /// **'Android 설치 화면의 확인을 기다리고 있어요'**
  String get androidUpdateWaitingConfirm;

  /// No description provided for @androidUpdateInstallingNow.
  ///
  /// In ko, this message translates to:
  /// **'Android가 업데이트를 설치하고 있어요.\n끝나면 앱이 닫혀요. 다시 열면 새 버전으로 시작해요.'**
  String get androidUpdateInstallingNow;

  /// No description provided for @androidReadingFolder.
  ///
  /// In ko, this message translates to:
  /// **'폴더를 읽는 중이에요'**
  String get androidReadingFolder;

  /// No description provided for @androidChangingPassword.
  ///
  /// In ko, this message translates to:
  /// **'비밀번호를 바꾸는 중이에요'**
  String get androidChangingPassword;

  /// No description provided for @androidTestingServer.
  ///
  /// In ko, this message translates to:
  /// **'서버 연결을 테스트하는 중이에요'**
  String get androidTestingServer;

  /// No description provided for @androidMouseLockConnecting.
  ///
  /// In ko, this message translates to:
  /// **'마우스를 데스크톱에 연결하는 중이에요'**
  String get androidMouseLockConnecting;

  /// No description provided for @tileOpeningConsole.
  ///
  /// In ko, this message translates to:
  /// **'Console을 여는 중이에요'**
  String get tileOpeningConsole;

  /// No description provided for @tileOpeningWindow.
  ///
  /// In ko, this message translates to:
  /// **'원격 창을 여는 중이에요'**
  String get tileOpeningWindow;

  /// No description provided for @tileRequestingHostUpdate.
  ///
  /// In ko, this message translates to:
  /// **'Host 업데이트를 요청하는 중이에요'**
  String get tileRequestingHostUpdate;

  /// No description provided for @signingOutNow.
  ///
  /// In ko, this message translates to:
  /// **'로그아웃하는 중이에요'**
  String get signingOutNow;

  /// No description provided for @loadingDesktopList.
  ///
  /// In ko, this message translates to:
  /// **'데스크톱 목록을 불러오는 중이에요'**
  String get loadingDesktopList;

  /// No description provided for @updateClosingRemoteWindows.
  ///
  /// In ko, this message translates to:
  /// **'열려 있는 원격 창을 닫는 중이에요'**
  String get updateClosingRemoteWindows;

  /// No description provided for @sessionEndClosingConnections.
  ///
  /// In ko, this message translates to:
  /// **'열린 연결을 닫는 중이에요'**
  String get sessionEndClosingConnections;

  /// No description provided for @hostStepDisplayPackage.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 파일이 원본과 같은지 확인하는 중이에요'**
  String get hostStepDisplayPackage;

  /// No description provided for @hostStepDisplayTrust.
  ///
  /// In ko, this message translates to:
  /// **'드라이버 서명을 확인하는 중이에요'**
  String get hostStepDisplayTrust;

  /// No description provided for @hostStepDisplayDevice.
  ///
  /// In ko, this message translates to:
  /// **'가상 화면 드라이버를 설치하는 중이에요. 화면이 잠시 깜빡일 수 있어요.'**
  String get hostStepDisplayDevice;

  /// No description provided for @hostStepDisplayService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스가 가상 화면을 여는 것을 기다리는 중이에요'**
  String get hostStepDisplayService;

  /// No description provided for @hostStepDisplayRestart.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스를 다시 시작하는 중이에요'**
  String get hostStepDisplayRestart;

  /// No description provided for @hostStepWinfspPackage.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 설치 파일을 확인하는 중이에요'**
  String get hostStepWinfspPackage;

  /// No description provided for @hostStepWinfspInstall.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp를 설치하는 중이에요'**
  String get hostStepWinfspInstall;

  /// No description provided for @hostStepWinfspRollback.
  ///
  /// In ko, this message translates to:
  /// **'WinFsp 설치를 되돌리는 중이에요'**
  String get hostStepWinfspRollback;

  /// No description provided for @hostStepWinfspWaiting.
  ///
  /// In ko, this message translates to:
  /// **'다른 설치가 끝나기를 기다리는 중이에요'**
  String get hostStepWinfspWaiting;

  /// No description provided for @hostStepWinfspCheck.
  ///
  /// In ko, this message translates to:
  /// **'설치된 WinFsp를 확인하는 중이에요'**
  String get hostStepWinfspCheck;

  /// No description provided for @hostStepUsbRemove.
  ///
  /// In ko, this message translates to:
  /// **'다른 USB 연결 기능을 지우는 중이에요'**
  String get hostStepUsbRemove;

  /// No description provided for @hostStepUsbInstall.
  ///
  /// In ko, this message translates to:
  /// **'Multitap USB 연결 기능을 설치하는 중이에요'**
  String get hostStepUsbInstall;

  /// No description provided for @hostPhaseTestingServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결해 응답을 기다리고 있어요.'**
  String get hostPhaseTestingServer;

  /// No description provided for @hostPhaseRequestingServer.
  ///
  /// In ko, this message translates to:
  /// **'서버에 연결 요청을 보내고 응답을 기다리고 있어요.'**
  String get hostPhaseRequestingServer;

  /// No description provided for @hostPhaseRestartingService.
  ///
  /// In ko, this message translates to:
  /// **'Host 서비스를 멈췄다가 다시 시작하고 있어요. 창을 닫지 마세요.'**
  String get hostPhaseRestartingService;

  /// No description provided for @hostCheckingInstallResult.
  ///
  /// In ko, this message translates to:
  /// **'설치 결과 확인 중'**
  String get hostCheckingInstallResult;

  /// No description provided for @hostPhaseCheckingInstallResult.
  ///
  /// In ko, this message translates to:
  /// **'설치한 항목을 Windows에서 다시 확인하고 있어요.'**
  String get hostPhaseCheckingInstallResult;

  /// No description provided for @hostCheckingRequestResult.
  ///
  /// In ko, this message translates to:
  /// **'요청 결과 확인 중'**
  String get hostCheckingRequestResult;

  /// No description provided for @hostPhaseCheckingRequestResult.
  ///
  /// In ko, this message translates to:
  /// **'Windows 자동 로그인 요청이 어떻게 끝났는지 조회하고 있어요.'**
  String get hostPhaseCheckingRequestResult;

  /// No description provided for @hostApprovalWaitingLine.
  ///
  /// In ko, this message translates to:
  /// **'승인을 기다리는 중이에요. 승인되면 바로 다음으로 넘어가요.'**
  String get hostApprovalWaitingLine;

  /// No description provided for @hostProgressPercent.
  ///
  /// In ko, this message translates to:
  /// **'{percent}%'**
  String hostProgressPercent(Object percent);

  /// No description provided for @usbUnreadableDevices.
  ///
  /// In ko, this message translates to:
  /// **'장치 정보를 읽지 못한 USB 장치가 {count}개 있어요. 빼고 다시 꽂아 보세요.'**
  String usbUnreadableDevices(Object count);
}

class _AppLocalizationsDelegate
    extends LocalizationsDelegate<AppLocalizations> {
  const _AppLocalizationsDelegate();

  @override
  Future<AppLocalizations> load(Locale locale) {
    return SynchronousFuture<AppLocalizations>(lookupAppLocalizations(locale));
  }

  @override
  bool isSupported(Locale locale) =>
      <String>['en', 'ko'].contains(locale.languageCode);

  @override
  bool shouldReload(_AppLocalizationsDelegate old) => false;
}

AppLocalizations lookupAppLocalizations(Locale locale) {
  // Lookup logic when only language code is specified.
  switch (locale.languageCode) {
    case 'en':
      return AppLocalizationsEn();
    case 'ko':
      return AppLocalizationsKo();
  }

  throw FlutterError(
    'AppLocalizations.delegate failed to load unsupported locale "$locale". This is likely '
    'an issue with the localizations generation tool. Please file an issue '
    'on GitHub with a reproducible sample app and the gen-l10n configuration '
    'that was used.',
  );
}
