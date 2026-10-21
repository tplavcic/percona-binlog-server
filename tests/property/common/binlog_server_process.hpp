// Copyright (c) 2026 Percona and/or its affiliates.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 2.0,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License, version 2.0, for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

#ifndef PBS_TESTS_PROPERTY_COMMON_BINLOG_SERVER_PROCESS_HPP
#define PBS_TESTS_PROPERTY_COMMON_BINLOG_SERVER_PROCESS_HPP

// Running the binlog_server binary from a property (layers C and D):
// a synchronous run with stdout captured, a background process that can
// be signalled (pull mode, kill -9), and a temporary directory under
// $TMPDIR.
//
// The binary is the binlog_server built next to the test
// (PBS_BINLOG_SERVER_PATH compile definition); PBT_BINLOG_SERVER overrides
// it.

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/pbt_helpers.hpp"

#ifndef PBS_BINLOG_SERVER_PATH
#define PBS_BINLOG_SERVER_PATH ""
#endif

namespace pbt::process {

inline std::string binlog_server_path() {
  if (const auto value{pbt::detail::env("PBT_BINLOG_SERVER")}; value) {
    return *value;
  }
  const std::string built{PBS_BINLOG_SERVER_PATH};
  if (built.empty() || !std::filesystem::exists(built)) {
    pbt::infrastructure_error("binlog_server binary not found (build the "
                              "binlog_server target or set PBT_BINLOG_SERVER)");
  }
  return built;
}

class temporary_directory {
public:
  explicit temporary_directory(const std::string &prefix = "pbs-pbt") {
    std::string pattern{
        (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX"))
            .string()};
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (::mkdtemp(std::data(pattern)) == nullptr) {
      throw std::runtime_error{"mkdtemp failed"};
    }
    path_ = pattern;
  }
  temporary_directory(const temporary_directory &) = delete;
  temporary_directory &operator=(const temporary_directory &) = delete;
  temporary_directory(temporary_directory &&) = delete;
  temporary_directory &operator=(temporary_directory &&) = delete;
  ~temporary_directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

struct run_result {
  bool signaled{false};
  int signal_number{0};
  int exit_code{0};
  std::string output;

  [[nodiscard]] std::string describe() const {
    return signaled ? "killed by signal " + std::to_string(signal_number)
                    : "exit code " + std::to_string(exit_code);
  }
};

namespace detail {

inline std::vector<std::string>
make_argv(const std::vector<std::string> &arguments) {
  std::vector<std::string> result{binlog_server_path()};
  result.insert(result.end(), arguments.begin(), arguments.end());
  return result;
}

// in the child: stdout to 'stdout_fd' (or /dev/null), stderr to /dev/null,
// then exec; never returns. 'file_size_limit': RLIMIT_FSIZE with SIGXFSZ
// ignored, so a write beyond the limit fails (EFBIG) like a write to a full
// disk (ENOSPC) instead of killing the process.
[[noreturn]] inline void exec_child(std::vector<std::string> &argv_storage,
                                    int stdout_fd,
                                    std::optional<std::uint64_t> file_size_limit = {}) {
  std::vector<char *> argv;
  argv.reserve(argv_storage.size() + 1U);
  for (auto &item : argv_storage) {
    argv.push_back(item.data());
  }
  argv.push_back(nullptr);
  // the test process may have handlers / masks installed (fork tests,
  // Boost.Test): start the binary with the defaults
  sigset_t empty{};
  ::sigemptyset(&empty);
  ::sigprocmask(SIG_SETMASK, &empty, nullptr);
  for (const int sig : {SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGCHLD}) {
    std::ignore = ::signal(sig, SIG_DFL);
  }
  if (file_size_limit) {
    const rlimit limit{.rlim_cur = static_cast<rlim_t>(*file_size_limit),
                       .rlim_max = static_cast<rlim_t>(*file_size_limit)};
    ::setrlimit(RLIMIT_FSIZE, &limit);
    std::ignore = ::signal(SIGXFSZ, SIG_IGN);
  }
  if (stdout_fd >= 0) {
    ::dup2(stdout_fd, STDOUT_FILENO);
  } else {
    std::ignore = std::freopen("/dev/null", "w", stdout);
  }
  std::ignore = std::freopen("/dev/null", "w", stderr);
  ::execv(argv_storage.front().c_str(), argv.data());
  ::_exit(127);
}

inline run_result decode_status(int status) {
  run_result result;
  if (WIFSIGNALED(status)) {
    result.signaled = true;
    result.signal_number = WTERMSIG(status);
  } else {
    result.exit_code = WEXITSTATUS(status);
  }
  return result;
}

} // namespace detail

// runs binlog_server with the arguments (no shell) and waits for it;
// stdout captured, stderr discarded
inline run_result run(const std::vector<std::string> &arguments,
                      std::optional<std::uint64_t> file_size_limit = {}) {
  auto argv_storage{detail::make_argv(arguments)};
  std::array<int, 2> pipe_ends{};
  if (::pipe(pipe_ends.data()) != 0) {
    throw std::runtime_error{"pipe failed"};
  }
  const pid_t child{::fork()};
  if (child < 0) {
    throw std::runtime_error{"fork failed"};
  }
  if (child == 0) {
    ::close(pipe_ends[0]);
    detail::exec_child(argv_storage, pipe_ends[1], file_size_limit);
  }
  ::close(pipe_ends[1]);
  std::string output;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto count{::read(pipe_ends[0], buffer.data(), buffer.size())};
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(pipe_ends[0]);
  int status{0};
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  auto result{detail::decode_status(status)};
  result.output = std::move(output);
  return result;
}

// binlog_server running in the background (stdout / stderr discarded);
// killed with SIGKILL when destroyed if still running
class background_run {
public:
  explicit background_run(const std::vector<std::string> &arguments,
                          std::optional<std::uint64_t> file_size_limit = {}) {
    auto argv_storage{detail::make_argv(arguments)};
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error{"fork failed"};
    }
    if (pid_ == 0) {
      detail::exec_child(argv_storage, -1, file_size_limit);
    }
  }
  background_run(const background_run &) = delete;
  background_run &operator=(const background_run &) = delete;
  background_run(background_run &&) = delete;
  background_run &operator=(background_run &&) = delete;
  ~background_run() {
    if (!finished_) {
      ::kill(pid_, SIGKILL);
      std::ignore = wait();
    }
  }

  // the result if the process has already exited
  [[nodiscard]] std::optional<run_result> poll() {
    if (finished_) {
      return result_;
    }
    int status{0};
    const auto waited{::waitpid(pid_, &status, WNOHANG)};
    if (waited == pid_) {
      finished_ = true;
      result_ = detail::decode_status(status);
      return result_;
    }
    return std::nullopt;
  }

  void send(int sig) const {
    if (!finished_) {
      ::kill(pid_, sig);
    }
  }

  run_result wait() {
    if (!finished_) {
      int status{0};
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
      finished_ = true;
      result_ = detail::decode_status(status);
    }
    return result_;
  }

  // waits up to 'timeout'; SIGKILL after that. Returns the result and
  // whether the process exited on its own in time.
  std::pair<run_result, bool> wait_for(std::chrono::milliseconds timeout) {
    const auto deadline{std::chrono::steady_clock::now() + timeout};
    while (std::chrono::steady_clock::now() < deadline) {
      if (const auto result{poll()}; result) {
        return {*result, true};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    send(SIGKILL);
    return {wait(), false};
  }

private:
  pid_t pid_{-1};
  bool finished_{false};
  run_result result_{};
};

} // namespace pbt::process

#endif // PBS_TESTS_PROPERTY_COMMON_BINLOG_SERVER_PROCESS_HPP
