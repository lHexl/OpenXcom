$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$openxcom = Join-Path $root 'build-msvc-ninja\bin\openxcom.exe'
$openxcomUser = 'C:\Users\user\Documents\OpenXcom'
$timeoutSeconds = 300
$saves = 21..29
$runsPerSave = 4

if (-not (Test-Path -LiteralPath $openxcom)) {
    Write-Error "openxcom.exe not found: $openxcom"
    exit 1
}

Get-ChildItem $openxcomUser -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like 'auto-battle-log-*' -or $_.Name -like 'auto-battle-map-*' } |
    Remove-Item -Force -ErrorAction SilentlyContinue

$jobs = foreach ($save in $saves) {
    foreach ($run in 1..$runsPerSave) {
        $seed = ($save * 1000) + $run
        $staggerSeconds = (($save - $saves[0]) * $runsPerSave + ($run - 1)) % 10
        Start-Job -Name "autobattle-$save-$run" -ArgumentList $openxcom, $openxcomUser, $save, $run, $seed, $timeoutSeconds, $staggerSeconds -ScriptBlock {
            param($openxcom, $openxcomUser, $save, $run, $seed, $timeoutSeconds, $staggerSeconds)

            if ($staggerSeconds -gt 0) {
                Start-Sleep -Seconds $staggerSeconds
            }

            $args = @(
                '-user', $openxcomUser,
                '-config', $openxcomUser,
                '-master', 'xcom1',
                '-load', "$save.sav",
                '-playIntro', 'false',
                '-autoBattle', 'true',
                '-autoQuitAfterBattle', 'true',
                '-autoBattleLog', 'true',
                '-autoBattleSeed', "$seed"
            )

            $started = Get-Date
            $process = Start-Process -FilePath $openxcom -ArgumentList $args -PassThru
            $timedOut = -not $process.WaitForExit($timeoutSeconds * 1000)
            if ($timedOut) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
            $process.Refresh()

            [pscustomobject]@{
                Save = $save
                Run = $run
                Seed = $seed
                Started = $started
                Finished = Get-Date
                TimedOut = $timedOut
                ExitCode = if ($timedOut) { $null } else { $process.ExitCode }
            }
        }
    }
}

$jobTimeoutSeconds = $timeoutSeconds + 60
$finished = Wait-Job -Job $jobs -Timeout $jobTimeoutSeconds
$unfinished = $jobs | Where-Object { $_.State -eq 'Running' }
foreach ($job in $unfinished) {
    Stop-Job -Job $job -ErrorAction SilentlyContinue
}

$results = @()
foreach ($job in $jobs) {
    if ($job.State -eq 'Completed') {
        $results += Receive-Job -Job $job
    } else {
        $parts = $job.Name -replace '^autobattle-', ''
        $save, $run = $parts -split '-'
        $seed = ([int]$save * 1000) + [int]$run
        $results += [pscustomobject]@{
            Save = [int]$save
            Run = [int]$run
            Seed = $seed
            Started = $null
            Finished = Get-Date
            TimedOut = $true
            ExitCode = $null
        }
    }
}
Remove-Job -Job $jobs -Force -ErrorAction SilentlyContinue

$failed = $false
foreach ($result in ($results | Sort-Object Save, Run)) {
    if ($result.TimedOut) {
        Write-Host ("{0}.sav run={1} seed={2} timeout after {3}s" -f $result.Save, $result.Run, $result.Seed, $timeoutSeconds)
        $failed = $true
    } else {
        Write-Host ("{0}.sav run={1} seed={2} exit={3}" -f $result.Save, $result.Run, $result.Seed, $result.ExitCode)
        if ($result.ExitCode -ne 0) {
            $failed = $true
        }
    }
}

if ($failed) {
    exit 1
}
exit 0
