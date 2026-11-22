// EmuSuNi - Super Nintendo for PS4. GPL-2.0-or-later (see LICENSE).

// The PS Vita's keys from a file of the player's (ps4_vita_keys.h). Nothing here in a build with the keys inside.

#ifdef EMUSUNI_VITA_NOKEYS

#include "ps4_vita_keys.h"

#include "ps4_vita_sys.h"  // emusuni_vita_log

#include <openssl/evp.h>  // (EmuSuNi's layer over Mbed TLS: SHA-256)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

// The tables Vita3K's own sources had the keys in: here zeros until the file is read. Those of the packages, the
// game card's image and the registry are this file's (their headers only declare them in this build); those of the
// encrypted file system stay psvpfstools's own, without their values.
unsigned char pkg_vita_2[16];
unsigned char pkg_vita_3[16];
unsigned char pkg_vita_4[16];
unsigned char BIND_KEY[0x10];
extern "C" {
unsigned char emusuni_vita_registry_xor[16];
}
extern unsigned char hmac_key0[0x14];
extern unsigned char hmac_key1[0x14];
extern unsigned char iv0[0x10];
extern unsigned char contract_key0[0x10];
extern unsigned char sealedkey_retail_key[0x10];
extern unsigned char keystone_hmac_secret[0x20];
extern unsigned char passcode_hmac_secret[0x20];
extern unsigned char PFS_EncKey[0x10];

namespace {

struct Table {
    const char *name;  // in the file (scripts/make-vita-keys.py)
    unsigned char *bytes;
    size_t size;
};
const Table TABLES[] = {
    { "pkg_vita_2", pkg_vita_2, sizeof(pkg_vita_2) },
    { "pkg_vita_3", pkg_vita_3, sizeof(pkg_vita_3) },
    { "pkg_vita_4", pkg_vita_4, sizeof(pkg_vita_4) },
    { "vci_bind_key", BIND_KEY, sizeof(BIND_KEY) },
    { "registry_xor", emusuni_vita_registry_xor, sizeof(emusuni_vita_registry_xor) },
    { "pfs_hmac_key0", hmac_key0, 0x14 },
    { "pfs_hmac_key1", hmac_key1, 0x14 },
    { "pfs_iv0", iv0, 0x10 },
    { "pfs_contract_key0", contract_key0, 0x10 },
    { "pfs_sealedkey_retail_key", sealedkey_retail_key, 0x10 },
    { "pfs_keystone_hmac_secret", keystone_hmac_secret, 0x20 },
    { "pfs_passcode_hmac_secret", passcode_hmac_secret, 0x20 },
    { "pfs_enc_key", PFS_EncKey, 0x10 },
};
constexpr size_t TABLE_COUNT = sizeof(TABLES) / sizeof(TABLES[0]);

struct SceKey {
    int set = 0;
    std::string names[3];  // KeyType, SceType, SelfType
    int revision = 0;
    std::string key, iv;
    unsigned long long versions[2] = {};
};

std::vector<SceKey> s_sce;
bool s_ready = false;

void forget() {
    s_ready = false;
    s_sce.clear();
    for (const Table &table : TABLES)
        memset(table.bytes, 0, table.size);
}

bool is_hex(const std::string &text, size_t bytes) {
    if (text.size() != bytes * 2)
        return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

int hex_digit(char c) {
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

bool one_of(const std::string &name, std::initializer_list<const char *> names) {
    for (const char *known : names) {
        if (name == known)
            return true;
    }
    return false;
}

bool number(const std::string &text, unsigned long long *value) {
    if (text.empty() || text.size() > 18)
        return false;
    char *end = nullptr;
    *value = strtoull(text.c_str(), &end, 0);
    return end && *end == 0;
}

// The file's lines as their words: no comments, no empty lines.
std::vector<std::vector<std::string>> words_of(const std::string &text) {
    std::vector<std::vector<std::string>> lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::vector<std::string> words;
        size_t i = at;
        while (i < end) {
            while (i < end && static_cast<unsigned char>(text[i]) <= ' ')
                i++;
            size_t start = i;
            while (i < end && static_cast<unsigned char>(text[i]) > ' ')
                i++;
            if (i > start)
                words.push_back(text.substr(start, i - start));
        }
        if (!words.empty() && words[0][0] != '#')
            lines.push_back(std::move(words));
        at = end + 1;
    }
    return lines;
}

// The file's SHA-256 as scripts/make-vita-keys.py tells it: of each line's words one after the other and a '\n'.
std::string digest_of(const std::vector<std::vector<std::string>> &lines) {
    std::string all;
    for (const auto &words : lines) {
        for (const std::string &word : words)
            all += word;
        all += '\n';
    }
    unsigned char digest[32];
    unsigned int size = 0;
    if (!EVP_Digest(all.data(), all.size(), digest, &size, EVP_sha256(), nullptr) || size != sizeof(digest))
        return {};
    char text[65];
    for (int i = 0; i < 32; i++)
        snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}

// The lines into the tables and the list. False for a line that isn't one (never, with the SHA-256 right).
bool take(const std::vector<std::vector<std::string>> &lines) {
    bool filled[TABLE_COUNT] = {};
    for (const auto &words : lines) {
        if (words[0] == "table" && words.size() == 3) {
            size_t t = 0;
            while (t < TABLE_COUNT && words[1] != TABLES[t].name)
                t++;
            if (t == TABLE_COUNT || filled[t] || !is_hex(words[2], TABLES[t].size))
                return false;
            for (size_t i = 0; i < TABLES[t].size; i++)
                TABLES[t].bytes[i] = static_cast<unsigned char>(hex_digit(words[2][i * 2]) << 4 | hex_digit(words[2][i * 2 + 1]));
            filled[t] = true;
        } else if (words[0] == "sce" && words.size() == 10) {
            SceKey key;
            unsigned long long set = 0, revision = 0;
            if (!number(words[1], &set) || set > 1 || !number(words[4], &revision) || revision > 255
                || !one_of(words[2], { "METADATA", "NPDRM" }) || !one_of(words[3], { "SELF", "SRVK", "SPKG", "DEV" })
                || !one_of(words[9], { "NONE", "KERNEL", "APP", "BOOT", "SECURE", "USER" })
                || !(is_hex(words[5], 32) || is_hex(words[5], 16)) || !is_hex(words[6], 16)
                || !number(words[7], &key.versions[0]) || !number(words[8], &key.versions[1]))
                return false;
            key.set = static_cast<int>(set);
            key.revision = static_cast<int>(revision);
            key.names[0] = words[2];
            key.names[1] = words[3];
            key.names[2] = words[9];
            key.key = words[5];
            key.iv = words[6];
            s_sce.push_back(std::move(key));
        } else {
            return false;
        }
    }
    for (bool had : filled) {
        if (!had)
            return false;
    }
    return !s_sce.empty();
}

} // namespace

extern "C" int emusuni_vita_keys_load(const char *path) {
    forget();
    std::string text;
    FILE *file = fopen(path, "rb");
    if (!file) {
        emusuni_vita_log("vita: keys: %s isn't there. This build has no key inside: the firmware can't be installed "
                         "and no game can start (the system's modules are encrypted)",
            path);
        return 0;
    }
    char buffer[4096];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < (1u << 20))
        text.append(buffer, got);
    fclose(file);

