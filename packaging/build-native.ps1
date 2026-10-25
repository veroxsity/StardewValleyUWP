[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $SdlSource,
    [Parameter(Mandatory = $true)][string] $Fna3dSource,
    [Parameter(Mandatory = $true)][string] $FAudioSource,
    [Parameter(Mandatory = $true)][string] $BuildRoot
)

$ErrorActionPreference = 'Stop'

$generator = 'Visual Studio 17 2022'
$platform = 'x64,version=10.0.26100.0'
$instance = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -version '[17.0,18.0)' -products * -property installationPath | Select-Object -First 1
if (-not $instance) {
    throw 'No Visual Studio 2022 install found'
}
$instance = $instance -replace '\\', '/'
$sdlRevision = 'e1bc7d6de9d78df354807327785703c4044e807b'
$fna3dRevision = 'a5b6c07b89fd48cf64e310d86f0be3108e76f21b'
$faudioRevision = '221bfe4383542f5e8ccc7bc97db968c7068f86e3'
$fna3dPatch = Join-Path $PSScriptRoot '..\patches\fna3d\uwp-corewindow-swapchain.patch'
$BuildRoot = [System.IO.Path]::GetFullPath($BuildRoot)

foreach ($source in $SdlSource, $Fna3dSource, $FAudioSource) {
    if (-not (Test-Path -LiteralPath (Join-Path $source '.git'))) {
        throw "Native source is not a git checkout: $source"
    }
}

if (-not (Test-Path -LiteralPath $fna3dPatch)) {
    throw "Missing FNA3D patch: $fna3dPatch"
}

if (Test-Path -LiteralPath $BuildRoot) {
    throw "Build folder already exists: $BuildRoot"
}

New-Item -ItemType Directory -Path $BuildRoot | Out-Null

function Copy-PinnedSource([string] $Source, [string] $Destination, [string] $Revision) {
    git clone --no-checkout $Source $Destination
    if ($LASTEXITCODE -ne 0) {
        throw "Clone failed: $Source"
    }

    git -C $Destination checkout --detach $Revision
    if ($LASTEXITCODE -ne 0) {
        throw "Revision $Revision is unavailable from $Source"
    }

    git -C $Destination submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        throw "Submodule setup failed: $Destination"
    }
}

$sdlWork = Join-Path $BuildRoot 'source\SDL2'
$fna3dWork = Join-Path $BuildRoot 'source\FNA3D'
$faudioWork = Join-Path $BuildRoot 'source\FAudio'
$sdlBuild = Join-Path $BuildRoot 'SDL2'
$fna3dBuild = Join-Path $BuildRoot 'FNA3D'
$faudioBuild = Join-Path $BuildRoot 'FAudio'

New-Item -ItemType Directory -Path (Join-Path $BuildRoot 'source') | Out-Null
Copy-PinnedSource $SdlSource $sdlWork $sdlRevision
Copy-PinnedSource $Fna3dSource $fna3dWork $fna3dRevision
Copy-PinnedSource $FAudioSource $faudioWork $faudioRevision

git -C $fna3dWork apply --check $fna3dPatch
if ($LASTEXITCODE -ne 0) {
    throw 'FNA3D patch does not match the pinned revision'
}

git -C $fna3dWork apply $fna3dPatch
if ($LASTEXITCODE -ne 0) {
    throw 'FNA3D patch apply failed'
}

& cmake -S $sdlWork -B $sdlBuild -G $generator -A $platform "-DCMAKE_GENERATOR_INSTANCE=$instance" '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' '-DSDL_LIBC=ON' '-DSDL_SHARED=ON' '-DSDL_STATIC=OFF' '-DSDL_TEST=OFF' '-DSDL_TESTS=OFF' '-DSDL2_DISABLE_INSTALL=ON'
if ($LASTEXITCODE -ne 0) {
    throw 'SDL2 configure failed'
}

cmake --build $sdlBuild --config Release --target SDL2 --parallel
if ($LASTEXITCODE -ne 0) {
    throw 'SDL2 build failed'
}

$sdlIncludes = "$(Join-Path $sdlBuild 'include-config-release\SDL2');$(Join-Path $sdlWork 'include')"
$sdlLibrary = Join-Path $sdlBuild 'Release\SDL2.lib'

foreach ($pair in @(
    @($fna3dWork, $fna3dBuild, 'FNA3D'),
    @($faudioWork, $faudioBuild, 'FAudio')
)) {
    & cmake -S $pair[0] -B $pair[1] -G $generator -A $platform "-DCMAKE_GENERATOR_INSTANCE=$instance" '-DCMAKE_SYSTEM_NAME=WindowsStore' '-DCMAKE_SYSTEM_VERSION=10.0' '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' "-DSDL2_INCLUDE_DIRS=$sdlIncludes" "-DSDL2_LIBRARIES=$sdlLibrary"
    if ($LASTEXITCODE -ne 0) {
        throw "$($pair[2]) configure failed"
    }

    cmake --build $pair[1] --config Release --target $pair[2] --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "$($pair[2]) build failed"
    }
}

$outputs = @(
    (Join-Path $sdlBuild 'Release\SDL2.dll'),
    (Join-Path $fna3dBuild 'Release\FNA3D.dll'),
    (Join-Path $faudioBuild 'Release\FAudio.dll')
)

foreach ($output in $outputs) {
    if (-not (Test-Path -LiteralPath $output)) {
        throw "Native output missing: $output"
    }
    Write-Host $output
}
