Add-Type -AssemblyName System

function New-SerialTransport {
    param(
        [Parameter(Mandatory=$true)][string]$PortName,
        [int]$BaudRate = 115200
    )

    $sp = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, 'None', 8, 'One'
    $sp.ReadTimeout = 20
    $sp.WriteTimeout = 200

    $obj = [PSCustomObject]@{
        Name = 'serial'
        Open = {
            param()
            if (-not $sp.IsOpen) { $sp.Open() }
            return $sp.IsOpen
        }
        Close = {
            param()
            if ($sp.IsOpen) { $sp.Close() }
            return $true
        }
        Send = {
            param([byte[]]$data)
            if (-not $sp.IsOpen) { return $false }
            $sp.Write($data, 0, $data.Length)
            return $true
        }
        TryReceive = {
            param()
            if (-not $sp.IsOpen) { return $null }
            $count = $sp.BytesToRead
            if ($count -le 0) { return $null }
            $buf = New-Object byte[] $count
            $read = $sp.Read($buf, 0, $count)
            if ($read -le 0) { return $null }
            if ($read -lt $count) {
                $tmp = New-Object byte[] $read
                [Array]::Copy($buf, 0, $tmp, 0, $read)
                return ,$tmp
            }
            return ,$buf
        }
    }

    return $obj
}
