// The WASI (preview 1) calls wasi-libc makes for the game: files and
// directories under five preopened folders, stdout/stderr, the clock and
// exit. Everything else returns ENOSYS. Paths are UTF-8 on every platform
// (std::filesystem converts on Windows).

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>

#include "host.h"

namespace fs = std::filesystem;

namespace {

enum : uint32_t {
  E_SUCCESS = 0, E_ACCES = 2, E_BADF = 8, E_EXIST = 20, E_INVAL = 28, E_IO = 29, E_ISDIR = 31,
  E_NOENT = 44, E_NOSYS = 52, E_NOTDIR = 54, E_NOTEMPTY = 55,
};
enum : uint8_t { FT_UNKNOWN = 0, FT_CHAR = 2, FT_DIR = 3, FT_FILE = 4 };

struct Fd {
  enum Kind { STDIO, DIR, FILE_ } kind;
  std::string guest;    // preopen name ("/game") for preopened dirs
  fs::path native;
  FILE *f = nullptr;
  bool preopen = false;
};

std::map<uint32_t, Fd> g_fds;
uint32_t g_next_fd = 3;

uint32_t err_of(const std::error_code &ec) {
  if (ec == std::errc::no_such_file_or_directory) return E_NOENT;
  if (ec == std::errc::file_exists) return E_EXIST;
  if (ec == std::errc::not_a_directory) return E_NOTDIR;
  if (ec == std::errc::is_a_directory) return E_ISDIR;
  if (ec == std::errc::permission_denied) return E_ACCES;
  if (ec == std::errc::directory_not_empty) return E_NOTEMPTY;
  return E_IO;
}

// Resolves a guest path relative to a directory fd, refusing to leave it.
bool resolve(uint32_t dirfd, uint32_t ptr, uint32_t len, fs::path *out) {
  auto it = g_fds.find(dirfd);
  if (it == g_fds.end() || it->second.kind != Fd::DIR || !mem_ok(ptr, len)) return false;
  std::string rel((const char *)mem(ptr), len);
  fs::path p = it->second.native;
  size_t start = 0;
  int depth = 0;
  while (start <= rel.size()) {
    size_t slash = rel.find('/', start);
    if (slash == std::string::npos) slash = rel.size();
    std::string part = rel.substr(start, slash - start);
    start = slash + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") {
      if (--depth < 0) return false;
      p = p.parent_path();
      continue;
    }
    depth++;
    p /= upath(part);
  }
  *out = p;
  return true;
}

uint8_t filetype(const fs::path &p) {
  std::error_code ec;
  auto st = fs::status(p, ec);
  if (ec) return FT_UNKNOWN;
  return fs::is_directory(st) ? FT_DIR : fs::is_regular_file(st) ? FT_FILE : FT_UNKNOWN;
}

uint64_t mtime_ns(const fs::path &p) {
  std::error_code ec;
  auto t = fs::last_write_time(p, ec);
  if (ec) return 0;
  auto sys = std::chrono::time_point_cast<std::chrono::nanoseconds>(
      t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
  return (uint64_t)sys.time_since_epoch().count();
}

void write_filestat(uint32_t buf, const fs::path &p, uint8_t type) {
  std::error_code ec;
  uint64_t size = type == FT_FILE ? (uint64_t)fs::file_size(p, ec) : 0;
  if (ec) size = 0;
  uint64_t t = mtime_ns(p);
  memset(mem(buf), 0, 64);
  mem(buf)[16] = type;
  mem_put_u64(buf + 24, 1);
  mem_put_u64(buf + 32, size);
  mem_put_u64(buf + 40, t);
  mem_put_u64(buf + 48, t);
  mem_put_u64(buf + 56, t);
}

FILE *open_native(const fs::path &p, const char *mode) {
#ifdef _WIN32
  std::wstring m(mode, mode + strlen(mode));
  return _wfopen(p.c_str(), m.c_str());
#else
  return fopen(p.c_str(), mode);
#endif
}

int64_t ftell64(FILE *f) {
#ifdef _WIN32
  return _ftelli64(f);
#else
  return ftello(f);
#endif
}

int fseek64(FILE *f, int64_t off, int whence) {
#ifdef _WIN32
  return _fseeki64(f, off, whence);
#else
  return fseeko(f, off, whence);
#endif
}

Fd *file_fd(uint32_t fd) {
  auto it = g_fds.find(fd);
  return it == g_fds.end() ? nullptr : &it->second;
}

}  // namespace

