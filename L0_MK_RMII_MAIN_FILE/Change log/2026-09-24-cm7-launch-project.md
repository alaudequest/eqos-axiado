# Resolve CM7 project name conflict - 2026-09-24

The current workspace registers eth_3_CM7 at
D:/STM32/Workspace/eth_3/CM7 (confirmed from its workspace .location file).
The eth_4 copy also used that project name, preventing it from being imported
alongside the old CM7 project. A header or the root container is not the CM7
debug target. The reported selection error cannot be confirmed from the UI,
but the duplicate name and old project mapping were verified.

## Changes

- CM7/.project: rename this project to eth_4_CM7.
- CM7/.cproject: update project name and Debug/Release build paths.
  The existing artifactName uses ProjName, so the ELF becomes eth_4_CM7.elf.
- Rename CM7/eth_3_CM7 Debug.launch to CM7/eth_4_CM7 Debug.launch;
  update project, mapped resource, load-list, executable and log paths.
- Update the generated Debug makefile to build eth_4_CM7.elf immediately.
- No firmware, CM4, old eth_3 project, or workspace metadata changes.

## CubeIDE steps

1. File > Import > General > Existing Projects into Workspace.
2. Select root directory D:/STM32/Workspace/eth_4/CM7.
3. Select eth_4_CM7, leave Copy projects into workspace unchecked, and Finish.
4. Select eth_4_CM7 in Project Explorer and build it.
5. Run > Debug Configurations > STM32 C/C++ Application >
   eth_4_CM7 Debug. Main tab: project eth_4_CM7, application
   Debug/eth_4_CM7.elf. Click Debug.

Select this configuration explicitly rather than launching the active CMSIS
header or the parent eth_4 container. If the configuration is not listed,
refresh eth_4_CM7 (F5) and reopen Debug Configurations.

## Validation

Project and launch XML parsed successfully. Debug build/link passed and produced
CM7/Debug/eth_4_CM7.elf. Log: CM7/Debug/launch-project-build.log.
IDE import and ST-LINK launch still need to be performed in CubeIDE.
