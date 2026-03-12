param(
    [string]$Endpoint = 'winusb://auto',
    [int]$DurationMs = 4000,
    [int]$FramesPerBurst = 16,
    [int]$DrainMs = 2000
)

$ErrorActionPreference = 'Stop'
Set-ExecutionPolicy -Scope Process Bypass

. "$PSScriptRoot\transports\winusb_transport.ps1"

$ProtocolVersion = 0x01
$FlagCanFd = 0x01
$FlagTx = 0x02
$FlagError = 0x04
$FlagControl = 0x08

$CmdSetChannelConfig = 0x01
$CmdGetChannelCapabilities = 0x04
$CmdGetRuntimeStatus = 0x05

function Write-U16Le([byte[]]$buffer, [int]$offset, [UInt16]$value) {
    $buffer[$offset + 0] = [byte]($value -band 0xFF)
    $buffer[$offset + 1] = [byte](($value -shr 8) -band 0xFF)
}

function Write-U32Le([byte[]]$buffer, [int]$offset, [UInt32]$value) {
    $buffer[$offset + 0] = [byte]($value -band 0xFF)
    $buffer[$offset + 1] = [byte](($value -shr 8) -band 0xFF)
    $buffer[$offset + 2] = [byte](($value -shr 16) -band 0xFF)
    $buffer[$offset + 3] = [byte](($value -shr 24) -band 0xFF)
}

function Read-U32Le([byte[]]$buffer, [int]$offset) {
    return ([UInt32]$buffer[$offset + 0]) -bor (([UInt32]$buffer[$offset + 1]) -shl 8) -bor (([UInt32]$buffer[$offset + 2]) -shl 16) -bor (([UInt32]$buffer[$offset + 3]) -shl 24)
}

function Copy-Bytes([byte[]]$source, [int]$offset, [int]$length) {
    $buffer = New-Object byte[] $length
    if ($length -gt 0) {
        [Array]::Copy($source, $offset, $buffer, 0, $length)
    }
    return $buffer
}

function Build-ControlPacket([byte]$channel, [byte]$command, [byte]$status, [byte]$sequence, [byte[]]$payload) {
    if ($null -eq $payload) {
        $payload = [byte[]]@()
    }
    $buffer = New-Object byte[] (8 + $payload.Length)
    $buffer[0] = 0xA5
    $buffer[1] = $channel
    $buffer[2] = [byte]$payload.Length
    $buffer[3] = $FlagControl
    $buffer[4] = $command
    $buffer[5] = $status
    $buffer[6] = $sequence
    $buffer[7] = $ProtocolVersion
    if ($payload.Length -gt 0) {
        [Array]::Copy($payload, 0, $buffer, 8, $payload.Length)
    }
    return $buffer
}

function Build-DataPacket([byte]$channel, [UInt32]$id, [byte]$flags, [byte[]]$payload) {
    if ($null -eq $payload) {
        $payload = [byte[]]@()
    }
    $buffer = New-Object byte[] (8 + $payload.Length)
    $buffer[0] = 0xA5
    $buffer[1] = $channel
    $buffer[2] = [byte]$payload.Length
    $buffer[3] = $flags
    Write-U32Le $buffer 4 $id
    if ($payload.Length -gt 0) {
        [Array]::Copy($payload, 0, $buffer, 8, $payload.Length)
    }
    return $buffer
}

function New-ChannelConfigPayload([bool]$isFd, [bool]$enabled, [bool]$terminationEnabled, [UInt32]$nominalBitrate, [UInt16]$nominalSamplePermille, [UInt32]$dataBitrate, [UInt16]$dataSamplePermille) {
    $payload = New-Object byte[] 16
    $payload[0] = $ProtocolVersion
    $payload[1] = [byte]([int]$isFd)
    $payload[2] = [byte]([int]$enabled)
    $payload[3] = [byte]([int]$terminationEnabled)
    Write-U32Le $payload 4 $nominalBitrate
    Write-U16Le $payload 8 $nominalSamplePermille
    Write-U32Le $payload 10 $dataBitrate
    Write-U16Le $payload 14 $dataSamplePermille
    return $payload
}

function Parse-Packet([byte[]]$packet) {
    if ($null -eq $packet -or $packet.Length -lt 8 -or $packet[0] -ne 0xA5) {
        return $null
    }
    $dlc = [int]$packet[2]
    if ($dlc -gt 64 -or $packet.Length -lt (8 + $dlc)) {
        return $null
    }
    $payload = Copy-Bytes -source $packet -offset 8 -length $dlc
    return [PSCustomObject]@{
        Channel = [int]$packet[1]
        Flags = [int]$packet[3]
        Command = [int]$packet[4]
        Status = [int]$packet[5]
        Sequence = [int]$packet[6]
        Version = [int]$packet[7]
        Payload = $payload
        Id = (Read-U32Le $packet 4)
        IsControl = (($packet[3] -band $FlagControl) -ne 0)
        IsTx = (($packet[3] -band $FlagTx) -ne 0)
        HasError = (($packet[3] -band $FlagError) -ne 0)
        IsFd = (($packet[3] -band $FlagCanFd) -ne 0)
        ErrorCode = (($packet[3] -shr 4) -band 0x0F)
    }
}

