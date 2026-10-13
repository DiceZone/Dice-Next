#pragma once
// ─── Dice!Next — Legacy Dice! (V2) binary deserializer ───────
// Reads the original Dice! custom binary formats (.dat / .RDconf),
// Decodes the supported binary layout into the current import structures.
// AttrVar::readb / AnysTable::readb, CharacterCard.cpp / User Player::readb).
//
// File layout (loadBFile):
//   [int32 count]  then  count × ( [key] [record] )
//   key   = fread<T>   (e.g. long long QQ for PlayerCards/UserConf)
//   record= class::readb(stream)
//
// Primitive encodings (fread<T>):
//   int=4B, short=2B, long long=8B, double=8B, bool/char=1B  (native LE)
//   string = [int16 len][len bytes]   (GBK or UTF-8, see AttrVar tag)
//
// AnysTable::readb:  [int16 len]  then len × ( [string key] [AttrVar] )
//   quirk: if the stream peeks 0 right after the length, skip 2 bytes.
// AttrVar::readb:    [char tag] + payload
//   0 Nil · 1 Bool(1B) · 2 Int(int32) · 3 Number(double) · 4 GBString ·
//   20 U8String · 21 Table · 6 Function(len+bytes) · 7 ID(int64) · 8 Set

#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <stdexcept>
#include <cmath>
#include <limits>
#include "../common/legacy_conf_value.h"
#include <nlohmann/json.hpp>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#  include <iconv.h>
#  include <cerrno>
#endif

namespace dice::legacy2 {

/// GBK (CP936) → UTF-8. Invalid input is a migration error, never invalid JSON.
inline std::string gbkToUtf8(const std::string& gbk) {
#if defined(_WIN32)
    if (gbk.empty()) return gbk;
    int wlen = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, gbk.data(), (int)gbk.size(), nullptr, 0);
    if (wlen <= 0) throw std::runtime_error("invalid GBK text in legacy data");
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, gbk.data(), (int)gbk.size(), w.data(), wlen);
    int u8 = WideCharToMultiByte(65001, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (u8 <= 0) throw std::runtime_error("cannot encode legacy text as UTF-8");
    std::string out(u8, '\0');
    WideCharToMultiByte(65001, 0, w.data(), wlen, out.data(), u8, nullptr, nullptr);
    return out;
#elif defined(__APPLE__) || defined(__linux__)
    if (gbk.empty()) return gbk;
    iconv_t converter = iconv_open("UTF-8", "GBK");
    if (converter == reinterpret_cast<iconv_t>(-1)) throw std::runtime_error("GBK converter unavailable");
    std::string out(gbk.size() * 4 + 8, '\0');
    char* input = const_cast<char*>(gbk.data()); size_t inputLeft = gbk.size();
    char* output = out.data(); size_t outputLeft = out.size();
    const size_t converted = iconv(converter, &input, &inputLeft, &output, &outputLeft);
    iconv_close(converter);
    if (converted == static_cast<size_t>(-1) || inputLeft) throw std::runtime_error("invalid GBK text in legacy data");
    out.resize(out.size() - outputLeft); return out;
#else
    throw std::runtime_error("GBK conversion unsupported on this platform");
#endif
}

/// One deserialized AttrVar value (we keep the kinds cards/users actually use).
struct AttrVal {
    enum class Kind { Nil, Bool, Int, Num, Str, Table, Set, Other };
    Kind kind = Kind::Nil;
    bool b = false;
    long long i = 0;
    double n = 0;
    std::string s;                       // UTF-8
    std::map<std::string, AttrVal> table;
    std::vector<AttrVal> set;
    uint64_t payloadBytes = 0;           // unsupported function: never executed

    bool isInt()  const { return kind == Kind::Int; }
    bool isStr()  const { return kind == Kind::Str; }
    long long asInt() const {
        if (kind == Kind::Int) return i;
        if (kind == Kind::Num) {
            if (!std::isfinite(n) || n < static_cast<double>((std::numeric_limits<long long>::min)())
                || n >= -static_cast<double>((std::numeric_limits<long long>::min)()))
                throw std::runtime_error("legacy number exceeds integer range");
            return (long long)n;
        }
        if (kind == Kind::Bool) return b ? 1 : 0;
        return 0;
    }
    std::string asStr() const { return kind == Kind::Str ? s : std::string(); }
    nlohmann::json toJson() const {
        switch (kind) {
            case Kind::Bool: return b;
            case Kind::Int: return i;
            case Kind::Num: return n;
            case Kind::Str: return s;
            case Kind::Table: {
                nlohmann::json j = nlohmann::json::object();
                for (const auto& [key, value] : table) j[key] = value.toJson();
                return j;
            }
            case Kind::Set: {
                auto j = nlohmann::json::array(); for (const auto& value : set) j.push_back(value.toJson());
                return legacy_conf::setValue(j);
            }
            case Kind::Other: return nlohmann::json{{legacy_conf::typeKey, "function"}, {"byteLength", payloadBytes}};
            default: return nullptr;
        }
    }
};

