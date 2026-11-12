# 9. Troubleshooting

[Guide index](README.md) · [Previous: Updates](08-updates.md) · [Next: Security](10-security.md) · [한국어](../09-문제-해결.md)

Find the item that matches the message on your screen.
After each fix, check the status again.

## Start with how far you got

Read from the top and follow the advice on the first row where you got stuck.
If there is an error message, look for the same message below.

| Where did you get stuck? | What to do first |
| --- | --- |
| Installing the server for the first time, and the admin console does not open | In [chapter 2](02-server-install.md#step-2-run-the-install-command), check the server IP, whether it is running, and the port. On the server, `sudo multitap status` shows the status and the address. |
| **Test connection** fails in the app | Check the address and certificate in [Server address and sign-in](#server-address-and-sign-in). |
| The server connects but sign-in fails | Check that it is a Multitap account the administrator made. Keep it apart from your Windows and Proxmox passwords. |
| Signed in, but my desktops are empty | Ask the administrator to check your [desktop assignment](05-users-and-desktops.md). |
| The desktop card shows Host offline or a screen setup error | See [Host and virtual display](#host-is-offline-or-the-screen-is-not-ready). |
| The connection window opened but the screen is black | See [Black streaming screen](#the-streaming-screen-is-black). |
| The screen works, but only file copy or USB does not | Check the allowed features in **Guide** under **Settings** on the toolbar. |
| Windows is signed out or locked | Sign in with **▾ → Console** on the card, or use **⏻ → Restart**. |
| You cannot click a security prompt (UAC) | See [Security prompts](#when-a-security-prompt-uac-appears). |
| Console does not open or is black | See [Console problems](#console-does-not-open-or-is-black). |

After changing the address settings, run **Test connection → Save** in the app again.

## Server address and sign-in

| Message you see | What to do first |
| --- | --- |
| **Can't find the server address.** | Check the spelling of the address and your internet connection. |
| **Can't connect securely over HTTPS.** | Check the server address. If it is right, tell the administrator (it may be a certificate problem). |
| **The server didn't respond in time.** | Check the address and port, then try again. |
| **This address isn't a Multitap server.** | Check that it is the server address the administrator gave you. |
| **This address isn't registered on the server.** (`origin_not_allowed`) | Check HTTP or HTTPS and the port in the address you were given. If they are right, tell the administrator. |
| **HTTP connections are turned off on the server.** | Use the HTTPS address or ask the administrator. |
| **This HTTP address is only for the server's first setup.** | You can connect after the administrator finishes **Server access**. |
| **This address doesn't match the server's settings.** | Check the address and port you were given again. If they are right, tell the administrator. |

### If you are the administrator

1. Open **Server access** in the left menu of the admin console.
2. Compare the address the user entered with the allowed addresses.
3. On the server, run the `multitap address …` command the page shows.
4. Run **Test connection** again in the client.

If you use an HTTPS proxy and get `403 http_disabled`, the trusted proxy setting may be missing.
Check the real proxy address and `MULTITAP_TRUSTED_PROXIES` as described in [Docker deployment](../../docker-deployment.md).

Do not connect by ignoring certificate errors.
To run over HTTP on the same internal network, read [Security](10-security.md) first.

## Host is offline or the screen is not ready

| Status | User | Administrator or whoever manages that Windows |
| --- | --- | --- |
| **Host offline** | Check the power state. | In Host settings, check the server connection and the service status. |
| **Waiting for administrator approval** | Ask the administrator to check the approval. | Compare the confirmation codes on both screens, then approve the Host connection request. |
| Host settings shows **This Windows looks like a copy of another registered desktop.** | Tell the administrator. | It is a copied VM. In Host settings, click **Request connection** in **Server connection**, and approve it like a new Host ([chapter 4](04-host-install.md#copied-vms)). |
| **The virtual display isn't ready** | Ask the administrator to check the Host. | Check the status and the needed action in **Virtual display** in Host settings. |
| **The Host is setting up the screen** | Connect again when setup is done. | If it stays stuck, check **Diagnostics** in Host settings. |
| **The Windows account needs checking** | Ask the administrator to check the Windows account. | Check the saved account in **Windows sign-in** in Host settings. |
| **Windows isn't signed in** or **Windows is locked** | Open it with **Console** on the card and sign in or unlock it. **Restart** signs in automatically. | Check that an account is saved in **Windows sign-in** on the Host. |
| **Windows needs to restart** | Save your open work, then **Restart**. | Check the cause shown, then restart that Windows. |
| **Cleaning up the previous connection** | You can connect when it is done. Wait a moment. | If it keeps happening, check the screen recovery status in **Diagnostics** on the Host. |

If Host settings shows **Start service** and the service is stopped, start it with that button.
After the status changes, click **Refresh** in the client.

## Proxmox warnings in the admin console

If a Proxmox certificate or token warning appears at the top of the admin console and in **To do now** on **Home**, follow [Certificate and token warnings](03-connect-proxmox.md#certificate-and-token-warnings) in chapter 3.
With **The Proxmox certificate changed** or **The Proxmox certificate has expired**, power, Console and status checks have stopped. Fix it right away.

## The streaming screen is black

1. Check whether the desktop window shows an error message.
2. If it does, take the action for that message.
3. If the screen stays black, close the connection and click **Refresh**.
4. If it is the same after you connect again, ask the administrator to check **Virtual display** and **Diagnostics** on the Host.

A "service running" status alone does not mean screen streaming is ready.
If the screen shows no cause, check diagnostics instead of removing drivers on your own.

## Console does not open or is black

Console shows the QEMU screen, the default screen of a Proxmox VM.
While Console is open, only this basic display is on. When you close it, the other displays come back.
Local PCs have no Console.

| Message in Console | What to do |
| --- | --- |
| **There is no picture for the Console** | The VM's Proxmox display is none or a serial terminal, or a passed-through GPU is the primary VGA. The administrator sets **Hardware → Display** of the VM to **Standard VGA** in Proxmox and restarts the VM. |
| **The sign-in screen may be on a display the Console can't show.** | The VM has a PCI device (such as a GPU) and is signed out or locked. **Restart** the desktop. It signs in automatically. |
| **Another remote connection is in use** | Disconnect the streaming connection you are using, then open Console again. |
| **Cleaning up the previous connection** | Connect again in a moment. |
| **The VM's original display is off** | Ask the administrator to check the screen recovery result. |
| **No recovery info for the original display** | Ask the administrator to check the VM display status. |
| **The VM display setup has changed** | Ask the administrator to check the VM display settings. |
| **The VM console is off** | Ask the administrator to check the QEMU console settings. |
| **You don't have permission to connect to the console** | Ask the administrator to check your desktop assignment and **Emergency Console permission** in **Proxmox connection**. |
| **Console sign-in has ended** | Close the window and open Console again from the card in the app. |
| **Nothing on screen yet. If it stays black, connect again.** | Connect Console again. |

**Console connected** does not guarantee a Windows sign-in or a normal screen.
If the screen is still black after you connect again, ask the administrator to check the QEMU screen and the GPU setup.

## When a security prompt (UAC) appears

Security prompts on the normal desktop and windows run as administrator can be seen and used inside the stream.
This also works on desktops used with a standard Windows account. Windows UAC settings are not changed.

These cases do not work yet or have not been checked:

| Case | What to do |
| --- | --- |
| A prompt that dims the screen (the secure desktop, Windows' default setting) | Not yet checked in a real environment. If you cannot use it, follow the advice below. |
| The **A security prompt is open on the desktop** screen | Disconnect, open it with **Console** on the card, and respond there. |
| Windows is signed out or locked | It does not stream. Use **Console** or **Restart**. |

## No sound

The desktop's sound always plays.

1. Check the volume on your PC or phone.
2. If the desktop card shows **No sound**, the desktop has no sound device. Ask the administrator.
3. If it already has a sound device, check the Windows sound output on the desktop.

The administrator adds the sound device in Proxmox.
In the Proxmox web interface, choose **Hardware → Add → Audio Device** for the VM.
The Multitap apps and the admin console do not add sound devices.

A **Restart needed** badge means the added device is not applied yet.
Shut down the VM in Proxmox, then start it again.
Restarting from inside Windows does not apply the new device.

For the sound device of a local PC, check Windows on that PC.

## Mouse lock and game input

| Symptom | What to do |
| --- | --- |
| The mouse is locked to the desktop screen | Press **Ctrl+Shift+Alt+Z** in the Windows client. |
| The lock is off in a new connection | New connections always start with it off. Turn it on with the same shortcut or in **Settings** on the toolbar when you need it. |
| The mouse does not move in a game | Ask the administrator to check USB sharing permission for that user and desktop. |
| Host USB status: **Another program is using the USB connection feature.** | See **Devices → Switch to Multitap** in Host settings. |
| The cursor position is off | Update the server, Host and client to the latest version. |

Switching the controller can affect other programs' USB connections.
Read the confirmation window before you go ahead.

The virtual USB mouse works only when USB sharing permission and the Host's USB connection feature are ready.
Turning on **Mouse lock** alone does not guarantee that every game works.

## Korean input and the Han/Eng (한/영) key

1. Click the desktop screen to choose where keyboard input goes.
2. In the Windows client, press the **Han/Eng (한/영)** key on your real keyboard.
3. On Android, tap **Han/Eng (한/영)** in the key row above the keyboard.
4. If you still get only English letters, check the Windows Korean input method on the desktop.

### Korean input in Console

In Console on a phone or tablet, Korean and English letters typed with the **Keyboard** button follow the VM's real Korean/English (한/영) mode.

| Message | What to do |
| --- | --- |
| **Korean text can be sent only while the desktop is signed in.** | On the sign-in and lock screens, only English letters, digits and symbols are sent. Sign in to Windows, then type. If it appears while signed in, update the Host. |
| **The Korean/English switch wasn't confirmed, so typing stopped.** | Type the same letters again. |

The Console text clipboard does not send characters such as Korean. For longer text, connect by **Streaming** and copy there.

## Defender or SmartScreen blocks the install

1. Write down the file name and the detection or block message in the warning window.
2. Check the item in **Windows Security → Protection history**.
3. Send the administrator the file version and the warning details, with private parts hidden.
4. Stop installing until the administrator checks the download source and the latest file.

A warning alone does not mean it is a false positive.
Do not turn off Defender or SmartScreen, and do not add the whole Multitap folder as an exclusion.
The project's release signature and Windows publisher trust are different things ([Security](10-security.md)).

To check protection history, see the [Microsoft support page](https://support.microsoft.com/en-us/windows/security/windows-security/protection-history-in-the-windows-security-app).

## What to send when you ask for help

1. Write down the server, Host and client versions.
2. Write down when the problem happened and the button you clicked just before.
3. Write down the error message exactly.
4. Hide addresses, user names, setup codes, passwords and tokens in screenshots and logs.

Microphone and camera forwarding is not supported.
There are no clients for Mac, iOS or Linux, and Android cannot use USB devices.
Do not write anything that looks like a security vulnerability in a public issue. Follow [private reporting](10-security.md#reporting-security-problems) instead.

[Next: Security](10-security.md)
