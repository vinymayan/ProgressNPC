#include "UI.h"
#include "Manager.h"
#include "Rule.h"





namespace SPIDUI {
    static std::set<std::string> activeTypeFilters;

    static std::string activeRuleID = "";      // ID da regra sendo editada no momento
    static int activeGroupIdx = -1;            // Índice do grupo de recompensa ativo
    static bool isPickingReward = false;
    //static RewardGroup* currentTargetGroup = nullptr;
    static std::string currentRewardType = "Perk";

    // Variáveis para controlar abertura dos modals fora do loop
    static bool openTargetsModal = false;
    static bool openRewardsModal = false;

    static std::vector<size_t> cachedFilteredIndices;
    static bool needsNPCListUpdate = true;
    static std::string lastNPCSearch = "";
    static bool lastShowOnlyAffected = false;
    static size_t lastRulesCount = 0;

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
        const std::vector<std::string> options = { "NPC", "Faction", "Keyword", "Perk" };

        ImGuiMCP::Text("Filtros Ativos:");
        if (activeTypeFilters.empty()) ImGuiMCP::TextDisabled("Nenhum (Mostrando tudo)");

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

        if (ImGuiMCP::BeginCombo("Adicionar Filtro de Tipo", "Selecionar...")) {
            for (const auto& opt : options) {
                if (ImGuiMCP::Selectable(opt.c_str(), activeTypeFilters.contains(opt))) {
                    activeTypeFilters.insert(opt);
                }
            }
            ImGuiMCP::EndCombo();
        }
    }

    bool IsIDSelected(const std::vector<std::string>& list, const std::string& id) {
        return std::find(list.begin(), list.end(), id) != list.end();
    }
    // Substitua sua função DrawSelectionTable em UI.cpp por esta:
    void DrawSelectionTable(Rule& rule, const std::string& listType, bool isRewardMode, RewardGroup* targetGroup = nullptr) {
        std::vector<InternalFormInfo> customList;
        const std::vector<InternalFormInfo>* sourceList = nullptr;

        if (listType == "All") {
            for (auto& type : { "Spell", "Perk", "Weapon", "Armor"}) {
                auto& l = Manager::GetSingleton()->GetList(type);
                customList.insert(customList.end(), l.begin(), l.end());
            }
            sourceList = &customList;
        }
        else if (listType == "Selected" && targetGroup) {
            for (auto& reward : targetGroup->rewards) {
                auto& l = Manager::GetSingleton()->GetList(reward.typeReward);
                for (auto& info : l) {
                    // CORREÇÃO: Usar FormatLocalFormID para consistência na comparação
                    std::string infoHex = FormatLocalFormID(info.formID, info.pluginName);
                    std::string compareID = info.pluginName + "|" + infoHex;

                    if (compareID == reward.formIDStr) {
                        customList.push_back(info);
                        break;
                    }
                }
            }
            sourceList = &customList;
        }
        else {
            sourceList = &Manager::GetSingleton()->GetList(listType);
        }

        const auto& list = *sourceList;
        static char searchBuf[128] = "";
        static std::string pluginFilter = "All";

        ImGuiMCP::InputText("Buscar Nome/ID", searchBuf, sizeof(searchBuf));
        if (ImGuiMCP::BeginCombo("Filtrar por Plugin", pluginFilter.c_str())) {
            if (ImGuiMCP::Selectable("All", pluginFilter == "All")) pluginFilter = "All";
            std::set<std::string> plugins;
            for (const auto& item : list) if (!item.pluginName.empty()) plugins.insert(item.pluginName);
            for (const auto& p : plugins) if (ImGuiMCP::Selectable(p.c_str(), pluginFilter == p)) pluginFilter = p;
            ImGuiMCP::EndCombo();
        }

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

        ImGuiMCP::ImVec2 avail;
        ImGuiMCP::GetContentRegionAvail(&avail);
        float largura = avail.x;
        float altura = avail.y;

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
            ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

        // 6 Colunas no Reward Mode: Ativo, ID, Nome, Plugin, Amount, Chance
        int columns = isRewardMode ? 6 : 4;

        if (ImGuiMCP::BeginTable("SelectionTable", columns, tableFlags, { largura, altura })) {
            ImGuiMCP::TableSetupColumn("Ativo", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 50.0f);
            ImGuiMCP::TableSetupColumn("FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGuiMCP::TableSetupColumn("Nome", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Plugin", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            if (isRewardMode) {
                ImGuiMCP::TableSetupColumn("Qtde", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGuiMCP::TableSetupColumn("Chance %", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 70.0f);
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
                std::string hexID = FormatLocalFormID(item.formID, item.pluginName); // Função criada no Rule.cpp
                std::string internalID = item.pluginName + "|" + hexID;

                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);

                if (isRewardMode && targetGroup) {
                    // Lógica para Reward Groups
                    auto it = std::find_if(targetGroup->rewards.begin(), targetGroup->rewards.end(), [&](const Reward& r) {
                        return r.formIDStr == internalID;
                        });
                    bool selected = (it != targetGroup->rewards.end());

                    if (ImGuiMCP::Checkbox(("##cb" + internalID).c_str(), &selected)) {
                        if (selected) {
                            Reward r;
                            r.typeReward = item.formType;
                            r.formIDStr = internalID;
                            r.amount = 1;
                            r.chanceReward = 100.0f;
                            targetGroup->rewards.push_back(r);
                        }
                        else {
                            targetGroup->rewards.erase(it);
                        }
                    }

                    ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::Text("%08X", item.formID);
                    ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text(item.name.empty() ? item.editorID.c_str() : item.name.c_str());
                    ImGuiMCP::TableSetColumnIndex(3); ImGuiMCP::Text(item.pluginName.c_str());

                    if (selected) {
                        auto& editIt = *std::find_if(targetGroup->rewards.begin(), targetGroup->rewards.end(), [&](const Reward& r) {
                            return r.formIDStr == internalID;
                            });

                        // Coluna Qtde
                        ImGuiMCP::TableSetColumnIndex(4);
                        if (listType == "Keyword" || listType == "Spell" || listType == "Perk") {
                            ImGuiMCP::TextDisabled("1");
                        }
                        else {
                            int val = (int)editIt.amount;
                            ImGuiMCP::SetNextItemWidth(-FLT_MIN);
                            if (ImGuiMCP::InputInt(("##amt" + internalID).c_str(), &val, 0, 0)) {
                                editIt.amount = (uint32_t)std::max(1, val);
                            }
                        }

                        // Coluna Chance
                        ImGuiMCP::TableSetColumnIndex(5);
                        ImGuiMCP::SetNextItemWidth(-FLT_MIN);
                        ImGuiMCP::InputFloat(("##ch" + internalID).c_str(), &editIt.chanceReward, 0, 0, "%.1f");
                        editIt.chanceReward = std::clamp(editIt.chanceReward, 0.0f, 100.0f);
                    }
                }
                else {
                    // Lógica de Targets (Alvos)
                    bool selected = IsIDSelected(rule.filterFormIDs, internalID);
                    if (ImGuiMCP::Checkbox(("##cb" + internalID).c_str(), &selected)) {
                        if (selected) rule.filterFormIDs.push_back(internalID);
                        else std::erase(rule.filterFormIDs, internalID);
                    }
                    ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::Text("%08X", item.formID);
                    ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text(item.name.empty() ? item.editorID.c_str() : item.name.c_str());
                    ImGuiMCP::TableSetColumnIndex(3); ImGuiMCP::Text(item.pluginName.c_str());
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
        if (ImGuiMCP::Button("Voltar")) openRewardsModal = false;
		ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("+ Novo Grupo")) {
            rule.rewardGroups.push_back({ "Novo Grupo", false, {} });
        }

        ImGuiMCP::Separator();

        for (size_t gIdx = 0; gIdx < rule.rewardGroups.size(); gIdx++) {
            auto& group = rule.rewardGroups[gIdx];
            ImGuiMCP::PushID(static_cast<int>(gIdx));

            // Título do CollapsingHeader com informações do grupo
            std::string headerLabel = group.name + " (" + std::to_string(group.rewards.size()) + " rewards)";
            if (group.isExclusive) headerLabel = "[EXCL] " + headerLabel;

            if (ImGuiMCP::CollapsingHeader(headerLabel.c_str())) {
                ImGuiMCP::Indent();

                char nameBuf[64];
                strcpy_s(nameBuf, group.name.c_str());
                if (ImGuiMCP::InputText("Nome do Grupo", nameBuf, sizeof(nameBuf))) group.name = nameBuf;

                ImGuiMCP::SameLine();
                if (ImGuiMCP::Button("Excluir Grupo")) {
                    rule.rewardGroups.erase(rule.rewardGroups.begin() + gIdx);
                    ImGuiMCP::Unindent();
                    ImGuiMCP::PopID();
                    continue;
                }

                ImGuiMCP::Checkbox("Exclusivo (Sorteia apenas um da lista)", &group.isExclusive);

                if (group.isExclusive) {
                    float total = 0;
                    for (const auto& r : group.rewards) total += r.chanceReward;
                    if (total > 100.0f) ImGuiMCP::TextColored({ 1,0,0,1 }, "Aviso: Soma das chances (%.1f%%) excede 100%%!", total);
                    else ImGuiMCP::TextDisabled("Chance total acumulada: %.1f%%", total);
                }

                if (ImGuiMCP::Button("Adicionar Rewards")) {
                    isPickingReward = true;
                    activeGroupIdx = static_cast<int>(gIdx);
                }

                ImGuiMCP::Spacing();
                ImGuiMCP::Text("Rewards no Grupo:");

                // --- Tabela de Visualização de Rewards ---
                auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg | ImGuiMCP::ImGuiTableFlags_Resizable;
                if (ImGuiMCP::BeginTable("GroupRewardsSummary", 4, tableFlags)) {
                    ImGuiMCP::TableSetupColumn("Tipo", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGuiMCP::TableSetupColumn("Reward", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
                    ImGuiMCP::TableSetupColumn("Qtde", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 50.0f);
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
        if (ImGuiMCP::InputText("Nome da Regra", nameBuf, sizeof(nameBuf))) rule.name = nameBuf;

        ImGuiMCP::SetNextItemWidth(150.0f);
        if (ImGuiMCP::InputInt("Nível Necessário", &rule.level)) {
            if (rule.level < 1) rule.level = 1;
        }

        auto viewport = ImGuiMCP::GetMainViewport();

        // Targets Section
        ImGuiMCP::Text("Alvos: %d selecionados", rule.filterFormIDs.size());

        if (ImGuiMCP::Button("Gerenciar Alvos")) {
            activeRuleID = rule.id;
            openTargetsModal = true;
        }

        ImGuiMCP::Separator();

        // Rewards Section (Redirecionada para RewardGroups)
        int totalRewards = 0;
        for (const auto& g : rule.rewardGroups) totalRewards += (int)g.rewards.size();

        ImGuiMCP::Text("Grupos: %d | Total de Itens: %d", rule.rewardGroups.size(), totalRewards);


        if (ImGuiMCP::Button("Gerenciar Recompensas")) {
            activeRuleID = rule.id;
            activeGroupIdx = -1;
            isPickingReward = false;
            openRewardsModal = true;
        }

        ImGuiMCP::PopID();
    }

    void Render() {
       
        // 1. Botão Criar Regra
        if (ImGuiMCP::Button(" + Nova Regra ")) {
            ImGuiMCP::OpenPopup("PopupNovaRegra");
        }

        // Lógica do Popup de Criação
        if (ImGuiMCP::BeginPopup("PopupNovaRegra")) {
            static char nBuf[64] = ""; static std::string t = "NPC";
            ImGuiMCP::InputText("Nome", nBuf, sizeof(nBuf));
            if (ImGuiMCP::BeginCombo("Tipo", t.c_str())) {
                for (auto opt : { "NPC", "Faction", "Keyword" }) if (ImGuiMCP::Selectable(opt, t == opt)) t = opt;
                ImGuiMCP::EndCombo();
            }
            if (ImGuiMCP::Button("Criar")) {
                auto& r = RuleManager::GetSingleton()->CreateRule();
                r.name = nBuf; r.type = t; ImGuiMCP::CloseCurrentPopup();
            }
            ImGuiMCP::EndPopup();
        }

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Salvar Tudo")) {
            RuleManager::GetSingleton()->SaveRules();
            RuleManager::GetSingleton()->InitializeAffectedNPCsDatabase();
            needsNPCListUpdate = true;
        }
        ImGuiMCP::SameLine();
        ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, { 0.2f, 0.5f, 0.2f, 1.0f }); // Cor verde para destacar
        if (ImGuiMCP::Button("Gerar Relatório (Log)")) {
            RuleManager::GetSingleton()->GenerateDistributionReport();
        }
        ImGuiMCP::PopStyleColor();

        ImGuiMCP::Separator();
        RenderTypeFilter();
        auto& rules = RuleManager::GetSingleton()->GetRules();
        std::string toDelete = "";

        for (auto& rule : rules) {
            if (!activeTypeFilters.empty() && !activeTypeFilters.contains(rule.type)) continue;

            // Verifica se a regra foi modificada para adicionar um marcador visual
            bool modified = rule.IsModified();
            std::string label = "[" + rule.type + "] " + (rule.name.empty() ? rule.id : rule.name);

            if (modified) {
                label += " * (Não Salvo)"; // Indicador visual de modificação
            }
            label += "###" + rule.id;

            // Se modificada, muda a cor do cabeçalho para destacar
            if (modified) {
                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Header, { 0.4f, 0.3f, 0.1f, 1.0f }); // Tom alaranjado/marrom
            }

            if (ImGuiMCP::CollapsingHeader(label.c_str())) {
                if (modified) ImGuiMCP::PopStyleColor(); // Remove a cor se abrir o header

                RenderRuleEditor(rule);

                if (ImGuiMCP::Button(("Deletar Regra###btnDel" + rule.id).c_str())) {
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

        // --- RENDERIZAÇÃO CENTRALIZADA DOS MODALS (Fora do loop de regras) ---
        Rule* activeRule = GetActiveRule();
        auto viewport = ImGuiMCP::GetMainViewport();
        if (openTargetsModal) {
            ImGuiMCP::SetNextWindowSize({ viewport->Size.x / 1.2f, viewport->Size.y / 1.2f });
            ImGuiMCP::OpenPopup("TargetsModal");
        }
        if (ImGuiMCP::BeginPopupModal("TargetsModal", &openTargetsModal)) {
            if (activeRule) DrawSelectionTable(*activeRule, activeRule->type, false);
            ImGuiMCP::EndPopup();
        }

        if (openRewardsModal) { 
            ImGuiMCP::SetNextWindowSize({ viewport->Size.x / 1.2f, viewport->Size.y / 1.2f });
            ImGuiMCP::OpenPopup("RewardsModal"); }
        if (ImGuiMCP::BeginPopupModal("RewardsModal", &openRewardsModal)) {
            if (activeRule) {
                if (isPickingReward && activeGroupIdx >= 0 && activeGroupIdx < (int)activeRule->rewardGroups.size()) {
                    if (ImGuiMCP::Button("Voltar")) isPickingReward = false;
					ImGuiMCP::SameLine();
                    ImGuiMCP::Text("Editando Grupo: %s", activeRule->rewardGroups[activeGroupIdx].name.c_str());
                    if (ImGuiMCP::BeginCombo("Filtrar", currentRewardType.c_str())) {
                        for (auto opt : { "All", "Selected", "Perk", "Spell", "Weapon", "Armor"}) if (ImGuiMCP::Selectable(opt, currentRewardType == opt)) currentRewardType = opt;
                        ImGuiMCP::EndCombo();
                    }
                    DrawSelectionTable(*activeRule, currentRewardType, true, &activeRule->rewardGroups[activeGroupIdx]);
                    
                }
                else {
                    RenderRewardGroups(*activeRule);
                    //if (ImGuiMCP::Button("Fechar")) openRewardsModal = false;
                }
            }
            else { openRewardsModal = false; }
            ImGuiMCP::EndPopup();
        }
    }

    void RenderNPCList() {
        auto manager = Manager::GetSingleton();
        auto ruleManager = RuleManager::GetSingleton();
        const auto& npcList = manager->GetList("NPC");
        const auto& affectedDB = ruleManager->GetAffectedNPCsDatabase();

        if (npcList.empty()) {
            ImGuiMCP::Text("Nenhum NPC carregado. Entre no jogo para popular a lista.");
            if (ImGuiMCP::Button("Forçar Escaneamento")) manager->PopulateAllLists();
            return;
        }

        static char filterBuffer[256] = "";
        static bool showOnlyAffected = false;
        static size_t lastDBSize = 0; // Para detectar mudanças no banco de dados

        // --- FILTRAGEM LEVE (UI/Busca) ---
        bool searchChanged = ImGuiMCP::InputText("Filtrar Nome/EditorID", filterBuffer, sizeof(filterBuffer));
        bool toggleChanged = ImGuiMCP::Checkbox("Mostrar apenas NPCs afetados", &showOnlyAffected);

        // Atualiza o cache de visualização se algo mudou ou se o DB de afetados foi reconstruído
        if (searchChanged || toggleChanged || needsNPCListUpdate || affectedDB.size() != lastDBSize) {
            cachedFilteredIndices.clear();
            std::string search(filterBuffer);
            std::transform(search.begin(), search.end(), search.begin(), ::tolower);

            for (size_t i = 0; i < npcList.size(); i++) {
                const auto& item = npcList[i];
                bool isAffected = affectedDB.contains(item.formID);

                if (showOnlyAffected && !isAffected) continue;

                if (!search.empty()) {
                    std::string n = item.name; std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                    std::string e = item.editorID; std::transform(e.begin(), e.end(), e.begin(), ::tolower);
                    if (n.find(search) == std::string::npos && e.find(search) == std::string::npos) continue;
                }
                cachedFilteredIndices.push_back(i);
            }
            needsNPCListUpdate = false;
            lastDBSize = affectedDB.size();
        }

        // --- RENDERIZAÇÃO ---
        ImGuiMCP::Text("Exibindo %d / %d NPCs (%llu afetados por regras)",
            (int)cachedFilteredIndices.size(), (int)npcList.size(), affectedDB.size());
        ImGuiMCP::Separator();

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
            ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

        ImGuiMCP::ImVec2 avail;
        ImGuiMCP::GetContentRegionAvail(&avail);

        if (ImGuiMCP::BeginTable("NPCDatabaseTable", 4, tableFlags, { 0, avail.y })) {
            ImGuiMCP::TableSetupColumn("FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn("Nome / EditorID", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Status", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGuiMCP::TableSetupColumn("Regras Ativas", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableHeadersRow();

            const float item_height = ImGuiMCP::GetTextLineHeightWithSpacing();
            int display_start = std::max(0, static_cast<int>(ImGuiMCP::GetScrollY() / item_height));
            int display_end = std::min(static_cast<int>(cachedFilteredIndices.size()), display_start + static_cast<int>(ceil(avail.y / item_height)) + 1);

            // Dummy superior para o Clipper
            ImGuiMCP::TableNextRow();
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::Dummy({ 0.0f, display_start * item_height });

            for (int i = display_start; i < display_end; i++) {
                size_t idx = cachedFilteredIndices[i];
                const auto& item = npcList[idx];

                // Busca no banco de dados pré-calculado
                auto it = affectedDB.find(item.formID);
                bool isAffected = (it != affectedDB.end());

                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0); ImGuiMCP::Text("%08X", item.formID);
                ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::TextUnformatted(item.name.empty() ? item.editorID.c_str() : item.name.c_str());

                ImGuiMCP::TableSetColumnIndex(2);
                if (isAffected) {
                    ImGuiMCP::TextColored({ 0.2f, 1.0f, 0.2f, 1.0f }, "AFETADO");
                }
                else {
                    ImGuiMCP::TextDisabled("Nenhum");
                }

                ImGuiMCP::TableSetColumnIndex(3);
                if (isAffected) {
                    std::string ruleSummary;
                    for (const auto& ruleID : it->second.ruleIDs) {
                        // Tenta achar o nome amigável da regra
                        auto& rules = ruleManager->GetRules();
                        auto rIt = std::find_if(rules.begin(), rules.end(), [&](const Rule& r) { return r.id == ruleID; });
                        std::string rName = (rIt != rules.end() && !rIt->name.empty()) ? rIt->name : ruleID;
                        ruleSummary += "[" + rName + "] ";
                    }
                    ImGuiMCP::TextWrapped(ruleSummary.c_str());
                }
                else {
                    ImGuiMCP::TextDisabled("-");
                }
            }

            // Dummy inferior para o Clipper
            ImGuiMCP::TableNextRow();
            ImGuiMCP::TableSetColumnIndex(0);
            ImGuiMCP::Dummy({ 0.0f, (static_cast<int>(cachedFilteredIndices.size()) - display_end) * item_height });

            ImGuiMCP::EndTable();
        }
    }

    void Register() {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::error("SKSEMenuFramework not installed!");
            return;
        }

        SKSEMenuFramework::SetSection("ProgressNPC");
        SKSEMenuFramework::AddSectionItem("Rules Manager", Render);
        SKSEMenuFramework::AddSectionItem("NPC Database", RenderNPCList);
        
        logger::info("UI Registered via SKSEMenuFramework");
    }
}
