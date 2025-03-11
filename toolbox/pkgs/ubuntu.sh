#!/usr/bin/env bash
# Query the linker version
ld -v || true

# Query the (g)libc version
ldd --version || true

# Unattended update, upgrade, and install
export DEBIAN_FRONTEND=noninteractive
export DEBIAN_PRIORITY=critical
apt-get -qy update
#apt-get -qy \
#  -o "Dpkg::Options::=--force-confdef" \
#  -o "Dpkg::Options::=--force-confold" upgrade
apt-get -qy --no-install-recommends install apt-utils
apt-get -qy autoclean
apt-get -qy install \
 meson \
 pipx \
 xfslibs-dev \
 xfsprogs

pipx install cijoe
pipx ensurepath
