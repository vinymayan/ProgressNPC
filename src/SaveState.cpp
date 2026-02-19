#include "SaveState.h"

std::vector<SaveHistoryEntry>& SaveStateManager::GetCharacterHistory(uint32_t characterID) {
    // Retorna a referência do histórico ou cria uma entrada vazia caso não exista
    return _characterHistory[characterID];
}

void SaveStateManager::SetCurrentContext(uint32_t a_charID, uint32_t a_saveNum) {
    _currentContext.charID = a_charID;
    _currentContext.saveNumber = a_saveNum;
    _currentContext.isValid = true;

    // 1. Inicializa sessão limpa
    _sessionData = SaveHistoryEntry{};
    _sessionData.saveNumber = a_saveNum;

    // 2. Tenta herdar o progresso do save carregado ou do anterior mais próximo
    auto& history = _characterHistory[a_charID];
    SaveHistoryEntry* bestSnapshot = nullptr;

    for (auto& entry : history) {
        if (entry.saveNumber == a_saveNum) {
            _sessionData.npcRuleVersions = entry.npcRuleVersions; // Clone exato
            return;
        }
        if (entry.saveNumber < a_saveNum && (!bestSnapshot || entry.saveNumber > bestSnapshot->saveNumber)) {
            bestSnapshot = &entry;
        }
    }

    if (bestSnapshot) {
        _sessionData.npcRuleVersions = bestSnapshot->npcRuleVersions; // Herança
    }
}

void SaveStateManager::PersistCurrentSave(const std::string& a_saveName) {
    auto& context = _currentContext;
    if (!context.isValid) return;

    // 1. Extração robusta do NOVO save number do nome do arquivo (ex: "Save5_...")
    uint32_t newSaveNumber = 0;
    try {
        std::string fileName = a_saveName;
        // Remove caminhos de diretório para focar apenas no nome do arquivo
        size_t lastSlash = fileName.find_last_of("\\/");
        std::string baseName = (lastSlash == std::string::npos) ? fileName : fileName.substr(lastSlash + 1);

        size_t savePos = baseName.find("Save");
        size_t underscorePos = baseName.find('_');

        if (savePos != std::string::npos && underscorePos != std::string::npos) {
            std::string numStr = baseName.substr(savePos + 4, underscorePos - (savePos + 4));
            newSaveNumber = std::stoul(numStr);
        }
        else {
            newSaveNumber = context.saveNumber; // Fallback para o atual se falhar
        }
    }
    catch (...) { newSaveNumber = context.saveNumber; }

    _sessionData.saveNumber = newSaveNumber;

    // 2. Persistimos a sessão no histórico e no disco
    UpdateSaveEntry(context.charID, _sessionData);

    // 3. Atualizamos o contexto de runtime
    context.saveNumber = newSaveNumber;

    logger::info("[SaveManager] Novo save {} registrado. Save original preservado.", newSaveNumber);
}

std::string SaveStateManager::GetCharacterPath(uint32_t characterID) {
    char hexID[9];
    sprintf_s(hexID, "%08X", characterID);
    return "Data/SKSE/Plugins/EDF/Saves/" + std::string(hexID) + ".json";
}

void SaveStateManager::LoadCharacterData(uint32_t characterID) {
    std::string path = GetCharacterPath(characterID);
    std::ifstream i(path);
    if (!i.is_open()) return;

    try {
        nlohmann::json j;
        i >> j;
        _characterHistory[characterID] = j.get<std::vector<SaveHistoryEntry>>();
        //logger::info("Historico carregado para personagem {:X}: {} entradas.", characterID, _characterHistory[characterID].size());
    }
    catch (...) {
        logger::error("Erro ao ler dados do personagem {:X}", characterID);
    }
}

void SaveStateManager::UpdateSaveEntry(uint32_t characterID, const SaveHistoryEntry& newEntry) {
    auto& history = _characterHistory[characterID];

    // Tenta encontrar se este saveNumber já existe para atualizar, senão adiciona
    auto it = std::find_if(history.begin(), history.end(), [&](const SaveHistoryEntry& e) {
        return e.saveNumber == newEntry.saveNumber;
        });

    if (it != history.end()) *it = newEntry;
    else history.push_back(newEntry);

    // Salva o arquivo físico
    std::string path = GetCharacterPath(characterID);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream o(path);
    nlohmann::json j = history;
    o << j.dump(4) << std::endl;
}

