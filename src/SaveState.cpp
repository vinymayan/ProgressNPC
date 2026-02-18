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

void ApplyRulesToInstance(RE::Actor* a_actor, int a_forcedLevel) {
    if (!a_actor || a_actor->IsDead()) return;
    bool isPlayer = (a_actor->GetFormID() == 0x00000014) || a_actor->IsPlayer();
    //logger::info("É o player? {}", isPlayer);

    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return;

    auto& context = SaveStateManager::GetSingleton()->GetCurrentContext();
    if (!context.isValid) return;

    auto& session = SaveStateManager::GetSingleton()->GetSessionData();
    std::string fileNameStr = "Dynamic";
    if (auto file = baseNPC->GetFile(0)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Created"; // Para NPCs gerados por scripts/leveled lists
    }


    std::string actorName = a_actor->GetName();
    std::string npcKey = fileNameStr + "|" + FormatLocalFormID(a_actor->GetFormID(), fileNameStr);

    const auto& affectedDB = RuleManager::GetSingleton()->GetAffectedNPCsDatabase();


    // --- NOVA LÓGICA DE COLETA DE REGRAS (3 FONTES) ---
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

    // 1. Regras do ID da Instância (Actor)
    addRulesFromID(a_actor->GetFormID());

    // 2. Regras do ID do Base NPC
    addRulesFromID(baseNPC->GetFormID());

    // 3. Regras do ID do Template (se houver)
    auto templateBase = a_actor->GetTemplateBase();
    if (templateBase) {
        addRulesFromID(templateBase->GetFormID());
    }

    if (rulesToProcess.empty()) return;
    std::vector<PendingEquip> equipQueue;
    for (const auto& ruleID : rulesToProcess) {
        auto& allRules = RuleManager::GetSingleton()->GetRules();
        auto ruleIt = std::find_if(allRules.begin(), allRules.end(), [&](const Rule& r) { return r.id == ruleID; });
        if (ruleIt == allRules.end()) continue;

        const Rule& currentRule = *ruleIt;
        if (!currentRule.isEnabled) continue;
        int level = (a_forcedLevel != -1) ? a_forcedLevel : a_actor->GetLevel();
        logger::debug("O ator esta no nivel {}", level);
        if (level < currentRule.level) continue;
        

        int appliedVersion = 0;
        if (session.npcRuleVersions.contains(npcKey) && session.npcRuleVersions[npcKey].contains(ruleID)) {
            appliedVersion = session.npcRuleVersions[npcKey][ruleID];
        }

        if (appliedVersion < currentRule.version) {
            std::vector<Reward> rewardsToApply;

            if (appliedVersion == 0) {
                // APLICAÇÃO TOTAL: Primeira vez que o NPC vê esta regra
                rewardsToApply = RuleManager::GetSingleton()->GetRewardsForSpecificRule(baseNPC, currentRule);
                logger::debug("[ApplyRules] Aplicando nova regra '{}' v{} em {}", currentRule.name, currentRule.version, actorName);
            }
            else {
                // ATUALIZAÇÃO INCREMENTAL: Comparar com a versão antiga
                Rule* oldRule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, appliedVersion);

                if (oldRule) {
                    logger::debug("[ApplyRules] Atualizando '{}' (v{} -> v{}) em {}", currentRule.name, appliedVersion, currentRule.version, actorName);

                    for (const auto& currentGroup : currentRule.rewardGroups) {
                        // Se o grupo for exclusivo e mudou, não aplicamos incrementalmente para não quebrar a lógica de "um por grupo"
                        if (currentGroup.isExclusive) continue;

                        // Localiza o grupo correspondente na regra antiga por nome
                        auto oldGroupIt = std::find_if(oldRule->rewardGroups.begin(), oldRule->rewardGroups.end(),
                            [&](const RewardGroup& g) { return g.name == currentGroup.name; });

                        for (const auto& reward : currentGroup.rewards) {
                            bool isNewReward = true;
                            if (oldGroupIt != oldRule->rewardGroups.end()) {
                                // Verifica se esse item específico já existia na versão anterior desse grupo
                                for (const auto& oldReward : oldGroupIt->rewards) {
                                    if (oldReward.formIDStr == reward.formIDStr) {
                                        isNewReward = false;
                                        break;
                                    }
                                }
                            }

                            if (isNewReward) {
                                float roll = RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f);
                                if (roll <= reward.chanceReward) {
                                    rewardsToApply.push_back(reward);
                                    logger::debug("  [+] Novo Reward detectado e sorteado: {}", reward.formIDStr);
                                }
                            }
                        }
                    }
                }
                else {
                    // Fallback: Se não achar o histórico, aplica tudo (segurança)
                    rewardsToApply = RuleManager::GetSingleton()->GetRewardsForSpecificRule(baseNPC, currentRule);
                }
            }

            // APLICAÇÃO DOS ITENS (Correção para Misc/Outros tipos)
            for (const auto& reward : rewardsToApply) {
                auto [plugin, fID] = reward.ParseFormID();
                auto rewardForm = RE::TESForm::LookupByID(fID);
                if (!rewardForm) continue;

                if (reward.typeReward == "Spell") {
                    if (auto spell = rewardForm->As<RE::SpellItem>()) {
                        if (!a_actor->HasSpell(spell)) {
                            a_actor->AddSpell(spell);
                        }
                    }
                }
                else if (reward.typeReward == "Perk") {
                    if (auto perk = rewardForm->As<RE::BGSPerk>()) {
                        if (!a_actor->HasPerk(perk)) {
                            a_actor->AddPerk(perk, 1);
                        }
                    }
                }
                else if (reward.typeReward == "Outfit") {
                    if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                        if (reward.isSleepOutfit) {
                            // APLICAÇÃO DE SLEEP OUTFIT
                            a_actor->SetSleepOutfit(outfit, false);
                            logger::debug("  [Outfit] Definido como Sleep Outfit: {}", rewardForm->GetName());
                        }
                        else {
                            for (auto* form : outfit->outfitItems) {
                                if (!form) continue;
                                auto* bound = form->As<RE::TESBoundObject>();
                                if (bound) {
                                    a_actor->AddObjectToContainer(bound, nullptr, 1, nullptr);
                                    auto actorHandle = a_actor->GetHandle();
                                    if (!isPlayer) equipQueue.push_back({ bound, 0 });
                                }
                            }
                        }
                    }
                }
                else {
                    // Weapon, Armor, Misc, Potion, Ingredient, Book, Ammo, etc.
                    if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
                        RE::ExtraDataList* xList = nullptr;
                        /*if (!reward.lootable) {
                            a_actor->AddObjectToContainer(bound, xList, reward.amount, nullptr);
                        }*/
                        a_actor->AddObjectToContainer(bound, nullptr, reward.amount, nullptr);
                        if (reward.typeReward == "Weapon" || reward.typeReward == "Armor" || reward.typeReward == "Ammo") {
                            if (!isPlayer) equipQueue.push_back({ bound, 1 });
                        }
                        logger::debug("  [Item] Adicionado: {} (Tipo: {})", rewardForm->GetName(), reward.typeReward);
                    }
                }
            }
            session.npcRuleVersions[npcKey][ruleID] = currentRule.version;
        }
    }
    if (!equipQueue.empty() && !isPlayer) {
        auto equipManager = RE::ActorEquipManager::GetSingleton();
        if (equipManager) {
            // Ordena: Prioridade 0 (Outfit) primeiro, 1 (Individual) depois.
            // Assim, o item individual sobrescreve o do outfit no mesmo slot.
            std::sort(equipQueue.begin(), equipQueue.end(), [](const PendingEquip& a, const PendingEquip& b) {
                return a.priority < b.priority;
                });

            for (auto& entry : equipQueue) {
                logger::debug("  [EquipQueue] Equipando: {} (Prio: {})", entry.object->GetName(), entry.priority);
                // IMPORTANTE: p_queueEquip (último param) como TRUE é essencial para evitar o ILS
                equipManager->EquipObject(a_actor, entry.object, nullptr, 1, nullptr, false, true, false, true);
            }
        }
    }
}


