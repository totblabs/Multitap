# 클라이언트 UI 자산 출처

## 배경 그림

배경은 Multitap를 위해 AI 이미지 생성(OpenAI image generation)으로 새로 만든 그림이다. 사진이 아니며, 다른 제품의 배경 파일이나 화면을 복사하지 않았다.

| 파일 (`apps/windows_client/assets/backgrounds/`) | 출처 | SHA-256 |
| --- | --- | --- |
| `alpine-dusk.png` | AI 생성 원본 (아래 프롬프트) | `8fccdbd2e1d014e2571ba06c2d42e68b1a7c5134b38ce09e325cf09875aed2be` |
| `alpine-daylight.png` | AI 생성 원본, 2026-10-23 (`alpine-daylight.provenance.txt`) | `9dcd1ff6253c8c269795bc22500a68c51809eef36a8c87f031f8eceadbbbe3d5` |
| `alpine-dusk-day.jpg` | `alpine-dusk.png`를 낮으로 보정 (2026-10-26) | `128e829c9cdc5378659cb49b0bb8708831008bbe8f22fecad31a3af8ea3bb27a` |
| `alpine-daylight-night.jpg` | `alpine-daylight.png`를 밤으로 보정 (2026-10-26) | `e3490e1d358f1f2507e981da94cfc26bd3258683c8f000c7d4b7ca8f471f958a` |

`alpine-dusk.png`는 이미지 생성 도구의 출력 `C:/Users/<user>/.codex/generated_images/01a16255-436c-7e42-a640-c35f770a02da/exec-aad629d2-3a36-46f7-896d-3ed70edac599.png`를 바이트 그대로 복사한 것이다. 최종 생성 프롬프트:

```text
Use case: photorealistic-natural. Asset type: full-window background wallpaper for a Windows remote desktop client. Create an original wide 16:9 photorealistic Alpine mountain landscape: massive layered limestone cliffs with warm golden late-afternoon sun on the lower mountain faces, distant snow-dusted ridges, dark evergreen forest silhouettes along the bottom edge, a spacious moody dark blue-gray clouded sky occupying the upper third. Quiet sophisticated desktop-wallpaper photography, natural rock texture, atmospheric depth, restrained contrast. Composition must leave a calm darker upper-left sky area for a white UI card and a darker upper-right sky area for white toolbar controls. No people, buildings, text, logos, icons, UI, borders or watermarks. This is a new scenic photograph-style wallpaper, not an app screenshot. Output a landscape image.
```

### 낮/밤 장면

다크와 라이트 테마는 같은 장소의 밤과 낮을 보여 준다. AI 생성 원본 두 장은 바이트 그대로 두고, 상대편만 `tools/backgrounds/render-backgrounds.ps1`(`Grade.cs`, 결정적, 오프라인)로 보정해 만든다.

| 장면 | 밤 (다크) | 낮 (라이트) |
| --- | --- | --- |
| 봉우리 (기본) | `alpine-dusk.png` 원본 | `alpine-dusk-day.jpg`: 그림자를 열고, 금빛 노을을 중립 오후광으로, 어두운 구름을 흰 구름과 파란 하늘로 |
| 초원 | `alpine-daylight-night.jpg`: 달빛 톤, 남색 하늘, 맑은 하늘에만 달과 별 | `alpine-daylight.png` 원본 |

하늘 경계는 1672×941 원본 위에 손으로 딴 능선 좌표와, 그 근처에서 열마다 하늘/지면 색을 비교하는 마스크로 정한다. 사용자가 고른 장면은 `appearance.json`의 `scene`(`peaks`/`meadow`)에 테마와 따로 저장된다.

관리자 콘솔과 첫 화면의 배경 `server/admin/scene-dusk.jpg`, `scene-daylight.jpg`는 두 AI 생성 원본을 축소한 것이다(`server/admin/scene.provenance.txt`).

## 글꼴과 줄바꿈

