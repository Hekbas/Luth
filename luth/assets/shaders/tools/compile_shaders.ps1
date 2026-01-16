<#
.SYNOPSIS
    Compiles GLSL shaders to SPIR-V for the Luth Engine.
    Features incremental building and a retro-styled console interface :D

.PARAMETER Force
    Recompiles all shaders regardless of file timestamps.
#>
param (
    [switch]$Force = $false
)

# ==============================================================================
# CONSOLE INTERFACE SETUP
# ==============================================================================
$HOST.UI.RawUI.WindowTitle = "LUTH // SHADER COMPILER"
[Console]::BackgroundColor = "Black"
Clear-Host

# Function: Simulates teletype text output
function Type-Text($text, $color="Gray", $speed=2) {
    $text.ToCharArray() | ForEach-Object {
        Write-Host $_ -NoNewline -ForegroundColor $color
        Start-Sleep -Milliseconds $speed
    }
    Write-Host ""
}

# ASCII Art Logo
$Logo = @"
  _      _   _ _____ _   _ 
 | |    | | | |_   _| | | |
 | |    | | | | | | | |_| |
 | |____| |_| | | | |  _  |
 |______|\___/  |_| |_| |_|
      E  N  G  I  N  E
"@

# Render Header
Write-Host $Logo -ForegroundColor Cyan
Write-Host ""
Write-Host "----------------------------------------------------" -ForegroundColor Cyan
Type-Text "> INITIALIZING BUILD PIPELINE..." "Cyan" 5

# ==============================================================================
# CONFIGURATION & PATHS
# ==============================================================================
$ScriptDir   = $PSScriptRoot
$ProjectRoot = Split-Path -Parent $ScriptDir
$VulkanBin   = "$env:VULKAN_SDK\Bin\glslc.exe"
$SourceDir   = $ProjectRoot
$OutputDir   = Join-Path $ProjectRoot "spv"

# Valid Shader Extensions
$Extensions  = @("vert", "frag", "comp", "geom", "tesc", "tese", "mesh", "task", "rgen", "rint", "rahit", "rchit", "miss", "call", "glsl")

# ==============================================================================
# ENVIRONMENT CHECKS
# ==============================================================================

if (-not (Test-Path $VulkanBin)) {
    Type-Text "[CRITICAL ERROR] VULKAN SDK NOT FOUND." "Red" 10
    Read-Host "PRESS ENTER TO EXIT"
    exit 1
}

if (-not (Test-Path $SourceDir)) {
    Type-Text "[ERROR] PROJECT ROOT NOT FOUND." "Red" 10
    Read-Host "PRESS ENTER TO EXIT"
    exit 1
}

if (-not (Test-Path $OutputDir)) {
    Type-Text "> CREATING OUTPUT DIRECTORY: SPV..." "Cyan"
    New-Item -ItemType Directory -Path $OutputDir | Out-Null
}

# ==============================================================================
# COMPILATION LOOP
# ==============================================================================

$GlobalStopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$CompiledCount = 0
$SkippedCount  = 0
$ErrorCount    = 0

$SourceFiles = Get-ChildItem -Path $SourceDir -File | Where-Object { $Extensions -contains $_.Extension.TrimStart('.') }

if ($SourceFiles.Count -eq 0) {
    Type-Text "[WARNING] NO SHADERS FOUND IN ROOT." "Yellow"
}

