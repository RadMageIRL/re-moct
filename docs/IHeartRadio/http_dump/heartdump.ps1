# heartdump.ps1 — fire all four handshake variants at the same break
$dur = 180
$poll = 3
$variants = 'baseline','clone','emptyid','synth'

$jobs = foreach ($v in $variants) {
    Start-Process -FilePath '.\iheart_http_dump.exe' `
                  -ArgumentList $dur, $poll, $v `
                  -NoNewWindow -PassThru
}

Write-Host "launched $($jobs.Count) variants for $dur seconds - waiting..."
$jobs | Wait-Process
Write-Host "done. logs:"
Get-ChildItem ihearthttpdump_*_*.log | Sort-Object LastWriteTime | Select-Object -Last 4 -ExpandProperty Name