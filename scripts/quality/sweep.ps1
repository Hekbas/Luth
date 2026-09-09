# Ranks render settings against a path-traced reference of the same scene.
#
# Bake the reference first (PathTrace mode writes its fp32 accumulator, so the reference carries no
# tonemap and no fp16 rounding), then score each knob value against it. Metrics run on LINEAR HDR:
# comparing tonemapped PNGs reads near-zero error exactly where fireflies live, because ACES clips them
# to white in the test and the reference alike.
#
#   .\sweep.ps1 -Project <p>.luthproj -Scene <s>.luth -Knob svgfGi.atrousIterations -Values 2,4,6,8
#
# Reference bake is skipped when -Ref already exists, so a sweep can be re-run cheaply.

param(
    [Parameter(Mandatory)] [string]   $Project,
    [Parameter(Mandatory)] [string]   $Scene,
    [Parameter(Mandatory)] [string]   $Knob,
    [Parameter(Mandatory)] [string[]] $Values,
    [string]   $Out    = "sweep",
    [string]   $Ref    = "",
    [int]      $Width  = 640,
    [int]      $Height = 360,
    [int]      $Spp    = 512,
    [int]      $Frames = 64,
    # Applied to BOTH the reference and every test run, so the only difference between them is $Knob.
    [string[]] $Common = @()
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot '..\..\bin\windows-x86_64\Debug\LuthShot\LuthShot.exe'
if (-not (Test-Path $exe)) { throw "LuthShot not built: $exe" }
New-Item -ItemType Directory -Force $Out | Out-Null
if ([string]::IsNullOrEmpty($Ref)) { $Ref = Join-Path $Out 'reference.hdr' }

$commonArgs = @()
foreach ($c in $Common) { $commonArgs += @('--set', $c) }

if (-not (Test-Path $Ref)) {
    Write-Host "Baking reference at $Spp spp -> $Ref"
    & $exe $Project --scene $Scene --hdr $Ref --width $Width --height $Height `
        --frames 32 --spp $Spp --set "renderMode=1" --set "pathTrace.fireflyClamp=0" `
        @commonArgs *> (Join-Path $Out 'reference.log')
    if ($LASTEXITCODE -ne 0) { throw "reference bake failed (exit $LASTEXITCODE), see reference.log" }
} else {
    Write-Host "Reusing reference $Ref"
}

$rows = @()
foreach ($v in $Values) {
    $tag = ($Knob -replace '[^A-Za-z0-9]', '_') + '_' + ($v -replace '[^A-Za-z0-9]', '_')
    Write-Host "  $Knob = $v"
    & $exe $Project --scene $Scene `
        --hdr       (Join-Path $Out "$tag.hdr") `
        --shot      (Join-Path $Out "$tag.png") `
        --compare   $Ref `
        --metrics   (Join-Path $Out "$tag.json") `
        --error-map (Join-Path $Out "$tag.err.png") `
        --width $Width --height $Height --frames $Frames `
        --set "$Knob=$v" @commonArgs *> (Join-Path $Out "$tag.log")
    if ($LASTEXITCODE -ne 0) { Write-Warning "  run failed (exit $LASTEXITCODE), see $tag.log"; continue }

    $m = Get-Content (Join-Path $Out "$tag.json") -Raw | ConvertFrom-Json
    $rows += [pscustomobject]@{
        Value    = $v
        relMSE   = [math]::Round($m.relMSE, 5)
        p99      = [math]::Round($m.p99RelErr, 4)
        p999     = [math]::Round($m.p999RelErr, 4)
        SSIM     = [math]::Round($m.ssim, 4)
        LumRatio = [math]::Round($m.meanLumRatio, 4)
    }
}

if ($rows.Count -eq 0) { throw 'every run failed' }

# relMSE ranks overall agreement; p99/p99.9 are the columns that track fireflies specifically, since a
# mean flattens sparse outliers. A LumRatio far from 1 means the run disagrees on ENERGY, and then every
# other column is measuring that offset rather than noise -- fix the bias before reading the rest.
Write-Host "`nRanked by relMSE (lower is closer to reference):"
$rows | Sort-Object relMSE | Format-Table -AutoSize
$rows | Sort-Object relMSE | Export-Csv (Join-Path $Out 'sweep.csv') -NoTypeInformation
Write-Host "wrote $(Join-Path $Out 'sweep.csv')"
