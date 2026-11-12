# 10. Security

[Guide index](README.md) · [Previous: Troubleshooting](09-troubleshooting.md) · [Next: Guide index](README.md) · [한국어](../10-보안-안내.md)

The server relays sign-ins and remote connections.
Use it where you can trust the server and the people who run it.

## Five things to remember at the start

1. On the internet, use an `https://` address.
2. Change both the administrator ID and the password set at install.
3. When you approve a Host, compare the confirmation codes on both screens.
4. Allow copy and USB sharing only for the users and desktops that need them.
5. Do not share passwords, setup codes, tokens or backup files.

A token is a secret value that a program uses like a password.
When you ask for help, keep only the error message and hide anything that contains a secret.

## HTTPS and HTTP

On the internet, use an **HTTPS** address with a valid certificate.
The name in the address must match the certificate.

**HTTP does not encrypt sign-in or WebSocket traffic.**
Passwords, the screen and key presses can be exposed on the network.
Use HTTP on a trusted internal network. If you reach it from outside over HTTP through router port forwarding, all of this crosses the internet as it is. If you use it from outside often, use a domain and HTTPS.

Agreeing to the app's HTTP warning does not add encryption.
Check that the server address, the port and the operator's settings are right.

### It is not end-to-end encryption

The screen and input pass through the broker and relay server.
HTTPS protects each connection: between the client and the server, and between the Host and the server.
There is no **end-to-end encryption (E2E)** that only the client and the Host can decrypt.

Do not assume the server operator cannot reach the traffic.
Encryption of UDP media does not mean end-to-end encryption either.

## First setup right after install

1. Open the admin console right after installing.
2. Sign in with `admin` / `admin`.
3. Enter the one-time setup code from the server log.
4. Change the administrator ID to a new one.
5. Set a new password that you do not use for any other service.
6. Finish the Proxmox connection and the access address settings.

You can see the setup code again with this command on the server.

```sh
sudo multitap code
```

The setup code appears only in the server log and is needed to finish first setup.
Do not send the code, or a log that contains it, to anyone.
If you restarted the server before setup, use the code from the latest log.

Each account can turn on **two-step sign-in** (a 6-digit code from an authenticator app).
In the admin console, set it up in **Account security** in the left menu; in the apps, in **Two-step verification** in **⚙** Settings. Keep the recovery codes somewhere separate.
Use a strong password and do not share administrator accounts.

## Host approval and user permissions

1. Compare the confirmation codes shown in Host settings and in the admin console.
2. Approve only when the request comes from that Windows and the codes match.
3. If there are duplicate requests for the same VM, check what each request is for first.
4. Assign users only the desktops they need.
5. Turn on only the copy and device sharing that is needed.

Sharing applies per **user × desktop**, and a new assignment starts at **Default**, which shares nothing.
Permission to update a Host and permission to control power are separate.
Having a desktop assigned does not allow every power action and device sharing.

### What is recorded

The server records who signed in when and which desktop they connected to for how long, and shows it in **Monitoring** in the admin console.
The records include the IP address the connection came from and are kept for 2 years. Deleting an account also deletes its records.
**Connection time** is the time spent connected, not the time spent working.

If people share the same Windows account on the same desktop, they share that account's files and programs.
Separate Multitap accounts alone do not separate the workspace inside Windows.

## How update files are checked

| Target | What is checked |
| --- | --- |
| Official Windows client and Host updates | File size, SHA-256 and the project's own Ed25519 release signature |
| Server update notices | Release info signed with a pinned public key |
| Android updates | File size, SHA-256 and a package signature that matches the installed app |

If a file or signature check fails, stop installing and tell the administrator.
The client gets updates from the release list of the server you signed in to.
Use a server address you trust.

<details>
<summary>Project signature vs. Windows code signing</summary>

The Ed25519 release signature is how official release files are checked against a public key pinned by Multitap.
It is separate from Authenticode code signing, which Windows uses to show the publisher of an installer.
Release files have no purchased code signing certificate.
A valid release signature does not remove SmartScreen warnings or mean that Windows trusts the publisher.

The signed update notice for Docker does not sign the container image tag itself either.
Operators should check the official release source and the version the admin console shows.

</details>

## Passwords, backups and logs

The Windows client saves your password in the current Windows user's Credential Manager only when you choose **Remember password**.
Turn this off on shared PCs.

The server data volume and backups contain account information, desktop assignments, sign-in and connection history, and the server's secret keys.
Keep them where only administrators can reach them.
Do not upload `.env`, backups, logs or screenshots to a public repository or a public issue.

Backup files made in the admin console (`.mtbk`) are encrypted with a backup password ([chapter 2: backup and restore](02-server-install.md#backup-and-restore--server-data-in-one-file)).
The file alone shows only when it was made and the server version, but a weak password can be guessed. Use a long password you do not use anywhere else, and keep the file and the password apart.

## Reporting security problems

**Do not write how to reproduce a vulnerability, or any secret, in a public issue.**
Read the [security policy](../../../SECURITY.md) first for what to report and how reports are handled.

If private reporting is turned on in the official GitHub repository, you can report this way:

1. Open **Security → Advisories** in the repository.
2. Click **Report a vulnerability**.
3. Write the affected component and version.
4. Write the permissions needed, the steps to reproduce and the expected impact.
5. Hide real addresses, passwords and tokens, then submit.

If you do not see the button, check the repository's latest security policy.
If it gives no guidance, open a public issue that only asks for private reporting to be turned on, with no vulnerability details.
This project does not offer a separate reporting email.

The button appears only when the repository has turned the feature on ([GitHub documentation](https://docs.github.com/en/code-security/how-tos/report-and-fix-vulnerabilities/report-privately)).
Security fixes target the latest public version, and no response time is guaranteed.

## What the Host does to Windows

- The virtual display driver is the original signed by Sunshine, used as is. At install, it asks for your consent and adds that one signing certificate (code signing only) to the Windows trust stores. It adds no other certificate.
- It does not change Windows security settings such as test mode or Defender exclusions. It does not change UAC settings either.
- Input to security prompts and administrator windows is passed by the Host service only in your own stream of your own session that is not locked.
- The Windows password saved on the Host stays in that Windows and is not sent to the Multitap server.

[Next: Guide index](README.md)
