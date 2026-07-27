#include "UI.h"
#include <algorithm>
#include <cctype>
#include <miniz.h>
#include <rapidjson/document.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>
namespace SPIDUI {
    const std::string LANG_PATH = "Data/Viny Mods/ProgressNPC/Language.json";
    inline std::unordered_map<std::string, std::string> locMap;

    void LoadLanguage() {
        locMap.clear();

        std::ifstream file(LANG_PATH, std::ios::binary);
        if (!file.is_open()) {
            logger::warn("[ProgressNPC] Language.json not found. Using default texts.");
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        rapidjson::Document doc;
        doc.Parse(buffer.str().c_str());
        if (doc.HasParseError() || !doc.IsObject()) {
            logger::warn("[ProgressNPC] Invalid Language.json. Using default texts.");
            return;
        }

        for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
            if (it->value.IsString()) {
                locMap[it->name.GetString()] = it->value.GetString();
                continue;
            }

            if (!it->value.IsObject()) {
                continue;
            }

            const std::string category = it->name.GetString();
            for (auto sub = it->value.MemberBegin(); sub != it->value.MemberEnd(); ++sub) {
                if (sub->value.IsString()) {
                    locMap[category + "." + sub->name.GetString()] = sub->value.GetString();
                }
            }
        }
    }

    const char* GetLoc(const std::string& key, const char* fallback) {
        const auto it = locMap.find(key);
        return it != locMap.end() ? it->second.c_str() : fallback;
    }

    static std::set<std::string> activeTypeFilters;
    static std::string activePackageID = "edf.local-rules";
    static std::string packageFilterID;
    static char newPackageName[128] = "";

    static std::string activeRuleID = "";      // ID da regra sendo editada no momento
    static int activeGroupIdx = -1;            // Índice do grupo de recompensa ativo
    static bool isPickingReward = false;
    //static RewardGroup* currentTargetGroup = nullptr;
    static std::string currentRewardType = "All";

    // Variáveis para controlar abertura dos modals fora do loop
    static bool openTargetsModal = false;
    static bool openRewardsModal = false;

    inline std::vector<size_t> cachedFilteredIndices;
    static bool needsNPCListUpdate = true;
    static std::string lastNPCSearch = "";
    static bool lastShowOnlyAffected = false;
    static size_t lastRulesCount = 0;

    static bool openBlacklistModal = false;
    static bool isPickingBlacklist = false;
    static std::string currentBlacklistType = "All";

    static char selectionSearchBuf[128] = "";
    static std::string selectionPluginFilter = "All";

    // Função auxiliar para limpar o estado da busca
    void ResetSelectionState() {
        memset(selectionSearchBuf, 0, sizeof(selectionSearchBuf));
        selectionPluginFilter = "All";
        currentRewardType = "All";
        currentBlacklistType = "All";
    }
    bool previewnpc = false;
    static std::vector<const InternalFormInfo*> affectedCache;
    static std::string lastPreviewID = "";
    static int FilterFileNameChars(ImGuiMCP::ImGuiInputTextCallbackData* data) {
        // Lista de caracteres proibidos: < > : " / \ | ? *
        if (strchr("<>:\"/\\|?*", (char)data->EventChar)) {
            return 1; // Descarta o caractere
        }
        return 0; // Aceita o caractere
    };

    struct NPCMatchInfo {
        bool isAffected = false;
        std::string ruleNames; // Cache das strings das regras para exibição rápida
    };

    static std::vector<NPCMatchInfo> g_npcMatchCache; // Cache pesado (Keywords/Factions)
    static bool g_needsFullCacheUpdate = true;      // Controla se as regras mudaram

    struct ResolvedRuleTarget {
        std::string type;
        std::string npcID; // Para tipo NPC
        RE::BGSKeyword* kwd = nullptr;
        RE::TESFaction* fac = nullptr;
    };

    struct CompiledRule {
        std::string displayName;
        bool isGlobal;
        std::vector<ResolvedRuleTarget> targets;
    };
    static std::vector<CompiledRule> g_compiledRules;

    Rule* GetActiveRule() {
        if (activeRuleID.empty()) return nullptr;
        auto& rules = RuleManager::GetSingleton()->GetRules();
        for (auto& r : rules) {
            if (r.id == activeRuleID) return &r;
        }
        return nullptr;
    }

    const RulePackage* FindPackage(const std::string_view packageID)
    {
        const auto& packages = RuleManager::GetSingleton()->GetPackages();
        const auto found = std::ranges::find_if(packages, [packageID](const RulePackage& package) {
            return package.id == packageID;
        });
        return found == packages.end() ? nullptr : &*found;
    }

    void RenderPackageWorkspace()
    {
        auto manager = RuleManager::GetSingleton();
        const auto& packages = manager->GetPackages();
        if (packages.empty()) {
            return;
        }
        if (!FindPackage(activePackageID)) {
            activePackageID = packages.front().id;
        }

        ImGuiMCP::TextUnformatted(GetLoc("auto.package_workspace", "Package Workspace"));
        const auto* active = FindPackage(activePackageID);
        ImGuiMCP::SetNextItemWidth(260.0f);
        if (ImGuiMCP::BeginCombo(
                GetLoc("auto.active_package", "Active Package"),
                active ? active->displayName.c_str() : "Local Rules")) {
            for (const auto& package : packages) {
                const bool selected = package.id == activePackageID;
                if (ImGuiMCP::Selectable(package.displayName.c_str(), selected)) {
                    activePackageID = package.id;
                }
                if (selected) {
                    ImGuiMCP::SetItemDefaultFocus();
                }
            }
            ImGuiMCP::EndCombo();
        }

        ImGuiMCP::SameLine();
        ImGuiMCP::SetNextItemWidth(220.0f);
        ImGuiMCP::InputText(
            GetLoc("auto.new_package", "New Package"),
            newPackageName,
            sizeof(newPackageName),
            ImGuiMCP::ImGuiInputTextFlags_CallbackCharFilter,
            FilterFileNameChars);
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(GetLoc("auto.create_package", "Create Package")) && newPackageName[0] != '\0') {
            if (const auto packageID = manager->CreatePackage(newPackageName)) {
                activePackageID = *packageID;
                newPackageName[0] = '\0';
            }
        }

        const char* filterLabel = GetLoc("auto.all_packages", "All Packages");
        if (!packageFilterID.empty()) {
            if (const auto* package = FindPackage(packageFilterID)) {
                filterLabel = package->displayName.c_str();
            }
        }
        ImGuiMCP::SetNextItemWidth(260.0f);
        if (ImGuiMCP::BeginCombo(GetLoc("auto.package_filter", "Package Filter"), filterLabel)) {
            const bool allSelected = packageFilterID.empty();
            if (ImGuiMCP::Selectable(GetLoc("auto.all_packages", "All Packages"), allSelected)) {
                packageFilterID.clear();
            }
            for (const auto& package : packages) {
                const bool selected = package.id == packageFilterID;
                if (ImGuiMCP::Selectable(package.displayName.c_str(), selected)) {
                    packageFilterID = package.id;
                }
            }
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::Separator();
    }

    bool FilterUsesNumericValue(const std::string& type)
    {
        return type == "Inventory Count" || type == "Gold" || type == "Faction Rank";
    }

    int DefaultFilterNumericValue(const std::string& type)
    {
        return type == "Faction Rank" ? 0 : 1;
    }

    std::string GetFilterBaseFormID(const std::string& formIDStr)
    {
        auto tokens = split(formIDStr, '|');
        if (tokens.size() < 2) return formIDStr;
        return tokens[0] + "|" + tokens[1];
    }

    std::string GetInternalFormID(const InternalFormInfo& info)
    {
        return info.pluginName + "|" + FormatLocalFormID(info.formID, info.pluginName);
    }