function Receive-UntilIdle([VbaCan.Debug.WinUsbSession]$session, [int]$timeoutMs, [hashtable]$summary, [hashtable]$runtimeByChannel, [hashtable]$capsByChannel) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $packet = $session.TryReceive()
        if ($null -eq $packet) {
            Start-Sleep -Milliseconds 5
            continue
        }
        $parsed = Parse-Packet $packet
        if ($null -eq $parsed) {
            $summary.InvalidRx++
            continue
        }
        if ($parsed.IsControl) {
            $summary.ControlRx++
            if ($parsed.Command -eq $CmdGetRuntimeStatus -and $parsed.Payload.Length -ge 20) {
                $runtimeByChannel[$parsed.Channel] = [PSCustomObject]@{
                    Flags = $parsed.Payload[1]
                    TxCount = Read-U32Le $parsed.Payload 4
                    RxCount = Read-U32Le $parsed.Payload 8
                    LastError = Read-U32Le $parsed.Payload 12
                    HostPending = [int]$parsed.Payload[16]
                    UsbPending = [int]$parsed.Payload[17]
                    HostDrop = [int]$parsed.Payload[18]
                    UsbDrop = [int]$parsed.Payload[19]
                    Status = $parsed.Status
                }
            }
            elseif ($parsed.Command -eq $CmdGetChannelCapabilities -and $parsed.Payload.Length -ge 20) {
                $capsByChannel[$parsed.Channel] = [PSCustomObject]@{
                    Flags = $parsed.Payload[2]
                    DriverType = $parsed.Payload[3]
                    NominalMin = Read-U32Le $parsed.Payload 4
                    NominalMax = Read-U32Le $parsed.Payload 8
                    DataMax = Read-U32Le $parsed.Payload 12
                    Status = $parsed.Status
                }
            }
            continue
        }

        if ($parsed.HasError) {
            $summary.ErrorRx[$parsed.Channel]++
        }
        elseif ($parsed.IsTx) {
            $summary.TxEcho[$parsed.Channel]++
        }
        else {
            $summary.RxData[$parsed.Channel]++
        }
    }
}

$summary = @{
    InvalidRx = 0
    ControlRx = 0
    TxSent = @(0, 0, 0, 0)
    SendFail = @(0, 0, 0, 0)
    TxEcho = @(0, 0, 0, 0)
    ErrorRx = @(0, 0, 0, 0)
    RxData = @(0, 0, 0, 0)
}
$runtimeByChannel = @{}
$runtimeBeforeByChannel = @{}
$capsByChannel = @{}

$session = New-Object VbaCan.Debug.WinUsbSession
if (-not $session.Open($Endpoint)) {
    throw "WinUSB open failed for $Endpoint"
}

