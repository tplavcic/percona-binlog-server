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
# separate build directory per build variant
# ($WORKDIR/percona-binlog-server-build-<variant>-pbt, override with
# $PBT_BUILD_DIR), so the regular preset build directory is left untouched.
#
#   extra/qa/build_property_tests.sh [--preset P] [--sanitize LIST] [--clean]
#                                    [--all] [-- <extra cmake args>]
#
#   --preset P       CMake preset (default: $BUILD_PRESET or release_gcc14)
#   --sanitize LIST  extra -fsanitize= checks on top of the preset, comma
#                    separated (default: $PBT_SANITIZE), e.g. "undefined";
#                    the variant name gets a suffix (undefined -> -ubsan)
#   --clean          remove the build directory first
#   --all            build every target (default: binlog_server + property
#                    tests)
#
# Examples:
#   extra/qa/build_property_tests.sh --preset asan_gcc14
#   extra/qa/build_property_tests.sh --preset release_gcc14 --sanitize undefined
#
# The build directory gets pbt_build_info.json (variant, preset, sanitizers)
# which run_property_tests.py uses to label its reports.

set -euo pipefail
# shellcheck source=extra/qa/pbt_env.sh
source "$(dirname "${BASH_SOURCE[0]}")/pbt_env.sh"

clean=0
build_all=0
extra_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset) BUILD_PRESET="$2"; shift ;;
    --preset=*) BUILD_PRESET="${1#*=}" ;;
    --sanitize) PBT_SANITIZE="$2"; shift ;;
    --sanitize=*) PBT_SANITIZE="${1#*=}" ;;
    --clean) clean=1 ;;
    --all) build_all=1 ;;
    --) shift; extra_args=("$@"); break ;;
    -h|--help) sed -n '17,38p' "$0"; exit 0 ;;
    *) pbt_die "unknown argument: $1" ;;
  esac
  shift
done

variant="$(pbt_variant "${BUILD_PRESET}" "${PBT_SANITIZE}")"
: "${PBT_BUILD_DIR:=$(pbt_build_dir "${variant}")}"

hegel_dir="${HEGEL_INSTALL_DIR}/lib/cmake/hegel"
[[ -f "${hegel_dir}/hegelConfig.cmake" ]] \
  || pbt_die "hegel-cpp not installed in ${HEGEL_INSTALL_DIR}; run extra/qa/setup_pbt_env.sh"
for dep in "boost-install-${BUILD_PRESET}" "aws-sdk-cpp-install-${BUILD_PRESET}"; do
  [[ -d "${WORKDIR}/${dep}" ]] \
    || pbt_die "${WORKDIR}/${dep} is missing - build it for preset ${BUILD_PRESET} as described in README.md"
done

sanitizer_args=()
if [[ -n "${PBT_SANITIZE}" ]]; then
  # recoverable: UBSan reports each location once and the property keeps
  # running; run_property_tests.py turns any report into a failure
  san_flags="-fsanitize=${PBT_SANITIZE} -fno-omit-frame-pointer"
  case ",${PBT_SANITIZE}," in
    *,undefined,*)
      # g++-14 reports a -Wconversion false positive in instrumented code
      # (src/binsrv/gtids/gtid_set.cpp, 'result <<= 4U' on uint8_t) which
      # -Werror turns into a build failure; keep it a warning (todo.md T5)
      if [[ "${PBT_CXX}" == g++* ]]; then
        san_flags+=" -Wno-error=conversion"
      fi ;;
  esac
  case ",${PBT_SANITIZE}," in
    *,thread,*)
      pbt_log "WARNING: ThreadSanitizer needs Boost / AWS SDK built with -fsanitize=thread too, otherwise expect false positives (todo.md T6)" ;;
  esac
  sanitizer_args=(
    "-DCMAKE_CXX_FLAGS=${san_flags}"
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=${PBT_SANITIZE}"
  )
fi

pbt_require_in_workdir "${PBT_BUILD_DIR}"
if [[ ${clean} -eq 1 ]]; then
  pbt_log "removing ${PBT_BUILD_DIR}"
  rm -rf "${PBT_BUILD_DIR}"
fi

pbt_log "configuring variant ${variant} (preset ${BUILD_PRESET}${PBT_SANITIZE:+, sanitize=${PBT_SANITIZE}}) in ${PBT_BUILD_DIR}"
cmake -S "${PBT_REPO_DIR}" -B "${PBT_BUILD_DIR}" --preset "${BUILD_PRESET}" \
  -DWITH_PROPERTY_TESTS=ON \
  -DPBS_HEGEL_FETCH=OFF \
  -Dhegel_DIR="${hegel_dir}" \
  "${sanitizer_args[@]}" \
  "${extra_args[@]}"

# label used by run_property_tests.py for reports / junit suite names
cat > "${PBT_BUILD_DIR}/pbt_build_info.json" <<EOF
{
  "variant": "${variant}",
  "preset": "${BUILD_PRESET}",
  "sanitizers": "${PBT_SANITIZE}",
  "git_revision": "$(git -C "${PBT_REPO_DIR}" rev-parse HEAD 2>/dev/null || true)"
}
EOF

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
pbt_log "done. Next: extra/qa/run_property_tests.py --preset ${BUILD_PRESET}${PBT_SANITIZE:+ --sanitize ${PBT_SANITIZE}}"
