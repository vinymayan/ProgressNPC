#include "Rule.h"
#include "RulePackageStore.h"
#include "SaveState.h"
#include <miniz.h> // Inclua a biblioteca miniz
#include <algorithm>
#include <cctype>
#include <chrono>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace fs = std::filesystem;
// Helper to split string
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// Novo Helper para formatar o FormID conforme a regra solicitada
std::string FormatLocalFormID(uint32_t a_formID, const std::string& a_pluginName) {
    std::string plugin = a_pluginName;
    std::transform(plugin.begin(), plugin.end(), plugin.begin(), ::tolower);

    auto dataHandler = RE::TESDataHandler::GetSingleton();
    auto file = dataHandler ? dataHandler->LookupModByName(plugin) : nullptr;

    char buf[10];
    // Plugins "Light" (ESL ou ESP com flag FE) usam os últimos 3 dígitos (12 bits)
    if (plugin == "dynamic" || plugin == "created") {
        sprintf_s(buf, "%08X", a_formID);
    }
    else if (file && file->IsLight()) {
        sprintf_s(buf, "%03X", a_formID & 0x00000FFF);
    }
    // Plugins "Full" (ESM e ESP comuns) usam os últimos 6 dígitos (24 bits)
    else {
        sprintf_s(buf, "%06X", a_formID & 0x00FFFFFF);
    }
    return std::string(buf);
}

namespace
{
    bool IEquals(const std::string& lhs, const std::string& rhs)
    {
        return lhs.size() == rhs.size() &&
            std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
                return std::tolower(a) == std::tolower(b);
            });
    }

    bool IsDynamicPluginAlias(const std::string& plugin)
    {
        return IEquals(plugin, "Dynamic") || IEquals(plugin, "Created");
    }

    const RE::TESFile* GetSourceFileByFormID(RE::TESForm* a_form)
    {
        if (!a_form) return nullptr;
        if (auto file = a_form->GetFile(0)) return file;

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;

        const auto formID = a_form->GetFormID();
        const auto modIndex = static_cast<std::uint8_t>(formID >> 24);
        if (modIndex == 0xFE) {
            const auto lightIndex = static_cast<std::uint16_t>((formID >> 12) & 0xFFF);
            return dataHandler->LookupLoadedLightModByIndex(lightIndex);
        }
        if (modIndex != 0xFF) {
            return dataHandler->LookupLoadedModByIndex(modIndex);
        }
        return nullptr;
    }

    RE::FormID ResolvePluginFormID(const std::string& formIDStr)
    {
        auto tokens = split(formIDStr, '|');
        if (tokens.size() < 2) return 0;

        try {
            auto localID = static_cast<RE::FormID>(std::stoul(tokens[1], nullptr, 16));
            if (IsDynamicPluginAlias(tokens[0])) {
                return localID;
            }

            auto dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler ? dataHandler->LookupFormID(localID, tokens[0]) : 0;
        } catch (...) {
            return 0;
        }
    }

    bool IsActorDependentFilterType(std::string_view type)
    {
        return type == "Keyword" ||
            type == "Faction" ||
            type == "Faction Rank" ||
            type == "Perk" ||
            type == "Spell" ||
            type == "Shout" ||
            type == "Inventory Item" ||
            type == "Inventory Count" ||
            type == "Gold" ||
            type == "Equipped Item" ||
            type == "Location" ||
            type == "Cell" ||
            type == "Worldspace" ||
            type == "Cell Type" ||
            type == "Location Keyword" ||
            type == "Quest" ||
            type == "Relationship Rank" ||
            type == "Equipped Category" ||
            type == "Actor Value";
    }

    bool IsNonFormFilterType(const std::string_view type)
    {
        return type == "Actor Value" ||
            type == "Source Plugin" ||
            type == "NPC Trait" ||
            type == "Relationship Rank" ||
            type == "Cell Type" ||
            type == "Equipped Category";
    }

    bool HasActorDependentFilters(const Rule& rule)
    {
        return rule.combatState != RuleCombatState::kAny ||
            rule.followerState != RuleFollowerState::kAny ||
            std::ranges::any_of(rule.targetFilters, [](const auto& filter) {
            return IsActorDependentFilterType(filter.type);
        }) || std::ranges::any_of(rule.blacklistFilters, [](const auto& filter) {
            return IsActorDependentFilterType(filter.type);
        });
    }

    RE::TESFaction* GetCurrentFollowerFaction()
    {
        static auto* faction = []() -> RE::TESFaction* {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler ?
                dataHandler->LookupForm<RE::TESFaction>(
                    0x0001CA7D, "Skyrim.esm") :
                nullptr;
        }();
        return faction;
    }

    RE::TESFaction* GetPotentialFollowerFaction()
    {
        static auto* faction = []() -> RE::TESFaction* {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler ?
                dataHandler->LookupForm<RE::TESFaction>(
                    0x0005C84D, "Skyrim.esm") :
                nullptr;
        }();
        return faction;
    }

    RE::BGSKeyword* GetActorTypeNPCKeyword()
    {
        static auto* keyword = []() -> RE::BGSKeyword* {
            auto* dataHandler = RE::TESDataHandler::GetSingleton();
            return dataHandler ?
                dataHandler->LookupForm<RE::BGSKeyword>(
                    0x00013794, "Skyrim.esm") :
                nullptr;
        }();
        return keyword;
    }

    bool IsFollowerIdentityFaction(const RE::FormID a_formID)
    {
        const auto* current = GetCurrentFollowerFaction();
        const auto* potential = GetPotentialFollowerFaction();
        return (current && current->GetFormID() == a_formID) ||
            (potential && potential->GetFormID() == a_formID);
    }
}

RE::TESForm* ResolveEDFForm(const std::string& a_type, const std::string& a_editorID, const std::string& a_formIDStr)
{
    if (!a_editorID.empty()) {
        if (const auto formID =
                Manager::GetSingleton()->FindFormIDByEditorID(
                    a_type, a_editorID)) {
            if (auto form = RE::TESForm::LookupByID(*formID)) {
                return form;
            }
        }
    }

    if (auto formID = ResolvePluginFormID(a_formIDStr)) {
        if (auto form = RE::TESForm::LookupByID(formID)) {
            return form;
        }
    }

    auto tokens = split(a_formIDStr, '|');
    if (tokens.size() >= 2 && IsDynamicPluginAlias(tokens[0])) {
        try {
            const auto legacyLocalID = static_cast<RE::FormID>(std::stoul(tokens[1], nullptr, 16));
            if (legacyLocalID <= 0x00FFFFFF) {
                const auto& list = Manager::GetSingleton()->GetList(a_type);
                auto it = std::find_if(list.begin(), list.end(), [&](const InternalFormInfo& info) {
                    return (info.formID & 0x00FFFFFF) == legacyLocalID;
                });
                if (it != list.end()) {
                    return RE::TESForm::LookupByID(it->formID);
                }
            }
        } catch (...) {
            return nullptr;
        }
    }

    return nullptr;
}

RE::FormID ResolveEDFFormID(const std::string& a_type, const std::string& a_editorID, const std::string& a_formIDStr)
{
    if (auto form = ResolveEDFForm(a_type, a_editorID, a_formIDStr)) {
        return form->GetFormID();
    }
    return 0;
}

bool IsEquipmentRewardType(const std::string_view type)
{
    return type == "Outfit" || type == "Armor" ||
        type == "Weapon" || type == "Ammo";
}

RE::ActorValue ResolveActorValue(const std::string_view a_name)
{
    if (a_name.empty()) {
        return RE::ActorValue::kNone;
    }

    const std::string name(a_name);
    if (const auto resolved =
            RE::ActorValueList::LookupActorValueByName(name.c_str());
        resolved != RE::ActorValue::kNone) {
        return resolved;
    }

    for (auto index = 0;
         index < std::to_underlying(RE::ActorValue::kTotal);
         ++index) {
        const auto actorValue = static_cast<RE::ActorValue>(index);
        const auto actorValueName =
            RE::ActorValueList::GetActorValueName(actorValue);
        if (actorValueName && IEquals(name, actorValueName)) {
            return actorValue;
        }
    }
    return RE::ActorValue::kNone;
}

bool IsMaximumActorValueSupported(const RE::ActorValue a_actorValue)
{
    return a_actorValue == RE::ActorValue::kHealth ||
        a_actorValue == RE::ActorValue::kMagicka ||
        a_actorValue == RE::ActorValue::kStamina;
}

bool IsActorValueFilterValid(const BlacklistFilter& a_filter)
{
    if (a_filter.type != "Actor Value") {
        return true;
    }
    const auto actorValue =
        ResolveActorValue(a_filter.actorValueName);
    return actorValue != RE::ActorValue::kNone &&
        (a_filter.actorValueMode != ActorValueMode::kMaximum ||
            IsMaximumActorValueSupported(actorValue));
}

bool IsActivePlayerFollower(RE::Actor* actor)
{
    if (!actor || actor->IsPlayerRef() || actor->IsDisabled() ||
        actor->IsSummoned()) {
        return false;
    }

    const auto lifeState = actor->GetLifeState();
    if (lifeState == RE::ACTOR_LIFE_STATE::kDying ||
        lifeState == RE::ACTOR_LIFE_STATE::kDead) {
        return false;
    }

    if (auto* currentFollowerFaction = GetCurrentFollowerFaction();
        currentFollowerFaction &&
        actor->IsInFaction(currentFollowerFaction)) {
        return true;
    }

    if (!actor->IsPlayerTeammate()) {
        return false;
    }

    if (auto* potentialFollowerFaction =
            GetPotentialFollowerFaction();
        potentialFollowerFaction &&
        actor->IsInFaction(potentialFollowerFaction)) {
        return true;
    }

    auto* race = actor->GetRace();
    auto* actorTypeNPC = GetActorTypeNPCKeyword();
    return race && actorTypeNPC && race->HasKeyword(actorTypeNPC);
}

RuleDependencyMask GetFilterDependencyMask(std::string_view a_type)
{
    if (a_type == "Actor Value") {
        return ToMask(RuleDependency::kActorValue);
    }
    if (a_type == "Keyword" || a_type == "Faction") {
        return ToMask(RuleDependency::kTag);
    }
    if (a_type == "Faction Rank") {
        return ToMask(RuleDependency::kFactionRank);
    }
    if (a_type == "Perk" || a_type == "Spell" || a_type == "Shout") {
        return ToMask(RuleDependency::kAbility);
    }
    if (a_type == "Inventory Item" || a_type == "Inventory Count" ||
        a_type == "Gold" || a_type == "Equipped Item") {
        return ToMask(RuleDependency::kInventory);
    }
    if (a_type == "Location" || a_type == "Cell" ||
        a_type == "Worldspace" || a_type == "Cell Type" ||
        a_type == "Location Keyword") {
        return ToMask(RuleDependency::kEnvironment);
    }
    if (a_type == "Quest" || a_type == "NPC Trait") {
        return a_type == "NPC Trait" ?
            ToMask(RuleDependency::kStatic) |
                ToMask(RuleDependency::kQuest) :
            ToMask(RuleDependency::kQuest);
    }
    if (a_type == "Relationship Rank") {
        return ToMask(RuleDependency::kRelationship);
    }
    if (a_type == "Equipped Category") {
        return ToMask(RuleDependency::kEquipment);
    }
    return ToMask(RuleDependency::kStatic);
}

RuleDependencyMask GetRewardDependencyMask(std::string_view a_type)
{
    if (a_type == "Keyword") {
        return ToMask(RuleDependency::kTag);
    }
    if (a_type == "Faction") {
        return ToMask(RuleDependency::kTag) |
            ToMask(RuleDependency::kFactionRank) |
            ToMask(RuleDependency::kFollower);
    }
    if (a_type == "Perk" || a_type == "Spell" || a_type == "Shout") {
        return ToMask(RuleDependency::kAbility);
    }
    if (a_type == "Outfit") {
        return ToMask(RuleDependency::kInventory) |
            ToMask(RuleDependency::kSleep) |
            ToMask(RuleDependency::kEquipment);
    }
    if (a_type == "Weapon" || a_type == "Armor" || a_type == "Ammo" ||
        a_type == "Potion" || a_type == "Ingredient" || a_type == "Scroll" ||
        a_type == "Book" || a_type == "Misc" || a_type == "SoulGem" ||
        a_type == "Key") {
        return ToMask(RuleDependency::kInventory) |
            ((a_type == "Weapon" || a_type == "Armor" ||
                 a_type == "Ammo") ?
                ToMask(RuleDependency::kEquipment) :
                ToMask(RuleDependency::kNone));
    }
    return ToMask(RuleDependency::kNone);
}

std::pair<std::string, RE::FormID> Reward::ParseFormID() const {
    auto tokens = split(formIDStr, '|');
    const auto plugin = tokens.empty() ? "" : tokens[0];
    return { plugin, ResolveEDFFormID(typeReward, editorID, formIDStr) };
}

std::string SanitizeFilename(std::string name) {
    if (name.empty()) return "Unnamed_Rule";

    // Caracteres proibidos em sistemas de arquivos
    std::string illegalChars = "<>:\"/\\|?*";
    for (char& c : name) {
        if (illegalChars.find(c) != std::string::npos) {
            c = '_'; // Substitui por underscore
        }
    }
    // Remove espaços no fim ou pontos que podem causar problemas
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name;
}

namespace {
    using JsonAllocator = rapidjson::Document::AllocatorType;

    const rapidjson::Value* FindMember(const rapidjson::Value& obj, const char* key) {
        if (!obj.IsObject()) return nullptr;
        auto it = obj.FindMember(key);
        return it != obj.MemberEnd() ? &it->value : nullptr;
    }

