// Let's Go NSP importer: prod.keys + NSP -> extracted/exefs/ + extracted/romfs.bin
// Based on the PFS0/NCA/RomFS logic from the Wonder project's nximport.cpp.

#include "lgimport.h"
#include "nxcrypto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace lgi {
namespace {

[[noreturn]] void fail(const std::string& msg) { throw std::runtime_error(msg); }

// ---- little-endian helpers ----
inline uint16_t rd16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
inline uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t rd64(const uint8_t* p) {
    return uint64_t(rd32(p)) | uint64_t(rd32(p + 4)) << 32;
}

// ---- storage abstraction (pread over an fd) ----
struct Storage {
    virtual ~Storage() = default;
    virtual uint64_t size() const = 0;
    virtual void read(uint64_t off, void* dst, size_t n) const = 0;
};

struct FdStorage : Storage {
    int fd;
    uint64_t sz;
    std::string name;
    FdStorage(int f, const std::string& n) : fd(f), name(n) {
        struct stat sb;
        if (fstat(f, &sb)) fail("cannot stat " + n);
        sz = sb.st_size;
    }
    uint64_t size() const override { return sz; }
    void read(uint64_t off, void* dst, size_t n) const override {
        size_t got = 0;
        while (got < n) {
            ssize_t r = pread(fd, (char*)dst + got, n - got, off + got);
            if (r <= 0) fail("read failed on " + name + " at " + std::to_string(off));
            got += r;
        }
    }
};

// ---- keys ----
struct Keys {
    std::map<std::string, std::vector<uint8_t>> k;

    static std::string lower(std::string s) {
        for (auto& c : s) c = tolower(c);
        return s;
    }
    static void trim(std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
    void load(const std::string& text) {
        for (size_t i = 0; i < text.size();) {
            size_t j = text.find('\n', i);
            std::string line = text.substr(i, j == std::string::npos ? j - i : j - i);
            i = j == std::string::npos ? text.size() : j + 1;
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string name = lower(line.substr(0, eq)), val = line.substr(eq + 1);
            trim(name); trim(val);
            if (name.empty() || val.empty()) continue;
            std::vector<uint8_t> bytes;
            for (size_t p = 0; p + 1 < val.size(); p += 2) {
                if (!isxdigit(val[p]) || !isxdigit(val[p+1])) break;
                bytes.push_back(uint8_t(strtoul(val.substr(p, 2).c_str(), nullptr, 16)));
            }
            if (!bytes.empty()) k[name] = bytes;
        }
    }
    bool get(const std::string& name, size_t n, uint8_t* out) const {
        auto it = k.find(name);
        if (it == k.end() || it->second.size() < n) return false;
        memcpy(out, it->second.data(), n);
        return true;
    }
    static void unwrap(const uint8_t kek[16], const uint8_t src[16], uint8_t out[16]) {
        nxi::Aes128 k; k.set_key(kek);
        // AES-ECB decrypt then... (simplified: actual key derivation uses AES unwrap)
        uint8_t tmp[16];
        memcpy(tmp, src, 16);
        nxi::aes_ecb_decrypt(k, tmp, 16);
        memcpy(out, tmp, 16);
        nxi::secure_zero(tmp, 16);
    }
    bool header_key(uint8_t out[32]) const {
        if (get("header_key", 32, out)) return true;
        // Derive from header_key_source if available
        uint8_t mk[16], kek_src[16], key_src[16], hkek_src[16];
        uint8_t hsrc[32], kek[16], srckek[16], hkek[16];
        bool ok = get("master_key_00", 16, mk) && get("aes_kek_generation_source", 16, kek_src) &&
                  get("aes_key_generation_source", 16, key_src) && get("header_kek_source", 16, hkek_src) &&
                  get("header_key_source", 32, hsrc);
        if (ok) {
            unwrap(mk, kek_src, kek);
            unwrap(kek, hkek_src, srckek);
            unwrap(srckek, key_src, hkek);
            uint8_t tmp[16];
            memcpy(tmp, hsrc, 16); nxi::Aes128 hk; hk.set_key(hkek);
            nxi::aes_ecb_decrypt(hk, tmp, 16); memcpy(out, tmp, 16);
            memcpy(tmp, hsrc + 16, 16);
            nxi::aes_ecb_decrypt(hk, tmp, 16); memcpy(out + 16, tmp, 16);
            nxi::secure_zero(tmp, 16);
        }
        nxi::secure_zero(mk, 16); nxi::secure_zero(kek, 16);
        nxi::secure_zero(srckek, 16); nxi::secure_zero(hkek, 16);
        return ok;
    }
};

}  // namespace
}  // namespace lgi

namespace {
// ---- PFS0 (NSP container) ----
struct Pfs0File { std::string name; uint64_t off, size; };

std::vector<Pfs0File> parse_pfs0(const Storage* st) {
    uint8_t hdr[16];
    st->read(0, hdr, 16);
    if (memcmp(hdr, "PFS0", 4)) fail("not a PFS0");
    uint32_t nfiles = rd32(hdr + 4);
    uint32_t strtab_size = rd32(hdr + 8);
    uint64_t data_off = 16 + nfiles * 24 + strtab_size;
    std::vector<Pfs0File> files;
    for (uint32_t i = 0; i < nfiles; i++) {
        uint8_t e[24];
        st->read(16 + i * 24, e, 24);
        uint64_t off = rd64(e), size = rd64(e + 8);
        uint32_t name_off = rd32(e + 16);
        // read name from string table
        char name[256] = "";
        uint64_t name_pos = 16 + nfiles * 24 + name_off;
        for (int j = 0; j < 255; j++) {
            char c; st->read(name_pos + j, &c, 1);
            if (!c) break;
            name[j] = c;
        }
        files.push_back({name, data_off + off, size});
    }
    return files;
}

// ---- NCA ----
struct NcaSection {
    uint64_t off, size;
    uint8_t ctr[16];
    int type;  // 0 = PFS0 (ExeFS), 1 = RomFS
};

struct Nca {
    const Storage* st;
    uint64_t base;
    uint8_t hdr[0x400];
    NcaSection sections[4];
    int nsections = 0;
};

void nca_decrypt_header(Nca* n, const Keys& keys) {
    uint8_t hkey[32];
    if (!keys.header_key(hkey)) fail("missing header_key");
    nxi::Aes128 k1, k2;
    k1.set_key(hkey); k2.set_key(hkey + 16);
    n->st->read(n->base, n->hdr, 0x400);
    nxi::aes_xts_decrypt(k1, k2, 0, 0x200, n->hdr, 0x400);
    if (memcmp(n->hdr + 0x200, "NCA3", 4) && memcmp(n->hdr, "NCA3", 4))
        fail("bad NCA magic (wrong keys?)");
}

}  // namespace
}  // namespace lgi
