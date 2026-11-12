param([switch]$SelfTest,[string]$Product)
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$projectRoot=Split-Path -Parent $PSScriptRoot
$source=Join-Path $projectRoot 'drivers/virtual_display'
$cmakeSource=Get-Content -LiteralPath (Join-Path $source 'CMakeLists.txt') -Raw -Encoding UTF8
if($cmakeSource -notmatch 'project\(MultitapDisplay VERSION (\d+\.\d+\.\d+)'){throw 'Display helper version is missing.'}
$helperVersion=$Matches[1]
$pin=Get-Content -LiteralPath (Join-Path $source 'upstream.json') -Raw -Encoding UTF8|ConvertFrom-Json
$stage=Join-Path $projectRoot '.artifacts/virtual-display/25.7.23'
$archive=Join-Path $stage 'VirtualDisplayDriver-x86.Driver.Only.zip'
New-Item -ItemType Directory -Force -Path $stage|Out-Null
if(-not(Test-Path -LiteralPath $archive)){Invoke-WebRequest $pin.archiveUrl -OutFile $archive -UseBasicParsing}
if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.archiveSha256){throw 'Virtual display archive hash mismatch.'}
$extract=Join-Path $stage 'package'
if(-not(Test-Path -LiteralPath $extract)){Expand-Archive -LiteralPath $archive -DestinationPath $extract}
$upstream=Join-Path $extract 'VirtualDisplayDriver'
foreach($entry in $pin.files.PSObject.Properties){
  $file=Join-Path $upstream $entry.Name
  if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value){throw "Driver hash mismatch: $($entry.Name)"}
}
foreach($name in 'MttVDD.dll','mttvdd.cat'){
  $sig=Get-AuthenticodeSignature -LiteralPath (Join-Path $upstream $name)
  if($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Thumbprint -ne $pin.signerThumbprint){throw "Driver signature mismatch: $name"}
}
$license=Join-Path $source 'LICENSE.upstream.txt'
if(-not(Test-Path -LiteralPath $license)){Invoke-WebRequest "https://raw.githubusercontent.com/VirtualDrivers/Virtual-Display-Driver/$($pin.sourceCommit)/LICENSE" -OutFile $license -UseBasicParsing}
if((Get-FileHash -LiteralPath $license -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.licenseSha256){throw 'Upstream license hash mismatch.'}
if((Get-FileHash -LiteralPath (Join-Path $source 'vdd_settings.xml') -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.settingsSha256){throw 'Display configuration hash mismatch.'}
$visualStudio=Join-Path $projectRoot '.tools/visualstudio-buildtools'
$cmake=Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$build=Join-Path $projectRoot ('.artifacts/virtual-display/build-'+$helperVersion)
& $cmake -S $source -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$visualStudio"
if($LASTEXITCODE -ne 0){throw 'Display helper configure failed.'}
& $cmake --build $build --config Release --parallel 2
if($LASTEXITCODE -ne 0){throw 'Display helper build failed.'}
if(-not$Product){$Product=Join-Path $projectRoot ('.artifacts/virtual-display/product-'+$helperVersion)}
$product=[IO.Path]::GetFullPath($Product)
$immutableProduct=[IO.Path]::GetFullPath((Join-Path $projectRoot '.artifacts/virtual-display/product'))
if($product.TrimEnd('\') -eq $immutableProduct.TrimEnd('\')){throw 'The released 0.1.3 product is immutable; choose a separate candidate directory.'}
$immutableCountCandidate=[IO.Path]::GetFullPath((Join-Path $projectRoot '.artifacts/virtual-display/product-0.1.4'))
if($product.TrimEnd('\') -eq $immutableCountCandidate.TrimEnd('\')){throw 'The frozen 0.1.4 product is immutable; choose a separate candidate directory.'}
$immutablePrevious=[IO.Path]::GetFullPath((Join-Path $projectRoot '.artifacts/virtual-display/product-0.1.5'))
if($product.TrimEnd('\') -eq $immutablePrevious.TrimEnd('\')){throw 'The released 0.1.5 product is immutable; choose a separate candidate directory.'}
$driver=Join-Path $product 'driver'
New-Item -ItemType Directory -Force -Path $driver|Out-Null
Copy-Item -LiteralPath (Join-Path $build 'Release/MultitapDisplay.exe') -Destination $product -Force
foreach($name in 'MttVDD.inf','MttVDD.dll','mttvdd.cat'){Copy-Item -LiteralPath (Join-Path $upstream $name) -Destination $driver -Force}
Copy-Item -LiteralPath (Join-Path $source 'vdd_settings.xml'),$license,(Join-Path $source 'upstream.json') -Destination $driver -Force
Copy-Item -LiteralPath (Join-Path $source 'README.md') -Destination (Join-Path $product 'README.VirtualDisplay.md') -Force
$crt=Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Redist/MSVC') -Directory|Sort-Object Name -Descending|ForEach-Object{ $candidate=Join-Path $_.FullName 'x64/Microsoft.VC143.CRT';if(Test-Path -LiteralPath $candidate){Get-Item -LiteralPath $candidate}}|Select-Object -First 1
if(-not$crt){throw 'VC runtime unavailable.'}
Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll'|Copy-Item -Destination $product -Force
if($SelfTest){
  $helper=Join-Path $product 'MultitapDisplay.exe'
  foreach($command in 'verify','status','warp-test','plan-test','topology-test','viewport-test','json-test'){
    $output=& $helper $command
    if($LASTEXITCODE -ne 0){throw "Display helper check failed: $command"}
    if($command -eq 'status' -and ($output|ConvertFrom-Json).helperVersion -ne $helperVersion){throw 'The packaged display helper version is stale.'}
    Write-Output $output
  }
  $settings=[xml](Get-Content -LiteralPath (Join-Path $driver 'vdd_settings.xml') -Raw -Encoding UTF8)
  if($settings.vdd_settings.monitors.count -ne '2'){throw 'Dual-monitor configuration missing.'}
}
Write-Output "Display product: $product"
