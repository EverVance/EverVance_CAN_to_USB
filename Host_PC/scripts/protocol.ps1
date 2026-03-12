# 协议定义：
# Byte0: SYNC(0xA5)
# Byte1: Channel
# Byte2: DLC
# Byte3: Flags
# Byte4..7: ID (little endian)
# Byte8.. : Data[0..DLC-1]

$Script:CAN_SYNC = 0xA5
$Script:CAN_MAX_DLC = 64

function New-CanPacket {
    param(
        [Parameter(Mandatory=$true)][byte]$Channel,
        [Parameter(Mandatory=$true)][uint32]$Id,
        [Parameter(Mandatory=$true)][byte]$Flags,
        [Parameter(Mandatory=$true)][byte[]]$Data
    )

    if ($Data.Length -gt $Script:CAN_MAX_DLC) {
        throw "DLC 超过 64"
    }

    $len = 8 + $Data.Length
    $buf = New-Object byte[] $len
    $buf[0] = [byte]$Script:CAN_SYNC
    $buf[1] = $Channel
    $buf[2] = [byte]$Data.Length
    $buf[3] = $Flags
    $buf[4] = [byte]($Id -band 0xFF)
    $buf[5] = [byte](($Id -shr 8) -band 0xFF)
    $buf[6] = [byte](($Id -shr 16) -band 0xFF)
    $buf[7] = [byte](($Id -shr 24) -band 0xFF)
    if ($Data.Length -gt 0) {
        [Array]::Copy($Data, 0, $buf, 8, $Data.Length)
    }
    return $buf
}

function Parse-CanPacket {
    param(
        [Parameter(Mandatory=$true)][byte[]]$Packet
    )

    if ($Packet.Length -lt 8) { return $null }
    if ($Packet[0] -ne $Script:CAN_SYNC) { return $null }

    $dlc = [int]$Packet[2]
    if ($dlc -gt $Script:CAN_MAX_DLC) { return $null }
    if ($Packet.Length -lt (8 + $dlc)) { return $null }

    $id = [uint32]($Packet[4] -bor ($Packet[5] -shl 8) -bor ($Packet[6] -shl 16) -bor ($Packet[7] -shl 24))
    $data = New-Object byte[] $dlc
    if ($dlc -gt 0) {
        [Array]::Copy($Packet, 8, $data, 0, $dlc)
    }

    return [PSCustomObject]@{
        Channel = [int]$Packet[1]
        Dlc     = $dlc
        Flags   = [int]$Packet[3]
        Id      = $id
        Data    = $data
    }
}

function Format-CanPacket {
    param([Parameter(Mandatory=$true)]$Obj)
    $dataHex = ($Obj.Data | ForEach-Object { '{0:X2}' -f $_ }) -join ' '
    return ('CH={0} ID=0x{1:X3} DLC={2} FLAGS=0x{3:X2} DATA=[{4}]' -f $Obj.Channel, $Obj.Id, $Obj.Dlc, $Obj.Flags, $dataHex)
}
