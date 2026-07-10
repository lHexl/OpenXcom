param(
    [string]$OutputRoot = 'D:\oxce\TestConst',
    [int]$MaxParallelBattles = 90,
    [int]$TimeoutSeconds = 300,
    [int[]]$Saves = @(31, 32, 33),
    [int]$RunsPerSave = 5,
    [int]$MaxConstants = 0,
    [int]$MaxVariantsPerConstant = 6,
    [string]$VariantTable = 'scripts\ai-constant-variants-31-33.csv',
    [string[]]$SourceFiles = @(
        'src\Battlescape\PlayerFactionAI.cpp',
        'src\Battlescape\PlayerFactionPlanner.cpp'
    )
)

$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$openxcomUser = 'C:\Users\user\Documents\OpenXcom'
$buildScript = Join-Path $root 'scripts\build-msvc-ninja.cmd'
$builtExe = Join-Path $root 'build-msvc-ninja\bin\openxcom.exe'
$tablePath = Join-Path $OutputRoot 'constant-variant-table.csv'
$runSummaryPath = Join-Path $OutputRoot 'run-summary.csv'
$variantErrorsPath = Join-Path $OutputRoot 'variant-errors.csv'
$statePath = Join-Path $OutputRoot 'state.json'
$runtimeRoot = Join-Path $OutputRoot 'runtime'
$runBinRoot = Join-Path $OutputRoot 'runbin'
$runBinInitialized = $false

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputRoot 'binaries') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $OutputRoot 'logs') -Force | Out-Null
New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
New-Item -ItemType Directory -Path $runBinRoot -Force | Out-Null

if (-not (Test-Path -LiteralPath $buildScript)) {
    throw "Build script not found: $buildScript"
}
if (-not (Test-Path -LiteralPath $openxcomUser)) {
    throw "OpenXcom user dir not found: $openxcomUser"
}

function Convert-ToIdentifier([string]$text) {
    return ($text -replace '[^A-Za-z0-9_]+', '_').Trim('_')
}

function Get-VariantValues([string]$type, [string]$rawValue) {
    $culture = [Globalization.CultureInfo]::InvariantCulture
    if ($type -eq 'int') {
        $base = [int]$rawValue
        $candidates = @(
            [math]::Floor($base * 0.50),
            [math]::Floor($base * 0.75),
            [math]::Floor($base * 0.90),
            [math]::Ceiling($base * 1.10),
            [math]::Ceiling($base * 1.25),
            [math]::Ceiling($base * 1.50)
        ) | ForEach-Object { [int]$_ }

        if ($base -gt 0) {
            $candidates = $candidates | ForEach-Object { [math]::Max(0, $_) }
        }
        elseif ($base -lt 0) {
            $candidates = $candidates | ForEach-Object { [math]::Min(0, $_) }
        }
        else {
            $candidates = @(-3, -1, 1, 2, 3, 5)
        }

        $unique = @()
        foreach ($candidate in $candidates) {
            if ($candidate -ne $base -and $unique -notcontains $candidate) {
                $unique += $candidate
            }
        }
        $delta = 1
        while ($unique.Count -lt 6) {
            foreach ($candidate in @($base - $delta, $base + $delta)) {
                if ($candidate -ne $base -and $unique -notcontains $candidate) {
                    $unique += [int]$candidate
                    if ($unique.Count -ge 6) { break }
                }
            }
            $delta++
        }
        return $unique[0..5] | ForEach-Object { $_.ToString($culture) }
    }

    $base = [double]::Parse(($rawValue -replace 'f$', ''), $culture)
    $candidates = @(0.50, 0.75, 0.90, 1.10, 1.25, 1.50) | ForEach-Object { $base * $_ }
    if ([math]::Abs($base) -lt 0.000001) {
        $candidates = @(0.1, 0.25, 0.5, 1.0, 1.5, 2.0)
    }
    $suffix = if ($type -eq 'float') { 'f' } else { '' }
    return $candidates | ForEach-Object { ([double]$_).ToString('0.######', $culture) + $suffix }
}

function Get-VariantKey([string]$file, [string]$name) {
    return "$file|$name"
}

