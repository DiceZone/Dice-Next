// Included inside CommandRouter. A game day is explicitly started by the
// player/Keeper; wall-clock dates cannot represent a scenario's timeline.
static int coc7DayThreshold(int san) { return (std::max)(1, san / 5); }
json coc7Day(const Message& msg, const json& data) const {
    const auto meta = data.find("__meta");
    if (meta == data.end() || !meta->is_object()) return json::object();
    const auto days = meta->find("coc7SanityDays");
    if (days == meta->end() || !days->is_object()) return json::object();
    const auto day = days->find(coc7rScope(msg));
    if (day == days->end() || !day->is_object()) return json::object();
    for (const char* key : {"start", "loss"})
        if (!day->contains(key) || !(*day)[key].is_number_integer()) return json::object();
    if ((*day)["start"] < 1 || (*day)["start"] > 99 || (*day)["loss"] < 0 || (*day)["loss"] > 1000000)
        return json::object();
    return *day;
}
std::string coc7SanityText(Locale loc, const std::string& key, const I18n::Args& args = {}) const {
    return i18n_.tr(loc, "card.sc." + key, args);
}
bool coc7SaveDay(const Message& msg, const CharacterCardStore::Snapshot& before,
                const json& day, std::optional<int> san = {}) {
    auto data = before.data;
    if (!data.contains("__meta")) data["__meta"] = json::object();
    auto& meta = data["__meta"];
    if (!meta.is_object()) return false;
    if (!meta.contains("coc7SanityDays")) meta["coc7SanityDays"] = json::object();
    auto& days = meta["coc7SanityDays"];
    if (!days.is_object()) return false;
    if (day.empty()) days.erase(coc7rScope(msg));
    else days[coc7rScope(msg)] = day;
    if (san) {
        for (auto it = data.begin(); it != data.end();) {
            if (CharacterCardStore::canonical(it.key()) == "理智") it = data.erase(it);
            else ++it;
        }
        if (meta.contains("texts") && meta["texts"].is_object()) {
            auto& texts = meta["texts"];
            for (auto it = texts.begin(); it != texts.end();) {
                if (CharacterCardStore::canonical(it.key()) == "理智") it = texts.erase(it);
                else ++it;
            }
        }
        data["理智"] = *san;
    }
    return cards_.replaceSnapshot(msg.senderId, before, data);
}
std::string coc7DayCommand(Locale loc, const Message& msg, const std::string& command) {
    const auto card = cards_.snapshot(msg.senderId, cards_.boundCard(msg.senderId, cardScope(msg)));
    if (!card) return coc7SanityText(loc, "no_san");
    const auto day = coc7Day(msg, card->data);
    if (command == "show") {
        if (day.empty()) return coc7SanityText(loc, "day_off");
        const int start = day["start"], loss = day["loss"];
        return coc7SanityText(loc, "day_status", {{"san", std::to_string(start)},
            {"loss", std::to_string(loss)}, {"threshold", std::to_string(coc7DayThreshold(start))}});
    }
    if (cards_.cardLocked(msg.senderId, cardScope(msg), "w")) return i18n_.tr(loc, "card.locked_w");
    if (command == "day off") {
        if (!coc7SaveDay(msg, *card, json::object())) return coc7SanityText(loc, "conflict");
        return coc7SanityText(loc, "day_off");
    }
    auto san = cards_.getAttr(msg.senderId, cardScope(msg), "理智");
    if (!san) san = cards_.getAttr(msg.senderId, cardScope(msg), "意志");
    if (!san) return coc7SanityText(loc, "no_san");
    if (*san <= 0 || *san > 99) return coc7SanityText(loc, "invalid_san");
    if (!coc7SaveDay(msg, *card, {{"start", *san}, {"loss", 0}})) return coc7SanityText(loc, "conflict");
    return coc7SanityText(loc, "day_started", {{"san", std::to_string(*san)},
        {"threshold", std::to_string(coc7DayThreshold(*san))}});
}
// Only append confirmed outcomes. Missing INT and explicit-SAN calculations
// ask for an INT roll rather than borrowing unrelated/default card attributes.
std::string coc7InsanityNotice(Locale loc, const Message& msg, int finalSan, int loss,
                               bool fromCard, const json& day) {
    if (finalSan == 0) return "\n" + coc7SanityText(loc, "permanent");
    if (!day.empty() && day["loss"].get<int>() >= coc7DayThreshold(day["start"].get<int>()))
        return "\n" + coc7SanityText(loc, "indefinite", {{"loss", std::to_string(day["loss"].get<int>())},
            {"threshold", std::to_string(coc7DayThreshold(day["start"].get<int>()))}});
    if (loss < 5) return {};
    const auto intel = fromCard ? cards_.getAttr(msg.senderId, cardScope(msg), "智力") : std::nullopt;
    if (!intel || *intel < 1) return "\n" + coc7SanityText(loc, "int_needed");
    const auto roll = engine_.roll("1d100");
    if (!roll.ok()) return "\n" + coc7SanityText(loc, "int_error", {{"error", roll.error}});
    // Core CoC7 regular INT check, independent of .setcoc house rules.
    const bool success = roll.modifiedTotal != 100 && (roll.modifiedTotal == 1 || roll.modifiedTotal <= *intel);
    I18n::Args args{{"res", std::to_string(roll.modifiedTotal)}, {"int", std::to_string(*intel)}};
    if (!success) return "\n" + coc7SanityText(loc, "int_failed", args);
    const auto duration = engine_.roll("1d10");
    if (!duration.ok()) return "\n" + coc7SanityText(loc, "temporary_duration_pending", args);
    args["hours"] = std::to_string(duration.modifiedTotal);
    return "\n" + coc7SanityText(loc, "temporary", args);
}
