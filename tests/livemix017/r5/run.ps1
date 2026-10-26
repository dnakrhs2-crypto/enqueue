# Run only through rb.py, from the repository root. Outputs stay in the ignored build directory.
$ErrorActionPreference = 'Stop'
$r5Evidence = Join-Path (Get-Location) 'build/r5-validation'
New-Item -ItemType Directory -Path $r5Evidence -Force | Out-Null
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors |
    ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $r5Evidence 'machine.json')
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --preset local |
    Tee-Object -FilePath (Join-Path $r5Evidence 'configure.txt')
if ($LASTEXITCODE) { throw 'R5 configure failed' }
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --build --preset local --target LiveMixRecorderProbe --parallel 2 |
    Tee-Object -FilePath (Join-Path $r5Evidence 'probe-build.txt')
if ($LASTEXITCODE) { throw 'R5 probe build failed' }
& './build/vs2022/LiveMixRecorderProbe_artefacts/Release/LiveMixRecorderProbe.exe' |
    Tee-Object -FilePath (Join-Path $r5Evidence 'probe.txt')
$r5ProbeCode = $LASTEXITCODE
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --build --preset local --target LiveMix EnqueueTests --parallel 2 |
    Tee-Object -FilePath (Join-Path $r5Evidence 'build.txt')
if ($LASTEXITCODE) { throw 'R5 build failed' }
& './build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe' 'LiveMix Recorder' |
    Tee-Object -FilePath (Join-Path $r5Evidence 'recorder.txt')
$r5RecorderCode = $LASTEXITCODE
& './build/vs2022/tests/EnqueueTests_artefacts/Release/EnqueueTests.exe' 'LiveMix Collab' |
    Tee-Object -FilePath (Join-Path $r5Evidence 'collab.txt')
$r5CollabCode = $LASTEXITCODE
if ($r5RecorderCode -or $r5ProbeCode -or $r5CollabCode) { throw 'R5 validation failed; see build/r5-validation' }
