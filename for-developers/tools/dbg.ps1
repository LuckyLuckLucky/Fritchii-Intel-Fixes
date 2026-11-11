# Minimal client for Lichii's debug server (EDEN_NX2_DEBUG_SERVER, JSON lines over TCP).
# Usage: dbg.ps1 '<method>' '<params json>' [timeout_s]
param([string]$Method = 'status', [string]$Params = '{}', [int]$Timeout = 120)
$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect('127.0.0.1', 6545)
} catch {
    Write-Output '{"error":"no debug server"}'
    exit 1
}
$client.ReceiveTimeout = $Timeout * 1000
$stream = $client.GetStream()
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($stream)
# Params may use ' for " (quotes are lost when passed through powershell -File).
$Params = $Params -replace "'", '"'
$writer.WriteLine("{`"id`":1,`"method`":`"$Method`",`"params`":$Params}")
try {
    $line = $reader.ReadLine()
    # Screenshots: the PNG goes to a file; status: drop the frame time histogram.
    $m = [regex]::Match($line, '"png_base64":"([^"]+)"')
    if ($m.Success) {
        $out = 'C:\work\Fritchii-intel\_tools\debugdump\shot.png'
        [IO.File]::WriteAllBytes($out, [Convert]::FromBase64String($m.Groups[1].Value))
        $line = $line.Replace($m.Groups[1].Value, "saved to $out")
    }
    $line = [regex]::Replace($line, '"frame_times":\[[^\]]*\],', '')
    Write-Output $line
} catch {
    Write-Output '{"error":"timeout"}'
}
$client.Close()