    std::string GetString(const rapidjson::Value& obj, const char* key, const std::string& fallback = {}) {
        auto value = FindMember(obj, key);
        return value && value->IsString() ? value->GetString() : fallback;
    }

    bool GetBool(const rapidjson::Value& obj, const char* key, bool fallback = false) {
        auto value = FindMember(obj, key);
        return value && value->IsBool() ? value->GetBool() : fallback;
    }

    int GetInt(const rapidjson::Value& obj, const char* key, int fallback = 0) {
        auto value = FindMember(obj, key);
        return value && value->IsInt() ? value->GetInt() : fallback;
    }

    uint32_t GetUint(const rapidjson::Value& obj, const char* key, uint32_t fallback = 0) {
        auto value = FindMember(obj, key);
        return value && value->IsUint() ? value->GetUint() : fallback;
    }

    float GetFloat(const rapidjson::Value& obj, const char* key, float fallback = 0.0f) {
        auto value = FindMember(obj, key);
        return value && value->IsNumber() ? value->GetFloat() : fallback;
    }

    void AddString(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, const std::string& value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        rapidjson::Value jsonValue;
        jsonValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
        obj.AddMember(jsonKey, jsonValue, alloc);
    }

    void AddBool(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, bool value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    void AddInt(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, int value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    void AddUint(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, uint32_t value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    void AddFloat(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, float value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    rapidjson::Value WriteBlacklistFilter(const BlacklistFilter& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddString(obj, alloc, "type", p.type);
        AddString(obj, alloc, "formID", p.formIDStr);
        AddString(obj, alloc, "editorID", p.editorID);
        AddString(obj, alloc, "actorValueName", p.actorValueName);
        AddInt(obj, alloc, "optionMode", p.optionMode);
        AddInt(obj, alloc, "optionValue", p.optionValue);
        AddString(obj, alloc, "optionText", p.optionText);
        AddInt(obj, alloc, "actorValueMode", static_cast<int>(p.actorValueMode));
        AddInt(obj, alloc, "comparison", static_cast<int>(p.comparison));
        AddFloat(obj, alloc, "minimumValue", p.minimumValue);
        AddFloat(obj, alloc, "maximumValue", p.maximumValue);
        return obj;
    }

    BlacklistFilter ReadBlacklistFilter(const rapidjson::Value& value) {
        BlacklistFilter p;
        p.type = GetString(value, "type");
        p.formIDStr = GetString(value, "formID");
        p.editorID = GetString(value, "editorID");
        p.actorValueName = GetString(value, "actorValueName");
        p.optionMode = GetInt(value, "optionMode", 0);
        p.optionValue = GetInt(value, "optionValue", 0);
        p.optionText = GetString(value, "optionText");
        p.actorValueMode = static_cast<ActorValueMode>(std::clamp(
            GetInt(value, "actorValueMode", 0), 0, 2));
        p.comparison = static_cast<NumericComparison>(std::clamp(
            GetInt(value, "comparison", 0), 0, 3));
        p.minimumValue = GetFloat(value, "minimumValue", 0.0f);
        p.maximumValue = GetFloat(value, "maximumValue", 0.0f);
        return p;
    }

    rapidjson::Value WriteFilterArray(const std::vector<BlacklistFilter>& filters, JsonAllocator& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& filter : filters) {
            array.PushBack(WriteBlacklistFilter(filter, alloc).Move(), alloc);
        }
        return array;
    }

    std::vector<BlacklistFilter> ReadFilterArray(const rapidjson::Value* value) {
        std::vector<BlacklistFilter> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsObject()) result.push_back(ReadBlacklistFilter(item));
        }
        return result;
    }

    rapidjson::Value WriteReward(const Reward& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddString(obj, alloc, "typeReward", p.typeReward);
        AddString(obj, alloc, "FormID", p.formIDStr);
        AddString(obj, alloc, "editorID", p.editorID);
        AddUint(obj, alloc, "Amount", p.amount);
        AddFloat(obj, alloc, "Chance", p.chanceReward);
        AddInt(obj, alloc, "functionOnType", p.functionOnType);
        AddInt(obj, alloc, "equipContexts", p.equipContexts);
        AddBool(obj, alloc, "isPersistent", p.isPersistent);
        return obj;
    }

    Reward ReadReward(const rapidjson::Value& value) {
        Reward p;
        p.typeReward = GetString(value, "typeReward");
        p.formIDStr = GetString(value, "FormID");
        p.editorID = GetString(value, "editorID");
        p.amount = GetUint(value, "Amount", 1);
        p.chanceReward = GetFloat(value, "Chance", 100.0f);
        p.functionOnType = GetInt(value, "functionOnType", GetInt(value, "isSleepOutfit", 0));
        if (const auto contexts = FindMember(value, "equipContexts");
            contexts && contexts->IsInt()) {
            p.equipContexts = static_cast<EquipmentContextMask>(
                std::clamp(
                    contexts->GetInt(), 1,
                    static_cast<int>(kAllEquipmentContexts)));
        }
        else if (p.typeReward == "Outfit") {
            p.equipContexts =
                p.functionOnType == 1 ?
                    ToMask(EquipmentContext::kSleep) :
                p.functionOnType == 2 ?
                    ToMask(EquipmentContext::kNormal) |
                        ToMask(EquipmentContext::kSleep) :
                    ToMask(EquipmentContext::kNormal);
        }
        p.isPersistent = GetBool(value, "isPersistent", false);
        return p;
    }

    rapidjson::Value WriteRewardArray(const std::vector<Reward>& rewards, JsonAllocator& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& reward : rewards) {
            array.PushBack(WriteReward(reward, alloc).Move(), alloc);
        }
        return array;
    }

    std::vector<Reward> ReadRewardArray(const rapidjson::Value* value) {
        std::vector<Reward> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsObject()) result.push_back(ReadReward(item));
        }
        return result;
    }

    rapidjson::Value WriteRewardGroup(const RewardGroup& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddString(obj, alloc, "name", p.name);
        AddBool(obj, alloc, "exclusive", p.isExclusive);
        AddFloat(obj, alloc, "chanceGroup", p.chanceGroup);
        auto rewards = WriteRewardArray(p.rewards, alloc);
        obj.AddMember("rewards", rewards, alloc);
        return obj;
    }

    RewardGroup ReadRewardGroup(const rapidjson::Value& value) {
        RewardGroup p;
        p.name = GetString(value, "name", "New Group");
        p.isExclusive = GetBool(value, "exclusive", false);
        p.chanceGroup = GetFloat(value, "chanceGroup", 100.0f);
        p.rewards = ReadRewardArray(FindMember(value, "rewards"));
        return p;
    }

    rapidjson::Value WriteRewardGroupArray(const std::vector<RewardGroup>& groups, JsonAllocator& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& group : groups) {
            array.PushBack(WriteRewardGroup(group, alloc).Move(), alloc);
        }
        return array;
    }

    std::vector<RewardGroup> ReadRewardGroupArray(const rapidjson::Value* value) {
        std::vector<RewardGroup> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsObject()) result.push_back(ReadRewardGroup(item));
        }
        return result;
    }

    rapidjson::Value WriteCompactRule(const Rule& p, bool isLatest, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        if (isLatest) {
            AddString(obj, alloc, "id", p.id);
            AddString(obj, alloc, "n", p.name);
            AddBool(obj, alloc, "en", p.isEnabled);
        }
        AddInt(obj, alloc, "v", p.version);
        AddInt(obj, alloc, "l", p.level);
        AddInt(obj, alloc, "g", p.targetGender);
        AddInt(obj, alloc, "h", p.targetHumanoid);
        AddInt(obj, alloc, "c", p.targetChild);
        AddInt(obj, alloc, "cb", static_cast<int>(p.combatState));
        AddInt(obj, alloc, "fs", static_cast<int>(p.followerState));
        AddBool(obj, alloc, "ra", p.targetRequiresAll);
        AddBool(obj, alloc, "ex", p.isExclusive);
        obj.AddMember("tf", WriteFilterArray(p.targetFilters, alloc), alloc);
        obj.AddMember("rg", WriteRewardGroupArray(p.rewardGroups, alloc), alloc);
        AddInt(obj, alloc, "bg", p.blacklistedGender);
        AddInt(obj, alloc, "bh", p.blacklistedHumanoid);
        AddInt(obj, alloc, "bc", p.blacklistedChild);
        AddBool(obj, alloc, "bra", p.blacklistRequiresAll);
        obj.AddMember("bf", WriteFilterArray(p.blacklistFilters, alloc), alloc);
        return obj;
    }

    rapidjson::Value WriteHashRule(const Rule& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddBool(obj, alloc, "enabled", p.isEnabled);
        AddString(obj, alloc, "name", p.name);
        AddInt(obj, alloc, "level", p.level);
        AddInt(obj, alloc, "t_gender", p.targetGender);
        AddInt(obj, alloc, "t_humanoid", p.targetHumanoid);
        AddInt(obj, alloc, "t_child", p.targetChild);
        AddInt(obj, alloc, "combat_state", static_cast<int>(p.combatState));
        AddInt(obj, alloc, "follower_state", static_cast<int>(p.followerState));
        AddBool(obj, alloc, "t_reqAll", p.targetRequiresAll);
        obj.AddMember("t_filters", WriteFilterArray(p.targetFilters, alloc), alloc);
        obj.AddMember("groups", WriteRewardGroupArray(p.rewardGroups, alloc), alloc);
        AddInt(obj, alloc, "b_gender", p.blacklistedGender);
        AddInt(obj, alloc, "b_humanoid", p.blacklistedHumanoid);
        AddInt(obj, alloc, "b_child", p.blacklistedChild);
        AddBool(obj, alloc, "b_reqAll", p.blacklistRequiresAll);
        obj.AddMember("b_filters", WriteFilterArray(p.blacklistFilters, alloc), alloc);
        AddBool(obj, alloc, "isExclusive", p.isExclusive);
        for (const auto& group : p.rewardGroups) {
            const auto key = "g_chance_" + group.name;
            AddFloat(obj, alloc, key.c_str(), group.chanceGroup);
        }
        return obj;
    }

    std::string SerializeJson(const rapidjson::Value& value) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }

}

std::string Rule::CalculateHash() const {
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    auto hashValue = WriteHashRule(*this, alloc);
    return std::to_string(std::hash<std::string>{}(SerializeJson(hashValue)));
}

Rule ProcessRuleVersion(const rapidjson::Value& j, const std::string& fallbackId, const std::string& fallbackName, bool fallbackEnabled) {
    Rule p;
    p.id = GetString(j, "id", fallbackId);
    p.name = GetString(j, "n", GetString(j, "name", fallbackName));
    p.isEnabled = GetBool(j, "en", GetBool(j, "enabled", fallbackEnabled));

    p.version = GetInt(j, "v", GetInt(j, "version", 1));
    p.level = GetInt(j, "l", GetInt(j, "level", 1));
    p.targetGender = GetInt(j, "g", GetInt(j, "targetGender", 0));
    p.targetHumanoid = GetInt(j, "h", GetInt(j, "t_humanoid", GetInt(j, "targetHumanoid", 0)));
    p.targetChild = GetInt(j, "c", GetInt(j, "t_child", GetInt(j, "targetChild", 0)));
    p.combatState = static_cast<RuleCombatState>(std::clamp(
        GetInt(
            j,
            "cb",
            GetInt(j, "combatState", GetInt(j, "combat_state", 0))),
        0,
        2));
    p.followerState = static_cast<RuleFollowerState>(std::clamp(
        GetInt(
            j,
            "fs",
            GetInt(j, "followerState", GetInt(j, "follower_state", 0))),
        0,
        2));
    p.targetRequiresAll = GetBool(j, "ra", GetBool(j, "targetRequiresAll", false));
    p.isExclusive = GetBool(j, "ex", GetBool(j, "ruleExclusive", false));

    if (auto value = FindMember(j, "tf")) p.targetFilters = ReadFilterArray(value);
    else p.targetFilters = ReadFilterArray(FindMember(j, "targetFilters"));

    if (auto value = FindMember(j, "rg")) p.rewardGroups = ReadRewardGroupArray(value);
    else p.rewardGroups = ReadRewardGroupArray(FindMember(j, "RewardGroups"));

    p.blacklistedGender = GetInt(j, "bg", GetInt(j, "blacklistedGender", 0));
    p.blacklistedHumanoid = GetInt(j, "bh", GetInt(j, "b_humanoid", GetInt(j, "blacklistedHumanoid", 0)));
    p.blacklistedChild = GetInt(j, "bc", GetInt(j, "b_child", GetInt(j, "blacklistedChild", 0)));
    p.blacklistRequiresAll = GetBool(j, "bra", GetBool(j, "blacklistRequiresAll", false));

    if (auto value = FindMember(j, "bf")) p.blacklistFilters = ReadFilterArray(value);
    else p.blacklistFilters = ReadFilterArray(FindMember(j, "blacklistFilters"));

    p.lastSavedHash = p.CalculateHash();
    return p;
}

bool ParseLegacyRuleFile(
    const std::filesystem::path& path,
    Rule& latest,
    std::vector<Rule>& history,
    std::string& error)
{
    history.clear();
    std::ifstream input(path);
    if (!input.is_open()) {
        error = "could not open file";
        return false;
    }

    rapidjson::IStreamWrapper stream(input);
    rapidjson::Document document;
    document.ParseStream(stream);
    if (document.HasParseError()) {
        error = std::format("invalid JSON at byte {}", document.GetErrorOffset());
        return false;
    }
    if (!document.IsArray() || document.Empty()) {
        error = "the root must be a non-empty version array";
        return false;
    }

    latest = ProcessRuleVersion(document[0], "", "Sem Nome", true);
    if (latest.id.empty()) {
        error = "latest version has no rule ID";
        return false;
    }

    std::set<int> versions;
    history.reserve(document.Size());
    for (rapidjson::SizeType index = 0; index < document.Size(); ++index) {
        if (!document[index].IsObject()) {
            error = std::format("version entry {} is not an object", index);
            return false;
        }
        auto version = index == 0
            ? latest
            : ProcessRuleVersion(document[index], latest.id, latest.name, latest.isEnabled);
        if (version.id != latest.id) {
            error = std::format("version entry {} changes the rule ID", index);
            return false;
        }
        if (!versions.insert(version.version).second) {
            error = std::format("duplicate version {}", version.version);
            return false;
        }
        history.push_back(std::move(version));
    }
    return true;
}

