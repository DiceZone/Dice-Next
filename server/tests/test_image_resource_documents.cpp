#include "test_framework.h"
#include "../src/service/image_resource_documents.h"
#include <chrono>
#include <fstream>

using namespace dice;
namespace fs=std::filesystem;
namespace {
struct DocumentWorkspace {
    fs::path previous=fs::current_path(),root=fs::temp_directory_path()/("dice-image-documents-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    DocumentWorkspace(){fs::create_directories(root/"data/decks");fs::current_path(root);}
    ~DocumentWorkspace(){fs::current_path(previous);std::error_code ec;fs::remove_all(root,ec);}
};
}
TEST(ImageResourceDocuments, RealSqlConfigAndDeckReferencesUpdateTogetherWithoutReplacingOtherValues) {
    DocumentWorkspace workspace;Database db;ASSERT_TRUE(db.open("data/dice.db"));
    ConfigManager cfg("config");ASSERT_TRUE(cfg.load());
    I18n i18n("i18n",Locale::kZhHans);PersonaManager personas(db,i18n,cfg);ReplyManager replies(db,cfg);CardDeck decks;
    auto resources=std::make_shared<image_resources::Service>("data");
    const std::string png=std::string("\x89PNG\r\n\x1a\n",8)+"fixture";resources->upload("reply","old.png",png);
    ReplyRuleRow rule;rule.matchType=0;rule.matchContent="image";rule.replyContent="[img,file=data/images/reply/old.png]";rule.results=R"(["::2::{sample:好|棒}[图:data/images/reply/old.png]"])";
    const auto id=db.getStorage()->insert(rule);replies.loadRules();
    cfg.set<std::string>("dice/image_management_fixture","[图:data/images/reply/old.png]");ASSERT_TRUE(cfg.save());
    {std::ofstream out("data/decks/example.json");out<<R"({"example":["[图:data/images/reply/old.png]"],"_author":["保留作者"]})";}
    image_resources::configureDocuments(resources,db,cfg,i18n,personas,replies,decks);
    const auto usages=resources->usages("reply/old.png");ASSERT_TRUE(usages["items"].size()>=std::size_t(4));
    resources->operate("rename",{"reply/old.png"},"reply/new.png");
    const auto stored=db.getStorage()->get<ReplyRuleRow>(id);ASSERT_EQ(stored.replyContent,std::string("[img,file=data/images/reply/new.png]"));
    ASSERT_TRUE(stored.results.find("::2::{sample:好|棒}")!=std::string::npos);ASSERT_TRUE(stored.results.find("reply/new.png")!=std::string::npos);
    ASSERT_EQ(cfg.get<std::string>("dice/image_management_fixture",""),std::string("[图:data/images/reply/new.png]"));
    auto cards=decks.entries("example");ASSERT_TRUE(cards.has_value());ASSERT_TRUE((*cards)[0].find("reply/new.png")!=std::string::npos);
    ASSERT_TRUE(resources->resolve("data/images/reply/old.png").has_value());
}
