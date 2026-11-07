#include "license.h"

#include "core.h"
#include "edition.h"

#include <bcrypt.h>

#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

namespace lic {

// Публічний ключ продавця (BCRYPT_ECCPUBLIC_BLOB, P-256). Секретна пара — у keys-private/, не в репозиторії.
static const unsigned char kPublicKey[] = {
    0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00, 0xEF, 0xAA, 0xE5, 0xAE,
    0x33, 0x2F, 0xB8, 0x9D, 0x3A, 0x13, 0xBE, 0x78, 0x0C, 0xCC, 0xE4, 0xB0,
    0x75, 0x23, 0x31, 0x94, 0x50, 0x4C, 0xAA, 0x50, 0xC3, 0xDC, 0x2A, 0x89,
    0x3B, 0x0D, 0x60, 0xD6, 0x06, 0xEF, 0x20, 0xCB, 0x92, 0x06, 0x91, 0x8A,
    0x13, 0x67, 0xE8, 0x0C, 0x13, 0xC8, 0xF6, 0xC5, 0xB2, 0x5B, 0xD1, 0x36,
    0x86, 0x50, 0xCC, 0x73, 0x7C, 0x38, 0x6C, 0xC3, 0x95, 0xBD, 0x26, 0x37,
};
static const char* kMsgPrefix = "KairoBoost-License-v1";
static const char* kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static const char* kLicKey = "Software\\KairoBoost\\License";

const char* TierName(Tier t) { return t == Pro ? "Pro" : (t == Standard ? "Standard" : "—"); }

// Crockford base32 -> байти (I/L сприймаємо як 1, O — як 0)
static bool Decode(const std::string& text, std::vector<unsigned char>& out, size_t nbytes) {
    std::vector<int> vals;
    for (char c : text) {
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        c = (char)toupper((unsigned char)c);
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        const char* p = strchr(kAlphabet, c);
        if (!p || !c) return false;
        vals.push_back((int)(p - kAlphabet));
    }
    if (vals.size() != (nbytes * 8 + 4) / 5) return false;
    out.assign(nbytes, 0);
    size_t totalBits = vals.size() * 5, extra = totalBits - nbytes * 8;
    // збираємо біти зліва направо і відкидаємо зайві молодші
    size_t bit = 0;
    for (int v : vals)
        for (int b = 4; b >= 0; --b, ++bit) {
            if (bit >= nbytes * 8) continue;
            if ((v >> b) & 1) out[bit / 8] |= (unsigned char)(0x80 >> (bit % 8));
        }
    (void)extra;
    return true;
}

static bool Sha256(const unsigned char* data, size_t len, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0) == 0 && BCryptFinishHash(h, out, 32, 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool Verify(const std::string& key, Tier& tier, unsigned& serial) {
    std::vector<unsigned char> raw;
    if (!Decode(key, raw, 70)) return false;
    if (raw[0] != 1 || (raw[1] != Standard && raw[1] != Pro)) return false;
    std::vector<unsigned char> msg(kMsgPrefix, kMsgPrefix + strlen(kMsgPrefix));
    msg.insert(msg.end(), raw.begin(), raw.begin() + 6);
    unsigned char hash[32];
    if (!Sha256(msg.data(), msg.size(), hash)) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &k, (PUCHAR)kPublicKey, sizeof(kPublicKey), 0) == 0 &&
              BCryptVerifySignature(k, nullptr, hash, 32, raw.data() + 6, 64, 0) == 0;
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    tier = (Tier)raw[1];
    memcpy(&serial, raw.data() + 2, 4);
    return true;
}

// ---------------------------------------------------------------------------

static std::mutex g_m;
static bool g_cached = false;
static Status g_status;

[[maybe_unused]] static std::string RegGet(const char* name) {
    std::string v;
    lb::RegGetString(HKEY_CURRENT_USER, kLicKey, name, v);
    return v;
}

[[maybe_unused]] static void RegSet(const char* name, const std::string& v) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, kLicKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)v.size() + 1);
        RegCloseKey(k);
    }
}

static Status Compute() {
    Status s;
#if !KB_IS_SALE
    s.tier = s.keyTier = Pro;  // особиста редакція — усе відкрито
    return s;
#else
    Tier t;
    unsigned serial;
    std::string key = RegGet("Key");
    if (!key.empty() && Verify(key, t, serial)) {
        s.keyTier = t;
        s.serial = serial;
    }
    // Пробний період Pro — від першого запуску
    long long now = (long long)time(nullptr);
    std::string start = RegGet("TrialStart");
    if (start.empty()) {
        start = std::to_string(now);
        RegSet("TrialStart", start);
    }
    long long days = (now - atoll(start.c_str())) / 86400;
    int left = kTrialDays - (int)days;
    if (left < 0) left = 0;
    if (s.keyTier == Pro) {
        s.tier = Pro;
    } else if (left > 0) {
        s.tier = Pro;
        s.trial = true;
        s.trialDaysLeft = left;
    } else {
        s.tier = s.keyTier;
    }
    return s;
#endif
}

Status Get() {
    std::lock_guard<std::mutex> l(g_m);
    if (!g_cached) {
        g_status = Compute();
        g_cached = true;
    }
    return g_status;
}

bool IsPro() { return Get().tier == Pro; }
bool HasAccess() { return Get().tier != None; }

bool Activate(const std::string& key, std::string& error) {
    Tier t;
    unsigned serial;
    if (!Verify(key, t, serial)) {
        error = "Ключ недійсний. Перевір, чи скопійовано його повністю.";
        return false;
    }
    RegSet("Key", key);
    std::lock_guard<std::mutex> l(g_m);
    g_cached = false;
    return true;
}

void Deactivate() {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, kLicKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueA(k, "Key");
        RegCloseKey(k);
    }
    std::lock_guard<std::mutex> l(g_m);
    g_cached = false;
}

}  // namespace lic
