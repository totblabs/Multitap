# Changelog

[한국어](CHANGELOG.ko.md)

What changed for the people who install and use Multitap. A release is numbered after the server; 1.1.1 to 1.1.11 carry the Windows app, the Android app and the Host at the same number. 1.1.12 is the server and the two apps; the Host stays at 1.1.11. 1.1.13 is the server only; the apps stay at 1.1.12 and the Host at 1.1.11. Installers are on the [Releases](https://github.com/totblabs/Multitap/releases) page.

## [1.1.13](https://github.com/totblabs/Multitap/releases/tag/v1.1.13)

### Starting a PC (server)

- **Fixed: on a server installed as a Linux service, starting a local PC never worked.** The server service's security settings kept it from reading its network list, so it sent no Wake-on-LAN signal at all and failed at once. It sends the signal now; updating the server to 1.1.13 applies it.
- A signal that cannot be sent is recorded in the admin console's log as "PC wake signal not sent", with the reason. Until now it read "VM power request failed".
- A server installed with Docker cannot start a local PC yet: Docker's network does not pass the signal on to the PCs' network. Proxmox VMs start as before.

### Gamepads (server)

- **Fixed: a button or trigger held when an app's connection dropped for a moment could stay held on the desktop.** The server now releases the virtual pads while the app is away.
- When the app is back, the server sends it the game's latest rumble again.

### Copy and paste

- Correction: the 1.1.8 entry below says that a copy and paste permission an administrator widens during a connection applies at once. It does not; it applies from the next connection.

### Admin console (server)

- The Host's log shows the virtual mouse and gamepad stages with their port, pad number and failure reason; until now they read as unrelated USB sharing reasons.
- **Fixed: on a server with no Proxmox connection yet, a Host request read "Couldn't connect to Proxmox".** It now says the server is not connected to Proxmox yet and offers no manual VM entry, which cannot work then. A failed connection says what to check: the address and firewall, the token or the certificate.
- **The Proxmox connection page has "Reconnect".** Until now the way to make a new integration account was only inside the folded "Change connection settings" at the bottom. When Proxmox refuses the token (removed in Proxmox, or a backup restored from another server, for example), the page says that renewing will not fix it and offers "Reconnect" right there.

## [1.1.12](https://github.com/totblabs/Multitap/releases/tag/v1.1.12)

### Gamepad records (server, Windows app, Android app)

- **The app's connection statistics record the gamepads.** Whether the connection was allowed gamepads, the pads connected on the Host, the pad inputs sent and the rumbles received. No button or stick value and no pad name is recorded.
- When a pad does not work, the records tell the causes apart: a connection not allowed gamepads, a PC or phone that read no pad (no input), or inputs sent with no response on the desktop.
- It needs the server at 1.1.12. An older server drops these values and keeps the rest of the record.

### Host

- No change; it stays at 1.1.11.

## [1.1.11](https://github.com/totblabs/Multitap/releases/tag/v1.1.11)

### Gamepads (server, Windows app, Android app, Host)

- **A gamepad connected to the PC or the phone plays the desktop's games.** The Host plugs in a virtual Xbox 360 controller for each pad, which the desktop's games see as a real Xbox controller. Up to four pads; the game's rumble comes back to the pad in your hands.
- The Windows app reads Xbox and other XInput controllers; the Android app reads Bluetooth and USB game controllers, and during a connection the pad's B button is not Back.
- No new driver on the Host: the virtual pads are plugged in with the driver used for USB devices (installed then if it is not yet).
- With the window inactive or the app in the background, the pads are sent as released.
- It needs all four at 1.1.11.

## [1.1.10](https://github.com/totblabs/Multitap/releases/tag/v1.1.10)

### Less video while the picture is hidden (server, Windows app, Android app, Host)

- **With the Android app in the background or the screen off, the desktop's picture is sent once a second.** Until now every frame (60 fps and 15 Mbps, say) kept arriving although nothing showed it, using data and battery. Sound and the connection go on, and coming back brings every frame again at once.
- The same for the Windows app while its window is minimised.
- It needs all four at 1.1.10; with any of them older, every frame is sent as before.

## [1.1.9](https://github.com/totblabs/Multitap/releases/tag/v1.1.9)

### Host

- **Fixed: on a desktop with no mouse attached, the pointer was still not seen with the mouse free.** The 1.1.8 fix had no effect: with no mouse Windows removes the pointer itself, so there was no pointer to show. The Host now plugs in a virtual USB mouse of its own that never moves (Multitap Pointer), and Windows shows the pointer as it does in a VM.
- The virtual mouse is plugged in with the driver used for USB devices (installed then if it is not yet). A desktop with a mouse never gets it, and it goes when the Host service stops. Device Manager lists it as "Multitap Pointer".

### Server

- The Host's log in the admin console names the virtual mouse being plugged in, failing to plug in and going.

### Windows app, Android app

- No change; the number follows.

## [1.1.8](https://github.com/totblabs/Multitap/releases/tag/v1.1.8)

### Host

- **Fixed: on a PC with no mouse attached, the pointer was not seen with the mouse free.** Windows hides the pointer when no mouse is attached; it is now shown anyway. A pointer an application hides stays hidden.

### Windows app

- **Fixed: a clipboard permission the administrator widened during a connection (files allowed, say) took effect only after connecting again.** It now applies at once.

### Server, Android app

- No change; the number follows.

## [1.1.7](https://github.com/totblabs/Multitap/releases/tag/v1.1.7)

### Clipboard (server, Windows app, Host)

- **Fixed: pasting the same large copy again could close the clipboard connection.** After a paste of a large folder failed or was cancelled, pasting it again was taken by the server for a protocol error. It is now answered again, from the list already made.
- **Fixed: cancelling a paste and pasting again at once could wait 30 seconds and fail.** The cancel of the first paste reached the second one.
- **Fixed: a copy made during a paste could break that paste.** Copying something on the side you paste on (on the PC while pasting from the desktop, for example) cut the paste off; so did a new copy while the list of a large folder was being made. A paste that has begun now finishes, as with RDP; the next paste gets the newer copy.
- **Fixed: pasting a very large folder from the desktop to the PC (about 40,000 items or more) closed the clipboard connection.** The desktop now sends the list only as fast as the connection takes it.
- On a slow connection a long list no longer counts as a dead connection while it is still being delivered.
- A paste on the PC that the desktop stopped answering now says so on the card ("The desktop didn't respond in time.") instead of ending silently; with file copying turned off by the administrator, the paste says that instead of a general failure.
- While the PC makes the list of a large folder, the clipboard keeps working (it waited for the list to finish).

### Mouse lock (Host)

- Fixed: after changing the pointer's colour or scheme during a mouse-locked session, the old pointer could stay in the picture.

### Android app

- No change; the number follows.

## [1.1.6](https://github.com/totblabs/Multitap/releases/tag/v1.1.6)

### Clipboard (server, Windows app, Host)

- **A large copy gets its list when it is pasted, as with RDP.** Copying more than 256 items or 16 MB of files and folders no longer walks them: only the copy is announced. The list is made when you paste, and the card shows "Listing · N items" with the real count. A folder of more than 200,000 items is refused with the limit when you paste.
- **Fixed: pasting a large folder failed with "what you copied is no longer available".** Windows sometimes renumbers the clipboard without a new copy, and that was taken for a new copy. A paste now goes ahead while the same files are copied, for small copies too.
- Small copies (up to 256 items and 16 MB) are ready at once as before, and paste into programs that take only file paths.
- The new behaviour needs the server, the Windows app and the Host at 1.1.6; a mixed set works as before. The renumbering fix needs only the side you copy on at 1.1.6 (the Host for a copy in the desktop, the Windows app for a copy on the PC).

### Mouse lock (Host)

- **Fixed: the cursor looked small while the mouse was locked.** At a desktop scale of 150 % or 200 % it was drawn at two thirds or half of its size, and a game's large cursor (64 pixels, say) was squeezed to 32. It is now drawn at the size and hotspot Windows shows.

### Android app

- No change; the number follows.

## [1.1.5](https://github.com/totblabs/Multitap/releases/tag/v1.1.5)

### Android app

- **Fixed: sound played at first and never came back after the first pause.** Android's own Opus decoder (from Android 16 QPR2 and the updated media module) stamps its output from its first input on and ignores later timestamps. The desktop sends nothing while it is silent, so after the first pause every decoded frame looked old and was dropped without an error. The app now gives the decoder timestamps of its own and plays each frame at the desktop's time.

### Server, Windows app and Host

- No change; the number follows the Android app.

## [1.1.4](https://github.com/totblabs/Multitap/releases/tag/v1.1.4)

### Sound

- **Host**: sound capture recovers from device errors and follows a change of the default output device, instead of staying silent until the next connection. Sound is sent evenly every 10 ms. Start, failure and restart of the capture appear in the Host's log.
- **Android**: sound comes back after another app or a call has taken it, in every state (paused, waiting, lost, split screen). A failing Opus decoder is tried again instead of being given up for the whole run. Connection statistics have an audio section: what the Host sends, the decoder, audio focus, media volume and playback.

### Admin console

- A page left open during a server update switches to the new version within 30 seconds.

## [1.1.3](https://github.com/totblabs/Multitap/releases/tag/v1.1.3)

### Server and admin console

- **The connection test really tries each address** on Server access, extra addresses included, and says whether it works and why (name not found, nothing answering, not HTTPS, another server, an untrusted proxy). A main address that fails cannot be saved.
- **Confirm it yourself and save**: an address the server cannot reach from inside but that opens from outside (a router without loopback, a name known only outside) is saved after you open it from outside within 5 minutes, for example on a phone. Until then nothing changes.
- **Every save ends with its result** on the page, naming the address when the previous settings came back. The waiting screen never stays.
- **Sending the picture and sound over UDP is off by default** and is a switch on Server access. Turning it on or off restarts nothing and disconnects nobody.
- Turning on public IP following registers the address within seconds.
- The console says when the server stops answering or the sign-in has ended, and keeps what you typed (never passwords or codes) for the next sign-in.
- A saved access setting that cannot be read starts the server with only the admin console, and every page says how to fix it.
- Release notes show the console's language only.

### Install command and `multitap`

- `multitap update` without a version never goes back to an older release.
- The home page and the guide say what the install command does and how to read it before running it.

### Windows app and Android app

- **Android sound** no longer plays, then crackles and stops: it is buffered and played evenly, as in the Windows app, and comes back after another app has taken it.
- Long operations show their progress: power, Host updates, opening the Console, signing out, loading the list, USB and folders; an Android update shows how much is copied.

### Host

- The first install shows what each step does and its progress; installing WinFsp fills a real progress bar.
- The installer is smaller and carries no paths of the machine it was built on.

### Updating

From 1.1.1, see the note under 1.1.2. 1.1.2 was published as a pre-release only.

## [1.1.2](https://github.com/totblabs/Multitap/releases/tag/v1.1.2)

### Server and admin console

- **Server access saves by itself.** On a Docker install, the page saves the addresses apps and Hosts use, the primary address, the trusted proxy IPs and public IP following. The server applies them by restarting itself, which ends open connections, and goes back to the previous settings if it cannot reach itself at the new address.
- **Public IP following** is a switch on the same page. While it is on, the server checks its public IP every 10 minutes and follows it.
- **The proxy is on this server**: one button adds a reverse proxy on the same machine to the trusted proxy IPs.
- **Restart server** is a button under Server info on a Docker install.
- **UDP screen streaming works on a default install** once UDP 18481 reaches the server. Apps and Hosts send to the address they connected on, and Server access shows whether UDP is in use.
- The memory limit of the server container can be raised with `MULTITAP_MEMORY_LIMIT` in `/opt/multitap/.env` (512 MB by default).
- The admin console says a Host request expires after 10 minutes, which is what the server does.

### Install command and `multitap`

- The installer's question about HTTP from outside now defaults to **No**.
- The installer keeps a log in `/var/log/multitap-install.log`, without the setup code. In a Proxmox CT with nesting off, it stops before installing anything and names the setting to change.
- `multitap status` shows whether the server answers, the last lines of its log and the free disk space, and warns when less than 2 GB is free.
- `multitap update` checks the new version through the container's own health check, so it works whatever address or port the server uses. It keeps the 3 most recent pre-update copies and removes server images older than the previous version.
- `multitap address` changes the same settings as Server access when the admin console cannot be reached: up to four HTTPS and four HTTP addresses, the primary address and the trusted proxies.

### Windows app

- Fixed: changing from one HTTP server address to another said the address was not saved and kept using the old one.
- With no server saved yet, the sign-in card opens on the server address form.

### Android app and Host

- No visible changes.

### Updating from 1.1.1

Run `sudo multitap update` on the server. If the server's only address is HTTPS, or its port is not 18480, run it twice: the 1.1.1 command checks the new version at `http://<server IP>:18480` and cannot see such a server, so the first run goes back to 1.1.1, but it installs the new command, and the second run succeeds.

## [1.1.1](https://github.com/totblabs/Multitap/releases/tag/v1.1.1)

First public release.
