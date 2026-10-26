$ErrorActionPreference = 'Continue'
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --preset local
if ($LASTEXITCODE) { throw 'R9 configure failed' }
& 'C:/Users/claude/tools/cmake/bin/cmake.exe' --build --preset local-release --target LiveMix EnqueueTests LiveMixCollabProbe
if ($LASTEXITCODE) { throw 'R9 build failed' }
