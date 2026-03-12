$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\protocol.ps1"
. "$PSScriptRoot\transports\mock_transport.ps1"
. "$PSScriptRoot\transports\serial_transport.ps1"
. "$PSScriptRoot\transports\winusb_transport.ps1"

$logDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'logs'
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
$logFile = Join-Path $logDir ("host_{0}.log" -f (Get-Date -Format 'yyyyMMdd_HHmmss'))

function Write-Log {
    param([string]$Text)
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $Text
    $line | Tee-Object -FilePath $logFile -Append
}

function Show-Help {
    Write-Host '可用命令:'
    Write-Host '  help'
    Write-Host '  send <ch> <id_hex> <data_hex...>'
    Write-Host '  quit'
    Write-Host '示例:'
    Write-Host '  send 0 123 11 22 33 44'
}

function Parse-SendCmd {
    param([string[]]$parts)
    if ($parts.Length -lt 3) { return $null }

    $ch = [byte][int]$parts[1]
    $id = [uint32]([Convert]::ToInt32($parts[2], 16))
    $dataList = New-Object System.Collections.Generic.List[byte]
    for ($i = 3; $i -lt $parts.Length; $i++) {
        $b = [byte]([Convert]::ToInt32($parts[$i], 16))
        $dataList.Add($b)
    }

    return [PSCustomObject]@{
        Channel = $ch
        Id = $id
        Data = $dataList.ToArray()
    }
}

Write-Host '=== VBA_CAN Host ==='
Write-Host '选择模式: mock / serial / winusb'
$mode = (Read-Host 'mode').Trim().ToLowerInvariant()

$transport = $null
switch ($mode) {
    'mock' {
        $transport = New-MockTransport
    }
    'serial' {
        $port = Read-Host '串口号(例如 COM5)'
        $baudInput = Read-Host '波特率(默认115200)'
        $baud = 115200
        if (-not [string]::IsNullOrWhiteSpace($baudInput)) {
            $baud = [int]$baudInput
        }
        $transport = New-SerialTransport -PortName $port -BaudRate $baud
    }
    'winusb' {
        $endpoint = Read-Host 'WinUSB 端点(默认 winusb://auto)'
        if ([string]::IsNullOrWhiteSpace($endpoint)) {
            $endpoint = 'winusb://auto'
        }
        $transport = New-WinUsbTransport -Endpoint $endpoint
    }
    default {
        throw "未知模式: $mode"
    }
}

if (-not (& $transport.Open)) {
    throw '传输层打开失败'
}

Write-Log ("传输层打开成功: {0}" -f $transport.Name)
Show-Help

try {
    while ($true) {
        $raw = Read-Host 'host>'
        if ([string]::IsNullOrWhiteSpace($raw)) {
            $pkt = & $transport.TryReceive
            if ($pkt -ne $null) {
                $obj = Parse-CanPacket -Packet $pkt
                if ($obj -ne $null) {
                    Write-Log ("RX " + (Format-CanPacket -Obj $obj))
                }
            }
            continue
        }

        $parts = $raw.Trim().Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)
        $cmd = $parts[0].ToLowerInvariant()

        switch ($cmd) {
            'help' {
                Show-Help
            }
            'quit' {
                break
            }
            'send' {
                $parsed = Parse-SendCmd -parts $parts
                if ($parsed -eq $null) {
                    Write-Log 'send 参数错误'
                    continue
                }

                $pkt = New-CanPacket -Channel $parsed.Channel -Id $parsed.Id -Flags 0 -Data $parsed.Data
                if (& $transport.Send $pkt) {
                    Write-Log ("TX CH={0} ID=0x{1:X3} LEN={2}" -f $parsed.Channel, $parsed.Id, $parsed.Data.Length)
                } else {
                    Write-Log 'TX 失败'
                }

                $rx = & $transport.TryReceive
                if ($rx -ne $null) {
                    $obj = Parse-CanPacket -Packet $rx
                    if ($obj -ne $null) {
                        Write-Log ("RX " + (Format-CanPacket -Obj $obj))
                    }
                }
            }
            default {
                Write-Log ("未知命令: {0}" -f $cmd)
            }
        }
    }
}
finally {
    & $transport.Close | Out-Null
    Write-Log '已关闭'
}
