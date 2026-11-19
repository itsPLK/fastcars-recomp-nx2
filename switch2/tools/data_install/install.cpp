// The game data install (see install.h). From the upstream desktop launcher's installer
// (loader/src/install.cpp), trimmed to its data copy for the Switch 2 builder container.
//
// Layout made in HOME/data:
//   game/                    Disc 2 files + the TU's default.xexp
//   update/update.rpf        from the TU
//   user/0000000000000000/545408A7/00000002/   Disc 1 install partitions
//   data_installed.txt       written last; install_state.txt lets an interrupted run resume
#include "install.h"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "sources.h"

namespace fs = std::filesystem;

namespace loader {
namespace {

constexpr char kDataVersion[] = "fastcars data 1";
constexpr char kTitleId[] = "545408A7";
constexpr char kInstallPackageDir[] = "content/0000000000000000/545408A7/00000002/";

std::wstring Num(uint64_t v) { return std::to_wstring(v); }

std::wstring Gb(uint64_t bytes) {
  wchar_t buf[32];
  swprintf(buf, 32, L"%.1f GB", double(bytes) / (1u << 30));
  return buf;
}

// FIPS 180-4 SHA-256 (no crypto library assumed).
class Sha256 {
 public:
  void Update(const uint8_t* data, size_t size) {
    total_ += size;
    if (fill_) {
      size_t n = std::min(size, sizeof(buf_) - fill_);
      memcpy(buf_ + fill_, data, n);
      fill_ += n;
      data += n;
      size -= n;
      if (fill_ < sizeof(buf_)) return;
      Block(buf_);
      fill_ = 0;
    }
    for (; size >= 64; data += 64, size -= 64) Block(data);
    memcpy(buf_, data, size);
    fill_ = size;
  }
  std::string Hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad[72] = {0x80};
    size_t n = (fill_ < 56 ? 56 : 120) - fill_;
    for (int i = 0; i < 8; ++i) pad[n + i] = uint8_t(bits >> (56 - 8 * i));
    Update(pad, n + 8);
    static const char* k = "0123456789abcdef";
    std::string s;
    for (uint32_t w : h_) {
      for (int i = 28; i >= 0; i -= 4) s += k[(w >> i) & 15];
    }
    return s;
  }

 private:
  static uint32_t Ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void Block(const uint8_t* p) {
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 |
             p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t t1 = h + (Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
      uint32_t t2 = (Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }
  uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t buf_[64];
  size_t fill_ = 0;
  uint64_t total_ = 0;
};

bool HashFile(const fs::path& p, std::string* hex, uint64_t* size) {
  std::wstring err;
  auto blob = OpenFileBlob(p.wstring(), &err);
  if (!blob) return false;
  Sha256 h;
  std::vector<uint8_t> buf(4u << 20);
  uint64_t off = 0, total = blob->size();
  while (off < total) {
    size_t n = size_t(std::min<uint64_t>(buf.size(), total - off));
    if (!blob->ReadAt(off, buf.data(), n)) return false;
    h.Update(buf.data(), n);
    off += n;
  }
  *hex = h.Hex();
  if (size) *size = total;
  return true;
}

// Writes a file through a .part name and renames it into place when complete.
class OutFile {
 public:
  bool Open(const fs::path& p, std::wstring* error) {
    final_ = p;
    part_ = p;
    part_ += L".part";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fd_ = open(part_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      *error = L"Could not create " + part_.wstring() + L": " + Widen(strerror(errno));
      return false;
    }
    return true;
  }
  bool Write(const uint8_t* data, size_t size) {
    while (size) {
      ssize_t done = write(fd_, data, std::min<size_t>(size, 64u << 20));
      if (done < 0 && errno == EINTR) continue;
      if (done <= 0) return false;
      data += done;
      size -= size_t(done);
    }
    return true;
  }
  bool Commit() {
    bool ok = close(fd_) == 0;
    fd_ = -1;
    return ok && rename(part_.c_str(), final_.c_str()) == 0;
  }
  ~OutFile() {
    if (fd_ >= 0) {
      close(fd_);
      unlink(part_.c_str());
    }
  }

 private:
  int fd_ = -1;
  fs::path final_, part_;
};

//-----------------------------------------------------------------------------
// Expected files (switch2/data/expected.txt): <kind> <size> <sha256> <path>
// kinds: disc2 (Disc 2 file), part (install partition file, path below 00000002/),
//        tu (title update file), gen (codegen output file; not used here)
//-----------------------------------------------------------------------------

struct Expected {
  uint64_t size = 0;
  std::string sha256;
};

struct ExpectedSet {
  std::map<std::string, Expected> disc2, part, tu;
  bool Load(const fs::path& p, std::wstring* error) {
    std::ifstream f(p);
    if (!f) {
      *error = L"Missing " + p.wstring() + L".";
      return false;
    }
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      std::string kind, sha, path;
      uint64_t size = 0;
      ss >> kind >> size >> sha;
      std::getline(ss >> std::ws, path);
      auto* m = kind == "disc2" ? &disc2 : kind == "part" ? &part : kind == "tu" ? &tu : nullptr;
      if (!m || path.empty()) continue;
      (*m)[path] = Expected{size, sha};
    }
    if (disc2.empty() || part.empty() || tu.empty()) {
      *error = p.wstring() + L" is damaged.";
      return false;
    }
    return true;
  }
  uint64_t TotalBytes() const {
    uint64_t t = 0;
    for (auto* m : {&disc2, &part, &tu})
      for (auto& [k, v] : *m) t += v.size;
    return t;
  }
};

//-----------------------------------------------------------------------------
// The install
//-----------------------------------------------------------------------------

class Installer {
 public:
  Installer(fs::path home, const InstallInputs& in, InstallUi& ui)
      : home_(std::move(home)), in_(in), ui_(ui), data_(home_ / L"data") {}