std::string guest_to_native(const std::string &path) {
  for (auto &kv : g_fds) {
    const Fd &d = kv.second;
    if (!d.preopen) continue;
    const std::string &pre = d.guest;
    if (path.compare(0, pre.size(), pre) == 0 && (path.size() == pre.size() || path[pre.size()] == '/'))
      return path_str(d.native) + path.substr(pre.size());
  }
  return "";
}

void wasi_init() {
  g_fds.clear();
  for (uint32_t i = 0; i < 3; i++) g_fds[i] = Fd{Fd::STDIO, "", {}, nullptr, false};
  auto pre = [](const char *guest, const std::string &native) {
    std::error_code ec;
    fs::create_directories(upath(native), ec);
    g_fds[g_next_fd++] = Fd{Fd::DIR, guest, upath(native), nullptr, true};
  };
  pre("/game", g_paths.game);
  pre("/image", g_paths.image);
  pre("/fonts", g_paths.fonts);
  pre("/assets", g_paths.cache);
  pre("/mods", g_paths.mods);
}

#define WASI(ret, name, ...) extern "C" ret w2c_wasi__snapshot__preview1_##name(struct w2c_wasi__snapshot__preview1 *, __VA_ARGS__)

WASI(u32, fd_prestat_get, u32 fd, u32 buf) {
  Fd *d = file_fd(fd);
  if (!d || !d->preopen) return E_BADF;
  mem_put_u32(buf, 0);
  mem_put_u32(buf + 4, (uint32_t)d->guest.size());
  return E_SUCCESS;
}

WASI(u32, fd_prestat_dir_name, u32 fd, u32 path, u32 len) {
  Fd *d = file_fd(fd);
  if (!d || !d->preopen) return E_BADF;
  if (len < d->guest.size() || !mem_ok(path, len)) return E_INVAL;
  memcpy(mem(path), d->guest.data(), d->guest.size());
  return E_SUCCESS;
}

WASI(u32, fd_fdstat_get, u32 fd, u32 buf) {
  Fd *d = file_fd(fd);
  if (!d) return E_BADF;
  memset(mem(buf), 0, 24);
  mem(buf)[0] = d->kind == Fd::STDIO ? FT_CHAR : d->kind == Fd::DIR ? FT_DIR : FT_FILE;
  mem_put_u64(buf + 8, ~0ull);
  mem_put_u64(buf + 16, ~0ull);
  return E_SUCCESS;
}

WASI(u32, fd_fdstat_set_flags, u32 fd, u32 flags) {
  (void)flags;
  return file_fd(fd) ? E_SUCCESS : E_BADF;
}

WASI(u32, path_open, u32 dirfd, u32 dirflags, u32 path, u32 path_len, u32 oflags, u64 rights,
     u64 rights_inh, u32 fdflags, u32 fd_out) {
  (void)dirflags; (void)rights_inh;
  fs::path p;
  if (!resolve(dirfd, path, path_len, &p)) return E_NOENT;
  uint8_t type = filetype(p);
  const bool creat = oflags & 1, directory = oflags & 2, excl = oflags & 4, trunc = oflags & 8;
  if (type == FT_DIR) {
    if (creat && excl) return E_EXIST;
    uint32_t fd = g_next_fd++;
    g_fds[fd] = Fd{Fd::DIR, "", p, nullptr, false};
    mem_put_u32(fd_out, fd);
    return E_SUCCESS;
  }
  if (directory) return type == FT_UNKNOWN ? E_NOENT : E_NOTDIR;
  if (type == FT_UNKNOWN && !creat) return E_NOENT;
  if (type != FT_UNKNOWN && creat && excl) return E_EXIST;
  const bool write = rights & ((1ull << 6) | (1ull << 9));   // fd_write, fd_allocate/truncate
  const char *mode;
  if (fdflags & 1) mode = "a+b";
  else if (trunc || (creat && type == FT_UNKNOWN)) mode = write ? "w+b" : "w+b";
  else mode = write ? "r+b" : "rb";
  FILE *f = open_native(p, mode);
  if (!f) return type == FT_UNKNOWN ? E_NOENT : E_ACCES;
  uint32_t fd = g_next_fd++;
  g_fds[fd] = Fd{Fd::FILE_, "", p, f, false};
  mem_put_u32(fd_out, fd);
  return E_SUCCESS;
}

WASI(u32, fd_close, u32 fd) {
  auto it = g_fds.find(fd);
  if (it == g_fds.end() || it->second.preopen) return E_BADF;
  if (it->second.f) fclose(it->second.f);
  if (it->second.kind != Fd::STDIO) g_fds.erase(it);
  return E_SUCCESS;
}