/// Little binary reader over a file stream.
class BReader {
public:
    explicit BReader(const std::filesystem::path& path, unsigned sizeWidth = 8)
        : fin_(path, std::ios::in | std::ios::binary), sizeWidth_(sizeWidth) {
        if (sizeWidth != 4 && sizeWidth != 8) throw std::runtime_error("unsupported legacy source architecture");
    }
    bool ok() const { return (bool)fin_; }
    bool eof() { return fin_.peek() == EOF; }
    int peek() { return fin_.peek(); }
    void skip(std::streamsize n) {
        if (n < 0) throw std::runtime_error("negative legacy payload length");
        fin_.ignore(n); if (fin_.gcount() != n) throw std::runtime_error("truncated legacy payload");
    }

    template <typename T> T raw() {
        T v{}; fin_.read(reinterpret_cast<char*>(&v), sizeof(T));
        if (!fin_) throw std::runtime_error("truncated legacy binary data"); return v;
    }
    bool good() const { return (bool)fin_; }
    int16_t rShort()   { return raw<int16_t>(); }
    int32_t rInt()     { return raw<int32_t>(); }
    int64_t rLL()      { return raw<int64_t>(); }
    double  rDouble()  { return raw<double>(); }
    char    rChar()    { return raw<char>(); }
    bool    rBool()    { return raw<char>() != 0; }
    uint64_t rSize()   { return sizeWidth_ == 4 ? raw<uint32_t>() : raw<uint64_t>(); }

    /// fread<string>: [int16 len][len bytes]. Raw bytes (no transcode).
    std::string rStr() {
        int16_t len = rShort();
        if (len < 0) throw std::runtime_error("negative legacy string length");
        if (len == 0) return {};
        std::string s(len, '\0');
        fin_.read(s.data(), len);
        if (!fin_) throw std::runtime_error("truncated legacy string");
        return s;
    }

private:
    std::ifstream fin_;
    unsigned sizeWidth_;
};

inline std::map<std::string, AttrVal> readAnysTableImpl(BReader& r, bool gbKeys, int depth);

/// AttrVar::readb
inline AttrVal readAttrVar(BReader& r, int depth = 0) {
    AttrVal v;
    if (depth > 64) throw std::runtime_error("legacy table nesting exceeds safety limit");
    char tag = r.rChar();
    switch (tag) {
        case 1:  v.kind = AttrVal::Kind::Bool; v.b = r.rBool(); break;
        case 2:  v.kind = AttrVal::Kind::Int;  v.i = r.rInt();  break;
        case 3:  v.kind = AttrVal::Kind::Num;  v.n = r.rDouble(); if (!std::isfinite(v.n)) throw std::runtime_error("non-finite legacy number"); break;
        case 4:  v.kind = AttrVal::Kind::Str;  v.s = gbkToUtf8(r.rStr()); break;  // GBString
        case 20: v.kind = AttrVal::Kind::Str;  v.s = r.rStr(); break;             // U8String
        case 7:  v.kind = AttrVal::Kind::Int;  v.i = r.rLL(); break;              // ID
        case 21:                                                                   // Table
        case 22: { v.kind = AttrVal::Kind::Table; v.table = readAnysTableImpl(r, tag == 22, depth + 1); break; }
        case 6:  { v.kind = AttrVal::Kind::Other; const auto len = r.rSize();
                   if (len >= (1ULL << 24)) throw std::runtime_error("invalid legacy function length");
                   v.payloadBytes = len; r.skip(static_cast<std::streamsize>(len)); break; }
        case 8:  { v.kind = AttrVal::Kind::Set; const auto cnt = r.rSize();
                   if (cnt > 100000) throw std::runtime_error("invalid legacy set size");
                   for (uint64_t k = 0; k < cnt; ++k) v.set.push_back(readAttrVar(r, depth + 1)); break; }
        case 0: v.kind = AttrVal::Kind::Nil; break;
        default: throw std::runtime_error("unknown legacy attribute tag");
    }
    return v;
}

