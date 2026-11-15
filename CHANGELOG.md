# Changelog

[한국어](CHANGELOG.ko.md)

What changed for the people who install and use Multitap. A release is numbered after the server; 1.1.1 to 1.1.11 carry the Windows app, the Android app and the Host at the same number. 1.1.12 is the server and the two apps; the Host stays at 1.1.11. 1.1.13 is the server only; the apps stay at 1.1.12 and the Host at 1.1.11. 1.1.14 is the server at 1.1.14 with the Windows app and the Android app at 1.1.13; the Host stays at 1.1.11. 1.1.15 brings the Host to 1.1.13 and 1.1.16 to 1.1.14; the server is unchanged in both and the apps stay at 1.1.13. 1.1.17 is the server; the Host stays at 1.1.14. 1.1.18 is the server with the Windows app and the Android app at 1.1.14; the Host stays at 1.1.14. 1.1.19 brings the Android app to 1.1.15; the server is unchanged, the Windows app and the Host stay at 1.1.14. 1.1.20 brings the Host to 1.1.17; the server is unchanged, the Windows app stays at 1.1.14 and the Android app at 1.1.15. 1.1.21 brings the Host to 1.1.18; the server is unchanged, the Windows app stays at 1.1.14 and the Android app at 1.1.15. 1.1.22 brings the Host to 1.1.20; the server is unchanged, the Windows app stays at 1.1.14 and the Android app at 1.1.15. 1.1.23 brings the Host to 1.1.21; the server is unchanged, the Windows app stays at 1.1.14 and the Android app at 1.1.15. 1.1.24 is the server, the Windows app 1.1.15 and the Android app 1.1.16; the Host stays at 1.1.21. 1.1.25 is the server only. 1.1.26 brings the Host to 1.1.22; the server is unchanged. 1.1.27 is the Windows app 1.1.16 and the Android app 1.1.17; the server is unchanged and the Host stays at 1.1.22. Installers are on the [Releases](https://github.com/totblabs/Multitap/releases) page.

## [1.1.27](https://github.com/totblabs/Multitap/releases/tag/v1.1.27)

### Desktop list (Windows app, Android app)

- The "Local PC" badge on a desktop card stands out: a blue tint and a PC icon set it apart from the operating system line.

### Server

- No change; the number follows the apps.

## [1.1.26](https://github.com/totblabs/Multitap/releases/tag/v1.1.26)

### Sound (Host)

- **While a local PC (a PC with its own monitor and speakers) is streamed, its speakers are muted and the sound plays only in the app.** They come back when the connection ends, at the same volume; a PC that was already muted stays as it was. Unmuting at the PC during a connection is undone within a second. VMs are unchanged.

### Server

- No change; the number follows the Host.

## [1.1.25](https://github.com/totblabs/Multitap/releases/tag/v1.1.25)

### Server

- Prepares muting a local PC's own speakers while it is streamed: the server tells a Host that supports it that the connection is to a local PC. Nothing changes with the Host 1.1.21 in this release.
- The admin console's Host records name the speaker-muting steps of a Host that supports it.

## [1.1.24](https://github.com/totblabs/Multitap/releases/tag/v1.1.24)

### Connecting (Windows app, server)

- **The Windows app no longer connects to the desktop whose Host runs on the same PC.** That connection switched the PC's own monitor off, moved every window (the app included) onto the invisible virtual screen and could repeat keystrokes endlessly. The card now reads "This desktop is the PC you're using" and the app refuses to connect; connect to it from another device. The server tells the app which desktop that is without sending the PC's identifier.

### Host bulk update (Android app, Windows app)

- The Android app can update every Host at once too: the update button at the top → **Host bulk update**.
- In both apps the bulk update offers "Only N idle" to pick the desktops nobody is connected to. Connected desktops can be chosen as well; their connection ends with "Disconnected for a Host update" and the app waits for the update.

### USB (Windows app)

- **Fixed: a shared USB drive always read "USB drive · no drive letter" with a Disk Management warning.** While a drive is shared, the PC no longer sees its volumes; the panel now keeps the name and details the PC showed before sharing.

## [1.1.23](https://github.com/totblabs/Multitap/releases/tag/v1.1.23)

### Display (Host)

- **Fixed: with Host 1.1.20 some PCs and VMs (with several display adapters) showed "The virtual display needs repair" and could not be connected to.** The Host turned the virtual screen off at rest and then did not recognise that state. It now recognises it there too, and where it cannot, it leaves the virtual screen on.

### Server

- No change; the number follows the Host.

## [1.1.22](https://github.com/totblabs/Multitap/releases/tag/v1.1.22)

### USB (Host)

- **Fixed: a USB 3.0 device (a SanDisk Ultra USB 3.0, say) connected to the desktop did not start, so its drive never appeared.** The USB hardening of 2026-11-12 also blocked a request USB 3.0 devices need (the BOS descriptor). It now reaches the device; the parts of the answer that could ask the desktop for another driver (Microsoft OS 2.0) are still removed.

### Display (Host)

- **Fixed: a PC or VM with a display of its own could not change its number of virtual monitors while nobody was connected, so connecting with a different number failed** ("Can't prepare a screen of this size right now", or "The virtual display isn't ready"). A new Host starts with two virtual monitors, so the Android app and single-screen connections to it failed. Changing the number at rest now keeps the virtual screens off and leaves your own monitors exactly as they were, including which one is the main monitor.
- **Fixed: when Windows switched the Multitap virtual screen on again while nobody was connected** (a monitor waking from sleep or plugged back in, a dock, Win+P), the PC could not be connected to until the Host restarted. The Host now switches it off again within seconds.
- Windows now really remembers the layout without the Multitap virtual screen (it never did before), so it stops extending onto it by itself.

### Server

- No change; the number follows the Host.

## [1.1.21](https://github.com/totblabs/Multitap/releases/tag/v1.1.21)

### Display (Host)

- **Fixed: with Host 1.1.17, a PC with a monitor could not be connected to while it was at rest** ("Can't prepare a screen of this size right now"). At rest the Host reported its virtual screen in a place a connection never uses, and the server refuses a PC whose first virtual screen is not the main one. It now reports the screen where a connection puts it (the main screen, top left). VMs were not affected.

### Server

- No change; the number follows the Host.

## [1.1.20](https://github.com/totblabs/Multitap/releases/tag/v1.1.20)

### Display (Host)

- **Fixed: on a PC with a monitor (or a VM with a display of its own), the Multitap virtual screen stayed on as an invisible second screen while nobody was connected.** The Host switched it on again by itself after every connection and restart. It is now off at rest and comes on only for a connection; the Host stays ready to connect in between. A VM without a display of its own keeps it on as before.
- Host settings shows "N virtual monitors, on during a connection" for it.

### Server

- No change; the number follows the Host.

## [1.1.19](https://github.com/totblabs/Multitap/releases/tag/v1.1.19)

### Android app

- **Fixed: "Small" said it couldn't save the card size setting.** The size you choose is remembered now.
- **Card size.** The default card is the earlier "Small" size, and "Small" makes it smaller still: two per row on a phone, more on a tablet. The buttons on a card get smaller with it.
- **Fixed: the screen scale, resolution and orientation chosen in a remote session, and the side the tool button rests on, were not remembered** for the next connection.

### Server

- No change; the number follows the Android app.

## [1.1.18](https://github.com/totblabs/Multitap/releases/tag/v1.1.18)

### Desktop list (server, Windows app, Android app)

- **Groups.** Make groups of your own and the list shows them as sections, each with its title and card count; clicking a title folds it. Cards in no group gather in "Other" at the end. A card's ⋯ menu puts it in a group, takes it out or makes a new group; a section's ⋯ menu renames, moves or deletes the group (its cards go to "Other"). Groups belong to each account and are kept on the server, so every PC and phone of the account shows the same. Up to 30 groups.
- **Small cards.** "Small" shows the same cards at a smaller size, more per row; it is remembered on each device.
- It needs the server at 1.1.18 and the apps at 1.1.14. With an older server the list stays as it was.

## [1.1.17](https://github.com/totblabs/Multitap/releases/tag/v1.1.17)

### Monitoring (server)

- **Fixed: a deleted desktop stayed in "Connection time by desktop", and a VM added again under a number used before carried the earlier one's connections.** The chart now lists only desktops that exist and counts each from its current Host registration. Records are still kept for two years: the by-user chart, the totals and the sign-in and connection list keep every connection.

## [1.1.16](https://github.com/totblabs/Multitap/releases/tag/v1.1.16)

### Display (Host)

- **Fixed: after a connection ended, a PC with a monitor kept an invisible second screen.** The virtual screen made for the connection stayed on beside the monitor, extending the desktop. It now goes off when the connection ends and the original configuration comes back; it stays on only for a VM without a display of its own.

### Server

- No change; the number follows the Host.

## [1.1.15](https://github.com/totblabs/Multitap/releases/tag/v1.1.15)

### Gamepads (Host)

- **Fixed: a button or trigger held when the connection dropped for a moment could stay held on the desktop.** While the connection is away the Host releases its virtual pads, and it sends the rumble again when the connection is back.
- A pad input that arrives just after a session ended no longer plugs a pad in again, and plugging and unplugging a pad fast no longer piles up work. A rare memory error is fixed.

### USB sharing (Host)

- A real wired Xbox 360 controller shared over USB is no longer taken for a virtual pad when its start is checked.
- Removing a device no longer unplugs a port the virtual mouse or a pad uses.

### Host settings

- **Fixed: after connecting to a server, the window stayed on "connecting" until it was refreshed.** It reads the status while it is visible, not only while it is in front, and every second while the connection changes. While the server changes it says "connecting" instead of a failure, and an expired or rejected request shows at once.
- The Korean input fix of the Windows app 1.1.13 applies to the Host settings' password fields too.

### Server

- No change; the number follows the Host.

## [1.1.14](https://github.com/totblabs/Multitap/releases/tag/v1.1.14)

### Starting a PC (server)

- **Fixed: on a server installed with Docker, starting a local PC was recorded as sent and failed four minutes later.** Docker's network does not pass the wake signal on to the PCs' network, so the server now says at once that it cannot send it. Proxmox VMs start as before.

### Host log (server)

- A Host that was deleted and added again starts its log in the admin console fresh: events of its earlier registration no longer come back. Sign-in and connection records are kept as before.

### Admin console (server)

- **Fixed: scrolling down a page jumped back up a few seconds later.** The page's automatic refresh gave the focus back to the page title and the browser scrolled to it; it now keeps the page where it is.

### Copy and paste permission (Windows app, Android app)

- **Fixed: copy and paste permissions an administrator widened during a connection (files allowed, say) applied only after connecting again.** Now only the copy and paste channel opens again with the new permission; the screen stays connected, and a copy under way finishes first.
- It needs the server at 1.1.13 or later and the app at 1.1.13 or later. A connection that started with copy and paste all off gets it from the next connection.

### Gamepads (Windows app, Android app)

- **Fixed: right after connecting, the virtual pad was plugged in only on the first press, so a short first press was lost.** It is plugged in when the first picture comes.
- **Fixed: a pad kept vibrating when the game stopped the rumble while the connection was frozen for a moment.** The motors stop while it is frozen, and every pad's state and the latest rumble come again when it is back.
- **Fixed (Windows app): 30 minutes of play with only a gamepad signed you out for inactivity.** With a Multitap window in front, buttons, triggers and stick movement count as input; a resting stick's small drift does not.
- Only the window in front drives the rumble: a game in a window behind no longer shakes the pad in your hands, and a window that ends no longer stops the rumble of the game you are playing.
- Android: a TV remote or a keyboard is not taken for a gamepad (only a controller with sticks or the A button is); PlayStation-style triggers send how far they are pulled; nothing held stays held after you leave the app and come back; rumble no longer stops after 60 seconds.

### Less video while the picture is hidden (Windows app, Android app)

- **Fixed: a hidden-picture notice lost while the connection was frozen, or just before it opened, could leave a visible picture at one frame a second.** It is sent again when the connection is back and when the first picture comes.

### Korean input (Windows app)

- **Fixed: Korean could not be typed in the sign-in ID field until the app was quit.** When a focused password field went away (signed out with the two-step verification window open, for example), Korean input stayed off for the whole window.

### USB devices on your PC (Windows app)

- **Fixed: "Install USB driver" always failed.** The app did not include the driver's installer. It does now, and a driver that is installed but not running yet (a restart is needed, for example) is shown as such instead of "no devices".

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
