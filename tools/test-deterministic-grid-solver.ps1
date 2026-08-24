[CmdletBinding()]
param(
    [int]$RandomSeeds = 250
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw $Message }
}

function Get-PropertySum {
    param([System.Collections.IEnumerable]$Items, [string]$Property)
    $sum = 0.0
    foreach ($item in $Items) { $sum += [double]$item.$Property }
    $sum
}

function Format-TestNumber {
    param([double]$Value)
    $Value.ToString('F6', [Globalization.CultureInfo]::InvariantCulture)
}

function New-TestNode {
    param(
        [string]$Id,
        [ValidateSet('Source', 'Pass', 'Load')] [string]$Type,
        [double]$Capacity = 0,
        [double]$Demand = 0,
        [double]$Self = 0,
        [double]$Virtual = 0,
        [bool]$On = $true,
        [bool]$GateClosed = $false
    )
    [pscustomobject]@{
        Id = $Id; Type = $Type; Capacity = $Capacity; Demand = $Demand
        Self = $Self; Virtual = $Virtual; On = $On; GateClosed = $GateClosed
        Request = 0.0; Input = 0.0; RealOutput = 0.0
        Powered = $false; Overloaded = $false
    }
}

function New-TestEdge {
    param([string]$Source, [string]$Target, [double]$Share = 0)
    [pscustomobject]@{ Source = $Source; Target = $Target; Share = $Share; Request = 0.0; Allocation = 0.0 }
}

function Invoke-ReferenceSolve {
    param([object[]]$Nodes, [object[]]$Edges)

    $byId = @{}
    $incoming = @{}
    $outgoing = @{}
    $indegree = @{}
    foreach ($node in $Nodes) {
        $byId[$node.Id] = $node
        $incoming[$node.Id] = [System.Collections.Generic.List[object]]::new()
        $outgoing[$node.Id] = [System.Collections.Generic.List[object]]::new()
        $indegree[$node.Id] = 0
        $node.Request = 0.0; $node.Input = 0.0; $node.RealOutput = 0.0
        $node.Powered = $false; $node.Overloaded = $false
    }
    foreach ($edge in $Edges) {
        Assert-True ($byId.ContainsKey($edge.Source) -and $byId.ContainsKey($edge.Target)) "Edge references a missing node"
        $outgoing[$edge.Source].Add($edge)
        $incoming[$edge.Target].Add($edge)
        $indegree[$edge.Target]++
        $edge.Request = 0.0; $edge.Allocation = 0.0
    }

    $ready = [System.Collections.Generic.List[string]]::new()
    foreach ($id in ($indegree.Keys | Sort-Object)) { if ($indegree[$id] -eq 0) { $ready.Add($id) } }
    $order = [System.Collections.Generic.List[string]]::new()
    while ($ready.Count -gt 0) {
        $ready.Sort()
        $id = $ready[0]
        $ready.RemoveAt(0)
        $order.Add($id)
        foreach ($edge in $outgoing[$id]) {
            $indegree[$edge.Target]--
            if ($indegree[$edge.Target] -eq 0) { $ready.Add($edge.Target) }
        }
    }
    if ($order.Count -ne $Nodes.Count) { throw 'cycle detected' }

    for ($i = $order.Count - 1; $i -ge 0; $i--) {
        $node = $byId[$order[$i]]
        if ($node.Type -eq 'Load') {
            $node.Request = $node.Demand
        } elseif ($node.Type -eq 'Pass') {
            if ($node.GateClosed) {
                $node.Request = [Math]::Max(1.0, $node.Self)
            } else {
                $downstream = Get-PropertySum $outgoing[$node.Id] Request
                $node.Request = [Math]::Max(0.0, $downstream + $node.Self - $node.Virtual)
                if ($node.Capacity -gt 0) { $node.Request = [Math]::Min($node.Request, $node.Capacity) }
            }
        }

        $liveInputs = @($incoming[$node.Id])
        foreach ($edge in $liveInputs) {
            $share = $edge.Share
            if ($share -le 0) { $share = 1.0 / $liveInputs.Count }
            $edge.Request = $node.Request * $share
        }
    }

    foreach ($id in $order) {
        $node = $byId[$id]
        $node.Input = Get-PropertySum $incoming[$id] Allocation
        if ($node.Type -eq 'Source') {
            $node.Powered = $node.On
            if ($node.On) { $node.RealOutput = $node.Capacity }
        } elseif ($node.Type -eq 'Pass') {
            $effective = $node.Input + $node.Virtual
            if ($effective -ge $node.Self -and $effective -gt 0) {
                $node.Powered = $true
                $node.RealOutput = [Math]::Max(0.0, $effective - $node.Self)
                if ($node.Capacity -gt 0) { $node.RealOutput = [Math]::Min($node.RealOutput, $node.Capacity) }
            }
        } else {
            $node.Powered = $node.Input + 0.0001 -ge $node.Demand
        }

        if ($node.Type -ne 'Load') {
            $available = $node.RealOutput
            if ($node.GateClosed) { $available = 0.0 }
            $requested = Get-PropertySum $outgoing[$id] Request
            $node.Overloaded = $requested -gt $available + 0.0001
            foreach ($edge in $outgoing[$id]) {
                if (-not $node.Overloaded) { $edge.Allocation = $edge.Request }
            }
            if ($node.RealOutput -le 0 -or $node.GateClosed) { $node.Overloaded = $false }
        }
    }

    foreach ($edge in $Edges) {
        Assert-True ($edge.Allocation -ge 0) "Negative allocation on $($edge.Source)->$($edge.Target)"
        Assert-True ($edge.Allocation -le $edge.Request + 0.0001) "Allocation exceeds frozen demand"
    }
    foreach ($node in $Nodes) {
        if ($node.Type -ne 'Load') {
            $allocated = Get-PropertySum $outgoing[$node.Id] Allocation
            $limit = if ($node.GateClosed) { 0.0 } else { $node.RealOutput }
            Assert-True ($allocated -le $limit + 0.0001) "Producer capacity exceeded at $($node.Id)"
        } else {
            Assert-True ($node.Powered -eq ($node.Input + 0.0001 -ge $node.Demand)) "Load power invariant failed at $($node.Id)"
        }
    }

    [pscustomobject]@{
        Order = @($order)
        Nodes = @($Nodes | Sort-Object Id | ForEach-Object { "$($_.Id):$(Format-TestNumber $_.Request):$(Format-TestNumber $_.Input):$(Format-TestNumber $_.RealOutput):$($_.Powered):$($_.Overloaded)" })
        Edges = @($Edges | Sort-Object Source, Target | ForEach-Object { "$($_.Source)>$($_.Target):$(Format-TestNumber $_.Request):$(Format-TestNumber $_.Allocation)" })
    }
}

