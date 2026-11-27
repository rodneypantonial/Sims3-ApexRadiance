param([string]$OutDir=(Join-Path $env:TEMP 'ApexStartDiagChecks'))
# Start diagnostics offline checks (check.cpp)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT /Festart_diag_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Start diagnostics test compilation failed'}
 & '.\start_diag_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Start diagnostics checks failed'}
} finally {Pop-Location}
