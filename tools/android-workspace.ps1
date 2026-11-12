# Workspace resolution (and the APK versionCode rule) shared by build-android.ps1, verify-android-release.ps1 and
# publish-android-release.ps1. Dot-source only; defines functions and changes nothing.
#
# The workspace is the checkout that physically owns .tools; its .tools, .local and .artifacts are used.
# Starting point: -ToolRoot, else the source checkout when it has .tools, else the main checkout (Git common
# directory). A worktree's .tools junction is resolved to its target, so a worktree build uses the main
# checkout's toolchain, preview key and earlier APKs through canonical paths. Gradle and the Kotlin DSL
# compare canonical paths: a GRADLE_USER_HOME, JAVA_HOME or Flutter SDK reached through a junction breaks
# their caches and the daemons they reuse.

function Resolve-MultitapCanonicalPath {
  # Full path with every junction or symbolic-link component replaced by its target. A missing tail is kept.
  param([Parameter(Mandatory)][string]$Path)
  $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
  for ($depth = 0; $depth -lt 32; $depth++) {
    $root = [IO.Path]::GetPathRoot($full)
    $parts = @($full.Substring($root.Length).Split('\') | Where-Object { $_ })
    $current = $root.TrimEnd('\')
    $redirected = $false
    for ($i = 0; $i -lt $parts.Count; $i++) {
      $next = $current + '\' + $parts[$i]
      $rest = if ($i + 1 -lt $parts.Count) { $parts[($i + 1)..($parts.Count - 1)] -join '\' } else { '' }
      $item = Get-Item -LiteralPath $next -Force -ErrorAction SilentlyContinue
      if (-not $item) { $current = if ($rest) { $next + '\' + $rest } else { $next }; break }
      if ($item.LinkType -eq 'Junction' -or $item.LinkType -eq 'SymbolicLink') {
        $target = [string](@($item.Target)[0])
        if (-not $target) { throw "Unreadable link: $next" }
        $target = $target -replace '^\\\\\?\\', '' -replace '^\\\?\?\\', ''
        if (-not [IO.Path]::IsPathRooted($target)) { $target = Join-Path $current $target }
        $full = [IO.Path]::GetFullPath($(if ($rest) { Join-Path $target $rest } else { $target })).TrimEnd('\')
        $redirected = $true
        break
      }
      $current = $next
    }
    if (-not $redirected) { if ($current.EndsWith(':')) { return $current + '\' } else { return $current } }
  }
  throw "Too many links: $Path"
}

function Resolve-MultitapAndroidWorkspace {
  param([Parameter(Mandatory)][string]$SourceRoot, [string]$ToolRoot)
  $start = $ToolRoot
  if (-not $start) {
    if (Test-Path -LiteralPath (Join-Path $SourceRoot '.tools')) { $start = $SourceRoot }
    else {
      $common = (& git -C $SourceRoot rev-parse --path-format=absolute --git-common-dir 2>$null)
      if ($LASTEXITCODE -ne 0 -or -not $common) { throw 'No .tools in this checkout and no Git main checkout. Pass -ToolRoot <main checkout>.' }
      $start = Split-Path -Parent ([string]$common).Trim()
    }
  }
  $declared = Join-Path ([IO.Path]::GetFullPath($start).TrimEnd('\')) '.tools'
  if (-not (Test-Path -LiteralPath $declared -PathType Container)) { throw "No .tools under $start. Pass -ToolRoot <main checkout>." }
  $tools = Resolve-MultitapCanonicalPath $declared
  $workspace = Split-Path -Parent $tools
  if (-not (Test-Path -LiteralPath (Join-Path $workspace 'flutter.cmd'))) { throw "The owner of $tools ($workspace) has no flutter.cmd. Pass -ToolRoot <main checkout>." }
  [pscustomobject]@{
    Workspace = $workspace
    Tools = $tools
    DeclaredTools = $declared
    ToolsWasLinked = -not [string]::Equals($declared, $tools, [StringComparison]::OrdinalIgnoreCase)
    Sdk = Join-Path $tools 'android-sdk'
    Jdk = Join-Path $tools 'jdk-21.0.12.1+1'
    GradleUserHome = Join-Path $tools 'android-gradle-cache'
    FlutterSdk = Join-Path $tools 'flutter'
    Flutter = Join-Path $workspace 'flutter.cmd'
  }
}

function Test-MultitapLinkedToolPath {
  # True when a Gradle or Kotlin daemon's executable or command line reaches $Tools through a link
  # (e.g. C:\worktree\.tools\... while $Tools is C:\main\.tools). Pure apart from reading link targets.
  param([Parameter(Mandatory)][string]$Tools, [string]$ExecutablePath, [string]$CommandLine)
  if ([string]$CommandLine -notmatch 'org\.gradle\.launcher\.daemon\.bootstrap\.GradleDaemon|org\.jetbrains\.kotlin\.daemon\.KotlinCompileDaemon') { return $false }
  $canonical = $Tools.TrimEnd('\')
  $prefixes = @([regex]::Matches(([string]$ExecutablePath + ';' + [string]$CommandLine), '(?i)[A-Z]:\\[^;"\s=]*?\\\.tools(?=\\)') | ForEach-Object { $_.Value }) | Sort-Object -Unique
  foreach ($prefix in $prefixes) {
    if ([string]::Equals($prefix, $canonical, [StringComparison]::OrdinalIgnoreCase)) { continue }
    if ([string]::Equals((Resolve-MultitapCanonicalPath $prefix), $canonical, [StringComparison]::OrdinalIgnoreCase)) { return $true }
  }
  return $false
}

function Get-MultitapAndroidVersionCode {
  # The APK base versionCode of an Android versionName, as android/app/build.gradle.kts derives it (keep both equal):
  # x.y.z-preview.N -> N (1..999); 1.0.z -> 100 + z (z = 0..199); 1.1.z -> 300 + z (z = 0..699). Each split APK carries 1000 * ABI + code, and the
  # released in-app updater installs only a higher code in the same ABI thousand, so the code stays below 1000.
  # $null for any other name (no code can be derived; the build refuses it).
  param([Parameter(Mandatory)][string]$Version)
  if ($Version -cmatch '^\d+\.\d+\.\d+-preview\.([1-9]\d{0,2})$') { return [int]$Matches[1] }
  if ($Version -cmatch '^1\.0\.(0|[1-9]\d{0,2})$' -and [int]$Matches[1] -le 199) { return 100 + [int]$Matches[1] }
  if ($Version -cmatch '^1\.1\.(0|[1-9]\d{0,2})$' -and [int]$Matches[1] -le 699) { return 300 + [int]$Matches[1] }
  return $null
}

function Get-MultitapAndroidSourceRevision {
  # An exported source folder has no Git history. Its exact staged input
  # hashes remain the provenance; never borrow a containing repository's HEAD.
  param([Parameter(Mandatory)][string]$SourceRoot, [string]$GitExecutable)
  if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot '.git'))) {
    return [pscustomobject]@{ commit = $null; uncommittedChanges = $null; state = 'export_without_git' }
  }
  if (-not $GitExecutable -or -not (Test-Path -LiteralPath $GitExecutable -PathType Leaf)) {
    $gitCommand = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if (-not $gitCommand) { throw 'Git is required to record revision metadata for this Git checkout.' }
    $GitExecutable = $gitCommand.Source
  }
  $commit = & $GitExecutable -C $SourceRoot rev-parse --verify HEAD 2>$null
  if ($LASTEXITCODE -ne 0 -or [string]$commit -cnotmatch '^[a-f0-9]{40}(?:[a-f0-9]{24})?$') { throw 'Cannot record the Android source Git revision.' }
  $changes = & $GitExecutable -C $SourceRoot status --porcelain -- apps/windows_client tools/build-android.ps1 tools/android-workspace.ps1 2>$null
  if ($LASTEXITCODE -ne 0) { throw 'Cannot record Android source Git status.' }
  return [pscustomobject]@{ commit = [string]$commit; uncommittedChanges = [bool]$changes; state = 'git_revision' }
}

function Get-MultitapLinkedGradleDaemon {
  # Running Gradle/Kotlin daemons of this toolchain that were started through a linked .tools path.
  param([Parameter(Mandatory)][string]$Tools)
  foreach ($process in @(Get-CimInstance Win32_Process -Filter "Name='java.exe'" -ErrorAction SilentlyContinue)) {
    if (Test-MultitapLinkedToolPath -Tools $Tools -ExecutablePath $process.ExecutablePath -CommandLine $process.CommandLine) {
      [pscustomobject]@{ ProcessId = $process.ProcessId; ExecutablePath = $process.ExecutablePath; CreationDate = $process.CreationDate }
    }
  }
}
