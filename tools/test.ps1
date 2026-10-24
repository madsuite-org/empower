# Install a built Windows archive with install.ps1, then solve cases with both forms.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$t = Join-Path ([System.IO.Path]::GetTempPath()) ("opfsolve-test-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $t | Out-Null
Set-Location $t
# Run as on a machine without Julia: the build job has Julia on PATH, which would
# hide a runtime DLL that the archive fails to provide.
$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notmatch 'julia' }) -join ';'
$env:OPFSOLVE_DIR = Join-Path $t 'opfsolve'
$env:OPFSOLVE_ARCHIVE = Join-Path $root 'opfsolve-windows-x86_64.tar.gz'
$env:OPFSOLVE_YES = '1'
$env:OPFSOLVE_ADD_TO_PATH = 'no'
# CI has no HSL licence: install without HSL and ask for MUMPS explicitly.
$env:OPFSOLVE_SKIP_HSL = '1'
& (Join-Path $root 'install.ps1')
$op = Join-Path $env:OPFSOLVE_DIR 'bin\opfsolve.exe'
& $op case14_ieee case118_ieee --form=polar '--options=tol=1e-8 max_iter=500 linear_solver=mumps' --output=polar
& $op case14_ieee case118_ieee --form=rect --options=linear_solver=mumps --output=rect

$bad = 0
# Without HSL and without an explicit linear solver, opfsolve must refuse and say how to get MA27.
$nohsl = & $op case14_ieee --output=nohsl 2>&1 | Out-String
if ($LASTEXITCODE -eq 0 -or $nohsl -notmatch 'hsl\.rl\.ac\.uk/download/MA27') { Write-Host 'opfsolve ran without MA27'; $bad = 1 }
$ref = @{ 'pglib_opf_case14_ieee' = 2178.1; 'pglib_opf_case118_ieee' = 97214 }
foreach ($f in 'polar', 'rect') {
    Get-Content "$f\summary.csv"
    if (-not (Select-String -Quiet -Pattern '^opfsolve: ' -Path "$f\system.txt")) { Write-Host "no system.txt in $f"; $bad = 1 }
    foreach ($row in Import-Csv "$f\summary.csv") {
        if ($row.termination_status -ne 'LOCALLY_SOLVED') { Write-Host "not solved: $($row.case) $f"; $bad = 1 }
        $r = $ref[$row.case]
        if ([math]::Abs(([double]$row.objective - $r) / $r) -gt 1e-4) { Write-Host "objective mismatch: $($row.case) $f $($row.objective)"; $bad = 1 }
        if (-not (Select-String -Quiet -Pattern 'linear solver mumps' -Path "$f\$($row.case)\ipopt.log")) { Write-Host "not mumps: $($row.case) $f"; $bad = 1 }
    }
    if ((Import-Csv "$f\summary.csv").Count -ne 2) { Write-Host "expected 2 cases in $f"; $bad = 1 }
}
if ($bad) { exit 1 }
Write-Host 'all checks passed'
# The deliberate no-MA27 run left a nonzero $LASTEXITCODE, which the CI wrapper would report.
exit 0
