#pragma once

#include <algorithm>
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
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd_ < 0) fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    return Lock(LOCK_EX);
  }
  bool AcquireShared(const std::string &path, bool optional = false) {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0 && optional && errno == ENOENT) return true;
    return Lock(LOCK_SH);
  }
  int Descriptor() const { return fd_; }
 private:
  bool Lock(int operation) {
    if (fd_ < 0) return false;
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

class AtomicGraphOutput {
 public:
  ~AtomicGraphOutput() {
    FinishGuardian(published_);
    if (!published_) { RemoveOwnedTemporary(db_temp_); RemoveOwnedTemporary(meta_temp_); }
    if (directory_ >= 0) ::close(directory_);
  }
  bool Prepare(const std::string &requested) {
    std::filesystem::path destination;
    if (!ResolveGraphDestination(requested, destination)) return false;
    destination_ = destination.string(); meta_ = destination_ + ".meta";
    if (!lock_.Acquire(destination_ + ".lock")) return false;
    directory_ = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_ < 0) return false;
    // Names are bounded even when the DB basename is near NAME_MAX. The sidecar
    // lock serializes their use; recovery only removes regular, singly-linked
    // files owned by this uid in the reserved staging namespace.
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : destination_) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream stem; stem << ".rtl_trace_stage_" << std::hex << hash;
    db_temp_ = (destination.parent_path() / (stem.str() + ".db.tmp")).string();
    meta_temp_ = (destination.parent_path() / (stem.str() + ".meta.tmp")).string();
    if (!RemoveOwnedTemporary(db_temp_) || !RemoveOwnedTemporary(meta_temp_)) return false;
    if (!StartGuardian()) return false;
    if (!CreateTemporary(db_temp_, destination_, db_attributes_) ||
        !CreateTemporary(meta_temp_, meta_, meta_attributes_)) return false;
    GraphPublishTestStop("prepared");
    return true;
  }
  const std::string &TemporaryPath() const { return db_temp_; }
  bool Publish(const std::string &fingerprint) {
    if (!SyncFile(db_temp_, db_attributes_)) return false;
    GraphPublishTestStop("db_synced");
    {
      std::ofstream stream(meta_temp_, std::ios::binary | std::ios::trunc);
      stream << fingerprint; stream.close();
      if (stream.fail()) return false;
    }
    if (!SyncFile(meta_temp_, meta_attributes_)) return false;
    GraphPublishTestStop("meta_synced");
    // Two names cannot be renamed atomically. A durable missing fingerprint is
    // safe: incremental callers rebuild instead of reusing the wrong graph.
    if (::unlink(meta_.c_str()) != 0 && errno != ENOENT) return false;
    if (::fsync(directory_) != 0) return false;
    GraphPublishTestStop("meta_invalidated");
    if (::rename(db_temp_.c_str(), destination_.c_str()) != 0 || ::fsync(directory_) != 0) return false;
    GraphPublishTestStop("db_published");
    if (::rename(meta_temp_.c_str(), meta_.c_str()) != 0 || ::fsync(directory_) != 0) return false;
    GraphPublishTestStop("meta_published");
    published_ = true;
    return FinishGuardian(true);
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
  static bool CreateTemporary(const std::string &path, const std::string &old_path, Attributes &attributes) {
    struct stat old;
    bool exists = ::stat(old_path.c_str(), &old) == 0;
    if (!exists && errno != ENOENT) return false;
    if (exists && (!S_ISREG(old.st_mode) || !(old.st_mode & 0222) || ::access(old_path.c_str(), W_OK) != 0)) return false;
    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_NOFOLLOW | O_WRONLY | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    struct stat created;
    bool okay = ::fstat(fd, &created) == 0;
    if (okay) {
      attributes.mode = (exists ? old.st_mode : created.st_mode) & 07777;
      attributes.group = exists ? old.st_gid : created.st_gid;
      // Private writable staging; restore final attributes after all writes so
      // write/truncate cannot clear setgid bits after preservation.
      okay = ::fchmod(fd, 0600) == 0;
    }
    ::close(fd); return okay;
  }
  static bool SyncFile(const std::string &path, const Attributes &attributes) {
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    // Replacing another uid's file may change owner without privilege. Preserve
    // its group and mode, or fail before publication; never silently downgrade.
    bool okay = ::fchown(fd, -1, attributes.group) == 0 &&
                ::fchmod(fd, attributes.mode) == 0 && ::fsync(fd) == 0;
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
    guardian_ = ::fork();
    if (guardian_ < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return false; }
    if (guardian_ == 0) {
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
  std::string destination_, meta_, db_temp_, meta_temp_;
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
  if (!std::filesystem::is_regular_file(target)) return false;
  std::ifstream meta(target.string() + ".meta", std::ios::binary);
  if (!meta) return false;
  return std::string(std::istreambuf_iterator<char>(meta), std::istreambuf_iterator<char>()) == fingerprint;
}

} // namespace rtl_trace
