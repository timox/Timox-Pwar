param(
    [Parameter(Mandatory=$true)][string]$ClientPath,
    [string]$DriverClsid='{A4D39126-78CB-4D89-9E0A-54494D4F5856}',
    [string]$Peer='10.1.0.101'
)
$ErrorActionPreference='Stop'
function Start-PwarClient {
    $info=[System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName=(Resolve-Path -LiteralPath $ClientPath).Path
    $info.UseShellExecute=$false
    $info.CreateNoWindow=$true
    $info.RedirectStandardInput=$true
    $info.RedirectStandardOutput=$true
    $info.RedirectStandardError=$true
    foreach($argument in @('--driver-clsid',$DriverClsid,'--peer',$Peer,'--stdio')) { $info.ArgumentList.Add($argument) }
    $process=[System.Diagnostics.Process]::Start($info)
    return $process
}
function Read-Message($process) {
    $read=$process.StandardOutput.ReadLineAsync()
    if(-not $read.Wait(10000)) { throw 'PWAR control response timeout' }
    if($null -eq $read.Result) { throw 'PWAR closed stdout before the expected response' }
    $value=$read.Result | ConvertFrom-Json
    if($value.version -ne 1) { throw 'Wrong control version' }
    return $value
}
function Read-Reply($process,$id) {
    for($attempt=0;$attempt -lt 8;$attempt++) {
        $message=Read-Message $process
        if($message.id -eq $id) { return $message }
    }
    throw "No response for request $id"
}
foreach($mode in @('stop','parent-eof')) {
    $process=Start-PwarClient
    $stderr=$process.StandardError.ReadToEndAsync()
    try {
        $ready=Read-Message $process
        if($ready.event -ne 'asio_start_accepted') { throw "ASIO start rejected: $($ready | ConvertTo-Json -Compress -Depth 5)" }
        $process.StandardInput.WriteLine('{"version":9,"id":"bad-version","command":"stop"}')
        $process.StandardInput.Flush()
        $rejected=Read-Reply $process 'bad-version'
        if($rejected.ok -or $rejected.error.code -ne 'UNSUPPORTED_VERSION') { throw 'Unsupported request was not rejected' }
        $process.StandardInput.WriteLine('{"version":1,"id":"read-1","command":"status"}')
        $process.StandardInput.Flush()
        $reply=Read-Reply $process 'read-1'
        if(-not $reply.ok -or $reply.status.state -notin @('waiting_clock','running')) { throw 'Invalid live status' }
        if($null -ne $reply.status.latency_ms -or $null -ne $reply.status.drift_ppm) { throw 'Unmeasured latency or drift reported as known' }
        if($mode -eq 'stop') {
            $process.StandardInput.WriteLine('{"version":1,"id":"stop-1","command":"stop"}')
            $process.StandardInput.Flush()
            $reply=Read-Reply $process 'stop-1'
            if(-not $reply.ok -or $reply.status.state -ne 'stopping') { throw 'Stop was not acknowledged' }
        } else { $process.StandardInput.Close() }
        for($attempt=0;$attempt -lt 8;$attempt++) {
            $finished=Read-Message $process
            if($finished.event -ne 'state_changed') { break }
        }
        if($finished.event -ne 'stopped' -or $finished.status.state -ne 'stopped') { throw "Missing stopped event: $($finished | ConvertTo-Json -Compress -Depth 5)" }
        if(-not $process.WaitForExit(10000)) { throw 'PWAR did not terminate' }
        if($process.ExitCode -ne 0) { throw "PWAR failed: $($stderr.Result)" }
        [pscustomobject]@{test=$mode;passed=$true;callbacks=$finished.status.callbacks;peer=$finished.status.peer_address}
    } finally {
        if(-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
