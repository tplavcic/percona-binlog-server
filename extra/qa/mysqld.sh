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

# Private MySQL server for the layer D (MySQL) property tests. It is a
# throw-away instance: data, socket and logs live in $MYSQLD_DIR (inside
# WORKDIR), it listens on 127.0.0.1:$MYSQLD_PORT only, and the tests reset
# its binary logs and GTIDs. A system MySQL server is never touched.
#
#   extra/qa/mysqld.sh [--replica] start    # initialize (first time) and start
#   extra/qa/mysqld.sh [--replica] stop     # stop
#   extra/qa/mysqld.sh [--replica] restart  # stop (graceful) and start
#   extra/qa/mysqld.sh [--replica] crash    # kill -9 and start (crash recovery)
#   extra/qa/mysqld.sh [--replica] status   # exit 0 if it accepts connections
#   extra/qa/mysqld.sh [--replica] env      # print PBT_MYSQL_* variables to 'eval'
#   extra/qa/mysqld.sh [--replica] wipe     # stop and delete the instance
#
# --replica: the second instance ($MYSQLD_REPLICA_DIR, port
# $MYSQLD_REPLICA_PORT, server_id 2) used as a downstream replica of the
# Binlog Server's replication source listener.
#
# mysqld: $MYSQLD_BASEDIR/bin/mysqld, else $WORKDIR/mysql/bin/mysqld, else
# mysqld from PATH or /usr/sbin/mysqld. Settings: MYSQLD_* in pbt_env.sh.

set -euo pipefail
# shellcheck source=extra/qa/pbt_env.sh
source "$(dirname "${BASH_SOURCE[0]}")/pbt_env.sh"

instance=source
server_id=1
if [[ "${1:-}" == "--replica" ]]; then
  shift
  instance=replica
  server_id=2
  MYSQLD_DIR="${MYSQLD_REPLICA_DIR}"
  MYSQLD_PORT="${MYSQLD_REPLICA_PORT}"
fi

data_dir="${MYSQLD_DIR}/data"
run_dir="${MYSQLD_DIR}/run"
tmp_dir="${MYSQLD_DIR}/tmp"
pid_file="${run_dir}/mysqld.pid"
log_file="${run_dir}/mysqld.log"
socket="${run_dir}/mysqld.sock"
init_file="${run_dir}/init.sql"

find_mysqld() {
  local candidate
  for candidate in \
      "${MYSQLD_BASEDIR:+${MYSQLD_BASEDIR}/bin/mysqld}" \
      "${WORKDIR}/mysql/bin/mysqld" \
      "$(command -v mysqld 2>/dev/null || true)" \
      /usr/sbin/mysqld; do
    if [[ -n "${candidate}" && -x "${candidate}" ]]; then
      printf '%s' "${candidate}"
      return 0
    fi
  done
  pbt_die "mysqld not found: set MYSQLD_BASEDIR to a MySQL / Percona Server binary directory, or run extra/qa/setup_pbt_env.sh --with-mysql"
}

is_running() {
  [[ -f "${pid_file}" ]] && kill -0 "$(cat "${pid_file}")" 2>/dev/null
}

port_open() {
  (exec 3<>"/dev/tcp/127.0.0.1/${MYSQLD_PORT}") 2>/dev/null
}

# the server answers with its handshake packet once it accepts connections
accepts_connections() {
  port_open || return 1
  local greeting
  greeting="$(timeout 3 bash -c "exec 3<>/dev/tcp/127.0.0.1/${MYSQLD_PORT}; head -c 64 <&3 | tr -dc '[:print:]'" 2>/dev/null || true)"
  [[ "${greeting}" == *[0-9].[0-9]* ]]
}

fail_start() {
  tail -30 "${log_file}" >&2 || true
  stop
  pbt_die "$1 (log: ${log_file})"
}

server_options() {
  local mysqld="$1" basedir
  basedir="$(cd "$(dirname "${mysqld}")/.." && pwd)"
  printf '%s\n' \
    --no-defaults \
    "--basedir=${basedir}" \
    "--datadir=${data_dir}" \
    "--tmpdir=${tmp_dir}" \
    "--socket=${socket}" \
    "--pid-file=${pid_file}" \
    "--log-error=${log_file}" \
    "--port=${MYSQLD_PORT}" \
    --bind-address=127.0.0.1 \
    --mysqlx=OFF \
    "--server-id=${server_id}" \
    --log-bin=binlog \
    --gtid-mode=ON \
    --enforce-gtid-consistency=ON \
    --binlog-checksum=CRC32 \
    --binlog-format=ROW \
    --secure-file-priv= \
    --innodb-buffer-pool-size=64M \
    --innodb-redo-log-capacity=64M \
    --max-connections=100 \
    --performance-schema=OFF
  # mysqld refuses to run as root without an explicit --user
  if [[ ${EUID} -eq 0 ]]; then
    printf '%s\n' --user=root
  fi
}

