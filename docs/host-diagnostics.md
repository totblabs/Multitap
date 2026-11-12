# Host 자동 진단

Host 서비스는 자신의 상태를 등록된 브로커에 자동으로 보고한다. 관리자가 로그를 복사하거나 보내기 버튼을 누를 필요가 없다. 보고는 고정된 필드와 코드·숫자만 담는 관찰 기록이며, 연결 권한이나 스트리밍 준비 완료의 근거로 쓰지 않는다. 코드: Host `apps/windows_host/src/host_diagnostics.hpp`, `host_diagnostic_schedule.hpp`; 브로커 `server/src/host-diagnostics.mjs`, `relay.mjs`.

## 전송과 보관

- 이미 쓰는 HTTPS/WSS 브로커 연결과 Host 인증을 그대로 쓴다. 추가 포트나 별도 전송 서버는 없다.
- Host가 capability에 `diagnosticsSupported:true`를 알리고 브로커가 ACK에 `diagnosticsProtocol:'host-diagnostics-v1'`을 명시해야 보내기 시작한다. 이 프로토콜을 모르는 브로커에는 보내지 않는다.
- 연결 직후 한 번, 그 뒤 약 30초마다 보낸다. 보고 사이 최소 간격은 5초다. 관리자가 **지금 상태 수집**을 누르면 브로커가 `host.diagnostics.request`를 보내고(VM당 5초에 한 번), 요청은 5초 제한에 걸려도 사라지지 않고 다음 보고에 반영된다.
- 보고서는 6,144바이트(UTF-8) 이하이고 최근 이벤트 8개까지 담는다. 브로커는 선택 항목(인코더, 장치, 복구 설명)을 이 크기 안에 들어갈 때만 남긴다.
- 브로커는 받을 때마다 현재 소켓, Host ID, 해지되지 않은 등록을 다시 확인한다. 이전 연결이나 해지된 등록의 보고는 저장하지 않는다. 저장에 실패하면 ACK만 실패로 보내고 제어·스트리밍 채널은 유지한다.
- `host_diagnostics` 표에는 Host마다 최신 보고 하나만 둔다. 이벤트는 `host_diagnostic_events`에 Host별로 누적하는데, 같은 시각·이벤트·값은 한 번만 넣고 Host마다 최근 500건, 7일까지 둔다. 읽을 때 같은 고정 필드로 다시 거른다. Host를 다시 등록해 이전 Host 행이 지워지면 그 진단도 함께 지워진다. 계정·할당·설정·다운로드는 건드리지 않는다.
- Host는 브로커가 받았다고 ACK한 이벤트를 기억해 아직 보내지 않은 이벤트부터 싣고, 남은 이벤트가 있으면 30초 대신 5초 뒤에 다음 보고를 보낸다.

## 담는 정보

- 가상 디스플레이 장치의 설치·시작·소유·활성 여부, 모니터 수와 크기, 설정·복구·재시작 대기, 원격 세션 여부, 로그인·콘솔 세션과 캡처 가능 여부.
- native 디스플레이 제어의 단계, 고정 실패 사유, lease 활성, 활성화 검증 여부, 마지막 활성화 시도의 사유·Windows 오류 번호·시도 여부·Windows 확인 여부와 그 관찰이 현재 세대의 것인지(`activationCurrent`). 롤백에 성공해도 처음 실패 사유를 지우지 않는다.
- 실제 화면 상태를 관찰했는지와 그 관찰의 나이(`observed`, `observationAgeMs`). 아직 관찰하지 못했으면 `observed:false`이고, 콘솔은 이를 모니터 0개나 드라이버 미설치로 단정하지 않고 조회 대기로 표시한다.
- 선택 항목: 인코더 상태; 장치 상태(공유 폴더·USB·프린터·소리를 지금 제공할 수 있는지, 가상 디스플레이 어댑터와 USB 컨트롤러 구성); 디스플레이 복구 설명.
- 최근 Host 이벤트: 정해진 이벤트 이름과 시각, 숫자·boolean 값만.

비밀번호, 토큰, 클립보드, Windows 계정, 원시 예외 문자열, 장치 경로, lease·capability ID는 넣지 않는다. Host와 브로커 양쪽이 허용 목록에 있는 고정 필드만 뽑는다. 보고는 Host의 자기 보고이므로 드라이버 무결성이나 실제 영상 수신·표시 성공을 증명하지 않는다.

## 관리자 콘솔

**데스크톱 → VM 상세 → Host·화면** 탭의 "가상 화면 · 콘솔" 패널에 마지막 보고, 서버 수신 시각, native 단계와 실패 사유가 나오고 **지금 상태 수집** 버튼이 있다. Host 기록은 VM 상세의 활동 탭과 **활동 기록**에 클라이언트·관리 기록과 함께 나온다. API는 관리자 전용이다: `GET /api/v1/admin/desktops/:id/diagnostics`(최신 보고와 `history`), `POST` 같은 경로(수집 요청, Origin·CSRF 확인), `GET /api/v1/admin/hosts`(요약). 할당된 일반 사용자에게는 보고 내용을 주지 않는다. 90초 넘게 보고가 없거나 Host가 오프라인이면 "이전 기록"으로 표시하며, 수집 요청이 받아들여졌다는 것만으로 수집 완료로 표시하지 않는다.

## 장치 표시 이름

Host 설치는 새 설치와 기존 장치를 유지하는 업데이트 모두에서, 설치가 소유한 것으로 확인된 가상 디스플레이 장치(`Root\SunshineVirtualDisplay`)에만 Windows 표시 이름([FriendlyName](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdisetdeviceregistrypropertya))을 `Multitap Virtual Display Adapter`로 정하고 다시 읽어 확인한다(`apps/windows_host/src/setup_pnp.hpp`). 표시 이름 설정에 실패해도 정상 장치를 지우거나 설치를 롤백하지 않는다.

원본 INF/DLL/CAT와 라이선스는 바꾸지 않는다. 드라이버 서명자가 Multitap로 바뀌거나 Multitap 전용 드라이버가 새로 서명된 것이 아니다. 포함한 드라이버는 고정된 upstream 서명 패키지(libvirtualdisplay 1.6.5, `apps/windows_host/third_party/libvirtualdisplay/upstream.json`)다.
