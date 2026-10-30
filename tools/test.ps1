# Install a built Windows archive with install.ps1, then solve cases with each formulation.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$t = Join-Path ([System.IO.Path]::GetTempPath()) ("empower-test-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $t | Out-Null
Set-Location $t
# Run as on a machine without Julia: the build job has Julia on PATH, which would
# hide a runtime DLL that the archive fails to provide.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notmatch 'julia' }) -join ';'
$env:EMPOWER_DIR = Join-Path $t 'empower'
$env:EMPOWER_ARCHIVE = Join-Path $root 'empower-windows-x86_64.tar.gz'
$env:EMPOWER_YES = '1'
$env:EMPOWER_ADD_TO_PATH = 'no'
# CI has no HSL licence: install without HSL and ask for MUMPS explicitly.
$env:EMPOWER_SKIP_HSL = '1'
& (Join-Path $root 'install.ps1')
$op = Join-Path $env:EMPOWER_DIR 'bin\empower.exe'
& $op case14_ieee case118_ieee --form=polar '--options=tol=1e-8 max_iter=500 linear_solver=mumps' --output=polar
# Two worker processes, --quiet, and a time limit.
& $op case14_ieee case118_ieee --form=rect --options=linear_solver=mumps --output=rect -j 2 --quiet --time-limit=600
& $op case14_ieee case118_ieee --form=dc --options=linear_solver=mumps --output=dc

$bad = 0
# Without HSL and without an explicit linear solver, empower uses MUMPS and says how to get MA27.
$nohsl = & $op case14_ieee --output=nohsl 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or $nohsl -notmatch 'hsl\.rl\.ac\.uk/download/MA27' -or $nohsl -notmatch 'empower hsl') { Write-Host 'no MUMPS run with the HSL warning'; $bad = 1 }
if (-not (Select-String -Quiet -Pattern 'linear solver mumps' -Path 'nohsl\pglib_opf_case14_ieee\ipopt.log')) { Write-Host 'the run without HSL did not use MUMPS'; $bad = 1 }
# The whole PGLib-OPF library: three sets of cases.
$dl = & $op download | Out-String
if ($LASTEXITCODE -ne 0 -or ([regex]::Matches($dl, ' cases, ')).Count -ne 3) { Write-Host "empower download failed: $dl"; $bad = 1 }
# AC references from the PGLib-OPF README; DC from PowerModels' DCPPowerModel.
$refs = @{
    'ac' = @{ 'pglib_opf_case14_ieee' = 2178.1; 'pglib_opf_case118_ieee' = 97214 }
    'dc' = @{ 'pglib_opf_case14_ieee' = 2051.5; 'pglib_opf_case118_ieee' = 93101 }
}
foreach ($f in 'polar', 'rect', 'dc') {
    $ref = $refs[$(if ($f -eq 'dc') { 'dc' } else { 'ac' })]
    Get-Content "$f\summary.csv"
    if (-not (Select-String -Quiet -Pattern '^empower: ' -Path "$f\system.txt")) { Write-Host "no system.txt in $f"; $bad = 1 }
    foreach ($row in Import-Csv "$f\summary.csv") {
        if ($row.termination_status -ne 'LOCALLY_SOLVED') { Write-Host "not solved: $($row.case) $f"; $bad = 1 }
        $r = $ref[$row.case]
        if ([math]::Abs(([double]$row.objective - $r) / $r) -gt 1e-4) { Write-Host "objective mismatch: $($row.case) $f $($row.objective)"; $bad = 1 }
        if ([double]$row.constraint_violation -gt 1e-5) { Write-Host "constraint violation: $($row.case) $f $($row.constraint_violation)"; $bad = 1 }
        foreach ($g in 'power_balance', 'power_flow', 'branch_flow', 'angle_difference', 'voltage', 'generation') {
            $v = $row."violation_$g"
            if (-not $v -or [double]$v -gt 1e-5) { Write-Host "violation of $g`: $($row.case) $f $v"; $bad = 1 }
        }
        if ((Get-Content "$f\$($row.case)\violations.csv" | Select-Object -Skip 1).Count -ne 6) { Write-Host "violations.csv: $($row.case) $f"; $bad = 1 }
        if (-not (Select-String -Quiet -Pattern 'linear solver mumps' -Path "$f\$($row.case)\ipopt.log")) { Write-Host "not mumps: $($row.case) $f"; $bad = 1 }
        if (-not (Select-String -Quiet -Pattern 'status = LOCALLY_SOLVED' -Path "$f\$($row.case)\solution.m")) { Write-Host "no solved case file: $($row.case) $f"; $bad = 1 }
        $js = Get-Content -Raw "$f\$($row.case)\solution.json" | ConvertFrom-Json
        if ($js.termination_status -ne 'LOCALLY_SOLVED' -or -not $js.solution.bus) { Write-Host "solution.json: $($row.case) $f"; $bad = 1 }
    }
    if ((Import-Csv "$f\summary.csv").Count -ne 2) { Write-Host "expected 2 cases in $f"; $bad = 1 }
}
# The Python package (the C API is built and tested on Linux and macOS).
$py = Get-Command python -ErrorAction SilentlyContinue
if ($py) {
    $env:PYTHONPATH = Join-Path $env:EMPOWER_DIR 'python'
    & $py.Source (Join-Path $root 'tools\api\test.py') cpu linear_solver=mumps
    if ($LASTEXITCODE -ne 0) { Write-Host 'Python package test failed'; $bad = 1 }
}
if (-not (Test-Path (Join-Path $env:EMPOWER_DIR 'bin\empower.dll'))) { Write-Host 'no bin\empower.dll'; $bad = 1 }
if ($bad) { exit 1 }
Write-Host 'all checks passed'
exit 0
