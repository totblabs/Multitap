# 2. Server install and basic setup — Docker

[Contents](README.md) · [Previous: Introduction](01-introduction.md) · [Next: Connect Proxmox](03-connect-proxmox.md) · [한국어](../02-서버-설치.md)

**Goal:** sign in to the Multitap admin console from the browser on your PC.
One command installs it on Debian or Ubuntu. At first, focus on connecting one Windows VM.

## First: where do you type the commands?

| Label in this chapter | What to open |
| --- | --- |
| **Linux server** | A terminal connected to the server over SSH, or in Proxmox that **CT or VM → Console** |
| **PC** | The browser on the PC you are using now |

**The Shell of a Proxmox node and the Console of a Linux CT or VM are different.**
Run the commands in this chapter **inside the Linux where you install Multitap**.

Copy only what is inside a command box. Do not type labels such as `sh`.
When you type a password, nothing (not even `*`) is shown, but it is being typed.

> `10.0.0.10` and `vdi.example.com` are examples.
> Replace them with your server's IP and domain. Do the first setup on an internal network you trust.

## Install the server

First install and start the Multitap server on Linux.
When the server runs, go on to **Basic server setup** below.

### Step 1. Prepare Linux — a Proxmox CT or VM

**Where: the Proxmox web UI, then the Linux server**

1. Prepare a CT or VM with Debian or Ubuntu. Only x86_64 (amd64) works.
   A VM is the safer place and a CT the lighter one. Docker inside a CT depends on the CT's nesting feature, and there have been cases where containers in a CT stopped starting after a Proxmox or Docker update. For a server you keep for a long time, a VM is recommended.
2. For a CT, turn on **nesting** under **CT → Options → Features** in Proxmox. For an unprivileged CT, turn on **keyctl** too.
3. If you changed the features, restart the CT.
4. Sign in to Linux as root, or with an account that can use `sudo`.
5. Check that the server can reach the internet. It downloads Docker and the server image.

You do not need to install Docker first. The install command installs it if it is missing and uses it if it is there.

**Check:** you are signed in to a Linux terminal, on a VM or on a CT with nesting turned on.

### Step 2. Run the install command

**Where: Linux server**

The command runs inside the CT or VM, never on the Proxmox node.

| Where you install | How to get in |
| --- | --- |
| CT | `pct enter <CT number>` in the Proxmox node's **Shell**, or **CT → Console** in the Proxmox web UI |
| VM | **VM → Console** in the Proxmox web UI. If pasting does not work there, type the line or SSH to the VM's IP. `ip -4 a` in the Console shows the IP |

If SSH says `Permission denied (publickey)`, the VM accepts keys only, as VMs made from a cloud image do. Use the Console.

Run this one line in a root terminal.

```sh
curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | MULTITAP_LANG=en bash
```

If you are not root, add `sudo`.

```sh
curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | sudo env MULTITAP_LANG=en bash
```

If you see `curl: command not found`, install it with `apt update && apt install -y curl` and run again. If you are not root, put `sudo` in front of both commands.

`MULTITAP_LANG=en` shows the messages in English (`ko` for Korean). Without it, the install follows the Linux language setting: Korean when it is Korean, English otherwise.

