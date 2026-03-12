param(
    [ValidateSet("usb", "uart")]
    [string]$RomTransport = "uart",
    [string]$RomDevice = "COM6,115200",
    [string]$FlashloaderTransport = "",
    [string]$FlashloaderDevice = "",
    [string]$BootloaderElf = "",
    [string]$BootloaderBin = "",
    [string]$FlashloaderBin = "",
    [switch]$NoReset
)

$ErrorActionPreference = "Stop"

$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\\..")).Path
$sdkRoot = "F:\\MCUXpressoIDE_25.6.136\\SDK_2_16_100_MIMXRT1062xxxxB"
$objcopy = "F:\\MCUXpressoIDE_25.6.136\\ide\\tools\\bin\\arm-none-eabi-objcopy.exe"
$sdphost = Join-Path $sdkRoot "middleware\\mcu_bootloader\\bin\\Tools\\sdphost\\win\\sdphost.exe"
$blhost = "F:\\LinkServer_25.6.131\\dist\\blhost.exe"

$flashloaderLoadAddress = "0x20002000"
$flashloaderEntryAddress = "0x20002401"

if ([string]::IsNullOrWhiteSpace($BootloaderElf)) {
    $BootloaderElf = Join-Path $workspaceRoot "out\\artifacts\\Bootloader\\bootloader.elf"
}

if ([string]::IsNullOrWhiteSpace($BootloaderBin)) {
    $BootloaderBin = Join-Path (Split-Path -Parent $BootloaderElf) "bootloader.bin"
}

if ([string]::IsNullOrWhiteSpace($FlashloaderBin)) {
    $FlashloaderBin = "F:\\AS\\NXP_Workspace\\LINCAN\\Bootloader\\ReleaseRam\\evkmimxrt1060_flashloader_ram.bin"
}

foreach ($path in @($objcopy, $sdphost, $blhost, $FlashloaderBin, $BootloaderElf)) {
    if (-not (Test-Path $path)) {
        throw "Required file not found: $path"
    }
}

function Invoke-CheckedTool {
    param(
        [string]$Exe,
        [string[]]$Args
    )

    Write-Host ">> $Exe $($Args -join ' ')"
    & $Exe @Args
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE: $Exe $($Args -join ' ')"
    }
}

function Get-TransportArgs {
    param(
        [ValidateSet("usb", "uart")]
        [string]$Transport,
        [string]$Device
    )

    if ($Transport -eq "usb") {
        return @("-u", $Device)
    }

    return @("-p", $Device)
}

function Test-BlhostConnection {
    param(
        [ValidateSet("usb", "uart")]
        [string]$Transport,
        [string]$Device
    )

    $args = Get-TransportArgs -Transport $Transport -Device $Device
    & $blhost @args "get-property" "1" *> $null
    return ($LASTEXITCODE -eq 0)
}

function Wait-ForFlashloader {
    param(
        [int]$TimeoutSeconds = 12
    )

    $candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($FlashloaderTransport) -and -not [string]::IsNullOrWhiteSpace($FlashloaderDevice)) {
        $candidates += [pscustomobject]@{ Transport = $FlashloaderTransport; Device = $FlashloaderDevice }
    }

    $candidates += @(
        [pscustomobject]@{ Transport = "usb"; Device = "0x1fc9:0x0021" },
        [pscustomobject]@{ Transport = "usb"; Device = "0x1fc9:0x0135" },
        [pscustomobject]@{ Transport = "uart"; Device = "COM6,115200" },
        [pscustomobject]@{ Transport = "uart"; Device = "COM6,57600" }
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        foreach ($candidate in $candidates) {
            Write-Host "Trying flashloader on $($candidate.Transport):$($candidate.Device)"
            if (Test-BlhostConnection -Transport $candidate.Transport -Device $candidate.Device) {
                return $candidate
            }
        }

        Start-Sleep -Milliseconds 700
    }

    throw "Flashloader did not appear on any tested USB/UART transport."
}

Invoke-CheckedTool -Exe $objcopy -Args @("-O", "binary", $BootloaderElf, $BootloaderBin)

$bootloaderSize = [int64](Get-Item $BootloaderBin).Length
$eraseSize = [int64]([Math]::Ceiling($bootloaderSize / 4096.0) * 4096)
$eraseSizeHex = "0x{0:X}" -f $eraseSize
$bootloaderSizeHex = "0x{0:X}" -f $bootloaderSize
$readbackFile = Join-Path $PSScriptRoot "bootloader_readback.bin"

$romArgs = Get-TransportArgs -Transport $RomTransport -Device $RomDevice

Write-Host "Bootloader size: $bootloaderSize bytes ($bootloaderSizeHex)"
Write-Host "Erase size:      $eraseSize bytes ($eraseSizeHex)"
Write-Host "Flashloader bin: $FlashloaderBin"

Invoke-CheckedTool -Exe $sdphost -Args ($romArgs + @("error-status"))
Invoke-CheckedTool -Exe $sdphost -Args ($romArgs + @("write-file", $flashloaderLoadAddress, $FlashloaderBin))
Invoke-CheckedTool -Exe $sdphost -Args ($romArgs + @("jump-address", $flashloaderEntryAddress))

$flashloaderLink = Wait-ForFlashloader
$flashloaderArgs = Get-TransportArgs -Transport $flashloaderLink.Transport -Device $flashloaderLink.Device

Write-Host "Flashloader connected via $($flashloaderLink.Transport):$($flashloaderLink.Device)"

Invoke-CheckedTool -Exe $blhost -Args ($flashloaderArgs + @("get-property", "1"))
Invoke-CheckedTool -Exe $blhost -Args ($flashloaderArgs + @("fill-memory", "0x2000", "4", "0xc0000006"))
Invoke-CheckedTool -Exe $blhost -Args ($flashloaderArgs + @("configure-memory", "0x9", "0x2000"))
Invoke-CheckedTool -Exe $blhost -Args ($flashloaderArgs + @("flash-erase-region", "0x60000000", $eraseSizeHex))
Invoke-CheckedTool -Exe $blhost -Args ($flashloaderArgs + @("write-memory", "0x60000000", $BootloaderBin))
Invoke-CheckedTool -Exe $blhost -Args ($flashloaderArgs + @("read-memory", "0x60000000", $bootloaderSizeHex, $readbackFile))

$srcHash = (Get-FileHash $BootloaderBin -Algorithm SHA256).Hash
$dstHash = (Get-FileHash $readbackFile -Algorithm SHA256).Hash

Write-Host "Source SHA256:   $srcHash"
Write-Host "Readback SHA256: $dstHash"

if ($srcHash -ne $dstHash) {
    throw "Readback hash mismatch."
}

if (-not $NoReset) {
    Invoke-CheckedTool -Exe $blhost -Args ($flashloaderArgs + @("reset"))
}

Write-Host "Bootloader programmed and verified successfully."
