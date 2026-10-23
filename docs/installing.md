# Windows installation

1. Download `HazeRecomp-<version>-Setup.exe` and `SHA256SUMS.txt` from the same
   release. Compare `Get-FileHash .\HazeRecomp-<version>-Setup.exe` with its hash.
2. Run the installer and select your **disc-decrypted Haze USA ISO**, BLUS30094,
   disc 01.02 / application 01.00. The README lists the tested ISO's SHA-256.
3. Click **Install**. The installer installs the application for your user,
   downloads the verified official RPCS3 helper and Sony firmware, extracts your
   disc, decrypts its executable and prepares the firmware.
4. When preparation and launcher validation finish, click **Play Haze**. Later,
   start **Haze Recomp** from the Start menu.

The default installation folder is `%LOCALAPPDATA%\Programs\Haze Recomp`.
No separate RPCS3, 7-Zip, Python, compiler or .NET SDK installation is required.
The installer uses Windows' included .NET Framework and PowerShell. It needs
internet access during preparation and at least 12 GB free. Windows x64, SSE4.1
and D3D12 graphics support are required. Encrypted raw dumps, other regions and
updated executables are unsupported.

If the required Microsoft Visual C++ system runtime is absent or outdated, the
installer downloads and verifies Microsoft's signed redistributable and installs
it automatically. Windows may request administrator approval for that prerequisite;
the Haze application itself is installed for the current user.

The installed application contains the ready-to-use launcher and runtime files.
Installer scripts and extraction tools are temporary payloads, not a setup task
that the launcher asks you to run later. Firmware and game data come from the
download and your disc; they are not included in the installer.

## Upgrades and recovery

Run a newer installer at the same location. **Keep my existing prepared game and
firmware** skips extraction/downloads and validates your existing configuration.
If preparation failed previously, rerun the installer with the ISO. Failed setup
keeps previous configuration and saves, and leaves logs for diagnosis.

Installer logs are under `%LOCALAPPDATA%\Haze Recomp\InstallerLogs`.
Windows Installer's source MSI is retained under the adjacent `InstallerCache`
folder so repair works after temporary preparation tools are removed.
Preparation logs/data are under `local/user-data/setup-<id>` inside the installation.
Each attempt has its own portable RPCS3 directory and does not change another
RPCS3 installation. Do not delete the successful attempt referenced by launcher.ini.

MSI upgrades/repairs preserve `launcher.ini`, customized `config/*.ini` and
user-created data. Saves default to `local/state/savedata`, with automatic
backups in its `.haze-saves` directory (docs/native-saves.md). Back up saves before
upgrading. Uninstall from Windows Settings retains user-created data and settings.

The portable ZIP is for an already prepared installation: extract it to a writable
directory and supply launcher.ini with DiscDirectory, Firmware and Savedata paths.
Use the installer for first-time ISO preparation. Do not copy stale launcher.ini
paths from another machine.

`HazeLauncher.exe --check --no-ui` checks configuration without starting the game.
Game logs are under `local/sessions`. GPU compatibility, full campaign completion,
co-op and consistent performance remain experimental; see each release's notes.
