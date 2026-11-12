# Launcher pictures and privacy

Launcher cards can show a small picture of each assigned desktop. A picture is
screen content, so it must never outlive the Windows sign-in that produced it:
after a lock, sign-out or user switch on the VM, the old picture is not shown
again, and the lock screen, the sign-in screen and UAC prompts are never
captured.

There are two sources of pictures:

| | Host preview | Last streamed screen |
|---|---|---|
| Clients | Windows and Android | Windows only |
| Taken by | the Host, when the broker asks | the client's session window |
| Kept | broker memory, at most 10 minutes | the PC user's `%LOCALAPPDATA%` |
| Needs a prior connection | no | yes |

Both are bound to the same privacy epoch.

## Privacy epoch (`thumbnail-privacy-v1`)

The Host reports `thumbnailPrivacy: {protocol, epoch, allowed}` in its
capabilities (`apps/windows_host/src/thumbnail_privacy.hpp`). The epoch is 32
hex digits: a random 64-bit prefix per service run and a 64-bit counter. Every
Windows session event (lock, unlock, sign-in, sign-out, console connect)
advances the counter. `allowed` is true only while a signed-in console session
is unlocked. An observation that started before a change cannot label the newer
state, and after a denial the next allowed report always carries a new epoch.

The broker accepts the value only in order for one Host connection
(`server/src/thumbnail-privacy.mjs`). It passes it to clients in the desktop
list (`host.thumbnailPrivacy`) only while the Host's capability report is
fresh, at most three heartbeats old. The raw Host field is never forwarded. The
epoch grants nothing: it is not a permission to stream, send input or capture.

## Host previews (`desktop-preview-v1`)

A recent picture on the card without connecting first:

- **On demand.** A client asks `GET /api/v1/desktops/:id/preview` about once a
  minute for each assigned desktop whose list entry carries `previewProtocol`.
  The broker asks that desktop's Host (`host.thumbnail.request`) at most once
  per 30 s per desktop. Nothing is captured while nobody looks.
- **Assigned accounts only.** The route checks the account and its assignment
  before asking the Host and again after the wait, with a per-account rate
  limit.
- **Capture.** The Host service checks the console session first: no signed-in
  session answers `signin`, a locked one `locked`, and nothing is started.
  Only an unlocked console session gets one short run of `MultitapHost.exe
  --thumbnail-capture` with the signed-in user's own unelevated token (no
  window, 8 s limit). With that token it cannot open Winlogon's secure desktop,
  and it answers `locked` without a picture when the input desktop is not
  `Default`, so the lock screen, the sign-in screen and UAC prompts are never
  captured. The picture is the primary monitor scaled to at most 480x270, JPEG
  of at most 40,000 bytes, at most one capture per 10 s
  (`apps/windows_host/src/thumbnail_capture.hpp`). A session change during the
  capture discards it.
- **Memory only.** The broker keeps the JPEG in memory for at most 10 minutes,
  bound to the epoch it was taken in. A changed epoch, a Host that disconnects,
  or a `locked`/`signin` answer drops it. It is never written to disk, the
  audit log, diagnostics or any administrator view
  (`server/src/desktop-previews.mjs`). A dark output answers `blank` and keeps
  the previous picture of the same epoch.
- **Off switch.** 미리보기 화면 끄기 in the admin console (System) turns
  previews off for the whole broker. It forgets every picture at once, and the
  list stops offering `previewProtocol`, so clients stop asking.

The card shows the picture's age from the broker's `ageMs`, not from the
client's clock.

## Last streamed screen (Windows client)

The session window writes the last decoded frame, scaled to at most 480x270,
when the stream ends normally and about once a minute while it runs
(`windows/runner/thumbnail_writer.cpp`). The file is
`%LOCALAPPDATA%\Multitap\thumbnails\<key>\<desktop>.jpg`, where `<key>` is the
first 16 hex digits of SHA-256(broker origin + newline + account id). Pictures
are never uploaded and never enter logs or diagnostics. All scaling, encoding
and file work runs on one background thread.

The epoch binding:

- A session window writes pictures only when the launcher handed it the
  desktop's current allowed epoch. The epoch is stored inside the JPEG (a COM
  marker `Multitap:thumbnail-privacy-v1:<epoch>`).
- The launcher reads a file only while the broker's fresh projection allows
  that desktop's epoch, and only if the epoch in the file is that epoch. A
  picture from before a lock, sign-out or restart is therefore never shown,
  even when the old file is still on disk.
- When a stream ends in any way other than an ordinary user disconnect (a
  failure, a lost connection, the Host ending it), the client revokes the
  picture before the media engine retires its last frame. When the launcher
  sees a desktop's epoch denied or replaced, it revokes the old epoch too.
- A revocation is immediate in memory: queued writes for that target are
  cancelled and a late write cannot publish. On disk it writes a `.revoked`
  marker holding the revoked epoch, which survives a crash of the client, and
  deletes the picture through the exact file handle it checks. A newer picture
  under the same name is never deleted by an older revocation. The launcher
  treats an unreadable or malformed marker as revoked.
- Sign-out deletes the account's folder; signing in to another account or
  server hides the other folders.

Notices between the native writer and the launcher carry only the account key,
desktop id, a generation number, the epoch and an `invalidated` flag, never
pixels or user names. Accepting a revocation does not claim that the deletion
on disk has finished.

## Limits

- The epoch only follows Windows session events and the Host's view of the
  console session. It does not prove which Windows user is signed in.
- Deleting old files is best effort; the guarantee is that a file from another
  epoch is never shown.
- Android shows only Host previews; it keeps no pictures of its own.