foreach ($File in $SourceFiles) {
    $InputPath  = $File.FullName
    $OutputPath = Join-Path $OutputDir ($File.Name + ".spv")
    $ShouldCompile = $true
    
    # Incremental Build Logic
    if ((Test-Path $OutputPath) -and (-not $Force)) {
        if ($File.LastWriteTime -le (Get-Item $OutputPath).LastWriteTime) {
            $ShouldCompile = $false
        }
    }

    if ($ShouldCompile) {
        $FileStopwatch = [System.Diagnostics.Stopwatch]::StartNew()

        Write-Host " > COMPILING: " -NoNewline -ForegroundColor DarkCyan
        Write-Host $File.Name.PadRight(30) -NoNewline -ForegroundColor White

        $ProcessInfo = New-Object System.Diagnostics.ProcessStartInfo
        $ProcessInfo.FileName = $VulkanBin
        $ProcessInfo.Arguments = """$InputPath"" -o ""$OutputPath"""
        $ProcessInfo.RedirectStandardOutput = $true
        $ProcessInfo.RedirectStandardError = $true
        $ProcessInfo.UseShellExecute = $false
        $ProcessInfo.CreateNoWindow = $true

        $Process = [System.Diagnostics.Process]::Start($ProcessInfo)
        $Process.WaitForExit()
        
        $FileStopwatch.Stop()
        $TimeStr = "{0:N0} ms" -f $FileStopwatch.Elapsed.TotalMilliseconds

        $StdOut = $Process.StandardOutput.ReadToEnd()
        $StdErr = $Process.StandardError.ReadToEnd()

        if ($Process.ExitCode -eq 0) {
            Write-Host "[ OK ] " -NoNewline -ForegroundColor Green
            Write-Host "($TimeStr)" -ForegroundColor DarkGray
            $CompiledCount++
        } else {
            Write-Host "[FAIL]" -ForegroundColor Red
            Write-Host "   -------------------------------------------------" -ForegroundColor DarkRed
            if ($StdOut) { Write-Host $StdOut -ForegroundColor Red }
            if ($StdErr) { Write-Host $StdErr -ForegroundColor Red }
            Write-Host "   -------------------------------------------------" -ForegroundColor DarkRed
            $ErrorCount++
        }
    } else {
        $SkippedCount++
    }
}

$GlobalStopwatch.Stop()

# ==============================================================================
# CALCULATE BINARY SIZE
# ==============================================================================
$TotalBytes = 0
if (Test-Path "$OutputDir\*.spv") {
    $TotalBytes = (Get-ChildItem -Path "$OutputDir\*.spv" | Measure-Object -Property Length -Sum).Sum
}
$SizeKB = "{0:N2} KB" -f ($TotalBytes / 1KB)

# ==============================================================================
# EXTENSIVE REPORT
# ==============================================================================
Write-Host ""
Write-Host "----------------------------------------------------" -ForegroundColor Cyan
Type-Text "> GENERATING SYSTEM REPORT..." "Cyan" 5
Write-Host ""

$TotalTime = "{0:N2} s" -f $GlobalStopwatch.Elapsed.TotalSeconds

Write-Host " EXECUTION TIME : " -NoNewline -ForegroundColor Gray
Write-Host $TotalTime -ForegroundColor White

Write-Host " FILES PROCESSED: " -NoNewline -ForegroundColor Gray
Write-Host ($CompiledCount + $SkippedCount + $ErrorCount) -ForegroundColor White

Write-Host " ----------------" -ForegroundColor DarkGray

Write-Host " [V] COMPILED   : " -NoNewline -ForegroundColor Gray
Write-Host $CompiledCount -ForegroundColor Green

Write-Host " [-] SKIPPED    : " -NoNewline -ForegroundColor Gray
Write-Host $SkippedCount -ForegroundColor DarkGray

Write-Host " [X] ERRORS     : " -NoNewline -ForegroundColor Gray
Write-Host $ErrorCount -ForegroundColor Red

Write-Host " ----------------" -ForegroundColor DarkGray

Write-Host " [#] TOTAL SIZE : " -NoNewline -ForegroundColor Gray
Write-Host $SizeKB -ForegroundColor Yellow

Write-Host ""
Write-Host "----------------------------------------------------" -ForegroundColor Cyan

if ($ErrorCount -gt 0) {
    Write-Host "STATUS: BUILD FAILED." -ForegroundColor Red
} else {
    Write-Host "STATUS: BUILD SUCCESSFUL." -ForegroundColor Cyan
}

Write-Host ""
Read-Host "PRESS ENTER TO EXIT"