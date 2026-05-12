<div align="center">

# CaaL – Container as a Login
[Install](#installation) · [Configuration](#configuration) · [Help](#need-help-) · [Website](https://caal.douxx.tech)

</div>

> CaaL provides disposable SSH login environments using OCI containers. Users connect with a normal SSH client and get a fresh, isolated shell that is destroyed when the session ends. No VMs, no persistent state, and minimal host setup required.

## How It Works

When a user logs in via SSH, their shell is replaced by **CaaLsh** (Container as a Login Shell) – a small C binary that:

1. Clears the entire environment (no SSH vars, no host state leaks in)
2. Mounts a per-session **overlay filesystem** over the container's rootfs, backed by a loop-mounted ext4 image – writes go there, the base image stays untouched
3. Execs **[crun](https://github.com/containers/crun)** to start the OCI container
4. Optionally enforces a **session timeout** (kills the container after N seconds)
5. On exit, tears down the overlay, and wipes the loop image – the host is left exactly as it was

Each session is fully ephemeral: nothing persists between logins.

## Use Cases

CaaL is useful for:
- disposable SSH lab environments
- student shells
- honeypots
- shared demo systems
- temporary contractor access
- isolated CI/debug environments
- SSH access without persistent host accounts

## Requirements

> [!WARNING]
> `caalsh` requires root privileges (setuid or direct root execution)
> to perform mounts and launch containers.

- Linux host with **overlay filesystem** support (modern kernels)
- [`crun`](https://github.com/containers/crun) – lightweight OCI container runtime
- [`skopeo`](https://github.com/containers/skopeo) + [`umoci`](https://github.com/opencontainers/umoci) – for pulling and unpacking OCI images
- Root access for install and user setup
- `gcc`, `make`

The setup script handles installing all of these automatically on supported distros.

## Installation

```bash
# one-liner:
curl -sSL caal.douxx.tech/get | sudo bash

# or
git clone https://github.com/douxxtech/caal
cd caal
sudo bash scripts/setup.sh
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
disk    = 1024                         # session disk size in MB
enabled = true                         # set to false to lock out without deleting
```

**Notes:**
- `bundle` must be an absolute path
- `timeout` is enforced via `SIGKILL` – the container is forcibly terminated when it expires
- `disk` cant be 0 – it'll be replaced by the default (`1024`)
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

> [!NOTE]
> Umoci default bundles have a very minimal setup. It is recommended to heavily modify them to ensure correct permissions (in the container and on the host), and resources management.  
> The OCI config file is the following: `/opt/caal/bundles/<image>/config.json`

## Security Notes

- CaaLsh **clears the entire environment** before launching the container – nothing from the SSH session leaks in
- The overlay filesystem ensures the container rootfs is **always clean** – a user cannot permanently modify it
- The SSH drop-in written by `newcaal` disables agent forwarding, TCP forwarding, and X11 for the user
- Users are created with `/tmp` as their home directory – they have no persistent home on the host
- CaaLsh must be **setuid root** (or run as root) to perform mounts – the Makefile handles this

## Need Help ?
Have an issue running CaaL ? Feel free to open a new [issue](https://github.com/douxxtech/caal/issues/new/)

## License

CaaL is free software, distributed under the [GPLv3.0](LICENSE).  
Copyright (C) 2026 [douxxtech](https://github.com/douxxtech)