bool RuleManager::IsAffected(RE::Actor* actor) {
    if (!actor) return false;
    auto baseNPC = actor->GetActorBase();
	//logger::debug("Verificando NPC: {} (FormID: {:08X})", baseNPC ? baseNPC->GetName() : "Unknown", baseNPC->GetFormID());
    if (!baseNPC) return false;

    const auto candidates = GetCandidateRuleIDs(
        actor, RuleEvaluationDelta::Full());
    return std::ranges::any_of(candidates, [&](const std::string& a_ruleID) {
        const auto* rule = FindRule(a_ruleID);
        return rule && rule->isEnabled;
    });
}

bool IsNPCInLeveledList(RE::TESNPC* a_npc, RE::TESLevCharacter* a_levList) {
    if (!a_npc || !a_levList) return false;

    for (auto& entry : a_levList->entries) {
        auto form = entry.form;
        if (!form) continue;

        // Se a entrada for o próprio NPC, encontramos o match
        if (form->Is(RE::FormType::NPC)) {
            if (form->GetFormID() == a_npc->GetFormID()) return true;
        }
        // Se a entrada for outra Leveled List, entra nela recursivamente
        else if (form->Is(RE::FormType::LeveledNPC)) {
            if (IsNPCInLeveledList(a_npc, form->As<RE::TESLevCharacter>())) return true;
        }
    }
    return false;
}

bool MatchesTriStateFilter(int filter, bool value)
{
    if (filter == 0) return true;
    if (filter == 1) return value;
    if (filter == 2) return !value;
    return true;
}

bool ResolveIsChild(RE::TESNPC* npc, RE::Actor* actor)
{
    if (actor) return actor->IsChild();
    return npc && npc->race && npc->race->IsChildRace();
}

bool ResolveIsHumanoid(RE::TESNPC* npc, RE::Actor* actor)
{
    if (actor) return actor->IsHumanoid();
    if (npc) {
        auto dobj = RE::BGSDefaultObjectManager::GetSingleton();
        if (dobj) {
            constexpr auto keywordNPCIndex = static_cast<std::size_t>(RE::DEFAULT_OBJECTS::kKeywordNPC);
            auto keyword = dobj->objects[keywordNPCIndex] ? dobj->objects[keywordNPCIndex]->As<RE::BGSKeyword>() : nullptr;
            if (keyword) {
                return npc->HasKeyword(keyword);
            }
        }
    }
    return true;
}

int GetFilterValue(const std::vector<std::string>& tokens, int fallback)
{
    if (tokens.size() < 3) return fallback;
    try {
        return std::stoi(tokens[2]);
    } catch (...) {
        return fallback;
    }
}