What the command does, and how to read it before you run it, is in [What this command does](#what-this-command-does) below.

The install runs in four steps: **[1/4] Checking the system → [2/4] Docker → [3/4] Server files and image → [4/4] Starting the server**.
On the way it asks two questions. Pressing Enter means **yes** to the first and **no** to the second.

| Question | What it means |
| --- | --- |
| **Let apps and Hosts connect at this address?** `http://<server IP>:18480` | Apps and Hosts on the same network connect at this address. It is not encrypted. |
| **Also allow HTTP connections from outside? (router port forwarding)** `http://<public IP>:18480` | With `y`, the server's public IP is registered as an HTTP address, and the server follows it within 10 minutes when it changes. It is not encrypted over the internet, so an HTTPS address is recommended from outside. You can turn it on later in **Server access**. Asked only when a public IP is found. |

The two questions only give a new server its first addresses. After that, addresses are changed in **Server access** in the admin console.

**Check:** at the end you see **✔ Installed** and three lines.

```text
  ① Open in a browser   http://10.0.0.10:18480
  ② First sign-in       admin / admin
  ③ Setup code          (one-time code)
```

You use these three lines in **Basic server setup** next. Do not send the setup code to anyone.

<details>
<summary>The install stopped — ✖ Stopped</summary>

Under **✖ Stopped** you see the reason and what to do next.
The install log is kept in `/var/log/multitap-install.log` (readable by root only; the setup code is not written to it).

- **Run this as root**: use the `sudo` command above.
- **Nesting is off in this CT…**: checked before anything is installed. Turn on nesting as the message says (and keyctl for an unprivileged CT), restart the CT, and run the same command again.
- **A command failed in step…**: the failed command and the last lines of the log are shown together. The reason is in those lines.
- **Could not download the server image**: the reason Docker gives (access denied, DNS, a timeout and so on) is shown as it is.
- **this distribution's packages have no 'docker compose' (Compose v2)**: check that Docker's own repository (`download.docker.com`) can be reached, then run the command again.
- **Multitap installs only on Debian or Ubuntu**: on another distribution, use the Compose method in the [Docker deployment guide](../../docker-deployment.md).
- **Docker could not run a container in this CT**: turn on nesting from step 1 (and keyctl for an unprivileged CT), restart the CT, and run the same command again.
- **The server did not answer within 3 minutes**: check the log shown above. To see it again: `sudo multitap logs`.

Running the same command again is safe. A finished install is left unchanged, and an install that stopped half way continues with the settings and data already there.

</details>

<details>
<summary>Details: install options</summary>

- It installs the server command `multitap`. The language of the messages is kept in `.env`, and `multitap` answers in the same language.
- The data (accounts, assignments, Host registrations, secret key, downloads) is in the Docker volume `multitap-broker-data`.

Settings you can put in front of `bash` (like `MULTITAP_LANG=en`):

| Setting | What it does |
| --- | --- |
| `MULTITAP_VERSION=1.2.3` | Installs this version instead of the newest |
| `MULTITAP_LAN_HTTP=0` | Answers **no** to the first question without asking (`1` is **yes**) |
| `MULTITAP_PUBLIC_HTTP=1` | Registers the HTTP address from outside without asking (`0`: does not register it, without asking). Run without a terminal, it is registered only with `1` |

To use Docker Compose yourself, or to install on another Linux, see the [Docker deployment guide](../../docker-deployment.md).

</details>

#### What this command does

The install command downloads one file, [`deploy/docker/install.sh`](../../../deploy/docker/install.sh), and runs it with bash. This is everything that file does.

- **It checks first.** It runs as root, on Debian or Ubuntu, on x86_64; in a CT, nesting is on. If any of these fails, it stops before installing anything. On a server that is already installed, it changes nothing and ends.
- **It prepares Docker.** If `docker compose` works, the Docker you have is used as it is. Otherwise it adds Docker's own repository (`download.docker.com`) to apt and installs from there, or from the Debian or Ubuntu `docker.io` package when that repository cannot be used. It also installs `ca-certificates`, `curl` and `gnupg`, and sets Docker to start at boot.
- **It downloads.** It looks up the newest release on GitHub, then downloads `compose.yaml`, `.env.example` and the `multitap` command from that release's tag (`v<version>`). The server image, `ghcr.io/totblabs/multitap-broker`, comes from GitHub's container registry.
- **It creates** `/opt/multitap` (`compose.yaml`, `.env` and a `backups` folder), `/usr/local/bin/multitap`, the Docker volume `multitap-broker-data`, one server container, and the install log `/var/log/multitap-install.log` (readable by root only). If Docker came from Docker's repository, that repository's apt settings (`/etc/apt/sources.list.d/docker.list` and its key) stay too.
- **It opens ports.** Docker opens TCP 18480 (admin console, apps, Hosts) and UDP 18481 (screen stream).
- **It asks two questions**, the two in the table above. If you answer yes to the first, it asks `1.1.1.1` (Cloudflare) once for the server's public IP, for the second question.
- **What it does not do.** It creates no service or scheduled job other than Docker and the server container. It does not touch login accounts, SSH keys or SSH settings. It sends no usage data, and neither does the server it installs. Where the server connects on its own is in [What the server contacts on its own](#what-the-server-contacts-on-its-own) below.
- **Removing it.** `sudo multitap uninstall` removes the server container and the `multitap` command. The data volume, `/opt/multitap`, Docker and the downloaded server image stay.

#### Read it first

The one-line command runs the script as soon as it arrives. To read it first, download it to a file, read it, then run it. It installs the same way and asks the same two questions.

```sh
curl -fsSLo install.sh https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh
less install.sh
MULTITAP_LANG=en bash install.sh
```

If you are not root, run the last line as `sudo env MULTITAP_LANG=en bash install.sh`. Press `q` to close `less`.
The script itself comes from `main`; the files it downloads come from the tag of the release it installs. All of its work starts from the script's last line, so if the one-line command is cut off while downloading, nothing is installed.

Multitap is open source under AGPL-3.0, and the install script, the server, the apps and the Host can all be read in this repository.

- The server image is built from the source at the release tag by a public [GitHub Actions workflow](../../../.github/workflows/broker-image.yml) and pushed to `ghcr.io`. The app and Host installers inside the image are the files attached to that release. To see which commit an image was built from: `docker image inspect --format '{{index .Config.Labels "org.opencontainers.image.revision"}}' ghcr.io/totblabs/multitap-broker:<version>`.
- The server container runs as a user other than root, on a read-only file system, with every Linux capability dropped (`compose.yaml`).
- The new-version notice in the admin console is signed with the project's Ed25519 key, and the server checks the signature before showing it. `sudo multitap update` pulls the server image by its version; the image itself carries no signature.
- The Windows installers are not signed with a purchased code signing certificate, so Windows asks you to confirm ([Troubleshooting](09-troubleshooting.md#defender-or-smartscreen-blocks-the-install)).

| Server command | What it does |
| --- | --- |
| `sudo multitap status` | Status, version and address |
| `sudo multitap logs` | The server's log (Ctrl+C to stop) |
| `sudo multitap code` | Shows the first-setup code again |
| `sudo multitap address` | Shows the addresses apps and Hosts use. They are changed in **Server access** |
| `sudo multitap address <address>…` | Changes the same address settings on the server when the admin console cannot be reached |
| `sudo multitap public-ip [on\|off]` | Turns public IP following on or off (the same switch as in **Server access**). Without on/off: shows the state |
| `sudo multitap update [version]` | Backs up, then updates to the new version ([chapter 8](08-updates.md#server--installed-with-the-install-command)) |
| `sudo multitap backup` | Copies all data to `/opt/multitap/backups` |
| `sudo multitap restore <file>` | Puts a backup back ([backup and restore below](#backup-and-restore--server-data-in-one-file), [chapter 8](08-updates.md#restore-from-a-backup)) |
| `sudo multitap restart` · `stop` · `start` | Restart, stop, start. A restart also works from **Server info → Restart server** in the admin console |
| `sudo multitap uninstall` | Stops and removes the server. The data stays |

## Basic server setup

The server is installed. Now set up the **admin account → addresses**, in that order.
Hosts and clients can connect only after this setup.

### Step 1. Set up the first admin account

**Where: the browser on your PC**

1. Enter the address from ① on the install screen (`http://<server IP>:18480`) in the address bar.
2. Enter `admin` as the **username**.
3. Enter `admin` as the **password**.
4. Click **Sign in**.
5. Enter the code from ③ on the install screen as the **setup code**.
6. Enter a **new admin username**.
7. Enter a **new password** of 10 characters or more.
8. Enter the same password in **Confirm password**.
9. Click **Change account and sign in again**.
10. Sign in with the new admin account.

To see the setup code again, run this on the **Linux server**.

```sh
sudo multitap code
```

**Check:** you enter the admin console with the new account instead of `admin / admin`.
You must change both the username and the password.

> Until the first setup is done, the setup code changes every time the server restarts.
> `multitap code` shows the newest code. Do not send the code, or a log that contains it, to anyone.

### Step 2. Check the address Hosts and apps use

Host and client connections and downloads work only at the addresses in **Server access**.
The addresses, the primary server address, the trusted proxy IPs and public IP following are all set on that page, and the server applies them itself.

#### Same home or office network — already applied at install

If you answered **yes** (Enter) to the first question during the install, `http://<server IP>:18480` is already applied.

**Where: the admin console on your PC**

1. Open **Server access** in the left menu.
2. Check that the **HTTP** address shows `http://<server IP>:18480`.

**Check:** opening `/get/host` at that address on your PC starts the Host download.

If you answered **no**, or want to change the address, change it on the same page.

1. Enter or correct the addresses. Up to 4 HTTPS and 4 HTTP addresses.
2. Click **Connection test**, then **Save changes**.
3. The server ends open connections and starts again with the new addresses. The admin console drops for a moment too.
4. The server checks that it can reach itself at the new address. If it cannot, the previous settings come back and the page says so.

An HTTP address made of the server's IP must use the port Docker opens (`18480`). The page shows that port.

> **HTTP is not encrypted.** Sign-in details and screens travel as they are.
> If you use it from the internet often, use a domain and HTTPS.

<details>
<summary>When the admin console cannot be reached — setting the address on the server</summary>

When a wrong address was saved and the admin console does not open, or when the port Docker opens has to change, run this on the **Linux server**.
It reads and writes the same settings as the admin console.

```sh
sudo multitap address
sudo multitap address http://10.0.0.10:18480
```

Without an address it shows the current ones. With addresses, they replace all current ones; separate several with spaces.
With an HTTP address, the port Docker opens follows that address's port.
While public IP following is on, the address it follows stays in the list. To drop it, run `sudo multitap public-ip off` first.
The server restarts, and goes back to the previous settings if it cannot start with the new addresses.
`sudo multitap address --help` shows how to set the primary address and the trusted proxies as well.

</details>

Ports published by Docker can bypass firewall rules such as UFW.
Also check what your router and the Proxmox firewall allow ([Docker documentation](https://docs.docker.com/engine/install/ubuntu/#firewall-limitations)).

#### Connecting from outside — public IP and port forwarding

The simple way in from outside, without a domain. It works, but it is HTTP and not encrypted. For long-term use from outside, the HTTPS way below is recommended.

**Where: the admin console on your PC, and your router**

1. In **Server access**, turn on **Follow the public IP** under the HTTP address and save.
   The server finds its public IP, registers it as an HTTP address, checks it every 10 minutes and follows it when it changes.
   If you answered `y` to the second question during the install, it is on already.
2. In your router settings, forward **TCP 18480** and **UDP 18481** to the Linux server's IP.
3. If the server has a firewall, open the same two ports.

Until you forward the ports, nothing comes in from outside. The same network keeps working.
Public IP following needs one other HTTP address, and the public IP address cannot be the primary server address.

The same switch can be turned on the server: `sudo multitap public-ip on`, `sudo multitap public-ip off`. Turned off, the registered address stays.

#### Connecting from the internet — HTTPS

You need a domain, a valid certificate and an HTTPS reverse proxy.
A reverse proxy is a program that accepts HTTPS connections and passes them on to the Multitap server.

<details>
<summary>HTTPS setup — when the proxy runs on the same machine as the Linux server</summary>

1. Point the domain's DNS at a server address reachable from outside.
2. Set up the proxy's HTTPS certificate.
3. Set the proxy's upstream address to `http://127.0.0.1:18480`.
4. Pass on `Host`, `X-Forwarded-Proto: https` and the WebSocket `Upgrade` and `Connection` headers.
5. Set the proxy's WebSocket read timeout long enough.
6. Allow TCP 443 to the proxy in the server firewall and, if you use a router, forward outside TCP 443 to the proxy.

A complete nginx example is in the [Docker deployment guide](../../docker-deployment.md).

Continue in the **admin console on your PC**.

1. In **Server access**, enter the domain in the HTTPS address field, for example `https://vdi.example.com`.
2. Under **Trusted proxy IPs**, click **The proxy is on this server**. The address the proxy arrives from is added to the list.
3. Click **Connection test**, then **Save changes**.
4. Sign in from the browser on your PC at the real HTTPS address.

Replace the domain in `https://vdi.example.com` with your real domain.
Hosts and apps you already use must also test the connection at the new address and save it.
If you use HTTPS only, delete the HTTP addresses and turn off public IP following.

If the proxy is on another machine, do not use the `127.0.0.1` upstream address.
Forward to the broker's real address, and enter the proxy's real address, as the broker sees it, under **Trusted proxy IPs**.

To use HTTPS only and keep the Docker port open only inside the server, change `/opt/multitap/.env` on the **Linux server** to `MULTITAP_PUBLISH_PORT=127.0.0.1:18480` and run `sudo multitap start`.
To use TLS directly without a proxy, see the [Docker deployment guide](../../docker-deployment.md).

</details>

### Step 3. Screen streaming over UDP — forward one port on the router

UDP is a faster path for screen and sound.
Apps and Hosts send to UDP port 18481 of the server address they connected on. Nothing has to be set on the server.

1. Allow UDP 18481 in the server firewall.
2. For access from outside, forward UDP 18481 on your router to the Linux server.

Where UDP does not arrive, the screen uses the WebSocket path. It is a little slower, but it connects.
UDP is not used in these cases.

- Docker's UDP port is open only inside the server
- the address is an IPv6 literal
- the HTTPS address points at a proxy or tunnel on another machine that does not forward UDP

In the last case you can name the address that receives UDP in `/opt/multitap/.env`. Both lines are needed; save, then run `sudo multitap start`.

```dotenv
MULTITAP_UDP_HOST_RELAYS=udp.example.com:18481
MULTITAP_UDP_CLIENT_RELAYS=udp.example.com:18481
MULTITAP_UDP_MEDIA=on
```

> TCP 443 and UDP 18481 are separate. Forward UDP on its own, apart from the HTTPS proxy.

**Check:** **Screen streaming over UDP** in **Server access** shows the current state. After you connect by streaming, check the UDP/TCP path under **Connection quality**.

### What the server contacts on its own

Without a request from a user, the server connects out only to these.

| Where | When and why | How to turn it off |
| --- | --- | --- |
| `github.com` (the project's releases) | While the admin console is open, at most once an hour: the signed information about a new version | `MULTITAP_DOCKER_UPDATE_FEED=off` in `/opt/multitap/.env` |
| `1.1.1.1` (Cloudflare's address echo) | Only while **Follow the public IP** is on, every 10 minutes: the server's public IP | Turn off public IP following in **Server access** |
| The Proxmox you connected | For the desktop list, power and Console | Remove the Proxmox connection |

The install command and `multitap update` download files when you run them: from `github.com`, `ghcr.io` (the server image) and `download.docker.com` (when Docker is missing).
The install command looks up the public IP once, for the second question.

### Memory the server uses

The server container uses up to 512 MB of memory. Giving the CT or VM more memory does not change that limit.
If many people connect at once and it is not enough, put `MULTITAP_MEMORY_LIMIT=1g` (for example) in `/opt/multitap/.env` and run `sudo multitap start`.

### Backup and restore — server data in one file

In the admin console, **Settings → Backup and restore** downloads the server data as one file.
If you lose the server or move to a new one, this file brings back accounts, assignments, Host registrations and the Proxmox connection.

**Make a backup file**

1. Click **Make backup file** and enter a backup password twice. It is 12 characters or more.
2. Enter your account password. With two-step sign-in, also enter the code from your authenticator app.
3. Keep the downloaded `.mtbk` file and the backup password in different places.

The file is locked with the backup password. If you forget the password you cannot restore, and the server cannot tell it to you.

| Included | Not included |
| --- | --- |
| Accounts and passwords, two-step sign-in | Who is signed in right now |
| Desktop assignments and sharing settings | Power actions in progress, Host connection codes |
| Host registrations, Proxmox connection | Client and Host diagnostics (7 days' worth) |
| Admin logs, usage history | `.env`, TLS certificate files, download files |

**Restore**

1. On the same page, add the backup file and the backup password, and click **Check file**.
   Nothing changes yet. It shows when and on which server the backup was made, and how many accounts, desktops and Hosts it holds.
2. Check the contents and click **Restore this backup**.

Restoring replaces the current data with the backup's, and everyone connected is disconnected.
After the restore, sign in with an admin account and password from the backup.
The server keeps the state from before the restore, and goes back to it if the restarted server is not healthy.

- The current Server access settings stay. The backup's addresses apply only when you turn on **Also restore Server access**.
- Registered Hosts reconnect by themselves if the server address is the same. If the address changed, change it on each Host.
- A newly installed server can start with **Restore from backup** on the first admin account screen. It needs the setup code (`sudo multitap code`).

**Restoring on a Docker install**

On a server installed with this chapter's install command, the admin console checks the file and the server restores it with a command.
The console's restore page shows this command too.

1. Copy the backup file to the server. For example: `scp multitap-backup-20261110-0730.mtbk root@<server IP>:/root/`
2. Run this on the **Linux server**.

   ```sh
   sudo multitap restore /root/multitap-backup-20261110-0730.mtbk
   ```

3. Enter the backup password. What you type is not shown.
4. Check when it was made, the server version and the numbers of accounts, desktops, assignments and Hosts, then answer `y` to **Restore? [y/N]**.
   Nothing changes until then. Enter alone cancels.

After `y` the server stops for a moment, the whole current data volume is kept as `/opt/multitap/backups/…-before-restore.tar.gz`, the backup is restored and the server starts again.
If the restore or the start fails, it puts that safety backup back and shows the reason.

**Check:** **✔ Restored.** appears, and you can sign in with an admin account from the backup.

- The addresses and the download files stay this server's own. Addresses are changed in **Server access**.
- A newly installed server is restored the same way once the install has finished.
- A backup made by a newer server version is not restored. Update first with `sudo multitap update`.
- A data volume copy made with `sudo multitap backup` is put back with the same command ([chapter 8](08-updates.md#restore-from-a-backup)).

## Before the next chapter

- [ ] You signed in with the new admin username and password.
- [ ] **Server access** shows the address Hosts and apps use.
- [ ] `/get/host` at that address starts a download.

If all of this works, go to [chapter 3: Connect Proxmox](03-connect-proxmox.md).
**is running and answering** in `sudo multitap status` is about the server; it does not mean a desktop screen is ready.

> Accounts and desktop assignments are stored in the Docker data volume.
> `multitap uninstall` keeps the data too. Do not run `docker compose down -v` or delete the volume.

When you remove Multitap completely, also remove the dedicated user and role it created in Proxmox. `multitap uninstall` does not touch Proxmox.
Run this in the **Proxmox node's Shell**. The names are listed under **Datacenter → Permissions → Users** and **Roles** in the Proxmox web UI.

```sh
pveum user delete multitap-<name>@pve
pveum role delete Multitap-<12 characters>
```

Deleting the user also removes its API token and its permissions.
