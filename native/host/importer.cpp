// First-run install from the player's own copy of the game, the native
// counterpart of web/pages/install-worker.js. Takes the SimGolf disc image
// (.iso, or a raw .bin/.img/.mdf) and the v1.02 no-CD golf.exe (bare, or in
// its .zip), or a .zip/.7z holding them. It reads the disc, unpacks the
// InstallShield cabinets with unshield and writes, under the data folder:
//   game/...         the installed game files
//   image/golf.bin   golf.exe mapped as the loader maps it
//   image/jgld.bin   jgld.dll mapped, with our NOP patches (jgld-nops.json)
//   installed.txt    marks a finished install
// Nothing leaves the player's machine. The intro and closing videos (Bink)
// aren't converted; the game skips them.

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <stdexcept>

#include "host.h"

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "libunshield.h"
}

namespace fs = std::filesystem;

namespace {

const char *kNoCdSha1 = "6f05a3ae3b5050ed82dc0010c03842a2e0ab166b";   // golf.exe v1.02, no-CD
const char *kGroup = "Program Files (ENGLISH)";
// Loose files in the disc root that the installer copies next to the game
const std::regex kRootExtra("^(autosavebase\\.pcx|autosavebuttons\\.pcx|jackal\\.pcx|jackal\\.txt)$",
                            std::regex::icase);

struct Missing : std::runtime_error { using std::runtime_error::runtime_error; };
using Bytes = std::vector<uint8_t>;

std::string base_name(const std::string &p) { return path_str(upath(p).filename()); }

std::string lower(std::string s) {
  for (char &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string ext(const std::string &name) {
  auto dot = name.rfind('.');
  return dot == std::string::npos ? "" : lower(name.substr(dot + 1));
}

bool is_disc_ext(const std::string &e) { return e == "iso" || e == "bin" || e == "img" || e == "mdf"; }

Bytes read_file(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) throw std::runtime_error("Couldn't read " + path_str(p.filename()));
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path &p, const uint8_t *data, size_t n) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write((const char *)data, (std::streamsize)n);
  if (!out) throw std::runtime_error("Couldn't write " + path_str(p));
}

// ---------------------------------------------------------------- SHA-1

std::string sha1(const Bytes &data) {
  uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  Bytes m = data;
  uint64_t bits = (uint64_t)data.size() * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bits >> (i * 8)));
  auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)m[off + i * 4] << 24 | m[off + i * 4 + 1] << 16 | m[off + i * 4 + 2] << 8 | m[off + i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
      else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
      else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
      else f = b ^ c ^ d, k = 0xCA62C1D6;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  char out[41];
  for (int i = 0; i < 5; i++) snprintf(out + i * 8, 9, "%08x", h[i]);
  return out;
}

// ---------------------------------------------------------------- PE images (tools/pages/pe-image.mjs)

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

Bytes map_pe(const Bytes &buf) {
  if (buf.size() < 0x40) throw std::runtime_error("not a Windows program");
  uint32_t pe = le32(&buf[0x3c]);
  if (pe + 0x100 > buf.size()) throw std::runtime_error("not a Windows program");
  uint16_t nsec = le16(&buf[pe + 6]), opt_size = le16(&buf[pe + 20]);
  uint32_t opt = pe + 24, size = le32(&buf[opt + 56]), size_hdr = le32(&buf[opt + 60]);
  Bytes img(size, 0);
  std::copy_n(buf.begin(), std::min<size_t>({size_hdr, size, buf.size()}), img.begin());
  for (uint32_t i = 0; i < nsec; i++) {
    uint32_t s = opt + opt_size + i * 40;
    uint32_t vsz = le32(&buf[s + 8]), va = le32(&buf[s + 12]), rsz = le32(&buf[s + 16]), rp = le32(&buf[s + 20]);
    int64_t n = std::min<int64_t>({(int64_t)rsz, (int64_t)(vsz ? vsz : rsz), (int64_t)size - va,
                                   (int64_t)buf.size() - rp});
    if (n > 0) std::copy_n(buf.begin() + rp, n, img.begin() + va);
  }
  return img;
}

