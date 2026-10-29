param([ValidateSet('build', 'plc', 'regression', 'realtime', 'before-fix1', 'burst')][string]$Stage = 'plc',
      [string]$EvidenceDirectory = 'build/plc0177')
$ErrorActionPreference = 'Stop'
if ($env:COMPUTERNAME -ne 'DESKTOP-7RO3E0O') { throw 'This verification must run on roomc through rb.py.' }
$cmake = 'C:/Users/claude/tools/cmake/bin/cmake.exe'
$evidence = $EvidenceDirectory
New-Item -ItemType Directory -Force $evidence | Out-Null
# PowerShell 5 treats a native tool's stderr warning as an ErrorRecord. Judge
# native tools by their exit codes; CMake deprecation warnings are not failures.
$ErrorActionPreference = 'Continue'
if ($Stage -eq 'burst') {
    & $cmake --preset local *> "$evidence/burst-configure.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$evidence/burst-configure.log" -Tail 40; exit $LASTEXITCODE }
    & $cmake --build build/vs2022 --config Release --target LiveMixCollabPlcBurstBeforeTest LiveMixCollabPlcBurstTest --parallel 2 *> "$evidence/burst-build.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$evidence/burst-build.log" -Tail 40; exit $LASTEXITCODE }
    & ./build/vs2022/tests/Release/LiveMixCollabPlcBurstBeforeTest.exe *> "$evidence/before-burst.log"
    $before = $LASTEXITCODE
    & ./build/vs2022/tests/Release/LiveMixCollabPlcBurstTest.exe *> "$evidence/after-burst.log"
    $after = $LASTEXITCODE
    "FIX2 before_exit=$before after_exit=$after" | Out-File "$evidence/timing.txt" -Append -Encoding utf8
    Get-Content "$evidence/before-burst.log" -Tail 2
    Get-Content "$evidence/after-burst.log" -Tail 4
    exit $after
}
if ($Stage -eq 'before-fix1') {
    & $cmake --preset local *> "$evidence/before-configure.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$evidence/before-configure.log" -Tail 40; exit $LASTEXITCODE }
    & $cmake --build build/vs2022 --config Release --target LiveMixCollabPlcRecoveryBeforeTest --parallel 2 *> "$evidence/before-build.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$evidence/before-build.log" -Tail 40; exit $LASTEXITCODE }
    & ./build/vs2022/tests/Release/LiveMixCollabPlcRecoveryBeforeTest.exe *> "$evidence/before-recovery.log"
    $code = $LASTEXITCODE
    Get-Content "$evidence/before-recovery.log"
    exit $code
}
if ($Stage -eq 'realtime') {
    & $cmake --build build/vs2022 --config Debug --target LiveMixCollabPlcRealtimeTest --parallel 2 *> "$evidence/realtime-build.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$evidence/realtime-build.log" -Tail 40; exit $LASTEXITCODE }
    & ./build/vs2022/tests/Debug/LiveMixCollabPlcRealtimeTest.exe *> "$evidence/realtime.log"
    $code = $LASTEXITCODE
    Get-Content "$evidence/realtime.log"
    exit $code
}
if ($Stage -eq 'build') {
    & $cmake --preset local *> "$evidence/configure.log"
    if ($LASTEXITCODE -ne 0) { Get-Content "$evidence/configure.log" -Tail 60; exit $LASTEXITCODE }
    & $cmake --build build/vs2022 --config Release --target EnqueueTests LiveMixCollabProbe --parallel 2 *> "$evidence/build.log"
    $code = $LASTEXITCODE
    Get-Content "$evidence/build.log" -Tail 40
    exit $code
}
$exe = 'build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe'
$filters = if ($Stage -eq 'plc') { @('LiveMix Collab PLC', 'LiveMix Collab playout') } else { @('LiveMix Collab', 'LiveMix WebGuest') }
foreach ($filter in $filters) {
    $log = "$evidence/$($filter.Replace(' ', '-')).log"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    & $exe $filter *> $log
    $code = $LASTEXITCODE
    $timer.Stop()
    "$filter seconds=$($timer.Elapsed.TotalSeconds) exit=$code" | Out-File "$evidence/timing.txt" -Append -Encoding utf8
    Get-Content $log -Tail 12
    if ($code -ne 0) { exit $code }
}
exit 0
