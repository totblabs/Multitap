# Thumbnail privacy epoch fixture

Launcher cards may show a recent picture of a desktop. The Host tells the
broker whether such a picture may be taken now, and the broker exposes it only
while that report is current. Locked and sign-in screens are never captured.
The picture sources and how they use this epoch are described in
[windows-signin-thumbnail-privacy.md](../../../../docs/windows-signin-thumbnail-privacy.md).

## The capability

The Host's optional, read-only capability is exactly
`thumbnailPrivacy:{protocol:'thumbnail-privacy-v1', epoch:<32 lowercase hex>, allowed:<boolean>}`.
It carries no Windows account, SID, logon ID, pixels or credentials, and it is
never used for streaming, display, input or login admission.

- **Epoch** (`apps/windows_host/src/thumbnail_privacy.hpp`): a random 16-hex
  owner prefix, fixed per Host bridge, plus a 16-hex monotonic counter. Every
  non-empty batch of Windows session (WTS) notifications advances the counter,
  also when no stream is active; ordinary polling does not.
- **Observations** capture a ticket before the Windows status reads. A stale
  read can only deny its own epoch, never mark a successor allowed or denied. A
  denial is final for its epoch; a later fresh successful read moves to a new
  epoch, so a transient uncertainty recovers without another WTS event. Allowed
  needs a current interactive, unlocked session on the same console with an
  unchanged session selection; anything unknown is denied. If the random
  prefix cannot be created the capability is omitted (a later WTS event
  retries), and counter exhaustion denies instead of wrapping. Neither affects
  Host start or streaming.
- **Broker** (`server/src/thumbnail-privacy.mjs`, used by `relay.mjs` and the
  preview capture in `desktop-previews.mjs`): validates the exact schema and a
  positive counter; malformed or contradictory privacy is dropped without
  rejecting the other capabilities. Per authenticated Host channel it keeps the
  owner prefix, the highest counter and the allowed bit, rejecting older
  counters, a foreign prefix and an allowed-after-denied replay within one
  epoch. The desktop list shows the value only for the current, open Host
  channel with a fresh report (no older than three heartbeats); otherwise the
  field is absent, and clients must not read absence as permission. Epoch
  changes reach the list fingerprint even when the allowed bit ends up where it
  was. A new channel starts unknown.

## Tests

- `fixture.cpp` (`MultitapHostThumbnailPrivacyFixture`): the production epoch
  class in memory, without Windows headers: start and restart, lock/unlock
  history, old allowed/denied observations, uncertainty and repeated recovery
  without WTS events, prefix failure and retry, monotonic counters, counter
  exhaustion, late initialisation and the schema.
- `server/test/thumbnail-privacy.test.mjs`: the broker's validator, freshness
  and monotonicity helpers, relay decoration and the desktop-list fingerprint;
  its last case runs the relay's attach/message/close/reconnect hooks over an
  in-memory emitter (new channel unknown, ordinary capabilities kept despite bad
  privacy, a late old-channel message rejected). No socket, store or network.

```powershell
$cmake = '.tools/visualstudio-buildtools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S apps/windows_host/test/thumbnail_privacy -B <build> -A x64
& $cmake --build <build> --config Release
& <build>/Release/MultitapHostThumbnailPrivacyFixture.exe
node --test --test-concurrency=1 server/test/thumbnail-privacy.test.mjs
```
