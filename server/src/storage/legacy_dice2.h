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

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace dice::legacy2 {

/// GBK (CP936) → UTF-8. On non-Windows returns input unchanged (best effort).
inline std::string gbkToUtf8(const std::string& gbk) {
#if defined(_WIN32)
    if (gbk.empty()) return gbk;
    int wlen = MultiByteToWideChar(936, 0, gbk.data(), (int)gbk.size(), nullptr, 0);
    if (wlen <= 0) return gbk;
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(936, 0, gbk.data(), (int)gbk.size(), w.data(), wlen);
    int u8 = WideCharToMultiByte(65001, 0, w.data(), wlen, nullptr, 0, nullptr, nullptr);
    if (u8 <= 0) return gbk;
    std::string out(u8, '\0');
    WideCharToMultiByte(65001, 0, w.data(), wlen, out.data(), u8, nullptr, nullptr);
    return out;
#else
    return gbk;
#endif
}

/// One deserialized AttrVar value (we keep the kinds cards/users actually use).
struct AttrVal {
    enum class Kind { Nil, Bool, Int, Num, Str, Table, Other };
    Kind kind = Kind::Nil;
    bool b = false;
    long long i = 0;
    double n = 0;
    std::string s;                       // UTF-8
    // (nested tables are parsed-and-discarded; card attrs are flat scalars)

    bool isInt()  const { return kind == Kind::Int; }
    bool isStr()  const { return kind == Kind::Str; }
    long long asInt() const {
        if (kind == Kind::Int) return i;
        if (kind == Kind::Num) return (long long)n;
        if (kind == Kind::Bool) return b ? 1 : 0;
        return 0;
    }
    std::string asStr() const { return kind == Kind::Str ? s : std::string(); }
};

/// Little binary reader over a file stream.
class BReader {
public:
    explicit BReader(const std::string& path)
        : fin_(path, std::ios::in | std::ios::binary) {}
    bool ok() const { return (bool)fin_; }
    bool eof() { return fin_.peek() == EOF; }
    int peek() { return fin_.peek(); }
    void skip(std::streamsize n) { fin_.ignore(n); }

    template <typename T> T raw() {
        T v{}; if (fin_) fin_.read(reinterpret_cast<char*>(&v), sizeof(T)); return v;
    }
    bool good() const { return (bool)fin_; }
    int16_t rShort()   { return raw<int16_t>(); }
    int32_t rInt()     { return raw<int32_t>(); }
    int64_t rLL()      { return raw<int64_t>(); }
    double  rDouble()  { return raw<double>(); }
    char    rChar()    { return raw<char>(); }
    bool    rBool()    { return raw<char>() != 0; }

    /// fread<string>: [int16 len][len bytes]. Raw bytes (no transcode).
    std::string rStr() {
        int16_t len = rShort();
        if (len <= 0 || !fin_) return {};
        std::string s(len, '\0');
        fin_.read(s.data(), len);
        return s;
    }

private:
    std::ifstream fin_;
};

inline std::map<std::string, AttrVal> readAnysTableImpl(BReader& r, bool gbKeys, int depth);

/// AttrVar::readb
inline AttrVal readAttrVar(BReader& r, int depth = 0) {
    AttrVal v;
    if (depth > 64 || !r.good()) { v.kind = AttrVal::Kind::Nil; return v; }
    char tag = r.rChar();
    switch (tag) {
        case 1:  v.kind = AttrVal::Kind::Bool; v.b = r.rBool(); break;
        case 2:  v.kind = AttrVal::Kind::Int;  v.i = r.rInt();  break;
        case 3:  v.kind = AttrVal::Kind::Num;  v.n = r.rDouble(); break;
        case 4:  v.kind = AttrVal::Kind::Str;  v.s = gbkToUtf8(r.rStr()); break;  // GBString
        case 20: v.kind = AttrVal::Kind::Str;  v.s = r.rStr(); break;             // U8String
        case 7:  v.kind = AttrVal::Kind::Int;  v.i = r.rLL(); break;              // ID
        case 21:                                                                   // Table
        case 22: { v.kind = AttrVal::Kind::Table; readAnysTableImpl(r, tag == 22, depth + 1); break; } // parse+discard
        case 6:  { v.kind = AttrVal::Kind::Other; int32_t len = r.rInt(); if (len > 0 && len < (1 << 24)) r.skip(len); break; } // Function
        case 8:  { v.kind = AttrVal::Kind::Other; long long cnt = r.rLL();                     // Set
                   for (long long k = 0; k < cnt && k < 100000 && r.good(); ++k) readAttrVar(r, depth + 1); break; }
        case 0:
        default: v.kind = AttrVal::Kind::Nil; break;
    }
    return v;
}

/// AnysTable::readb (gb = GBK keys, i.e. readgb variant for tag 22/GBTable).
inline std::map<std::string, AttrVal> readAnysTableImpl(BReader& r, bool gbKeys, int depth = 0) {
    std::map<std::string, AttrVal> out;
    if (depth > 64) return out;
    int16_t len = r.rShort();
    if (len > 0) {
        if (r.peek() == 0) r.skip(2);   // original quirk
        for (int n = 0; n < len && r.good(); ++n) {
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
};
struct LegacyPlayer {
    std::vector<LegacyCard> cards;
    std::map<unsigned long long, unsigned short> groupBind;   // gid → card idx
};

inline LegacyCard readCharaCard(BReader& r, unsigned short idx) {
    LegacyCard c; c.idx = idx;
    for (int guard = 0; guard < 100000; ++guard) {
        std::string tag = r.rStr();
        if (tag == "END" || tag.empty() || r.eof()) break;
        if (tag == "Tag")            c.name = r.rStr();
        else if (tag == "Name")      c.name = gbkToUtf8(r.rStr());
        else if (tag == "Type")      c.type = r.rStr();
        else if (tag == "Attr")      c.attrs = readAnysTableImpl(r, false);
        else if (tag == "Attrs")     c.attrs = readAnysTableImpl(r, true);
        else if (tag == "DiceExp") { int16_t n = r.rShort(); for (int i = 0; i < n; ++i) { std::string k = r.rStr(); std::string v = r.rStr(); AttrVal av; av.kind = AttrVal::Kind::Str; av.s = v; c.attrs["&" + k] = av; } }
        else if (tag == "Lock")    { int16_t n = r.rShort(); for (int i = 0; i < n; ++i) r.rStr(); }
        else if (tag == "Note" || tag == "Info") { r.rStr(); }
        else break;   // unknown tag → can't size payload, stop safely
    }
    return c;
}

inline LegacyPlayer readPlayer(BReader& r) {
    LegacyPlayer p;
    (void)r.rShort();                          // indexMax
    int16_t n = r.rShort();
    while (n-- > 0) {
        unsigned short idx = r.raw<unsigned short>();
        LegacyCard c = readCharaCard(r, idx);
        if (!c.name.empty()) p.cards.push_back(std::move(c));
    }
    int16_t m = r.rShort();
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
        if (tag == "END" || tag.empty() || r.eof()) break;
        if (tag == "Cfg")  { auto t = readAnysTableImpl(r, false); for (auto& kv : t) u.conf[kv.first] = kv.second; }
        else if (tag == "Conf") { auto t = readAnysTableImpl(r, true); for (auto& kv : t) u.conf[kv.first] = kv.second; }
        else if (tag == "NN")   { int16_t n = r.rShort(); for (int i = 0; i < n && r.good(); ++i) { long long k = r.rLL(); u.nicks[k] = r.rStr(); } }
        else if (tag == "Nick") { int16_t n = r.rShort(); for (int i = 0; i < n && r.good(); ++i) { long long k = r.rLL(); u.nicks[k] = gbkToUtf8(r.rStr()); } }
        else if (tag == "ID")   { r.rLL(); }
        else break;
    }
    return u;
}

/// Read a loadBFile-style container with long long keys. @p perRecord parses one
/// record body (positioned right after the key). Returns number of records, or
/// -1 if the file can't be opened.
inline int loadLLMap(const std::string& path,
                     const std::function<void(BReader&, long long key)>& perRecord) {
    BReader r(path);
    if (!r.ok()) return -1;
    int32_t count = r.rInt();
    int n = 0;
    while (!r.eof() && n < count) {
        long long key = r.rLL();
        perRecord(r, key);
        ++n;
    }
    return n;
}

}  // namespace dice::legacy2
