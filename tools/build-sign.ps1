[CmdletBinding()]
param(
    [string]$DayZToolsDir,
    [string]$SigningKey,
    [string]$OutputDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

if ([string]::IsNullOrWhiteSpace($DayZToolsDir)) {
    $DayZToolsDir = $env:DAYZ_TOOLS_DIR
}
if ([string]::IsNullOrWhiteSpace($DayZToolsDir)) {
    $DayZToolsDir = 'E:\SteamLibrary\steamapps\common\DayZ Tools'
}

if ([string]::IsNullOrWhiteSpace($SigningKey)) {
    $SigningKey = $env:LFPG_SIGNING_KEY
}
if ([string]::IsNullOrWhiteSpace($SigningKey)) {
    $SigningKey = 'E:\DayZDev\SigningKeys\LFPowerGrid_Test.biprivatekey'
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $projectRoot 'dist\build'
}

$DayZToolsDir = [System.IO.Path]::GetFullPath($DayZToolsDir)
$SigningKey = [System.IO.Path]::GetFullPath($SigningKey)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)

$addonBuilder = Join-Path $DayZToolsDir 'Bin\AddonBuilder\AddonBuilder.exe'
$includeList = Join-Path $projectRoot 'include.lst'
$publicKey = [System.IO.Path]::ChangeExtension($SigningKey, '.bikey')
$distRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot 'dist'))
$distPrefix = $distRoot + [System.IO.Path]::DirectorySeparatorChar

if (-not (Test-Path -LiteralPath $addonBuilder -PathType Leaf)) {
    throw "DayZ Addon Builder was not found at '$addonBuilder'. Set DAYZ_TOOLS_DIR or pass -DayZToolsDir."
}
if (-not (Test-Path -LiteralPath $SigningKey -PathType Leaf)) {
    throw "Signing key was not found at '$SigningKey'. Set LFPG_SIGNING_KEY or pass -SigningKey."
}
if (-not (Test-Path -LiteralPath $publicKey -PathType Leaf)) {
    throw "Public key matching the signing key was not found at '$publicKey'."
}
if (-not (Test-Path -LiteralPath $includeList -PathType Leaf)) {
    throw "Addon Builder include list was not found at '$includeList'."
}
if (-not $OutputDir.StartsWith($distPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDir must be a child of '$distRoot'. Refusing '$OutputDir'."
}
if ($OutputDir.Equals($distRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDir cannot be the dist root itself.'
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("LFPowerGrid-build-" + $PID)
$builderTemp = Join-Path $tempRoot 'binarized'
$builderOutput = Join-Path $tempRoot 'output'
$modRoot = Join-Path $OutputDir '@LFPowerGrid_Test'
$addonsDir = Join-Path $modRoot 'addons'
$keysDir = Join-Path $modRoot 'keys'

try {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $builderTemp, $builderOutput -Force | Out-Null

    Write-Host 'Building and signing LFPowerGrid...' -ForegroundColor Cyan
    Write-Host "  Source:  $projectRoot"
    Write-Host "  Tools:   $DayZToolsDir"
    Write-Host "  Key:     $SigningKey"
    Write-Host "  Output:  $OutputDir"

    $builderArgs = @(
        $projectRoot,
        $builderOutput,
        '-clear',
        "-temp=$builderTemp",
        '-prefix=LFPowerGrid',
        "-project=$([System.IO.Path]::GetDirectoryName($projectRoot))",
        "-include=$includeList",
        "-sign=$SigningKey",
        "-toolsDirectory=$DayZToolsDir",
        '-binarizeFullLogs'
    )

    & $addonBuilder @builderArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Addon Builder failed with exit code $LASTEXITCODE."
    }

    $builtPbo = Join-Path $builderOutput 'LFPowerGrid.pbo'
    if (-not (Test-Path -LiteralPath $builtPbo -PathType Leaf)) {
        throw "Addon Builder reported success but did not create '$builtPbo'."
    }

    $builtSignature = Get-ChildItem -LiteralPath $builderOutput -Filter 'LFPowerGrid.pbo.*.bisign' -File | Select-Object -First 1
    if (-not $builtSignature) {
        throw 'Addon Builder created the PBO but no BI signature. Check the private key and DSSignFile installation.'
    }

    if (Test-Path -LiteralPath $OutputDir) {
        Remove-Item -LiteralPath $OutputDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $addonsDir, $keysDir -Force | Out-Null

    Copy-Item -LiteralPath $builtPbo -Destination $addonsDir
    Copy-Item -LiteralPath $builtSignature.FullName -Destination $addonsDir
    Copy-Item -LiteralPath $publicKey -Destination $keysDir

    $commit = 'unknown'
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $resolvedCommit = & git -C $projectRoot rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $resolvedCommit) {
            $commit = $resolvedCommit.Trim()
        }
    }

    $manifest = @(
        'LFPowerGrid signed build',
        '',
        "Source revision: $commit (working tree)",
        "Built:  $([DateTimeOffset]::Now.ToString('yyyy-MM-dd HH:mm:ss zzz'))",
        "PBO:    $($builtPbo | Split-Path -Leaf)",
        "Key:    $([System.IO.Path]::GetFileName($publicKey))"
    )
    Set-Content -LiteralPath (Join-Path $OutputDir 'BUILD_INFO.txt') -Value $manifest -Encoding utf8

    Write-Host ''
    Write-Host 'Build and signature completed successfully.' -ForegroundColor Green
    Write-Host "Ready-to-deploy mod: $modRoot" -ForegroundColor Green
}
finally {
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
}
