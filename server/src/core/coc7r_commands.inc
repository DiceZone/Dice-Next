// Included inside CommandRouter. Native rules are opt-in and never register
// aliases/defaults globally. Page references use the printed book pagination.
bool coc7rOn(const Message& msg) const {
    auto pack = activeRulePack(msg);
    return pack && pack->mechanics == coc7r::profile;
}
std::string coc7rText(Locale loc, const std::string& key, const I18n::Args& args = {}) const {
    return i18n_.tr(loc, "coc7r." + key, args);
}
bool coc7rOption(const Message& msg, const std::string& key) const {
    const auto saved = getGroupSetting(msg, "coc7r:option:" + key);
    if (!saved.empty()) return saved == "1";
    auto pack = activeRulePack(msg);
    return pack && pack->nativeOptions.count(key) && pack->nativeOptions.at(key);
}
std::string profileAttribute(const Message& msg, const std::string& name) const {
    if (!coc7rOn(msg)) return name;
    return coc7r::canonical(CharacterCardStore::canonical(coc7r::canonical(name)));
}
std::optional<int> coc7rAttr(const Message& msg, const std::string& raw) const {
    const auto name = profileAttribute(msg, raw);
    // Do not merge old Charm/Fast Talk/Intimidate into Persuade. Explicit fields
    // remain untouched. Only subsequent input/check aliases use the new mapping.
    const auto lookup = [&](const std::string& key) { return cards_.getAttr(msg.senderId, cardScope(msg), key); };
    if (name == "冲锋枪" || name == "宗教") return {};
    if (name == "最大理智" || name == "最大理智值") return (std::max)(0, 99 - (std::clamp)(lookup("克苏鲁神话").value_or(0), 0, 99));
    const bool familySkill=std::find(coc7r::fighting().begin(),coc7r::fighting().end(),name)!=coc7r::fighting().end()
        || std::find(coc7r::firearms().begin(),coc7r::firearms().end(),name)!=coc7r::firearms().end();
    if (!familySkill) if (auto v = lookup(name)) return v;
    if (name == "移动") {
        auto str = lookup("力量"), dex = lookup("敏捷"), siz = lookup("体型");
        if (!str || !dex || !siz || *str < 0 || *dex < 0 || *siz < 0) return {};
        return coc7r::movement(*str, *dex, *siz, lookup("年龄").value_or(20));
    }
    if (name == "兴趣点") {
        auto edu = lookup("教育"), intel = lookup("智力");
        if (!edu || !intel || *edu < 0 || *intel < 0 || int64_t(*edu)+*intel > 1000000) return {};
        return *edu + *intel;
    }
    if (name == "最大魔法值") { auto v=lookup("意志"); return v ? std::optional<int>(*v/5) : std::nullopt; }
    if (name == "最大生命值") {
        auto con=lookup("体质"), siz=lookup("体型");
        if (!con || !siz || *con < 0 || *siz < 0) return {};
        return static_cast<int>((int64_t(*con)+*siz)/10);
    }
    return coc7r::value(name, lookup, coc7rOption(msg,"bases"), coc7rOption(msg,"specialties"));
}
// State is stored with the card and survives renaming/restart/export. Scenario
// state is isolated by platform + adapter + conversation, not by display names.
std::string coc7rScope(const Message& msg) const {
    return json::array({msg.platform, msg.adapterId, cardScope(msg)}).dump();
}
json coc7rState(const Message& msg) const {
    auto card = cards_.snapshot(msg.senderId, cards_.boundCard(msg.senderId, cardScope(msg)));
    if (!card) return json::object();
    return coc7rStateFrom(msg, card->data);
}
json coc7rStateFrom(const Message& msg, const json& data) const {
    auto meta=data.find("__meta"); if (meta==data.end() || !meta->is_object()) return json::object();
    auto profiles=meta->find("coc7r"); if (profiles==meta->end() || !profiles->is_object()) return json::object();
    auto state=profiles->find(coc7rScope(msg));
    return state!=profiles->end() && state->is_object() ? *state : json::object();
}
bool coc7rSave(const Message& msg, const CharacterCardStore::Snapshot& before,
              const coc7r::SanityState& state, const std::map<std::string,int>& attrs = {}, bool mythosKnown=false) {
    auto data = before.data;
    if (!data.contains("__meta")) data["__meta"]=json::object();
    if (!data["__meta"].is_object()) return false;
    auto& meta=data["__meta"];
    if (!meta.contains("coc7r")) meta["coc7r"]=json::object();
    if (!meta["coc7r"].is_object()) return false;
    meta["coc7r"][coc7rScope(msg)]=coc7r::jsonState(state);
    if (mythosKnown) meta["coc7rMythosKnown"]=true;
    for (const auto& [key,value]:attrs) data[key]=value;
    // Compare-and-swap prevents a concurrent edit from being overwritten. It
    // writes SAN and state together, including for the unnamed/default card.
    return cards_.replaceSnapshot(msg.senderId,before,data);
}
struct Coc7rRollError : std::runtime_error { using std::runtime_error::runtime_error; };
int coc7rRoll(const std::string& expr) {
    auto roll=engine_.roll(expr);
    if (!roll.ok()) throw Coc7rRollError(roll.error);
    return roll.modifiedTotal;
}
std::string coc7rStatus(Locale loc, const Message& msg, const coc7r::SanityState& state) const {
    auto pow=coc7rAttr(msg,"意志");
    return coc7rText(loc,"status", {{"nick",displayName(msg)},
        {"loss",std::to_string(state.chapterLoss)},{"threshold",pow ? std::to_string(*pow/5) : "?"},
        {"shock",coc7rText(loc,state.shock?"yes":"no")},{"rounds",std::to_string(state.shockRounds)},
        {"aftershock",coc7rText(loc,state.aftershock?"yes":"no")},{"changed",coc7rText(loc,state.changed?"yes":"no")},
        {"reassured",coc7rText(loc,state.reassured?"yes":"no")},
        {"reality",coc7rText(loc,state.realityProtected?"yes":"no")}});
}
std::string coc7rGenerate(Locale loc, const Message& msg, int count) {
    if (!groupFeatureEnabled(msg,"roll")) return {};
    std::string values;
    for (int n=0;n<count;++n) {
        if (!values.empty()) values+="\n";
        for (int k=0;k<8;++k) {
            if (k) values+=" / ";
            values+=std::to_string((coc7rRoll("2d6")+5)*5);
        }
    }
    return coc7rText(loc,"generate",{{"nick",displayName(msg)},{"values",values}});
}
std::string coc7rApplyLoss(Locale loc, const Message& msg,
    const CharacterCardStore::Snapshot& card, int san, int requestedLoss, bool mythos,
    const std::string& lead) {
    auto pow=coc7rAttr(msg,"意志"), intel=coc7rAttr(msg,"智力");
    if (!pow || *pow<=0 || *pow>1000000 || !intel || *intel<0 || *intel>1000000)
        return coc7rText(loc,"need_stats");
    auto state=coc7r::sanityState(coc7rStateFrom(msg,card.data));
    auto result=coc7r::loseSanity(state,san,requestedLoss,*pow);
    std::string detail;
    if (result.loss>0) detail+="\n"+coc7rText(loc,"involuntary");
    if (result.intRequired) {
        int r=coc7rRoll("1d100");
        const auto grade=rollSuccessLevel(r,*intel,0);
        const bool passed=grade!=SuccessLevel::kFailure && grade!=SuccessLevel::kFumble;
        result=coc7r::loseSanity(state,san,requestedLoss,*pow,passed);
        detail+="\n"+coc7rText(loc,"int_check",{{"roll",std::to_string(r)},{"value",std::to_string(*intel)},
            {"level",i18n_.tr(loc,levelKey(grade))}});
    }
    if (result.newShock) {
        result.state.shockRounds=coc7rRoll("1d10");
        detail+="\n"+coc7rText(loc,"shock_choice",{{"rounds",std::to_string(result.state.shockRounds)}});
    }
    if (result.newChanged) detail+="\n"+coc7rText(loc,"changed_help");
    if (result.lost) detail+="\n"+coc7rText(loc,"lost");
    if (mythos && (result.newShock || result.newChanged)) {
        result.state.mythosPending=(std::min)(1000000,result.state.mythosPending+1);
        detail+="\n"+coc7rText(loc,"mythos_pending");
    }
    if (!coc7rSave(msg,card,result.state,{{"理智",result.remaining}})) return coc7rText(loc,"conflict");
    return lead + "\n"+coc7rText(loc,"loss_result",{{"loss",std::to_string(result.loss)},
        {"final",std::to_string(result.remaining)},{"total",std::to_string(result.state.chapterLoss)}})+detail;
}
std::string coc7rSanityCheck(Locale loc, const Message& msg, const std::string& input) {
    // Unlike the old SC parser, malformed extra arguments cannot silently fall
    // back to the card and mutate it. Explicit SAN is a calculation only.
    std::istringstream stream(input); std::string costs, arg; stream>>costs;
    auto slash=costs.find('/');
    if (slash==std::string::npos) return coc7rText(loc,"sc_usage");
    auto success=costs.substr(0,slash), failure=costs.substr(slash+1);
    auto bounds=coc7r::cost(success), failBounds=coc7r::cost(failure);
    if (!bounds || !failBounds) return coc7rText(loc,"sc_usage");
    bool mythos=false; std::optional<int> explicitSan;
    while (stream>>arg) {
        if (arg=="mythos" && !mythos) mythos=true;
        else if (!explicitSan && coc7r::number(arg,99)) explicitSan=coc7r::number(arg,99);
        else return coc7rText(loc,"sc_usage");
    }
    auto san=explicitSan ? explicitSan : cards_.getAttr(msg.senderId,cardScope(msg),"理智");
    if (!san && !explicitSan) san=coc7rAttr(msg,"意志");   // Starting SAN equals POW (p.85).
    if (!san || *san<=0 || *san>99) return i18n_.tr(loc,"card.sc.invalid_san");
    auto card=cards_.snapshot(msg.senderId,cards_.boundCard(msg.senderId,cardScope(msg)));
    if (!explicitSan) {
        if (!card) return coc7rText(loc,"need_stats");
        if (cards_.cardLocked(msg.senderId,cardScope(msg),"w")) return i18n_.tr(loc,"card.locked_w");
        auto pow=coc7rAttr(msg,"意志"), intel=coc7rAttr(msg,"智力");
        if (!pow || *pow<=0 || !intel || *intel<0) return coc7rText(loc,"need_stats");
        if (coc7r::sanityState(coc7rStateFrom(msg,card->data)).shock) return coc7rText(loc,"shock_immune");
    }
    const int r=coc7rRoll("1d100");
    // Companion retains the core SAN roll thresholds. Custom skill house rules
    // do not add critical healing or change SAN's no-bonus/no-push semantics.
    auto grade=rollSuccessLevel(r,*san,0);
    const bool failed=grade==SuccessLevel::kFailure || grade==SuccessLevel::kFumble;
    const int loss=grade==SuccessLevel::kFumble ? failBounds->maximum : coc7rRoll(failed?failure:success);
    auto args=outcomeArgs(displayName(msg),"理智","","1D100="+std::to_string(r),r,*san,i18n_.tr(loc,levelKey(grade)));
    args["res"]=std::to_string(r); args["san"]=std::to_string(*san); args["grade"]=i18n_.tr(loc,levelKey(grade));
    args["loss"]=std::to_string((std::min)(loss,*san)); args["final"]=std::to_string((std::max)(0,*san-loss));
    auto custom=i18n_.trCandidates(loc,check_reply::candidates("sanity",replyGrade(grade)),args);
    std::string lead=custom.value_or(i18n_.tr(loc,loss?"card.sc.result":"card.sc.result_noloss",args));
    if ((failed && loss==0) || (explicitSan && loss>0)) lead+="\n"+coc7rText(loc,"involuntary");
    if (explicitSan) return lead+"\n"+coc7rText(loc,"calculation_only");
    return coc7rApplyLoss(loc,msg,*card,*san,loss,mythos,lead);
}
std::optional<std::string> tryHandleCoc7r(Locale loc, const Message& msg, const std::string& command) {
    try { return handleCoc7rCommand(loc,msg,command); }
    catch (const Coc7rRollError& e) { return i18n_.tr(loc,"dice.error.roll",{{"error",e.what()}}); }
}
std::optional<std::string> handleCoc7rCommand(Locale loc, const Message& msg, const std::string& command) {
    auto [raw,rest]=splitCommand(command);
    if (toLower(raw)!="coc7r") return std::nullopt;
    auto [action,param]=splitCommand(trim(rest)); action=toLower(action); param=trim(param);
    if (action=="help" || action=="?") return coc7rText(loc,"help");
    if (action.empty() || isAllDigits(action)) {
        if (!param.empty()) return coc7rText(loc,"help");
        return coc7rGenerate(loc,msg,action.empty()?1:clampCount(parseIntOr(action,1)));
    }
    if (!coc7rOn(msg)) return coc7rText(loc,"inactive");
    if (action=="options") {
        if (param.empty()) return coc7rText(loc,"options",{{"bases",coc7rOption(msg,"bases")?"on":"off"},
            {"specialties",coc7rOption(msg,"specialties")?"on":"off"},{"take5",coc7rOption(msg,"take5")?"on":"off"}});
        auto [key,value]=splitCommand(param);
        if ((key!="bases" && key!="specialties" && key!="take5") || (value!="on" && value!="off"))
            return coc7rText(loc,"options_usage");
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc,"fun.set.no_perm");
        setGroupSetting(msg,"coc7r:option:"+key,value=="on"?"1":"0");
        return coc7rText(loc,"option_saved",{{"key",key},{"value",value}});
    }
    if (!groupFeatureEnabled(msg,"roll")) return std::string();
    if (action=="array") {
        auto index=coc7r::number(param,10);
        if (!index || *index<1) return coc7rText(loc,"array_usage");
        std::string values; for (int value:coc7r::arrays()[*index-1]) { if (!values.empty()) values+=" / "; values+=std::to_string(value); }
        return coc7rText(loc,"generate",{{"nick",displayName(msg)},{"values",values}});
    }
    if (action=="points") return coc7rText(loc,"points");
    if (action=="mov") {
        auto mov=coc7rAttr(msg,"移动"); if (!mov || *mov<0 || *mov>1000) return coc7rText(loc,"need_stats");
        return coc7rText(loc,"movement",{{"mov",std::to_string(*mov)},{"action",std::to_string(*mov*2)},{"turn",std::to_string(*mov*4)}});
    }
    if (action=="fire") {
        std::istringstream s(param); std::string rounds,damage,armor,extra; s>>rounds>>damage>>armor;
        auto n=coc7r::number(rounds,100), d=coc7r::number(damage,10000), a=coc7r::number(armor,10000);
        if (!n || !d || !a || (s>>extra) || !coc7r::automaticMultiplier(*n)) return coc7rText(loc,"fire_usage");
        auto mult=coc7r::automaticMultiplier(*n);
        return coc7rText(loc,"fire",{{"damage",std::to_string(*d*mult)},{"armor",std::to_string(*a*mult)},
            {"net",std::to_string((std::max)(0,(*d-*a)*mult))}});
    }
    if (action=="spray") {
        auto rounds=coc7r::number(param,100);
        if (!rounds || !coc7r::sprayWidth(*rounds)) return coc7rText(loc,"fire_usage");
        return coc7rText(loc,"spray",{{"width",std::to_string(coc7r::sprayWidth(*rounds))}});
    }
    if (action=="chase") {
        std::istringstream s(param); std::string points,result,place,extra; s>>points>>result>>place;
        auto p=coc7r::number(points,10000), l=place.empty()?std::optional<int>(1):coc7r::number(place,10000);
        if (!p || *p==0 || !l || *l<1 || (s>>extra) || (result!="success" && result!="fail" && result!="pushfail"))
            return coc7rText(loc,"chase_usage");
        int loss=result=="fail"?coc7rRoll("1d6"):0;
        auto out=coc7r::chase(*p,*l,result=="success",loss,result=="pushfail");
        return coc7rText(loc,"chase",{{"loss",std::to_string(result=="pushfail"?*p:(std::min)(*p,loss))},
            {"mvp",std::to_string(out.mvp)},{"location",std::to_string(out.location)},
            {"out",coc7rText(loc,out.out?"yes":"no")}});
    }
    if (action=="reassure") {
        if (msg.type==MessageType::kPrivate) return i18n_.tr(loc,"fun.set.group_only");
        if (!senderIsGroupAdmin(msg)) return i18n_.tr(loc,"gate.no_perm"); // Changing another player's SAN requires Keeper/admin authority.
        auto [target,mode]=splitCommand(param);
        // Adapters strip @ segments from the text and list them in atList.
        if (auto at=atTarget(msg); !at.empty()) { target=at; mode=param; }
        if (target.empty() || target.size()>128 || (!mode.empty() && mode!="push")) return coc7rText(loc,"reassure_usage");
        if (target==msg.senderId) return coc7rText(loc,"reassure_self");
        Message recipient=msg; recipient.senderId=target;
        auto card=cards_.snapshot(target,cards_.boundCard(target,cardScope(msg)));
        auto san=cards_.getAttr(target,cardScope(msg),"理智"), pow=coc7rAttr(recipient,"意志"), intel=coc7rAttr(recipient,"智力");
        auto skill=coc7rAttr(msg,"安抚");
        if (!card || !san || *san<=0 || *san>99 || !pow || *pow<=0 || !intel || *intel<0 || !skill || *skill<0)
            return coc7rText(loc,"need_stats");
        if (cards_.cardLocked(target,cardScope(msg),"w")) return i18n_.tr(loc,"card.locked_w");
        auto state=coc7r::sanityState(coc7rStateFrom(recipient,card->data));
        int r=coc7rRoll("1d100"); auto grade=rollSuccessLevel(r,*skill,0);
        std::string lead=coc7rText(loc,"reassure_check",{{"target",target},{"roll",std::to_string(r)},
            {"value",std::to_string(*skill)},{"level",i18n_.tr(loc,levelKey(grade))}});
        if (grade==SuccessLevel::kFumble || (grade==SuccessLevel::kFailure && mode=="push"))
            return coc7rApplyLoss(loc,recipient,*card,*san,coc7rRoll(coc7r::reassureFailureCost),false,lead);
        if (grade==SuccessLevel::kFailure) return lead;
        // Once per chapter for the SAN gain (p.22); ending Shock still applies.
        if (state.reassured) lead+="\n"+coc7rText(loc,"reassure_used");
        const int final=(std::min)(*san+(state.reassured?0:2),coc7rAttr(recipient,"最大理智").value_or(99));
        state.reassured=true; state.shock=false; state.shockRounds=0;
        if (!coc7rSave(recipient,*card,state,{{"理智",(std::max)(*san,final)}})) return coc7rText(loc,"conflict");
        return lead+"\n"+coc7rText(loc,"reassure_success",{{"gain",std::to_string((std::max)(0,final-*san))},
            {"final",std::to_string((std::max)(*san,final))},{"minutes",std::to_string(coc7rRoll("1d6"))}});
    }
    auto card=cards_.snapshot(msg.senderId,cards_.boundCard(msg.senderId,cardScope(msg)));
    if (!card) return coc7rText(loc,"need_stats");
    auto state=coc7r::sanityState(coc7rStateFrom(msg,card->data));
    if (action=="show") return coc7rStatus(loc,msg,state);
    if (cards_.cardLocked(msg.senderId,cardScope(msg),"w")) return i18n_.tr(loc,"card.locked_w");
    if (action=="init" && param.empty()) {
        if (cards_.getAttr(msg.senderId,cardScope(msg),"理智")) return coc7rText(loc,"san_exists");
        auto pow=coc7rAttr(msg,"意志"); if (!pow || *pow<=0 || *pow>99) return coc7rText(loc,"need_stats");
        int san=(std::min)(*pow,coc7rAttr(msg,"最大理智").value_or(99));
        if (!coc7rSave(msg,*card,state,{{"理智",san}})) return coc7rText(loc,"conflict");
        return coc7rText(loc,"san_initialized",{{"san",std::to_string(san)}});
    }
    if (action=="endshock" || action=="sleep" || action=="chapter" || action=="recover") {
        if (param!="confirm") return coc7rText(loc,"state_confirm");
        if (action=="chapter") { state.chapterLoss=0; state.reassured=false; }
        if (action=="endshock" || action=="sleep") { state.shock=false; state.shockRounds=0; }
        if (action=="sleep") state.aftershock=false;
        if (action=="recover") state.changed=false; // Keeper-confirmed rest + backstory, not midnight.
        if (!coc7rSave(msg,*card,state)) return coc7rText(loc,"conflict");
        return coc7rStatus(loc,msg,state);
    }
    auto san=cards_.getAttr(msg.senderId,cardScope(msg),"理智");
    if (!san || *san<=0 || *san>99) return i18n_.tr(loc,"card.sc.invalid_san");
    if (action=="reward") {
        if (!coc7r::cost(param)) return coc7rText(loc,"recovery_usage");
        const int gain=coc7rRoll(param);
        const int next=(std::max)(*san,(std::min)(*san+gain,coc7rAttr(msg,"最大理智").value_or(99)));
        if (!coc7rSave(msg,*card,state,{{"理智",next}})) return coc7rText(loc,"conflict");
        return coc7rText(loc,"recovery",{{"gain",std::to_string(next-*san)},{"san",std::to_string(next)}});
    }
    if (action=="therapy") {
        std::istringstream s(param); std::string rating,mode,extra; s>>rating>>mode;
        auto skill=coc7r::number(rating,1000);
        if (!skill || (s>>extra) || (!mode.empty() && mode!="push" && mode!="phobia")) return coc7rText(loc,"recovery_usage");
        const int r=coc7rRoll("1d100"); const auto grade=rollSuccessLevel(r,*skill,0);
        auto lead=coc7rText(loc,"therapy_check",{{"roll",std::to_string(r)},{"skill",rating},{"level",i18n_.tr(loc,levelKey(grade))}});
        if (grade==SuccessLevel::kFumble || (grade==SuccessLevel::kFailure && mode=="push"))
            return coc7rApplyLoss(loc,msg,*card,*san,coc7rRoll("1d6"),false,lead+"\n"+coc7rText(loc,"therapy_end"));
        if (grade==SuccessLevel::kFailure) return lead;
        if (mode=="phobia") return lead+"\n"+coc7rText(loc,"therapy_phobia");
        const int next=(std::max)(*san,(std::min)(*san+coc7rRoll("1d3"),coc7rAttr(msg,"最大理智").value_or(99)));
        if (!coc7rSave(msg,*card,state,{{"理智",next}})) return coc7rText(loc,"conflict");
        return lead+"\n"+coc7rText(loc,"recovery",{{"gain",std::to_string(next-*san)},{"san",std::to_string(next)}});
    }
    if (action=="selfhelp") {
        // Settle an already rolled INT/POW result, allowing a legal Luck spend
        // chosen after the roll. It is not another random attempt.
        std::istringstream s(param); std::string trait,result,spend,extra; s>>trait>>result>>spend;
        const auto attribute=profileAttribute(msg,trait);
        auto roll=coc7r::number(result,100), luck=spend.empty()?std::optional<int>(0):coc7r::number(spend,99);
        auto value=coc7rAttr(msg,attribute);
        if ((attribute!="智力" && attribute!="意志") || !roll || *roll<1 || !luck || *luck>=*roll || !value || *value<0 || (s>>extra))
            return coc7rText(loc,"recovery_usage");
        const int currentLuck=cards_.getAttr(msg.senderId,cardScope(msg),"幸运").value_or(0);
        if (*luck>currentLuck || (*luck>0 && rollSuccessLevel(*roll,*value,0)==SuccessLevel::kFumble)) return coc7rText(loc,"luck_invalid");
        const int adjusted=*roll-*luck;
        if (!(adjusted==1 || (adjusted<100 && adjusted<=*value/2))) {
            auto pow=coc7rAttr(msg,"意志"); if (!pow || *pow<=0) return coc7rText(loc,"need_stats");
            auto outcome=coc7r::loseSanity(state,*san,1,*pow);
            std::map<std::string,int> attrs{{"理智",outcome.remaining}};
            if (*luck) attrs["幸运"]=currentLuck-*luck;
            if (!coc7rSave(msg,*card,outcome.state,attrs)) return coc7rText(loc,"conflict");
            return coc7rText(loc,"selfhelp_failed")+"\n"+coc7rText(loc,"loss_result",{{"loss",std::to_string(outcome.loss)},
                {"final",std::to_string(outcome.remaining)},{"total",std::to_string(outcome.state.chapterLoss)}})
                +(outcome.lost?"\n"+coc7rText(loc,"lost"):"")+(outcome.newChanged?"\n"+coc7rText(loc,"changed_help"):"");
        }
        const int next=(std::max)(*san,(std::min)(*san+coc7rRoll("1d6"),coc7rAttr(msg,"最大理智").value_or(99)));
        std::map<std::string,int> attrs{{"理智",next}}; if (*luck) attrs["幸运"]=currentLuck-*luck;
        if (!coc7rSave(msg,*card,state,attrs)) return coc7rText(loc,"conflict");
        return coc7rText(loc,"recovery",{{"gain",std::to_string(next-*san)},{"san",std::to_string(next)}});
    }
    if (action=="mythos" && (param=="first" || param=="later")) {
        if (!state.mythosPending) return coc7rText(loc,"no_mythos_pending");
        if (param=="first" && card->data.contains("__meta") && card->data["__meta"].is_object() &&
            card->data["__meta"].value("coc7rMythosKnown",json(false))==true) return coc7rText(loc,"mythos_already_known");
        auto skill=cards_.getAttr(msg.senderId,cardScope(msg),"克苏鲁神话").value_or(0);
        if (skill<0 || skill>99) return coc7rText(loc,"need_stats");
        const int gain=(std::min)(99-skill,param=="first"?5:1);
        --state.mythosPending;
        const int final=(std::min)(*san,99-skill-gain);
        if (!coc7rSave(msg,*card,state,{{"理智",final},{"克苏鲁神话",skill+gain}},true)) return coc7rText(loc,"conflict");
        return coc7rText(loc,"mythos",{{"gain",std::to_string(gain)},{"value",std::to_string(skill+gain)},{"san",std::to_string(final)}});
    }
    if (action=="reality") {
        if (!param.empty() && param!="assisted") return coc7rText(loc,"reality_usage");
        std::string detail; int r=param=="assisted"?rollBonusPenalty(1,true,loc,detail):coc7rRoll("1d100");
        auto grade=rollSuccessLevel(r,*san,0);
        std::string lead=coc7rText(loc,"reality_check",{{"roll",detail.empty()?std::to_string(r):detail},
            {"san",std::to_string(*san)},{"level",i18n_.tr(loc,levelKey(grade))}});
        if (grade==SuccessLevel::kFailure || grade==SuccessLevel::kFumble)
            return coc7rApplyLoss(loc,msg,*card,*san,1,false,lead+"\n"+coc7rText(loc,"involuntary"));
        state.realityProtected=true;
        if (!coc7rSave(msg,*card,state)) return coc7rText(loc,"conflict");
        return lead+"\n"+coc7rText(loc,"reality_success");
    }
    if (action=="loss") {
        auto [expr,source]=splitCommand(param);
        if (!coc7r::cost(expr) || (!source.empty() && source!="mythos")) return coc7rText(loc,"loss_usage");
        return coc7rApplyLoss(loc,msg,*card,*san,coc7rRoll(expr),source=="mythos",coc7rText(loc,"direct_loss"));
    }
    return coc7rText(loc,"help");
}

