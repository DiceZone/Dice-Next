#include "test_framework.h"
#include "../src/core/identity/avatar_identity.h"

#include <chrono>
#include <filesystem>

using namespace dice;
using namespace dice::identity;

namespace {
struct TempDb {
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
        ("dice-avatar-proof-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Database db;
    TempDb() {
        std::filesystem::create_directories(dir);
        db.open((dir / "test.db").string());
    }
    ~TempDb() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
};
}   // namespace

TEST(AvatarIdentity, HashesBytesStably) {
    ASSERT_EQ(avatarSha256("abc"),
              std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ASSERT_EQ(avatarSha256("abc"), avatarSha256("abc"));
    ASSERT_TRUE(avatarSha256("abc") != avatarSha256("abd"));
    ASSERT_TRUE(avatarSha256("").empty());
}

TEST(AvatarIdentity, BuildsBothOfficialEndpoints) {
    ASSERT_EQ(qqAvatarUrl("10001"), std::string("https://q1.qlogo.cn/g?b=qq&nk=10001&s=40"));
    ASSERT_EQ(openIdAvatarUrl("102000000", "ABCDEF"),
              std::string("https://q.qlogo.cn/qqapp/102000000/ABCDEF/40"));
    // 探针 OpenID 必须是 32 位十六进制，且每次不同——固定值可能被缓存成特例。
    const std::string first = defaultAvatarProbeId();
    ASSERT_EQ(first.size(), static_cast<size_t>(32));
    ASSERT_TRUE(first != defaultAvatarProbeId());
}

TEST(AvatarIdentity, MatchesOnlyWhenBytesAreIdenticalAndNotTheDefaultAvatar) {
    const std::string placeholder = "default-avatar-bytes";
    const std::string mine = "my-own-avatar-bytes";

    auto matched = classifyAvatars(placeholder, mine, mine);
    ASSERT_TRUE(matched.ok());
    ASSERT_EQ(matched.openIdSha256, matched.qqSha256);

    ASSERT_TRUE(classifyAvatars(placeholder, mine, "someone-else").status == AvatarProof::kMismatch);

    // 两个都用着默认头像的账号必须验不过，否则任何没设头像的人都能互相冒认。
    ASSERT_TRUE(classifyAvatars(placeholder, placeholder, placeholder).status
                == AvatarProof::kSharedAvatar);
}

TEST(AvatarIdentity, RefusesToConcludeWhenAnyImageIsMissing) {
    const std::string bytes = "avatar";
    ASSERT_TRUE(classifyAvatars("probe", "", bytes).status == AvatarProof::kOpenIdUnavailable);
    ASSERT_TRUE(classifyAvatars("probe", bytes, "").status == AvatarProof::kQQUnavailable);
    // 探针缺席时不能放行：没有它就分不出眼前这张是不是默认头像。
    ASSERT_TRUE(classifyAvatars("", bytes, bytes).status == AvatarProof::kProbeFailed);
}

TEST(AvatarIdentity, RejectsAnAvatarAlreadyProvenForAnotherIdentity) {
    TempDb temp;
    const std::string sha = avatarSha256("shop-avatar");

    // 没记过的哈希不算撞车。
    ASSERT_FALSE(avatarClaimedByOther(temp.db, sha, "app-1", "openid-1", "10001"));
    rememberAvatarProof(temp.db, sha, "app-1", "openid-1", "10001");

    // 同一个人重复验证仍然放行。
    ASSERT_FALSE(avatarClaimedByOther(temp.db, sha, "app-1", "openid-1", "10001"));
    // 换个 OpenID 或换个 QQ 号顶着同一张头像，就是商城头像那类共用图，必须拒绝。
    ASSERT_TRUE(avatarClaimedByOther(temp.db, sha, "app-1", "openid-2", "10002"));
    ASSERT_TRUE(avatarClaimedByOther(temp.db, sha, "app-2", "openid-1", "10001"));

    // 另一张头像不受影响。
    ASSERT_FALSE(avatarClaimedByOther(temp.db, avatarSha256("other"), "app-1", "openid-2", "10002"));
}

TEST(AvatarIdentity, RemembersEachProofOnce) {
    TempDb temp;
    const std::string sha = avatarSha256("mine");
    for (int i = 0; i < 3; ++i) rememberAvatarProof(temp.db, sha, "app-1", "openid-1", "10001");
    const auto rows = temp.db.getStorage()->get_all<AvatarProofRow>();
    ASSERT_EQ(rows.size(), static_cast<size_t>(1));
    ASSERT_EQ(rows.front().qq, std::string("10001"));

    // 空哈希不入库，免得"取不到图"被记成一条可撞车的记录。
    rememberAvatarProof(temp.db, "", "app-1", "openid-9", "10009");
    ASSERT_EQ(temp.db.getStorage()->get_all<AvatarProofRow>().size(), static_cast<size_t>(1));
}
