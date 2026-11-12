param([switch]$CreatePreviewKey, [string]$Version = '', [string]$ToolRoot = '', [switch]$ResolveOnly, [switch]$VersionCheckOnly)
$ErrorActionPreference = 'Stop'
# $root holds the source that is staged and built. $workspace holds the local
# toolchain (.tools), the private preview key (.local) and the artifacts
# (.artifacts): the checkout that physically owns .tools (android-workspace.ps1).
# In the main checkout that is the checkout itself; a git worktree, with no
# .tools or with a .tools junction, uses the main checkout's (never copied
# into the worktree, never committed). Toolchain paths are canonical: Gradle's
# Kotlin DSL breaks when GRADLE_USER_HOME/JAVA_HOME/the Flutter SDK are reached
# through a junction.
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'android-workspace.ps1')
$paths = Resolve-MultitapAndroidWorkspace -SourceRoot $root -ToolRoot $ToolRoot
$workspace = $paths.Workspace
if ($ResolveOnly) {
    # Prints the paths this build would use, and the stale daemons it would stop; builds nothing.
    [ordered]@{sourceRoot=$root;workspace=$workspace;tools=$paths.Tools;declaredTools=$paths.DeclaredTools;toolsWasLinked=$paths.ToolsWasLinked;javaHome=$paths.Jdk;androidHome=$paths.Sdk;gradleUserHome=$paths.GradleUserHome;flutterSdk=$paths.FlutterSdk;flutter=$paths.Flutter;artifacts=(Join-Path $workspace '.artifacts/android');linkedDaemons=@(Get-MultitapLinkedGradleDaemon -Tools $paths.Tools | ForEach-Object ProcessId)} | ConvertTo-Json
    exit 0
}
$app = Join-Path $root 'apps/windows_client'
$sdk = $paths.Sdk
$jdk = $paths.Jdk
$private = Join-Path $workspace '.local/android/preview-signing.json'
$gradleText = Get-Content -LiteralPath (Join-Path $app 'android/app/build.gradle.kts') -Raw -Encoding UTF8
$versionMatch = [regex]::Match($gradleText, 'versionName\s*=\s*"([^"]+)"')
if (!$versionMatch.Success) { throw 'Android Gradle versionName is missing.' }
# The checked source owns the default; explicit release versions must still match it.
if (!$Version) { $Version = $versionMatch.Groups[1].Value }
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?$') { throw 'Invalid Android version.' }
if ($versionMatch.Groups[1].Value -ne $Version) { throw 'Requested output version does not match the Android Gradle version.' }
# build.gradle.kts derives the APK versionCode from versionName (preview N -> N,
# 1.0.z -> 100 + z; Get-MultitapAndroidVersionCode); each split APK carries
# 1000 * ABI + code. Installed apps (and their in-app updater) refuse a code
# that is not higher, so the code must exceed every earlier build.
$versionCode = Get-MultitapAndroidVersionCode -Version $Version
if ($null -eq $versionCode) { throw 'Android versions are x.y.z-preview.N (N = 1..999, versionCode N) or 1.0.z (z = 0..899, versionCode 100 + z).' }
$earlierArtifacts = Join-Path $workspace '.artifacts/android'
$earlierCodes = @()
if (Test-Path -LiteralPath $earlierArtifacts) {
    foreach ($earlier in (Get-ChildItem -LiteralPath $earlierArtifacts -Directory)) {
        if ($earlier.Name -eq $Version) { continue }
        $earlierCode = Get-MultitapAndroidVersionCode -Version $earlier.Name
        if ($null -eq $earlierCode) { continue }
        $earlierCodes += [pscustomobject]@{ version = $earlier.Name; code = $earlierCode }
        if ($earlierCode -ge $versionCode) { throw "versionCode $versionCode ($Version) does not exceed the earlier build $($earlier.Name) (code $earlierCode); installed apps would refuse it (not_newer)." }
    }
}
if ($VersionCheckOnly) {
    # The checks above (Gradle versionName, derived code, every earlier build) without a build, key or staging.
    $highest = @($earlierCodes | Sort-Object code | Select-Object -Last 1)
    [ordered]@{version=$Version;gradleVersionName=$versionMatch.Groups[1].Value;versionCode=$versionCode;apkVersionCodes=[ordered]@{'armeabi-v7a'=1000+$versionCode;'arm64-v8a'=2000+$versionCode;'x86_64'=4000+$versionCode};earlierBuilds=$earlierCodes.Count;highestEarlier=$(if($highest.Count){"$($highest[0].version) (code $($highest[0].code))"}else{$null});workspace=$workspace;built=$false} | ConvertTo-Json -Depth 3
    exit 0
}
$requestedKeyAlias = $env:MULTITAP_ANDROID_KEY_ALIAS
$savedSigningEnvironment = @{
    MULTITAP_ANDROID_STOREPASS = $env:MULTITAP_ANDROID_STOREPASS
    MULTITAP_ANDROID_KEYSTORE = $env:MULTITAP_ANDROID_KEYSTORE
    MULTITAP_ANDROID_KEY_ALIAS = $requestedKeyAlias
}
$locationPushed = $false
# Signing secrets must be restored even if key creation, source staging or
# daemon inspection fails before Flutter is invoked.
try {
if (!(Test-Path -LiteralPath "$sdk/platforms/android-36/android.jar")) { throw 'Install the verified Android SDK 36 toolchain first.' }
$signingAlias = if ($requestedKeyAlias) { $requestedKeyAlias } else { 'multitap-preview' }
if ($signingAlias -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') { throw 'Invalid Android signing key alias.' }
if (!(Test-Path -LiteralPath $private)) {
    if (!$CreatePreviewKey) { throw 'Preview signing key is missing. Use -CreatePreviewKey once. Keep .local/android backed up privately to preserve APK upgrades.' }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $private) | Out-Null
    $random = New-Object byte[] 32
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($random)
    $secret = [Convert]::ToBase64String($random)
    $keyPath = Join-Path $workspace '.local/android/multitap-preview.p12'
    $env:MULTITAP_ANDROID_STOREPASS = $secret
    $keyLog = Join-Path $workspace '.artifacts/android-preview-keygen'
    $keyArgs = @('-genkeypair', '-keystore', ('"' + $keyPath + '"'), '-storetype', 'PKCS12',
        '-alias', $signingAlias, '-storepass:env', 'MULTITAP_ANDROID_STOREPASS',
        '-keypass:env', 'MULTITAP_ANDROID_STOREPASS', '-keyalg', 'RSA', '-keysize', '3072',
        '-validity', '10000', '-dname', '"CN=Multitap Android Preview, OU=Development"')
    $keyProcess = Start-Process -FilePath "$jdk/bin/keytool.exe" -ArgumentList $keyArgs -WindowStyle Hidden -Wait -PassThru -RedirectStandardOutput "$keyLog.out.log" -RedirectStandardError "$keyLog.err.log"
    if ($keyProcess.ExitCode -ne 0) { throw 'Preview key generation failed. See the key-generation log.' }
    [IO.File]::WriteAllText($private, (@{path=$keyPath;password=$secret;alias=$signingAlias} | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
}
$config = Get-Content -LiteralPath $private -Raw -Encoding UTF8 | ConvertFrom-Json
# Existing keystores keep their keys and aliases. Set the alias in the private
# signing configuration or override it for this build through the environment.
if (!$requestedKeyAlias -and $config.alias) { $signingAlias = [string]$config.alias }
if ($signingAlias -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') { throw 'Invalid Android signing key alias.' }
$utf8=[Text.UTF8Encoding]::new($false)
# Earlier preview releases stay byte-identical: hash every earlier APK now and
# again after this build; any difference fails the build.
$artifacts = Join-Path $workspace '.artifacts/android'
$previous=@()
if (Test-Path -LiteralPath $artifacts) {
    foreach ($file in (Get-ChildItem -LiteralPath $artifacts -Recurse -File -Filter '*.apk')) {
        if ($file.Directory.Name -eq $Version) { continue }
        $previous+=@{path=$file.FullName.Substring($workspace.Length+1).Replace('\','/');bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
}
$sourceRevision = Get-MultitapAndroidSourceRevision -SourceRoot $root -GitExecutable (Join-Path $paths.Tools 'git/cmd/git.exe')
$sourceCommit = $sourceRevision.commit
$sourceDirty = $sourceRevision.uncommittedChanges
# Each Android release gets its own source/tooling directory. Flutter tests or
# Windows builds in the shared app can otherwise regenerate the Android plugin
# registrant with dev-only integration_test while release Gradle excludes it.
# Preserve the exact pubspec; Flutter's normal release-mode filtering runs here
# without another Flutter invocation replacing this stage's generated files.
$stage=Join-Path $workspace ('.artifacts/android-build/'+$Version+'-'+[guid]::NewGuid().ToString('N').Substring(0,12))
New-Item -ItemType Directory -Path $stage | Out-Null
# The Gradle wrapper Flutter generates is tracked in git. A checkout of an
# older commit, made before it was tracked, takes exactly these files from the
# tool root's checkout; each pin names where it came from.
$wrapper=@('android/gradlew','android/gradlew.bat','android/gradle/wrapper/gradle-wrapper.jar')
$workspaceApp=Join-Path $workspace 'apps/windows_client'
$inputs=@('pubspec.yaml','pubspec.lock','analysis_options.yaml','android/settings.gradle.kts','android/build.gradle.kts','android/gradle.properties')+$wrapper
foreach($directory in @('lib','assets','android/app','android/gradle')){
    $boundary=[IO.Path]::GetFullPath((Join-Path $app $directory)).TrimEnd('\')+'\'
    foreach($file in (Get-ChildItem -LiteralPath (Join-Path $app $directory) -Recurse -File)){
        if(($file.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw 'Android source must not contain linked files'}
        $relative=$file.FullName.Substring($app.Length+1).Replace('\','/')
        if($relative-match'(^|/)(build|\.gradle|\.cxx)/|/GeneratedPluginRegistrant\.java$|\.iml$'){continue}
        if(!$file.FullName.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Android source escaped the expected directory'}
        $inputs+=$relative
    }
}
$pins=@()
$pinSources=@{}
foreach($relative in ($inputs | Sort-Object -Unique)){
    if($relative-match'(^|/)(settings\.json|login\.json|host\.dat|vault\.key|credentials\.dpapi)$|\.(p12|jks|keystore|dpapi|pdb)$'){throw 'Private Android input refused'}
    $source=Join-Path $app $relative
    $from='source'
    if(!(Test-Path -LiteralPath $source) -and $wrapper -contains $relative -and $workspaceApp -ne $app){
        $source=Join-Path $workspaceApp $relative
        $from='toolRootGradleWrapper'
    }
    $pinSources[$relative]=$source
    $destination=Join-Path $stage $relative
    $item=Get-Item -LiteralPath $source
    if(($item.Attributes-band[IO.FileAttributes]::ReparsePoint)-ne0){throw 'Linked Android input refused'}
    $sha=(Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination
    if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()-cne$sha){throw 'Android staged input differs'}
    $pins+=@{path=$relative;bytes=$item.Length;sha256=$sha;from=$from}
}
foreach($pin in $pins){
    if((Get-FileHash -LiteralPath $pinSources[$pin.path] -Algorithm SHA256).Hash.ToLowerInvariant()-cne$pin.sha256){throw 'Android source changed while snapshotting; retry with a consistent source tree'}
}
$properties='sdk.dir='+$sdk.Replace('\','\\').Replace(':','\:')+"`nflutter.sdk="+$paths.FlutterSdk.Replace('\','\\').Replace(':','\:')+"`n"
[IO.File]::WriteAllText((Join-Path $stage 'android/local.properties'),$properties,$utf8)
[IO.File]::WriteAllText((Join-Path $stage 'source-inputs.json'),(ConvertTo-Json -InputObject @{version=$Version;sourceRoot='apps/windows_client';sourceCommit=$sourceCommit;sourceUncommittedChanges=$sourceDirty;sourceRevisionState=$sourceRevision.state;pubspecModified=$false;generatedPluginRegistrantCopied=$false;files=$pins} -Depth 5),$utf8)
$env:MULTITAP_ANDROID_KEYSTORE = $config.path
$env:MULTITAP_ANDROID_STOREPASS = $config.password
$env:MULTITAP_ANDROID_KEY_ALIAS = $signingAlias
$env:JAVA_HOME = $jdk
$env:ANDROID_HOME = $sdk
$env:ANDROID_SDK_ROOT = $sdk
$env:GRADLE_USER_HOME = $paths.GradleUserHome
# A daemon started through a .tools junction keeps those paths and is reused by
# later builds; stop only daemons of this toolchain that were started that way.
foreach ($daemon in @(Get-MultitapLinkedGradleDaemon -Tools $paths.Tools)) {
    Write-Output "Stopping Gradle/Kotlin daemon $($daemon.ProcessId) (started through a linked .tools path)."
    Stop-Process -Id $daemon.ProcessId -Force
}
    Push-Location $stage
    $locationPushed = $true
    & $paths.Flutter build apk --release --target lib/android_main.dart --target-platform android-arm64,android-arm,android-x64 --split-per-abi
    if ($LASTEXITCODE -ne 0) { throw 'Android APK build failed.' }
    $out = Join-Path $workspace ".artifacts/android/$Version"
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $built=@()
    $signer=''
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    foreach ($abi in @('arm64-v8a','armeabi-v7a','x86_64')) {
        $source = Join-Path $stage "build/app/outputs/flutter-apk/app-$abi-release.apk"
        if (!(Test-Path -LiteralPath $source)) { throw "Missing APK: $abi" }
        $target = Join-Path $out "Multitap-$Version-android-$abi.apk"
        if (Test-Path -LiteralPath $target) {
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) {
                throw "Published artifact bytes differ. Bump the Android version: $target"
            }
        } else { Copy-Item -LiteralPath $source -Destination $target }
        $signature=@(& "$sdk/build-tools/36.0.0/apksigner.bat" verify --verbose --print-certs $target)
        if ($LASTEXITCODE -ne 0) { throw "APK signature verification failed: $abi" }
        $signature|Write-Output
        $signerMatch=[regex]::Match(($signature-join"`n"),'Signer #1 certificate SHA-256 digest:\s*([a-fA-F0-9]{64})')
        if(!$signerMatch.Success){throw 'APK signer identity unavailable'}
        $currentSigner=$signerMatch.Groups[1].Value.ToLowerInvariant()
        if($signer-and$signer-cne$currentSigner){throw 'APK signer differs across ABIs'}
        $signer=$currentSigner
        # Upgrade continuity: every earlier preview's recorded signer.
        # (Windows PowerShell ignores -Include with -LiteralPath; filter by name.)
        foreach($record in @(Get-ChildItem -LiteralPath $artifacts -Recurse -File | Where-Object { ($_.Name -eq 'manifest.json' -or $_.Name -like 'build-receipt*.json') -and $_.Directory.Name -ne $Version })){
            $previousSigner=(Get-Content -LiteralPath $record.FullName -Raw -Encoding UTF8|ConvertFrom-Json).signerSha256
            if($previousSigner-and$previousSigner-cne$signer){throw "APK upgrade signer changed: $($record.FullName)"}
        }
        $badging=@(& "$sdk/build-tools/36.0.0/aapt.exe" dump badging $target)
        if ($LASTEXITCODE -ne 0) { throw "APK manifest verification failed: $abi" }
        $badging|Select-Object -First 8|Write-Output
        $abiNumber = @{'armeabi-v7a'=1;'arm64-v8a'=2;'x86_64'=4}[$abi]
        if(($badging-join"`n")-notmatch("name='me\.totb\.multitap_client'.*versionCode='"+($abiNumber*1000+$versionCode)+"'.*versionName='"+[regex]::Escape($Version)+"'")){throw 'APK product/version/versionCode mismatch'}
        if(($badging-join"`n")-notmatch('native-code:\s*'''+[regex]::Escape($abi)+'''')-or($badging-join"`n")-notmatch"sdkVersion:'26'"-or($badging-join"`n")-notmatch"targetSdkVersion:'36'"){throw 'APK ABI or SDK mismatch'}
        $zip=[IO.Compression.ZipFile]::OpenRead($target)
        try{
            foreach($library in @('libflutter.so','libapp.so')){
                if(!$zip.GetEntry("lib/$abi/$library")){throw 'Packaged native media/app library missing'}
            }
            if(@($zip.Entries|Where-Object {$_.FullName-match'(^|/)(vault\.key|credentials\.dpapi|preview-signing\.json)$|\.(p12|jks|keystore|dpapi|pdb)$'}).Count){throw 'Private APK payload refused'}
        }finally{$zip.Dispose()}
        $built+=@{name=(Split-Path -Leaf $target);abi=$abi;bytes=(Get-Item -LiteralPath $target).Length;sha256=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()}
        Get-Item -LiteralPath $target | Select-Object Name,Length
        Get-FileHash -LiteralPath $target -Algorithm SHA256 | Select-Object Algorithm,Hash
    }
    foreach($apk in $previous){
        $current=Join-Path $workspace $apk.path
        if(!(Test-Path -LiteralPath $current) -or (Get-FileHash -LiteralPath $current -Algorithm SHA256).Hash.ToLowerInvariant()-cne$apk.sha256){throw "Earlier preview APK changed: $($apk.path)"}
    }
    $receipt=@{version=$Version;applicationId='me.totb.multitap_client';minSdk=26;targetSdk=36;signerSha256=$signer;files=$built;sourceStage=$stage.Substring($workspace.Length+1).Replace('\','/');sourceCommit=$sourceCommit;sourceUncommittedChanges=$sourceDirty;sourceRevisionState=$sourceRevision.state;sourcePinsSHA256=(Get-FileHash -LiteralPath (Join-Path $stage 'source-inputs.json') -Algorithm SHA256).Hash.ToLowerInvariant();pubspecModified=$false;isolatedGeneratedFiles=$true;preservedEarlierApks=$previous;nativeDeviceTest='not performed by this build';builtAt=[DateTime]::UtcNow.ToString('o')}
    $receiptPath=Join-Path $out 'build-receipt.json'
    if(Test-Path -LiteralPath $receiptPath){$receiptPath=Join-Path $out ('build-receipt-'+[guid]::NewGuid().ToString('N')+'.json')}
    [IO.File]::WriteAllText($receiptPath,(ConvertTo-Json -InputObject $receipt -Depth 6),$utf8)
    exit 0
} finally {
    if ($locationPushed) { Pop-Location }
    foreach ($name in $savedSigningEnvironment.Keys) {
        if ($null -eq $savedSigningEnvironment[$name]) { Remove-Item -LiteralPath ('Env:' + $name) -ErrorAction SilentlyContinue }
        else { Set-Item -LiteralPath ('Env:' + $name) -Value $savedSigningEnvironment[$name] }
    }
}
