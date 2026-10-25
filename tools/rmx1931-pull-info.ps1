# Read-only hardware discovery & partition backup script for Realme X2 Pro (RMX1931 / samurai)
# Run while the phone is booted into TWRP or OrangeFox custom recovery with USB connected.
# Nothing on the phone is modified or written to.

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$SdkAdb = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$Adb = if (Test-Path $SdkAdb) { $SdkAdb } elseif (Test-Path "C:\adb\adb.exe") { "C:\adb\adb.exe" } else { "adb" }

New-Item -ItemType Directory -Force -Path "$Root\dtb" | Out-Null
New-Item -ItemType Directory -Force -Path "$Root\backup" | Out-Null

Write-Host "[1/4] Checking ADB connection..." -ForegroundColor Cyan
$state = (& $Adb get-state 2>&1) | Out-String
$state = $state.Trim()
if ($state -ne "recovery" -and $state -ne "device") {
    throw "Phone not detected in ADB recovery/device mode (got: '$state'). Boot into TWRP/OrangeFox and connect USB."
}
Write-Host "      Connected in '$state' mode." -ForegroundColor Green

Write-Host "[2/4] Pulling live Flattened Device Tree (/sys/firmware/fdt)..." -ForegroundColor Cyan
& $Adb pull /sys/firmware/fdt "$Root\dtb\live.dtb"

Write-Host "[3/4] Capturing /proc/iomem, /proc/cmdline, /proc/partitions, and block layout..." -ForegroundColor Cyan
$infoCmd = @"
echo '=== CMDLINE ==='
cat /proc/cmdline
echo ''
echo '=== IOMEM ==='
cat /proc/iomem
echo ''
echo '=== PARTITIONS ==='
cat /proc/partitions
echo ''
echo '=== BY-NAME ==='
ls -la /dev/block/bootdevice/by-name 2>/dev/null || ls -la /dev/block/by-name 2>/dev/null
echo ''
echo '=== DISPLAY / TOUCH SYSFS ==='
cat /sys/class/graphics/fb0/modes 2>/dev/null || true
cat /sys/class/graphics/fb0/virtual_size 2>/dev/null || true
cat /sys/class/graphics/fb0/stride 2>/dev/null || true
ls -la /proc/touchpanel 2>/dev/null || true
cat /proc/touchpanel/ vendor_id 2>/dev/null || true
grep -rn . /sys/bus/i2c/devices/*/name /sys/bus/spi/devices/*/modalias 2>/dev/null || true
"@
($infoCmd -replace "`r`n", "`n") | & $Adb shell | Set-Content -Encoding UTF8 "$Root\dtb\proc_info.txt"

Write-Host "[4/4] Backing up critical per-device & boot partitions to backup/ (gitignored)..." -ForegroundColor Cyan
$parts = @("boot", "dtbo", "vbmeta", "recovery", "persist", "modemst1", "modemst2", "fsg", "fsc")
foreach ($p in $parts) {
    $dest = "$Root\backup\$p.img"
    if (Test-Path $dest) {
        Write-Host "      Skipping $p.img (already backed up)" -ForegroundColor Yellow
        continue
    }
    Write-Host "      Pulling $p -> backup\$p.img ..."
    & $Adb pull "/dev/block/bootdevice/by-name/$p" $dest
}

Write-Host "`nDone! All files saved under dtb/ and backup/ (excluded from git)." -ForegroundColor Green
