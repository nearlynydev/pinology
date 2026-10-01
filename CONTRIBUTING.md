# Contributing to Pinology

English and Russian issues and pull requests are welcome. Start with an issue
for a new board, device model, storage format or change to deployment behavior.
This is an experimental emulator, not an official Synology or QEMU product.

## Development

Use Python 3.11+, a C toolchain for emulator changes, and a disposable VM.

```sh
python3 scripts/test.py
python3 scripts/check-public.py
```

Do not use real NAS data for tests. Keep changes focused, add regression tests,
and describe host architecture, accelerator, model, DSM build and what was
actually verified. Unit tests do not establish hardware or package support.
Changes to device registers need device-level tests and a documented basis for
the behavior. Guest changes must never conceal failed services or disk errors.

Retain the stock-media hash allowlist, exclusive instance locks, no-overwrite
creation, loopback networking and explicit accelerator selection. No silent
TCG fallback, automatic host kernel changes or forced guest power-off. Discuss
any changes to these boundaries before implementation.

Update both English and Russian documentation for user-visible changes. Do not
add public CI steps that download or boot DSM; emulator-only synthetic tests
and builds are appropriate for CI.

## Provenance, licensing and AI assistance

Pinology was developed with substantial AI assistance. Contributions may use
AI tools, but the contributor must review the result, explain its provenance,
test it, and disclose material AI-generated code in the PR. Do not invent test
results, authorship, copyright notices or upstream endorsements. No generated
serial numbers, firmware dumps, stolen identities or license-bypass services.

Contribute under the applicable license in [NOTICE](NOTICE.md). Keep upstream
notices intact. Use a `Signed-off-by` only if you can truthfully make the
[Developer Certificate of Origin](https://developercertificate.org/).
The public account name or a consistent pseudonym is acceptable here; do not
publish a private email inadvertently.

QEMU has a separate [AI contribution policy](https://www.qemu.org/docs/master/devel/code-provenance.html#use-of-ai-generated-content).
Acceptance here is **not** approval to submit the same code upstream. Never
conceal AI assistance or remove attribution to evade another project's rules.

## Privacy

Do not upload PAT files, proprietary executables, serial numbers, OTP dumps,
VM disks, credentials, private IP addresses or unredacted console logs. A
stock boot log may contain the serial number. Describe the failure or attach
a minimal sanitized excerpt instead. Use private reporting for vulnerabilities.
