# Security policy

Pinology is experimental. The current `main` branch receives best-effort
security fixes; there is no response-time or production-support guarantee.
Older revisions and upstream dependencies may contain vulnerabilities.

Report suspected vulnerabilities through
[GitHub private vulnerability reporting](https://github.com/nearlynydev/pinology/security/advisories/new).
Do not post credentials, working exploit targets, firmware or guest disks in
public issues. Include a minimal reproduction with synthetic data, the commit,
host OS/CPU and the affected security boundary. No bounty is offered.

## Deployment boundaries

- Never use the only copy of important data. Keep verified cold checkpoints.
- Do not expose the DSM web UI, SMB or QMP/console sockets to the Internet.
  Default HTTP and SMB forwards bind to host loopback; use an SSH tunnel.
  Explicit `--network host --bind-address` exposes selected ports on a host IPv4;
  it is not Docker host networking. Use only a trusted LAN and prefer HTTPS.
  Wildcard `0.0.0.0` includes VPN and other interfaces. Never expose an unconfigured
  installer or publish ports to the internet; see [networking](docs/en/networking.md).
- Unix QMP/console sockets give powerful access to the guest. Protect the
  instance directory and the host account running it.
- Docker requires only a directory containing this VM and, for acceleration,
  `/dev/kvm`. Do not use privileged mode, host networking, a Docker socket mount
  or host block devices. Docker access on the host itself is privileged.
- Guest shutdown is authenticated or performed inside DSM. Killing QEMU or
  `docker stop` without a working graceful-shutdown setup can corrupt data.
- Download checksums pin known inputs; they do not make obsolete DSM releases
  secure. Unknown firmware is intentionally rejected, not automatically trusted.
- Optional serial numbers appear in flash, cached media and stock boot logs.