static uint32_t do_rw(uint32_t fd, uint32_t iovs, uint32_t n, uint32_t nout, bool write, int64_t at) {
  Fd *d = file_fd(fd);
  if (!d) return E_BADF;
  if (!mem_ok(iovs, (uint64_t)n * 8)) return E_INVAL;
  uint64_t total = 0;
  if (d->kind == Fd::STDIO) {
    if (!write) { mem_put_u32(nout, 0); return E_SUCCESS; }
    FILE *out = fd == 1 ? stdout : stderr;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t b = mem_u32(iovs + i * 8), l = mem_u32(iovs + i * 8 + 4);
      if (!mem_ok(b, l)) return E_INVAL;
      fwrite(mem(b), 1, l, out);
      total += l;
    }
    fflush(out);
    mem_put_u32(nout, (uint32_t)total);
    return E_SUCCESS;
  }
  if (d->kind != Fd::FILE_) return E_ISDIR;
  int64_t saved = -1;
  if (at >= 0) {
    saved = ftell64(d->f);
    fseek64(d->f, at, SEEK_SET);
  } else {
    fseek64(d->f, 0, SEEK_CUR);   // C requires a seek between reads and writes
  }
  for (uint32_t i = 0; i < n; i++) {
    uint32_t b = mem_u32(iovs + i * 8), l = mem_u32(iovs + i * 8 + 4);
    if (!mem_ok(b, l)) return E_INVAL;
    size_t got = write ? fwrite(mem(b), 1, l, d->f) : fread(mem(b), 1, l, d->f);
    total += got;
    if (got < l) break;
  }
  if (write) fflush(d->f);
  if (saved >= 0) fseek64(d->f, saved, SEEK_SET);
  mem_put_u32(nout, (uint32_t)total);
  return E_SUCCESS;
}

WASI(u32, fd_read, u32 fd, u32 iovs, u32 n, u32 nread) { return do_rw(fd, iovs, n, nread, false, -1); }
WASI(u32, fd_write, u32 fd, u32 iovs, u32 n, u32 nwritten) { return do_rw(fd, iovs, n, nwritten, true, -1); }
WASI(u32, fd_pread, u32 fd, u32 iovs, u32 n, u64 off, u32 nread) { return do_rw(fd, iovs, n, nread, false, (int64_t)off); }

WASI(u32, fd_seek, u32 fd, u64 off, u32 whence, u32 out) {
  Fd *d = file_fd(fd);
  if (!d) return E_BADF;
  if (d->kind != Fd::FILE_) return E_INVAL;
  int w = whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END;
  if (fseek64(d->f, (int64_t)off, w) != 0) return E_INVAL;
  mem_put_u64(out, (uint64_t)ftell64(d->f));
  return E_SUCCESS;
}

WASI(u32, fd_tell, u32 fd, u32 out) {
  Fd *d = file_fd(fd);
  if (!d || d->kind != Fd::FILE_) return E_BADF;
  mem_put_u64(out, (uint64_t)ftell64(d->f));
  return E_SUCCESS;
}

WASI(u32, fd_filestat_get, u32 fd, u32 buf) {
  Fd *d = file_fd(fd);
  if (!d) return E_BADF;
  if (d->kind == Fd::STDIO) {
    memset(mem(buf), 0, 64);
    mem(buf)[16] = FT_CHAR;
    return E_SUCCESS;
  }
  if (d->f) fflush(d->f);
  write_filestat(buf, d->native, d->kind == Fd::DIR ? FT_DIR : FT_FILE);
  return E_SUCCESS;
}

WASI(u32, fd_filestat_set_size, u32 fd, u64 size) {
  Fd *d = file_fd(fd);
  if (!d || d->kind != Fd::FILE_) return E_BADF;
  fflush(d->f);
  std::error_code ec;
  fs::resize_file(d->native, size, ec);
  return ec ? err_of(ec) : E_SUCCESS;
}

WASI(u32, path_filestat_get, u32 dirfd, u32 flags, u32 path, u32 len, u32 buf) {
  (void)flags;
  fs::path p;
  if (!resolve(dirfd, path, len, &p)) return E_NOENT;
  uint8_t t = filetype(p);
  if (t == FT_UNKNOWN) return E_NOENT;
  write_filestat(buf, p, t);
  return E_SUCCESS;
}

WASI(u32, path_create_directory, u32 dirfd, u32 path, u32 len) {
  fs::path p;
  if (!resolve(dirfd, path, len, &p)) return E_NOENT;
  std::error_code ec;
  if (fs::exists(p, ec)) return E_EXIST;
  fs::create_directory(p, ec);
  return ec ? err_of(ec) : E_SUCCESS;
}

