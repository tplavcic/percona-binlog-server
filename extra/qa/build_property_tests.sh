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

# Configures and builds percona-binlog-server with -DWITH_PROPERTY_TESTS=ON
# using the CMake preset $BUILD_PRESET (default release_gcc14) into a
# separate build directory ($PBT_BUILD_DIR, default
# $WORKDIR/percona-binlog-server-build-$BUILD_PRESET-pbt), so the regular
# preset build directory is left untouched.
#
#   extra/qa/build_property_tests.sh [--clean] [--all] [-- <extra cmake args>]
#
#   --clean  remove the build directory first
#   --all    build every target (default: binlog_server + property tests)

set -euo pipefail
# shellcheck source=extra/qa/pbt_env.sh
source "$(dirname "${BASH_SOURCE[0]}")/pbt_env.sh"

clean=0
build_all=0
extra_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean) clean=1 ;;
    --all) build_all=1 ;;
    --) shift; extra_args=("$@"); break ;;
    -h|--help) sed -n '17,27p' "$0"; exit 0 ;;
    *) pbt_die "unknown argument: $1" ;;
  esac
  shift
done

hegel_dir="${HEGEL_INSTALL_DIR}/lib/cmake/hegel"
[[ -f "${hegel_dir}/hegelConfig.cmake" ]] \
  || pbt_die "hegel-cpp not installed in ${HEGEL_INSTALL_DIR}; run extra/qa/setup_pbt_env.sh"

pbt_require_in_workdir "${PBT_BUILD_DIR}"
if [[ ${clean} -eq 1 ]]; then
  pbt_log "removing ${PBT_BUILD_DIR}"
  rm -rf "${PBT_BUILD_DIR}"
fi

pbt_log "configuring ${PBT_REPO_DIR} (preset ${BUILD_PRESET}) in ${PBT_BUILD_DIR}"
cmake -S "${PBT_REPO_DIR}" -B "${PBT_BUILD_DIR}" --preset "${BUILD_PRESET}" \
  -DWITH_PROPERTY_TESTS=ON \
  -DPBS_HEGEL_FETCH=OFF \
  -Dhegel_DIR="${hegel_dir}" \
  "${extra_args[@]}"

targets=()
if [[ ${build_all} -eq 0 ]]; then
  # binlog_server is needed by the system-level (binary driving) properties
  mapfile -t targets < <(ctest --test-dir "${PBT_BUILD_DIR}" -L property -N 2>/dev/null \
    | sed -n 's/^ *Test *#[0-9]*: *//p')
  targets+=(binlog_server)
fi

pbt_log "building ${targets[*]:-all targets}"
if [[ ${#targets[@]} -gt 0 ]]; then
  cmake --build "${PBT_BUILD_DIR}" --parallel "${PBT_JOBS}" --target "${targets[@]}"
else
  cmake --build "${PBT_BUILD_DIR}" --parallel "${PBT_JOBS}"
fi
pbt_log "done. Next: extra/qa/run_property_tests.py"
