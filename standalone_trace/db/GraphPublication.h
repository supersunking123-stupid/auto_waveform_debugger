#pragma once

#include "db/GraphDbTypes.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <iostream>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rtl_trace {

class ScopedFileLock {
 public:
  ScopedFileLock() = default;
  ScopedFileLock(const ScopedFileLock &) = delete;
  ScopedFileLock &operator=(const ScopedFileLock &) = delete;
  ~ScopedFileLock() { if (fd_ >= 0) ::close(fd_); }
  bool Acquire(const std::string &path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NONBLOCK, 0666);
    if (fd_ < 0) fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    return Lock(LOCK_EX);
  }
  bool AcquireShared(const std::string &path) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    return Lock(LOCK_SH);
  }
  int Descriptor() const { return fd_; }
 private:
  bool Lock(int operation) {
    if (fd_ < 0) return false;
    struct stat st;
    if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
      ::close(fd_); fd_ = -1; errno = EINVAL; return false;
    }
    while (::flock(fd_, operation) != 0) {
      if (errno == EINTR) continue;
      ::close(fd_); fd_ = -1; return false;
    }
    return true;
  }
  int fd_ = -1;
};

inline bool ResolveGraphDestination(const std::string &requested, std::filesystem::path &out) {
  std::error_code ec;
  auto path = std::filesystem::absolute(requested, ec);
  if (ec) return false;
  for (size_t links = 0; links < 40; ++links) {
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return false;
    if (!std::filesystem::is_symlink(status)) {
      ec.clear(); out = std::filesystem::weakly_canonical(path, ec); return !ec;
    }
    const auto target = std::filesystem::read_symlink(path, ec);
    if (ec) return false;
    path = target.is_absolute() ? target : path.parent_path() / target;
  }
  return false;
}

// Optional deterministic test stop; no CLI or file-format change.
inline void GraphPublishTestStop(const char *stage) {
  const char *requested = std::getenv("RTL_TRACE_TEST_PUBLISH_STOP");
  if (requested && std::string_view(requested) == stage) ::raise(SIGSTOP);
}

// Small bounded metadata envelope; this checks the fixed header and size only.
inline bool ReadCacheHeader(int fd, GraphDbFileHeader &header, uint64_t &size) {
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < off_t(sizeof(header))) return false;
  size = static_cast<uint64_t>(st.st_size);
  size_t done = 0;
  while (done < sizeof(header)) {
    const auto n = ::pread(fd, reinterpret_cast<char *>(&header) + done, sizeof(header) - done, done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    done += static_cast<size_t>(n);
  }
  return std::memcmp(header.magic, kGraphDbMagic, sizeof(header.magic)) == 0 &&
         header.version >= 1 && header.version <= 6 &&
         (header.version != 6 || header.reserved == 1 || header.reserved == 3 || header.reserved == 7);
}
inline std::string CacheEnvelope(const GraphDbFileHeader &header, uint64_t size) {
  static constexpr char hex[] = "0123456789abcdef";
  const auto *bytes = reinterpret_cast<const unsigned char *>(&header);
  std::string encoded; encoded.reserve(2 * sizeof(header));
  for (size_t i = 0; i < sizeof(header); ++i) {
    encoded += hex[bytes[i] >> 4]; encoded += hex[bytes[i] & 15];
  }
  return "RTL_TRACE_CACHE_ENVELOPE:1\nDB_SIZE:" + std::to_string(size) + "\nDB_HEADER:" + encoded + "\n";
}

