# Third-party notices

Multitap is licensed under the GNU Affero General Public License version 3 only (SPDX: `AGPL-3.0-only`, [LICENSE](LICENSE)). It uses the third-party components below. Each keeps its own licence; the full licence texts are in the files named in the last column. Components marked *fetched* are not stored in this repository: the build scripts download them, check pinned SHA-256 hashes (and signers, for signed drivers) and place them unmodified in the packages.

The Host statically links x264 (GPL-2.0-or-later, used under GPL-3.0) and uses WinFsp under GPL-3.0 with its FLOSS exception. The Host program is AGPL-3.0 code linked with GPL-3.0 code, which section 13 of both licences permits. Host packages carry `COPYING.*` and `THIRD-PARTY-NOTICES.txt` files and a source archive (`host-native-corresponding-source-<version>.zip`) with the native build's source snapshot and the pinned x264, libopus and libvirtualdisplay sources.

The apps also show these notices under 정보·라이선스, read from `apps/windows_client/assets/licenses/`: `native_notices.json` (Windows client), `host_native_notices.json` (Host settings) and `android_native_notices.json` (Android, which carries none of the Windows-only parts: UsbDk, the Visual C++ runtime, usb.ids, libopus, WebView2), together with Flutter's generated package notices.

## Bundled or linked in released packages

