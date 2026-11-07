param([string]$EnvironmentRoot = 'C:\Users\TUSB\Desktop\agent\environment')
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$aidlTool = Join-Path $EnvironmentRoot 'android-sdk\build-tools\37.0.0\aidl.exe'
$compiler = Join-Path $EnvironmentRoot 'android-ndk-r29\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe'
New-Item -ItemType Directory -Force -Path "$taskRoot\generated", "$taskRoot\include", "$taskRoot\out" | Out-Null
foreach ($kind in @('omapi', 'hal')) {
    $folder = if ($kind -eq 'omapi') { 'android\se\omapi' } else { 'android\hardware\secure_element' }
    $hash = (Get-Content -LiteralPath "$taskRoot\$kind.hash").Trim()
    $inputs = Get-ChildItem -LiteralPath "$taskRoot\aidl\$folder" -Filter *.aidl | ForEach-Object FullName
    & $aidlTool --lang=ndk --structured --stability=vintf --version=1 "--hash=$hash" --min_sdk_version=35 --omit_invocation "--include=$taskRoot\aidl" "--out=$taskRoot\generated" "--header_out=$taskRoot\include" @inputs
    if ($LASTEXITCODE -ne 0) { throw "AIDL generation failed for $kind" }
}
$sources = Get-ChildItem -LiteralPath "$taskRoot\generated" -Recurse -Filter *.cpp | ForEach-Object FullName
& $compiler --target=aarch64-linux-android35 -std=c++17 -O2 -Wall -Wextra -Werror -DBINDER_STABILITY_SUPPORT -fPIE -pie "-I$taskRoot\include" "$taskRoot\main.cpp" @sources -static-libstdc++ -lbinder_ndk -ldl -o "$taskRoot\out\omapi-bridge"
if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed' }
Get-FileHash -LiteralPath "$taskRoot\out\omapi-bridge" -Algorithm SHA256
