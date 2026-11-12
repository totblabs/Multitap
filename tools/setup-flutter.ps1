$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkVersion = '3.47.7'
$expectedSha256 = 'c2c875da25a1a1448d18ee4e8f398b2f6fce888b4775d8f9130538e16d826cb4'
$archiveName = "flutter_windows_$sdkVersion-stable.zip"
$archivePath = Join-Path $projectRoot ".tools/downloads/$archiveName"
$sdkPath = Join-Path $projectRoot '.tools/flutter'

if (Test-Path -LiteralPath (Join-Path $sdkPath 'bin/flutter.bat')) {
    Write-Output "Flutter already exists at $sdkPath"
    exit 0
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $archivePath) | Out-Null
if (-not (Test-Path -LiteralPath $archivePath)) {
    & curl.exe --fail --location --output $archivePath "https://storage.googleapis.com/flutter_infra_release/releases/stable/windows/$archiveName"
    if ($LASTEXITCODE -ne 0) { throw 'Flutter SDK download failed.' }
}
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expectedSha256) {
    throw "SHA-256 mismatch: $archivePath"
}

$sevenZip = Join-Path $projectRoot '.tools/7zip/7z.exe'
if (Test-Path -LiteralPath $sevenZip) {
    & $sevenZip x $archivePath "-o$(Join-Path $projectRoot '.tools')" -y -bso0 -bsp0
    if ($LASTEXITCODE -ne 0) { throw 'Flutter SDK extraction failed.' }
} else {
    Expand-Archive -LiteralPath $archivePath -DestinationPath (Join-Path $projectRoot '.tools')
}
Write-Output "Flutter $sdkVersion ready at $sdkPath"
Write-Output 'Run flutter.cmd doctor -v to check Windows C++ build prerequisites.'