std::optional<float> ReadActorValue(
    RE::Actor* a_actor,
    const RE::ActorValue a_actorValue,
    const ActorValueMode a_mode)
{
    if (!a_actor || a_actorValue == RE::ActorValue::kNone) {
        return std::nullopt;
    }
    auto owner = a_actor->AsActorValueOwner();
    if (!owner) {
        return std::nullopt;
    }

    switch (a_mode) {
    case ActorValueMode::kCurrent:
        return owner->GetActorValue(a_actorValue);
    case ActorValueMode::kPermanent:
        return owner->GetPermanentActorValue(a_actorValue);
    case ActorValueMode::kMaximum:
        if (IsMaximumActorValueSupported(a_actorValue)) {
            return a_actor->GetActorValueMax(a_actorValue);
        }
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

bool MatchesActorValueFilter(
    RE::Actor* a_actor,
    const BlacklistFilter& a_filter)
{
    const auto actorValue =
        ResolveActorValue(a_filter.actorValueName);
    const auto value =
        ReadActorValue(a_actor, actorValue, a_filter.actorValueMode);
    if (!value || !std::isfinite(*value)) {
        return false;
    }

    const auto epsilon = std::max(
        0.001f,
        std::abs(a_filter.minimumValue) * 0.00001f);
    switch (a_filter.comparison) {
    case NumericComparison::kGreaterOrEqual:
        return *value + epsilon >= a_filter.minimumValue;
    case NumericComparison::kLessOrEqual:
        return *value - epsilon <= a_filter.minimumValue;
    case NumericComparison::kEqual:
        return std::abs(*value - a_filter.minimumValue) <= epsilon;
    case NumericComparison::kBetween: {
        const auto [minimum, maximum] = std::minmax(
            a_filter.minimumValue,
            a_filter.maximumValue);
        return *value + epsilon >= minimum &&
            *value - epsilon <= maximum;
    }
    default:
        return false;
    }
}

bool IsActorEquippedItem(RE::Actor* actor, RE::TESBoundObject* item)
{
    if (!actor || !item) return false;

    if (actor->GetEquippedObject(false) == item || actor->GetEquippedObject(true) == item) {
        return true;
    }

    auto inventory = actor->GetInventory();
    auto it = inventory.find(item);
    return it != inventory.end() && it->second.second && it->second.second->IsWorn();
}

bool MatchesNumericComparison(
    const float value,
    const BlacklistFilter& filter)
{
    switch (filter.comparison) {
    case NumericComparison::kGreaterOrEqual:
        return value >= filter.minimumValue;
    case NumericComparison::kLessOrEqual:
        return value <= filter.minimumValue;
    case NumericComparison::kEqual:
        return value == filter.minimumValue;
    case NumericComparison::kBetween: {
        const auto [minimum, maximum] = std::minmax(
            filter.minimumValue, filter.maximumValue);
        return value >= minimum && value <= maximum;
    }
    default:
        return false;
    }
}

bool MatchesQuestAlias(
    RE::Actor* actor,
    RE::TESQuest* quest,
    const std::optional<std::uint32_t> aliasID)
{
    if (!actor || !quest) {
        return false;
    }
    const auto matchesAlias = [&](const std::uint32_t id) {
        auto handle = quest->GetAliasedRef(id);
        const auto reference = handle.get();
        return reference && reference.get() == actor;
    };
    if (aliasID) {
        return matchesAlias(*aliasID);
    }
    return std::ranges::any_of(
        quest->aliases,
        [&](const RE::BGSBaseAlias* alias) {
            return alias && matchesAlias(alias->aliasID);
        });
}

bool MatchesQuestFilter(
    RE::Actor* actor,
    RE::TESQuest* quest,
    const BlacklistFilter& filter)
{
    if (!quest) {
        return false;
    }
    switch (static_cast<QuestFilterMode>(filter.optionMode)) {
    case QuestFilterMode::kRunning:
        return quest->IsRunning();
    case QuestFilterMode::kCompleted:
        return quest->IsCompleted();
    case QuestFilterMode::kStopped:
        return quest->IsStopped();
    case QuestFilterMode::kNotStarted:
        return !quest->alreadyRun && !quest->IsRunning() &&
            !quest->IsCompleted();
    case QuestFilterMode::kStage:
        return quest->GetCurrentStageID() ==
            static_cast<std::uint16_t>(
                std::clamp(filter.optionValue, 0, 0xFFFF));
    case QuestFilterMode::kSpecificAlias:
        return MatchesQuestAlias(
            actor, quest,
            static_cast<std::uint32_t>(
                std::max(0, filter.optionValue)));
    case QuestFilterMode::kAnyAlias:
        return MatchesQuestAlias(actor, quest, std::nullopt);
    default:
        return false;
    }
}

bool MatchesEquippedCategory(
    RE::Actor* actor,
    const EquippedCategoryFilter category,
    const RE::TESObjectREFR::InventoryItemMap* inventorySnapshot = nullptr)
{
    if (!actor) {
        return false;
    }
    const auto left = actor->GetEquippedObject(true);
    const auto right = actor->GetEquippedObject(false);
    const auto leftWeapon = left ? left->As<RE::TESObjectWEAP>() : nullptr;
    const auto rightWeapon = right ? right->As<RE::TESObjectWEAP>() : nullptr;
    const auto matchesWeapon = [category](const RE::TESObjectWEAP* weapon) {
        if (!weapon) return false;
        const auto type = weapon->GetWeaponType();
        switch (category) {
        case EquippedCategoryFilter::kAnyWeapon:
            return true;
        case EquippedCategoryFilter::kOneHanded:
            return type >= RE::WEAPON_TYPE::kOneHandSword &&
                type <= RE::WEAPON_TYPE::kOneHandMace;
        case EquippedCategoryFilter::kTwoHanded:
            return type == RE::WEAPON_TYPE::kTwoHandSword ||
                type == RE::WEAPON_TYPE::kTwoHandAxe;
        case EquippedCategoryFilter::kBow:
            return weapon->IsBow();
        case EquippedCategoryFilter::kCrossbow:
            return weapon->IsCrossbow();
        case EquippedCategoryFilter::kStaff:
            return weapon->IsStaff();
        default:
            return false;
        }
    };
    if (category == EquippedCategoryFilter::kUnarmed) {
        return !leftWeapon && !rightWeapon;
    }
    if (matchesWeapon(leftWeapon) || matchesWeapon(rightWeapon)) {
        return true;
    }

    if (category != EquippedCategoryFilter::kShield &&
        category != EquippedCategoryFilter::kHeavyArmor &&
        category != EquippedCategoryFilter::kLightArmor &&
        category != EquippedCategoryFilter::kClothing) {
        return false;
    }
    auto ownedInventory =
        RE::TESObjectREFR::InventoryItemMap{};
    if (!inventorySnapshot) {
        ownedInventory = actor->GetInventory();
        inventorySnapshot = std::addressof(ownedInventory);
    }
    return std::ranges::any_of(
        *inventorySnapshot,
        [category](const auto& entry) {
            const auto* armor =
                entry.first ? entry.first->As<RE::TESObjectARMO>() : nullptr;
            const auto& data = entry.second.second;
            if (!armor || !data || !data->IsWorn()) {
                return false;
            }
            switch (category) {
            case EquippedCategoryFilter::kShield:
                return armor->IsShield();
            case EquippedCategoryFilter::kHeavyArmor:
                return armor->IsHeavyArmor();
            case EquippedCategoryFilter::kLightArmor:
                return armor->IsLightArmor();
            case EquippedCategoryFilter::kClothing:
                return armor->IsClothing();
            default:
                return false;
            }
        });
}

bool IsNPCMatchingTargets(RE::TESNPC* npc, const Rule& rule, bool isBlacklist, RE::Actor* actor) {
    // 1. Seleciona os dados baseados no modo (Target vs Blacklist)
    int genderFilter = isBlacklist ? rule.blacklistedGender : rule.targetGender;
    int humanoidFilter = isBlacklist ? rule.blacklistedHumanoid : rule.targetHumanoid;
    int childFilter = isBlacklist ? rule.blacklistedChild : rule.targetChild;
    const auto& filters = isBlacklist ? rule.blacklistFilters : rule.targetFilters;
    bool requiresAll = isBlacklist ? rule.blacklistRequiresAll : rule.targetRequiresAll;

    // 2. Verificação de Gênero (0: None, 1: Male, 2: Female)
    if (genderFilter != 0) {
        bool isFemale = npc->IsFemale();
        bool genderMatch = (genderFilter == 1 && !isFemale) || (genderFilter == 2 && isFemale);

        if (isBlacklist && genderMatch) return true;  // Se for blacklist e deu match no gênero, bloqueia
        if (!isBlacklist && !genderMatch) return false; // Se for target e NÃO deu match, descarta
    }

    // 3. Filtros de corpo/idade
    if (humanoidFilter != 0) {
        const bool humanoidMatch = MatchesTriStateFilter(humanoidFilter, ResolveIsHumanoid(npc, actor));
        if (isBlacklist && humanoidMatch) return true;
        if (!isBlacklist && !humanoidMatch) return false;
    }

    if (childFilter != 0) {
        const bool childMatch = MatchesTriStateFilter(childFilter, ResolveIsChild(npc, actor));
        if (isBlacklist && childMatch) return true;
        if (!isBlacklist && !childMatch) return false;
    }

    if (!isBlacklist && rule.combatState != RuleCombatState::kAny) {
        if (!actor) {
            return false;
        }
        const bool inCombat = IsActorInCombatContext(actor);
        if ((rule.combatState == RuleCombatState::kInCombat && !inCombat) ||
            (rule.combatState == RuleCombatState::kOutOfCombat && inCombat)) {
            return false;
        }
    }

    if (!isBlacklist &&
        rule.followerState != RuleFollowerState::kAny) {
        if (!actor) {
            return false;
        }
        const bool isActiveFollower =
            IsActivePlayerFollower(actor);
        if ((rule.followerState == RuleFollowerState::kActiveOnly &&
                !isActiveFollower) ||
            (rule.followerState == RuleFollowerState::kExcludeActive &&
                isActiveFollower)) {
            return false;
        }
    }

    if (filters.empty()) {
        // Na Blacklist, vazio significa "não bloqueia ninguém". No Target, significa "afeta todos".
        return !isBlacklist;
    }

    int matches = 0;
    std::optional<RE::TESObjectREFR::InventoryItemMap>
        equippedInventorySnapshot;
    for (const auto& filter : filters) {
        bool match = false;
        if (filter.type == "Actor Value") {
            match = MatchesActorValueFilter(actor, filter);
            if (match) {
                ++matches;
                if (!requiresAll) {
                    return true;
                }
            }
            continue;
        }

        if (filter.type == "Source Plugin") {
            const auto* source = GetSourceFileByFormID(npc);
            const auto sourceName = source ?
                std::string(source->GetFilename()) :
                std::string("Dynamic");
            const auto& expectedSource =
                filter.optionText.empty() ?
                    filter.editorID :
                    filter.optionText;
            match = IEquals(sourceName, expectedSource) ||
                (!source && IEquals(expectedSource, "Created"));
        }
        else if (filter.type == "NPC Trait") {
            switch (static_cast<NPCTraitFilter>(filter.optionMode)) {
            case NPCTraitFilter::kUnique:
                match = npc->IsUnique();
                break;
            case NPCTraitFilter::kEssential:
                match = actor ? actor->IsEssential() :
                    npc->IsEssential();
                break;
            case NPCTraitFilter::kProtected:
                match = actor ? actor->IsProtected() :
                    npc->IsProtected();
                break;
            default:
                break;
            }
        }
        else if (filter.type == "Relationship Rank") {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* playerBase = player ? player->GetActorBase() : nullptr;
            const auto* relationship = playerBase ?
                RE::BGSRelationship::GetRelationship(npc, playerBase) :
                nullptr;
            const auto rank = relationship ?
                4 - static_cast<int>(relationship->level.get()) :
                -5;
            match = MatchesNumericComparison(
                static_cast<float>(rank), filter);
        }
        else if (filter.type == "Cell Type") {
            const auto* cell = actor ? actor->GetParentCell() : nullptr;
            match = cell &&
                (static_cast<CellTypeFilter>(filter.optionMode) ==
                    CellTypeFilter::kInterior ?
                    cell->IsInteriorCell() :
                    !cell->IsInteriorCell());
        }
        else if (filter.type == "Equipped Category") {
            const auto category =
                static_cast<EquippedCategoryFilter>(
                    filter.optionMode);
            const auto needsInventory =
                category == EquippedCategoryFilter::kShield ||
                category == EquippedCategoryFilter::kHeavyArmor ||
                category == EquippedCategoryFilter::kLightArmor ||
                category == EquippedCategoryFilter::kClothing;
            if (needsInventory &&
                !equippedInventorySnapshot) {
                equippedInventorySnapshot.emplace(
                    actor ? actor->GetInventory() :
                        RE::TESObjectREFR::InventoryItemMap{});
            }
            match = MatchesEquippedCategory(
                actor,
                category,
                equippedInventorySnapshot ?
                    std::addressof(*equippedInventorySnapshot) :
                    nullptr);
        }

        if (filter.type == "Source Plugin" ||
            filter.type == "NPC Trait" ||
            filter.type == "Relationship Rank" ||
            filter.type == "Cell Type" ||
            filter.type == "Equipped Category") {
            if (match) {
                ++matches;
                if (!requiresAll) return true;
            }
            continue;
        }

        auto tokens = split(filter.formIDStr, '|');
        if (tokens.size() < 2) continue;

        auto fID = ResolveEDFFormID(filter.type, filter.editorID, filter.formIDStr);
        if (fID == 0 && filter.type == "Cell") {
            // Plugin-backed Cells may not have a TESObjectCELL instantiated
            // until the area is loaded. Their runtime FormID can still be
            // resolved directly from Plugin|LocalFormID.
            fID = ResolvePluginFormID(filter.formIDStr);
        }
        const auto filterValue = GetFilterValue(tokens, 1);

        if (filter.type == "NPC") {
            if (npc->GetFormID() == fID ||
                (npc->baseTemplateForm && npc->baseTemplateForm->GetFormID() == fID) ||
                (actor && actor->GetTemplateBase() && actor->GetTemplateBase()->GetFormID() == fID)) {
                match = true;
            }
        }
        else if (filter.type == "Leveled NPC") {
            auto levList = RE::TESForm::LookupByID<RE::TESLevCharacter>(fID);
            if (levList && IsNPCInLeveledList(npc, levList)) {
                match = true;
            }
        }
        else if (filter.type == "Keyword") {
            auto kwd = RE::TESForm::LookupByID<RE::BGSKeyword>(fID);
            if (kwd && (npc->HasKeyword(kwd) || SaveStateManager::GetSingleton()->HasVirtualKeyword(actor, kwd))) {
                match = true;
            }
            if (!match && kwd && npc->race) {
                npc->race->ForEachKeyword([&](const RE::BGSKeyword* a_keyword) {
                    if (a_keyword == kwd) {
                        match = true;
                        return RE::BSContainer::ForEachResult::kStop;
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
        }
        else if (filter.type == "Faction") {
            auto fact = RE::TESForm::LookupByID<RE::TESFaction>(fID);
            if (fact && ((actor && actor->IsInFaction(fact)) || npc->IsInFaction(fact))) {
                match = true;
            }
        }
        else if (filter.type == "Faction Rank") {
            auto fact = RE::TESForm::LookupByID<RE::TESFaction>(fID);
            if (actor && fact && actor->GetFactionRank(fact, actor->IsPlayer()) >= filterValue) {
                match = true;
            }
        }
        else if (filter.type == "Perk") {
            auto perk = RE::TESForm::LookupByID<RE::BGSPerk>(fID);
            if (actor && perk && actor->HasPerk(perk)) {
                match = true;
            }
        }
        else if (filter.type == "Spell") {
            auto spell = RE::TESForm::LookupByID<RE::SpellItem>(fID);
            if (actor && spell && actor->HasSpell(spell)) {
                match = true;
            }
        }
        else if (filter.type == "Shout") {
            auto shout = RE::TESForm::LookupByID<RE::TESShout>(fID);
            if (actor && shout && actor->HasShout(shout)) {
                match = true;
            }
        }
        else if (filter.type == "Race") {
            auto race = RE::TESForm::LookupByID<RE::TESRace>(fID);
            if (race && (npc->race == race)) {
                match = true;
            }
        }
        else if (filter.type == "Combat Style") {
            if (npc->combatStyle && npc->combatStyle->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Voice Type") {
            if (npc->voiceType && npc->voiceType->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Class") {
            if (npc->npcClass && npc->npcClass->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Skin") {
            // A Skin do NPC é um ponteiro para um TESObjectARMO (Armor)
            if (npc->skin && npc->skin->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Inventory Item") {
            auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(fID);
            if (actor && item && actor->GetInventoryCount(item) > 0) {
                match = true;
            }
        }
        else if (filter.type == "Inventory Count") {
            auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(fID);
            if (actor && item && actor->GetInventoryCount(item) >= std::max(1, filterValue)) {
                match = true;
            }
        }
        else if (filter.type == "Gold") {
            if (actor && actor->GetGoldAmount() >= std::max(0, filterValue)) {
                match = true;
            }
        }
        else if (filter.type == "Equipped Item") {
            auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(fID);
            if (IsActorEquippedItem(actor, item)) {
                match = true;
            }
        }
        else if (filter.type == "Package") {
            auto pkg = RE::TESForm::LookupByID<RE::TESPackage>(fID);
            if (pkg) {
                // Verifica na lista de pacotes do NPC base
                for (auto* pak : npc->aiPackages.packages) {
                    if (pak && pak->GetFormID() == fID) {
                        match = true;
                        break;
                    }
                }
            }
        }
        else if (filter.type == "Hair" || filter.type == "Facial Hair" ||
            filter.type == "HeadPart Misc" || filter.type == "HeadPart Face" ||
            filter.type == "HeadPart Eyes" || filter.type == "HeadPart Scar" ||
            filter.type == "HeadPart Eyebrows") {
            if (npc->headParts && npc->numHeadParts > 0) {
                for (std::int8_t i = 0; i < npc->numHeadParts; i++) {
                    if (npc->headParts[i] && npc->headParts[i]->GetFormID() == fID) {
                        match = true;
                        break;
                    }
                }
            }
        }
        else if (filter.type == "Location") {
            if (actor) {
                // Estamos em tempo de execução com um Actor real.
                auto targetLoc = RE::TESForm::LookupByID<RE::BGSLocation>(fID);
                auto currentLoc = actor->GetCurrentLocation();
                if (targetLoc && currentLoc) {
                    if (currentLoc == targetLoc || currentLoc->IsParent(targetLoc)) {
                        match = true;
                    }
                }
            }
        }
        else if (filter.type == "Cell") {
            if (actor) {
                auto currentCell = actor->GetParentCell();
                if (currentCell && currentCell->GetFormID() == fID) {
                    match = true;
                }
            }
        }
        else if (filter.type == "Worldspace") {
            if (actor) {
                const auto* worldspace =
                    RE::TESForm::LookupByID<RE::TESWorldSpace>(fID);
                match = worldspace &&
                    actor->GetWorldspace() == worldspace;
            }
        }
        else if (filter.type == "Location Keyword") {
            if (actor) {
                const auto* keyword =
                    RE::TESForm::LookupByID<RE::BGSKeyword>(fID);
                for (auto* location = actor->GetCurrentLocation();
                     keyword && location && !match;
                     location = location->parentLoc) {
                    match = location->HasKeyword(keyword);
                }
            }
        }
        else if (filter.type == "Quest") {
            auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(fID);
            match = MatchesQuestFilter(actor, quest, filter);
        }



        if (match) {
            matches++;
            if (!requiresAll) return true; // Se não exige todos, o primeiro match já valida
        }
    }

    return (requiresAll && matches == filters.size() && matches > 0);
}

#if 0  // Legacy per-file JSON storage retained only as migration reference.
void RuleManager::LoadRules() {
    _rules.clear();
    _ruleHistories.clear();
    _ruleIdToFileName.clear();

    EnsureRuleStorage();

    auto loadDirectory = [&](const fs::path& directory, bool fallbackOnly) {
        if (!fs::exists(directory)) return;

        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.path().extension() != ".json") continue;

            std::ifstream i(entry.path());
            try {
                rapidjson::IStreamWrapper stream(i);
                rapidjson::Document j;
                j.ParseStream(stream);

                if (!j.HasParseError() && j.IsArray() && !j.Empty()) {
                    std::vector<Rule> history;

                    const rapidjson::Value& latestJson = j[0];
                    Rule latest = ProcessRuleVersion(latestJson, "", "Sem Nome", true);

                    std::string ruleId = latest.id;
                    std::string ruleName = latest.name;
                    bool ruleEnabled = latest.isEnabled;
                    if (fallbackOnly && _ruleHistories.contains(ruleId)) {
                        continue;
                    }

                    history.push_back(latest);

                    for (rapidjson::SizeType idx = 1; idx < j.Size(); ++idx) {
                        history.push_back(ProcessRuleVersion(j[idx], ruleId, ruleName, ruleEnabled));
                    }

                    _ruleHistories[ruleId] = history;
                    _rules.push_back(latest);
                    _ruleIdToFileName[ruleId] = entry.path().stem().string();
                }
            }
            catch (const std::exception& e) {
                logger::error("Erro ao carregar regra {}: {}", entry.path().string(), e.what());
            }
        }
    };

    loadDirectory(_rulesDir, false);
    loadDirectory(_legacyRulesDir, true);
    logger::info("Carregadas {} regras com seus historicos (Suporte a Formato Compacto ativado).", _rules.size());
}
void RuleManager::SaveRules() {
    EnsureRuleStorage();

    const size_t MAX_HISTORY = 100;
    int updatedTotal = 0;

    for (const auto& filePath : _rulesToDelete) {
        if (fs::exists(filePath)) {
            fs::remove(filePath);
            logger::info("Arquivo deletado permanentemente: {}", filePath);
            updatedTotal++;
        }
    }
    _rulesToDelete.clear(); // Limpa a fila de espera

    for (auto& currentRule : _rules) {
        std::string currentContentHash = currentRule.CalculateHash();

        // Se o hash atual for diferente do último salvo
        if (currentRule.lastSavedHash != currentContentHash) {

            std::string newFileName = SanitizeFilename(currentRule.name);
            std::string oldFileName = _ruleIdToFileName[currentRule.id];

            // 2. Se o nome mudou, deleta o arquivo antigo para evitar duplicatas
            if (!oldFileName.empty() && oldFileName != newFileName) {
                std::string oldPath = _rulesDir + oldFileName + ".json";
                if (fs::exists(oldPath)) {
                    fs::remove(oldPath);
                    logger::info("Renomeando regra: deletando arquivo antigo '{}'", oldFileName);
                }
            }
            _ruleIdToFileName[currentRule.id] = newFileName;
            // 1. Incrementa a versão numérica
            currentRule.version++;
            currentRule.lastSavedHash = currentContentHash;

            // 2. Atualiza o histórico em memória
            auto& history = _ruleHistories[currentRule.id];
            history.insert(history.begin(), currentRule);

            // PODA: Mantém apenas as últimas X versões
            if (history.size() > MAX_HISTORY) {
                history.resize(MAX_HISTORY);
            }

            // SALVAMENTO OTIMIZADO
            rapidjson::Document historyDoc;
            historyDoc.SetArray();
            auto& alloc = historyDoc.GetAllocator();
            for (size_t i = 0; i < history.size(); ++i) {
                historyDoc.PushBack(WriteCompactRule(history[i], i == 0, alloc).Move(), alloc);
            }

            std::string filePath = _rulesDir + newFileName + ".json";
            std::ofstream o(filePath);
            // Salva sem indentação (dump) para velocidade e espaço
            o << SerializeJson(historyDoc) << std::endl;

            updatedTotal++;
            logger::info("Regra '{}' otimizada e salva. Versão: {}", currentRule.name, currentRule.version);
        }
    }

    if (updatedTotal > 0) {
        InitializeAffectedNPCsDatabase();
    }
}


void RuleManager::ExportRule(const Rule& rule) {
    namespace fs = std::filesystem;

    if (!_ruleIdToFileName.contains(rule.id)) {
        logger::error("Export: ID da regra não encontrado no mapeamento de arquivos.");
        return;
    }
    // 1. Caminhos de origem e destino
    std::string ruleFileName = _ruleIdToFileName[rule.id] + ".json";
    std::string sourcePath = _rulesDir + ruleFileName;
    if (!fs::exists(sourcePath)) {
        const auto legacySource = fs::path(_legacyRulesDir) / ruleFileName;
        if (fs::exists(legacySource)) {
            sourcePath = legacySource.string();
        }
    }

    fs::path exportDir = _exportDir;
    fs::create_directories(exportDir);

    std::string zipPath = (exportDir / (SanitizeFilename(rule.name) + ".zip")).string();

    // 2. Inicializa o arquivo ZIP
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) {
        logger::error("Export: Falha ao inicializar arquivo ZIP em {}", zipPath);
        return;
    }

    // 3. Define o caminho interno (onde o arquivo ficará dentro do ZIP)
    std::string internalZipPath = "Viny Mods/EDF/Rules/" + ruleFileName;

    // 4. Adiciona o arquivo ao ZIP
    // mz_zip_writer_add_file(arquivo_zip, nome_dentro_do_zip, caminho_no_disco, ...)
    if (!mz_zip_writer_add_file(&zip_archive, internalZipPath.c_str(), sourcePath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION)) {
        logger::error("Export: Falha ao adicionar arquivo {} ao ZIP", ruleFileName);
        mz_zip_writer_finalize_archive(&zip_archive);
        mz_zip_writer_end(&zip_archive);
        return;
    }

    // 5. Finaliza e fecha
    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);

    logger::info("Regra '{}' exportada com sucesso para: {}", rule.name, zipPath);
}

void RuleManager::ExportRulesPackage(const std::string& packageName, const std::set<std::string>& ruleIDs)
{
    if (ruleIDs.empty()) {
        logger::warn("Export: nenhuma regra selecionada.");
        return;
    }

    fs::create_directories(_exportDir);
    const auto safeName = SanitizeFilename(packageName.empty() ? "EDF_Export" : packageName);
    const auto zipPath = (fs::path(_exportDir) / (safeName + ".zip")).string();

    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));
    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) {
        logger::error("Export: Falha ao inicializar arquivo ZIP em {}", zipPath);
        return;
    }

    std::set<std::string> addedPaths;
    for (const auto& ruleID : ruleIDs) {
        auto fileIt = _ruleIdToFileName.find(ruleID);
        if (fileIt == _ruleIdToFileName.end()) continue;

        const auto ruleFileName = fileIt->second + ".json";
        fs::path sourcePath = fs::path(_rulesDir) / ruleFileName;
        if (!fs::exists(sourcePath)) {
            sourcePath = fs::path(_legacyRulesDir) / ruleFileName;
        }
        if (!fs::exists(sourcePath)) continue;

        const auto internalPath = "Viny Mods/EDF/Rules/" + ruleFileName;
        if (!addedPaths.insert(internalPath).second) continue;

        if (!mz_zip_writer_add_file(&zip_archive, internalPath.c_str(), sourcePath.string().c_str(), nullptr, 0, MZ_BEST_COMPRESSION)) {
            logger::error("Export: Falha ao adicionar '{}' como '{}'", sourcePath.string(), internalPath);
        }
    }

    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);
    logger::info("Pacote de regras exportado com sucesso para: {}", zipPath);
}

std::string GenerateUUID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    int i;
    ss << std::hex;
    for (i = 0; i < 8; i++) ss << dis(gen);
    return ss.str();
}

Rule& RuleManager::CreateRule() {
    Rule r;
    r.id = GenerateUUID();
    r.level = 1;
    _rules.push_back(r);
    return _rules.back();
}

void RuleManager::DeleteRule(const std::string& id) {
    // 1. Localiza o nome do arquivo antes de limpar os mapas
    if (_ruleIdToFileName.contains(id)) {
        std::string fileName = _ruleIdToFileName[id];
        std::string filePath = _rulesDir + fileName + ".json";

        // Adiciona à lista de pendências para deletar do disco no Save
        _rulesToDelete.push_back(filePath);

        // Limpa o rastro nos mapas
        _ruleIdToFileName.erase(id);
    }

    // 2. Remove do histórico
    _ruleHistories.erase(id);

    // 3. Remove do vetor principal
    std::erase_if(_rules, [&](const Rule& r) { return r.id == id; });

    logger::info("Regra {} marcada para deleção física.", id);
}

#endif

void RuleManager::LoadRules() {
    _packagesToDelete.clear();
    if (!RulePackageStore::GetSingleton()->Load(_rules, _ruleHistories, _ruleOwners)) {
        logger::error("Rules were only partially loaded because one or more packages failed.");
    }
    RebuildDependencyIndex();
}

std::string GenerateRuleUUID() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<std::uint32_t> dis(0, 0xFFFFFFFF);
    auto a = dis(gen);
    auto b = (dis(gen) & 0xFFFF0FFFU) | 0x00004000U;
    auto c = (dis(gen) & 0x3FFFFFFFU) | 0x80000000U;
    auto d = dis(gen);
    return std::format(
        "{:08x}-{:04x}-{:04x}-{:04x}-{:04x}{:08x}",
        a,
        b >> 16,
        b & 0xFFFF,
        c >> 16,
        c & 0xFFFF,
        d);
}

