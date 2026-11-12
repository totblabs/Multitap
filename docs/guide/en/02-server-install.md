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

> `192.0.2.10` and `vdi.example.com` are examples, not addresses you can reach.
> Replace them with your server's IP and domain. Do the first setup on an internal network you trust.

## Install the server

First install and start the Multitap server on Linux.
When the server runs, go on to **Basic server setup** below.

### Step 1. Prepare Linux — a Proxmox CT or VM

**Where: the Proxmox web UI, then the Linux server**

1. Prepare a CT or VM with Debian or Ubuntu. Only x86_64 (amd64) works.
2. For a CT, turn on **nesting** under **CT → Options → Features** in Proxmox. For an unprivileged CT, turn on **keyctl** too.
3. If you changed the features, restart the CT.
4. Sign in to Linux as root, or with an account that can use `sudo`.
5. Check that the server can reach the internet. It downloads Docker and the server image.

You do not need to install Docker first. The install command installs it if it is missing and uses it if it is there.

**Check:** you are signed in to a Linux terminal, on a VM or on a CT with nesting turned on.

### Step 2. Run the install command

**Where: Linux server**

Run this one line in a root terminal.

```sh
curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | MULTITAP_LANG=en bash
```

If you are not root, add `sudo`.

```sh
curl -fsSL https://raw.githubusercontent.com/totblabs/Multitap/main/deploy/docker/install.sh | sudo env MULTITAP_LANG=en bash
```

`MULTITAP_LANG=en` shows the messages in English (`ko` for Korean). Without it, the install follows the Linux language setting: Korean when it is Korean, English otherwise.

The install runs in four steps: **[1/4] Checking the system → [2/4] Docker → [3/4] Server files and image → [4/4] Starting the server**.
On the way it asks two questions. Pressing Enter means **yes**.

| Question | What it means |
| --- | --- |
| **Let apps and Hosts connect at this address?** `http://<server IP>:18480` | Apps and Hosts on the same network connect at this address. It is not encrypted. |
| **Allow connections from outside too (router port forwarding)?** `http://<public IP>:18480` | The server's public IP is found and registered automatically, and followed within 10 minutes when it changes. Asked only when a public IP is found. |

**Check:** at the end you see **✔ Installed** and three lines.

```text
  ① Open in a browser   http://192.0.2.10:18480
  ② First sign-in       admin / admin
  ③ Setup code          (one-time code)
```

You use these three lines in **Basic server setup** next. Do not send the setup code to anyone.

<details>
<summary>The install stopped — ✖ Stopped</summary>

Under **✖ Stopped** you see the reason and what to do next.

- **Run this as root**: use the `sudo` command above.
- **Multitap installs only on Debian or Ubuntu**: on another distribution, use the Compose method in the [Docker deployment guide](../../docker-deployment.md).
- **Docker could not run a container in this CT**: turn on nesting from step 1 (and keyctl for an unprivileged CT), restart the CT, and run the same command again.
- **The server did not answer within 3 minutes**: check the log shown above. To see it again: `sudo multitap logs`.
- `curl: command not found`: install it with `apt-get install -y curl` and run again.

Running the same command again is safe. A finished install is left unchanged, and an install that stopped half way continues with the settings and data already there.

</details>

<details>
<summary>Details: what the install command does, and options</summary>

- It writes `compose.yaml` and the settings file `.env` to `/opt/multitap` and starts the server container.
- It installs the server command `multitap`. The language of the messages is kept in `.env`, and `multitap` answers in the same language.
- The data (accounts, assignments, Host registrations, secret key, downloads) is in the Docker volume `multitap-broker-data`.

Settings you can put in front of `bash` (like `MULTITAP_LANG=en`):

| Setting | What it does |
| --- | --- |
| `MULTITAP_VERSION=1.2.3` | Installs this version instead of the newest |
| `MULTITAP_LAN_HTTP=0` | Answers **no** to the first question without asking (`1` is **yes**) |
| `MULTITAP_PUBLIC_HTTP=0` | Does not register the address from outside (`1` registers it without asking). Run without a terminal, it is registered only with `1` |

To use Docker Compose yourself, or to install on another Linux, see the [Docker deployment guide](../../docker-deployment.md).

</details>

