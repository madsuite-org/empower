# Install a built Windows archive with install.ps1 and solve cases with both forms.
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$t = Join-Path ([System.IO.Path]::GetTempPath()) ("opfsolve-test-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $t | Out-Null
Set-Location $t
$env:OPFSOLVE_DIR = Join-Path $t 'opfsolve'
$env:OPFSOLVE_ARCHIVE = Join-Path $root 'opfsolve-windows-x86_64.tar.gz'

& (Join-Path $root 'install.ps1') case14_ieee case118_ieee --form=polar '--options=tol=1e-8 max_iter=500' --output=polar
& (Join-Path $root 'install.ps1') case14_ieee case118_ieee --form=rect --output=rect

$bad = 0
$ref = @{ 'pglib_opf_case14_ieee' = 2178.1; 'pglib_opf_case118_ieee' = 97214 }
foreach ($f in 'polar', 'rect') {
    Get-Content "$f\summary.csv"
    foreach ($row in Import-Csv "$f\summary.csv") {
        if ($row.termination_status -ne 'LOCALLY_SOLVED') { Write-Host "not solved: $($row.case) $f"; $bad = 1 }
        $r = $ref[$row.case]
        if ([math]::Abs(([double]$row.objective - $r) / $r) -gt 1e-4) { Write-Host "objective mismatch: $($row.case) $f $($row.objective)"; $bad = 1 }
        if (-not (Select-String -Quiet -Pattern 'linear solver ma27' -Path "$f\$($row.case)\ipopt.log")) { Write-Host "not ma27: $($row.case) $f"; $bad = 1 }
    }
    if ((Import-Csv "$f\summary.csv").Count -ne 2) { Write-Host "expected 2 cases in $f"; $bad = 1 }
}
if ($bad) { exit 1 }
Write-Host 'all checks passed'
