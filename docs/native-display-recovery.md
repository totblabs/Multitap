# Native 가상 화면 복구

Native provider(`drivers/display_bridge/`의 libvirtualdisplay SDK와 서명된 SunshineVirtualDisplayDriver)에서 Host 서비스가 소유 가상 출력을 다시 만들고 다시 확인하는 규칙이다. 스트리밍 중 Windows 화면 구성을 저장하고 되돌리는 일은 [display manager v2](display-manager-v2.md)가 맡는다. 이 문서는 그 아래층, 즉 드라이버의 임시 출력과 lease를 다룬다.

코드: `apps/windows_host/src/native_display_service.hpp`, `drivers/display_bridge/runtime.hpp`, `drivers/display_bridge/lease_recovery.hpp`.

## 기록

| 파일 (`C:\ProgramData\Multitap\Host\`) | 내용 |
|---|---|
| `native-display.json` | provider 선택: `provider`(`libvirtualdisplay-1.6.4`), `enabled`, 장치 인스턴스 ID |
| `private\native-display.dpapi` | machine DPAPI로 보호한 lease ID, owner capability, 공개 출력 ID |
| `private\native-display-runtime.json` | 장치 인스턴스, 단계(`starting`, `changing`, `stable`, `stopped`, `recovery`), 단조 증가 generation, 마지막으로 완료한 출력 1/2개와 크기 |

출력 수와 크기는 설치 기본값(1920x1080)에서 시작한다. 이후에는 runtime 기록이 클라이언트가 고른 값을 서비스 재시작과 Host 업데이트 너머로 유지한다. SDK를 바꾸기 전에 새 generation과 단계를 먼저 저장한다. 저장에 실패하면 드라이버를 바꾸지 않는다.

## controller가 없을 때: 출력 다시 만들기

서비스 시작이나 중단 뒤에 runtime 기록이 `starting`, `changing`, `recovery`이면 복구 상태다.

1. 시도 간격은 1초에서 시작해 두 배씩 늘고 최대 30초다.
2. 보호된 설정이 가리키는 정확한 장치의 control interface만 연다.
3. 자기 lease ID로 SDK의 읽기 전용 `query_lease`를 부른다. 응답 ID가 같고, `lease_exists=false`, 임시 출력 0, 제거 대기 0일 때만 진행한다. 조회 실패, 응답 불일치, 살아 있는 lease, 남은 출력은 다음 시도까지 기다린다. 이 조건을 만들려고 lease를 강제로 풀지 않는다.
4. 새 generation과 `starting`을 저장한 뒤 마지막으로 완료한 출력 수와 크기로 다시 만든다. 중단 당시 끝나지 않은 크기는 쓰지 않는다. 브리지의 `Session::start`(`drivers/display_bridge/provider.hpp`)도 생성 직전에 lease를 다시 조회하므로 그 사이에 생긴 충돌도 거부된다.

실행 중 heartbeat가 실패해도 같은 복구 상태로 들어간다.

## Windows 활성화 확인

SDK가 target을 만든 것만으로는 준비가 아니다. 콘솔 세션의 worker가 새 generation의 target을 실제 Windows 구성에서 활성화하고 크기와 캡처를 확인해야 한다. 서비스가 같은 generation에 대해 `stable`을 저장하고 보호된 snapshot을 게시한 뒤에야 Host가 스트리밍 가능을 알린다. 그 전에는 `native_display_activation_pending`으로 보고한다. 이전 generation의 응답, 취소, 세션 변경, 게시 실패는 완료가 아니다.

## controller가 살아 있을 때: 활성화만 다시 확인

활성화 worker의 결과가 불확실하면 그 worker가 정말 끝났는지 process handle의 signalled 상태로 확인한다. job을 닫았거나 시간이 지났다는 것만으로는 종료로 보지 않는다.

종료가 확인되고, lease가 살아 있고, 현재 출력이 마지막 완료 출력과 같고, 진행 중인 크기·개수 변경이 없을 때만 새 generation으로 활성화 확인을 다시 한다. target을 지우거나 다시 만들지 않고, lease를 풀지 않고, 예전 외부 화면 구성을 다시 적용하지 않는다. 간격은 위와 같이 1초에서 30초까지다.

display manager v2에서는 두 가지가 더 있다. 건강한 lease에 이전 크기 변경의 복구 표시만 남아 있으면 실제 SDK 출력을 완료 출력으로 받아들이고 위의 활성화 확인으로 돌아간다. 이전 방식이 남긴 baseline 연결 기록은 controller 시작을 막지 않고 버린다. Windows 구성의 복원은 v2 상태 파일이 맡기 때문이다.

## 관리자 초기화

Host 설정의 **가상 화면 복구 기록 초기화**는 자동 복구로 풀리지 않는 기록을 위한 관리자 동작이다. 확인을 받은 뒤에만 실행한다. 스트림, 콘솔 준비, 화면 변경, 복구가 진행 중이면 거절한다. SDK가 알려진 lease와 출력이 모두 없다고 답해야 진행한다. 콘솔 세션에서 활성 화면이 있는지 확인하고, 없으면 확실히 식별된 QEMU 콘솔만 다시 켠다. 그다음 복구 기록을 타임스탬프 백업 폴더로 옮긴다(삭제하지 않음). DPAPI 소유 정보는 남긴다.

## 하지 않는 일

- 이름만으로 프로세스를 찾아 끝내지 않는다. 직접 만든 자식 프로세스만 정리한다.
- 다른 제품의 lease, 물리 출력, Proxmox 콘솔을 제거하지 않는다.
- Windows를 자동으로 재시작하지 않는다. 장치가 재시작을 요구하면 그대로 보고한다.
- 드라이버 INF/DLL/CAT 파일과 서명을 바꾸지 않는다.

진단 코드(`runtime_recovery_required`, `controller_start_failed`, `heartbeat_failed`, `transport_open_failed` 등)는 [host-diagnostics.md](host-diagnostics.md)의 자동 진단으로 관리자에게 전달된다.
