param(
	[string]$LogDir = "C:\Users\user\Documents\OpenXcom",
	[string]$Filter = "auto-battle-log-*.txt"
)

$logs = Get-ChildItem -LiteralPath $LogDir -Filter $Filter -ErrorAction Stop | Sort-Object Name
if (!$logs) {
	Write-Host "No auto battle logs found in $LogDir"
	exit 1
}

$rows = foreach ($file in $logs) {
	$text = Get-Content -LiteralPath $file.FullName -Raw
	$seed = if ($file.Name -match 'seed-(\d+)') { [int]$Matches[1] } else { 0 }
	$save = [int][math]::Floor($seed / 1000)
	$run = $seed % 1000
	$outcome = if ($text -match 'Outcome:\s*(\w+)') { $Matches[1] } else { "unknown" }
	$score = if ($text -match 'Score:\s*(-?\d+)') { [int]$Matches[1] } else { 0 }
	$turn = if ($text -match 'Final turn:\s*(\d+)') { [int]$Matches[1] } else { 0 }
	$deaths = 0
	$alienKills = 0
	$deathsFound = $false
	$alienKillsFound = $false
	foreach ($summaryLine in ($text -split "`r?`n")) {
		if (!$alienKillsFound -and $summaryLine -match '^- .*qty=(\d+),\s*score=' -and $summaryLine -notmatch 'X-COM') {
			$alienKills = [int]$Matches[1]
			$alienKillsFound = $true
		}
		if (!$deathsFound -and $summaryLine -match '^- .*X-COM.*qty=(\d+),\s*score=') {
			$deaths = [int]$Matches[1]
			$deathsFound = $true
		}
		if ($deathsFound -and $alienKillsFound) {
			break
		}
	}

	$currentTurn = 0
	$firstTurnDecisions = 0
	$firstTurnIdle = 0
	$firstTurnWalk = 0
	$firstTurnShot = 0
	$firstTurnThrow = 0
	$firstTurnSupport = 0
	$firstTurnStagingHold = 0
	$firstTurnStrategies = @{}
	$firstTurnExamples = New-Object System.Collections.Generic.List[string]

	foreach ($line in [System.IO.File]::ReadLines($file.FullName)) {
		if ($line -match 'Player faction plan: turn=(\d+).*strategy=([^,]+)') {
			$currentTurn = [int]$Matches[1]
			if ($currentTurn -eq 1) {
				$strategy = $Matches[2]
				if (!$firstTurnStrategies.ContainsKey($strategy)) { $firstTurnStrategies[$strategy] = 0 }
				$firstTurnStrategies[$strategy]++
			}
			continue
		}
		if ($currentTurn -eq 1 -and $line -match 'AI decision: unit #(\d+)') {
			$unitId = [int]$Matches[1]
			if ($unitId -lt 100000) {
				$firstTurnDecisions++
				if ($line -match ' chose idle') { $firstTurnIdle++ }
				elseif ($line -match ' chose walk') { $firstTurnWalk++ }
				elseif ($line -match ' chose (auto shot|snap shot|aimed shot|shot)') { $firstTurnShot++ }
				elseif ($line -match ' chose throw') { $firstTurnThrow++ }
				if ($firstTurnExamples.Count -lt 6) {
					$firstTurnExamples.Add($line)
				}
			}
		}
		if ($currentTurn -eq 1 -and $line -match 'Player faction support move: unit=(\d+)') {
			$unitId = [int]$Matches[1]
			if ($unitId -lt 100000) {
				$firstTurnSupport++
			}
		}
		if ($currentTurn -eq 1 -and $line -match 'Player faction staging reserve hold: unit=(\d+)') {
			$unitId = [int]$Matches[1]
			if ($unitId -lt 100000) {
				$firstTurnStagingHold++
			}
		}
	}

	$strategySummary = ($firstTurnStrategies.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name):$($_.Value)" }) -join ";"
	$grenadeNoTUSum = 0
	foreach ($m in [regex]::Matches($text, 'noTU=(\d+)')) { $grenadeNoTUSum += [int]$m.Groups[1].Value }
	$grenadeNoThrowPathSum = 0
	foreach ($m in [regex]::Matches($text, 'noThrowPath=(\d+)')) { $grenadeNoThrowPathSum += [int]$m.Groups[1].Value }
	$grenadeScoreRejectedSum = 0
	foreach ($m in [regex]::Matches($text, 'scoreRejected=(\d+)')) { $grenadeScoreRejectedSum += [int]$m.Groups[1].Value }

	[pscustomobject]@{
		File = $file.Name
		Save = $save
		Run = $run
		Outcome = $outcome
		Score = $score
		Turn = $turn
		Deaths = $deaths
		AlienKills = $alienKills
		FirstDecisions = $firstTurnDecisions
		FirstIdle = $firstTurnIdle
		FirstWalk = $firstTurnWalk
		FirstShot = $firstTurnShot
		FirstThrow = $firstTurnThrow
		FirstSupport = $firstTurnSupport
		FirstStagingHold = $firstTurnStagingHold
		FirstStrategies = $strategySummary
		CautiousPatrol = ([regex]::Matches($text, 'Player faction cautious patrol step')).Count
		SupportMove = ([regex]::Matches($text, 'Player faction support move')).Count
		StagingHold = ([regex]::Matches($text, 'Player faction staging reserve hold')).Count
		CleanMove = ([regex]::Matches($text, 'Player faction clean shot move:')).Count
		CleanReject = ([regex]::Matches($text, 'Player faction clean shot move rejected')).Count
		Fallback = ([regex]::Matches($text, 'Player faction fallback cover move')).Count
		LineBreak = ([regex]::Matches($text, 'hit-and-run line break')).Count
		PanicSuppressed = ([regex]::Matches($text, 'Player panic suppressed for autobattle')).Count
		ProximityMine = ([regex]::Matches($text, 'proximity mine ambush|proximity mine staged placement|Player faction explosive action:.*proximity=1')).Count
		ProximityStage = ([regex]::Matches($text, 'proximity mine staging')).Count
		ProximityBlocked = ([regex]::Matches($text, 'proximity mine staged placement blocked')).Count
		ExplosiveAction = ([regex]::Matches($text, 'Player faction explosive action:')).Count
		TeamExplosive = ([regex]::Matches($text, 'Player faction team-spotted explosive attack')).Count
		ExplosiveNoTarget = ([regex]::Matches($text, 'Player faction explosive no target:')).Count
		ExplosiveRejected = ([regex]::Matches($text, 'Player faction explosive candidate rejected')).Count
		GrenadeNoTU = $grenadeNoTUSum
		GrenadeNoThrowPath = $grenadeNoThrowPathSum
		GrenadeScoreRejected = $grenadeScoreRejectedSum
		GrenadeAllyRisk = ([regex]::Matches($text, 'bestRejectedReason=ally_projectile_or_scatter_risk|bestRejectedReason=ally_cannot_evacuate_blast|bestRejectedReason=thrower_in_blast')).Count
		FirstExamples = ($firstTurnExamples -join " || ")
	}
}

