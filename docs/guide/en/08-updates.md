# 8. Updates

[Guide contents](README.md) · [Previous: Android](07-android.md) · [Next: Troubleshooting](09-troubleshooting.md) · [한국어](../08-업데이트.md)

The server, the Host and the clients are updated separately.
A badge only tells you a new version exists; the install starts when you choose it.

## What do you update?

| What to update | Where | Section of this chapter |
| --- | --- | --- |
| The Multitap app on your PC | Windows PC | [Windows client](#windows-client) |
| The Multitap app on your phone | Android device | [Android client](#android-client) |
| The Host on a desktop | **⋯** on the desktop card in the app | [One Host at a time](#host--from-the-windows-client-one-at-a-time) |
| The relay and sign-in server | Admin console + Linux server | [Server update](#server--installed-with-the-install-command) |

The app, the Host and the server are separate programs.
Updating the app does not update the others.

> **Before you start:** save your work. Updating the server or a Host can briefly drop remote connections.

## Server — installed with the install command

**Where: check the version in the admin console, then run the command on the Linux server**

A server installed with the install command from chapter 2 updates with one line: `multitap update`.

1. Open **Updates** in the left menu of the admin console.
2. Click **Check for updates**.
3. Read the new version's changes.
4. On the **Linux server**, run this command.

   ```sh
   sudo multitap update
   ```

   To update to the version the admin console shows instead of the newest, add the version. For example: `sudo multitap update 1.2.3`

5. Open the admin console again and check the running version.

`multitap update` works in this order.

1. It downloads the server image of the new version. If that fails, the current server stays as it is.
2. It stops the server and copies the data volume to `/opt/multitap/backups` (`…-before-update.tar.gz`). The app and Host setup files stay on the server, so this copy leaves them out.
3. It switches to the new version's `compose.yaml` and `multitap` command and starts the server with the new version.
4. It waits for the new version to answer. If it does not answer, it goes back to the previous version.
5. It keeps the 3 most recent pre-update copies, and the server images of the current and the previous version. What it removes is shown.

Copies you made yourself with `sudo multitap backup` and the safety backups of a restore are never removed automatically. Delete them from `/opt/multitap/backups` when you no longer need them.
`sudo multitap status` shows the free disk space and says when it is low.

**Check:** you see **✔ Updated to (version). Your data is unchanged.**, you can sign in to the new version, and existing users and desktop assignments are still there.
Also connect once to a desktop you use every day.

The `docker compose` commands under **Updates** in the admin console are for installs that use Compose directly.
On a server installed with the install command, use `multitap update`. It also updates `compose.yaml` to the new version.

Accounts, desktop assignments, the server's secret key and downloads stay in the data volume.
**Do not run `docker compose down -v` or delete the data volume.**

If the server does not come up, check its status and recent log with these commands.

```sh
sudo multitap status
sudo multitap logs
```

Before you share a log, hide the setup code, addresses and secrets.
For details on proxy and HTTP settings and on backups, see the [Docker deployment guide](../../docker-deployment.md).

<details>
<summary>Server update on an install that uses Docker Compose directly</summary>

Work in the folder with `compose.yaml` and `.env`.

1. Under **Updates** in the admin console, check the new version and its commands.
2. Keep `.env` and the data volume with the [backup steps below](#backup-before-a-server-update).
3. Open `nano .env` and change `MULTITAP_VERSION` to the version shown.
4. In the same Compose folder, run this command.

   ```sh
   sudo docker compose pull && sudo docker compose up -d
   ```

5. Open the admin console again and check the running version.

In the editor, save and close with `Ctrl+O`, Enter, `Ctrl+X`.
Use the version exactly as the admin console shows it.
For status and logs, use `sudo docker compose ps` and `sudo docker compose logs --tail=50 broker`.

</details>

### Backup before a server update

On a server installed with the install command, `multitap update` makes a backup before it updates.
To make one yourself, run this on the **Linux server**.

```sh
sudo multitap backup
```

It stops the server for a moment, copies the whole data volume to `/opt/multitap/backups/multitap-data-(version)-(date).tar.gz` and starts it again.
Tell users first and let them save their open work.

**Check:** **✔ Data backed up:** is followed by the file path.
The backup holds the secret key (`vault.key`) and account details. Only root can read it; do not publish it or share it with regular users.
`.env` is not in the backup file. Keep `/opt/multitap/.env` as well.

Including this server's CT or VM in a Proxmox backup job is the complete backup: data, settings and Docker come back in one step.

### Restore from a backup

On a server installed with the install command, `multitap restore` puts a backup back. Run it on the **Linux server**.

```sh
sudo multitap restore /opt/multitap/backups/multitap-data-(version)-(date).tar.gz
```

With a copy made by `multitap backup` or `multitap update`, the whole data volume goes back to that time.
A copy made by `multitap update` (`…-before-update.tar.gz`) is for going back on that same server. To move to another server, use a `multitap backup` copy or a backup file from the admin console.
A `.mtbk` backup file from the admin console is put back with the same command; it then asks for the backup password ([chapter 2: backup and restore](02-server-install.md#backup-and-restore--server-data-in-one-file)).

1. It checks the file and shows when it was made, the server version and the numbers of accounts, desktops and assignments. Nothing changes yet.
2. Answer `y` to **Restore? [y/N]**. Enter alone cancels.
3. It stops the server and keeps the current data as a `…-before-restore.tar.gz` safety backup.
4. It restores, starts the server and waits for it to answer. If anything fails, it puts the safety backup back and shows the reason. It does not try again by itself.

**Check:** **✔ Restored.** appears and the server answers. Tell users first: connections end during the restore.
A backup made by a newer server version is not restored. Update first with `sudo multitap update`.
`.env` does not change, and the addresses stay as this server has them now.
In scripts, `--yes` skips the question, and the backup password of a `.mtbk` file comes from the `MULTITAP_BACKUP_PASSPHRASE` environment variable.

<details>
<summary>Backup steps for an install that uses Docker Compose directly</summary>

Work in the folder with the Compose files (for example `~/multitap`).
This stops the server for a moment, so tell users first and let them save their open work.

Follow the steps in order **in the same terminal on the Linux server**.

1. Go to the Compose folder.

   ```sh
   cd ~/multitap
   ```

2. Check that there is enough space for the backup.

   ```sh
   df -h .
   ```

3. Make a new backup folder that other users cannot read.

   ```sh
   MULTITAP_BACKUP_DIR=$(mktemp -d "$HOME/multitap-backup-XXXXXXXX")
   ```

   If you see an error, stop here. Do not stop the server before the backup folder exists.

4. Stop the server for a moment.

   ```sh
   sudo docker compose stop broker
   ```

5. Copy all data to the backup folder.

   ```sh
   sudo docker compose cp broker:/var/lib/multitap "${MULTITAP_BACKUP_DIR:?}/data"
   ```

6. Copy the install settings too.

   ```sh
   cp .env compose.yaml "${MULTITAP_BACKUP_DIR:?}/"
   ```

7. Check that the files are there.

   ```sh
   sudo ls -la "${MULTITAP_BACKUP_DIR:?}" "${MULTITAP_BACKUP_DIR:?}/data"
   ```

8. Start the server again.

   ```sh
   sudo docker compose start broker
   ```

9. Show the backup folder and note it down.

   ```sh
   printf '%s\n' "$MULTITAP_BACKUP_DIR"
   ```

**Check:** there are the two settings files and, in `data`, the data including `broker.sqlite` and `vault.key`.
If you back up only the database, the saved Proxmox token may not be recoverable. Keep the whole data folder.

If an error appears while copying, stop the update.
Start the old server with `sudo docker compose start broker` and find the cause.

The backup holds the secret key and account details. Do not publish it or share it with regular users.
Seeing the folder is not the same as testing a restore. For an important server, also test the restore somewhere else.

This copy uses Docker's [Compose cp](https://docs.docker.com/reference/cli/docker/compose/cp/).
It can copy files from a stopped container too ([Docker documentation](https://docs.docker.com/reference/cli/docker/container/cp/)).

</details>

## Host — from the Windows client, one at a time

You can update the Host of a desktop assigned to you.
Regular users can also install the newest Host on desktops assigned to them.

1. Open **⋯ → Update Host** on the desktop card.
2. Check the current version, the new version and **What's new in this version**.
3. If no one is connected, click **Update**.

If someone is connected, you get two choices.

| Choice | What happens |
| --- | --- |
| **Update when done** | Starts after all connections end. |
| **Update now** | Disconnects the connections you may end and starts at once. |

Regular users can end only their own connection.
If another user is connected, choose **Update when done** or ask an administrator.

## Host — from the Windows client, several at once

1. Click **Check for updates** above the list.
2. Click **Update Hosts**.
3. Choose the desktops to update.
4. Read the notice about ending connections.
5. Click **Update N selected**.
6. Check the progress and the install result for each one.

Your own connections are ended and the update starts at once.
If another user is connected to a desktop assigned to a regular user, it waits until that connection ends.
A request from an admin account can end every connection to the desktop.

**Stop remaining requests** stops only the requests not sent yet.
Updates already accepted continue.

## Host — Android and the admin console

| Where | Choice | When the desktop is in use |
| --- | --- | --- |
| Android desktop card **⋯ → Update Host** | **Update** after reading the changes | Starts after the connection ends. |
| Admin console **Desktops → the desktop**, **Update Host** | **Update**, or **Update when connections end** | The **Update now (disconnects)** button is also available. |
| Admin console **Updates**, **Update all N** (or the **Update** mark under **Desktops**) | **Disconnect and update N** after checking who is connected | Choosing **Only the N not in use** disconnects no one. |

Android has no update for several Hosts at once.
For desktops not assigned to you, use the admin console.

### Reconnecting after a Host update

A Host update does not restart Windows by itself.
If you see **Windows restart needed** or another notice, read it.

A Windows or Android streaming screen dropped by the update waits up to 10 minutes for the Host to come back and tries to reconnect by itself.
Keep the desktop window open while it waits.
If the time ran out, or you clicked **Stop waiting**, click **Reconnect** after the update.

## Windows client

1. Click the **New version** mark above the list, or the check for updates button.
2. Read the changes.
3. Click **Get update**.
4. When the file check is done, click **Restart to update**.
5. Confirm the Windows administrator prompt.

If a connection is open, you are asked to confirm **Disconnect and update**.
All connections end, but the desktops and the work running on them stay.
When the app opens again after the update, check the new version.

## Android client

1. Tap **Check for updates** above the list.
2. Read the changes and tap **Update**.
3. Tap **Download and install**.
4. If you see a notice about allowing installs, tap **Open settings**.
5. In Android settings, allow **Install unknown apps** for Multitap.
6. Return to the app and tap **Download and install**.
7. On the Android install screen, tap **Update**.
8. When the install finishes, open the app again.

App data and sign-in settings stay.
If the downloaded file fails its check, or the app signature does not match, the install stops.
If the error keeps coming back, tell your administrator before you delete the app.

[Next: Troubleshooting](09-troubleshooting.md)
