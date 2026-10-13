# Windows installation

Haze Recomp is experimental. Full campaign completion, co-op and consistent
30 FPS are not validated. Read the release notes before installing.

1. Download the MSI and SHA256SUMS.txt from the same release. Compare
   `Get-FileHash .\HazeRecomp-<version>-windows-x64.msi` with its published hash.
2. Run the MSI. It installs for the current user under
   `%LOCALAPPDATA%\Programs\Haze Recomp`, without administrator privileges.
3. Open **Haze Recomp** from Start, select **Set up from disc ISO**, and choose
   your decrypted USA disc ISO: BLUS30094, disc 01.02, application 01.00.
   Encrypted dumps, other regions and updated executables are unsupported.
4. Choose **Yes** to download firmware from Sony, or **No** to choose a local
   PS3UPDAT.PUP. Setup verifies firmware before using it. The pinned 4.93
   download and compatible 2.17 firmware contain the supported decoder bytes.
5. Setup extracts the disc, decrypts EBOOT and extracts firmware using the
   bundled tools. The launcher continues after preparation succeeds.

No separate RPCS3, 7-Zip, Python, Git, .NET SDK or compiler installation is
required. Firmware and game data are not included in the package. Allow at least
12 GB of free space per setup attempt. Windows x64, SSE4.1 and D3D12 graphics
support are required. GPU compatibility and later missions remain unvalidated.

Alternatively, extract the **entire** portable ZIP into a writable directory
and open HazeLauncher.exe. Setup.cmd starts preparation independently.

Custom paths and offline/unattended setup:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup.ps1 `
  -Iso 'D:\Disc images\Haze.iso' -DownloadFirmware `
  -DataDirectory 'D:\Haze data' -NoGui
# Offline: replace -DownloadFirmware with -Firmware 'D:\PS3UPDAT.PUP'.
```

Sony's [HTTPS support page](https://www.playstation.com/en-us/support/hardware/ps3/system-software/)
currently links an HTTP firmware CDN. The release pins SHA-256 and rejects a
missing/corrupt download before extraction. Setup does not bypass certificate
validation or use third-party mirrors.

## Upgrades and recovery

Install a newer MSI over the previous version. Back up `launcher.ini`,
`config/*.ini` and `local/state/savedata` first. Saves default to that directory
under the installation folder; launcher.ini can override it with `Savedata=...`.
MSI upgrades/repairs preserve settings. Uninstall retains settings and user-created
data, saves and logs; remove those manually only if you intend to remove them.

For ZIP upgrades, extract into a new folder, copy configuration, and point
DiscDirectory, Firmware and Savedata in launcher.ini at the existing absolute
paths. Never delete the old `local` directory as part of upgrading. Only
`local/cache/rsx-shaders` is disposable shader cache.

Every setup attempt uses an isolated directory under `local/user-data` or your
chosen data directory. Its bundled RPCS3 copy uses portable configuration, so
an existing RPCS3 installation is untouched. Failed attempts keep their logs
and leave launcher.ini and previous prepared data unchanged. Successful setup
backs up an existing configuration to launcher.ini.before-setup. Do not remove
the successful attempt referenced by launcher.ini.

The pinned RPCS3 helper can exit with 0xC0000409 during headless firmware
teardown after successful extraction. Setup records that exit code and accepts
it only if **both** required decoder hashes match. Other exits, missing files
and incompatible decoder hashes fail setup. See firmware.exit-code.txt and the
helper logs within the attempt directory.

`HazeLauncher.exe --check --no-ui` validates configured paths without starting
the game. Game logs are under `local/sessions`. Missing DLLs indicate an
incomplete package; reinstall the full release. Release notes record remaining
validation limits, including clean-Windows acceptance.
