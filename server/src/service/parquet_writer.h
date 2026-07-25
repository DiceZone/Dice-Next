#pragma once
// ─── Dice!Next — 最小 Parquet 写入器 ───────────────────────────────
// 针对固定的 LogOneItem 扁平 schema（9 个 REQUIRED 原始类型列，无嵌套 → 最大
// 定义/重复层级均为 0，无需 def/rep level 数据），用 PLAIN 编码 + ZSTD 页压缩，
// 手写 Thrift Compact Protocol 的文件元数据。不引入 Arrow，仅链 zstd（约 1MB）。
//
// 列名、类型和顺序由日志上传数据契约固定：
//   id UINT_64 | nickname UTF8 | IMUserId UTF8 | time INT_64 | message UTF8 |
//   isDice BOOLEAN | commandId INT_64 | commandInfo UTF8 | uniformId UTF8
//
// Parquet 文件布局：'PAR1' + 各列数据页 + FileMetaData(thrift) + i32(len) + 'PAR1'

#include <cstdint>
#include <string>
#include <vector>
#include <zstd.h>

namespace dice::parquetw {

struct LogRow {
    uint64_t id = 0;
    std::string nickname;
    std::string imUserId;
    int64_t time = 0;
    std::string message;
    bool isDice = false;
    int64_t commandId = 0;
    std::string commandInfo;
    std::string uniformId;
};

// ── Thrift Compact Protocol 编码器 ─────────────────────────────
namespace tc {
enum CType { T_BOOL_TRUE = 1, T_BOOL_FALSE = 2, T_BYTE = 3, T_I16 = 4, T_I32 = 5,
             T_I64 = 6, T_DOUBLE = 7, T_BINARY = 8, T_LIST = 9, T_SET = 10,
             T_MAP = 11, T_STRUCT = 12 };

struct Writer {
    std::string buf;
    std::vector<int> fidStack;   // 每层结构体的「上一个字段号」

