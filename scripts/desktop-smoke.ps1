param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$WorkingDirectory)
$ErrorActionPreference = 'Stop'
# Use a normal redirected Windows launch for a GUI-subsystem executable.
$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $Executable
$startInfo.Arguments = '--smoke-test'
$startInfo.WorkingDirectory = $WorkingDirectory
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardError = $true
$startInfo.RedirectStandardOutput = $true
$process = [System.Diagnostics.Process]::Start($startInfo)
$stderr = $process.StandardError.ReadToEndAsync()
$stdout = $process.StandardOutput.ReadToEndAsync()
if (-not $process.WaitForExit(35000)) { $process.Kill(); throw 'The desktop smoke test timed out.' }
Write-Output $stdout.Result
Write-Output $stderr.Result
exit $process.ExitCode
