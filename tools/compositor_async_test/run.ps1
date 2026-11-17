param([string]$OutDir=(Join-Path $env:TEMP 'ApexCompositorAsyncChecks'))
# Asynchronous compositor read-back: the queue decisions of features/compositor_async.h over a model of the game's queue
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
. (Join-Path $repo 'tools/terrain_lighting_test/compiler_env.ps1')
Initialize-TerrainCompiler
Push-Location -LiteralPath $OutDir
try {
 & cl.exe /nologo /std:c++20 /EHsc /W4 /O2 /MT ('/I'+(Join-Path $repo 'features')) /Fecompositor_async_checks.exe (Join-Path $PSScriptRoot 'check.cpp')
 if($LASTEXITCODE -ne 0){throw 'Compositor async test compilation failed'}
 & '.\compositor_async_checks.exe'
 if($LASTEXITCODE -ne 0){throw 'Compositor async checks failed'}
} finally {Pop-Location}

# The tile comparison of the in-game job (compare_tiles.ps1): its line pattern and counts
& (Join-Path $PSScriptRoot 'compare_tiles.ps1') -SelfCheck
