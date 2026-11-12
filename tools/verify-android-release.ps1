param(
    [Parameter(Mandatory=$true)][string]$Version,
    # Optional: resolved like build-android.ps1 (android-workspace.ps1), the checkout that owns .tools.
    [string]$ToolRoot
)
$ErrorActionPreference = 'Stop'
# Packaging verification only. No build, regression tests, installation or publication.
# Keep these limits/payload rules aligned with build-android.ps1 and
# publish-android-release.ps1; receipts are written only after all APKs pass.
. (Join-Path $PSScriptRoot 'android-workspace.ps1')
# The APK base versionCode (build.gradle.kts rule): preview N -> N, 1.0.z -> 100 + z.
$match = [regex]::Match($Version, '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-preview\.([1-9][0-9]{0,2}))?$')
$versionCode = Get-MultitapAndroidVersionCode -Version $Version
if (!$match.Success -or $Version.Length -gt 40 -or $null -eq $versionCode) { throw 'Expected x.y.z-preview.N (N=1..999) or 1.0.z (z=0..899).' }
$workspace = (Resolve-MultitapAndroidWorkspace -SourceRoot (Split-Path -Parent $PSScriptRoot) -ToolRoot $ToolRoot).Workspace
$boundary = $workspace + '\'
$utf8 = New-Object Text.UTF8Encoding($false, $true)
function Assert-SafePath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -ne $workspace -and !$full.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) { throw 'Artifact/tool path escaped ToolRoot.' }
    $cursor = $full
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Linked artifact/tool path refused.' }
        }
        if ($cursor -eq $workspace) { break }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}
function Read-Record([string]$Path) {
    Assert-SafePath $Path
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or $item.Length -le 0 -or $item.Length -gt 4MB) { throw 'Invalid Android receipt.' }
    return ([IO.File]::ReadAllText($Path, $utf8) | ConvertFrom-Json)
}
function Write-Immutable([string]$Path, [string]$Text) {
    Assert-SafePath $Path
    if (Test-Path -LiteralPath $Path) {
        if ([IO.File]::ReadAllText($Path, $utf8) -cne $Text) { throw 'Immutable verification metadata conflict.' }
        return
    }
    $bytes = $utf8.GetBytes($Text)
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
}
$androidRoot = Join-Path $workspace '.artifacts/android'
$inputRoot = Join-Path $androidRoot $Version
$releaseRoot = Join-Path $workspace '.artifacts/releases'
$build = Read-Record (Join-Path $inputRoot 'build-receipt.json')
if ($build.version -cne $Version -or $build.applicationId -cne 'me.totb.multitap_client' -or
    $build.minSdk -ne 26 -or $build.targetSdk -ne 36 -or $build.signerSha256 -cnotmatch '^[a-f0-9]{64}$' -or
    @($build.files).Count -ne 3) { throw 'Build receipt identity mismatch.' }
