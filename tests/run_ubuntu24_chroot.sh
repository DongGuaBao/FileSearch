#!/usr/bin/env bash
# Invoke inside a private mount namespace under WSL:
# unshare --mount --fork bash tests/run_ubuntu24_chroot.sh ROOTFS SOURCE
set -euo pipefail
rootfs=$(realpath "$1")
source_tree=$(realpath "$2")
if [[ "$rootfs" != /home/donggua/filesearch-ubuntu24-root ]]; then
    echo 'Unexpected test rootfs' >&2
    exit 2
fi
if [[ ! -f "$rootfs/etc/os-release" || ! -f "$source_tree/CMakeLists.txt" ]]; then
    echo 'Missing rootfs or source tree' >&2
    exit 2
fi
mount --make-rprivate /
# Give the chroot an explicit root mount, so mountinfo exposes its ext4 root.
mount --bind "$rootfs" "$rootfs"
mkdir -p "$rootfs/proc" "$rootfs/sys" "$rootfs/dev" "$rootfs/source" "$rootfs/home/donggua"
mount -t proc proc "$rootfs/proc"
mount -t sysfs -o ro sysfs "$rootfs/sys"
mount --rbind /dev "$rootfs/dev"
mount --make-rslave "$rootfs/dev"
mount --bind "$source_tree" "$rootfs/source"
mount -o remount,bind,ro "$rootfs/source"
cp -L /etc/resolv.conf "$rootfs/etc/resolv.conf"
cp "$source_tree/tests/ubuntu24.sources.list" "$rootfs/etc/apt/sources.list"
chroot "$rootfs" /bin/bash -lc '
    set -euo pipefail
    . /etc/os-release
    test "$VERSION_ID" = 24.04
    cat /etc/os-release
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential cmake ninja-build python3 libsqlite3-dev nodejs xfsprogs btrfs-progs
    cmake -S /source -B /home/donggua/filesearch-build24 -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build /home/donggua/filesearch-build24 --clean-first
    HOME=/home/donggua ctest --test-dir /home/donggua/filesearch-build24 --output-on-failure
'
# All temporary mounts disappear when this private namespace exits.
