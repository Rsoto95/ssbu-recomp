# Memory sampler for the Test_*_mem.bat runs: waits for ssbu_native.exe, then every 2 s writes
# seconds, private MB, working set MB, free RAM MB, free commit MB to the CSV until the game exits.
param([string]$Csv)
"seconds,private_mb,working_set_mb,free_ram_mb,free_commit_mb" | Out-File -Encoding ascii $Csv
$g = $null
for ($i = 0; $i -lt 600 -and -not $g; $i++) { Start-Sleep -Milliseconds 500; $g = Get-Process -Name ssbu_native -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $g) { exit }
$t0 = Get-Date
while (-not $g.HasExited) {
    $g.Refresh()
    $os = Get-CimInstance Win32_OperatingSystem
    (@([int]((Get-Date) - $t0).TotalSeconds, [int]($g.PrivateMemorySize64 / 1MB), [int]($g.WorkingSet64 / 1MB), [int]($os.FreePhysicalMemory / 1KB), [int]($os.FreeVirtualMemory / 1KB)) -join ',') | Out-File -Append -Encoding ascii $Csv
    Start-Sleep -Seconds 2
}
