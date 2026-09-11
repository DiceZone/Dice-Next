#include "test_framework.h"
#include "../src/adapter/qq_official_adapter.h"

using namespace dice;

TEST(QQOfficialAdapter, BuildsQueryStringForGetEndpoints) {
    // 官方 GET 接口（分页游标等）只读查询串，不读请求体。
    const std::string query = QQOfficialAdapter::queryStringForTest(
        json{{"cursor", "abc"}, {"limit", 30}});
    ASSERT_EQ(query, std::string("cursor=abc&limit=30"));

    // 布尔要写成 true/false，不是 1/0。
    ASSERT_EQ(QQOfficialAdapter::queryStringForTest(json{{"flag", true}}), std::string("flag=true"));

    // 对象与数组无法出现在查询串里，跳过而不是塞一段 JSON 进去。
    ASSERT_EQ(QQOfficialAdapter::queryStringForTest(
        json{{"a", "1"}, {"nested", json::object()}, {"list", json::array({1, 2})}}),
        std::string("a=1"));

    ASSERT_EQ(QQOfficialAdapter::queryStringForTest(json::object()), std::string());
}

TEST(QQOfficialAdapter, PercentEncodesQueryValues) {
    ASSERT_EQ(QQOfficialAdapter::urlEncodeForTest("a b"), std::string("a%20b"));
    ASSERT_EQ(QQOfficialAdapter::urlEncodeForTest("a&b=c"), std::string("a%26b%3Dc"));
    // 未保留字符必须原样留下，否则 openid 会被改写。
    ASSERT_EQ(QQOfficialAdapter::urlEncodeForTest("A1b2-_.~"), std::string("A1b2-_.~"));
}

TEST(QQOfficialAdapter, MapsExtensionsToOfficialFileTypes) {
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(1, "a.png"), 1);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(1, "a.JPG"), 1);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(2, "a.mp4"), 2);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(3, "a.silk"), 3);
    // 官方只认 png/jpg、mp4、silk；其余一律降级成文件类型，而不是报错丢弃。
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(1, "a.psd"), 4);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(2, "a.mkv"), 4);
    ASSERT_EQ(QQOfficialAdapter::fileTypeForTest(4, "a.zip"), 4);
}

TEST(QQOfficialAdapter, SanitisesMediaFileNames) {
    // 无后缀时补该类型的默认后缀，否则官方按后缀判类型会失败。
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("", 1, "data/assets/abc"), std::string("abc.png"));
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("", 2, "data/assets/clip"), std::string("clip.mp4"));
    // 路径分隔符与保留字符会被换掉，避免拼出非法文件名。
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("a/b:c.png", 1, ""), std::string("a_b_c.png"));
    // 显式文件名优先于路径 basename。
    ASSERT_EQ(QQOfficialAdapter::mediaFileNameForTest("report.pdf", 4, "data/assets/xyz"), std::string("report.pdf"));
}

TEST(QQOfficialAdapter, SplitsMediaCodesAndKeepsUnsendableOnes) {
    const json parts = QQOfficialAdapter::splitMediaForTest(
        "before[CQ:image,file=https://example.test/a.png]middle"
        "[file,url=https://example.test/b.pdf,name=b.pdf]after");
    ASSERT_EQ(parts["text"].get<std::string>(), std::string("beforemiddleafter"));
    ASSERT_EQ(parts["media"].size(), static_cast<size_t>(2));
    ASSERT_EQ(parts["media"][0]["kind"].get<int>(), 1);
    ASSERT_EQ(parts["media"][0]["ref"].get<std::string>(), std::string("https://example.test/a.png"));
    ASSERT_EQ(parts["media"][1]["kind"].get<int>(), 4);
    ASSERT_EQ(parts["media"][1]["name"].get<std::string>(), std::string("b.pdf"));

    // 没有实体引用的码（入站群文件记录）发不出去，原样留在文本里而不是吞掉。
    const json kept = QQOfficialAdapter::splitMediaForTest("x[CQ:file,name=doc.txt,id=42]y");
    ASSERT_EQ(kept["media"].size(), static_cast<size_t>(0));
    ASSERT_EQ(kept["text"].get<std::string>(), std::string("x[CQ:file,name=doc.txt,id=42]y"));
}

TEST(QQOfficialAdapter, KeepsOfficialHardSizeLimit) {
    // 官方硬上限 200MB，超过直接报 850031；预检靠这个常量。
    ASSERT_EQ(QQOfficialAdapter::mediaHardLimitForTest(), static_cast<size_t>(200) * 1024 * 1024);
}
