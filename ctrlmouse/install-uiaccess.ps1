<#
  Installs ctrlmouse so its on-screen keyboard can draw above the Start menu.

  Windows only lets a process into that z-order band if it has uiAccess, and it
  only grants uiAccess when BOTH of these hold:

    * the exe is signed by a certificate that chains to a trusted root, and
    * the exe lives in a protected folder (Program Files or System32).

  So this script creates a self-signed code-signing certificate, trusts it on
  this machine, signs the uiAccess build, and installs it to Program Files.

  READ THIS BEFORE RUNNING: trusting a self-signed certificate means this
  machine will accept ANY binary signed with that key as coming from a trusted
  publisher. The private key stays in this machine's certificate store. That is
  a reasonable trade on your own PC and a bad idea on a shared or work machine.
  Uninstall.ps1 semantics: pass -Remove to undo everything this does.

  Usage (right-click > Run with PowerShell, or from an admin prompt):
      .\install-uiaccess.ps1 [-ExePath .\ctrlmouse-uiaccess.exe]
      .\install-uiaccess.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$ExePath = "$PSScriptRoot\ctrlmouse-uiaccess.exe",
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$Subject   = 'CN=ctrlmouse self-signed'
$InstallTo = Join-Path $env:ProgramFiles 'ctrlmouse'
$RunKey    = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

function Assert-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    if (-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host 'Needs administrator rights - relaunching elevated...' -ForegroundColor Yellow
        $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
        if ($Remove)  { $argList += '-Remove' }
        else          { $argList += @('-ExePath',"`"$ExePath`"") }
        Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
        exit
    }
}

function Find-SignTool {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "$env:ProgramFiles\Windows Kits\10\bin")
    foreach ($r in $roots) {
        if (-not (Test-Path $r)) { continue }
        $hit = Get-ChildItem $r -Directory -ErrorAction SilentlyContinue |
               Sort-Object Name -Descending |
               ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
               Where-Object { Test-Path $_ } |
               Select-Object -First 1
        if ($hit) { return $hit }
    }
    throw 'signtool.exe not found. Install the Windows SDK signing tools.'
}

Assert-Admin

if ($Remove) {
    Write-Host 'Removing ctrlmouse uiAccess install...' -ForegroundColor Cyan
    if (Test-Path $InstallTo) { Remove-Item $InstallTo -Recurse -Force }
    foreach ($store in 'Root','TrustedPublisher','My') {
        Get-ChildItem "Cert:\LocalMachine\$store" -ErrorAction SilentlyContinue |
            Where-Object { $_.Subject -eq $Subject } |
            ForEach-Object {
                Write-Host "  removing certificate from $store"
                Remove-Item $_.PSPath -Force
            }
    }
    $run = Get-ItemProperty $RunKey -Name ctrlmouse -ErrorAction SilentlyContinue
    if ($run) { Remove-ItemProperty $RunKey -Name ctrlmouse; Write-Host '  removed Run entry' }
    Write-Host 'Done. The portable exe is unaffected.' -ForegroundColor Green
    Read-Host 'Press Enter to close'
    exit
}

if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath" }
$signtool = Find-SignTool

# 1. Certificate. Reused if this has been run before, so repeat installs do not
#    pile up trusted roots.
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1
if ($cert) {
    Write-Host "Reusing certificate $($cert.Thumbprint)" -ForegroundColor Cyan
} else {
    Write-Host 'Creating a self-signed code-signing certificate...' -ForegroundColor Cyan
    $cert = New-SelfSignedCertificate -Subject $Subject -Type CodeSigningCert `
                -CertStoreLocation Cert:\LocalMachine\My -NotAfter (Get-Date).AddYears(10)
    # Chaining to a trusted root is what uiAccess actually checks for.
    $tmp = Join-Path $env:TEMP 'ctrlmouse-cert.cer'
    Export-Certificate -Cert $cert -FilePath $tmp | Out-Null
    Import-Certificate -FilePath $tmp -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $tmp -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
    Remove-Item $tmp -Force
    Write-Host "  created $($cert.Thumbprint) and trusted it on this machine"
}

# 2. Install into a protected folder, then sign in place.
Write-Host "Installing to $InstallTo ..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $InstallTo | Out-Null
$target = Join-Path $InstallTo 'ctrlmouse.exe'
Copy-Item $ExePath $target -Force

Write-Host 'Signing...' -ForegroundColor Cyan
& $signtool sign /sha1 $cert.Thumbprint /fd SHA256 $target
if ($LASTEXITCODE -ne 0) { throw 'signtool failed' }
& $signtool verify /pa $target | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'signature did not verify' }

# 3. Point an existing login entry at the installed copy.
$run = Get-ItemProperty $RunKey -Name ctrlmouse -ErrorAction SilentlyContinue
if ($run) {
    Set-ItemProperty $RunKey -Name ctrlmouse -Value "`"$target`" --tray"
    Write-Host 'Updated the existing Run entry to the installed copy.'
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
Write-Host "  Run it from: $target"
Write-Host '  Close any copy still running from the old location first.'
Write-Host '  Settings in %APPDATA%\ctrlmouse are shared, so nothing is lost.'
Write-Host ''
Write-Host 'The keyboard should now sit above the Start menu. If it does not,'
Write-Host 'the uiAccess grant was refused - check the exe really is the'
Write-Host 'uiaccess build and really is under Program Files.'
Read-Host 'Press Enter to close'
