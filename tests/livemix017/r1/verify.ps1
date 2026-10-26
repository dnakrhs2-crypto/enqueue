param([switch]$Baseline, [switch]$AuditOnly)
$ErrorActionPreference = 'Stop'
if ($env:COMPUTERNAME -ne 'DESKTOP-7RO3E0O') { throw 'This verification must run on roomc via rb.py' }
$repo = (Resolve-Path "$PSScriptRoot/../../..").Path
Set-Location $repo
$logs = Join-Path $repo 'build/r1-validation'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$phase = if ($Baseline) { 'before' } else { 'after' }
$cmake = 'C:/Users/claude/tools/cmake/bin/cmake.exe'
function Invoke-Logged([string]$Command, [string[]]$Arguments, [string]$Log) {
    # Windows PowerShell 5.1 turns native stderr (including CMake warnings) into
    # ErrorRecords. Preserve it in the log and decide failure by the exit code.
    $ErrorActionPreference = 'Continue'
    & $Command @Arguments 2>&1 | Tee-Object -FilePath $Log
    if ($LASTEXITCODE) { throw "$Command failed ($LASTEXITCODE)" }
}
if (-not $AuditOnly) {
    Invoke-Logged $cmake @('--preset', 'local') "$logs/$phase-configure.txt"
    $targets = if ($Baseline) { @('LiveMix') } else { @('LiveMix', 'EnqueueTests', 'LiveMixWebGuestProbe') }
    Invoke-Logged $cmake (@('--build', '--preset', 'local', '--target') + $targets) "$logs/$phase-build.txt"
}
if (Select-String -Path "$logs/$phase-build.txt" -Pattern 'LNK4098' -Quiet) { throw 'mixed MSVC runtimes' }
$exe = Get-Item 'build/vs2022/LiveMix_artefacts/Release/LiveMix.exe'
@{ phase = $phase; bytes = $exe.Length; sha256 = (Get-FileHash $exe.FullName).Hash } |
    ConvertTo-Json | Set-Content -Encoding UTF8 "$logs/$phase-size.json"
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -property installationPath
$dumpbin = Get-ChildItem "$vs/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe" | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $dumpbin) { throw 'dumpbin missing' }
& $dumpbin.FullName /dependents $exe.FullName | Tee-Object "$logs/$phase-dependents.txt"
if ($LASTEXITCODE) { throw 'dumpbin failed' }
if (-not $Baseline) {
    if (-not $AuditOnly) {
        Invoke-Logged './build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe' @('LiveMix WebGuest') "$logs/webguest.txt"
        Invoke-Logged './build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe' @('LiveMix Collab') "$logs/collab.txt"
    }
    if (-not (Select-String -Path "$logs/collab.txt" -Pattern '16874 passed, 0 failed' -Quiet)) {
        throw 'Collab assertion count differs from the R0 baseline'
    }
    $runtimeTargets = @('mbedtls', 'mbedx509', 'mbedcrypto', 'everest', 'p256m', 'usrsctp', 'juice-static', 'datachannel-static')
    $runtime = foreach ($project in Get-ChildItem 'build/vs2022/third_party' -Filter '*.vcxproj' -Recurse) {
        if ($project.BaseName -notin $runtimeTargets) { continue }
        [xml]$xml = Get-Content -LiteralPath $project.FullName
        foreach ($group in $xml.Project.ItemDefinitionGroup) {
            $value = [string]$group.ClCompile.RuntimeLibrary
            if (-not $value) { continue }
            if ($group.Condition -match 'Debug\|') { $expected = 'MultiThreadedDebug' }
            else { $expected = 'MultiThreaded' }
            if ($value -ne $expected) { throw "Wrong runtime: $($project.BaseName) $($group.Condition) = $value" }
            "$($project.BaseName) $($group.Condition) = $value"
        }
    }
    $runtime | Set-Content -Encoding UTF8 "$logs/runtimes.txt"
    & $dumpbin.FullName /dependents './build/vs2022/LiveMixWebGuestProbe_artefacts/Release/LiveMixWebGuestProbe.exe' |
        Tee-Object "$logs/probe-dependents.txt"
    if ($LASTEXITCODE) { throw 'probe dumpbin failed' }
}