  bool Run(const fs::path& expected, std::wstring* error) {
    error_ = error;
    std::error_code ec;
    fs::create_directories(data_ / L"logs", ec);
    log_.open(data_ / L"logs" / L"install.log", std::ios::app);
    Log(L"---- game data install started");
    if (!expected_.Load(expected, error)) return false;
    if (!OpenInputs()) return false;
    if (!CheckSpace()) return false;
    LoadDone();
    copy_total_ = expected_.TotalBytes();
    if (!CopyDisc2() || !CopyTitleUpdate() || !CopyInstallPartitions()) return Fail();
    if (!ReviewMismatches()) return false;
    std::ofstream(data_ / L"data_installed.txt") << kDataVersion << "\n";
    fs::remove(data_ / L"install_state.txt", ec);
    ui_.Progress(1.0);
    ui_.Stage(L"Done.");
    Log(L"---- game data install finished");
    return true;
  }

 private:
  bool Fail() {
    if (ui_.Cancelled() && error_->empty()) *error_ = L"Cancelled.";
    return false;
  }

  void Log(const std::wstring& s) {
    ui_.Log(s);
    if (log_) log_ << Narrow(s) << "\n" << std::flush;
  }

  bool SetError(const std::wstring& s) {
    *error_ = s;
    Log(L"error: " + s);
    return false;
  }

  //--- inputs ----------------------------------------------------------------

  bool OpenInputs() {
    ui_.Stage(L"Reading your discs and title update...");
    std::wstring err;
    disc2_ = OpenDisc(in_.disc2, &err);
    if (!disc2_) return SetError(L"Disc 2: " + err);
    disc1_ = OpenDisc(in_.disc1, &err);
    if (!disc1_) return SetError(L"Disc 1: " + err);

    bool disc2_has_play_data = disc2_->Find("xbox360b.rpf") && disc2_->Find("audio_rel.rpf");
    bool disc2_has_install = disc2_->Find(std::string(kInstallPackageDir) + kTitleId + "00000000");
    bool disc1_has_install = disc1_->Find(std::string(kInstallPackageDir) + kTitleId + "00000000");
    if (!disc2_has_play_data && disc2_has_install)
      return SetError(L"The file picked as Disc 2 is Disc 1 (Install). Swap the two discs.");
    if (!disc2_has_play_data || !disc2_->Find("default.xex"))
      return SetError(L"Disc 2 (Play) is missing game files. Pick the Disc 2 image or folder.");
    if (!disc1_has_install)
      return SetError(L"Disc 1 (Install) has no install packages (content\\0000000000000000\\"
                      L"545408A7\\00000002). Pick the Disc 1 image or folder.");

    // Title update: an STFS package file, or a folder with default.xexp + update.rpf.
    std::error_code ec;
    if (fs::is_directory(in_.tu, ec)) {
      tu_ = OpenDisc(in_.tu, &err);
      if (!tu_) return SetError(L"Title update: " + err);
    } else {
      auto blob = OpenFileBlob(in_.tu, &err);
      if (!blob) return SetError(L"Title update: " + err);
      if (!LooksLikeStfs(*blob))
        return SetError(L"The title update file is not an Xbox 360 package. Pick the TU27 file "
                        L"itself (for example tu00000009_00000000), not a zip.");
      tu_ = OpenStfs(std::move(blob), &err);
      if (!tu_) return SetError(L"Title update: " + err);
    }
    for (const char* name : {"default.xexp", "update.rpf"}) {
      tu_files_[name] = nullptr;
      for (const char* prefix : {"disc002/", "", "disc001/"}) {
        if (auto* e = tu_->Find(std::string(prefix) + name)) {
          tu_files_[name] = e;
          break;
        }
      }
      if (!tu_files_[name])
        return SetError(L"The title update has no " + Widen(name) + L". Pick Title Update 27.");
    }
    Log(L"Disc 1: " + disc1_->Describe());
    Log(L"Disc 2: " + disc2_->Describe());
    Log(L"Title update: " + in_.tu);
    return true;
  }

