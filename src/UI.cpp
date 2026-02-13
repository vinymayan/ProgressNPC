#include "UI.h"
#include <unordered_set>
namespace SPIDUI {
    static std::set<std::string> activeTypeFilters;

    static std::string activeRuleID = "";      // ID da regra sendo editada no momento
    static int activeGroupIdx = -1;            // Índice do grupo de recompensa ativo
    static bool isPickingReward = false;
    //static RewardGroup* currentTargetGroup = nullptr;
    static std::string currentRewardType = "All";

    // Variáveis para controlar abertura dos modals fora do loop
    static bool openTargetsModal = false;
    static bool openRewardsModal = false;

    static std::vector<size_t> cachedFilteredIndices;
    static bool needsNPCListUpdate = true;
    static std::string lastNPCSearch = "";
    static bool lastShowOnlyAffected = false;
    static size_t lastRulesCount = 0;

    static bool openBlacklistModal = false;
    static bool isPickingBlacklist = false;
    static std::string currentBlacklistType = "All";

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

    void RenderTypeFilter() {
        const std::vector<std::string> options = { "NPC", "Faction", "Keyword", "Race" };

        ImGuiMCP::Text("Active Filters:");
        if (activeTypeFilters.empty()) ImGuiMCP::TextDisabled("None (Showing all)");

        // Mostra "chips" dos filtros ativos
        for (auto it = activeTypeFilters.begin(); it != activeTypeFilters.end(); ) {
            if (ImGuiMCP::Button((*it + " x").c_str())) {
                it = activeTypeFilters.erase(it);
            }
            else {
                ImGuiMCP::SameLine();
                ++it;
            }
        }
        ImGuiMCP::NewLine();

        if (ImGuiMCP::BeginCombo("Add Type Filter", "Select...")) {
            for (const auto& opt : options) {
                if (ImGuiMCP::Selectable(opt.c_str(), activeTypeFilters.contains(opt))) {
                    activeTypeFilters.insert(opt);
                }
            }
            ImGuiMCP::EndCombo();
        }
    }

