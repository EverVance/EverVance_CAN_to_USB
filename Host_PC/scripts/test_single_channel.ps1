param(
    [string]$Endpoint = 'winusb://auto',
    [int]$Channel = 2,
    [switch]$Fd,
    [switch]$EnableTermination,
    [uint32]$NominalBitrate = 500000,
    [uint16]$NominalSamplePermille = 800,
    [uint32]$DataBitrate = 2000000,
    [uint16]$DataSamplePermille = 750,
    [uint32]$FrameId = 0x120,
    [string]$DataHex = '11 22 33 44',
    [int]$ListenMs = 1200
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

function Build-ControlPacket([byte]$channel, [byte]$command, [byte]$sequence, [byte[]]$payload) {
    if ($null -eq $payload) {
        $payload = [byte[]]@()
    }
    $buffer = New-Object byte[] (8 + $payload.Length)
    $buffer[0] = 0xA5
    $buffer[1] = $channel
    $buffer[2] = [byte]$payload.Length
    $buffer[3] = $FlagControl
    $buffer[4] = $command
    $buffer[5] = 0
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
        Payload = $payload
        Id = (Read-U32Le $packet 4)
        IsControl = (($packet[3] -band $FlagControl) -ne 0)
        IsTx = (($packet[3] -band $FlagTx) -ne 0)
        HasError = (($packet[3] -band $FlagError) -ne 0)
        IsFd = (($packet[3] -band $FlagCanFd) -ne 0)
        ErrorCode = (($packet[3] -shr 4) -band 0x0F)
    }
}

function Parse-HexBytes([string]$text) {
    if ([string]::IsNullOrWhiteSpace($text)) {
        return [byte[]]@()
    }

    $parts = $text -split '[\s,]+'
    $values = New-Object System.Collections.Generic.List[byte]
    foreach ($part in $parts) {
        if ([string]::IsNullOrWhiteSpace($part)) {
            continue
        }
        $values.Add([byte]([Convert]::ToInt32($part, 16)))
    }
    return $values.ToArray()
}

$session = New-Object VbaCan.Debug.WinUsbSession
if (-not $session.Open($Endpoint)) {
    throw "WinUSB open failed for $Endpoint"
}

try {
    $sequence = 1
    for ($ch = 0; $ch -lt 4; $ch++) {
        $isActiveChannel = ($ch -eq $Channel)
        $isFdChannel = $Fd.IsPresent -and $isActiveChannel
        $termEnabled = $EnableTermination.IsPresent -and $isActiveChannel
        $channelDataBitrate = if ($isFdChannel) { $DataBitrate } else { 0 }
        $channelDataSample = if ($isFdChannel) { $DataSamplePermille } else { 0 }
        $payload = New-ChannelConfigPayload -isFd $isFdChannel `
            -enabled $isActiveChannel `
            -terminationEnabled $termEnabled `
            -nominalBitrate $NominalBitrate `
            -nominalSamplePermille $NominalSamplePermille `
            -dataBitrate $channelDataBitrate `
            -dataSamplePermille $channelDataSample
        [void]$session.Send((Build-ControlPacket -channel ([byte]$ch) -command $CmdSetChannelConfig -sequence ([byte]$sequence) -payload $payload))
        $sequence++
    }

    Start-Sleep -Milliseconds 200
    [void]$session.Send((Build-ControlPacket -channel ([byte]$Channel) -command $CmdGetRuntimeStatus -sequence ([byte]$sequence) -payload $null))
    $sequence++

    $data = Parse-HexBytes $DataHex
    $flags = if ($Fd.IsPresent) { [byte]$FlagCanFd } else { [byte]0 }
    [void]$session.Send((Build-DataPacket -channel ([byte]$Channel) -id $FrameId -flags $flags -payload $data))

    $deadline = [DateTime]::UtcNow.AddMilliseconds($ListenMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $packet = $session.TryReceive()
        if ($null -eq $packet) {
            Start-Sleep -Milliseconds 5
            continue
        }

        $parsed = Parse-Packet $packet
        if ($null -eq $parsed) {
            continue
        }

        if ($parsed.IsControl) {
            if ($parsed.Command -eq $CmdGetRuntimeStatus -and $parsed.Payload.Length -ge 20) {
                Write-Output ("CTRL Runtime ch={0} flags=0x{1:X2} tx={2} rx={3} err=0x{4:X8} hostQ={5} usbQ={6}" -f `
                    $parsed.Channel,
                    $parsed.Payload[1],
                    (Read-U32Le $parsed.Payload 4),
                    (Read-U32Le $parsed.Payload 8),
                    (Read-U32Le $parsed.Payload 12),
                    $parsed.Payload[16],
                    $parsed.Payload[17])
            }
            else {
                Write-Output ("CTRL ch={0} cmd=0x{1:X2} status=0x{2:X2}" -f $parsed.Channel, $parsed.Command, $parsed.Status)
            }
            continue
        }

        Write-Output ("DATA ch={0} tx={1} err={2} fd={3} id=0x{4:X3} dlc={5} code=0x{6:X1}" -f `
            $parsed.Channel,
            ([int]$parsed.IsTx),
            ([int]$parsed.HasError),
            ([int]$parsed.IsFd),
            $parsed.Id,
            $parsed.Payload.Length,
            $parsed.ErrorCode)
    }
}
finally {
    $session.Close()
}
