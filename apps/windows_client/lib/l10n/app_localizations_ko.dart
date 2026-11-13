// ignore: unused_import
import 'package:intl/intl.dart' as intl;

import 'app_localizations.dart';

// ignore_for_file: type=lint

/// The translations for Korean (`ko`).
class AppLocalizationsKo extends AppLocalizations {
  AppLocalizationsKo([String locale = 'ko']) : super(locale);

  @override
  String get couldntSavePreviewSetting => '미리보기 설정을 저장하지 못했어요.';

  @override
  String get quitApp => '앱을 완전히 종료할까요?';

  @override
  String get appClosesRemovedFromRecent =>
      '앱을 닫고 최근 앱 목록에서 지워요.\n데스크톱은 계속 켜져 있어요.';

  @override
  String get quit => '완전 종료';

  @override
  String get disconnectRemoteScreenThenQuit => '원격 화면에서 연결을 끊은 뒤 종료해 주세요.';

  @override
  String get couldntQuitAppTryAgain => '앱을 종료하지 못했어요. 다시 시도하세요.';

  @override
  String get desktopNoLongerAssignedYou => '이 데스크톱은 더 이상 내게 배정돼 있지 않아요.';

  @override
  String get couldntChangeFavoritesTryAgain => '즐겨찾기를 바꾸지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get updateInstalled => '업데이트를 설치했어요.';

  @override
  String get couldntLoadSavedServerSettings =>
      '저장된 서버 설정을 불러오지 못했어요. 서버 설정에서 주소를 다시 입력하세요.';

  @override
  String get couldntLoadSavedSignDetails =>
      '저장된 로그인 정보를 불러오지 못했어요. 아이디와 비밀번호를 입력하세요.';

  @override
  String get couldntLoadServerSettings => '서버 설정을 불러오지 못했어요.';

  @override
  String get couldntTurnOffRememberPassword => '비밀번호 기억을 해제하지 못했어요.';

  @override
  String get enterIdPassword => '아이디와 비밀번호를 입력해 주세요.';

  @override
  String get signedButCouldntSaveRemember =>
      '로그인했지만 아이디·비밀번호 기억 설정을 저장하지 못했어요.';

  @override
  String get signHasExpiredSignAgain => '로그인이 만료됐어요. 다시 로그인해 주세요.';

  @override
  String get signOut => '로그아웃할까요?';

  @override
  String get server => '서버';

  @override
  String get desktopsRunningWorkStayAs => '데스크톱과 실행 중인 작업은 그대로 유지돼요.';

  @override
  String get signOut2 => '로그아웃';

  @override
  String get notificationsOffSoConnectionStatus =>
      '알림이 꺼져 있어 알림창에 연결 상태와 ‘연결 끊기’ 버튼이 보이지 않아요.';

  @override
  String get notificationSettings => '알림 설정';

  @override
  String get couldntProcessRequestCheckNetwork =>
      '요청을 처리하지 못했어요. 네트워크를 확인하고 다시 시도하세요.';

  @override
  String get anotherConnectionOpening => '다른 연결을 여는 중이에요.';

  @override
  String get desktopTaskProgress => '데스크톱 작업이 진행 중이에요.';

  @override
  String get startDesktopThenConnect => '데스크톱을 켠 뒤 연결하세요.';

  @override
  String get endCurrentConnectionThenConnect => '기존 연결을 끊은 뒤 연결하세요.';

  @override
  String get disconnectConsoleConnect => 'Console을 끊고 연결할까요?';

  @override
  String get openConsoleConnectionWillEnd =>
      '지금 열려 있는 Console 연결이 끊겨요.\n데스크톱과 실행 중인 작업은 그대로예요.';

  @override
  String get disconnectConnect => '끊고 연결';

  @override
  String get couldntEndConsoleConnection => 'Console 연결을 끊지 못했어요.';

  @override
  String get openConsole => 'Console로 열까요?';

  @override
  String opensDesktopsProxmoxConsoleApp(Object desktopName) {
    return '$desktopName\n\n앱 안에서 데스크톱의 Proxmox 콘솔 화면을 열어요.\nProxmox에 따로 로그인하지 않아도 돼요.';
  }

  @override
  String get openConsole2 => 'Console로 열기';

  @override
  String get couldntOpenConsoleCheckNetwork =>
      'Console을 열지 못했어요. 네트워크를 확인하고 다시 시도하세요.';

  @override
  String connect(Object assignedName) {
    return '$assignedName에 연결할까요?';
  }

  @override
  String get connect2 => '연결';

  @override
  String get couldntSaveStreamingSettingsThey =>
      '스트리밍 설정을 저장하지 못했어요. 이번 연결에는 적용해요.';

  @override
  String get bringOverWindowsScreen => 'Windows 화면을 가져올까요?';

  @override
  String continuesWindowsScreenYouWere(Object desktopName) {
    return '$desktopName\nRDP에서 쓰던 Windows 화면으로 이어서 연결해요.';
  }

  @override
  String get bringOverConnect => '가져와 연결';

  @override
  String waitingPreviousConnectionCleanUp(Object label) {
    return '이전 연결 정리를 기다리는 중이라 $label 요청을 받지 않았어요. 잠시 후 다시 시도하세요.';
  }

  @override
  String numberMonitorsChangingSoRequest(Object label) {
    return '모니터 수를 바꾸는 중이라 $label 요청을 받지 않았어요. 끝난 뒤 다시 시도하세요.';
  }

  @override
  String hostUpdateInstallingSoRequest(Object label) {
    return 'Host 업데이트를 설치하는 중이라 $label 요청을 받지 않았어요. 설치가 끝난 뒤 다시 시도하세요.';
  }

  @override
  String couldntProcessRequestTryAgain(Object label) {
    return '$label 요청을 처리하지 못했어요. 다시 시도하세요.';
  }

  @override
  String get autoConnectStoppedBecauseDesktop =>
      '5분 안에 연결 준비가 되지 않아 자동 연결을 멈췄어요. 연결 준비 점검에서 상태를 확인하세요.';

  @override
  String get autoConnectCanceled => '자동 연결을 취소했어요.';

  @override
  String get couldntFindDesktop => '이 데스크톱을 찾지 못했어요';

  @override
  String get unknown => '미확인';

  @override
  String get passwordChangedSignNewPassword => '비밀번호를 바꿨어요. 새 비밀번호로 로그인해 주세요.';

  @override
  String get powerTaskProgress => '전원 작업이 진행 중이에요.';

  @override
  String get serverLocalPcsHostDoesnt => '이 서버나 로컬 PC의 Host는 원격 전원을 지원하지 않아요.';

  @override
  String get checkingDesktopsPowerStateRefresh =>
      '데스크톱 전원 상태를 확인하는 중이에요. 잠시 후 새로고침하세요.';

  @override
  String get numberMonitorsChangingPowerAvailable =>
      '모니터 수를 바꾸는 중이에요. 끝나면 전원을 쓸 수 있어요.';

  @override
  String get refresh => '새로고침';

  @override
  String get checkingList => '목록을 확인하는 중이에요';

  @override
  String get refreshList => '목록 새로고침';

  @override
  String get previews => '미리보기';

  @override
  String get screenPreviewsTapTurnOff => '화면 미리보기 켜짐 · 누르면 꺼요';

  @override
  String get screenPreviewsOffTapTurn => '화면 미리보기 꺼짐 · 누르면 켜요';

  @override
  String get search => '검색';

  @override
  String sort(Object sortLabel) {
    return '정렬 · $sortLabel';
  }

  @override
  String get clearAllFilters => '필터를 모두 해제';

  @override
  String get myDesktops => '내 데스크톱';

  @override
  String get tryAgain => '다시 시도';

  @override
  String get couldntLoadList => '목록을 불러오지 못했어요';

  @override
  String get noDesktopsAssignedYet => '아직 배정된 데스크톱이 없어요';

  @override
  String get askAdministratorAssignDesktopPull =>
      '관리자에게 데스크톱 배정을 요청하세요.\n아래로 당기면 다시 확인해요.';

  @override
  String listMayOutDate(Object listError) {
    return '목록이 최신이 아닐 수 있어요. $listError';
  }

  @override
  String get clearFilters => '필터 해제';

  @override
  String get clearSearch => '검색 지우기';

  @override
  String connectionOkMultitapMs(Object version, Object elapsedMs) {
    return '연결 확인 · Multitap $version · $elapsedMs ms';
  }

  @override
  String get checkServerAddressPort => '서버 주소와 포트를 확인해 주세요.';

  @override
  String get couldntSaveServerAddress => '서버 주소를 저장하지 못했어요.';

  @override
  String get signServer => '로그인 서버';

  @override
  String get httpsSecureConnection => 'HTTPS 보안 연결';

  @override
  String get httpNotEncrypted => 'HTTP · 암호화 없음';

  @override
  String get youCanSaveAddressAfter => '테스트에 성공하면 이 주소를 저장할 수 있어요.';

  @override
  String get serverAddress => '서버 주소';

  @override
  String get httpsServerAddress => 'https://서버주소';

  @override
  String get testConnection => '연결 테스트';

  @override
  String get cancel => '취소';

  @override
  String get save => '저장';

  @override
  String get streamingSettings => '스트리밍 설정';

  @override
  String get custom => '사용자 설정';

  @override
  String qualityMode(Object value) {
    return '화질 모드 · $value';
  }

  @override
  String get useRecommended => '권장값 사용';

  @override
  String get workKeepsTextSharpSaver => '작업은 글자를 선명하게, 절약은 전송량을 줄여요.';

  @override
  String get advancedSettings => '고급 설정';

  @override
  String get targetFrameRate => '목표 프레임';

  @override
  String get videoBitrateMbps => '영상 전송률 · Mbps';

  @override
  String get auto => '자동';

  @override
  String get fitsOneVirtualDisplayDevices =>
      '가상 화면 1개를 이 기기 크기에 맞춰요.\n다른 스트리밍 연결은 끊어질 수 있어요.';

  @override
  String get newPasswordMust10256 => '새 비밀번호는 10~256자이고 확인 입력과 같아야 해요.';

  @override
  String get couldntChangePassword => '비밀번호를 바꾸지 못했어요.';

  @override
  String get changePassword => '비밀번호 변경';

  @override
  String get changingSignsYouOutAll => '변경하면 모든 기기에서 로그아웃돼요.';

  @override
  String get currentPassword => '현재 비밀번호';

  @override
  String get newPassword => '새 비밀번호';

  @override
  String get confirmNewPassword => '새 비밀번호 확인';

  @override
  String get least10Characters => '10자 이상';

  @override
  String get change => '변경';

  @override
  String get disconnect => '연결 끊기';

  @override
  String get keyboard => '키보드';

  @override
  String get screen => '화면';

  @override
  String get originalSize => '원본 크기';

  @override
  String get windowsKey => 'Windows 키';

  @override
  String get reconnect => '다시 연결';

  @override
  String get close => '닫기';

  @override
  String get console => '콘솔';

  @override
  String get noPermissionOpenFolderChoose => '이 폴더를 열 권한이 없어요. 폴더를 다시 골라 주세요.';

  @override
  String get cantFindFolderDeviceMay =>
      '이 기기에서 폴더를 찾을 수 없어요. 지웠거나 SD 카드·USB 저장장치가 빠졌을 수 있어요.';

  @override
  String get filesAppTurnedOffSo =>
      '‘파일’ 앱이 꺼져 있어서 폴더를 고를 수 없어요.\n‘파일’ 앱 설정에서 ‘사용’을 누른 뒤 다시 시도하세요.';

  @override
  String get deviceHasNoFolderPicker =>
      '이 기기에는 폴더를 고르는 화면이 없어서 공유 폴더를 쓸 수 없어요.';

  @override
  String get couldntOpenFolderPickerTry =>
      '폴더를 고르는 화면을 열지 못했어요.\n다시 시도하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get device => '이 기기';

  @override
  String get openFilesAppSettings => '‘파일’ 앱 설정 열기';

  @override
  String get youreAboutShareAllStorage =>
      '저장공간 전체를 공유하려고 해요.\n데스크톱의 프로그램이 이 저장공간의 모든 파일을 읽고 바꿀 수 있어요.\n꼭 필요한 폴더만 고르는 걸 권해요.';

  @override
  String get chosenFolder => '고른 폴더';

  @override
  String stoppedSharingBecausePermissionOpen(Object name) {
    return '‘$name’ 폴더를 열 권한이 없어져서 공유를 끝냈어요';
  }

  @override
  String stoppedSharingBecauseFolderCant(Object name) {
    return '‘$name’ 폴더를 찾을 수 없어서 공유를 끝냈어요';
  }

  @override
  String get cantOpenFoldersDeviceCheck =>
      '이 기기에서 폴더를 열 수 없어요. SD 카드·USB 저장장치를 확인하고 다시 시도하세요.';

  @override
  String get timesAverageMaximumOverLast =>
      '시간은 프레임이 있었던 최근 1초의 평균과 최대예요.\n화면에 실제로 보이기까지의 시간은 포함하지 않아요.';

  @override
  String get notMeasured => '미측정';

  @override
  String avg(Object average) {
    return '평균 $average';
  }

  @override
  String avgMax(Object average, Object highest) {
    return '평균 $average · 최대 $highest';
  }

  @override
  String get preparing => '준비 중';

  @override
  String get defaultFallbackAfterError => '기본 설정 (오류 뒤 대체)';

  @override
  String get androidLowLatency => 'Android 저지연';

  @override
  String get realTimePriority => '실시간 우선';

  @override
  String get vendor => '제조사';

  @override
  String lowLatencyKeys(Object vendorKeys) {
    return '$vendorKeys 저지연 키';
  }

  @override
  String get offDefault => '꺼짐 (기본 설정)';

  @override
  String get onLabel => '켜짐';

  @override
  String get off => '꺼짐';

  @override
  String get normal => '정상';

  @override
  String get slightlyWarm => '약간 따뜻함';

  @override
  String get warm => '따뜻함';

  @override
  String get hot => '뜨거움';

  @override
  String get veryHot => '매우 뜨거움';

  @override
  String get critical => '위험';

  @override
  String get aboutShutDown => '종료 직전';

  @override
  String highestConnection(Object now, Object top) {
    return '$now (이번 연결 최고: $top)';
  }

  @override
  String get requested => '요청함';

  @override
  String get requestedHighPerformanceMode => '요청함 (고성능 모드)';

  @override
  String get refusedByDevice => '기기가 거절함';

  @override
  String get notRequested => '요청 안 함';

  @override
  String get decoderLowLatency => '디코더 저지연';

  @override
  String get decoderFallback => '디코더 대체';

  @override
  String get none => '없음';

  @override
  String text(num fallbacks) {
    return '$fallbacks회';
  }

  @override
  String get decodeTime => '디코딩 시간';

  @override
  String get receivedDecoded => '수신→디코딩 완료';

  @override
  String get decodedSentScreen => '디코딩 완료→화면 전달';

  @override
  String get framesReplacedWaitingDropped => '프레임 (교체 · 대기 · 버림)';

  @override
  String get audioBuffer => '오디오 버퍼';

  @override
  String queuedTargetBuffer(Object queued, Object target, Object buffer) {
    return '대기 $queued · 목표 $target · 버퍼 $buffer';
  }

  @override
  String get audioOutput => '오디오 출력';

  @override
  String get lowLatencyPath => '저지연 경로';

  @override
  String lowLatencyPathLatency(Object output) {
    return '저지연 경로 · 지연 $output';
  }

  @override
  String get normalPath => '일반 경로';

  @override
  String normalPathLatency(Object output) {
    return '일반 경로 · 지연 $output';
  }

  @override
  String get displayRefreshRate => '화면 주사율';

  @override
  String get sharp => '선명하게';

  @override
  String get wiFiLowLatency => 'Wi-Fi 저지연';

  @override
  String get deviceHeat => '기기 발열';

  @override
  String get allowAccessAllFiles => '모든 파일 접근을 허용할까요?';

  @override
  String get chooseFolderMultitapNeedsSee =>
      '폴더를 고르려면 Multitap이 이 기기의 저장공간을 볼 수 있어야 해요.\n허용하면 Multitap이 저장공간을 읽고 쓸 수 있어요. 데스크톱에는 고른 폴더만 보여요.';

  @override
  String get openSettings => '설정 열기';

  @override
  String get cantOpenFolderChooseAnother => '이 폴더는 열 수 없어요. 다른 폴더를 골라 주세요.';

  @override
  String get upOneFolder => '위 폴더로';

  @override
  String get chooseFolderShare => '공유할 폴더 고르기';

  @override
  String get storage => '저장공간';

  @override
  String get couldntFindStorage => '저장공간을 찾지 못했어요.';

  @override
  String get noFoldersInside => '안에 폴더가 없어요.';

  @override
  String get shareFolder => '이 폴더 공유';

  @override
  String get lostConnectionHostCouldntConfirm =>
      'Host 연결이 끊겼어요. 업데이트 요청 결과는 확인하지 못했어요.';

  @override
  String get couldntConfirmResultUpdateRequest =>
      '업데이트 요청 결과를 확인하지 못했어요. 목록을 다시 확인해 주세요.';

  @override
  String get couldntConfirmResultUpdateRequest2 => '업데이트 요청 결과를 확인하지 못했어요.';

  @override
  String currentStatus(Object observed) {
    return '현재 상태 · $observed';
  }

  @override
  String lastCheckedStatus(Object observed) {
    return '마지막으로 확인한 상태 · $observed';
  }

  @override
  String get localPc => '로컬 PC';

  @override
  String get desktop => '데스크톱';

  @override
  String serverSignStatusChangedCancel(Object kind) {
    return '서버·로그인 또는 $kind 상태가 바뀌었어요. 취소한 뒤 다시 열어 주세요.';
  }

  @override
  String get targetListDiffersFromLatest =>
      '목록의 대상과 최신 Host 배포 버전이 달라요. 목록을 새로고침한 뒤 다시 확인해 주세요.';

  @override
  String get couldntFindLatestHostRelease => '최신 Host 배포 정보를 찾지 못했어요.';

  @override
  String get couldntCheckHostReleaseInformation =>
      'Host 배포 정보와 변경 내역을 확인하지 못했어요. 다시 확인해 주세요.';

  @override
  String get hostUpdate => 'Host 업데이트';

  @override
  String currentNewVersion(Object currentVersion, Object targetVersion) {
    return '현재 $currentVersion\n새 버전 $targetVersion';
  }

  @override
  String get updateStartsWhenConnectionUse => '사용 중인 연결이 끝나면 업데이트를 시작해요.';

  @override
  String updatesOnlyHostProgramYou(Object kind) {
    return '$kind의 Host 프로그램만 업데이트해요.\n설치 중에는 잠시 연결할 수 없어요.';
  }

  @override
  String get changeLog => '변경 내역';

  @override
  String get noChangeLogRegisteredServer => '이 서버에 변경 내역이 등록되지 않았어요.';

  @override
  String get checkAgain => '다시 확인';

  @override
  String get update => '업데이트';

  @override
  String get themeBackground => '테마와 배경';

  @override
  String get theme => '테마를';

  @override
  String get background => '배경을';

  @override
  String couldntSaveChooseAgain(Object what) {
    return '$what 저장하지 못했어요. 다시 선택해 주세요.';
  }

  @override
  String get dark => '다크';

  @override
  String get light => '라이트';

  @override
  String background2(Object sceneLabel) {
    return '배경 · $sceneLabel';
  }

  @override
  String hostUpdates(Object hostUpdates) {
    return 'Host 업데이트 $hostUpdates대';
  }

  @override
  String newVersion(Object update) {
    return '새 버전 $update';
  }

  @override
  String get checkUpdates => '업데이트 확인';

  @override
  String get aboutLicenses => '정보·라이선스';

  @override
  String get androidClient => 'Android 클라이언트';

  @override
  String get viewLogs => '로그 보기';

  @override
  String get twoStepVerification => '2단계 인증';

  @override
  String accountMenu(Object account) {
    return '계정 메뉴 · $account';
  }

  @override
  String get idleAutoSignOut => '유휴 자동 로그아웃';

  @override
  String get serverSetupNeeded => '서버 설정 필요';

  @override
  String get changeServer => '서버 변경';

  @override
  String get id => '아이디';

  @override
  String get password => '비밀번호';

  @override
  String get rememberPassword => '비밀번호 기억';

  @override
  String get passwordsRememberedOnlyHttps => 'HTTPS 서버에서만 비밀번호를 기억해요.';

  @override
  String get passwordEncryptedKeptDevice => '비밀번호를 암호화해 이 기기에 보관해요.';

  @override
  String get signing => '로그인 중…';

  @override
  String get sign => '로그인';

  @override
  String get serverSettings => '서버 설정';

  @override
  String get askAdministratorAccount => '계정은 관리자에게 요청하세요';

  @override
  String get androidStreaming => 'Android · 스트리밍';

  @override
  String newVersion2(Object updateVersion) {
    return '새 버전 $updateVersion';
  }

  @override
  String maintenanceNotice(Object noticeTitle) {
    return '점검 예고 · $noticeTitle';
  }

  @override
  String printerConnectionEnded(num lostDocuments) {
    return '프린터 연결이 끊겼어요. 아직 처리하지 않은 문서 $lostDocuments개는 받을 수 없게 됐어요.';
  }

  @override
  String get printerConnectionEndedReconnectPrint =>
      '프린터 연결이 끊겼어요. 다시 연결하면 인쇄할 수 있어요.';

  @override
  String get couldntOpenReceivedPrintDocument =>
      '받은 인쇄 문서를 열지 못했어요. 데스크톱에서 다시 인쇄하세요.';

  @override
  String get androidPrintingIsntAvailableCheck =>
      'Android 인쇄를 사용할 수 없어요. 기기의 인쇄 서비스를 확인해 주세요.';

  @override
  String get couldntPassPrintDocumentPrint =>
      '인쇄 문서를 전달하지 못했어요. 데스크톱에서 다시 인쇄하세요.';

  @override
  String get receivingPrintDocument => '인쇄 문서를 받는 중';

  @override
  String receivingPrintDocument2(Object label) {
    return '인쇄 문서 수신 · $label';
  }

  @override
  String get couldntKeepConnectionRunningBackground =>
      '백그라운드 연결 유지를 시작하지 못했어요. 앱을 벗어나면 연결이 끊길 수 있어요.';

  @override
  String get stoppedPreparingConnectionWhileOutside =>
      '앱 밖에서 연결 준비를 중단했어요. 앱으로 돌아오면 다시 연결해요.';

  @override
  String get hostsVirtualDisplayIsntReady =>
      'Host의 가상 화면 준비가 아직 끝나지 않았어요. 잠시 후 다시 연결하세요.';

  @override
  String get desktopWasntReadyWithin3 =>
      '데스크톱이 3분 안에 준비되지 않았어요.\n잠시 후 ‘다시 연결’을 누르세요.';

  @override
  String get connectionEndedCouldntReconnectCheck =>
      '연결이 끊겼고 다시 연결하지 못했어요. 네트워크를 확인하고 다시 연결하세요.';

  @override
  String get couldntChangeWindowsScaleScreen =>
      'Windows 배율을 바꾸지 못했어요. 화면은 그대로 선명하게 보여요.';

  @override
  String setLargestScaleWindowsAllows(Object scale) {
    return '이 화면 크기에서 Windows가 허용하는 최대 배율 $scale%로 맞췄어요.';
  }

  @override
  String get couldntSendInputWhileScreen =>
      '화면 크기를 맞추는 중이라 입력을 보내지 못했어요. 끝난 뒤 다시 입력하세요.';

  @override
  String get couldntConfirmInputWasDelivered =>
      '입력 전달을 확인하지 못해 키보드를 닫았어요. 다시 열어 입력해 주세요.';

  @override
  String get hostReceivesKoreanTextAs =>
      '이 Host는 한글을 키 입력으로 받아요. 데스크톱이 한글 입력 상태일 때 그대로 입력되고, 영문이 나오면 키 줄의 한/영을 누르세요.';

  @override
  String get controlCharactersCantSentSo => '제어 문자는 보낼 수 없어서 뺐어요.';

  @override
  String get hostCantReceiveSomeCharacters =>
      '이 Host는 이모지 같은 일부 문자를 받을 수 없어요. Host를 업데이트하면 보낼 수 있어요.';

  @override
  String get mouseLock => '마우스 잠금';

  @override
  String get tapOkLockMouseInside =>
      '확인을 누르면 마우스를 데스크톱 안에 잠가요.\n뒤로 가기를 누르면 풀려요.';

  @override
  String get mouseLockedInsideDesktopPress => '마우스를 데스크톱 안에 잠갔어요 · 뒤로 가기로 풀기';

  @override
  String get someGamesSuchAsLeague => '롤 같은 일부 게임에서는 마우스가 움직이지 않을 수 있어요';

  @override
  String get mouseLockOff => '마우스 잠금 꺼짐';

  @override
  String get couldntFitScreenSizeTap =>
      '화면 크기를 맞추지 못했어요. 메뉴 맨 위 연결 품질에서 ‘화면 크기 다시 적용’을 누르세요.';

  @override
  String get deviceCantReceiveSharperPicture =>
      '이 기기에서는 더 선명하게 받을 수 없어 자동 크기로 보여요.';

  @override
  String get couldntApplySharpSoShows => '선명하게를 적용하지 못해 자동 크기로 보여요.';

  @override
  String get stoppedWaitingHostUpdateTap =>
      'Host 업데이트 기다리기를 취소했어요.\n업데이트가 끝나면 ‘다시 연결’을 누르세요.';

  @override
  String get stoppedWaitingTapReconnectWhen =>
      '기다리기를 취소했어요.\n데스크톱이 준비되면 ‘다시 연결’을 누르세요.';

  @override
  String get screenSettings => '화면 설정';

  @override
  String get applyReconnect => '적용 후 다시 연결';

  @override
  String get hostCantChangeScreenSize =>
      '이 Host는 연결 중 화면 크기 변경을 지원하지 않아요. 다음 연결부터 적용돼요.';

  @override
  String get resolutionWindowsScaleSeparate => '해상도와 Windows 배율은 따로 적용돼요.';

  @override
  String resolutionWindowsScaleApplySeparately(
    Object session,
    Object session2,
  ) {
    return '$session\n$session2\n해상도와 Windows 배율은 따로 적용돼요.';
  }

  @override
  String current(Object width, Object height) {
    return '현재 $width×$height';
  }

  @override
  String requested2(Object width, Object height) {
    return ' · 요청 $width×$height';
  }

  @override
  String current2(Object currentWidth, Object currentHeight) {
    return '현재 $currentWidth×$currentHeight';
  }

  @override
  String get checkingCurrent => '현재 확인 중';

  @override
  String recommended(
    Object desired,
    Object desired2,
    Object requested,
    Object current,
  ) {
    return '권장 $desired×$desired2$requested · $current';
  }

  @override
  String get supportedRange => '지원 범위';

  @override
  String get deviceSupport => '기기 지원';

  @override
  String selected(
    Object desiredWidth,
    Object desiredHeight,
    Object requestNote,
    Object current,
  ) {
    return '선택 $desiredWidth×$desiredHeight$requestNote · $current';
  }

  @override
  String sharp2(Object width, Object height, Object plan, Object current) {
    return '선명하게 $width×$height$plan · $current';
  }

  @override
  String couldntApplySharpSoShows2(Object sharpRefusedReason, Object current) {
    return '선명하게를 적용하지 못해 자동 크기로 보여요 ($sharpRefusedReason) · $current';
  }

  @override
  String checkingSharp(Object current) {
    return '선명하게 확인 중 · $current';
  }

  @override
  String cantReceiveSharperPictureSo(Object planned, Object current) {
    return '더 선명하게 받을 수 없어 자동 크기로 보여요$planned · $current';
  }

  @override
  String remoteResolution(Object width, Object height) {
    return '원격 해상도 $width × $height';
  }

  @override
  String get remoteResolutionUnknown => '원격 해상도 미확인';

  @override
  String get connected => '연결됨';

  @override
  String windowsScale(Object session, Object session2) {
    return '$session\nWindows 배율 · $session2';
  }

  @override
  String get remoteScreen => '원격 화면';

  @override
  String get n1Screen => '화면 1개';

  @override
  String get n2Screens => '화면 2개';

  @override
  String get screenShow => '표시할 화면';

  @override
  String screen2(Object intValue, Object trackId) {
    return '화면 $intValue · $trackId';
  }

  @override
  String get view => '보기';

  @override
  String get wholeScreen => '전체 보기';

  @override
  String get fill => '꽉 채우기';

  @override
  String get zoomOut => '축소';

  @override
  String zoom(Object scale) {
    return '확대 $scale%';
  }

  @override
  String get zoomsDeviceOnlyRemoteResolution => '이 기기에서만 확대 · 원격 해상도는 그대로예요';

  @override
  String get zoom2 => '확대';

  @override
  String get pan => '화면 이동';

  @override
  String get useWhenZoomedWhenKeyboard => '확대했거나 키보드가 가릴 때 써요';

  @override
  String get pointerSpeed => '포인터 속도';

  @override
  String get inputMode => '입력 방식';

  @override
  String get streaming => '스트리밍';

  @override
  String get reapplyScreenSize => '화면 크기 다시 적용';

  @override
  String get pressBackRelease => '뒤로 가기로 풀어요';

  @override
  String get locksMouseInsideDesktop => '마우스를 데스크톱 안에 잠가요';

  @override
  String get availableWhenMouseConnected => '마우스를 연결하면 쓸 수 있어요';

  @override
  String get touchControlsPrinterCopy => '터치 조작 · 프린터 · 복사';

  @override
  String speed(Object modeLabel, Object speedLabel) {
    return '$modeLabel · 속도 $speedLabel';
  }

  @override
  String get shortcutsAvailableOnceScreenSize => '화면 크기를 맞춘 뒤 단축키를 쓸 수 있어요';

  @override
  String get switchApps => '앱 전환';

  @override
  String get taskManager => '작업 관리자';

  @override
  String get edit => '편집';

  @override
  String get copy => '복사';

  @override
  String get paste => '붙여넣기';

  @override
  String get selectAll => '전체 선택';

  @override
  String get undo => '실행 취소';

  @override
  String get cantSend => '보낼 수 없어요';

  @override
  String get ctrlAltDelCantSent =>
      '스트리밍에서는 Ctrl+Alt+Del을 보낼 수 없어요. 작업 관리자는 Ctrl+Shift+Esc로 열 수 있어요.';

  @override
  String get youCanChangeOnceConnected => '연결된 뒤 바꿀 수 있어요';

  @override
  String get screenWholeScreenZoomPan => '화면 · 전체 보기, 확대, 화면 이동';

  @override
  String get screenViewScaleResolution => '화면 · 보기, 배율, 해상도';

  @override
  String text2(num mapped) {
    return '공유 폴더 $mapped개 연결됨';
  }

  @override
  String get useDevicesFoldersDesktop => '이 기기의 폴더를 데스크톱에서 쓰기';

  @override
  String get availableOnceConnected => '연결된 뒤 쓸 수 있어요';

  @override
  String get settingsPictureQualityMouse => '설정 · 화질, 마우스 잠금, 입력 방식, 사용안내';

  @override
  String get screenSizeAdjustingChooseShortcut =>
      '화면 크기를 맞추는 중이에요. 끝난 뒤 단축키를 다시 선택하세요.';

  @override
  String get couldntConfirmShortcutWasSent =>
      '단축키 전송 결과를 확인하지 못했어요. 원격 화면을 확인한 뒤 필요하면 다시 선택해 주세요.';

  @override
  String get technology => '연결 기술';

  @override
  String get decoder => '디코더';

  @override
  String get hardwareDecoding => '하드웨어 디코딩';

  @override
  String get onLabel2 => '사용';

  @override
  String get off2 => '미사용';

  @override
  String get appCpu => '앱 CPU';

  @override
  String get receivedSize => '수신 화면';

  @override
  String get remoteOriginal => '원격 원본';

  @override
  String get devicePixels => '기기 화소';

  @override
  String get half => '절반';

  @override
  String fixed(Object shortSide) {
    return '고정 $shortSide';
  }

  @override
  String selected2(Object width, Object height) {
    return '선택 $width×$height';
  }

  @override
  String selected3(Object desktopLabel) {
    return '선택 $desktopLabel';
  }

  @override
  String selected4(Object workspaceLabel) {
    return '선택 $workspaceLabel';
  }

  @override
  String get windowsScale2 => 'Windows 배율';

  @override
  String get pixelScale => '화소 배율';

  @override
  String get zoomFitInterpolated => '확대/맞춤 (보간)';

  @override
  String devicePixels1RemotePixel(Object viewPixelScale) {
    return '기기 화소 $viewPixelScale개 = 원격 1화소';
  }

  @override
  String get mouseCursor => '마우스 커서';

  @override
  String get shownDevice => '이 기기에서 표시';

  @override
  String get video => '영상에 포함';

  @override
  String get audio => '오디오';

  @override
  String get framesNetwork => '프레임과 네트워크';

  @override
  String get sourceCapture => '소스 캡처';

  @override
  String get decodingDevice => '이 기기 디코딩';

  @override
  String get sentScreen => '화면 전달';

  @override
  String get receiveRate => '수신 속도';

  @override
  String get inputAppliedHostEstP50 => '입력→Host 적용 (추정 p50/p95)';

  @override
  String get inputNewFrameDecodedP50 => '입력→새 프레임 디코딩 (p50/p95)';

  @override
  String get inputLatency => '입력 지연';

  @override
  String get hostDoesntSupportMeasuring => '이 Host는 측정을 지원하지 않아요';

  @override
  String get audioDrops => '오디오 드롭';

  @override
  String text3(num frames) {
    return '$frames프레임';
  }

  @override
  String get inputNewFrameDecodedRuns =>
      '입력→새 프레임 디코딩은 입력 송신부터 측정 대상 프레임의 MediaCodec 출력 콜백까지예요. 이후 프레임의 텍스처 전달·폐기, Flutter 합성과 실제 화면 표시는 포함하지 않아요. RTT는 이 지연 측정값이 아니에요.';

  @override
  String get processingDevice => '이 기기 처리';

  @override
  String get connectionStats => '연결 통계';

  @override
  String streaming2(Object name) {
    return '$name · 스트리밍';
  }

  @override
  String get detailedDiagnosticsOffByDefault =>
      '자세한 진단은 기본으로 꺼져 있어요.\nHost·수신 큐의 계측 설정은 다음 연결부터 적용돼요.';

  @override
  String get backList => '목록으로 돌아가기';

  @override
  String get reconnectAutoResolution => '자동 해상도로 다시 연결';

  @override
  String get pictureQualitySettings => '화질 설정';

  @override
  String get disconnectConnectHere => '끊고 여기서 연결';

  @override
  String get hostPreparingVirtualDisplayPlease =>
      'Host가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.';

  @override
  String get hostUpdatingReconnectsAutomaticallyWhen =>
      'Host 업데이트 중이에요.\n끝나면 자동으로 다시 연결해요.';

  @override
  String get waitingDesktopConnectsAutomaticallyWhen =>
      '데스크톱을 기다리고 있어요.\n준비되면 자동으로 연결해요.';

  @override
  String get connectionDroppedReconnecting => '연결이 끊겨 다시 연결하고 있어요.';

  @override
  String get requestingConnection => '연결을 요청하고 있어요.';

  @override
  String get stopWaiting => '기다리기 취소';

  @override
  String get hostUpdating => 'Host 업데이트 중';

  @override
  String get waitingDesktop => '데스크톱을 기다리는 중';

  @override
  String get desktopRestarting => '데스크톱 다시 시작 중';

  @override
  String get reconnecting2 => '다시 연결 중';

  @override
  String get fittingScreenSize => '화면 크기 맞추는 중';

  @override
  String get nowPointerTrackpadTapTouch =>
      '지금 포인터(트랙패드) · 눌러서 터치로 · 길게 누르면 포인터 속도';

  @override
  String get nowTouchTapPointerTrackpad =>
      '지금 터치 · 눌러서 포인터(트랙패드)로 · 길게 누르면 입력 방식';

  @override
  String get closeKeyboard => '키보드 닫기';

  @override
  String get openKeyboard => '키보드 열기';

  @override
  String get phone => '휴대폰';

  @override
  String get foldableInnerScreen => '폴더블 안쪽 화면';

  @override
  String get tablet => '태블릿';

  @override
  String get fitDevice => '기기에 맞춤';

  @override
  String get n125 => '1.25배';

  @override
  String get n15 => '1.5배';

  @override
  String get n2 => '2배';

  @override
  String get autoRotate => '자동 회전';

  @override
  String get landscape => '가로';

  @override
  String get portrait => '세로';

  @override
  String get autoRecommended => '자동 (권장)';

  @override
  String lockedTapRelease(Object mLabel) {
    return '$mLabel 고정됨 · 눌러서 해제';
  }

  @override
  String pressedCombinesNextKey(Object mLabel) {
    return '$mLabel 눌림 · 다음 키와 함께';
  }

  @override
  String longPressLock(Object mLabel) {
    return '$mLabel · 길게 눌러 고정';
  }

  @override
  String get hanEng => '한/영';

  @override
  String get ctrlAltDelCantSent2 => 'Ctrl+Alt+Del은 보낼 수 없어요';

  @override
  String get basicKeys => '기본 키로';

  @override
  String get f1F12NavigationKeys => 'F1–F12 · 탐색 키';

  @override
  String get actualValueUnknownNotSupported => '실제 값 미확인 · Host 미지원';

  @override
  String get manual => '수동';

  @override
  String actualValueUnknown(Object mode) {
    return '$mode · 실제 값 미확인';
  }

  @override
  String requestedActualValueUnknown(Object mode, Object wanted) {
    return '$mode $wanted% 요청 · 실제 값 미확인';
  }

  @override
  String requested3(Object wanted) {
    return ' $wanted% 요청';
  }

  @override
  String cantChangeActualValueUnknown(Object mode, Object wanted) {
    return '$mode$wanted · 바꾸지 못함 · 실제 값 미확인';
  }

  @override
  String nowRequested(Object scale, Object mode, Object wanted) {
    return '현재 $scale% · $mode $wanted% 요청';
  }

  @override
  String now(Object scale, Object mode) {
    return '현재 $scale% · $mode';
  }

  @override
  String get resolution => '해상도';

  @override
  String get recommended2 => '권장';

  @override
  String get fitted => '맞춰 그림';

  @override
  String get screensRatio => '이 화면 비율';

  @override
  String get usesDevicesOwnPixelsHost =>
      '배율을 바꿀 수 있는 Host에서는 기기 화소 그대로, 아니면 읽기 편한 정수 배로 맞춰요';

  @override
  String get recommendedSizeKeepsDeviceRatio =>
      '기기 비율을 유지하는 권장 크기 · 실제 적용 크기는 현재 해상도에서 확인하세요';

  @override
  String get sharpest => '가장 선명해요';

  @override
  String get quarterPixelsDrawnCrisply2 =>
      '화소 4분의 1 · 2:1로 또렷하게 그려 Host와 TV의 부담이 줄어요';

  @override
  String get fixedSizeScreensRatio => '이 화면 비율의 고정 크기';

  @override
  String get sizeKeepsDeviceRatioSmall => '기기 비율을 유지하는 크기 · 작은 화면을 확대하지 않아요';

  @override
  String get remoteWorkspaceSizeCheckApplied =>
      '원격 작업 공간 크기 · 실제 적용 크기는 현재 해상도에서 확인하세요';

  @override
  String get remoteWorkspaceFittedDevicesScreen =>
      '기기 화면 비율에 맞춘 원격 작업 공간 · 실제 적용 크기는 현재 해상도에서 확인하세요';

  @override
  String windowsScale3(Object deviceClassLabel) {
    return 'Windows 배율 · $deviceClassLabel';
  }

  @override
  String get calculating => '계산 중';

  @override
  String get choosesSuitTvsResolution => 'TV 화면의 해상도에 맞춰 골라요';

  @override
  String get orientation => '화면 방향';

  @override
  String get reconnectChangedSettings => '바꾼 설정으로 다시 연결해요';

  @override
  String get noSettingsChanged => '바꾼 설정이 없어요';

  @override
  String get qualityMode2 => '화질 모드';

  @override
  String get tapSwipe => '탭 · 밀기';

  @override
  String get sentAsWindowsTouchWorks => 'Windows 터치로 그대로 전달 · 터치 화면처럼 조작';

  @override
  String get longPress => '길게 누르기';

  @override
  String get rightClick => '오른쪽 클릭';

  @override
  String get twoFingers => '두 손가락';

  @override
  String get zoomScrollInsideAppsWhen => '앱 안에서 확대·스크롤 (앱이 지원할 때)';

  @override
  String get slideOneFinger => '한 손가락으로 밀기';

  @override
  String get moveCursor => '커서 이동';

  @override
  String get tap => '탭';

  @override
  String get clickCursorDoubleTapDouble => '커서 위치를 클릭 · 두 번 탭은 더블 클릭';

  @override
  String get doubleTapMoveWhileHolding => '두 번 탭하고 두 번째를 누른 채 움직이기';

  @override
  String get dragSelectMoveWindows => '끌기 · 범위 선택 · 창 이동';

  @override
  String get twoFingersUpDown => '두 손가락으로 위아래';

  @override
  String get scroll => '스크롤';

  @override
  String get clickWhereYouTapDouble => '누른 곳을 클릭 · 두 번 탭은 더블 클릭';

  @override
  String get oneFingerUpDown => '한 손가락으로 위아래';

  @override
  String get doubleTapMoveWhileHolding2 => '두 번 탭하고 누른 채 움직이기';

  @override
  String get longPressTwoFingerTap => '길게 누르기 · 두 손가락 탭';

  @override
  String get spreadTwoFingers => '두 손가락 벌리기';

  @override
  String get zoomPinchBackSeeWhole => '확대 · 오므리면 전체 보기까지';

  @override
  String get threeFingerTap => '세 손가락 탭';

  @override
  String get openCloseKeyboard => '키보드 열기·닫기';

  @override
  String get fourFingerTap => '네 손가락 탭';

  @override
  String get switchInputModePointerTouch => '입력 방식 전환 · 포인터 ↔ 터치';

  @override
  String get ok => '확인';

  @override
  String inputMode2(Object modeLabel) {
    return '입력 방식 · $modeLabel';
  }

  @override
  String get zoomScreenItselfSwitchInput => '화면 자체를 확대하려면 입력 방식을 포인터로 바꾸세요.';

  @override
  String get tapClicksLittleSlowWhile => '탭 클릭은 두 번 탭인지 확인하느라 조금 늦어요.';

  @override
  String longPressInputModeButton(Object windowsTouch) {
    return '입력 방식 버튼을 길게 누르면 입력 방식과 포인터 속도를 바꿔요.\n$windowsTouch\n마우스를 연결하면 바로 마우스로 조작해요.';
  }

  @override
  String get featuresConnection => '이 연결에서 쓸 수 있는 기능';

  @override
  String get available => '사용 가능';

  @override
  String get checkingConnection => '연결 확인 중';

  @override
  String get notAvailableConnection => '이번 연결에서 사용 불가';

  @override
  String get usesMoreDataBattery => '데이터·배터리 더 씀';

  @override
  String get getsScreensExactPixelsSo =>
      '이 화면의 화소 그대로 받아서 글자가 또렷해요. 데이터와 배터리를 더 써요.';

  @override
  String get windowsScale200Limit => 'Windows 배율 200% 한도';

  @override
  String get desktopLimit => '데스크톱 한도';

  @override
  String get decoderSizeLimitDevice => '이 기기 디코더 크기 한도';

  @override
  String get decoderSpeedLimitDevice => '이 기기 디코더 속도 한도';

  @override
  String get memoryDevice => '이 기기 메모리';

  @override
  String get autoSizeAlreadyNativePixels => '자동 크기가 이미 기기 화소';

  @override
  String get couldntCheck => '확인하지 못함';

  @override
  String backAutoSize(num count, Object stats) {
    return ' · 자동 크기로 돌아감 $count회 ($stats)';
  }

  @override
  String off3(Object fell) {
    return '꺼짐$fell';
  }

  @override
  String nativeDevicePixels(Object size) {
    return '기기 화소 그대로$size';
  }

  @override
  String get lowered => '낮춤';

  @override
  String onLabel3(Object state, Object fell) {
    return '켜짐 · $state$fell';
  }

  @override
  String get checkHttpConsentAgain => 'HTTP 동의를 다시 확인하세요.';

  @override
  String get viewScaleResolution => '보기 · 배율 · 해상도';

  @override
  String get qualityInputHelp => '화질 · 입력 · 사용안내';

  @override
  String get textScreenSize => '글자와 화면 크기';

  @override
  String get frameRateBitrate => '프레임·전송률';

  @override
  String get useExternalMouse => '외부 마우스 사용';

  @override
  String get availableFeatures => '사용할 수 있는 기능';

  @override
  String get roundTripLatencyHostResponse => '왕복 지연 · Host 응답';

  @override
  String get notMeasuredYet => '측정 전';

  @override
  String get menu => '메뉴';

  @override
  String get closeMenu => '메뉴 닫기';

  @override
  String get openMenu => '메뉴 열기';

  @override
  String openMenu2(Object latencyText) {
    return '메뉴 열기 · $latencyText';
  }

  @override
  String get screenPanHandleDragMove => '화면 이동 손잡이 · 끌어서 보이는 부분을 옮겨요';

  @override
  String get lookLeft => '왼쪽 보기';

  @override
  String get lookUp => '위쪽 보기';

  @override
  String get lookDown => '아래쪽 보기';

  @override
  String get lookRight => '오른쪽 보기';

  @override
  String get back => '뒤로';

  @override
  String get touch => '터치';

  @override
  String get pointer => '포인터';

  @override
  String get precise => '정밀';

  @override
  String get standard => '표준';

  @override
  String get fast => '빠름';

  @override
  String get couldntOpenTvInputField => 'TV 입력창을 열지 못했어요. 다시 선택해 주세요.';

  @override
  String get updateAppUseTvInput => 'TV 입력을 사용하려면 앱을 업데이트해 주세요.';

  @override
  String get drag => '끌기';

  @override
  String get scrollUpDown => '스크롤 · 위/아래';

  @override
  String get draggingOkRelease => '끄는 중 · OK로 놓기';

  @override
  String get dragOkGrab => '끌기 · OK로 잡기';

  @override
  String get remoteScreenMenu => '원격 화면 메뉴';

  @override
  String get shortcuts => '단축키';

  @override
  String get backRemoteScreen => '원격 화면으로';

  @override
  String get installPermissionConfirmedTapDownload =>
      '설치 허용을 확인했어요. ‘받고 설치’를 누르세요.';

  @override
  String get couldntCheckInstallPermissionDownload =>
      '설치 권한이나 다운로드 위치를 확인하지 못했어요. 다시 시도해 주세요.';

  @override
  String get couldntOpenAndroidSettingsAllow =>
      'Android 설정을 열지 못했어요. 설정 → 앱 → Multitap에서 직접 허용해 주세요.';

  @override
  String get appDataSignSettingsKept =>
      '앱 데이터와 로그인 설정은 유지돼요.\n받은 파일을 확인한 뒤 Android 설치 화면을 열어요.\nAndroid가 설치 여부를 다시 물어봐요.';

  @override
  String get androidSettingsAllowInstallUnknown =>
      'Android 설정에서 Multitap의\n‘알 수 없는 앱 설치’를 허용해 주세요.\n허용한 뒤 이 화면으로 돌아오세요.\n설정에서 언제든 다시 끌 수 있어요.';

  @override
  String get checkingInstallPermissionDownloadLocation =>
      '설치 권한과 다운로드 위치를 확인하고 있어요.';

  @override
  String downloadingMb(Object received, Object total, Object total2) {
    return '받는 중 · $received / $total MB ($total2%)';
  }

  @override
  String get checkingDownloadedFilesSizeSha => '받은 파일의 크기와 SHA-256을 확인하고 있어요.';

  @override
  String get openingAndroidInstaller => 'Android 설치 화면을 여는 중이에요.';

  @override
  String get tapUpdateAndroidInstallerOpen =>
      'Android 설치 화면에서 ‘업데이트’를 누르세요.\n설치 후 앱을 다시 열면 새 버전으로 시작해요.';

  @override
  String get later => '나중에';

  @override
  String get downloadInstall => '받고 설치';

  @override
  String newVersion3(Object version) {
    return '새 버전 $version';
  }

  @override
  String currentMb(Object current, Object total) {
    return '현재 $current · $total MB';
  }

  @override
  String released(Object releasedAt) {
    return '$releasedAt 배포';
  }

  @override
  String get current3 => '현재';

  @override
  String get newVersion4 => '새 버전';

  @override
  String get installedVersion => '설치된 버전';

  @override
  String get newerThanServer => '서버보다 새 버전';

  @override
  String get upDate => '최신';

  @override
  String text4(num hostsLength) {
    return 'Host 업데이트 $hostsLength대 가능';
  }

  @override
  String get checkingServersUpdateList => '서버의 업데이트 목록을 확인하고 있어요.';

  @override
  String get serverHasNoAndroidInstaller =>
      '이 서버에는 이 기기용 Android 설치 파일이 아직 없어요.';

  @override
  String get whatsNewVersion => '이번 버전에서 달라진 점';

  @override
  String whatsNewServerVersion(Object version) {
    return '서버 버전 $version에서 달라진 점';
  }

  @override
  String get whatsNewCurrentVersion => '지금 버전에서 달라진 점';

  @override
  String get appUpdate => '앱 업데이트';

  @override
  String get couldntGetUpdateInformationCheck =>
      '업데이트 정보를 확인하지 못했어요. 네트워크를 확인하고 다시 시도하세요.';

  @override
  String get serversUpdateInformationIsntValid =>
      '서버의 업데이트 정보가 올바르지 않아요. 관리자에게 알려 주세요.';

  @override
  String get couldntDownloadUpdateFileTry => '업데이트 파일을 받지 못했어요. 다시 시도하세요.';

  @override
  String get downloadedFilesSizeSha256 =>
      '받은 파일의 크기나 SHA-256이 서버 정보와 달라 설치하지 않았어요.';

  @override
  String get couldntSaveUpdateFileCheck =>
      '업데이트 파일을 저장하지 못했어요. 기기 저장 공간을 확인하세요.';

  @override
  String get cantCheckUpdatesBecauseApps =>
      '이 앱의 현재 버전을 확인하지 못해 업데이트를 확인할 수 없어요.';

  @override
  String get cantCheckUpdatesFitDevice => '이 기기에 맞는 업데이트를 확인할 수 없어요.';

  @override
  String get allowInstallUnknownAppsMultitap =>
      'Android 설정에서 Multitap의 ‘알 수 없는 앱 설치’를 허용해야 업데이트를 설치할 수 있어요.';

  @override
  String get downloadedFileIsntMultitapUpdate =>
      '받은 파일이 같은 서명의 Multitap 업데이트가 아니라 설치하지 않았어요.';

  @override
  String get downloadedFileIsntNewerThan =>
      '받은 파일이 지금 버전보다 새 버전이 아니라 설치하지 않았어요.';

  @override
  String get anotherUpdateBeingInstalledTry =>
      '다른 업데이트 설치가 진행 중이에요. 끝난 뒤 다시 시도하세요.';

  @override
  String get couldntCheckInstallStatusRestart =>
      '설치 상태를 확인하지 못했어요. Multitap을 다시 시작한 뒤 업데이트를 확인하세요.';

  @override
  String get updateCanceled => '업데이트를 취소했어요.';

  @override
  String get updateInstallCanceledYouKeep => '업데이트 설치를 취소했어요. 지금 버전을 계속 써요.';

  @override
  String get androidBlockedUpdateInstallCheck =>
      'Android가 업데이트 설치를 막았어요. 기기 관리 정책을 확인하세요.';

  @override
  String get couldntUpdateBecauseDoesntMatch => '설치된 앱과 맞지 않아 업데이트하지 못했어요.';

  @override
  String get updateIsntCompatibleDevice => '이 기기와 호환되지 않는 업데이트예요.';

  @override
  String get notEnoughStorageInstallUpdate => '저장 공간이 부족해 업데이트를 설치하지 못했어요.';

  @override
  String get androidFoundUpdateFileInvalid =>
      'Android가 업데이트 파일을 올바르지 않다고 판단했어요.';

  @override
  String get updateInstallTimedOutTry => '업데이트 설치 시간이 초과됐어요. 다시 시도하세요.';

  @override
  String get couldntOpenAndroidInstallerTry =>
      'Android 설치 화면을 열지 못했어요. 다시 시도하세요.';

  @override
  String notice(Object bannerTitle) {
    return '공지: $bannerTitle';
  }

  @override
  String get couldntOpenConsoleWindow => 'Console 창을 열지 못했어요.';

  @override
  String get couldntGetResponseFromServer => '서버 응답을 확인하지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get youWereSignedOutMultitap => 'Multitap에서 로그아웃됐어요. 클릭해서 이유를 확인하세요.';

  @override
  String get couldntSignOutCheckNetwork => '로그아웃하지 못했어요. 네트워크를 확인하고 다시 시도하세요.';

  @override
  String get couldntCloseRemoteWindowClose =>
      '원격 창을 닫지 못했어요. 원격 창을 닫은 뒤 다시 로그아웃하세요.';

  @override
  String get passwordWasChangedSignAgain => '비밀번호가 변경됐어요. 새 비밀번호로 다시 로그인해 주세요.';

  @override
  String get demo => '데모';

  @override
  String get notSet => '미설정';

  @override
  String get unnamedComponent => '이름이 없는 구성 요소';

  @override
  String get packageLicense => '패키지 라이선스';

  @override
  String get peaks => '봉우리';

  @override
  String get meadow => '초원';

  @override
  String get desktopConnectedFromAnotherWindow =>
      '이 데스크톱은 다른 창이나 기기에서 연결 중이에요. 아래 버튼을 누르면 그쪽 연결을 끊고 여기서 이어서 써요. 데스크톱과 실행 중인 작업은 그대로예요.';

  @override
  String get connectionSetupCanceledYouCan => '연결 준비를 취소했어요. 다시 연결할 수 있어요.';

  @override
  String get youNeedAgreeUnencryptedHttp =>
      '암호화 없는 HTTP 연결에 먼저 동의해야 해요. 서버 설정에서 ‘연결 테스트’를 다시 누르세요.';

  @override
  String get serverDoesntAllowHttpConnections =>
      '이 서버에서는 HTTP 연결을 쓸 수 없어요. HTTPS 주소를 쓰거나 관리자에게 알려 주세요.';

  @override
  String get httpConnectionsTurnedOffServer =>
      '서버에서 HTTP 연결이 꺼져 있어요. HTTPS 주소를 쓰거나 관리자에게 요청하세요.';

  @override
  String get addressIsntRegisteredServerCheck =>
      '이 주소는 서버에 등록된 주소가 아니에요. 받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.';

  @override
  String get addressOnlyServersFirstSetup =>
      '이 주소는 서버 초기 설정에만 쓸 수 있어요. 관리자가 서버 접속 설정을 마친 뒤에 연결할 수 있어요.';

  @override
  String get applyingServerAccessSettingsConnect =>
      '서버 접속 설정을 적용하고 있어요.\n잠시 후 다시 연결해 주세요.';

  @override
  String get addressDoesntMatchServersSettings =>
      '이 주소가 서버의 설정과 맞지 않아요. 받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.';

  @override
  String get changeInitialUsernamePasswordAdmin =>
      '관리자 웹에서 초기 아이디와 비밀번호를 먼저 변경해 주세요.';

  @override
  String get checkUsernamePassword => '아이디 또는 비밀번호를 확인해 주세요.';

  @override
  String get tooManyAttemptsTryAgain => '시도가 너무 많아요. 1분 후 다시 시도하세요.';

  @override
  String get serverBusyTryAgainMoment => '서버가 처리 중이에요. 잠시 후 다시 시도하세요.';

  @override
  String get checkVerificationCodeEnterAgain => '인증 코드를 확인하고 다시 입력하세요.';

  @override
  String get verificationTimeUpVerifyAgain =>
      '본인 확인 시간이 끝났어요. 인증 앱의 다음 코드로 다시 확인하세요.';

  @override
  String get verificationTimeUpStartAgain => '인증 시간이 끝났어요. 로그인부터 다시 시작하세요.';

  @override
  String get updateMultitapLatestVersionUse =>
      '2단계 인증을 쓰려면 Multitap을 최신 버전으로 업데이트하세요.';

  @override
  String get twoStepVerificationWorksHttps =>
      '2단계 인증은 HTTPS 서버 주소에서 사용할 수 있어요.';

  @override
  String get serverCouldntCheckTwoStep =>
      '서버에서 2단계 인증을 확인하지 못했어요. 관리자에게 알려 주세요.';

  @override
  String get enterEitherVerificationCodeRecovery => '인증 코드 또는 복구 코드 하나만 입력하세요.';

  @override
  String get couldntOpenBrowserCheckDefault =>
      '브라우저를 열지 못했어요.\n기본 브라우저 설정을 확인하고 다시 시도하세요.';

  @override
  String get couldntOpenConsoleCheckNetwork2 =>
      'Console을 열지 못했어요.\n네트워크를 확인하고 다시 시도하세요.';

  @override
  String get consoleNeedsMicrosoftEdgeWebview2 =>
      'Console을 열려면 Microsoft Edge WebView2가 필요해요.\nWebView2 Runtime을 설치한 뒤 다시 시도하세요.';

  @override
  String get consoleCantOpenedServerTell =>
      '이 서버에서는 Console을 열 수 없어요.\n관리자에게 알려 주세요.';

  @override
  String get anotherDesktopsConsoleOpenClose =>
      '다른 데스크톱의 Console이 열려 있어요.\n그 Console을 닫고 다시 시도하세요.';

  @override
  String get consoleOpensOnlyHttpsServer =>
      'Console은 HTTPS 서버 주소에서만 열 수 있어요.\nHTTPS 주소로 다시 로그인하세요.';

  @override
  String get tooManySignInsUse =>
      '동시에 사용 중인 로그인이 많아요. 사용하지 않는 기기에서 로그아웃한 뒤 다시 로그인하세요.';

  @override
  String get serverNeedsUpdateBeforeYou =>
      '이 앱으로 로그인하려면 서버를 업데이트해야 해요. 관리자에게 알려 주세요.';

  @override
  String get timeRanOutWhileChecking =>
      '확인하는 사이에 시간이 지났거나 기존 로그인이 바뀌었어요. 다시 로그인해 주세요.';

  @override
  String get connectionUseHasChangedConnect =>
      '사용 중인 연결이 바뀌었어요. 다시 연결해 현재 연결을 확인해 주세요.';

  @override
  String get timeConfirmConnectionSwitchUp =>
      '연결 전환 확인 시간이 끝났어요. 다시 연결해 현재 연결을 확인해 주세요.';

  @override
  String get couldntCheckConnectionSwitchDetails =>
      '연결 전환 정보를 확인하지 못했어요. 다시 연결해 주세요.';

  @override
  String get couldntDisconnectOtherConnectionConnect =>
      '다른 곳의 연결을 끊고 연결하지 못했어요. 다시 연결해 보고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get couldntCheckConnectionUseConnect =>
      '사용 중인 연결 정보를 확인하지 못했어요. 잠시 후 다시 연결해 주세요.';

  @override
  String get couldntConnectServerCheckNetwork =>
      '서버에 연결하지 못했어요. 네트워크를 확인해 주세요.';

  @override
  String get serverIsntRespondingTryAgain => '서버가 응답하지 않아요. 잠시 후 다시 시도하세요.';

  @override
  String get checkCurrentPassword => '현재 비밀번호를 확인해 주세요.';

  @override
  String get newPasswordConfirmationDontMatch => '새 비밀번호와 확인 입력이 일치하지 않아요.';

  @override
  String get passwordsMust10256Characters => '비밀번호는 10자 이상 256자 이하로 입력해 주세요.';

  @override
  String get lostConnectionHostTryAgain => 'Host 연결이 끊겼어요. 잠시 후 다시 시도하세요.';

  @override
  String get desktopHasAnotherConnection => '이 데스크톱에 다른 연결이 있어요.';

  @override
  String get couldntProcessRequestTryAgain2 =>
      '요청을 처리하지 못했어요. 잠시 후 다시 시도해 주세요.';

  @override
  String get couldntConnectSecurelyOverHttps =>
      'HTTPS로 안전하게 연결하지 못했어요. 서버 주소를 확인하고, 맞다면 관리자에게 알려 주세요.';

  @override
  String get couldntFindServerAddressCheck =>
      '서버 주소를 찾지 못했어요. 주소와 네트워크를 확인하세요.';

  @override
  String get serverDidntRespondTimeCheck =>
      '서버가 시간 안에 응답하지 않았어요. 주소와 포트를 확인하고 다시 시도하세요.';

  @override
  String get cantReachServerAddressPort =>
      '이 주소와 포트로는 서버에 연결되지 않아요. 받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.';

  @override
  String get desktopIsntRespondingConnectAgain =>
      '데스크톱이 응답하지 않아요. 잠시 후 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get hostStillGettingReadyConnect => 'Host가 아직 준비 중이에요. 잠시 후 다시 연결하세요.';

  @override
  String get cantPrepareScreenFitsWindow =>
      '지금은 이 크기의 화면을 준비할 수 없어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get hostsStateChangedConnectAgain => 'Host 상태가 바뀌었어요. 다시 연결하세요.';

  @override
  String get pictureQualitySettingsDontMatch =>
      '화면 품질 설정이 서버와 맞지 않아요. Multitap을 업데이트해도 계속되면 관리자에게 알려 주세요.';

  @override
  String get cantOpenVirtualDisplayScreen =>
      '이 화면 크기로 가상 화면을 열 수 없어요. 화면 모드나 크기를 바꿔 다시 연결하세요.';

  @override
  String get hostPreparingVirtualDisplayConnect =>
      'Host가 가상 화면을 준비하고 있어요. 잠시 후 다시 연결하세요.';

  @override
  String get onlyAdministratorAccountsCanUpdate => '관리자 계정만 Host를 업데이트할 수 있어요.';

  @override
  String get hostDoesntSupportRemoteUpdates =>
      '이 Host는 원격 업데이트를 지원하지 않아요. 데스크톱에서 Host 설치 파일을 직접 실행하세요.';

  @override
  String get hostUpdateProgressDesktopTry =>
      '이 데스크톱에서 Host 업데이트가 진행 중이에요. 끝난 뒤 다시 시도하세요.';

  @override
  String get hostAlreadyUpDate => '이미 최신 Host 버전이에요.';

  @override
  String get isntNewerThanCurrentHost => '현재 Host보다 새 버전이 아니에요.';

  @override
  String get serversLatestHostInstallerHas =>
      '서버의 최신 Host 설치 파일이 바뀌었어요. 목록을 새로고침하세요.';

  @override
  String get couldntCheckHostsCurrentVersion =>
      'Host의 현재 버전을 확인하지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get tooManyHostUpdateRequests => 'Host 업데이트 요청이 많아요. 잠시 후 다시 시도하세요.';

  @override
  String get cleaningUpPreviousConnectionConnect =>
      '이전 연결을 정리하고 있어요. 잠시 후 다시 연결하세요.';

  @override
  String get virtualDisplayIsntReadyIf =>
      '가상 화면이 준비되지 않았어요. 다시 연결해도 같으면 관리자에게 알려 주세요.';

  @override
  String get windowsSignedOutLockedOpen =>
      'Windows가 로그아웃됐거나 잠겨 있어요.\nConsole로 열어 로그인하세요.';

  @override
  String get hostIsntReadyStreamYet =>
      'Host가 아직 스트리밍을 준비하지 못했어요. 잠시 후 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get desktopHasAnotherConnectionDisconnect =>
      '이 데스크톱에 다른 연결이 있어요. 그 연결을 끊은 뒤 다시 연결하세요.';

  @override
  String get anotherUserUsingDesktopConnect =>
      '다른 사용자가 이 데스크톱을 쓰고 있어요. 그 연결이 끝난 뒤 다시 연결하세요.';

  @override
  String get tooManyConnectionSwitchRequests =>
      '연결 전환 요청이 많아요. 잠시 후 다시 시도해 주세요.';

  @override
  String get powerActionProgressTryAgain => '전원 작업이 진행 중이에요. 끝난 뒤 다시 시도하세요.';

  @override
  String get cantDoneCurrentPowerState =>
      '지금 전원 상태에서는 할 수 없는 작업이에요. 목록을 새로고침해 상태를 확인하세요.';

  @override
  String get couldntCheckVmsPowerState =>
      'Proxmox에서 VM 전원 상태를 확인하지 못했어요. 잠시 후 새로고침해 주세요.';

  @override
  String get couldntFinishPowerActionRefresh =>
      '전원 작업을 끝내지 못했어요. 목록을 새로고침해 상태를 확인하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get powerActionsResultIsntConfirmed =>
      '전원 작업 결과를 아직 확인하지 못했어요. 다시 요청하지 말고 목록을 새로고침하세요.';

  @override
  String get proxmoxConnectionSettingsChangedRefresh =>
      'Proxmox 연결 설정이 변경됐어요. 상태를 새로고침한 뒤 다시 시도해 주세요.';

  @override
  String get youDontHavePermissionDesktop =>
      '이 데스크톱을 쓸 권한이 없거나 배정이 바뀌었어요. 목록을 새로고침하고, 필요하면 관리자에게 요청하세요.';

  @override
  String get cantCheckPowerActionsResult =>
      '전원 작업 결과를 더 확인할 수 없어요. 목록을 새로고침해 상태를 확인하세요.';

  @override
  String get powerActionIsntSupported => '지원하지 않는 전원 작업이에요.';

  @override
  String get anotherPowerActionEndedStatus => '다른 전원 작업으로 이전 요청의 상태 확인을 마쳤어요.';

  @override
  String get hostDoesntSupportChangingNumber =>
      '이 Host는 모니터 수 변경을 지원하지 않거나 아직 준비되지 않았어요.';

  @override
  String get changingNumberMonitorsTryAgain => '모니터 수를 바꾸고 있어요. 끝난 뒤 다시 시도하세요.';

  @override
  String get couldntChangeNumberMonitorsCheck =>
      '모니터 수를 바꾸지 못했어요. 데스크톱 목록에서 지금 상태를 확인하세요.';

  @override
  String get itsNotConfirmedYetWhether =>
      '모니터 수가 바뀌었는지 아직 확인하지 못했어요. 다시 요청하지 말고 데스크톱 목록에서 확인하세요.';

  @override
  String get cantCheckMonitorChangeResult =>
      '모니터 수 변경 결과를 더 확인할 수 없어요. 데스크톱 목록에서 지금 상태를 확인하세요.';

  @override
  String get youCanSet12 => '모니터는 1대 또는 2대로 설정할 수 있어요.';

  @override
  String get allMonitorsCantUsedDesktop =>
      '이 데스크톱에서는 지금 모든 모니터를 쓸 수 없어요. 창 모드나 전체 화면을 쓰세요.';

  @override
  String get couldntCheckLayoutPcs2 =>
      'PC 모니터 2대의 배치를 확인하지 못했어요. 모니터 연결을 확인한 뒤 다시 선택하세요.';

  @override
  String get pcsCurrentMonitorSetupCant =>
      '지금 PC의 모니터 구성으로는 모든 모니터를 쓸 수 없어요. 모니터 2대, 합계 830만 화소 이하일 때만 쓸 수 있어요.';

  @override
  String get tooManyMonitorChangeRequests => '모니터 수 변경 요청이 많아요. 잠시 후 다시 시도하세요.';

  @override
  String get localPcsHostDoesntSupport =>
      '로컬 PC의 Host가 원격 전원을 지원하지 않아요. 관리자에게 Host 업데이트를 요청하세요.';

  @override
  String get couldntStartLocalPc => '로컬 PC를 켜지 못했어요.';

  @override
  String get localPcLockedSoWindows => '로컬 PC가 잠겨 있어 Windows가 요청을 받지 않았어요.';

  @override
  String get anotherUserSignedLocalPc =>
      '다른 사용자가 로컬 PC에 로그인해 있어 Windows가 요청을 받지 않았어요.';

  @override
  String get localPcAlreadyShuttingDown => '로컬 PC가 이미 꺼지거나 다시 시작하는 중이에요.';

  @override
  String get windowsDidntGivePermissionShut =>
      'Windows가 로컬 PC를 끄거나 다시 시작할 권한을 주지 않았어요. 관리자에게 알려 주세요.';

  @override
  String get windowsCouldntProcessRequestCheck =>
      'Windows가 요청을 처리하지 못했어요. 로컬 PC에서 직접 확인하세요.';

  @override
  String get localPcDidntRespondCheck =>
      '로컬 PC가 응답하지 않았어요. 잠시 후 목록에서 상태를 확인하세요.';

  @override
  String get windowsSignedOutLockedSign =>
      'Windows가 로그아웃됐거나 잠겨 있어요.\n로컬 PC에서 직접 로그인하세요.';

  @override
  String get serverIsntRespondingMomentRetrying =>
      '서버가 잠시 응답하지 않아요. 자동으로 다시 시도해요.';

  @override
  String get couldntFindServerAddressCheck2 =>
      '서버 주소를 찾지 못했어요. 주소의 철자와 인터넷 연결을 확인하세요.';

  @override
  String serverCouldntProcessRequestTry(Object status) {
    return '서버가 요청을 처리하지 못했어요. 잠시 후 다시 시도하고, 계속되면 관리자에게 알려 주세요. (HTTP $status)';
  }

  @override
  String get addressIsntMultitapServerCheck =>
      '이 주소는 Multitap 서버가 아니에요. 서버 주소를 확인하세요.';

  @override
  String get youNeedAgreeUnencryptedHttp2 =>
      '암호화 없는 HTTP 연결에 먼저 동의해야 해요. ‘연결 테스트’를 다시 누르세요.';

  @override
  String get serverDoesntAllowHttpConnections2 =>
      '이 서버에서는 HTTP 연결을 쓸 수 없어요.\nHTTPS 주소를 쓰거나 관리자에게 알려 주세요.';

  @override
  String get httpConnectionsTurnedOffServer2 =>
      '서버에서 HTTP 연결이 꺼져 있어요.\nHTTPS 주소를 쓰거나 관리자에게 요청하세요.';

  @override
  String get addressIsntRegisteredServerCheck2 =>
      '이 주소는 서버에 등록된 주소가 아니에요.\n받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.';

  @override
  String get httpAddressOnlyServersFirst =>
      '이 HTTP 주소는 서버 초기 설정에만 쓸 수 있어요.\n관리자가 서버 접속 설정을 마친 뒤에 연결할 수 있어요.';

  @override
  String get addressDoesntMatchServersSettings2 =>
      '이 주소가 서버의 설정과 맞지 않아요.\n받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.';

  @override
  String get cantReachServerAddressPort2 =>
      '이 주소와 포트로는 서버에 연결되지 않아요.\n받은 주소와 포트를 다시 확인하고, 맞다면 관리자에게 알려 주세요.';

  @override
  String get couldntConnectServerCheckAddress =>
      '서버에 연결하지 못했어요. 주소·포트와 네트워크를 확인하세요.';

  @override
  String get checkYouEnteredServerAddress =>
      '서버 주소를 입력했는지, 공백이나 특수 문자가 없는지 확인하세요.';

  @override
  String get enterHttpHttpsServerAddress => 'HTTP 또는 HTTPS 서버 주소를 입력하세요.';

  @override
  String get dontIncludePathServerAddress => '서버 주소에는 경로를 넣지 마세요.';

  @override
  String get checkIpv6Address => 'IPv6 주소를 확인하세요.';

  @override
  String get checkServerPort => '서버 포트를 확인하세요.';

  @override
  String get enterServerPort => '서버 포트를 입력하세요.';

  @override
  String get ipv6AddressesNeedSquareBrackets => 'IPv6 주소에는 대괄호가 필요해요.';

  @override
  String get enterIpAddressAsFour => 'IP 주소는 0~255 숫자 네 개를 점으로 이어 입력하세요.';

  @override
  String get checkServerAddress => '서버 주소를 확인하세요.';

  @override
  String get portNumberFrom165535 => '포트는 1~65535 숫자예요.';

  @override
  String get agreeHttpServerAddressFirst => '이 HTTP 서버 주소에 먼저 동의하고 연결을 확인하세요.';

  @override
  String get serverSettingsChangedConnectAgain => '서버 설정이 바뀌었어요. 다시 연결하세요.';

  @override
  String get httpConsentAddressDifferent => 'HTTP 동의 주소가 달라요.';

  @override
  String get checkServerSettings => '서버 설정을 확인하세요.';

  @override
  String get testConnectionCurrentHttpAddress => '현재 HTTP 주소의 연결을 먼저 확인하세요.';

  @override
  String get invalidRelayAddress => '잘못된 중계 주소예요.';

  @override
  String get checkServersAccessSettings => '서버의 접속 설정을 확인하세요.';

  @override
  String get waiting => '대기';

  @override
  String get download => '다운로드';

  @override
  String get install => '설치';

  @override
  String get done => '완료';

  @override
  String get needsCheck => '확인 필요';

  @override
  String get failed => '실패';

  @override
  String get stopped => '중지';

  @override
  String get updateAvailable => '업데이트 가능';

  @override
  String get notEligible => '대상 아님';

  @override
  String get assignmentWasRemovedSoResult => '할당이 해제돼 접수된 요청의 결과를 확인할 수 없어요.';

  @override
  String get desktopIsntAssignedYouNow => '현재 할당된 데스크톱이 아니에요.';

  @override
  String get useRequestingDisconnects => '사용 중 · 요청하면 연결을 끊어요';

  @override
  String get notUseUpdatesRightAway => '사용 중 아님 · 요청하면 바로 업데이트해요';

  @override
  String get notRequestedYet => '아직 요청하지 않았어요';

  @override
  String get requestingFromServer => '서버에 요청하는 중…';

  @override
  String get notAcceptedYetRequestingAgain =>
      '아직 접수되지 않았어요. 잠시 후 자동으로 다시 요청해요.';

  @override
  String get acceptedByServerWaitingInstall => '서버 접수됨 · 설치 결과를 기다려요';

  @override
  String get updatesWhenOtherUsersConnection => '다른 사용자의 연결이 끝나면 업데이트해요';

  @override
  String get useUpdatesWhenConnectionEnds => '사용 중 · 연결이 끝나면 업데이트해요';

  @override
  String downloadingInstaller(Object op) {
    return '설치 파일 다운로드$op';
  }

  @override
  String get checkingInstaller => '설치 파일 확인 중';

  @override
  String get installingHost => 'Host 설치 중';

  @override
  String get hostServiceRestartedWaitingConnection =>
      'Host 서비스가 다시 시작돼 연결을 기다려요';

  @override
  String get serverConfirmedInstallFinished => '서버에서 설치 완료를 확인했어요';

  @override
  String get installedWindowsRestartNeeded => '설치 완료 · Windows 다시 시작 필요';

  @override
  String get installedOpenHostSettingsDesktop =>
      '설치 완료 · 데스크톱에서 Host 설정을 열어 확인하세요';

  @override
  String get runningNewVersionInstallResult => '새 버전으로 실행 중 · 설치 결과는 보고되지 않았어요';

  @override
  String get checkRequestInstallResultWont =>
      '요청·설치 결과 확인 필요 · 자동으로 다시 요청하지 않아요';

  @override
  String get stoppedRemainingRequestsWerentAccepted =>
      '아직 접수되지 않은 남은 요청을 중지했어요';

  @override
  String get serverCantCheckUpdatePermission => '서버에서 업데이트 권한·상태를 확인할 수 없어요';

  @override
  String get hostOffline => 'Host 오프라인';

  @override
  String get remoteUpdateNotSupportedInstall =>
      '원격 업데이트 미지원 · 데스크톱에서 직접 설치해야 해요';

  @override
  String get latestReleaseChangedCloseWindow => '최신 배포가 바뀌었어요. 창을 닫고 다시 확인하세요.';

  @override
  String get checkCurrentHostVersion => '현재 Host 버전 확인 필요';

  @override
  String get noNewVersionInstall => '설치할 새 버전이 없어요';

  @override
  String get anotherHostUpdateProgress => '다른 Host 업데이트가 진행 중이에요';

  @override
  String get serverDoesntAllowUpdatesRight => '현재 서버에서 업데이트를 허용하지 않아요';

  @override
  String get hostOfflineSoRequestWasnt => 'Host가 오프라인이라 요청을 접수하지 못했어요';

  @override
  String get anotherUpdateAlreadyProgress => '다른 업데이트가 이미 진행 중이에요';

  @override
  String get serverConfirmedItsAlreadyUp => '서버에서 이미 최신 버전임을 확인했어요';

  @override
  String get hostDoesntSupportRemoteUpdates2 => '이 Host는 원격 업데이트를 지원하지 않아요';

  @override
  String get noUpdatePermissionSignExpired => '업데이트 권한이 없거나 로그인이 만료됐어요';

  @override
  String get tooManyRequestsSoWasnt => '요청이 많아 접수되지 않았어요. 잠시 후 다시 시도하세요.';

  @override
  String get serverCouldntAcceptRequestClose =>
      '서버에서 요청을 접수하지 못했어요. 창을 닫고 다시 시도하세요.';

  @override
  String text5(Object stage, Object stage2) {
    return '$stage $stage2대';
  }

  @override
  String progress(Object running, Object value, Object value2) {
    return '진행 중 $running대 · $value · $value2';
  }

  @override
  String get allFinished => '모두 끝났어요';

  @override
  String get versionUnknown => '버전 확인 필요';

  @override
  String get youSignedSomewhereElseSo => '다른 곳에서 로그인해 여기서는 로그아웃됐어요.';

  @override
  String get youWereSignedOutAfter => '오랫동안 입력이 없어 로그아웃됐어요.';

  @override
  String get useAnotherDeviceSignAgain => '다른 기기에서 사용 중이에요. 다시 로그인해 주세요.';

  @override
  String get youWereSignedOutEnter => '로그아웃됐어요. 비밀번호를 입력해 다시 로그인하세요.';

  @override
  String get multitapKeepsRunningTrayClose =>
      'Multitap은 트레이에서 계속 실행 중이에요. 완전히 끝내려면 트레이 아이콘이나 계정 메뉴의 \'완전 종료\'를 누르세요.';

  @override
  String get signExpiredClickSignAgain => '로그인이 만료됐어요. 클릭해서 다시 로그인하세요.';

  @override
  String text6(num bytes) {
    return '$bytes바이트';
  }

  @override
  String get zipThemMoveThemShared => '압축하거나 ‘공유 폴더’로 옮기세요';

  @override
  String get zipThemFirst => '압축해서 옮기세요';

  @override
  String get moveLargeFilesSharedFolder => '큰 파일은 ‘공유 폴더’로 옮기세요';

  @override
  String get splitThemIntoSmallerFiles => '더 작은 파일로 나눠 옮기세요';

  @override
  String upPerFileOnce(Object maxFileBytes, Object maxTotalBytes) {
    return '파일 하나에 $maxFileBytes, 한 번에 $maxTotalBytes까지';
  }

  @override
  String textTooLongYouCan(Object maxTextBytes) {
    return '텍스트가 너무 길어요. 한 번에 $maxTextBytes까지 옮길 수 있어요.';
  }

  @override
  String tooManyFilesYouCan(Object maxFiles, Object pack) {
    return '파일이 너무 많아요. 한 번에 $maxFiles개까지 옮길 수 있어요. 여러 개는 $pack.';
  }

  @override
  String fileTooLargeClipboardCan(Object files, Object big) {
    return '파일이 너무 커요. 클립보드로는 $files 옮길 수 있어요. $big.';
  }

  @override
  String imageTooLargeYouCan(Object formats) {
    return '이미지가 너무 커요. 6,400만 화소, $formats까지 옮길 수 있어요. 이미지를 파일로 저장해서 옮기세요.';
  }

  @override
  String tooManyItemsFolderYou(Object maxEntries, Object pack) {
    return '폴더 안의 항목이 너무 많아요. 한 번에 $maxEntries개까지 옮길 수 있어요. 폴더를 $pack.';
  }

  @override
  String folderTooDeepYouCan(Object maxDepth, Object pack) {
    return '폴더가 너무 깊어요. 폴더 안에 $maxDepth단계까지 옮길 수 있어요. 폴더를 $pack.';
  }

  @override
  String longestPathCharacters(Object chars) {
    return '가장 긴 경로 $chars자, ';
  }

  @override
  String someFilesHavePathsToo(Object chars, Object limit, Object pack) {
    return '경로가 너무 긴 파일이 있어요 ($chars폴더 안의 경로는 $limit자까지). 폴더를 $pack.';
  }

  @override
  String get overClipboardTransferLimit => '클립보드로 옮길 수 있는 한도를 넘었어요.';

  @override
  String overClipboardLimitYouCan(
    Object files,
    Object maxFiles,
    Object maxTextBytes,
    Object big,
  ) {
    return '클립보드 한도를 넘었어요. $files, 파일 $maxFiles개까지, 텍스트는 $maxTextBytes까지 옮길 수 있어요. $big.';
  }

  @override
  String get clipboardConnected => '클립보드 연결됨';

  @override
  String get clipboardBeingTransferredPasteAgain =>
      '클립보드를 옮기는 중이에요. 끝난 뒤 다시 붙여넣으세요.';

  @override
  String get couldntPrepareContentPasteCopy => '붙여넣을 내용을 준비하지 못했어요. 다시 복사하세요.';

  @override
  String get preparingClipboardConnection => '클립보드 연결 준비';

  @override
  String get checkingPcsClipboard => 'PC의 클립보드 확인 중';

  @override
  String get requestingDesktopsClipboard => '데스크톱 클립보드 요청 중';

  @override
  String get preparingPaste => '붙여넣기를 준비하는 중';

  @override
  String get checkAutomaticClipboardSharingAgain => '클립보드 자동 공유를 다시 확인하세요.';

  @override
  String get checkingWasAppliedDesktop => '데스크톱 적용 확인 중';

  @override
  String get sendingClipboard => '클립보드 보내는 중';

  @override
  String get gettingClipboardFiles => '클립보드 파일 가져오는 중';

  @override
  String get couldntPrepareAutomaticClipboardSharing =>
      '클립보드 자동 공유를 준비하지 못했어요.';

  @override
  String get gettingClipboard => '클립보드 가져오는 중';

  @override
  String get filesArriveWhenYouPaste => '붙여넣으면 파일을 받아요';

  @override
  String get arrivesWhenYouPaste => '붙여넣으면 받아요';

  @override
  String get cancelingClipboardTransfer => '클립보드 전송 취소 중';

  @override
  String copiedFilesArriveWhenYou(num treeSkipped) {
    return '복사했어요. 붙여넣을 때 파일을 받아요. 링크나 쓸 수 없는 이름 $treeSkipped개는 빠져요.';
  }

  @override
  String get copiedFilesArriveWhenYou2 => '복사했어요. 붙여넣을 때 파일을 받아요.';

  @override
  String get copiedArrivesWhenYouPaste => '복사했어요. 붙여넣을 때 받아요.';

  @override
  String clipboardTransferDoneSkipped(num treeSkipped) {
    return '클립보드 전송 완료. 링크나 쓸 수 없는 이름 $treeSkipped개는 건너뛰었어요.';
  }

  @override
  String get clipboardTransferDone => '클립보드 전송 완료';

  @override
  String get originalFileChangedCopyAgain => '원본 파일이 바뀌었어요. 다시 복사한 뒤 붙여넣으세요.';

  @override
  String get whatYouCopiedNoLonger =>
      '복사한 내용이 더 이상 남아 있지 않아요. 다시 복사한 뒤 붙여넣으세요.';

  @override
  String get somethingNewWasCopiedSo => '새로 복사한 내용이 있어 덮어쓰지 않았어요. 다시 붙여넣으세요.';

  @override
  String get administratorDoesntAllowClipboardTransfer =>
      '관리자가 허용하지 않은 클립보드 전송이에요.';

  @override
  String get nothingCopiedNoTextImage => '복사한 텍스트, 이미지, 파일이 없어요.';

  @override
  String get nothingCopiedNoTextFile => '복사한 텍스트나 파일이 없어요.';

  @override
  String get contentCantMovedOnlyText =>
      '이 내용은 옮길 수 없어요. 텍스트, 이미지, 파일, 폴더만 옮길 수 있어요.';

  @override
  String onlyTextFilesCanMoved(Object sharedFolders) {
    return '지금은 텍스트와 파일만 옮길 수 있어요. 폴더는 $sharedFolders. 이미지와 폴더는 앱과 데스크톱의 Host가 최신이어야 옮길 수 있어요.';
  }

  @override
  String get tryAgainAfterOtherClipboard => '다른 클립보드 작업이 끝난 뒤 다시 시도하세요.';

  @override
  String get clipboardTransferCanceled => '클립보드 전송을 취소했어요.';

  @override
  String get couldntConfirmWasMovedTry =>
      '옮겨졌는지 확인하지 못했어요. 붙여넣어 보고, 안 되면 다시 복사하세요.';

  @override
  String get couldntReadWhatYouCopied => '복사한 내용을 읽지 못했어요. 다시 복사한 뒤 붙여넣으세요.';

  @override
  String get couldntPutReceivingClipboardCopy =>
      '받는 쪽 클립보드에 넣지 못했어요. 다시 복사한 뒤 붙여넣으세요.';

  @override
  String get cantReadCopiedFileCheck =>
      '복사한 파일을 읽을 수 없어요. 파일이 있는지, 이름이 올바른지 확인한 뒤 다시 복사하세요.';

  @override
  String get anotherProgramUsingFileSo =>
      '다른 프로그램이 파일을 사용 중이라 읽을 수 없어요. 그 프로그램에서 파일을 닫은 뒤 다시 복사하세요.';

  @override
  String get notEnoughDiskSpaceReceiving =>
      '받는 쪽 디스크 공간이 부족해요. 공간을 확보한 뒤 다시 시도하세요.';

  @override
  String get desktopDidntRespondTime => '데스크톱이 시간 안에 응답하지 않았어요.';

  @override
  String get clipboardCantUsedBecauseDesktop =>
      '데스크톱 화면이 잠겼거나 보안 확인 창(사용자 계정 컨트롤 등)이 떠 있어 클립보드를 쓸 수 없어요. 잠금을 풀거나 창을 닫은 뒤 다시 시도하세요.';

  @override
  String get clipboardIsntAvailableConnection => '이 연결에서는 클립보드를 쓸 수 없어요.';

  @override
  String get permissionsDesktopHaveChanged => '이 데스크톱의 권한이 바뀌었어요.';

  @override
  String get administratorChangedClipboardPermission => '관리자가 클립보드 권한을 바꿨어요.';

  @override
  String get youveReachedClipboardLimitConnection =>
      '이 연결에서 클립보드를 쓸 수 있는 횟수를 넘었어요. 데스크톱에 다시 연결하면 쓸 수 있어요.';

  @override
  String get receivedContentWasInvalidSo =>
      '받은 내용이 올바르지 않아 옮기지 않았어요. 다시 복사한 뒤 붙여넣으세요.';

  @override
  String get clipboardConnectionDroppedSoWasnt =>
      '클립보드 연결이 끊겨 옮기지 못했어요. 다시 복사한 뒤 붙여넣으세요.';

  @override
  String get couldntTransferWhatYouCopied =>
      '복사한 내용을 옮기지 못했어요. 다시 복사한 뒤 붙여넣으세요.';

  @override
  String get clipboardSharingStoppedReconnectDesktop =>
      '클립보드 공유가 멈췄어요. 데스크톱에 다시 연결하면 쓸 수 있어요.';

  @override
  String clipboardSharingStopped(Object status) {
    return '클립보드 공유가 멈췄어요. $status';
  }

  @override
  String get connectionRequest => '연결 요청';

  @override
  String get preparingVirtualDisplay => '가상 화면 준비';

  @override
  String get serverConnection => '서버 연결';

  @override
  String get waitingFirstFrame => '첫 화면 수신 대기';

  @override
  String get waitingHostUpdate => 'Host 업데이트 대기';

  @override
  String get waitingDesktopRestart => '데스크톱 다시 시작 대기';

  @override
  String get locked => '잠겨 있어요';

  @override
  String get signScreen => '로그인 화면';

  @override
  String get screenOff => '화면이 꺼져 있어요';

  @override
  String get savedLastScreen => '저장된 마지막 화면';

  @override
  String get lastScreen => '마지막 화면';

  @override
  String get justNow => '방금';

  @override
  String minAgo(Object ageInMinutes) {
    return '$ageInMinutes분 전';
  }

  @override
  String text7(num ageInHours) {
    return '$ageInHours시간 전';
  }

  @override
  String text8(num ageInDays) {
    return '$ageInDays일 전';
  }

  @override
  String get folder => '폴더';

  @override
  String get mediaFoundationDeviceNotVerified => 'Media Foundation · 장치 미확인';

  @override
  String get checkingInitialization => '초기화 확인 중';

  @override
  String outputNotVerified(Object label) {
    return '$label · 출력 확인 전';
  }

  @override
  String get buildWithoutGpuEncoder => 'GPU 인코더가 포함되지 않은 빌드';

  @override
  String get selectedGpuNotFound => '선택한 GPU를 찾지 못함';

  @override
  String get vendorNotSupportedCurrentGpu => '현재 GPU 경로에서 지원하지 않는 제조사';

  @override
  String get gpuDriverEncoderUnavailable => 'GPU 드라이버 인코더를 사용할 수 없음';

  @override
  String get gpuApiVersionNotSupported => 'GPU API 버전 미지원';

  @override
  String get requestedH264FormatNot => '요청한 H.264 형식 미지원';

  @override
  String get gpuInitializationFailed => 'GPU 초기화 실패';

  @override
  String get gpuMemoryAllocationFailed => 'GPU 메모리 할당 실패';

  @override
  String get gpuFrameSubmissionFailed => 'GPU 프레임 제출 실패';

  @override
  String get gpuOutputTimedOut => 'GPU 출력 응답 시간 초과';

  @override
  String get gpuOutputFormatCheckFailed => 'GPU 출력 형식 확인 실패';

  @override
  String get gpuDeviceDisconnected => 'GPU 장치 연결이 끊김';

  @override
  String get gpuSettingsChangeFailed => 'GPU 설정 변경 실패';

  @override
  String get x264SettingsRejected => 'x264 설정 거절';

  @override
  String get x264InitializationFailed => 'x264 초기화 실패';

  @override
  String get x264EncodingFailed => 'x264 인코딩 실패';

  @override
  String get x264OutputFormatCheckFailed => 'x264 출력 형식 확인 실패';

  @override
  String get x264OutputTooLarge => 'x264 출력 크기 초과';

  @override
  String get x264MemoryAllocationFailed => 'x264 메모리 할당 실패';

  @override
  String get x264ProcessingFailed => 'x264 처리 실패';

  @override
  String get ready => '준비됨';

  @override
  String get autoInstall => '자동 설치';

  @override
  String get setupNeeded => '설정 필요';

  @override
  String get notSupported => '미지원';

  @override
  String get unknown2 => '확인 불가';

  @override
  String get installWinfsp => 'WinFsp 설치';

  @override
  String get startPrintSpooler => '인쇄 스풀러 시작';

  @override
  String get switchMultitap => 'Multitap용으로 바꾸기';

  @override
  String get sharedFolder => '공유 폴더';

  @override
  String get installWinfsp2 => 'WinFsp를 설치하세요.';

  @override
  String get runHostInstallerAgainInstall =>
      'Host 설치 파일을 다시 실행해 WinFsp를 설치하세요.';

  @override
  String get refreshStatus => '상태를 새로고침하세요.';

  @override
  String get runHostInstallerAgain => 'Host 설치 파일을 다시 실행하세요.';

  @override
  String get saveWorkRestartWindowsWont =>
      '작업을 저장하고 Windows를 다시 시작하세요. 자동으로 다시 시작하지 않아요.';

  @override
  String get printer => '프린터';

  @override
  String get maySecuritySettingSoIsnt =>
      '보안 설정일 수 있어 자동으로 켜지 않아요. 필요하면 Windows 서비스에서 직접 켜세요.';

  @override
  String get refreshStatusMoment => '잠시 후 상태를 새로고침하세요.';

  @override
  String get startPrintSpoolerWindowsServices =>
      'Windows 서비스에서 Print Spooler를 시작하세요.';

  @override
  String get turnMicrosoftPrintPdfWindows =>
      'Windows 기능에서 ‘Microsoft Print to PDF’를 켜세요.';

  @override
  String get soundCard => '사운드카드';

  @override
  String get administratorNeedsAddAudioDevice =>
      '관리자가 Proxmox에서 오디오 장치를 추가해야 해요.';

  @override
  String get checkAgainFromStreamingProxmox =>
      '스트리밍이나 Proxmox 콘솔(로컬 PC는 본체 화면)에서 다시 확인하세요.';

  @override
  String get deviceFactWinfsp => 'WinFsp 사용';

  @override
  String get deviceFactNoWinfsp => 'WinFsp 없음, 4 GB 넘는 파일 불가';

  @override
  String get deviceFactNoWinfspWebClient => 'WinFsp 없음, WebClient 꺼짐';

  @override
  String get deviceFactUsbWindows => 'Windows 10(1809) 이상, Server 제외';

  @override
  String get deviceFactUsbFilesMissing => '드라이버 파일 없음';

  @override
  String get deviceFactUsbFirstUse => '처음 연결할 때 설치';

  @override
  String get deviceFactUsbRunning => '드라이버 실행 중';

  @override
  String get deviceFactUsbStopped => '드라이버 시작 안 됨';

  @override
  String get deviceFactUsbInstalled => '드라이버 설치됨';

  @override
  String get deviceFactUsbForeign => '다른 프로그램이 사용 중';

  @override
  String get deviceSwitchUsbMultitap => 'USB 연결 기능을 Multitap용으로 바꾸세요.';

  @override
  String get deviceFactPrinterReady => '연결하면 Multitap 프린터 추가';

  @override
  String get deviceFactSpoolerOff => '인쇄 스풀러 꺼짐';

  @override
  String get deviceStartSpooler => '인쇄 스풀러를 시작하세요.';

  @override
  String get deviceFactSpoolerDisabled => '인쇄 스풀러 ‘사용 안 함’';

  @override
  String get deviceFactSpoolerStarting => '인쇄 스풀러 시작 중';

  @override
  String get deviceFactPdfMissing => 'PDF 인쇄 기능 없음';

  @override
  String get deviceFactAudioPresent => '출력 장치 있음';

  @override
  String get deviceFactAudioAbsent => '소리 출력 장치 없음';

  @override
  String get deviceFactAudioRemote => 'RDP 로그인 중';

  @override
  String get checkDeviceStatus => '장치 상태 확인 필요';

  @override
  String ready2(Object l) {
    return '$l 준비됨';
  }

  @override
  String notSupported2(Object unsupported) {
    return '$unsupported 미지원';
  }

  @override
  String get couldntConfirmInstallResultRefresh =>
      '설치 결과를 확인하지 못했어요.\n다시 설치하기 전에 상태를 새로고침하세요.';

  @override
  String get restartWindowsApplySaveWork =>
      'Windows를 다시 시작해야 적용돼요. 작업을 저장한 뒤 다시 시작하세요. 자동으로 다시 시작하지 않아요.';

  @override
  String winfspInstalled(Object restartNotice) {
    return 'WinFsp를 설치했어요.\n$restartNotice';
  }

  @override
  String get winfspInstalledFromNextConnection =>
      'WinFsp를 설치했어요.\n다음 연결부터 공유 폴더에서 큰 파일도 바로 열 수 있어요.';

  @override
  String get winfspAlreadyInstalled => 'WinFsp가 이미 설치돼 있어요.';

  @override
  String get couldntInstallBecauseAnotherProgram =>
      '다른 프로그램을 설치하는 중이라 설치하지 못했어요.\n그 설치가 끝나면 다시 시도하세요.';

  @override
  String get windowsPolicyBlockedInstallCheck =>
      'Windows 정책이 설치를 막았어요.\nWindows의 설치 허용 정책을 확인하세요.';

  @override
  String get couldntVerifyInstallerFileDownload =>
      '설치 파일을 확인하지 못했어요.\nHost 설치 파일을 다시 받아 실행하세요.';

  @override
  String get couldntInstallRestartWindowsThen =>
      '설치하지 못했어요.\nWindows를 다시 시작한 뒤 다시 시도하세요.';

  @override
  String get couldntVerifyAfterInstallingRestart =>
      '설치한 뒤 확인하지 못했어요.\nWindows를 다시 시작한 뒤 상태를 확인하세요.';

  @override
  String get couldntConfirmInstallResultRefresh2 =>
      '설치 결과를 확인하지 못했어요.\n상태를 새로고침해 확인하세요.';

  @override
  String errorCode(Object code) {
    return ' (오류 코드: $code)';
  }

  @override
  String meanwhileSharedFoldersUseBasic(
    Object cause,
    Object restart,
    Object safe,
  ) {
    return '$cause$restart\n그동안 공유 폴더는 기본 방식으로 동작해요.$safe';
  }

  @override
  String get saveWorkThenRestartWindows =>
      '작업을 저장한 뒤 Windows를 다시 시작하세요. 자동으로 다시 시작하지 않아요.';

  @override
  String get usbConnectionFeatureSwitchedMultitap =>
      'USB 연결 기능을 Multitap용으로 바꿨어요.\n다음 연결부터 USB를 쓸 수 있어요.';

  @override
  String restartWindowsSwitchUsbConnection(Object restartNotice) {
    return 'USB 연결 기능을 바꾸려면 Windows를 다시 시작해야 해요.\n$restartNotice';
  }

  @override
  String get alreadySetUpMultitap => '이미 Multitap용이에요.';

  @override
  String get couldntRemoveOtherProgramsUsb =>
      '다른 프로그램의 USB 연결 기능을 지우지 못했어요.\n그 프로그램을 먼저 끄거나 제거한 뒤 다시 시도하세요.';

  @override
  String get multitapUsbDriverCantInstalled =>
      'Multitap USB 드라이버를 설치할 수 없어요.\nHost 설치 파일을 다시 실행하세요.';

  @override
  String get couldntInstallMultitapUsbDriver =>
      'Multitap USB 드라이버를 설치하지 못했어요.\n상태를 새로고침한 뒤 다시 시도하세요.';

  @override
  String get printSpoolerHasStartedYou =>
      '인쇄 스풀러를 시작했어요.\n다음 연결부터 프린터를 쓸 수 있어요.';

  @override
  String get printSpoolerSetDisabledIsnt =>
      '인쇄 스풀러가 ‘사용 안 함’으로 설정돼 있어요.\n자동으로 바꾸지 않아요.';

  @override
  String get couldntStartPrintSpoolerCheck =>
      '인쇄 스풀러를 시작하지 못했어요.\nWindows 서비스에서 Print Spooler 상태를 확인하세요.';

  @override
  String get couldntCheckPrintSpoolerStatus =>
      '인쇄 스풀러 상태를 확인하지 못했어요.\n상태를 새로고침하세요.';

  @override
  String get oldMultitapVirtualDisplayAdapter => '이전 Multitap 가상 화면 어댑터';

  @override
  String get leftoverAdapterMadeByMultitap => 'Multitap이 만든 남은 어댑터';

  @override
  String get sameDriverNoMultitapRecord => '같은 드라이버 · Multitap 기록 없음';

  @override
  String get couldntCheckVirtualDisplayAdapters => '가상 화면 어댑터를 확인하지 못했어요.';

  @override
  String virtualDisplayAdaptersUseNone(Object current) {
    return '가상 화면 어댑터: 사용 중 $current개 · 남은 장치 없음';
  }

  @override
  String virtualDisplayAdaptersUseLeft(Object current, Object leftover) {
    return '가상 화면 어댑터: 사용 중 $current개 · 남은 장치 $leftover개';
  }

  @override
  String get couldntConfirmCleanupResultRefresh =>
      '정리 결과를 확인하지 못했어요.\n상태를 새로고침하세요.';

  @override
  String get listChangedMeantimeRefreshStatus =>
      '목록이 그사이 바뀌었어요.\n상태를 새로고침한 뒤 다시 확인하세요.';

  @override
  String get couldntReadVirtualDisplayDevices =>
      '가상 화면 장치를 읽지 못했어요.\n상태를 새로고침하세요.';

  @override
  String text9(num removed) {
    return '$removed개를 지웠어요.';
  }

  @override
  String afterYouRestartWindowsWont(num pending) {
    return '$pending개는 Windows를 다시 시작하면 사라져요. 자동으로 다시 시작하지 않아요.';
  }

  @override
  String refreshStatusThenTryAgain(num failed) {
    return '$failed개는 지우지 못했어요. 상태를 새로고침한 뒤 다시 시도하세요.';
  }

  @override
  String get thereWereNoDevicesRemove => '지울 장치가 없었어요.';

  @override
  String get noInfo => '정보 없음';

  @override
  String get multitapHostDiagnostics => 'Multitap Host 진단';

  @override
  String hostVersion(Object state) {
    return 'Host 버전: $state';
  }

  @override
  String desktop2(Object state) {
    return '데스크톱: $state';
  }

  @override
  String server2(Object state) {
    return '서버: $state';
  }

  @override
  String get registered => '등록됨';

  @override
  String get notRegistered => '미등록';

  @override
  String serverRegistration(Object state) {
    return '서버 등록: $state';
  }

  @override
  String get installed => '설치됨';

  @override
  String get notInstalled => '미설치';

  @override
  String serviceInstall(Object service) {
    return '서비스 설치: $service';
  }

  @override
  String get running => '실행 중';

  @override
  String serviceState(Object service) {
    return '서비스 실행: $service';
  }

  @override
  String serviceQueryError(Object service) {
    return '서비스 조회 오류: $service';
  }

  @override
  String get serverResponded => '서버 응답 확인';

  @override
  String serverConnection2(Object verifiedOnline) {
    return '서버 연결: $verifiedOnline';
  }

  @override
  String serverConnectionStage(Object brokerStates) {
    return '서버 연결 단계: $brokerStates';
  }

  @override
  String s(Object value) {
    return '$value초';
  }

  @override
  String sinceLastServerResponse(Object validAge) {
    return '마지막 서버 응답 경과: $validAge';
  }

  @override
  String serverConnectionFailureReason(Object broker) {
    return '서버 연결 실패 사유: $broker';
  }

  @override
  String get yes => '있음';

  @override
  String windowsSign(Object state) {
    return 'Windows 로그인: $state';
  }

  @override
  String virtualDisplayDriver(Object display) {
    return '가상 화면 드라이버: $display';
  }

  @override
  String get started => '시작됨';

  @override
  String virtualDisplayDevice(Object display) {
    return '가상 화면 장치: $display';
  }

  @override
  String virtualDisplay(Object display) {
    return '가상 화면: $display';
  }

  @override
  String virtualMonitor(Object m, Object m2, Object m3) {
    return '가상 모니터 $m: $m2 × $m3';
  }

  @override
  String get needed => '필요';

  @override
  String get notNeeded => '필요 없음';

  @override
  String displayRecovery(Object display) {
    return '화면 복구: $display';
  }

  @override
  String get notRequested2 => '요청 없음';

  @override
  String windowsRestart(Object display) {
    return 'Windows 다시 시작: $display';
  }

  @override
  String get saved => '저장됨';

  @override
  String windowsAccount(Object account) {
    return 'Windows 계정: $account';
  }

  @override
  String get unknownVendor => '제조사 미확인';

  @override
  String get anotherSettingsChangeWasMade => '다른 설정 변경이 있었어요. 현재 선택을 새로 확인하세요.';

  @override
  String get selectedGpuChangedCheckList => '선택한 GPU가 바뀌었어요. 목록을 새로 확인하세요.';

  @override
  String get anotherEncoderSettingsTaskProgress =>
      '다른 인코더 설정 작업 중이에요. 상태를 다시 확인하세요.';

  @override
  String get couldntVerifyEncoderSettingsProtection =>
      '인코더 설정의 보호 상태를 확인하지 못했어요. Host 설치 파일을 다시 실행해 복구하세요.';

  @override
  String get couldntReadEncoderSettingsFile =>
      '인코더 설정 파일을 읽지 못했어요. 상태를 새로고침하세요.';

  @override
  String get couldntConfirmSaveResultCheck =>
      '저장 결과를 확인하지 못했어요. 다시 적용하기 전에 상태를 확인하세요.';

  @override
  String get saveResponseDoesntMatchCurrent =>
      '저장 응답과 현재 설정이 일치하지 않아요. 상태를 다시 확인하세요.';

  @override
  String get couldntReadGpuListCpu => 'GPU 목록을 읽지 못했어요. CPU 설정은 그대로 사용할 수 있어요.';

  @override
  String get couldntCheckEncoderSettingsUpdate =>
      '인코더 설정을 확인하지 못했어요. Host를 업데이트하거나 상태를 다시 확인하세요.';

  @override
  String get couldntReadStatus => '상태 읽기 실패';

  @override
  String get hostServiceMissing => 'Host 서비스 없음';

  @override
  String get hostServiceOff => 'Host 서비스 꺼짐';

  @override
  String get checkHostService => 'Host 서비스 확인 필요';

  @override
  String get installationNeedsFinishing => '설치 마무리 필요';

  @override
  String get checkVirtualDisplayDriver => '가상 화면 드라이버 확인 필요';

  @override
  String get restartWindows => 'Windows 다시 시작 필요';

  @override
  String get virtualDisplayNeedsRecovery => '가상 화면 복구 필요';

  @override
  String get serverConnectionNeeded => '서버 연결 필요';

  @override
  String get administratorApprovalNeeded => '관리자 승인 필요';

  @override
  String get notConnectedServer => '서버 연결 안 됨';

  @override
  String get windowsSignNeeded => 'Windows 로그인 필요';

  @override
  String get streamingDisplaySwitchNeeded => '스트리밍 화면 전환 필요';

  @override
  String get preparingVirtualDisplay2 => '가상 화면 준비 중';

  @override
  String get checkVirtualDisplay => '가상 화면 확인 필요';

  @override
  String get autoSignSetupNeeded => '자동 로그인 설정 필요';

  @override
  String get checkWindowsSign => 'Windows 로그인 확인 필요';

  @override
  String get checkDevices => '장치 확인 필요';

  @override
  String desktopRegisteredServer(Object id) {
    return '서버에 등록된 데스크톱 · $id';
  }

  @override
  String get desktopRegisteredServer2 => '서버에 등록된 데스크톱';

  @override
  String get checkedJustNow => '방금 확인';

  @override
  String checkedSAgo(Object ageSeconds) {
    return '$ageSeconds초 전 확인';
  }

  @override
  String get existingVirtualDisplaySettingsNeed =>
      '기존 가상 화면 설정을 확인해야 해요.\n진단 정보를 복사해 관리자에게 알려 주세요.';

  @override
  String get virtualDisplayDeviceDidntStart =>
      '가상 화면 장치가 시작되지 않았어요.\nWindows를 다시 시작한 뒤 상태를 확인하세요.';

  @override
  String get couldntVerifyVirtualDisplayDriver =>
      '가상 화면 드라이버 파일을 확인하지 못했어요.\nHost 설치 파일을 다시 받아 실행하세요.';

  @override
  String get finishInstallation => '설치 마무리';

  @override
  String get virtualDisplayDriverSharedFolder =>
      '가상 화면 드라이버와 공유 폴더 구성 요소(WinFsp)가 아직 없어요.\n설치해야 이 데스크톱에 연결할 수 있어요.';

  @override
  String get virtualDisplayDriverIsntInstalled =>
      '가상 화면 드라이버가 아직 없어요.\n설치해야 이 데스크톱에 연결할 수 있어요.';

  @override
  String get sharedFolderComponentWinfspIsnt =>
      '공유 폴더 구성 요소(WinFsp)가 아직 없어요.\n설치하면 큰 파일도 바로 열 수 있어요.';

  @override
  String get trustVirtualDisplayDriversSignature => '가상 화면 드라이버의 서명을 신뢰할까요?';

  @override
  String get thenDriverSharedFolderComponent =>
      '이어서 드라이버와 공유 폴더 구성 요소(WinFsp)를 설치해요.';

  @override
  String get thenDriverInstalled => '이어서 드라이버를 설치해요.';

  @override
  String virtualDisplayOnlyOriginalSunshine(Object winfsp) {
    return '가상 화면만 Sunshine이 서명한 원본 드라이버를 그대로 써요.\n서명 인증서 하나를 Windows 신뢰 저장소(Root·TrustedPublisher)에 넣어요.\n$winfsp';
  }

  @override
  String get trustInstall => '신뢰하고 설치';

  @override
  String get installVirtualDisplayDriver => '가상 화면 드라이버를 설치할까요?';

  @override
  String get sharedFolderComponentWinfspInstalled =>
      '\n공유 폴더 구성 요소(WinFsp)도 함께 설치해요.';

  @override
  String installsAlreadyTrustedOriginalSunshine(Object winfsp) {
    return '이미 신뢰한 Sunshine 서명 원본 드라이버를 설치해요.$winfsp';
  }

  @override
  String get couldntConfirmVirtualDisplayDriver =>
      '가상 화면 드라이버 설치 결과를 확인하지 못했어요.\n상태를 새로고침해 확인하세요.';

  @override
  String get virtualDisplayDriverAlreadyInstalled => '가상 화면 드라이버가 이미 설치돼 있어요.';

  @override
  String get virtualDisplayDriverInstalled => '가상 화면 드라이버를 설치했어요.';

  @override
  String get rdpSignYouCantCheck =>
      'RDP 로그인에서는 화면이 켜졌는지 확인할 수 없어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 확인하세요.';

  @override
  String get couldntRestartHostServiceRestart =>
      'Host 서비스를 다시 시작하지 못했어요. 진단 보고서에서 서비스를 다시 시작하세요.';

  @override
  String get notInstalledBecauseSignatureVerification =>
      '서명 확인이 끝나지 않아 설치하지 않았어요.\n설치 마무리를 다시 눌러 주세요.';

  @override
  String get couldntRegisterDriverSignatureWindows =>
      'Windows에 드라이버 서명을 등록하지 못했어요.\nWindows의 인증서 정책을 확인하세요.';

  @override
  String get existingVirtualDisplaySettingsNeed2 =>
      '기존 가상 화면 설정을 확인해야 해요.\n기존 장치는 바꾸지 않았어요. 진단 정보를 복사해 관리자에게 알려 주세요.';

  @override
  String get anotherInstallationProgressTryAgain =>
      '다른 설치가 진행 중이에요.\n잠시 후 다시 시도하세요.';

  @override
  String get couldntSaveInstallResultTry => '설치 결과를 저장하지 못했어요.\n다시 시도하세요.';

  @override
  String get windowsCouldntInstallVirtualDisplay =>
      'Windows가 가상 화면 드라이버를 설치하지 못했어요.\nWindows를 다시 시작한 뒤 다시 시도하세요.';

  @override
  String get restart => '다시 시작';

  @override
  String get winfspWasAlreadyInstalled => 'WinFsp는 이미 설치돼 있었어요.';

  @override
  String get nothingInstall => '설치할 것이 없었어요.';

  @override
  String get savedInfoHasChangedGone =>
      '저장 정보가 바뀌었거나 없어졌어요. 새 상태를 확인한 뒤 다시 선택하세요.';

  @override
  String get couldntConfirmDeletionDontRun =>
      '삭제 완료를 확인하지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.';

  @override
  String get updateHostClearSavedInfo => 'Host를 업데이트한 뒤 저장 정보를 지울 수 있어요.';

  @override
  String get couldntConfirmServiceRestartedDont =>
      '서비스를 다시 시작했는지 확인하지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.';

  @override
  String get connectionRequestChangedCheckCurrent =>
      '연결 요청이 바뀌었어요. 현재 요청을 확인한 뒤 다시 취소하세요.';

  @override
  String get checkServerAddressPort1 => '서버 주소와 1~65535 포트를 확인하세요.';

  @override
  String get agreeHttpServerAddressFirst2 => '이 HTTP 서버 주소에 먼저 동의해 주세요.';

  @override
  String get currentAddressDoesntMatchServers =>
      '현재 주소와 서버의 접속 설정이 맞지 않아요. 주소·포트와 서버 설정을 확인하세요.';

  @override
  String get cantConfirmHttpSupportServer =>
      '서버의 HTTP 지원을 확인할 수 없어요. 서버를 업데이트하거나 HTTPS 주소를 사용하세요.';

  @override
  String get httpConnectionsOffServerAsk =>
      '서버에서 HTTP 연결이 꺼져 있어요. 관리자에게 요청하세요.';

  @override
  String get addressIsntServersAllowedList =>
      '이 주소는 서버의 허용 주소에 등록되지 않았어요. 주소와 포트를 확인하세요.';

  @override
  String get addressOnlyFirstSetupSet =>
      '이 주소는 초기 설정에만 사용할 수 있어요. 관리자 웹에서 접속 주소를 설정하세요.';

  @override
  String get couldntConnectSecurelyOverHttps2 =>
      'HTTPS로 안전하게 연결하지 못했어요. 서버 주소와 인증서를 확인하세요.';

  @override
  String get couldntConnectAddressPortCheck =>
      '이 주소와 포트로 연결하지 못했어요. 서버 실행 상태와 포트를 확인하세요.';

  @override
  String get serverDidntRespondTimeCheck2 =>
      '서버 응답 시간이 초과됐어요. 주소·포트와 서버 상태를 확인하세요.';

  @override
  String get couldntReachServerBecauseNetwork =>
      '네트워크 문제로 서버에 연결하지 못했어요. 네트워크 연결을 확인하세요.';

  @override
  String get couldntVerifyMultitapServerResponse =>
      'Multitap 서버 응답을 확인하지 못했어요. 주소·포트와 서버 상태를 확인하세요.';

  @override
  String get couldntReadHostSettingsResponse =>
      'Host 설정 응답을 읽지 못했어요. Host 설치 파일을 다시 실행하세요.';

  @override
  String get couldntFinishSetupRefreshStatus =>
      '설정을 완료하지 못했어요. 상태를 새로고침해 확인하세요.';

  @override
  String get deletionIsntConfirmedYetDont =>
      '삭제 완료를 아직 확인하지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.';

  @override
  String get serviceRestartIsntConfirmedYet =>
      '서비스를 다시 시작했는지 아직 확인하지 못했어요. 다시 실행하지 말고 잠시 후 상태를 새로고침하세요.';

  @override
  String get requestCancellationIsntConfirmedYet =>
      '요청 취소 결과를 아직 확인하지 못했어요. 다시 취소하지 말고 상태를 새로고침하세요.';

  @override
  String get encoderSaveIsntConfirmedYet =>
      '인코더 저장 결과를 아직 확인하지 못했어요. 다시 저장하지 말고 상태를 확인하세요.';

  @override
  String get winfspInstallIsntConfirmedYet =>
      'WinFsp 설치 결과를 아직 확인하지 못했어요. 다시 설치하지 말고 잠시 후 상태를 새로고침하세요.';

  @override
  String get virtualDisplayDriverInstallIsnt =>
      '가상 화면 드라이버 설치 결과를 아직 확인하지 못했어요. 다시 설치하지 말고 잠시 후 상태를 새로고침하세요.';

  @override
  String get deviceChangeIsntConfirmedYet =>
      '장치 변경 결과를 아직 확인하지 못했어요. 다시 실행하지 말고 잠시 후 상태를 새로고침하세요.';

  @override
  String get passwordResultIsntConfirmedYet => '비밀번호 결과를 아직 확인하지 못했어요.';

  @override
  String get autoSignRequestResultIsnt =>
      '자동 로그인 요청 결과를 아직 확인하지 못했어요. 다시 적용하지 말고 결과를 확인하세요.';

  @override
  String get settingsResponseTimedOutRefresh =>
      '설정 응답 시간이 초과됐어요. 상태를 새로고침해 적용 결과를 확인한 뒤 다시 시도하세요.';

  @override
  String get couldntFindHostConfigurationFile =>
      'Host 구성 파일을 찾지 못했어요. Host 설치 파일을 다시 실행하세요.';

  @override
  String get setupTimeRanOutStart => '설정 시간이 끝났어요. 본인 확인부터 다시 시작하세요.';

  @override
  String get checkAgainMoment => '잠시 후 다시 확인하세요.';

  @override
  String get couldntFinishIdentityVerificationTry =>
      '본인 확인을 마치지 못했어요. 다시 시도하세요.';

  @override
  String get checkResultOtherSettingsChange => '다른 설정 변경 결과를 먼저 확인하세요.';

  @override
  String get settingChangedSignOutThen => '설정은 바뀌었어요. 로그아웃한 뒤 다시 로그인하세요.';

  @override
  String get couldntConfirmChange => '변경 결과를 확인하지 못했어요.';

  @override
  String get twoStepVerificationRequired => '2단계 인증이 필요해요.';

  @override
  String get documentTooLargeOver256 => '문서가 너무 커서(256 MB 초과) 받지 못했어요.';

  @override
  String get tooManyPrintJobsStill => '아직 처리하지 않은 인쇄가 많아 새 문서를 받지 않았어요.';

  @override
  String get couldntSaveDocumentTemporarilyDesktop =>
      '데스크톱에 문서를 임시로 저장하지 못해 받지 못했어요. 데스크톱의 디스크 공간을 확인하세요.';

  @override
  String get couldntReceiveDocumentPrintAgain =>
      '문서를 받지 못했어요. 데스크톱에서 다시 인쇄하세요.';

  @override
  String get microsoftPrintPdfMissingDesktop =>
      '데스크톱에 Microsoft Print to PDF가 없어 프린터를 만들지 못했어요. 관리자에게 알려 주세요.';

  @override
  String get printSpoolerServiceOffDesktop =>
      '데스크톱의 인쇄 스풀러 서비스가 꺼져 있어 프린터를 만들지 못했어요. 관리자에게 알려 주세요.';

  @override
  String get anotherPrinterSameNameExists =>
      '데스크톱에 같은 이름의 다른 프린터가 있어 만들지 않았어요. 데스크톱에서 그 프린터를 지운 뒤 다시 연결하세요.';

  @override
  String get couldntCreatePrinterDesktop => '데스크톱에 프린터를 만들지 못했어요.';

  @override
  String get administratorTurnedOffPrinterPermission => '관리자가 프린터 권한을 해제했어요.';

  @override
  String get cantFindSelectedPrinterCheck =>
      '선택한 프린터를 찾을 수 없어요. 프린터가 켜져 있는지, 목록에서 지워지지 않았는지 확인해 주세요.';

  @override
  String get printerOfflineQueuedDocumentsPrint =>
      '프린터가 오프라인이에요. 켜지면 대기열의 문서가 인쇄돼요.';

  @override
  String get printerPausedResumeWindowsPrint =>
      '프린터가 일시 중지돼 있어요. Windows에서 일시 중지를 풀어야 인쇄돼요.';

  @override
  String get printerNeedsAttentionPaperToner => '프린터에 용지·토너·덮개 등 확인할 것이 있어요.';

  @override
  String get printerHasProblemCheckPrinter => '프린터에 문제가 있어요. 프린터 상태를 확인해 주세요.';

  @override
  String get storageFullNotWritableSo => '저장 공간이 부족하거나 쓸 수 없어서 문서를 받지 못했어요.';

  @override
  String get printingCanceled => '인쇄를 취소했어요.';

  @override
  String get couldntOpenDocumentSoWasnt =>
      '문서를 열지 못해 인쇄하지 못했어요. PDF로 저장할 수 있어요.';

  @override
  String get printingIsntAvailablePc => 'PC에서 인쇄를 사용할 수 없어요.';

  @override
  String get couldntSaveFile => '파일을 저장하지 못했어요.';

  @override
  String get couldntPrint => '인쇄하지 못했어요.';

  @override
  String get am => '오전';

  @override
  String get pm => '오후';

  @override
  String get soon => '곧';

  @override
  String min(Object left) {
    return '$left분 뒤';
  }

  @override
  String get today => '오늘';

  @override
  String get tomorrow => '내일';

  @override
  String text10(Object month, Object day) {
    return '$month월 $day일';
  }

  @override
  String minAgo2(Object passedInMinutes) {
    return '$passedInMinutes분 전';
  }

  @override
  String text11(num passedInHours) {
    return '$passedInHours시간 전';
  }

  @override
  String get myDesktop => '나의 데스크톱';

  @override
  String get samplePcServerRelay => '예시 PC · 서버 중계';

  @override
  String get windows11PhysicalPc => 'Windows 11 · 물리 PC';

  @override
  String get proxmoxWorkspace => 'Proxmox 워크스페이스';

  @override
  String get lightweightVirtualDesktopStart => '가볍게 시작하는 가상 데스크톱';

  @override
  String get devDesktop => '개발용 데스크톱';

  @override
  String get spaceNewIdeas => '새로운 아이디어를 위한 공간';

  @override
  String get multitapPrint => 'Multitap 인쇄';

  @override
  String printAgainFromDesktop(num count) {
    return '인쇄하던 문서 $count개를 받지 못했어요. 데스크톱에서 다시 인쇄해 주세요.';
  }

  @override
  String get couldntChangeScreenSizeSo => '화면 크기를 바꾸지 못해 이전 크기를 유지해요.';

  @override
  String get disconnected => '연결 끊김';

  @override
  String get p2pDirect => 'P2P 직결';

  @override
  String get udpViaServer => 'UDP 서버 경유';

  @override
  String get tcpViaServer => 'TCP 서버 경유';

  @override
  String get connectingServer => '서버에 연결하는 중…';

  @override
  String get waiting2 => '수신 대기';

  @override
  String get desktopHasNoSoundDevice => '데스크톱에 소리 장치가 없어요';

  @override
  String get disconnectedReconnect => '연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get couldntPrepareKeyboardInputSo =>
      '키보드 입력을 준비하지 못해 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get preparingVirtualDisplayResolution => '가상 화면 해상도를 준비하는 중';

  @override
  String get waitingHostScreen => 'Host 화면 기다리는 중';

  @override
  String get virtualDisplayTookTooLong => '가상 화면 준비 시간이 초과됐어요. 다시 연결해 주세요.';

  @override
  String get couldntConfirmVirtualDisplayChange =>
      '가상 화면 변경을 확인하지 못했어요. 다시 연결해 주세요.';

  @override
  String get preparingRemoteScreen => '원격 화면 준비 중';

  @override
  String get resizingVirtualDisplay => '가상 화면 크기 변경 중';

  @override
  String get restoringPreviousScreenSize => '이전 화면 크기 복구 중';

  @override
  String get couldntResizeVirtualDisplayReconnect =>
      '가상 화면 크기를 변경하지 못했어요. 다시 연결해 주세요.';

  @override
  String get couldntPrepareVirtualDisplayResolution =>
      '가상 화면 해상도를 준비하지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get virtualDisplayOpenedDifferentResolution =>
      '가상 화면이 다른 해상도로 열렸어요. 다시 연결하세요.';

  @override
  String get preparingScreenNewResolution => '새 해상도의 화면을 준비하는 중';

  @override
  String get preparingFirstFrame => '첫 화면을 준비하는 중';

  @override
  String get changingRemoteScreenResolution => '원격 화면 해상도를 변경하는 중';

  @override
  String get couldntResizeRemoteScreenSo =>
      '원격 화면 크기를 바꾸지 못해 연결이 끊겼어요. 다시 연결해 주세요.';

  @override
  String get hostStoppedRespondingSoConnection =>
      'Host가 응답하지 않아 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get didntReceiveFirstFrameReconnect =>
      '첫 화면을 받지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get screenStoppedArrivingSoConnection =>
      '화면을 더 받지 못해 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get oneMonitorsScreenStoppedArriving =>
      '모니터 한 대의 화면을 더 받지 못해 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get couldntShowScreenSoConnection => '화면을 표시하지 못해 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get couldntApplyRequestedSizeSo => '요청한 크기를 적용하지 못해 이전 크기로 복구했어요.';

  @override
  String get oneMonitorsDisplayFrozeSo =>
      '모니터 한 대의 화면 표시가 멈춰 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get displayFrozeSoConnectionEnded => '화면 표시가 멈춰 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get soundOutputDeviceIsntAvailable => '소리 출력 장치를 쓸 수 없어요';

  @override
  String get playing => '재생 중';

  @override
  String get couldntCheckDisplayStateSo =>
      '화면 표시 상태를 확인하지 못해 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get couldntPrepareVideoNewResolution =>
      '새 해상도의 영상을 준비하지 못했어요. 다시 연결해 주세요.';

  @override
  String get remoteResolutionTookTooLong => '원격 해상도 적용 시간이 초과됐어요. 다시 연결해 주세요.';

  @override
  String get largerResolutionsNeedHostServer =>
      '더 큰 해상도는 Host와 서버를 업데이트해야 쓸 수 있어요. 관리자에게 알려 주세요.';

  @override
  String get connectionEnded2 => '연결 종료';

  @override
  String get networkConnectionDropped => '네트워크 연결이 끊겼어요.';

  @override
  String get couldntChangeAutoSignOut =>
      '자동 로그아웃 설정을 변경하지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get couldntConfirmAutoSignOut => '자동 로그아웃 설정을 확인하지 못했어요. 현재 설정은 유지돼요.';

  @override
  String get hostCantHandleScreenSize => 'Host가 이 화면 크기를 처리할 수 없어요.';

  @override
  String get couldntCheckHostsAvailableMemory => 'Host의 사용 가능한 메모리를 확인하지 못했어요.';

  @override
  String get hostDoesntHaveEnoughFree => 'Host의 사용 가능한 메모리가 화면 처리에 부족해요.';

  @override
  String get hostDoesntSupportScreenSize => 'Host가 이 화면 크기나 형식을 지원하지 않아요.';

  @override
  String get virtualDisplayDoesntSupportResolution =>
      '가상 화면이 이 해상도·주사율을 지원하지 않아요.';

  @override
  String keepingPreviousSize(Object message) {
    return '$message 이전 크기를 유지해요.';
  }

  @override
  String get reconnectMoment => ' 잠시 후 다시 연결해 주세요.';

  @override
  String get makeScreenSmallerReconnect => ' 화면 크기를 줄여 다시 연결하세요.';

  @override
  String get reconnectDifferentScreenSize => ' 다른 화면 크기로 다시 연결해 주세요.';

  @override
  String get console2 => 'Console에서';

  @override
  String get fromAnotherWindowDevice => '다른 창이나 기기에서';

  @override
  String sameAccount(Object where) {
    return '같은 계정으로 $where';
  }

  @override
  String account(Object account, Object where) {
    return '$account 계정이 $where';
  }

  @override
  String anotherAccount(Object where) {
    return '다른 계정이 $where';
  }

  @override
  String connectedDesktopSoConnectionEnded(Object who) {
    return '$who 이 데스크톱에 연결해 이 연결이 끊겼어요.\n다시 연결하면 그쪽 연결을 끊을 수 있어요.';
  }

  @override
  String get desktopWasConnectedFromAnother =>
      '다른 창이나 기기에서 이 데스크톱에 연결해 이 연결이 끊겼어요. 다시 연결하면 그쪽 연결을 끊을 수 있어요.';

  @override
  String get administratorChangedClipboardSharingPermission =>
      '관리자가 클립보드 공유 권한을 변경했어요. 다시 연결하면 새 권한이 적용돼요.';

  @override
  String get disconnectedHostUpdateReconnectWhen =>
      'Host 업데이트 때문에 연결이 끊겼어요. 업데이트가 끝나면 다시 연결하세요.';

  @override
  String get signExpiredDesktopsAssignmentChanged =>
      '로그인이 만료됐거나 이 데스크톱의 배정이 바뀌었어요. 데스크톱 목록에서 확인하세요.';

  @override
  String get desktopsHostWasUnregisteredLet =>
      '이 데스크톱의 Host 등록이 해제됐어요. 관리자에게 알려 주세요.';

  @override
  String get hostCouldntPrepareScreenReconnect =>
      'Host가 화면을 준비하지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get hostStoppedSendingScreenReconnect =>
      'Host가 화면을 보내다 멈췄어요. 다시 연결하세요.';

  @override
  String get hostConnectionDroppedReconnectMoment =>
      'Host 연결이 끊겼어요. 잠시 후 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get serverRestartingReconnectMoment =>
      '서버가 다시 시작되고 있어요. 잠시 후 다시 연결하세요.';

  @override
  String get screenCouldntProcessedTimeSo =>
      '화면을 제때 처리하지 못해 연결이 끊겼어요. 화질 설정을 낮추거나 다시 연결하세요.';

  @override
  String get virtualDisplaySetupChangedScreen =>
      '가상 화면 구성이 바뀌었거나 화면 크기를 바꾸지 못해 연결이 끊겼어요. 다시 연결하세요.';

  @override
  String get couldntConnectAllMonitorsFrom =>
      '모든 모니터로 연결하지 못했어요.\n목록에서 창 모드나 전체 화면으로 연결하세요.';

  @override
  String get couldntConnectServerCheckNetwork2 =>
      '서버에 연결하지 못했어요. 네트워크를 확인하고 다시 연결하세요.';

  @override
  String get couldntConnectDesktopReconnectIf =>
      '데스크톱에 연결하지 못했어요. 다시 연결하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get notEnoughMemoryPrepareScreen => '메모리가 부족해 이 화면 크기를 준비하지 못했어요.';

  @override
  String get twoScreensCantShownOnce => '여기서는 화면 두 개를 동시에 표시할 수 없어요.';

  @override
  String get screenSizeFrameRateCant => '여기서는 이 화면 크기와 프레임 속도를 표시할 수 없어요.';

  @override
  String get videoFormatCantShownHere => '여기서는 이 영상 형식을 표시할 수 없어요.';

  @override
  String get screenSizeCantShownHere => '여기서는 이 화면 크기를 표시할 수 없어요.';

  @override
  String get couldntCheckWhichScreenSizes =>
      '표시할 수 있는 화면 크기를 확인하지 못했어요. 이전 크기를 유지해요.';

  @override
  String get pcDecoderSizeLimit => 'PC 디코더 크기 한도';

  @override
  String get pcDecoderSpeedLimit => 'PC 디코더 속도 한도';

  @override
  String get pcDecodingLoad => 'PC 디코딩 부하';

  @override
  String get desktopEncodingLoad => '데스크톱 인코딩 부하';

  @override
  String get limitSetByAdministrator => '관리자 설정 한도';

  @override
  String get desktopConnectedSomewhereElseInformation =>
      '다른 곳에서 연결 중이에요. 그 연결을 끊는 데 필요한 정보를 받지 못했어요. 다시 연결해 보고, 계속되면 관리자에게 알려 주세요.';

  @override
  String connectedAccount(Object username) {
    return '연결 중인 계정 $username';
  }

  @override
  String connectedAccountSinceDate(Object username, Object date, Object time) {
    return '연결 중인 계정 $username · $date $time부터';
  }

  @override
  String get useAnotherWindow => '다른 창에서 쓰는 중이에요';

  @override
  String useWindow(Object name) {
    return '$name 창에서 쓰는 중이에요';
  }

  @override
  String get deviceAlsoNeedsPermissionIts =>
      '이 장치를 쓰려면 장치 종류에 맞는 권한(공유 폴더·프린터·스마트 카드·직렬(COM) 포트)도 필요해요. 관리자에게 요청하세요.';

  @override
  String get devicesSuchAsKeyboardsMice =>
      '키보드·마우스·허브·블루투스·오디오·카메라 같은 장치는 연결할 수 없어요.';

  @override
  String get keyboardsMiceArentConnectedDesktop => '키보드·마우스는 데스크톱에 연결하지 않아요.';

  @override
  String get couldntReadDeviceInformationSo => '장치 정보를 읽지 못해 연결할 수 없어요.';

  @override
  String youCanConnectUpDevices(Object usbMaxDevices) {
    return '장치는 한 번에 $usbMaxDevices개까지 연결할 수 있어요.';
  }

  @override
  String get desktopDidntReceiveDevice => '데스크톱이 장치를 받지 못했어요.';

  @override
  String get anotherUsbVirtualDeviceProgram =>
      '데스크톱에 다른 USB 가상 장치 프로그램이 있어요.\n데스크톱의 Host 설정 → 장치 → USB에서 해결할 수 있어요.';

  @override
  String get desktopsHostHasNoUsb =>
      '데스크톱의 Host에 USB 연결 드라이버가 없어요. 관리자에게 알려 주세요.';

  @override
  String get couldntInstallUsbConnectionDriver =>
      '데스크톱에 USB 연결 드라이버를 설치하지 못했어요. 관리자에게 알려 주세요.';

  @override
  String get restartDesktopUseUsbDevices => '데스크톱을 다시 시작해야 USB 장치를 쓸 수 있어요.';

  @override
  String get desktopsUsbConnectionDriverIsnt =>
      '데스크톱의 USB 연결 드라이버가 보호되지 않아 USB 장치를 연결하지 않아요. 관리자에게 알려 주세요.';

  @override
  String get administratorTurnedOffUsbPermission => '관리자가 USB 권한을 해제했어요.';

  @override
  String get deviceWasUnpluggedFromPc => '장치가 PC에서 분리됐어요.';

  @override
  String get deviceWasRemovedDesktop => '데스크톱에서 장치를 분리했어요.';

  @override
  String get anotherProgramUsingDevice => '다른 프로그램이 이 장치를 쓰고 있어요.';

  @override
  String get itsUsePcSoCant => 'PC에서 사용 중이라 연결할 수 없어요. 열린 파일이나 창을 닫고 다시 시도하세요.';

  @override
  String get useSomewhereElse => '다른 곳에서 쓰는 중이에요.';

  @override
  String get couldntTakeDeviceFromPc => 'PC에서 장치를 가져오지 못했어요.';

  @override
  String get anotherWindowProgramAlreadyUsing =>
      '다른 창이나 프로그램이 이 장치를 이미 쓰고 있어요. 그쪽에서 연결을 끊고 다시 시도하세요.';

  @override
  String get couldntGetDeviceBackFrom =>
      '다른 창에서 장치를 돌려받지 못했어요. 그 창에서 연결을 끊고 다시 시도하세요.';

  @override
  String get couldntCommunicateDeviceTryAgain => '장치와 통신하지 못했어요. 다시 시도하세요.';

  @override
  String get takingDeviceFromPcTook => 'PC에서 장치를 가져오는 데 너무 오래 걸렸어요. 다시 시도하세요.';

  @override
  String get openFileCheckDidntFinish => '열린 파일 확인이 끝나지 않았어요. 잠시 후 다시 시도하세요.';

  @override
  String get couldntCheckWhetherPcCan =>
      'PC에서 장치를 쓸 수 있는지 확인하지 못했어요. ‘PC로 되돌리기’를 눌러 주세요.';

  @override
  String get deviceDidntReturnPcSelect =>
      '장치가 PC로 돌아오지 않았어요. ‘PC로 되돌리기’를 눌러 주세요. 안 되면 USB를 다시 꽂아 주세요.';

  @override
  String get noMoreDevicesCanConnected =>
      '이 연결에서는 장치를 더 연결할 수 없어요. 데스크톱에 다시 연결하세요.';

  @override
  String get couldntFindUsbDriverIf =>
      'USB 드라이버를 찾지 못했어요. PC를 다시 시작해도 같으면 클라이언트를 다시 설치해 주세요.';

  @override
  String get couldntVerifyInstallersSignatureSo =>
      '설치 파일의 서명을 확인하지 못해 설치하지 않았어요.';

  @override
  String get couldntInstallUsbDriverTry => 'USB 드라이버를 설치하지 못했어요. 다시 시도하세요.';

  @override
  String get windowsAdministratorApprovalWasCanceled =>
      'Windows 관리자 승인을 취소해서 USB 드라이버를 설치하지 않았어요. 설치하려면 ‘USB 드라이버 설치…’를 다시 누르고 ‘예’를 누르세요.';

  @override
  String get restartPcUseUsbDevices => 'PC를 다시 시작하면 USB 장치를 쓸 수 있어요.';

  @override
  String get couldntConnectDevice => '장치를 연결하지 못했어요.';

  @override
  String get usbReturnedChannelEnded => '연결이 끊겨 장치가 PC로 돌아왔어요.';

  @override
  String get smartCard => '스마트 카드';

  @override
  String get serialComPort => '직렬(COM) 포트';

  @override
  String deviceAlsoNeedsPermissionAsk(Object value) {
    return '이 장치를 쓰려면 $value 권한도 필요해요. 관리자에게 요청하세요.';
  }

  @override
  String get usbDeviceReconnected => 'USB 장치를 다시 연결했어요';

  @override
  String couldntReconnectUsbDevice(Object failure) {
    return 'USB 장치를 다시 연결하지 못했어요. $failure';
  }

  @override
  String get usbConnectionLost => 'USB 연결이 끊겼어요';

  @override
  String get multitapHostSettings => 'Multitap Host 설정';

  @override
  String get couldntPrepareRemoteWindow => '원격 창을 준비하지 못했어요';

  @override
  String get multitapRemoteWindow => 'Multitap 원격 창';

  @override
  String get closeWindowConnectAgainFrom => '창을 닫고 데스크톱 목록에서 다시 연결하세요.';

  @override
  String get windowsLockedUnlockSameWindows =>
      'Windows가 잠겨 있어요. 같은 Windows 계정으로 잠금을 해제한 뒤 연결하세요.';

  @override
  String get theresNoWindowsSignSession =>
      '스트리밍할 Windows 로그인 세션이 없어요. Windows에 로그인하세요.';

  @override
  String get youCanBringWindowsScreen =>
      'RDP에서 쓰던 Windows 화면을 스트리밍으로 가져올 수 있어요.';

  @override
  String get checkHostSettingsWindowsSign =>
      'Host 설정 → Windows 로그인 → Windows 계정을 확인하세요.';

  @override
  String get anotherWindowsUserSeveralSign =>
      '다른 Windows 사용자나 여러 로그인이 있어 세션을 자동으로 선택할 수 없어요.';

  @override
  String get windowsSignStateChangedDuring =>
      'Windows 로그인 상태가 작업 중 변경됐어요. 목록을 새로고침하세요.';

  @override
  String get sessionSwitchResultIsntConfirmed =>
      '세션 전환 결과를 아직 확인하지 못했어요. 같은 작업을 반복하지 말고 상태를 새로고침하세요.';

  @override
  String get couldntReturnWindowsSessionStreaming =>
      'Windows 세션을 스트리밍 화면으로 복귀하지 못했어요. Host의 세션 상태를 확인하세요.';

  @override
  String get windowsSessionVirtualDisplayBeing =>
      'Windows 세션이나 가상 화면을 구성 중이에요. 완료 후 연결하세요.';

  @override
  String get windowsSessionBackButVirtual =>
      'Windows 세션은 복귀했지만 가상 화면 준비가 끝나지 않았어요. Host의 가상 화면 상태를 확인하세요.';

  @override
  String get returnWindowsSessionConfirmSwitch =>
      'Windows 세션을 복귀하려면 전환 안내에서 확인하세요.';

  @override
  String get hostCouldntConfirmScreenWas => 'Host가 이전 연결의 화면 복원을 확인하지 못했어요.';

  @override
  String get hostReportedScreenWasRestored =>
      'Host가 화면 복원을 보고했지만 새 화면 상태는 아직 확인되지 않았어요.';

  @override
  String get hostCouldntConfirmTimePrevious =>
      'Host가 이전 연결의 종료와 화면 복원을 제한 시간 안에 확인하지 못했어요.';

  @override
  String get hostReportRestoreUnconfirmed => 'Host 보고 · 복원 미확인';

  @override
  String get restoreReportedWaitingScreenCheck => '복원 보고 · 화면 상태 확인 대기';

  @override
  String get hostCheckTimedOut => 'Host 확인 시간 초과';

  @override
  String get endCheckDelayed => '종료 확인 지연';

  @override
  String get endPreviousStreamingIsntConfirmed =>
      '이전 스트리밍 종료도 확인되지 않았어요. 새로고침해 다시 확인하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get refreshCheckAgainIfKeeps => '새로고침해 다시 확인하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get checkingPreviousConnectionEndedScreen =>
      '이전 연결의 종료와 화면 복원을 확인하고 있어요.';

  @override
  String get connectingBlockedBecauseScreenRestore =>
      '이전 연결의 화면 복원이 확인되지 않아 연결을 막아 뒀어요. 새로고침하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get settingUpVirtualDisplayYou => '가상 화면을 구성하고 있어요. 끝나면 연결할 수 있어요.';

  @override
  String get cantStartConnectionRightNow => '지금은 연결을 시작할 수 없어요. 잠시 후 다시 시도하세요.';

  @override
  String get desktopOff => '데스크톱이 꺼져 있어요.';

  @override
  String get desktopIsntResponding => '데스크톱이 응답하지 않아요.';

  @override
  String get virtualDisplayIsntReadyTell => '가상 화면이 준비되지 않았어요. 관리자에게 알려 주세요.';

  @override
  String get checkingWhetherStreamingReady => '스트리밍 준비 상태를 확인하고 있어요.';

  @override
  String get waitingHostConnect => 'Host 연결을 기다리고 있어요.';

  @override
  String get switchingPower => '전원 전환';

  @override
  String get restoreUnconfirmed => '복원 미확인';

  @override
  String get restoringScreen => '화면 복원 중';

  @override
  String get settingUpScreen => '화면 구성 중';

  @override
  String get poweredOff => '전원 꺼짐';

  @override
  String get offline => '오프라인';

  @override
  String get screenSetup => '화면 준비';

  @override
  String get number => '번호순';

  @override
  String get name => '이름순';

  @override
  String get recent => '최근 사용순';

  @override
  String get runningFirst => '실행 중 먼저';

  @override
  String get all => '전체';

  @override
  String get favorites => '즐겨찾기';

  @override
  String get showAllDesktops => '모든 데스크톱 보기';

  @override
  String get showRunningDesktopsOnly => '실행 중인 데스크톱만 보기';

  @override
  String get showOffDesktopsOnly => '꺼진 데스크톱만 보기';

  @override
  String get noDesktopsAssigned => '배정된 데스크톱이 없어요';

  @override
  String get noRunningDesktops => '실행 중인 데스크톱이 없어요';

  @override
  String get noDesktopsOff => '꺼진 데스크톱이 없어요';

  @override
  String get showVmsOnly => 'VM만 보기';

  @override
  String get showLocalPcsOnly => '로컬 PC만 보기';

  @override
  String get noVmsAssigned => '배정된 VM이 없어요';

  @override
  String get noLocalPcsAssigned => '배정된 로컬 PC가 없어요';

  @override
  String get noDesktopsMatch => '조건에 맞는 데스크톱이 없어요';

  @override
  String noDesktopsMatch2(Object short) {
    return '\'$short\'에 맞는 데스크톱이 없어요';
  }

  @override
  String get searchByNameNumberGroup => '이름, 번호, 분류로 찾을 수 있어요.';

  @override
  String searchedWithin(Object parts) {
    return '$parts 조건 안에서 찾았어요.';
  }

  @override
  String get connectedAccount2 => '내 계정으로 연결돼 있어요';

  @override
  String get anotherUserUsing => '다른 사용자가 사용 중이에요';

  @override
  String accountUsingIt(Object account) {
    return '$account 님이 사용 중이에요';
  }

  @override
  String inUseByAccount(Object account) {
    return '$account 사용 중';
  }

  @override
  String accountUsingItWhenYou(Object account) {
    return '$account 님이 사용 중이에요. 연결하면 그 연결을 끊을지 먼저 물어봐요.';
  }

  @override
  String get connectionAlreadyOpen => '이미 열린 연결이 있어요';

  @override
  String get connection => '내 연결';

  @override
  String get useByAnotherUser => '다른 사용자 사용 중';

  @override
  String get use => '사용 중';

  @override
  String get connectedAccountIfItsConnection =>
      '내 계정으로 연결돼 있어요. 다른 기기에서 연 연결이면 연결할 때 이쪽으로 가져올 수 있어요.';

  @override
  String get anotherUserUsingWhenYou =>
      '다른 사용자가 사용 중이에요. 연결하면 그 연결을 끊을지 먼저 물어봐요.';

  @override
  String get desktopAlreadyHasOpenConnection => '이 데스크톱에 이미 열린 연결이 있어요.';

  @override
  String hostUpdate2(Object version, Object latestVersion) {
    return 'Host 업데이트 · $version → $latestVersion';
  }

  @override
  String get hostUpdateHostOffline => 'Host 업데이트 · Host 오프라인';

  @override
  String get hostUpdateCheckingVersion => 'Host 업데이트 · 버전 확인 중';

  @override
  String hostRemoteUpdateNotSupported(Object version) {
    return 'Host $version · 원격 업데이트 미지원';
  }

  @override
  String hostLatestVersion(Object version) {
    return 'Host $version · 최신 버전';
  }

  @override
  String hostCheckingUpdates(Object version) {
    return 'Host $version · 업데이트 확인 중';
  }

  @override
  String get checkingHostVersion => 'Host 버전 확인 중';

  @override
  String get hostOfflineVersionUnknown => 'Host 오프라인 · 버전 확인 안 됨';

  @override
  String updating(Object host) {
    return '$host · 업데이트 중';
  }

  @override
  String offline2(Object host) {
    return '$host · 오프라인';
  }

  @override
  String upDate2(Object host) {
    return '$host · 최신';
  }

  @override
  String remoteUpdateNotSupportedInstall2(Object host) {
    return '$host · 원격 업데이트 미지원 · 직접 설치 필요';
  }

  @override
  String newVersionAvailable(Object host, Object latestVersion) {
    return '$host · 새 버전 $latestVersion 설치 가능';
  }

  @override
  String get youCanReconnectWhenIts => '끝나면 다시 연결할 수 있어요.';

  @override
  String downloading(Object op, Object after) {
    return '다운로드 중이에요$op. $after';
  }

  @override
  String checkingFile(Object after) {
    return '파일을 확인하고 있어요. $after';
  }

  @override
  String installing(Object after) {
    return '설치 중이에요. $after';
  }

  @override
  String updateStarted(Object after) {
    return '업데이트를 시작했어요. $after';
  }

  @override
  String get hostUpdateWaitingStartsWhen => 'Host 업데이트 대기 · 연결이 끝나면 시작해요';

  @override
  String updatingHostDownloading(Object op) {
    return 'Host 업데이트 중 · 다운로드$op';
  }

  @override
  String get updatingHostCheckingFile => 'Host 업데이트 중 · 파일 확인';

  @override
  String get updatingHostInstalling => 'Host 업데이트 중 · 설치';

  @override
  String hostUpdatedWindowsRestartNeeded(Object targetVersion) {
    return 'Host $targetVersion 업데이트 완료 · Windows 다시 시작 필요';
  }

  @override
  String hostUpdatedCheckHostSettings(Object targetVersion) {
    return 'Host $targetVersion 업데이트 완료 · Host 설정 확인 필요';
  }

  @override
  String hostUpdated(Object targetVersion) {
    return 'Host $targetVersion 업데이트 완료';
  }

  @override
  String checkHostUpdateResult(Object code) {
    return 'Host 업데이트 결과 확인 필요 · $code';
  }

  @override
  String get hostUpdateWaitExpired => 'Host 업데이트 대기 만료';

  @override
  String hostUpdateFailed(Object code) {
    return 'Host 업데이트 실패 · $code';
  }

  @override
  String get hostDidntReconnectAfterUpdate => 'Host가 업데이트 후 다시 연결되지 않았어요';

  @override
  String get checkConnectionStatusHostDiagnostics =>
      'Host 진단과 서버 활동 기록에서 연결 상태를 확인하세요.';

  @override
  String get checkServiceStatusLocalPcs =>
      '로컬 PC의 Host 설정 → 진단에서 서비스 상태를 확인하세요.';

  @override
  String get openDesktopConsoleCheckHost =>
      'Console로 데스크톱을 열고 Host 설정 → 진단을 확인하세요.';

  @override
  String couldntConfirmUpdateResultConnection(Object host) {
    return '$host 업데이트 결과와 연결을 확인하지 못했어요.';
  }

  @override
  String couldntGetResponseFromAfter(Object host) {
    return '$host 업데이트 실패 후 응답을 확인하지 못했어요.';
  }

  @override
  String couldntGetResponseFromAfter2(Object host) {
    return '$host 업데이트 완료 보고 뒤 응답을 확인하지 못했어요.';
  }

  @override
  String get connectionUse => '사용 중인 연결이 있어요';

  @override
  String get anotherUpdateProgress => '다른 업데이트가 진행 중이에요';

  @override
  String get notNewerVersion => '새 버전이 아니에요';

  @override
  String get couldntDownloadInstaller => '설치 파일을 받지 못했어요';

  @override
  String get fileSizeDoesntMatchRelease => '파일 크기가 배포 정보와 달라요';

  @override
  String get filesSha256DoesntMatch => '파일의 SHA-256이 배포 정보와 달라요';

  @override
  String get couldntVerifyReleaseSignature => '배포 서명을 확인하지 못했어요';

  @override
  String get downloadedFileIsntInstaller => '받은 파일이 설치 프로그램이 아니에요';

  @override
  String get couldntStartInstaller => '설치 프로그램을 시작하지 못했어요';

  @override
  String get mustInstalledDesktopDirectly => '데스크톱에서 직접 설치해야 해요';

  @override
  String get noExistingHost => '기존 Host가 없어요';

  @override
  String get didntReceiveInstallResult => '설치 결과를 받지 못했어요';

  @override
  String get couldntFinishInstalling => '설치를 완료하지 못했어요';

  @override
  String get noResponseFromHost => 'Host 응답이 없어요';

  @override
  String get hostConnectionLost => 'Host 연결이 끊겼어요';

  @override
  String get noMoreDetails => '추가 정보가 없어요';

  @override
  String get serverHasntReceivedLocalPcs => '서버가 이 로컬 PC의 랜카드 정보를 아직 받지 못했어요.';

  @override
  String get wakeSignalWasSentBut => '켜기 신호를 보냈지만 4분 안에 켜지지 않았어요.';

  @override
  String get serverCouldntSendWakeSignal => '서버가 켜기 신호를 보내지 못했어요.';

  @override
  String get localPcMustConnectedBy => '로컬 PC가 유선 랜으로 연결돼 있어야 해요.';

  @override
  String get wakeLanMustBiosUefi =>
      'BIOS/UEFI 설정에서 Wake-on-LAN(랜으로 켜기)이 켜져 있어야 해요.';

  @override
  String get deviceManagerNetworkAdapterPower =>
      '장치 관리자 → 랜카드 → 전원 관리·고급에서 ‘매직 패킷으로 깨우기’가 켜져 있어야 해요.';

  @override
  String get localPcMustSameNetwork =>
      '로컬 PC가 서버와 같은 네트워크(같은 공유기·스위치)에 있어야 해요.';

  @override
  String get installLatestHostLocalPc =>
      '로컬 PC에 최신 Host를 설치하고 한 번 켜 두면 서버가 랜카드 정보를 받아요.';

  @override
  String get windowed => '창 모드';

  @override
  String get fullScreen => '전체 화면';

  @override
  String get allMonitors => '모든 모니터';

  @override
  String get askAdministrator => '관리자에게 요청하세요.';

  @override
  String get selectMultitapPrinterDesktopPrint =>
      '데스크톱에서 Multitap 프린터를 선택하세요.\n기기의 인쇄 화면에서 인쇄하거나 PDF로 저장해요.';

  @override
  String get chooseFolderDeviceUnderShared =>
      '메뉴의 공유 폴더에서 이 기기의 폴더를 고르세요.\n데스크톱에서 저장한 파일의 수정 날짜는 저장한 시각이에요.';

  @override
  String get copyPaste => '복사·붙여넣기';

  @override
  String get youCanPasteCopiedText => '복사한 글을 기기와 데스크톱 사이에서 붙여넣을 수 있어요.';

  @override
  String get youCanPasteTextCopied => '기기에서 복사한 글을 데스크톱에 붙여넣을 수 있어요.';

  @override
  String get youCanPasteTextCopied2 => '데스크톱에서 복사한 글을 기기에 붙여넣을 수 있어요.';

  @override
  String get onceConnectionReady => '연결 준비가 끝나면 ';

  @override
  String get pcDesktop => 'PC ↔ 데스크톱';

  @override
  String get pcDesktop2 => 'PC → 데스크톱';

  @override
  String get desktopPc => '데스크톱 → PC';

  @override
  String get text12 => '텍스트';

  @override
  String get files => '파일';

  @override
  String selectMultitapPrinterDesktopPreview(Object printState) {
    return '$printState데스크톱에서 Multitap 프린터를 선택하세요.\nPC에서 미리보기 후 인쇄하거나 PDF로 저장해요.';
  }

  @override
  String get usbDevices => 'USB 장치';

  @override
  String selectUsbDeviceToolbarConnected(Object usbState) {
    return '$usbState툴바에서 USB 장치를 선택하세요.\n연결한 장치는 데스크톱에서만 쓸 수 있어요.';
  }

  @override
  String selectPcFolderToolbarSharing(Object folderState) {
    return '$folderState툴바에서 PC 폴더를 선택하세요.\n연결을 끊으면 공유도 끝나요.';
  }

  @override
  String get filesTransferredWhenYouPaste => '\n파일은 붙여넣을 때 전송해요.';

  @override
  String useCtrlCCtrlV(Object clipboardState, Object n, Object policy) {
    return '${clipboardState}Ctrl+C / Ctrl+V를 사용하세요.\n$n$policy';
  }

  @override
  String get desktopHasNoSoundDevice2 =>
      '이 데스크톱에는 소리 장치가 없어요. 관리자가 Proxmox에서 오디오 장치를 추가해야 해요.';

  @override
  String get restartDesktopUseSoundDevice => '소리 장치를 쓰려면 데스크톱을 다시 시작해야 해요.';

  @override
  String get work => '작업';

  @override
  String get n60FpsAutoBitrate => '60 fps · 자동 전송률';

  @override
  String get saver => '절약';

  @override
  String get n30Fps8MbpsUses => '30 fps · 8 Mbps · 데이터 사용량과 처리 부담을 줄여요';

  @override
  String get restartNeeded => '다시 시작 필요';

  @override
  String get restartWindowsGetVirtualDisplay => 'Windows를 다시 시작해야 가상 화면이 준비돼요.';

  @override
  String get consoleNeeded => '콘솔 필요';

  @override
  String get virtualDisplayCantPreparedRdp =>
      'RDP 로그인에서는 가상 화면을 준비할 수 없어요. 관리자에게 알려 주세요.';

  @override
  String get signNeeded => '로그인 필요';

  @override
  String get signWindowsConnect => 'Windows에 로그인하면 연결할 수 있어요.';

  @override
  String get checkWindowsAccount => 'Windows 계정 확인';

  @override
  String get windowsAccountMustCheckedHost =>
      'Host 설정 → Windows 로그인에서 Windows 계정을 확인해야 해요. 관리자에게 알려 주세요.';

  @override
  String get checkWindowsScreen => 'Windows 화면 확인';

  @override
  String get couldntCheckWindowsScreenIf =>
      'Windows 화면을 확인하지 못했어요. 계속되면 관리자에게 알려 주세요.';

  @override
  String get resumeWindows => 'Windows 복귀';

  @override
  String get windowsLocked => 'Windows 잠김';

  @override
  String get windowsLockedUnlockConnect =>
      'Windows가 잠겨 있어요. 잠금을 해제하면 연결할 수 있어요.';

  @override
  String get adminAccountNeeded => '관리자 계정 필요';

  @override
  String get windowsAccountSignedNowIsnt =>
      '지금 로그인한 Windows 계정은 관리자가 아니에요. 관리자 계정으로 로그인하면 연결할 수 있어요.';

  @override
  String get repairNeeded => '복구 필요';

  @override
  String get virtualDisplayNeedsRepairTell => '가상 화면을 복구해야 해요. 관리자에게 알려 주세요.';

  @override
  String get notReady => '준비 안 됨';

  @override
  String get virtualDisplayIsntReadyIf2 =>
      '가상 화면이 준비되지 않았어요. 계속되면 관리자에게 알려 주세요.';

  @override
  String get checkDriver => '드라이버 확인';

  @override
  String get couldntCheckVirtualDisplayDriver =>
      '가상 화면 드라이버를 확인하지 못했어요. 관리자에게 알려 주세요.';

  @override
  String get checkSettings => '설정 확인';

  @override
  String get virtualDisplaySettingsNeedChecking =>
      '가상 화면 설정을 확인해야 해요. 관리자에게 알려 주세요.';

  @override
  String get hostSettingUpVirtualDisplay =>
      'Host가 가상 화면을 구성하고 있어요. 끝나면 연결할 수 있어요.';

  @override
  String get checkStatus => '상태 확인';

  @override
  String get couldntCheckVirtualDisplayStatus =>
      '가상 화면 상태를 확인하지 못했어요. 계속되면 관리자에게 알려 주세요.';

  @override
  String get checkingMonitors => '모니터 수 확인 중';

  @override
  String get checkMonitorCount => '모니터 수 확인 필요';

  @override
  String get checkAgain2 => ' · 다시 확인 필요';

  @override
  String text13(num activeCount, Object stale) {
    return '모니터 $activeCount대$stale';
  }

  @override
  String get theme2 => '테마';

  @override
  String get background3 => '배경';

  @override
  String get disconnect2 => '연결을 끊을까요?';

  @override
  String streaming3(Object machine) {
    return '$machine · 스트리밍';
  }

  @override
  String ifOneUseUnsavedWork(num usbShared) {
    return '데스크톱에 연결한 USB 장치 $usbShared개도 끊겨요. 쓰는 중이면 저장하지 않은 내용이 사라질 수 있어요.';
  }

  @override
  String programsDesktopSavingThemMay(num foldersShared) {
    return '데스크톱에 공유한 폴더 $foldersShared개도 끊겨요. 데스크톱에서 그 폴더에 저장 중인 프로그램은 저장하지 못할 수 있어요.';
  }

  @override
  String text14(num printsWaiting) {
    return '아직 인쇄하거나 저장하지 않은 문서 $printsWaiting개는 사라져요.';
  }

  @override
  String get clientInfo => '클라이언트 정보';

  @override
  String get windowsClient => 'Windows 클라이언트';

  @override
  String get version => '버전';

  @override
  String get openSourceLicenses => '오픈소스 라이선스';

  @override
  String get licenses => '라이선스';

  @override
  String get about => '정보';

  @override
  String get build => '빌드';

  @override
  String get demoMode => '데모 모드';

  @override
  String get multitapServer => 'Multitap 서버';

  @override
  String get screenIsntConnectedRealServer => '실제 서버에 연결하지 않은 화면이에요.';

  @override
  String get serverManagesDesktopListConnections =>
      '데스크톱 목록·연결·업데이트를 이 서버가 관리해요.';

  @override
  String get address => '주소';

  @override
  String get serverVersion => '서버 버전';

  @override
  String get unavailable => '확인할 수 없음';

  @override
  String get account2 => '계정';

  @override
  String get searchComponentsLicenseText => '구성 요소 또는 원문 검색';

  @override
  String componentsSearchNameLicenseText(Object componentsLength) {
    return '구성 요소 $componentsLength개 · 이름 또는 원문 검색';
  }

  @override
  String get someLicensesCouldntLoaded => '일부 라이선스를 불러오지 못했어요.';

  @override
  String get loading => '불러오는 중…';

  @override
  String get reload => '다시 불러오기';

  @override
  String get noLicensesShow => '표시할 라이선스가 없어요.';

  @override
  String get noMatchingLicenses => '일치하는 라이선스가 없어요.';

  @override
  String text15(num noticesLength) {
    return '원문 $noticesLength개';
  }

  @override
  String version2(Object version) {
    return '버전 $version';
  }

  @override
  String get couldntSaveThemeChooseAgain => '테마를 저장하지 못했어요. 다시 선택해 주세요.';

  @override
  String get couldntSaveBackgroundChooseAgain => '배경을 저장하지 못했어요. 다시 선택해 주세요.';

  @override
  String get connectWithoutEncryption => '암호화 없이 연결할까요?';

  @override
  String get signDetailsScreenInputArent =>
      '로그인 정보·화면·입력이 암호화되지 않아요.\n믿을 수 있는 네트워크에서만 쓰세요.\n동의는 이 주소·포트에만 적용돼요.';

  @override
  String get useHttpAddress => '이 주소에 HTTP 사용';

  @override
  String get hostBulkUpdate => 'Host 일괄 업데이트';

  @override
  String get hostsAssignedDesktops => '배정된 데스크톱의 Host';

  @override
  String assignedDesktopsLatest(Object visibleRowsLength, Object version) {
    return '배정된 데스크톱 $visibleRowsLength대 · 최신 $version';
  }

  @override
  String get checkingServersLatestHostRelease =>
      '이 서버의 최신 Host 배포 정보를 확인하고 있어요.';

  @override
  String get couldntLoadLatestHostRelease =>
      '최신 Host 배포 정보를 불러오지 못했어요.\n창을 닫고 업데이트를 다시 확인하세요.';

  @override
  String hostChanges(Object version) {
    return 'Host $version 변경 내역';
  }

  @override
  String get theresNoConnectionCheckInstall => '설치 결과를 확인할 연결이 없어 요청할 수 없어요.';

  @override
  String get couldntGetLatestStatusFrom =>
      '서버에서 최신 상태를 받지 못했어요. 접수된 요청의 결과는 계속 확인하고 있어요.';

  @override
  String get remainingRequestsStoppedBecauseYou =>
      '권한이 없거나 로그인이 만료돼 남은 요청을 중지했어요. 접수된 업데이트는 계속돼요.';

  @override
  String get latestReleaseChangedSoRemaining =>
      '최신 배포가 바뀌어 남은 요청을 중지했어요. 이 창을 닫고 새 버전을 확인하세요.';

  @override
  String waitingNextRequestLeft(Object pendingCount) {
    return '다음 요청을 기다리는 중 · 남은 $pendingCount대';
  }

  @override
  String get refreshInstallStatus => '설치 상태 새로고침';

  @override
  String get noDesktopsAssigned2 => '배정된 데스크톱이 없어요.';

  @override
  String get checkProgress => '진행 상태 확인 필요';

  @override
  String get requestServerAcceptedIsntFinished =>
      '서버 접수는 설치 완료가 아니에요.\n각 데스크톱의 결과를 확인하세요.';

  @override
  String windowsRestartNeededCheckingResults(
    Object rebootCount,
    Object checkingCount,
  ) {
    return 'Windows 다시 시작 필요 $rebootCount대 · 결과 확인 $checkingCount대';
  }

  @override
  String get connectedDesktopsDisconnectedUpdatedRight =>
      '연결된 데스크톱은 연결을 끊고 바로 업데이트해요.\nWindows는 다시 시작하지 않아요.';

  @override
  String get connectedDesktopsConnectionEndsUpdate =>
      '연결된 데스크톱은 내 연결을 끊고 바로 업데이트해요.\n다른 사용자가 연결 중이면 그 연결이 끝난 뒤 업데이트해요.';

  @override
  String get stopRemainingRequestsClose => '남은 요청 중지·닫기';

  @override
  String get stopRemainingRequests => '남은 요청 중지';

  @override
  String updateSelected(Object selectedCount) {
    return '선택한 $selectedCount대 업데이트';
  }

  @override
  String get selectAll2 => '모두 선택';

  @override
  String get currentVersionUnknown => '현재 버전 확인 필요';

  @override
  String get windowsRestartNeeded => 'Windows 다시 시작 필요';

  @override
  String get enterCurrentPasswordChooseNew =>
      '현재 비밀번호를 입력하고 새 비밀번호는 10~256자로 정해 주세요.';

  @override
  String get couldntChangePasswordTryAgain => '비밀번호를 변경하지 못했어요. 다시 시도해 주세요.';

  @override
  String get newPasswordLeast10Characters => '새 비밀번호 · 10자 이상';

  @override
  String get changing => '변경 중…';

  @override
  String get transferring => '옮기는 중';

  @override
  String get preparing2 => '준비하는 중…';

  @override
  String get finishing => '마무리하는 중…';

  @override
  String get image => '이미지';

  @override
  String get richText => '서식 있는 텍스트';

  @override
  String ofLabel(Object fileCount, Object progress) {
    return '$fileCount개 중 $progress개';
  }

  @override
  String folders(Object folderCount) {
    return '폴더 $folderCount개';
  }

  @override
  String files2(Object fileCount) {
    return '파일 $fileCount개';
  }

  @override
  String get currentFile => '옮기는 파일';

  @override
  String get items => '항목';

  @override
  String get saved2 => '저장 위치';

  @override
  String get transferred => '옮겼어요';

  @override
  String inLabel(Object destinationName) {
    return '$destinationName에 있어요';
  }

  @override
  String get transferCanceled => '전송을 취소했어요';

  @override
  String get couldntTransfer => '옮기지 못했어요';

  @override
  String get transferringFiles => '파일 옮기는 중';

  @override
  String get openFolder => '폴더 열기';

  @override
  String get minimize => '작게 접기';

  @override
  String get transferProgress => '전송 진행률';

  @override
  String get less => '간단히';

  @override
  String get details => '자세히';

  @override
  String get canceling => '취소하는 중이에요';

  @override
  String get cancelTransfer => '이 전송을 취소해요';

  @override
  String get canceling2 => '취소 중…';

  @override
  String get showTransferDetails => '전송 상세 보기';

  @override
  String get almostDone => '곧 끝나요';

  @override
  String aboutSecLeft(Object seconds) {
    return '약 $seconds초 남음';
  }

  @override
  String aboutMinLeft(Object minutes) {
    return '약 $minutes분 남음';
  }

  @override
  String aboutLeft(num minutes) {
    return '약 $minutes시간 남음';
  }

  @override
  String aboutHMinLeft(Object minutes, Object rest) {
    return '약 $minutes시간 $rest분 남음';
  }

  @override
  String elapsed(Object elapsed) {
    return '경과 $elapsed';
  }

  @override
  String elapsed2(Object stepLabel, Object elapsed) {
    return '$stepLabel, 경과 $elapsed';
  }

  @override
  String get couldntConfirmSessionSwitchResult =>
      '세션 전환 결과를 확인하지 못했어요. 목록을 새로고침하세요.';

  @override
  String get couldntTakeOverWindowsScreen => 'Windows 화면을 넘겨받지 못했어요';

  @override
  String get takingOverWindowsScreen => 'Windows 화면을 넘겨받는 중';

  @override
  String get bringingOverWindowsScreenYou =>
      'RDP에서 쓰던 Windows 화면을 가져오고 있어요.\n끝나면 바로 연결해요.\n취소해도 이미 시작한 전환은 계속될 수 있어요.';

  @override
  String get start => '켜기';

  @override
  String get stop => '정지';

  @override
  String get shutDown => '종료';

  @override
  String get stopIsntAvailableServerYet =>
      '이 서버에서는 아직 정지를 쓸 수 없어요. 관리자에게 알려 주세요.';

  @override
  String get localPc2 => '로컬 PC를';

  @override
  String get desktop3 => '데스크톱을';

  @override
  String start2(Object noun) {
    return '$noun 켤까요?';
  }

  @override
  String the(Object noun, Object label) {
    return '$noun $label할까요?';
  }

  @override
  String get startsLocalPcOverNetwork => '네트워크로 로컬 PC를 켜요. 켜진 뒤에 연결할 수 있어요.';

  @override
  String get youCanConnectOnceIts => '켜진 뒤에 연결할 수 있어요.';

  @override
  String get cutsPowerRightAwayUnsaved =>
      '전원을 바로 끊어요. 저장하지 않은 작업이 사라질 수 있어요. Windows가 응답하지 않을 때만 쓰세요.';

  @override
  String get saveAnyUnsavedWorkFirst => '저장하지 않은 작업이 있으면 먼저 저장하세요.';

  @override
  String get asksWindowsShutDownNormally =>
      'Windows를 정상적으로 끄도록 요청해요. 저장하지 않은 작업이 있으면 먼저 저장하세요.';

  @override
  String get pcNeeds2Monitors => 'PC에 모니터가 2대 있어야 해요.';

  @override
  String get desktopsHostDoesntSupportAll => '이 데스크톱의 Host가 모든 모니터를 지원하지 않아요.';

  @override
  String get defaultLabel => '기본';

  @override
  String get streamingSettings2 => '스트리밍 설정…';

  @override
  String get removeFromFavorites => '즐겨찾기에서 빼기';

  @override
  String get addFavorites => '즐겨찾기에 추가';

  @override
  String get checkReadiness => '연결 준비 점검';

  @override
  String get searchCtrlF => '검색 (Ctrl+F)';

  @override
  String get clear => '지우기';

  @override
  String get closeSearch => '검색 닫기';

  @override
  String get searchNameNumberGroup => '이름, 번호, 분류 검색';

  @override
  String get statusFilter => '상태 필터';

  @override
  String get typeFilter => '구분 필터';

  @override
  String text16(Object label, Object count) {
    return '$label $count개';
  }

  @override
  String text17(Object chipLabel, Object count) {
    return '$chipLabel $count개';
  }

  @override
  String get selectAgainClear => '다시 누르면 해제돼요';

  @override
  String showOnly(Object chipLabel) {
    return '$chipLabel만 보기';
  }

  @override
  String get windowsNeedsRestart => 'Windows를 다시 시작해야 해요';

  @override
  String get getsVirtualDisplayReadyUnsaved =>
      '그래야 가상 화면이 준비돼요. 저장하지 않은 작업은 사라질 수 있어요.';

  @override
  String get signedOverRdp => 'RDP로 로그인돼 있어요';

  @override
  String get virtualDisplayCantPreparedRdp2 => 'RDP 로그인에서는 가상 화면을 준비할 수 없어요.';

  @override
  String get notSignedWindows => 'Windows에 로그인돼 있지 않아요';

  @override
  String get windowsAccountNeedsChecking => 'Windows 계정 확인이 필요해요';

  @override
  String get couldntCheckWindowsScreen => 'Windows 화면을 확인하지 못했어요';

  @override
  String get refreshCheckAgainIfKeeps2 => '새로고침해 다시 확인하세요. 계속되면 관리자에게 알려 주세요.';

  @override
  String get windowsScreenFromRdpOpen => 'RDP에서 쓰던 Windows 화면이 있어요';

  @override
  String get bringScreenIntoStreamingConnect => '그 화면을 스트리밍으로 가져와야 연결할 수 있어요.';

  @override
  String get windowsLocked2 => 'Windows가 잠겨 있어요';

  @override
  String get unlockConnect => '잠금을 해제하면 연결할 수 있어요.';

  @override
  String get signWindowsAdministratorAccount => 'Windows 관리자 계정으로 로그인해야 해요';

  @override
  String get virtualDisplayNeedsRepair => '가상 화면을 복구해야 해요';

  @override
  String get youCanRepairHostSettings => 'Host 설정에서 복구할 수 있어요. 관리자에게 알려 주세요.';

  @override
  String get virtualDisplayIsntReady => '가상 화면이 준비되지 않았어요';

  @override
  String get refreshMomentIfKeepsHappening =>
      '잠시 후 새로고침하세요. 계속되면 관리자에게 알려 주세요.';

  @override
  String get virtualDisplayDriverNeedsChecking => '가상 화면 드라이버를 확인해야 해요';

  @override
  String get youCanCheckHostSettings =>
      'Host 설정의 진단에서 확인할 수 있어요. 관리자에게 알려 주세요.';

  @override
  String get virtualDisplaySettingsNeedChecking2 => '가상 화면 설정 확인이 필요해요';

  @override
  String get hostSettingUpScreen => 'Host가 화면을 구성하고 있어요';

  @override
  String get youCanConnectWhenSetup => '구성이 끝나면 연결할 수 있어요.';

  @override
  String get couldntCheckVirtualDisplayStatus2 => '가상 화면 상태를 확인하지 못했어요';

  @override
  String get checkingWindowsSign => 'Windows 로그인 확인 중';

  @override
  String get checkingWindowsSignStateUpdates =>
      'Windows 로그인 상태를 확인하고 있어요. 준비되면 바로 바뀌어요.';

  @override
  String get preparingScreen => '화면 준비 중';

  @override
  String get preparingVirtualDisplayUpdatesAs =>
      '가상 화면을 준비하고 있어요. 끝나면 바로 바뀌어요.';

  @override
  String get preparing3 => '연결 준비 중';

  @override
  String get hostPreparingConnectionUpdatesAs =>
      'Host가 연결을 준비하고 있어요. 끝나면 바로 바뀌어요.';

  @override
  String get power => '전원';

  @override
  String get checkedAfterPowerActionFinishes => '전원 작업이 끝나면 확인해요';

  @override
  String get confirmedByHostResponse => '켜짐 · Host 응답으로 확인';

  @override
  String get off4 => '꺼져 있어요';

  @override
  String get pausedTellAdministrator => '일시 중지됨 · 관리자에게 알려 주세요';

  @override
  String get powerStateNotReceivedYet => '전원 상태를 아직 받지 못했어요';

  @override
  String get hostService => 'Host 서비스';

  @override
  String get checkedWhenDesktopStarts => '데스크톱이 켜지면 확인해요';

  @override
  String get responding => '응답함';

  @override
  String get notRespondingIfWindowsStarting =>
      '응답하지 않아요 · Windows가 시작 중이면 잠시 기다려 주세요';

  @override
  String get noResponseYet => '응답을 아직 받지 못했어요';

  @override
  String get virtualDisplay2 => '가상 화면';

  @override
  String get checkedWhenHostServiceResponds => 'Host 서비스가 응답하면 확인해요';

  @override
  String get screenRestoreFromLastConnection => '이전 연결의 화면 복원이 확인되지 않았어요';

  @override
  String get checkingScreenRestoreFromLast => '이전 연결의 화면 복원을 확인하는 중';

  @override
  String get checkingVirtualDisplaySetup => '가상 화면 구성을 확인하는 중';

  @override
  String get numberScreensNotReceivedYet => '화면 개수를 아직 받지 못했어요';

  @override
  String ready3(Object count, Object m) {
    return '$count개 준비됨$m';
  }

  @override
  String get readyOnceScreenUsedRdp => 'RDP에서 쓰던 화면을 가져오면 준비돼요';

  @override
  String get notReadyCheckHostSettings => '준비 안 됨 · Host 설정에서 확인해야 해요';

  @override
  String get couldNotCheckWindowsScreen => '스트리밍할 Windows 화면을 확인하지 못했어요';

  @override
  String get checkingStreamingScreen => '스트리밍 화면 상태를 확인하는 중';

  @override
  String get connectionPath => '연결 경로';

  @override
  String availableViaCheckedAgainWhen(Object server) {
    return '$server 경유 연결 가능 · 연결할 때 다시 확인해요';
  }

  @override
  String get notCheckedCheckedOnceEarlier => '확인 안 됨 · 앞 단계가 준비되면 확인해요';

  @override
  String get sample => '예시';

  @override
  String get screenPreview => '화면 미리보기';

  @override
  String get designSampleNotConnectedServer => '디자인 예시 · 서버에 연결하지 않아요';

  @override
  String starting(Object noun) {
    return '$noun 켜는 중이에요';
  }

  @override
  String restarting(Object noun) {
    return '$noun 다시 시작하는 중이에요';
  }

  @override
  String shuttingDown(Object noun) {
    return '$noun 종료하는 중이에요';
  }

  @override
  String stopping(Object noun) {
    return '$noun 정지하는 중이에요';
  }

  @override
  String get powerActionProgress => '전원 작업 중이에요';

  @override
  String get starting2 => '켜는 중';

  @override
  String get shuttingDown2 => '종료하는 중';

  @override
  String get stopping2 => '정지하는 중';

  @override
  String get powerAction => '전원 작업 중';

  @override
  String get ifWindowsDoesNotRespond => 'Windows가 응답하지 않으면 전원 메뉴에서 정지할 수 있어요.';

  @override
  String get statusCheckedAgainWhenFinishes => '작업이 끝나면 상태를 다시 확인해요.';

  @override
  String get windowsSign2 => 'Windows 로그인';

  @override
  String get takeOverWindowsScreenConnect => 'Windows 화면을 넘겨받아 연결';

  @override
  String get bringsWindowsScreenUsedRdp => 'RDP에서 쓰던 Windows 화면을 스트리밍으로 가져와요.';

  @override
  String get readyConnect => '연결 준비됨';

  @override
  String get serverSaysYouCanConnect => '서버가 연결할 수 있다고 알려 줬어요.';

  @override
  String get pressStartStartDesktop => '‘켜기’를 누르면 데스크톱이 켜져요.';

  @override
  String get paused => '일시 중지';

  @override
  String get desktopPaused => '데스크톱이 일시 중지돼 있어요';

  @override
  String get letAdministratorKnow => '관리자에게 알려 주세요.';

  @override
  String get cleaningUpConnectionJustEnded =>
      '방금 끝난 연결을 정리하고 있어요. 누르면 끝나는 대로 연결해요.';

  @override
  String get screenRestoreNotConfirmed => '화면 복원이 확인되지 않았어요';

  @override
  String get cleaningUpLastConnection => '이전 연결을 정리하고 있어요';

  @override
  String get youCanConnectWhenDone => '끝나면 연결할 수 있어요.';

  @override
  String get useConsole => 'Console에서 사용 중이에요';

  @override
  String get youCanDisconnectConsoleConnect => 'Console을 끊고 스트리밍으로 연결할 수 있어요.';

  @override
  String get checking => '확인 중';

  @override
  String get settingUpVirtualDisplay => '가상 화면을 구성하고 있어요';

  @override
  String get updating2 => '업데이트 중';

  @override
  String get checkHost => 'Host 확인 필요';

  @override
  String get windowsStarting => 'Windows 시작 중';

  @override
  String get windowsStarting2 => 'Windows를 시작하고 있어요';

  @override
  String get changesAutomaticallyWhenRestartFinishes => '다시 시작이 끝나면 자동으로 바뀌어요.';

  @override
  String get changesAutomaticallyWhenStartupFinishes => '시작이 끝나면 자동으로 바뀌어요.';

  @override
  String get preparingConnection => '연결을 준비하고 있어요';

  @override
  String get windowsHasStartedChangesAs =>
      'Windows가 시작됐어요. Host가 연결되면 바로 바뀌어요.';

  @override
  String get hostDisconnected => 'Host 연결 끊김';

  @override
  String get desktopRunningChangesAutomaticallyWhen =>
      '데스크톱은 실행 중이에요. Host가 돌아오면 자동으로 바뀌어요.';

  @override
  String get hostNotResponding => 'Host가 응답하지 않아요';

  @override
  String get ifYouJustStartedWait => '방금 켰다면 잠시 기다려 주세요. 계속되면 관리자에게 알려 주세요.';

  @override
  String get unlockConsole => 'Console에서 잠금을 해제하세요.';

  @override
  String get ifSigningAutomaticallyYouCan =>
      '자동 로그인 중이면 곧 연결할 수 있어요. 아니면 Console에서 로그인하세요.';

  @override
  String get signFromConsoleAfterRestart =>
      'Console에서 로그인하세요. 다시 시작하면 자동으로 로그인돼요.';

  @override
  String get virtualDisplayNeedsCheckedHost =>
      'Host 설정에서 가상 화면을 확인해야 해요. 관리자에게 알려 주세요.';

  @override
  String get checkingStreamingReadiness => '스트리밍 준비를 확인하고 있어요';

  @override
  String get changesAsSoonAsHost => 'Host가 상태를 보고하면 바로 바뀌어요.';

  @override
  String get waitingHostConnect2 => 'Host 연결을 기다리고 있어요';

  @override
  String get checkingStatus => '상태를 확인하고 있어요';

  @override
  String get hostConnectsOnceWindowsStarts => 'Windows가 시작되면 Host가 연결돼요.';

  @override
  String get checkingAgainAutomaticallyMoment => '잠시 후 자동으로 다시 확인해요.';

  @override
  String get power2 => '전원 켜기';

  @override
  String get preparing4 => '연결 준비';

  @override
  String get connecting => '연결하는 중';

  @override
  String get manageLocalPcHostUpdate => '로컬 PC 관리 · Host 업데이트 있음';

  @override
  String get manageLocalPc => '로컬 PC 관리';

  @override
  String get manageDesktopHostUpdateAvailable => '데스크톱 관리 · Host 업데이트 있음';

  @override
  String get manageDesktop => '데스크톱 관리';

  @override
  String step3ElapsedDesktopStays(Object progress, Object clock) {
    return '$progress/3단계 · $clock 경과 · 취소해도 데스크톱은 켜져 있어요';
  }

  @override
  String defaultStreaming(Object mode) {
    return '기본 연결: 스트리밍 · $mode';
  }

  @override
  String get diagnostics => '진단';

  @override
  String get restart2 => '다시 시작…';

  @override
  String get disconnectConsoleConnect2 => 'Console 끊고 연결';

  @override
  String get working => '처리 중…';

  @override
  String get preparing5 => '준비 중…';

  @override
  String get streamingConnection => '스트리밍 연결';

  @override
  String get streamingFullScreen => '스트리밍 · 전체 화면';

  @override
  String get streamingAllMonitors => '스트리밍 · 모든 모니터';

  @override
  String get startsDesktopYouCanConnect => '데스크톱을 켜요. 켜진 뒤에 연결할 수 있어요.';

  @override
  String streamingConnection2(Object mode) {
    return '스트리밍 연결 · $mode';
  }

  @override
  String get connectionOptions => '연결 옵션';

  @override
  String get otherWaysConnect => '다른 연결 방식';

  @override
  String get connectsByStreamingAsSoon => '준비되면 바로 스트리밍으로 연결해요';

  @override
  String get qemuAgentOff => 'QEMU Agent 꺼짐';

  @override
  String get noSound => '소리 없음';

  @override
  String screenFromMayDifferNow(Object picture, Object machine) {
    return '$picture $machine 화면이에요. 지금과 다를 수 있어요.';
  }

  @override
  String lastSavedScreenScreenMay(Object machine) {
    return '저장된 마지막 화면이에요. 지금 $machine 화면과 다를 수 있어요.';
  }

  @override
  String get noPreview => '미리보기 없음';

  @override
  String get qemuGuestAgentOffProxmox =>
      'Proxmox VM 옵션에서 QEMU Guest Agent가 꺼져 있어요.\n관리자가 켜면 Windows가 시작 중이거나 Host 연결이 끊겼을 때 상태를 더 정확히 보여 줘요.';

  @override
  String startedAgo(Object uptime) {
    return '$uptime 전에 켜졌어요';
  }

  @override
  String startedAgoAccordingProxmox(Object uptime) {
    return 'Proxmox 기준 $uptime 전에 켜졌어요';
  }

  @override
  String get statusDetails => '상태 자세히';

  @override
  String get unknown3 => '확인 못함';

  @override
  String monitor(Object o, Object o2) {
    return '모니터 $o $o2';
  }

  @override
  String get couldNotFindRemoteScreen => '원격 화면을 찾지 못했어요.';

  @override
  String get couldNotCheckWindowsScale => 'Windows 배율을 확인하지 못했어요.';

  @override
  String get windowsDidNotAllowScale => 'Windows가 배율 변경을 허용하지 않았어요.';

  @override
  String get couldNotSaveOriginalScale => '원래 배율을 저장하지 못해 배율을 바꾸지 않았어요.';

  @override
  String get remoteScreenDoesNotSupport => '이 원격 화면은 Windows 배율 변경을 지원하지 않아요.';

  @override
  String get couldNotCheckResultScale => '배율 변경 결과를 확인하지 못했어요.';

  @override
  String get disconnected2 => '연결이 끊겼어요.';

  @override
  String get youCannotChangeScaleConnection => '이 연결에서는 배율을 바꿀 수 없어요.';

  @override
  String autoNow(Object currentScale) {
    return '자동 · 현재 $currentScale%';
  }

  @override
  String get autoCurrentScaleNotChecked => '자동 · 현재 배율 확인 전';

  @override
  String get apply => '적용';

  @override
  String requested4(Object wanted) {
    return ' · 요청 $wanted%';
  }

  @override
  String applied(Object currentScale, Object wanted) {
    return '적용 $currentScale%$wanted';
  }

  @override
  String requestedChecking(Object wanted) {
    return '$wanted% 요청 · 적용 확인 중';
  }

  @override
  String get currentScaleNotCheckedYet => '현재 배율 확인 전';

  @override
  String resolutionAutoNow(Object sourceWidth, Object sourceHeight) {
    return '해상도 자동 · 현재 $sourceWidth × $sourceHeight';
  }

  @override
  String get resolutionAutoCurrentSizeNot => '해상도 자동 · 현재 크기 확인 전';

  @override
  String get scale => '배율';

  @override
  String autoCurrentScaleUnknown(Object scaleStatus) {
    return '자동 · 현재 배율 확인 못함\n$scaleStatus';
  }

  @override
  String max(Object o) {
    return '최대 · $o';
  }

  @override
  String upScreenSize(Object maximum) {
    return '이 화면 크기에서 최대 $maximum%';
  }

  @override
  String get folderAlreadyShared => '이미 공유한 폴더예요.';

  @override
  String youCanShareUpFolders(Object driveMaxShares) {
    return '폴더는 $driveMaxShares개까지 공유할 수 있어요.';
  }

  @override
  String get couldNotFindUserSigned =>
      'PC에 로그인한 사용자를 확인하지 못해 공유하지 못했어요.\nWindows 탐색기가 실행 중인지 확인하세요.';

  @override
  String get pcUserCannotOpenFolder =>
      'PC 사용자 권한으로 열 수 없는 폴더예요. 다른 폴더를 고르거나 폴더 권한을 확인해 주세요.';

  @override
  String get couldNotFindFolderPc =>
      'PC에서 폴더를 찾을 수 없어요. 이름을 바꿨거나 지웠거나, USB·네트워크 드라이브가 연결돼 있지 않을 수 있어요.';

  @override
  String get youCanOnlyShareFolders => '폴더만 공유할 수 있어요.';

  @override
  String get pathCannotShared => '공유할 수 없는 경로예요.';

  @override
  String get cannotOpenExplorerDesktopCheck =>
      '데스크톱의 탐색기에서 열 수 없어요.\n데스크톱에 로그인돼 있는지 확인하세요.';

  @override
  String get couldNotOpenFolderExplorer =>
      '데스크톱의 탐색기에서 폴더를 열지 못했어요.\n‘열기’를 다시 누르세요.';

  @override
  String get couldNotCheckWhetherFolder =>
      '폴더가 열렸는지 확인하지 못했어요.\n데스크톱의 탐색기를 보고, 안 열렸으면 ‘열기’를 다시 누르세요.';

  @override
  String get sharingFolderHasEndedReconnecting => '이 폴더의 공유가 끝났거나 다시 연결 중이에요.';

  @override
  String get tryAgainAfterOtherTask => '다른 작업이 끝난 뒤 다시 시도하세요.';

  @override
  String get sharedFoldersNotAvailableWindow =>
      '이 창에서는 공유 폴더를 쓸 수 없어요. 창을 닫고 다시 연결해 보세요.';

  @override
  String get webclientServiceDesktopOffMissing =>
      '데스크톱의 WebClient 서비스가 꺼져 있거나 없어요.\n관리자에게 알려 주세요.';

  @override
  String get desktopHasNoDriveLetters =>
      '데스크톱에 남은 드라이브 문자가 없어요. 데스크톱에서 안 쓰는 드라이브 연결을 끊은 뒤 폴더를 다시 추가하세요.';

  @override
  String get youNotSignedDesktopSign =>
      '데스크톱에 로그인돼 있지 않아요. Windows에 로그인한 뒤 다시 연결하세요.';

  @override
  String get couldNotConnectDriveDesktop =>
      '데스크톱에서 드라이브를 연결하지 못했어요. 공유를 끝내고 폴더를 다시 추가하세요.';

  @override
  String get administratorTurnedOffSharedFolder => '관리자가 공유 폴더 권한을 해제했어요.';

  @override
  String get couldNotShareFolder => '폴더를 공유하지 못했어요.';

  @override
  String get youAboutShareWholeDrive =>
      '드라이브 전체를 공유하려고 해요.\n데스크톱의 프로그램이 이 드라이브의 모든 파일을 읽고 바꿀 수 있어요.\n꼭 필요한 폴더만 고르는 걸 권해요.';

  @override
  String get windowsProgramFolderChangingDeleting =>
      'Windows·프로그램 폴더예요.\n데스크톱에서 이 폴더의 파일을 바꾸거나 지우면 PC가 제대로 동작하지 않을 수 있어요.';

  @override
  String get youAboutShareWholeAccount =>
      '내 계정 폴더 전체(또는 AppData)를 공유하려고 해요.\n로그인 정보와 프로그램 설정까지 데스크톱에서 보일 수 있어요.';

  @override
  String get networkFolderIfConnectionDrops =>
      '네트워크 폴더예요. 연결이 끊기면 데스크톱에서 쓸 수 없고, 파일을 여는 데 시간이 오래 걸릴 수 있어요.';

  @override
  String get foldersMoreThan20000 =>
      '파일이 20,000개를 넘는 폴더는 데스크톱에서 처음 20,000개만 보여요. 폴더를 나눠 공유해 보세요.';

  @override
  String get untilDesktopRestartsFilesOver =>
      '데스크톱을 다시 시작하기 전까지는 50 MB가 넘는 파일을 옮길 수 없어요.';

  @override
  String get singleFileOver4Gb =>
      '파일 하나가 4 GB를 넘으면 공유 폴더로 옮길 수 없어요. 클립보드 복사·붙여넣기로 옮기세요.';

  @override
  String get cannotOpenFolderPcCheck =>
      'PC에서 폴더를 열 수 없어요. USB·네트워크 연결을 확인하고 다시 시도하세요.';

  @override
  String get sharedFolderWasDisconnectedConnect =>
      '공유 폴더 연결이 끊겼어요. 데스크톱에 다시 연결한 뒤 폴더를 다시 고르세요.';

  @override
  String get sharedFoldersReconnectingWait =>
      '공유 폴더를 다시 연결하고 있어요. 잠시만 기다려 주세요.';

  @override
  String wasDeletedSoSharingEnded(Object name) {
    return '‘$name’ 폴더가 삭제돼서 공유를 끝냈어요';
  }

  @override
  String get shareFolderDesktopAsDrive => '이 폴더를 데스크톱에 드라이브로 공유할까요?';

  @override
  String folderCautionYouCanTurn(Object path, Object risk) {
    return '폴더: $path\n\n주의: $risk';
  }

  @override
  String get share => '공유';

  @override
  String get stopSharing => '공유를 끝낼까요?';

  @override
  String programsDesktopHaveFilesFrom(
    Object shownName,
    Object path,
    Object device,
  ) {
    return '‘$shownName’ ($path)\n\n데스크톱에서 이 폴더의 파일을 열어 둔 프로그램은 저장하지 못할 수 있어요. 폴더와 파일은 $device에 그대로 남아요.';
  }

  @override
  String get stopSharing2 => '공유 끝내기';

  @override
  String get reconnectingSharedFolders => '공유 폴더를 다시 연결하고 있어요';

  @override
  String get sharedFoldersDisconnected => '공유 폴더 연결이 끊겼어요';

  @override
  String get preparingSharedFolders => '공유 폴더를 준비하고 있어요';

  @override
  String get choosingFolder => '폴더를 고르는 중이에요';

  @override
  String youCanShareUpFolders2(Object driveMaxShares) {
    return '폴더는 $driveMaxShares개까지 공유할 수 있어요';
  }

  @override
  String get desktopCanReadChangeFiles => '데스크톱에서 공유한 폴더의 파일을 읽고 바꿀 수 있어요.';

  @override
  String shareFoldersFrom(Object device) {
    return '$device 폴더 공유';
  }

  @override
  String get pcDeviceWord => 'PC';

  @override
  String get onlyFoldersYouChooseShared =>
      '고른 폴더만 이번 연결에서 공유해요.\n연결을 끊으면 공유도 끝나요.';

  @override
  String get noSharedFoldersYet => '아직 공유한 폴더가 없어요.';

  @override
  String get preparingSharedFolders2 => '공유 폴더를 준비하고 있어요…';

  @override
  String get hideDetails => '자세히 접기';

  @override
  String get sharingLimits => '공유 제한 자세히';

  @override
  String get openingFolderTakingWhilePlease =>
      '폴더를 여는 데 시간이 걸리고 있어요.\n잠시 기다려 주세요.';

  @override
  String chooseFolderShare2(Object device) {
    return '공유할 폴더를 $device에서 골라요';
  }

  @override
  String get addFolder => '폴더 추가…';

  @override
  String get disconnected3 => '공유 끊김';

  @override
  String get checking2 => '확인 중…';

  @override
  String get connecting2 => '연결 중…';

  @override
  String connected3(Object letter) {
    return '$letter: 연결됨';
  }

  @override
  String get checkingWhetherOpened => '열기 결과를 확인하고 있어요';

  @override
  String get youCanOpenOnceShared => '공유 폴더가 연결되면 열 수 있어요';

  @override
  String get opensSharedFolderExplorerDesktop => '공유 폴더를 데스크톱의 탐색기에서 열어요';

  @override
  String get open => '열기';

  @override
  String get closeRemoteWindow => '원격 창 닫기';

  @override
  String get encoderApplied => '적용했어요';

  @override
  String get couldNotConfirmSaveYet => '적용 결과를 아직 확인하지 못했어요. 상태를 다시 확인하세요.';

  @override
  String get couldNotStartSavingTry =>
      '적용을 시작하지 못했어요. 잠시 후 다시 시도하세요. 설정은 바꾸지 않았어요.';

  @override
  String get saveResponseCurrentSettingsNeed =>
      '적용 응답과 현재 설정이 달라요. 다시 고르지 말고 상태를 새로 확인하세요.';

  @override
  String get encoderAuto => '자동 (권장)';

  @override
  String get encoderAutoDetail => 'GPU가 있으면 GPU, 없으면 CPU';

  @override
  String get encoderGpu => 'GPU';

  @override
  String get encoderGpuDetail => 'NVIDIA·AMD·Intel';

  @override
  String get encoderX264 => 'CPU (x264)';

  @override
  String get encoderMft => 'CPU (Windows 기본)';

  @override
  String get encoderWhichGpu => '쓸 GPU';

  @override
  String get encoderPickGpu => '쓸 GPU를 고르세요.';

  @override
  String get encoderAdminOnly => '관리자 권한으로 Host 설정을 열면 바꿀 수 있어요.';

  @override
  String get encoderReloadedAfterConflict =>
      '다른 곳에서 설정이 바뀌어 다시 불러왔어요. 다시 골라 주세요.';

  @override
  String encoderRecentLine(Object label, Object time) {
    return '최근 확인: $label ($time)';
  }

  @override
  String get hostEncoder => 'Host 인코더';

  @override
  String get chooseMethodNextStreamingConnection =>
      '고르면 바로 적용되고, 다음 스트리밍 연결부터 쓰여요.\n지금 연결은 끊기지 않고 인코더도 그대로예요.\nGPU가 시작되지 않거나 출력이 실패하면 CPU로 바꿔요.';

  @override
  String get savingChoice => '적용 중';

  @override
  String get checkingCurrentChoiceGpuCandidates => '현재 선택과 GPU 후보 확인 중';

  @override
  String get hostCannotReportEncoderChoice =>
      '이 Host에서 인코더 선택 상태를 확인할 수 없어요. Host를 업데이트하거나 새로고침하세요.';

  @override
  String savedChoice(Object current) {
    return '저장된 선택: $current';
  }

  @override
  String get triesGpuUsedCaptureEarlier =>
      '캡처에 사용한 GPU와 확인된 이전 후보를 먼저 시도해요.\n현재 해상도·H.264 출력이 확인되는 후보를 사용하고, 실패하면 CPU로 전환해요.';

  @override
  String get couldNotCheckGpuList => 'GPU 목록을 확인하지 못했어요. 상태를 다시 확인하세요.';

  @override
  String get noConfirmedGpuCandidatesYou =>
      '확인된 GPU 후보가 없어요. CPU 방식은 그대로 사용할 수 있어요.';

  @override
  String get listedCandidateDoesNotMean =>
      '후보 표시는 인코딩 성공을 뜻하지 않아요. 연결 시 실제 지원과 출력을 확인해요.';

  @override
  String get discardChanges => '변경 취소';

  @override
  String get encoderConfirmedRecordedTimeDoes =>
      '기록 시각에 확인된 인코더예요.\n현재 연결 상태를 뜻하지 않아요.';

  @override
  String lastOutputLine(Object screen, Object label, Object time) {
    return '최근 출력: 화면 $screen · $label · $time';
  }

  @override
  String get noOutputRecord => '최근 확인: 아직 기록이 없어요';

  @override
  String get noOutputRecordMeaning => '기록이 없다고 GPU를 쓰지 않은 것은 아니에요.';

  @override
  String get checkSaveStatus => '저장 상태 확인';

  @override
  String get refreshStatus2 => '상태 새로고침';

  @override
  String get gpuChoiceRefersAdapterCurrent =>
      'GPU는 현재 Windows의 어댑터를 선택해요. Windows를 다시 시작하거나 드라이버를 바꾼 뒤에는 다시 선택해야 할 수 있어요.\nCPU 기본값은 GPU가 없는 데스크톱에서도 유지돼요.';

  @override
  String get pendingApproval => '승인 대기';

  @override
  String get cancelingRequest => '요청 취소 중';

  @override
  String get available2 => '확인 가능';

  @override
  String get overview => '개요';

  @override
  String get diagnosticReport => '진단 보고서';

  @override
  String get devices => '장치';

  @override
  String needsAttention(Object titles) {
    return '$titles, 확인 필요';
  }

  @override
  String get checkingHostStatus => 'Host 상태 확인 중';

  @override
  String get waitingWindowsRespond => 'Windows 응답을 기다리고 있어요.';

  @override
  String get checkEncoderSaveStatusFirst => '인코더 저장 상태를 먼저 확인하세요.';

  @override
  String get discardEncoderChoice => '인코더 선택을 버릴까요?';

  @override
  String get unsavedEncoderChoiceNotApplied => '저장하지 않은 인코더 선택은 적용하지 않아요.';

  @override
  String get discardChanges2 => '변경 버리기';

  @override
  String get savingEncoderChoice => '인코더 선택 저장 중';

  @override
  String get savingChoiceNextStreamingConnection =>
      '다음 스트리밍 연결부터 쓸 선택을 저장하고 있어요.';

  @override
  String get checkEncoderSaveStatusBefore => '인코더 저장 상태를 먼저 확인한 뒤 창을 닫아 주세요.';

  @override
  String get closeWindowAfterSettingsTask => '설정 작업이 끝난 뒤 창을 닫아 주세요.';

  @override
  String get discardUnsavedSettings => '저장하지 않은 설정을 버릴까요?';

  @override
  String get serverAddressAccountDetailsEncoder =>
      '입력 중인 서버 주소, 계정 정보와 인코더 선택은 저장하지 않아요.';

  @override
  String get closeWithoutSaving => '저장하지 않고 닫기';

  @override
  String get taskStatusChangedSoWindow =>
      '작업 상태가 바뀌어 창을 닫지 않았어요. 상태를 확인한 뒤 다시 시도하세요.';

  @override
  String get statusResponseDelayedCheckAgain =>
      '상태 응답이 지연되고 있어요. 잠시 후 다시 확인하세요.';

  @override
  String get previousStatusCheckWasCanceled => '이전 상태 조회를 취소했어요.';

  @override
  String get administratorApprovedServerRegistrationComplete =>
      '관리자 승인으로 서버 등록이 완료됐어요. 서비스와 가상 화면 상태를 확인하세요.';

  @override
  String get checkingWindowsServiceStatus => 'Windows와 서비스 상태를 조회하고 있어요.';

  @override
  String get hostServicePreparingVirtualDisplay =>
      'Host 서비스가 가상 화면과 서버 연결을 준비하고 있어요.';

  @override
  String get installingVirtualDisplayDriver => '가상 화면 드라이버 설치 중';

  @override
  String get checkingSignatureInstallingDriverScreen =>
      '서명을 확인하고 드라이버를 설치하고 있어요. 화면이 잠시 깜빡일 수 있어요. 창을 닫지 마세요.';

  @override
  String get installingWinfsp => 'WinFsp 설치 중';

  @override
  String get installingWinfspMayTake1 => 'WinFsp를 설치하고 있어요. 창을 닫지 마세요.';

  @override
  String get couldNotCheckWinfspInstall =>
      'WinFsp 설치 결과를 확인하지 못했어요.\n상태를 새로고침해 확인하세요.';

  @override
  String get installationFinished => '설치를 마쳤어요';

  @override
  String get couldNotFinishInstallation => '설치 마무리를 끝내지 못했어요';

  @override
  String get continueLabel => '계속';

  @override
  String get requestSentWaitingWindowsRespond =>
      '요청을 전달했어요. Windows 응답을 기다리고 있어요.';

  @override
  String get couldNotStartTaskOpen =>
      '작업을 시작하지 못했어요. Host 설정을 다시 연 뒤 시도하세요. 설정은 바꾸지 않았어요.';

  @override
  String get checkingChangedStatus => '바뀐 상태를 확인하고 있어요.';

  @override
  String get statusCheckStoppedRefreshCurrent =>
      '상태 조회가 중단됐어요. 현재 상태를 새로고침하세요.';

  @override
  String get taskRespondedButChangedStatus =>
      '작업 응답은 받았지만 바뀐 상태를 읽지 못했어요. 다시 실행하지 말고 상태를 새로고침하세요.';

  @override
  String get checkResult => '결과 확인이 필요해요';

  @override
  String get done2 => '작업 완료';

  @override
  String get refreshStatusCheck => '상태를 새로고침해 확인하세요.';

  @override
  String get passwordsDoNotMatchEnter => '비밀번호가 일치하지 않아요. 다시 입력하세요.';

  @override
  String get checkFormatWindowsAccountPassword =>
      'Windows 계정 또는 비밀번호 형식을 확인하세요.';

  @override
  String get checkWindowsAccountPassword => 'Windows 계정과 비밀번호를 확인하세요.';

  @override
  String get windowsAccountLockedItsSign =>
      'Windows 계정이 잠겼거나 로그인이 제한돼 있어요. Windows에서 계정 상태를 확인하세요.';

  @override
  String get windowsCouldNotCheckAccount =>
      'Windows에서 계정을 확인하지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get couldNotSaveDeleteWindows =>
      'Windows 계정을 저장하거나 삭제하지 못했어요. 상태를 새로고침해 확인하세요.';

  @override
  String get couldNotFinishSettingUp =>
      'Windows 계정 설정을 완료하지 못했어요. 계정 정보와 현재 상태를 확인하세요.';

  @override
  String get couldNotFinishSettingUp2 => '가상 화면 설정을 완료하지 못했어요.';

  @override
  String get couldNotCheckVirtualDisplay =>
      '가상 화면 정리·설치 결과를 확인하지 못했어요. 실제 상태와 충돌 목록을 새로 확인하세요.';

  @override
  String get useHttpAddressOpenSettings => 'HTTP 주소는 관리자 권한으로 설정을 열어야 쓸 수 있어요.';

  @override
  String get testingServerConnection => '서버 연결 테스트 중';

  @override
  String get serverChecked => '서버를 확인했어요.';

  @override
  String get changeServerConnection => '서버 연결을 변경할까요?';

  @override
  String serverRequestOnceAdministratorApproves(Object origin) {
    return '요청할 서버: $origin\n\n관리자가 승인하면 지금 등록을 새 서버로 바꿔요. 사용 중인 연결이 끊길 수 있어요.';
  }

  @override
  String get requestChange => '변경 요청';

  @override
  String get requestingServerConnection => '서버에 연결 요청 중';

  @override
  String get connectionRequestSentApproveAdmin =>
      '연결 요청을 보냈어요. 관리자 웹에서 이 요청을 승인해 주세요.';

  @override
  String get updateHostCancelRequestSafely => '이 요청을 안전하게 취소하려면 Host를 업데이트하세요.';

  @override
  String get editServerConnect => '연결할 서버를 수정할까요?';

  @override
  String get cancelConnectionRequest => '연결 요청을 취소할까요?';

  @override
  String get cancelsCurrentApprovalRequestSo =>
      '현재 승인 요청을 취소한 뒤 서버 주소를 다시 입력해요. 기존에 등록된 서버 연결은 유지돼요.';

  @override
  String get stopsWaitingAdministratorApprovalExisting =>
      '관리자 승인 대기를 중단해요. 기존에 등록된 서버 연결은 유지돼요.';

  @override
  String get cancelEdit => '취소 후 수정';

  @override
  String get cancelRequest => '요청 취소';

  @override
  String get cancelingConnectionRequest => '연결 요청 취소 중';

  @override
  String get connectionRequestWasCanceledCheck =>
      '연결 요청 취소를 처리했어요. 새 요청 전 현재 상태를 확인하세요.';

  @override
  String get hostUpdateNeeded => 'Host 업데이트가 필요해요';

  @override
  String get startHostService => 'Host 서비스를 시작할까요?';

  @override
  String get onceServiceStartsConnectsRegistered =>
      '서비스가 켜지면 등록된 서버에 자동으로 연결해요.';

  @override
  String get startService => '서비스 시작';

  @override
  String get startingService => '서비스 시작 중';

  @override
  String get serviceHasStartedReconnectsServer =>
      '서비스를 시작했어요. 잠시 후 서버에 다시 연결해요.';

  @override
  String get updateHostRestartSafelyCurrent =>
      '안전하게 다시 시작하려면 Host를 업데이트하세요. 기존 서비스는 계속 실행돼요.';

  @override
  String get restartHostService => 'Host 서비스를 다시 시작할까요?';

  @override
  String get activeConnectionsDropBrieflyYou =>
      '사용 중인 연결이 잠깐 끊겨요. 서비스가 다시 켜지면 다시 연결할 수 있어요.\nWindows와 설정은 그대로예요.';

  @override
  String get restartingService => '서비스 다시 시작 중';

  @override
  String get serviceHasRestartedReconnectsServer =>
      '서비스를 다시 시작했어요. 잠시 후 서버에 다시 연결해요.';

  @override
  String get restoreVirtualDisplay => '가상 화면을 복구할까요?';

  @override
  String get setUpVirtualDisplay => '가상 화면을 설정할까요?';

  @override
  String get restoresInterruptedScreenChangePrevious =>
      '중단된 화면 변경을 이전 설정으로 복구해요. 화면이 잠시 끊길 수 있어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 실행하세요.';

  @override
  String get checksSignedDriverPreparesVirtual =>
      '서명된 드라이버를 확인하고 가상 화면을 준비해요. 화면이 잠시 끊기거나 Windows를 다시 시작해야 할 수 있어요. 자동으로 다시 시작하지 않아요.';

  @override
  String get restore => '복구';

  @override
  String get settings => '설정';

  @override
  String get virtualDisplayRequestDoneCheck =>
      '가상 화면 요청을 처리했어요. 켜진 모니터와 다시 시작 안내를 확인하세요.';

  @override
  String get currentSign => '현재 로그인';

  @override
  String get activeConnections => '사용 중인 연결';

  @override
  String get screenTasksProgress => '진행 중인 화면 작업';

  @override
  String get earlierVirtualMonitor => '이전 가상 모니터';

  @override
  String get proxmoxConsoleScreen => 'Proxmox 콘솔 화면';

  @override
  String get logBackup => '기록 백업';

  @override
  String get virtualDisplayService => '가상 화면 서비스';

  @override
  String get checked => '확인됨';

  @override
  String get needsChecking => '확인이 필요해요';

  @override
  String get checkConsoleSign => '콘솔 로그인 확인';

  @override
  String get cannotRunFromRdpScreen => 'RDP 화면에서는 실행할 수 없어요';

  @override
  String get runFromSignProxmoxConsole =>
      'Proxmox 콘솔(로컬 PC는 본체 화면) 로그인에서 실행하세요';

  @override
  String get unlockWindows => 'Windows 잠금을 해제하세요';

  @override
  String get serviceNotRunning => '서비스가 실행 중이 아니에요';

  @override
  String get anotherResetProgress => '다른 초기화가 진행 중이에요';

  @override
  String get couldNotCheckServiceResponse => '서비스 응답을 확인하지 못했어요';

  @override
  String get noActiveConnections => '사용 중인 연결 없음';

  @override
  String get streamingConnected => '스트리밍 연결 중';

  @override
  String get windowsRdpSignActive => 'Windows RDP 로그인이 활성 상태';

  @override
  String get consoleSwitchProgress => '콘솔 전환 작업 중';

  @override
  String get changingNumberMonitors => '모니터 수 변경 중';

  @override
  String get automaticRestoreProgress => '자동 복구 진행 중';

  @override
  String get waitingEarlierScreenTaskEnd => '이전 화면 작업 종료 대기';

  @override
  String get resolutionMonitorChangeProgress => '해상도·모니터 변경 진행 중';

  @override
  String get virtualMonitorUse => '가상 모니터가 사용 중';

  @override
  String get streamingScreenTaskProgress => '스트리밍 화면 작업 진행 중';

  @override
  String get noneLeft => '남아 있지 않음';

  @override
  String get stillPresentDriver => '드라이버에 아직 남아 있음';

  @override
  String get couldNotConnectDriver => '드라이버에 연결하지 못함';

  @override
  String get couldNotReadVirtualDisplay => '가상 화면 소유 정보를 읽지 못함';

  @override
  String get multitapVirtualDisplayDriverNot => 'Multitap 가상 화면 드라이버가 선택되지 않음';

  @override
  String get onLabel4 => '켜져 있음';

  @override
  String get turnedAgain => '다시 켰음';

  @override
  String get anotherScreenNoConsoleId => '다른 화면이 켜져 있음 (콘솔 식별 정보 없음)';

  @override
  String get noScreen => '켜진 화면이 없어요';

  @override
  String get multitapVirtualMonitorStill => 'Multitap 가상 모니터가 아직 켜져 있음';

  @override
  String get windowsRefusedTurnConsoleScreen => 'Windows가 콘솔 화면 켜기를 거부함';

  @override
  String get couldNotConfirmAfterTurning => '콘솔 화면을 켠 뒤 확인하지 못함';

  @override
  String get couldNotReadScreenSetup => '화면 구성을 읽지 못함';

  @override
  String get cannotCheckFromCurrentSign => '현재 로그인에서 확인할 수 없음';

  @override
  String get movedBackupFolder => '백업 폴더로 이동함';

  @override
  String get noLogsCleanUp => '정리할 기록 없음';

  @override
  String get couldNotCreateBackupFolder => '백업 폴더를 만들지 못함';

  @override
  String get onlyPartlyMoved => '일부만 이동함';

  @override
  String get preparedAgain => '다시 준비함';

  @override
  String get couldNotCheckResetResult => '초기화 결과를 확인하지 못했어요. 상태를 새로고침하세요.';

  @override
  String text18(num moved) {
    return ' $moved개를';
  }

  @override
  String get text19 => '을';

  @override
  String backedUpRecoveryLogsPrepared(Object moved) {
    return '복구 기록$moved 백업하고 가상 화면 서비스를 다시 준비했어요.\n새로고침해 가상 화면을 확인하세요.';
  }

  @override
  String get nothingWasResetLogsScreens =>
      '초기화하지 않았어요.\n기록과 화면은 그대로예요.\n아래 항목을 해결한 뒤 다시 시도하세요.';

  @override
  String get resetVirtualDisplayRecoveryLogs => '가상 화면 복구 기록을 초기화할까요?';

  @override
  String get movesLogsEarlierStreamingScreens =>
      '자동 복구가 끝나지 않은 이전 스트리밍 화면 기록을 백업 폴더로 옮기고 가상 화면 서비스를 다시 준비해요.\n\n• 스트리밍 연결이나 화면 변경이 진행 중이면 실행하지 않아요.\n• 이전 Multitap 가상 모니터가 드라이버에 남아 있지 않은지 먼저 확인해요.\n• Proxmox 콘솔 화면이 꺼져 있으면 그 화면만 다시 켜요. 다른 화면·드라이버·보안 설정은 바꾸지 않아요.\n• 기록은 삭제하지 않고 백업해요. Windows를 다시 시작하지 않아요.\n\nProxmox 콘솔(로컬 PC는 본체 화면)의 Windows 로그인에서 실행하세요.';

  @override
  String get resetLogs => '기록 초기화';

  @override
  String get resettingVirtualDisplayRecoveryLogs => '가상 화면 복구 기록 초기화 중';

  @override
  String get virtualDisplayRecoveryLogsWere => '가상 화면 복구 기록 초기화를 처리했어요.';

  @override
  String get saveWorkRestartWindowsThen =>
      '작업을 저장하고 Windows를 다시 시작한 뒤 화면을 확인하세요.';

  @override
  String get anotherUserSignedConsoleTheir =>
      '콘솔에 다른 사용자가 로그인해 있어요. 해당 로그인을 유지하며 전환을 보류해요.';

  @override
  String get signStatusChangedRefreshStatus =>
      '로그인 상태가 바뀌었어요. 상태를 새로고침한 뒤 다시 시도하세요.';

  @override
  String get openHostSettingsAsAdministrator => '관리자 권한으로 Host 설정을 열어 주세요.';

  @override
  String get currentSignAlreadyConnectedStreaming =>
      '현재 로그인이 스트리밍용 콘솔에 연결돼 있어요. 가상 화면 준비 상태를 확인하세요.';

  @override
  String get virtualDisplayDeviceNotReady =>
      '가상 화면 장치가 준비되지 않았어요. 장치 상태를 먼저 확인하세요.';

  @override
  String get windowsDidNotAllowSign =>
      'Windows가 로그인 전환을 허용하지 않았어요. 현재 상태를 새로고침해 확인하세요.';

  @override
  String get couldNotCheckCurrentSign =>
      '현재 로그인과 콘솔 상태를 확인하지 못했어요. 새로고침한 뒤 다시 시도하세요.';

  @override
  String get switchStreamingScreen => '스트리밍 화면으로 전환할까요?';

  @override
  String get movesCurrentWindowsSignStreaming =>
      '현재 Windows 로그인을 스트리밍 화면으로 옮겨요.\nRDP 연결이 끊길 수 있어요.\n전환 후 Multitap 클라이언트에서 스트리밍으로 연결하세요.';

  @override
  String get switchScreen => '화면 전환';

  @override
  String get switchingWindowsSign => 'Windows 로그인 전환 중';

  @override
  String get switchRequestedCheckWhetherVirtual =>
      '전환을 요청했어요. 가상 화면 준비 상태를 확인하세요.';

  @override
  String get windowsSignSwitchConfirmedOnce =>
      'Windows 로그인 전환을 확인했어요. 가상 화면이 준비되면 클라이언트에서 스트리밍으로 연결하세요.';

  @override
  String get switchRequestWasReceivedBut =>
      '전환 요청은 받았지만 완료를 확인하지 못했어요. 다시 전환하기 전에 상태를 새로고침하세요.';

  @override
  String get checkingVirtualDisplayConflicts => '가상 화면 충돌 항목 확인 중';

  @override
  String get lookingUpExistingDevicesLeftover =>
      '기존 장치와 남아 있는 설정을 조회해요. 아직 삭제하지 않아요.';

  @override
  String get couldNotCheckConflictsRefresh =>
      '충돌 항목을 확인하지 못했어요. 상태를 새로고침하고 다시 확인하세요.';

  @override
  String get leftoverSetting => '남아 있는 설정';

  @override
  String impact(Object kind, Object item, Object location, Object item2) {
    return '$kind · $item\n$location\n영향: $item2';
  }

  @override
  String get cleanUpCheckedConflictsInstall => '확인한 충돌 항목을 정리하고 설치할까요?';

  @override
  String allRequiredItemsShownCleaned(Object description) {
    return '$description\n\n표시된 필수 항목을 모두 정리하고 Multitap 드라이버를 설치해요.\n가상 화면이 끊기거나 Windows를 다시 시작해야 할 수 있어요.\n드라이버 보관함·기존 설정 파일은 유지하며, 자동으로 다시 시작하지 않아요.';
  }

  @override
  String get cleanUpInstall => '정리 후 설치';

  @override
  String get cleaningUpConflictsInstallingMultitap =>
      '충돌 항목 정리·Multitap 드라이버 설치 중';

  @override
  String get operationTimedOutCheckActual =>
      '작업 응답 시간이 지났어요. 실제 상태와 충돌 목록을 확인한 뒤 다시 시도하세요.';

  @override
  String get devicesSettingsChangedSoCleanup =>
      '장치 또는 설정이 변경되어 정리를 시작하지 않았어요. 충돌 목록을 새로 확인하세요.';

  @override
  String get deviceRemovalRequestWasSent =>
      '장치 제거 요청은 전달했지만 실제 제거를 확인하지 못했어요. 장치 상태와 충돌 목록을 확인하세요.';

  @override
  String get onlySomeItemsWereCleaned => '일부 항목만 정리됐어요. 충돌 목록을 다시 확인한 뒤 조치하세요.';

  @override
  String get conflictsWereCleanedUpBut =>
      '충돌 항목은 정리했지만 드라이버 설치를 완료하지 못했어요. 현재 상태와 진단 정보를 확인하세요.';

  @override
  String get cleanupInstallDidNotFinish =>
      '충돌 항목 정리·설치를 완료하지 못했어요. 현재 상태와 충돌 목록을 다시 확인하세요.';

  @override
  String get driverInstalledSaveWorkRestart =>
      '드라이버를 설치했어요. 작업을 저장하고 Windows를 다시 시작하세요. 자동으로 다시 시작하지 않아요.';

  @override
  String get driverInstalledSignWindowsFrom =>
      '드라이버를 설치했어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 Windows에 로그인한 뒤 가상 화면 설정을 완료하세요.';

  @override
  String get driverInstalledCheckNewVirtual =>
      '드라이버를 설치했어요. 새로 확인된 가상 화면 상태를 확인하세요.';

  @override
  String get enterWindowsAccount => 'Windows 계정을 입력하세요.';

  @override
  String get enterNewPassword => '새 비밀번호를 입력하세요.';

  @override
  String get enterWindowsPassword => 'Windows 비밀번호를 입력하세요.';

  @override
  String get passwordTooLong => '비밀번호가 너무 길어요.';

  @override
  String useLeastCharacters(Object minimum) {
    return '$minimum자 이상으로 만들어 주세요.';
  }

  @override
  String get passwordsDoNotMatch => '비밀번호가 일치하지 않아요.';

  @override
  String get createNewPassword => '새 비밀번호를 만들까요?';

  @override
  String get saveWindowsAccountTurnAuto => 'Windows 계정을 저장하고 자동 로그인을 켤까요?';

  @override
  String get saveWindowsAccount => 'Windows 계정을 저장할까요?';

  @override
  String windowsAccount2(Object username) {
    return 'Windows 계정: $username';
  }

  @override
  String get accountGetsWindowsPasswordNeeded =>
      '이 계정에 Windows 비밀번호가 생겨요. 다음 로그인부터 이 비밀번호가 필요해요.';

  @override
  String get thenAccountSavedAutoSign => '그다음 계정을 저장하고 자동 로그인을 켜요.';

  @override
  String get streamingConnectsWindowsSignedAccount =>
      '스트리밍은 이 계정으로 로그인된 Windows에 연결해요.';

  @override
  String get whenWindowsStartsSignsAccount =>
      'Windows가 시작되면 이 계정으로 자동 로그인해요. 잠금 해제는 별도예요.';

  @override
  String get hostTakesOverExistingAuto =>
      '기존 자동 로그인 설정은 이 Host가 넘겨받아요. Windows 레지스트리에 남은 비밀번호가 있으면 삭제되며 복원할 수 없어요.';

  @override
  String get passwordStoredOnlyHostsProtected =>
      '비밀번호는 이 Host의 보호 저장소에만 저장해요. 로컬 관리자는 접근할 수 있어요.';

  @override
  String get windowsAccountPasswordsSecurityPolicies =>
      'Windows 계정 비밀번호와 보안 정책은 바꾸지 않아요.';

  @override
  String get createPassword => '비밀번호 만들기';

  @override
  String get saveTurn => '저장하고 켜기';

  @override
  String get windowsPasswordCreatedNeededFrom =>
      'Windows 비밀번호: 만들었어요. 다음 로그인부터 필요해요.';

  @override
  String get windowsPasswordResultNotConfirmed =>
      'Windows 비밀번호: 결과를 확인하지 못했어요. 비밀번호 칸을 비우고 다시 저장해 확인하세요.';

  @override
  String get windowsPasswordNotCreatedReopen =>
      'Windows 비밀번호: 만들지 않았어요. Host 설정을 다시 연 뒤 시도하세요.';

  @override
  String windowsPasswordNotCreated(Object refused) {
    return 'Windows 비밀번호: 만들지 못했어요. $refused';
  }

  @override
  String get windowsAccountNotSaved => 'Windows 계정: 저장하지 않았어요.';

  @override
  String get autoSignNotChanged => '자동 로그인: 바꾸지 않았어요.';

  @override
  String get checkingSavingWindowsAccount => 'Windows 계정 확인·저장 중';

  @override
  String get windowsAccountCheckedSaved => 'Windows 계정을 확인하고 저장했어요.';

  @override
  String windowsAccountNotSaved2(Object value) {
    return 'Windows 계정: 저장하지 못했어요. $value';
  }

  @override
  String get settingsChangedDuringCheckCheck =>
      '확인하는 동안 설정이 바뀌었어요. 상태를 확인한 뒤 계정을 다시 저장하세요.';

  @override
  String get notChangedRefreshStatusCheck => '바뀌지 않았어요. 상태를 새로고침해 확인하세요.';

  @override
  String get windowsAccountSaved => 'Windows 계정: 저장했어요.';

  @override
  String autoSign(Object autologon) {
    return '자동 로그인: $autologon';
  }

  @override
  String get creatingWindowsPassword => 'Windows 비밀번호 만드는 중';

  @override
  String get checkingWindowsAccount => 'Windows 계정 확인 중';

  @override
  String get checkingWhetherAccountHasPassword => '이 계정에 비밀번호가 있는지 확인하고 있어요.';

  @override
  String get couldNotConfirmAccountHas => '비밀번호가 없는지 확인하지 못했어요. 비밀번호를 입력하세요.';

  @override
  String get accountAlreadyHasPasswordEnter =>
      '이 계정에는 이미 비밀번호가 있어요. 그 비밀번호를 입력하세요.';

  @override
  String get doesNotMeetWindowsPassword =>
      'Windows 비밀번호 규칙에 맞지 않아요. 더 길게, 대·소문자·숫자·기호를 섞어 주세요.';

  @override
  String get couldNotConfirmAccountHas2 => '비밀번호가 없는지 확인하지 못했어요.';

  @override
  String get windowsRefusedChange => 'Windows가 변경을 거부했어요.';

  @override
  String get checkPasswordFormat => '비밀번호 형식을 확인하세요.';

  @override
  String get notLocalAccountWindowsEnter =>
      '이 Windows의 로컬 계정이 아니에요. 비밀번호를 입력하세요.';

  @override
  String get accountDoesNotExistWindows => '이 Windows에 없는 계정이에요. 계정 이름을 확인하세요.';

  @override
  String get accountBlockedCheckAccountStatus =>
      '사용이 막힌 계정이에요. Windows에서 계정 상태를 확인하세요.';

  @override
  String get accountChangedDuringCheckSave => '확인 중 계정이 바뀌었어요. 다시 저장하세요.';

  @override
  String get couldNotCheckAccountTry => '계정을 확인하지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get hostDoesNotSupportAuto => '이 Host는 자동 로그인 설정을 지원하지 않아요. 계정만 저장해요.';

  @override
  String get noAdministratorRightsSoAuto => '관리자 권한이 없어 자동 로그인은 바꾸지 않아요.';

  @override
  String get windowsPolicyPreventsAutoSign =>
      'Windows 정책으로 자동 로그인을 켤 수 없어요. 정책은 바꾸지 않아요.';

  @override
  String get autoSignSettingAlreadyExists =>
      '기존 자동 로그인 설정이 있어요. 이 Host 버전은 넘겨받을 수 없어요.';

  @override
  String get previousChangeMustRecoveredFirst =>
      '이전 변경의 복구가 먼저 필요해요. 계정만 저장해요.';

  @override
  String get autoSignCannotChangedRight => '지금은 자동 로그인을 바꿀 수 없어요. 상태를 확인하세요.';

  @override
  String get updateHostClearSavedInfo2 => '저장 정보를 안전하게 지우려면 Host를 업데이트하세요.';

  @override
  String get clearWindowsSignInfoSaved => 'Host에 저장된 Windows 로그인 정보를 지울까요?';

  @override
  String onlyHostsSavedInfoCleared(Object username) {
    return '$username의 Host 저장 정보만 지워요.\nWindows 사용자와 기존 자동 로그인 설정은 유지해요.';
  }

  @override
  String get clearSavedInfo => '저장 정보 지우기';

  @override
  String get savedInfoChanged => '저장 정보가 바뀌었어요';

  @override
  String get checkNewStatusThenChoose => '새 상태를 확인한 뒤 다시 선택하세요.';

  @override
  String get clearingSignInfoSavedHost => 'Host 저장 로그인 정보 지우는 중';

  @override
  String get windowsSignInfoSavedHost => 'Host에 저장된 Windows 로그인 정보를 지웠어요.';

  @override
  String get checkingVirtualDisplayStatus => '가상 화면 상태를 확인하고 있어요.';

  @override
  String get noVirtualDisplayDriverYet =>
      '가상 화면 드라이버가 아직 없어요. 설치 마무리를 눌러 설치하세요.';

  @override
  String get screenRecoveryFromLastStreaming =>
      '이전 스트리밍의 화면 복구가 끝나지 않았어요. 계속되면 아래에서 복구 기록을 초기화하세요.';

  @override
  String get multitapVirtualDisplayAvailableCurrent =>
      '현재 Windows 로그인에서 Multitap 가상 화면을 사용할 수 있어요.';

  @override
  String get youSignedViaRdpSwitch => '현재 RDP 로그인이에요. 아래에서 스트리밍 화면으로 전환하세요.';

  @override
  String get servicePreparingVirtualDisplayPlease =>
      '서비스가 가상 화면을 준비하고 있어요. 잠시 기다려 주세요.';

  @override
  String get previousScreenChangeWasInterrupted =>
      '이전 화면 변경이 중단됐어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 복구를 실행하세요.';

  @override
  String get anotherVirtualDisplayDevicePresent =>
      '다른 가상 화면 장치가 있어요. 그 장치는 바꾸지 않아요. 아래에서 기존 장치를 확인하세요.';

  @override
  String get signWindowsFromProxmoxConsole =>
      'Proxmox 콘솔(로컬 PC는 본체 화면)에서 Windows에 로그인한 뒤 가상 화면 설정을 실행하세요.';

  @override
  String get runVirtualDisplaySetupPrepare => '가상 화면 설정을 실행해 모니터를 준비하세요.';

  @override
  String get virtualDisplayDeviceDidNot =>
      '가상 화면 장치가 시작되지 않았어요. 설정을 다시 적용하고 Windows 장치 상태를 확인하세요.';

  @override
  String get noVirtualDisplayRunVirtual =>
      '켜진 가상 화면이 없어요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 설정을 실행하세요.';

  @override
  String get virtualDisplaySetupNeedsAdministrator => '가상 화면 설정에 관리자 권한이 필요해요.';

  @override
  String get windowsCouldNotFinishPreparing =>
      'Windows가 가상 화면 드라이버 준비를 완료하지 못했어요. 진단 정보를 복사해 관리자에게 알려 주세요.';

  @override
  String get existingVirtualDisplaySetupNeeds =>
      '기존 가상 화면 설정 확인이 필요해요. 진단 정보를 복사해 관리자에게 알려 주세요. 기존 장치는 변경하지 않았어요.';

  @override
  String get virtualDisplayDeviceOwnershipDuplicate =>
      '가상 화면 장치의 소유권 또는 중복 설치 확인이 필요해요. 다른 제품의 장치는 변경하지 않았어요.';

  @override
  String get couldNotVerifyDriverFiles =>
      '드라이버 파일을 확인하지 못했어요. Host 설치 파일을 다시 받아 실행하세요. Windows 보안 설정은 바꾸지 마세요.';

  @override
  String get couldNotVerifyVirtualDisplay =>
      '가상 화면 구성 파일을 확인하지 못했어요. Host 설치 파일을 다시 받아 실행하세요.';

  @override
  String get couldNotSaveVirtualDisplay =>
      '가상 화면 장치 설정을 저장하지 못했어요. 관리자 권한과 Windows 장치 상태를 확인하세요.';

  @override
  String get previousScreenChangeNeedsRecovery =>
      '이전 화면 변경의 복구가 필요해요. Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 복구를 실행하세요.';

  @override
  String get hostServiceManagesVirtualDisplay =>
      '이 가상 화면은 Host 서비스가 관리해요. 상태를 새로고침해 현재 준비 단계를 확인하세요.';

  @override
  String get couldNotReadVirtualDisplay2 =>
      '현재 서비스의 가상 화면 상태를 읽지 못했어요. 서비스 상태와 진단 정보를 확인하세요.';

  @override
  String get couldNotCheckCurrentDisplay =>
      '현재 화면 배치를 확인하지 못했어요. 상태를 새로고침하고 계속되면 진단 정보를 확인하세요.';

  @override
  String get youSignedViaRdpSwitch2 =>
      '현재 RDP 로그인이에요. 스트리밍 화면으로 전환하거나 Proxmox 콘솔(로컬 PC는 본체 화면)에서 같은 Windows 계정으로 로그인하세요.';

  @override
  String get couldNotCheckVirtualDisplay2 =>
      '가상 화면 상태를 확인하지 못했어요. 진단 정보를 복사해 관리자에게 알려 주세요.';

  @override
  String get hostInfo => 'Host 정보';

  @override
  String get hostToolsVersion => 'Host 도구 버전';

  @override
  String get availableWhenOperationFinishes => '작업이 끝나면 확인할 수 있어요.';

  @override
  String get availableAfterRegisteringServer => '서버에 등록하면 확인할 수 있어요.';

  @override
  String update2(Object newer) {
    return '$newer 업데이트';
  }

  @override
  String get connectSoundOutputDeviceTurn => '소리 출력 장치를 연결하거나 Windows에서 켜세요.';

  @override
  String get proxmoxConsoleLocalPcScreen => 'Proxmox 콘솔(로컬 PC는 본체 화면)';

  @override
  String get localPcScreen => '본체 화면';

  @override
  String get consoleLocalPcScreen => 'Console(로컬 PC는 본체 화면)';

  @override
  String get proxmoxConsole => 'Proxmox 콘솔';

  @override
  String get checkVirtualDisplay2 => '가상 화면 확인 필요';

  @override
  String get notRegistered2 => '등록 안 됨';

  @override
  String get desktop4 => '이 데스크톱';

  @override
  String get readyStream => '스트리밍 가능';

  @override
  String get checkingStatus2 => '상태 확인 중';

  @override
  String get checkingHostStatus2 => 'Host 상태를 확인하고 있어요';

  @override
  String get notRegisteredServerYet => '아직 서버에 등록하지 않았어요';

  @override
  String get components => '구성 요소';

  @override
  String get checking3 => '확인하고 있어요';

  @override
  String get signedOutLocked => '로그인돼 있지 않거나 잠겨 있어요';

  @override
  String accountAutoSign(Object accountLabel) {
    return '$accountLabel 계정 · 자동 로그인 켜짐';
  }

  @override
  String get noSavedWindowsAccount => '저장된 Windows 계정이 없어요';

  @override
  String get autoSignAccountDiffersFrom => '자동 로그인 계정과 저장된 계정이 달라요';

  @override
  String ready4(num monitors) {
    return '가상 모니터 $monitors대 · 준비됨';
  }

  @override
  String get noDriverYet => '드라이버가 아직 없어요';

  @override
  String get screenRecoveryNeeded => '화면 복구가 필요해요';

  @override
  String get mustSwitchStreamingScreen => '스트리밍 화면으로 전환해야 해요';

  @override
  String get servicePreparing => '서비스가 준비하고 있어요';

  @override
  String get virtualDisplayNeedsChecking => '가상 화면을 확인해야 해요';

  @override
  String get whileStreamingOnlyVirtualDisplay =>
      '스트리밍하는 동안에는 가상 화면만 켜지고, 끝나면 원래 화면으로 돌아가요.';

  @override
  String get couldNotReadHostStatus => 'Host 상태를 읽지 못했어요. 새로고침해 다시 확인해 주세요.';

  @override
  String get couldNotReadHostService =>
      'Host 서비스 상태를 읽지 못했어요. 새로고침해 다시 확인해 주세요.';

  @override
  String get hostServiceNotInstalledRun =>
      'Host 서비스가 설치돼 있지 않아요. Host 설치 파일을 다시 실행해 주세요.';

  @override
  String get hostServiceChangingStateRefresh =>
      'Host 서비스가 상태를 바꾸는 중이에요. 잠시 뒤 새로고침해 주세요.';

  @override
  String get hostServiceOffSoCannot =>
      'Host 서비스가 꺼져 있어 서버에 연결하지 못해요. 서비스를 시작하면 자동으로 다시 연결해요.';

  @override
  String get screenRecoveryFromLastStreaming2 =>
      '이전 스트리밍의 화면 복구가 끝나지 않았어요. 가상 화면에서 확인하세요.';

  @override
  String checkVirtualDisplayStatusFrom(Object restart) {
    return 'Proxmox 콘솔(로컬 PC는 본체 화면)에서 가상 화면 상태를 확인하고 복구를 진행하세요.$restart';
  }

  @override
  String get viewVirtualDisplay => '가상 화면 보기';

  @override
  String get notRegisteredServerYetRequest =>
      '아직 서버에 등록하지 않았어요. 관리자에게 받은 서버 주소로 연결을 요청하세요.';

  @override
  String get connect4 => '연결하기';

  @override
  String get cancelingConnectionRequestWaitingServer =>
      '연결 요청을 취소하고 있어요. 서버의 취소 확인을 기다리고 있어요.';

  @override
  String get requestExpiredRefreshStatusThen =>
      '요청 유효 시간이 지났어요. 상태를 새로고침한 뒤 다시 요청하세요.';

  @override
  String get checkingRegistrationResultApprovedConnection =>
      '승인된 연결의 등록 결과를 확인하고 있어요.';

  @override
  String get adminConsoleUnderPendingApprovals =>
      '관리자 웹의 승인 대기에서 확인 코드가 같은지 본 뒤 승인해 주세요.';

  @override
  String get approveConnectionRequestAdminConsole =>
      '관리자 웹의 승인 대기에서 이 연결 요청을 승인해 주세요.';

  @override
  String get viewRequest => '요청 보기';

  @override
  String get connectionDiagnostics => '연결 진단';

  @override
  String get fromLocalPcScreen => '본체 화면에서';

  @override
  String get fromConsoleClient => '클라이언트의 Console로';

  @override
  String signRestartDesktopSignAutomatically(Object where) {
    return '$where 로그인하거나 데스크톱을 다시 시작하면 자동으로 로그인돼요.';
  }

  @override
  String sign2(Object where) {
    return '$where 로그인해 주세요.';
  }

  @override
  String windowsSignedOutLocked(Object where) {
    return 'Windows에 로그인돼 있지 않거나 잠겨 있어요. $where';
  }

  @override
  String get signSettings => '로그인 설정';

  @override
  String get currentWindowsSignNotStreaming =>
      '지금 Windows 로그인은 스트리밍 화면에 연결돼 있지 않아요. 가상 화면에서 스트리밍 화면으로 전환하세요.';

  @override
  String get autoSignOffSaveWindows =>
      '자동 로그인이 꺼져 있어요. Windows 계정을 저장하면 다시 시작한 뒤에도 바로 연결할 수 있어요.';

  @override
  String get autoSignNeedsCheckingSave =>
      '자동 로그인 설정을 확인해야 해요. Windows 로그인에서 계정을 다시 저장하세요.';

  @override
  String get someDeviceFeaturesNotReady => '바로 쓸 수 없는 장치 기능이 있어요. 장치에서 확인하세요.';

  @override
  String get viewDevices => '장치 보기';

  @override
  String get openAdministratorSettingsWindow => '관리자 설정 창을 열까요?';

  @override
  String get youCanChangeSettingsNew =>
      'Windows 관리자 승인을 받은 새 창에서 설정을 변경할 수 있어요.';

  @override
  String get openNewWindow => '새 창 열기';

  @override
  String get waitingWindowsAdministratorApproval => 'Windows 관리자 승인 대기 중';

  @override
  String get administratorSettingsWindowOpenContinue =>
      '관리자 설정 창을 열었어요. 새 창에서 계속하세요.';

  @override
  String get settingsAsAdministrator => '관리자 권한으로 설정';

  @override
  String get changingSettingsNeedsAdministratorRights =>
      '설정을 변경하려면 관리자 권한이 필요해요.';

  @override
  String get administratorRejectedRequestCheckServer =>
      '관리자가 요청을 거절했어요. 서버 주소를 확인한 뒤 다시 요청하세요.';

  @override
  String get connectionRequestExpiredCheckServer =>
      '연결 요청이 만료됐어요. 서버를 확인한 뒤 다시 요청하세요.';

  @override
  String get hostCopiedRegistrationRequestAgain =>
      '이 Windows는 등록된 다른 데스크톱을 복사한 것 같아요. 이 데스크톱의 연결을 새로 요청하세요.';

  @override
  String get connectionRequestCanceled => '연결 요청을 취소했어요.';

  @override
  String get checkServerAddress2 => '서버 주소 확인 필요';

  @override
  String get checkApprovalWaitTime => '승인 대기 시간 확인 필요';

  @override
  String get checkingServerRegistration => '서버 등록 확인 중';

  @override
  String get awaitingAdministratorApproval => '관리자 승인 대기';

  @override
  String get confirmationCode => '확인 코드';

  @override
  String get adminConsoleMustShowSame =>
      '관리자 웹에도 이 코드가 보여야 해요. 코드가 다르면 승인하면 안 돼요.';

  @override
  String get waitingServerConfirmCancellationAfter =>
      '서버의 취소 확인을 기다리고 있어요. 확인 후 다시 연결을 요청할 수 있어요.';

  @override
  String get adminConsoleGoDesktopsPending =>
      '관리자 웹 → 데스크톱의 승인 대기에서 확인 코드가 같은지 확인한 뒤 승인해 주세요.';

  @override
  String get adminConsoleApproveConnectionRequest =>
      '관리자 웹 → 데스크톱의 승인 대기에서 이 연결 요청을 승인해 주세요.';

  @override
  String get evenIfYouCloseWindow => '창을 닫아도 Windows 서비스가 연결 요청을 계속 확인해요.';

  @override
  String get startWindowsServiceKeepChecking =>
      '승인을 계속 확인하려면 Windows 서비스를 시작하세요.';

  @override
  String approvalValid(Object expiryText) {
    return '승인 유효 시간 · $expiryText';
  }

  @override
  String get couldNotCheckApprovalStatus => '승인 상태를 확인하지 못했어요. 잠시 후 다시 확인해요.';

  @override
  String get requestAlreadyApprovedCheckingServer =>
      '이미 승인된 요청이에요. 서버 등록 결과를 확인하고 있어요.';

  @override
  String get editServer => '서버 수정';

  @override
  String get serverConnection3 => '서버 연결 정보';

  @override
  String get connectServer => '서버에 연결';

  @override
  String get registrationKeptAfterUpdates => '등록 정보는 업데이트 후에도 유지돼요.';

  @override
  String get checkServerAddressRequestConnection =>
      '서버 주소를 확인하고 연결을 요청하면 관리자가 승인해요.';

  @override
  String get desktopId => '데스크톱 ID';

  @override
  String get connectionStatus => '연결 상태';

  @override
  String get changeServerReRegister => '서버 변경·다시 등록';

  @override
  String get connectionChecked => '연결 확인 완료';

  @override
  String get testServerConnection => '서버 연결 테스트';

  @override
  String get nowSendConnectionRequest => '이제 연결 요청을 보내세요.';

  @override
  String get firstFinishServerConnectionTest => '먼저 서버 연결 테스트를 완료하세요.';

  @override
  String text20(num monitorsLength) {
    return '가상 모니터 $monitorsLength대 켜짐';
  }

  @override
  String get switchStreamingScreen2 => '스트리밍 화면으로 전환';

  @override
  String monitor2(Object index) {
    return '모니터 $index';
  }

  @override
  String get noMultitapVirtualMonitor => '켜진 Multitap 가상 모니터가 없어요.';

  @override
  String get switchScreenAfterWindowsRestart => 'Windows 다시 시작 후 화면 전환';

  @override
  String get refreshVirtualDisplayStatus => '가상 화면 상태 새로고침';

  @override
  String get resetVirtualDisplayRecoveryRecord => '가상 화면 복구 기록 초기화';

  @override
  String get checkAfterWindowsRestart => 'Windows 다시 시작 후 확인';

  @override
  String get recoverVirtualDisplay => '가상 화면 복구';

  @override
  String get setUpVirtualDisplayFrom => 'Proxmox 콘솔에서 가상 화면 설정';

  @override
  String get setUpVirtualDisplay2 => '가상 화면 설정';

  @override
  String get checkExistingDevicesLeftoverSettings => '기존 장치·남은 설정 확인';

  @override
  String get onceDisplayReadyResolutionFollows =>
      '화면이 준비되면 클라이언트 창 크기에 맞춰 해상도를 조절해요.';

  @override
  String get setNumberMonitors12 =>
      '모니터 수(1~2대)와 해상도는 연결한 클라이언트에서 조절해요.\n스트리밍에는 Multitap 가상 화면만 사용해요.';

  @override
  String get conflictsFound => '확인된 충돌 항목';

  @override
  String devicesLeftoverSettingsNothingDeleted(
    Object devices,
    Object configurations,
  ) {
    return '장치 $devices개 · 남아 있는 설정 $configurations개. 아직 삭제하지 않았어요.';
  }

  @override
  String get noConflictsCleanUp => '정리할 충돌 항목이 없어요.';

  @override
  String get virtualDisplayDeviceRequired => '가상 화면 장치 · 필수';

  @override
  String get leftoverSettingsRequired => '남아 있는 설정 · 필수';

  @override
  String impact2(Object items) {
    return '영향: $items';
  }

  @override
  String get requiredItemsAboveCleanedUp =>
      '위 필수 항목을 함께 정리해요. Windows 드라이버 보관함과 기존 설정 파일은 유지해요.';

  @override
  String get cleanUpCheckedConflictsInstall2 => '확인한 충돌 항목 정리·설치';

  @override
  String get couldNotVerifyDriverFiles2 =>
      '드라이버 파일을 확인하지 못했어요. Host 설치 파일을 다시 받아 실행한 뒤 확인하세요.';

  @override
  String get couldNotSafelyVerifyVirtual =>
      '가상 화면 구성 파일을 안전하게 확인하지 못했어요. Host 설치 파일을 다시 받아 실행한 뒤 확인하세요.';

  @override
  String get restartWindowsThenCheckConflict =>
      'Windows를 다시 시작한 뒤 충돌 목록을 새로 확인하세요.';

  @override
  String get signWindowsFromProxmoxConsole2 =>
      'Proxmox 콘솔(로컬 PC는 본체 화면)에서 Windows에 로그인한 뒤 다시 확인하세요.';

  @override
  String get firstRecoverPreviousScreenChange =>
      '이전 화면 변경을 먼저 복구한 뒤 충돌 목록을 새로 확인하세요.';

  @override
  String get automaticCleanupCannotProceedCopy =>
      '자동 정리를 진행할 수 없어요. 진단 정보를 복사해 관리자에게 알려 주세요.';

  @override
  String get lastCleanupInstallResult => '직전 정리·설치 결과';

  @override
  String get conflictCleanup => '충돌 항목 정리';

  @override
  String get devicesRequestedRemoval => '제거 요청한 장치';

  @override
  String text21(Object result) {
    return '$result개';
  }

  @override
  String get devicesConfirmedRemoved => '제거 확인된 장치';

  @override
  String get driverInstall => '드라이버 설치';

  @override
  String get notCompleted => '완료하지 못함';

  @override
  String get previousSettingsRestored => '이전 설정을 복원했어요.';

  @override
  String get someItemsRemainCheckConflict =>
      '일부 항목이 남아 있어요. 다시 시도하기 전에 충돌 목록을 다시 확인하세요.';

  @override
  String get deviceRemovalNotConfirmedYet => '장치 제거 여부를 아직 확인하지 못했어요.';

  @override
  String get windowsNeedsRestartDoesNot =>
      'Windows를 다시 시작해야 해요. 자동으로 다시 시작하지 않아요.';

  @override
  String get offTurnsWhenYouSave => '꺼짐 · 계정을 저장하면 켜져요';

  @override
  String get setAppliesFromNextWindows => '설정됨 · 다음 Windows 시작부터 적용';

  @override
  String get windowsSignSetAccountConfirmed => '설정한 계정의 Windows 로그인을 확인했어요';

  @override
  String get accountCheckNeededSaveAccount => '계정 확인 필요 · 계정 정보를 다시 저장하세요';

  @override
  String get windowsHasExistingAutoSign => 'Windows에 기존 자동 로그인 설정이 있어요';

  @override
  String get windowsPolicyPreventsTurning => 'Windows 정책으로 켤 수 없어요';

  @override
  String get autoSignOffRecoveryNeeded => '자동 로그인 꺼짐 · 복구가 필요해요';

  @override
  String get checkChangeResultRecoveryStatus => '변경 결과와 복구 상태를 확인해야 해요';

  @override
  String get couldNotCheckCurrentSetting => '현재 설정을 확인하지 못했어요';

  @override
  String get hostDoesNotSupportAuto2 => '이 Host는 자동 로그인 설정을 지원하지 않아요';

  @override
  String get windowsAccountPasswordWrongNot =>
      'Windows 계정 또는 비밀번호가 맞지 않아요. 자동으로 다시 시도하지 않아요.';

  @override
  String get enterAccountPasswordPasswordConfirmation =>
      '계정과 비밀번호, 비밀번호 확인 칸을 다시 입력하세요. 빈 비밀번호와 PIN은 저장하지 않아요.';

  @override
  String get onlyLocalWindowsAccountsPassword =>
      '비밀번호가 있는 로컬 Windows 계정만 지원해요.';

  @override
  String get accountChangedRestrictedCheckAccount =>
      '계정이 변경됐거나 사용이 제한돼 있어요. Windows에서 계정 상태를 확인하세요.';

  @override
  String get windowsPolicyBlocksAutoSign =>
      'Windows 정책이 자동 로그인을 막고 있어요. 정책은 바꾸지 않아요.';

  @override
  String get windowsSettingsChangedDuringCheck =>
      '확인 중 Windows 설정이 바뀌었어요. 현재 상태를 다시 확인하세요.';

  @override
  String get settingsChangedDuringCheckRefresh =>
      '확인 중 설정이 바뀌었어요. 새로고침 후 변경 내용을 다시 확인하세요.';

  @override
  String get anotherSettingsTaskRunningCheck =>
      '다른 설정 작업이 진행 중이에요. 잠시 후 결과를 확인하세요.';

  @override
  String get autoSignChangeStoppedMidway =>
      '자동 로그인 변경이 중간에 멈췄어요. 아래에서 Multitap이 바꾼 설정을 복구하세요.';

  @override
  String get couldNotConfirmResultCheck =>
      '설정 결과를 확인하지 못했어요. 다시 적용하기 전에 결과를 확인하세요.';

  @override
  String get recoverSettingsMultitapChanged => 'Multitap이 바꾼 설정을 복구할까요?';

  @override
  String get onlyAutoSignSettingsMultitap =>
      'Multitap이 바꾼 자동 로그인 설정만 원래 값으로 복구해요.\n다른 곳에서 바뀐 값은 덮어쓰지 않아요.\n다시 시작·로그아웃·잠금 해제는 하지 않아요.';

  @override
  String get recoverSettings => '설정 복구';

  @override
  String get settingUpWindowsAutoSign => 'Windows 자동 로그인 설정 중';

  @override
  String get checkingCurrentSettingsAccountPermissions =>
      '현재 설정과 계정 권한을 확인하고 있어요.';

  @override
  String get couldNotPrepareWindowProtection =>
      '작업 중 창 보호를 준비하지 못했어요. 설정은 변경하지 않았어요.';

  @override
  String get currentSettingsPermissionsChangedRefresh =>
      '현재 설정 또는 권한이 바뀌었어요. 새로고침 후 다시 확인하세요.';

  @override
  String get couldNotVerifyWindowProtection =>
      '작업 중 창 보호를 확인하지 못했어요. 설정은 변경하지 않았어요.';

  @override
  String get couldNotChangeSettingsRefresh => '설정을 변경하지 못했어요. 상태를 새로고침해 확인하세요.';

  @override
  String get requestResultNotConfirmedYet =>
      '요청 결과를 아직 확인하지 못했어요. 다시 적용하지 말고 결과를 확인하세요.';

  @override
  String get changeStoppedMidwayRecoverSettings =>
      '변경이 중간에 멈췄어요. Windows 로그인에서 Multitap이 바꾼 설정을 복구하세요.';

  @override
  String get couldNotConfirmWhetherRequest =>
      '이 요청의 완료 여부를 확인하지 못했어요. 결과 확인만 다시 시도하세요.';

  @override
  String get turnedAppliesFromNextWindows => '켰어요. 다음 Windows 시작부터 적용돼요.';

  @override
  String get settingsMultitapChangedRecoveredAuto =>
      'Multitap이 바꾼 설정을 복구했어요. 자동 로그인은 꺼져 있어요. 계정을 다시 저장하면 켜져요.';

  @override
  String get requestFinishedButCurrentSetting =>
      '요청은 끝났지만 현재 설정을 확인하지 못했어요. Windows 로그인에서 상태를 확인하세요.';

  @override
  String get resultNotConfirmedYetSame =>
      '아직 결과를 확인하지 못했어요. 같은 요청을 다시 적용하지 않아요.';

  @override
  String get whenYouSaveAccountHost => '계정을 저장하면 이 Host가 기존 설정을 넘겨받아 켜요.';

  @override
  String get hostVersionCannotTakeOver => '이 Host 버전은 기존 설정을 넘겨받을 수 없어요.';

  @override
  String get onceWindowsSignsAccountWhen => '켜지면 Windows가 시작될 때 이 계정으로 로그인해요.';

  @override
  String get windowsSignsAccountAutomaticallyWhen =>
      'Windows가 시작될 때 이 계정으로 자동 로그인해요.';

  @override
  String get refreshCheckCurrentSetting => '새로고침으로 현재 설정을 확인하세요.';

  @override
  String get windowsAccount3 => 'Windows 계정';

  @override
  String get streamingAutoSignUseAccount =>
      '스트리밍과 자동 로그인에 이 계정을 써요.\nMultitap 로그인 계정과는 별도예요.';

  @override
  String get savedAccount => '저장된 계정';

  @override
  String get autoSignAccount => '자동 로그인 계정';

  @override
  String get windowsSetting => 'Windows 설정';

  @override
  String get confirmed => '켜짐 (확인됨)';

  @override
  String get offConfirmed => '꺼짐 (확인됨)';

  @override
  String get autoSignAccountDiffersFrom2 =>
      '자동 로그인 계정과 저장된 계정이 달라요. 계정을 다시 저장하세요.';

  @override
  String get updateHostThenCheckAgain => 'Host 업데이트 후 다시 확인하세요.';

  @override
  String get savingAlsoSwitchesAutoSign => '저장하면 자동 로그인도 이 계정으로 바뀌어요.';

  @override
  String get enterPasswordOnceMoreTurn => '비밀번호를 한 번 더 입력하면 자동 로그인이 켜져요.';

  @override
  String get turnAutoSign => '자동 로그인 켜기';

  @override
  String get changeAccount => '계정 변경';

  @override
  String get clearSavedSignInfo => '저장 로그인 정보 지우기';

  @override
  String get unclearWhetherAutoSignFinished =>
      '자동 로그인 완료 여부가 불확실해요. 같은 변경을 다시 실행하지 않고 결과만 확인해요.';

  @override
  String get checkRequestResult => '요청 결과 확인';

  @override
  String get recoverSettingsMultitapChanged2 => 'Multitap이 바꾼 설정 복구';

  @override
  String get savedWindowsAccountNeedsChecking =>
      '저장된 Windows 계정을 확인해야 해요. 계정 정보를 다시 확인하고 저장하세요.';

  @override
  String get currentWindowsSign => '현재 Windows 로그인';

  @override
  String get signedNotLocked => '로그인돼 있고 잠겨 있지 않아요';

  @override
  String get checkSignLockState => '로그인 또는 잠금 상태를 확인하세요';

  @override
  String get signChecked => '로그인 확인 시각';

  @override
  String get ifWindowsSignsOutLocks =>
      'Windows에서 로그아웃되거나 잠기면 Console(로컬 PC는 본체 화면)에서 로그인하세요.\n다시 시작하면 자동으로 로그인돼요.';

  @override
  String get localUserNameComputerName =>
      '로컬 아이디 또는 컴퓨터 이름\\아이디 · PIN이 아닌 계정 비밀번호';

  @override
  String get createNewPassword2 => '새 비밀번호 만들기';

  @override
  String leastCharacters(Object minimumLength) {
    return '$minimumLength자 이상, ';
  }

  @override
  String accountHasNoWindowsPassword(Object setup) {
    return '이 계정은 Windows 비밀번호가 없어요.\n${setup}Windows 비밀번호 규칙에 맞게 만드세요.';
  }

  @override
  String get newWindowsPassword => '새 Windows 비밀번호';

  @override
  String get windowsPassword => 'Windows 비밀번호';

  @override
  String get leaveBlankSaveIfThere => '비밀번호가 없으면 비워 두고 저장하세요.';

  @override
  String get confirmPassword => '비밀번호 확인';

  @override
  String get createPasswordSave => '비밀번호 만들고 저장';

  @override
  String get saveTurnAutoSign => '저장하고 자동 로그인 켜기';

  @override
  String get checkSaveAccount => '계정 확인·저장';

  @override
  String get discardWindowsAccountInfoYou => '입력한 Windows 계정 정보를 버릴까요?';

  @override
  String get savedWindowsAccountKept => '저장된 Windows 계정은 유지해요.';

  @override
  String get serviceStopping => '서비스 중지 중';

  @override
  String get servicePaused => '서비스 일시 중지';

  @override
  String get serviceChangingState => '서비스 상태 전환 중';

  @override
  String get serviceRunning => '서비스 실행 중';

  @override
  String get serviceNotInstalled => '서비스 미설치';

  @override
  String get serviceStopped => '서비스 중지';

  @override
  String get serviceQueryNeeded => '서비스 조회 필요';

  @override
  String get serviceStartNeeded => '서비스 시작 필요';

  @override
  String get checkServiceStatus => '서비스 상태 확인 필요';

  @override
  String get connectionStatusDelayed => '연결 상태 조회 지연';

  @override
  String get serverConnectionConfirmed => '서버 연결 확인됨';

  @override
  String get connectingServer2 => '서버 연결 중';

  @override
  String get waitingServerResponse => '서버 응답 대기';

  @override
  String get reconnectingServer => '서버 다시 연결 중';

  @override
  String get checkServerResponse => '서버 응답 확인 필요';

  @override
  String get checkServerRegistration => '서버 등록 확인 필요';

  @override
  String get checkServerConnection => '서버 연결 확인 필요';

  @override
  String get runHostInstallerAgainInstall2 =>
      'Host 설치 파일을 다시 실행해 Windows 서비스를 설치하세요.';

  @override
  String get hostServiceOffStartService =>
      'Host 서비스가 꺼져 있어요. 서비스를 시작하면 서버에 자동으로 연결해요.';

  @override
  String get couldNotReadServiceStatus => '서비스 상태를 읽지 못했어요. 상태를 새로고침하세요.';

  @override
  String get couldNotReadLatestStatus => '최근 상태를 읽지 못했어요. 잠시 후 다시 확인해요.';

  @override
  String get connectedServerYouCanConnect =>
      '서버에 연결돼 있어요. 클라이언트에서 이 데스크톱에 연결할 수 있어요.';

  @override
  String get connectedServerCheckVirtualDisplay =>
      '서버에 연결돼 있어요. 가상 화면 상태를 확인하세요.';

  @override
  String get serverCouldNotConfirmHosts =>
      '서버가 이 Host의 등록을 확인하지 못했어요. 서버 연결에서 연결을 다시 요청하세요.';

  @override
  String get couldNotVerifyServerCertificate =>
      '서버 인증서를 확인하지 못했어요. HTTPS 주소와 인증서를 확인하세요.';

  @override
  String get couldNotConnectServerCheck =>
      '서버에 연결하지 못했어요. Windows의 네트워크와 서버 주소를 확인하세요.';

  @override
  String get serverNotRespondingTellAdministrator =>
      '서버가 응답하지 않아요. 관리자에게 알려 주세요.';

  @override
  String get hostServerVersionsDoNot => 'Host와 서버 버전이 서로 맞지 않아요. 둘 다 업데이트하세요.';

  @override
  String get couldNotConfirmRecentServer =>
      '최근 서버 응답을 확인하지 못했어요. 서비스가 자동으로 다시 연결해요.';

  @override
  String get serverConnectionDroppedServiceReconnects =>
      '서버 연결이 끊겼어요. 서비스가 자동으로 다시 연결해요.';

  @override
  String get serviceConnectingServerPleaseWait =>
      '서비스가 서버에 연결하고 있어요. 잠시 기다려 주세요.';

  @override
  String get connectedServerWaitingResponse => '서버에 연결했고 응답을 기다리고 있어요.';

  @override
  String get couldNotConfirmRecentServer2 =>
      '최근 서버 응답을 확인하지 못했어요. 계속되면 진단 보고서에서 서비스를 다시 시작하세요.';

  @override
  String get couldNotCheckServerConnection =>
      '서버 연결을 확인하지 못했어요. 계속되면 진단 보고서에서 서비스를 다시 시작하세요.';

  @override
  String get noConfirmedResponse => '확인된 응답 없음';

  @override
  String sAgo(Object age) {
    return '$age초 전';
  }

  @override
  String get diagnosticsCopied => '진단 정보를 복사했어요.';

  @override
  String get couldNotCopyDiagnosticsTry => '진단 정보를 복사하지 못했어요. 다시 시도하세요.';

  @override
  String get queryNeeded => '조회 필요';

  @override
  String get restartService => '서비스 다시 시작';

  @override
  String get summary => '요약';

  @override
  String get diagnosticsSentServerAutomaticallyPasswords =>
      '진단 정보는 서버로 자동으로 보내요.\n비밀번호·인증 정보·클립보드 내용은 보내지 않아요.';

  @override
  String get copyDiagnostics => '진단 정보 복사';

  @override
  String get hostVersion2 => 'Host 버전';

  @override
  String get service => '서비스';

  @override
  String get serverRegistrationNeeded => '서버 등록 필요';

  @override
  String get signed => '로그인됨';

  @override
  String get checkSign => '로그인 확인 필요';

  @override
  String ready5(num outputs) {
    return '준비됨 · 모니터 $outputs대';
  }

  @override
  String checkNeeded(num outputs) {
    return '확인 필요 · 모니터 $outputs대';
  }

  @override
  String get serviceQueryError2 => '서비스 조회 오류';

  @override
  String windowsError(Object s) {
    return 'Windows 오류 $s';
  }

  @override
  String get serviceInstall2 => '서비스 설치';

  @override
  String get sinceServerResponse => '서버 응답 경과';

  @override
  String get lastConnected => '마지막 연결 시각';

  @override
  String get lastResponse => '마지막 응답 시각';

  @override
  String get connectionGuidance => '연결 안내';

  @override
  String get virtualDisplayDriver2 => '가상 화면 드라이버';

  @override
  String get virtualDisplayDevice2 => '가상 화면 장치';

  @override
  String get virtualMonitors => '켜진 가상 모니터';

  @override
  String text22(Object outputs) {
    return '$outputs대';
  }

  @override
  String get screenRecovery => '화면 복구';

  @override
  String get windowsRestart2 => 'Windows 다시 시작';

  @override
  String get couldNotReadDeviceStatus => '장치 상태를 읽지 못했어요.\n상태를 새로고침하세요.';

  @override
  String get permissionsTurnedPerUserDesktop =>
      '사용 권한은 관리자 웹에서 사용자·데스크톱마다 켜요. 이 화면은 이 데스크톱이 준비됐는지만 보여 줘요.\n스마트 카드와 직렬 장치는 USB로 연결해요.\n마이크와 카메라는 연결할 수 없어요.';

  @override
  String get installWinfsp3 => 'WinFsp를 설치할까요?';

  @override
  String get installsSharedFolderDriverWinfsp =>
      '공유 폴더용 드라이버(WinFsp)를 설치해요.\nWindows를 다시 시작해야 하면 알려 드려요. 자동으로 다시 시작하지 않아요.';

  @override
  String get switchUsbConnectionMultitap => 'USB 연결 기능을 Multitap용으로 바꿀까요?';

  @override
  String get usbConnectionFromOtherPrograms =>
      '다른 프로그램의 USB 연결 기능은 더 이상 동작하지 않아요.\n연결된 USB 장치도 끊어져요.\nWindows를 다시 시작해야 하면 알려 드려요. 자동으로 다시 시작하지 않아요.';

  @override
  String get switchLabel => '바꾸기';

  @override
  String get switchingUsbConnection => 'USB 연결 기능 바꾸는 중';

  @override
  String get switchingUsbConnectionMultitapDo =>
      'USB 연결 기능을 Multitap용으로 바꾸고 있어요. 창을 닫지 마세요.';

  @override
  String get startingPrintSpooler => '인쇄 스풀러 시작 중';

  @override
  String get startingWindowsPrintSpooler => 'Windows 인쇄 스풀러를 시작하고 있어요.';

  @override
  String get openSettingsAsAdministrator => '관리자 권한으로 설정을 열어야 해요.';

  @override
  String get cleanUp => '정리';

  @override
  String remove(num extrasLength) {
    return '남은 가상 화면 장치 $extrasLength개를 지울까요?';
  }

  @override
  String get devicesWithoutMultitapRecordMay =>
      '\nMultitap 기록이 없는 장치는 다른 프로그램이 쓰고 있을 수 있어요. 그 프로그램의 가상 화면이 사라질 수 있어요.';

  @override
  String adapterUseDevicesFromOther(Object list, Object adapters) {
    return '지금 쓰는 어댑터와 다른 회사 장치는 그대로 둬요.\n$list$adapters\n화면이 잠깐 깜빡일 수 있어요. 자동으로 다시 시작하지 않아요.';
  }

  @override
  String get cleaningUpVirtualDisplayDevices => '가상 화면 장치 정리 중';

  @override
  String get removingOnlyCheckedDevicesOne => '확인한 장치만 하나씩 지우고 있어요.';

  @override
  String get allow => '허용';

  @override
  String get checkResult2 => '결과 확인 필요';

  @override
  String get waitExpired => '대기 만료';

  @override
  String get progress2 => '진행 중';

  @override
  String get installerCouldNotFinishUpdate =>
      '설치 프로그램이 업데이트를 완료하지 못했어요.\n이 오류 코드만으로는 자세한 원인을 알 수 없어요.';

  @override
  String get hostUpdateDiagnostics => 'Host 업데이트 진단';

  @override
  String get currentVersion => '현재 버전';

  @override
  String get unknown4 => '확인 안 됨';

  @override
  String get requestedVersion => '요청 버전';

  @override
  String get result => '결과';

  @override
  String get errorCode2 => '오류 코드';

  @override
  String get checked2 => '확인 시각';

  @override
  String get localPc3 => '로컬 PC의';

  @override
  String get openDesktopConsole => 'Console로 데스크톱을 열고';

  @override
  String checkInstallStatusHostSettings(Object machine) {
    return '$machine Host 설정 → 진단에서 설치 상태를 확인하세요.\n관리자는 서버 활동 기록에서 같은 시각의 결과를 보세요.';
  }

  @override
  String get updateHost => 'Host를 업데이트할까요?';

  @override
  String current4(Object current) {
    return '현재 $current';
  }

  @override
  String newVersion5(Object latest) {
    return '새 버전 $latest';
  }

  @override
  String get earlierChanges => '지난 변경 내역';

  @override
  String get couldNotLoadChangeList =>
      '변경 내역을 불러오지 못했어요. 업데이트 파일은 서버의 배포 정보로 확인해요.';

  @override
  String hostDownloadsFromServerInstalls(Object kind) {
    return 'Host가 이 서버에서 받아 자동으로 설치해요.\n$kind 설정은 그대로이고 Windows는 다시 시작하지 않아요.';
  }

  @override
  String fileSizeSha256Checked(Object kind) {
    return '받은 파일의 크기와 SHA-256을 확인한 뒤 설치해요.\n드라이버처럼 $kind에서 직접 확인해야 하면 설치하지 않고 알려요.';
  }

  @override
  String someoneConnectedNowUpdateWhen(Object kind) {
    return '지금 이 $kind에 연결된 사용자가 있어요.\n‘끝나면 업데이트’는 연결이 끝난 뒤 시작해요.\n‘지금 업데이트’는 연결을 끊고 바로 시작해요.';
  }

  @override
  String get updateWhenDone => '끝나면 업데이트';

  @override
  String get updateNow => '지금 업데이트';

  @override
  String get category => '분류';

  @override
  String get noSavedScreen => '저장된 화면 없음';

  @override
  String select(Object machineName) {
    return '$machineName · 선택';
  }

  @override
  String get screenPreviewOff => '화면 미리보기 꺼짐';

  @override
  String get manageVm => 'VM 관리';

  @override
  String get manageVmHostUpdateAvailable => 'VM 관리 · Host 업데이트 있음';

  @override
  String get cards => '카드형';

  @override
  String get list => '리스트형';

  @override
  String get mini => '미니';

  @override
  String get explorer => '탐색기형';

  @override
  String get monitorWall => '모니터월';

  @override
  String get dock => '도킹형';

  @override
  String get recoveryCode => '복구 코드';

  @override
  String get n6DigitCodeFromAuthenticator => '인증 앱의 6자리 코드';

  @override
  String get checkingSetupStatus => '설정 상태를 확인하고 있어요.';

  @override
  String get couldNotCheckSetupStatus => '설정 상태를 확인하지 못했어요.';

  @override
  String get twoStepVerification2 => '2단계 인증을 사용하고 있어요.';

  @override
  String get twoStepVerificationOff => '2단계 인증을 사용하지 않고 있어요.';

  @override
  String text23(num recoveryCodesRemaining) {
    return '사용할 수 있는 복구 코드 $recoveryCodesRemaining개';
  }

  @override
  String get changingSignsYouOutAll2 => '설정을 변경하면 모든 기기에서 로그아웃돼요.';

  @override
  String get setUpAuthenticatorApp => '인증 앱 설정';

  @override
  String get makeNewRecoveryCodes => '복구 코드 다시 만들기';

  @override
  String get turnOffTwoStepVerification => '2단계 인증 끄기';

  @override
  String get n13VerifyItsYou => '1/3 · 본인 확인';

  @override
  String get verifyItsYou => '본인 확인';

  @override
  String get enterNextCodeFromAuthenticator =>
      '이미 사용한 코드 대신 인증 앱의 다음 코드를 입력하세요.';

  @override
  String get useAuthenticatorCode => '인증 앱 코드 사용';

  @override
  String get useRecoveryCode => '복구 코드 사용';

  @override
  String get makingNewCodesStopsOld => '새 코드를 만들면 기존 복구 코드는 더 이상 사용할 수 없어요.';

  @override
  String get whenChangeDoneYouSigned => '변경을 완료하면 모든 기기에서 로그아웃돼요.';

  @override
  String get n23LinkAuthenticatorApp => '2/3 · 인증 앱 연결';

  @override
  String get n33SaveRecoveryCodes => '3/3 · 복구 코드 보관';

  @override
  String get scanQrCodeAuthenticatorApp =>
      '다른 기기의 인증 앱으로 QR 코드를 스캔하거나 설정 키를 직접 입력하세요.';

  @override
  String get hideQrSetupKey => 'QR·설정 키 숨기기';

  @override
  String get showQrSetupKey => 'QR·설정 키 보기';

  @override
  String timeLeftSYouCan(Object remainingInSeconds) {
    return '남은 시간 $remainingInSeconds초\n이 키는 이 화면에서만 확인할 수 있어요.';
  }

  @override
  String get setupKeyDisplayTimeEnded => '설정 키 표시 시간이 끝나 키를 지웠어요.';

  @override
  String get waitingResultSetupRequestAlready =>
      '이미 보낸 설정 요청의 결과를 기다리고 있어요. 요청을 다시 보내지 않아요.';

  @override
  String get changeRequestsNotSentAgain => '변경 요청은 자동으로 다시 보내지 않아요.';

  @override
  String get signAgainThenCheckTwo => '다시 로그인한 뒤 2단계 인증 설정 상태를 확인하세요.';

  @override
  String get settingsChangedSignAgain => '설정을 변경했어요. 다시 로그인하세요.';

  @override
  String get signChangedSoSetupWas => '로그인 상태가 바뀌어 설정을 종료했어요.';

  @override
  String get finishSetup => '설정 완료';

  @override
  String get closeRecoveryCodes => '복구 코드를 닫을까요?';

  @override
  String get youCannotSeeTheseCodes =>
      '이 코드는 다시 볼 수 없어요. 아직 보관하지 않았다면 돌아가서 보관하세요.';

  @override
  String get goBack => '돌아가기';

  @override
  String get closeWithoutSaving2 => '보관하지 않고 닫기';

  @override
  String get twoStepVerificationOff2 => '2단계 인증을 껐어요';

  @override
  String get saveRecoveryCodes => '복구 코드를 보관하세요';

  @override
  String get youSignedOutAllDevices => '모든 기기에서 로그아웃했어요. 다시 로그인해야 해요.';

  @override
  String get eachCodeWorksOnlyOnce =>
      '각 코드는 한 번만 사용할 수 있어요. 이 화면을 닫으면 다시 볼 수 없어요.';

  @override
  String get iSavedRecoveryCodesSomewhere => '복구 코드를 따로 보관했어요';

  @override
  String get cannotShowQrCodeEnter => 'QR 코드를 표시할 수 없어요. 아래 설정 키를 직접 입력해 주세요.';

  @override
  String get setupQrCodeScanAuthenticator => '인증 앱으로 스캔할 설정 QR 코드';

  @override
  String get enterRecoveryCodeYouSaved => '보관한 복구 코드를 입력하세요.';

  @override
  String get enter6DigitCode => '6자리 인증 코드를 입력하세요.';

  @override
  String get couldNotConfirmResultTry => '인증 결과를 확인하지 못했어요. 다시 시도하세요.';

  @override
  String get couldNotConfirmResultStart => '인증 결과를 확인하지 못했어요.\n로그인부터 다시 시작하세요.';

  @override
  String get verificationTimeRanOutStart => '인증 시간이 끝났어요.\n로그인부터 다시 시작하세요.';

  @override
  String get enterRecoveryCodeEachOne => '복구 코드를 입력하세요.\n한 번만 쓸 수 있어요.';

  @override
  String get enter6DigitCodeFrom => '인증 앱의 6자리 코드를 입력하세요.';

  @override
  String timeLeft(Object seconds, Object seconds2) {
    return '남은 시간 $seconds:$seconds2';
  }

  @override
  String get authenticatorApp => '인증 앱';

  @override
  String get code => '인증 코드';

  @override
  String get n6Digits => '6자리 숫자';

  @override
  String get backSign => '로그인으로 돌아가기';

  @override
  String get verifySign => '인증하고 로그인';

  @override
  String get plannedMaintenance => '점검 예고';

  @override
  String plannedMaintenance2(Object when) {
    return '점검 예고 · $when';
  }

  @override
  String get notice2 => '공지';

  @override
  String text24(num waiting) {
    return '공지 $waiting개 더 있어요.';
  }

  @override
  String get closeNotice => '공지 닫기';

  @override
  String text25(num unread) {
    return '공지 $unread개';
  }

  @override
  String get deleteNotice => '이 공지를 지울까요?';

  @override
  String get deletedEveryDeviceSignedAccount => '이 계정으로 로그인한 모든 기기에서 지워져요.';

  @override
  String text26(num noticesLength) {
    return '공지 $noticesLength개를 모두 지울까요?';
  }

  @override
  String get deleteAll => '모두 지우기';

  @override
  String get noNotices => '받은 공지가 없어요.';

  @override
  String get couldNotStartLocalPc => '로컬 PC를 켜지 못했어요';

  @override
  String get collapse => '접기';

  @override
  String get whatNeedsStart => '켜려면 필요한 것';

  @override
  String get sendingPrinter => '프린터로 보내는 중…';

  @override
  String get saving => '저장하는 중…';

  @override
  String printingPage(Object done, Object total) {
    return '인쇄 중 · 페이지 $done/$total';
  }

  @override
  String get preparingPrint => '인쇄 준비 중…';

  @override
  String text27(num pages) {
    return '$pages페이지';
  }

  @override
  String get documentSentFromDesktop => '데스크톱에서 보낸 문서';

  @override
  String moreWaiting(Object waiting) {
    return ' · 대기 $waiting개 더';
  }

  @override
  String get paperColorTwoSidedPrinting => '용지·색상·양면은 프린터의 기본 설정을 따라요.';

  @override
  String get noPrinterInstalledPcYou => 'PC에 설치된 프린터가 없어요. PDF로 저장할 수 있어요.';

  @override
  String defaultLabel2(Object printerName) {
    return '$printerName (기본)';
  }

  @override
  String get copies => '매수';

  @override
  String get oneFewerCopy => '한 장 줄이기';

  @override
  String get n1CopyMinimum => '1장이 가장 적어요';

  @override
  String get oneMoreCopy => '한 장 늘리기';

  @override
  String youCanPrintUpCopies(Object maximumCopies) {
    return '$maximumCopies장까지 한 번에 인쇄할 수 있어요';
  }

  @override
  String get saveAsPdf => 'PDF로 저장';

  @override
  String get noPrinterPrint => '인쇄할 프린터가 없어요';

  @override
  String get print => '인쇄';

  @override
  String get previousPage => '이전 페이지';

  @override
  String get nextPage => '다음 페이지';

  @override
  String pagePreview(Object preview) {
    return '$preview페이지 미리보기';
  }

  @override
  String get eachPageKeepsItsOriginal => '페이지마다 원본 방향을 유지해요.';

  @override
  String get couldNotOpenPreview => '미리보기를 열지 못했어요.';

  @override
  String multitapLog(Object loadHost) {
    return 'Multitap $loadHost 로그';
  }

  @override
  String get recentEntriesPasswordsTokensClipboard =>
      '최근 기록 · 비밀번호/토큰/클립보드 내용 제외';

  @override
  String get couldNotReadHostLog => 'Host 기록을 읽지 못했어요. 앱 기록만 표시해요.';

  @override
  String get logFileCannotWrittenSo => '기록 파일을 쓸 수 없어 이번 실행의 기록만 표시해요.';

  @override
  String multitap(Object loadHost, num entriesLength) {
    return 'Multitap $loadHost · 최근 기록 $entriesLength개';
  }

  @override
  String needsAttention2(Object attention) {
    return '확인 필요 $attention';
  }

  @override
  String get loading2 => '불러오는 중';

  @override
  String get copied => '복사됨';

  @override
  String get copyAgain => '복사 다시 시도';

  @override
  String get noEntriesShow => '표시할 기록이 없어요.';

  @override
  String get appStarted => '앱 시작';

  @override
  String get desktopListLoaded => '데스크톱 목록 조회';

  @override
  String get streamingConnectionStarted => '스트리밍 연결 시작';

  @override
  String get streamingReadyReply => '스트리밍 준비 응답';

  @override
  String get streamingEnded => '스트리밍 종료';

  @override
  String get resolutionChangeRequested => '해상도 변경 요청';

  @override
  String get resolutionChangeResult => '해상도 적용 결과';

  @override
  String get updateCheckResult => '업데이트 조회 결과';

  @override
  String get updateDownload => '업데이트 다운로드';

  @override
  String get hostInstallStarted => 'Host 설치 시작';

  @override
  String get installStep => '설치 단계';

  @override
  String get installResult => '설치 결과';

  @override
  String get virtualDisplayStatus => '가상 화면 상태';

  @override
  String get existingDriverKept => '기존 드라이버 유지';

  @override
  String get virtualDisplayDriverInstall => '가상 화면 드라이버 설치';

  @override
  String get virtualDisplayTurned => '가상 화면 활성화';

  @override
  String get resolutionListChecked => '해상도 목록 확인';

  @override
  String get serviceStatus => '서비스 상태';

  @override
  String get serverResponseStatus => '서버 응답 상태';

  @override
  String get serverAddressSave => '서버 주소 저장';

  @override
  String get systemErrorCode => '시스템 오류 코드';

  @override
  String get hostSettingsAction => 'Host 설정 작업';

  @override
  String get updateInstall => '업데이트 설치';

  @override
  String get clipboard => '클립보드';

  @override
  String get driveSharing => '드라이브 공유';

  @override
  String get androidPrinting => 'Android 인쇄';

  @override
  String get androidPrintingDocumentRequested => 'Android 인쇄 · 문서 요청';

  @override
  String get androidPrintingRequestReceived => 'Android 인쇄 · 요청 받음';

  @override
  String get androidPrintingDocumentChecked => 'Android 인쇄 · 문서 확인';

  @override
  String get androidPrintingSystemPrintRequested => 'Android 인쇄 · 시스템 인쇄 요청';

  @override
  String get androidPrintingPrintServiceConnected => 'Android 인쇄 · 인쇄 서비스 연결';

  @override
  String get androidPrintingLayoutChecked => 'Android 인쇄 · 레이아웃 확인';

  @override
  String get androidPrintingDocumentTransferStarted => 'Android 인쇄 · 문서 전달 시작';

  @override
  String get androidPrintingDocumentTransferFinished => 'Android 인쇄 · 문서 전달 완료';

  @override
  String get androidPrintingPrintScreenClosed => 'Android 인쇄 · 인쇄 화면 종료';

  @override
  String get androidPrintingSystemErrorSeen => 'Android 인쇄 · 시스템 오류 관측';

  @override
  String get androidPrintingJobAccepted => 'Android 인쇄 · 접수 확인';

  @override
  String get androidPrintingCanceled => 'Android 인쇄 · 취소';

  @override
  String get androidPrintingRequestFailed => 'Android 인쇄 · 요청 실패';

  @override
  String get updateCheckFailedCheckRunning => '업데이트 확인 실패 · 실행 버전 확인 필요';

  @override
  String get updateCheckFailedUpdateList => '업데이트 확인 실패 · 업데이트 목록 형식 오류';

  @override
  String get updateCheckFailedCouldNot => '업데이트 확인 실패 · 서버 목록을 불러오지 못함';

  @override
  String get updateCheckFailed => '업데이트 확인 실패';

  @override
  String get updateCheckNewVersionAvailable => '업데이트 확인 · 새 버전 있음';

  @override
  String updateCheckNewVersionFound(Object latest) {
    return '업데이트 확인 · 새 버전 $latest 발견';
  }

  @override
  String get updateCheckNoPublishedVersion => '업데이트 확인 · 게시된 버전 없음';

  @override
  String get updateCheckUpDate => '업데이트 확인 · 최신 버전';

  @override
  String updateCheckUpDate2(Object current) {
    return '업데이트 확인 · 최신 버전 ($current)';
  }

  @override
  String downloadFailed(Object name) {
    return '$name 다운로드 실패';
  }

  @override
  String downloadFinished(Object name) {
    return '$name 다운로드 완료';
  }

  @override
  String installFailed(Object step) {
    return '$step 설치 실패';
  }

  @override
  String installerStarted(Object step) {
    return '$step 설치 프로그램 실행';
  }

  @override
  String restartingAfterUpdate(Object step) {
    return '$step 업데이트 후 다시 시작';
  }

  @override
  String get clipboardConnectedCopyPasteShared => '클립보드 연결됨 · 복사·붙여넣기 공유';

  @override
  String clipboardTransferFailed(Object direction) {
    return '클립보드 $direction 전송 실패';
  }

  @override
  String clipboardTransfer(Object direction) {
    return '클립보드 $direction 전송';
  }

  @override
  String get clipboardDisconnected => '클립보드 연결 끊김';

  @override
  String get clipboardConnectionEnded => '클립보드 연결 끝남';

  @override
  String get couldNotPrepareAutomaticClipboard => '클립보드 자동 공유 준비 실패';

  @override
  String get sharedFolderConnected => '공유 폴더 연결됨';

  @override
  String get sharedFolderChanged => '공유 폴더 변경';

  @override
  String get desktopDriveConnectionFailed => '데스크톱 드라이브 연결 실패';

  @override
  String get desktopDriveConnected => '데스크톱 드라이브 연결됨';

  @override
  String get sharedFolderDisconnected => '공유 폴더 연결 끊김';

  @override
  String get sharedFolderConnectionEnded => '공유 폴더 연결 끝남';

  @override
  String get usbConnectionReady => 'USB 연결 준비됨';

  @override
  String get usbDeviceConnectionFailed => 'USB 장치 연결 실패';

  @override
  String get usbDeviceConnected => 'USB 장치 연결됨';

  @override
  String get usbDeviceRemoved => 'USB 장치 분리';

  @override
  String get desktopDidNotAcceptUsb => 'USB 장치를 데스크톱이 받지 않음';

  @override
  String get usbDisconnected => 'USB 연결 끊김';

  @override
  String get usbConnectionEnded => 'USB 연결 끝남';

  @override
  String get usbCaptureDriverInstallFailed => 'USB 캡처 드라이버 설치 실패';

  @override
  String get notInstalled2 => '설치 안 됨';

  @override
  String get deviceNotStarted => '장치 시작 안 됨';

  @override
  String get notRunning => '실행 안 됨';

  @override
  String get noResponse => '응답 없음';

  @override
  String get noChange => '변경 없음';

  @override
  String get consoleSessionOff => '콘솔 꺼짐';

  @override
  String resolution2(Object width, Object height) {
    return '해상도 $width×$height';
  }

  @override
  String tookMs(Object value) {
    return '소요 $value ms';
  }

  @override
  String get succeeded => '성공';

  @override
  String get step => '단계';

  @override
  String get deviceStarted => '장치 시작';

  @override
  String get activeDisplay => '활성 화면';

  @override
  String get ownDevices => '소유 장치';

  @override
  String get waitingSetup => '설정 대기';

  @override
  String get rdpSession => 'RDP 세션';

  @override
  String get monitors => '모니터 수';

  @override
  String get responded => '응답 확인';

  @override
  String get changed => '변경됨';

  @override
  String get deviceRestartTried => '장치 다시 시작 시도';

  @override
  String get restore2 => '복원 대상';

  @override
  String get notRestored => '미복원';

  @override
  String get notRestoredVirtualMonitor => '미복원(가상 모니터)';

  @override
  String get backConsoleSession => '콘솔 복귀';

  @override
  String get consoleSessionNeedsRestoring => '콘솔 복원 필요';

  @override
  String get consoleSessionActive => '콘솔 활성';

  @override
  String get adapterBeforeRestart => '다시 시작 전 어댑터';

  @override
  String get adapterReconnected => '어댑터 다시 연결';

  @override
  String get skippedPackets => '건너뛴 패킷';

  @override
  String get newVersionAvailable2 => '새 버전 있음';

  @override
  String get desktopDrive => '데스크톱 드라이브';

  @override
  String get fileRequests => '파일 요청';

  @override
  String get failedRequests => '실패한 요청';

  @override
  String get rememberedFolders => '기억한 폴더';

  @override
  String get reopenedFolders => '다시 연 폴더';

  @override
  String get connectedDevices => '연결된 장치';

  @override
  String get handledRequests => '처리한 요청';

  @override
  String get kibTransferred => '주고받은 KiB';

  @override
  String get deviceType => '장치 종류';

  @override
  String get disconnectUpdate => '연결을 끊고 업데이트할까요?';

  @override
  String get allOpenConnectionsClosedDesktops =>
      '열린 연결이 모두 끊겨요.\n데스크톱과 실행 중인 작업은 그대로 유지돼요.';

  @override
  String get disconnectUpdate2 => '연결 끊고 업데이트';

  @override
  String get couldNotOpenHostUpdate => 'Host 업데이트를 열지 못했어요. 로그인과 서버 연결을 확인하세요.';

  @override
  String get download2 => '받기';

  @override
  String get installer => '설치 창';

  @override
  String get networkDroppedSoDownloadResumes => '네트워크가 끊겨 받은 부분부터 이어받고 있어요.';

  @override
  String get checkingDownloadedFilesSizeSha2 =>
      '받은 파일의 크기, SHA-256, 설치 파일 버전을 확인하고 있어요.';

  @override
  String get whenInstallerOpensFollowIts => '설치 창이 열리면 안내에 따라 설치하세요.';

  @override
  String get onceInstallerFinishesItsChecks =>
      '설치 프로그램이 확인을 마치면 Host 설정이 잠시 닫혔다가 새 버전으로 다시 열려요.';

  @override
  String get onceInstallerFinishesItsChecks2 =>
      '설치 프로그램이 확인을 마치면 앱이 잠시 닫혔다가 같은 자리에서 새 버전으로 다시 열려요.';

  @override
  String sizeMatchesBytes(Object size) {
    return '크기 $size바이트 일치';
  }

  @override
  String sha256Matches(Object digest) {
    return 'SHA-256 일치 · $digest';
  }

  @override
  String installerVersionConfirmed(Object version) {
    return '설치 파일 버전 $version 확인';
  }

  @override
  String get multitapReleaseSignatureConfirmed => 'Multitap 배포 서명 확인';

  @override
  String code3(Object code) {
    return '코드 $code';
  }

  @override
  String hostUpdatesAvailable(Object hostUpdates) {
    return 'Host 업데이트 $hostUpdates대 가능';
  }

  @override
  String get whenInstallerOpensInstallContinues =>
      '설치 프로그램을 열면 그 창에서 설치를 진행해요.';

  @override
  String get hostSettingsClosesConnectionsEnd =>
      'Host 설정이 닫히고 연결이 끊긴 뒤 Host 서비스가 다시 시작돼요.\nHost 등록·가상 화면 설정은 그대로예요.\nWindows는 자동으로 다시 시작하지 않아요.';

  @override
  String get appClosesReopensSamePlace =>
      '앱이 닫혔다가 같은 자리에서 새 버전으로 열려요.\n열린 연결은 끊겨요.\n서버 주소·로그인 설정은 그대로예요.';

  @override
  String get checkingUpdates => '업데이트 확인 중';

  @override
  String get youreUpDate => '최신 버전이에요';

  @override
  String get noInstallerPublished => '등록된 설치 파일이 없어요';

  @override
  String get newVersionAvailable3 => '새 버전이 있어요';

  @override
  String get downloadingUpdate => '업데이트 받는 중';

  @override
  String get checkingFile2 => '파일 확인 중';

  @override
  String get updateReady => '업데이트 준비 완료';

  @override
  String get openingInstaller => '설치 프로그램 여는 중';

  @override
  String get applyingUpdate => '업데이트 적용 중';

  @override
  String get installerOpened => '설치 프로그램을 열었어요';

  @override
  String current5(Object currentVersion) {
    return '현재 $currentVersion';
  }

  @override
  String get checkingReleaseInfoServer => '설정한 서버의 배포 정보를 확인하고 있어요.';

  @override
  String get youCanUpdateOnceAdministrator =>
      '관리자가 서버에 설치 파일을 올리면 업데이트할 수 있어요.';

  @override
  String get followStepsInstallerWindowYou =>
      '설치 창의 안내에 따라 설치하세요. 이 창은 닫아도 돼요.';

  @override
  String get downloadUpdate => '업데이트 받기';

  @override
  String get openInstaller => '설치 프로그램 열기';

  @override
  String get restartUpdate => '다시 시작해서 업데이트';

  @override
  String get continueInstaller => '설치 프로그램으로 계속';

  @override
  String get installInstaller => '설치 프로그램으로 직접 설치';

  @override
  String get minimize2 => '최소화';

  @override
  String unknown5(Object detail) {
    return '확인 안 됨 · $detail';
  }

  @override
  String get usageReportedByHost => '사용량 · Host 보고';

  @override
  String get usageReportedByProxmox => '사용량 · Proxmox 보고';

  @override
  String get checking4 => '점검 중…';

  @override
  String get couldNotCheck => '점검하지 못했어요';

  @override
  String get lastResult => '마지막 결과';

  @override
  String get checked3 => '점검 시각';

  @override
  String get statusServerGotFromHost => '서버가 Host와 Proxmox에서 받은 상태';

  @override
  String get checkAgain3 => '다시 점검';

  @override
  String get noServerAddressSetSo => '서버 주소가 설정되지 않아 변경 내역을 불러올 수 없어요.';

  @override
  String get couldNotLoadServersChange =>
      '이 서버의 변경 내역을 불러오지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get noReleaseDate => '배포일 정보 없음';

  @override
  String get noChangesListedVersion => '이 버전의 변경 내역이 등록되지 않았어요.';

  @override
  String text28(num total) {
    return '버전 $total개';
  }

  @override
  String get byVersion => '버전별';

  @override
  String get releaseHistoryShowAll => '전체 보기';

  @override
  String get seeVersionListEachVersions => '버전 목록과 그 버전의 내용을 나란히 봐요';

  @override
  String get seeChangesAllVersionsOne => '모든 버전의 내용을 한 번에 이어서 봐요';

  @override
  String get loadingChangeList => '변경 내역을 불러오고 있어요.';

  @override
  String get serverHasNoChangeList => '이 서버에 등록된 변경 내역이 없어요.';

  @override
  String get couldNotLoadEarlierVersions => '이전 버전을 불러오지 못했어요 · 다시 시도';

  @override
  String get youveReachedFirstVersion => '첫 버전까지 모두 봤어요';

  @override
  String noChangesListed(Object note) {
    return '$note · 내역 없음';
  }

  @override
  String get newerVersion => '더 최신 버전 (↑)';

  @override
  String get olderVersion => '이전 버전 (↓)';

  @override
  String get administratorApprovalNeeded2 => '관리자 허용이 필요해요';

  @override
  String get couldNotPrepareScreenFit => '창 크기에 맞는 화면을 준비하지 못했어요. 다시 연결하세요.';

  @override
  String get pressOkLockMouseInside =>
      '확인을 누르면 마우스를 데스크톱 안에 잠가요.\nCtrl+Shift+Alt+Z로 다시 끌 수 있어요.';

  @override
  String get mouseLockedInsideDesktopCtrl =>
      '마우스를 데스크톱 안에 잠갔어요 · Ctrl+Shift+Alt+Z로 켜기·끄기';

  @override
  String get couldNotApplyScreenMode => '화면 모드를 적용하지 못했어요. 현재 연결은 유지돼요.';

  @override
  String get couldNotConfirmOriginalScreen =>
      '원래 화면 모드로 돌아갔는지 확인하지 못했어요. 툴바의 화면에서 다시 선택하세요.';

  @override
  String get couldNotChangeScreenMode => '화면 모드를 변경하지 못했어요. 다시 선택하세요.';

  @override
  String get filesBeingCopiedCancelCopy => '파일 복사 중이에요. 복사를 취소한 뒤 연결을 끊으세요.';

  @override
  String desktopCannotPrintPc(Object reason) {
    return '이 데스크톱에서는 PC로 인쇄할 수 없어요. $reason';
  }

  @override
  String printerConnectionWasLostReconnect(num lostDocuments) {
    return '프린터 연결이 끊겼어요. 아직 인쇄하지 않은 문서 $lostDocuments개는 인쇄할 수 없어요. 데스크톱에 다시 연결한 뒤 다시 인쇄하세요.';
  }

  @override
  String get printerConnectionWasLostReconnect2 =>
      '프린터 연결이 끊겼어요. 데스크톱에 다시 연결하면 인쇄할 수 있어요.';

  @override
  String get printed => '인쇄했어요';

  @override
  String get savedAsPdf => 'PDF로 저장했어요.';

  @override
  String get printingCanceledPrintAgainFrom =>
      '인쇄를 취소했어요. 필요하면 데스크톱에서 다시 인쇄하세요.';

  @override
  String screenMode(Object shownLabel) {
    return '화면 모드 · $shownLabel';
  }

  @override
  String text29(num mappedFolders) {
    return '공유 폴더 $mappedFolders개 연결됨';
  }

  @override
  String text30(num sharedUsb) {
    return 'USB 장치 $sharedUsb개 연결됨';
  }

  @override
  String get foldersUsbUsePcsFolders => '폴더·USB · PC의 폴더와 USB 장치를 데스크톱에서 쓰기';

  @override
  String get sharedFoldersOpenPcFolders => '공유 폴더 · PC 폴더를 데스크톱에서 열기';

  @override
  String get usbDevicesUsePcsUsb => 'USB 장치 · PC의 USB 장치를 데스크톱에서 쓰기';

  @override
  String connected4(Object mappedFolders) {
    return '$mappedFolders개 연결됨';
  }

  @override
  String connected5(Object sharedUsb) {
    return '$sharedUsb개 연결됨';
  }

  @override
  String get settingsPictureQualityGuide => '설정 · 화질, 사용안내 등';

  @override
  String get youCanChangeWhenScreen => '화면 전환이 끝나면 바꿀 수 있어요';

  @override
  String get guideOpen => '사용안내가 열려 있어요';

  @override
  String get availableOnceConnected2 => '연결된 뒤 볼 수 있어요';

  @override
  String get filesBeingCopiedTryAgain => '파일 복사 중이에요. 끝난 뒤 다시 시도하세요.';

  @override
  String get couldNotReconnectUsbDevice =>
      'USB 장치를 다시 연결하지 못했어요. 이 연결에서는 USB를 쓸 수 없어요.';

  @override
  String get sharedFoldersEndedByReconnect =>
      '다시 연결하면서 폴더 공유를 끝냈어요. 폴더·USB에서 다시 추가하세요.';

  @override
  String get movedUsbDeviceAnotherWindow => 'USB 장치를 다른 창으로 옮겼어요';

  @override
  String movedUsbDeviceWindow(Object to) {
    return 'USB 장치를 $to 창으로 옮겼어요';
  }

  @override
  String get remoteResolutionPreparedAgainFit =>
      '창 크기에 맞춰 원격 해상도를 다시 준비해요.\n화면이 너무 크면 창 크기를 줄인 뒤 연결하세요.';

  @override
  String get reconnectMode => '이 모드로 다시 연결';

  @override
  String get takeOverWindowsScreen => 'Windows 화면을 넘겨받을까요?';

  @override
  String get currentWindowsScreenMovesStreaming =>
      '지금 Windows 화면을 스트리밍으로 가져와요.\nWindows 원격 데스크톱(RDP) 연결은 끊길 수 있어요. 실행 중인 작업은 그대로예요.';

  @override
  String get takeOverScreen => '화면 넘겨받기';

  @override
  String get connectionCanceled => '연결을 취소했어요.';

  @override
  String get couldNotConnectAllMonitors =>
      '모든 모니터로 연결하지 못했어요. 목록에서 창 모드나 전체 화면으로 연결하세요.';

  @override
  String get couldNotApplyScreenMode2 => '화면 모드를 적용하지 못했어요. 다시 연결하세요.';

  @override
  String get hostConnectionLostReconnecting => 'Host 연결이 끊겨 다시 연결하고 있어요';

  @override
  String get connectionLostReconnecting => '연결이 끊겨 다시 연결하고 있어요';

  @override
  String get couldNotChangeScreenSize => '화면 크기를 바꾸지 못해 다시 연결하고 있어요';

  @override
  String get monitorSetupChangedDisconnectThen =>
      '모니터 구성이 변경됐어요. 연결을 끊은 뒤 화면 모드를 다시 선택하세요.';

  @override
  String get cantShowScreenOnMonitors => '화면을 표시할 수 없어요';

  @override
  String text31(num activeCount) {
    return '모니터를 $activeCount대로 바꿨어요';
  }

  @override
  String get disconnected4 => '연결을 끊었어요';

  @override
  String get reconnectPickUpWhereYou => '다시 연결하면 이어서 쓸 수 있어요.';

  @override
  String get checkNumberMonitorsDesktopList => '데스크톱 목록에서 모니터 수를 확인하세요.';

  @override
  String get reconnect2 => '다시 연결…';

  @override
  String get hostUpdating2 => 'Host 업데이트 중이에요';

  @override
  String get reconnectsAutomaticallyWhenDoneWaits =>
      '끝나면 자동으로 다시 연결해요.\n최대 10분 기다려요.';

  @override
  String get pictureQuality => '화질';

  @override
  String get connectedSomewhereElse => '다른 곳에서 연결 중이에요';

  @override
  String get ifWindowsRestartingConnectsAs =>
      'Windows가 다시 시작되는 중이면 끝나는 대로 연결해요.\n최대 3분 기다려요.';

  @override
  String get connectsAutomaticallyWhenDesktopBack =>
      '데스크톱이 다시 켜지면 자동으로 연결해요.\n최대 3분 기다려요.';

  @override
  String get cancelConnection => '연결 취소';

  @override
  String get preparingScreenFitWindow => '창 크기에 맞는 화면을 준비하는 중';

  @override
  String get checkingAccountPermissionsHostConnection =>
      '계정 권한과 Host 연결을 확인하고 있어요.';

  @override
  String get fittingScreenSize2 => '화면 크기를 맞추고 있어요.';

  @override
  String get preparingHostConnection => 'Host 연결을 준비하고 있어요.';

  @override
  String get connectionCantMatchResolutionWindow =>
      '이 연결에서는 해상도를 창 크기에 맞출 수 없어요. 관리자에게 알려 주세요.';

  @override
  String get matchesResolutionWindowSizeAgain => '창 크기에 맞춰 해상도를 다시 바꿔요.';

  @override
  String get retryResolution => '해상도 다시 시도';

  @override
  String get fixedResolution => '해상도 고정';

  @override
  String get checkingPcsMonitors => 'PC의 모니터 확인 중';

  @override
  String get applyingWindowMode => '화면 모드 적용 중';

  @override
  String get updateStepVerify => '확인';

  @override
  String get closingPreviousConnection => '이전 연결 정리 중';

  @override
  String get waitingHostScreen2 => 'Host 화면 준비 기다리는 중';

  @override
  String get settingUpRemoteMonitors => '원격 모니터 구성 중';

  @override
  String get connectingNewScreen => '새 화면 연결 중';

  @override
  String get waitingFirstFrame3 => '첫 화면 기다리는 중';

  @override
  String get restoringPreviousWindowMode => '이전 창 모드 복원 중';

  @override
  String switching(Object target) {
    return '$target 전환 중';
  }

  @override
  String get cancellingSwitch => '전환 취소 중';

  @override
  String get youCanTypeOnceNew => '새 화면이 준비되면 입력할 수 있어요.';

  @override
  String get cancelSwitch => '전환 취소';

  @override
  String get tryAgainMoment => '잠시 후 다시 시도하세요.';

  @override
  String get tryAgain2 => '다시 시도하세요.';

  @override
  String get saving2 => '저장 중…';

  @override
  String get connectionConfirmed => '연결이 확인됐어요';

  @override
  String get couldntSaveServerAddress2 => '서버 주소를 저장하지 못했어요';

  @override
  String get couldntConfirmConnection => '연결을 확인하지 못했어요';

  @override
  String get checkingServerResponse => '서버 응답을 확인하고 있어요';

  @override
  String get checkConnection => '연결을 확인해 주세요';

  @override
  String multitapResponseTimeMs(Object version, Object elapsedMs) {
    return 'Multitap $version · 응답 시간 $elapsedMs ms';
  }

  @override
  String get pleaseWait => '잠시만 기다려 주세요.';

  @override
  String get addressServerYouSign => '로그인할 서버의 주소';

  @override
  String account3(Object username) {
    return '계정: $username';
  }

  @override
  String text32(num sessionCount) {
    return '기존 로그인 $sessionCount개';
  }

  @override
  String signed2(Object createdAt) {
    return '로그인 $createdAt';
  }

  @override
  String connectedDesktops(Object session) {
    return '연결된 데스크톱: $session';
  }

  @override
  String get signingHereEndsStreamingConsole =>
      '로그인하면 기존 로그인의 스트리밍·Console 연결이 끊겨요.';

  @override
  String get recoveryCodeHasBeenUsed => '복구 코드는 사용됐어요. 취소해도 다시 쓸 수 없어요.';

  @override
  String get signOutElsewhereSignHere => '다른 곳에서 로그아웃하고 여기서 로그인할까요?';

  @override
  String get signOutSign => '로그아웃하고 로그인';

  @override
  String get signedOutMultitap => 'Multitap에서 로그아웃됐어요';

  @override
  String get notYetConfirmedAllConnections =>
      '연결이 모두 끊겼는지 아직 확인하지 못했어요. ‘연결 끊김 다시 확인’을 누르세요.';

  @override
  String get couldntConfirmAllConnectionsClosed =>
      '연결이 모두 끊겼는지 확인하지 못했어요. Multitap을 종료한 뒤 다시 여세요.';

  @override
  String get checking5 => '확인하는 중…';

  @override
  String get checkAgain4 => '연결 끊김 다시 확인';

  @override
  String get youllSignedOutSoonIf => '입력이 없으면 곧 로그아웃돼요. 연결도 모두 끊겨요.';

  @override
  String youllSignedOutInSeconds(Object seconds) {
    return '입력이 없으면 $seconds초 뒤 로그아웃돼요. 연결도 모두 끊겨요.';
  }

  @override
  String get keepUsing => '계속 사용';

  @override
  String get checkingAutoSignOut => '자동 로그아웃 확인 중';

  @override
  String get autoSignOutOff => '자동 로그아웃 꺼짐';

  @override
  String autoSignOut(Object left) {
    return '자동 로그아웃 $left';
  }

  @override
  String get youStaySignedWithoutInput =>
      '입력이 없어도 로그아웃하지 않아요. 다른 곳에서 로그인하거나 로그인이 만료되면 로그아웃돼요.';

  @override
  String get signsYouOutMultitapWhen => '키보드·마우스·터치 입력이 없으면 Multitap에서 로그아웃해요.';

  @override
  String get cantCheckServerSetting => '서버 설정을 확인할 수 없어요.';

  @override
  String get changingSetting => '설정을 바꾸는 중이에요.';

  @override
  String get youStaySignedWithoutInput2 => '입력이 없어도 로그아웃하지 않아요.';

  @override
  String get checkingTimeLeft => '남은 시간을 확인하고 있어요.';

  @override
  String withoutInputYoureSignedOut(Object left) {
    return '입력이 없으면 $left 후 로그아웃해요.';
  }

  @override
  String get autoSignOut2 => '자동 로그아웃';

  @override
  String get couldntChangeSettingTryAgain => '설정을 바꾸지 못했어요. 다시 시도해 주세요.';

  @override
  String get couldntConfirmScreenWasRestored => '화면 복원을 확인하지 못했어요';

  @override
  String connectionsBlockedUntilRestoreConfirmed(Object cleanupCause) {
    return '$cleanupCause\n복원이 확인될 때까지 연결을 막아 둬요.';
  }

  @override
  String statusRecorded(Object cleanupEvidence, Object at) {
    return '$cleanupEvidence · 상태 기록 $at';
  }

  @override
  String get waiting3 => '연결 대기';

  @override
  String get received => '수신';

  @override
  String get connectionSlowLowerPictureQuality =>
      '연결이 느려요. 화질을 ‘절약’으로 낮추면 덜 끊겨요.';

  @override
  String get reconnectSaver => '절약으로 다시 연결';

  @override
  String get showConnectionStats => '연결 통계 보기';

  @override
  String get toolbarHere => '툴바는 여기 있어요.';

  @override
  String get changeScreenModeScaleSettings =>
      '화면 모드, 배율, 설정을 여기서 바꿔요.\n손잡이를 누르면 접히고, 다시 누르면 열려요.';

  @override
  String get guide => '사용안내';

  @override
  String get featuresAvailableConnection => '이 연결에서 쓸 수 있는 기능이에요.';

  @override
  String get dontShowAgainDesktop => '이 데스크톱에서 다시 보지 않기';

  @override
  String get youCanSeeAgainUnder => '설정의 사용안내에서 다시 볼 수 있어요.';

  @override
  String get closeGuideEsc => '사용안내 닫기 (Esc)';

  @override
  String get good => '좋음';

  @override
  String get fair => '보통';

  @override
  String get slow => '느림';

  @override
  String get monitors2 => '모니터 수…';

  @override
  String get windowsTextIconSize => 'Windows 글자·아이콘 크기';

  @override
  String cantUseSharedFolders(Object why) {
    return '공유 폴더를 쓸 수 없어요.$why';
  }

  @override
  String cantUseUsbDevices(Object why) {
    return 'USB 장치를 쓸 수 없어요.$why';
  }

  @override
  String get printerUsbSharedFoldersCopy => '프린터 · USB · 공유 폴더 · 복사';

  @override
  String get turnOffUsbMouse => '로 꺼요 · USB 마우스';

  @override
  String get turnOffNormalInput => '로 꺼요 · 일반 입력';

  @override
  String get turnOff => '로 켜고 꺼요';

  @override
  String get staysOpenWhenYouClick => '데스크톱 화면을 눌러도 접지 않아요';

  @override
  String get notConnected => '연결되지 않음';

  @override
  String get hideDetails2 => '상세 정보 숨기기';

  @override
  String get showDetails => '상세 정보 표시';

  @override
  String get remoteScreenOpensHere => '여기에 원격 화면이 열려요';

  @override
  String get designDemoNoRealConnection => '디자인 데모 · 실제 연결 없음';

  @override
  String get notConnected2 => '미연결';

  @override
  String get frameRate => '프레임 속도';

  @override
  String get latency => '지연';

  @override
  String get input => '입력 전달';

  @override
  String get inactive => '비활성';

  @override
  String get backDesktopList => '데스크톱 목록으로';

  @override
  String get copyDiagnostics2 => '진단 복사';

  @override
  String get detailedDiagnostics => '자세한 진단';

  @override
  String get copyJson => 'JSON 복사';

  @override
  String get connecting3 => '연결 중';

  @override
  String get notConnected3 => '연결 안 됨';

  @override
  String get connectingDesktop => '데스크톱에 연결하는 중';

  @override
  String get notConnected4 => '연결돼 있지 않아요';

  @override
  String get latencyUnder1Ms => '지연 1 ms 미만';

  @override
  String latencyAboutMs(Object value) {
    return '지연 약 $value ms';
  }

  @override
  String get connectionQuality => '연결 품질';

  @override
  String connectionQuality2(Object word) {
    return '연결 품질 · $word';
  }

  @override
  String connectionQuality3(Object summary) {
    return '연결 품질 · $summary';
  }

  @override
  String get menuScreenSettingsMore => '메뉴 · 화면, 설정 등';

  @override
  String get toolbar => '툴바';

  @override
  String get window => '창';

  @override
  String get release => '로 풀기';

  @override
  String slow2(Object latencyText) {
    return '$latencyText · 느림';
  }

  @override
  String get n999MsSlow => '999 ms · 느림';

  @override
  String clickOpenToolbar(Object summary) {
    return '$summary · 눌러서 툴바 열기';
  }

  @override
  String get foldToolbarEsc => '툴바 접기 (Esc)';

  @override
  String get foldersUsb => '폴더·USB';

  @override
  String get keepToolbarOpen => '툴바 열어 두기';

  @override
  String get windowed2 => '창 모드로';

  @override
  String get resolutionRemoteScreen => '해상도 · 원격 화면';

  @override
  String screenDecoding(Object device) {
    return '화면 · $device 디코딩';
  }

  @override
  String get receivedIncomingVideo => '수신 · 받은 영상';

  @override
  String get videoPath => '영상 경로';

  @override
  String get encodingHost => '인코딩 · Host 처리';

  @override
  String decoding(Object device) {
    return '디코딩 · $device 처리';
  }

  @override
  String get couldntGetDesktopConnectionInfo => '데스크톱 연결 정보를 확인하지 못했어요.';

  @override
  String get disconnectedClickCloseRemoteWindow =>
      '연결을 끊었어요. ‘원격 창 닫기’를 다시 누르세요.';

  @override
  String get checkingDesktopConnectionInfo => '데스크톱 연결 정보를 확인하고 있어요.';

  @override
  String get product => '제품';

  @override
  String get couldntLoadSavedSignInfo => '저장한 로그인 정보를 불러오지 못했어요.';

  @override
  String get couldntRememberUsernameYouCan =>
      '아이디를 기억하지 못했어요. 로그인은 그대로 할 수 있어요.';

  @override
  String get couldntTurnOffRememberPassword2 =>
      '비밀번호 기억을 해제하지 못했어요. 다시 시도해 주세요.';

  @override
  String get couldntConfirmRememberSignSetting =>
      '로그인 정보 기억 설정의 저장 결과를 확인하지 못했어요.';

  @override
  String get couldntSaveRememberSignSetting => '로그인 정보 기억 설정을 저장하지 못했어요.';

  @override
  String get changeServerAddress => '서버 주소 변경';

  @override
  String get setServerAddress => '서버 주소 설정';

  @override
  String get saveWindowsCredentialManager => 'Windows 자격 증명에 저장';

  @override
  String get setUpServerStart => '서버 설정하고 시작';

  @override
  String get exploreDemo => '데모 화면 둘러보기';

  @override
  String get collectingDetailedDiagnostics => '상세 진단 수집 중';

  @override
  String get hostDetailsApplyFromNext => 'Host 상세 정보는 다음 연결부터 적용';

  @override
  String viaTcpServerAppRound(
    Object rttMs,
    Object clientDecodeMs,
    Object clientQueueMs,
    Object current,
  ) {
    return 'TCP 서버 경유 · 앱 왕복 $rttMs ms\n디코딩 $clientDecodeMs ms · 큐 $clientQueueMs ms\n$current';
  }

  @override
  String get notMeasured2 => '측정 안 함';

  @override
  String p50P95Ms(Object p50Ms, Object p95Ms, num samples) {
    return 'p50 $p50Ms / p95 $p95Ms ms · $samples회';
  }

  @override
  String get inputHostApplied => '입력→Host 적용';

  @override
  String get inputScreenShown => '입력→화면 표시';

  @override
  String get hostReceivedApplied => 'Host 수신→적용';

  @override
  String get inputAckRoundTrip => '입력 확인 왕복';

  @override
  String get currentServerHostDoesntSupport =>
      '현재 서버 또는 Host가 입력 지연 측정을 지원하지 않아요.';

  @override
  String get samplesFromLast30Seconds =>
      '최근 30초 표본이에요. 입력→Host 적용은 측정한 왕복 시간에서 Host 대기 시간을 뺀 절반(추정)에 Host가 측정한 수신→SendInput 반환 시간을 더한 값이에요. 입력→화면 표시는 입력 송신부터 그 뒤 처음 바뀐 원격 화면이 PC에서 디코딩돼 텍스처로 전달될 때까지이며, 모니터 표시 시간은 포함하지 않아요.';

  @override
  String get fpsPerScreen => 'fps (화면당)';

  @override
  String get status => '상태';

  @override
  String get encodingScale => '인코딩 배율';

  @override
  String get autoResolution => '자동 해상도';

  @override
  String get notSupportedConnection => '현재 연결에서 지원하지 않음';

  @override
  String get applyingRemoteScreen => '원격 화면에 적용 중';

  @override
  String get requestedScreen => '요청 화면';

  @override
  String auto2(Object desired) {
    return '자동 · $desired';
  }

  @override
  String get targetFps => '목표 FPS';

  @override
  String requestedApplied(Object fps, Object session) {
    return '요청 $fps / 적용 $session';
  }

  @override
  String get targetBitrate => '목표 비트레이트';

  @override
  String autoNowMaxMbps(Object bitrateTarget, Object bitrateMax) {
    return '자동 · 현재 $bitrateTarget / 최대 $bitrateMax Mbps';
  }

  @override
  String requestedAppliedMbps(Object options, Object session) {
    return '요청 $options / 적용 $session Mbps';
  }

  @override
  String get captureMethod => '캡처 방식';

  @override
  String get hostGpuUse => '사용 중인 Host GPU';

  @override
  String get selectedHostGpu => '선택한 Host GPU';

  @override
  String get encoderFallbackReason => '인코더 전환 이유';

  @override
  String get losslessSharpening => '무손실 선명화';

  @override
  String get off5 => '사용 안 함';

  @override
  String get waitingAppliesWhenScreenStill => '대기 · 화면이 멈추면 적용';

  @override
  String active(num losslessTiles, Object session) {
    return '적용 중 · 타일 $losslessTiles개$session';
  }

  @override
  String get drawnPcCursorV1 => 'PC에서 표시 (cursor-v1)';

  @override
  String get hostEncoding => 'Host 인코딩';

  @override
  String get pcDecoding => 'PC 디코딩';

  @override
  String get pcDecoder => 'PC 디코더';

  @override
  String get otherAdapter => ' · 다른 어댑터';

  @override
  String hardwareGpu(Object session, Object decodeVendor) {
    return '하드웨어 (GPU$session$decodeVendor)';
  }

  @override
  String get softwareCpu => '소프트웨어 (CPU)';

  @override
  String get frameDelivery => '화면 전달 방식';

  @override
  String get gpuSurface => 'GPU 표면';

  @override
  String get cpuPixelBuffer => 'CPU 픽셀 버퍼';

  @override
  String get newTextureDelivery => '새 텍스처 전달';

  @override
  String get networkWait => '전송 대기';

  @override
  String get textureDeliveryMeasuredFlutterRender =>
      '텍스처 전달은 Flutter 렌더 요청 기준이에요. 화면 표시 완료나 입력 지연을 뜻하지 않아요.';

  @override
  String get hostProcessing => 'Host 처리';

  @override
  String get displayLayoutCheck => '화면 구성 확인';

  @override
  String get captureRead => '캡처·읽기';

  @override
  String get colorConversion => '색 변환';

  @override
  String get encoding => '인코딩';

  @override
  String get totalAvgP95 => '합계 평균 / p95';

  @override
  String get processingMax => '처리 최댓값';

  @override
  String get waitingRecentHostMeasurements => '최근 Host 측정값을 기다리고 있어요.';

  @override
  String get shownOnceHostReportsProcessing => 'Host가 처리 시간을 보고하면 표시돼요.';

  @override
  String lastSSampledNetworkWait(Object session, num samples) {
    return '최근 $session초 · $samples프레임 표본. 네트워크 대기 시간은 포함하지 않아요.';
  }

  @override
  String get pcProcessing => 'PC 처리';

  @override
  String get decodeAvg => '디코딩 평균';

  @override
  String get decodeMax => '디코딩 최댓값';

  @override
  String get videoQueue => '영상 대기';

  @override
  String ms(num clientQueuePackets, Object clientQueueMs) {
    return '$clientQueuePackets개 · $clientQueueMs ms';
  }

  @override
  String get replacedBeforeDelivery => '전달 전 교체';

  @override
  String text33(num skippedDecodedFrames) {
    return '$skippedDecodedFrames프레임';
  }

  @override
  String get gpuVideoDecoding => 'GPU 영상 디코딩';

  @override
  String get averagesCoverRecentPollingWindow =>
      '평균은 최근 조회 구간, 최댓값과 교체 수는 현재 해상도 구간이에요.';

  @override
  String pcTotal(Object app, Object systemTenths) {
    return '$app (PC 전체 $systemTenths)';
  }

  @override
  String get cpuDecoder => 'CPU 디코더';

  @override
  String get gpuDecoder => ' · GPU 디코더';

  @override
  String unknown6(Object path) {
    return '알 수 없음$path';
  }

  @override
  String autoBitrateMaxMbps(Object maxMbps) {
    return '자동 전송률 · 최대 $maxMbps Mbps';
  }

  @override
  String get videoBitrate => '영상 전송률';

  @override
  String get auto3 => '  자동';

  @override
  String get manual2 => '직접 설정';

  @override
  String adjustsBitrateConnectionMaxMbps(Object maxMbps) {
    return '연결 상태에 맞춰 전송률을 조절해요. 최대 $maxMbps Mbps';
  }

  @override
  String get ifNetworkCantKeepUp => '네트워크가 감당하지 못하면 지연이 쌓일 수 있어요.';

  @override
  String get decodesGpuFallsBackCpu => 'GPU로 디코딩해요. 안 되면 CPU를 써요.';

  @override
  String get decodesCpuOnlyTryIf => 'CPU로만 디코딩해요. 화면이 깨질 때 써 보세요.';

  @override
  String get eachMonitorMatchesItsOwn => '모니터마다 그 모니터의 크기에 맞춰요.';

  @override
  String get remoteResolutionFollowsWindowSize => '창 크기에 맞춰 원격 해상도가 바뀌어요.';

  @override
  String get backSettings => '설정으로 돌아가기';

  @override
  String get quitMultitap => 'Multitap을 종료할까요?';

  @override
  String get allOpenConnectionsCloseToo => '열려 있는 연결도 모두 끊겨요. 데스크톱은 계속 켜져 있어요.';

  @override
  String get desktopsStay => '데스크톱은 계속 켜져 있어요.';

  @override
  String version3(Object version) {
    return '$version 버전으로';
  }

  @override
  String get text34 => '으로';

  @override
  String get text35 => '로';

  @override
  String get calculatingTimeLeft => '남은 시간 계산 중';

  @override
  String aboutMinLeft2(Object seconds) {
    return '약 $seconds분 남음';
  }

  @override
  String get checkNeeded2 => '직접 확인이 필요해요';

  @override
  String get couldntUpdate => '업데이트하지 못했어요';

  @override
  String get existingHostUnchanged => '기존 Host는 그대로예요.';

  @override
  String get installedAppUnchanged => '설치된 앱은 그대로예요.';

  @override
  String get couldntGetUpdateInfoCheck =>
      '업데이트 정보를 확인하지 못했어요.\n네트워크를 확인하고 다시 시도하세요. 계속되면 관리자에게 알려 주세요.';

  @override
  String get couldntDownloadFileCheckNetwork =>
      '파일을 받지 못했어요.\n네트워크를 확인하고 다시 시도하세요.';

  @override
  String get networkDroppedSoFileWasnt =>
      '네트워크가 끊겨 파일을 받지 못했어요.\n네트워크를 확인하고 다시 시도하세요.';

  @override
  String get serverDidntRespondWasToo =>
      '서버 응답이 없거나 너무 느려 파일을 받지 못했어요.\n네트워크를 확인하고 다시 시도하세요.';

  @override
  String get serverCouldntRespondJustNow => '서버가 잠시 응답하지 못했어요.\n잠시 후 다시 시도하세요.';

  @override
  String get serverDoesntHaveFileVersion =>
      '서버에 이 버전의 파일이 없어요.\n창을 닫고 업데이트를 다시 확인하세요.';

  @override
  String get serverRefusedFileRequestCheck =>
      '서버가 파일 요청을 거절했어요.\n서버 주소를 확인하고 다시 시도하세요.';

  @override
  String get notEnoughDiskSpaceSave =>
      '디스크 공간이 부족해 파일을 저장하지 못했어요.\n공간을 확보한 뒤 다시 시도하세요.';

  @override
  String get couldntSaveFileTempFolder =>
      '임시 폴더에 파일을 저장하지 못했어요.\n보안 프로그램이나 폴더 권한을 확인하세요.';

  @override
  String get downloadedFileSizeDoesntMatch =>
      '받은 파일 크기가 배포 정보와 달라 설치하지 않았어요.\n다시 시도하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String get downloadedFileDoesntMatchRelease =>
      '받은 파일이 배포 정보의 크기나 SHA-256과 달라 설치하지 않았어요.\n다시 시도하고, 계속되면 관리자에게 알려 주세요.';

  @override
  String fileVersionDoesntMatchRelease(Object version, Object kept) {
    return '파일 버전이 배포 버전$version과 달라 설치하지 않았어요.\n$kept\n관리자에게 알려 주세요.';
  }

  @override
  String releaseSignatureMissingInvalidSo(Object kept) {
    return '배포 서명이 없거나 맞지 않아 설치하지 않았어요.\n$kept\n관리자에게 알려 주세요.';
  }

  @override
  String administratorApprovalWasCancelledSo(Object kept) {
    return '관리자 승인을 취소해 업데이트하지 않았어요.\n$kept\n다시 시도할 때 승인 창에서 ‘예’를 선택하세요.';
  }

  @override
  String fileServerIsntNewerThan(Object kept) {
    return '이 서버의 파일이 지금 실행 중인 버전보다 새롭지 않아 설치하지 않았어요. $kept';
  }

  @override
  String installerClosedBeforeUpdateStarted(Object kept) {
    return '설치 프로그램이 업데이트를 시작하기 전에 종료됐어요. $kept';
  }

  @override
  String get couldntCloseOpenConnectionsDisconnect =>
      '열린 연결을 끊지 못했어요. 연결을 끊은 뒤 다시 시도하세요.';

  @override
  String get couldntCheckRunningVersionRestart =>
      '실행 중인 버전을 확인하지 못했어요. 앱을 다시 시작한 뒤 업데이트를 확인하세요.';

  @override
  String get updateAlreadyBeingPreparedTry =>
      '이미 업데이트를 준비하고 있어요. 잠시 후 다시 시도하세요.';

  @override
  String installationFilesNotValidInstall(Object kept) {
    return '설치 파일 구성이 올바르지 않아요. 설치 프로그램으로 직접 설치하세요. $kept';
  }

  @override
  String couldntCommunicateInstallerTryAgain(Object kept) {
    return '설치 프로그램과 통신하지 못했어요. 다시 시도하세요. $kept';
  }

  @override
  String get desktopHasNoHostInstalled =>
      '이 데스크톱에는 업데이트할 Host가 설치돼 있지 않아요. 설치 프로그램으로 직접 설치하세요.';

  @override
  String get windowsDoesntTrustVirtualDisplay =>
      '가상 화면 드라이버 서명을 이 Windows가 아직 신뢰하지 않아요. 설치 프로그램에서 서명을 확인한 뒤 설치하세요. 기존 Host는 그대로예요.';

  @override
  String get updateNeedsInstallChangeVirtual =>
      '이 업데이트는 가상 화면 드라이버 설치나 변경이 필요해요. 설치 프로그램에서 직접 확인한 뒤 설치하세요. 기존 Host는 그대로예요.';

  @override
  String fileCameFromSomewhereOther(Object kept) {
    return '확인된 서버와 다른 곳에서 받은 파일이라 설치하지 않았어요.\n$kept\n서버 주소를 다시 저장하고 업데이트를 확인하세요.';
  }

  @override
  String get installationStoppedItsUnknownWhether =>
      '설치가 중단됐고 복구 여부도 확인하지 못했어요.\nHost의 서비스 상태를 확인한 뒤 설치 프로그램으로 다시 설치하세요.';

  @override
  String get installationStoppedReinstallApp =>
      '설치가 중단됐고 복구 여부도 확인하지 못했어요.\n설치 프로그램으로 다시 설치하세요.';

  @override
  String get problemOccurredDuringInstallationSo =>
      '설치 중 문제가 생겨 Host를 이전 상태로 되돌렸어요. Host 등록과 가상 화면 설정은 그대로예요.';

  @override
  String get problemOccurredDuringInstallationSo2 =>
      '설치 중 문제가 생겨 이전 버전을 다시 열었어요. 서버 주소와 로그인 설정은 그대로예요.';

  @override
  String get couldntCompleteInstallationItsUnknown =>
      '설치를 완료하지 못했어요.\n어디서 멈췄는지, 복구됐는지는 확인하지 못했어요.\n다시 시도하거나 설치 프로그램으로 직접 설치하세요.';

  @override
  String couldntStartInstallerTryAgain(Object kept) {
    return '설치 프로그램을 시작하지 못했어요. 다시 시도하세요. $kept';
  }

  @override
  String updated(Object product, Object toVersion) {
    return '$product $toVersion 업데이트했어요';
  }

  @override
  String get windowsNeedsRestartSaveWork =>
      'Windows를 다시 시작해야 해요. 작업을 저장한 뒤 직접 다시 시작하세요.';

  @override
  String get checkDiagnosticsHostSettings => 'Host 설정에서 진단을 확인해 주세요.';

  @override
  String get installationFinishedButSomeItems => '설치는 끝났지만 확인이 필요한 항목이 남았어요.';

  @override
  String get hostServiceRunningNewVersion => 'Host 서비스가 새 버전으로 실행 중이에요.';

  @override
  String get reopenedNewVersionSettingsUnchanged =>
      '새 버전으로 다시 열었어요. 설정은 그대로예요.';

  @override
  String get viewChanges => '변경 내역 보기';

  @override
  String stoppedAfterStep6(Object stage) {
    return ' · $stage/6단계 완료 후 중단';
  }

  @override
  String code4(Object code, Object notice) {
    return '코드 $code$notice';
  }

  @override
  String get restartPcNow => 'PC를 지금 다시 시작할까요?';

  @override
  String get saveOpenWorkFirstUnsaved =>
      '열려 있는 작업을 먼저 저장하세요.\n저장하지 않은 작업은 사라질 수 있어요. 데스크톱 연결도 끊겨요.';

  @override
  String get couldntRequestRestartRestartFrom =>
      '다시 시작을 요청하지 못했어요. Windows 시작 메뉴에서 다시 시작해 주세요.';

  @override
  String get installUsbDriver => 'USB 드라이버 설치';

  @override
  String get installsDriverPcNeedsUsb =>
      'USB 연결에 필요한 드라이버를 PC에 설치해요.\nWindows가 관리자 승인을 물으면 ‘예’를 누르세요.';

  @override
  String get onlyInstallersVerifiedSignatureUsed =>
      '서명을 확인한 설치 파일만 사용해요.\nPC를 다시 시작해야 하면 설치 후 알려 드려요. 자동으로 다시 시작하지는 않아요.';

  @override
  String get disconnectUsb => 'USB 연결 끊기';

  @override
  String get ifDesktopUsingDeviceUnsaved =>
      '데스크톱에서 이 장치를 쓰는 중이면 저장하지 않은 내용이 사라질 수 있어요.\n끊으면 장치가 PC로 돌아와요.';

  @override
  String get hideSupportedDevices => '지원 장치 안내 접기';

  @override
  String get supportedDevices => '지원 장치 안내';

  @override
  String get connectedDeviceCanOnlyUsed =>
      '연결한 장치는 데스크톱에서만 쓸 수 있어요.\n연결을 끊으면 PC로 돌아와요.';

  @override
  String get keyboardsMiceHubsBluetoothAudio =>
      '키보드·마우스·허브·블루투스·오디오·카메라는 목록에 표시하지 않아요.';

  @override
  String get restartNow => '지금 다시 시작';

  @override
  String get installsAfterVerifyingInstallersSignature =>
      '설치 파일의 서명을 확인한 뒤 설치해요';

  @override
  String get installing2 => '설치하는 중…';

  @override
  String get installUsbDriver2 => 'USB 드라이버 설치…';

  @override
  String get lookingDevices => '장치를 찾고 있어요…';

  @override
  String get noUsbDevicesConnect => '연결할 USB 장치가 없어요.';

  @override
  String devicesPcNeedsCantConnected(Object hiddenNeeded) {
    return 'PC를 쓰는 데 필요한 장치는 연결할 수 없어요 · $hiddenNeeded개 숨김';
  }

  @override
  String get reconnectingUsb => 'USB를 다시 연결하고 있어요.';

  @override
  String get usbDisconnectedReconnectDesktopUse =>
      'USB 연결이 끊겼어요. 데스크톱에 다시 연결하면 쓸 수 있어요.';

  @override
  String get desktopGettingReadyReceiveUsb => '데스크톱이 USB 장치를 받을 준비를 하고 있어요…';

  @override
  String get pcCantConnectUsbDevices => 'PC에서 USB 장치를 데스크톱에 연결할 수 없어요.';

  @override
  String get restartRequestedWindowsAsksIf =>
      '다시 시작을 요청했어요. 저장하지 않은 작업이 있으면 Windows가 물어봐요.';

  @override
  String get usbDriverNeedsInstalled => 'USB 드라이버 설치가 필요해요.';

  @override
  String text36(num count) {
    return '기타 USB 장치 $count개';
  }

  @override
  String otherUsbDevices(Object count) {
    return '기타 USB 장치 ($count)';
  }

  @override
  String get justPlugged => '방금 꽂음';

  @override
  String get checkingOpenFiles => '열린 파일이 없는지 확인하는 중…';

  @override
  String get connectingDesktop2 => '데스크톱에 연결하는 중…';

  @override
  String get connectedDesktopNotAvailablePc => '데스크톱에 연결됨 · 지금은 PC에서 쓸 수 없어요';

  @override
  String get openFileCheckTimedOut => '열린 파일 확인 시간이 초과됐어요. PC 작업이 끝나기를 기다리는 중…';

  @override
  String get deviceConnectionTimedOutChecking =>
      '장치 연결 시간이 초과됐어요. PC로 돌아왔는지 확인하는 중…';

  @override
  String get checkingDevicePc => 'PC의 장치 상태를 확인하는 중…';

  @override
  String get returningDevicePc => 'PC로 장치를 돌려주는 중…';

  @override
  String programUsing(Object code, Object blocker) {
    return '$code 사용 중인 프로그램: $blocker';
  }

  @override
  String get movingFromAnotherWindow => '다른 창에서 가져오는 중…';

  @override
  String movingFromWindow(Object from) {
    return '$from 창에서 가져오는 중…';
  }

  @override
  String get reconnectingUsb2 => 'USB를 다시 연결하고 있어요';

  @override
  String get youCanConnectOnceDesktop => '데스크톱이 준비되면 연결할 수 있어요';

  @override
  String get deviceUseElsewhere => '다른 곳에서 이 장치를 쓰고 있어요';

  @override
  String get anotherProgramUsingDevice2 => '다른 프로그램이 이 장치를 쓰고 있어요';

  @override
  String get movesWindowsDesktop => '이 창의 데스크톱으로 옮겨요';

  @override
  String get moveWindow => '이 창으로 옮기기';

  @override
  String get usbMovingHere => '옮기는 중';

  @override
  String get moveUsbHereTitle => '이 창으로 옮길까요?';

  @override
  String get moveUsbHereMessage =>
      '다른 창의 데스크톱에서 이 장치를 쓰는 중이면 저장하지 않은 내용이 사라질 수 있어요.\n옮기면 이 창의 데스크톱에서 쓸 수 있어요.';

  @override
  String get moveUsbHereConfirm => '옮기기';

  @override
  String usbSuperSpeedDevices(Object count) {
    return 'USB 3.0 포트에 꽂은 USB 3.0 장치 $count개는 연결할 수 없어요. USB 2.0 포트에 꽂아 주세요.';
  }

  @override
  String get returnPc => 'PC로 되돌리기';

  @override
  String get returning => '되돌리는 중';

  @override
  String get connectsUseDesktop => '데스크톱에서 쓰도록 연결해요';

  @override
  String get securityTokenSmartCard => '보안 토큰·스마트 카드';

  @override
  String get ifDriveDoesntAppearDesktop =>
      '데스크톱에서 드라이브가 보이지 않으면 ‘디스크 관리’를 확인하세요.';

  @override
  String get pcHasNoReadableDrive =>
      'PC에서 읽을 수 있는 드라이브가 없어요. 데스크톱에서도 ‘디스크 관리’를 확인해야 할 수 있어요.';

  @override
  String get useSafelyRemoveHardwareDesktop =>
      '데스크톱에서 ‘하드웨어 안전하게 제거’를 한 뒤 연결을 끊으세요. 쓰는 도중 끊으면 파일이 손상될 수 있어요.';

  @override
  String get couldntConfirmNumberMonitorsChanged =>
      '모니터 수가 바뀌었는지 확인하지 못했어요. 데스크톱 목록에서 확인하세요.';

  @override
  String get connectionClosesReconnectAfterNumber =>
      '연결이 끊겨요. 모니터 수가 바뀐 뒤 다시 연결하세요.';

  @override
  String get disconnectApply => '연결 끊고 적용';

  @override
  String get n1Monitor => '1대';

  @override
  String get n2Monitors => '2대';

  @override
  String get numberMonitorsKeptDesktopWhich =>
      '데스크톱에 켜 둘 모니터 수예요. 볼 화면과 화질 설정은 따로예요.';

  @override
  String get changingNumberMonitors2 => '모니터 수를 바꾸고 있어요…';

  @override
  String switchLabel2(num count) {
    return '모니터를 $count대로 바꿀까요?';
  }

  @override
  String get measuredLocalPc => '로컬 PC에서 측정';

  @override
  String get measuredInsideDesktop => '데스크톱 안에서 측정';

  @override
  String get fromProxmox => 'Proxmox 기준';

  @override
  String cpu(num vcpus) {
    return 'CPU $vcpus코어';
  }

  @override
  String memory(Object memoryTotalBytes) {
    return '메모리 $memoryTotalBytes';
  }

  @override
  String get justStarted => '방금 켜짐';

  @override
  String min2(Object dInMinutes) {
    return '$dInMinutes분';
  }

  @override
  String hr(Object dInHours) {
    return '$dInHours시간';
  }

  @override
  String text37(num dInDays) {
    return '$dInDays일';
  }

  @override
  String get under1Min => '1분 미만';

  @override
  String hrMin(Object dInHours, Object minutes) {
    return '$dInHours시간 $minutes분';
  }

  @override
  String hr2(num dInDays, Object hours) {
    return '$dInDays일 $hours시간';
  }

  @override
  String get memoryValueProxmoxReportsSo =>
      '메모리는 Proxmox가 보고한 값이라 Windows 작업 관리자와 다를 수 있어요.';

  @override
  String text38(num vcpus) {
    return ' · $vcpus코어';
  }

  @override
  String get previousValue => '이전 값';

  @override
  String get couldntGetLatestStatus => '\n최신 상태를 확인하지 못했어요.';

  @override
  String get memory2 => '메모리';

  @override
  String onFor(Object uptime) {
    return '켜진 지 $uptime';
  }

  @override
  String get sampleValuesDesignNotMeasured =>
      '디자인 예시 값이에요. 실제 데스크톱을 측정한 값이 아니에요.';

  @override
  String get gettingCpuMemoryUsage => 'CPU와 메모리 사용량을 받아오는 중이에요.';

  @override
  String text39(num vcpus) {
    return ' ($vcpus코어)';
  }

  @override
  String memory3(Object used, Object total) {
    return '메모리 $used$total';
  }

  @override
  String get couldntGetCpuMemoryInfo => '로컬 PC에서 CPU와 메모리 정보를 받지 못했어요.';

  @override
  String get usageShowsWhileIts => '켜져 있을 때 사용량을 보여줘요.';

  @override
  String get couldntGetCpuMemoryInfo2 => 'Proxmox에서 CPU와 메모리 정보를 받지 못했어요.';

  @override
  String configuredSpecsUsageShowsWhile(Object spec) {
    return '설정된 사양 · $spec\n켜져 있을 때 사용량을 보여줘요.';
  }

  @override
  String get specs => '사양';

  @override
  String get configuredSpecs => '설정된 사양';

  @override
  String couldntGetLatestUsageShows(Object pc, Object spec) {
    return '$pc · $spec\n최신 사용량을 확인하지 못했어요. 확인되면 다시 보여줘요.';
  }

  @override
  String used(Object cpu) {
    return ' 사용 $cpu';
  }

  @override
  String used2(Object used) {
    return ' 중 $used 사용';
  }

  @override
  String measuring(Object cpuLabel, Object memoryLabel) {
    return '$cpuLabel, $memoryLabel 측정 중';
  }

  @override
  String get measuring2 => '측정 중';

  @override
  String newV(Object update) {
    return '새 버전 v$update';
  }

  @override
  String newAppVersionV(Object update) {
    return '앱 새 버전 v$update';
  }

  @override
  String get updatesCantCheckedDemo => '데모에서는 업데이트를 확인할 수 없어요.';

  @override
  String get checkAppUpdates => '앱 업데이트 확인';

  @override
  String get closeDemo => '데모 닫기';

  @override
  String get quitsMultitapCompletelyInsteadKeeping =>
      '트레이에 남기지 않고 Multitap을 완전히 끝내요.';

  @override
  String get autoConnectStoppedBecauseConnection =>
      '5분 안에 연결 준비가 되지 않아 자동 연결을 멈췄어요. 자세히에서 상태를 확인하세요.';

  @override
  String get opensDesktopsOwnScreenApp =>
      '앱 안에서 이 데스크톱의 원래 화면을 열어요.\nWindows가 로그아웃됐거나 잠겨 있을 때 쓰세요.';

  @override
  String get broughtDesktopsOpenWindowFront =>
      '이 데스크톱의 열려 있는 창을 보여 드렸어요. 다른 방식으로 열려면 그 창을 먼저 닫으세요.';

  @override
  String get connectionReadinessChangedCheckList =>
      '연결 준비 상태가 바뀌었어요. 목록을 다시 확인하세요.';

  @override
  String connect5(Object currentName) {
    return '$currentName에 연결할까요?';
  }

  @override
  String get takeOverConnect => '넘겨받아 연결';

  @override
  String get couldntOpenRemoteWindowTry => '원격 창을 열지 못했어요. 다시 시도하세요.';

  @override
  String get openRemoteWindowsCloseToo =>
      '열린 원격 창도 함께 닫혀요.\n데스크톱과 실행 중인 작업은 그대로 유지돼요.';

  @override
  String localPc4(Object machineName) {
    return '$machineName: 로컬 PC가 켜졌어요.';
  }

  @override
  String windowsReceivedRequest(Object machineName, Object label) {
    return '$machineName: Windows가 $label 요청을 받았어요.';
  }

  @override
  String finished(Object machineName, Object label) {
    return '$machineName $label 작업이 완료됐어요.';
  }

  @override
  String couldntProcessRequestTryAgain3(Object label) {
    return '$label 요청을 처리하지 못했어요. 잠시 후 다시 시도하세요.';
  }

  @override
  String hostUpdateStartsWhenConnection(
    Object machineName,
    Object latestVersion,
  ) {
    return '$machineName: 연결이 끝나면 Host $latestVersion 업데이트를 시작해요.';
  }

  @override
  String hostUpdateRequested(Object machineName, Object latestVersion) {
    return '$machineName: Host $latestVersion 업데이트를 요청했어요.';
  }

  @override
  String get couldntRequestHostUpdateTry =>
      'Host 업데이트를 요청하지 못했어요. 잠시 후 다시 시도하세요.';

  @override
  String get allMonitorsWorksOnlyWhen =>
      '모든 모니터는 PC에 모니터가 2대이고 이 데스크톱의 Host가 지원할 때만 쓸 수 있어요.';

  @override
  String get allMonitorsWorksOnlyWhen2 => '모든 모니터는 PC에 모니터가 2대일 때만 쓸 수 있어요.';

  @override
  String get checkingPowerStatusRefreshMoment =>
      '전원 상태를 확인하는 중이에요. 잠시 후 새로고침하세요.';

  @override
  String get refreshingList => '목록을 새로고침하는 중이에요.';

  @override
  String get demoNoServer => '데모 · 서버 미연결';

  @override
  String get refreshingListTryAgainMoment => '목록을 새로고침하는 중이에요. 잠시 후 다시 시도하세요.';

  @override
  String get refreshingList2 => '목록을 새로고침하는 중이에요';

  @override
  String get askAdministratorAssignYouDesktop => '관리자에게 데스크톱 배정을 요청하세요.';

  @override
  String get sampleAssignmentsNotConnectedYet => '예시 할당 목록 · 실제 연결 전';

  @override
  String get couldntLoadDesktopList => '데스크톱 목록을 불러오지 못했어요.';

  @override
  String get language => '언어';

  @override
  String get languageSaveFailed => '언어를 저장하지 못했어요. 다시 고르세요.';

  @override
  String get localPcInSentence => '로컬 PC';

  @override
  String get desktopInSentence => '데스크톱';

  @override
  String get setUpLabel => '설정';

  @override
  String get restoringVirtualDisplayShort => '가상 화면 복구 중';

  @override
  String get settingUpVirtualDisplayShort => '가상 화면 설정 중';

  @override
  String get appliedStatus => '적용';

  @override
  String get quitLabel => '종료';

  @override
  String logCode(Object code) {
    return '코드 $code';
  }

  @override
  String logErrorCode(Object code) {
    return '오류 코드 $code';
  }

  @override
  String get listSeparator => '·';

  @override
  String untilDate(Object date) {
    return '$date까지';
  }

  @override
  String get languageNameKorean => '한국어';

  @override
  String get languageNameEnglish => 'English';

  @override
  String get languageHint => '자동은 시스템 언어를 따라요.';

  @override
  String get mfaSettingHint => '로그인할 때 인증 앱의 코드도 확인해요.';

  @override
  String get passwordSettingHint => 'Multitap 로그인 비밀번호를 바꿔요.';

  @override
  String get hostDoesntMatchRegisteredVm =>
      '이 데스크톱의 Host가 등록된 VM과 맞지 않아요. 관리자에게 알려 주세요.';

  @override
  String get desktopRegistrationChangedRefresh =>
      '이 데스크톱의 등록이 바뀌었어요. 데스크톱 목록을 새로고침한 뒤 다시 연결하세요.';

  @override
  String get proxmoxNoLongerListsVm =>
      'Proxmox에서 이 데스크톱의 VM을 찾지 못했어요. 관리자에게 알려 주세요.';

  @override
  String progressStepOf(Object index, Object count) {
    return '$index/$count단계';
  }

  @override
  String get androidSigningOut => '로그아웃하는 중이에요';

  @override
  String get androidHostUpdateRequesting => '업데이트를 요청하는 중이에요';

  @override
  String get androidHostUpdateReading => 'Host 배포 정보를 확인하는 중이에요';

  @override
  String get androidCheckingOtherConnection => '다른 연결이 있는지 확인하는 중이에요';

  @override
  String get androidCheckingDesktopAgain => '데스크톱 상태를 다시 확인하는 중이에요';

  @override
  String get androidOpeningConsole => 'Console을 여는 중이에요';

  @override
  String get androidEndingConsole => 'Console 연결을 끊는 중이에요';

  @override
  String get androidUpdateCheckingPackage => '설치 파일의 서명을 확인하는 중이에요';

  @override
  String get androidUpdatePreparingInstall => '설치를 준비하는 중이에요';

  @override
  String get androidUpdateWaitingConfirm => 'Android 설치 화면의 확인을 기다리고 있어요';

  @override
  String get androidUpdateInstallingNow =>
      'Android가 업데이트를 설치하고 있어요.\n끝나면 앱이 닫혀요. 다시 열면 새 버전으로 시작해요.';

  @override
  String get androidReadingFolder => '폴더를 읽는 중이에요';

  @override
  String get androidChangingPassword => '비밀번호를 바꾸는 중이에요';

  @override
  String get androidTestingServer => '서버 연결을 테스트하는 중이에요';

  @override
  String get androidMouseLockConnecting => '마우스를 데스크톱에 연결하는 중이에요';

  @override
  String get tileOpeningConsole => 'Console을 여는 중이에요';

  @override
  String get tileOpeningWindow => '원격 창을 여는 중이에요';

  @override
  String get tileRequestingHostUpdate => 'Host 업데이트를 요청하는 중이에요';

  @override
  String get signingOutNow => '로그아웃하는 중이에요';

  @override
  String get loadingDesktopList => '데스크톱 목록을 불러오는 중이에요';

  @override
  String get updateClosingRemoteWindows => '열려 있는 원격 창을 닫는 중이에요';

  @override
  String get sessionEndClosingConnections => '열린 연결을 닫는 중이에요';

  @override
  String get hostStepDisplayPackage => '드라이버 파일이 원본과 같은지 확인하는 중이에요';

  @override
  String get hostStepDisplayTrust => '드라이버 서명을 확인하는 중이에요';

  @override
  String get hostStepDisplayDevice =>
      '가상 화면 드라이버를 설치하는 중이에요. 화면이 잠시 깜빡일 수 있어요.';

  @override
  String get hostStepDisplayService => 'Host 서비스가 가상 화면을 여는 것을 기다리는 중이에요';

  @override
  String get hostStepDisplayRestart => 'Host 서비스를 다시 시작하는 중이에요';

  @override
  String get hostStepWinfspPackage => 'WinFsp 설치 파일을 확인하는 중이에요';

  @override
  String get hostStepWinfspInstall => 'WinFsp를 설치하는 중이에요';

  @override
  String get hostStepWinfspRollback => 'WinFsp 설치를 되돌리는 중이에요';

  @override
  String get hostStepWinfspWaiting => '다른 설치가 끝나기를 기다리는 중이에요';

  @override
  String get hostStepWinfspCheck => '설치된 WinFsp를 확인하는 중이에요';

  @override
  String get hostStepUsbRemove => '다른 USB 연결 기능을 지우는 중이에요';

  @override
  String get hostStepUsbInstall => 'Multitap USB 연결 기능을 설치하는 중이에요';

  @override
  String get hostPhaseTestingServer => '서버에 연결해 응답을 기다리고 있어요.';

  @override
  String get hostPhaseRequestingServer => '서버에 연결 요청을 보내고 응답을 기다리고 있어요.';

  @override
  String get hostPhaseRestartingService =>
      'Host 서비스를 멈췄다가 다시 시작하고 있어요. 창을 닫지 마세요.';

  @override
  String get hostCheckingInstallResult => '설치 결과 확인 중';

  @override
  String get hostPhaseCheckingInstallResult => '설치한 항목을 Windows에서 다시 확인하고 있어요.';

  @override
  String get hostCheckingRequestResult => '요청 결과 확인 중';

  @override
  String get hostPhaseCheckingRequestResult =>
      'Windows 자동 로그인 요청이 어떻게 끝났는지 조회하고 있어요.';

  @override
  String get hostApprovalWaitingLine => '승인을 기다리는 중이에요. 승인되면 바로 다음으로 넘어가요.';

  @override
  String hostProgressPercent(Object percent) {
    return '$percent%';
  }

  @override
  String usbUnreadableDevices(Object count) {
    return '장치 정보를 읽지 못한 USB 장치가 $count개 있어요. 빼고 다시 꽂아 보세요.';
  }
}
