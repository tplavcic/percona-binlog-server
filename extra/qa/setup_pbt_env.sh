#!/usr/bin/env bash
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

# Prepares WORKDIR for the property-based tests. Idempotent: every step is
# skipped when its result is already present.
#
#   extra/qa/setup_pbt_env.sh [--no-seaweedfs] [--with-mysql] [--force]
#
# Steps:
#   1. checks the main build prerequisites from README.md (Boost and AWS SDK
#      installed for $BUILD_PRESET)
#   2. downloads libhegel (Hegel native engine) and verifies its SHA-256
#   3. clones hegel-cpp v$HEGEL_CPP_VERSION, builds and installs it into
#      $HEGEL_INSTALL_DIR
#   4. downloads the SeaweedFS 'weed' binary (S3 endpoint for S3 tests)
#   5. reports the mysqld the MySQL (layer D) tests will use; with
#      --with-mysql downloads the MySQL / Percona Server binary tarball
#      $MYSQLD_TARBALL_URL into $WORKDIR/mysql (when no mysqld is usable,
#      e.g. /usr/sbin/mysqld confined by AppArmor)
#
# Nothing is written outside WORKDIR.

set -euo pipefail
# shellcheck source=extra/qa/pbt_env.sh
source "$(dirname "${BASH_SOURCE[0]}")/pbt_env.sh"

with_seaweedfs=1
with_mysql=0
force=0
for arg in "$@"; do
  case "${arg}" in
    --no-seaweedfs) with_seaweedfs=0 ;;
    --with-mysql) with_mysql=1 ;;
    --force) force=1 ;;
    -h|--help) sed -n '17,35p' "$0"; exit 0 ;;
    *) pbt_die "unknown argument: ${arg}" ;;
  esac
done

download() { # url destination
  pbt_require_in_workdir "$2"
  curl --fail --location --silent --show-error --retry 3 -o "$2" "$1"
}

# --------------------------------------------------------------------------
pbt_log "WORKDIR=${WORKDIR} BUILD_PRESET=${BUILD_PRESET}"
for dep in "boost-install-${BUILD_PRESET}" "aws-sdk-cpp-install-${BUILD_PRESET}"; do
  if [[ -d "${WORKDIR}/${dep}" ]]; then
    pbt_log "found ${dep}"
  else
    pbt_log "WARNING: ${WORKDIR}/${dep} is missing - build it as described in README.md"
  fi
done
for tool in cmake git curl python3 "${PBT_CXX}"; do
  command -v "${tool}" >/dev/null || pbt_die "required tool not found: ${tool}"
done

# --------------------------------------------------------------------------
libhegel_dir="${WORKDIR}/libhegel"
libhegel_file="${libhegel_dir}/libhegel-${LIBHEGEL_VERSION}-linux-amd64.so"
pbt_require_in_workdir "${libhegel_dir}"
if [[ ${force} -eq 1 || ! -f "${libhegel_file}" ]]; then
  [[ "$(uname -m)" == "x86_64" ]] || pbt_die "only linux/amd64 is scripted; see https://hegel.dev/reference/installation"
  mkdir -p "${libhegel_dir}"
  pbt_log "downloading libhegel ${LIBHEGEL_VERSION}"
  download "https://github.com/hegeldev/hegel-rust/releases/download/libhegel-v${LIBHEGEL_VERSION}/libhegel-linux-amd64.so" \
    "${libhegel_file}.part"
  echo "${LIBHEGEL_SHA256}  ${libhegel_file}.part" | sha256sum --check --status \
    || { rm -f "${libhegel_file}.part"; pbt_die "libhegel checksum mismatch"; }
  mv "${libhegel_file}.part" "${libhegel_file}"
else
  pbt_log "libhegel ${LIBHEGEL_VERSION} already present"
fi

