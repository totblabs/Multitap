# 1. Introduction

[Contents](README.md) · [Next: Server install](02-server-install.md) · [한국어](../01-소개.md)

Multitap for Proxmox VE is a self-hosted VDI for Proxmox VE.
With a server you run yourself, you use the Windows VMs of Proxmox VE from a PC or a phone.
An administrator assigns desktops, and users connect to them from the app.

A **desktop** is a Windows you use remotely. It is a Windows VM in Proxmox, or a real Windows PC with the Host installed (a **local PC**).
A **VM** is a virtual computer that runs inside Proxmox.

## First, tell the three places apart

| Word in this guide | Meaning | Example |
| --- | --- | --- |
| PC | The computer in front of you, used with its mouse and keyboard | Your laptop at home |
| Desktop | A Windows you use remotely | A Windows VM in Proxmox, a Windows PC at the office |
| Linux server | The computer that relays sign-in and connections | The Linux CT or VM where the Multitap server is installed |

Run the Host installer **inside the Windows that will become a desktop**.
Run the client installer **on your PC**.

## Which program goes where?

| Part | Installed on | What it does |
| --- | --- | --- |
| Server (broker) | Linux server | Provides sign-in, user assignments, screen relay and the admin console. |
| Host | The Windows VM or PC that becomes a desktop | Sends screen and sound, and receives keyboard and mouse input. |
| Windows client | The user's Windows PC | Connects to the assigned desktops. |
| Android client | The user's phone or tablet | Connects to the assigned desktops. |

```text
Windows / Android client
             ↕
      Multitap server (broker) ── Proxmox VE
             ↕               VM list, power, Console
       Windows Host
        VM or PC
```

The Host and the clients connect to the same Multitap server.
They do not need to be on the same internal network; the server relays between them.

> The screen passes through the server.
> It is not end-to-end encrypted. See [Security](10-security.md).

## What you need

| For | What to prepare |
| --- | --- |
| Server | A Debian or Ubuntu x86_64 CT or VM with internet access. The install command installs Docker with it |
| Proxmox | Proxmox VE that manages the VMs, and access to a node's root Shell |
| Host | A Windows 10/11 x64 VM or PC, and administrator rights for the install |
| Windows client | A Windows 10/11 x64 PC |
| Android client | A device with Android 8.0 or later |
| Access from the internet | A server domain, an HTTPS certificate, and rights to change the firewall and router |

An administrator installing the server for the first time starts with **being able to sign in to Linux**.
Installing Proxmox and the Linux operating system themselves is outside this guide.

### Not supported

- There are no Mac, iOS or Linux clients. Clients are Windows and Android only.
- USB devices cannot be used on Android.
- Microphone and camera redirection are not supported.
- Desktops are Windows only. The Host is for Windows 10/11 x64.

Docker is a tool that runs programs packaged together on a Linux server.
Compose reads a file called `compose.yaml` and runs the Multitap server.

Windows VMs without a GPU work too.
Picture quality and frame rate depend on VM performance, the encoder and the network.

On an internal network you can also run it over HTTP without a certificate.
HTTP does not encrypt sign-in details or the screen.

## Which password is it?

| Screen or task | Account to use |
| --- | --- |
| Signing in to the Multitap app | The Multitap user account an administrator created |
| Signing in to the Multitap admin console | The admin account you changed during the server's first setup |
| **Windows sign-in** in the Host | The Windows account that really signs in to that VM or PC. Use its password, not a PIN |
| The Proxmox web UI and **Shell** | A Proxmox administrator account |
| Server terminal, SSH | The Linux account created when Linux was installed |

These accounts are all different.
Creating a Multitap ID does not create a Windows account.

## Order for a first install

1. [Install and start the server.](02-server-install.md#install-the-server)
2. [Set up the server's admin account and addresses.](02-server-install.md#basic-server-setup)
3. [Connect Proxmox.](03-connect-proxmox.md)
4. [Install the Host in Windows and approve it.](04-host-install.md)
5. [Create user accounts and assign desktops.](05-users-and-desktops.md)
6. Connect with the [Windows client](06-windows-client.md) or the [Android client](07-android.md).

The administrator gives users the server address and their Multitap account.
Never give users a Proxmox account or API token.

> `vdi.example.com` and `10.0.0.10` in this guide are examples.
> Replace them with the addresses of your own server.

**Ready?** When you can tell the PC, the desktop and the Linux server apart, and can sign in to each of them, go to [chapter 2](02-server-install.md).
