param(
    [Parameter(Mandatory = $true)]
    [string]$ResultPath
)

$ErrorActionPreference = 'Stop'

function Write-Frame([System.IO.Stream]$Stream, [string]$Json) {
    $body = [System.Text.Encoding]::UTF8.GetBytes($Json)
    $size = [System.BitConverter]::GetBytes([uint32]$body.Length)
    $Stream.Write($size, 0, $size.Length)
    $Stream.Write($body, 0, $body.Length)
    $Stream.Flush()
}

function Read-Exact([System.IO.Stream]$Stream, [int]$Count) {
    $buffer = [byte[]]::new($Count)
    $offset = 0
    while ($offset -lt $Count) {
        $read = $Stream.Read($buffer, $offset, $Count - $offset)
        if ($read -le 0) {
            throw 'Pipe closed before a complete frame was read.'
        }
        $offset += $read
    }
    return $buffer
}

function Read-Frame([System.IO.Stream]$Stream) {
    $sizeBytes = Read-Exact $Stream 4
    $size = [System.BitConverter]::ToUInt32($sizeBytes, 0)
    if ($size -eq 0 -or $size -gt 1MB) {
        throw "Invalid frame size: $size"
    }
    $body = Read-Exact $Stream ([int]$size)
    return [System.Text.Encoding]::UTF8.GetString($body) | ConvertFrom-Json
}

function Read-ExactWithTimeout([System.IO.Stream]$Stream, [int]$Count, [int]$TimeoutMilliseconds) {
    $buffer = [byte[]]::new($Count)
    $offset = 0
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMilliseconds)
    while ($offset -lt $Count) {
        $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        if ($remaining -le 1 -and [DateTime]::UtcNow -ge $deadline) {
            return $null
        }
        $cancellation = [System.Threading.CancellationTokenSource]::new($remaining)
        try {
            $task = $Stream.ReadAsync($buffer, $offset, $Count - $offset, $cancellation.Token)
            $read = $task.GetAwaiter().GetResult()
        }
        catch [System.OperationCanceledException] {
            return $null
        }
        finally {
            $cancellation.Dispose()
        }
        if ($read -le 0) {
            throw 'Pipe closed before a complete frame was read.'
        }
        $offset += $read
    }
    return $buffer
}

function Read-FrameWithTimeout([System.IO.Stream]$Stream, [int]$TimeoutMilliseconds) {
    $prefix = Read-ExactWithTimeout $Stream 4 $TimeoutMilliseconds
    if ($null -eq $prefix) {
        return $null
    }
    $size = [System.BitConverter]::ToUInt32($prefix, 0)
    if ($size -eq 0 -or $size -gt 1MB) {
        throw "Invalid frame size: $size"
    }
    $body = Read-ExactWithTimeout $Stream ([int]$size) $TimeoutMilliseconds
    if ($null -eq $body) {
        throw 'Timed out while reading an event body.'
    }
    return [System.Text.Encoding]::UTF8.GetString($body) | ConvertFrom-Json
}

function Invoke-SearchRequest([hashtable]$Request) {
    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', 'FileSearch',
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)
    try {
        $pipe.Connect(3000)
        Write-Frame $pipe ($Request | ConvertTo-Json -Compress)
        return Read-Frame $pipe
    }
    finally {
        $pipe.Dispose()
    }
}

function Wait-Ready([int]$TimeoutSeconds) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        try {
            $status = Invoke-SearchRequest @{ command = 'status' }
            if ($status.ready -and $status.search_running) {
                return $status
            }
        }
        catch {
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'FileSearch did not become ready before the timeout.'
}

$eventsPipe = $null
$filteredPipe = $null
$testDirectory = Join-Path $PSScriptRoot 'event-e2e'
$token = 'fsevent_' + [Guid]::NewGuid().ToString('N')
$createdPath = Join-Path $testDirectory ($token + '_created.txt')
$renamedPath = Join-Path $testDirectory ($token + '_renamed.txt')