function Read-Constants {
    $constants = @()
    foreach ($relativePath in $SourceFiles) {
        $path = Join-Path $root $relativePath
        if (-not (Test-Path -LiteralPath $path)) {
            Write-Warning "Source file not found, skipped: $relativePath"
            continue
        }
        $lines = Get-Content -LiteralPath $path
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^(?<prefix>\s*constexpr\s+(?<type>int|double|float)\s+(?<name>PLAYER_AI_[A-Za-z0-9_]+)\s*=\s*)(?<value>[-+]?\d+(?:\.\d+)?f?)(?<suffix>\s*;.*)$') {
                $constants += [pscustomobject]@{
                    File = $relativePath
                    Line = $i
                    Type = $Matches['type']
                    Name = $Matches['name']
                    OriginalValue = $Matches['value']
                    Prefix = $Matches['prefix']
                    Suffix = $Matches['suffix']
                    Description = ($Matches['suffix'] -replace '^\s*;\s*//\s*', '').Trim()
                }
            }
        }
    }
    return $constants
}

function Read-VariantTable([string]$path) {
    $resolvedPath = if ([IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $root $path }
    if (-not (Test-Path -LiteralPath $resolvedPath)) {
        Write-Warning "Variant table not found, generated fallback values will be used: $resolvedPath"
        return @{}
    }

    $table = @{}
    foreach ($row in (Import-Csv -LiteralPath $resolvedPath)) {
        if (-not $row.File -or -not $row.Constant) {
            continue
        }
        $values = @(
            $row.Value1,
            $row.Value2,
            $row.Value3,
            $row.Value4,
            $row.Value5,
            $row.Value6
        ) | Where-Object { $_ -and $_.Trim().Length -gt 0 }

        if ($values.Count -gt 0) {
            $table[(Get-VariantKey $row.File $row.Constant)] = $values
        }
    }
    return $table
}

function Get-ConfiguredVariantValues($constant, $variantRows) {
    $key = Get-VariantKey $constant.File $constant.Name
    if ($variantRows.ContainsKey($key)) {
        return $variantRows[$key]
    }
    return Get-VariantValues $constant.Type $constant.OriginalValue
}

function Set-ConstantValue($constant, [string]$value) {
    $path = Join-Path $root $constant.File
    $lines = Get-Content -LiteralPath $path
    $current = $lines[$constant.Line]
    if ($current -notmatch "constexpr\s+$($constant.Type)\s+$($constant.Name)\s*=") {
        throw "Constant moved or changed before edit: $($constant.File):$($constant.Line + 1) $($constant.Name)"
    }
    $lines[$constant.Line] = "$($constant.Prefix)$value$($constant.Suffix)"
    Set-Content -LiteralPath $path -Value $lines
}

function Restore-OriginalSources($constants) {
    foreach ($constant in $constants) {
        Set-ConstantValue $constant $constant.OriginalValue
    }
}

function Initialize-RunBin {
    if ($script:runBinInitialized) {
        return
    }

    $builtBin = Split-Path $builtExe
    Get-ChildItem -LiteralPath $builtBin -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -notlike 'openxcom*.exe' -and
            $_.Name -notlike 'openxcom*.pdb' -and
            $_.Name -notlike 'openxcom*.lib' -and
            $_.Name -notlike '*.txt'
        } |
        Copy-Item -Destination $runBinRoot -Force

    Get-ChildItem -LiteralPath $builtBin -Directory -ErrorAction SilentlyContinue |
        Copy-Item -Destination $runBinRoot -Recurse -Force

    $script:runBinInitialized = $true
}

function Invoke-Build([string]$variantId) {
    $buildLog = Join-Path $OutputRoot "build-$variantId.log"
    & $buildScript *> $buildLog
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed for $variantId. Log: $buildLog"
    }
    if (-not (Test-Path -LiteralPath $builtExe)) {
        throw "Built executable not found after build: $builtExe"
    }
    Initialize-RunBin
    $binaryDir = Join-Path $OutputRoot 'binaries'
    $archiveBinaryPath = Join-Path $binaryDir "openxcom-$variantId.exe"
    $runBinaryPath = Join-Path $runBinRoot "openxcom-$variantId.exe"
    Copy-Item -LiteralPath $builtExe -Destination $archiveBinaryPath -Force
    Copy-Item -LiteralPath $builtExe -Destination $runBinaryPath -Force
    return [pscustomobject]@{
        ArchiveBinary = $archiveBinaryPath
        RunBinary = $runBinaryPath
    }
}