function Assert-Repeatable {
    param([object[]]$Nodes, [object[]]$Edges, [string]$Name)
    $firstResult = Invoke-ReferenceSolve $Nodes $Edges
    $first = [pscustomobject]@{ Nodes = $firstResult.Nodes; Edges = $firstResult.Edges } | ConvertTo-Json -Compress -Depth 5
    $secondResult = Invoke-ReferenceSolve $Nodes $Edges
    $second = [pscustomobject]@{ Nodes = $secondResult.Nodes; Edges = $secondResult.Edges } | ConvertTo-Json -Compress -Depth 5
    Assert-True ($first -eq $second) "Scenario '$Name' changed on a second solve"

    # A legal topology must settle identically regardless of persistence or
    # registry insertion order. Reverse both collections to exercise that path.
    [object[]]$reorderedNodes = @($Nodes)
    [object[]]$reorderedEdges = @($Edges)
    [array]::Reverse($reorderedNodes)
    [array]::Reverse($reorderedEdges)
    $reorderedResult = Invoke-ReferenceSolve $reorderedNodes $reorderedEdges
    $reordered = [pscustomobject]@{ Nodes = $reorderedResult.Nodes; Edges = $reorderedResult.Edges } | ConvertTo-Json -Compress -Depth 5
    Assert-True ($first -eq $reordered) "Scenario '$Name' depends on insertion order"
}

# Explicit shapes: chain, fan-out overload, multi-source combine, incident-like
# shared-supply overload, reconvergent diamond, nested split/combine, and gate.
Assert-Repeatable @(
    (New-TestNode S Source 50), (New-TestNode P Pass 100), (New-TestNode L Load -Demand 20)
) @((New-TestEdge S P), (New-TestEdge P L)) 'chain'

