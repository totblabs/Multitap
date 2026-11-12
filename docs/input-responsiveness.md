# 입력 반응성

목표는 마우스·키보드·클릭·드래그·지도 조작이 로컬 PC처럼 느껴지는 것이다. 이 문서는 입력 경로, 각 구간을 재는 방법, 측정에서 나온 설계 결정을 정리한다. 통신 형식은 [세션 프로토콜](remote-session-protocol.md#input-latency-measurement-input-latency-v1)에 있다.

## 경로

클라이언트 키보드 훅·포인터 → Dart `RemoteSession` → 인증된 WSS 중계 → Host 서비스(Session0) → 대화형 worker 파이프 → `SendInput` → 화면 캡처(Windows Graphics Capture) → H.264 인코딩 → 중계(WSS 또는 UDP) → 클라이언트 디코딩 → 텍스처.

- 포커스를 잃거나 연결이 끊기면 눌린 키와 버튼을 원격에서 해제한다.
- 높은 권한 앱(작업 관리자 등)에 입력하려면 주입하는 프로세스의 무결성 수준이 대상 이상이어야 한다(UIPI). `tools/input-integrity-qa`가 이를 확인한다.
- 커서는 영상과 따로 보낼 수 있다(`cursor-v1`, 커서 에코 `cursor-echo-v1`). 커서 모양·핫스팟은 원격 그대로 쓰고, 협상하지 못하면 커서를 영상에 합성한다. 형식은 프로토콜 문서에 있다.
- 마우스 잠금(`relative-mouse-v1`)은 연결할 때마다 꺼져 있다. 켜면 상대 이동을 보내고, 권한이 있으면 가상 USB HID 마우스를 쓴다.

## 측정 구간

`input-latency-v1`을 협상한 단일 출력 세션에서 클라이언트 통계 창에 `입력→호스트 적용`과 `입력→화면 표시`의 p50/p95가 나온다. 클라이언트 진단 보고에는 구간별 값이 들어간다.

| 필드 | 구간 |
|---|---|
| `inputHostP50Us` | 입력 송신 → Host 주입 반환(왕복에서 Host 대기를 뺀 절반의 추정 + 실측 주입 시간) |
| `inputFrameP50Us` | 입력 송신 → 그 뒤 처음 바뀐 프레임이 디코딩되어 텍스처에 넘겨짐 |
| `hostInputCaptureP50Us` | Host 주입 → 바뀐 프레임의 캡처 틱(게스트 앱·DWM 반응 포함) |
| `hostCaptureEncodedP50Us` | 캡처 틱 → 인코더 출력 |
| `hostSendQueueP50Us` | 인코더 출력 → 송신 시작 |
| `inputPushP50Us`, `inputDecodeP50Us` | 클라이언트 수신 → 디코더 큐, 디코더 큐 → 텍스처 |
| `inputTransferP50Us`, `inputFrameBytes` | 태그 도착 → 영상 도착, 그 프레임의 크기 |
| `inputDownlinkP50Us` | Host 태그 기록 → 영상 도착(도착 − 송신 − 상향 추정 − Host 시간) |

`inputFrame`에는 클라이언트 훅·Flutter 처리 이전 시간, Flutter 래스터, 클라이언트 DWM, 모니터 표시가 들어가지 않는다. 서로 다른 구간의 백분위수는 더해지지 않으므로, p50끼리 빼서 남은 값은 측정값이 아니라 추정이다. `inputDownlink`가 크고 `inputTransfer`가 `inputFrameBytes`와 함께 커지면 망·중계 전달이고, `hostSendQueue`가 크면 Host 송신 큐다. 서비스 ping, `SendInput` 성공, 디코더 콜백은 서로 다른 단계이며 체감 지연을 대신하지 않는다.

## 측정에서 나온 결정

### 효과 없는 입력의 귀속(`expired`, `ambient`)

아무것도 바꾸지 않는 입력(효과 없는 키, 스크롤 끝의 휠, 빈 곳 클릭)은 다음의 무관한 변화(캐럿 깜박임, 시계)에 붙어 그 기다림이 지연으로 보고된다. 화면이 거의 정지한 세션에서 `hostInputCapture` p95가 수백 ms로 보이던 원인이 이것이었다.

`MultitapHostInputAttributionBenchmark`로 재현했다(GPU 없는 테스트 VM, 자기 창만 WGC 캡처, `SendInput` 미사용). 530 ms 주기로 깜박이는 요소가 있는 창에서 1.1–1.9초 유휴 뒤 입력했다.

| | 효과 없는 입력이 프레임에 붙음 | 효과 있는 입력(10 ms 뒤 다시 그림) p50 / p95 |
|---|---|---|
| 이전(1초 창) | 25/25, 가짜 지연 p50 241–246 ms, p95 447–505 ms | 33 / 48 ms |
| 지금(500 ms 창 + 주기 필터) | 0/25 | 36 / 47 ms |

그래서 Host는 500 ms(`inputLatencyFrameWindowUs`) 안에서만 입력과 프레임을 잇고, 입력이 없던 동안 반복되는 100 ms 이상 주기의 변화는 입력 결과로 세지 않는다. 대가로 효과 있는 입력의 약 10–16 %가 깜박임 박자와 겹쳐 귀속되지 않고 `expired`·`ambient`로 집계된다. 영상 재생처럼 빠른 변화는 걸러내지 못한다. 이 실험에서 WGC는 유휴 뒤 첫 프레임을 늦추지 않았다.

프레임 후보는 스스로 화면을 바꿀 수 있는 입력(Shift/Ctrl/Alt/Windows가 아닌 키 누름, 버튼, 휠, 커서가 영상에 있거나 버튼을 누른 채 하는 이동)만이다. 클라이언트는 키 놓기와 수정 키를 표본으로 쓰지 않고, 표본이 된 버튼 누름의 놓기는 항상 표본으로 보낸다.

### WGC `MinUpdateInterval` 1 ms

WGC 세션의 기본 `MinUpdateInterval`은 16 ms이고(Windows 11, `MultitapHostCapturePacingTest`가 기록), 그보다 빨리 합성된 프레임은 버리지 않고 간격 뒤까지 붙잡는다. 60 Hz 출력에서 합성 간격이 흔들리면 프레임 하나가 한 주기(16.7 ms) 늦어질 수 있다. Host는 이 값을 1 ms로 두고 자체 `FramePacer`로만 속도를 제한한다(`apps/windows_host/src/capture_session_interval.hpp`).

### Nagle 끔

| 소켓 | 상태 | 근거 |
|---|---|---|
| Host WinHTTP WebSocket | TCP_NODELAY 켜짐 | 측정: `MultitapHostWebSocketNoDelayProbe`가 같은 설정의 WinHTTP 소켓을 찾아 읽음 |
| 중계 `ws`, 클라이언트 Dart 소켓 | TCP_NODELAY 켜짐 | 라이브러리 기본값(`ws`의 `setNoDelay`, `dart:io` 소켓) |

입력 상향과 영상 하향은 반대 방향이므로 영상 큐가 입력을 직접 막지 않는다. 지연을 숨기려고 P 프레임을 버리지 않는다.

### 측정된 구간 값

GPU 없는 테스트 VM(10 vCPU, 원격 데스크톱 세션 안이라 합성 약 32 Hz)에서:

- 클라이언트 수신 → 텍스처: p50 3.1–3.5 ms, p95 3.7–5.6 ms. 바쁜 스레드 20개를 함께 돌려도 같았고, 프로세스 우선순위를 올려도 나아지지 않았다(`tools/media-path-qa`).
- 단순 앱 변경 → 캡처 틱: GDI 창 평균 19–25 ms, Edge 키 입력 평균 21 ms(p95 34). 32 Hz 합성기 기준이며, 60 Hz 가상 출력에서는 더 짧을 것으로 본다.
- WinHTTP `WinHttpWebSocketSend`(8–200 KB): p50 0.15–0.6 ms. 같은 프로세스의 중계 전달: p50 0.7–2 ms.

이 값들을 빼고 남는 `inputFrame`의 대부분은 게스트 앱·DWM의 반응과 망 경로다. 같은 보고의 `hostInputCaptureP50Us`, `captureFps10`, `encodeFps10`으로 확인한다.
