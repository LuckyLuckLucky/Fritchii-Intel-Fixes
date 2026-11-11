# Launches a build, loads the first save (Hyrule Field), turns Link to look across the field,
# prints fps samples, takes a screenshot, then closes the emulator.
# -Env "A=1;B=2" sets environment variables for the run. -Keep leaves the emulator running.
param([string]$Folder, [string]$Env = '', [int]$Samples = 6, [string]$Shot = '', [switch]$Keep)
$r = 'C:\work\Fritchii-intel'; $d = "$r\_tools\dbg.ps1"
# Never two instances at once: close any running emulator first.
Stop-Process -Name lichii -Force -ErrorAction SilentlyContinue; Start-Sleep 2
& "$r\_tools\field_run.ps1" -Folder $Folder -Env $Env -Samples 2
& powershell -NoProfile -ExecutionPolicy Bypass -File $d input_sequence "{'steps':[{'left_stick':[0,-1],'frames':40},{'frames':10},{'buttons':['ZL'],'frames':10},{'frames':150}]}" 60 | Out-Null
Start-Sleep 8
'distance:'
1..$Samples | % {
    Start-Sleep 5
    $s = & powershell -NoProfile -ExecutionPolicy Bypass -File $d status "{'fps_window_ms':5000}" 15
    [regex]::Match($s, '"fps":[0-9.]+').Value
}
if ($Shot) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $d screenshot "{}" 30 | Out-Null
    Copy-Item "$r\_tools\debugdump\shot.png" "$r\_tools\debugdump\$Shot.png" -Force
}
if (-not $Keep) { Stop-Process -Name lichii -Force -ErrorAction SilentlyContinue }
