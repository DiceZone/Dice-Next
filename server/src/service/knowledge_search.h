#pragma once
// ─── Dice!Next — 知识库检索入口 ──────────────────────────
// 把帮助系统接到知识库：帮助词条即时遍历计分（不复制正文），命中后按名取回正文；
// 开启语义检索且分配了嵌入模型时，自定义条目额外做向量比较。对话、工具调用与管理 API 共用。

#include "../core/command_router.h"
#include "knowledge_base.h"
#include <cctype>
#include <optional>

namespace dice::kb {

inline HelpSource helpSourceFor(const CommandRouter& router, Locale loc) {
    HelpSource hs;
    hs.visit = [&router, loc](const std::function<bool(const std::string&, const std::string&, const std::string&)>& fn) {
        router.visitHelp(loc, "", [&](const CommandRouter::HelpEntryView& e) { return fn(e.key, e.content, e.source); });
    };
    hs.fetch = [&router, loc](const std::string& key) {
        std::string lower = key;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        auto h = router.findHelp(loc, lower);
        return h ? h->content : std::string();
    };
    return hs;
}

inline SearchResult searchFor(ConfigManager& cfg, const CommandRouter& router, Locale loc,
                              const std::string& query, std::optional<Options> override = std::nullopt) {
    Options opt = override ? *override : optionsFrom(conf(cfg));
    HelpSource hs = helpSourceFor(router, loc);
    std::function<std::vector<std::vector<float>>(const std::vector<std::string>&)> vectors;
    if (opt.semantic && ai::hasModel(cfg, "embed"))
        vectors = [&cfg](const std::vector<std::string>& texts) {
            auto r = ai::embedFor(cfg, texts, 30);
            return r.ok ? r.vectors : std::vector<std::vector<float>>{};
        };
    auto res = KnowledgeBase::instance().search(query, opt, &hs, vectors);
    if (ai::debugOn()) {
        std::string lines;
        auto cov = [](double v) { char b[16]; std::snprintf(b, sizeof b, "%.2f", v); return std::string(b); };
        for (auto& h : res.hits)
            lines += "\n" + h.source + "「" + h.title + "」覆盖度 " + cov(h.coverage) + "，分数 " + cov(h.score);
        for (auto& h : res.belowThreshold)
            lines += "\n（未达阈值）" + h.source + "「" + h.title + "」覆盖度 " + cov(h.coverage);
        DICE_LOG_DEBUG_M("AI 知识库", "检索「{}」：计分 {} 条，耗时 {} 毫秒，命中 {} 条{}{}", query, res.scanned, res.elapsedMs,
                         res.hits.size(), res.faq ? "，FAQ 直答「" + res.faq->title + "」" : std::string(), lines);
    }
    return res;
}

}  // namespace dice::kb
