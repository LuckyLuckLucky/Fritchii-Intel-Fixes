# Launches a build (launch_env.cmd args), loads the first save, waits, prints fps samples.
# -Env "A=1;B=2" sets environment variables for the run.
param([string]$Folder, [string]$Env = '', [int]$Samples = 6)
$r = 'C:\work\Fritchii-intel'; $d = "$r\_tools\dbg.ps1"
$list = @("`"$Folder`"")
foreach ($e in $Env.Split(';')) { if ($e) { $list += "`"$e`"" } }
cmd /c "$r\_tools\launch_env.cmd $($list -join ' ')"
for ($i = 0; $i -lt 90; $i++) {
    Start-Sleep 2
    $s = & powershell -NoProfile -ExecutionPolicy Bypass -File $d status "{'fps_window_ms':0}" 5
    if ($s -match '"presents":(\d+)' -and [int]$Matches[1] -gt 600) { break }
}
& powershell -NoProfile -ExecutionPolicy Bypass -File $d input_sequence "{'steps':[{'buttons':['A'],'frames':6},{'frames':180},{'buttons':['A'],'frames':6},{'frames':120},{'buttons':['A'],'frames':6},{'frames':120},{'buttons':['A'],'frames':6},{'frames':600}]}" 120 | Out-Null
Start-Sleep 30
& powershell -NoProfile -ExecutionPolicy Bypass -File $d screenshot "{}" 30 | Out-Null
1..$Samples | % {
    Start-Sleep 5
    $s = & powershell -NoProfile -ExecutionPolicy Bypass -File $d status "{'fps_window_ms':5000}" 15
    [regex]::Match($s, '"fps":[0-9.]+').Value
}
