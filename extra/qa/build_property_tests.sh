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
#                                    [--all] [--mysql-client-root DIR]
#                                    [-- <extra cmake args>]
#
#   --preset P       CMake preset (default: $BUILD_PRESET or release_gcc14)
#   --mysql-client-root DIR
#                    build against the MySQL client of a binary tarball
#                    (DIR/include/mysql.h, DIR/lib/libmysqlclient.so or
#                    libperconaserverclient.so)
#                    instead of the system libmysqlclient-dev (default:
#                    $MYSQL_CLIENT_ROOT; without it and without
#                    /usr/include/mysql/mysql.h: $MYSQLD_BASEDIR, then
#                    $WORKDIR/mysql)
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
    --mysql-client-root) MYSQL_CLIENT_ROOT="$2"; shift ;;
    --mysql-client-root=*) MYSQL_CLIENT_ROOT="${1#*=}" ;;
    --clean) clean=1 ;;
    --all) build_all=1 ;;
    --) shift; extra_args=("$@"); break ;;
    -h|--help) sed -n '/^# Configures/,/^# which run_property/p' "$0"; exit 0 ;;
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

# MySQL client from a binary tarball (MYSQL_CLIENT_ROOT): the tarball has
# include/mysql.h, the sources include <mysql/mysql.h>, so a directory with
# a 'mysql' link to the tarball's include/ becomes the include directory.
# Both cache entries are passed on every configure, so a value cached
# earlier (e.g. a removed /usr/include/mysql) does not survive.
mysql_args=()
mysql_client_root="${MYSQL_CLIENT_ROOT}"
if [[ -z "${mysql_client_root}" && ! -f /usr/include/mysql/mysql.h ]]; then
  for candidate in "${MYSQLD_BASEDIR}" "${WORKDIR}/mysql"; do
    if [[ -n "${candidate}" && -f "${candidate}/include/mysql.h" ]]; then
      mysql_client_root="${candidate}"
      break
    fi
  done
  [[ -n "${mysql_client_root}" ]] \
    || pbt_die "no MySQL client headers: /usr/include/mysql/mysql.h is missing; set MYSQL_CLIENT_ROOT (or --mysql-client-root) to a MySQL / Percona Server tarball directory"
fi
if [[ -n "${mysql_client_root}" ]]; then
  mysql_client_root="$(cd "${mysql_client_root}" && pwd)"
  [[ -f "${mysql_client_root}/include/mysql.h" ]] \
    || pbt_die "${mysql_client_root}/include/mysql.h not found (MYSQL_CLIENT_ROOT)"
  mysql_library=""
  # Percona Server tarballs name it libperconaserverclient
  for candidate in "${mysql_client_root}/lib/libmysqlclient.so" \
                   "${mysql_client_root}/lib/libperconaserverclient.so" \
                   "${mysql_client_root}/lib/libmysqlclient.a" \
                   "${mysql_client_root}/lib/libperconaserverclient.a"; do
    if [[ -e "${candidate}" ]]; then
      mysql_library="${candidate}"
      break
    fi
  done
  [[ -n "${mysql_library}" ]] \
    || pbt_die "no libmysqlclient / libperconaserverclient (.so / .a) in ${mysql_client_root}/lib (MYSQL_CLIENT_ROOT)"
  mysql_include_dir="${WORKDIR}/mysql-client-include"
  pbt_require_in_workdir "${mysql_include_dir}"
  mkdir -p "${mysql_include_dir}"
  ln -sfn "${mysql_client_root}/include" "${mysql_include_dir}/mysql"
  pbt_log "MySQL client: ${mysql_library} (headers ${mysql_client_root}/include)"
  # src/minimysql/connection_context.cpp includes <mysql/mysqld_error.h>
  # but the binlog_server target does not link MySQL::client, so only a
  # system-wide /usr/include/mysql works there (todo.md T9): the directory
  # is added to every C++ compile instead
  mysql_args=(
    "-DMYSQL_INCLUDE_DIR=${mysql_include_dir}"
    "-DMYSQL_LIBRARY=${mysql_library}"
    "-DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES=${mysql_include_dir}"
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
  "${mysql_args[@]}" \
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