bool RuleManager::SaveRules() {
    bool ok = true;
    int updatedTotal = 0;
    for (auto& currentRule : _rules) {
        if (_packagesToDelete.contains(currentRule.packageID)) {
            continue;
        }
        if (!currentRule.IsModified()) {
            continue;
        }
        auto& history = _ruleHistories[currentRule.id];
        if (RulePackageStore::GetSingleton()->SaveRule(currentRule, history)) {
            _ruleOwners[currentRule.id] = currentRule.packageID;
            ++updatedTotal;
            logger::info("Rule '{}' saved to package '{}' at version {}.", currentRule.name, currentRule.packageID, currentRule.version);
        } else {
            ok = false;
            logger::error("Rule '{}' could not be saved.", currentRule.name);
        }
    }
    if (!ok) {
        return false;
    }

    bool deletedPackage = false;
    const auto pendingPackages = _packagesToDelete;
    for (const auto& packageID : pendingPackages) {
        if (!RulePackageStore::GetSingleton()->DeletePackage(packageID)) {
            ok = false;
            continue;
        }

        std::vector<std::string> deletedRuleIDs;
        for (const auto& rule : _rules) {
            if (rule.packageID == packageID) {
                deletedRuleIDs.push_back(rule.id);
            }
        }
        for (const auto& ruleID : deletedRuleIDs) {
            _ruleHistories.erase(ruleID);
            _ruleOwners.erase(ruleID);
        }
        std::erase_if(
            _rules,
            [&packageID](const Rule& rule) {
                return rule.packageID == packageID;
            });
        _packagesToDelete.erase(packageID);
        deletedPackage = true;
    }

    if (deletedPackage) {
        RebuildDependencyIndex();
    }
    if (updatedTotal > 0 || deletedPackage) {
        InitializeAffectedNPCsDatabase();
    }
    return ok;
}

const std::vector<RulePackage>& RuleManager::GetPackages() const {
    return RulePackageStore::GetSingleton()->GetPackages();
}

Rule* RuleManager::FindRule(const std::string& ruleID) {
    const auto found = _ruleIndices.find(ruleID);
    if (found == _ruleIndices.end() || found->second >= _rules.size()) {
        return nullptr;
    }
    return std::addressof(_rules[found->second]);
}

const Rule* RuleManager::FindRule(const std::string& ruleID) const {
    const auto found = _ruleIndices.find(ruleID);
    if (found == _ruleIndices.end() || found->second >= _rules.size()) {
        return nullptr;
    }
    return std::addressof(_rules[found->second]);
}

std::optional<std::string> RuleManager::CreatePackage(const std::string_view displayName) {
    return RulePackageStore::GetSingleton()->CreatePackage(displayName);
}

bool RuleManager::MarkPackageForDeletion(const std::string_view packageID)
{
    if (packageID.empty() ||
        packageID == RulePackageStore::LOCAL_PACKAGE_ID ||
        IsPackagePendingDeletion(packageID)) {
        return false;
    }
    const auto& packages = GetPackages();
    if (std::ranges::none_of(
            packages,
            [packageID](const RulePackage& package) {
                return package.id == packageID;
            })) {
        return false;
    }
    _packagesToDelete.emplace(packageID);
    logger::info(
        "Package '{}' marked for deletion; files remain until Save.",
        packageID);
    return true;
}

bool RuleManager::CancelPackageDeletion(const std::string_view packageID)
{
    return _packagesToDelete.erase(std::string(packageID)) > 0;
}

bool RuleManager::IsPackagePendingDeletion(
    const std::string_view packageID) const
{
    return _packagesToDelete.contains(std::string(packageID));
}

Rule& RuleManager::CreateRule(const std::string_view packageID) {
    Rule rule;
    do {
        rule.id = GenerateRuleUUID();
    } while (_ruleOwners.contains(rule.id));
    const auto& packages = GetPackages();
    const auto packageExists = std::ranges::any_of(packages, [packageID](const RulePackage& package) {
        return package.id == packageID;
    });
    rule.packageID = packageExists &&
            !IsPackagePendingDeletion(packageID)
        ? std::string(packageID)
        : std::string(RulePackageStore::LOCAL_PACKAGE_ID);
    rule.level = 1;
    _rules.push_back(rule);
    _ruleOwners[rule.id] = rule.packageID;
    RebuildDependencyIndex();
    return _rules.back();
}

std::optional<std::string> RuleManager::DuplicateRule(
    const std::string_view sourceRuleID,
    const std::string_view destinationPackageID,
    const std::string_view copyName)
{
    const auto source = FindRule(std::string(sourceRuleID));
    if (!source) {
        return std::nullopt;
    }
    const auto& packages = GetPackages();
    const auto packageExists = std::ranges::any_of(
        packages,
        [destinationPackageID](const RulePackage& package) {
            return package.id == destinationPackageID &&
                package.enabled;
        });
    if (!packageExists ||
        IsPackagePendingDeletion(destinationPackageID)) {
        return std::nullopt;
    }

    // Copy only authoring content. A duplicate deliberately has no database
    // history and no relationship with the source rule's runtime/save ledger.
    Rule copy = *source;
    do {
        copy.id = GenerateRuleUUID();
    } while (_ruleOwners.contains(copy.id));
    copy.packageID = std::string(destinationPackageID);
    copy.name = copyName.empty() ?
        std::format("{} (Copy)", source->name) :
        std::string(copyName);
    copy.isEnabled = false;
    copy.version = 0;
    copy.lastSavedHash.clear();

    const auto copyID = copy.id;
    _rules.push_back(std::move(copy));
    _ruleOwners[copyID] = std::string(destinationPackageID);
    _ruleHistories.erase(copyID);
    RebuildDependencyIndex();
    logger::info(
        "Rule '{}' duplicated as '{}' in package '{}'; the copy remains "
        "disabled and unsaved until the next Save.",
        sourceRuleID,
        copyID,
        destinationPackageID);
    return copyID;
}

bool RuleManager::DeleteRule(const std::string& id) {
    const auto found = std::ranges::find_if(_rules, [&id](const Rule& rule) {
        return rule.id == id;
    });
    if (found == _rules.end()) {
        return false;
    }
    if (found->version > 0 &&
        !RulePackageStore::GetSingleton()->DeleteRule(id, found->packageID)) {
        logger::error("Rule '{}' could not be deleted from package '{}'.", id, found->packageID);
        return false;
    }
    _ruleHistories.erase(id);
    _ruleOwners.erase(id);
    _rules.erase(found);
    RebuildDependencyIndex();
    logger::info("Rule '{}' deleted.", id);
    return true;
}

bool RuleManager::CreateRulesPackageSnapshot(
    const std::string& packageName,
    const std::vector<Rule>& rules,
    const fs::path& stagingRoot,
    RulePackage& outPackage)
{
    std::map<std::string, std::vector<Rule>> histories;
    for (const auto& rule : rules) {
        if (const auto found = _ruleHistories.find(rule.id); found != _ruleHistories.end()) {
            histories.emplace(rule.id, found->second);
        }
    }
    return RulePackageStore::GetSingleton()->CreateSnapshot(
        packageName,
        rules,
        histories,
        stagingRoot,
        outPackage);
}

bool RuleManager::ExportRule(const Rule& rule) {
    return ExportRulesPackage(rule.name.empty() ? "EDF_Rule" : rule.name, { rule.id });
}