void EquipBestInventoryItems(RE::Actor* a_actor)
{
    if (!a_actor) return;

    auto equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) return;

    auto inventory = a_actor->GetInventory();

    // Criamos uma lista de pares para armazenar e ordenar os itens
    using InventoryPair = std::pair<RE::TESBoundObject*, RE::InventoryEntryData*>;
    std::vector<InventoryPair> itemsToProcess;

    for (auto& [item, invData] : inventory) {
        auto& [count, entry] = invData;

        if (count > 0 && item->IsArmor()) {
            auto armor = item->As<RE::TESObjectARMO>();

            // 1. FILTRO: Ignora se for um escudo
            if (armor && armor->IsShield()) {
                continue;
            }

            // Adiciona à nossa lista temporária
            itemsToProcess.push_back({ item, entry.get() });
        }
    }

    // 2. PRIORIZAÇÃO: Ordena por Armor Rating (maior primeiro)
    // Isso garante que armaduras pesadas/leves venham antes de roupas (rating 0)
    std::sort(itemsToProcess.begin(), itemsToProcess.end(), [](const InventoryPair& a, const InventoryPair& b) {
        auto armorA = a.first->As<RE::TESObjectARMO>();
        auto armorB = b.first->As<RE::TESObjectARMO>();
        return armorA->GetArmorRating() > armorB->GetArmorRating();
        });

    // 3. EQUIPAGEM: O motor do Skyrim gerencia os slots automaticamente.
    // Ao equipar o item com maior rating primeiro, ele ocupará o slot.
    for (auto& [item, entry] : itemsToProcess) {
        if (!entry->IsWorn()) {
            auto extraData = (entry->extraLists && !entry->extraLists->empty()) ? entry->extraLists->front() : nullptr;

            // Equipamos com o flag 'p_queueEquip' como true para evitar conflitos imediatos de animação
            equipManager->EquipObject(a_actor, item, extraData, 1, nullptr, false, true, false, true);

            logger::debug("  [Tentativa] Equipando item '{}' (Rating: {}).",
                item->GetName(), item->As<RE::TESObjectARMO>()->GetArmorRating());
        }
    }

    a_actor->Update3DModel();
    logger::debug("[OutfitSync] Equipamento para {}. Verificação completa.", a_actor->GetName());
}

