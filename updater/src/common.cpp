// Versión, criptografía (CNG) y registro del updater.
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include "wl/adapter.h"
#include "wl/update.h"
#include "wl_update_config.h"

namespace wl::upd {

// ------------------------------------------------------------------ versión

namespace {
bool parse_uint(const std::string& s, int& out) {
    if (s.empty() || s.size() > 6) return false;
    if (s.size() > 1 && s[0] == '0') return false;  // SemVer: sin ceros a la izquierda
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}
}  // namespace

bool parse_semver(const std::string& text, SemVer& out) {
    std::string s = text;
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    if (s.empty() || s.size() > 40) return false;
    std::string core = s, pre;
    size_t dash = s.find('-');
    if (dash != std::string::npos) {
        core = s.substr(0, dash);
        pre = s.substr(dash + 1);
        if (pre.empty()) return false;
        for (const auto& part : split(pre, '.')) {
            if (part.empty()) return false;
            for (char c : part)
                if (!isalnum((unsigned char)c)) return false;
        }
    }
    auto parts = split(core, '.');
    SemVer v;
    if (parts.size() != 3 || !parse_uint(parts[0], v.major) || !parse_uint(parts[1], v.minor) || !parse_uint(parts[2], v.patch)) return false;
    v.pre = pre;
    out = v;
    return true;
}

int compare(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre == b.pre) return 0;
    if (a.pre.empty()) return 1;  // 1.0.0 > 1.0.0-beta.1
    if (b.pre.empty()) return -1;
    auto pa = split(a.pre, '.'), pb = split(b.pre, '.');
    for (size_t i = 0; i < pa.size() && i < pb.size(); ++i) {
        int na, nb;
        bool da = parse_uint(pa[i], na), db = parse_uint(pb[i], nb);
        if (da && db) {
            if (na != nb) return na < nb ? -1 : 1;
        } else if (da != db) {
            return da ? -1 : 1;  // numérico < alfanumérico
        } else if (pa[i] != pb[i]) {
            return pa[i] < pb[i] ? -1 : 1;
        }
    }
    if (pa.size() == pb.size()) return 0;
    return pa.size() < pb.size() ? -1 : 1;
}

int compare_versions(const std::string& a, const std::string& b) {
    SemVer va, vb;
    bool oka = parse_semver(a, va), okb = parse_semver(b, vb);
    if (!oka || !okb) return oka == okb ? 0 : (oka ? 1 : -1);
    return compare(va, vb);
}

std::string current_version() { return WL_VERSION; }
std::string update_repo() { return WL_UPDATE_REPO; }
std::string public_key_hex() { return WL_UPDATE_PUBKEY; }
bool testing_build() { return WL_UPDATER_TESTING != 0; }

// ------------------------------------------------------------------ CNG

namespace {

std::string to_hex(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

bool from_hex(const std::string& hex, std::vector<unsigned char>& out) {
    std::string h;
    for (char c : hex)
        if (!isspace((unsigned char)c)) h += c;
    if (h.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < h.size(); i += 2) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int a = nib(h[i]), b = nib(h[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back((unsigned char)(a * 16 + b));
    }
    return true;
}

struct Alg {
    BCRYPT_ALG_HANDLE h = nullptr;
    explicit Alg(LPCWSTR id) {
        if (BCryptOpenAlgorithmProvider(&h, id, nullptr, 0) != 0) h = nullptr;
    }
    ~Alg() {
        if (h) BCryptCloseAlgorithmProvider(h, 0);
    }
};

struct Hasher {
    Alg alg{BCRYPT_SHA256_ALGORITHM};
    BCRYPT_HASH_HANDLE h = nullptr;
    Hasher() {
        if (alg.h && BCryptCreateHash(alg.h, &h, nullptr, 0, nullptr, 0, 0) != 0) h = nullptr;
    }
    ~Hasher() {
        if (h) BCryptDestroyHash(h);
    }
    bool add(const void* p, size_t n) { return h && BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0) == 0; }
    bool finish(unsigned char out[32]) { return h && BCryptFinishHash(h, out, 32, 0) == 0; }
};

bool sha256_raw(const std::string& data, unsigned char out[32]) {
    Hasher hs;
    return hs.add(data.data(), data.size()) && hs.finish(out);
}

}  // namespace

std::string sha256_hex(const std::string& data) {
    unsigned char d[32];
    return sha256_raw(data, d) ? to_hex(d, 32) : std::string();
}

std::string sha256_file_hex(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Hasher hs;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = in.gcount();
        if (n > 0 && !hs.add(buf.data(), (size_t)n)) return {};
    }
    if (in.bad()) return {};
    unsigned char d[32];
    return hs.finish(d) ? to_hex(d, 32) : std::string();
}

