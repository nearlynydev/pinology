# Network access

[Русский](../ru/networking.md) · [Documentation](../README.md)

Pinology currently offers two NAT-based access modes on Mac and Linux ARM64.
Neither gives DSM its own address from your router.

| Mode | Reachability | Router DHCP lease for DSM |
| --- | --- | --- |
| `--network local` (default) | This host only, or an SSH tunnel | No |
| `--network host --bind-address HOST_IPV4` | Selected ports on the host's IPv4 address | No |
| Bridged / direct LAN | Not implemented in the public CLI | Would be required for a separate router address |

`host` here means **using the host's address**, not Docker `--network host`.
Linux containers keep their isolated network namespace. No bridge, host interface,
router, firewall or VPN configuration is modified by Pinology. Docker manages
its normal port-publishing rules; see [Docker's port-publishing documentation](https://docs.docker.com/engine/network/port-publishing/).

## Access via the Mac/Pi address

Find the IPv4 address of the host on your trusted LAN in its network settings
or your router's device list. It must belong to this host, not be an unused
address you want to assign to DSM. Replace `HOST_IPV4` below:

```sh
./pinology start instances/nas --network host --bind-address HOST_IPV4 \
  --port 15504 --https-port 15505 --smb-port 14445
```

Keep `--experimental-ds423` and/or `--experimental-gicv2` if your model/host
requires them. From another LAN device, use:

- Web: `https://HOST_IPV4:15505/` (guest TCP 5001).
- HTTP/setup: `http://HOST_IPV4:15504/` (guest TCP 5000).
- SMB: `smb://HOST_IPV4:14445/SHARE` (guest TCP 445); create the shared folder in DSM.

`--https-port` is optional and does not configure TLS inside DSM. HTTPS becomes
usable when DSM enables its HTTPS service; the installer may provide only HTTP.
Use a trusted certificate and verify its identity rather than blindly bypassing
certificate warnings. Prefer HTTPS for credentials on LAN. If DSM's automatic
redirect points to guest port 5001, open the explicit forwarded HTTPS URL above.
SMB clients that cannot specify a nonstandard port cannot use this mapping directly.

Host ports must be distinct, unused and in 1024..65535. No root or privileged
container is needed for these forwards. Only the selected HTTP/SMB/optional HTTPS
ports are published, not every package port; broadcast discovery is not forwarded.
Do not set the host IP as DSM's guest interface address: leave the guest's NAT
network configuration unchanged. Reserve the **host's** DHCP address on your router
if you need a stable URL. If that address changes, stop DSM safely and restart
with the new bind address; a specific bind does not silently fall back to all interfaces.

To deliberately listen on every IPv4 interface, specify `--network host
--bind-address 0.0.0.0`. This includes VPN and other reachable interfaces, not just
your LAN; connect using the actual host IP, never `0.0.0.0`. Prefer a specific LAN
address. Do not configure internet port forwarding or expose an unconfigured DSM
installer on an untrusted network. Start initial setup in local mode when possible.

## Change modes or return to local-only

Shut down in DSM and wait for the launcher/container to exit. Restart the **same**
instance with the desired options; do not run `init` or change its disk/flash.
Options apply to each launch and are not persisted as a new default.

```sh
./pinology start instances/nas --network local
```

Local mode binds only `127.0.0.1`; `--bind-address` cannot silently widen it.
`./pinology status` and authenticated `./pinology stop` continue to work with the
selected bind address (inside the container network on Linux). See [usage](usage.md).

## Existing builds and troubleshooting

Existing Mac bundles need the new runtime: follow [Updating Pinology](updates.md#updating-pinology).
Rebuild the Docker image as well to use the current runtime consistently. No new
DSM installation is required. A bind error usually means the address is not assigned
to the host or the port is in use; inspect the launcher and QEMU logs.

If host-local access works but another device cannot connect, check the chosen
address, host firewall, Wi-Fi client isolation, VLAN routing and Docker publishing.
Pinology does not automatically open firewall rules. A successful local request
does not prove remote LAN reachability. Separate router DHCP requires a future
bridged/direct-LAN backend with host-specific privilege and adapter handling;
this feature does not claim to implement that backend.