  bool CheckSpace() {
    struct statvfs sv;
    if (statvfs(data_.c_str(), &sv) != 0) return true;
    uint64_t free_bytes = uint64_t(sv.f_bavail) * sv.f_frsize;
    uint64_t need = expected_.TotalBytes() + (512ull << 20);
    // Files already in place (an interrupted earlier run) count as done.
    auto have = [&](const fs::path& p, uint64_t size) {
      std::error_code ec;
      if (fs::file_size(p, ec) == size && !ec) need -= size;
    };
    for (auto& [path, exp] : expected_.disc2) have(data_ / L"game" / Widen(path), exp.size);
    for (auto& [path, exp] : expected_.part)
      have(data_ / L"user" / L"0000000000000000" / Widen(kTitleId) / L"00000002" / Widen(path),
           exp.size);
    if (auto* e = Find(expected_.tu, "default.xexp")) have(data_ / L"game" / L"default.xexp", e->size);
    if (auto* e = Find(expected_.tu, "update.rpf")) have(data_ / L"update" / L"update.rpf", e->size);
    if (free_bytes < need)
      return SetError(L"Not enough free space for the game: " + Gb(need) + L" needed, " +
                      Gb(free_bytes) + L" free.");
    return true;
  }

  //--- copying ---------------------------------------------------------------

  // Files verified by an earlier, interrupted run: "<sha256> <size> <dest path>".
  void LoadDone() {
    std::ifstream f(data_ / L"install_state.txt");
    std::string sha, path;
    uint64_t size;
    while (f >> sha >> size && std::getline(f >> std::ws, path)) done_[path] = {size, sha};
  }

  void MarkDone(const fs::path& dest, uint64_t size, const std::string& sha) {
    std::ofstream(data_ / L"install_state.txt", std::ios::app)
        << sha << " " << size << " " << Narrow(dest.wstring()) << "\n";
    done_[Narrow(dest.wstring())] = {size, sha};
  }

  // Copies one entry to dest, hashing on the way. Records a mismatch against `exp`.
  bool CopyEntry(Source& src, const Entry& e, const fs::path& dest, const Expected* exp,
                 const std::wstring& what) {
    if (ui_.Cancelled()) return false;
    std::error_code ec;
    auto it = done_.find(Narrow(dest.wstring()));
    if (it != done_.end() && fs::file_size(dest, ec) == e.size && !ec) {
      copied_ += e.size;
      Report();
      if (exp && it->second.sha256 != exp->sha256) mismatches_.push_back(what);
      return true;
    }
    ui_.Stage(L"Copying " + what + L" (" + Gb(e.size) + L")");
    OutFile out;
    if (!out.Open(dest, error_)) return false;
    Sha256 h;
    std::wstring err;
    bool ok = src.Read(e, [&](const uint8_t* d, size_t n) {
      if (ui_.Cancelled()) return false;
      h.Update(d, n);
      if (!out.Write(d, n)) {
        err = L"Write error on " + dest.wstring() + L" (disk full?)";
        return false;
      }
      copied_ += n;
      Report();
      return true;
    }, &err);
    if (!ok) {
      if (!ui_.Cancelled()) SetError(err);
      return false;
    }
    if (!out.Commit()) return SetError(L"Could not write " + dest.wstring());
    std::string sha = h.Hex();
    MarkDone(dest, e.size, sha);
    if (exp && (sha != exp->sha256 || e.size != exp->size)) {
      mismatches_.push_back(what);
      Log(L"differs from the known dump: " + what);
    }
    return true;
  }

  void Report() {
    double f = copy_total_ ? double(copied_) / double(copy_total_) : 0;
    ui_.Progress(std::min(f, 1.0));
  }

