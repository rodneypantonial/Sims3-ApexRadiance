param([string]$OutDir=(Join-Path $env:TEMP 'ApexLightProbeBudgetChecks'))
# The light capture's time budget (features/light_probe_budget.h)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /utf-8 ('/I'+(Join-Path $repo 'features')) /Felight_probe_budget_checks.exe (Join-Path $PSScriptRoot 'light_probe_budget_test.cpp')
 if($LASTEXITCODE -ne 0){throw 'Light probe budget test compilation failed'}
 & '.\light_probe_budget_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Light probe budget checks failed'}
} finally {Pop-Location}
