<div align="center">

# CaaL – Container as a Login
[Install](#installation) · [Configuration](#configuration) · [Help](#need-help-) · [Website](https://caal.douxx.tech)

</div>

> CaaL provides disposable SSH login environments using OCI containers. Users connect with a normal SSH client and get a fresh, isolated shell that is destroyed when the session ends. No VMs, no persistent state, and minimal host setup required.

<div align="center">
<img src="https://images.dbo.one/63b19736" alt="demo" width="500">
</div>

## How It Works

CaaL is made of three binaries that work together:

- **`caalsh`** – the login shell. Replaces the user's shell in `/etc/passwd`. Every SSH login goes through it.
- **`caald`** – a background daemon that tracks active sessions and allows sessions managment.
- **`caalctl`** – an admin CLI for inspecting and managing live sessions.

When a user logs in via SSH, `caalsh`:

1. Reads `/etc/caal/caal.toml` and verifies the user is configured and enabled
2. Checks with `caald` that the session limit hasn't been reached
3. Clears the entire environment – no SSH vars or host state leaks into the container
4. Creates a per-session **overlay filesystem** over the container's rootfs, backed by a loop-mounted ext4 image – writes go there, the base image stays untouched
5. Execs **[crun](https://github.com/containers/crun)** to start the OCI container
6. Registers the session with `caald`
7. Optionally enforces a **session timeout** (kills the container after N seconds)
8. On exit, tears down the overlay, wipes the loop image, and unregisters from `caald` – the host is left exactly as it was

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
- `e2fsprogs` – provides `mkfs.ext4` for session disk creation
- `git` – required by the one-liner installer
- Root access for install and user setup
- `gcc`, `make`

The setup script handles installing all of these automatically on supported distros (except `git`).

> [!NOTE]
> `max_sessions` is bounded by your kernel's available loopback devices.
> The default is 8 (`/dev/loop0`–`loop7`). You can raise it with `max_loop=N`
> in your kernel parameters or via `modprobe loop max_loop=N`.

## Installation

```bash
# one-liner (clones to /tmp, runs setup, then cleans up):
curl -sSL caal.douxx.tech/get | sudo bash

# or manually:
git clone https://github.com/douxxtech/caal
cd caal
sudo bash scripts/setup.sh
```

The setup script will:
- Detect your package manager and install dependencies (Debian/Ubuntu, Fedora/RHEL, CentOS, Arch, openSUSE supported)
- Build and install `caalsh`, `caald`, and `caalctl` to `/usr/local/bin/`
- Register `caalsh` as a valid shell in `/etc/shells`
- Install and optionally enable/start the `caald` systemd service
- Pull a default [busybox](https://hub.docker.com/_/busybox) bundle to `/opt/caal/bundles/default`
- Create a base config at `/etc/caal/caal.toml`

## The Daemon (`caald`)

`caald` is a small background daemon that keeps track of active sessions over a Unix socket (`/run/caald.sock`). `caalsh` registers each session with it on login and unregisters on logout.

Its main jobs are:
- Providing an accurate session count so `max_sessions` is enforced reliably
- Giving `caalctl` a live view of running sessions

Managing the daemon:

```bash
systemctl status caald  # check if it's running
systemctl start caald   # start it
systemctl enable caald  # enable on boot
journalctl -u caald     # view logs
```

> [!NOTE]
> `caalsh` has a fallback for when `caald` isn't running, session count is less accurate but keeps logins working.
> Running without `caald` is not recommended for production.

## Managing Sessions (`caalctl`)

`caalctl` talks to `caald` to give you a live view of what's running and let you kill sessions remotely.

```bash
# List all active sessions
caalctl list

# Print the number of active sessions
caalctl count

# Kill a specific session by its container ID
caalctl kill caalsh-1234-1718000000

# Kill all sessions for a user
caalctl killuser bob
```

Example output of `caalctl list`:

```
USERNAME             CONTAINER ID                   PID      STARTED
--------             ------------                   ---      -------
bob                  caalsh-1234-1718000000         1234     2026-01-15 14:23:01
alice                caalsh-5678-1718000120         5678     2026-01-15 14:25:21
```

> [!NOTE]
> `caalctl` requires `caald` to be running. If it can't connect, it will say so.

## Creating a CaaL User

Use `newcaal` to set up a new user – it handles the system account, the config entry, and the SSH hardening in one shot:

```bash
# Interactive setup
sudo newcaal bob

# Or use all defaults (busybox bundle, no timeout, 1GB disk space, enabled)
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

> [!NOTE]
> `uid=0` here is **container root**, not host root. The container runs in an
> isolated namespace – it has no privileges on the host.

## Removing a CaaL User

```bash
sudo delcaal bob     # interactive
sudo delcaal bob -y  # skip confirmation
```

This removes the system user account, the `[bob]` entry from `caal.toml`, and the per-user sshd drop-in at `/etc/ssh/sshd_config.d/caal-bob.conf`. sshd is reloaded automatically.

> [!NOTE]
> `delcaal` does not kill active sessions for the user. If bob is currently logged in,
> his session will run until it ends naturally. Use `caalctl killuser bob` first
> if you want to terminate active sessions immediately.

## Configuration

CaaL's config lives at `/etc/caal/caal.toml`. Every user that should be allowed in **must** have an entry – no entry means access denied, regardless of whether the system account exists.

```toml
# general config
max_sessions = 4                       # max simultaneous sessions across all users

[bob]
bundle  = "/opt/caal/bundles/default"  # absolute path to the OCI bundle
timeout = 0                            # session lifetime in seconds, 0 = unlimited
disk    = 1024                         # session disk size in MB
enabled = true                         # set to false to lock out without deleting
```

**Notes:**
- `max_sessions` is capped by available loopback devices (see [Requirements](#requirements))
- `bundle` must be an absolute path
- `timeout` is enforced via `SIGKILL` – the container is forcibly terminated when it expires
- `disk` sets the session ext4 image size; space is reserved upfront via `fallocate`, so it counts against real disk immediately – not just when written
- `disk` can't be 0 – it'll be replaced by the default (`1024`)
- Setting `enabled = false` blocks new logins for that user without removing their account or config. It does **not** kill active sessions – use `caalctl killuser <user>` for that

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
> Umoci default bundles are very minimal. Before using a bundle in production,
> you likely want to:
> - Fix `/etc/passwd` and `/etc/group` so the container has the users it expects
> - Set correct permissions on `/tmp`, `/var`, and any runtime directories
> - Add timezone data if needed (`/etc/localtime`, `/usr/share/zoneinfo`)
> - Configure resource limits (memory, CPU) in `config.json` under `linux.resources`
> - Update other configurations such as hostname, namespaces, etc.
>
> The OCI config is at `/opt/caal/bundles/<image>/config.json`.

## Creating a Fedora Bundle

This demo walks through building a developer-focused Fedora bundle, creating a CaaL user for it, and showing ephemeral sessions in action.

### 1. Pull and Unpack the Image

```bash
sudo skopeo copy docker://fedora:latest oci:/tmp/fedora-oci:latest
sudo umoci unpack --image /tmp/fedora-oci:latest /opt/caal/bundles/fedora
```

### 2. Chroot to Configure the Bundle

Mount the required pseudo-filesystems, then enter the rootfs:

```bash
ROOTFS="/opt/caal/bundles/fedora/rootfs"
sudo mount --bind /dev "$ROOTFS/dev"
sudo mount --bind /proc "$ROOTFS/proc"
sudo mount --bind /sys "$ROOTFS/sys"
sudo mount --bind /run "$ROOTFS/run"

sudo chroot "$ROOTFS" /bin/bash
```

Tune DNF for faster installs:

```bash
tee /etc/dnf/dnf.conf > /dev/null <<'EOF'
[main]
gpgcheck=True
installonly_limit=3
clean_requirements_on_remove=True
best=True
skip_if_unavailable=True
fastestmirror=True
max_parallel_downloads=20
keepcache=True
deltarpm=False
defaultyes=True
EOF
```

Update and install a small developer toolset:

```bash
dnf update
dnf install \
  bash-completion btop curl nano fd-find fzf \
  gcc gcc-c++ gh git htop iproute jq make \
  neovim openssh-clients procps-ng python3 python3-pip \
  ripgrep rsync strace tar tmux tree unzip \
  util-linux vim wget which zip
```

Customize the shell environment (optional):

```bash
nano /root/.bashrc
```

Then exit the chroot and unmount:

```bash
exit

sudo umount "$ROOTFS/dev"
sudo umount "$ROOTFS/proc"
sudo umount "$ROOTFS/sys"
sudo umount "$ROOTFS/run"
```

### 3. Configure the OCI Bundle

Edit `/opt/caal/bundles/fedora/config.json` and apply these changes:

- **Remove** the network namespace entry from `linux.namespaces` – this gives the container access to the host network (needed for `git`, `gh`, etc.)
- **Set** `hostname` to something like `fedodev`
- **Set** `process.cwd` to `/root`
- **Remove** `CAP_NET_BIND_SERVICE` from capabilities – users shouldn't bind privileged ports
- **Add** `CAP_DAC_OVERRIDE` – required so the container can write files as expected
- **Add** `linux.resources.cpu` and `linux.resources.memory` if you wish to apply cgroups limitations

### 4. Create the CaaL User

```bash
sudo newcaal fedora
# bundle: /opt/caal/bundles/fedora
# timeout: 43200 (12 hours)
# disk: 4096 (4 GB)
```

### 5. Session Demo

Log in and do some real work: authenticate with GitHub, clone a repo, make [a commit](https://github.com/douxxtech/caal/commit/4c2578b44e18faf35e00cadc192d0255504ca009), and push it:

<div align="center"><img src="https://images.dbo.one/af1dc6ee" alt="Session demo" width="500"></div>

Then log out and log back in. The session is fully ephemeral: no `~/.config/gh` auth, no cloned repo, nothing persists:

<div align="center"><img src="https://images.dbo.one/ad39d7b0" alt="Session demo 2" width="500"></div>

Each session starts from the same clean image. The disk, any installed tools, cloned repos, and credentials are all wiped on logout. The next session gets a fresh environment identical to the first.

## Security Notes

- `caalsh` **clears the entire environment** before launching the container – nothing from the SSH session leaks in
- The overlay filesystem ensures the container rootfs is **always clean** – a user cannot permanently modify it
- The SSH drop-in written by `newcaal` disables agent forwarding, TCP forwarding, and X11 for the user
- Users are created with `/tmp` as their home directory – they have no persistent home on the host
- `caalsh` must be **setuid root** (or run as root) to perform mounts – the Makefile handles this

## Need Help ?
Have an issue running CaaL ? Feel free to open a new [issue](https://github.com/douxxtech/caal/issues/new/)

## License

CaaL is free software, distributed under the [GPLv3.0](LICENSE).  
Copyright (C) 2026 [douxxtech](https://github.com/douxxtech)
