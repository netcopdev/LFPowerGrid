$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert-True {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) {
        throw "FAIL: $Message"
    }
}

function New-EdgeList {
    param([object[]]$Pairs)
    $result = [System.Collections.Generic.List[object]]::new()
    foreach ($pair in $Pairs) {
        $result.Add([pscustomobject]@{ Source = [string]$pair[0]; Target = [string]$pair[1] })
    }
    # Unary comma prevents PowerShell from unrolling the generic list into an
    # object array; the sanitizer must mutate the same list instance.
    return ,$result
}

function Test-DirectedPath {
    param(
        [System.Collections.Generic.List[object]]$Edges,
        [string]$Start,
        [string]$Target,
        [int]$ExcludedIndex
    )

    if ($Start -eq $Target) { return $true }

    $stack = [System.Collections.Generic.Stack[string]]::new()
    $visited = [System.Collections.Generic.HashSet[string]]::new()
    $stack.Push($Start)

    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        if (-not $visited.Add($current)) { continue }
        if ($current -eq $Target) { return $true }

        for ($i = 0; $i -lt $Edges.Count; $i++) {
            if ($i -eq $ExcludedIndex) { continue }
            $edge = $Edges[$i]
            if ($edge.Source -eq $current -and -not $visited.Contains($edge.Target)) {
                $stack.Push($edge.Target)
            }
        }
    }
    return $false
}

function Remove-CycleEdges {
    param([System.Collections.Generic.List[object]]$Edges)

    $removed = 0
    $changed = $true
    while ($changed) {
        $changed = $false
        for ($i = 0; $i -lt $Edges.Count; $i++) {
            $edge = $Edges[$i]
            if (Test-DirectedPath -Edges $Edges -Start $edge.Target -Target $edge.Source -ExcludedIndex $i) {
                $Edges.RemoveAt($i)
                $removed++
                $changed = $true
                break
            }
        }
    }
    return $removed
}

function Assert-Topology {
    param([string]$Name, [object[]]$Pairs, [int]$ExpectedRemoved)
    $edges = New-EdgeList -Pairs $Pairs
    $removed = Remove-CycleEdges -Edges $edges
    Assert-True ($removed -eq $ExpectedRemoved) "$Name removed $removed edge(s), expected $ExpectedRemoved"
    Assert-True ((Remove-CycleEdges -Edges $edges) -eq 0) "$Name did not converge to a DAG"
}

# Legal electrical shapes: none may be mistaken for a cycle.
Assert-Topology 'linear chain' @(@('source','gate'), @('gate','load')) 0
Assert-Topology 'split branch' @(@('source','splitter'), @('splitter','loadA'), @('splitter','loadB')) 0
Assert-Topology 'reconvergent diamond' @(@('source','left'), @('source','right'), @('left','combiner'), @('right','combiner')) 0
Assert-Topology 'multi-source combiner' @(@('generator','combiner'), @('battery','combiner'), @('combiner','load')) 0
Assert-Topology 'split-combine-split' @(@('generator','split1'), @('split1','left'), @('split1','right'), @('left','combine'), @('right','combine'), @('combine','split2'), @('split2','loadA'), @('split2','loadB')) 0
Assert-Topology 'parallel forward wires' @(@('source','load'), @('source','load')) 0

# Invalid/corrupt directed loops: remove the minimum encountered closing edge
# and always converge, including batteries and self-loops.
Assert-Topology 'two-node loop' @(@('battery','combiner'), @('combiner','battery')) 1
Assert-Topology 'battery branch loop' @(@('generator','battery'), @('battery','splitter'), @('splitter','combiner'), @('combiner','battery'), @('splitter','load')) 1
Assert-Topology 'self loop' @(@('switch','switch'), @('switch','load')) 1
Assert-Topology 'two independent loops' @(@('a','b'), @('b','a'), @('c','d'), @('d','e'), @('e','c')) 2

# Model the EEInit -> OnStoreLoad registry transition.  One entity must own one
# authoritative key, and a correlational alias must resolve to that key instead
# of creating another graph identity.
$byId = @{}
$byEntity = @{}
function Register-Model {
    param([string]$Entity, [string]$RequestedId, [string]$AuthoritativeId)
    $id = if ($AuthoritativeId) { $AuthoritativeId } else { $RequestedId }
    if ($byEntity.ContainsKey($Entity) -and $byEntity[$Entity] -ne $id) {
        $byId.Remove($byEntity[$Entity])
    }
    $byId[$id] = $Entity
    $byEntity[$Entity] = $id
}

Register-Model 'battery-object' 'temporary-id' 'temporary-id'
Register-Model 'battery-object' 'persisted-id' 'persisted-id'
Register-Model 'battery-object' 'stale-rpc-alias' 'persisted-id'
Assert-True ($byId.Count -eq 1) 'registry retained more than one ID for one entity'
Assert-True ($byId.ContainsKey('persisted-id')) 'registry lost the authoritative persisted ID'
Assert-True (-not $byId.ContainsKey('temporary-id')) 'registry retained the EEInit temporary alias'
Assert-True (-not $byId.ContainsKey('stale-rpc-alias')) 'registry accepted a correlational alias'

# Tie the model checks to the shipped implementation so accidental removal of
# either boundary defense fails CI.
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$registrySource = Get-Content -LiteralPath (Join-Path $root 'scripts\4_World\LFPG_DeviceRegistry.c') -Raw
$deviceSource = Get-Content -LiteralPath (Join-Path $root 'scripts\4_World\lfpg_devicebase.c') -Raw
$graphSource = Get-Content -LiteralPath (Join-Path $root 'scripts\5_Mission\LFPG_ElecGraphImpl.c') -Raw
Assert-True ($registrySource.Contains('m_IdByEntity')) 'registry reverse ownership is missing'
Assert-True ($deviceSource.Contains('preLoadDeviceId')) 'persistence transition cleanup is missing'
Assert-True ($graphSource.Contains('SanitizeCompletedGraphCycles')) 'completed graph DAG audit is missing'
Assert-True ($graphSource.Contains('FindDirectedPathExcludingEdge')) 'independent cycle path search is missing'

Write-Host 'PASS: registry identity and graph topology invariants'