    void uvarint(uint64_t v) {
        while (v >= 0x80) { buf.push_back((char)(v | 0x80)); v >>= 7; }
        buf.push_back((char)v);
    }
    void zigzag(int64_t v) { uvarint(((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }

    void beginStruct() { fidStack.push_back(0); }
    void endStruct() { buf.push_back(0x00); fidStack.pop_back(); }

    void fieldHeader(int fid, int ctype) {
        int& last = fidStack.back();
        int delta = fid - last;               // 本实现所有字段按升序写，delta ∈ [1,15]
        if (delta > 0 && delta <= 15) buf.push_back((char)((delta << 4) | ctype));
        else { buf.push_back((char)ctype); zigzag(fid); }
        last = fid;
    }
    void boolField(int fid, bool v) {
        int& last = fidStack.back();
        int ct = v ? T_BOOL_TRUE : T_BOOL_FALSE;
        int delta = fid - last;
        if (delta > 0 && delta <= 15) buf.push_back((char)((delta << 4) | ct));
        else { buf.push_back((char)ct); zigzag(fid); }
        last = fid;
    }
    void i32Field(int fid, int32_t v) { fieldHeader(fid, T_I32); zigzag(v); }
    void i64Field(int fid, int64_t v) { fieldHeader(fid, T_I64); zigzag(v); }
    void byteField(int fid, uint8_t v) { fieldHeader(fid, T_BYTE); buf.push_back((char)v); }   // i8：单字节，非 zigzag
    void strField(int fid, const std::string& s) { fieldHeader(fid, T_BINARY); uvarint(s.size()); buf += s; }

    // 列表头（size<<4|elemType；size≥15 用长式）。之后由调用方写元素（无字段头）。
    void listHeader(int size, int elemType) {
        if (size < 15) buf.push_back((char)((size << 4) | elemType));
        else { buf.push_back((char)(0xF0 | elemType)); uvarint(size); }
    }
    void listField(int fid, int size, int elemType) { fieldHeader(fid, T_LIST); listHeader(size, elemType); }
    void structField(int fid) { fieldHeader(fid, T_STRUCT); beginStruct(); }
};
}  // namespace tc

// ── 小工具：PLAIN 编码 + zstd ─────────────────────────────
namespace detail {
inline void putI64LE(std::string& s, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; ++i) s.push_back((char)((u >> (8 * i)) & 0xFF));
}
inline void putI32LE(std::string& s, int32_t v) {
    uint32_t u = (uint32_t)v;
    for (int i = 0; i < 4; ++i) s.push_back((char)((u >> (8 * i)) & 0xFF));
}
inline std::string zstd(const std::string& in) {
    size_t bound = ZSTD_compressBound(in.size());
    std::string out; out.resize(bound ? bound : 1);
    size_t n = ZSTD_compress(&out[0], out.size(), in.data(), in.size(), 3);
    if (ZSTD_isError(n)) return {};
    out.resize(n);
    return out;
}

// Parquet 物理类型 / 转换类型 / 编码 / 压缩 / 页类型 枚举值
enum PType { PT_BOOLEAN = 0, PT_INT32 = 1, PT_INT64 = 2, PT_BYTE_ARRAY = 6 };
enum Conv  { CV_UTF8 = 0, CV_UINT_64 = 14, CV_INT_64 = 18 };
enum Enc   { EN_PLAIN = 0, EN_RLE = 3 };
enum Codec { CODEC_ZSTD = 6 };
enum PageT { PAGE_DATA = 0 };
enum Rep   { REP_REQUIRED = 0 };

struct Col {
    std::string name;
    int ptype;          // 物理类型
    int conv;           // 转换类型（<0 = 无）
    std::string plain;  // PLAIN 编码后的原始列数据
};

// 组一个 DATA_PAGE 的 PageHeader（thrift compact）。
inline std::string pageHeader(int numValues, int uncompressed, int compressed) {
    tc::Writer w; w.beginStruct();
    w.i32Field(1, PAGE_DATA);          // type
    w.i32Field(2, uncompressed);       // uncompressed_page_size
    w.i32Field(3, compressed);         // compressed_page_size
    w.structField(5);                  // data_page_header
        w.i32Field(1, numValues);      //   num_values
        w.i32Field(2, EN_PLAIN);       //   encoding
        w.i32Field(3, EN_RLE);         //   definition_level_encoding（无层级也需填）
        w.i32Field(4, EN_RLE);         //   repetition_level_encoding
    w.endStruct();
    w.endStruct();
    return w.buf;
}
}  // namespace detail

// 主函数：把日志行序列化成一个完整 parquet 文件（bytes）。失败返回空串。
inline std::string buildLogParquet(const std::vector<LogRow>& rows) {
    using namespace detail;
    const int64_t N = (int64_t)rows.size();

    // 1) 各列 PLAIN 编码（列顺序须与 schema 一致）
    std::vector<Col> cols;
    // 字符串列：BYTE_ARRAY，每值 4字节LE长度 + 字节
    auto strCol = [&rows](const char* nm, int which) -> Col {
        Col c; c.name = nm; c.ptype = PT_BYTE_ARRAY; c.conv = CV_UTF8;
        for (const auto& r : rows) {
            const std::string& v = which == 0 ? r.nickname : which == 1 ? r.imUserId
                                 : which == 2 ? r.message : which == 3 ? r.commandInfo : r.uniformId;
            putI32LE(c.plain, (int32_t)v.size()); c.plain += v;
        }
        return c;
    };
    // 整数列：INT64，8字节LE
    auto i64Col = [&rows](const char* nm, int conv, int which) -> Col {
        Col c; c.name = nm; c.ptype = PT_INT64; c.conv = conv;
        for (const auto& r : rows) {
            int64_t v = which == 0 ? (int64_t)r.id : which == 1 ? r.time : r.commandId;
            putI64LE(c.plain, v);
        }
        return c;
    };
    cols.push_back(i64Col("id", CV_UINT_64, 0));
    cols.push_back(strCol("nickname", 0));
    cols.push_back(strCol("IMUserId", 1));
    cols.push_back(i64Col("time", CV_INT_64, 1));
    cols.push_back(strCol("message", 2));
    {   // isDice BOOLEAN：LSB-first 位打包
        Col c; c.name = "isDice"; c.ptype = PT_BOOLEAN; c.conv = -1;
        std::string bits; unsigned char cur = 0; int nb = 0;
        for (const auto& r : rows) { if (r.isDice) cur |= (1u << nb); if (++nb == 8) { bits.push_back((char)cur); cur = 0; nb = 0; } }
        if (nb) bits.push_back((char)cur);
        c.plain = std::move(bits);
        cols.push_back(std::move(c));
    }
    cols.push_back(i64Col("commandId", CV_INT_64, 2));
    cols.push_back(strCol("commandInfo", 3));
    cols.push_back(strCol("uniformId", 4));

    // 2) 文件体：PAR1 + 各列（pageHeader + zstd(plain)）
    std::string body = "PAR1";
    struct ChunkMeta { int64_t offset; int64_t uncompressed; int64_t compressed; };
    std::vector<ChunkMeta> metas;
    metas.reserve(cols.size());
    for (auto& c : cols) {
        std::string comp = zstd(c.plain);
        if (comp.empty() && !c.plain.empty()) return {};
        std::string ph = pageHeader((int)N, (int)c.plain.size(), (int)comp.size());
        ChunkMeta m;
        m.offset = (int64_t)body.size();                        // 页头起始 = data_page_offset
        m.uncompressed = (int64_t)ph.size() + (int64_t)c.plain.size();
        m.compressed = (int64_t)ph.size() + (int64_t)comp.size();
        body += ph; body += comp;
        metas.push_back(m);
    }

    // 3) FileMetaData（thrift compact）
    tc::Writer w; w.beginStruct();
    w.i32Field(1, 2);                                            // version（与 parquet-go 一致=2）
    // field 2: list<SchemaElement>，含 1 个根 + 9 个叶
    w.listField(2, (int)cols.size() + 1, tc::T_STRUCT);
    {   // 根节点：仅 name + num_children
        w.beginStruct();
        w.strField(4, "LogOneItem");
        w.i32Field(5, (int)cols.size());
        w.endStruct();
    }
    for (auto& c : cols) {
        w.beginStruct();
        w.i32Field(1, c.ptype);            // type（物理）
        w.i32Field(3, REP_REQUIRED);       // repetition_type
        w.strField(4, c.name);             // name
        if (c.conv >= 0) w.i32Field(6, c.conv);  // converted_type（旧，兼容）
        // field 10: logicalType（现代读取器优先用它；缺它有的染色器判「无法识别」）。
        if (c.conv == CV_UTF8) {
            w.structField(10);             // LogicalType
                w.structField(1);          //   STRING = StringType{}
                w.endStruct();
            w.endStruct();
        } else if (c.conv == CV_UINT_64 || c.conv == CV_INT_64) {
            w.structField(10);             // LogicalType
                w.structField(10);         //   INTEGER = IntType
                    w.byteField(1, 64);    //     bitWidth i8
                    w.boolField(2, c.conv == CV_INT_64);  // isSigned
                w.endStruct();
            w.endStruct();
        }
        w.endStruct();
    }
    w.i64Field(3, N);                                            // num_rows
    // field 4: list<RowGroup>（1 个）
    w.listField(4, 1, tc::T_STRUCT);
    {
        w.beginStruct();
        // field 1: list<ColumnChunk>
        w.listField(1, (int)cols.size(), tc::T_STRUCT);
        int64_t totalUncompressed = 0;
        for (size_t i = 0; i < cols.size(); ++i) {
            auto& c = cols[i]; auto& m = metas[i];
            totalUncompressed += m.uncompressed;
            w.beginStruct();
            w.i64Field(2, 0);                  // file_offset（与 parquet-go 一致=0；实际用 data_page_offset）
            w.structField(3);                  // meta_data (ColumnMetaData)
                w.i32Field(1, c.ptype);        //   type
                w.listField(2, 1, tc::T_I32);  //   encodings = [PLAIN]
                    w.zigzag(EN_PLAIN);
                w.listField(3, 1, tc::T_BINARY);//  path_in_schema = [name]
                    w.uvarint(c.name.size()); w.buf += c.name;
                w.i32Field(4, CODEC_ZSTD);     //   codec
                w.i64Field(5, N);              //   num_values
                w.i64Field(6, m.uncompressed); //   total_uncompressed_size
                w.i64Field(7, m.compressed);   //   total_compressed_size
                w.i64Field(9, m.offset);       //   data_page_offset
                w.structField(12);             //   statistics（parquet-go 都带；缺它有的读取器不认）
                    w.i64Field(3, 0);          //     null_count = 0（REQUIRED 列无空）
                w.endStruct();
            w.endStruct();
            w.endStruct();
        }
        w.i64Field(2, totalUncompressed);      // total_byte_size
        w.i64Field(3, N);                      // num_rows
        w.endStruct();
    }
    w.strField(6, "DiceNext");                                   // created_by
    w.endStruct();

    // 4) 收尾：body + metadata + i32(len) + PAR1
    std::string out = std::move(body);
    out += w.buf;
    putI32LE(out, (int32_t)w.buf.size());
    out += "PAR1";
    return out;
}

}  // namespace dice::parquetw