// ---------------------------------------------------------------- zip

struct ZipEntry { std::string name; uint16_t method; uint32_t csize, usize, local; };

std::vector<ZipEntry> zip_entries(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  in.seekg(0, std::ios::end);
  int64_t size = in.tellg();
  int64_t tail_len = std::min<int64_t>(size, 65557);
  Bytes tail((size_t)tail_len);
  in.seekg(size - tail_len);
  in.read((char *)tail.data(), tail_len);
  int64_t eocd = -1;
  for (int64_t i = tail_len - 22; i >= 0; i--)
    if (le32(&tail[(size_t)i]) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw std::runtime_error(path_str(p.filename()) + " is not a zip file");
  uint16_t n = le16(&tail[(size_t)eocd + 10]);
  uint32_t cd_size = le32(&tail[(size_t)eocd + 12]), cd_off = le32(&tail[(size_t)eocd + 16]);
  Bytes cd(cd_size);
  in.seekg(cd_off);
  in.read((char *)cd.data(), cd_size);
  std::vector<ZipEntry> out;
  for (size_t q = 0, i = 0; i < n && q + 46 <= cd.size(); i++) {
    ZipEntry e;
    e.method = le16(&cd[q + 10]);
    e.csize = le32(&cd[q + 20]);
    e.usize = le32(&cd[q + 24]);
    uint16_t name_len = le16(&cd[q + 28]), extra = le16(&cd[q + 30]), comment = le16(&cd[q + 32]);
    e.local = le32(&cd[q + 42]);
    e.name.assign((const char *)&cd[q + 46], name_len);
    out.push_back(e);
    q += 46 + name_len + extra + comment;
  }
  return out;
}

// Streams one entry to a file (or memory when out is empty).
Bytes zip_extract(const fs::path &zip, const ZipEntry &e, const fs::path &out_path,
                  const std::function<void(uint64_t)> &progress) {
  std::ifstream in(zip, std::ios::binary);
  uint8_t h[30];
  in.seekg(e.local);
  in.read((char *)h, 30);
  in.seekg(e.local + 30 + le16(h + 26) + le16(h + 28));
  std::ofstream out;
  if (!out_path.empty()) out.open(out_path, std::ios::binary | std::ios::trunc);
  Bytes mem_out;
  auto emit = [&](const uint8_t *d, size_t n) {
    if (out.is_open()) out.write((const char *)d, (std::streamsize)n);
    else mem_out.insert(mem_out.end(), d, d + n);
  };
  const size_t kChunk = 1 << 20;
  Bytes inbuf(kChunk), outbuf(kChunk);
  uint64_t left = e.csize, done = 0;
  if (e.method == 0) {
    while (left) {
      size_t n = (size_t)std::min<uint64_t>(left, kChunk);
      in.read((char *)inbuf.data(), (std::streamsize)n);
      emit(inbuf.data(), n);
      left -= n;
      done += n;
      progress(done);
    }
  } else if (e.method == 8) {
    z_stream z{};
    inflateInit2(&z, -15);
    int r = Z_OK;
    while (r != Z_STREAM_END) {
      if (z.avail_in == 0) {
        if (!left) break;
        size_t n = (size_t)std::min<uint64_t>(left, kChunk);
        in.read((char *)inbuf.data(), (std::streamsize)n);
        left -= n;
        z.next_in = inbuf.data();
        z.avail_in = (uInt)n;
      }
      z.next_out = outbuf.data();
      z.avail_out = (uInt)kChunk;
      r = inflate(&z, Z_NO_FLUSH);
      if (r != Z_OK && r != Z_STREAM_END) {
        inflateEnd(&z);
        throw std::runtime_error("Couldn't unzip " + e.name);
      }
      emit(outbuf.data(), kChunk - z.avail_out);
      done += kChunk - z.avail_out;
      progress(done);
    }
    inflateEnd(&z);
  } else {
    throw std::runtime_error("Unsupported compression in the zip (" + e.name + ")");
  }
  return mem_out;
}

// ---------------------------------------------------------------- 7z (LZMA SDK)

std::vector<fs::path> un7z(const fs::path &archive, const fs::path &scratch,
                           const std::function<void(const std::string &)> &progress) {
  CFileInStream stream;
  CLookToRead2 look;
  CSzArEx db;
  ISzAlloc alloc = {SzAlloc, SzFree}, alloc_temp = {SzAllocTemp, SzFreeTemp};
#ifdef _WIN32
  if (InFile_OpenW(&stream.file, archive.c_str()) != 0)
#else
  if (InFile_Open(&stream.file, archive.c_str()) != 0)
#endif
    throw std::runtime_error("Couldn't open the .7z file.");
  FileInStream_CreateVTable(&stream);
  stream.wres = 0;
  LookToRead2_CreateVTable(&look, False);
  const size_t kBuf = 1 << 18;
  look.buf = (Byte *)ISzAlloc_Alloc(&alloc, kBuf);
  look.bufSize = kBuf;
  look.realStream = &stream.vt;
  LookToRead2_INIT(&look);
  CrcGenerateTable();
  SzArEx_Init(&db);
  std::vector<fs::path> out;
  SRes res = SzArEx_Open(&db, &look.vt, &alloc, &alloc_temp);
  UInt32 block = 0xFFFFFFFF;
  Byte *obuf = nullptr;
  size_t obuf_size = 0;
  for (UInt32 i = 0; res == SZ_OK && i < db.NumFiles; i++) {
    if (SzArEx_IsDir(&db, i)) continue;
    size_t len = SzArEx_GetFileNameUtf16(&db, i, nullptr);
    std::vector<UInt16> name16(len);
    SzArEx_GetFileNameUtf16(&db, i, name16.data());
    std::string name;
    for (UInt16 c : name16) {
      if (!c) break;
      name += c < 0x80 ? (char)c : '_';
    }
    std::string base = name.substr(name.find_last_of("/\\") + 1);
    std::string e = ext(base);
    if (!is_disc_ext(e) && e != "exe" && e != "zip") continue;
    progress("Unpacking " + base + " (this takes a minute)…");
    size_t offset = 0, processed = 0;
    res = SzArEx_Extract(&db, &look.vt, i, &block, &obuf, &obuf_size, &offset, &processed, &alloc, &alloc_temp);
    if (res != SZ_OK) break;
    fs::path p = scratch / ("x" + std::to_string(out.size()) + "_" + base);
    write_file(p, obuf + offset, processed);
    out.push_back(p);
  }
  ISzAlloc_Free(&alloc, obuf);
  SzArEx_Free(&db, &alloc);
  ISzAlloc_Free(&alloc, look.buf);
  File_Close(&stream.file);
  if (res != SZ_OK) throw std::runtime_error("Couldn't unpack the .7z file (error " + std::to_string(res) + ").");
  return out;
}

// ---------------------------------------------------------------- disc image (ISO 9660, Joliet when present)

struct DiscEntry { std::string name; bool dir; uint32_t lba, size; Bytes rec; };

class Disc {
 public:
  explicit Disc(const fs::path &p) : in_(p, std::ios::binary) {
    uint8_t head[16];
    in_.read((char *)head, 16);
    raw_ = head[0] == 0 && std::all_of(head + 1, head + 11, [](uint8_t b) { return b == 0xff; }) && head[11] == 0;
    sector_ = raw_ ? 2352 : 2048;
    data_off_ = raw_ ? (head[15] == 2 ? 24 : 16) : 0;
    for (uint32_t lba = 16; lba < 64; lba++) {
      Bytes d = read(lba, 1);
      if (memcmp(&d[1], "CD001", 5) || d[0] == 255) break;
      if (d[0] == 1 && root_.empty()) root_.assign(d.begin() + 156, d.begin() + 190);
      if (d[0] == 2 && d[88] == 0x25 && d[89] == 0x2f && (d[90] == 0x40 || d[90] == 0x43 || d[90] == 0x45)) {
        root_.assign(d.begin() + 156, d.begin() + 190);
        joliet_ = true;
      }
    }
    if (root_.empty()) throw std::runtime_error("That doesn't look like a CD image.");
  }

  Bytes read(uint32_t lba, uint32_t count) {
    Bytes out((size_t)count * 2048);
    for (uint32_t i = 0; i < count; i++) {
      in_.clear();
      in_.seekg((int64_t)(lba + i) * sector_ + data_off_);
      in_.read((char *)&out[(size_t)i * 2048], 2048);
    }
    return out;
  }

  // A file's bytes, in pieces, to fn
  void extract(const DiscEntry &e, const std::function<void(const uint8_t *, size_t)> &fn) {
    const uint32_t kSectors = 512;
    for (uint32_t done = 0, s = e.lba; done < e.size; s += kSectors) {
      Bytes b = read(s, kSectors);
      uint32_t n = std::min<uint32_t>(e.size - done, kSectors * 2048);
      fn(b.data(), n);
      done += n;
    }
  }

  std::vector<DiscEntry> root() { return dir(root_); }

  std::vector<DiscEntry> dir(const Bytes &rec) {
    uint32_t lba = le32(&rec[2]), size = le32(&rec[10]);
    Bytes data = read(lba, (size + 2047) / 2048);
    std::vector<DiscEntry> out;
    for (uint32_t p = 0; p < size;) {
      uint8_t len = data[p];
      if (!len) { p = (p / 2048 + 1) * 2048; continue; }
      const uint8_t *e = &data[p];
      uint8_t name_len = e[32];
      if (!(name_len == 1 && e[33] <= 1)) {
        std::string name;
        if (joliet_) {
          for (int i = 0; i + 1 < name_len; i += 2) {
            uint16_t c = (uint16_t)(e[33 + i] << 8 | e[34 + i]);
            name += c < 0x80 ? (char)c : '_';
          }
        } else {
          name.assign((const char *)e + 33, name_len);
        }
        if (name.size() > 2 && name.compare(name.size() - 2, 2, ";1") == 0) name.resize(name.size() - 2);
        if (!name.empty() && name.back() == '.') name.pop_back();
        out.push_back({name, (e[25] & 2) != 0, le32(e + 2), le32(e + 10), Bytes(e, e + len)});
      }
      p += len;
    }
    return out;
  }

 private:
  std::ifstream in_;
  bool raw_ = false, joliet_ = false;
  int sector_ = 2048, data_off_ = 0;
  Bytes root_;
};

// ---------------------------------------------------------------- install

struct Found { fs::path disc; Bytes exe; std::string wrong_exe; };

void visit(const fs::path &p, const std::string &name, const fs::path &scratch, Found &f,
           const std::function<void(float, const std::string &)> &progress, int depth = 0) {
  std::string e = ext(name);
  if (is_disc_ext(e)) {
    if (f.disc.empty()) f.disc = p;
    return;
  }
  if (e == "exe") {
    Bytes data = read_file(p);
    if (sha1(data) == kNoCdSha1) f.exe = std::move(data);
    else if (f.wrong_exe.empty()) f.wrong_exe = name;
    return;
  }
  if (e == "zip") {
    for (const ZipEntry &z : zip_entries(p)) {
      std::string b = z.name.substr(z.name.find_last_of('/') == std::string::npos ? 0 : z.name.find_last_of('/') + 1);
      std::string be = ext(b);
      if (is_disc_ext(be) && f.disc.empty()) {
        fs::path out = scratch / ("disc_" + b);
        zip_extract(p, z, out, [&](uint64_t done) {
          progress(0.01f, "Unzipping " + b + " (" + std::to_string(done >> 20) + " MB)");
        });
        f.disc = out;
      } else if ((lower(b) == "golf.exe" || be == "zip") && z.csize <= (64u << 20) && depth < 3) {
        fs::path out = scratch / ("z" + std::to_string(depth) + "_" + b);
        zip_extract(p, z, out, [](uint64_t) {});
        visit(out, b, scratch, f, progress, depth + 1);
      }
    }
    return;
  }
  if (e == "7z") {
    for (const fs::path &x : un7z(p, scratch, [&](const std::string &t) { progress(0.01f, t); })) {
      std::string xb = path_str(x.filename());
      xb = xb.substr(xb.find('_') + 1);
      visit(x, xb, scratch, f, progress, depth + 1);
    }
    return;
  }
  if (e == "cue" || e == "mds" || e == "nfo" || e == "txt" || e == "log") return;
  throw std::runtime_error("Not sure what " + name +
                           " is. Choose the SimGolf disc image (.iso) and the no-CD golf.exe, or the zip they came in.");
}

std::vector<std::pair<uint32_t, uint32_t>> jgld_nops(const std::string &resources) {
  std::ifstream in(upath(resources + "jgld-nops.json"));
  if (!in) throw std::runtime_error("jgld-nops.json is missing from the app.");
  std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<uint32_t> nums;
  for (size_t i = 0; i < s.size();) {
    if (isdigit((unsigned char)s[i])) {
      size_t j = i;
      while (j < s.size() && isdigit((unsigned char)s[j])) j++;
      nums.push_back((uint32_t)std::stoul(s.substr(i, j - i)));
      i = j;
    } else {
      i++;
    }
  }
  std::vector<std::pair<uint32_t, uint32_t>> out;
  for (size_t i = 0; i + 1 < nums.size(); i += 2) out.emplace_back(nums[i], nums[i + 1]);
  return out;
}

}  // namespace