클라이언트와 Host 설정 UI의 글꼴은 Pretendard 1.3.9다(맑은 고딕은 대체 글꼴). `apps/windows_client/assets/fonts/Pretendard-{Regular,Medium,SemiBold,Bold}.otf`는 orioncactus/pretendard 공식 릴리스 `Pretendard-1.3.9.zip`(SHA-256 `04be351a74d6bf7d60c480a3087e51d185485d35a52023142af1df19eb8c428a`)의 `public/static` 파일을 그대로 쓴 것이고, SIL Open Font License 1.1 전문을 `Pretendard-LICENSE.txt`로 함께 배포한다. 파일별 크기와 해시는 같은 폴더의 `PROVENANCE.txt`에 있다. 관리자 페이지는 같은 릴리스의 가변 WOFF2를 쓴다(`server/admin/fonts/PROVENANCE.txt`).

한국어 문장은 단어 단위로 줄을 바꾼다. `lib/ui/korean_text.dart`의 `KeepAllText`가 한글이 든 단어의 글자 사이에 U+2060(폭 없는 단어 결합자)을 넣어 표시만 바꾸고, 위젯의 `data`는 원문 그대로 둔다. 설치 창(네이티브 GDI+/DirectWrite)도 같은 규칙으로 공백에서만 줄을 바꾼다.

## 제품 아이콘

제품 기호는 멀티탭이다: 속을 채운 둥근 막대에 둥근 구멍 세 개, 왼쪽 끝에서 나와 아래로 꺾이는 전선. 2026-11-11에 Multitap을 위해 직접 그린 벡터이며 AGPL-3.0-only로 배포한다. Proxmox나 Microsoft의 로고와 색은 쓰지 않는다. 앱 아이콘은 앰버 타일(`#F5A623`)에 어두운 기호(`#1E1503`)이고, 타일 없이 쓰는 곳(앱 머리글과 로그인, 관리자 콘솔과 브라우저 Console 머리글, Host 트레이, Android 알림과 테마 아이콘)은 기호만 한 가지 색으로 쓴다. 글자는 "Multitap" 한 굵기 한 색(Pretendard 700, 자간 -0.04em)에 아래 "for Proxmox VE"다.

- 원본 SVG: `tools/icons/src`. `multitap-mark.svg`는 큰 그림(40 px 이상), `multitap-mark-small.svg`는 구멍과 전선을 키운 작은 그림(32 px 이하)이다. `windows/tile-<크기>.svg`는 앱 아이콘으로, 16/20/24/32/40/48 px는 크기마다 픽셀에 맞춰 따로 그렸고 256이 그 위 크기의 원본이다. 클라이언트, Host, 클라이언트 설치, Host 설치 네 가지는 같은 그림을 쓴다. `windows/tray-<크기>.svg`는 Host 트레이용 기호다(어두운 작업 표시줄에는 앰버, 밝은 작업 표시줄에는 어두운 색). `android/`에는 적응형 아이콘의 전경·배경·단색 레이어, 시작 화면용 기호, 알림 아이콘, TV 배너, 예전 Android용 타일 그림이 있다.
- `node tools/icons/build-icons.mjs`가 설치된 Edge(또는 Chrome)를 헤드리스로 써서 `.ico`(16~256 px)와 트레이 `.ico`, Android mipmap과 VectorDrawable을 다시 만들고, 기호 SVG를 `apps/windows_client/assets/brand/multitap.svg`, `server/admin/mark.svg`, `website/static/assets/mark.svg`로, 16 px 타일을 두 `favicon.svg`로 복사한다. npm 패키지는 쓰지 않는다. 자세한 내용은 `tools/icons/README.md`.
- 앱 안의 `ProductMark`(`lib/ui/components.dart`)는 두 기호 그림을 Flutter 경로로 그린다. 32 px 이하에서는 작은 그림을 쓴다.
- `MultitapHostSettings.exe`는 클라이언트 EXE의 복사본이라, Host를 패키징할 때 `tools/set-executable-icon.ps1`로 Host 아이콘을 넣는다.

UI의 일반 아이콘은 Flutter SDK의 Material Icons를 쓴다.
