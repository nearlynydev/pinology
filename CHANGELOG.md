# Changelog

## Unreleased

- `start --network host` listens on `0.0.0.0` without requiring an IP argument;
  optional `--bind-address` restricts it. Local-only mode remains the default.
- Public `start` defaults to native HTTP/HTTPS/SMB ports 5000/5001/445, with
  independent overrides. Docker retains isolation and unprivileged QEMU ports;
  native Mac launch reports port conflicts without stopping other services.
- Native health/shutdown uses the chosen bind address, with legacy metadata support.
- English/Russian network and update guides. Separate router DHCP/bridging remains
  unimplemented.

## 0.1.0 — 2026-10-01

Initial experimental, source-only public distribution.

- RTD1619B DS223 and experimental DS423 QEMU board/device models.
- Native Apple Silicon HVF and Linux ARM64 Docker/KVM launch paths.
- Explicit experimental GICv2 mode for the CM4-class host path.
- Pinned DSM installation media, one virtual disk, persistent flash and
  model-scoped boot allowlists; no proprietary media redistributed.
- Optional creation-time serial with stock vendor checksum and read-only
  identity validation on later boots.
- Instance locks, bounded guest-reboot supervision, authenticated shutdown and
  cold checkpoints.
- English/Russian guides, contribution/security policies and synthetic CI tests.

Not a stable release: package compatibility, cloud services, Pi 5, physical
peripheral passthrough, power-loss recovery and live migration are not certified.
