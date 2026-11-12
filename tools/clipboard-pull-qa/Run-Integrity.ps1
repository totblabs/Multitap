param(
  [Parameter(Mandatory = $true)][string]$Executable,
  [ValidateSet('PublisherElevated', 'PasterElevated')][string]$Direction = 'PublisherElevated'
)
# Cross-integrity check of the paste-pull data object (docs/remote-session-protocol.md, paste-pull profile).
# One process publishes an offer on the OLE clipboard (--publish) and another extracts it with the folder drop target
# (--paste), the way Explorer's Paste does. One of them runs elevated, the other not. Elevation is requested with
# Start-Process -Verb RunAs, so this only runs unattended where the UAC admin prompt is "elevate without prompting"
# (ConsentPromptBehaviorAdmin = 0); elsewhere it shows the prompt. It REPLACES the clipboard content; the previous text
# or file list is put back.
$ErrorActionPreference = 'Stop'
$files = $null; $text = $null
try { $files = Get-Clipboard -Format FileDropList } catch {}
if (-not $files) { try { $text = Get-Clipboard -Raw } catch {} }
$work = Join-Path ([IO.Path]::GetTempPath()) ('multitap-pull-il-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $work | Out-Null
$ready = Join-Path $work 'ready.txt'; $done = Join-Path $work 'done.txt'; $pub = Join-Path $work 'pub'; $dest = Join-Path $work 'dest'
$exitCode = 1
try {
  $publishArguments = "--publish `"$pub`" `"$ready`" `"$done`""
  $pasteArguments = "--paste `"$pub`" `"$dest`""
  if ($Direction -eq 'PublisherElevated') { Start-Process -FilePath $Executable -ArgumentList $publishArguments -Verb RunAs -WindowStyle Hidden }
  else { Start-Process -FilePath $Executable -ArgumentList $publishArguments -WindowStyle Hidden }
  for ($i = 0; $i -lt 150 -and -not (Test-Path $ready); $i++) { Start-Sleep -Milliseconds 200 }
  if (-not (Test-Path $ready)) { throw 'The publisher did not become ready.' }
  'publisher: ' + (Get-Content $ready)
  if ($Direction -eq 'PasterElevated') {
    Start-Process -FilePath $Executable -ArgumentList $pasteArguments -Verb RunAs -Wait -WindowStyle Hidden
    # An elevated process cannot be captured; its result is the extracted tree.
    $exitCode = if (Test-Path (Join-Path $dest 'Folder')) { 0 } else { 1 }
  } else {
    $process = Start-Process -FilePath $Executable -ArgumentList $pasteArguments -PassThru -Wait -RedirectStandardOutput (Join-Path $work 'paste.out') -RedirectStandardError (Join-Path $work 'paste.err')
    Get-Content (Join-Path $work 'paste.out'), (Join-Path $work 'paste.err') -ErrorAction SilentlyContinue
    $exitCode = $process.ExitCode
  }
} finally {
  New-Item $done -ItemType File -Force | Out-Null
  Start-Sleep -Seconds 2
  if (Test-Path "$ready.final") { Get-Content "$ready.final" }
  if ($files) { Set-Clipboard -Path $files.FullName } elseif ($text) { Set-Clipboard -Value $text }
  Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
exit $exitCode
