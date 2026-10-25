[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Source,
    [Parameter(Mandatory = $true)][string] $Work
)

$ErrorActionPreference = 'Stop'

$revision = '2d16be6c66ffa9a157ff1bb50ffe215558b40394'
$patch = Join-Path $PSScriptRoot '..\patches\fna\stardew-fna.patch'

if (-not (Test-Path -LiteralPath (Join-Path $Source '.git'))) {
    throw "FNA source is not a git checkout: $Source"
}

if (-not (Test-Path -LiteralPath $patch)) {
    throw "Missing FNA patch: $patch"
}

if (Test-Path -LiteralPath $Work) {
    throw "Work folder already exists: $Work"
}

git clone --no-checkout $Source $Work
if ($LASTEXITCODE -ne 0) {
    throw 'FNA clone failed'
}

git -C $Work checkout --detach $revision
if ($LASTEXITCODE -ne 0) {
    throw "FNA revision $revision is unavailable from $Source"
}

git -C $Work submodule update --init --recursive
if ($LASTEXITCODE -ne 0) {
    throw 'FNA submodule setup failed'
}

git -C $Work apply --check $patch
if ($LASTEXITCODE -ne 0) {
    throw 'FNA patch does not match the pinned revision'
}

git -C $Work apply $patch
if ($LASTEXITCODE -ne 0) {
    throw 'FNA patch apply failed'
}

dotnet build (Join-Path $Work 'FNA.Core.csproj') -c Release --nologo
if ($LASTEXITCODE -ne 0) {
    throw 'FNA build failed'
}

Write-Host "built $(Join-Path $Work 'bin\Release\net6.0\MonoGame.Framework.dll')"