function New-BattleRuntimeDir([string]$prefix, [int]$save) {
    $runtimeDir = Join-Path $runtimeRoot $prefix
    if (Test-Path -LiteralPath $runtimeDir) {
        Remove-Item -LiteralPath $runtimeDir -Recurse -Force
    }
    $saveDir = Join-Path $runtimeDir 'xcom1'
    New-Item -ItemType Directory -Path $saveDir -Force | Out-Null
    $sourceSave = Join-Path (Join-Path $openxcomUser 'xcom1') "$save.sav"
    if (-not (Test-Path -LiteralPath $sourceSave)) {
        throw "Save not found: $sourceSave"
    }
    Copy-Item -LiteralPath $sourceSave -Destination (Join-Path $saveDir "$save.sav") -Force
    return $runtimeDir
}

function Move-BattleLogs($entry) {
    if (-not (Test-Path -LiteralPath $entry.RuntimeDir)) {
        return
    }
    Get-ChildItem -LiteralPath $entry.RuntimeDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'auto-battle-log-*' -or $_.Name -like 'auto-battle-map-*' } |
        ForEach-Object {
            $targetName = "$($entry.LogPrefix)-$($_.Name)"
            Move-Item -LiteralPath $_.FullName -Destination (Join-Path $entry.LogDir $targetName) -Force -ErrorAction SilentlyContinue
        }
}

function Complete-BattleEntry([System.Collections.ArrayList]$running, [int]$index) {
    $entry = $running[$index]
    $entry.Process.Refresh()
    $elapsed = ((Get-Date) - $entry.Started).TotalSeconds
    if (-not $entry.Process.HasExited -and $elapsed -le $TimeoutSeconds) {
        return $false
    }

    if (-not $entry.Process.HasExited) {
        Stop-Process -Id $entry.Process.Id -Force -ErrorAction SilentlyContinue
        $entry.TimedOut = $true
    }
    $entry.Finished = Get-Date
    $entry.ExitCode = if ($entry.TimedOut) { $null } else { $entry.Process.ExitCode }
    Move-BattleLogs $entry
    [pscustomobject]@{
        VariantId = $entry.VariantId
        Save = $entry.Save
        Run = $entry.Run
        Seed = $entry.Seed
        LogPrefix = $entry.LogPrefix
        LogDir = $entry.LogDir
        Binary = $entry.Binary
        Started = $entry.Started
        Finished = $entry.Finished
        TimedOut = $entry.TimedOut
        ExitCode = $entry.ExitCode
    } | Export-Csv -LiteralPath $runSummaryPath -Append -NoTypeInformation -Encoding UTF8
    Remove-Item -LiteralPath $entry.RuntimeDir -Recurse -Force -ErrorAction SilentlyContinue
    $running.RemoveAt($index)
    return $true
}

function Wait-BattleSlot([System.Collections.ArrayList]$running) {
    while ($running.Count -ge $MaxParallelBattles) {
        $removedAny = $false
        for ($i = $running.Count - 1; $i -ge 0; $i--) {
            if (Complete-BattleEntry $running $i) {
                $removedAny = $true
            }
        }
        if (-not $removedAny -and $running.Count -ge $MaxParallelBattles) {
            Start-Sleep -Milliseconds 500
        }
    }
}

function Wait-AllBattles([System.Collections.ArrayList]$running) {
    while ($running.Count -gt 0) {
        $removedAny = $false
        for ($i = $running.Count - 1; $i -ge 0; $i--) {
            if (Complete-BattleEntry $running $i) {
                $removedAny = $true
            }
        }
        if (-not $removedAny -and $running.Count -gt 0) {
            Start-Sleep -Milliseconds 500
        }
    }
}

function Start-VariantBattles([System.Collections.ArrayList]$running, [string]$variantId, $binaryInfo) {
    $variantLogDir = Join-Path (Join-Path $OutputRoot 'logs') $variantId
    New-Item -ItemType Directory -Path $variantLogDir -Force | Out-Null

    if ($RunsPerSave -le 0) {
        return
    }

    foreach ($save in $Saves) {
        foreach ($run in 1..$RunsPerSave) {
            Wait-BattleSlot $running
            $seed = ($save * 1000) + $run
            $prefix = "$variantId-save$save-run$run-seed$seed"
            $runtimeDir = New-BattleRuntimeDir $prefix $save
            $stdout = Join-Path $variantLogDir "$prefix.stdout.txt"
            $stderr = Join-Path $variantLogDir "$prefix.stderr.txt"
            $args = @(
                '-user', $runtimeDir,
                '-config', $openxcomUser,
                '-master', 'xcom1',
                '-load', "$save.sav",
                '-playIntro', 'false',
                '-autoBattle', 'true',
                '-autoBattleHeadless', 'true',
                '-autoQuitAfterBattle', 'true',
                '-autoBattleLog', 'true',
                '-autoBattleDisablePlayerPanic', 'true',
                '-autoBattleSeed', "$seed"
            )
            $started = Get-Date
            $process = Start-Process -FilePath $binaryInfo.RunBinary -WorkingDirectory $runBinRoot -ArgumentList $args -PassThru -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            [void]$running.Add([pscustomobject]@{
                VariantId = $variantId
                Save = $save
                Run = $run
                Seed = $seed
                LogPrefix = $prefix
                LogDir = $variantLogDir
                Binary = $binaryInfo.ArchiveBinary
                RuntimeDir = $runtimeDir
                Started = $started
                Finished = $null
                TimedOut = $false
                ExitCode = $null
                Process = $process
            })
        }
    }
}