class AtomicGraphOutput {
 public:
  ~AtomicGraphOutput() {
    FinishGuardian(published_);
    if (!published_) { RemoveOwnedTemporary(db_temp_); RemoveOwnedTemporary(meta_temp_); }
    if (directory_ >= 0) ::close(directory_);
  }
  bool Prepare(const std::string &requested) {
    std::filesystem::path destination;
    if (!ResolveGraphDestination(requested, destination)) return Fail("resolve destination", requested);
    destination_ = destination.string(); meta_ = destination_ + ".meta";
    if (!lock_.Acquire(destination_ + ".lock")) return Fail("lock publication", destination_ + ".lock");
    directory_ = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_ < 0) return Fail("open directory", destination.parent_path().string());
    // Names are bounded even when the DB basename is near NAME_MAX. The sidecar
    // lock serializes their use; recovery only removes regular, singly-linked
    // files owned by this uid in the reserved staging namespace.
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : destination_) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream stem; stem << ".rtl_trace_stage_" << ::geteuid() << "_" << std::hex << hash;
    db_temp_ = (destination.parent_path() / (stem.str() + ".db.tmp")).string();
    meta_temp_ = (destination.parent_path() / (stem.str() + ".meta.tmp")).string();
    if (!RemoveOwnedTemporary(db_temp_) || !RemoveOwnedTemporary(meta_temp_)) return Fail("recover staging", destination_);
    if (!StartGuardian()) std::cerr << "warning: cleanup guardian unavailable; continuing without it\n";
    if (!CreateTemporary(db_temp_, destination_, db_attributes_) ||
        !CreateTemporary(meta_temp_, meta_, meta_attributes_)) return false;
    GraphPublishTestStop("prepared");
    return true;
  }
  const std::string &TemporaryPath() const { return db_temp_; }
  const std::string &Error() const { return error_; }
  bool Fail(const std::string &operation, const std::string &path) {
    const int saved = errno;
    error_ = operation + ": " + path + ": " + std::strerror(saved ? saved : EIO);
    return false;
  }
  bool Publish(const std::string &fingerprint) {
    if (!FinalizeFile(db_temp_, db_attributes_)) return false;
    GraphPublishTestStop("db_closed");
    int fd = ::open(db_temp_.c_str(), O_RDONLY | O_CLOEXEC);
    GraphDbFileHeader header; uint64_t size = 0;
    const bool valid = fd >= 0 && ReadCacheHeader(fd, header, size);
    if (fd >= 0) ::close(fd);
    if (!valid) return Fail("read completed DB header", db_temp_);
    {
      std::ofstream stream(meta_temp_, std::ios::binary | std::ios::trunc);
      stream << fingerprint << CacheEnvelope(header, size); stream.close();
      if (stream.fail()) return Fail("write metadata", meta_temp_);
    }
    if (!FinalizeFile(meta_temp_, meta_attributes_)) return false;
    GraphPublishTestStop("meta_closed");
    // Meta-last process-level publication, deliberately without crash durability.
    if (::unlink(meta_.c_str()) != 0 && errno != ENOENT) return Fail("invalidate metadata", meta_);
    GraphPublishTestStop("meta_invalidated");
    if (::rename(db_temp_.c_str(), destination_.c_str()) != 0) return Fail("rename DB", destination_);
    GraphPublishTestStop("db_published");
    if (::rename(meta_temp_.c_str(), meta_.c_str()) != 0) return Fail("rename metadata", meta_);
    GraphPublishTestStop("meta_published");
    published_ = true;
    if (!FinishGuardian(true)) std::cerr << "warning: published DB successfully; cleanup guardian disarm/reap failed\n";
    return true;
  }
 private:
  static bool RemoveOwnedTemporary(const std::string &path) {
    if (path.empty()) return true;
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_nlink != 1) return false;
    return ::unlink(path.c_str()) == 0;
  }
  struct Attributes { mode_t mode = 0600; gid_t group = 0; };
  bool CreateTemporary(const std::string &path, const std::string &old_path, Attributes &attributes) {
    struct stat old;
    bool exists = ::stat(old_path.c_str(), &old) == 0;
    if (!exists && errno != ENOENT) return Fail("stat destination", old_path);
    if (exists && (!S_ISREG(old.st_mode) || !(old.st_mode & 0222) || ::access(old_path.c_str(), W_OK) != 0)) { errno = EACCES; return Fail("check writable destination", old_path); }
    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) return Fail("create staging", path);
    struct stat created;
    bool okay = ::fstat(fd, &created) == 0;
    if (okay) {
      attributes.mode = (exists ? old.st_mode : created.st_mode) & 07777;
      attributes.group = exists ? old.st_gid : created.st_gid;
      // Private writable staging; restore final attributes after all writes so
      // write/truncate cannot clear setgid bits after preservation.
      okay = ::fchown(fd, -1, attributes.group) == 0 && ::fchmod(fd, 0600) == 0;
    }
    if (!okay) Fail("precheck staging group/mode", path);
    ::close(fd); return okay;
  }
  bool FinalizeFile(const std::string &path, const Attributes &attributes) {
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return Fail("open staging attributes", path);
    // Replacing another uid's file may change owner without privilege. Preserve
    // its group and mode, or fail before publication; never silently downgrade.
    bool okay = ::fchown(fd, -1, attributes.group) == 0 &&
                ::fchmod(fd, attributes.mode) == 0;
    if (!okay) Fail("restore staging group/mode", path);
    ::close(fd); return okay;
  }
  bool StartGuardian() {
    int pipefd[2];
    // A socketpair has pipe-like EOF lifetime with MSG_NOSIGNAL for disarm.
    // A killed guardian must not terminate the compiler with SIGPIPE.
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pipefd) != 0) return false;
    const int lockfd = lock_.Descriptor();
    const long maxfd = ::sysconf(_SC_OPEN_MAX);
    if (maxfd < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }
    const int low = std::min(pipefd[0], lockfd), high = std::max(pipefd[0], lockfd);
    const char *db = db_temp_.c_str(), *meta = meta_temp_.c_str();
    sigset_t blocked, prior;
    ::sigemptyset(&blocked);
    const int ignored[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    for (int sig : ignored) ::sigaddset(&blocked, sig);
    const int mask_error = ::pthread_sigmask(SIG_BLOCK, &blocked, &prior);
    if (mask_error != 0) { ::close(pipefd[0]); ::close(pipefd[1]); errno = mask_error; return false; }
    struct sigaction action{}; action.sa_handler = SIG_IGN; ::sigemptyset(&action.sa_mask);
    guardian_ = std::getenv("RTL_TRACE_TEST_GUARDIAN_FORK_FAILURE") ? -1 : ::fork();
    if (guardian_ < 0) { ::pthread_sigmask(SIG_SETMASK, &prior, nullptr); ::close(pipefd[0]); ::close(pipefd[1]); return false; }
    if (guardian_ == 0) {
      for (int sig : ignored) if (::sigaction(sig, &action, nullptr) != 0) ::_exit(1);
      if (::pthread_sigmask(SIG_SETMASK, &prior, nullptr) != 0) ::_exit(1);
      ::close(pipefd[1]);
      // Async-signal-safe child: retain only read pipe and exclusive lock.
      bool closed = true;
#ifdef SYS_close_range
      if (low > 0 && ::syscall(SYS_close_range, 0U, static_cast<unsigned>(low - 1), 0U) != 0) closed = false;
      if (high > low + 1 && ::syscall(SYS_close_range, static_cast<unsigned>(low + 1), static_cast<unsigned>(high - 1), 0U) != 0) closed = false;
      if (::syscall(SYS_close_range, static_cast<unsigned>(high + 1), ~0U, 0U) != 0) closed = false;
#else
      closed = false;
#endif
      if (!closed) for (long fd = 0; fd < maxfd; ++fd) if (fd != pipefd[0] && fd != lockfd) ::close(static_cast<int>(fd));
      char command = 0; ssize_t count;
      do { count = ::read(pipefd[0], &command, 1); } while (count < 0 && errno == EINTR);
      if (count != 1 || command != 'D') {
        struct stat st;
        if (::lstat(db, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1) ::unlink(db);
        if (::lstat(meta, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid() && st.st_nlink == 1) ::unlink(meta);
      }
      ::close(pipefd[0]); ::close(lockfd); ::_exit(0);
    }
    ::pthread_sigmask(SIG_SETMASK, &prior, nullptr);
    ::close(pipefd[0]); guardian_pipe_ = pipefd[1]; return true;
  }
  bool FinishGuardian(bool disarm) {
    bool okay = true;
    if (guardian_pipe_ >= 0) {
      if (disarm) { char command = 'D'; ssize_t n; do { n = ::send(guardian_pipe_, &command, 1, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR); okay = n == 1; }
      ::close(guardian_pipe_); guardian_pipe_ = -1;
    }
    if (guardian_ > 0) {
      int status; pid_t got;
      do { got = ::waitpid(guardian_, &status, 0); } while (got < 0 && errno == EINTR);
      okay = okay && got == guardian_ && WIFEXITED(status) && WEXITSTATUS(status) == 0;
      guardian_ = -1;
    }
    return okay;
  }
  ScopedFileLock lock_;
  std::string destination_, meta_, db_temp_, meta_temp_, error_;
  int directory_ = -1, guardian_pipe_ = -1;
  pid_t guardian_ = -1;
  bool published_ = false;
  Attributes db_attributes_, meta_attributes_;
};

inline bool GraphCompileCacheHit(const std::string &requested, const std::string &fingerprint) {
  std::filesystem::path target;
  if (!ResolveGraphDestination(requested, target)) return false;
  ScopedFileLock lock;
  // A legacy DB can lack a sidecar. Do not make an unlocked cache decision
  // while the first atomic writer creates that mutex; rebuild under EX instead.
  if (!lock.AcquireShared(target.string() + ".lock")) return false;
  int fd = ::open(target.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  GraphDbFileHeader header; uint64_t size = 0;
  const bool valid = fd >= 0 && ReadCacheHeader(fd, header, size);
  if (fd >= 0) ::close(fd);
  if (!valid) return false;
  const std::string expected = fingerprint + CacheEnvelope(header, size);
  fd = ::open((target.string() + ".meta").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st;
  bool okay = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
              static_cast<uint64_t>(st.st_size) == expected.size();
  std::string actual(okay ? expected.size() : 0, '\0');
  size_t done = 0;
  while (okay && done < actual.size()) {
    const auto n = ::read(fd, actual.data() + done, actual.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { okay = false; break; }
    done += static_cast<size_t>(n);
  }
  ::close(fd);
  return okay && actual == expected;
}

} // namespace rtl_trace
