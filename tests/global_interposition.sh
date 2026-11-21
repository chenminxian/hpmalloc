#!/usr/bin/env bash
# Write libhpmalloc.so into /etc/ld.so.preload and take effect globally
# Use with caution!

set -xu

if [ "$(id -u)" -ne 0 ]; then
    echo 'root privilege needed' >&2
    exit 1
fi

LIB="${1:-}"
if [[ -z "${LIB}" || ! -r "${LIB}" ]]; then
    echo "usage: $0 /path/to/libhpmalloc.so" >&2
    exit 1
fi

echo "${LIB}" >>/etc/ld.so.preload && ldconfig

echo -e 'run `systemctl soft-reboot` to restart userspace now? [y/N]:\b'
read -r choice

if [[ "${choice}" == 'y' ]]; then
    systemctl soft-reboot
fi
