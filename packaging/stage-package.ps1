# lays out the package half from public downloads and the FNA build, no game files go anywhere near it

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $MonoGameFramework,
    [Parameter(Mandatory = $true)][string] $Work,
    [Parameter(Mandatory = $true)][string] $PackageOut
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$smapiVersion = '4.5.2'
$smapiUrl = "https://github.com/Pathoschild/SMAPI/releases/download/$smapiVersion/SMAPI-$smapiVersion-installer.zip"
$smapiSha256 = 'DD01DDCA7B566BFE0D3B3D2D03833496ABC56C53DA976241F2AB443F5484ACC4'
$runtimeVersion = '6.0.32'
$runtimeUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.netcore.app.runtime.win-x64/$runtimeVersion/microsoft.netcore.app.runtime.win-x64.$runtimeVersion.nupkg"
$runtimeSha256 = 'D6A773ED6AA9774828D147E971AA35F3A47090C04B9CB1E5CD5EB68FDC274B03'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
# dotnet build resolves relative paths from the project folder, not from where this was run
$Work = [System.IO.Path]::GetFullPath($Work)
$PackageOut = [System.IO.Path]::GetFullPath($PackageOut)
$MonoGameFramework = [System.IO.Path]::GetFullPath($MonoGameFramework)

if (-not (Test-Path -LiteralPath $MonoGameFramework)) {
    throw "Missing FNA build: $MonoGameFramework"
}
if (Test-Path -LiteralPath $PackageOut) {
    throw "Package folder already exists: $PackageOut"
}

function Get-Pinned([string] $Url, [string] $Sha256, [string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        Invoke-WebRequest $Url -OutFile $Path -UseBasicParsing
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($actual -ne $Sha256) {
        throw "$Url changed, expected $Sha256 and got $actual"
    }
}

New-Item -ItemType Directory -Path $Work -Force | Out-Null
$smapiZip = Join-Path $Work "smapi-$smapiVersion.zip"
$runtimeZip = Join-Path $Work "runtime-$runtimeVersion.zip"
Get-Pinned $smapiUrl $smapiSha256 $smapiZip
Get-Pinned $runtimeUrl $runtimeSha256 $runtimeZip

$smapiInstaller = Join-Path $Work "smapi-$smapiVersion"
$smapiFiles = Join-Path $Work "smapi-$smapiVersion-windows"
$runtime = Join-Path $Work "runtime-$runtimeVersion"
foreach ($path in $smapiInstaller, $smapiFiles, $runtime) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Recurse -Force
    }
}
Expand-Archive -LiteralPath $smapiZip -DestinationPath $smapiInstaller
Expand-Archive -LiteralPath $runtimeZip -DestinationPath $runtime

# the installer keeps the files it copies into a game folder in a zip named install.dat
$installDat = Get-ChildItem -LiteralPath $smapiInstaller -Recurse -Filter install.dat | Where-Object { $_.Directory.Name -eq 'windows' } | Select-Object -First 1
if (-not $installDat) {
    throw "No windows install.dat in the SMAPI $smapiVersion installer"
}
$installZip = Join-Path $Work 'smapi-install.zip'
Copy-Item -LiteralPath $installDat.FullName -Destination $installZip -Force
Expand-Archive -LiteralPath $installZip -DestinationPath $smapiFiles

$overlay = Join-Path $PackageOut 'overlay'
$bundledMods = Join-Path $PackageOut 'bundled-mods'
New-Item -ItemType Directory -Path $PackageOut, $overlay, $bundledMods | Out-Null

# hostfxr and the ucrt shims must sit at the package root, the rest of the runtime arrives with the game
$native = Join-Path $runtime 'runtimes\win-x64\native'
Copy-Item -LiteralPath (Join-Path $native 'hostfxr.dll'), (Join-Path $native 'ucrtbase.dll') -Destination $PackageOut
Copy-Item -Path (Join-Path $native 'api-ms-win-*.dll') -Destination $PackageOut

# SMAPI refuses to start unless it sits in the game folder, so the host copies the overlay over it at launch
foreach ($name in 'StardewModdingAPI.dll', 'StardewModdingAPI.runtimeconfig.json') {
    Copy-Item -LiteralPath (Join-Path $smapiFiles $name) -Destination $overlay
}
Copy-Item -LiteralPath (Join-Path $smapiFiles 'smapi-internal') -Destination $overlay -Recurse
Copy-Item -LiteralPath $MonoGameFramework -Destination (Join-Path $overlay 'MonoGame.Framework.dll')

# config.user.json survives SMAPI updates, the console has no window and the update check is not wanted
@{ ListenForConsoleInput = $false; CheckForUpdates = $false } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $overlay 'smapi-internal\config.user.json') -Encoding UTF8

Copy-Item -Path (Join-Path $smapiFiles 'Mods\*') -Destination $bundledMods -Recurse

$xboxPathsOut = Join-Path $Work 'XboxPaths'
dotnet build (Join-Path $repo 'src\mods\XboxPaths\XboxPaths.csproj') -c Release --nologo "-p:StagePath=$overlay" "-p:OutDir=$xboxPathsOut\"
if ($LASTEXITCODE -ne 0) {
    throw 'Xbox Paths build failed'
}
$xboxPaths = Join-Path $bundledMods 'XboxPaths'
New-Item -ItemType Directory -Path $xboxPaths | Out-Null
Copy-Item -LiteralPath (Join-Path $xboxPathsOut 'XboxPaths.dll'), (Join-Path $xboxPathsOut 'manifest.json') -Destination $xboxPaths

foreach ($name in 'hostfxr.dll', 'ucrtbase.dll', 'overlay\StardewModdingAPI.dll', 'overlay\MonoGame.Framework.dll', 'overlay\smapi-internal\0Harmony.dll', 'bundled-mods\XboxPaths\XboxPaths.dll', 'bundled-mods\ConsoleCommands\manifest.json') {
    if (-not (Test-Path -LiteralPath (Join-Path $PackageOut $name))) {
        throw "Package half is missing: $name"
    }
}

$files = Get-ChildItem -LiteralPath $PackageOut -Recurse -File
$size = ($files | Measure-Object -Sum Length).Sum
Write-Host "package $($files.Count) files, $([math]::Round($size / 1MB, 1)) MB, at $PackageOut"
