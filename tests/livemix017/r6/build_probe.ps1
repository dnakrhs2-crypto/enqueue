$ErrorActionPreference = 'Stop'
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --preset local
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --build build/vs2022 --config Release --target LiveMixWebGuestProbe
exit $LASTEXITCODE
