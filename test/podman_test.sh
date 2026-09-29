#!/bin/bash
#
# Run the full build/test/package cycle (autogen, configure, make check,
# make distcheck, make rpm) inside a throwaway Fedora podman container, so
# it can be run locally without polluting the host with build dependencies.
#
# Usage: test/podman_test.sh [--clean|-c] [image]
#   --clean, -c   remove build artifacts left behind by a previous run
#                 (rpmbuild-out/, libstoragemgmt-golang/,
#                 libstoragemgmt-rust/) instead of running the container
#   image         defaults to "fedora:latest"

set -e

TOP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RPMBUILD_DIR="${TOP_DIR}/rpmbuild-out"

if [ "$1" = "--clean" ] || [ "$1" = "-c" ]; then
    rm -rf "${RPMBUILD_DIR}" \
        "${TOP_DIR}/libstoragemgmt-golang" \
        "${TOP_DIR}/libstoragemgmt-rust"
    exit 0
fi

IMAGE="${1:-fedora:latest}"

# rpmbuild -ta writes into ~root/rpmbuild inside the container; bind-mount
# that so the resulting RPMs/SRPMs survive after the container is removed.
mkdir -p "${RPMBUILD_DIR}"

exec podman run --rm \
    -v "${TOP_DIR}:/libstoragemgmt-code:rw,z" \
    -v "${RPMBUILD_DIR}:/root/rpmbuild:rw,z" \
    -w /libstoragemgmt-code \
    "${IMAGE}" \
    /bin/bash -c "./test/docker_ci_test.sh"
