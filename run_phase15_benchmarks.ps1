# run_phase15_benchmarks.ps1
# Phase 15 Fair Benchmarking Suite

$port = 17390
$serverExe = "d:\PulseKV\build\pulsekv-server.exe"
$benchExe = "d:\PulseKV\build\pulsekv-benchmark.exe"

function Run-Suite {
    Write-Host "`n======================================================="
    Write-Host " RUNNING PHASE 15 BENCHMARKS"
    Write-Host "======================================================="

    # 1. Pure In-Memory Baseline (ThreadPool 12 threads, Mutex)
    Write-Host "`n>>> [SECTION 1: Pure In-Memory Server]"
    $srvProc = Start-Process -FilePath $serverExe -ArgumentList "$port --threads 12 --lock mutex" -PassThru
    Start-Sleep -Seconds 1

    Write-Host "`n--- In-Memory: 100% GET (10 Clients, 50,000 reqs, Uniform) ---"
    & $benchExe --port $port --workload get --requests 5000 --clients 10 --dist uniform --warmup 500

    Write-Host "`n--- In-Memory: 100% GET (10 Clients, 50,000 reqs, Zipfian) ---"
    & $benchExe --port $port --workload get --requests 5000 --clients 10 --dist zipfian --warmup 500

    Write-Host "`n--- In-Memory: 100% SET (10 Clients, 50,000 reqs, Uniform) ---"
    & $benchExe --port $port --workload set --requests 5000 --clients 10 --dist uniform --warmup 500

    Write-Host "`n--- In-Memory: Mixed 90/10 (10 Clients, 50,000 reqs, Uniform) ---"
    & $benchExe --port $port --workload mixed90 --requests 5000 --clients 10 --dist uniform --warmup 500

    Write-Host "`n--- In-Memory: Mixed 80/20 (10 Clients, 50,000 reqs, Uniform) ---"
    & $benchExe --port $port --workload mixed80 --requests 5000 --clients 10 --dist uniform --warmup 500

    Write-Host "`n--- In-Memory: Mixed 50/50 (10 Clients, 50,000 reqs, Uniform) ---"
    & $benchExe --port $port --workload mixed50 --requests 5000 --clients 10 --dist uniform --warmup 500

    # Concurrency Scaling: 1, 5, 10, 20 clients (Mixed 80/20)
    Write-Host "`n--- Concurrency Scaling: 1 Client ---"
    & $benchExe --port $port --workload mixed80 --requests 10000 --clients 1 --warmup 500

    Write-Host "`n--- Concurrency Scaling: 5 Clients ---"
    & $benchExe --port $port --workload mixed80 --requests 5000 --clients 5 --warmup 500

    Write-Host "`n--- Concurrency Scaling: 20 Clients ---"
    & $benchExe --port $port --workload mixed80 --requests 2500 --clients 20 --warmup 500

    Stop-Process -Id $srvProc.Id -Force
    Start-Sleep -Milliseconds 500

    # 2. Durability Cost Benchmark (With WAL enabled)
    Write-Host "`n>>> [SECTION 2: In-Memory + WAL Persistence]"
    $walFile = "d:\PulseKV\build\bench_wal.log"
    if (Test-Path $walFile) { Remove-Item $walFile -Force }

    $srvWalProc = Start-Process -FilePath $serverExe -ArgumentList "$port --threads 12 --lock mutex --wal $walFile" -PassThru
    Start-Sleep -Seconds 1

    Write-Host "`n--- WAL Enabled: 100% GET (10 Clients, 50,000 reqs) ---"
    & $benchExe --port $port --workload get --requests 5000 --clients 10 --dist uniform --warmup 500

    Write-Host "`n--- WAL Enabled: Mixed 80/20 (10 Clients, 50,000 reqs) ---"
    & $benchExe --port $port --workload mixed80 --requests 5000 --clients 10 --dist uniform --warmup 500

    Write-Host "`n--- WAL Enabled: 100% SET (10 Clients, 50,000 reqs) ---"
    & $benchExe --port $port --workload set --requests 5000 --clients 10 --dist uniform --warmup 500

    Stop-Process -Id $srvWalProc.Id -Force
    if (Test-Path $walFile) { Remove-Item $walFile -Force }
}

Run-Suite