Write-Host "=== OVERALL ==="
$rows | Sort-Object Save, Run | Format-Table Save,Run,Outcome,Score,Turn,Deaths,AlienKills,FirstIdle,FirstWalk,FirstShot,FirstThrow,FirstSupport,FirstStagingHold,CautiousPatrol,SupportMove,StagingHold,CleanMove,LineBreak,PanicSuppressed,ExplosiveAction,TeamExplosive -AutoSize

Write-Host ""
Write-Host "=== SUMMARY ==="
[pscustomobject]@{
	Logs = $rows.Count
	Success = ($rows | Where-Object Outcome -eq "success").Count
	Failure = ($rows | Where-Object Outcome -eq "failure").Count
	Unknown = ($rows | Where-Object Outcome -eq "unknown").Count
	AvgScore = [math]::Round((($rows | Measure-Object Score -Average).Average), 2)
	AvgDeaths = [math]::Round((($rows | Measure-Object Deaths -Average).Average), 2)
	AvgAlienKills = [math]::Round((($rows | Measure-Object AlienKills -Average).Average), 2)
	FirstIdle = ($rows | Measure-Object FirstIdle -Sum).Sum
	FirstWalk = ($rows | Measure-Object FirstWalk -Sum).Sum
	FirstShot = ($rows | Measure-Object FirstShot -Sum).Sum
	FirstThrow = ($rows | Measure-Object FirstThrow -Sum).Sum
	FirstSupport = ($rows | Measure-Object FirstSupport -Sum).Sum
	FirstStagingHold = ($rows | Measure-Object FirstStagingHold -Sum).Sum
	CautiousPatrol = ($rows | Measure-Object CautiousPatrol -Sum).Sum
	SupportMove = ($rows | Measure-Object SupportMove -Sum).Sum
	StagingHold = ($rows | Measure-Object StagingHold -Sum).Sum
	CleanMove = ($rows | Measure-Object CleanMove -Sum).Sum
	CleanReject = ($rows | Measure-Object CleanReject -Sum).Sum
	Fallback = ($rows | Measure-Object Fallback -Sum).Sum
	LineBreak = ($rows | Measure-Object LineBreak -Sum).Sum
	PanicSuppressed = ($rows | Measure-Object PanicSuppressed -Sum).Sum
	ProximityMine = ($rows | Measure-Object ProximityMine -Sum).Sum
	ProximityStage = ($rows | Measure-Object ProximityStage -Sum).Sum
	ProximityBlocked = ($rows | Measure-Object ProximityBlocked -Sum).Sum
	ExplosiveAction = ($rows | Measure-Object ExplosiveAction -Sum).Sum
	TeamExplosive = ($rows | Measure-Object TeamExplosive -Sum).Sum
	ExplosiveNoTarget = ($rows | Measure-Object ExplosiveNoTarget -Sum).Sum
	ExplosiveRejected = ($rows | Measure-Object ExplosiveRejected -Sum).Sum
	GrenadeNoTU = ($rows | Measure-Object GrenadeNoTU -Sum).Sum
	GrenadeNoThrowPath = ($rows | Measure-Object GrenadeNoThrowPath -Sum).Sum
	GrenadeScoreRejected = ($rows | Measure-Object GrenadeScoreRejected -Sum).Sum
	GrenadeAllyRisk = ($rows | Measure-Object GrenadeAllyRisk -Sum).Sum
} | Format-List