$fanNodes = @((New-TestNode S Source 25), (New-TestNode P Pass 100), (New-TestNode A Load -Demand 20), (New-TestNode B Load -Demand 20))
$fanEdges = @((New-TestEdge S P), (New-TestEdge P A), (New-TestEdge P B))
$fan = Invoke-ReferenceSolve $fanNodes $fanEdges
Assert-True (($fanNodes | Where-Object Id -eq S).Overloaded) 'Fan-out source was not stable-overloaded'
Assert-Repeatable $fanNodes $fanEdges 'fan-out overload'

Assert-Repeatable @(
    (New-TestNode S1 Source 15), (New-TestNode S2 Source 15), (New-TestNode C Pass 100), (New-TestNode L Load -Demand 30)
) @((New-TestEdge S1 C 0.5), (New-TestEdge S2 C 0.5), (New-TestEdge C L)) 'two-source combine'

$incidentNodes = @(
    (New-TestNode S1 Source 20), (New-TestNode S2 Source 15), (New-TestNode C Pass 100),
    (New-TestNode Extra Load -Demand 20), (New-TestNode L Load -Demand 30)
)
$incidentEdges = @((New-TestEdge S1 C 0.5), (New-TestEdge S2 C 0.5), (New-TestEdge S1 Extra), (New-TestEdge C L))
$incident = Invoke-ReferenceSolve $incidentNodes $incidentEdges
Assert-True (($incidentNodes | Where-Object Id -eq C).Overloaded) 'Incident-like combiner did not settle overloaded'
Assert-True (-not ($incidentNodes | Where-Object Id -eq L).Powered) 'Incident-like downstream load received partial power'
Assert-Repeatable $incidentNodes $incidentEdges 'incident-like shared supply'

Assert-Repeatable @(
    (New-TestNode S Source 40), (New-TestNode P Pass 100), (New-TestNode A Pass 100),
    (New-TestNode B Pass 100), (New-TestNode C Pass 100), (New-TestNode L Load -Demand 30)
) @(
    (New-TestEdge S P), (New-TestEdge P A), (New-TestEdge P B),
    (New-TestEdge A C 0.5), (New-TestEdge B C 0.5), (New-TestEdge C L)
) 'reconvergent diamond'

Assert-Repeatable @(
    (New-TestNode S1 Source 60), (New-TestNode S2 Source 30), (New-TestNode A Pass 100),
    (New-TestNode B Pass 100), (New-TestNode C Pass 100), (New-TestNode D Pass 100),
    (New-TestNode L1 Load -Demand 25), (New-TestNode L2 Load -Demand 25)
) @(
    (New-TestEdge S1 A), (New-TestEdge A B), (New-TestEdge S2 B),
    (New-TestEdge B C), (New-TestEdge C D), (New-TestEdge D L1), (New-TestEdge D L2)
) 'nested split-combine-split'

Assert-Repeatable @(
    (New-TestNode S Source 50), (New-TestNode G Pass 100 -GateClosed $true), (New-TestNode L Load -Demand 20)
) @((New-TestEdge S G), (New-TestEdge G L)) 'closed gate'

Assert-Repeatable @(
    (New-TestNode S Source 40), (New-TestNode P Pass 100 -Self 5), (New-TestNode L Load -Demand 25)
) @((New-TestEdge S P), (New-TestEdge P L)) 'self-consuming passthrough'

$batteryNodes = @(
    (New-TestNode S Source 50 -On $false), (New-TestNode B Pass 30 -Virtual 20), (New-TestNode L Load -Demand 15)
)
$batteryEdges = @((New-TestEdge S B), (New-TestEdge B L))
Invoke-ReferenceSolve $batteryNodes $batteryEdges | Out-Null
Assert-True (($batteryNodes | Where-Object Id -eq L).Powered) 'Virtual generation did not supply its load'
Assert-Repeatable $batteryNodes $batteryEdges 'virtual-generation passthrough'

$limitedNodes = @(
    (New-TestNode S Source 100), (New-TestNode P Pass 20),
    (New-TestNode A Load -Demand 15), (New-TestNode B Load -Demand 15)
)
$limitedEdges = @((New-TestEdge S P), (New-TestEdge P A), (New-TestEdge P B))
Invoke-ReferenceSolve $limitedNodes $limitedEdges | Out-Null
Assert-True (($limitedNodes | Where-Object Id -eq P).Overloaded) 'Throughput-limited passthrough did not settle overloaded'
Assert-Repeatable $limitedNodes $limitedEdges 'passthrough throughput limit'