| Server command | What it does |
| --- | --- |
| `sudo multitap status` | Status, version and address |
| `sudo multitap logs` | The server's log (Ctrl+C to stop) |
| `sudo multitap code` | Shows the first-setup code again |
| `sudo multitap address <address>…` | Changes the addresses apps and Hosts use. Without an address: shows them |
| `sudo multitap public-ip [on\|off]` | Turns public IP following on or off. Without on/off: shows the state |
| `sudo multitap update [version]` | Backs up, then updates to the new version ([chapter 8](08-updates.md#server--installed-with-the-install-command)) |
| `sudo multitap backup` | Copies all data to `/opt/multitap/backups` |
| `sudo multitap restore <file>` | Puts a backup back ([backup and restore below](#backup-and-restore--server-data-in-one-file), [chapter 8](08-updates.md#restore-from-a-backup)) |
| `sudo multitap restart` · `stop` · `start` | Restart, stop, start |
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

#### Same home or office network — already applied at install

If you answered **yes** (Enter) to the first question during the install, `http://<server IP>:18480` is already applied.

**Where: the admin console on your PC**

1. Open **Server access** in the left menu.
2. Check that the **HTTP** address shows `http://<server IP>:18480`.

**Check:** opening `/get/host` at that address on your PC starts the Host download.

If you answered **no**, or want to change the address, enter the addresses in **Server access**.
The page shows the **command to run on the Multitap server (CT or VM)**. Run that command on the **Linux server**.

```sh
sudo multitap address http://192.0.2.10:18480
```

Separate several addresses with spaces. Up to 4 HTTP addresses and 1 HTTPS address.
The server restarts with the new addresses, and goes back to the previous settings if it cannot start.

> **HTTP is not encrypted.** Sign-in details and screens travel as they are.
> If you use it from the internet often, use a domain and HTTPS.

<details>
<summary>If the server was installed directly on Linux (systemd)</summary>

With this method you run no command.
Enter the addresses in **Server access** and click **Save changes**: open connections are closed and the new settings apply at once.
If they cannot be applied, the previous settings come back.

</details>

Ports published by Docker can bypass firewall rules such as UFW.
Also check what your router and the Proxmox firewall allow ([Docker documentation](https://docs.docker.com/engine/install/ubuntu/#firewall-limitations)).

#### Connecting from outside — public IP and port forwarding

If you answered **yes** to the second question during the install, the public IP address (`http://<public IP>:18480`) is registered too.
When the public IP changes, the server switches to the new address by itself within 10 minutes.

To get in from outside, set up port forwarding on your router.

1. In your router settings, forward **TCP 18480** and **UDP 18481** to the Linux server's IP.
2. If the server has a firewall, open the same two ports.

Until you forward the ports, nothing comes in from outside. The same network keeps working.

To turn it on or off later, run this on the **Linux server**.

```sh
sudo multitap public-ip on
sudo multitap public-ip off
```

`on` finds and registers the public IP and checks it every 10 minutes. `off` only stops the check; the registered address stays.

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

A complete nginx example is in the [Docker deployment guide](../../docker-deployment.md).

On the **Linux server**, find the gateway address of the Docker network.

```sh
sudo docker network inspect multitap_default -f '{{range .IPAM.Config}}{{.Gateway}}{{end}}'
```

Continue on the **Linux server**.

1. Open the settings file with `sudo nano /opt/multitap/.env`.
2. Put the gateway IP you just found after `MULTITAP_TRUSTED_PROXIES=`.
3. If you use HTTPS only, change it to `MULTITAP_PUBLISH_PORT=127.0.0.1:18480`.
4. Save and close with `Ctrl+O`, Enter, `Ctrl+X`.
5. If you use HTTPS only and public IP following is on, turn it off with `sudo multitap public-ip off`.
6. Apply the address. If you keep HTTP addresses too, add them after it, separated by spaces.

   ```sh
   sudo multitap address https://vdi.example.com
   ```

7. Allow TCP 443 to the proxy in the server firewall.
8. If you use a router, forward outside TCP 443 to the proxy.
9. Sign in from the browser on your PC at the real HTTPS address.

Replace the domain in `https://vdi.example.com` with your real domain.
Hosts and apps you already use must also test the connection at the new address and save it.

If the proxy is on another machine, do not use the `127.0.0.1` upstream address.
Forward to the broker's real address, and put the proxy's real address, as the broker sees it, in `MULTITAP_TRUSTED_PROXIES`.

If you use HTTP as well, keep the published address at `0.0.0.0:18480`. `multitap address` sets it so when you give HTTP addresses too.
To use TLS directly without a proxy, see the [Docker deployment guide](../../docker-deployment.md).

</details>

### Step 3. UDP media — can be set up after basic access works

UDP is an extra path for screen and sound.
If it is not set up, or blocked, connections use the WebSocket path.

1. Allow UDP 18481 in the server firewall.
2. If you use a router for access from outside, forward UDP 18481 to the Linux server.
3. On the **Linux server**, open `sudo nano /opt/multitap/.env` and add the lines below.
4. Replace `vdi.example.com` with the real address Hosts and apps can reach.
5. Save, then run `sudo multitap start`. It recreates the server with the changed settings.

```dotenv
MULTITAP_UDP_HOST_RELAYS=vdi.example.com:18481
MULTITAP_UDP_CLIENT_RELAYS=vdi.example.com:18481
MULTITAP_UDP_MEDIA=on
```

For an install that uses HTTP on the internal network only, use the real server IP with `:18481` on both lines.
Both address lists must be set.

> TCP 443 and UDP 18481 are separate. Forward UDP on its own, apart from the HTTPS proxy.

**Check:** later, after you connect via streaming, look at the UDP/TCP path in **Connection quality**.
Saving the settings file alone does not show that UDP works.

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

- The addresses (`.env`) and the download files stay this server's own. To change the addresses, use `sudo multitap address`.
- A newly installed server is restored the same way once the install has finished.
- A backup made by a newer server version is not restored. Update first with `sudo multitap update`.
- A data volume copy made with `sudo multitap backup` is put back with the same command ([chapter 8](08-updates.md#restore-from-a-backup)).

## Before the next chapter

- [ ] You signed in with the new admin username and password.
- [ ] **Server access** shows the address Hosts and apps use.
- [ ] `/get/host` at that address starts a download.

If all of this works, go to [chapter 3: Connect Proxmox](03-connect-proxmox.md).
**Running** in `sudo multitap status` means the server process runs; it does not mean a desktop screen is ready.

> Accounts and desktop assignments are stored in the Docker data volume.
> `multitap uninstall` keeps the data too. Do not run `docker compose down -v` or delete the volume.
