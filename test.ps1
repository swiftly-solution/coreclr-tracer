xmake -j 6
dotnet build ./test/Test.csproj -v q

$env:CORECLR_ENABLE_PROFILING=1
$env:CORECLR_PROFILER="{2e7d6025-9ef7-4942-b35e-4a9d3c4aa024}"
$env:CORECLR_PROFILER_PATH="./build/windows/x64/release/coreclr-tracer.dll"
Write-Host "`nEnvironment Variables:" -ForegroundColor Cyan
Get-ChildItem Env: | Where-Object { $_.Name -like "*CORECLR*" -or $_.Name -like "*COMPlus*" } | Format-Table
& ./test/bin/Debug/net10.0/Test.exe