    void RenderFilterEditor(Rule& rule, bool isBlacklist) {
        if (ImGuiMCP::Button("Back")) {
            if (isBlacklist) openBlacklistModal = false;
            else openTargetsModal = false;
        }
        ImGuiMCP::SameLine();
        // 1. Resolvemos as referências dos dados com base no modo
        auto& filters = isBlacklist ? rule.blacklistFilters : rule.targetFilters;
        auto& gender = isBlacklist ? rule.blacklistedGender : rule.targetGender;
        auto& requiresAll = isBlacklist ? rule.blacklistRequiresAll : rule.targetRequiresAll;

        // 2. Textos dinâmicos para a UI
        const char* genderLabel = isBlacklist ? "Excluded Gender:" : "Target Gender:";
        const char* genderNoneOption = isBlacklist ? "None" : "All";
        const char* checkLabel = isBlacklist ? "Require ALL filters to invalidate (AND)" : "Require ALL filters (AND)";
        const char* tooltip = isBlacklist ? "(?) If unchecked, any match invalidates" : "(?) If unchecked, any match validates (OR)";
        const char* buttonLabel = isBlacklist ? "Add New Filter" : "Add New Target Filter";
        const char* tableName = isBlacklist ? "BlacklistTable" : "TargetsTable";
        const std::string idPrefix = isBlacklist ? "X##bl" : "X##target";

        // --- Renderização da Interface ---
        ImGuiMCP::Text(genderLabel);
        ImGuiMCP::SameLine();
        const char* genders[] = { genderNoneOption, "Male", "Female" };
        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::BeginCombo(isBlacklist ? "##gender" : "##targetGender", genders[gender])) {
            for (int i = 0; i < 3; i++) {
                if (ImGuiMCP::Selectable(genders[i], gender == i)) gender = i;
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
            isPickingBlacklist = true;
        }

        if (ImGuiMCP::BeginTable(tableName, 4, ImGuiMCP::ImGuiTableFlags_Borders)) {
            ImGuiMCP::TableSetupColumn("Type", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGuiMCP::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Identifier", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Action", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
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
                                    resolvedName = fullName->GetFullName();
                                }
                                // 2. Fallback para EditorID (Keywords ou nomes vazios)
                                if (resolvedName.empty() || resolvedName == "Not Found") {
                                    resolvedName = clib_util::editorID::get_editorID(form);
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

        // --- SISTEMA DE CACHE PARA O MODO "SELECTED" ---
        static std::vector<InternalFormInfo> cachedSelectedList;
        static std::vector<InternalFormInfo> rewardAllCache;
        static std::vector<InternalFormInfo> filterAllCache; // Novo cache para filtros
        static void* lastPointer = nullptr;
        static size_t lastSize = 0;


        const std::vector<InternalFormInfo>* sourceList = nullptr;

        if (listType == "All") {
            if (isRewardMode) {
                if (rewardAllCache.empty()) {
                    for (auto& type : { "Spell", "Perk", "Weapon", "Armor", "Potion", "Ingredient", "Scroll", "Book", "Ammo", "Misc", "Key", "Outfit" }) {
                        auto& l = Manager::GetSingleton()->GetList(type);
                        rewardAllCache.insert(rewardAllCache.end(), l.begin(), l.end());
                    }
                }
                sourceList = &rewardAllCache;
            }
            else {
                // Lógica "All" para Targets e Blacklist
                if (filterAllCache.empty()) {
                    for (auto& type : { "NPC", "Faction", "Keyword", "Race" }) {
                        auto& l = Manager::GetSingleton()->GetList(type);
                        filterAllCache.insert(filterAllCache.end(), l.begin(), l.end());
                    }
                }
                sourceList = &filterAllCache;
            }
        }
        else if (listType == "Selected") {
            // Lógica de atualização de cache para itens selecionados
            bool needsUpdate = false;
            if (isRewardMode && targetGroup) {
                if (targetGroup != lastPointer || targetGroup->rewards.size() != lastSize) needsUpdate = true;
            }
            else if (!isRewardMode && blacklistTarget) {
                if (blacklistTarget != lastPointer || blacklistTarget->size() != lastSize) needsUpdate = true;
            }

            if (needsUpdate) {
                cachedSelectedList.clear();
                if (isRewardMode && targetGroup) {
                    for (auto& reward : targetGroup->rewards) {
                        auto& l = Manager::GetSingleton()->GetList(reward.typeReward);
                        auto [plugin, fID] = reward.ParseFormID();
                        for (auto& info : l) {
                            if (info.formID == fID && info.pluginName == plugin) {
                                cachedSelectedList.push_back(info);
                                break;
                            }
                        }
                    }
                    lastPointer = targetGroup;
                    lastSize = targetGroup->rewards.size();
                }
                else if (blacklistTarget) {
                    for (auto& filter : *blacklistTarget) {
                        auto& l = Manager::GetSingleton()->GetList(filter.type);
                        auto tokens = split(filter.formIDStr, '|');
                        if (tokens.size() == 2) {
                            uint32_t fID = std::stoul(tokens[1], nullptr, 16);
                            for (auto& info : l) {
                                if (info.formID == fID && info.pluginName == tokens[0]) {
                                    cachedSelectedList.push_back(info);
                                    break;
                                }
                            }
                        }
                    }
                    lastPointer = blacklistTarget;
                    lastSize = blacklistTarget->size();
                }
            }
            sourceList = &cachedSelectedList;
        }
        else {
            sourceList = &Manager::GetSingleton()->GetList(listType);
        }


        if (!sourceList || sourceList->empty()) {
            ImGuiMCP::TextDisabled("No items found in this category.");
            return;
        }
        const auto& list = *sourceList;
        static char searchBuf[128] = "";
        static std::string pluginFilter = "All";

        ImGuiMCP::SetNextItemWidth(200.0f);
        ImGuiMCP::InputText("Search", searchBuf, sizeof(searchBuf));
        ImGuiMCP::SameLine();
        ImGuiMCP::SetNextItemWidth(200.0f);
        if (ImGuiMCP::BeginCombo("##FilterType", listType.c_str())) {
            std::vector<const char*> options;
            if (isRewardMode) {
                options = { "All", "Selected", "Perk", "Spell", "Weapon", "Armor", "Potion", "Ingredient", "Scroll", "Book", "Ammo", "Misc", "Key", "Outfit" };
            }
            else {
                options = { "All", "Selected", "NPC", "Faction", "Keyword", "Race" };
            }
            for (auto opt : options) {
                if (ImGuiMCP::Selectable(opt, listType == opt)) listType = opt;
            }
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::SameLine();
		ImGuiMCP::Text(": Type Filter");
        ImGuiMCP::SameLine();
        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::BeginCombo("##Plugin", pluginFilter.c_str())) {
            if (ImGuiMCP::Selectable("All Plugins", pluginFilter == "All")) pluginFilter = "All";
            std::set<std::string> plugins;
            for (const auto& item : *sourceList) if (!item.pluginName.empty()) plugins.insert(item.pluginName);
            for (const auto& p : plugins) if (ImGuiMCP::Selectable(p.c_str(), pluginFilter == p)) pluginFilter = p;
            ImGuiMCP::EndCombo();
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::Text(": Plugin Filter");
        
        std::string search(searchBuf);
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);

        std::vector<size_t> filteredIndices;
        for (size_t i = 0; i < list.size(); i++) {
            if (pluginFilter != "All" && list[i].pluginName != pluginFilter) continue;
            if (!search.empty()) {
                std::string n = list[i].name; std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                std::string e = list[i].editorID; std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                if (n.find(search) == std::string::npos && e.find(search) == std::string::npos) continue;
            }
            filteredIndices.push_back(i);
        }

        std::unordered_set<std::string> activeIDs;
        if (blacklistTarget) {
            for (auto& f : *blacklistTarget) activeIDs.insert(f.formIDStr);
        }
        else if (isRewardMode && targetGroup) {
            for (auto& r : targetGroup->rewards) activeIDs.insert(r.formIDStr);
        }
        else {
            for (auto& f : rule.targetFilters) activeIDs.insert(f.formIDStr);
        }

        ImGuiMCP::ImVec2 avail;
        ImGuiMCP::GetContentRegionAvail(&avail);
        float largura = avail.x;
        float altura = avail.y;

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
            ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

        // 6 Colunas no Reward Mode: Ativo, ID, Nome, Plugin, Amount, Chance
        bool showTypeColumn = (listType == "All" || listType == "Selected");

        // AJUSTE AQUI: Se for Filtro e tiver que mostrar o tipo, são 5 colunas. Se não, 4.
        int columns = isRewardMode ? (showTypeColumn ? 7 : 6) : (showTypeColumn ? 5 : 4);

        if (ImGuiMCP::BeginTable("SelectionTable", columns, tableFlags, { largura, altura })) {

            ImGuiMCP::TableSetupColumn("Active", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGuiMCP::TableSetupColumn("FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGuiMCP::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            if (showTypeColumn) {
                ImGuiMCP::TableSetupColumn("Type", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            }
            ImGuiMCP::TableSetupColumn("Plugin", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            if (isRewardMode) {
                ImGuiMCP::TableSetupColumn("Qty", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGuiMCP::TableSetupColumn("Chance %", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
                //ImGuiMCP::TableSetupColumn("Loot", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 40.0f);
            }
            ImGuiMCP::TableHeadersRow();

            // MANTENDO CLIPPER MANUAL
            const float item_height = ImGuiMCP::GetTextLineHeightWithSpacing();
            int display_start = std::max(0, static_cast<int>(ImGuiMCP::GetScrollY() / item_height));
            int display_end = std::min(static_cast<int>(filteredIndices.size()), display_start + static_cast<int>(ceil(altura / item_height)) + 1);

            // Espaçador Inicial (Simula o scroll para cima)
            ImGuiMCP::TableNextRow();
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::Dummy({ 0.0f, display_start * item_height });

            for (int i = display_start; i < display_end; i++) {
                const auto& item = list[filteredIndices[i]];
                std::string internalID = item.pluginName + "|" + FormatLocalFormID(item.formID, item.pluginName);

                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);
                bool isSelected = false;
                if (blacklistTarget) {
                    // MODO BLACKLIST (OU TARGETS VIA MODAL)
                    auto it = std::find_if(blacklistTarget->begin(), blacklistTarget->end(), [&](const BlacklistFilter& f) {
                        return f.formIDStr == internalID;
                        });
                    bool selected = (it != blacklistTarget->end());

                    if (ImGuiMCP::Checkbox(("##bl" + internalID).c_str(), &selected)) {
                        // CORREÇÃO: Usar item.formType em vez de listType
                        if (selected) blacklistTarget->push_back({ item.formType, internalID });
                        else if (it != blacklistTarget->end()) blacklistTarget->erase(it);
                    }
                }
                else if (isRewardMode && targetGroup) {
                    auto it = std::find_if(targetGroup->rewards.begin(), targetGroup->rewards.end(), [&](const Reward& r) {
                        return r.formIDStr == internalID;
                        });
                    isSelected = (it != targetGroup->rewards.end());
                    if (ImGuiMCP::Checkbox(("##cb" + internalID).c_str(), &isSelected)) {
                        if (isSelected) {
                            Reward r; r.typeReward = item.formType; r.formIDStr = internalID; r.amount = 1; r.chanceReward = 100.0f;
                            targetGroup->rewards.push_back(r);
                        }
                        else {
                            targetGroup->rewards.erase(it);
                        }
                    }
                }
                else {
                    // MODO TARGETS (ALVOS DIRETOS)
                    auto it = std::find_if(rule.targetFilters.begin(), rule.targetFilters.end(), [&](const BlacklistFilter& f) {
                        return f.formIDStr == internalID;
                        });
                    bool isSelected = (it != rule.targetFilters.end());

                    if (ImGuiMCP::Checkbox(("##cb" + internalID).c_str(), &isSelected)) {
                        // CORREÇÃO: Usar item.formType em vez de listType
                        if (isSelected) rule.targetFilters.push_back({ item.formType, internalID });
                        else if (it != rule.targetFilters.end()) rule.targetFilters.erase(it);
                    }
                }

                ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::Text("%08X", item.formID);
                ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text(item.name.empty() ? item.editorID.c_str() : item.name.c_str());
                int nextIdx = 3;
                if (showTypeColumn) {
                    ImGuiMCP::TableSetColumnIndex(nextIdx++);
                    ImGuiMCP::Text(item.formType.c_str());
                }
                ImGuiMCP::TableSetColumnIndex(nextIdx++); // Plugin
                ImGuiMCP::Text(item.pluginName.c_str());

                if (isRewardMode && targetGroup && isSelected) {
                    auto& editIt = *std::find_if(targetGroup->rewards.begin(), targetGroup->rewards.end(), [&](const Reward& r) {
                        return r.formIDStr == internalID;
                        });

                    // Quantidade
                    ImGuiMCP::TableSetColumnIndex(nextIdx++);
                    if (item.formType == "Keyword" || item.formType == "Spell" || item.formType == "Perk" || item.formType == "Outfit") {
                        ImGuiMCP::TextDisabled("-");
                    }
                    else {
                        int val = (int)editIt.amount;
                        ImGuiMCP::SetNextItemWidth(-FLT_MIN);
                        if (ImGuiMCP::InputInt(("##amt" + internalID).c_str(), &val, 0, 0)) editIt.amount = (uint32_t)std::max(1, val);
                    }

                    // Chance
                    ImGuiMCP::TableSetColumnIndex(nextIdx++);
                    ImGuiMCP::SetNextItemWidth(-FLT_MIN);
                    ImGuiMCP::InputFloat(("##ch" + internalID).c_str(), &editIt.chanceReward, 0, 0, "%.1f");

                    // LOOTABLE (Novo)
                    /*ImGuiMCP::TableSetColumnIndex(nextIdx++);
                    if (item.formType == "Spell" || item.formType == "Perk") {
                        ImGuiMCP::TextDisabled("N/A");
                    }
                    else {
                        ImGuiMCP::Checkbox(("##loot" + internalID).c_str(), &editIt.lootable);
                        if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("If unchecked, item will be 'Non-Playable' (won't appear in loot).");
                    }*/
                }
            }


            ImGuiMCP::TableNextRow();
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::Dummy({ 0.0f, (static_cast<int>(filteredIndices.size()) - display_end) * item_height });

            ImGuiMCP::EndTable();
        }
    }

    // --- NOVO: Gerenciador de Grupos de Recompensa ---
    void RenderRewardGroups(Rule& rule) {
        if (ImGuiMCP::Button("Back")) openRewardsModal = false;
        ImGuiMCP::SameLine();

        if (ImGuiMCP::Button("+ New Group")) {
            rule.rewardGroups.push_back({ "New Group", false, {} });
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
                if (ImGuiMCP::InputText("Group Name", nameBuf, sizeof(nameBuf))) group.name = nameBuf;

                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Delete Group")) {
                    rule.rewardGroups.erase(rule.rewardGroups.begin() + gIdx);
                    ImGuiMCP::Unindent();
                    ImGuiMCP::PopID();
                    continue;
                }
                if (ImGuiMCP::Button("Add Rewards")) {
                    isPickingReward = true;
                    activeGroupIdx = static_cast<int>(gIdx);
                }
                ImGuiMCP::SameLine();
                ImGuiMCP::Checkbox("Exclusive (Picks only one from list)", &group.isExclusive);
                
                if (group.isExclusive) {
                    float total = 0;
                    for (const auto& r : group.rewards) total += r.chanceReward;
                    if (total > 100.0f) ImGuiMCP::TextColored({ 1,0,0,1 }, "Warning: Sum of chances (%.1f%%) exceeds 100%%!", total);
                    else ImGuiMCP::TextDisabled("Total accumulated chance: %.1f%%", total);
                }

                

                ImGuiMCP::Spacing();
                ImGuiMCP::Text("Rewards in Group:");

                // --- Tabela de Visualização de Rewards ---
                auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable;
                if (ImGuiMCP::BeginTable("GroupRewardsSummary", 4, tableFlags)) {
                    ImGuiMCP::TableSetupColumn("Type", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGuiMCP::TableSetupColumn("Reward", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                    ImGuiMCP::TableSetupColumn("Qty", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGuiMCP::TableSetupColumn("Chance", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
                    ImGuiMCP::TableHeadersRow();

                    for (const auto& r : group.rewards) {
                        ImGuiMCP::TableNextRow();
                        ImGuiMCP::TableSetColumnIndex(0); ImGuiMCP::Text(r.typeReward.c_str());
                        ImGuiMCP::TableSetColumnIndex(1);
                        auto [plugin, fID] = r.ParseFormID();
                        auto form = RE::TESForm::LookupByID(fID);

                        if (form) {
                            std::string dName = "";
                            if (auto fullName = form->As<RE::TESFullName>()) {
                                dName = fullName->GetFullName();
                            }
                            if (dName.empty()) {
                                dName = clib_util::editorID::get_editorID(form);
                            }
                            ImGuiMCP::Text(dName.empty() ? r.formIDStr.c_str() : dName.c_str());
                        }
                        else {
                            ImGuiMCP::TextDisabled(r.formIDStr.c_str());
                        }
                        ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text("%d", r.amount);
                        ImGuiMCP::TableSetColumnIndex(3); ImGuiMCP::Text("%.1f%%", r.chanceReward);
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
            ImGuiMCP::InputText("Search", searchBuf, sizeof(searchBuf));
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

        char nameBuf[256];
        strcpy_s(nameBuf, rule.name.c_str());
        if (ImGuiMCP::InputText("Rule Name", nameBuf, sizeof(nameBuf),
            ImGuiMCP::ImGuiInputTextFlags_CallbackCharFilter, FilterFileNameChars))
        {
            rule.name = nameBuf;
        }

        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::InputInt("Required Level", &rule.level)) {
            if (rule.level < 1) rule.level = 1;
        }
		ImGuiMCP::SameLine();
        auto viewport = ImGuiMCP::GetMainViewport();

        // Targets Section
        //ImGuiMCP::Text("Alvos: %d selecionados", rule.filterFormIDs.size());

        if (ImGuiMCP::Button("Manage Targets")) {
            activeRuleID = rule.id;
            openTargetsModal = true;
        }
        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Manage Blacklist")) {
            activeRuleID = rule.id;
            openBlacklistModal = true;
        }
        ImGuiMCP::Separator();

        // Rewards Section (Redirecionada para RewardGroups)
        int totalRewards = 0;
        for (const auto& g : rule.rewardGroups) totalRewards += (int)g.rewards.size();

        ImGuiMCP::Text("Groups: %d | Total Items: %d", rule.rewardGroups.size(), totalRewards);


        if (ImGuiMCP::Button("Manage Rewards")) {
            activeRuleID = rule.id;
            activeGroupIdx = -1;
            isPickingReward = false;
            openRewardsModal = true;
        }
        ImGuiMCP::SameLine();
        //if (ImGuiMCP::Button("Preview Affected NPCs")) {
        //    activeRuleID = rule.id;
        //    activeGroupIdx = -1;
        //    previewnpc = true;
        //    //lastPreviewID = "";
        //    //affectedCache.clear();
        //}
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

        // 1. Botão Criar Regra
        if (ImGuiMCP::Button(" + New Rule ")) {
            ImGuiMCP::OpenPopup("PopupNovaRegra");
        }

        // Lógica do Popup de Criação
        if (ImGuiMCP::BeginPopup("PopupNovaRegra")) {
            static char nBuf[64] = "";
            if (ImGuiMCP::InputText("Name", nBuf, sizeof(nBuf),
                ImGuiMCP::ImGuiInputTextFlags_CallbackCharFilter, FilterFileNameChars))
            {
                // O texto já sai filtrado aqui
            }
            if (ImGuiMCP::Button("Create")) {
                auto& r = RuleManager::GetSingleton()->CreateRule();
                r.name = nBuf;
                ImGuiMCP::CloseCurrentPopup();
                nBuf[0] = '\0';
            }
            ImGuiMCP::EndPopup();
        }

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Save")) {
            RuleManager::GetSingleton()->SaveRules();
            RuleManager::GetSingleton()->InitializeAffectedNPCsDatabase();
            needsNPCListUpdate = true;
        }


        ImGuiMCP::Separator();
        RenderTypeFilter();
        auto& rules = RuleManager::GetSingleton()->GetRules();
        std::string toDelete = "";

        for (auto& rule : rules) {
            if (!activeTypeFilters.empty()) {
                bool matchesFilter = false;
                for (auto& f : rule.targetFilters) {
                    if (activeTypeFilters.contains(f.type)) { matchesFilter = true; break; }
                }
                if (!matchesFilter) continue;
            }

            // Verifica se a regra foi modificada para adicionar um marcador visual
            bool modified = rule.IsModified();
            std::string label = rule.name + " [V:" + std::to_string(rule.version) + "]";

            if (modified) {
                label += " (Need save)"; // Indicador visual de modificação
            }
            label += "###" + rule.id;

            // Se modificada, muda a cor do cabeçalho para destacar
            if (modified) {
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Header, { 0.4f, 0.3f, 0.1f, 1.0f }); // Tom alaranjado/marrom
            }

            if (ImGuiMCP::CollapsingHeader(label.c_str())) {
                if (modified) ImGuiMCP::PopStyleColor(); // Remove a cor se abrir o header

                RenderRuleEditor(rule);
                
               
                // Modal de Preview
                
                if (ImGuiMCP::Button(("Delete Rule###btnDel" + rule.id).c_str())) {
                    toDelete = rule.id;
                }
            }
            else {
                if (modified) ImGuiMCP::PopStyleColor();
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
            ImGuiMCP::OpenPopup("Manage Targets");
        }
        if (ImGuiMCP::BeginPopupModal("Manage Targets", &openTargetsModal)) {
            if (activeRule) {
                if (isPickingBlacklist) { // Modo de busca de ID
                    if (ImGuiMCP::Button("Back")) isPickingBlacklist = false;
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
            ImGuiMCP::OpenPopup("Rewards");
        }
        if (ImGuiMCP::BeginPopupModal("Rewards", &openRewardsModal)) {
            if (activeRule) {
                if (isPickingReward && activeGroupIdx >= 0 && activeGroupIdx < (int)activeRule->rewardGroups.size()) {
                    if (ImGuiMCP::Button("Back")) isPickingReward = false;
                    ImGuiMCP::SameLine();
                    ImGuiMCP::Text("Editing Group: %s", activeRule->rewardGroups[activeGroupIdx].name.c_str());
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
            ImGuiMCP::OpenPopup("Manage Blacklist");
        }

        if (ImGuiMCP::BeginPopupModal("Manage Blacklist", &openBlacklistModal)) {
            if (activeRule) {
                if (isPickingBlacklist) {
                    if (ImGuiMCP::Button("Back")) isPickingBlacklist = false;
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
            ImGuiMCP::OpenPopup("NPCPreview");
        }
        // Substitua o bloco do NPCPreview por este:
        if (ImGuiMCP::BeginPopupModal("NPCPreview", &previewnpc, ImGuiMCP::ImGuiWindowFlags_AlwaysAutoResize)) {

            // Verificamos se temos uma regra ativa selecionada
            if (activeRule) {
                // Atualiza o cache se mudamos de regra
                if (lastPreviewID != activeRuleID) {
                    affectedCache = GetNPCsForRule(*activeRule);
                    lastPreviewID = activeRuleID; // Usar activeRuleID, não rule.id
                }

                ImGuiMCP::Text("NPCs affected by '%s': %zu", activeRule->name.c_str(), affectedCache.size());

                float childHeight = 400.0f;
                if (ImGuiMCP::BeginChild("PreviewList", { 500, childHeight }, true, ImGuiMCP::ImGuiWindowFlags_HorizontalScrollbar)) {

                    auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
                        ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

                    if (ImGuiMCP::BeginTable("PreviewTable", 2, tableFlags)) {
                        ImGuiMCP::TableSetupColumn("FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGuiMCP::TableSetupColumn("Name", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
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
                ImGuiMCP::Text("No active rule selected.");
            }

            /*if (ImGuiMCP::Button("Close")) {
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
            ImGuiMCP::Text("No NPCs loaded. Enter the game to populate the list.");
            if (ImGuiMCP::Button("Force Scan")) manager->PopulateAllLists();
            return;
        }

        static char filterBuffer[256] = "";
        static bool showOnlyAffected = false;

        // --- FILTRAGEM ---
        bool searchChanged = ImGuiMCP::InputText("Filter Name/EditorID", filterBuffer, sizeof(filterBuffer));
        bool toggleChanged = ImGuiMCP::Checkbox("Show only affected NPCs (includes Preview)", &showOnlyAffected);

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

        ImGuiMCP::Text("Showing %d NPCs", (int)cachedFilteredIndices.size());
        ImGuiMCP::Separator();

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
            ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

        ImGuiMCP::ImVec2 avail;
        ImGuiMCP::GetContentRegionAvail(&avail);

        if (ImGuiMCP::BeginTable("NPCDatabaseTable", 4, tableFlags, { 0, avail.y })) {
            ImGuiMCP::TableSetupColumn("FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn("Name / EditorID", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Status", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn("Active Rules", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
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
                    ImGuiMCP::TextColored({ 1.0f, 0.8f, 0.2f, 1.0f }, "PREVIEW");
                    if (ImGuiMCP::IsItemHovered()) ImGuiMCP::SetTooltip("This NPC will be affected by unsaved changes.");
                }
                else if (hasSavedRules) {
                    ImGuiMCP::TextColored({ 0.2f, 1.0f, 0.2f, 1.0f }, "AFFECTED");
                }
                else {
                    ImGuiMCP::TextDisabled("None");
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

        ImGuiMCP::Text("Global Settings");
        ImGuiMCP::Separator();
        ImGuiMCP::Spacing();

        const char* modes[] = {
            "Off",
            "On (Only Empty Outfit)",
            "On (Full Conversion)"
        };

        int currentMode = static_cast<int>(settings->outfitMode);

        ImGuiMCP::SetNextItemWidth(350.0f);
        if (ImGuiMCP::BeginCombo("Outfit Conversion Mode", modes[currentMode])) {
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
        ImGuiMCP::TextDisabled("(?)");
        if (ImGuiMCP::IsItemHovered()) {
            ImGuiMCP::SetTooltip("Defines how the plugin handles default NPC outfits when loading the game.");
        }

        if (currentMode == 0) {
            ImGuiMCP::TextColored({ 1.0f, 0.5f, 0.5f, 1.0f }, "Note: The SPID equipment system may not work correctly on NPCs with original Outfits.");
        }
    }

    void Register() {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::error("SKSEMenuFramework not installed!");
            return;
        }

        SKSEMenuFramework::SetSection("EDF");
        SKSEMenuFramework::AddSectionItem("Rules Manager", Render);
        SKSEMenuFramework::AddSectionItem("NPC Database", RenderNPCList);
        //SKSEMenuFramework::AddSectionItem("Settings", MenuSettings);

        logger::info("UI Registered via SKSEMenuFramework");
    }
}

