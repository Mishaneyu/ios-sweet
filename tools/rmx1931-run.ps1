param(
    [Parameter(Mandatory = $true)]
    [string]$Image,
    [int]$TimeoutSeconds = 60,
    [string]$RescueProbe = ""
)

# Safe temporary boot-partition test runner for Realme X2 Pro (RMX1931 / samurai).
# Because OPPO/Realme ABL disables `fastboot boot`, this script:
#   1. Verifies backup\boot.img exists before touching anything.
#   2. Reboots ADB -> bootloader if currently in Android/TWRP.
#   3. Flashes $Image to `boot` and reboots.
#   4. Waits up to $TimeoutSeconds for the shim/XNU watchdog or PON flag to return to Fastboot.
#   5. Automatically restores backup\boot.img back to `boot`.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BackupBoot = Join-Path $Root "backup\boot.img"
if (-not (Test-Path $BackupBoot)) {
    throw "Refusing to run: $BackupBoot is missing!"
}
if (-not (Test-Path $Image)) {
    throw "Test image not found: $Image"
}

$Adb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$Fb  = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\fastboot.exe"

$fbDevs = (& $Fb devices) | Out-String
if ($fbDevs.Trim().Length -eq 0) {
    Write-Host "[1/4] Rebooting phone into Fastboot via ADB..." -ForegroundColor Cyan
    & $Adb reboot bootloader
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Seconds 1
        $fbDevs = (& $Fb devices) | Out-String
        if ($fbDevs.Trim().Length -gt 0) { break }
    }
}
if ($fbDevs.Trim().Length -eq 0) {
    throw "Phone not detected in Fastboot mode."
}

Write-Host "[2/4] Flashing test image ($Image) to boot..." -ForegroundColor Cyan
& $Fb flash boot $Image
if ($LASTEXITCODE -ne 0) { throw "fastboot flash boot failed" }

Write-Host "[3/4] Rebooting into test image (waiting up to ${TimeoutSeconds}s for return to Fastboot)..." -ForegroundColor Cyan
& $Fb reboot
Start-Sleep -Seconds 5

$returned = $false
for ($t = 0; $t -lt $TimeoutSeconds; $t++) {
    Start-Sleep -Seconds 1
    $fbDevs = (& $Fb devices) | Out-String
    if ($fbDevs.Trim().Length -gt 0) {
        Write-Host "      Returned to Fastboot at +$($t + 5)s." -ForegroundColor Green
        $returned = $true
        break
    }
}

if (-not $returned) {
    Write-Host "WARNING: Did not auto-return within $($TimeoutSeconds + 5)s. Hold Power + Vol Down to enter Fastboot, then run:" -ForegroundColor Yellow
    Write-Host "  `"$Fb`" flash boot `"$BackupBoot`"" -ForegroundColor Yellow
    exit 2
}

if ($RescueProbe -ne "" -and (Test-Path $RescueProbe)) {
    Write-Host "[3b/4] Flashing zero-write rescue probe ($RescueProbe) to capture SWEETMIR XNU console..." -ForegroundColor Cyan
    & $Fb flash boot $RescueProbe
    if ($LASTEXITCODE -eq 0) {
        & $Fb reboot
        Start-Sleep -Seconds 5
        for ($t = 0; $t -lt 45; $t++) {
            Start-Sleep -Seconds 1
            $fbDevs = (& $Fb devices) | Out-String
            if ($fbDevs.Trim().Length -gt 0) {
                Write-Host "      Rescue probe returned to Fastboot at +$($t + 5)s." -ForegroundColor Green
                break
            }
        }
    }
}

Write-Host "[4/4] Restoring original backup\boot.img..." -ForegroundColor Cyan
& $Fb flash boot $BackupBoot
Write-Host "Original boot.img restored!" -ForegroundColor Green
