#pragma once
#include "ai_chat.h"
#include "../storage/database.h"
#include "../adapter/adapter_interface.h"

namespace dice::aichat {

template<class Store>
History loadTurnHistory(ConfigManager& cfg, Store* cst, const Message& msg, int rounds,
                        int64_t historyEnd, const std::string& quotedId, bool vision, const std::string& curText) {
    History ctx;
    namespace orm = sqlite_orm;
    if (cst && historyEnd > 0 && rounds > 0) {
        try {
            auto rows = cst->template get_all<dice::ChatMsgRow>(
                orm::where(orm::c(&dice::ChatMsgRow::id) <= historyEnd
                    and orm::c(&dice::ChatMsgRow::platform) == msg.platform
                    and orm::c(&dice::ChatMsgRow::groupId) == msg.targetId
                    and orm::c(&dice::ChatMsgRow::recalled) == 0
                    and (orm::c(&dice::ChatMsgRow::adapterId) == msg.adapterId or orm::c(&dice::ChatMsgRow::adapterId) == std::string())),
                orm::order_by(&dice::ChatMsgRow::id).desc(), orm::limit(rounds));
            const auto imageKey = [&](const std::string& id) -> dice::aichat::RecentImageObservations::Key {
                return {msg.platform, msg.adapterId, msg.targetId, id};
            };
            const bool asksAboutImage = msg.rawContent.find("[CQ:image") == std::string::npos
                && dice::aichat::refersToEarlierImage(curText);
            // Without a quote only use the sender's most recent image, and only for a visual follow-up.
            std::string priorImageId;
            if (vision && quotedId.empty() && asksAboutImage) {
                for (const auto& row : rows) {
                    if (row.adapterId == msg.adapterId && row.userId == msg.senderId
                        && dice::aichat::stripCodes(row.content).find("[图片]") != std::string::npos) {
                        priorImageId = row.msgId;
                        break;
                    }
                }
            }
            auto append = [&](const dice::ChatMsgRow& row, bool quoted) {
                dice::aichat::HistoryLine line{row.sender, dice::aichat::cleanForAi(cfg, row.content), row.self != 0};
                line.quoted = quoted;
                if (quoted) line.text = dice::aichat::truncateChars(line.text, 1500);
                if (vision && row.adapterId == msg.adapterId && (quoted || row.msgId == priorImageId))
                    line.image = dice::aichat::recentImages().find(imageKey(row.msgId));
                ctx.push_back(std::move(line));
            };
            for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                append(*it, !quotedId.empty() && it->adapterId == msg.adapterId && it->msgId == quotedId);
            if (!quotedId.empty() && std::none_of(ctx.begin(), ctx.end(), [](const auto& line) { return line.quoted; })) {
                auto cited = cst->template get_all<dice::ChatMsgRow>(
                    orm::where(orm::c(&dice::ChatMsgRow::msgId) == quotedId
                        and orm::c(&dice::ChatMsgRow::platform) == msg.platform
                        and orm::c(&dice::ChatMsgRow::adapterId) == msg.adapterId
                        and orm::c(&dice::ChatMsgRow::groupId) == msg.targetId
                        and orm::c(&dice::ChatMsgRow::recalled) == 0
                        and orm::c(&dice::ChatMsgRow::id) <= historyEnd),
                    orm::order_by(&dice::ChatMsgRow::id).desc(), orm::limit(1));
                if (!cited.empty()) append(cited.front(), true);
            }
        } catch (...) { DICE_LOG_EXCEPTION("AI chat context database read"); }
    }
    if (!quotedId.empty() && std::none_of(ctx.begin(), ctx.end(), [](const auto& line) { return line.quoted; }))
        ctx.push_back({{}, {}, false, true, {}});
    return ctx;
}

} // namespace dice::aichat