/// AnysTable::readb (gb = GBK keys, i.e. readgb variant for tag 22/GBTable).
inline std::map<std::string, AttrVal> readAnysTableImpl(BReader& r, bool gbKeys, int depth = 0) {
    std::map<std::string, AttrVal> out;
    if (depth > 64) throw std::runtime_error("legacy table nesting exceeds safety limit");
    int16_t len = r.rShort();
    if (len < 0) throw std::runtime_error("negative legacy table size");
    if (len > 0) {
        if (r.peek() == 0) r.skip(2);   // original quirk
        for (int n = 0; n < len; ++n) {
            std::string key = r.rStr();
            if (gbKeys) key = gbkToUtf8(key);
            out[key] = readAttrVar(r, depth + 1);
        }
    }
    return out;
}
inline std::map<std::string, AttrVal> readAnysTable(BReader& r) { return readAnysTableImpl(r, false, 0); }

// ─── Character cards (PlayerCards.RDconf) ────────────────────
// CharaCard::readb is a tag-stream: [string tag] + payload, until "END".
//   mCardTag: Name(1,GBK) Tag(4,UTF8) Type(2) Attrs(3,GBK table) Attr(11,table)
//             Lock(103,set<string>) DiceExp(21,map<string,string>) Note/Info(str)
// Player::readb: short indexMax; short n; n×(u16 idx + CharaCard); short m; m×(u64 gid + u16 pcid)

struct LegacyCard {
    unsigned short idx = 0;
    std::string name;                          // UTF-8
    std::string type;                          // __Type (coc7/dnd/…)
    std::map<std::string, AttrVal> attrs;
    std::vector<std::string> locks;            // CharaCard::locks
    std::map<std::string, std::string> info;   // old Info fields
    std::string note;                          // old Note field
    std::map<std::string, std::string> diceExp;
};
struct LegacyPlayer {
    std::vector<LegacyCard> cards;
    std::map<unsigned long long, unsigned short> groupBind;   // gid → card idx
};

inline LegacyCard readCharaCard(BReader& r, unsigned short idx) {
    LegacyCard c; c.idx = idx;
    for (int guard = 0; guard < 100000; ++guard) {
        std::string tag = r.rStr();
        if (tag == "END") {
            if (auto it = c.attrs.find("__Name"); it != c.attrs.end() && it->second.isStr()) c.name = it->second.s;
            if (auto it = c.attrs.find("__Type"); it != c.attrs.end() && it->second.isStr()) c.type = it->second.s;
            return c;
        }
        if (tag == "Tag")            c.name = r.rStr();
        else if (tag == "Name")      c.name = gbkToUtf8(r.rStr());
        else if (tag == "Type")      c.type = r.rStr();
        else if (tag == "Attr")      c.attrs = readAnysTableImpl(r, false);
        else if (tag == "Attrs")     c.attrs = readAnysTableImpl(r, true);
        else if (tag == "DiceExp") {
            int16_t n = r.rShort();
            if (n < 0) throw std::runtime_error("negative legacy expression count");
            for (int i = 0; i < n && r.good(); ++i) {
                std::string k = r.rStr(), v = r.rStr();
                c.diceExp[k] = v;
            }
        }
        else if (tag == "Lock") {
            int16_t n = r.rShort();
            if (n < 0) throw std::runtime_error("negative legacy lock count");
            for (int i = 0; i < n && r.good(); ++i) c.locks.push_back(r.rStr());
        }
        // Info is an old unordered_map<string,string>; its generic binary map
        // layout starts with an int16 count, followed by key/value strings.
        else if (tag == "Info") {
            int16_t n = r.rShort();
            if (n < 0) throw std::runtime_error("negative legacy Info count");
            for (int i = 0; i < n; ++i) { auto key = r.rStr(); auto value = r.rStr(); c.info[key] = value; }
        }
        else if (tag == "Note") c.note = r.rStr();
        else throw std::runtime_error("unsupported legacy character-card field: " + tag);
    }
    throw std::runtime_error("legacy character card missing END");
}

inline LegacyPlayer readPlayer(BReader& r) {
    LegacyPlayer p;
    (void)r.rShort();                          // indexMax
    int16_t n = r.rShort();
    if (n < 0) throw std::runtime_error("negative legacy card count");
    while (n-- > 0) {
        unsigned short idx = r.raw<unsigned short>();
        LegacyCard c = readCharaCard(r, idx);
        p.cards.push_back(std::move(c));
    }
    int16_t m = r.rShort();
    if (m < 0) throw std::runtime_error("negative legacy card-binding count");
    while (m-- > 0) {
        unsigned long long gid = r.raw<unsigned long long>();
        unsigned short pcid = r.raw<unsigned short>();
        p.groupBind[gid] = pcid;
    }
    return p;
}

