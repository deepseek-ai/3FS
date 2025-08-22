param (
    [String] $ProjectFilter = "*.pproj",
    [Alias('ms')]
    [Int] $MaxSteps = 200000,
    [Alias('i')]
    [Int] $NumIters = 10,
    [Alias('p')]
    [Int] $Parallel = 1,
    [Alias('s')]
    [Int64] $Seed = -1,
    [ValidateSet('random', 'pos', 'feedback', 'feedbackpos')]
    [Alias('sch')]
    [String] $Scheduling = "pos",
    [Alias('v')]
    [Switch] $Verbose,
    [Alias('m')]
    [String[]] $TestMethods = @(),
    [Alias('t')]
    [String] $TestFilter = ".*",
    [Alias('c')]
    [Switch] $ContinueOnFailure,
    [Alias('k')]
    [Switch] $SkipBuildProject,
    [Alias('w')]
    [Int] $StartTaskDelayMilliSecs = 100,
    [Alias('o')]
    [Int] $TimeoutSecs = 0
);


try {
    $projectPath = Get-ChildItem -Filter $ProjectFilter | Select-Object -First 1
    $projectFolder = Split-Path -Parent $projectPath.FullName
    [xml]$projectObj = Get-Content -Path $projectPath.FullName
    $projectName = $projectObj.Project.ProjectName
} catch {
    Write-Error "Cannot load project file: $ProjectFilter"
    exit 1
}

if ($SkipBuildProject) {
    Write-Host -ForegroundColor DarkYellow "Skip building project"
    $LASTEXITCODE = 0
} else {
    try {
        Push-Location $projectFolder
        Write-Host -ForegroundColor DarkYellow "Building project: $projectName"
        dotnet tool run p compile --pproj $projectPath.FullName
    } finally {
        Pop-Location
    }
}

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($TestMethods.Count -eq 0) {
    $TestMethods = dotnet tool run p check --list-tests | Select-String -SimpleMatch tc
    Write-Host -ForegroundColor Blue "Test methods: {$TestMethods}"
}

$exitCode = 0
$testResults = @();
$failedTasks = @();
$startTime = Get-Date
$outputRoot = Join-Path "PCheckerOutput" $startTime.ToString("yyyy-MM-dd_HH-mm-ss")