$signer = [string]$build.signerSha256
$sdk = Join-Path $workspace '.tools/android-sdk/build-tools/36.0.0'
$jdk = Join-Path $workspace '.tools/jdk-21.0.12.1+1'
foreach ($tool in @("$sdk/apksigner.bat", "$sdk/aapt.exe", "$jdk/bin/java.exe")) {
    Assert-SafePath $tool
    if (!(Test-Path -LiteralPath $tool -PathType Leaf)) { throw 'Required Android verification tool missing.' }
}
# Same historical signer continuity rule as the build; never read private key files.
foreach ($record in @(Get-ChildItem -LiteralPath $androidRoot -Recurse -File | Where-Object {
    ($_.Name -eq 'manifest.json' -or $_.Name -like 'build-receipt*.json') -and $_.Directory.Name -ne $Version
})) {
    $prior = Read-Record $record.FullName
    if (!$prior.signerSha256 -or $prior.signerSha256 -cne $signer) { throw 'Earlier Android signer differs or is unavailable.' }
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$abiCodes = [ordered]@{'arm64-v8a'=2; 'armeabi-v7a'=1; 'x86_64'=4}
$verified = @()
$locks = @()
$oldJava = $env:JAVA_HOME
try {
    $env:JAVA_HOME = $jdk
    foreach ($abi in $abiCodes.Keys) {
        $filename = "Multitap-$Version-android-$abi.apk"
        $path = Join-Path $inputRoot $filename
        Assert-SafePath $path
        $item = Get-Item -LiteralPath $path
        if ($item.PSIsContainer -or $item.Length -le 0 -or $item.Length -gt 128MB) { throw 'Invalid APK size/type.' }
        # Deny writes/deletion through verification and immutable copying.
        $locks += [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $sha = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $record = @($build.files | Where-Object { $_.name -ceq $filename -and $_.abi -ceq $abi })
        if ($record.Count -ne 1 -or $record[0].bytes -ne $item.Length -or $record[0].sha256 -cne $sha) { throw 'APK differs from build receipt.' }
        $signature = @(& "$sdk/apksigner.bat" verify --verbose --print-certs $path)
        if ($LASTEXITCODE -ne 0 -or !($signature -cmatch '^Number of signers: 1$')) { throw 'APK signature verification failed.' }
        $digests = @($signature | Where-Object { $_ -cmatch '^Signer #1 certificate SHA-256 digest: [a-fA-F0-9]{64}$' })
        if ($digests.Count -ne 1 -or ($digests[0] -replace '^Signer #1 certificate SHA-256 digest: ', '').ToLowerInvariant() -cne $signer) { throw 'APK signer mismatch.' }
        $badging = @(& "$sdk/aapt.exe" dump badging $path)
        if ($LASTEXITCODE -ne 0) { throw 'APK badging failed.' }
        $code = [int]$abiCodes[$abi] * 1000 + $versionCode
        if (!($badging -cmatch ("^package: name='me\.totb\.multitap_client' versionCode='" + $code + "' versionName='" + [regex]::Escape($Version) + "'( |$)")) -or
            !($badging -cmatch "^sdkVersion:'26'$" ) -or !($badging -cmatch "^targetSdkVersion:'36'$" ) -or
            !($badging -cmatch ("^native-code: '" + [regex]::Escape($abi) + "'$"))) { throw 'APK package/version/versionCode/ABI/SDK mismatch.' }
        $zip = [IO.Compression.ZipFile]::OpenRead($path)
        try {
            if ($zip.Entries.Count -le 0 -or $zip.Entries.Count -gt 8192) { throw 'Unexpected APK entry count.' }
            $names = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
            $sum = [int64]0
            $actualTotal = [int64]0
            foreach ($entry in $zip.Entries) {
                $name = $entry.FullName
                if (!$name -or $name.Length -gt 1024 -or !$names.Add($name) -or
                    $name -match '(^/|\\|[:\x00-\x1f\x7f]|//|(^|/)\.\.?(/|$))' -or
                    (($entry.ExternalAttributes -shr 16) -band 0xf000) -eq 0xa000 -or
                    $entry.Length -lt 0 -or $entry.Length -gt 256MB) { throw 'Unsafe/duplicate APK entry.' }
                $sum += $entry.Length
                if ($sum -gt 512MB) { throw 'APK expanded size exceeds limit.' }
                if ($name -match '(?i)(^|/)(\.local|\.env[^/]*|secrets|host\.dat|host-token[^/]*|settings\.json|login\.json|vault\.key|broker\.sqlite[^/]*|ssh\.json|preview-signing\.json|credentials\.dpapi)(/|$)|\.(exe|dll|pdb|dpapi|p12|pfx|jks|keystore|pem|key|sqlite|sqlite3)$') { throw 'Private/runtime/Windows payload in APK.' }
                if ($name.StartsWith('lib/') -and !$name.StartsWith("lib/$abi/", [StringComparison]::Ordinal)) { throw 'Unexpected native ABI in APK.' }
                $input = $entry.Open()
                try {
                    $prefix = New-Object byte[] 4096
                    $read = 0
                    while ($read -lt $prefix.Length) {
                        $count = $input.Read($prefix, $read, $prefix.Length - $read)
                        if ($count -eq 0) { break }
                        $read += $count
                    }
                    $text = [Text.Encoding]::UTF8.GetString($prefix, 0, $read)
                    if (($read -ge 2 -and $prefix[0] -eq 0x4d -and $prefix[1] -eq 0x5a) -or
                        $text.StartsWith('SQLite format 3') -or
                        $text -match '-----BEGIN (RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----|"(hostToken|sessionToken|clientSecret|tokenSecret|password)"\s*:\s*"[^"\r\n]+"') { throw 'Executable/private runtime content signature in APK.' }
                    # Inflate every entry, not just its prefix: do not trust ZIP
                    # declared sizes when enforcing the expansion limits.
                    $actual = [int64]$read
                    $buffer = New-Object byte[] 65536
                    while (($count = $input.Read($buffer, 0, $buffer.Length)) -gt 0) {
                        $actual += $count
                        if ($actual -gt 256MB -or $actualTotal + $actual -gt 512MB) { throw 'Actual APK expansion exceeds limit.' }
                    }
                    if ($actual -ne $entry.Length) { throw 'APK entry size mismatch.' }
                    $actualTotal += $actual
                } finally { $input.Dispose() }
            }
            foreach ($required in @('AndroidManifest.xml','classes.dex',"lib/$abi/libapp.so","lib/$abi/libflutter.so")) {
                if (@($zip.Entries | Where-Object { $_.FullName -ceq $required -and $_.Length -gt 0 }).Count -ne 1) { throw 'Missing APK product payload.' }
            }
            $entryCount = $zip.Entries.Count
        } finally { $zip.Dispose() }
        $verified += [pscustomobject]@{path=$path;name=$filename;abi=$abi;bytes=$item.Length;sha256=$sha;entryCount=$entryCount;expandedBytes=$sum;versionCode=$code}
    }
    # No verified=true receipt or release output exists until every ABI passed.
    foreach ($dir in @($releaseRoot, $inputRoot)) {
        Assert-SafePath $dir
        if (!(Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    }
    foreach ($apk in $verified) {
        $target = Join-Path $releaseRoot $apk.name
        Assert-SafePath $target
        if (!(Test-Path -LiteralPath $target)) { [IO.File]::Copy($apk.path, $target, $false) }
        $copy = Get-Item -LiteralPath $target
        if ($copy.PSIsContainer -or $copy.Length -ne $apk.bytes -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -cne $apk.sha256) { throw 'Immutable release APK conflict.' }
        # The publisher's contentSignaturesVerified flag covers its bounded
        # per-entry prefix patterns, not a scan of every byte for secrets.
        $receipt = [ordered]@{policyVersion=1;product='android-client';version=$Version;abi=$apk.abi;filename=$apk.name;size=$apk.bytes;sha256=$apk.sha256;signerSha256=$signer;payloadVerified=$true;signatureVerified=$true;contentSignaturesVerified=$true;contentScanPrefixBytes=4096;inflatedAllEntries=$true;minSdk=26;targetSdk=36;deviceValidated=$false;entryCount=$apk.entryCount;expandedBytes=$apk.expandedBytes;versionCode=$apk.versionCode}
        Write-Immutable ($target + '.verification.json') ((ConvertTo-Json -InputObject $receipt -Depth 5) + "`n")
    }
    $files = @($verified | ForEach-Object { [ordered]@{name=$_.name;abi=$_.abi;bytes=$_.bytes;sha256=$_.sha256} })
    $manifest = [ordered]@{version=$Version;applicationId='me.totb.multitap_client';minSdk=26;targetSdk=36;signerSha256=$signer;files=$files;deviceValidated=$false}
    Write-Immutable (Join-Path $inputRoot 'manifest.json') ((ConvertTo-Json -InputObject $manifest -Depth 6) + "`n")
    Write-Output "Android packaging verified: $Version (3 ABIs). Device/live-VM acceptance remains unverified."
} finally {
    foreach ($stream in $locks) { $stream.Dispose() }
    $env:JAVA_HOME = $oldJava
}
