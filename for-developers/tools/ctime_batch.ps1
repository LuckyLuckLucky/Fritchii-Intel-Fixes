# Reflects layouts for a sample of OoT's compute shaders (Intel profile SPIR-V in hot\compute\coop)
# and times their pipeline builds on the Intel GPU (gplbench\ctime.exe). Output: hot\compute\ctime.csv
param([int]$Every = 4)
$root = 'C:\work\Fritchii-intel\_tools'
$c = "$root\hot\compute"
$cross = 'C:\VulkanSDK\1.4.363.0\Bin\spirv-cross.exe'
New-Item -ItemType Directory -Force "$c\layout" | Out-Null
$list = @()
$i = 0
foreach ($spv in Get-ChildItem "$c\coop\*.spv" | Sort-Object Name) {
    if (($i++ % $Every) -ne 0) { continue }
    $hash = $spv.BaseName
    $txt = "$c\layout\$hash.txt"
    if (-not (Test-Path $txt)) {
        & $cross $spv.FullName --reflect --output "$c\layout\$hash.json" 2>$null
        if (-not (Test-Path "$c\layout\$hash.json")) { continue }
        $j = Get-Content "$c\layout\$hash.json" -Raw | ConvertFrom-Json
        $lines = @()
        $count = { param($r) if ($r.array) { [int]$r.array[0] } else { 1 } }
        foreach ($r in $j.ubos) { $lines += "$($r.set) $($r.binding) 6 $(& $count $r)" }
        foreach ($r in $j.ssbos) { $lines += "$($r.set) $($r.binding) 7 $(& $count $r)" }
        foreach ($r in $j.textures) { $t = if ($r.type -match 'Buffer') { 4 } else { 1 }; $lines += "$($r.set) $($r.binding) $t $(& $count $r)" }
        foreach ($r in $j.separate_images) { $t = if ($r.type -match 'Buffer') { 4 } else { 2 }; $lines += "$($r.set) $($r.binding) $t $(& $count $r)" }
        foreach ($r in $j.separate_samplers) { $lines += "$($r.set) $($r.binding) 0 $(& $count $r)" }
        foreach ($r in $j.images) { $t = if ($r.type -match 'Buffer') { 5 } else { 3 }; $lines += "$($r.set) $($r.binding) $t $(& $count $r)" }
        Set-Content -Encoding ascii $txt $lines
    }
    $list += "$($spv.FullName) $txt"
}
Set-Content -Encoding ascii "$c\ctime_list.txt" $list
& "$root\gplbench\ctime.exe" "$c\ctime_list.txt" | Set-Content -Encoding ascii "$c\ctime.csv"