    bool EditorIDMatches(const std::string& lhs, const std::string& rhs)
    {
        if (lhs.empty() || rhs.empty() || lhs.size() != rhs.size()) return false;
        return std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
            });
    }

    int GetFilterNumericValue(const BlacklistFilter& filter)
    {
        auto tokens = split(filter.formIDStr, '|');
        if (tokens.size() < 3) return DefaultFilterNumericValue(filter.type);
        try {
            return std::stoi(tokens[2]);
        }
        catch (...) {
            return DefaultFilterNumericValue(filter.type);
        }
    }

    void SetFilterNumericValue(BlacklistFilter& filter, int value)
    {
        if (filter.type != "Faction Rank") value = std::max(0, value);
        auto baseID = GetFilterBaseFormID(filter.formIDStr);
        filter.formIDStr = baseID + "|" + std::to_string(value);
    }

    void RenderTypeFilter() {
        const std::vector<std::string> options = {
        "NPC", "Faction", "Faction Rank", "Keyword", "Race", "Package",
        "Spell", "Perk", "Shout", "Combat Style", "Voice Type", "Class", "Location", "Cell", "Skin", "Inventory Item",
        "Inventory Count", "Gold", "Equipped Item",
        "Hair", "Facial Hair", "HeadPart Misc", "HeadPart Face",
        "HeadPart Eyes", "HeadPart Scar", "HeadPart Eyebrows", "Leveled NPC"
        };

        ImGuiMCP::Text(GetLoc("auto.active_filters", "Active Filters:"));
        if (activeTypeFilters.empty()) ImGuiMCP::TextDisabled(GetLoc("auto.none_showing_all", "None (Showing all)"));

        // Mostra "chips" dos filtros ativos
        for (auto it = activeTypeFilters.begin(); it != activeTypeFilters.end(); ) {
            if (ImGuiMCP::Button((*it + GetLoc("auto.remove_suffix", " x")).c_str())) {
                it = activeTypeFilters.erase(it);
            }
            else {
                ImGuiMCP::SameLine();
                ++it;
            }
        }
        ImGuiMCP::NewLine();

        if (ImGuiMCP::BeginCombo(GetLoc("auto.add_type_filter", "Add Type Filter"), GetLoc("auto.select", "Select..."))) {
            for (const auto& opt : options) {
                if (ImGuiMCP::Selectable(opt.c_str(), activeTypeFilters.contains(opt))) {
                    activeTypeFilters.insert(opt);
                }
            }
            ImGuiMCP::EndCombo();
        }
    }

    void RenderFilterEditor(Rule& rule, bool isBlacklist) {
        if (ImGuiMCP::Button(GetLoc("auto.back", "Back"))) {
            if (isBlacklist) openBlacklistModal = false;
            else openTargetsModal = false;
        }
        ImGuiMCP::SameLine();
        // 1. Resolvemos as referências dos dados com base no modo
        auto& filters = isBlacklist ? rule.blacklistFilters : rule.targetFilters;
        auto& gender = isBlacklist ? rule.blacklistedGender : rule.targetGender;
        auto& humanoid = isBlacklist ? rule.blacklistedHumanoid : rule.targetHumanoid;
        auto& child = isBlacklist ? rule.blacklistedChild : rule.targetChild;
        auto& requiresAll = isBlacklist ? rule.blacklistRequiresAll : rule.targetRequiresAll;

        // 2. Textos dinâmicos para a UI
        const char* genderLabel = isBlacklist ? "Excluded Gender:" : "Target Gender:";
        const char* genderNoneOption = isBlacklist ? "None" : "All";
        const char* checkLabel = isBlacklist ? "Require ALL filters to invalidate (AND)" : "Require ALL filters (AND)";
        const char* tooltip = isBlacklist ? GetLoc("auto.any_match_invalidates", "(?) If unchecked, any match invalidates") : GetLoc("auto.any_match_validates", "(?) If unchecked, any match validates (OR)");
        const char* buttonLabel = isBlacklist ? GetLoc("auto.add_new_filter", "Add New Filter") : GetLoc("auto.add_new_target_filter", "Add New Target Filter");
        const char* tableName = isBlacklist ? "BlacklistTable" : "TargetsTable";
        const std::string idPrefix = isBlacklist ? "X##bl" : "X##target";

        // --- Renderização da Interface ---
        ImGuiMCP::Text(genderLabel);
        ImGuiMCP::SameLine();
        const char* genders[] = { genderNoneOption, GetLoc("auto.male", "Male"), GetLoc("auto.female", "Female") };
        gender = std::clamp(gender, 0, 2);
        humanoid = std::clamp(humanoid, 0, 2);
        child = std::clamp(child, 0, 2);
        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::BeginCombo(isBlacklist ? "##gender" : "##targetGender", genders[gender])) {
            for (int i = 0; i < 3; i++) {
                if (ImGuiMCP::Selectable(genders[i], gender == i)) gender = i;
            }
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::Text("%s", isBlacklist ? GetLoc("auto.excluded_body", "Excluded Body:") : GetLoc("auto.target_body", "Target Body:"));
        ImGuiMCP::SameLine();
        const char* humanoidOptions[] = { GetLoc("auto.both", "Both"), GetLoc("auto.only_humanoids", "Only Humanoids"), GetLoc("auto.only_non_humanoids", "Only Non-Humanoids") };
        ImGuiMCP::SetNextItemWidth(180.0f);
        if (ImGuiMCP::BeginCombo(isBlacklist ? "##blacklistHumanoid" : "##targetHumanoid", humanoidOptions[humanoid])) {
            for (int i = 0; i < 3; i++) {
                if (ImGuiMCP::Selectable(humanoidOptions[i], humanoid == i)) humanoid = i;
            }
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::Text("%s", isBlacklist ? GetLoc("auto.excluded_age", "Excluded Age:") : GetLoc("auto.target_age", "Target Age:"));
        ImGuiMCP::SameLine();
        const char* childOptions[] = { GetLoc("auto.both", "Both"), GetLoc("auto.only_children", "Only Children"), GetLoc("auto.only_non_children", "Only Non-Children") };
        ImGuiMCP::SetNextItemWidth(180.0f);
        if (ImGuiMCP::BeginCombo(isBlacklist ? "##blacklistChild" : "##targetChild", childOptions[child])) {
            for (int i = 0; i < 3; i++) {
                if (ImGuiMCP::Selectable(childOptions[i], child == i)) child = i;
            }
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::Checkbox(checkLabel, &requiresAll);
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip(tooltip);
        }

        ImGuiMCP::Separator();
        if (ImGuiMCP::Button(buttonLabel)) {
            ResetSelectionState();
            isPickingBlacklist = true;
        }

        if (ImGuiMCP::BeginTable(tableName, 4, ImGuiMCP::ImGuiTableFlags_Borders)) {
            ImGuiMCP::TableSetupColumn(GetLoc("auto.type", "Type"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.name", "Name"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.identifier", "Identifier"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.action", "Action"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGuiMCP::TableHeadersRow();
            auto dataHandler = RE::TESDataHandler::GetSingleton();
            for (int i = 0; i < filters.size(); i++) {
                auto& f = filters[i];
                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0); ImGuiMCP::Text(f.type.c_str());
                ImGuiMCP::TableSetColumnIndex(1);
                std::string resolvedName = "Not Found";

                // Puxamos apenas o necessário: Split da string e busca direta no Form
                auto tokens = split(f.formIDStr, '|'); // Função auxiliar definida em Rule.h
                if (tokens.size() == 2 && dataHandler) {
                    try {
                        uint32_t localID = std::stoul(tokens[1], nullptr, 16);
                        if (auto actualFormID = dataHandler->LookupFormID(localID, tokens[0])) {
                            if (auto form = RE::TESForm::LookupByID(actualFormID)) {
                                // 1. Tenta nome completo (NPC, Faction, Race)
                                if (auto fullName = form->As<RE::TESFullName>()) {
                                    resolvedName = Manager::ToUTF8(fullName->GetFullName());
                                }
                                // 2. Fallback para EditorID (Keywords ou nomes vazios)
                                if (resolvedName.empty() || resolvedName == "Not Found") {
                                    resolvedName = Manager::ToUTF8(clib_util::editorID::get_editorID(form));
                                }
                                // 3. Se nada funcionar, exibe o ID local
                                if (resolvedName.empty()) resolvedName = tokens[1];
                            }
                        }
                    }
                    catch (...) { resolvedName = "Invalid ID"; }
                }
                ImGuiMCP::TextUnformatted(resolvedName.c_str());
                ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text(f.formIDStr.c_str());
                ImGuiMCP::TableSetColumnIndex(3);

                // Usamos o prefixo para evitar conflitos de ID no ImGui
                if (ImGuiMCP::Button((idPrefix + std::to_string(i)).c_str())) {
                    filters.erase(filters.begin() + i);
                    break;
                }
            }
            ImGuiMCP::EndTable();
        }
    }

    bool IsIDSelected(const std::vector<std::string>& list, const std::string& id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    }

    // Substitua sua função DrawSelectionTable em UI.cpp por esta:
    void DrawSelectionTable(Rule& rule, std::string& listType, bool isRewardMode,
        RewardGroup* targetGroup = nullptr,
        std::vector<BlacklistFilter>* blacklistTarget = nullptr) {


        static std::vector<size_t> rewardFilteredIndices;
        static std::vector<size_t> blacklistFilteredIndices;

        // --- SISTEMA DE CACHE PARA O MODO "SELECTED" ---
        static std::vector<InternalFormInfo> rewardAllCache;
        static std::vector<InternalFormInfo> filterAllCache;

        static std::string lastSearch = "";
        static std::string lastPluginFilter = "";
        static std::string lastListType = "";
        static void* lastTargetPtr = nullptr;
        static size_t lastTargetSize = 0;
        static bool needsRebuildFiltered = true;

        std::vector<size_t>& currentCache = isRewardMode ? rewardFilteredIndices : blacklistFilteredIndices;



        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::InputText(GetLoc("auto.search", "Search"), selectionSearchBuf, sizeof(selectionSearchBuf))) needsRebuildFiltered = true;

        ImGuiMCP::SameLine();
        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::BeginCombo("##FilterType", listType.c_str())) {
            std::vector<const char*> options = isRewardMode ?
                std::vector<const char*>{ "All", "Selected", "Perk", "Spell", "Shout", "Keyword", "Faction", "Weapon", "Armor", "Potion", "Ingredient", "Scroll", "Book", "Ammo", "Misc", "SoulGem", "Key", "Outfit" } :
                std::vector<const char*>{
                "All", "Selected", "NPC", "Faction", "Faction Rank", "Keyword", "Race",
                "Spell", "Perk", "Shout", "Package", "Combat Style", "Voice Type", "Class", "Location", "Cell", "Skin", "Inventory Item",
                "Inventory Count", "Gold", "Equipped Item",
                "Hair", "Facial Hair", "HeadPart Misc", "HeadPart Face",
                "HeadPart Eyes", "HeadPart Scar", "HeadPart Eyebrows", "Leveled NPC"
            };

            for (auto opt : options) {
                if (ImGuiMCP::Selectable(opt, listType == opt)) {
                    listType = opt;
                    needsRebuildFiltered = true;
                }
            }
            ImGuiMCP::EndCombo();
        }

        const std::vector<InternalFormInfo>* sourceList = nullptr;
        bool changed = false;
        if (listType == "All") {
            if (isRewardMode) {
                if (rewardAllCache.empty()) {
                    for (auto& type : { "Spell", "Shout", "Perk", "Keyword", "Faction", "Weapon", "Armor", "Potion", "Ingredient", "Scroll", "Book", "Ammo", "Misc", "SoulGem", "Key", "Outfit" }) {
                        auto& l = Manager::GetSingleton()->GetList(type);
                        rewardAllCache.insert(rewardAllCache.end(), l.begin(), l.end());
                    }
                }
                sourceList = &rewardAllCache;
            }
            else {
                if (filterAllCache.empty()) {
                    for (auto& type : {
                        "NPC", "Faction", "Faction Rank", "Keyword", "Race",
                        "Spell", "Perk", "Shout", "Combat Style", "Voice Type", "Class", "Location", "Cell", "Skin", "Inventory Item",
                        "Inventory Count", "Gold", "Equipped Item",
                        "Hair", "Facial Hair", "HeadPart Misc", "HeadPart Face",
                        "HeadPart Eyes", "HeadPart Scar", "HeadPart Eyebrows", "Leveled NPC"
                        }) {
                        auto& l = Manager::GetSingleton()->GetList(type);
                        filterAllCache.insert(filterAllCache.end(), l.begin(), l.end());
                    }
                }
                sourceList = &filterAllCache;
            }
        }
        else if (listType == "Selected") {
            // Lógica de cache para itens selecionados (já existente, mas otimizada)
            static std::vector<InternalFormInfo> selectedCache;
            if (lastListType != listType) changed = true;
            if (isRewardMode && targetGroup) {
                if (targetGroup != lastTargetPtr || targetGroup->rewards.size() != lastTargetSize) changed = true;
            }
            else if (!isRewardMode && blacklistTarget) {
                if (blacklistTarget != lastTargetPtr || blacklistTarget->size() != lastTargetSize) changed = true;
            }

            if (changed) {
                selectedCache.clear();
                if (isRewardMode && targetGroup) {
                    for (const auto& reward : targetGroup->rewards) {
                        const auto& fullList = Manager::GetSingleton()->GetList(reward.typeReward);
                        auto it = std::find_if(fullList.begin(), fullList.end(), [&](const InternalFormInfo& info) {
                            return EditorIDMatches(info.editorID, reward.editorID) || GetInternalFormID(info) == reward.formIDStr;
                            });
                        if (it != fullList.end()) selectedCache.push_back(*it);
                    }
                }
                else if (!isRewardMode && blacklistTarget) {
                    for (const auto& filter : *blacklistTarget) {
                        auto baseFormID = GetFilterBaseFormID(filter.formIDStr);
                        const auto& fullList = Manager::GetSingleton()->GetList(filter.type);
                        auto it = std::find_if(fullList.begin(), fullList.end(), [&](const InternalFormInfo& info) {
                            return EditorIDMatches(info.editorID, filter.editorID) || GetInternalFormID(info) == baseFormID;
                            });
                        if (it != fullList.end()) selectedCache.push_back(*it);
                    }
                }
                lastTargetPtr = isRewardMode ? (void*)targetGroup : (void*)blacklistTarget;
                lastTargetSize = isRewardMode ? (targetGroup ? targetGroup->rewards.size() : 0) : (blacklistTarget ? blacklistTarget->size() : 0);
                needsRebuildFiltered = true;
            }
            sourceList = &selectedCache;
        }
        else {
            sourceList = &Manager::GetSingleton()->GetList(listType);
        }


        if (!sourceList || sourceList->empty()) {
            ImGuiMCP::TextDisabled(GetLoc("auto.no_items_found_in_this_category", "No items found in this category."));
            return;
        }


        static std::string pluginFilter = "All";

        ImGuiMCP::SameLine();
        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::BeginCombo("##Plugin", selectionPluginFilter.c_str())) {
            if (ImGuiMCP::Selectable(GetLoc("auto.all_plugins", "All Plugins"), selectionPluginFilter == "All")) { selectionPluginFilter = "All"; needsRebuildFiltered = true; }
            std::set<std::string> plugins;
            for (const auto& item : *sourceList) if (!item.pluginName.empty()) plugins.insert(item.pluginName);
            for (const auto& p : plugins) {
                if (ImGuiMCP::Selectable(p.c_str(), selectionPluginFilter == p)) {
                    selectionPluginFilter = p;
                    needsRebuildFiltered = true;
                }
            }
            ImGuiMCP::EndCombo();
        }
        if (lastListType != listType) {
            changed = true;
        }
        // 3. PROCESSAMENTO DO FILTRO (Apenas se necessário)
        if (needsRebuildFiltered || lastListType != listType) {
            currentCache.clear();

            std::string searchStr = selectionSearchBuf;
            std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

            for (size_t i = 0; i < sourceList->size(); i++) {
                const auto& item = (*sourceList)[i];
                if (selectionPluginFilter != "All" && item.pluginName != selectionPluginFilter) continue;

                if (!searchStr.empty()) {
                    std::string n = item.name; std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                    std::string e = item.editorID; std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                    if (n.find(searchStr) == std::string::npos && e.find(searchStr) == std::string::npos) continue;
                }
                currentCache.push_back(i);
            }
            lastListType = listType;
            needsRebuildFiltered = false;
            logger::info("[UI] Filtro Rebuilt: {} itens para tipo {}", currentCache.size(), listType);
        }


        ImGuiMCP::ImVec2 avail;
        ImGuiMCP::GetContentRegionAvail(&avail);

        // No ImGuiMCP, usamos GetScrollY e o tamanho da região visível para definir o range

        float tableHeight = avail.y; // Mesma altura definida no BeginTable
        // 4. TABELA COM IMGUILISTCLIPPER
        bool showTypeColumn = (listType == "All" || listType == "Selected");
        int columns = 5; // Active, FormID, Name, Plugin
        //if (showTypeColumn) columns += 1; // Type
        if (isRewardMode)   columns += 4; // Qty, Chance, Sleep
        else columns += 1; // Numeric filter value

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
            ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

        if (ImGuiMCP::BeginTable("SelectionTable", columns, tableFlags, { 0, tableHeight })) {
            ImGuiMCP::TableSetupColumn(GetLoc("auto.active", "Active"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.formid", "FormID"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.name", "Name"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.type", "Type"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.plugin", "Plugin"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            if (isRewardMode) {
                ImGuiMCP::TableSetupColumn(GetLoc("auto.qty", "Qty"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGuiMCP::TableSetupColumn(GetLoc("auto.chance", "Chance"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGuiMCP::TableSetupColumn(GetLoc("auto.persist", "Persist"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGuiMCP::TableSetupColumn(GetLoc("auto.mode", "Mode"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 130.0f);
            }
            else {
                ImGuiMCP::TableSetupColumn(GetLoc("auto.value", "Value"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 90.0f);
            }
            ImGuiMCP::TableHeadersRow();
            int totalItems = static_cast<int>(currentCache.size());
            //logger::debug("valor de total itens: {}", totalItems);
            static auto clipper = ImGuiMCP::ImGuiListClipperManager::Create();
            ImGuiMCP::ImGuiListClipperManager::Begin(clipper, (int)currentCache.size(), -1.0f);


            while (ImGuiMCP::ImGuiListClipperManager::Step(clipper)) {
                for (int i = clipper->DisplayStart; i < clipper->DisplayEnd; i++) {
                    // Segurança contra índices inválidos
                    if (i < 0 || i >= totalItems) {
                        continue;
                    }

                    size_t sourceIdx = currentCache[i];

                    // Outra trava: verificar se o índice do sourceIdx ainda é válido no sourceList
                    if (sourceIdx >= sourceList->size()) {
                        continue;
                    }
                    const auto& item = (*sourceList)[sourceIdx];
                    std::string internalID = GetInternalFormID(item);

                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0);

                    // Checkbox e Lógica de Seleção
                    if (blacklistTarget) {
                        auto it = std::find_if(blacklistTarget->begin(), blacklistTarget->end(), [&](const BlacklistFilter& f) {
                            return f.type == item.formType &&
                                (EditorIDMatches(f.editorID, item.editorID) || GetFilterBaseFormID(f.formIDStr) == internalID);
                            });
                        bool selected = (it != blacklistTarget->end());
                        if (ImGuiMCP::Checkbox(("##" + internalID).c_str(), &selected)) {
                            if (selected) {
                                auto formIDStr = internalID;
                                if (FilterUsesNumericValue(item.formType)) {
                                    formIDStr += "|" + std::to_string(DefaultFilterNumericValue(item.formType));
                                }
                                blacklistTarget->push_back({ item.formType, formIDStr, item.editorID });
                            }
                            else if (it != blacklistTarget->end()) {
                                blacklistTarget->erase(it);
                                if (listType == "Selected") changed = true;
                            }
                        }
                    }
                    else if (targetGroup) {
                        auto it = std::find_if(targetGroup->rewards.begin(), targetGroup->rewards.end(), [&](const Reward& r) {
                            return r.typeReward == item.formType &&
                                (EditorIDMatches(r.editorID, item.editorID) || r.formIDStr == internalID);
                            });
                        bool selected = (it != targetGroup->rewards.end());
                        if (ImGuiMCP::Checkbox(("##" + internalID).c_str(), &selected)) {
                            if (selected) targetGroup->rewards.push_back({ item.formType, internalID, item.editorID, 1, 100.0f });
                            else { targetGroup->rewards.erase(it); if (listType == "Selected") changed = true; }
                        }
                    }

                    ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::Text("%08X", item.formID);
                    ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::TextUnformatted(item.GetDisplayName().c_str());
                    ImGuiMCP::TableSetColumnIndex(3); ImGuiMCP::TextUnformatted(item.formType.c_str()); // Renderiza sem condição
                    ImGuiMCP::TableSetColumnIndex(4); ImGuiMCP::TextUnformatted(item.pluginName.c_str());
                    if (!isRewardMode) {
                        ImGuiMCP::TableSetColumnIndex(5);
                        if (blacklistTarget && FilterUsesNumericValue(item.formType)) {
                            auto filterIt = std::find_if(blacklistTarget->begin(), blacklistTarget->end(), [&](const BlacklistFilter& f) {
                                return f.type == item.formType &&
                                    (EditorIDMatches(f.editorID, item.editorID) || GetFilterBaseFormID(f.formIDStr) == internalID);
                                });
                            if (filterIt != blacklistTarget->end()) {
                                int value = GetFilterNumericValue(*filterIt);
                                ImGuiMCP::SetNextItemWidth(-1.0f);
                                if (ImGuiMCP::InputInt(("##filterValue" + internalID + item.formType).c_str(), &value, 0, 0)) {
                                    SetFilterNumericValue(*filterIt, value);
                                }
                            }
                            else {
                                ImGuiMCP::TextDisabled("-");
                            }
                        }
                        else {
                            ImGuiMCP::TextDisabled("-");
                        }
                    }
                    int nextCol = 3; // Começamos a controlar o índice dinamicamente



                    if (isRewardMode && targetGroup) {
                        auto it = std::find_if(targetGroup->rewards.begin(), targetGroup->rewards.end(), [&](const Reward& r) {
                            return r.typeReward == item.formType &&
                                (EditorIDMatches(r.editorID, item.editorID) || r.formIDStr == internalID);
                            });

                        if (it != targetGroup->rewards.end()) {
                            ImGuiMCP::TableSetColumnIndex(5);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            int val = (int)it->amount;
                            if (ImGuiMCP::InputInt(("##q" + internalID).c_str(), &val, 0, 0)) it->amount = (uint32_t)val;

                            ImGuiMCP::TableSetColumnIndex(6);
                            ImGuiMCP::SetNextItemWidth(-1.0f);
                            ImGuiMCP::InputFloat(("##c" + internalID).c_str(), &it->chanceReward, 0, 0, "%.1f");

                            ImGuiMCP::TableSetColumnIndex(7); // Nova Coluna para Persist
                            ImGuiMCP::Checkbox(("##per" + internalID).c_str(), &it->isPersistent);
                            if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip(GetLoc("auto.if_checked_item_won_t_be_removed_when_rule_becomes_invalid", "If checked, item won't be removed when rule becomes invalid."));

                            ImGuiMCP::TableSetColumnIndex(8);
                            if (item.formType == "Outfit" || item.formType == "Spell") {
                                const char* modes[] = { GetLoc("auto.standard", "Standard"), GetLoc("auto.special", "Special"), GetLoc("auto.both", "Both") };
                                ImGuiMCP::SetNextItemWidth(-1.0f); // Aplicando sua dúvida anterior
                                if (ImGuiMCP::BeginCombo(("##mode" + internalID).c_str(), modes[it->functionOnType])) {
                                    for (int m = 0; m < 3; m++) {
                                        if (ImGuiMCP::Selectable(modes[m], it->functionOnType == m)) it->functionOnType = m;
                                    }
                                    ImGuiMCP::EndCombo();
                                }
                                if (ImGuiMCP::IsItemHovered()) {
                                    if (item.formType == "Outfit") ImGuiMCP::SetTooltip(GetLoc("auto.0_normal_1_sleep_beta_2_both_beta", "0: Normal, 1: Sleep(beta), 2: Both(beta)"));
                                    else ImGuiMCP::SetTooltip(GetLoc("auto.0_teach_1_cast_apply_2_both", "0: Teach, 1: Cast/Apply, 2: Both"));
                                }
                            }
                            else {
                                ImGuiMCP::TextDisabled("-");
                            }
                        }
                    }
                }
            }
            ImGuiMCP::EndTable();
        }
    }

    // --- NOVO: Gerenciador de Grupos de Recompensa ---
    void RenderRewardGroups(Rule& rule) {
        if (ImGuiMCP::Button(GetLoc("auto.back", "Back"))) openRewardsModal = false;
        ImGuiMCP::SameLine();

        if (ImGuiMCP::Button(GetLoc("auto.new_group", "+ New Group"))) {
            std::string baseName = "New Group";
            std::string finalName = baseName;
            int counter = 1;
            bool exists = true;

            // Loop para encontrar um nome que não esteja em uso na regra atual
            while (exists) {
                exists = false;
                for (const auto& g : rule.rewardGroups) {
                    if (g.name == finalName) {
                        finalName = baseName + " (" + std::to_string(counter++) + ")";
                        exists = true;
                        break;
                    }
                }
            }
            rule.rewardGroups.push_back({ finalName, false, 100.0f, {} });
        }
        if (rule.isExclusive) {
            float totalGroupsChance = 0.0f;
            for (const auto& g : rule.rewardGroups) totalGroupsChance += g.chanceGroup;

            ImGuiMCP::SameLine();
            if (totalGroupsChance > 100.0f) {
                ImGuiMCP::TextColored({ 1.0f, 0.0f, 0.0f, 1.0f }, GetLoc("auto.group_sum_exceeds_100", " [!] Group Sum: %.1f%% (Exceeds 100%%)"), totalGroupsChance);
            }
            else {
                ImGuiMCP::TextDisabled(GetLoc("auto.group_sum", " | Group Sum: %.1f%%"), totalGroupsChance);
            }
        }
        ImGuiMCP::Separator();

        for (size_t gIdx = 0; gIdx < rule.rewardGroups.size(); gIdx++) {
            auto& group = rule.rewardGroups[gIdx];
            ImGuiMCP::PushID(static_cast<int>(gIdx));

            // Título do CollapsingHeader com informações do grupo
            std::string headerLabel = group.name + " (" + std::to_string(group.rewards.size()) + " rewards)";
            if (group.isExclusive) headerLabel = "[EXCL] " + headerLabel;
            headerLabel += "###header_group_" + std::to_string(gIdx);

            if (ImGuiMCP::CollapsingHeader(headerLabel.c_str())) {
                ImGuiMCP::Indent();

                char nameBuf[64];
                strcpy_s(nameBuf, group.name.c_str());
                ImGuiMCP::SetNextItemWidth(200.0f);
                static std::string lastDuplicateAttempt = "";
                static size_t duplicateGroupIdx = -1;
                if (ImGuiMCP::InputText(GetLoc("auto.group_name", "Group Name"), nameBuf, sizeof(nameBuf))) {
                    std::string newName = nameBuf;
                    bool alreadyTaken = false;

                    // Verifica se algum OUTRO grupo já usa esse nome
                    for (size_t i = 0; i < rule.rewardGroups.size(); i++) {
                        if (i != gIdx && rule.rewardGroups[i].name == newName) {
                            alreadyTaken = true;
                            break;
                        }
                    }

                    if (!alreadyTaken && !newName.empty()) {
                        group.name = newName;
                        if (duplicateGroupIdx == gIdx) duplicateGroupIdx = -1; // Limpa o erro se corrigido
                    }
                    else if (alreadyTaken) {
                        lastDuplicateAttempt = newName;
                        duplicateGroupIdx = gIdx;
                    }
                }
                if (duplicateGroupIdx == gIdx) {
                    ImGuiMCP::SameLine();
                    ImGuiMCP::TextColored({ 1.0f, 0.2f, 0.2f, 1.0f }, GetLoc("auto.this_name_is_already_in_use", " [!] This name is already in use"));
                }

                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button(GetLoc("auto.delete_group", "Delete Group"))) {
                    rule.rewardGroups.erase(rule.rewardGroups.begin() + gIdx);
                    ImGuiMCP::Unindent();
                    ImGuiMCP::PopID();
                    continue;
                }
                ImGuiMCP::SetNextItemWidth(200.0f);
                if (ImGuiMCP::InputFloat(GetLoc("auto.activation_chance", "Activation Chance (%)"), &group.chanceGroup, 1.0f, 10.0f, "%.1f")) {
                    // Clamping para garantir que o valor fique entre 0 e 100
                    if (group.chanceGroup < 0.0f) group.chanceGroup = 0.0f;
                    if (group.chanceGroup > 100.0f) group.chanceGroup = 100.0f;
                }
                if (ImGuiMCP::Button(GetLoc("auto.manage_rewards", "Manage Rewards"))) {
                    ResetSelectionState();
                    isPickingReward = true;
                    activeGroupIdx = static_cast<int>(gIdx);
                }
                ImGuiMCP::SameLine();
                ImGuiMCP::Checkbox(GetLoc("auto.exclusive_picks_only_one_from_list", "Exclusive (Picks only one from list)"), &group.isExclusive);

                if (group.isExclusive) {
                    float total = 0;
                    for (const auto& r : group.rewards) total += r.chanceReward;
                    if (total > 100.0f) ImGuiMCP::TextColored({ 1,0,0,1 }, GetLoc("auto.warning_sum_of_chances_exceeds_100", "Warning: Sum of chances (%.1f%%) exceeds 100%%!"), total);
                    else ImGuiMCP::TextDisabled(GetLoc("auto.total_accumulated_chance", "Total accumulated chance: %.1f%%"), total);
                }



                ImGuiMCP::Spacing();
                ImGuiMCP::Text(GetLoc("auto.rewards_in_group", "Rewards in Group:"));

                // --- Tabela de Visualização de Rewards ---
                auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable;
                if (ImGuiMCP::BeginTable("GroupRewardsSummary", 7, tableFlags)) {
                    ImGuiMCP::TableSetupColumn(GetLoc("auto.type", "Type"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGuiMCP::TableSetupColumn(GetLoc("auto.reward", "Reward"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                    ImGuiMCP::TableSetupColumn(GetLoc("auto.qty", "Qty"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGuiMCP::TableSetupColumn(GetLoc("auto.chance", "Chance"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGuiMCP::TableSetupColumn(GetLoc("auto.persist", "Persist"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f); // Nova
                    ImGuiMCP::TableSetupColumn(GetLoc("auto.mode", "Mode"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGuiMCP::TableSetupColumn(GetLoc("auto.action", "Action"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGuiMCP::TableHeadersRow();

                    for (size_t rIdx = 0; rIdx < group.rewards.size(); ++rIdx) {
                        auto& r = group.rewards[rIdx];
                        ImGuiMCP::TableNextRow();

                        // Coluna 0: Tipo
                        ImGuiMCP::TableSetColumnIndex(0); ImGuiMCP::Text(r.typeReward.c_str());

                        // Coluna 1: Nome/ID
                        ImGuiMCP::TableSetColumnIndex(1);
                        auto [plugin, fID] = r.ParseFormID();
                        auto form = RE::TESForm::LookupByID(fID);
                        if (form) {
                            std::string dName = "";
                            if (auto fullName = form->As<RE::TESFullName>()) dName = fullName->GetFullName();
                            if (dName.empty()) dName = clib_util::editorID::get_editorID(form);
                            ImGuiMCP::Text(dName.empty() ? r.formIDStr.c_str() : dName.c_str());
                        }
                        else {
                            ImGuiMCP::TextDisabled(r.formIDStr.c_str());
                        }

                        // Coluna 2: Quantidade
                        ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text("%d", r.amount);

                        // Coluna 3: Chance
                        ImGuiMCP::TableSetColumnIndex(3); ImGuiMCP::Text("%.1f%%", r.chanceReward);

                        ImGuiMCP::TableSetColumnIndex(4);
                        std::string persistID = "##p" + std::to_string(gIdx) + "_" + std::to_string(rIdx);
                        if (ImGuiMCP::Checkbox(persistID.c_str(), &r.isPersistent)) {
                            // A referência '&r.isPersistent' já atualiza o valor automaticamente
                        }
                        if (ImGuiMCP::IsItemHovered()) {
                            ImGuiMCP::SetTooltip(GetLoc("auto.if_checked_item_won_t_be_removed_when_rule_becomes_invalid", "If checked, item won't be removed when rule becomes invalid."));
                        }

                        ImGuiMCP::TableSetColumnIndex(5);
                        if (r.typeReward == "Outfit" || r.typeReward == "Spell") {
                            const char* modeNames[] = { GetLoc("auto.standard", "Standard"), GetLoc("auto.special", "Special"), GetLoc("auto.both", "Both") };
                            ImGuiMCP::Text("%s", modeNames[r.functionOnType]);
                        }
                        else {
                            ImGuiMCP::TextDisabled("-");
                        }

                        // Coluna 4: Ação de Remover (Botão X)
                        ImGuiMCP::TableSetColumnIndex(6);
                        if (ImGuiMCP::Button(("X##r" + std::to_string(rIdx)).c_str())) {
                            group.rewards.erase(group.rewards.begin() + rIdx);
                            break; // Interrompe o frame para evitar erro de índice após remoção
                        }
                    }
                    ImGuiMCP::EndTable();
                }

                ImGuiMCP::Unindent();
                ImGuiMCP::Separator();
            }
            ImGuiMCP::PopID();
        }
    }

    bool InputTextString(const char* label, std::string& value) {
        char buf[2048];
        strncpy_s(buf, value.c_str(), _TRUNCATE);
        if (ImGuiMCP::InputText(label, buf, sizeof(buf))) {
            value = buf;
            return true;
        }
        return false;
    }

    bool DrawFormPicker(const char* label, std::string& currentIDStr, const std::string& typeName) {
        auto& list = Manager::GetSingleton()->GetList(typeName);
        bool changed = false;

        if (ImGuiMCP::BeginCombo(label, currentIDStr.c_str())) {
            static char searchBuf[64] = "";
            ImGuiMCP::InputText(GetLoc("auto.search", "Search"), searchBuf, sizeof(searchBuf));
            std::string search(searchBuf);
            std::transform(search.begin(), search.end(), search.begin(), ::tolower);

            for (const auto& info : list) {
                std::string displayName = info.editorID + " - " + info.name;
                std::string lowerName = displayName;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

                if (!search.empty() && lowerName.find(search) == std::string::npos) continue;

                // Formatamos como Plugin|FormID para salvar
                std::string internalID = info.pluginName + "|" + std::to_string(info.formID);

                if (ImGuiMCP::Selectable(displayName.c_str(), currentIDStr == internalID)) {
                    currentIDStr = internalID;
                    changed = true;
                }
            }
            ImGuiMCP::EndCombo();
        }
        return changed;
    }

    void RenderRuleEditor(Rule& rule) {
        ImGuiMCP::PushID(rule.id.c_str());
        if (ImGuiMCP::Checkbox(GetLoc("auto.rule_enabled", "Rule Enabled"), &rule.isEnabled)) {
            // Opcional: Você pode forçar um save ou apenas deixar o hash detectar
        }

        ImGuiMCP::SameLine();
        char nameBuf[256];
        strcpy_s(nameBuf, rule.name.c_str());
        if (ImGuiMCP::InputText(GetLoc("auto.rule_name", "Rule Name"), nameBuf, sizeof(nameBuf),
            ImGuiMCP::ImGuiInputTextFlags_CallbackCharFilter, FilterFileNameChars))
        {
            rule.name = nameBuf;
        }

        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::InputInt(GetLoc("auto.required_level", "Required Level"), &rule.level)) {
            if (rule.level < 1) rule.level = 1;
        }
        ImGuiMCP::SameLine();

        // Targets Section
        //ImGuiMCP::Text(GetLoc("auto.alvos_selecionados", "Alvos: %d selecionados"), rule.filterFormIDs.size());

        if (ImGuiMCP::Button(GetLoc("auto.manage_targets", "Manage Targets"))) {
            ResetSelectionState();
            activeRuleID = rule.id;
            openTargetsModal = true;
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(GetLoc("auto.manage_blacklist", "Manage Blacklist"))) {
            ResetSelectionState();
            activeRuleID = rule.id;
            openBlacklistModal = true;
        }
        ImGuiMCP::SameLine();
        bool modified = rule.IsModified();

        if (ImGuiMCP::Button(GetLoc("auto.preview_affected_npcs", "Preview Affected NPCs"))) {
            activeRuleID = rule.id;
            activeGroupIdx = -1;
            previewnpc = true;
            lastPreviewID = "";
            affectedCache.clear();
        }
        ImGuiMCP::Separator();

        ImGuiMCP::Separator();
        // Rewards Section (Redirecionada para RewardGroups)
        int totalRewards = 0;
        float groupTotal = 0.0f;
        for (const auto& g : rule.rewardGroups) {
            totalRewards += (int)g.rewards.size();
            groupTotal += g.chanceGroup;
        }

        ImGuiMCP::Text(GetLoc("auto.groups_total_items", "Groups: %d | Total Items: %d"), rule.rewardGroups.size(), totalRewards);
        ImGuiMCP::Checkbox(GetLoc("auto.exclusive_groups_pick_only_one_group_from_this_rule", "Exclusive Groups (Pick only one group from this rule)"), &rule.isExclusive);
        if (rule.isExclusive) {
            ImGuiMCP::SameLine();
            if (groupTotal > 100.0f) ImGuiMCP::TextColored({ 1,0,0,1 }, GetLoc("auto.sum", "(Sum: %.1f%% !)"), groupTotal);
            else ImGuiMCP::TextDisabled(GetLoc("auto.sum", "(Sum: %.1f%%)"), groupTotal);
        }

        if (ImGuiMCP::Button(GetLoc("auto.manage_groups", "Manage Groups"))) {
            activeRuleID = rule.id;
            activeGroupIdx = -1;
            isPickingReward = false;
            openRewardsModal = true;
        }


        if (!modified) {
            ImGuiMCP::SameLine();
            if (ImGuiMCP::Button(GetLoc("auto.export_zip", "Export (.zip)"))) {
                RuleManager::GetSingleton()->ExportRule(rule);
            }
            if (ImGuiMCP::IsItemHovered()) {
                ImGuiMCP::SetTooltip(GetLoc("auto.creates_a_zip_file_in_data_viny_mods_edf_export_with_the_cor", "Creates a ZIP file in 'Data/Viny Mods/EDF/Export' with the correct folder structure."));
            }
        }
        ImGuiMCP::PopID();
    }

    std::vector<const InternalFormInfo*> GetNPCsForRule(const Rule& rule) {
        std::vector<const InternalFormInfo*> matches;
        auto manager = Manager::GetSingleton();
        const auto& npcList = manager->GetList("NPC");

        for (const auto& item : npcList) {
            if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(item.formID)) {
                // Reutiliza sua lógica de checagem (Target e Blacklist)
                if (IsNPCMatchingTargets(npc, const_cast<Rule&>(rule), false) &&
                    !IsNPCMatchingTargets(npc, const_cast<Rule&>(rule), true)) {
                    matches.push_back(&item);
                }
            }
        }
        return matches;
    }


    void Render() {
        Manager::GetSingleton()->PopulateAllLists();
        RenderPackageWorkspace();
        // 1. Botão Criar Regra
        if (ImGuiMCP::Button(GetLoc("auto.new_rule", " + New Rule "))) {
            ImGuiMCP::OpenPopup(GetLoc("auto.popupnovaregra", "PopupNovaRegra"));
        }

        // Lógica do Popup de Criação
        if (ImGuiMCP::BeginPopup(GetLoc("auto.popupnovaregra", "PopupNovaRegra"))) {
            static char nBuf[64] = "";
            if (ImGuiMCP::InputText(GetLoc("auto.name", "Name"), nBuf, sizeof(nBuf),
                ImGuiMCP::ImGuiInputTextFlags_CallbackCharFilter, FilterFileNameChars))
            {
                // O texto já sai filtrado aqui
            }
            if (ImGuiMCP::Button(GetLoc("auto.create", "Create"))) {
                auto& r = RuleManager::GetSingleton()->CreateRule(activePackageID);
                r.name = nBuf;
                ImGuiMCP::CloseCurrentPopup();
                nBuf[0] = '\0';
            }
            ImGuiMCP::EndPopup();
        }

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(GetLoc("auto.save", "Save"))) {
            RuleManager::GetSingleton()->SaveRules();
            RuleManager::GetSingleton()->InitializeAffectedNPCsDatabase();
            needsNPCListUpdate = true;
            auto saveMgr = SaveStateManager::GetSingleton();
            auto player = RE::PlayerCharacter::GetSingleton();

            // VERIFICAÇÃO: Contexto válido + Player existe + Player está em uma célula carregada
            // Se player->GetParentCell() for null, o jogador não está no mundo (Main Menu)
            if (saveMgr->GetCurrentContext().isValid && player && player->GetParentCell()) {
                logger::info("[UI] Mundo ativo detectado. Aplicando regras em tempo real...");

                // Aplica ao Player
                ApplyRulesToInstance(player);

                // Aplica aos NPCs próximos
                auto processLists = RE::ProcessLists::GetSingleton();
                if (processLists) {
                    for (auto& handle : processLists->highActorHandles) {
                        auto actorPtr = handle.get();
                        if (actorPtr) {
                            RE::Actor* npc = actorPtr.get();

                            // Filtros: Existe, não é o player, não está morto
                            if (npc && npc != player && !npc->IsDead()) {
                                // Verifica se a regra editada afeta este NPC específico
                                if (RuleManager::GetSingleton()->IsAffected(npc)) {
                                    logger::debug("[UI-LiveUpdate] Atualizando NPC: {}", npc->GetName());
                                    ApplyRulesToInstance(npc);
                                }
                            }
                        }
                    }
                }
                logger::info("[UI] Atualização concluída com sucesso.");
            }
            else {
                logger::info("[UI] Regras salvas. Nenhuma aplicação imediata (fora do jogo ou no Menu Principal).");
            }
        }


        ImGuiMCP::Separator();
        RenderTypeFilter();
        auto& rules = RuleManager::GetSingleton()->GetRules();
        std::string toDelete = "";
        std::string lastRenderedPackage;

        for (auto& rule : rules) {
            if (!packageFilterID.empty() && rule.packageID != packageFilterID) {
                continue;
            }
            if (!activeTypeFilters.empty()) {
                bool matchesFilter = false;
                for (auto& f : rule.targetFilters) {
                    if (activeTypeFilters.contains(f.type)) { matchesFilter = true; break; }
                }
                if (!matchesFilter) continue;
            }

            const auto* ownerPackage = FindPackage(rule.packageID);
            const auto packageName = ownerPackage ? ownerPackage->displayName : rule.packageID;
            if (packageName != lastRenderedPackage) {
                ImGuiMCP::SeparatorText(packageName.c_str());
                lastRenderedPackage = packageName;
            }

            bool modified = rule.IsModified();
            std::string label = rule.name;
            if (!rule.isEnabled) { label = "[OFF] " + label; }
            else if (rule.isEnabled && modified) {
                label += " (Need save)";
            }


            label += " [" + packageName + "] [V:" + std::to_string(rule.version) + "]###" + rule.id;

            // --- Lógica de Cor do Header e Texto ---
            bool stylePushed = false;
            if (!rule.isEnabled) {
                // Escurece o cabeçalho e o texto se estiver OFF
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Text, { 0.5f, 0.5f, 0.5f, 1.0f }); // Cinza
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Header, { 0.1f, 0.1f, 0.1f, 1.0f }); // Dark
                stylePushed = true;
            }
            else if (modified) {
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Header, { 0.4f, 0.3f, 0.1f, 1.0f }); // Laranja (Save)
                stylePushed = true;
            }

            if (ImGuiMCP::CollapsingHeader(label.c_str())) {
                if (stylePushed) ImGuiMCP::PopStyleColor(rule.isEnabled ? 1 : 2);
                RenderRuleEditor(rule);
                if (ImGuiMCP::Button((std::string(GetLoc("auto.delete_rule", "Delete Rule")) + "###btnDel" + rule.id).c_str())) {
                    toDelete = rule.id;
                }
            }
            else {
                if (stylePushed) ImGuiMCP::PopStyleColor(rule.isEnabled ? 1 : 2);
            }

        }

        if (!toDelete.empty()) {
            RuleManager::GetSingleton()->DeleteRule(toDelete);
            // Recalcula após deletar
            RuleManager::GetSingleton()->InitializeAffectedNPCsDatabase();
            needsNPCListUpdate = true;
        }


        Rule* activeRule = GetActiveRule();
        auto viewport = ImGuiMCP::GetMainViewport();
        if (openTargetsModal) {
            ImGuiMCP::SetNextWindowSize({ viewport->Size.x / 1.2f, viewport->Size.y / 1.2f });
            ImGuiMCP::OpenPopup(GetLoc("auto.manage_targets", "Manage Targets"));
        }
        if (ImGuiMCP::BeginPopupModal(GetLoc("auto.manage_targets", "Manage Targets"), &openTargetsModal)) {
            if (activeRule) {
                if (isPickingBlacklist) { // Modo de busca de ID
                    if (ImGuiMCP::Button(GetLoc("auto.back", "Back"))) isPickingBlacklist = false;
                    DrawSelectionTable(*activeRule, currentBlacklistType, false, nullptr, &activeRule->targetFilters);
                }
                else {
                    RenderFilterEditor(*activeRule, false);
                }
            }
            ImGuiMCP::EndPopup();
        }

        if (openRewardsModal) {
            ImGuiMCP::SetNextWindowSize({ viewport->Size.x / 1.2f, viewport->Size.y / 1.2f });
            ImGuiMCP::OpenPopup(GetLoc("auto.rewards", "Rewards"));
        }
        if (ImGuiMCP::BeginPopupModal(GetLoc("auto.rewards", "Rewards"), &openRewardsModal)) {
            if (activeRule) {
                if (isPickingReward && activeGroupIdx >= 0 && activeGroupIdx < (int)activeRule->rewardGroups.size()) {
                    if (ImGuiMCP::Button(GetLoc("auto.back", "Back"))) isPickingReward = false;
                    ImGuiMCP::SameLine();
                    ImGuiMCP::Text(GetLoc("auto.editing_group", "Editing Group: %s"), activeRule->rewardGroups[activeGroupIdx].name.c_str());
                    DrawSelectionTable(*activeRule, currentRewardType, true, &activeRule->rewardGroups[activeGroupIdx]);

                }
                else {
                    RenderRewardGroups(*activeRule);
                }
            }
            else { openRewardsModal = false; }
            ImGuiMCP::EndPopup();
        }

        if (openBlacklistModal) {
            ImGuiMCP::SetNextWindowSize({ viewport->Size.x / 1.2f, viewport->Size.y / 1.2f });
            ImGuiMCP::OpenPopup(GetLoc("auto.manage_blacklist", "Manage Blacklist"));
        }

        if (ImGuiMCP::BeginPopupModal(GetLoc("auto.manage_blacklist", "Manage Blacklist"), &openBlacklistModal)) {
            if (activeRule) {
                if (isPickingBlacklist) {
                    if (ImGuiMCP::Button(GetLoc("auto.back", "Back"))) isPickingBlacklist = false;
                    ImGuiMCP::Separator();
                    DrawSelectionTable(*activeRule, currentBlacklistType, false, nullptr, &activeRule->blacklistFilters);
                }
                else {
                    RenderFilterEditor(*activeRule, true);
                }
            }
            ImGuiMCP::EndPopup();
        }
        if (previewnpc) {
            ImGuiMCP::OpenPopup(GetLoc("auto.npcpreview", "NPCPreview"));
        }
        // Substitua o bloco do NPCPreview por este:
        if (ImGuiMCP::BeginPopupModal(GetLoc("auto.npcpreview", "NPCPreview"), &previewnpc, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {

            // Verificamos se temos uma regra ativa selecionada
            if (activeRule) {
                // Atualiza o cache se mudamos de regra
                if (lastPreviewID != activeRuleID) {
                    affectedCache = GetNPCsForRule(*activeRule);
                    lastPreviewID = activeRuleID; // Usar activeRuleID, não rule.id
                }

                ImGuiMCP::Text(GetLoc("auto.npcs_affected_by_u", "NPCs affected by '%s': %zu"), activeRule->name.c_str(), affectedCache.size());

                float childHeight = 400.0f;
                if (ImGuiMCP::BeginChild("PreviewList", { 500, childHeight }, true, ImGuiMCP::ImGuiWindowFlags_HorizontalScrollbar)) {

                    auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

                    if (ImGuiMCP::BeginTable("PreviewTable", 2, tableFlags)) {
                        ImGuiMCP::TableSetupColumn(GetLoc("auto.formid", "FormID"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGuiMCP::TableSetupColumn(GetLoc("auto.name", "Name"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                        ImGuiMCP::TableHeadersRow();

                        const float item_height = ImGuiMCP::GetTextLineHeightWithSpacing();
                        int total_items = static_cast<int>(affectedCache.size());

                        int display_start = std::max(0, static_cast<int>(ImGuiMCP::GetScrollY() / item_height));
                        int display_end = std::min(total_items, display_start + static_cast<int>(ceil(childHeight / item_height)) + 1);

                        ImGuiMCP::TableNextRow();
                        ImGuiMCP::TableSetColumnIndex(0);
                        ImGuiMCP::Dummy({ 0.0f, display_start * item_height });

                        for (int i = display_start; i < display_end; i++) {
                            auto npcInfo = affectedCache[i];
                            ImGuiMCP::TableNextRow();
                            ImGuiMCP::TableSetColumnIndex(0);
                            ImGuiMCP::Text("%08X", npcInfo->formID);
                            ImGuiMCP::TableSetColumnIndex(1);
                            ImGuiMCP::TextUnformatted(npcInfo->name.empty() ? npcInfo->editorID.c_str() : npcInfo->name.c_str());
                        }

                        ImGuiMCP::TableNextRow();
                        ImGuiMCP::TableSetColumnIndex(0);
                        ImGuiMCP::Dummy({ 0.0f, (total_items - display_end) * item_height });

                        ImGuiMCP::EndTable();
                    }
                }
                ImGuiMCP::EndChild();
            }
            else {
                ImGuiMCP::Text(GetLoc("auto.no_active_rule_selected", "No active rule selected."));
            }

            /*if (ImGuiMCP::Button(GetLoc("auto.close", "Close"))) {
                lastPreviewID = "";
                affectedCache.clear();
                previewnpc = false;
                ImGuiMCP::CloseCurrentPopup();
            }*/
            ImGuiMCP::EndPopup();
        }
    }


    void RenderNPCList() {
        auto manager = Manager::GetSingleton();
        auto ruleManager = RuleManager::GetSingleton();
        const auto& npcList = manager->GetList("NPC");
        const auto& affectedDB = ruleManager->GetAffectedNPCsDatabase();
        auto& allRules = ruleManager->GetRules();

        if (npcList.empty()) {
            ImGuiMCP::Text(GetLoc("auto.no_npcs_loaded_enter_the_game_to_populate_the_list", "No NPCs loaded. Enter the game to populate the list."));
            if (ImGuiMCP::Button(GetLoc("auto.force_scan", "Force Scan"))) manager->PopulateAllLists();
            return;
        }

        static char filterBuffer[256] = "";
        static bool showOnlyAffected = false;

        // --- FILTRAGEM ---
        bool searchChanged = ImGuiMCP::InputText(GetLoc("auto.filter_name_editorid", "Filter Name/EditorID"), filterBuffer, sizeof(filterBuffer));
        bool toggleChanged = ImGuiMCP::Checkbox(GetLoc("auto.show_only_affected_npcs_includes_preview", "Show only affected NPCs (includes Preview)"), &showOnlyAffected);

        if (searchChanged || toggleChanged || needsNPCListUpdate) {
            cachedFilteredIndices.clear();
            std::string search(filterBuffer);
            std::transform(search.begin(), search.end(), search.begin(), ::tolower);

            for (size_t i = 0; i < npcList.size(); i++) {
                const auto& item = npcList[i];

                // Para o filtro de "Apenas Afetados", precisamos checar o DB salvo 
                // OU se ele passaria em alguma regra modificada (Preview)
                bool isAffected = affectedDB.contains(item.formID);

                // Otimização: Se não estiver no DB, checamos se passaria em alguma regra modificada
                if (!isAffected && showOnlyAffected) {
                    if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(item.formID)) {
                        for (const auto& rule : allRules) {
                            if (rule.IsModified()) {
                                if (IsNPCMatchingTargets(npc, const_cast<Rule&>(rule), false) &&
                                    !IsNPCMatchingTargets(npc, const_cast<Rule&>(rule), true)) {
                                    isAffected = true;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (showOnlyAffected && !isAffected) continue;

                if (!search.empty()) {
                    std::string n = item.name; std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                    std::string e = item.editorID; std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                    if (n.find(search) == std::string::npos && e.find(search) == std::string::npos) continue;
                }
                cachedFilteredIndices.push_back(i);
            }
            needsNPCListUpdate = false;
        }

        ImGuiMCP::Text(GetLoc("auto.showing_npcs", "Showing %d NPCs"), (int)cachedFilteredIndices.size());
        ImGuiMCP::Separator();

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
            ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

        ImGuiMCP::ImVec2 avail;
        ImGuiMCP::GetContentRegionAvail(&avail);

        if (ImGuiMCP::BeginTable("NPCDatabaseTable", 4, tableFlags, { 0, avail.y })) {
            ImGuiMCP::TableSetupColumn(GetLoc("auto.formid", "FormID"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.name_editorid", "Name / EditorID"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.status", "Status"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.active_rules", "Active Rules"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableHeadersRow();

            const float item_height = ImGuiMCP::GetTextLineHeightWithSpacing();
            int display_start = std::max(0, static_cast<int>(ImGuiMCP::GetScrollY() / item_height));
            int display_end = std::min(static_cast<int>(cachedFilteredIndices.size()), display_start + static_cast<int>(ceil(avail.y / item_height)) + 1);

            ImGuiMCP::TableNextRow();
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::Dummy({ 0.0f, display_start * item_height });

            for (int i = display_start; i < display_end; i++) {
                size_t idx = cachedFilteredIndices[i];
                const auto& item = npcList[idx];

                auto it = affectedDB.find(item.formID);
                bool hasSavedRules = (it != affectedDB.end());

                std::vector<std::string> previewRules;

                // 2. LÓGICA DE PREVIEW (Apenas para novos NPCs)
                // Se já tem regras salvas, não processamos preview para este NPC
                if (!hasSavedRules) {
                    if (auto npc = RE::TESForm::LookupByID<RE::TESNPC>(item.formID)) {
                        for (const auto& rule : allRules) {
                            if (rule.IsModified()) {
                                if (IsNPCMatchingTargets(npc, const_cast<Rule&>(rule), false) &&
                                    !IsNPCMatchingTargets(npc, const_cast<Rule&>(rule), true)) {
                                    previewRules.push_back(rule.name.empty() ? "No Name" : rule.name);
                                }
                            }
                        }
                    }
                }

                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0); ImGuiMCP::Text("%08X", item.formID);
                ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::TextUnformatted(item.name.empty() ? item.editorID.c_str() : item.name.c_str());

                // Coluna de Status
                ImGuiMCP::TableSetColumnIndex(2);
                if (!previewRules.empty()) {
                    ImGuiMCP::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, GetLoc("auto.preview", "PREVIEW"));
                    if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip(GetLoc("auto.this_npc_will_be_affected_by_unsaved_changes", "This NPC will be affected by unsaved changes."));
                }
                else if (hasSavedRules) {
                    ImGuiMCP::TextColored({ 0.2f, 1.0f, 0.2f, 1.0f }, GetLoc("auto.affected", "AFFECTED"));
                }
                else {
                    ImGuiMCP::TextDisabled(GetLoc("auto.none", "None"));
                }

                // Coluna de Regras
                ImGuiMCP::TableSetColumnIndex(3);
                std::string ruleSummary = "";

                // Regras Salvas
                if (hasSavedRules) {
                    for (const auto& ruleID : it->second.ruleIDs) {
                        auto rIt = std::find_if(allRules.begin(), allRules.end(), [&](const Rule& r) { return r.id == ruleID; });
                        // Só mostra se a regra não estiver na lista de preview (para não duplicar)
                        if (rIt != allRules.end()) {
                            bool beingModified = std::find(previewRules.begin(), previewRules.end(), rIt->name) != previewRules.end();
                            if (!beingModified) {
                                ruleSummary += "[" + (rIt->name.empty() ? ruleID : rIt->name) + "] ";
                            }
                        }
                    }
                }

                // Regras Preview (Destaque visual)
                if (!previewRules.empty()) {
                    for (const auto& pName : previewRules) {
                        ruleSummary += "*" + pName + "* ";
                    }
                    ImGuiMCP::TextColored({ 1.0f, 0.8f, 0.4f, 1.0f }, ruleSummary.c_str());
                }
                else {
                    ImGuiMCP::TextWrapped(ruleSummary.c_str());
                }
            }

            ImGuiMCP::TableNextRow();
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::Dummy({ 0.0f, (static_cast<int>(cachedFilteredIndices.size()) - display_end) * item_height });

            ImGuiMCP::EndTable();
        }
    }

    void MenuSettings() {
        auto settings = NPCSettings::GetSingleton();

        ImGuiMCP::Text(GetLoc("auto.global_settings", "Global Settings"));
        ImGuiMCP::Separator();
        ImGuiMCP::Spacing();

        const char* modes[] = {
            "Off",
            "On (Only Empty Outfit)",
            "On (Full Conversion)"
        };

        int currentMode = static_cast<int>(settings->outfitMode);

        ImGuiMCP::SetNextItemWidth(350.0f);
        if (ImGuiMCP::BeginCombo(GetLoc("auto.outfit_conversion_mode", "Outfit Conversion Mode"), modes[currentMode])) {
            for (int i = 0; i < 3; i++) {
                bool isSelected = (currentMode == i);
                if (ImGuiMCP::Selectable(modes[i], isSelected)) {
                    settings->outfitMode = static_cast<OutfitConversionMode>(i);
                    settings->Save(); // Salva automaticamente ao mudar
                }
            }
            ImGuiMCP::EndCombo();
        }

        ImGuiMCP::SameLine();
        ImGuiMCP::TextDisabled(GetLoc("auto.outfit_mode_help_marker", "(?)"));
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip(GetLoc("auto.defines_how_the_plugin_handles_default_npc_outfits_when_load", "Defines how the plugin handles default NPC outfits when loading the game."));
        }

        if (currentMode == 0) {
            ImGuiMCP::TextColored({ 1.0f, 0.5f, 0.5f, 1.0f }, GetLoc("auto.note_the_spid_equipment_system_may_not_work_correctly_on_npc", "Note: The SPID equipment system may not work correctly on NPCs with original Outfits."));
        }
    }

    namespace SPIDConvert {
        namespace fs = std::filesystem;

        struct FormRef {
            RE::TESForm* form = nullptr;
            std::string plugin;
            std::string localID;
            std::string edfID;
            std::string editorID;
        };

        struct ConvertedEntry {
            std::string summary;
            std::vector<std::string> rewardIDs;
            Rule rule;
        };

        struct Result {
            bool success = false;
            int filesScanned = 0;
            int filesChanged = 0;
            int convertedLines = 0;
            int unsupportedLines = 0;
            std::string zipPath;
            std::string report;
            std::vector<std::string> convertedFiles;
            std::vector<std::string> partiallyConvertedFiles;
            std::vector<std::string> notConvertedFiles;
        };

        static std::string Trim(std::string value)
        {
            if (value.size() >= 3 &&
                static_cast<unsigned char>(value[0]) == 0xEF &&
                static_cast<unsigned char>(value[1]) == 0xBB &&
                static_cast<unsigned char>(value[2]) == 0xBF) {
                value.erase(0, 3);
            }
            const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
            const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
            if (begin >= end) return {};
            return std::string(begin, end);
        }

        static std::string ToLower(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        static bool IsEmptySPIDField(const std::string& value)
        {
            const auto trimmed = ToLower(Trim(value));
            return trimmed.empty() || trimmed == "none";
        }

        static std::vector<std::string> SplitPreserve(const std::string& value, char delimiter)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : value) {
                if (c == delimiter) {
                    parts.push_back(current);
                    current.clear();
                }
                else {
                    current.push_back(c);
                }
            }
            parts.push_back(current);
            return parts;
        }

        static std::string SanitizeZipName(std::string name)
        {
            name = Trim(name);
            if (name.empty()) return "SPID_to_EDF";
            for (char& c : name) {
                if (std::strchr("<>:\"/\\|?*", c)) c = '_';
            }
            while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
            return name.empty() ? "SPID_to_EDF" : name;
        }

        static std::string MakeSafeZipPath(const fs::path& path)
        {
            std::vector<std::string> parts;
            for (const auto& part : path) {
                auto text = part.generic_string();
                if (text.empty() || text == "." || text == ".." || text.ends_with(":")) continue;
                parts.push_back(SanitizeZipName(text));
            }

            if (parts.empty()) return "SPID_to_EDF.ini";

            std::ostringstream safePath;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) safePath << "/";
                safePath << parts[i];
            }
            return safePath.str();
        }

        static void AppendFileList(std::ostringstream& out, const char* title, const std::vector<std::string>& files)
        {
            out << title << " (" << files.size() << ")\n";
            if (files.empty()) {
                out << "  None\n\n";
                return;
            }

            for (const auto& file : files) {
                out << "  - " << file << "\n";
            }
            out << "\n";
        }

        static bool ParseHexID(std::string value, uint32_t& out)
        {
            value = Trim(value);
            if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
                value = value.substr(2);
            }
            if (value.empty()) return false;
            try {
                out = static_cast<uint32_t>(std::stoul(value, nullptr, 16));
                return true;
            }
            catch (...) {
                return false;
            }
        }

        static std::optional<FormRef> MakeFormRef(RE::TESForm* form)
        {
            if (!form) return std::nullopt;
            logger::debug("[SPID->EDF] MakeFormRef: form {:08X}", form->GetFormID());
            const RE::TESFile* file = form->GetFile(0);
            if (!file) {
                if (auto dataHandler = RE::TESDataHandler::GetSingleton()) {
                    const auto formID = form->GetFormID();
                    const auto modIndex = static_cast<std::uint8_t>(formID >> 24);
                    if (modIndex == 0xFE) {
                        const auto lightIndex = static_cast<std::uint16_t>((formID >> 12) & 0xFFF);
                        file = dataHandler->LookupLoadedLightModByIndex(lightIndex);
                    }
                    else if (modIndex != 0xFF) {
                        file = dataHandler->LookupLoadedModByIndex(modIndex);
                    }
                }
            }

            FormRef ref;
            ref.form = form;
            ref.plugin = file ? file->GetFilename() : "Dynamic";
            ref.localID = FormatLocalFormID(form->GetFormID(), ref.plugin);
            ref.edfID = ref.plugin + "|" + ref.localID;
            ref.editorID = Manager::ToUTF8(clib_util::editorID::get_editorID(form));
            logger::debug("[SPID->EDF] MakeFormRef: resolved {}", ref.edfID);
            return ref;
        }

        static std::optional<FormRef> ResolveFormToken(std::string token)
        {
            token = Trim(token);
            if (token.empty()) return std::nullopt;
            if (token.size() >= 2 && ((token.front() == '"' && token.back() == '"') || (token.front() == '\'' && token.back() == '\''))) {
                token = token.substr(1, token.size() - 2);
            }

            logger::debug("[SPID->EDF] ResolveFormToken: '{}'", token);

            if (!token.contains("~") && !token.contains("|")) {
                if (auto form = RE::TESForm::LookupByEditorID(token)) {
                    logger::debug("[SPID->EDF] ResolveFormToken: '{}' resolved by EditorID", token);
                    return MakeFormRef(form);
                }
            }

            auto dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) {
                logger::debug("[SPID->EDF] ResolveFormToken: TESDataHandler unavailable");
                return std::nullopt;
            }

            auto resolvePluginID = [&](std::string plugin, std::string idText) -> std::optional<FormRef> {
                uint32_t localID = 0;
                plugin = Trim(plugin);
                idText = Trim(idText);
                logger::debug("[SPID->EDF] ResolveFormToken: trying plugin '{}' local '{}'", plugin, idText);
                if (!ParseHexID(idText, localID)) {
                    logger::debug("[SPID->EDF] ResolveFormToken: invalid local id '{}'", idText);
                    return std::nullopt;
                }
                const auto formID = dataHandler->LookupFormID(localID, plugin);
                if (!formID) {
                    logger::debug("[SPID->EDF] ResolveFormToken: LookupFormID failed for {}|{}", plugin, idText);
                    return std::nullopt;
                }
                logger::debug("[SPID->EDF] ResolveFormToken: global id {:08X}", formID);
                return MakeFormRef(RE::TESForm::LookupByID(formID));
                };

            if (auto pos = token.find('~'); pos != std::string::npos) {
                return resolvePluginID(token.substr(pos + 1), token.substr(0, pos));
            }
            if (auto pos = token.find('|'); pos != std::string::npos) {
                return resolvePluginID(token.substr(0, pos), token.substr(pos + 1));
            }

            return std::nullopt;
        }

        static std::optional<std::string> InferRewardType(RE::TESForm* form)
        {
            if (!form) return std::nullopt;
            if (form->As<RE::SpellItem>()) return "Spell";
            if (form->As<RE::BGSPerk>()) return "Perk";
            if (form->As<RE::TESShout>()) return "Shout";
            if (form->As<RE::BGSKeyword>()) return "Keyword";
            if (form->As<RE::TESFaction>()) return "Faction";
            if (form->As<RE::BGSOutfit>()) return "Outfit";
            if (form->As<RE::TESObjectWEAP>()) return "Weapon";
            if (form->As<RE::TESObjectARMO>()) return "Armor";
            if (form->As<RE::AlchemyItem>()) return "Potion";
            if (form->As<RE::IngredientItem>()) return "Ingredient";
            if (form->As<RE::ScrollItem>()) return "Scroll";
            if (form->As<RE::TESObjectBOOK>()) return "Book";
            if (form->As<RE::TESAmmo>()) return "Ammo";
            if (form->As<RE::TESSoulGem>()) return "SoulGem";
            if (form->As<RE::TESKey>()) return "Key";
            if (form->As<RE::TESObjectMISC>()) return "Misc";
            return std::nullopt;
        }

        static bool IsRewardCompatible(const std::string& spidType, const std::string& rewardType, RE::TESForm* form)
        {
            if (spidType == "Form") return true;
            if (spidType == "Item") return form && form->As<RE::TESBoundObject>() && rewardType != "Outfit";
            if (spidType == "Spell") return rewardType == "Spell";
            if (spidType == "Perk") return rewardType == "Perk";
            if (spidType == "Shout") return rewardType == "Shout";
            if (spidType == "Keyword") return rewardType == "Keyword";
            if (spidType == "Faction") return rewardType == "Faction";
            if (spidType == "Outfit" || spidType == "SleepOutfit") return rewardType == "Outfit";
            return false;
        }

        static std::optional<std::string> InferFilterType(RE::TESForm* form)
        {
            if (!form) return std::nullopt;
            if (form->As<RE::TESNPC>()) return "NPC";
            if (form->As<RE::TESFaction>()) return "Faction";
            if (form->As<RE::BGSKeyword>()) return "Keyword";
            if (form->As<RE::TESRace>()) return "Race";
            if (form->As<RE::TESPackage>()) return "Package";
            if (form->As<RE::TESCombatStyle>()) return "Combat Style";
            if (form->As<RE::BGSVoiceType>()) return "Voice Type";
            if (form->As<RE::TESClass>()) return "Class";
            if (form->As<RE::BGSLocation>()) return "Location";
            if (form->As<RE::TESObjectCELL>()) return "Cell";
            if (form->As<RE::TESLevCharacter>()) return "Leveled NPC";
            return std::nullopt;
        }

        static bool IEquals(const std::string& lhs, const std::string& rhs)
        {
            return ToLower(lhs) == ToLower(rhs);
        }

        static std::optional<BlacklistFilter> MakeFilterFromRef(const FormRef& ref)
        {
            auto filterType = InferFilterType(ref.form);
            if (!filterType) return std::nullopt;
            return BlacklistFilter{ *filterType, ref.edfID, ref.editorID };
        }

        static std::optional<BlacklistFilter> MakeFilterFromInfo(const InternalFormInfo& info)
        {
            if (info.pluginName.empty()) return std::nullopt;
            BlacklistFilter filter;
            filter.type = info.formType;
            filter.formIDStr = info.pluginName + "|" + FormatLocalFormID(info.formID, info.pluginName);
            filter.editorID = info.editorID;
            return filter;
        }

        static std::vector<BlacklistFilter> ResolveStringFilterTerm(const std::string& term)
        {
            std::vector<BlacklistFilter> filters;

            if (auto ref = ResolveFormToken(term)) {
                if (auto filter = MakeFilterFromRef(*ref)) {
                    filters.push_back(*filter);
                    return filters;
                }
            }

            const auto& npcs = Manager::GetSingleton()->GetList("NPC");
            for (const auto& npcInfo : npcs) {
                if ((!npcInfo.editorID.empty() && IEquals(npcInfo.editorID, term)) ||
                    (!npcInfo.name.empty() && IEquals(npcInfo.name, term))) {
                    if (auto filter = MakeFilterFromInfo(npcInfo)) {
                        filters.push_back(*filter);
                    }
                }
            }

            return filters;
        }

        static bool AppendFilters(std::vector<BlacklistFilter>& destination, const std::vector<BlacklistFilter>& source)
        {
            for (const auto& filter : source) {
                const auto duplicate = std::ranges::any_of(destination, [&](const auto& existing) {
                    return existing.type == filter.type && existing.formIDStr == filter.formIDStr;
                    });
                if (!duplicate) destination.push_back(filter);
            }
            return !source.empty();
        }

        static bool IsSPIDConfigPath(const fs::path& path)
        {
            return ToLower(path.filename().string()).ends_with("_distr.ini");
        }

        static void AddConfigFile(std::vector<fs::path>& files, std::set<std::string>& seenFiles, const fs::path& path, const char* source)
        {
            if (!IsSPIDConfigPath(path)) return;
            const auto key = ToLower(path.lexically_normal().generic_string());
            if (!seenFiles.insert(key).second) return;
            logger::debug("[SPID->EDF] Found SPID file '{}' via {}", path.string(), source);
            files.push_back(path);
        }

        static void ScanDataWithWin32(std::vector<fs::path>& files, std::set<std::string>& seenFiles)
        {
            WIN32_FIND_DATAW findData{};
            HANDLE handle = FindFirstFileW(L"Data\\*_DISTR.ini", &findData);
            if (handle == INVALID_HANDLE_VALUE) {
                logger::debug("[SPID->EDF] Win32 scan found no Data\\*_DISTR.ini entries (error {})", GetLastError());
                return;
            }

            do {
                if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    AddConfigFile(files, seenFiles, fs::path("Data") / findData.cFileName, "Win32");
                }
            } while (FindNextFileW(handle, &findData));

            FindClose(handle);
        }

        static Reward MakeReward(const std::string& type, const FormRef& ref, uint32_t amount, float chance, int functionOnType)
        {
            Reward reward;
            reward.typeReward = type;
            reward.formIDStr = ref.edfID;
            reward.editorID = ref.editorID;
            reward.amount = amount;
            reward.chanceReward = chance;
            reward.functionOnType = functionOnType;
            reward.isPersistent = true;
            return reward;
        }

        static std::vector<ConvertedEntry> TryConvertLine(const std::string& key, const std::string& value, const fs::path& sourceFile, int lineNumber, std::string& reason)
        {
            std::vector<ConvertedEntry> convertedEntries;
            logger::debug("[SPID->EDF] TryConvertLine: {}:{} key='{}' value='{}'",
                sourceFile.string(), lineNumber, key, value);

            static const std::set<std::string> supportedTypes{
                "Form", "Spell", "Perk", "Item", "Shout", "Outfit", "SleepOutfit", "Keyword", "Faction"
            };
            static const std::set<std::string> knownUnsupportedTypes{
                "LevSpell", "Package", "Skin", "ExclusiveGroup", "Linked", "DeathItem"
            };

            std::string type = Trim(key);
            if (type.starts_with("Final")) {
                reason = "Final* records are order-sensitive and are kept in SPID.";
                logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return convertedEntries;
            }
            if (!supportedTypes.contains(type)) {
                if (type == "ExclusiveGroup") reason = "ExclusiveGroup metadata is kept in SPID.";
                else if (knownUnsupportedTypes.contains(type)) reason = "SPID type is not represented by EDF conversion yet.";
                else reason = "Unknown SPID type is kept in SPID.";
                if (!reason.empty()) {
                    logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                }
                return convertedEntries;
            }

            auto parts = SplitPreserve(value, '|');
            if (parts.size() > 7) {
                reason = "Too many SPID fields.";
                logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return convertedEntries;
            }
            parts.resize(7);

            if (!IsEmptySPIDField(parts[3])) {
                reason = "Level/skill filters are not converted safely yet.";
                logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return convertedEntries;
            }

            logger::debug("[SPID->EDF] TryConvertLine: resolving reward '{}'", parts[0]);
            auto rewardRef = ResolveFormToken(parts[0]);
            if (!rewardRef) {
                reason = "Reward form could not be resolved.";
                logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return convertedEntries;
            }

            logger::debug("[SPID->EDF] TryConvertLine: inferring reward type for {}", rewardRef->edfID);
            auto rewardType = InferRewardType(rewardRef->form);
            if (!rewardType || !IsRewardCompatible(type, *rewardType, rewardRef->form)) {
                reason = "Reward form type is not compatible with this SPID record.";
                logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return convertedEntries;
            }

            std::vector<BlacklistFilter> stringPositiveFilters;
            std::vector<BlacklistFilter> formPositiveFilters;
            std::vector<BlacklistFilter> blacklistFilters;

            const auto stringFilterText = Trim(parts[1]);
            if (!IsEmptySPIDField(stringFilterText)) {
                for (auto rawString : SplitPreserve(stringFilterText, ',')) {
                    rawString = Trim(rawString);
                    if (rawString.empty()) continue;
                    if (rawString.starts_with("*")) {
                        reason = "Partial string filters are kept in SPID.";
                        logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                        return convertedEntries;
                    }
                    if (rawString.contains("+")) {
                        reason = "Combined string filters are kept in SPID.";
                        logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                        return convertedEntries;
                    }

                    bool isNegative = false;
                    if (rawString.starts_with("-")) {
                        isNegative = true;
                        rawString.erase(0, 1);
                        rawString = Trim(rawString);
                    }

                    logger::debug("[SPID->EDF] TryConvertLine: resolving string filter '{}'", rawString);
                    auto resolved = ResolveStringFilterTerm(rawString);
                    if (resolved.empty()) {
                        reason = "A string filter could not be resolved to EDF targets.";
                        logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                        return convertedEntries;
                    }

                    AppendFilters(isNegative ? blacklistFilters : stringPositiveFilters, resolved);
                }
            }

            const auto filterText = Trim(parts[2]);
            if (!IsEmptySPIDField(filterText)) {
                for (auto rawFilter : SplitPreserve(filterText, ',')) {
                    rawFilter = Trim(rawFilter);
                    if (rawFilter.empty()) continue;
                    if (rawFilter.contains("+")) {
                        reason = "Combined form filters are kept in SPID.";
                        logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                        return convertedEntries;
                    }
                    bool isNegative = false;
                    if (rawFilter.starts_with("-")) {
                        isNegative = true;
                        rawFilter.erase(0, 1);
                        rawFilter = Trim(rawFilter);
                    }
                    logger::debug("[SPID->EDF] TryConvertLine: resolving target filter '{}'", rawFilter);
                    auto filterRef = ResolveFormToken(rawFilter);
                    if (!filterRef) {
                        reason = "A target form filter could not be resolved.";
                        logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                        return convertedEntries;
                    }
                    auto filter = MakeFilterFromRef(*filterRef);
                    if (!filter) {
                        reason = "A target form filter has no EDF target equivalent.";
                        logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                        return convertedEntries;
                    }
                    AppendFilters(isNegative ? blacklistFilters : formPositiveFilters, std::vector<BlacklistFilter>{ *filter });
                }
            }

            int gender = 0;
            int child = 0;
            const auto traitsText = Trim(parts[4]);
            if (!IsEmptySPIDField(traitsText)) {
                for (auto trait : SplitPreserve(traitsText, '/')) {
                    trait = Trim(trait);
                    if (trait.empty()) continue;
                    int newGender = 0;
                    int newChild = 0;
                    if (trait == "M" || trait == "-F") newGender = 1;
                    else if (trait == "F" || trait == "-M") newGender = 2;
                    else if (trait == "C") newChild = 1;
                    else if (trait == "-C") newChild = 2;
                    else {
                        reason = "Trait filter is not represented by EDF conversion yet.";
                        logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                        return convertedEntries;
                    }
                    if (newGender != 0) {
                        if (gender != 0 && gender != newGender) {
                            reason = "Conflicting gender traits.";
                            logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                            return convertedEntries;
                        }
                        gender = newGender;
                    }
                    if (newChild != 0) {
                        if (child != 0 && child != newChild) {
                            reason = "Conflicting child traits.";
                            logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                            return convertedEntries;
                        }
                        child = newChild;
                    }
                }
            }

            uint32_t amount = 1;
            const auto countText = Trim(parts[5]);
            if (!IsEmptySPIDField(countText)) {
                if (countText.contains("-")) {
                    reason = "Random count ranges are kept in SPID.";
                    logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                    return convertedEntries;
                }
                try {
                    amount = static_cast<uint32_t>(std::stoul(countText));
                }
                catch (...) {
                    reason = "Count/index field could not be parsed.";
                    logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                    return convertedEntries;
                }
            }

            float chance = 100.0f;
            auto chanceText = Trim(parts[6]);
            if (!IsEmptySPIDField(chanceText)) {
                if (chanceText.ends_with("!")) chanceText.pop_back();
                try {
                    chance = std::clamp(std::stof(chanceText), 0.0f, 100.0f);
                }
                catch (...) {
                    reason = "Chance field could not be parsed.";
                    logger::debug("[SPID->EDF] TryConvertLine: kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                    return convertedEntries;
                }
            }

            std::vector<std::vector<BlacklistFilter>> targetFilterSets;
            if (!stringPositiveFilters.empty() && !formPositiveFilters.empty()) {
                for (const auto& stringFilter : stringPositiveFilters) {
                    for (const auto& formFilter : formPositiveFilters) {
                        targetFilterSets.push_back({ stringFilter, formFilter });
                    }
                }
            }
            else if (!stringPositiveFilters.empty()) {
                targetFilterSets.push_back(stringPositiveFilters);
            }
            else if (!formPositiveFilters.empty()) {
                targetFilterSets.push_back(formPositiveFilters);
            }
            else {
                targetFilterSets.push_back({});
            }

            const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
            int comboIndex = 0;
            for (auto& targetFilters : targetFilterSets) {
                Rule rule;
                rule.id = std::format("spid-edf-{}-{}-{}", tick, lineNumber, comboIndex);
                rule.name = std::format("SPID {} {}{}", type, rewardRef->localID, targetFilterSets.size() > 1 ? std::format(" {}", comboIndex + 1) : "");
                rule.version = 1;
                rule.targetGender = gender;
                rule.targetChild = child;
                rule.targetRequiresAll = targetFilters.size() > 1;
                rule.targetFilters = std::move(targetFilters);
                rule.blacklistFilters = blacklistFilters;
                rule.blacklistRequiresAll = false;

                Reward reward = MakeReward(*rewardType, *rewardRef, amount, chance, (type == "SleepOutfit") ? 1 : 0);

                RewardGroup group;
                group.name = "SPID Converted";
                group.rewards.push_back(reward);
                rule.rewardGroups.push_back(group);

                ConvertedEntry entry;
                entry.rule = rule;
                entry.rewardIDs.push_back(reward.formIDStr);
                entry.summary = std::format("{}:{} -> {} {} amount={} chance={:.1f} targets={} blacklist={}",
                    sourceFile.filename().string(), lineNumber, reward.typeReward, reward.formIDStr, reward.amount, reward.chanceReward,
                    rule.targetFilters.size(), rule.blacklistFilters.size());
                logger::debug("[SPID->EDF] TryConvertLine: converted {}", entry.summary);
                convertedEntries.push_back(std::move(entry));
                comboIndex++;
            }
            return convertedEntries;
        }

        static bool TryApplyLinkedLine(
            const std::string& key,
            const std::string& value,
            const fs::path& sourceFile,
            int lineNumber,
            std::vector<ConvertedEntry>& convertedEntries,
            const std::map<std::string, std::vector<std::size_t>>& parentIndex,
            std::string& reason,
            bool& removeLine)
        {
            removeLine = false;
            std::string type = Trim(key);
            if (!type.starts_with("Linked") && !type.starts_with("GlobalLinked")) {
                return false;
            }

            if (type.starts_with("GlobalLinked") || type.starts_with("LinkedDeath") || type.starts_with("GlobalLinkedDeath")) {
                reason = "Global/Death Linked distribution is kept in SPID.";
                logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return true;
            }

            std::string linkedType = type.substr(std::string("Linked").size());
            if (linkedType.empty()) linkedType = "Form";

            auto parts = SplitPreserve(value, '|');
            if (parts.size() > 4) {
                reason = "Linked entry has too many fields.";
                logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return true;
            }
            parts.resize(4);

            auto linkedRef = ResolveFormToken(parts[0]);
            if (!linkedRef) {
                reason = "Linked form could not be resolved.";
                logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return true;
            }

            auto linkedRewardType = InferRewardType(linkedRef->form);
            if (!linkedRewardType || !IsRewardCompatible(linkedType, *linkedRewardType, linkedRef->form)) {
                reason = "Linked form type is not compatible with this Linked record.";
                logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return true;
            }

            const auto parentsText = Trim(parts[1]);
            if (IsEmptySPIDField(parentsText)) {
                reason = "Linked entry has no parent FormsList.";
                logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return true;
            }

            uint32_t amount = 1;
            const auto countText = Trim(parts[2]);
            if (!IsEmptySPIDField(countText)) {
                if (countText.contains("-")) {
                    reason = "Linked random count ranges are kept in SPID.";
                    logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                    return true;
                }
                try {
                    amount = static_cast<uint32_t>(std::stoul(countText));
                }
                catch (...) {
                    reason = "Linked count/index field could not be parsed.";
                    logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                    return true;
                }
            }

            float chance = 100.0f;
            auto chanceText = Trim(parts[3]);
            if (!IsEmptySPIDField(chanceText)) {
                if (chanceText.ends_with("!")) chanceText.pop_back();
                try {
                    chance = std::clamp(std::stof(chanceText), 0.0f, 100.0f);
                }
                catch (...) {
                    reason = "Linked chance field could not be parsed.";
                    logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                    return true;
                }
            }

            Reward linkedReward = MakeReward(*linkedRewardType, *linkedRef, amount, chance, (linkedType == "SleepOutfit") ? 1 : 0);
            bool appliedAny = false;
            bool missingAny = false;
            int parentCount = 0;

            for (auto parentToken : SplitPreserve(parentsText, ',')) {
                parentToken = Trim(parentToken);
                if (parentToken.empty()) continue;
                parentCount++;
                if (parentToken.contains("+") || parentToken.starts_with("-")) {
                    missingAny = true;
                    continue;
                }

                auto parentRef = ResolveFormToken(parentToken);
                if (!parentRef) {
                    missingAny = true;
                    continue;
                }

                auto found = parentIndex.find(parentRef->edfID);
                if (found == parentIndex.end() || found->second.empty()) {
                    missingAny = true;
                    continue;
                }

                for (auto entryIndex : found->second) {
                    if (entryIndex >= convertedEntries.size()) continue;
                    auto& entry = convertedEntries[entryIndex];
                    if (entry.rule.rewardGroups.empty()) continue;
                    entry.rule.rewardGroups.front().rewards.push_back(linkedReward);
                    entry.rewardIDs.push_back(linkedReward.formIDStr);
                    entry.summary += std::format(" + Linked {} {}", linkedReward.typeReward, linkedReward.formIDStr);
                    appliedAny = true;
                }
            }

            if (!appliedAny) {
                reason = "Linked parent was not converted in this file.";
                logger::debug("[SPID->EDF] Linked kept {}:{} ({})", sourceFile.string(), lineNumber, reason);
                return true;
            }

            removeLine = !missingAny && parentCount > 0;
            if (removeLine) {
                reason.clear();
                logger::debug("[SPID->EDF] Linked converted {}:{} -> {} {}", sourceFile.string(), lineNumber, linkedReward.typeReward, linkedReward.formIDStr);
            }
            else {
                reason = "Linked entry was partially converted and kept for unconverted parents.";
                logger::debug("[SPID->EDF] Linked partially converted {}:{} ({})", sourceFile.string(), lineNumber, reason);
            }
            return true;
        }

        static Result ConvertPackage(const std::string& packageName)
        {
            Result result;
            std::ostringstream report;
            mz_zip_archive zip;
            bool zipOpen = false;
            fs::path packageStaging;

            try {
                logger::info("[SPID->EDF] Conversion started. Package='{}'", packageName);
                const fs::path dataDir = "Data";
                const fs::path exportDir = "Data/Viny Mods/EDF/Export";
                logger::debug("[SPID->EDF] Ensuring export dir '{}'", exportDir.string());
                fs::create_directories(exportDir);

                const auto safeName = SanitizeZipName(packageName.empty() ? "SPID_to_EDF" : packageName);
                result.zipPath = (exportDir / (safeName + ".zip")).string();
                logger::debug("[SPID->EDF] ZIP output '{}'", result.zipPath);

                std::vector<fs::path> files;
                std::set<std::string> seenFiles;
                std::error_code ec;
                if (fs::exists(dataDir, ec)) {
                    logger::debug("[SPID->EDF] Scanning data dir '{}'", dataDir.string());
                    fs::recursive_directory_iterator it(dataDir, fs::directory_options::skip_permission_denied, ec);
                    fs::recursive_directory_iterator end;
                    while (!ec && it != end) {
                        const auto& entry = *it;
                        const auto path = entry.path();
                        if (entry.is_regular_file(ec)) {
                            if (ToLower(path.extension().string()) == ".ini") {
                                AddConfigFile(files, seenFiles, path, "std::filesystem");
                            }
                        }
                        else if (ec) {
                            logger::warn("[SPID->EDF] Failed to inspect '{}': {}", path.string(), ec.message());
                            ec.clear();
                        }
                        it.increment(ec);
                        if (ec) {
                            logger::warn("[SPID->EDF] Failed to advance Data scan: {}", ec.message());
                            ec.clear();
                        }
                    }
                }
                else if (ec) {
                    logger::warn("[SPID->EDF] Data dir check failed: {}", ec.message());
                    ec.clear();
                }
                else {
                    logger::warn("[SPID->EDF] Data dir '{}' was not found", dataDir.string());
                }

                logger::debug("[SPID->EDF] Scanning Data\\*_DISTR.ini with Win32 fallback");
                ScanDataWithWin32(files, seenFiles);
                std::sort(files.begin(), files.end(), [](const fs::path& lhs, const fs::path& rhs) {
                    return ToLower(lhs.filename().string()) < ToLower(rhs.filename().string());
                    });
                logger::info("[SPID->EDF] Found {} SPID DISTR file(s)", files.size());

                std::memset(&zip, 0, sizeof(zip));
                logger::debug("[SPID->EDF] Initializing ZIP writer");
                if (!mz_zip_writer_init_file(&zip, result.zipPath.c_str(), 0)) {
                    result.report = "Failed to initialize ZIP.";
                    logger::error("[SPID->EDF] {}", result.report);
                    return result;
                }
                zipOpen = true;

                std::ostringstream details;

                report << "SPID to EDF conversion report\n\n";
                report << "Important notes:\n";
                report << "- Original SPID INIs are not modified. Patched INIs are exported only as copies inside the ZIP.\n";
                report << "- The converter is conservative. Review this report, the generated EDF rules, and the log before installing the result.\n";
                report << "- Text/wildcard filters such as *Mage, dynamic temporary SPID keywords, Final entries, global/death linked entries, and random ranges may stay in SPID for manual review.\n";
                report << "- Prefer exact EditorID/FormID/form filters when you want automatic conversion; text/location-style matching needs extra review.\n\n";
                report << "Scanned files: " << files.size() << "\n\n";

                std::vector<Rule> packageRules;
                for (const auto& file : files) {
                    logger::info("[SPID->EDF] Processing file '{}'", file.string());
                    result.filesScanned++;
                    std::ifstream input(file);
                    if (!input) {
                        logger::warn("[SPID->EDF] Could not open '{}'", file.string());
                        result.notConvertedFiles.push_back(file.filename().string() + " (could not open)");
                        details << "[Skipped] " << file.string() << " -> could not open\n";
                        continue;
                    }

                    bool fileChanged = false;
                    int fileConvertedLines = 0;
                    int fileKeptLines = 0;
                    std::ostringstream patched;
                    std::vector<ConvertedEntry> fileConvertedEntries;
                    std::map<std::string, std::vector<std::size_t>> parentIndex;
                    std::string line;
                    int lineNumber = 0;
                    while (std::getline(input, line)) {
                        lineNumber++;
                        if (!line.empty() && line.back() == '\r') line.pop_back();

                        const auto trimmed = Trim(line);
                        const auto eqPos = line.find('=');
                        if (trimmed.empty() || trimmed.starts_with(";") || trimmed.starts_with("#") || trimmed.starts_with("[") || eqPos == std::string::npos) {
                            patched << line << "\n";
                            continue;
                        }

                        const auto key = Trim(line.substr(0, eqPos));
                        const auto value = line.substr(eqPos + 1);
                        std::string reason;
                        logger::debug("[SPID->EDF] Parsing {}:{}", file.string(), lineNumber);

                        bool removeLinkedLine = false;
                        if (TryApplyLinkedLine(key, value, file, lineNumber, fileConvertedEntries, parentIndex, reason, removeLinkedLine)) {
                            if (removeLinkedLine) {
                                fileChanged = true;
                                result.convertedLines++;
                                fileConvertedLines++;
                                continue;
                            }
                            if (!reason.empty()) {
                                details << "[Kept] " << file.filename().string() << ":" << lineNumber << " -> " << reason << "\n";
                                result.unsupportedLines++;
                                fileKeptLines++;
                            }
                            patched << line << "\n";
                            continue;
                        }

                        auto convertedEntries = TryConvertLine(key, value, file, lineNumber, reason);
                        if (!convertedEntries.empty()) {
                            for (auto& converted : convertedEntries) {
                                const auto entryIndex = fileConvertedEntries.size();
                                for (const auto& rewardID : converted.rewardIDs) {
                                    parentIndex[rewardID].push_back(entryIndex);
                                }
                                fileConvertedEntries.push_back(std::move(converted));
                                result.convertedLines++;
                                fileConvertedLines++;
                            }
                            fileChanged = true;
                            continue;
                        }

                        if (!reason.empty()) {
                            details << "[Kept] " << file.filename().string() << ":" << lineNumber << " -> " << reason << "\n";
                            result.unsupportedLines++;
                            fileKeptLines++;
                        }
                        patched << line << "\n";
                    }

                    if (fileChanged) {
                        result.filesChanged++;
                        for (const auto& converted : fileConvertedEntries) {
                            packageRules.push_back(converted.rule);
                            details << "[Converted] " << converted.summary << " -> SQL package\n";
                        }

                        fs::path sourceRelativePath;
                        try {
                            sourceRelativePath = fs::relative(file, dataDir);
                        }
                        catch (...) {
                            sourceRelativePath = file.filename();
                        }
                        const auto internalPath = "Patched SPID INIs/" + MakeSafeZipPath(sourceRelativePath);
                        const auto patchedText = patched.str();
                        logger::debug("[SPID->EDF] Adding patched SPID INI copy to ZIP as '{}'", internalPath);
                        if (!mz_zip_writer_add_mem(&zip, internalPath.c_str(), patchedText.data(), patchedText.size(), MZ_BEST_COMPRESSION)) {
                            const auto msg = std::format("Failed to add patched SPID INI '{}' to ZIP.", internalPath);
                            logger::error("[SPID->EDF] {}", msg);
                            details << "[Error] " << msg << "\n";
                        }
                        details << "[Patched SPID copy] " << file.string() << " -> " << internalPath << "\n";
                    }

                    const auto fileSummary = std::format("{} (converted={}, kept={})", file.filename().string(), fileConvertedLines, fileKeptLines);
                    if (fileConvertedLines > 0 && fileKeptLines == 0) {
                        result.convertedFiles.push_back(fileSummary);
                    }
                    else if (fileConvertedLines > 0) {
                        result.partiallyConvertedFiles.push_back(fileSummary);
                    }
                    else {
                        result.notConvertedFiles.push_back(fileSummary);
                    }
                }

                if (!packageRules.empty()) {
                    packageStaging = fs::temp_directory_path() /
                        std::format("edf_spid_{}", std::chrono::steady_clock::now().time_since_epoch().count());
                    RulePackage sqlPackage;
                    if (!RuleManager::GetSingleton()->CreateRulesPackageSnapshot(
                            safeName,
                            packageRules,
                            packageStaging,
                            sqlPackage)) {
                        throw std::runtime_error("Failed to build the converted SQL package.");
                    }
                    const auto folder = sqlPackage.path.filename().generic_string();
                    const auto manifestInternal = std::format("Viny Mods/EDF/Packages/{}/manifest.json", folder);
                    const auto databaseInternal = std::format("Viny Mods/EDF/Packages/{}/package.db", folder);
                    if (!mz_zip_writer_add_file(
                            &zip,
                            manifestInternal.c_str(),
                            (sqlPackage.path / "manifest.json").string().c_str(),
                            nullptr,
                            0,
                            MZ_BEST_COMPRESSION) ||
                        !mz_zip_writer_add_file(
                            &zip,
                            databaseInternal.c_str(),
                            (sqlPackage.path / "package.db").string().c_str(),
                            nullptr,
                            0,
                            MZ_BEST_COMPRESSION)) {
                        throw std::runtime_error("Failed to add the SQL package to the conversion ZIP.");
                    }
                    details << "[Package] " << packageRules.size() << " rule(s) -> "
                            << "Viny Mods/EDF/Packages/" << folder << "/package.db\n";
                }

                AppendFileList(report, "Converted files", result.convertedFiles);
                AppendFileList(report, "Partially converted files", result.partiallyConvertedFiles);
                AppendFileList(report, "Not converted files", result.notConvertedFiles);
                report << "Detailed line report\n\n";
                report << details.str();

                result.report = report.str();
                logger::debug("[SPID->EDF] Adding conversion report to ZIP");
                if (!mz_zip_writer_add_mem(&zip, "SPID_to_EDF_Report.txt", result.report.data(), result.report.size(), MZ_BEST_COMPRESSION)) {
                    logger::error("[SPID->EDF] Failed to add conversion report to ZIP");
                }
                logger::debug("[SPID->EDF] Finalizing ZIP");
                if (!mz_zip_writer_finalize_archive(&zip)) {
                    logger::error("[SPID->EDF] Failed to finalize ZIP '{}'", result.zipPath);
                    result.report += "\n[Error] Failed to finalize ZIP.\n";
                    mz_zip_writer_end(&zip);
                    zipOpen = false;
                    if (!packageStaging.empty()) {
                        std::error_code cleanupError;
                        fs::remove_all(packageStaging, cleanupError);
                    }
                    return result;
                }
                mz_zip_writer_end(&zip);
                zipOpen = false;
                if (!packageStaging.empty()) {
                    std::error_code cleanupError;
                    fs::remove_all(packageStaging, cleanupError);
                }
                result.success = true;
                logger::info("[SPID->EDF] Conversion finished. Files={}, changed={}, converted={}, kept={}, zip='{}'",
                    result.filesScanned, result.filesChanged, result.convertedLines, result.unsupportedLines, result.zipPath);
                return result;
            }
            catch (const std::exception& e) {
                logger::error("[SPID->EDF] Conversion exception: {}", e.what());
                result.report = std::format("Conversion failed with exception: {}", e.what());
            }
            catch (...) {
                logger::error("[SPID->EDF] Conversion failed with unknown exception");
                result.report = "Conversion failed with unknown exception.";
            }

            if (zipOpen) {
                mz_zip_writer_end(&zip);
            }
            if (!packageStaging.empty()) {
                std::error_code cleanupError;
                fs::remove_all(packageStaging, cleanupError);
            }
            return result;
        }
    }

    static void RenderSPIDConversionFileGroup(const char* label, const std::vector<std::string>& files)
    {
        const auto header = std::format("{} ({})", label, files.size());
        if (!ImGuiMCP::CollapsingHeader(header.c_str())) return;

        if (files.empty()) {
            ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, GetLoc("auto.none", "None"));
            return;
        }

        for (const auto& file : files) {
            ImGuiMCP::BulletText("%s", file.c_str());
        }
    }

    void SPIDToEDF()
    {
        static char packageName[128] = "SPID_to_EDF";
        static SPIDConvert::Result lastResult;

        ImGuiMCP::Text(GetLoc("auto.spid_to_edf", "SPID to EDF"));
        ImGuiMCP::Separator();
        ImGuiMCP::TextWrapped(GetLoc("auto.converts_supported_distr_ini_entries_into_edf_rules_and_crea", "Converts supported *_DISTR.ini entries into EDF rules and creates a ZIP with EDF rules, patched SPID INI copies, and a report."));
        ImGuiMCP::TextColored({ 0.95f, 0.75f, 0.35f, 1.0f }, GetLoc("auto.review_zip_report_and_log_before_installing", "Review the ZIP report and EasyDistribution.log before installing the result."));
        ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, GetLoc("auto.original_spid_inis_are_not_modified", "Original SPID INIs are not modified; unsupported or unsafe SPID entries stay in the patched copies."));
        ImGuiMCP::TextWrapped(GetLoc("auto.spid_to_edf_conservative_disclaimer", "This conversion is conservative: wildcard/text rules such as *Mage, temporary dynamic keywords, Final entries, global/death linked entries, and random ranges may stay in SPID. Prefer exact EditorID/FormID/form filters when possible; text or location-style matching needs manual review."));
        ImGuiMCP::Separator();

        ImGuiMCP::SetNextItemWidth(260.0f);
        ImGuiMCP::InputText(GetLoc("auto.package_name", "Package Name"), packageName, sizeof(packageName), ImGuiMCP::ImGuiInputTextFlags_CallbackCharFilter, FilterFileNameChars);
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(GetLoc("auto.convert_spid_to_edf", "Convert SPID to EDF"))) {
            logger::info("[SPID->EDF] Convert button clicked");
            try {
                lastResult = SPIDConvert::ConvertPackage(packageName);
                if (lastResult.success) {
                    logger::info("SPID to EDF: converted {} lines into {}", lastResult.convertedLines, lastResult.zipPath);
                }
                else {
                    logger::error("SPID to EDF: conversion failed: {}", lastResult.report);
                }
            }
            catch (const std::exception& e) {
                logger::error("[SPID->EDF] UI click exception: {}", e.what());
                lastResult.success = false;
                lastResult.report = std::format("UI click failed with exception: {}", e.what());
            }
            catch (...) {
                logger::error("[SPID->EDF] UI click failed with unknown exception");
                lastResult.success = false;
                lastResult.report = "UI click failed with unknown exception.";
            }
        }

        if (!lastResult.zipPath.empty()) {
            ImGuiMCP::Separator();
            ImGuiMCP::Text(GetLoc("auto.status", "Status: %s"), lastResult.success ? "Done" : "Failed");
            ImGuiMCP::Text(GetLoc("auto.zip", "ZIP: %s"), lastResult.zipPath.c_str());
            ImGuiMCP::Text(GetLoc("auto.files_scanned_changed_converted_kept", "Files scanned: %d | Changed: %d | Converted: %d | Kept: %d"),
                lastResult.filesScanned, lastResult.filesChanged, lastResult.convertedLines, lastResult.unsupportedLines);
            ImGuiMCP::Separator();
            ImGuiMCP::TextUnformatted(GetLoc("auto.summary", "Summary"));
            ImGuiMCP::BeginChild("SPIDToEDFSummary", { 0, 260.0f }, true);
            RenderSPIDConversionFileGroup(GetLoc("auto.converted_files", "Converted files"), lastResult.convertedFiles);
            RenderSPIDConversionFileGroup(GetLoc("auto.partially_converted_files", "Partially converted files"), lastResult.partiallyConvertedFiles);
            RenderSPIDConversionFileGroup(GetLoc("auto.not_converted_files", "Not converted files"), lastResult.notConvertedFiles);
            ImGuiMCP::EndChild();
            ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, GetLoc("auto.detailed_report_inside_zip", "Detailed line-by-line report is inside the ZIP as SPID_to_EDF_Report.txt."));
        }
    }

    void Export()
    {
        static std::set<std::string> selectedRules;
        static char packageName[128] = "EDF_Export";

        auto& rules = RuleManager::GetSingleton()->GetRules();

        ImGuiMCP::Text(GetLoc("auto.export_package", "Export Package"));
        ImGuiMCP::Separator();
        ImGuiMCP::TextWrapped(GetLoc("auto.select_rules_to_export_into_one_zip_package", "Select rules to export into one ZIP package."));
        ImGuiMCP::TextColored({ 0.7f, 0.7f, 0.7f, 1.0f }, GetLoc("auto.zip_files_are_saved_to_data_viny_mods_edf_export", "ZIP files are saved to Data/Viny Mods/EDF/Export."));
        ImGuiMCP::Separator();

        ImGuiMCP::SetNextItemWidth(260.0f);
        ImGuiMCP::InputText(GetLoc("auto.package_name", "Package Name"), packageName, sizeof(packageName), ImGuiMCP::ImGuiInputTextFlags_CallbackCharFilter, FilterFileNameChars);
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(GetLoc("auto.clear_selection", "Clear Selection"))) {
            selectedRules.clear();
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button(GetLoc("auto.export_selected", "Export Selected"))) {
            auto manager = RuleManager::GetSingleton();
            if (manager->SaveRules()) {
                manager->ExportRulesPackage(packageName, selectedRules);
            }
        }

        ImGuiMCP::Separator();

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable;
        if (ImGuiMCP::BeginTable("EDFExportRules", 3, tableFlags)) {
            ImGuiMCP::TableSetupColumn(GetLoc("auto.export", "Export"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.rule", "Rule"), ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn(GetLoc("auto.version", "Version"), ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGuiMCP::TableHeadersRow();

            for (const auto& rule : rules) {
                ImGuiMCP::TableNextRow();

                ImGuiMCP::TableSetColumnIndex(0);
                bool selected = selectedRules.contains(rule.id);
                ImGuiMCP::PushID(("export_" + rule.id).c_str());
                if (ImGuiMCP::Checkbox("##select", &selected)) {
                    if (selected) selectedRules.insert(rule.id);
                    else selectedRules.erase(rule.id);
                }

                ImGuiMCP::TableSetColumnIndex(1);
                ImGuiMCP::TextUnformatted(rule.name.empty() ? GetLoc("auto.no_name", "No Name") : rule.name.c_str());

                ImGuiMCP::TableSetColumnIndex(2);
                ImGuiMCP::Text("%d", rule.version);
                ImGuiMCP::PopID();
            }

            ImGuiMCP::EndTable();
        }

        ImGuiMCP::Text(GetLoc("auto.selected_rule_s", "Selected: %d rule(s)"), static_cast<int>(selectedRules.size()));
    }

    void Register() {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::error("SKSEMenuFramework not installed!");
            return;
        }

        LoadLanguage();

        SKSEMenuFramework::SetSection(GetLoc("menu.section", "EDF"));
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.rules_manager", "Rules Manager"), Render);
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.npc_database", "NPC Database"), RenderNPCList);
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.export", "Export"), Export);
        SKSEMenuFramework::AddSectionItem(GetLoc("menu.spid_to_edf", "SPID to EDF"), SPIDToEDF);
        //SKSEMenuFramework::AddSectionItem(GetLoc("menu.settings", "Settings"), MenuSettings);

        logger::info("UI Registered via SKSEMenuFramework");
    }
}

