[CmdletBinding()]
param(
    [string]$ArduinoCli = '',
    [string]$PythonExe = '',
    [string]$OutputRoot = '',
    [switch]$IncludePrev3,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$sketch = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $sketch 'build_variants'
}
$sketchFiles = @(Get-ChildItem -LiteralPath $sketch -Filter '*.ino' -File)
if ($sketchFiles.Count -ne 1) {
    throw 'Expected exactly one .ino file in the project folder.'
}
if ([string]::IsNullOrWhiteSpace($ArduinoCli) -or !(Test-Path -LiteralPath $ArduinoCli -PathType Leaf)) {
    $cliCandidates = @('C:\Program Files\Arduino CLI\arduino-cli.exe')
    $cliCandidates += @(Get-ChildItem -Path (Join-Path $env:USERPROFILE '.gemini\antigravity\brain\*\scratch\arduino-cli\arduino-cli.exe') -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
    $ArduinoCli = $cliCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($ArduinoCli)) {
        $ArduinoCli = (Get-Command 'arduino-cli' -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    }
}
if ([string]::IsNullOrWhiteSpace($env:ARDUINO_DIRECTORIES_DATA)) {
    $defaultBoards = @(Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Arduino15\packages\esp32\hardware\esp32\*\boards.txt') -ErrorAction SilentlyContinue)
    if ($defaultBoards.Count -eq 0) {
        $dataCandidate = Get-ChildItem -Path (Join-Path $env:USERPROFILE '.gemini\antigravity\brain\*\scratch\arduino-data') -Directory -ErrorAction SilentlyContinue |
            Where-Object { @(Get-ChildItem -Path (Join-Path $_.FullName 'packages\esp32\hardware\esp32\*\boards.txt') -ErrorAction SilentlyContinue).Count -gt 0 } |
            Select-Object -First 1
        if ($dataCandidate) {
            $env:ARDUINO_DIRECTORIES_DATA = $dataCandidate.FullName
        }
    }
}
if ([string]::IsNullOrWhiteSpace($PythonExe) -or !(Test-Path -LiteralPath $PythonExe -PathType Leaf)) {
    $pyCandidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python313\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311\python.exe'),
        'C:\Users\1234\AppData\Local\Programs\Python\Python312\python.exe'
    )
    $PythonExe = $pyCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($PythonExe)) {
        $PythonExe = (Get-Command 'python' -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    }
}
if (!(Test-Path -LiteralPath (Join-Path $sketch 'zh-CN/dashboard_source.zh-CN.html') -PathType Leaf)) {
    throw 'Missing zh-CN/dashboard_source.zh-CN.html'
}

$output = [System.IO.Path]::GetFullPath($OutputRoot)
$finalBuild = Join-Path $sketch 'Final-Build'
$sketchName = $sketchFiles[0].Name
$firmwareName = $sketchFiles[0].BaseName
$revisions = @{ postv3 = 'REV3.X'; prev3 = 'REV1.X' }
$options = 'FlashSize=32M,PartitionScheme=app5M_fat24M_32MB,PSRAM=enabled,USBMode=hwcdc,CDCOnBoot=cdc,FlashMode=qio'
$failed = @()

$variantsToBuild = @('postv3')
if ($IncludePrev3) { $variantsToBuild += 'prev3' }

$configs = @()
foreach ($variant in $variantsToBuild) {
    foreach ($hosted in @('C6', 'C5')) {
        foreach ($chinese in @($true, $false)) {
            $configs += @{ Variant = $variant; Hosted = $hosted; Chinese = $chinese }
        }
    }
}

foreach ($cfg in $configs) {
    $variant = $cfg.Variant
    $hosted = $cfg.Hosted
    $languageSuffix = if ($cfg.Chinese) { '.zh-CN' } else { '.en' }
    $buildId = "$variant-$hosted$languageSuffix"
    $destination = Join-Path $output $buildId
    $buildPath = Join-Path $destination 'build'
    $artifacts = Join-Path $destination 'firmware'
    $log = Join-Path $destination 'compile.log'
    $fqbn = "esp32:esp32:esp32p4:ChipVariant=$variant,$options"
    $revision = $revisions[$variant]
    $releaseName = "$firmwareName.$hosted$languageSuffix"
    $compileSketch = $sketch

    if ($cfg.Chinese -and !$DryRun) {
        $prepared = & $PythonExe (Join-Path $sketch 'tools/prepare_chinese_build.py') --project $sketch --destination (Join-Path $destination 'work') --output-root $output
        if ($LASTEXITCODE -ne 0) {
            $failed += $buildId
            Write-Host "FAILED: $buildId preparing Chinese dashboard" -ForegroundColor Red
            continue
        }
        $compileSketch = [string]($prepared | Select-Object -Last 1)
    }

    $cliArgs = @('compile', '--jobs', '2', '--fqbn', $fqbn, '--build-path', $buildPath, '--output-dir', $artifacts)
    $mapPath = (Join-Path $destination 'firmware.map') -replace '\\', '/'
    $c5ObjPath = ''
    if ($hosted -eq 'C5') {
        $c5ObjPath = (Join-Path $destination 'c5_hosted\transport_drv_c5.o') -replace '\\', '/'
        if (!$DryRun) {
            & $PythonExe (Join-Path $sketch 'tools\patch_c5_hosted.py') --out $c5ObjPath
            if ($LASTEXITCODE -ne 0) {
                $failed += $buildId
                Write-Host "FAILED: $buildId generating transport_drv_c5.o" -ForegroundColor Red
                continue
            }
        }
        $cliArgs += @(
            '--build-property', 'compiler.cpp.extra_flags=-DP4CAN_HOSTED_C5',
            '--build-property', 'compiler.c.extra_flags=-DP4CAN_HOSTED_C5',
            '--build-property', "compiler.c.elf.extra_flags=-Wl,-Map=$mapPath $c5ObjPath"
        )
    }
    $cliArgs += $compileSketch

    Write-Host "`n=== $buildId ==="
    Write-Host "Board: $fqbn (Hosted=$hosted)"
    Write-Host "Final OTA: $(Join-Path $finalBuild "$releaseName.OTA.$revision.bin")"
    Write-Host "Final fullflash: $(Join-Path $finalBuild "$releaseName.FULLFLASH.$revision.merged.bin")"
    if ($DryRun) { continue }

    New-Item -ItemType Directory -Path $destination, $buildPath, $artifacts -Force | Out-Null
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $ArduinoCli @cliArgs 2>&1 | Tee-Object -FilePath $log
        $result = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }

    if ($result -ne 0) {
        $failed += $buildId
        Write-Host "FAILED: $buildId (exit $result). Log: $log" -ForegroundColor Red
    } else {
        try {
            $publishFiles = @(
                @{ Source = "$sketchName.bin"; Target = "$releaseName.OTA.$revision.bin" },
                @{ Source = "$sketchName.merged.bin"; Target = "$releaseName.FULLFLASH.$revision.merged.bin" }
            )
            foreach ($file in $publishFiles) {
                $original = Join-Path $artifacts $file.Source
                if (!(Test-Path -LiteralPath $original -PathType Leaf) -or (Get-Item -LiteralPath $original).Length -eq 0) {
                    throw "Missing or empty compiler output: $original"
                }
            }
            New-Item -ItemType Directory -Path $finalBuild -Force | Out-Null
            foreach ($file in $publishFiles) {
                $original = Join-Path $artifacts $file.Source
                $renamed = Join-Path $artifacts $file.Target
                Move-Item -LiteralPath $original -Destination $renamed -Force
                Copy-Item -LiteralPath $renamed -Destination (Join-Path $finalBuild $file.Target) -Force
            }
            Write-Host "SUCCESS: $buildId ($revision). OTA and fullflash copied to $finalBuild" -ForegroundColor Green
        } catch {
            $failed += $buildId
            Write-Host "FAILED: $buildId packaging. $_" -ForegroundColor Red
        }
    }
}

if ($failed.Count -gt 0) {
    Write-Host ("Build failed: " + ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
Write-Host "`nAll variants compiled successfully. Final binaries: $finalBuild" -ForegroundColor Green
exit 0
