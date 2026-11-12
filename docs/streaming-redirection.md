# Streaming device redirection: overview

Streaming sessions (Multitap Streaming) carry devices between the user's PC and the VM,
like the redirections of RDP. Every class has its own per-assignment
(user × VM) permission, denied by default, enforced by the broker and again by
the Host, and carried only over the authenticated relay. This page lists what
is redirected and what is not, the driver signing rule that shaped the design,
and the security model the channels share. Details:
[clipboard](remote-session-protocol.md#clipboard),
[drives](drive-redirection.md), [printing](print-redirection.md),
[USB](usb-redirection.md).

## What is redirected

| Class | Channel | VM side | PC side | Permission |
|---|---|---|---|---|
| Clipboard (text, images, files) | clipboard socket | the Host's clipboard worker | the client's clipboard | text·image and file permissions, per direction |
| Folders as drives | `drive-v1` | WinFsp network volume; WebDAV redirector as fallback | the client serves the chosen folders | `drives` |
| Printing | `print-v1` | a "Multitap 프린터 (PC)" queue on the in-box Print To PDF driver | a local printer, or Save as PDF | `printers` |
| USB devices | `usb-v1` | usbip-win2 virtual host controller | UsbDk captures the device | `usbDevices` |
| Smart card readers, security tokens | as USB devices (class 0Bh) | in-box CCID driver | UsbDk | `usbDevices` + `smartCards` |
| USB serial (CDC) | as USB devices (class 02h/0Ah) | in-box `usbser.sys` | UsbDk | `usbDevices` + `serialPorts` |
| 마우스 잠금 (virtual USB mouse) | `usb-v1` | in-box HID mouse driver | made up by the client from Raw Input; no UsbDk | `usbDevices` |

The Android app uses printing and, for 마우스 잠금 with a connected mouse, the
virtual USB mouse; it does not share folders or physical USB devices.

## What is not redirected, and why

- **Camera and microphone** are out of scope. For the microphone there is also
  no driver-free path: Windows has no user-mode API that creates an audio
  capture endpoint, and no open-source virtual audio driver with a Microsoft
  signature was found (table below). USB headsets and webcams cannot be passed
  as raw USB devices either (next point).
- **Isochronous USB devices** (webcams, headsets, audio interfaces) are denied
  as raw USB, as are the audio (01h) and video (0Eh) classes: isochronous
  timing does not survive relay latency and jitter.
- **USB on Windows Server VMs.** usbip-win2 is attestation-signed, and Windows
  Server 2016 and later load only HLK-signed device drivers
  ([Microsoft](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)).
  The Host advertises USB only on workstation Windows 10 1809 or later.
- **Emulated smart card readers and serial ports.** A CCID reader or a CDC-ACM
  port emulated by the Host (so the PC would share a card through WinSCard or a
  COM port instead of handing over the USB device) was considered and not
  built. Card readers, security tokens and USB serial adapters work as
  forwarded USB devices; a PC's built-in COM port or a non-USB reader cannot be
  shared.

## The signing rule

- From Windows 10 1607 with Secure Boot on, Windows loads new kernel-mode
  drivers only when they are signed through the Microsoft Dev Portal
  ([Driver Signing Policy](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/kernel-mode-code-signing-policy--windows-vista-and-later-)),
  either HLK (WHQL) or attestation.
- Attestation works on Windows 10/11 desktop only; Windows Server loads only
  HLK-signed drivers. Microsoft describes attestation as intended for testing
  but keeps loading such drivers on desktop Windows, which is a policy risk
  ([Driver Signing Options](https://learn.microsoft.com/en-us/windows-hardware/drivers/dashboard/driver-signing-offerings)).
- A Dev Portal signature needs an EV certificate and a Partner Center account.
  Multitap buys no certificates, uses no test signing and never weakens Windows
  security, so it can only ship a third party's **already Microsoft-signed**
  package with its INF/SYS/CAT unchanged (the same model as the signed virtual
  display driver) and verify those signatures before installing.
- In-box Microsoft class drivers need nothing. Hence the USB design: **one
  signed virtual host controller, then in-box class drivers** for whatever
  device is attached.

## Third-party drivers considered

| Component | Licence | Signature | Use |
|---|---|---|---|
| [usbip-win2](https://github.com/vadimgrn/usbip-win2) | [BSD-2-Clause](https://github.com/vadimgrn/usbip-win2/blob/master/LICENSE.txt) | `usbip2_ude` and `usbip2_filter` (`.sys` and `.cat`) signed by *Microsoft Windows Hardware Compatibility Publisher* (issuer *Microsoft Windows Third Party Component CA 2014*, EKU `1.3.6.1.4.1.311.10.3.5.1`): attestation-signed | **VM side of USB.** Pinned to the 0.9.7.5-preview driver files published by the [Open Source Codesigning Initiative](https://github.com/OSSign/vadimgrn--usbip-win2-old/releases/tag/0.9.7.5-preview), whose six files could be checked directly (`apps/windows_host/third_party/usbip-win2/upstream.json`) |
| [UsbDk](https://github.com/daynix/UsbDk) 1.0.22 | Apache-2.0 | MSI and binaries signed by *Red Hat, Inc.*; `UsbDk.sys` also carries the Microsoft attestation signature | **PC side of USB** (`apps/windows_client/third_party/usbdk/upstream.json`) |
| [WinFsp](https://github.com/winfsp/winfsp) 2.1 | GPL-3.0 with the FLOSS exception | MSI signed by Navimatics LLC; `winfsp-x64.sys` attestation-signed by Microsoft | **VM side of drives** (`apps/windows_host/third_party/winfsp/upstream.json`) |
| [Virtual-Audio-Driver](https://github.com/VirtualDrivers/Virtual-Audio-Driver) | MIT, plus MS-PL Sysvad code | no published release; its README requires test signing | excluded |
| [SynchronousAudioRouter](https://github.com/eiz/SynchronousAudioRouter) | GPL-3.0 | unsigned; requires test signing, Secure Boot unsupported | excluded |
| [com0com](https://sourceforge.net/projects/com0com/) 3.0.0.0 | GPL | installers signed by a non-Microsoft certificate that has expired; such a signature cannot load a kernel driver under Secure Boot on 1607+ | excluded |
| [vsmartcard](https://github.com/frankmorgner/vsmartcard) BixVReader | GPL | UMDF catalog signed by an untrusted *UMDF Test Certificate* | excluded |
| Windows TPM virtual smart card | in-box | a card bound to the VM's own TPM, not the user's card ([overview](https://learn.microsoft.com/en-us/windows/security/identity-protection/virtual-smart-cards/virtual-smart-card-overview)) | not redirection |
| In-box class drivers | Windows | `usbser.inf` (CDC), `wudfusbcciddriver.inf` (CCID), HID, mass storage ([USB class drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/supported-usb-classes)) | bind to forwarded devices |

Findings carried into the USB implementation:

- usbip-win2's VHCI device object is world read/write by design
  (`SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R`): any local user could attach
  devices from their own USB/IP server, make the kernel open outbound TCP
  connections as SYSTEM, write its persistent attach list and detach other
  users' devices ([vhci.cpp](https://github.com/vadimgrn/usbip-win2/blob/master/drivers/ude/vhci.cpp),
  [vhci_ioctl.cpp](https://github.com/vadimgrn/usbip-win2/blob/master/drivers/ude/vhci_ioctl.cpp)).
  In a VM that would bypass the administrator's USB and drive permissions, so
  the Host locks the device down before using it (below).
- Upstream 0.9.7.8 shipped a memory-corruption bug fixed in 0.9.8.0, and
  0.9.8.1 warns of possible regressions. Newer releases are adopted only after
  their catalogs are checked like the pinned one.
- The package INF turns on WDF verifier and verbose tracing. Multitap never runs
  the upstream installer or tools; it installs only the pinned INF/SYS/CAT
  files and switches those debugging registry values off afterwards.
- Memory Integrity (HVCI) compatibility of `usbip2_ude.sys`,
  `usbip2_filter.sys` and `UsbDk.sys` has not been established. Windows 11
  turns HVCI on by default on capable PCs, which matters most for UsbDk on the
  user's own PC.

## Security model

- **Grant.** Each class has a key in the assignment's device policy
  (`server/src/device-policy.mjs`): `drives`, `printers`, `usbDevices`,
  `smartCards`, `serialPorts`. A channel exists only when the client asked for it, the Host
  advertised the class and the administrator allowed it; a missing grant never
  refuses the stream itself. The console shows the keys under 장치 공유.
- **Revocation** closes only that channel: the Host removes its printer,
  unmounts the drives or detaches the USB devices, and the stream continues.
- **Raw USB cannot bypass the other permissions.** A device whose descriptors
  include another class needs that permission too: mass storage (08h)
  `drives`, printer (07h) `printers`, smart card (0Bh) `smartCards`, CDC
  (02h/0Ah) `serialPorts`. The broker and the Host both enforce this on the
  validated descriptors, not on client claims.
- **Default-denied USB classes** (broker, Host and client share one rule set):
  hubs (09h), wireless controllers and Bluetooth (E0h), audio (01h), video
  (0Eh), any isochronous endpoint, and boot-protocol HID keyboards and mice.
  The client also never offers keyboards, mice and USB network adapters that
  the PC itself needs, and every device needs an explicit choice per session.
- **VHCI hardening in the VM.** The Host sets the controller's devnode and
  device-object security descriptor to SYSTEM and Administrators only and proves
  it with a restricted-token open before attaching anything; its loopback
  USB/IP server accepts only the kernel's own connection (PID 4); nothing is
  written to usbip-win2's persistent attach list. See
  [usb-redirection.md](usb-redirection.md#host-vm-side).
- **Privacy.** Diagnostics are fixed stage/error codes and counts only: never
  file names, paths, document titles, USB vendor/product IDs, serial numbers or
  transferred data.
- **Licences.** All shipped parts can be distributed with Multitap (AGPL-3.0):
  usbip-win2 BSD-2, UsbDk Apache-2.0, WinFsp GPLv3 with the FLOSS exception. Signed drivers ship
  as separate, unmodified packages with their notices.

## Alternative not taken

QEMU's `usb-redir` on the Proxmox node (SPICE-style) needs no guest driver, but
it would need Proxmox privileges beyond the broker's dedicated audit/power
identity (VM hardware configuration, console or monitor access), sockets on the
node and hypervisor configuration changes.
