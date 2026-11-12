# Multitap

[한국어](README.ko.md)

**A VDI for Proxmox VE.** Multitap is an open-source tool that lets you use Windows VMs on Proxmox VE (and Windows PCs) as remote desktops from another Windows PC or an Android device. Its first target is VMs without a GPU, with CPU H.264 encoding.

Multitap is an independent tool for Proxmox VE. It is not made or endorsed by, or affiliated with, Proxmox Server Solutions GmbH.

**No warranty.** Multitap is provided as is under AGPL-3.0, without warranty of any kind. You install and run it at your own risk: data loss, security incidents and downtime on your systems are your responsibility. See sections 15 and 16 of the [LICENSE](LICENSE).

> Status: a personal hobby project in real use. Features and quality keep changing. The apps, the admin console and the install scripts are in Korean and English.

For installation and everyday use, read the [guide](docs/guide/en/README.md) ([한국어](docs/guide/README.md)). It goes from installing the server and registering Hosts to connecting from Windows and Android and troubleshooting.

## Parts

| Part | Where | What it does |
|---|---|---|
| Broker / relay | `server/` (Node.js 24, Docker) | Sign-in, accounts and desktop assignments, Proxmox VM list and power, streaming relay (WSS, UDP), the admin console, installer downloads and updates |
| Host | `apps/windows_host/` (C++) | A service inside the Windows VM or PC. Captures and encodes the Multitap virtual display, and handles input, sound and device sharing |
| Clients | `apps/windows_client/` (Flutter, C++, Kotlin) | The Windows and Android apps: the list of assigned desktops and the streaming window. The Host settings window is the same code |
| Shared native code | `native/` | Video, UDP media, clipboard, signature checks and more, shared by the Host and the clients |

Every connection goes through the broker. The client and the Host do not need to be on the same LAN. The structure is described in [ARCHITECTURE.md](ARCHITECTURE.md).

## Features

- Streaming relayed by the broker (WSS, plus a loss-tolerant UDP path with automatic fallback when the broker enables it), H.264 (x264; NVENC/AMF when available), Opus sound
- One or two virtual displays inside the VM, with the resolution following the client window. Streaming works whatever the VM's Proxmox display type is
- Clipboard (text and files), drives, printers and USB devices, each allowed per desktop by the administrator
- Start, shut down, restart and stop for assigned VMs; Wake-on-LAN for local PCs
- An emergency Console through the Proxmox console (browser, noVNC). Only the VM's basic display is on while it is open. On phones and tablets: Menu and Keyboard buttons, and Korean input that follows the VM's real Korean/English mode
- A desktop follows its VM when the VM moves to another node or is restored to a new VM ID (assignments stay). A Host on a cloned VM requests a new connection
- Installers and updates signed with the project's own Ed25519 key

## What you need