start() {
  local mysqld
  mysqld="$(find_mysqld)"
  if is_running; then
    pbt_log "mysqld already running (pid $(cat "${pid_file}"))"
    return 0
  fi
  if port_open; then
    pbt_die "port ${MYSQLD_PORT} on 127.0.0.1 is already in use (another server?); set MYSQLD_PORT"
  fi
  pbt_require_in_workdir "${MYSQLD_DIR}"
  mkdir -p "${run_dir}" "${tmp_dir}"
  # not running: a socket / lock file left by a killed server is stale (the
  # PID in the lock file may belong to another process after a reboot, and
  # mysqld then refuses to start: "Unable to setup unix socket lock file")
  rm -f "${socket}" "${socket}.lock"
  local -a options
  mapfile -t options < <(server_options "${mysqld}")
  if [[ ! -d "${data_dir}/mysql" ]]; then
    pbt_log "initializing ${data_dir} with ${mysqld} ($("${mysqld}" --version 2>/dev/null | head -1))"
    rm -rf "${data_dir}"
    "${mysqld}" "${options[@]}" --initialize-insecure >"${log_file}" 2>&1 ||
      fail_start "mysqld --initialize-insecure failed (an AppArmor profile for /usr/sbin/mysqld can block a data directory outside /var/lib/mysql: use a binary tarball, MYSQLD_BASEDIR)"
  fi
  # the test account (TCP from 127.0.0.1), created / reset at every start
  cat > "${init_file}" <<EOF
CREATE USER IF NOT EXISTS '${MYSQLD_USER}'@'127.0.0.1' IDENTIFIED BY '${MYSQLD_PASSWORD}';
ALTER USER '${MYSQLD_USER}'@'127.0.0.1' IDENTIFIED BY '${MYSQLD_PASSWORD}';
GRANT ALL PRIVILEGES ON *.* TO '${MYSQLD_USER}'@'127.0.0.1' WITH GRANT OPTION;
EOF
  pbt_log "starting mysqld on 127.0.0.1:${MYSQLD_PORT} (log: ${log_file})"
  setsid "${mysqld}" "${options[@]}" "--init-file=${init_file}" \
    >>"${log_file}" 2>&1 < /dev/null &
  local launcher=$!
  for _ in $(seq 1 "${MYSQLD_START_TIMEOUT}"); do
    if is_running && accepts_connections; then
      pbt_log "mysqld ready ($("${mysqld}" --version 2>/dev/null | head -1))"
      return 0
    fi
    if ! kill -0 "${launcher}" 2>/dev/null && ! is_running; then
      fail_start "mysqld exited"
    fi
    sleep 1
  done
  fail_start "mysqld did not accept connections on 127.0.0.1:${MYSQLD_PORT} in ${MYSQLD_START_TIMEOUT}s"
}

# kill -9: the next start runs InnoDB and binlog crash recovery
crash() {
  if is_running; then
    local pid
    pid="$(cat "${pid_file}")"
    pbt_log "killing mysqld (pid ${pid}) with SIGKILL"
    kill -9 "${pid}" 2>/dev/null || true
    for _ in $(seq 1 30); do
      kill -0 "${pid}" 2>/dev/null || break
      sleep 1
    done
  fi
  rm -f "${pid_file}"
}

stop() {
  if is_running; then
    local pid
    pid="$(cat "${pid_file}")"
    pbt_log "stopping mysqld (pid ${pid})"
    kill "${pid}" 2>/dev/null || true
    for _ in $(seq 1 60); do
      kill -0 "${pid}" 2>/dev/null || break
      sleep 1
    done
    kill -9 "${pid}" 2>/dev/null || true
  fi
  rm -f "${pid_file}"
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  restart) stop; start ;;
  crash) crash; start ;;
  status)
    if is_running && accepts_connections; then
      echo "running (pid $(cat "${pid_file}")) 127.0.0.1:${MYSQLD_PORT}"
    else
      echo "not running"; exit 1
    fi ;;
  env)
    if [[ "${instance}" == replica ]]; then
      cat <<EOF
export PBT_MYSQL_REPLICA_PORT='${MYSQLD_PORT}'
EOF
    else
      # PBT_MYSQLD_CTL: how a property restarts / crashes this server
      cat <<EOF
export PBT_MYSQL_HOST='127.0.0.1'
export PBT_MYSQL_PORT='${MYSQLD_PORT}'
export PBT_MYSQL_USER='${MYSQLD_USER}'
export PBT_MYSQL_PASSWORD='${MYSQLD_PASSWORD}'
export PBT_MYSQLD_CTL='$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/mysqld.sh'
export PBT_MYSQLD_WORKDIR='${WORKDIR}'
EOF
    fi
    ;;
  wipe)
    stop
    pbt_require_in_workdir "${MYSQLD_DIR}"
    rm -rf "${MYSQLD_DIR}"
    pbt_log "mysqld instance removed" ;;
  *) sed -n '17,36p' "$0"; exit 2 ;;
esac
