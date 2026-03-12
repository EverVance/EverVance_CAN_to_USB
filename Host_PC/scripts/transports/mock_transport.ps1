function New-MockTransport {
    $state = [PSCustomObject]@{
        Name = 'mock'
        RxQueue = New-Object System.Collections.Generic.Queue[byte[]]
    }

    $obj = [PSCustomObject]@{
        Name = 'mock'
        Open = {
            param()
            return $true
        }
        Close = {
            param()
            return $true
        }
        Send = {
            param([byte[]]$data)
            # mock 回环：直接把发送包放回接收队列
            $state.RxQueue.Enqueue($data)
            return $true
        }
        TryReceive = {
            param()
            if ($state.RxQueue.Count -gt 0) {
                return ,$state.RxQueue.Dequeue()
            }
            return $null
        }
    }

    return $obj
}