| Component | Version / pin | Where | Licence | Shipped in | Licence text |
|---|---|---|---|---|---|
| [x264](https://code.videolan.org/videolan/x264) | commit `b35605a` (0.165.3222), unmodified, not vendored | `apps/windows_host/third_party/x264/` (build glue) | GPL-2.0-or-later | Host (static) | `apps/windows_client/assets/licenses/x264-COPYING.txt`; Host package `COPYING.x264.txt` + source archive |
| [Opus (libopus)](https://opus-codec.org/) | 1.6.1, tarball SHA-256 pinned, fetched at build | `native/third_party/opus/` | BSD-3-Clause + Opus patent licences | Host, Windows client (static) | `native/third_party/opus/COPYING` (in-app copy `apps/windows_client/assets/licenses/libopus-COPYING.txt`) |
| [WinFsp](https://github.com/winfsp/winfsp) | 2.1 (2.1.25156), MSI fetched and pinned | `apps/windows_host/third_party/winfsp/` (headers) | GPL-3.0 with FLOSS exception | Host (unmodified MSI) | `apps/windows_host/third_party/winfsp/License.txt` |
| [usbip-win2](https://github.com/vadimgrn/usbip-win2) | 0.9.7.5-preview (OSSign signed build), fetched and pinned | `apps/windows_host/third_party/usbip-win2/upstream.json` | BSD-2-Clause | Host (unmodified driver files) | `apps/windows_client/assets/licenses/usbip-win2-LICENSE.txt`; Host package `COPYING.usbip-win2.txt` beside `usb-driver/` |
| [UsbDk](https://github.com/daynix/UsbDk) | 1.0.22, MSI fetched and pinned | `apps/windows_client/third_party/usbdk/upstream.json` | Apache-2.0 | Windows client (unmodified MSI) | `apps/windows_client/assets/licenses/UsbDk-LICENSE.txt` |
| [Virtual Display Driver (MttVDD)](https://github.com/VirtualDrivers/Virtual-Display-Driver) | 25.7.23, signed release fetched and pinned | `drivers/virtual_display/` | MIT | Host (unmodified driver) | `drivers/virtual_display/LICENSE.upstream.txt` |
| [libvirtualdisplay](https://github.com/Nonary/libvirtualdisplay) control SDK and SunshineVirtualDisplayDriver | v1.6.5 (source commit `b6874684`); source archive and signed release package fetched and pinned | `apps/windows_host/third_party/libvirtualdisplay/upstream.json`, `tools/fetch-libvirtualdisplay.ps1`; bridge `drivers/display_bridge/` | MIT | Host (SDK static; unmodified signed driver INF/DLL/CAT/CER) | `drivers/display_bridge/UPSTREAM-LICENSE.txt` (in-app copy `libvirtualdisplay-LICENSE.txt`; full text in the Host package's `THIRD-PARTY-NOTICES.txt`) |
| NVIDIA Video Codec SDK header `nvEncodeAPI.h` | API 12.0, from NVIDIA/VideoProcessingFramework | `apps/windows_host/third_party/nvenc/` | MIT | Host (header only; driver runtime not shipped) | `apps/windows_host/third_party/nvenc/LICENSE.nvEncodeAPI.txt` (in-app copy `nvEncodeAPI-LICENSE.txt`) |
| [AMD AMF](https://github.com/GPUOpen-LibrariesAndSDKs/AMF) public headers | 1.4.30 | `apps/windows_host/third_party/amf/` | MIT (+ AMD notice) | Host (headers only; runtime not shipped) | `apps/windows_host/third_party/amf/LICENSE.txt`, `NOTICE.txt` (in-app copy `AMF-LICENSE.txt`) |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) (Ed25519 verification follows it; the Android X25519 uses its limb layout) | — | `native/windows_admin/release_signature.hpp`, `apps/windows_client/android/.../UdpMediaCrypto.kt` | Public domain | Host, clients | (public domain; attribution in the source) |
| [usb.ids](http://www.linux-usb.org/usb.ids) vendor names | 2026.06.26 | `apps/windows_client/third_party/usb-ids/` | GPL-2.0-or-later or BSD-3-Clause (used under BSD-3-Clause) | Windows client, Host settings | `apps/windows_client/third_party/usb-ids/NOTICE.txt` |
| [noVNC](https://github.com/novnc/noVNC) | 1.7.0 (commit `63107bd0`), pinned | `server/admin/console/vendor/novnc/1.7.0/` (`core/`, `vendor/pako`) | MPL-2.0 (core), MIT (pako), BSD (DES) | Broker image | `server/admin/console/vendor/novnc/1.7.0/LICENSE.txt`, `server/admin/console/THIRD-PARTY-NOTICES.txt` |
| [ws](https://github.com/websockets/ws) (npm) | 8.22.0 | `server/package.json` | MIT | Broker image (only production npm dependency) | in `node_modules/ws/LICENSE` at build |
| [Flutter](https://flutter.dev/) engine and framework, `crypto`, `intl`, `qr`, `collection` and other Dart packages | Flutter 3.47.7 (pinned) | `apps/windows_client/pubspec.lock` | BSD-3-Clause; `material_color_utilities` Apache-2.0 | Windows client, Host settings, Android | Flutter's generated `NOTICES` in each build; `apps/windows_client/assets/licenses/qr-NOTICE.txt` |
| AndroidX, Kotlin standard library | via the Flutter Android embedding | `apps/windows_client/android/` | Apache-2.0 | Android | Flutter's generated `NOTICES` |
| Material Icons (font subset) | from the Flutter SDK | built in by Flutter | CC-BY-4.0 | Clients | `apps/windows_client/assets/licenses/MaterialIcons-CC-BY-4.0.txt` |
| [Pretendard](https://github.com/orioncactus/pretendard) | 1.3.9 (OTF); variable WOFF2 in the admin console | `apps/windows_client/assets/fonts/`, `server/admin/fonts/`, `server/admin/console/fonts/` | SIL OFL-1.1 | Clients, broker image | `apps/windows_client/assets/fonts/Pretendard-LICENSE.txt`, `server/admin/fonts/LICENSE.txt` |
| Microsoft WebView2 SDK (loader, static) | NuGet 1.0.4314.35, fetched and pinned | `apps/windows_client/windows/runner/webview2.cmake` | Microsoft WebView2 SDK licence (BSD-3-Clause style) | Windows client, Host settings | `apps/windows_client/assets/licenses/WebView2-LICENSE.txt`, `WebView2-NOTICE.txt` (from the pinned package; the build also copies them into `data/licenses/`) |
| Microsoft Visual C++ runtime (VC143) | from Visual Studio Build Tools | build scripts | Microsoft redistributable licence (system library) | Host, Windows client | `apps/windows_client/assets/licenses/Microsoft-VC-Runtime-LICENSE.txt` |
| [NSIS](https://nsis.sourceforge.io/) installer stub | 3.13, pinned | `deploy/windows/product.nsi` | zlib/libpng (LZMA: CPL-1.0 with exception) | Windows client installer EXE | NSIS distribution |
| Proxmox VE documentation image | `pve-docs` | `server/admin/help/proxmox-role-add.png` | GFDL-1.3 | Broker image | `server/admin/help/LICENSE-proxmox.txt` |
| Node.js base image | `node:24-bookworm-slim` (digest pinned) | `server/Dockerfile` | Node.js (MIT) and Debian packages (various) | Broker image | inside the image |

## Corresponding source

A distributed Host build is Multitap's AGPL-3.0-only code linked with x264 under GPL-3.0. Its complete corresponding source is this repository at the commit the build was made from, plus the pinned upstream sources the build takes from outside the repository:

| Upstream source | Pin | How to get it |
|---|---|---|
| x264 | commit `b35605ace3ddf7c1a5d67a2eb553f034aef41d55` | `apps/windows_host/third_party/x264/README.md`; every Host package also carries it in `host-native-corresponding-source-<version>.zip` |
| libopus | 1.6.1 tarball, SHA-256 in `native/third_party/opus/upstream.json` | CMake downloads and checks it during the build |
| libvirtualdisplay control SDK | commit `b6874684d82925d0d76f487d8e4ba3ec71c9721a`, archive SHA-256 and tree digest in `apps/windows_host/third_party/libvirtualdisplay/upstream.json` | `tools/fetch-libvirtualdisplay.ps1` |
| WebView2 SDK loader (Host settings and Windows client) | NuGet package pinned in `apps/windows_client/windows/runner/webview2.cmake` | CMake downloads and checks it during the build |

The NVENC, AMF and WinFsp headers are kept in the tree. The `THIRD-PARTY-NOTICES.txt` of every Host package built with x264 (all released ones) contains a written offer of the corresponding source; whoever distributes a Host build must honour that offer for their build, for example by pointing to this repository at the matching release tag together with the pinned sources above.

## Project artwork

- Product icons (`tools/icons/src/`, `apps/windows_client/assets/brand/`, `server/admin/mark.svg`, `.ico` files, Android launcher icons): drawn for Multitap, AGPL-3.0-only like the rest of the project. See `docs/client-ui-assets.md`.
- Background images (`apps/windows_client/assets/backgrounds/`, `server/admin/scene-*.jpg`): AI-generated images made for Multitap (OpenAI image generation), not photographs, graded by `tools/backgrounds/`. Provenance: `docs/client-ui-assets.md`, `*.provenance.txt`.

## Build tools only (not shipped)

NASM, the Windows Driver Kit, 7-Zip, Gradle/Android Gradle Plugin and test-only packages (`linkedom` and its dependencies, `fake_async`, `flutter_lints`, JUnit) are used to build or test and are not part of released packages. The repository carries the standard Gradle wrapper that Flutter generates (`apps/windows_client/android/gradlew`, `gradlew.bat`, `gradle/wrapper/gradle-wrapper.jar`; Gradle, Apache-2.0); it downloads the Gradle version pinned in `gradle-wrapper.properties`.

## Not part of this repository

Multitap does not include or require Sunshine, Moonlight or another separately installed remote-desktop product; streaming uses its own engine. "Proxmox" and "Proxmox VE" are used only to name the platform Multitap works with.
