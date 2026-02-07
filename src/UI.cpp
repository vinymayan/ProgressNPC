#include "UI.h"
#include "Manager.h"
#include "Rule.h"





namespace SPIDUI {
    static std::set<std::string> activeTypeFilters;

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
    void DrawSelectionTable(Rule& rule, const std::string& listType, bool isRewardMode) {
        auto& list = Manager::GetSingleton()->GetList(listType);
        static char searchBuf[128] = "";
        static std::string pluginFilter = "All";

        ImGuiMCP::InputText("Buscar Nome/ID", searchBuf, sizeof(searchBuf));
        if (ImGuiMCP::BeginCombo("Filtrar por Plugin", pluginFilter.c_str())) {
            if (ImGuiMCP::Selectable("All", pluginFilter == "All")) pluginFilter = "All";

            std::set<std::string> plugins;
            for (const auto& item : list) {
                if (!item.pluginName.empty()) plugins.insert(item.pluginName);
            }
            for (const auto& p : plugins) {
                if (ImGuiMCP::Selectable(p.c_str(), pluginFilter == p)) pluginFilter = p;
            }
            ImGuiMCP::EndCombo();
        }
        // Filtros de busca
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
        float altura = avail.y; // Deixa espaço para o botão Fechar

        auto tableFlags = ImGuiMCP::ImGuiTableFlags_Borders | ImGuiMCP::ImGuiTableFlags_RowBg |
            ImGuiMCP::ImGuiTableFlags_Resizable | ImGuiMCP::ImGuiTableFlags_ScrollY;

        // Aumentado para 5 colunas se for Reward (para incluir Amount)
        int columns = isRewardMode ? 5 : 4;

        if (ImGuiMCP::BeginTable("SelectionTable", columns, tableFlags, { largura, altura })) {
            ImGuiMCP::TableSetupColumn("Ativo", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGuiMCP::TableSetupColumn("FormID", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn("Nome", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Plugin", ImGuiMCP::ImGuiTableColumnFlags_WidthStretch);
            if (isRewardMode) ImGuiMCP::TableSetupColumn("Amount", ImGuiMCP::ImGuiTableColumnFlags_WidthFixed, 120.0f);

            ImGuiMCP::TableHeadersRow();

            const float item_height = ImGuiMCP::GetTextLineHeightWithSpacing();
            const float scroll_y = ImGuiMCP::GetScrollY();
            int display_start = static_cast<int>(scroll_y / item_height);
            int display_end = display_start + static_cast<int>(altura / item_height) + 2;

            display_start = std::max(0, display_start);
            display_end = std::min(static_cast<int>(filteredIndices.size()), display_end);

            if (display_start > 0) ImGuiMCP::Dummy({ 0.0f, display_start * item_height });

            for (int i = display_start; i < display_end; i++) {
                const auto& item = list[filteredIndices[i]];
                std::string internalID = item.pluginName + "|" + std::to_string(item.formID);

                ImGuiMCP::TableNextRow();
                ImGuiMCP::TableSetColumnIndex(0);

                // LOGICA DE SELEÇÃO CORRIGIDA
                if (isRewardMode) {
                    // Procura se a recompensa já existe
                    auto it = std::find_if(rule.rewards.begin(), rule.rewards.end(), [&](const Reward& r) {
                        return r.formIDStr == internalID;
                        });
                    bool selected = (it != rule.rewards.end());

                    if (ImGuiMCP::Checkbox(("##cb" + internalID).c_str(), &selected)) {
                        if (selected) {
                            Reward r;
                            r.typeReward = listType;
                            r.formIDStr = internalID;
                            r.amount = 1;
                            rule.rewards.push_back(r);
                        }
                        else {
                            rule.rewards.erase(it);
                        }
                    }

                    // Colunas comuns
                    ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::Text("%08X", item.formID);
                    ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text(item.name.empty() ? item.editorID.c_str() : item.name.c_str());
                    ImGuiMCP::TableSetColumnIndex(3); ImGuiMCP::Text(item.pluginName.c_str());

                    // COLUNA AMOUNT (Apenas para Rewards selecionados)
                    ImGuiMCP::TableSetColumnIndex(4);
                    if (selected) {
                        auto editIt = std::find_if(rule.rewards.begin(), rule.rewards.end(), [&](const Reward& r) {
                            return r.formIDStr == internalID;
                            });

                        if (listType == "Keyword" || listType == "Spell" || listType == "Perk") {
                            editIt->amount = 1;
                            ImGuiMCP::Text("1");
                        }
                        else {
                            int val = (int)editIt->amount;
                            ImGuiMCP::SetNextItemWidth(-FLT_MIN);
                            if (ImGuiMCP::InputInt(("##amt" + internalID).c_str(), &val)) {
                                editIt->amount = (uint32_t)std::max(1, val);
                            }
                        }
                    }
                }
                else {
                    // Lógica de Targets
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

            int remaining_items = static_cast<int>(filteredIndices.size()) - display_end;
            if (remaining_items > 0) ImGuiMCP::Dummy({ 0.0f, remaining_items * item_height });

            ImGuiMCP::EndTable();
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

        // Nome da Regra
        InputTextString("Nome da Regra", rule.name);
        ImGuiMCP::SetNextItemWidth(150.0f); // Define uma largura fixa para o campo numérico
        if (ImGuiMCP::InputInt("Nível Necessário", &rule.level)) {
            if (rule.level < 1) rule.level = 1; // Garante que o nível não seja menor que 1
        }

        auto viewport = ImGuiMCP::GetMainViewport();

        // 2. Define a posição no topo (opcional, para garantir que centralize ou alinhe ao topo)

        // 3. Define o tamanho: Largura total e Altura total - 20
        

        // Targets Summary
        ImGuiMCP::Text("Targets: %d selecionados", rule.filterFormIDs.size());
        static bool showTargets = false;
        if (ImGuiMCP::Button("Gerenciar Alvos")) {
            showTargets = true;
            ImGuiMCP::SetNextWindowSize({ viewport->Size.x / 1.2f, viewport->Size.y / 1.2f });
            ImGuiMCP::OpenPopup("ManageTargetsPopup");
        }

        if (ImGuiMCP::BeginPopupModal("ManageTargetsPopup", &showTargets)) {
            DrawSelectionTable(rule, rule.type, false);
           // if (ImGuiMCP::Button("Fechar")) ImGuiMCP::CloseCurrentPopup();
            ImGuiMCP::EndPopup();
        }

        ImGuiMCP::Separator();

        // Rewards Summary
        ImGuiMCP::Text("Rewards: %d ativos", rule.rewards.size());
        static bool showRewards = false;
        if (ImGuiMCP::Button("Gerenciar Recompensas")) {
            showRewards = true;
            ImGuiMCP::SetNextWindowSize({ viewport->Size.x / 1.2f, viewport->Size.y / 1.2f });
            ImGuiMCP::OpenPopup("ManageRewardsPopup");
        }

        if (ImGuiMCP::BeginPopupModal("ManageRewardsPopup", &showRewards)) {
            // Dropdown para escolher o tipo de recompensa antes da tabela
            static std::string rewardType = "Spell";
            if (ImGuiMCP::BeginCombo("Tipo de Recompensa", rewardType.c_str())) {
                for (auto t : { "Spell", "Perk", "Weapon", "Armor", "Keyword" })
                    if (ImGuiMCP::Selectable(t, rewardType == t)) rewardType = t;
                ImGuiMCP::EndCombo();
            }
            DrawSelectionTable(rule, rewardType, true); // No modo Reward, checkbox adicionaria ao rule.rewards

           // if (ImGuiMCP::Button("Fechar")) ImGuiMCP::CloseCurrentPopup();
            ImGuiMCP::EndPopup();
        }

        ImGuiMCP::PopID();
    }

    // Em UI.cpp, ajuste a função Render
    void Render() {
        // 1. Botão Criar Regra
        if (ImGuiMCP::Button(" + Nova Regra ")) {
            ImGuiMCP::OpenPopup("PopupNovaRegra");
        }

        // Lógica do Popup de Criação
        if (ImGuiMCP::BeginPopup("PopupNovaRegra")) {
            static char novoNome[64] = "";
            static std::string novoTipo = "NPC";

            ImGuiMCP::InputText("Nome", novoNome, sizeof(novoNome));
            if (ImGuiMCP::BeginCombo("Tipo de Alvo", novoTipo.c_str())) {
                for (auto t : { "NPC", "Faction", "Keyword" })
                    if (ImGuiMCP::Selectable(t, novoTipo == t)) novoTipo = t;
                ImGuiMCP::EndCombo();
            }

            if (ImGuiMCP::Button("Criar")) {
                auto& r = RuleManager::GetSingleton()->CreateRule();
                r.name = novoNome;
                r.type = novoTipo;
                novoNome[0] = '\0'; // Limpa buffer
                ImGuiMCP::CloseCurrentPopup();
            }
            ImGuiMCP::EndPopup();
        }

        ImGuiMCP::SameLine();
        if (ImGuiMCP::Button("Salvar Tudo")) {
            RuleManager::GetSingleton()->SaveRules();
        }

        ImGuiMCP::Separator();
        RenderTypeFilter();
        static char ruleSearchBuf[128] = "";
        ImGuiMCP::SetNextItemWidth(-FLT_MIN); // Ocupa a largura total disponível
        ImGuiMCP::InputText("Pesquisar Regra (Nome ou ID)", ruleSearchBuf, sizeof(ruleSearchBuf));

        std::string ruleSearch(ruleSearchBuf);
        std::transform(ruleSearch.begin(), ruleSearch.end(), ruleSearch.begin(), ::tolower);
        ImGuiMCP::Separator();

        // 2. Listagem de Regras
        auto& rules = RuleManager::GetSingleton()->GetRules();
        std::string ruleToDelete = "";

        for (auto& rule : rules) {
            if (!activeTypeFilters.empty() && !activeTypeFilters.contains(rule.type)) continue;
            if (!ruleSearch.empty()) {
                std::string nameLower = rule.name;
                std::string idLower = rule.id;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                std::transform(idLower.begin(), idLower.end(), idLower.begin(), ::tolower);

                if (nameLower.find(ruleSearch) == std::string::npos &&
                    idLower.find(ruleSearch) == std::string::npos) {
                    continue;
                }
            }
            std::string label = rule.name.empty() ? ("ID: " + rule.id) : rule.name;
            label = "[" + rule.type + "] " + label + "###" + rule.id;

            bool open = ImGuiMCP::CollapsingHeader(label.c_str());

            // Botão de deletar rápido à direita (opcional) ou dentro do editor
            if (open) {
                RenderRuleEditor(rule);

                ImGuiMCP::PushStyleColor(ImGuiMCP::ImGuiCol_Button, { 0.6f, 0.2f, 0.2f, 1.0f });
                if (ImGuiMCP::Button(("Deletar Regra: " + rule.name).c_str())) {
                    ruleToDelete = rule.id;
                }
                ImGuiMCP::PopStyleColor();
                ImGuiMCP::Separator();
            }
        }

        // Processa deleção fora do loop para evitar crash de iterador
        if (!ruleToDelete.empty()) {
            RuleManager::GetSingleton()->DeleteRule(ruleToDelete);
        }
    }

    void RenderNPCList() {
        auto manager = Manager::GetSingleton();
        const auto& list = manager->GetList("NPC");

        if (list.empty()) {
            ImGuiMCP::Text("No NPCs loaded yet. (Did you enter the game?)");
            if (ImGuiMCP::Button("Force Populate")) {
                manager->PopulateAllLists();
            }
            return;
        }

        static char filterBuffer[256] = "";
        ImGuiMCP::InputText("Filter by Name/EditorID", filterBuffer, sizeof(filterBuffer));

        // Filter
        static std::vector<size_t> filteredIndices;
        std::string search(filterBuffer);
        std::transform(search.begin(), search.end(), search.begin(), ::tolower);
        
        // Rebuild filter list (optimize: only when dirty)
        filteredIndices.clear();
        for (size_t i = 0; i < list.size(); i++) {
            if (search.empty()) {
                filteredIndices.push_back(i);
                continue;
            }
            std::string name = list[i].name;
            std::string editorID = list[i].editorID;
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            std::transform(editorID.begin(), editorID.end(), editorID.begin(), ::tolower);

            if (name.find(search) != std::string::npos || editorID.find(search) != std::string::npos) {
                filteredIndices.push_back(i);
            }
        }
        
        ImGuiMCP::Text("Showing %d / %d NPCs", filteredIndices.size(), list.size());

        /*auto tableFlags = ImGuiMCPTableFlags_Borders | ImGuiMCPTableFlags_RowBg | ImGuiMCPTableFlags_Resizable | ImGuiMCPTableFlags_ScrollY;
        
        if (ImGuiMCP::BeginTable("NPCTable", 4, tableFlags, ImVec2(0, 0))) {
            ImGuiMCP::TableSetupColumn("FormID", ImGuiMCPTableColumnFlags_WidthFixed, 100.0f);
            ImGuiMCP::TableSetupColumn("EditorID", ImGuiMCPTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Name", ImGuiMCPTableColumnFlags_WidthStretch);
            ImGuiMCP::TableSetupColumn("Plugin", ImGuiMCPTableColumnFlags_WidthStretch);
            ImGuiMCP::TableHeadersRow();

            ImGuiMCPListClipper clipper;
            clipper.Begin((int)filteredIndices.size());
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    const auto& item = list[filteredIndices[i]];
                    ImGuiMCP::TableNextRow();
                    ImGuiMCP::TableSetColumnIndex(0); ImGuiMCP::Text("%08X", item.formID);
                    ImGuiMCP::TableSetColumnIndex(1); ImGuiMCP::Text("%s", item.editorID.c_str());
                    ImGuiMCP::TableSetColumnIndex(2); ImGuiMCP::Text("%s", item.name.c_str());
                    ImGuiMCP::TableSetColumnIndex(3); ImGuiMCP::Text("%s", item.pluginName.c_str());
                }
            }
            ImGuiMCP::EndTable();
        }*/
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