try {
    New-Item -ItemType Directory -Force -Path $testDirectory | Out-Null
    $status = Wait-Ready 180

    $eventsPipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', 'FileSearch.Events',
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::Asynchronous)
    $eventsPipe.Connect(5000)
    Write-Frame $eventsPipe (@{ command = 'subscribe_changes'; version = 1 } | ConvertTo-Json -Compress)
    $subscribed = Read-Frame $eventsPipe
    if ($subscribed.type -ne 'subscribed') {
        throw "Unexpected subscription response: $($subscribed | ConvertTo-Json -Compress)"
    }
    $filteredPipe = [System.IO.Pipes.NamedPipeClientStream]::new('.', 'FileSearch.Events',
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::Asynchronous)
    $filteredPipe.Connect(5000)
    Write-Frame $filteredPipe (@{ command = 'subscribe_changes'; version = 1;
        scope_path = $testDirectory; actions = @('rename'); include_directories = $false } | ConvertTo-Json -Compress)
    if ((Read-Frame $filteredPipe).type -ne 'subscribed') { throw 'Filtered subscription rejected.' }
    $directoryBefore = Join-Path $testDirectory ($token + '_directory_before')
    $directoryAfter = Join-Path $testDirectory ($token + '_directory_after')
    New-Item -ItemType Directory -Path $directoryBefore | Out-Null
    Move-Item -LiteralPath $directoryBefore -Destination $directoryAfter
    Remove-Item -LiteralPath $directoryAfter

    [System.IO.File]::WriteAllText($createdPath, 'created')
    Move-Item -LiteralPath $createdPath -Destination $renamedPath
    [System.IO.File]::AppendAllText($renamedPath, '-modified')

    $searchDeadline = [DateTime]::UtcNow.AddSeconds(30)
    $searchFound = $false
    do {
        $response = Invoke-SearchRequest @{ query = ($token + '_renamed'); max_results = 20 }
        $searchFound = @($response.results | Where-Object { $_.path -like "*$token*renamed.txt" }).Count -gt 0
        if (-not $searchFound) {
            Start-Sleep -Milliseconds 250
        }
    } while (-not $searchFound -and [DateTime]::UtcNow -lt $searchDeadline)
    if (-not $searchFound) {
        throw 'Renamed file did not become searchable.'
    }

    Remove-Item -LiteralPath $renamedPath

    $events = [System.Collections.Generic.List[object]]::new()
    $eventDeadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $eventDeadline) {
        $event = Read-FrameWithTimeout $eventsPipe 1000
        if ($null -eq $event) {
            continue
        }
        $events.Add($event)
        $matching = @($events | Where-Object {
            $_.type -eq 'change' -and
            (($_.name -like "$token*") -or ($_.old_name -like "$token*"))
        })
        $actions = @($matching | ForEach-Object { $_.action } | Select-Object -Unique)
        if ($actions -contains 'create' -and
            $actions -contains 'rename' -and
            $actions -contains 'modify' -and
            $actions -contains 'delete') {
            break
        }
    }

    $matching = @($events | Where-Object {
        $_.type -eq 'change' -and
        (($_.name -like "$token*") -or ($_.old_name -like "$token*"))
    })
    $actions = @($matching | ForEach-Object { $_.action } | Select-Object -Unique)
    foreach ($required in @('create', 'rename', 'modify', 'delete')) {
        if ($actions -notcontains $required) {
            throw "Did not receive required '$required' event. Received: $($actions -join ', ')"
        }
    }
    $filtered = Read-FrameWithTimeout $filteredPipe 5000
    if ($null -eq $filtered -or $filtered.action -ne 'rename' -or
        $filtered.is_directory -or $filtered.path -ne $renamedPath -or $filtered.old_path -ne $createdPath) {
        throw "Incorrect action/type filter: $($filtered | ConvertTo-Json -Compress)"
    }
    $same = @($matching | Where-Object { $_.action -eq 'rename' -and $_.path -eq $renamedPath })
    if ($same.Count -eq 0 -or $same[0].position.epoch -ne $filtered.position.epoch -or
        $same[0].position.sequence -ne $filtered.position.sequence) { throw 'Subscribers did not receive the same event.' }

    $deleteDeadline = [DateTime]::UtcNow.AddSeconds(30)
    $searchRemoved = $false
    do {
        $response = Invoke-SearchRequest @{ query = ($token + '_renamed'); max_results = 20 }
        $searchRemoved = @($response.results | Where-Object { $_.path -like "*$token*renamed.txt" }).Count -eq 0
        if (-not $searchRemoved) {
            Start-Sleep -Milliseconds 250
        }
    } while (-not $searchRemoved -and [DateTime]::UtcNow -lt $deleteDeadline)
    if (-not $searchRemoved) {
        throw 'Deleted file remained searchable.'
    }

    & node (Join-Path $PSScriptRoot 'node_service_client_e2e.js') --windows-live $testDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Node service client end-to-end test failed.' }
    $success = @{
        ok = $true
        status = $status
        actions = $actions
        events = $matching
        renamed_search_found = $searchFound
        deleted_search_removed = $searchRemoved
        filtered_rename = $filtered
        node_client_verified = $true
    } | ConvertTo-Json -Depth 12
    [System.IO.File]::WriteAllText($ResultPath, $success)
}
catch {
    $failure = @{
        ok = $false
        error = $_.Exception.Message
        stack = $_.ScriptStackTrace
        all_events = @($events)
        matching_events = @($matching)
    } | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($ResultPath, $failure)
    throw
}
finally {
    if ($filteredPipe) { $filteredPipe.Dispose() }
    if ($eventsPipe) {
        $eventsPipe.Dispose()
    }
    try {
        Invoke-SearchRequest @{ command = 'shutdown' } | Out-Null
    }
    catch {
    }
    Remove-Item -LiteralPath $createdPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $renamedPath -Force -ErrorAction SilentlyContinue
}