foreach ($testMethod in $TestMethods) {
    if (!($testMethod -match $TestFilter)) {
        Write-Host -ForegroundColor Blue "Skipped test: $testMethod"
        continue;
    }

    $testParams = "--fail-on-maxsteps --max-steps $MaxSteps --schedules $NumIters --sch-$Scheduling --testcase $testMethod"
    if ($Seed -ge 0) { $testParams += " --seed $Seed" }
    if ($Verbose) { $testParams += " --verbose" }

    Write-Host -ForegroundColor DarkYellow "Running test: $testMethod"

    # start the test tasks
    $testStart = Get-Date
    $testTasks = @{}
    $testOutputs = @{}

    for ($taskId = 0; $taskId -lt $Parallel; $taskId++) {
        Start-Sleep -Milliseconds $StartTaskDelayMilliSecs
        $outputPath =  Join-Path $outputRoot "$testMethod" "t$taskId"
        New-Item -ItemType Directory -Force $outputPath | Out-Null
        $testOutput = New-Item (Join-Path $outputPath "test.log")
        Write-Host "Test task output: $($testOutput.FullName)"
        $testTask = Start-Process -NoNewWindow -Passthru -RedirectStandardOutput $testOutput -FilePath "dotnet" -ArgumentList "tool run p check $testParams --outdir $outputPath"
        $testTasks[$taskId] = $testTask
        $testOutputs[$taskId] = $testOutput
    }

    while ($true) {
        $runningTasks = @()
        for ($taskId = 0; $taskId -lt $Parallel; $taskId++) {
            if (!$testTasks[$taskId].HasExited) { $runningTasks += $taskId }
        }

        if ($runningTasks.Count -eq 0) {
            break;
        }

        Write-Host "[$(Get-Date)] Found $($runningTasks.Count) running test processes: $($runningTasks)"
        Start-Sleep -Seconds 10

        if ($TimeoutSecs -gt 0) {
            $elapsedSecs = (New-TimeSpan -Start $testStart -End (Get-Date)).TotalSeconds
            if ($elapsedSecs -ge $TimeoutSecs) {
                Write-Host -ForegroundColor DarkYellow "[$(Get-Date)] Elapsed time ($($elapsedSecs.ToString("#.#"))s) exceeds timeout ($($timeoutSecs)s), stopping tests..."
                for ($taskId = 0; $taskId -lt $Parallel; $taskId++) {
                    Stop-Process -Force $testTasks[$taskId]
                    $testTasks[$taskId].WaitForExit()
                }
            }
        }
    }

    $elapsedSecs = (New-TimeSpan -Start $testStart -End (Get-Date)).TotalSeconds
    Write-Host "[$(Get-Date)] Test processes stopped after running for $($elapsedSecs.ToString("#.#"))s"

    # print outputs of test processes
    for ($taskId = 0; $taskId -lt $Parallel; $taskId++) {
        if ($Verbose) {
            Copy-Item -Verbose -Force $testOutputs[$taskId] ($testMethod + ".$taskId.txt")
        }

        $numSchedPoints = Get-Content $testOutputs[$taskId] | Select-String -Pattern "(\d+) \(min\), (\d+) \(avg\), (\d+) \(max\)" |
        Foreach-Object {
            $min, $avg, $max = $_.Matches[0].Groups[1..3].Value
            [PSCustomObject] @{
                min = $min
                avg = $avg
                max = $max
            }
        } | Select-Object -Last 1

        $taskName = "$testMethod[$taskId]"

        $foundBug = Get-Content $testOutputs[$taskId] | Select-String -Pattern "Checker found a bug" |
            Foreach-Object { ($_.Matches[0].Groups[0].Value) } | Select-Object -First 1

        $testStatus = if ($null -eq $foundBug) { "pass" } else { "fail" }

        $testSeed = Get-Content $testOutputs[$taskId] | Select-String -Pattern "Checker is using '[A-Za-z]+' strategy \(seed:(\d+)\)" |
            Foreach-Object { [Int64]($_.Matches[0].Groups[1].Value) } | Select-Object -First 1

        $testSchedules = Get-Content $testOutputs[$taskId] | Select-String -Pattern "Explored ([\d.]+) schedules" |
            Foreach-Object { [Int64]($_.Matches[0].Groups[1].Value) } | Select-Object -First 1

        $testSeconds = Get-Content $testOutputs[$taskId] | Select-String -Pattern "Elapsed ([\d.]+) sec" |
            Foreach-Object { [Double]($_.Matches[0].Groups[1].Value) } | Select-Object -First 1

        $testResults += [PSCustomObject] @{
            test = $taskName
            status = $testStatus
            seed = $testSeed
            schedules = $testSchedules
            seconds = $testSeconds.ToString("0.0")
            min = if ($null -ne $numSchedPoints) { $numSchedPoints.min } else { -1 }
            avg = if ($null -ne $numSchedPoints) { $numSchedPoints.avg } else { -1 }
            max = if ($null -ne $numSchedPoints) { $numSchedPoints.max } else { -1 }
        };

        if ($null -ne $foundBug) {
            Write-Host -ForegroundColor DarkYellow "Test process output #${taskId}/${Parallel}:"
            Get-Content $testOutputs[$taskId]

            $checkerOutputPath = Get-Content $testOutputs[$taskId] | Select-String -Pattern "Writing (.*)" |
                Foreach-Object { ($_.Matches[0].Groups[1].Value) } | Select-String -SimpleMatch -Pattern ".txt" | Select-Object -First 1

            # filter key messages from output
            $checkerOutputFile = Get-Item $checkerOutputPath
            $debugLogFile = (Join-Path $checkerOutputFile.DirectoryName $checkerOutputFile.BaseName) + ".$testMethod" + ".csv"
            $logPattern = @(
                "sent event 'eWriteWork with payload", "sent event 'eWriteWorkDone with payload",
                "sent event 'eCommitWork with payload", "sent event 'eCommitWorkDone with payload",
                "sent event 'eWriteReq with payload", "sent event 'eWriteResp with payload",
                "sent event 'eUpdateMsg with payload", "sent event 'eCommitMsg with payload",
                "sent event 'eGetTargetSyncInfoResult with payload",
                "sent event 'eSyncStartResp with payload",
                "sent event 'eSyncDoneResp with payload",
                "sent event 'eHaltReq with payload",
                "sent event 'eShutDown with payload",
                "sent event 'eRestart with payload",
                # "dequeued event 'eUpdateTargetStateMsg with payload",
                "dequeued event 'eNewRoutingInfo with payload",
                "set its targets offline",
                "replication chain updated",
                "start write process",
                "updatesOfChunkReplica",
                # "aliveStorageServices",
                "<ErrorLog>"
            )
            Select-String -Path $checkerOutputFile -SimpleMatch -CaseSensitive -Pattern $logPattern |
                Select-Object -Property 'Line' -First 100000 |
                Out-File -Width 10000 -Encoding utf8 $debugLogFile
            Write-Host -ForegroundColor DarkYellow "Debug log: $debugLogFile"
            # save reprod script to file
            $outputPath =  Join-Path $outputRoot "$testMethod" "t$taskId"
            $reprodCmdstr = "dotnet tool run p check $testParams -s $testSeed --outdir $outputPath"
            $reprodScriptFile = (Join-Path $checkerOutputFile.DirectoryName $checkerOutputFile.BaseName) + ".$testMethod" + ".ps1"
            Set-Content -Path $reprodScriptFile -Value $reprodCmdstr
            Write-Host -ForegroundColor DarkYellow "Reprod script: $reprodScriptFile"
            Write-Host -ForegroundColor DarkYellow "Reprod command: $reprodCmdstr"

            # set exit code to indicate the failure
            $exitCode = -1
            $failedTasks += $taskName
        }
    }

    if (($exitCode -ne 0) -and (-not $ContinueOnFailure)) {
        Write-Host -ForegroundColor DarkYellow "[$(Get-Date)] Test $testMethod failed, stopping..."
        break;
    }
}

$elapsedTime = New-TimeSpan -Start $startTime -End (Get-Date)
Write-Host -ForegroundColor DarkYellow "-----------------------"
Write-Host -ForegroundColor DarkYellow "Summary of test results"
Write-Host -ForegroundColor DarkYellow "-----------------------"
Write-Host -ForegroundColor DarkYellow "[$(Get-Date)] Elapsed time: $($elapsedTime.TotalSeconds.ToString(`"#.#`"))s"

Format-Table -AutoSize -InputObject $testResults
$testResults | Export-Csv -NoTypeInformation -Path (Join-Path $outputRoot "test_results.csv")

if ($exitCode -eq 0) {
    Write-Host -ForegroundColor DarkYellow "[$(Get-Date)] All tests passed"
} else {
    Write-Host -ForegroundColor DarkYellow "[$(Get-Date)] Failed test tasks: $failedTasks"
}

exit $exitCode
