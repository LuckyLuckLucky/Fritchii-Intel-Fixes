# Differential test of every HMMA compute shader: software HMMA vs 8x16x16 cooperative matrices
# (optionally with the Vulkan memory model coherence pass) on the Intel GPU.
param([string]$Variant = 'coop')   # coop | coherent
$ErrorActionPreference = 'Continue'
$root = 'C:\work\Fritchii-intel\_tools'
$c = "$root\hot\compute"
$sweep = 'C:\work\Fritchii-intel\_build\bin\shader-sweep.exe'
$cross = 'C:\VulkanSDK\1.4.363.0\Bin\spirv-cross.exe'
New-Item -ItemType Directory -Force "$c\sw", "$c\layout", "$c\coherent" | Out-Null
$results = @()
foreach ($line in Get-Content "$c\hmma.txt") {
    $hash = $line.Split(' ')[0]
    $env:SWEEP_INTEL = '1'; Remove-Item Env:SWEEP_COOP8 -ErrorAction SilentlyContinue
    & $sweep --environment "$c\env_$hash.bin" --cache-version 52 --output "$c\sw\$hash.spv" | Out-Null
    Remove-Item Env:SWEEP_INTEL
    $b = "$c\coop\$hash.spv"
    if ($Variant -eq 'coherent') {
        & "$root\gplbench\spvcoherent.exe" $b "$c\coherent\$hash.spv" | Out-Null
        $b = "$c\coherent\$hash.spv"
    }
    & $cross $b --reflect --output "$c\layout\$hash.json" 2>$null
    $j = Get-Content "$c\layout\$hash.json" -Raw | ConvertFrom-Json
    $lines = @()
    $count = { param($r) if ($r.array) { [int]$r.array[0] } else { 1 } }
    $dim = { param($t) if ($t -match '3D') { '3D' } elseif ($t -match 'Array') { 'Array2D' } elseif ($t -match 'Buffer') { 'Buffer' } else { '2D' } }
    foreach ($r in $j.ubos) { $lines += "$($r.set) $($r.binding) 6 $(& $count $r) -" }
    foreach ($r in $j.ssbos) { $lines += "$($r.set) $($r.binding) 7 $(& $count $r) -" }
    foreach ($r in $j.textures) { $t = if ($r.type -match 'Buffer') { 4 } else { 1 }; $lines += "$($r.set) $($r.binding) $t $(& $count $r) $(& $dim $r.type)" }
    foreach ($r in $j.separate_images) { $t = if ($r.type -match 'Buffer') { 4 } else { 2 }; $lines += "$($r.set) $($r.binding) $t $(& $count $r) $(& $dim $r.type)" }
    foreach ($r in $j.separate_samplers) { $lines += "$($r.set) $($r.binding) 0 $(& $count $r) -" }
    foreach ($r in $j.images) { $t = if ($r.type -match 'Buffer') { 5 } else { 3 }; $lines += "$($r.set) $($r.binding) $t $(& $count $r) $(& $dim $r.type)" }
    Set-Content -Encoding ascii "$c\layout\$hash.txt" $lines
    $verdicts = @()
    foreach ($fill in '80', 'rand', '400') {
        $out = & "$root\gplbench\difftest.exe" "$c\sw\$hash.spv" $b "$c\layout\$hash.txt" 8 8 1 $fill 2>&1 | Out-String
        $m = [regex]::Match($out, 'RESULT changed (\d+) of (\d+) halves, differing (\d+) \(worst binding (-?\d+)\) bitwise (\d+) special (\d+) maxrel ([0-9.]+) meanrel ([0-9.]+)')
        if ($m.Success) { $verdicts += "${fill}:chg=$($m.Groups[1].Value),bad=$($m.Groups[3].Value),nanmis=$($m.Groups[6].Value),maxrel=$($m.Groups[7].Value),mean=$($m.Groups[8].Value)" }
        else { $verdicts += "${fill}:ERROR($($out.Trim().Split("`n")[-1]))" }
    }
    $results += "$line  |  $($verdicts -join '  ')"
    Write-Output "$hash  $($verdicts -join '  ')"
}
Set-Content "$c\difftest_$Variant.txt" $results
