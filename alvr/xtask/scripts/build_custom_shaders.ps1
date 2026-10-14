# Requires: Windows SDK (fxc.exe) installed
# Compiles custom HLSL foveation pixel shaders to .cso used by ALVR (Win32 D3D11)
# Outputs: AADT2PixelShader.cso, AADT3PixelShader.cso, FRWPixelShader.cso
# To build, run this script in terminal (edit the path as needed):
# Set-Location -Path 'D:\ResearchProjects\ALVR\alvr\xtask\scripts'; Set-ExecutionPolicy RemoteSigned -Scope Process -Force; .\build_custom_shaders.ps1
# Written by [AW]

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Find-Fxc {
    if ($env:FXC -and (Test-Path $env:FXC)) { return (Resolve-Path $env:FXC).Path }

    $cands = @()
    $base10 = "C:\Program Files (x86)\Windows Kits\10\bin"
    if (Test-Path $base10) {
        $cands += Get-ChildItem -Path $base10 -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            Join-Path $_.FullName 'x64\fxc.exe'
        }
    }
    $base81 = "C:\Program Files (x86)\Windows Kits\8.1\bin\x64\fxc.exe"
    if (Test-Path $base81) { $cands += $base81 }

    $fxc = $cands | Where-Object { Test-Path $_ } | Sort-Object -Descending | Select-Object -First 1
    if (-not $fxc) { throw "fxc.exe not found. Install Windows 10/11 SDK or set `$env:FXC to fxc.exe path." }
    return $fxc
}

$ScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot   = Resolve-Path (Join-Path $ScriptRoot '..\..\..')
$ShaderDir  = Join-Path $RepoRoot 'alvr\server_openvr\cpp\alvr_server\shader'
$OutDir     = Join-Path $RepoRoot 'alvr\server_openvr\cpp\platform\win32'

$fxc = Find-Fxc
Write-Host "Using fxc: $fxc"

$items = @(
    @{ in = 'CompressAxisAlignedPixelShader.hlsl'; out = 'CompressAxisAlignedPixelShader.cso'   ; entry = 'main'; profile = 'ps_5_0' }
    @{ in = 'AADT2PixelShader.hlsl'; out = 'AADT2PixelShader.cso'; entry = 'main'; profile = 'ps_5_0' },
    @{ in = 'AADT3PixelShader.hlsl'; out = 'AADT3PixelShader.cso'; entry = 'main'; profile = 'ps_5_0' },
    @{ in = 'FRWPixelShader.hlsl'  ; out = 'FRWPixelShader.cso'  ; entry = 'main'; profile = 'ps_5_0' }
)

foreach ($it in $items) {
    $inPath  = Join-Path $ShaderDir $it.in
    $outPath = Join-Path $OutDir   $it.out
    if (-not (Test-Path $inPath)) { throw "Missing input shader: $inPath" }

    $args = @(
        '/nologo', '/Ges',
        '/T', $it.profile,
        '/E', $it.entry,
        '/I', $ShaderDir,
        '/Fo', $outPath,
        $inPath
    )

    Write-Host "Compiling" $it.in '->' $outPath
    $p = Start-Process -FilePath $fxc -ArgumentList $args -NoNewWindow -PassThru -Wait
    if ($p.ExitCode -ne 0) { throw "fxc failed for $($it.in) with code $($p.ExitCode)" }
    if (-not (Test-Path $outPath)) { throw "fxc did not produce $outPath" }
}

Write-Host "Custom shaders compiled successfully to:" -ForegroundColor Green
$items | ForEach-Object { Join-Path $OutDir $_.out } | ForEach-Object { Write-Host " - $_" }
