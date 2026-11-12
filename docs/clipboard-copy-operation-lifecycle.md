# Clipboard copy-operation lifecycle

With the on-paste revision of the clipboard channel
([remote-session-protocol.md](remote-session-protocol.md#on-paste-revision)) a
copy only announces an offer, and file bytes move when a consumer pastes. A
consumer such as Explorer extracts the peer's virtual files inside an
asynchronous operation of the data object
(`IDataObjectAsyncCapability::StartOperation` / `EndOperation`). This note
describes how the PC client and the Host track those operations: when somebody
counts as waiting (the transfer card and its cancel button on the PC,
`clipboard.paste.waiting` from the Host), and how a late notification of an
old paste is kept from affecting a newer one. None of this adds a relay
message, an admission rule or a permission.

## On the PC (pasting the VM's offer)

Code: `native/windows_clipboard/pull.hpp` (`PullHooks::operationState`),
`apps/windows_client/lib/data/clipboard_pull.dart`.

* The native data object reports each start and end with the offer's
  `offerId` and a local operation epoch in the event's `rangeId` field. Neither
  value goes into diagnostics or to the broker.
* Repeated starts or ends are idempotent. An end must match the current epoch
  of that offer; an epoch at or below one already retired is ignored, so a late
  end of an older offer or epoch cannot release a newer operation or replace
  its failure.
* A start alone is silent. Only an operation that has actually requested a file
  range counts as waiting: it keeps the transfer card through the gaps between
  files and stays until its consumer ends it, even after the last advertised
  byte, so the short linger after a single pull does not release a multi-file
  extraction early. A metadata-only extraction shows no progress.
* On an on-paste pair the app's card is the only progress (Explorer's own copy
  dialog is not requested); older pairs keep Windows' copy progress.
* Cancelling retires the local epoch, a withdrawn offer is removed, and closing
  the channel discards all operation state before the asynchronous native
  cleanup. A failure stops only the ranges of its own offer, and an older
  operation's failure does not overwrite a newer one's. Completed files are
  never removed or replayed.
* An end that reports a failure awaits native range cleanup, while the same
  offer may already be extracted again. Before publishing the failure, clearing
  the gap state or notifying the UI, the continuation checks an owner revision
  (invalidated by cancel and close) and a per-offer revision (advanced by every
  accepted start, end, request and publication; removed on withdrawal, so
  withdrawing and republishing the same ID cannot revive an old continuation).
  Queued current work is started before yielding to the old cleanup, and that
  cleanup still finishes its own old ranges after a cancel without touching a
  newer operation.

## On the Host (pasting the PC's offer)

Code: `apps/windows_host/src/clipboard_operation_state.hpp`, used by
`clipboard_worker.hpp` under its pull mutex.

* State is kept per published offer and channel generation: whether a reader is
  blocked, whether an operation is live and whether it requested data.
* `clipboard.paste.waiting` stays `active:true` while any reader is blocked or
  any live operation has requested data, so it holds across the gaps between
  files. A metadata-only operation stays silent, and one offer's end or
  unblocked read cannot clear another offer's wait. The change back to
  `active:false` is debounced by 1.5 s of quiet, so a paste of many small files
  is one waiting episode.
* Cancel, retire and close reset only their own state; retired offers and
  earlier channel generations cannot recreate it.
* A failed `EndOperation` produces only the existing fixed pull diagnostic
  (`hostAction 253`). It never sends a late `clipboard.error` for a retired
  range and never ends the pair.

## The EndOperation result

The `hResult` passed to `EndOperation` is the
[extraction outcome](https://learn.microsoft.com/en-us/windows/win32/api/shldisp/nf-shldisp-idataobjectasynccapability-endoperation);
the method's own return value only acknowledges the notification. Only a
`FAILED` result is a failure: `E_ABORT` and `HRESULT_FROM_WIN32(ERROR_CANCELLED)`
map to `clipboard_cancelled`, anything else to `clipboard_transfer_failed`
(`pullOperationFailure`). `S_FALSE` or `DROPEFFECT_NONE` alone is neither a
cancellation nor proof that every file was saved. `StartOperation` marks the
extraction as running, following its
[documented lifetime](https://learn.microsoft.com/en-us/windows/win32/api/shldisp/nf-shldisp-idataobjectasynccapability-startoperation).
Only fixed codes cross the native boundary; no raw HRESULT or exception text
reaches diagnostics.

## Tests

`apps/windows_client/test/clipboard_operations_test.dart` covers silent
metadata operations, gaps between files, release on end, duplicate starts and
ends, old offer and epoch rejection, failure cleanup, cancel, withdrawal,
close, same-ID republication and on-paste progress suppression.
