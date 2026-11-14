# Product-specific admission of the third-party installer packages in a Windows
# installer payload (paths as its receipts list them: bin/...). Exact upstream
# hashes preserve the signed MSIs. tools/build-windows-installer.ps1 applies it
# before the client installer is made, tools/publish-windows-release.ps1 before
# a release is published; tools/test-windows-publisher-msi.ps1 is its fixture.
function Assert-BundledWindowsMsiManifest([string]$ForProduct,[object[]]$Files,$WinFspPin,$UsbDkPin){
  $packages=@($Files|Where-Object{[IO.Path]::GetExtension($_.path) -ieq '.msi'})
  if($packages.Count -gt 1){throw 'Unexpected extra installer package.'}
  if($ForProduct -eq 'host'){
    foreach($msi in $packages){
      if($msi.path -cne ('bin/winfsp/'+$WinFspPin.msi.name) -or $msi.sha256 -ine $WinFspPin.msi.sha256 -or $msi.size -ne $WinFspPin.msi.size){throw 'Unexpected or modified WinFsp installer.'}
    }
    $companions=@($Files|Where-Object{$_.path -match '(^|[/\\])winfsp[/\\]'})
    foreach($other in $companions){if($other.path -cne ('bin/winfsp/'+$WinFspPin.msi.name) -and $other.path -cne 'bin/winfsp/upstream.json'){throw 'Unexpected file beside the WinFsp installer.'}}
    if(($packages.Count -gt 0 -or $companions.Count -gt 0) -and ($packages.Count -ne 1 -or @($Files|Where-Object{$_.path -ceq 'bin/COPYING.winfsp.txt'}).Count -ne 1)){throw 'The WinFsp installer must be accompanied by its licence (COPYING.winfsp.txt) and be unique.'}
  }elseif($ForProduct -eq 'client'){
    # Every client carries exactly the unmodified UsbDk MSI in bin/usbdk
    # (windows/CMakeLists.txt) with its licence: the USB panel's driver
    # installation runs that file (usb_capture.cpp InstallUsbDk) and cannot
    # work without it.
    $usbDk='bin/usbdk/'+$UsbDkPin.file.name
    foreach($other in @($Files|Where-Object{$_.path -match '(^|[/\\])(winfsp|usbdk)[/\\]'})){
      if($other.path -cne $usbDk){throw 'Unexpected bundled client installer file.'}
    }
    if($packages.Count -ne 1 -or $packages[0].path -cne $usbDk -or $packages[0].sha256 -ine $UsbDkPin.file.sha256 -or $packages[0].size -ne $UsbDkPin.file.size){throw 'usbdk_installer_missing: a client release must carry the pinned UsbDk installer.'}
    if(@($Files|Where-Object{$_.path -ceq 'bin/data/flutter_assets/assets/licenses/UsbDk-LICENSE.txt'}).Count -ne 1){throw 'usbdk_installer_missing: the UsbDk installer must be accompanied by its licence (UsbDk-LICENSE.txt).'}
  }else{throw 'Unexpected Windows installer product.'}
}