$constants = Read-Constants
if ($constants.Count -eq 0) {
    throw "No constexpr PLAYER_AI constants found."
}
$variantRows = Read-VariantTable $VariantTable

if (-not (Test-Path -LiteralPath $tablePath)) {
    'VariantId,Constant,File,OriginalValue,VariantValue,Binary,LogDir' | Set-Content -LiteralPath $tablePath -Encoding UTF8
}
if (-not (Test-Path -LiteralPath $runSummaryPath)) {
    'VariantId,Save,Run,Seed,LogPrefix,LogDir,Binary,Started,Finished,TimedOut,ExitCode' | Set-Content -LiteralPath $runSummaryPath -Encoding UTF8
}
if (-not (Test-Path -LiteralPath $variantErrorsPath)) {
    'VariantId,Constant,File,OriginalValue,VariantValue,Stage,Message,Updated' | Set-Content -LiteralPath $variantErrorsPath -Encoding UTF8
}

$runningBattles = [System.Collections.ArrayList]::new()
try {
    $constantIndex = 0
    foreach ($constant in $constants) {
        $constantIndex++
        if ($MaxConstants -gt 0 -and $constantIndex -gt $MaxConstants) {
            break
        }
        $values = Get-ConfiguredVariantValues $constant $variantRows
        if ($MaxVariantsPerConstant -gt 0 -and $MaxVariantsPerConstant -lt $values.Count) {
            $values = $values[0..($MaxVariantsPerConstant - 1)]
        }
        $variantIndex = 0
        foreach ($value in $values) {
            $variantIndex++
            $variantId = ('c{0:0000}_v{1}_{2}' -f $constantIndex, $variantIndex, (Convert-ToIdentifier $constant.Name))
            Write-Host "=== ${variantId}: $($constant.Name) $($constant.OriginalValue) -> $value ==="
            try {
                Set-ConstantValue $constant $value
                $binaryInfo = Invoke-Build $variantId
                $logDir = Join-Path (Join-Path $OutputRoot 'logs') $variantId
                [pscustomobject]@{
                    VariantId = $variantId
                    Constant = $constant.Name
                    File = $constant.File
                    OriginalValue = $constant.OriginalValue
                    VariantValue = $value
                    Binary = $binaryInfo.ArchiveBinary
                    LogDir = $logDir
                } | Export-Csv -LiteralPath $tablePath -Append -NoTypeInformation -Encoding UTF8
                Start-VariantBattles $runningBattles $variantId $binaryInfo
                [pscustomobject]@{
                    LastVariantId = $variantId
                    LastConstant = $constant.Name
                    LastConstantIndex = $constantIndex
                    LastVariantIndex = $variantIndex
                    Updated = (Get-Date)
                } | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding UTF8
            }
            catch {
                Write-Warning "Variant failed: ${variantId}: $($_.Exception.Message)"
                [pscustomobject]@{
                    VariantId = $variantId
                    Constant = $constant.Name
                    File = $constant.File
                    OriginalValue = $constant.OriginalValue
                    VariantValue = $value
                    Stage = 'variant'
                    Message = $_.Exception.Message
                    Updated = (Get-Date)
                } | Export-Csv -LiteralPath $variantErrorsPath -Append -NoTypeInformation -Encoding UTF8
            }
            finally {
                Set-ConstantValue $constant $constant.OriginalValue
            }
        }
    }
    Wait-AllBattles $runningBattles
}
finally {
    Wait-AllBattles $runningBattles
    Restore-OriginalSources $constants
}
