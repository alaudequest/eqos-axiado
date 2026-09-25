param(
    [ValidateSet('Build', 'Flash', 'Probe')][string]$Action = 'Build',
    [ValidateSet('CM7', 'CM4', 'All')][string]$Core = 'CM7',
    [string]$CubeIdePath = 'D:\Stm32IDE\STM32CubeIDE_1.19.0\STM32CubeIDE',
    [string]$SerialNumber = ''
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$ide = Join-Path $CubeIdePath 'stm32cubeidec.exe'
if (!(Test-Path -LiteralPath $ide)) { throw "STM32CubeIDE not found: $ide. Set -CubeIdePath." }

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed (exit $LASTEXITCODE)." }
}

if ($Action -eq 'Probe' -or $Action -eq 'Flash') {
    $programmer = Get-ChildItem -Path (Join-Path $CubeIdePath 'plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*\tools\bin\STM32_Programmer_CLI.exe') |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    if (!$programmer) { throw 'STM32CubeProgrammer CLI not found in CubeIDE plugins.' }
    if ($Action -eq 'Probe') {
        Invoke-Checked $programmer @('-l', 'st-link')
        exit 0
    }
}

$cores = if ($Core -eq 'All') { @('CM4', 'CM7') } else { @($Core) }
$buildArgs = @('--launcher.suppressErrors', '-nosplash', '-application',
    'org.eclipse.cdt.managedbuilder.core.headlessbuild', '-data',
    (Join-Path $PSScriptRoot '.cubeide-workspace'))
$images = @()
foreach ($selectedCore in $cores) {
    $corePath = Join-Path $projectRoot $selectedCore
    [xml]$description = Get-Content -LiteralPath (Join-Path $corePath '.project')
    $projectName = $description.projectDescription.name
    $projectUri = ([System.Uri]($corePath + '\')).AbsoluteUri
    $buildArgs += @('-import', $projectUri, '-build', "$projectName/Debug")
    $images += Join-Path $corePath "Debug\$projectName.elf"
}
Invoke-Checked $ide $buildArgs
foreach ($elf in $images) {
    if (!(Test-Path -LiteralPath $elf)) { throw "Build did not produce $elf" }
    Write-Host "Firmware: $elf"
}

if ($Action -eq 'Flash') {
    $connection = @('-c', 'port=SWD', 'mode=UR', 'reset=HWrst')
    if ($SerialNumber) { $connection += "sn=$SerialNumber" }
    foreach ($elf in $images) {
        Invoke-Checked $programmer ($connection + @('-w', $elf, '-v'))
    }
    Invoke-Checked $programmer ($connection + @('-rst'))
}