bool ecdsa_verify(const std::string& public_key_hex, const std::string& data, const std::string& signature_hex) {
    std::vector<unsigned char> pub, sig;
    if (!from_hex(public_key_hex, pub) || pub.size() != 64) return false;
    if (!from_hex(signature_hex, sig) || sig.size() != 64) return false;
    unsigned char digest[32];
    if (!sha256_raw(data, digest)) return false;
    Alg alg(BCRYPT_ECDSA_P256_ALGORITHM);
    if (!alg.h) return false;
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    std::copy(pub.begin(), pub.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0) != 0) return false;
    NTSTATUS st = BCryptVerifySignature(key, nullptr, digest, 32, sig.data(), (ULONG)sig.size(), 0);
    BCryptDestroyKey(key);
    return st == 0;
}

bool ecdsa_generate(std::string& private_key_hex, std::string& public_key_hex) {
    Alg alg(BCRYPT_ECDSA_P256_ALGORITHM);
    if (!alg.h) return false;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (BCryptGenerateKeyPair(alg.h, &key, 256, 0) != 0) return false;
    bool ok = BCryptFinalizeKeyPair(key, 0) == 0;
    ULONG len = 0;
    std::vector<unsigned char> blob;
    if (ok) ok = BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &len, 0) == 0;
    if (ok) {
        blob.resize(len);
        ok = BCryptExportKey(key, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), len, &len, 0) == 0;
    }
    BCryptDestroyKey(key);
    if (!ok || blob.size() != sizeof(BCRYPT_ECCKEY_BLOB) + 96) return false;
    const unsigned char* k = blob.data() + sizeof(BCRYPT_ECCKEY_BLOB);
    private_key_hex = to_hex(k, 96);  // X || Y || d
    public_key_hex = to_hex(k, 64);
    SecureZeroMemory(blob.data(), blob.size());
    return true;
}

bool ecdsa_sign(const std::string& private_key_hex, const std::string& data, std::string& signature_hex) {
    std::vector<unsigned char> priv;
    if (!from_hex(private_key_hex, priv) || priv.size() != 96) return false;
    unsigned char digest[32];
    if (!sha256_raw(data, digest)) return false;
    Alg alg(BCRYPT_ECDSA_P256_ALGORITHM);
    if (!alg.h) return false;
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 96);
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
    hdr->cbKey = 32;
    std::copy(priv.begin(), priv.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    SecureZeroMemory(priv.data(), priv.size());
    BCRYPT_KEY_HANDLE key = nullptr;
    NTSTATUS st = BCryptImportKeyPair(alg.h, nullptr, BCRYPT_ECCPRIVATE_BLOB, &key, blob.data(), (ULONG)blob.size(), 0);
    SecureZeroMemory(blob.data(), blob.size());
    if (st != 0) return false;
    unsigned char sig[64];
    ULONG len = 0;
    st = BCryptSignHash(key, nullptr, digest, 32, sig, sizeof sig, &len, 0);
    BCryptDestroyKey(key);
    if (st != 0 || len != 64) return false;
    signature_hex = to_hex(sig, 64);
    return true;
}

// ------------------------------------------------------------------ registro

std::string redact(const std::string& text) {
    std::string out = text;
    auto replace_all = [&out](const std::string& what, const std::string& with) {
        if (what.size() < 4) return;
        for (size_t p = 0; (p = out.find(what, p)) != std::string::npos; p += with.size()) out.replace(p, what.size(), with);
    };
    if (const char* home = std::getenv("USERPROFILE")) {
        std::string h = home;
        replace_all(h, "%USERPROFILE%");
        for (char& c : h)
            if (c == '\\') c = '/';
        replace_all(h, "%USERPROFILE%");
    }
    if (const char* user = std::getenv("USERNAME")) replace_all(user, "<usuario>");
    return out;
}

void log(const std::string& line) {
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    std::error_code e;
    fs::path dir = local_app_data() / "WiiLauncher";
    fs::create_directories(dir, e);
    fs::path file = dir / "updater.log";
    if (fs::file_size(file, e) > 256 * 1024 && !e) {
        fs::path old = dir / "updater.log.1";
        fs::remove(old, e);
        fs::rename(file, old, e);
    }
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::tm tmv{};
    gmtime_s(&tmv, &t);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    std::ofstream(file, std::ios::app | std::ios::binary) << stamp << " [" << GetCurrentProcessId() << "] " << redact(line) << "\n";
}

}  // namespace wl::upd