  bool CopyDisc2() {
    Log(L"Copying Disc 2 game files");
    for (auto& [path, exp] : expected_.disc2) {
      const Entry* e = disc2_->Find(path);
      if (!e) return SetError(L"Disc 2 is missing " + Widen(path) + L". The image is incomplete.");
      if (!CopyEntry(*disc2_, *e, data_ / L"game" / Widen(path), &exp, L"Disc 2: " + Widen(path)))
        return false;
    }
    return true;
  }

  bool CopyTitleUpdate() {
    Log(L"Copying the title update");
    const Entry* xexp = tu_files_["default.xexp"];
    const Entry* rpf = tu_files_["update.rpf"];
    auto* exp_xexp = Find(expected_.tu, "default.xexp");
    auto* exp_rpf = Find(expected_.tu, "update.rpf");
    return CopyEntry(*tu_, *xexp, data_ / L"game" / L"default.xexp", exp_xexp, L"TU: default.xexp") &&
           CopyEntry(*tu_, *rpf, data_ / L"update" / L"update.rpf", exp_rpf, L"TU: update.rpf");
  }

  static const Expected* Find(const std::map<std::string, Expected>& m, const std::string& k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
  }

  bool CopyInstallPartitions() {
    Log(L"Unpacking the Disc 1 install partitions");
    for (int n = 0; n < 4; ++n) {
      std::string pkg = std::string(kTitleId) + "0000000" + char('0' + n);
      const Entry* e = disc1_->Find(kInstallPackageDir + pkg);
      if (!e) return SetError(L"Disc 1 is missing install package " + Widen(pkg));
      std::wstring err;
      auto blob = disc1_->OpenEntry(*e, &err);
      if (!blob) return SetError(L"Disc 1: " + err);
      auto stfs = OpenStfs(std::move(blob), &err);
      if (!stfs) return SetError(L"Disc 1 package " + Widen(pkg) + L": " + err);
      for (const Entry& f : stfs->entries()) {
        std::string rel = pkg + "/" + f.path;
        fs::path dest = data_ / L"user" / L"0000000000000000" / Widen(kTitleId) / L"00000002" /
                        Widen(pkg) / Widen(f.path);
        if (!CopyEntry(*stfs, f, dest, Find(expected_.part, rel), L"Disc 1: " + Widen(rel)))
          return false;
      }
    }
    return true;
  }

  bool ReviewMismatches() {
    // The recompiled code is generated from exactly one executable and patch.
    for (const std::wstring& m : mismatches_) {
      if (m.find(L"default.xexp") != std::wstring::npos)
        return SetError(L"Your title update is not Title Update 27 for this game (its default.xexp "
                        L"differs). The recompilation is made for TU27 only.");
    }
    std::string sha;
    uint64_t size = 0;
    auto exp_xex = Find(expected_.disc2, "default.xex");
    if (!HashFile(data_ / L"game" / L"default.xex", &sha, &size) || !exp_xex ||
        sha != exp_xex->sha256)
      return SetError(L"Your Disc 2 executable (default.xex) differs from the one this "
                      L"recompilation was made for, so it cannot be used.");
    if (mismatches_.empty()) {
      Log(L"All files match the known good dump.");
      return true;
    }
    std::wstring q = Num(mismatches_.size()) + L" file(s) differ from a known good dump of the "
                     L"game, for example:\n\n";
    for (size_t i = 0; i < mismatches_.size() && i < 5; ++i) q += L"  " + mismatches_[i] + L"\n";
    q += L"\nA damaged or modified dump can crash or misbehave. Continue anyway?";
    if (!ui_.Ask(q)) return SetError(L"Stopped: some game files differ from a known good dump.");
    return true;
  }

  fs::path home_;
  InstallInputs in_;
  InstallUi& ui_;
  fs::path data_;
  std::wstring* error_ = nullptr;
  std::ofstream log_;
  ExpectedSet expected_;
  std::unique_ptr<Source> disc1_, disc2_, tu_;
  std::map<std::string, const Entry*> tu_files_;
  std::map<std::string, Expected> done_;
  std::vector<std::wstring> mismatches_;
  uint64_t copy_total_ = 0, copied_ = 0;
};

}  // namespace

bool RunDataInstall(const fs::path& home, const fs::path& expected, const InstallInputs& in, InstallUi& ui,
                    std::wstring* error) {
  Installer inst(home, in, ui);
  return inst.Run(expected, error);
}

bool IsDataInstalled(const fs::path& home) {
  std::ifstream f(home / "data" / "data_installed.txt");
  std::string line;
  std::error_code ec;
  return std::getline(f, line) && line == kDataVersion &&
         fs::exists(home / "data" / "game" / "default.xex", ec);
}

}  // namespace loader