WASI(u32, path_unlink_file, u32 dirfd, u32 path, u32 len) {
  fs::path p;
  if (!resolve(dirfd, path, len, &p)) return E_NOENT;
  if (filetype(p) == FT_DIR) return E_ISDIR;
  std::error_code ec;
  if (!fs::remove(p, ec)) return ec ? err_of(ec) : E_NOENT;
  return E_SUCCESS;
}

// Entries: 24-byte header (next cookie, inode, name length, type) + name.
// The cookie is the index of the next entry; a full buffer means "more".
WASI(u32, fd_readdir, u32 fd, u32 buf, u32 buf_len, u64 cookie, u32 used_out) {
  Fd *d = file_fd(fd);
  if (!d || d->kind != Fd::DIR) return E_BADF;
  if (!mem_ok(buf, buf_len)) return E_INVAL;
  std::vector<std::pair<std::string, uint8_t>> entries = {{".", FT_DIR}, {"..", FT_DIR}};
  std::error_code ec;
  for (auto &e : fs::directory_iterator(d->native, ec)) {
    std::error_code ec2;
    uint8_t t = e.is_directory(ec2) ? FT_DIR : FT_FILE;
    entries.emplace_back(path_str(e.path().filename()), t);
  }
  uint32_t used = 0;
  for (uint64_t i = cookie; i < entries.size() && used < buf_len; i++) {
    uint8_t hdr[24] = {0};
    uint64_t next = i + 1, ino = i + 1;
    uint32_t nl = (uint32_t)entries[i].first.size();
    memcpy(hdr, &next, 8);
    memcpy(hdr + 8, &ino, 8);
    memcpy(hdr + 16, &nl, 4);
    hdr[20] = entries[i].second;
    for (uint32_t k = 0; k < 24 && used < buf_len; k++) mem(buf)[used++] = hdr[k];
    for (uint32_t k = 0; k < nl && used < buf_len; k++) mem(buf)[used++] = (uint8_t)entries[i].first[k];
  }
  mem_put_u32(used_out, used);
  return E_SUCCESS;
}

WASI(u32, clock_time_get, u32 id, u64 precision, u32 out) {
  (void)precision;
  uint64_t ns;
  if (id == 0) {
    ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
  } else {
    ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  mem_put_u64(out, ns);
  return E_SUCCESS;
}

extern "C" void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *, u32 code) {
  guest_exit((int)code);
}

// No environment variables reach the game.
WASI(u32, environ_sizes_get, u32 count, u32 size) {
  mem_put_u32(count, 0);
  mem_put_u32(size, 0);
  return E_SUCCESS;
}
WASI(u32, environ_get, u32 vars, u32 buf) {
  (void)vars; (void)buf;
  return E_SUCCESS;
}

WASI(u32, path_rename, u32 fd, u32 path, u32 len, u32 fd2, u32 path2, u32 len2) {
  fs::path a, b;
  if (!resolve(fd, path, len, &a) || !resolve(fd2, path2, len2, &b)) return E_NOENT;
  std::error_code ec;
  fs::rename(a, b, ec);
  return ec ? err_of(ec) : E_SUCCESS;
}

WASI(u32, path_remove_directory, u32 fd, u32 path, u32 len) {
  fs::path p;
  if (!resolve(fd, path, len, &p)) return E_NOENT;
  if (filetype(p) != FT_DIR) return E_NOTDIR;
  std::error_code ec;
  fs::remove(p, ec);
  return ec ? err_of(ec) : E_SUCCESS;
}

// Only clock subscriptions (what sleep() uses): waits for the earliest.
WASI(u32, poll_oneoff, u32 in, u32 out, u32 n, u32 nevents) {
  if (!n || !mem_ok(in, (uint64_t)n * 48) || !mem_ok(out, (uint64_t)n * 32)) return E_INVAL;
  uint64_t wait = UINT64_MAX;
  uint32_t which = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t s = in + i * 48;
    if (mem(s)[8] != 0) continue;
    uint64_t t;
    memcpy(&t, mem(s + 24), 8);
    uint16_t flags;
    memcpy(&flags, mem(s + 40), 2);
    if (flags & 1) {   // absolute: against the monotonic clock
      uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      t = t > now ? t - now : 0;
    }
    if (t < wait) wait = t, which = i;
  }
  if (wait == UINT64_MAX) return E_NOSYS;
  std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
  memset(mem(out), 0, 32);
  memcpy(mem(out), mem(in + which * 48), 8);
  mem(out)[10] = 0;
  mem_put_u32(nevents, 1);
  return E_SUCCESS;
}
