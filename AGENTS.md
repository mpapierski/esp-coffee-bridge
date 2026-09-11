# Repo Notes

Use the split docs directly:

- Reverse-engineered proprietary coffee-machine BLE protocol notes for the currently supported brand set, plus APK findings, register maps, and wire-level vectors:
  - [`docs/NIVONA.md`](docs/NIVONA.md)
- ESP coffee bridge HTTP API, firmware architecture, web UI, storage behavior, and capability coverage:
  - [`docs/API.md`](docs/API.md)

APK reverse-engineering artifacts and extracted outputs live under the local-only, gitignored analysis cache:

- [`.analysis/research/`](.analysis/research/)

The real APK used for reverse engineering was:

- [`.analysis/research/downloads/de.nivona.mobileapp-3.8.6.apk`](.analysis/research/downloads/de.nivona.mobileapp-3.8.6.apk)
  - package: `de.nivona.mobileapp`
  - version: `3.8.6`
  - SHA-256: `e4eb6063c7a1516f4e1820070282f907b132bea3fb8d05b15e3526a739c90ba6`

If `.analysis/research/` or the APK above is missing because the repo was freshly cloned, treat that as expected and restart the APK reverse-engineering workflow to regenerate the local artifacts before relying on the reverse-engineering notes.

## Commit Messages

Use these commit-message rules:

- Start with a capitalized imperative summary. Aim for roughly 50 characters;
  treat that as a target rather than a hard limit.
- Do not end the summary with a period. Write it so that it completes: “If
  applied, this commit will …”
- When a body is useful, separate it from the summary with a blank line.
- Wrap body text at about 72 characters. Use separate paragraphs for separate
  ideas.
- Explain the change and its motivation. Put details that do not fit in the
  concise summary in the body.
- Bullets are acceptable. Separate them clearly and use a hanging indent when
  a bullet wraps.