Write-Host "=== BY SAVE ==="
$rows | Group-Object Save | ForEach-Object {
	$g = $_.Group
	[pscustomobject]@{
		Save = $_.Name
		Success = ($g | Where-Object Outcome -eq "success").Count
		AvgScore = [math]::Round((($g | Measure-Object Score -Average).Average), 2)
		AvgDeaths = [math]::Round((($g | Measure-Object Deaths -Average).Average), 2)
		AvgKills = [math]::Round((($g | Measure-Object AlienKills -Average).Average), 2)
		FirstIdle = ($g | Measure-Object FirstIdle -Sum).Sum
		FirstWalk = ($g | Measure-Object FirstWalk -Sum).Sum
		FirstSupport = ($g | Measure-Object FirstSupport -Sum).Sum
		FirstStagingHold = ($g | Measure-Object FirstStagingHold -Sum).Sum
		CautiousPatrol = ($g | Measure-Object CautiousPatrol -Sum).Sum
		SupportMove = ($g | Measure-Object SupportMove -Sum).Sum
		StagingHold = ($g | Measure-Object StagingHold -Sum).Sum
		LineBreak = ($g | Measure-Object LineBreak -Sum).Sum
		PanicSuppressed = ($g | Measure-Object PanicSuppressed -Sum).Sum
		ExplosiveAction = ($g | Measure-Object ExplosiveAction -Sum).Sum
		TeamExplosive = ($g | Measure-Object TeamExplosive -Sum).Sum
		GrenadeNoTU = ($g | Measure-Object GrenadeNoTU -Sum).Sum
		GrenadeAllyRisk = ($g | Measure-Object GrenadeAllyRisk -Sum).Sum
	}
} | Format-Table -AutoSize

Write-Host ""
Write-Host "=== FIRST TURN STRATEGIES ==="
$rows | Select-Object Save,Run,FirstStrategies | Sort-Object Save,Run | Format-Table -AutoSize

Write-Host ""
Write-Host "=== FIRST TURN EXAMPLES ==="
$rows | Sort-Object Save,Run | Select-Object -First 8 Save,Run,FirstExamples | Format-List