bool RuleManager::ExportRulesPackage(const std::string& packageName, const std::set<std::string>& ruleIDs)
{
    if (ruleIDs.empty()) {
        logger::warn("Export: no rules selected.");
        return false;
    }

    std::map<std::string, std::vector<Rule>> selectedByPackage;
    for (const auto& rule : _rules) {
        if (ruleIDs.contains(rule.id)) {
            selectedByPackage[rule.packageID].push_back(rule);
        }
    }
    if (selectedByPackage.empty()) {
        logger::warn("Export: selected rule IDs were not found.");
        return false;
    }

    std::error_code ec;
    fs::create_directories(_exportDir, ec);
    if (ec) {
        logger::error("Export: could not create '{}': {}", _exportDir, ec.message());
        return false;
    }
    const auto safeName = SanitizeFilename(packageName.empty() ? "EDF_Export" : packageName);
    const auto zipPath = fs::path(_exportDir) / (safeName + ".zip");
    const auto stagingRoot = fs::temp_directory_path() /
        std::format("edf_export_{}", std::chrono::steady_clock::now().time_since_epoch().count());

    std::vector<RulePackage> snapshots;
    snapshots.reserve(selectedByPackage.size());
    const auto& packages = GetPackages();
    for (const auto& [packageID, selected] : selectedByPackage) {
        const auto source = std::ranges::find_if(
            packages,
            [&packageID](const RulePackage& package) {
                return package.id == packageID;
            });
        if (source == packages.end()) {
            logger::error(
                "Export: source package '{}' was not found.",
                packageID);
            fs::remove_all(stagingRoot, ec);
            return false;
        }

        std::map<std::string, std::vector<Rule>> histories;
        for (const auto& rule : selected) {
            if (const auto found = _ruleHistories.find(rule.id);
                found != _ruleHistories.end()) {
                histories.emplace(rule.id, found->second);
            }
        }

        RulePackage snapshot;
        if (!RulePackageStore::GetSingleton()->CreateSnapshot(
                *source,
                selected,
                histories,
                stagingRoot,
                snapshot)) {
            fs::remove_all(stagingRoot, ec);
            return false;
        }
        snapshots.push_back(std::move(snapshot));
    }

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zipPath.string().c_str(), 0)) {
        fs::remove_all(stagingRoot, ec);
        return false;
    }
    bool ok = true;
    for (const auto& package : snapshots) {
        const auto folder = package.path.filename().generic_string();
        const auto manifestInternal = std::format(
            "Viny Mods/EDF/Packages/{}/manifest.json", folder);
        const auto databaseInternal = std::format(
            "Viny Mods/EDF/Packages/{}/package.db", folder);
        ok =
            mz_zip_writer_add_file(
                &zip,
                manifestInternal.c_str(),
                (package.path / "manifest.json").string().c_str(),
                nullptr,
                0,
                MZ_BEST_COMPRESSION) &&
            mz_zip_writer_add_file(
                &zip,
                databaseInternal.c_str(),
                (package.path / "package.db").string().c_str(),
                nullptr,
                0,
                MZ_BEST_COMPRESSION) &&
            ok;
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    fs::remove_all(stagingRoot, ec);
    if (!ok) {
        logger::error("Export: failed to create package ZIP '{}'.", zipPath.string());
        return false;
    }
    logger::info(
        "{} source package(s) exported to '{}'.",
        snapshots.size(),
        zipPath.string());
    return true;
}

std::vector<Reward> RuleManager::GetRewardsForNPC(RE::TESNPC* npc) {
    std::vector<Reward> applicable;
    if (!npc) return applicable;

    // 1. Obter o identificador do NPC no formato correto (Hexadecimal 5 ou 3 dígitos)
    std::string npcPlugin = npc->IsDynamicForm() ? "Dynamic" : "";
    if (auto file = GetSourceFileByFormID(npc)) {
        npcPlugin = file->GetFilename();
    }

    // CORREÇÃO: Usar FormatLocalFormID em vez de std::to_string
    std::string npcIdentifier = npcPlugin + "|" + FormatLocalFormID(npc->GetFormID(), npcPlugin);

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 100.0f);

    for (const auto& rule : _rules) {
        if (IsNPCMatchingTargets(npc, rule, false) && !IsNPCMatchingTargets(npc, rule, true)) {
            // 3. Processa os grupos de recompensa da regra
            for (const auto& group : rule.rewardGroups) {
                if (group.isExclusive) {
                    // Lógica de Sorteio Único (Exclusivo)
                    float roll = dis(gen);
                    float cumulative = 0.0f;
                    for (const auto& reward : group.rewards) {
                        cumulative += reward.chanceReward;
                        if (roll <= cumulative) {
                            applicable.push_back(reward);
                            break;
                        }
                    }
                }
                else {
                    // Lógica de Sorteio Independente
                    for (const auto& reward : group.rewards) {
                        if (dis(gen) <= reward.chanceReward) {
                            applicable.push_back(reward);
                        }
                    }
                }
            }

        }
    }

    return applicable;
}

std::vector<RewardGroup> RuleManager::RollForGroups(RE::TESNPC* npc, const Rule& rule) {
    std::vector<RewardGroup> wonGroups;
    if (!npc) return wonGroups;

    if (rule.isExclusive) {
        // Lógica: Escolhe apenas UM grupo da regra baseado nas chances (chanceGroup)
        float roll = GetRandomFloat(0.0f, 100.0f);
        float cumulative = 0.0f;
        for (const auto& group : rule.rewardGroups) {
            cumulative += group.chanceGroup;
            if (roll <= cumulative) {
                wonGroups.push_back(group);
                break;
            }
        }
    }
    else {
        // Lógica: Testa cada grupo independentemente
        for (const auto& group : rule.rewardGroups) {
            if (GetRandomFloat(0.0f, 100.0f) <= group.chanceGroup) {
                wonGroups.push_back(group);
            }
        }
    }
    return wonGroups;
}




Rule* RuleManager::GetRuleVersion(const std::string& ruleID, int version) {
    if (_ruleHistories.contains(ruleID)) {
        for (auto& rule : _ruleHistories[ruleID]) {
            if (rule.version == version) return &rule;
        }
    }
    return nullptr;
}

std::vector<Reward> RuleManager::GetRewardsForSpecificRule(RE::TESNPC* npc, const Rule& rule) {
    std::vector<Reward> applicable;
    if (!npc) return applicable;

    std::string npcName = npc->GetName();
    logger::debug("[Sorteio] --- Iniciando processamento para: {} (Regra: {}) ---", npcName, rule.name);

    auto processGroup = [&](const RewardGroup& group) {
        if (group.isExclusive) {
            float roll = GetRandomFloat(0.0f, 100.0f);
            float cumulative = 0.0f;
            for (const auto& reward : group.rewards) {
                cumulative += reward.chanceReward;
                if (roll <= cumulative) { applicable.push_back(reward); break; }
            }
        }
        else {
            for (const auto& reward : group.rewards) {
                if (GetRandomFloat(0.0f, 100.0f) <= reward.chanceReward) applicable.push_back(reward);
            }
        }
        };

    if (rule.isExclusive) {
        // LÓGICA: Escolhe apenas UM grupo da regra baseado nas chances
        float roll = GetRandomFloat(0.0f, 100.0f);
        float cumulative = 0.0f;
        for (const auto& group : rule.rewardGroups) {
            cumulative += group.chanceGroup;
            if (roll <= cumulative) {
                logger::debug("  >> Grupo Selecionado Exclusivamente: {}", group.name);
                processGroup(group);
                break;
            }
        }
    }
    else {
        // LÓGICA: Testa cada grupo independentemente
        for (const auto& group : rule.rewardGroups) {
            if (GetRandomFloat(0.0f, 100.0f) <= group.chanceGroup) {
                processGroup(group);
            }
        }
    }
    return applicable;
}


