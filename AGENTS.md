# Repo Notes

Use the split docs directly:

- Reverse-engineered proprietary coffee-machine BLE protocol notes for the currently supported brand set, plus APK findings, register maps, and wire-level vectors:
  - [`docs/NIVONA.md`](docs/NIVONA.md)
- ESP coffee bridge firmware, saved-machine API, web UI, and bridge capability coverage:
  - [`docs/BRIDGE.md`](docs/BRIDGE.md)

APK reverse-engineering artifacts and extracted outputs live under the local-only, gitignored analysis cache:

- [`.analysis/research/`](.analysis/research/)

The real APK used for reverse engineering was:

- [`.analysis/research/downloads/de.nivona.mobileapp-3.8.6.apk`](.analysis/research/downloads/de.nivona.mobileapp-3.8.6.apk)
  - package: `de.nivona.mobileapp`
  - version: `3.8.6`
  - SHA-256: `e4eb6063c7a1516f4e1820070282f907b132bea3fb8d05b15e3526a739c90ba6`

If `.analysis/research/` or the APK above is missing because the repo was freshly cloned, treat that as expected and restart the APK reverse-engineering workflow to regenerate the local artifacts before relying on the reverse-engineering notes.
