param([Parameter(ValueFromRemainingArguments=$true)][string[]]$AppArguments)
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$global:LASTEXITCODE = 0
$root = Split-Path -Parent $PSCommandPath
Set-Location -LiteralPath $root
$portable = Join-Path $root "portable\Windows-x86_64\KoraLo.exe"
if (Test-Path -LiteralPath $portable) { & $portable @AppArguments; exit $LASTEXITCODE }
$build = Join-Path $root "build-windows-native"
foreach ($relative in @("Release\KoraLo.exe", "KoraLo.exe")) {
    $existing = Join-Path $build $relative
    if (Test-Path -LiteralPath $existing) { & $existing @AppArguments; exit $LASTEXITCODE }
}
foreach ($tool in @("cmake", "ctest")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Source build requires preinstalled CMake and C++ build tools. Use the ready portable package."
    }
}
& cmake -S $root -B $build -DKORALO_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw "Configure failed" }
& cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
& ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Tests failed" }
foreach ($relative in @("Release\KoraLo.exe", "KoraLo.exe")) {
    $exe = Join-Path $build $relative
    if (Test-Path -LiteralPath $exe) { & $exe @AppArguments; exit $LASTEXITCODE }
}
throw "KoraLo.exe was not produced"
