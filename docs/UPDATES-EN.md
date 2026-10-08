# WARDOGS Fire Control updates

[Русский](UPDATES-RU.md) · [README](../README.en.md)

The updater is available starting with **2.8.0**. It uses public stable releases of [sany86russ/Wardogs-Fire-Control](https://github.com/sany86russ/Wardogs-Fire-Control/releases/latest).

## Using the updater

1. The app checks for a new release in the background at startup. You can disable startup checks in Settings.
2. Use **Check for updates** in the header to check manually.
3. When a banner appears, **What's new** opens the release page; **Later** hides the banner until the next check.
4. **Update** opens the download progress window. You can cancel before installation.
5. After the archive is verified, the app exits, updates the full package and restarts.

Finish your current calculation before updating. Coordinates, history and session corrections live in memory and are cleared on restart. Settings, logs and imported `.wdt` files under `%LOCALAPPDATA%\WardogsFireControl` are preserved.

## First upgrade from an older version

Versions **2.7.0 and earlier** cannot update themselves. Download `WardogsFireControl-v2.8.0-win-x64.zip` from Assets once and extract the **entire** archive into a separate folder. Launch the new package. Settings and heights are restored from your Windows profile.

## Requirements and verification

- Windows 10/11 x64, HTTPS access to GitHub, Windows PowerShell 5.1 and write access to the application folder.
- A full portable package containing `package-manifest.json`, `Update.ps1`, DLLs and resources. Development builds must be updated manually.
- Only a newer stable `vX.Y.Z` version is accepted. Drafts and prereleases are skipped.
- The archive must have the exact name `WardogsFireControl-vX.Y.Z-win-x64.zip`, a correct URL in this repository and a SHA-256 digest in GitHub metadata.
- Downloads are bounded by the declared size. The updater verifies the complete ZIP SHA-256, file list, individual lengths and SHA-256 hashes. It also verifies the executable's Windows x64 format, product name and version.
- Archives with path traversal, duplicate paths, links, invalid manifests or damaged files are rejected before installed files are replaced.

TLS uses Windows certificate validation. The GitHub digest confirms integrity against release metadata; the updater does not independently verify GitHub attestations or publisher signatures. For manual downloads, you can additionally verify provenance using the command in the README.

Version checks do not send coordinates, images, settings or terrain. Calculations and local OCR remain available offline; manual checks report connection errors.

## Errors and recovery

Preparation uses a separate `.wardogs-update-…` directory inside the application folder. The archive, files and installation preparation are checked before the app closes. An external helper then waits for the app to exit and replaces only package files, keeping their previous versions. Unknown user files are preserved.

If replacement or process launch fails, the installer attempts to restore and restart the previous package. Backups and `error.log` remain in `.wardogs-update-…` for inspection and manual recovery. Rollback does not establish ongoing application health: a crash after the new process has launched successfully is not automatically diagnosed.

If the app will not start after an update, download a verified previous ZIP from Releases and extract it into a new folder. Settings and imported terrain remain in your Windows profile. When reporting an issue, provide the version, error description and `error.log`, checking the log for personal paths first.

Avoid running multiple copies from the same folder during an update. Files locked by another copy or protective software cause an installation error and retained diagnostics.