void RuleManager::RebuildDependencyIndex(const bool invalidateActorSnapshots)
{
    _affectedNPCsDatabaseValid = false;
    _ruleIndices.clear();
    _rulesByDependency.clear();
    _rulesByExactDependency.clear();
    _rulesByActorValue.clear();
    _rulesBySourcePlugin.clear();
    _rulesByNPCTrait.clear();
    _rulesByCellType.clear();
    _rulesByEquippedCategory.clear();
    _rulesByRelationship.clear();
    _rulesWithUnresolvedDependency.clear();
    _ruleDependencyMasks.clear();
    _broadFullEvaluationRules.clear();
    _unstableCycleRules.clear();
    _watchedActorValues.clear();
    _hasFollowerDependentRules = false;

    constexpr std::array dependencyBits{
        RuleDependency::kStatic,
        RuleDependency::kTag,
        RuleDependency::kFactionRank,
        RuleDependency::kAbility,
        RuleDependency::kInventory,
        RuleDependency::kEnvironment,
        RuleDependency::kLevel,
        RuleDependency::kSleep,
        RuleDependency::kCombat,
        RuleDependency::kActorValue,
        RuleDependency::kFollower,
        RuleDependency::kQuest,
        RuleDependency::kRelationship,
        RuleDependency::kEquipment
    };

    _ruleIndices.reserve(_rules.size());
    for (std::size_t index = 0; index < _rules.size(); ++index) {
        const auto& ruleID = _rules[index].id;
        if (!_ruleIndices.emplace(ruleID, index).second) {
            logger::error(
                "[RuleIndex] Duplicate rule ID '{}' found while rebuilding the runtime index; "
                "the first deterministic occurrence remains authoritative.",
                ruleID);
        }
    }

    const auto addDependency = [&](const Rule& a_rule, RuleDependencyMask a_mask) {
        for (const auto dependency : dependencyBits) {
            const auto bit = ToMask(dependency);
            if ((a_mask & bit) != 0) {
                _rulesByDependency[bit].insert(a_rule.id);
            }
        }
    };

    const auto addFilter = [&](const Rule& a_rule, const BlacklistFilter& a_filter) {
        const auto dependency = GetFilterDependencyMask(a_filter.type);
        addDependency(a_rule, dependency);
        const auto addUnresolved = [&]() {
            for (const auto dependencyBit : dependencyBits) {
                const auto bit = ToMask(dependencyBit);
                if ((dependency & bit) != 0) {
                    _rulesWithUnresolvedDependency[bit].insert(
                        a_rule.id);
                }
            }
        };
        if (a_filter.type == "Actor Value") {
            const auto actorValue =
                ResolveActorValue(a_filter.actorValueName);
            if (actorValue != RE::ActorValue::kNone &&
                (a_filter.actorValueMode != ActorValueMode::kMaximum ||
                    IsMaximumActorValueSupported(actorValue))) {
                _rulesByActorValue[actorValue].insert(a_rule.id);
                _watchedActorValues.emplace(
                    actorValue, a_filter.actorValueMode);
            }
            else {
                addUnresolved();
                logger::warn(
                    "[RuleIndex] Rule '{}' has invalid Actor Value filter '{}'.",
                    a_rule.name,
                    a_filter.actorValueName);
            }
            return dependency;
        }
        if (a_filter.type == "Source Plugin" ||
            a_filter.type == "NPC Trait" ||
            a_filter.type == "Relationship Rank" ||
            a_filter.type == "Cell Type" ||
            a_filter.type == "Equipped Category") {
            if (a_filter.type == "Source Plugin") {
                auto source = a_filter.optionText.empty() ?
                    a_filter.editorID :
                    a_filter.optionText;
                std::ranges::transform(
                    source, source.begin(),
                    [](const unsigned char value) {
                        return static_cast<char>(
                            std::tolower(value));
                    });
                _rulesBySourcePlugin[source].insert(a_rule.id);
            }
            else if (a_filter.type == "NPC Trait") {
                _rulesByNPCTrait[a_filter.optionMode].insert(
                    a_rule.id);
            }
            else if (a_filter.type == "Cell Type") {
                _rulesByCellType[a_filter.optionMode].insert(
                    a_rule.id);
            }
            else if (a_filter.type == "Equipped Category") {
                _rulesByEquippedCategory[
                    a_filter.optionMode].insert(a_rule.id);
            }
            else {
                _rulesByRelationship.insert(a_rule.id);
            }
            addUnresolved();
            return dependency;
        }
        const auto formID = ResolveEDFFormID(
            a_filter.type, a_filter.editorID, a_filter.formIDStr);
        if (formID != 0) {
            _rulesByExactDependency[dependency][formID].insert(a_rule.id);
        } else {
            addUnresolved();
        }
        return dependency;
    };

    for (const auto& rule : _rules) {
        RuleDependencyMask mask = ToMask(RuleDependency::kLevel);
        if (rule.combatState != RuleCombatState::kAny) {
            mask |= ToMask(RuleDependency::kCombat);
            addDependency(rule, ToMask(RuleDependency::kCombat));
        }
        if (rule.followerState != RuleFollowerState::kAny) {
            mask |= ToMask(RuleDependency::kFollower);
            addDependency(rule, ToMask(RuleDependency::kFollower));
            _hasFollowerDependentRules =
                _hasFollowerDependentRules || rule.isEnabled;
        }
        if (rule.targetFilters.empty()) {
            _broadFullEvaluationRules.insert(rule.id);
        }
        const bool hasBaseConstraints =
            rule.targetGender != 0 || rule.targetHumanoid != 0 ||
            rule.targetChild != 0 || rule.blacklistedGender != 0 ||
            rule.blacklistedHumanoid != 0 || rule.blacklistedChild != 0;
        if (hasBaseConstraints ||
            (rule.targetFilters.empty() && rule.blacklistFilters.empty())) {
            mask |= ToMask(RuleDependency::kStatic);
            addDependency(rule, ToMask(RuleDependency::kStatic));
        }

        for (const auto& filter : rule.targetFilters) {
            mask |= addFilter(rule, filter);
            if (filter.type == "Gold" || filter.type == "Leveled NPC" ||
                (!IsNonFormFilterType(filter.type) && ResolveEDFFormID(
                    filter.type, filter.editorID, filter.formIDStr) == 0)) {
                _broadFullEvaluationRules.insert(rule.id);
            }
        }
        for (const auto& filter : rule.blacklistFilters) {
            mask |= addFilter(rule, filter);
        }
        for (const auto& group : rule.rewardGroups) {
            for (const auto& reward : group.rewards) {
                if (reward.typeReward == "Spell" &&
                    (reward.functionOnType == 1 ||
                    reward.functionOnType == 2)) {
                    mask |= ToMask(RuleDependency::kSleep);
                    addDependency(rule, ToMask(RuleDependency::kSleep));
                }
                if (IsEquipmentRewardType(reward.typeReward)) {
                    const bool normal =
                        (reward.equipContexts &
                            ToMask(EquipmentContext::kNormal)) != 0;
                    const bool sleep =
                        (reward.equipContexts &
                            ToMask(EquipmentContext::kSleep)) != 0;
                    const bool combat =
                        (reward.equipContexts &
                            ToMask(EquipmentContext::kCombat)) != 0;
                    if (sleep != normal) {
                        mask |= ToMask(RuleDependency::kSleep);
                        addDependency(rule, ToMask(RuleDependency::kSleep));
                    }
                    if (combat != normal) {
                        mask |= ToMask(RuleDependency::kCombat);
                        addDependency(rule, ToMask(RuleDependency::kCombat));
                    }
                }
            }
        }

        addDependency(rule, ToMask(RuleDependency::kLevel));
        _ruleDependencyMasks[rule.id] = mask;
    }

    std::map<std::string, std::set<std::string>> dependencyGraph;
    std::set<std::pair<std::string, std::string>> negativeEdges;
    for (const auto& producer : _rules) {
        for (const auto& group : producer.rewardGroups) {
            for (const auto& reward : group.rewards) {
                const auto producedMask =
                    GetRewardDependencyMask(reward.typeReward);
                const auto [plugin, formID] = reward.ParseFormID();
                if (producedMask == ToMask(RuleDependency::kNone) ||
                    formID == 0) {
                    continue;
                }
                for (const auto dependency : dependencyBits) {
                    const auto bit = ToMask(dependency);
                    if ((producedMask & bit) == 0) {
                        continue;
                    }
                    if (dependency == RuleDependency::kFollower) {
                        if (reward.typeReward != "Faction" ||
                            !IsFollowerIdentityFaction(formID)) {
                            continue;
                        }
                        const auto consumers =
                            _rulesByDependency.find(bit);
                        if (consumers == _rulesByDependency.end()) {
                            continue;
                        }
                        dependencyGraph[producer.id].insert(
                            consumers->second.begin(),
                            consumers->second.end());
                        for (const auto& consumerID : consumers->second) {
                            const auto* consumer = FindRule(consumerID);
                            if (consumer &&
                                consumer->followerState ==
                                    RuleFollowerState::kExcludeActive) {
                                negativeEdges.emplace(
                                    producer.id, consumerID);
                            }
                        }
                        continue;
                    }
                    const auto byDependency =
                        _rulesByExactDependency.find(bit);
                    if (byDependency == _rulesByExactDependency.end()) {
                        continue;
                    }
                    if (const auto consumers =
                            byDependency->second.find(formID);
                        consumers != byDependency->second.end()) {
                        dependencyGraph[producer.id].insert(
                            consumers->second.begin(), consumers->second.end());
                        for (const auto& consumerID : consumers->second) {
                            const auto* consumer = FindRule(consumerID);
                            if (!consumer) {
                                continue;
                            }
                            const bool isNegative = std::ranges::any_of(
                                consumer->blacklistFilters,
                                [&](const BlacklistFilter& a_filter) {
                                    return (GetFilterDependencyMask(a_filter.type) &
                                                bit) != 0 &&
                                        ResolveEDFFormID(
                                            a_filter.type,
                                            a_filter.editorID,
                                            a_filter.formIDStr) == formID;
                                });
                            if (isNegative) {
                                negativeEdges.emplace(producer.id, consumerID);
                            }
                        }
                    }
                }
            }
        }
    }

    std::map<std::string, std::uint8_t> visitState;
    std::vector<std::string> visitStack;
    std::set<std::pair<std::string, std::string>> reportedCycles;
    std::function<void(const std::string&)> visit =
        [&](const std::string& a_ruleID) {
            visitState[a_ruleID] = 1;
            visitStack.push_back(a_ruleID);
            if (const auto edges = dependencyGraph.find(a_ruleID);
                edges != dependencyGraph.end()) {
                for (const auto& next : edges->second) {
                    if (visitState[next] == 0) {
                        visit(next);
                    } else if (visitState[next] == 1 &&
                        reportedCycles.emplace(a_ruleID, next).second) {
                        logger::warn(
                            "[RuleIndex] Potential nested-rule cycle: '{}' -> '{}'. "
                            "Runtime state-signature protection is enabled.",
                            a_ruleID, next);
                    }
                }
            }
            visitStack.pop_back();
            visitState[a_ruleID] = 2;
        };
    for (const auto& [ruleID, edges] : dependencyGraph) {
        if (visitState[ruleID] == 0) {
            visit(ruleID);
        }
    }

    std::map<std::string, int> tarjanIndex;
    std::map<std::string, int> tarjanLowLink;
    std::set<std::string> tarjanOnStack;
    std::vector<std::string> tarjanStack;
    int nextTarjanIndex = 0;
    std::function<void(const std::string&)> findComponents =
        [&](const std::string& a_ruleID) {
            tarjanIndex[a_ruleID] = nextTarjanIndex;
            tarjanLowLink[a_ruleID] = nextTarjanIndex;
            ++nextTarjanIndex;
            tarjanStack.push_back(a_ruleID);
            tarjanOnStack.insert(a_ruleID);

            if (const auto edges = dependencyGraph.find(a_ruleID);
                edges != dependencyGraph.end()) {
                for (const auto& next : edges->second) {
                    if (!tarjanIndex.contains(next)) {
                        findComponents(next);
                        tarjanLowLink[a_ruleID] = std::min(
                            tarjanLowLink[a_ruleID], tarjanLowLink[next]);
                    } else if (tarjanOnStack.contains(next)) {
                        tarjanLowLink[a_ruleID] = std::min(
                            tarjanLowLink[a_ruleID], tarjanIndex[next]);
                    }
                }
            }

            if (tarjanLowLink[a_ruleID] != tarjanIndex[a_ruleID]) {
                return;
            }

            std::set<std::string> component;
            while (!tarjanStack.empty()) {
                auto member = std::move(tarjanStack.back());
                tarjanStack.pop_back();
                tarjanOnStack.erase(member);
                component.insert(member);
                if (member == a_ruleID) {
                    break;
                }
            }

            const bool hasNegativeInternalEdge = std::ranges::any_of(
                negativeEdges,
                [&](const auto& a_edge) {
                    return component.contains(a_edge.first) &&
                        component.contains(a_edge.second);
                });
            if (hasNegativeInternalEdge) {
                _unstableCycleRules.insert(
                    component.begin(), component.end());
                logger::error(
                    "[RuleIndex] Unstable nested-rule component disabled: {}. "
                    "It contains a reward-to-blacklist cycle.",
                    fmt::join(component, ", "));
            }
        };

    std::set<std::string> graphVertices;
    for (const auto& [ruleID, edges] : dependencyGraph) {
        graphVertices.insert(ruleID);
        graphVertices.insert(edges.begin(), edges.end());
    }
    for (const auto& ruleID : graphVertices) {
        if (!tarjanIndex.contains(ruleID)) {
            findComponents(ruleID);
        }
    }

    if (invalidateActorSnapshots) {
        ++_dependencyRevision;
    }
    logger::info("[RuleIndex] Compiled {} rules at revision {}.",
        _rules.size(), _dependencyRevision);
}

std::vector<std::string> RuleManager::GetCandidateRuleIDs(
    const RuleEvaluationDelta& a_delta) const
{
    if (a_delta.IsFull()) {
        std::vector<std::string> all;
        all.reserve(_rules.size());
        for (const auto& rule : _rules) {
            all.push_back(rule.id);
        }
        return all;
    }

    std::set<std::string> candidates;
    constexpr std::array dependencyBits{
        RuleDependency::kStatic,
        RuleDependency::kTag,
        RuleDependency::kFactionRank,
        RuleDependency::kAbility,
        RuleDependency::kInventory,
        RuleDependency::kEnvironment,
        RuleDependency::kLevel,
        RuleDependency::kSleep,
        RuleDependency::kCombat,
        RuleDependency::kActorValue,
        RuleDependency::kFollower,
        RuleDependency::kQuest,
        RuleDependency::kRelationship,
        RuleDependency::kEquipment
    };

    for (const auto dependency : dependencyBits) {
        const auto bit = ToMask(dependency);
        if ((a_delta.mask & bit) == 0) {
            continue;
        }

        if (dependency == RuleDependency::kActorValue) {
            if (a_delta.changedActorValues.empty()) {
                if (const auto found = _rulesByDependency.find(bit);
                    found != _rulesByDependency.end()) {
                    candidates.insert(
                        found->second.begin(), found->second.end());
                }
            }
            else {
                for (const auto actorValue :
                     a_delta.changedActorValues) {
                    if (const auto found =
                            _rulesByActorValue.find(actorValue);
                        found != _rulesByActorValue.end()) {
                        candidates.insert(
                            found->second.begin(), found->second.end());
                    }
                }
            }
            continue;
        }

        if (dependency == RuleDependency::kFollower) {
            if (const auto found =
                    _rulesByDependency.find(bit);
                found != _rulesByDependency.end()) {
                candidates.insert(
                    found->second.begin(),
                    found->second.end());
            }
            continue;
        }

        if (a_delta.changedForms.empty()) {
            if (const auto found = _rulesByDependency.find(bit);
                found != _rulesByDependency.end()) {
                candidates.insert(found->second.begin(), found->second.end());
            }
            continue;
        }

        if (const auto unresolved =
                _rulesWithUnresolvedDependency.find(bit);
            unresolved != _rulesWithUnresolvedDependency.end()) {
            candidates.insert(unresolved->second.begin(), unresolved->second.end());
        }
        if (const auto exactByForm =
                _rulesByExactDependency.find(bit);
            exactByForm != _rulesByExactDependency.end()) {
            for (const auto formID : a_delta.changedForms) {
                if (const auto exact = exactByForm->second.find(formID);
                    exact != exactByForm->second.end()) {
                    candidates.insert(exact->second.begin(), exact->second.end());
                }
            }
        }
    }

    return { candidates.begin(), candidates.end() };
}

