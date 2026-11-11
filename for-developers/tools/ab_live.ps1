# Live A/B of EXP switches on a running emulator (debug server). For each round: every phase in
# -Phases ("name=value,name=value|name=value" ; "|" separates phases) is set, then fps is sampled.
param([string]$Phases, [int]$Rounds = 2, [int]$Settle = 4, [int]$Samples = 2)
$d = 'C:\work\Fritchii-intel\_tools\dbg.ps1'
function Fps { $s = & powershell -NoProfile -ExecutionPolicy Bypass -File $d status "{'fps_window_ms':5000}" 15; [double][regex]::Match($s, '"fps":([0-9.]+)').Groups[1].Value }
for ($round = 1; $round -le $Rounds; $round++) {
    foreach ($phase in $Phases.Split('|')) {
        foreach ($pair in $phase.Split(',')) {
            $n, $v = $pair.Split('=')
            & powershell -NoProfile -ExecutionPolicy Bypass -File $d switch "{'name':'$n','value':$v}" 10 | Out-Null
        }
        Start-Sleep $Settle
        $values = 1..$Samples | % { Start-Sleep 5; Fps }
        "{0,-28} {1}" -f $phase, (($values | % { $_.ToString('0.0') }) -join ' ')
    }
}