    const auto lines = words_of(text);
    const std::string digest = digest_of(lines);
    if (digest != EMUSUNI_VITA_KEYS_SHA256) {
        emusuni_vita_log("vita: keys: %s isn't the file expected (%u bytes, its SHA-256 %s, expected %s): as if it "
                         "weren't there",
            path, static_cast<unsigned>(text.size()), digest.c_str(), EMUSUNI_VITA_KEYS_SHA256);
        return 0;
    }
    if (!take(lines)) {
        forget();
        emusuni_vita_log("vita: keys: %s has a line this build doesn't know: as if it weren't there", path);
        return 0;
    }
    s_ready = true;
    emusuni_vita_log("vita: keys: %s is the file expected: %u keys of the system's modules, %u tables", path,
        static_cast<unsigned>(s_sce.size()), static_cast<unsigned>(TABLE_COUNT));
    return 1;
}

extern "C" int emusuni_vita_keys_ready(void) {
    return s_ready ? 1 : 0;
}

extern "C" int emusuni_vita_keys_missing(const char *what) {
    if (s_ready)
        return 0;
    emusuni_vita_log("vita: keys: %s needs the PS Vita's keys (vita-keys.txt in the system folder's vita3k), which "
                     "aren't there: not done",
        what);
    return 1;
}

extern "C" int emusuni_vita_keys_sce(int index, int *set, const char **names, int *revision, const char **key,
    const char **iv, unsigned long long *versions) {
    if (!s_ready || index < 0 || static_cast<size_t>(index) >= s_sce.size())
        return 0;
    const SceKey &k = s_sce[static_cast<size_t>(index)];
    *set = k.set;
    for (int i = 0; i < 3; i++)
        names[i] = k.names[i].c_str();
    *revision = k.revision;
    *key = k.key.c_str();
    *iv = k.iv.c_str();
    versions[0] = k.versions[0];
    versions[1] = k.versions[1];
    return 1;
}

#endif // EMUSUNI_VITA_NOKEYS
