$port = 17383
$serverProc = Start-Process -FilePath "d:\PulseKV\build\pulsekv-server.exe" -ArgumentList "$port" -PassThru
Start-Sleep -Seconds 1

Write-Host ">>> PHASE 8: 10 CONCURRENT CLIENTS - MIXED 80/20 <<<"
& "d:\PulseKV\build\pulsekv-benchmark.exe" --port $port --workload mixed80 --requests 5000 --clients 10

Write-Host "`n>>> PHASE 8: 10 CONCURRENT CLIENTS - 100% GET <<<"
& "d:\PulseKV\build\pulsekv-benchmark.exe" --port $port --workload get --requests 5000 --clients 10

Write-Host "`n>>> PHASE 8: 10 CONCURRENT CLIENTS - 100% SET <<<"
& "d:\PulseKV\build\pulsekv-benchmark.exe" --port $port --workload set --requests 5000 --clients 10 --no-prepopulate

Stop-Process -Id $serverProc.Id -Force
