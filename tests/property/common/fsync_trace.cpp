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

// pbt_fsync_trace: an LD_PRELOAD library for the durability property D61
// (mysql_property_test, BinlogDataIsDurableBeforeItsMetadataMySQL).
//
// It records, in the order they complete, the fsync / fdatasync calls of
// the process (the path of the file and its size at that moment) and its
// renames (source, target, and the "size" field of the source file when it
// is a JSON document: what a binlog metadata file records), one line each,
// appended to the file named by PBT_FSYNC_TRACE_LOG:
//   fsync<TAB><path><TAB><size>
//   rename<TAB><from><TAB><to><TAB><size or -1>
// The calls themselves are passed on unchanged. Built without sanitizers:
// it is loaded before the sanitizer runtime of the binary it traces (ASan
// then needs ASAN_OPTIONS=verify_asan_link_order=0).

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void append_line(const char *line, std::size_t length) {
  const char *log_path{std::getenv("PBT_FSYNC_TRACE_LOG")};
  if (log_path == nullptr || length == 0U) {
    return;
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  const int fd{::open(log_path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644)};
  if (fd < 0) {
    return;
  }
  // one write per line (O_APPEND): lines of several processes do not mix
  [[maybe_unused]] const auto written{::write(fd, line, length)};
  ::close(fd);
}

bool path_of_fd(int fd, char *out, std::size_t size) {
  char link[64];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  std::snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
  const auto length{::readlink(link, out, size - 1U)};
  if (length < 0) {
    return false;
  }
  out[length] = '\0';
  return true;
}

// a path relative to 'dir_fd' as an absolute one (best effort)
void resolve(int dir_fd, const char *path, char *out, std::size_t size) {
  if (path[0] != '/' && dir_fd != AT_FDCWD) {
    char dir[4096];
    if (path_of_fd(dir_fd, dir, sizeof dir)) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
      std::snprintf(out, size, "%s/%s", dir, path);
      return;
    }
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  std::snprintf(out, size, "%s", path);
}

// the "size" field of a small JSON file, or -1
long long json_size_field(const char *path) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  const int fd{::open(path, O_RDONLY | O_CLOEXEC)};
  if (fd < 0) {
    return -1;
  }
  char content[65536];
  std::size_t used{0U};
  while (used + 1U < sizeof content) {
    const auto count{::read(fd, content + used, sizeof content - 1U - used)};
    if (count <= 0) {
      break;
    }
    used += static_cast<std::size_t>(count);
  }
  ::close(fd);
  content[used] = '\0';
  const char *at{std::strstr(content, "\"size\"")};
  if (at == nullptr) {
    return -1;
  }
  at += 6;
  while (*at == ' ' || *at == ':' || *at == '\t' || *at == '\n' || *at == '\r') {
    ++at;
  }
  if (*at < '0' || *at > '9') {
    return -1;
  }
  long long value{0};
  while (*at >= '0' && *at <= '9') {
    value = value * 10 + (*at - '0');
    ++at;
  }
  return value;
}

void trace_sync(int fd) {
  char path[4096];
  struct stat status {};
  if (!path_of_fd(fd, path, sizeof path) || ::fstat(fd, &status) != 0 ||
      !S_ISREG(status.st_mode)) {
    return;
  }
  char line[4200];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  const int length{std::snprintf(line, sizeof line, "fsync\t%s\t%lld\n", path,
                                 static_cast<long long>(status.st_size))};
  if (length > 0 && static_cast<std::size_t>(length) < sizeof line) {
    append_line(line, static_cast<std::size_t>(length));
  }
}

void trace_rename(const char *from, const char *to, long long size) {
  char line[8400];
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
  const int length{std::snprintf(line, sizeof line, "rename\t%s\t%s\t%lld\n", from, to, size)};
  if (length > 0 && static_cast<std::size_t>(length) < sizeof line) {
    append_line(line, static_cast<std::size_t>(length));
  }
}

template <typename Function> Function next_symbol(const char *name) {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<Function>(::dlsym(RTLD_NEXT, name));
}

int renamed(int from_dir, const char *from, int to_dir, const char *to, int result) {
  if (result == 0) {
    const int saved_errno{errno};
    char from_path[4096];
    char to_path[4096];
    resolve(from_dir, from, from_path, sizeof from_path);
    resolve(to_dir, to, to_path, sizeof to_path);
    // the content now lives at the target
    trace_rename(from_path, to_path, json_size_field(to_path));
    errno = saved_errno;
  }
  return result;
}

} // namespace

extern "C" {

int fsync(int fd) {
  static const auto real{next_symbol<int (*)(int)>("fsync")};
  const int result{real(fd)};
  if (result == 0) {
    const int saved_errno{errno};
    trace_sync(fd);
    errno = saved_errno;
  }
  return result;
}

int fdatasync(int fd) {
  static const auto real{next_symbol<int (*)(int)>("fdatasync")};
  const int result{real(fd)};
  if (result == 0) {
    const int saved_errno{errno};
    trace_sync(fd);
    errno = saved_errno;
  }
  return result;
}

int rename(const char *from, const char *to) noexcept {
  static const auto real{next_symbol<int (*)(const char *, const char *)>("rename")};
  return renamed(AT_FDCWD, from, AT_FDCWD, to, real(from, to));
}

int renameat(int from_dir, const char *from, int to_dir, const char *to) noexcept {
  static const auto real{
      next_symbol<int (*)(int, const char *, int, const char *)>("renameat")};
  return renamed(from_dir, from, to_dir, to, real(from_dir, from, to_dir, to));
}

int renameat2(int from_dir, const char *from, int to_dir, const char *to,
              unsigned int flags) noexcept {
  static const auto real{
      next_symbol<int (*)(int, const char *, int, const char *, unsigned int)>("renameat2")};
  return renamed(from_dir, from, to_dir, to, real(from_dir, from, to_dir, to, flags));
}

} // extern "C"
