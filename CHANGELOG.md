# Changelog

[한국어](CHANGELOG.ko.md)

What changed for the people who install and use Multitap. A release is numbered after the server; 1.1.1 and 1.1.2 carry the Windows app, the Android app and the Host at the same number. Installers are on the [Releases](https://github.com/totblabs/Multitap/releases) page.

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
