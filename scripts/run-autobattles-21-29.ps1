$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$openxcom = Join-Path $root 'build-msvc-ninja\bin\openxcom.exe'
$openxcomUser = 'C:\Users\user\Documents\OpenXcom'
$timeoutSeconds = 300
$saves = 21..29

if (-not (Test-Path -LiteralPath $openxcom)) {
    Write-Error "openxcom.exe not found: $openxcom"
    exit 1
}

$jobs = foreach ($save in $saves) {
    Start-Job -Name "autobattle-$save" -ArgumentList $openxcom, $openxcomUser, $save, $timeoutSeconds, ($save - 21) -ScriptBlock {
        param($openxcom, $openxcomUser, $save, $timeoutSeconds, $staggerSeconds)

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
            '-autoBattleLog', 'true'
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
            Started = $started
            Finished = Get-Date
            TimedOut = $timedOut
            ExitCode = if ($timedOut) { $null } else { $process.ExitCode }
        }
    }
}

$jobTimeoutSeconds = $timeoutSeconds + $saves.Count + 30
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
        $save = [int]($job.Name -replace '^autobattle-', '')
        $results += [pscustomobject]@{
            Save = $save
            Started = $null
            Finished = Get-Date
            TimedOut = $true
            ExitCode = $null
        }
    }
}
Remove-Job -Job $jobs -Force -ErrorAction SilentlyContinue

$failed = $false
foreach ($result in ($results | Sort-Object Save)) {
    if ($result.TimedOut) {
        Write-Host ("{0}.sav timeout after {1}s" -f $result.Save, $timeoutSeconds)
        $failed = $true
    } else {
        Write-Host ("{0}.sav exit={1}" -f $result.Save, $result.ExitCode)
        if ($result.ExitCode -ne 0) {
            $failed = $true
        }
    }
}

if ($failed) {
    exit 1
}
exit 0
