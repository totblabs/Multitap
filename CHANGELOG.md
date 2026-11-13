# Changelog

[한국어](CHANGELOG.ko.md)

What changed for the people who install and use Multitap. A release is numbered after the server; 1.1.1 to 1.1.4 carry the Windows app, the Android app and the Host at the same number. Installers are on the [Releases](https://github.com/totblabs/Multitap/releases) page.

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
