# White-furnace suite: scores every QA furnace fixture raster-vs-path-tracer and gates on the
# unoccluded cases.
#
# A surface of known albedo lit only by the environment must give the same radiance down both paths, so
# any ratio between them is a normalisation or occlusion defect, not noise. The unoccluded fixtures are
# the ones that must hold at ~1.0 and are therefore the pass/fail gate; the roofed fixture is REPORTED
# but never gated, because its value legitimately tracks how much interreflection one-bounce GI drops.
#
#   .\furnace.ps1                       # score all fixtures against the in-repo samples project
#   .\furnace.ps1 -Tolerance 0.08       # loosen the unoccluded gate
#
# Fixtures come from make_furnace_fixtures.py; regenerate with that if you change the geometry.

param(
    [string] $Project   = "",
    [int]    $Width     = 320,
    [int]    $Height    = 240,
    [int]    $Spp       = 384,
    [int]    $Frames    = 64,
    # Environment weight. Kept well below 1 so a white albedo-1.0 surface cannot clip at 1.0 in the
    # LDR PNG; the metrics read linear HDR, but a clipped companion PNG is misleading to look at.
    [double] $Ibl       = 0.2,
    [double] $Tolerance = 0.06,
    [string] $Out       = "furnace"
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe  = Join-Path $repo 'bin\windows-x86_64\Debug\LuthShot\LuthShot.exe'
if (-not (Test-Path $exe)) { throw "LuthShot not built: $exe" }
if ([string]::IsNullOrEmpty($Project)) { $Project = Join-Path $repo 'samples\Samples.luthproj' }
$sceneDir = Join-Path $repo 'samples\assets\scenes'
New-Item -ItemType Directory -Force $Out | Out-Null

# name -> is it gated (unoccluded)?
$fixtures = [ordered]@{
    'qa_furnace_open'   = $true
    'qa_furnace_grey'   = $true
    'qa_furnace_metal'  = $true
    'qa_furnace_gloss'  = $true
    'qa_furnace_roofed' = $false
}

$rows = @()
foreach ($name in $fixtures.Keys) {
    $scene = Join-Path $sceneDir "$name.luth"
    if (-not (Test-Path $scene)) { Write-Warning "missing fixture $scene (run make_furnace_fixtures.py)"; continue }
    $ref  = Join-Path $Out "$name.ref.hdr"
    $test = Join-Path $Out "$name.test.hdr"

    if (-not (Test-Path $ref)) {
        & $exe $Project --scene $scene --hdr $ref --width $Width --height $Height `
            --frames 32 --spp $Spp --set "renderMode=1" --set "pathTrace.fireflyClamp=0" `
            --set "view.iblIntensity=$Ibl" *> (Join-Path $Out "$name.ref.log")
        if ($LASTEXITCODE -ne 0) { Write-Warning "$name reference bake failed"; continue }
    }

    & $exe $Project --scene $scene --hdr $test --compare $ref `
        --metrics (Join-Path $Out "$name.json") --width $Width --height $Height `
        --frames $Frames --set "view.iblIntensity=$Ibl" *> (Join-Path $Out "$name.test.log")
    if ($LASTEXITCODE -ne 0) { Write-Warning "$name score failed"; continue }

    $m = Get-Content (Join-Path $Out "$name.json") -Raw | ConvertFrom-Json
    $gated = $fixtures[$name]
    $off   = [math]::Abs($m.meanLumRatio - 1.0)
    $rows += [pscustomobject]@{
        Fixture  = $name -replace '^qa_furnace_', ''
        LumRatio = [math]::Round($m.meanLumRatio, 4)
        relMSE   = [math]::Round($m.relMSE, 5)
        SSIM     = [math]::Round($m.ssim, 4)
        Gated    = $gated
        Status   = if (-not $gated) { 'report' } elseif ($off -le $Tolerance) { 'PASS' } else { 'FAIL' }
    }
}

if ($rows.Count -eq 0) { throw 'no fixture produced a score' }
$rows | Format-Table -AutoSize
$rows | Export-Csv (Join-Path $Out 'furnace.csv') -NoTypeInformation

$failed = @($rows | Where-Object { $_.Status -eq 'FAIL' })
if ($failed.Count -gt 0) {
    Write-Host "`n$($failed.Count) unoccluded fixture(s) outside +/-$Tolerance of 1.0:" -ForegroundColor Red
    $failed | ForEach-Object { Write-Host "  $($_.Fixture): $($_.LumRatio)" -ForegroundColor Red }
    exit 1
}
Write-Host "`nAll gated fixtures within +/-$Tolerance of 1.0." -ForegroundColor Green
Write-Host "roofed is reported, not gated: it tracks the one-bounce GI interreflection deficit."
exit 0
