# CaaL – Container as a Login

CaaL solves a recurring problem: needing a temporary, clean, isolated environment – to test software, sandbox remote users, or hand someone a shell without touching your host system. Until now, popular solutions were either resource-intensive, complicated to deploy, or simply overkill. CaaL only requires two things: a host machine and an internet connection. One SSH command later, you get a fresh container environment, and it vanishes completely on session exit.

## How It Works

When a user logs in via SSH, their shell is replaced by **CaaLsh** (Container as a Login Shell) – a small C binary that:

1. Reads the configuration and looks up the connecting user
2. Clears the entire environment (no SSH vars, no host state leaks in)
3. Mounts a per-session **overlay filesystem** over the container's rootfs, backed by a tmpfs – writes go there, the base image stays untouched
4. Execs **[crun](https://github.com/containers/crun)** to start the OCI container
5. Optionally enforces a **session timeout** (kills the container after N seconds)
6. On exit, tears down the overlay, wipes the tmpfs, and runs `crun delete` – the host is left exactly as it was

Each session is fully ephemeral: nothing persists between logins.

## Requirements

- Linux host with **overlay filesystem** support (modern kernels)
- [`crun`](https://github.com/containers/crun) – lightweight OCI container runtime
- [`skopeo`](https://github.com/containers/skopeo) + [`umoci`](https://github.com/opencontainers/umoci) – for pulling and unpacking OCI images
- Root access for install and user setup
- `gcc`, `make`

The setup script handles installing all of these automatically on supported distros.

## Installation

```bash
git clone https://github.com/douxxtech/caal
cd caal
sudo bash scripts/setup.sh

# or, one-liner:
curl -sSL caal.douxx.tech/get | sudo bash
```

The setup script will:
- Detect your package manager and install dependencies (Debian/Ubuntu, Fedora/RHEL, CentOS, Arch, openSUSE supported)
- Build and install `caalsh` to `/usr/local/bin/caalsh`
- Register `caalsh` as a valid shell in `/etc/shells`
- Pull a default [busybox](https://hub.docker.com/_/busybox) bundle to `/opt/caal/bundles/default`
- Create a base config at `/etc/caal/caal.toml`

## Creating a CaaL User

Use `newcaal` to set up a new user – it handles the system account, the config entry, and the SSH hardening in one shot:

```bash
# Interactive setup
sudo newcaal bob

# Or use all defaults (busybox bundle, no timeout, enabled)
sudo newcaal bob -y
```

`newcaal` will:
- Create the system user with `caalsh` as their login shell
- Prompt you to set a password
- Write the user's entry in `caal.toml`
- Write a per-user `sshd_config.d` drop-in that disables TCP/agent/X11 forwarding and forces `caalsh` even if the user somehow has a different shell

Once created, the user can log in immediately:

```bash
ssh bob@host
bob@host's password:
/ # id
uid=0(root) gid=0(root) groups=10(wheel)
/ # busybox | head -1
BusyBox v1.37.0 (2024-09-26 21:31:42 UTC) multi-call binary.
```

## Removing a CaaL User

```bash
sudo delcaal bob    # interactive
sudo delcaal bob -y # skip confirmation
```

This removes the system user, the `caal.toml` entry, and the SSH drop-in config.

## Configuration

CaaL's config lives at `/etc/caal/caal.toml`. Every user that should be allowed in **must** have an entry – no entry means access denied, regardless of whether the system account exists.

```toml
[bob]
bundle  = "/opt/caal/bundles/default"  # absolute path to the OCI bundle
timeout = 0                            # session lifetime in seconds, 0 = unlimited
enabled = true                         # set to false to lock out without deleting
```

**Notes:**
- `bundle` must be an absolute path
- `timeout` is enforced via `SIGKILL` – the container is forcibly terminated when it expires
- Setting `enabled = false` is a clean way to temporarily suspend a user's access without removing their account or config

## Custom Bundles

The default bundle uses busybox, but you can point any user at any OCI bundle. To create a bundle from a different image:

```bash
# Pull the image
skopeo copy docker://alpine:latest oci:/tmp/alpine-oci:latest

# Unpack it as an OCI bundle
umoci unpack --image /tmp/alpine-oci:latest /opt/caal/bundles/alpine

# Point a user at it in caal.toml
# bundle = "/opt/caal/bundles/alpine"
```

The bundle directory must follow the [OCI Runtime Bundle spec](https://github.com/opencontainers/runtime-spec/blob/main/bundle.md): a `config.json` and a `rootfs/` directory.

## Security Notes

- CaaLsh **clears the entire environment** before launching the container – nothing from the SSH session leaks in
- The overlay filesystem ensures the container rootfs is **always clean** – a user cannot permanently modify it
- The SSH drop-in written by `newcaal` disables agent forwarding, TCP forwarding, and X11 for the user
- Users are created with `/tmp` as their home directory – they have no persistent home on the host
- CaaLsh must be **setuid root** (or run as root) to perform mounts – the Makefile handles this

## License

CaaL is free software, distributed under the [GPLv3.0](LICENSE).  
Copyright (C) 2026 [douxxtech](https://github.com/douxxtech)