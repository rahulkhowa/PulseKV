# run_phase9_benchmarks.ps1
$port = 17385

function Benchmark-Version([string]$name, [string]$threads, [string]$lockMode) {
    Write-Host "`n======================================================="
    Write-Host " STARTING: $name"
    Write-Host "======================================================="
    
    $proc = Start-Process -FilePath "d:\PulseKV\build\pulsekv-server.exe" -ArgumentList "$port --threads $threads --lock $lockMode" -PassThru
    Start-Sleep -Seconds 1
    
    # Run 1 Client
    Write-Host "`n--- [$name] 1 Client (Mixed 80/20, 10,000 reqs) ---"
    & "d:\PulseKV\build\pulsekv-benchmark.exe" --port $port --workload mixed80 --requests 10000 --clients 1
    
    # Run 5 Clients
    Write-Host "`n--- [$name] 5 Clients (Mixed 80/20, 25,000 reqs) ---"
    & "d:\PulseKV\build\pulsekv-benchmark.exe" --port $port --workload mixed80 --requests 5000 --clients 5
    
    # Run 10 Clients
    Write-Host "`n--- [$name] 10 Clients (Mixed 80/20, 50,000 reqs) ---"
    & "d:\PulseKV\build\pulsekv-benchmark.exe" --port $port --workload mixed80 --requests 5000 --clients 10

    # Run 10 Clients 100% GET
    Write-Host "`n--- [$name] 10 Clients (100% GET, 50,000 reqs) ---"
    & "d:\PulseKV\build\pulsekv-benchmark.exe" --port $port --workload get --requests 5000 --clients 10

    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Milliseconds 500
}

# Version A: Single-threaded
Benchmark-Version "Version A: Single-Threaded" "1" "mutex"

# Version B: Thread Pool + Mutex
Benchmark-Version "Version B: Thread Pool + Mutex" "12" "mutex"

# Version C: Thread Pool + Shared Mutex
Benchmark-Version "Version C: Thread Pool + Shared Mutex" "12" "shared"
