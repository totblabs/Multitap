# Multitap for Proxmox VE guide

[한국어](../README.md)

These chapters are the detailed reference, with every screen and setting. If you are installing for the first time, the [guide on the website](https://multitap.me/en/guide/) is shorter: nine chapters from install to the first connection. Come back here when you need the detail.
Below, choose whether you **received an account** or **are installing it for the first time**.

## I received an account — connect to a desktop

Get the **server address, username and initial password** from your administrator.
Then follow the guide for the device you use.

- **Windows PC:** follow [chapter 6: from installing the app to the first connection](06-windows-client.md#your-first-connection).
- **Android phone or tablet:** follow [chapter 7: from installing the app to the first connection](07-android.md#your-first-connection).

What a user needs, in order: **install the app → check the server address → sign in → connect via streaming**.

## I am installing it — start by connecting one desktop

At first, check the connection with **one Windows VM and one user**.
Then add the other desktops the same way.

First, tell the computers you install on apart in [Introduction and what you need](01-introduction.md).
Then go through these six steps.

1. **Install the server:** [install Multitap on a Linux server](02-server-install.md#install-the-server) with one command and start it.
2. **Basic server setup:** [set up the admin account and the addresses](02-server-install.md#basic-server-setup).
3. **Connect Proxmox:** [connect Proxmox](03-connect-proxmox.md) from the admin console and the Proxmox Shell. Go on when you see **Connected**.
4. **Install and approve the Host:** [install the Host inside the Windows that will become a desktop](04-host-install.md). It covers finishing the install, admin approval and the Windows account.
5. **Users, assignments and sharing:** [create user accounts and assign desktops](05-users-and-desktops.md).
6. **Connect a client:** install the [Windows app](06-windows-client.md#your-first-connection) or the [Android app](07-android.md#your-first-connection) on a PC or phone and connect.

**The Host goes inside the Windows that becomes a desktop; the client goes on the device you use right now.**
With only the server installed, no desktop screen appears yet.

## If you are stuck

| Situation | Where to look |
| --- | --- |
| No server address or password | Get them from the administrator who created your account. |
| You can sign in but see no desktop | [Check the first connection](05-users-and-desktops.md#check-the-first-connection) |
| Host offline, virtual display not ready, black screen | [Troubleshooting](09-troubleshooting.md#start-with-how-far-you-got) |
| You see the screen but copying does not work | [Copy and paste and permissions](06-windows-client.md#copy-and-paste) |
| A new version is out | [Updates](08-updates.md) |

> **Use HTTPS when you connect over the internet.**
> HTTP does not encrypt sign-in details, the screen or input. Use it only on an internal network you trust.

<details>
<summary>All chapters</summary>

| Chapter | Contents | Mainly for |
| --- | --- | --- |
| [1. Introduction](01-introduction.md) | Parts and what you need | Everyone |
| [2. Server install and basic setup](02-server-install.md) | One-line install → admin account → addresses and ports | Administrators |
| [3. Connect Proxmox](03-connect-proxmox.md) | One command creates the dedicated integration account | Administrators |
| [4. Install Host](04-host-install.md) | Registering Windows VMs and PCs, Windows sign-in and devices | Administrators, whoever installs the Host |
| [5. Users and desktop assignment](05-users-and-desktops.md) | Admin console menu, accounts, assignments and sharing, monitoring | Administrators |
| [6. Windows client](06-windows-client.md) | Sign-in, streaming, session toolbar, device sharing, Console, settings | Windows users |
| [7. Android](07-android.md) | Touch and pointer, keyboard and session menu, shared folders, settings, Console | Android users |
| [8. Updates](08-updates.md) | Updating the server, Host and apps | Everyone |
| [9. Troubleshooting](09-troubleshooting.md) | Error messages and what to do next | Everyone |
| [10. Security](10-security.md) | Protecting access, update signatures and reporting | Everyone |

</details>

## Examples in this guide

- `vdi.example.com`, `pve.example.com`, `10.0.0.10` and the account and desktop names on the screens are all examples. Use your own addresses and accounts.
- The screen images are renders of the real apps and admin console with example data. The states and versions shown are examples.
- The list of example screens is in the [image notes](../images/README.md).

[Back to the project page](../../../README.md)