// ─── User config (UserConf.dat) ──────────────────────────────
// User::readb (ManagerSystem.cpp): tag-stream til "END":
//   Cfg→AnysTable(UTF8) · Conf→AnysTable(GBK) · NN→map<ll,str>(UTF8) ·
//   Nick→short len + (ll gid + GBK str) · ID→ll(ignored)
// The conf table holds "trust" and "favor" (DiceFavor) among others.
struct LegacyUser {
    std::map<std::string, AttrVal> conf;            // merged Cfg + Conf
    std::map<long long, std::string> nicks;         // per-group nick (UTF-8)
};
inline LegacyUser readUser(BReader& r) {
    LegacyUser u;
    for (int guard = 0; guard < 100000; ++guard) {
        std::string tag = r.rStr();
        if (tag == "END") return u;
        if (tag == "Cfg")  { auto t = readAnysTableImpl(r, false); for (auto& kv : t) u.conf[kv.first] = kv.second; }
        else if (tag == "Conf") { auto t = readAnysTableImpl(r, true); for (auto& kv : t) u.conf[kv.first] = kv.second; }
        else if (tag == "NN")   { int16_t n = r.rShort(); if (n < 0) throw std::runtime_error("negative legacy nickname count"); for (int i = 0; i < n; ++i) { long long k = r.rLL(); u.nicks[k] = r.rStr(); } }
        else if (tag == "Nick") { int16_t n = r.rShort(); if (n < 0) throw std::runtime_error("negative legacy nickname count"); for (int i = 0; i < n; ++i) { long long k = r.rLL(); u.nicks[k] = gbkToUtf8(r.rStr()); } }
        else if (tag == "ID")   { r.rLL(); }
        else throw std::runtime_error("unsupported legacy user field: " + tag);
    }
    throw std::runtime_error("legacy user record missing END");
}

/// Before tag streams, UserConf.RDconf used ID + int/string maps + nicknames.
inline LegacyUser readOldUser(BReader& r) {
    LegacyUser u; (void)r.rLL();
    int16_t n = r.rShort(); if (n < 0) throw std::runtime_error("negative old user integer count");
    while (n--) { auto key = gbkToUtf8(r.rStr()); AttrVal v; v.kind = AttrVal::Kind::Int; v.i = r.rInt(); u.conf[key] = v; }
    n = r.rShort(); if (n < 0) throw std::runtime_error("negative old user string count");
    while (n--) { auto key = gbkToUtf8(r.rStr()); AttrVal v; v.kind = AttrVal::Kind::Str; v.s = gbkToUtf8(r.rStr()); u.conf[key] = v; }
    n = r.rShort(); if (n < 0) throw std::runtime_error("negative old user nickname count");
    while (n--) { auto gid = r.rLL(); u.nicks[gid] = gbkToUtf8(r.rStr()); }
    return u;
}

/// Read a loadBFile-style container with long long keys. @p perRecord parses one
/// record body (positioned right after the key). Returns number of records, or
/// -1 if the file can't be opened.
inline int loadLLMap(const std::filesystem::path& path,
                     const std::function<void(BReader&, long long key)>& perRecord, unsigned sizeWidth = 8) {
    BReader r(path, sizeWidth);
    if (!r.ok()) throw std::runtime_error("cannot open legacy binary file");
    int32_t count = r.rInt();
    if (count < 0 || count > 1000000) throw std::runtime_error("invalid legacy record count");
    int n = 0;
    while (n < count) {
        long long key = r.rLL();
        perRecord(r, key);
        ++n;
    }
    if (!r.eof()) throw std::runtime_error("unexpected trailing legacy binary data");
    return n;
}

/// Width trials never invoke a writing callback: a failed trial is discarded
/// entirely before its records can enter any database or compatibility store.
template<class Record, class Read>
inline std::vector<std::pair<long long, Record>> readRecords(const std::filesystem::path& path, Read read) {
    std::string lastError;
    for (const auto width : {8U, 4U}) {
        std::vector<std::pair<long long, Record>> records;
        try {
            loadLLMap(path, [&](BReader& reader, long long id) {
                if (id <= 0) throw std::runtime_error("invalid legacy record ID");
                records.emplace_back(id, read(reader));
            }, width);
            return records;
        } catch (const std::exception& error) { lastError = error.what(); }
    }
    throw std::runtime_error("neither 64-bit nor 32-bit legacy layout passed validation: " + lastError);
}

}  // namespace dice::legacy2