try {
    $configs = @(
        @{ Channel = 0; IsFd = $true;  Enabled = $true; Termination = $false; Nominal = 500000; NominalSp = 800; Data = 2000000; DataSp = 750 },
        @{ Channel = 1; IsFd = $true;  Enabled = $true; Termination = $false; Nominal = 500000; NominalSp = 800; Data = 2000000; DataSp = 750 },
        @{ Channel = 2; IsFd = $false; Enabled = $true; Termination = $false; Nominal = 500000; NominalSp = 800; Data = 0;       DataSp = 0   },
        @{ Channel = 3; IsFd = $false; Enabled = $true; Termination = $false; Nominal = 500000; NominalSp = 800; Data = 0;       DataSp = 0   }
    )

    $sequence = 1
    foreach ($cfg in $configs) {
        $payload = New-ChannelConfigPayload -isFd $cfg.IsFd -enabled $cfg.Enabled -terminationEnabled $cfg.Termination -nominalBitrate $cfg.Nominal -nominalSamplePermille $cfg.NominalSp -dataBitrate $cfg.Data -dataSamplePermille $cfg.DataSp
        $packet = Build-ControlPacket -channel ([byte]$cfg.Channel) -command $CmdSetChannelConfig -status 0 -sequence ([byte]$sequence) -payload $payload
        [void]$session.Send($packet)
        $sequence++
    }

    for ($channel = 0; $channel -lt 4; $channel++) {
        [void]$session.Send((Build-ControlPacket -channel ([byte]$channel) -command $CmdGetChannelCapabilities -status 0 -sequence ([byte]$sequence) -payload $null))
        $sequence++
    }
    for ($channel = 0; $channel -lt 4; $channel++) {
        [void]$session.Send((Build-ControlPacket -channel ([byte]$channel) -command $CmdGetRuntimeStatus -status 0 -sequence ([byte]$sequence) -payload $null))
        $sequence++
    }

    Receive-UntilIdle -session $session -timeoutMs 500 -summary $summary -runtimeByChannel $runtimeByChannel -capsByChannel $capsByChannel
    foreach ($key in $runtimeByChannel.Keys) {
        $runtimeBeforeByChannel[$key] = $runtimeByChannel[$key]
    }

    $payloadFd = New-Object byte[] 64
    for ($i = 0; $i -lt $payloadFd.Length; $i++) {
        $payloadFd[$i] = [byte]($i -band 0xFF)
    }
    $payloadClassic = New-Object byte[] 8
    for ($i = 0; $i -lt $payloadClassic.Length; $i++) {
        $payloadClassic[$i] = [byte](0xA0 + $i)
    }

    $channelModes = @(
        @{ Channel = 0; Flags = $FlagCanFd; Payload = $payloadFd; IdBase = 0x180 },
        @{ Channel = 1; Flags = $FlagCanFd; Payload = $payloadFd; IdBase = 0x280 },
        @{ Channel = 2; Flags = 0;          Payload = $payloadClassic; IdBase = 0x380 },
        @{ Channel = 3; Flags = 0;          Payload = $payloadClassic; IdBase = 0x480 }
    )

    $stopAt = [DateTime]::UtcNow.AddMilliseconds($DurationMs)
    while ([DateTime]::UtcNow -lt $stopAt) {
        foreach ($mode in $channelModes) {
            for ($i = 0; $i -lt $FramesPerBurst; $i++) {
                $packet = Build-DataPacket -channel ([byte]$mode.Channel) -id ([uint32]($mode.IdBase + ($summary.TxSent[$mode.Channel] -band 0x7F))) -flags ([byte]$mode.Flags) -payload $mode.Payload
                if ($session.Send($packet)) {
                    $summary.TxSent[$mode.Channel]++
                }
                else {
                    $summary.SendFail[$mode.Channel]++
                }
            }
        }
        Receive-UntilIdle -session $session -timeoutMs 20 -summary $summary -runtimeByChannel $runtimeByChannel -capsByChannel $capsByChannel
    }

    Receive-UntilIdle -session $session -timeoutMs $DrainMs -summary $summary -runtimeByChannel $runtimeByChannel -capsByChannel $capsByChannel

    for ($channel = 0; $channel -lt 4; $channel++) {
        [void]$session.Send((Build-ControlPacket -channel ([byte]$channel) -command $CmdGetRuntimeStatus -status 0 -sequence ([byte]$sequence) -payload $null))
        $sequence++
    }

    Receive-UntilIdle -session $session -timeoutMs 800 -summary $summary -runtimeByChannel $runtimeByChannel -capsByChannel $capsByChannel

    Write-Output ("Stress summary: duration={0}ms burst={1}" -f $DurationMs, $FramesPerBurst)
    for ($channel = 0; $channel -lt 4; $channel++) {
        $caps = $capsByChannel[$channel]
        $runtime = $runtimeByChannel[$channel]
        $capsText = if ($null -ne $caps) {
            "capFlags=0x{0:X2} dataMax={1}" -f $caps.Flags, $caps.DataMax
        }
        else {
            "capFlags=? dataMax=?"
        }
        $runtimeText = if ($null -ne $runtime) {
            $runtimeBefore = $runtimeBeforeByChannel[$channel]
            $txDelta = if ($null -ne $runtimeBefore) { [UInt32]($runtime.TxCount - $runtimeBefore.TxCount) } else { $runtime.TxCount }
            $rxDelta = if ($null -ne $runtimeBefore) { [UInt32]($runtime.RxCount - $runtimeBefore.RxCount) } else { $runtime.RxCount }
            "rtFlags=0x{0:X2} txCnt+={1} rxCnt+={2} err=0x{3:X8} hostQ={4} usbQ={5} hostDrop={6} usbDrop={7}" -f $runtime.Flags, $txDelta, $rxDelta, $runtime.LastError, $runtime.HostPending, $runtime.UsbPending, $runtime.HostDrop, $runtime.UsbDrop
        }
        else {
            "runtime=?"
        }
        Write-Output ("CH{0}: txSent={1} sendFail={2} txEcho={3} errRx={4} rxData={5} | {6} | {7}" -f $channel, $summary.TxSent[$channel], $summary.SendFail[$channel], $summary.TxEcho[$channel], $summary.ErrorRx[$channel], $summary.RxData[$channel], $capsText, $runtimeText)
    }
    Write-Output ("ControlRx={0} InvalidRx={1}" -f $summary.ControlRx, $summary.InvalidRx)
}
finally {
    $session.Close()
}