- **Server (broker):** a Debian or Ubuntu x86_64 CT or VM with internet access, for the one-line install below (it installs Docker). Any linux/amd64 machine with Docker Engine and the Compose plugin works with the [manual Compose install](docs/docker-deployment.md). For use from outside: router port forwarding, or a domain with HTTPS
- **Proxmox VE:** the server must reach the Proxmox API (port 8006 by default)
- **Host:** a Windows 10/11 x64 VM or PC
- **Clients:** Windows 10/11 x64, Android 8.0 (API 26) or later
- **Build PC:** Windows 10/11 x64. See [Build from source](#build-from-source)

## Install the server

`192.0.2.10` and `vdi.example.com` below are examples.

1. **Prepare Linux.** A Debian or Ubuntu CT or VM (x86_64). For a Proxmox CT, turn on **Options → Features → nesting** (and **keyctl** for an unprivileged CT) and restart it.

2. **Run the install command** in a root shell:

   ```sh
   curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | MULTITAP_LANG=en bash
   ```

   Without root: `… | sudo env MULTITAP_LANG=en bash`. For Korean messages use `MULTITAP_LANG=ko`; without the variable the locale decides (Korean when it starts with `ko`, English otherwise).

   The script installs Docker if it is missing, writes `/opt/multitap` (`compose.yaml`, `.env`), pulls `ghcr.io/totblabs/multitap-broker`, starts the server and installs the `multitap` command. It asks two questions, and Enter means yes:
   - **Let apps and Hosts connect at this address?** `http://<server IP>:18480`, for the same network (not encrypted).
   - **Allow connections from outside too (router port forwarding)?** Asked only when it finds a public IP: it registers `http://<public IP>:18480` and follows the public IP when it changes (checked every 10 minutes).

   Running it again is safe: a finished install is left as it is, and a half-finished one continues with its settings and data.

3. **First setup.** The end screen shows ① the address, ② the first sign-in `admin` / `admin` and ③ a one-time setup code (`sudo multitap code` shows it again). Open the address in a browser, sign in, enter the setup code and choose a new admin username and password.

4. **Addresses for apps and Hosts.** If you answered yes at install, `http://<server IP>:18480` already works. To change addresses, enter them in the admin console under **Server access**: it shows a command such as `sudo multitap address https://vdi.example.com http://192.0.2.10:18480` to run on the server. From outside, forward **TCP 18480** and **UDP 18481** on your router to the server; nothing comes in from outside without it.
   - **HTTP is not encrypted:** passwords, screens and keystrokes travel as they are. With a domain, use HTTPS through a reverse proxy to `http://127.0.0.1:18480` (pass `Host`, `X-Forwarded-Proto: https` and the WebSocket headers; a proxy on the same machine needs the Docker gateway address in `MULTITAP_TRUSTED_PROXIES`). Details: [docs/docker-deployment.md](docs/docker-deployment.md) and [guide chapter 2](docs/guide/en/02-server-install.md).

5. **Connect Proxmox.** In the admin console, open **Proxmox connection** and copy the command. Paste it into the **Shell** of a Proxmox node (root). It creates a dedicated user, role and API token with `VM.Audit`, `VM.PowerMgmt`, `VM.Console` and the guest-agent read privilege (no hardware privilege), and prints a `MULTITAP-CONNECTION-V1` block to paste back into the console. It finds the node's address itself and asks only if that address does not check out; you can also give it by adding it at the very end of the command: `… --lang en https://pve.example.com:8006`. The command checks the downloaded script against the SHA-256 the server shows and runs it only if they match. `--lang ko|en` chooses the language. [Guide chapter 3](docs/guide/en/03-connect-proxmox.md).

6. **Install and approve the Host.** In each Windows VM or PC, download the Host from `<server address>/get/host` and run it. Host settings opens and finishes the install (after one consent to the driver signature it installs the virtual display driver and WinFsp). Enter the server address, request a connection, and approve it in the admin console after comparing the verification code on both screens. Save the Windows account under **Windows sign-in**. [Guide chapter 4](docs/guide/en/04-host-install.md).

7. **Users.** Create accounts and assign desktops in the admin console. Users install the app from `/get/client` (Windows) or `/get/android` and sign in with the server address.

### The `multitap` command

| Command | What it does |
|---|---|
| `sudo multitap status` | Status, version and address |
| `sudo multitap logs` | The server's log (Ctrl+C to stop) |
| `sudo multitap code` | The one-time setup code again |
| `sudo multitap update [1.2.3]` | Back up, then update to the newest (or that) release; goes back if the new one does not answer |
| `sudo multitap backup` | A copy of all server data in `/opt/multitap/backups` |
| `sudo multitap restore <file> [--yes]` | Put a backup back: an admin console backup file (`.mtbk`, asks its backup password) or a `multitap backup` copy. Checks the file and asks first; keeps a safety backup and goes back to it if the restore fails |
| `sudo multitap address <url>…` | The addresses apps and Hosts use (`http://…` for the local network, `https://…` behind a proxy); no url: show them |
| `sudo multitap public-ip [on\|off]` | Follow the server's public IP as an HTTP address (every 10 minutes); no argument: show the state |
| `sudo multitap restart` · `stop` · `start` | Restart, stop, start |
| `sudo multitap uninstall` | Stop and remove the server; the data stays |

The command answers in the install's language (kept as `MULTITAP_LANG` in `/opt/multitap/.env`); `MULTITAP_LANG=ko|en` overrides it.

**UDP media:** the broker relays media on UDP 18481 and Compose publishes that port. Open it in the firewall, forward it on the router, and set `MULTITAP_UDP_HOST_RELAYS`, `MULTITAP_UDP_CLIENT_RELAYS` (e.g. `vdi.example.com:18481`) and `MULTITAP_UDP_MEDIA=on` in `/opt/multitap/.env`, then run `sudo multitap start`. Without them, or with UDP blocked, sessions stay on WebSocket.

**Download bundle:** the published image carries the Windows client and Host installers and the Android APKs (three ABIs), copied to the data volume on first start and served at `/get/client`, `/get/host` and `/get/android`. To build the image from source use `docker compose -f compose.yaml -f compose.build.yaml up -d --build`. The repository's `server/release-bundle/` is empty, so unpack a release's bundle archive there or set `MULTITAP_ALLOW_EMPTY_RELEASE_BUNDLE=true` ([docs/docker-deployment.md](docs/docker-deployment.md)). Release steps: [docs/releasing.md](docs/releasing.md). Administrator notes: [guide chapter 5](docs/guide/en/05-users-and-desktops.md).

## Build from source

The build scripts run in Windows PowerShell 5.1. Tools and external files live in `.tools/` inside the repository (ignored by git). Every external file is checked against a pinned version and SHA-256.

**Common setup**

1. Visual Studio 2022 Build Tools (C++): download `https://aka.ms/vs/17/release/vs_buildtools.exe` to `.tools\downloads\vs_BuildTools.exe` and run `Install-WindowsBuildTools.cmd` (installs into `.tools\visualstudio-buildtools`).
2. Flutter: `powershell -File tools\setup-flutter.ps1` (installs Flutter 3.47.7 into `.tools\flutter`).
3. For the Windows client installer, put the NSIS 3.13 zip at `.tools\nsis-download\nsis-3.13.zip` and unpack it in the same folder, and put 7-Zip's `7z.exe` in `.tools\7zip\`. `tools/build-windows-installer.ps1` checks the NSIS hash and unpacks the finished installer again with 7-Zip to inspect it.

**Windows client**

```powershell
Build-Client.cmd
powershell -File tools\build-windows-installer.ps1 -Product client -Version <x.y.z> -Payload apps\windows_client\build\windows\x64\runner\Release
```

**Android client:** put JDK 21 and Android SDK 36 in the `.tools` paths named by `tools/android-workspace.ps1` and run `powershell -File tools\build-android.ps1 -Version <versionName from build.gradle.kts>`. The first time, `-CreatePreviewKey` creates your own signing key in `.local\android`. The Gradle wrapper (`apps/windows_client/android/gradlew`, the standard wrapper Flutter generates) is in the repository.

**Host**

The Host installer is a Windows Installer package inside an EXE (`apps/windows_host/installer/host_installer.cpp`), built by the scripts in `tools\host-installer\`. The build starts with the pinned x264 commit and NASM ([apps/windows_host/third_party/x264/README.md](apps/windows_host/third_party/x264/README.md)). The scripts read a prepared toolchain from `.tools`; the paths are at the top of each script (an MSVC environment script `.tools\msvc-portable\env.ps1`, CMake 3.31.8, Ninja, the libvirtualdisplay SDK, x264, NASM, the Opus 1.6.1 tarball). No script sets up the compiler part of that layout.

1. `powershell -File tools\fetch-libvirtualdisplay.ps1` fetches the libvirtualdisplay SDK source and the signed driver.
2. `powershell -File tools\host-installer\build-native.ps1 -Source apps\windows_host -Build .artifacts\host-native` builds `MultitapHost.exe` and `MultitapHostTool.exe` and runs their C++ tests.
3. `powershell -File tools\build-portable-windows.ps1 -Variant HostSettings` builds Host settings (the client in Host-settings mode).
4. `tools\host-installer\reversion-msi.ps1` makes the package of a new version from the previous release's package and a payload folder laid out like the installed Host, and `tools\host-installer\build.ps1 -Msi <package> -Version <x.y.z>` wraps it into `Multitap-Host-<x.y.z>-windows-x64.exe`. Every package is made from the one before it, and that first package is not in this repository, so a fresh clone alone cannot finish this step.

The driver files of the payload come from pinned downloads, each checked by hash and signature: `tools\build-virtual-display.ps1` (MttVDD and its helper, `bin\driver`), `tools\fetch-libvirtualdisplay.ps1` (`bin\native-display-driver`), `tools\fetch-usb-drivers.ps1` (usbip-win2, `bin\usb-driver`) and `tools\fetch-winfsp.ps1` (WinFsp, `bin\winfsp`).

**Broker:** `npm ci` and `npm test` in `server/` (Node.js 24). To build the Docker image from source: `docker compose -f compose.yaml -f compose.build.yaml build`.

How to run the tests is in [CONTRIBUTING.md](CONTRIBUTING.md).

**Signing keys:** official releases are signed with this project's Ed25519 keys, and the clients and the Host check them against the public key pinned in the source (`native/windows_admin/release_signature.hpp`). To distribute your own builds, create your own keys (`tools/windows-release-signature.mjs`; for server updates `tools/create-server-update-key.mjs`) and replace the pinned public key. Never put a private key in the repository.

## License

Multitap is distributed under the [GNU AGPL version 3](LICENSE) only (SPDX: `AGPL-3.0-only`, no "or later" clause). If you run a modified version as a network service, you must offer its source to the users of that service (AGPL-3.0 section 13). The name "Multitap" and its logo are not licensed for use on modified versions in a way that suggests they are the original.

The external components used with it, their licenses and the Host's corresponding source are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The Host package links GPL x264 statically and ships its source and notices.

Report security problems privately as described in [SECURITY.md](SECURITY.md), not in a public issue. How to contribute: [CONTRIBUTING.md](CONTRIBUTING.md).
