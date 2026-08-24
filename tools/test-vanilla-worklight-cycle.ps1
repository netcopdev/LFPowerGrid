[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Invoke-VanillaConsumption {
    param(
        [double]$Energy,
        [double]$Capacity,
        [double]$UsagePerSecond = 0.14,
        [double]$IntervalSeconds = 50.0
    )

    $energyAfterRequest = $Energy - ($UsagePerSecond * $IntervalSeconds)
    $clampedEnergy = [Math]::Min([Math]::Max($energyAfterRequest, 0.0), $Capacity)
    $clampedAmount = $energyAfterRequest - $clampedEnergy

    [pscustomobject]@{
        Energy = $clampedEnergy
        ConsumedEnough = $clampedAmount -ge 0.0
        CanWorkNextTick = $clampedEnergy -gt 0.0
    }
}

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$deviceApiPath = Join-Path $projectRoot 'scripts\4_World\LFPG_IDevice.c'
$graphPath = Join-Path $projectRoot 'scripts\5_Mission\LFPG_ElecGraphImpl.c'
$deviceApi = Get-Content -LiteralPath $deviceApiPath -Raw
$graph = Get-Content -LiteralPath $graphPath -Raw

# Regression model: SetEnergy accepts 1000 even when Spotlight's configured
# storage maximum is zero. Its first 50-second consumption then clamps the
# remaining energy to zero, making the following update unable to work.
$oldCycle = Invoke-VanillaConsumption -Energy 1000.0 -Capacity 0.0
Assert-True ($oldCycle.Energy -eq 0.0) 'Zero-capacity regression was not reproduced'
Assert-True (-not $oldCycle.CanWorkNextTick) 'Zero-capacity Spotlight unexpectedly remained workable'

# The runtime LFPG capacity makes the same vanilla consumption retain energy.
$fixedCycle = Invoke-VanillaConsumption -Energy 1000.0 -Capacity 1000.0
Assert-True ($fixedCycle.ConsumedEnough) 'Fixed Spotlight could not consume its 50-second demand'
Assert-True ($fixedCycle.Energy -gt 0.0) 'Fixed Spotlight pool was still clamped to zero'
Assert-True $fixedCycle.CanWorkNextTick 'Fixed Spotlight cannot work on its next update'

# Exercise more than an hour of 50-second updates with LFPG's existing refill
# threshold. No timer boundary may expose zero energy.
$energy = 1000.0
for ($tick = 0; $tick -lt 80; $tick++) {
    if ($energy -lt 500.0) { $energy = 1000.0 }
    $cycle = Invoke-VanillaConsumption -Energy $energy -Capacity 1000.0
    Assert-True $cycle.CanWorkNextTick "Spotlight lost power at simulated update $tick"
    $energy = $cycle.Energy
}

# Source integration checks keep both the immediate SetPowered path and the
# periodic maintenance path from silently dropping the capacity preparation.
Assert-True ($deviceApi.Contains('static void EnsureSyntheticVanillaCapacity')) 'Capacity helper is missing'
Assert-True ($deviceApi.Contains('EnsureSyntheticVanillaCapacity(e, em);')) 'SetPowered does not prepare Spotlight capacity'
Assert-True ($deviceApi.Contains('ReleaseSyntheticVanillaCapacity(e, em);')) 'SetPowered does not restore disconnected Spotlight behavior'
Assert-True ($graph.Contains('LFPG_DeviceAPI.EnsureSyntheticVanillaCapacity(vanEnt, vanEm);')) 'Periodic maintenance does not preserve Spotlight capacity'

Write-Host 'Vanilla worklight cycle regression checks passed.' -ForegroundColor Green