std::vector<std::string> RuleManager::GetCandidateRuleIDs(
    RE::Actor* a_actor,
    const RuleEvaluationDelta& a_delta) const
{
    if (!a_actor || !a_delta.IsFull()) {
        return GetCandidateRuleIDs(a_delta);
    }

    auto npc = a_actor->GetActorBase();
    if (!npc) {
        return {};
    }

    std::set<std::string> candidates(
        _broadFullEvaluationRules.begin(),
        _broadFullEvaluationRules.end());
    std::set<RE::FormID> baseForms{
        a_actor->GetFormID(),
        npc->GetFormID()
    };
    const auto addForm = [&baseForms](const RE::TESForm* a_form) {
        if (a_form) {
            baseForms.insert(a_form->GetFormID());
        }
    };
    addForm(a_actor->GetTemplateBase());
    addForm(npc->baseTemplateForm);
    addForm(npc->race);
    addForm(npc->npcClass);
    addForm(npc->voiceType);
    addForm(npc->combatStyle);
    addForm(npc->skin);
    for (auto* package : npc->aiPackages.packages) {
        addForm(package);
    }
    if (npc->headParts && npc->numHeadParts > 0) {
        for (std::int8_t index = 0; index < npc->numHeadParts; ++index) {
            addForm(npc->headParts[index]);
        }
    }

    auto inventory = a_actor->GetInventory();
    std::set<RE::FormID> inventoryForms;
    for (const auto& [item, data] : inventory) {
        if (item && data.first > 0) {
            inventoryForms.insert(item->GetFormID());
        }
    }
    if (auto equipped = a_actor->GetEquippedObject(false)) {
        inventoryForms.insert(equipped->GetFormID());
    }
    if (auto equipped = a_actor->GetEquippedObject(true)) {
        inventoryForms.insert(equipped->GetFormID());
    }

    const auto append = [&candidates](
        const std::set<std::string>& a_rules) {
        candidates.insert(a_rules.begin(), a_rules.end());
    };

    auto sourcePlugin = std::string("Dynamic");
    if (const auto* source = GetSourceFileByFormID(npc)) {
        sourcePlugin = std::string(source->GetFilename());
    }
    std::ranges::transform(
        sourcePlugin, sourcePlugin.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    if (const auto found =
            _rulesBySourcePlugin.find(sourcePlugin);
        found != _rulesBySourcePlugin.end()) {
        append(found->second);
    }
    const auto appendTrait = [&](const NPCTraitFilter trait,
        const bool matches) {
        if (!matches) return;
        if (const auto found = _rulesByNPCTrait.find(
                static_cast<int>(trait));
            found != _rulesByNPCTrait.end()) {
            append(found->second);
        }
    };
    appendTrait(NPCTraitFilter::kUnique, npc->IsUnique());
    appendTrait(
        NPCTraitFilter::kEssential,
        a_actor->IsEssential());
    appendTrait(
        NPCTraitFilter::kProtected,
        a_actor->IsProtected());
    if (const auto* cell = a_actor->GetParentCell()) {
        const auto cellType = cell->IsInteriorCell() ?
            CellTypeFilter::kInterior :
            CellTypeFilter::kExterior;
        if (const auto found = _rulesByCellType.find(
                static_cast<int>(cellType));
            found != _rulesByCellType.end()) {
            append(found->second);
        }
    }
    append(_rulesByRelationship);
    for (const auto& [category, rules] :
         _rulesByEquippedCategory) {
        if (MatchesEquippedCategory(
                a_actor,
                static_cast<EquippedCategoryFilter>(
                    category),
                std::addressof(inventory))) {
            append(rules);
        }
    }
    const auto appendExactForms = [&](RuleDependency a_dependency,
        const std::set<RE::FormID>& a_forms) {
        const auto dependency = ToMask(a_dependency);
        const auto byDependency = _rulesByExactDependency.find(dependency);
        if (byDependency == _rulesByExactDependency.end()) {
            return;
        }
        for (const auto formID : a_forms) {
            if (const auto found = byDependency->second.find(formID);
                found != byDependency->second.end()) {
                append(found->second);
            }
        }
    };

    appendExactForms(RuleDependency::kStatic, baseForms);
    appendExactForms(RuleDependency::kInventory, inventoryForms);
    if (const auto actorValueRules = _rulesByDependency.find(
            ToMask(RuleDependency::kActorValue));
        actorValueRules != _rulesByDependency.end()) {
        append(actorValueRules->second);
    }

    const auto appendMatchingExact = [&](RuleDependency a_dependency,
        const auto& a_matches) {
        const auto dependency = ToMask(a_dependency);
        const auto byDependency = _rulesByExactDependency.find(dependency);
        if (byDependency == _rulesByExactDependency.end()) {
            return;
        }
        for (const auto& [formID, rules] : byDependency->second) {
            if (a_matches(formID)) {
                append(rules);
            }
        }
    };

    appendMatchingExact(RuleDependency::kTag, [&](RE::FormID a_formID) {
        if (auto keyword = RE::TESForm::LookupByID<RE::BGSKeyword>(a_formID)) {
            if (npc->HasKeyword(keyword) ||
                SaveStateManager::GetSingleton()->HasVirtualKeyword(
                    a_actor, keyword)) {
                return true;
            }
            bool raceMatch = false;
            if (npc->race) {
                npc->race->ForEachKeyword(
                    [&](const RE::BGSKeyword* a_keyword) {
                        if (a_keyword == keyword) {
                            raceMatch = true;
                            return RE::BSContainer::ForEachResult::kStop;
                        }
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
            }
            return raceMatch;
        }
        if (auto faction = RE::TESForm::LookupByID<RE::TESFaction>(a_formID)) {
            return a_actor->IsInFaction(faction) || npc->IsInFaction(faction);
        }
        return false;
    });

    appendMatchingExact(
        RuleDependency::kFactionRank,
        [&](RE::FormID a_formID) {
            auto faction = RE::TESForm::LookupByID<RE::TESFaction>(a_formID);
            return faction && a_actor->IsInFaction(faction);
        });

    appendMatchingExact(RuleDependency::kAbility, [&](RE::FormID a_formID) {
        if (auto perk = RE::TESForm::LookupByID<RE::BGSPerk>(a_formID)) {
            return a_actor->HasPerk(perk);
        }
        if (auto spell = RE::TESForm::LookupByID<RE::SpellItem>(a_formID)) {
            return a_actor->HasSpell(spell);
        }
        if (auto shout = RE::TESForm::LookupByID<RE::TESShout>(a_formID)) {
            return a_actor->HasShout(shout);
        }
        return false;
    });

    appendMatchingExact(
        RuleDependency::kEnvironment,
        [&](RE::FormID a_formID) {
            if (auto cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(a_formID)) {
                return a_actor->GetParentCell() == cell;
            }
            if (auto location =
                    RE::TESForm::LookupByID<RE::BGSLocation>(a_formID)) {
                auto current = a_actor->GetCurrentLocation();
                if (current &&
                    (current == location || current->IsParent(location))) {
                    return true;
                }
            }
            if (auto worldspace =
                    RE::TESForm::LookupByID<RE::TESWorldSpace>(a_formID)) {
                return a_actor->GetWorldspace() == worldspace;
            }
            if (auto keyword =
                    RE::TESForm::LookupByID<RE::BGSKeyword>(a_formID)) {
                for (auto* location = a_actor->GetCurrentLocation();
                     location;
                     location = location->parentLoc) {
                    if (location->HasKeyword(keyword)) {
                        return true;
                    }
                }
            }
            return false;
        });

    appendMatchingExact(
        RuleDependency::kQuest,
        [&](RE::FormID a_formID) {
            auto* quest =
                RE::TESForm::LookupByID<RE::TESQuest>(a_formID);
            if (!quest) {
                return false;
            }
            const auto byQuest = _rulesByExactDependency.find(
                ToMask(RuleDependency::kQuest));
            if (byQuest == _rulesByExactDependency.end()) {
                return false;
            }
            const auto rules = byQuest->second.find(a_formID);
            if (rules == byQuest->second.end()) {
                return false;
            }
            return std::ranges::any_of(
                rules->second,
                [&](const std::string& ruleID) {
                    const auto* rule = FindRule(ruleID);
                    if (!rule) return false;
                    const auto matches = [&](const BlacklistFilter& filter) {
                        return filter.type == "Quest" &&
                            ResolveEDFFormID(
                                filter.type,
                                filter.editorID,
                                filter.formIDStr) == a_formID &&
                            MatchesQuestFilter(a_actor, quest, filter);
                    };
                    return std::ranges::any_of(
                               rule->targetFilters, matches) ||
                        std::ranges::any_of(
                               rule->blacklistFilters, matches);
                });
        });

    return { candidates.begin(), candidates.end() };
}

RuleDependencyMask RuleManager::GetRuleDependencyMask(
    const std::string_view a_ruleID) const
{
    const auto found = _ruleDependencyMasks.find(std::string(a_ruleID));
    return found != _ruleDependencyMasks.end()
        ? found->second
        : ToMask(RuleDependency::kAll);
}

bool RuleManager::IsRuleInUnstableCycle(
    const std::string_view a_ruleID) const
{
    return _unstableCycleRules.contains(std::string(a_ruleID));
}

RuleEvaluationDelta RuleManager::DetectBaseNPCChanges(RE::Actor* a_actor)
{
    RuleEvaluationDelta delta;
    delta.mask = ToMask(RuleDependency::kNone);
    if (!a_actor) {
        return delta;
    }

    auto npc = a_actor->GetActorBase();
    if (!npc) {
        return delta;
    }

    std::uint64_t fingerprint = 1469598103934665603ULL;
    const auto mix = [&fingerprint](std::uint64_t a_value) {
        fingerprint ^= a_value;
        fingerprint *= 1099511628211ULL;
    };
    const auto mixForm = [&mix](const RE::TESForm* a_form) {
        mix(a_form ? a_form->GetFormID() : 0);
    };

    mix(npc->IsFemale() ? 1u : 0u);
    mix(npc->IsUnique() ? 1u : 0u);
    mix(a_actor->IsEssential() ? 1u : 0u);
    mix(a_actor->IsProtected() ? 1u : 0u);
    mixForm(npc->race);
    mixForm(npc->npcClass);
    mixForm(npc->voiceType);
    mixForm(npc->combatStyle);
    mixForm(npc->skin);
    mixForm(npc->baseTemplateForm);

    for (auto* package : npc->aiPackages.packages) {
        mixForm(package);
    }
    if (npc->headParts && npc->numHeadParts > 0) {
        for (std::int8_t index = 0; index < npc->numHeadParts; ++index) {
            mixForm(npc->headParts[index]);
        }
    }
    npc->ForEachKeyword([&](const RE::BGSKeyword* a_keyword) {
        mixForm(a_keyword);
        return RE::BSContainer::ForEachResult::kContinue;
    });
    if (npc->race) {
        npc->race->ForEachKeyword([&](const RE::BGSKeyword* a_keyword) {
            mixForm(a_keyword);
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }

    const auto baseID = npc->GetFormID();
    const auto current = std::pair{ _dependencyRevision, fingerprint };
    const auto found = _baseNPCFingerprints.find(baseID);
    if (found == _baseNPCFingerprints.end()) {
        _baseNPCFingerprints.emplace(baseID, current);
        delta.mask = ToMask(RuleDependency::kStatic) |
            ToMask(RuleDependency::kTag);
        return delta;
    }
    if (found->second == current) {
        return delta;
    }

    found->second = current;
    delta.mask = ToMask(RuleDependency::kStatic) |
        ToMask(RuleDependency::kTag);
    logger::info(
        "[RuleSnapshot] Base NPC '{}' ({:08X}) changed; static/tag candidates invalidated.",
        npc->GetName(), baseID);
    return delta;
}

RuleEvaluationDelta RuleManager::DetectActorValueChanges(RE::Actor* a_actor)
{
    RuleEvaluationDelta delta;
    delta.mask = ToMask(RuleDependency::kNone);
    if (!a_actor || _watchedActorValues.empty()) {
        return delta;
    }

    std::map<std::pair<RE::ActorValue, ActorValueMode>, float> current;
    for (const auto& [actorValue, mode] : _watchedActorValues) {
        if (const auto value =
                ReadActorValue(a_actor, actorValue, mode);
            value && std::isfinite(*value)) {
            current.emplace(
                std::pair{ actorValue, mode }, *value);
        }
    }

    const auto actorID = a_actor->GetFormID();
    const auto found = _actorValueSnapshots.find(actorID);
    if (found == _actorValueSnapshots.end() ||
        found->second.first != _dependencyRevision) {
        for (const auto& [key, value] : current) {
            delta.changedActorValues.insert(key.first);
        }
        _actorValueSnapshots[actorID] = {
            _dependencyRevision, std::move(current)
        };
    }
    else {
        auto& previous = found->second.second;
        for (const auto& [key, value] : current) {
            const auto old = previous.find(key);
            if (old == previous.end() ||
                std::abs(old->second - value) >
                    std::max(0.001f, std::abs(value) * 0.00001f)) {
                delta.changedActorValues.insert(key.first);
            }
        }
        for (const auto& [key, value] : previous) {
            if (!current.contains(key)) {
                delta.changedActorValues.insert(key.first);
            }
        }
        previous = std::move(current);
    }

    if (!delta.changedActorValues.empty()) {
        delta.mask = ToMask(RuleDependency::kActorValue);
        logger::debug(
            "[RuleSnapshot] Actor '{}' ({:08X}) changed {} watched Actor Value(s).",
            a_actor->GetName(),
            actorID,
            delta.changedActorValues.size());
    }
    return delta;
}

RuleEvaluationDelta RuleManager::DetectFollowerStateChanges(
    RE::Actor* a_actor)
{
    RuleEvaluationDelta delta;
    delta.mask = ToMask(RuleDependency::kNone);
    if (!a_actor || !_hasFollowerDependentRules) {
        return delta;
    }

    const auto actorID = a_actor->GetFormID();
    const auto isFollower = IsActivePlayerFollower(a_actor);
    const auto current =
        std::pair{ _dependencyRevision, isFollower };
    const auto found = _followerStateSnapshots.find(actorID);
    if (found != _followerStateSnapshots.end() &&
        found->second == current) {
        return delta;
    }

    const auto hadPreviousState =
        found != _followerStateSnapshots.end() &&
        found->second.first == _dependencyRevision;
    const auto previousState =
        hadPreviousState ? found->second.second : false;
    _followerStateSnapshots[actorID] = current;
    delta.mask = ToMask(RuleDependency::kFollower);
    if (hadPreviousState) {
        logger::info(
            "[FollowerState] Actor '{}' ({:08X}) follower state {} -> {}.",
            a_actor->GetName(),
            actorID,
            previousState,
            isFollower);
    }
    else {
        logger::debug(
            "[FollowerState] Actor '{}' ({:08X}) initial follower state: {}.",
            a_actor->GetName(),
            actorID,
            isFollower);
    }
    return delta;
}

void RuleManager::ResetRuntimeCaches()
{
    _baseNPCFingerprints.clear();
    _actorValueSnapshots.clear();
    _followerStateSnapshots.clear();
}

void RuleManager::InvalidateBaseNPCState(const RE::FormID a_npcFormID)
{
    if (a_npcFormID == 0) {
        return;
    }

    _baseNPCFingerprints.erase(a_npcFormID);
    _affectedNPCsDatabaseValid = false;
    logger::debug(
        "[RuleSnapshot] Invalidated Base NPC {:08X} and affected-NPC preview.",
        a_npcFormID);
}

void RuleManager::ForgetActorRuntimeState(const RE::FormID a_actorID)
{
    _actorValueSnapshots.erase(a_actorID);
    _followerStateSnapshots.erase(a_actorID);
}

const std::map<RE::FormID, AffectedNPC>&
RuleManager::GetAffectedNPCsDatabase()
{
    if (_affectedNPCsDatabaseValid) {
        return _affectedNPCsDatabase;
    }

    _affectedNPCsDatabase.clear();
    const auto& npcList = Manager::GetSingleton()->GetList("NPC");
    for (const auto& npcInfo : npcList) {
        auto npc = RE::TESForm::LookupByID<RE::TESNPC>(npcInfo.formID);
        if (!npc) {
            continue;
        }

        AffectedNPC affectedInfo;
        for (const auto& rule : _rules) {
            if (!rule.isEnabled || HasActorDependentFilters(rule)) {
                continue;
            }
            if (IsNPCMatchingTargets(npc, rule, false) &&
                !IsNPCMatchingTargets(npc, rule, true)) {
                affectedInfo.ruleIDs.push_back(rule.id);
            }
        }
        if (!affectedInfo.ruleIDs.empty()) {
            affectedInfo.npcFormID = npcInfo.formID;
            affectedInfo.npcName = npcInfo.name;
            _affectedNPCsDatabase.emplace(
                npcInfo.formID, std::move(affectedInfo));
        }
    }
    _affectedNPCsDatabaseValid = true;
    logger::info(
        "[RuleIndex] UI affected-NPC preview built lazily with {} entries.",
        _affectedNPCsDatabase.size());
    return _affectedNPCsDatabase;
}

void RuleManager::InitializeAffectedNPCsDatabase() {
    _affectedNPCsDatabase.clear();
    _affectedNPCsDatabaseValid = false;
    auto& rules = GetRules();
    RebuildDependencyIndex();
    _hasActorDependentRules = std::ranges::any_of(rules, [](const Rule& rule) {
        return rule.isEnabled && HasActorDependentFilters(rule);
    });
    logger::info(
        "[RuleIndex] Runtime dependency index ready; affected-NPC preview deferred until UI access.");
}
