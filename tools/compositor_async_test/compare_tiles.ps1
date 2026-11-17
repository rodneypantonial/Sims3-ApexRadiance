param([string]$RunDir, [string]$Reference = 'game', [switch]$SelfCheck)
# Compares the texture tiles logged by the loads of the in-game job compositor-async ([patches.AsyncCompositor]
# tileHashLog): every arm against the reference arm, as sets with counts (the arms finish tiles in another order).
# Read-only. -SelfCheck tests the line pattern and the comparison on built-in lines.
$ErrorActionPreference = 'Stop'
$pattern = '\[AsyncCompositor\] tile (\d+x\d+ at \d+,\d+ hash [0-9a-f]{16})\s*$'
function Get-Tiles([string[]]$Lines) {
    $t = @{}
    foreach ($l in $Lines) { if ($l -match $pattern) { $t[$Matches[1]] = 1 + [int]$t[$Matches[1]] } }
    $t
}
function Compare-Tiles($Ref, $Arm) {
    $onlyRef = 0; $onlyArm = 0; $same = 0
    foreach ($k in @($Ref.Keys) + @($Arm.Keys) | Sort-Object -Unique) {
        $r = [int]$Ref[$k]; $n = [int]$Arm[$k]
        $same += [Math]::Min($r, $n); $onlyRef += [Math]::Max(0, $r - $n); $onlyArm += [Math]::Max(0, $n - $r)
    }
    [pscustomobject]@{ Same = $same; OnlyReference = $onlyRef; OnlyArm = $onlyArm }
}
if ($SelfCheck) {
    $hit = '[12:00:01.123] [INFO] [AsyncCompositor] tile 256x256 at 0,0 hash 00ff00ff00ff00ff'
    $miss = @('[12:00:01.123] [INFO] [AsyncCompositor] 12 tiles issued, 12 ready', '[INFO] [AsyncCompositor] tile 256x256 at 0,0 hash xyz', '[INFO] [AsyncCompositor] On: mode 2 (several tiles in flight, tile hashes logged)')
    $ref = Get-Tiles @($hit, $hit, ($hit -replace '00ff$', '0100')); $arm = Get-Tiles (@($hit, $hit) + $miss)
    $c = Compare-Tiles $ref $arm
    if ($ref.Count -ne 2 -or $arm.Count -ne 1 -or $c.Same -ne 2 -or $c.OnlyReference -ne 1 -or $c.OnlyArm -ne 0) { throw 'compare_tiles self-check failed' }
    Write-Output 'compare_tiles self-check passed'
    return
}
if (-not $RunDir) { throw 'Give -RunDir (the output folder of a compositor-async run) or -SelfCheck' }
$arms = @{}
foreach ($d in Get-ChildItem -LiteralPath $RunDir -Directory | Where-Object Name -match '^compositor-async-\d+-(.+)$') {
    $log = Join-Path $d.FullName 'ApexRadiance_LOG.txt'
    if (Test-Path -LiteralPath $log) { $arms[($d.Name -replace '^compositor-async-\d+-', '')] = Get-Tiles @(Get-Content -LiteralPath $log) }
}
if (-not $arms.ContainsKey($Reference)) { throw "No load of the '$Reference' arm with a log in $RunDir" }
$refTotal = ($arms[$Reference].Values | Measure-Object -Sum).Sum
if (-not $refTotal) { throw "The '$Reference' arm logged no tiles (tileHashLog on, development build?)" }
$bad = 0
foreach ($name in $arms.Keys | Where-Object { $_ -ne $Reference } | Sort-Object) {
    $c = Compare-Tiles $arms[$Reference] $arms[$name]
    Write-Output ("{0} against {1}: {2} tiles the same, {3} only in {1}, {4} only in {0}" -f $name, $Reference, $c.Same, $c.OnlyReference, $c.OnlyArm)
    if ($c.OnlyReference -or $c.OnlyArm) { $bad++ }
}
if ($arms.Count -lt 2) { throw 'Only the reference arm has a log' }
if ($bad) { throw "$bad arm(s) differ from '$Reference'" }
Write-Output 'Every arm logged the same tiles.'