# --------------------------------------------------------------------------
hegel_src="${WORKDIR}/hegel-cpp"
hegel_build="${WORKDIR}/hegel-build"
hegel_stamp="${HEGEL_INSTALL_DIR}/.pbs-hegel-${HEGEL_CPP_VERSION}-${LIBHEGEL_VERSION}-$(basename "${PBT_CXX}")"
pbt_require_in_workdir "${hegel_src}"
pbt_require_in_workdir "${HEGEL_INSTALL_DIR}"
if [[ ${force} -eq 1 || ! -f "${hegel_stamp}" ]]; then
  if [[ ! -d "${hegel_src}/.git" ]]; then
    pbt_log "cloning hegel-cpp v${HEGEL_CPP_VERSION}"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "v${HEGEL_CPP_VERSION}" \
      https://github.com/hegeldev/hegel-cpp.git "${hegel_src}"
  else
    pbt_log "updating hegel-cpp checkout to v${HEGEL_CPP_VERSION}"
    git -C "${hegel_src}" fetch --quiet --depth 1 origin "refs/tags/v${HEGEL_CPP_VERSION}:refs/tags/v${HEGEL_CPP_VERSION}"
    git -C "${hegel_src}" -c advice.detachedHead=false checkout --quiet "v${HEGEL_CPP_VERSION}"
  fi
  rm -rf "${hegel_build}"
  pbt_log "building hegel-cpp with ${PBT_CXX}"
  # HEGEL_REFLECTION=OFF: no reflect-cpp dependency (no extra download)
  cmake -S "${hegel_src}" -B "${hegel_build}" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER="${PBT_CXX}" \
    -DCMAKE_INSTALL_PREFIX="${HEGEL_INSTALL_DIR}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DHEGEL_REFLECTION=OFF \
    -DHEGEL_BUILD_TESTS=OFF \
    -DHEGEL_LIBHEGEL_LIBRARY="${libhegel_file}" >/dev/null
  cmake --build "${hegel_build}" --parallel "${PBT_JOBS}" >/dev/null
  cmake --install "${hegel_build}" >/dev/null
  touch "${hegel_stamp}"
  pbt_log "hegel-cpp installed into ${HEGEL_INSTALL_DIR}"
else
  pbt_log "hegel-cpp ${HEGEL_CPP_VERSION} already installed"
fi

# --------------------------------------------------------------------------
if [[ ${with_seaweedfs} -eq 1 ]]; then
  weed="${SEAWEEDFS_DIR}/bin/weed"
  pbt_require_in_workdir "${SEAWEEDFS_DIR}"
  if [[ ${force} -eq 1 || ! -x "${weed}" ]] || ! "${weed}" version 2>/dev/null | grep -q " ${SEAWEEDFS_VERSION} "; then
    mkdir -p "${SEAWEEDFS_DIR}/bin" "${SEAWEEDFS_DIR}/download"
    archive="${SEAWEEDFS_DIR}/download/seaweedfs-${SEAWEEDFS_VERSION}-linux_amd64.tar.gz"
    base_url="https://github.com/seaweedfs/seaweedfs/releases/download/${SEAWEEDFS_VERSION}"
    pbt_log "downloading SeaweedFS ${SEAWEEDFS_VERSION}"
    download "${base_url}/linux_amd64.tar.gz" "${archive}"
    download "${base_url}/linux_amd64.tar.gz.md5" "${archive}.md5"
    [[ "$(md5sum "${archive}" | cut -d' ' -f1)" == "$(cut -d' ' -f1 "${archive}.md5")" ]] \
      || pbt_die "SeaweedFS archive checksum mismatch"
    tar -xzf "${archive}" -C "${SEAWEEDFS_DIR}/bin" weed
    pbt_log "SeaweedFS installed: $("${weed}" version 2>/dev/null | grep -m1 version)"
  else
    pbt_log "SeaweedFS ${SEAWEEDFS_VERSION} already present"
  fi
fi

# --------------------------------------------------------------------------
mysql_dir="${WORKDIR}/mysql"
if [[ ${with_mysql} -eq 1 ]]; then
  pbt_require_in_workdir "${mysql_dir}"
  if [[ ${force} -eq 1 || ! -x "${mysql_dir}/bin/mysqld" ]]; then
    mkdir -p "${WORKDIR}/mysql-download"
    archive="${WORKDIR}/mysql-download/$(basename "${MYSQLD_TARBALL_URL}")"
    pbt_log "downloading ${MYSQLD_TARBALL_URL}"
    download "${MYSQLD_TARBALL_URL}" "${archive}"
    rm -rf "${mysql_dir}" && mkdir -p "${mysql_dir}"
    tar -xzf "${archive}" -C "${mysql_dir}" --strip-components=1
    [[ -x "${mysql_dir}/bin/mysqld" ]] || pbt_die "no bin/mysqld in ${archive}"
  fi
fi
if [[ -n "${MYSQLD_BASEDIR}" && -x "${MYSQLD_BASEDIR}/bin/mysqld" ]]; then
  mysqld_bin="${MYSQLD_BASEDIR}/bin/mysqld"
elif [[ -x "${mysql_dir}/bin/mysqld" ]]; then
  mysqld_bin="${mysql_dir}/bin/mysqld"
else
  mysqld_bin="$(command -v mysqld 2>/dev/null || true)"
  [[ -z "${mysqld_bin}" && -x /usr/sbin/mysqld ]] && mysqld_bin=/usr/sbin/mysqld
fi
if [[ -n "${mysqld_bin}" ]]; then
  pbt_log "MySQL tests will use ${mysqld_bin} ($("${mysqld_bin}" --version 2>/dev/null | head -1))"
else
  pbt_log "WARNING: no mysqld found - the MySQL properties will report an infrastructure error (use --with-mysql or MYSQLD_BASEDIR, or run_property_tests.py --mysql off)"
fi

pbt_log "done. Next: extra/qa/build_property_tests.sh"