std::string g_resources;   // set by main: bundled files, ends with '/'

bool game_installed() {
  std::error_code ec;
  return fs::exists(upath(g_paths.data + "installed.txt"), ec) && fs::exists(upath(g_paths.image + "golf.bin"), ec) &&
         fs::exists(upath(g_paths.image + "jgld.bin"), ec);
}

std::string import_game(const std::vector<std::string> &files,
                        void (*progress_cb)(void *, float, const std::string &), void *ctx, bool *missing) {
  *missing = false;
  auto progress = [&](float f, const std::string &t) { progress_cb(ctx, f, t); };
  fs::path scratch = upath(g_paths.data + "import-scratch");
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch);
  try {
    progress(0, "Checking your files…");
    Found f;
    for (const std::string &file : files) visit(upath(file), base_name(file), scratch, f, progress);
    if (f.exe.empty() && !f.wrong_exe.empty())
      throw std::runtime_error("The golf.exe in " + f.wrong_exe + " isn't the v1.02 no-CD version this port is built from.");
    if (f.disc.empty() && f.exe.empty())
      throw std::runtime_error("Couldn't find the disc image or the no-CD golf.exe in what you chose.");
    if (f.disc.empty()) throw Missing("Found the no-CD golf.exe; now add the disc image (.iso) as well.");
    if (f.exe.empty()) throw Missing("Found the disc image; now add the no-CD golf.exe (or the zip it came in) as well.");

    Disc disc(f.disc);
    auto root = disc.root();
    auto find = [&](const std::string &n) -> const DiscEntry * {
      for (auto &e : root)
        if (!e.dir && lower(e.name) == n) return &e;
      return nullptr;
    };
    const DiscEntry *hdr = find("data1.hdr");
    std::vector<const DiscEntry *> cabs;
    for (auto &e : root)
      if (!e.dir && std::regex_match(e.name, std::regex("^data\\d+\\.cab$", std::regex::icase))) cabs.push_back(&e);
    if (!hdr || cabs.empty()) throw std::runtime_error("That disc image isn't the SimGolf CD (no installer cabinets found).");

    fs::path cd = scratch / "cd";
    fs::create_directories(cd);
    cabs.insert(cabs.begin(), hdr);
    for (const DiscEntry *e : cabs) {
      progress(0.02f, "Reading " + e->name + " from the disc…");
      std::ofstream out(cd / upath(lower(e->name)), std::ios::binary | std::ios::trunc);
      disc.extract(*e, [&](const uint8_t *d, size_t n) { out.write((const char *)d, (std::streamsize)n); });
    }

    std::string cab = path_str(cd / "data1.cab");
    Unshield *us = unshield_open(cab.c_str());
    if (!us) throw std::runtime_error("Couldn't open the installer on that disc image.");
    UnshieldFileGroup *g = unshield_file_group_find(us, kGroup);
    if (!g) {
      unshield_close(us);
      throw std::runtime_error("That disc image isn't the SimGolf CD (no game files in the installer).");
    }
    fs::path game = upath(g_paths.game), image = upath(g_paths.image);
    fs::create_directories(game);
    fs::path jgld_path;
    int first = (int)g->first_file, last = (int)g->last_file, total = last - first + 1;
    for (int i = first; i <= last; i++) {
      if (!unshield_file_is_valid(us, i)) continue;
      const char *dir = unshield_directory_name(us, unshield_file_directory(us, i));
      const char *name = unshield_file_name(us, i);
      std::string rel = std::string(dir ? dir : "") + (dir && *dir ? "\\" : "") + (name ? name : "");
      std::string clean;
      for (size_t k = 0, start = 0; k <= rel.size(); k++) {
        if (k == rel.size() || rel[k] == '\\' || rel[k] == '/') {
          std::string part = rel.substr(start, k - start);
          if (!part.empty() && part != "." && part != "..") clean += (clean.empty() ? "" : "/") + part;
          start = k + 1;
        }
      }
      if (clean.empty()) continue;
      fs::path out = game / upath(clean);
      fs::create_directories(out.parent_path());
      std::string out_s = path_str(out);
      if (!unshield_file_save(us, i, out_s.c_str())) {
        unshield_close(us);
        throw std::runtime_error("Unpacking " + clean + " failed.");
      }
      if (lower(clean) == "jgld.dll") jgld_path = out;
      if ((i - first) % 25 == 0) progress(0.05f + 0.85f * (i - first) / total, "Unpacking " + clean);
    }
    unshield_close(us);
    for (auto &e : root) {
      if (e.dir || !std::regex_match(e.name, kRootExtra)) continue;
      std::ofstream out(game / upath(e.name), std::ios::binary | std::ios::trunc);
      disc.extract(e, [&](const uint8_t *d, size_t n) { out.write((const char *)d, (std::streamsize)n); });
    }
    if (jgld_path.empty()) throw std::runtime_error("jgld.dll wasn't in the installer; is this the right disc?");
    fs::create_directories(game / "Saved Games");
    fs::create_directories(game / "Snapshots");

    progress(0.95f, "Preparing the game code…");
    Bytes golf = map_pe(f.exe);
    write_file(image / "golf.bin", golf.data(), golf.size());
    Bytes jimg = map_pe(read_file(jgld_path));
    for (auto [off, len] : jgld_nops(g_resources))
      if ((size_t)off + len <= jimg.size()) std::fill_n(jimg.begin() + off, len, 0x90);
    write_file(image / "jgld.bin", jimg.data(), jimg.size());
    std::string done = "installed\n";
    write_file(upath(g_paths.data + "installed.txt"), (const uint8_t *)done.data(), done.size());
    fs::remove_all(scratch, ec);
    progress(1, "Done");
    return "";
  } catch (const Missing &m) {
    *missing = true;
    fs::remove_all(scratch, ec);
    return m.what();
  } catch (const std::exception &x) {
    fs::remove_all(scratch, ec);
    return x.what();
  }
}
