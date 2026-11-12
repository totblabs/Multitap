# Streaming printing (print-v1)

A document printed in the VM arrives at the user's PC as a PDF, and the client
prints it on a printer of that PC (or saves it) after the user chooses, like
RDP Easy Print or Horizon integrated printing. No driver is installed on either
side. The wire protocol, negotiation and limits are in
[remote-session-protocol.md](remote-session-protocol.md#printing-print-v1);
this document is what a person meets and how each case is handled. Folders are
in [drive-redirection.md](drive-redirection.md), the overview of all
redirections in [streaming-redirection.md](streaming-redirection.md).

## Permission

`printers`, per assignment (user × VM), default denied; in the administrator
console it is 장치 공유 → 프린터. The broker checks it when it admits the
session and the relay keeps checking it (the 250 ms authorisation sweep and the
assignment update hook). Revoking it closes only the print channel
(`print_policy_changed`); the Host removes its printer and the stream goes on.
Without the permission there is no channel and nothing is shown.

## How it works

```
VM app -> "Multitap 프린터 (PC)" (in-box Print To PDF driver, loopback RAW port)
       -> Host service keeps the PDF in a delete-on-close file and offers it in
          128 KiB chunks over the session's own relayed print socket
       -> client keeps it under %LOCALAPPDATA%\Multitap\Client\print\<session>
       -> a dialog: 인쇄 (a printer of this PC, copies) / PDF로 저장 / 취소
       -> the Host is told printed | saved | cancelled | failed (a fixed word)
```

* Nothing prints without the person pressing 인쇄. One dialog at a time, with a
  page preview; the documents waiting behind it are counted in its subtitle
  (`대기 N개 더`).
* While the channel is open the queue is the session user's default printer,
  so an ordinary Print goes to the PC. The default is a per-user setting, so a
  helper started with that user's token (`MultitapHost.exe --print-default`)
  saves the previous default in a per-user record, sets the queue and restores
  the previous default when the channel ends, but only if the default is still
  the Multitap queue: a default the user picked meanwhile stays. A record left by
  a crash is settled at the next claim or Host start. "Windows가 기본 프린터
  관리" (`LegacyDefaultPrinterMode`) is left as the user has it
  (`apps/windows_host/src/print_default.hpp`).
* Layout settings are the PC printer's own defaults: paper source, colour and
  duplex are not offered in the dialog, and the dialog says so. A page is scaled
  down (never up) to the printable area and the paper is turned to the page's
  orientation, so Letter on A4 and landscape pages come out whole.
* Limits: 256 MiB per document; the Host keeps at most 8 documents / 512 MiB
  waiting (while that is full it leaves the spooler's next connection waiting,
  so the VM-side job stays queued instead of failing); the client keeps at most
  16 unprocessed documents.

## Requirements in the VM

The Host advertises `print-v1` only when the Print Spooler runs and the in-box
"Microsoft Print To PDF" driver and "Standard TCP/IP Port" monitor are present.
Otherwise it reports `printUnavailableReason` (`print_spooler_unavailable` or
`print_driver_unavailable`); the broker shows it only to accounts whose
assignment allows `printers`, and the client says it once after connecting.
Host settings → 장치 → 프린터 shows the same state and can start a stopped
spooler (인쇄 스풀러 시작). A spooler set to Disabled is often deliberate
hardening and is never changed.

## Diagnostics

* Host: `hostAction 242` with a fixed `stage`
  (`apps/windows_host/src/print_redirection.hpp`, `PrintStage`). Stages 27-31
  are the default-printer steps: 27 claimed, 28 restored, 29 kept (the user had
  chosen another default), 30 failed, 31 recovered after a crash. For stage 30
  `error` is the reason (`print_default.hpp`, `DefaultFailure`: 1 no Windows
  session, 2 helper did not answer, 3 record unreadable, 4 record not writable,
  5 Windows refused SetDefaultPrinter, 6 Windows refused the restore,
  7 unexpected, 8 helper not started) and `cause` the Windows error.
* Client records are fixed codes and counts only; document titles, names, paths
  and content are never logged.

## Cases

### On the VM

| Case | What happens | Message · diagnostic |
|---|---|---|
| An app prints to the Multitap printer | Every app renders through the same Print To PDF driver; the Host receives one PDF per spooler connection and offers it | `VM에서 문서를 받는 중` bar after 1.2 s, then the dialog |
| The app prints to another printer | The queue is the default only while claimed; the ready notice names the printer so it can be chosen | `VM에서 「Multitap 프린터 (PC)」로 인쇄하면 이 PC에서 인쇄할 수 있어요.` |
| The default printer could not be claimed | The printer still exists and works when chosen; the reason and the Windows error are recorded. Not retried blindly | `hostAction 242` stage 30 + `error` + `cause` |
| Several documents queued in the VM | Up to 8 / 512 MiB kept, sent one at a time with an acknowledgement window; one dialog at a time | `대기 N개 더` |
| A document above 256 MiB | Read to its end (so the VM queue completes) and declined | `문서가 너무 커서(256MB 초과) 받을 수 없어요.` |
| The Host's temporary storage is full or unusable | Read to its end and declined | `VM에 문서를 보관할 공간이 없어 받지 못했어요.` |
| Spooler stopped, PDF driver or TCP port monitor missing | `print-v1` is not advertised (nothing is installed or started); the reason is said once | `이 VM에서는 PC로 인쇄할 수 없어요 · …` |
| Another printer already has the name | Kept, never touched | `VM에 같은 이름의 다른 프린터가 있어 만들지 않았어요. …` |
| The Host or session ends with a document in the spooler | The printer is removed (its documents purged); crash leftovers (printers on `Multitap_Print_<n>` ports, unused ports, stale document files) are removed at the next service start; the default is restored while the queue still exists | stages 2, 9, 28, 31 |

### On the PC

| Case | What happens | Message |
|---|---|---|
| The PC has no printers | PDF로 저장 only; 인쇄 disabled with its reason | `이 PC에 설치된 프린터가 없어요. PDF로 저장할 수 있어요.` |
| The chosen printer is offline | Said before printing; printing stays possible (the job waits in that printer's queue) | `프린터가 오프라인이에요. 켜지면 대기열의 문서가 인쇄돼요.` |
| Paused | Same | `프린터가 일시 중지돼 있어요. 다시 시작해야 인쇄돼요.` |
| Out of paper, jam, cover open, no toner, user intervention | Same (priority: offline > paused > error > attention) | `프린터에 용지·토너·덮개 등 확인할 것이 있어요.` |
| Printer error | Same | `프린터에 문제가 있어요. 프린터 상태를 확인해 주세요.` |
| The printer disappeared between listing and printing | Refused before anything is sent | `선택한 프린터를 찾을 수 없어요. …` |
| The person cancels (취소 or Esc) | The document is deleted and the Host hears `cancelled`; the VM-side job had already finished, so a toast says so | `인쇄를 취소했어요 · 필요하면 VM에서 다시 인쇄해 주세요` |
| The PC printer is "Microsoft Print to PDF" and its save dialog is closed | A cancel, not a failure: the print dialog stays open | none |
| PDF로 저장 | Windows' save dialog; a link or a file with several names is refused; closing the save dialog keeps the print dialog open | `PDF로 저장했어요.` |
| Paper size and orientation differ | Scaled down to the printable area, centred, paper turned to the page; never enlarged | none |
| Colour, duplex, paper tray, quality | The printer's own defaults (Windows printing preferences) | `용지·색상·양면은 프린터의 기본 설정을 따라요.` |
| Copies | 1 to 20, collated | stepper tooltip |
| The PDF cannot be opened | Print refused; saving still works | `문서를 열 수 없어요.` |
| A long print | The dialog shows page progress after 1.2 s; after the last page the job belongs to the spooler | progress bar |
| The session window is behind other windows or minimised | The taskbar button flashes; nothing is moved or activated | flash |
| The PC's disk is full or the folder cannot be written | That document is declined (`print.cancel`); the printer and later documents stay | `이 PC의 저장 공간이 부족하거나 쓸 수 없어서 문서를 받지 못했어요.` |
| 16 unprocessed documents | The 17th is declined | `아직 처리하지 않은 인쇄가 많아 새 문서를 받지 않았어요.` |
| Digest mismatch | Deleted, `failed` reported, never offered | `문서를 받는 중 문제가 생겨 인쇄하지 못했어요.` |

### Ending and permissions

| Case | What happens | Message |
|---|---|---|
| The channel ends while a document is arriving | The partial file is deleted | counted in the notice below |
| The channel ends while a dialog is open | The document on screen stays on disk until the person finishes (print and save still work; the Host hears nothing) | none |
| The channel ends with documents waiting | They can no longer be offered; the notice says how many | `프린터 연결이 끊겼어요. 아직 처리하지 않은 문서 N개는 받을 수 없게 됐어요. 다시 연결한 뒤 VM에서 다시 인쇄해 주세요.` |
| The administrator removes `printers` | The channel closes, the Host removes its printer, the stream continues | `관리자가 프린터 사용을 해제했어요.` |
| A stalled or hostile transfer | Idle 60 s at the broker or 90 s at the client, or 30 min for one document, closes the channel | `프린터 연결이 끊겼어요. 다시 연결하면 인쇄할 수 있어요.` |

### Android

The Android app requests `print-v1` too. Each document opens Android's own print
screen (PrintManager, `AndroidPrint.kt`), where the user picks a printer or "Save
as PDF", one document at a time; the outcome is `printed` when Android wrote the
document, else `cancelled`. A document stays readable until that screen is
finished, even if the channel ends meanwhile. The app shows the same notices as
the Windows client (printer ready, printer not created, document skipped,
channel lost). Android has no folder sharing; the administrator's permission
text says so.
