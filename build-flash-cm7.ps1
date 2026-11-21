# Build the selected project's CM7, then program, verify, and reset the board.
[CmdletBinding()]
param(
    # Optional: select a specific ST-LINK when multiple boards are connected.
    [string]$SerialNumber,
    [ValidateSet('eth_3', 'L0_MK_RMII_END2END_PC_TX', 'L0_MK_RMII_MAC_LOOPBACK','L0_MK_RMII_MAIN_FILE')]
    [string]$Project = 'L0_MK_RMII_END2END_PC_TX',
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
$ide = 'D:/STM32/STM32CubeIDE_1.19.0/STM32CubeIDE/stm32cubeidec.exe'
$programmer = 'C:/Program Files/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe'
$buildDir = Join-Path $PSScriptRoot "$Project/CM7/Debug"
$projectDir = Join-Path $PSScriptRoot "$Project/CM7"
# Folder names can differ from the CubeIDE project and its ${ProjName} artifact.
[xml]$projectDescription = Get-Content -LiteralPath (Join-Path $projectDir '.project') -Raw
$projectName = $projectDescription.projectDescription.name
$firmware = Join-Path $buildDir "${projectName}.elf"

foreach ($tool in @($ide, $programmer)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required tool not found: $tool"
    }
}

$workspace = Join-Path $PSScriptRoot ".metadata/headless-$Project"
& $ide --launcher.suppressErrors -nosplash -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data $workspace -import $projectDir -build "${projectName}/Debug"
if ($LASTEXITCODE -ne 0) {
        throw "CM7 build failed (exit code $LASTEXITCODE). Flash skipped."
    }
if (-not (Test-Path -LiteralPath $firmware)) {
        throw "Build did not produce $firmware. Flash skipped."
    }
if ($BuildOnly) {
        Write-Host "Built $firmware"
        return
    }

$connectionArgs = @('-c', 'port=SWD')
if (-not [string]::IsNullOrWhiteSpace($SerialNumber)) {
    $connectionArgs += "sn=$($SerialNumber.Trim())"
}
$connectionArgs += @('mode=UR', 'reset=HWrst')
& $programmer @connectionArgs -d $firmware -v -rst
if ($LASTEXITCODE -ne 0) {
        throw "CM7 programming failed (exit code $LASTEXITCODE)."
    }
Write-Host 'CM7 firmware built, flashed, verified, and board reset.'