$offNodes = @((New-TestNode S Source 50 -On $false), (New-TestNode P Pass 100), (New-TestNode L Load -Demand 20))
$offEdges = @((New-TestEdge S P), (New-TestEdge P L))
Invoke-ReferenceSolve $offNodes $offEdges | Out-Null
Assert-True (-not ($offNodes | Where-Object Id -eq L).Powered) 'An off source delivered power'
Assert-Repeatable $offNodes $offEdges 'source off'

$removedNodes = @((New-TestNode S Source 50), (New-TestNode P Pass 100), (New-TestNode L Load -Demand 20))
$removedEdges = @((New-TestEdge S P))
Invoke-ReferenceSolve $removedNodes $removedEdges | Out-Null
Assert-True (-not ($removedNodes | Where-Object Id -eq L).Powered) 'A disconnected load retained power'
Assert-Repeatable $removedNodes $removedEdges 'wire removal'

$cycleRejected = $false
try {
    Invoke-ReferenceSolve @((New-TestNode A Pass 100), (New-TestNode B Pass 100)) @((New-TestEdge A B), (New-TestEdge B A)) | Out-Null
} catch {
    $cycleRejected = $_.Exception.Message -eq 'cycle detected'
}
Assert-True $cycleRejected 'A cycle did not take the bounded rejection path'

# Generated legal DAGs exercise arbitrary branch/reconvergence shapes and prove
# the two passes remain bounded and idempotent across insertion order.
for ($seed = 1; $seed -le $RandomSeeds; $seed++) {
    $rng = [Random]::new($seed)
    $count = 6 + $rng.Next(18)
    $nodes = [System.Collections.Generic.List[object]]::new()
    $edges = [System.Collections.Generic.List[object]]::new()
    $sourceCount = 1 + $rng.Next([Math]::Min(3, $count - 2))
    for ($i = 0; $i -lt $sourceCount; $i++) {
        $nodes.Add((New-TestNode "N$i" Source (20 + $rng.Next(100))))
    }
    for ($i = $sourceCount; $i -lt $count; $i++) {
        if ($i -eq $count - 1 -or $rng.NextDouble() -lt 0.30) {
            $nodes.Add((New-TestNode "N$i" Load -Demand (1 + $rng.Next(30))))
        } else {
            $nodes.Add((New-TestNode "N$i" Pass (20 + $rng.Next(100))))
        }
        $parentCount = 1 + $rng.Next([Math]::Min(3, $i))
        $parents = 0..($i - 1) | Sort-Object { $rng.Next() } | Select-Object -First $parentCount
        foreach ($parent in $parents) { $edges.Add((New-TestEdge "N$parent" "N$i")) }
    }
    Assert-Repeatable @($nodes) @($edges) "random DAG seed $seed"
}

# Production-path structural guard: normal propagation must contain exactly one
# reverse demand pass, one forward delivery pass, one validation, and one commit;
# it must not call the legacy per-node requeue solver.
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$impl = Get-Content -LiteralPath (Join-Path $root 'scripts\5_Mission\LFPG_ElecGraphImpl.c') -Raw
$start = $impl.IndexOf('override int ProcessDirtyQueue(int nodeBudget, int edgeBudget)')
$end = $impl.IndexOf('protected void SyncNodeToEntity', $start)
Assert-True ($start -ge 0 -and $end -gt $start) 'Could not isolate production ProcessDirtyQueue'
$production = $impl.Substring($start, $end - $start)
foreach ($required in @('BuildSolveTopologicalOrder', 'BuildFrozenDemand', 'BuildFrozenDelivery', 'ValidateFrozenSolution', 'CommitSolveState', 'ClearSolvedDirtyState')) {
    Assert-True ($production.Contains($required)) "Production solver is missing $required"
}
Assert-True (-not $production.Contains('LogRequeueLimit')) 'Production solver still invokes the requeue-limit path'
Assert-True (-not $impl.Contains('ProcessDirtyQueueLegacy(')) 'Legacy feedback solver is still present'

Write-Host "Deterministic grid solver checks passed: 13 explicit scenarios + $RandomSeeds generated DAGs." -ForegroundColor Green
