# Copyright (c) 2026 Percona and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# Common settings for the property-based-testing (PBT) helper scripts.
# Sourced by the other extra/qa/*.sh scripts; every value can be overridden
# from the environment.
#
# Layout (everything stays inside WORKDIR):
#   $WORKDIR/percona-binlog-server                    - this repository
#   $WORKDIR/boost-install-$BUILD_PRESET              - per README.md
#   $WORKDIR/aws-sdk-cpp-install-$BUILD_PRESET        - per README.md
#   $WORKDIR/hegel-cpp, hegel-build, hegel-install    - setup_pbt_env.sh
#   $WORKDIR/libhegel                                 - setup_pbt_env.sh
#   $WORKDIR/seaweedfs                                - setup_pbt_env.sh
#   $WORKDIR/percona-binlog-server-build-<variant>-pbt
#                                                     - build_property_tests.sh
#     <variant> = $BUILD_PRESET plus extra sanitizers, e.g. release_gcc14,
#                 asan_gcc14, asan_gcc14-ubsan (see pbt_variant below)
#   $WORKDIR/testruns/<timestamp>-<variant>           - run_property_tests.py
#   $WORKDIR/testruns/hegel-db                        - Hegel example database

PBT_QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PBT_REPO_DIR="$(cd "${PBT_QA_DIR}/../.." && pwd)"

: "${WORKDIR:=$(cd "${PBT_REPO_DIR}/.." && pwd)}"
: "${BUILD_PRESET:=release_gcc14}"
: "${PBT_SANITIZE:=}"   # extra sanitizers, e.g. "undefined" (see build_property_tests.sh)
: "${PBT_JOBS:=$(nproc 2>/dev/null || echo 4)}"

# hegel-cpp (C++ front-end) and libhegel (native engine) versions;
# libhegel version and checksum must match what hegel-cpp expects
# (see cmake/libhegel.cmake in hegel-cpp)
: "${HEGEL_CPP_VERSION:=0.13.1}"
: "${LIBHEGEL_VERSION:=0.42.4}"
: "${LIBHEGEL_SHA256:=60fa1b48f83def125d0d6904bf4351c0022572b52be252ded756aab9e0990682}"
: "${HEGEL_INSTALL_DIR:=${WORKDIR}/hegel-install}"

# SeaweedFS (S3-compatible endpoint for S3 storage tests)
: "${SEAWEEDFS_VERSION:=4.48}"
: "${SEAWEEDFS_DIR:=${WORKDIR}/seaweedfs}"
: "${SEAWEEDFS_S3_PORT:=8333}"
: "${SEAWEEDFS_MASTER_PORT:=9333}"
: "${SEAWEEDFS_VOLUME_PORT:=9340}"
: "${SEAWEEDFS_FILER_PORT:=8888}"
: "${SEAWEEDFS_ACCESS_KEY:=pbsqaaccess}"
: "${SEAWEEDFS_SECRET_KEY:=pbsqasecret}"
: "${SEAWEEDFS_BUCKET:=pbs-pbt}"
: "${SEAWEEDFS_START_TIMEOUT:=120}"
# small volumes and an explicit volume count: 'weed mini' otherwise derives
# the number of volumes from the free disk space and can end up with none
# (QA host: 4 GiB free -> "Free Volumes: 0", bucket never accessible)
: "${SEAWEEDFS_VOLUME_SIZE_MB:=64}"
: "${SEAWEEDFS_VOLUME_MAX:=8}"

# compiler used to build hegel-cpp (must be ABI compatible with BUILD_PRESET)
case "${BUILD_PRESET}" in
  *clang20) : "${PBT_CXX:=clang++-20}" ;;
  *)        : "${PBT_CXX:=g++-14}" ;;
esac

pbt_log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*" >&2; }
pbt_die() { pbt_log "ERROR: $*"; exit 1; }

# refuse to touch anything outside WORKDIR
pbt_require_in_workdir() {
  local path
  path="$(realpath -m "$1")"
  case "${path}/" in
    "$(realpath -m "${WORKDIR}")"/*) ;;
    *) pbt_die "refusing to write outside WORKDIR (${WORKDIR}): ${path}" ;;
  esac
}

# Build variant name: the preset plus a suffix per extra sanitizer
# (undefined -> ubsan, address -> asan, thread -> tsan, other -> its name).
# Used for the build directory and to label reports (junit suite names).
pbt_variant() {
  local sanitizers="$2" variant="$1" san
  local IFS=','
  for san in ${sanitizers}; do
    case "${san}" in
      undefined) variant+="-ubsan" ;;
      address) variant+="-asan" ;;
      thread) variant+="-tsan" ;;
      "") ;;
      *) variant+="-${san}" ;;
    esac
  done
  printf '%s' "${variant}"
}

pbt_build_dir() { # variant
  printf '%s' "${WORKDIR}/percona-binlog-server-build-$1-pbt"
}