struct PendingEquip {
    RE::TESBoundObject* object;
    int priority; // 0 para Outfit (Baixa), 1 para Recompensas Individuais (Alta)
};
void RemoveRuleRewards(RE::Actor* a_actor, const Rule& a_rule) {
    if (!a_actor) return;

    for (const auto& group : a_rule.rewardGroups) {
        for (const auto& reward : group.rewards) {
            auto [plugin, fID] = reward.ParseFormID();
            auto rewardForm = RE::TESForm::LookupByID(fID);
            if (!rewardForm) continue;

            if (reward.typeReward == "Spell") {
                if (auto spell = rewardForm->As<RE::SpellItem>()) {
                    a_actor->RemoveSpell(spell);
                }
            }
            else if (reward.typeReward == "Perk") {
                if (auto perk = rewardForm->As<RE::BGSPerk>()) {
                    a_actor->RemovePerk(perk);
                }
            }
            else if (reward.typeReward == "Outfit") {
                // Outfits são complexos de "remover", mas podemos tirar os itens
                if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                    for (auto* form : outfit->outfitItems) {
                        if (auto* bound = form->As<RE::TESBoundObject>()) {
                            a_actor->RemoveItem(bound, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                        }
                    }
                }
            }
            else {
                // Itens genéricos (Weapon, Armor, etc)
                if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
                    a_actor->RemoveItem(bound, reward.amount, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                }
            }
        }
    }
    logger::debug("[LocationUpdate] Regra '{}' desaplicada de {}.", a_rule.name, a_actor->GetName());
}
void ApplyRulesToInstance(RE::Actor* a_actor, int a_forcedLevel) {
    if (!a_actor) return;

    // --- LOG DE INÍCIO ---
    std::string actorName = a_actor->GetName();
    RE::FormID actorID = a_actor->GetFormID();
    logger::info("[ApplyRules] INICIANDO processamento para {} (ID: {:08X})", actorName, actorID);

    if (a_actor->IsDead()) {
        logger::info("[ApplyRules] FINALIZADO: Ator está morto.");
        return;
    }

    bool isPlayer = (actorID == 0x00000014) || a_actor->IsPlayer();
    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) {
        logger::info("[ApplyRules] FINALIZADO: BaseNPC não encontrado.");
        return;
    }

    auto& context = SaveStateManager::GetSingleton()->GetCurrentContext();
    if (!context.isValid) {
        logger::info("[ApplyRules] FINALIZADO: Contexto de save inválido.");
        return;
    }

    auto& session = SaveStateManager::GetSingleton()->GetSessionData();
    std::string fileNameStr = "Dynamic";
    if (auto file = baseNPC->GetFile(0)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Created";
    }

    std::string npcKey = fileNameStr + "|" + FormatLocalFormID(actorID, fileNameStr);

    // --- LÓGICA DE REMOÇÃO DE REGRAS INVÁLIDAS ---
    if (session.npcRuleVersions.contains(npcKey)) {
        auto& appliedRulesMap = session.npcRuleVersions[npcKey];
        auto& allRules = RuleManager::GetSingleton()->GetRules();

        for (auto it = appliedRulesMap.begin(); it != appliedRulesMap.end();) {
            const std::string& ruleID = it->first;
            auto ruleDefIt = std::find_if(allRules.begin(), allRules.end(), [&](const Rule& r) { return r.id == ruleID; });

            bool shouldRemove = false;
            if (ruleDefIt == allRules.end()) {
                shouldRemove = true;
            }
            else {
                const Rule& rule = *ruleDefIt;
                if (!rule.isEnabled ||
                    !IsNPCMatchingTargets(baseNPC, rule, false, a_actor) ||
                    IsNPCMatchingTargets(baseNPC, rule, true, a_actor)) {
                    shouldRemove = true;
                }
            }

            if (shouldRemove) {
                if (ruleDefIt != allRules.end()) {
                    RemoveRuleRewards(a_actor, *ruleDefIt);
                }
                it = appliedRulesMap.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    const auto& affectedDB = RuleManager::GetSingleton()->GetAffectedNPCsDatabase();

    // --- COLETA DE REGRAS ---
    std::vector<std::string> rulesToProcess;
    auto addRulesFromID = [&](RE::FormID a_id) {
        if (affectedDB.contains(a_id)) {
            for (const auto& id : affectedDB.at(a_id).ruleIDs) {
                if (std::find(rulesToProcess.begin(), rulesToProcess.end(), id) == rulesToProcess.end()) {
                    rulesToProcess.push_back(id);
                }
            }
        }
        };

    addRulesFromID(actorID);
    addRulesFromID(baseNPC->GetFormID());
    auto templateBase = a_actor->GetTemplateBase();
    if (templateBase) addRulesFromID(templateBase->GetFormID());

    if (rulesToProcess.empty()) {
        logger::info("[ApplyRules] FINALIZADO: Nenhuma regra aplicável para {}", actorName);
        return;
    }

    // --- PROCESSAMENTO E APLICAÇÃO ---
    std::vector<PendingEquip> equipQueue;
    for (const auto& ruleID : rulesToProcess) {
        auto& allRules = RuleManager::GetSingleton()->GetRules();
        auto ruleIt = std::find_if(allRules.begin(), allRules.end(), [&](const Rule& r) { return r.id == ruleID; });
        if (ruleIt == allRules.end()) continue;

        const Rule& currentRule = *ruleIt;
        if (!currentRule.isEnabled) continue;

        if (!IsNPCMatchingTargets(baseNPC, currentRule, false, a_actor) ||
            IsNPCMatchingTargets(baseNPC, currentRule, true, a_actor)) {
            continue;
        }

        int level = (a_forcedLevel != -1) ? a_forcedLevel : a_actor->GetLevel();
        if (level < currentRule.level) continue;

        int appliedVersion = 0;
        if (session.npcRuleVersions.contains(npcKey) && session.npcRuleVersions[npcKey].contains(ruleID)) {
            appliedVersion = session.npcRuleVersions[npcKey][ruleID];
        }

        if (appliedVersion < currentRule.version) {
            std::vector<Reward> rewardsToApply;
            if (appliedVersion == 0) {
                rewardsToApply = RuleManager::GetSingleton()->GetRewardsForSpecificRule(baseNPC, currentRule);
            }
            else {
                // Lógica incremental...
                Rule* oldRule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, appliedVersion);
                if (oldRule) {
                    for (const auto& currentGroup : currentRule.rewardGroups) {
                        if (currentGroup.isExclusive) continue;
                        auto oldGroupIt = std::find_if(oldRule->rewardGroups.begin(), oldRule->rewardGroups.end(),
                            [&](const RewardGroup& g) { return g.name == currentGroup.name; });

                        for (const auto& reward : currentGroup.rewards) {
                            bool isNewReward = true;
                            if (oldGroupIt != oldRule->rewardGroups.end()) {
                                for (const auto& oldReward : oldGroupIt->rewards) {
                                    if (oldReward.formIDStr == reward.formIDStr) { isNewReward = false; break; }
                                }
                            }
                            if (isNewReward && RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f) <= reward.chanceReward) {
                                rewardsToApply.push_back(reward);
                            }
                        }
                    }
                }
                else {
                    rewardsToApply = RuleManager::GetSingleton()->GetRewardsForSpecificRule(baseNPC, currentRule);
                }
            }

            // Aplicação física dos Rewards
            for (const auto& reward : rewardsToApply) {
                auto [plugin, fID] = reward.ParseFormID();
                auto rewardForm = RE::TESForm::LookupByID(fID);
                if (!rewardForm) continue;

                if (reward.typeReward == "Spell") {
                    if (auto spell = rewardForm->As<RE::SpellItem>()) { if (!a_actor->HasSpell(spell)) a_actor->AddSpell(spell); }
                }
                else if (reward.typeReward == "Perk") {
                    if (auto perk = rewardForm->As<RE::BGSPerk>()) { if (!a_actor->HasPerk(perk)) a_actor->AddPerk(perk, 1); }
                }
                else if (reward.typeReward == "Outfit") {
                    if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                        if (reward.isSleepOutfit) a_actor->SetSleepOutfit(outfit, false);
                        else {
                            for (auto* form : outfit->outfitItems) {
                                if (auto* bound = form->As<RE::TESBoundObject>()) {
                                    a_actor->AddObjectToContainer(bound, nullptr, 1, nullptr);
                                    if (!isPlayer) equipQueue.push_back({ bound, 0 });
                                }
                            }
                        }
                    }
                }
                else {
                    if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
                        a_actor->AddObjectToContainer(bound, nullptr, reward.amount, nullptr);
                        if (!isPlayer && (reward.typeReward == "Weapon" || reward.typeReward == "Armor" || reward.typeReward == "Ammo")) {
                            equipQueue.push_back({ bound, 1 });
                        }
                    }
                }
            }
            session.npcRuleVersions[npcKey][ruleID] = currentRule.version;
        }
    }

    // Equipagem final
    if (!equipQueue.empty() && !isPlayer) {
        auto equipManager = RE::ActorEquipManager::GetSingleton();
        if (equipManager) {
            std::sort(equipQueue.begin(), equipQueue.end(), [](const PendingEquip& a, const PendingEquip& b) {
                return a.priority < b.priority;
                });
            for (auto& entry : equipQueue) {
                equipManager->EquipObject(a_actor, entry.object, nullptr, 1, nullptr, false, true, false, true);
            }
        }
    }

    // --- LOG DE FINALIZAÇÃO ---
    logger::info("[ApplyRules] FINALIZADO com sucesso para {} ({:08X})", actorName, actorID);
}



