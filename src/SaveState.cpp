#include "SaveState.h"
#include <mutex>
#include <set>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// Conjunto estático para rastrear atores em processamento
static std::mutex g_processingMutex;
static std::set<RE::FormID> g_actorsInProcess;

namespace {
    const rapidjson::Value* FindMember(const rapidjson::Value& obj, const char* key) {
        if (!obj.IsObject()) return nullptr;
        auto it = obj.FindMember(key);
        return it != obj.MemberEnd() ? &it->value : nullptr;
    }

    uint32_t GetJsonUint(const rapidjson::Value& value, uint32_t fallback = 0) {
        if (value.IsUint()) return value.GetUint();
        if (value.IsInt() && value.GetInt() >= 0) return static_cast<uint32_t>(value.GetInt());
        return fallback;
    }

    std::vector<std::string> ReadStringArray(const rapidjson::Value* value) {
        std::vector<std::string> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsString()) result.emplace_back(item.GetString());
        }
        return result;
    }

    rapidjson::Value WriteStringArray(const std::vector<std::string>& values, rapidjson::Document::AllocatorType& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& value : values) {
            rapidjson::Value jsonValue;
            jsonValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
            array.PushBack(jsonValue, alloc);
        }
        return array;
    }

    std::string SerializeJson(const rapidjson::Value& value) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }
}

SaveStateManager* SaveStateManager::GetSingleton() {
    static SaveStateManager instance;
    return &instance;
}

int GetPoolIndex(std::vector<std::string>& pool, const std::string& value) {
    auto it = std::find(pool.begin(), pool.end(), value);
    if (it != pool.end()) return static_cast<int>(std::distance(pool.begin(), it));
    pool.push_back(value);
    return static_cast<int>(pool.size() - 1);
}

std::vector<SaveHistoryEntry>& SaveStateManager::GetCharacterHistory(uint32_t characterID) {
    // Retorna a referência do histórico ou cria uma entrada vazia caso não exista
    return _characterHistory[characterID];
}

void SaveStateManager::SetCurrentContext(uint32_t a_charID, uint32_t a_saveNum) {
    logger::info("[SaveManager] Definindo contexto: CharID {:08X}, Save #{}", a_charID, a_saveNum);

    _currentContext.charID = a_charID;
    _currentContext.saveNumber = a_saveNum;
    _currentContext.isValid = true; // Só é válido se tivermos um ID de personagem

    _sessionData = SaveHistoryEntry{};
    _sessionData.saveNumber = a_saveNum;

    if (a_charID != 0) {
        if (!_characterHistory.contains(a_charID)) {
            LoadCharacterData(a_charID);
        }

        auto& history = _characterHistory[a_charID];
        SaveHistoryEntry* bestSnapshot = nullptr;

        for (auto& entry : history) {
            if (entry.saveNumber == a_saveNum) {
                _sessionData.npcRuleVersions = entry.npcRuleVersions;
                logger::info("[SaveManager] Contexto restaurado com sucesso do save #{}", a_saveNum);
                return;
            }
            if (entry.saveNumber < a_saveNum && (!bestSnapshot || entry.saveNumber > bestSnapshot->saveNumber)) {
                bestSnapshot = &entry;
            }
        }

        if (bestSnapshot) {
            _sessionData.npcRuleVersions = bestSnapshot->npcRuleVersions;
            logger::info("[SaveManager] Contexto herdado do save anterior #{}", bestSnapshot->saveNumber);
        }
    }
}

void SaveStateManager::PersistCurrentSave(const std::string& a_saveName) {

    if (_currentContext.isValid && _currentContext.charID == 0) {
        try {
            std::filesystem::path p(a_saveName);
            std::string fileName = p.filename().string();
            size_t first_u = fileName.find('_');
            size_t second_u = fileName.find('_', first_u + 1);
            if (first_u != std::string::npos && second_u != std::string::npos) {
                std::string idStr = fileName.substr(first_u + 1, second_u - first_u - 1);
                _currentContext.charID = static_cast<uint32_t>(std::stoul(idStr, nullptr, 16));
                logger::info("[SaveManager] Migrando contexto de New Game para Personagem: {:X}", _currentContext.charID);
            }
        }
        catch (...) {}
    }
    auto& context = _currentContext;
    // Log de debug para rastrear por que pode estar falhando
    if (!context.isValid) {
        logger::warn("[SaveManager] PersistCurrentSave abortado: Contexto inválido (CharID: {:08X})", context.charID);
        return;
    }

    uint32_t newSaveNumber = context.saveNumber;
    try {
        // Extrai o nome do arquivo (ex: quiksave.ess ou Save1_...)
        std::filesystem::path p(a_saveName);
        std::string fileName = p.filename().string();

        // Tenta extrair o número do save se for o formato padrão do Skyrim
        size_t savePos = fileName.find("Save");
        size_t underscorePos = fileName.find('_');
        if (savePos != std::string::npos && underscorePos != std::string::npos) {
            std::string numStr = fileName.substr(savePos + 4, underscorePos - (savePos + 4));
            newSaveNumber = std::stoul(numStr);
        }
    }
    catch (...) {
        logger::error("[SaveManager] Erro ao processar número do save do nome: {}", a_saveName);
    }

    _sessionData.saveNumber = newSaveNumber;
    logger::info("[SaveManager] Sincronizando dados para o Save #{}...", newSaveNumber);

    UpdateSaveEntry(context.charID, _sessionData);
    context.saveNumber = newSaveNumber; // Atualiza o contexto para o novo número
}

void SaveStateManager::ClearContext()
{
	logger::info("[SaveManager] Limpando contexto atual para CharID {:08X}", _currentContext.charID);
    _currentContext.isValid = false;
    _currentContext.saveNumber = 0;
    _currentContext.charID = 0;
    _sessionData = SaveHistoryEntry{};
}

std::string SaveStateManager::BuildNPCKey(RE::Actor* a_actor) {
    if (!a_actor) return "";
    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return "";

    std::string fileNameStr = "Dynamic";
    if (auto file = baseNPC->GetFile(0)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Created";
    }

    return fileNameStr + "|" + FormatLocalFormID(a_actor->GetFormID(), fileNameStr);
}

std::string SaveStateManager::GetCharacterPath(uint32_t characterID) {
    char hexID[9];
    sprintf_s(hexID, "%08X", characterID);
    return "Data/SKSE/Plugins/EDF/Saves/" + std::string(hexID) + ".json";
}

void SaveStateManager::LoadCharacterData(uint32_t characterID) {
    std::string path = GetCharacterPath(characterID);
    logger::info("[SaveManager] Tentando carregar arquivo: {}", path);

    std::ifstream i(path);
    if (!i.is_open()) return;

    try {
        rapidjson::IStreamWrapper stream(i);
        rapidjson::Document doc;
        doc.ParseStream(stream);
        if (doc.HasParseError()) {
            logger::error("[SaveManager] JSON inválido em {}.", path);
            return;
        }

        // --- DETECÇÃO E CONVERSÃO DE FORMATO ANTIGO ---
        if (doc.IsArray()) {
            logger::info("[SaveManager] Formato antigo detectado para {:08X}. Iniciando migração...", characterID);

            std::vector<SaveHistoryEntry> legacyHistory;
            for (const auto& oldEntryJson : doc.GetArray()) {
                if (!oldEntryJson.IsObject()) continue;

                SaveHistoryEntry entry;
                if (auto saveNumber = FindMember(oldEntryJson, "saveNumber")) {
                    entry.saveNumber = GetJsonUint(*saveNumber);
                }

                // No formato antigo, npcRuleVersions mapeava ruleID para um int (versão)
                auto oldNpcMap = FindMember(oldEntryJson, "npcRuleVersions");
                if (oldNpcMap && oldNpcMap->IsObject()) {
                    for (auto npcIt = oldNpcMap->MemberBegin(); npcIt != oldNpcMap->MemberEnd(); ++npcIt) {
                        if (!npcIt->name.IsString() || !npcIt->value.IsObject()) continue;

                        const std::string npcKey = npcIt->name.GetString();
                        for (auto ruleIt = npcIt->value.MemberBegin(); ruleIt != npcIt->value.MemberEnd(); ++ruleIt) {
                            if (!ruleIt->name.IsString()) continue;

                            AppliedRuleState state;
                            state.version = static_cast<int>(GetJsonUint(ruleIt->value));
                            state.appliedGroups = {}; // Histórico de grupos não existia no formato antigo
                            entry.npcRuleVersions[npcKey][ruleIt->name.GetString()] = state;
                        }
                    }
                }
                legacyHistory.push_back(entry);
            }

            _characterHistory[characterID] = legacyHistory;

            if (!legacyHistory.empty()) {
                UpdateSaveEntry(characterID, legacyHistory.back());
                logger::info("[SaveManager] Migração concluída com sucesso para o disco.");
            }
            return;
        }

        if (!doc.IsObject()) return;

        // --- CARREGAMENTO DO FORMATO NOVO (EXISTENTE) ---
        auto plugins = ReadStringArray(FindMember(doc, "p_plugins"));
        auto rules = ReadStringArray(FindMember(doc, "p_rules"));
        auto groups = ReadStringArray(FindMember(doc, "p_groups"));
        auto historyArray = FindMember(doc, "history");
        if (!historyArray || !historyArray->IsArray()) return;

        std::vector<SaveHistoryEntry> decodedHistory;
        for (const auto& h : historyArray->GetArray()) {
            if (!h.IsObject()) continue;

            SaveHistoryEntry entry;
            if (auto saveNumber = FindMember(h, "sn")) {
                entry.saveNumber = GetJsonUint(*saveNumber);
            }

            auto npcsJson = FindMember(h, "npcs");
            if (!npcsJson || !npcsJson->IsArray()) continue;

            for (const auto& npcItem : npcsJson->GetArray()) {
                if (!npcItem.IsArray() || npcItem.Size() < 3) continue;

                int pIdx = npcItem[0].IsInt() ? npcItem[0].GetInt() : -1;
                uint32_t fID = GetJsonUint(npcItem[1]);
                if (pIdx < 0 || static_cast<size_t>(pIdx) >= plugins.size()) continue;

                std::string npcKey = plugins[pIdx] + "|" + FormatLocalFormID(fID, plugins[pIdx]);
                const auto& rulesMap = npcItem[2];
                if (!rulesMap.IsObject()) continue;

                for (auto ruleIt = rulesMap.MemberBegin(); ruleIt != rulesMap.MemberEnd(); ++ruleIt) {
                    if (!ruleIt->name.IsString() || !ruleIt->value.IsArray() || ruleIt->value.Size() < 2) continue;

                    int rIdx = std::stoi(ruleIt->name.GetString());
                    if (rIdx < 0 || static_cast<size_t>(rIdx) >= rules.size()) continue;

                    const auto& rData = ruleIt->value;
                    AppliedRuleState state;
                    state.version = rData[0].IsInt() ? rData[0].GetInt() : 0;
                    if (rData[1].IsArray()) {
                        for (const auto& groupIndex : rData[1].GetArray()) {
                            int gIdx = groupIndex.IsInt() ? groupIndex.GetInt() : -1;
                            if (gIdx >= 0 && static_cast<size_t>(gIdx) < groups.size()) state.appliedGroups.push_back(groups[gIdx]);
                        }
                    }
                    entry.npcRuleVersions[npcKey][rules[rIdx]] = state;
                }
            }
            decodedHistory.push_back(entry);
        }
        _characterHistory[characterID] = decodedHistory;
        logger::info("[SaveManager] Carregamento concluído. {} snapshots em memória.", decodedHistory.size());
    }
    catch (const std::exception& e) {
        logger::error("[SaveManager] Erro ao carregar/converter save: {}", e.what());
    }
}
void SaveStateManager::UpdateSaveEntry(uint32_t characterID, const SaveHistoryEntry& newEntry) {
    auto& history = _characterHistory[characterID];

    // Atualiza cache em memória
    auto it = std::find_if(history.begin(), history.end(), [&](const SaveHistoryEntry& e) {
        return e.saveNumber == newEntry.saveNumber;
        });
    if (it != history.end()) {
        *it = newEntry;
        logger::debug("[SaveManager] Atualizando snapshot existente #{} em memória.", newEntry.saveNumber);
    }
    else {
        history.push_back(newEntry);
        logger::debug("[SaveManager] Adicionando novo snapshot #{} ao histórico.", newEntry.saveNumber);
    }

    logger::info("[SaveManager] Iniciando compressão para escrita (Save #{})", newEntry.saveNumber);

    // --- COMPRESSÃO ---
    std::vector<std::string> p_plugins, p_rules, p_groups;

    try {
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();

        rapidjson::Value jHistory(rapidjson::kArrayType);
        for (const auto& entry : history) {
            rapidjson::Value jEntry(rapidjson::kObjectType);
            jEntry.AddMember("sn", entry.saveNumber, alloc);

            rapidjson::Value jNPCs(rapidjson::kArrayType);
            for (auto const& [npcKey, ruleMap] : entry.npcRuleVersions) {
                auto tokens = split(npcKey, '|');
                if (tokens.size() < 2) continue;

                rapidjson::Value jNPC(rapidjson::kArrayType);
                jNPC.PushBack(GetPoolIndex(p_plugins, tokens[0]), alloc);
                jNPC.PushBack(static_cast<uint32_t>(std::stoul(tokens[1], nullptr, 16)), alloc);

                rapidjson::Value jRules(rapidjson::kObjectType);
                for (auto const& [ruleID, state] : ruleMap) {
                    rapidjson::Value jState(rapidjson::kArrayType);
                    jState.PushBack(state.version, alloc);

                    rapidjson::Value jGrp(rapidjson::kArrayType);
                    for (const auto& gName : state.appliedGroups) {
                        jGrp.PushBack(GetPoolIndex(p_groups, gName), alloc);
                    }
                    jState.PushBack(jGrp, alloc);

                    const auto ruleIndex = std::to_string(GetPoolIndex(p_rules, ruleID));
                    rapidjson::Value ruleKey;
                    ruleKey.SetString(ruleIndex.c_str(), static_cast<rapidjson::SizeType>(ruleIndex.size()), alloc);
                    jRules.AddMember(ruleKey, jState, alloc);
                }
                jNPC.PushBack(jRules, alloc);
                jNPCs.PushBack(jNPC, alloc);
            }
            jEntry.AddMember("npcs", jNPCs, alloc);
            jHistory.PushBack(jEntry, alloc);
        }

        doc.AddMember("p_plugins", WriteStringArray(p_plugins, alloc), alloc);
        doc.AddMember("p_rules", WriteStringArray(p_rules, alloc), alloc);
        doc.AddMember("p_groups", WriteStringArray(p_groups, alloc), alloc);
        doc.AddMember("history", jHistory, alloc);

        std::string path = GetCharacterPath(characterID);
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());

        std::ofstream o(path);
        if (o.is_open()) {
            o << SerializeJson(doc);
            o.close();
            logger::info("[SaveManager] Arquivo de histórico atualizado com sucesso: {}", path);
        }
        else {
            logger::error("[SaveManager] Erro crítico: Não foi possível abrir o arquivo para escrita: {}", path);
        }
    }
    catch (const std::exception& e) {
        logger::error("[SaveManager] Exceção durante a compressão/escrita do JSON: {}", e.what());
    }
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
            equipManager->EquipObject(a_actor, item, extraData, 1, nullptr, false, false, false, true);

            logger::debug("  [Tentativa] Equipando item '{}' (Rating: {}).",
                item->GetName(), item->As<RE::TESObjectARMO>()->GetArmorRating());
        }
    }

    a_actor->Update3DModel();
    logger::debug("[OutfitSync] Equipamento para {}. Verificação completa.", a_actor->GetName());
}

bool IsActorSleeping(RE::Actor* a_actor) {
    if (!a_actor) return false;
    auto actorState = a_actor->AsActorState();
    if (!actorState) return false;

    auto sitSleepState = actorState->GetSitSleepState();
    return (sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping);
}

RE::BGSOutfit* GetAppliedSleepOutfit(RE::Actor* a_actor) {
    auto& manager = *SaveStateManager::GetSingleton();
    auto& session = manager.GetSessionData();

    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return nullptr;

    // Gerar npcKey consistente com a lógica de salvamento
    RE::FormID actorID = a_actor->GetFormID();
    std::string fileNameStr = "Dynamic";
    if (auto file = baseNPC->GetFile(0)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Created";
    }

    std::string npcKey = fileNameStr + "|" + FormatLocalFormID(actorID, fileNameStr);

    if (!session.npcRuleVersions.contains(npcKey)) return nullptr;

    // Percorre as regras aplicadas ao NPC
    for (auto const& [ruleID, state] : session.npcRuleVersions[npcKey]) {
        auto rule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, state.version);
        if (!rule) continue;

        // Verifica os grupos que o NPC ganhou no sorteio
        for (const auto& groupName : state.appliedGroups) {
            for (const auto& group : rule->rewardGroups) {
                if (group.name == groupName) {
                    for (const auto& reward : group.rewards) {
                        // Verifica se é Outfit e se o modo inclui Special/Sleep (1 ou 2)
                        if (reward.typeReward == "Outfit" && (reward.functionOnType == 1 || reward.functionOnType == 2)) {
                            auto [plugin, fID] = reward.ParseFormID();
                            if (auto outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(fID)) return outfit;
                        }
                    }
                }
            }
        }
    }
    return nullptr;
}

// --- NOVA FUNÇÃO: Gerencia a troca física de itens ---
void ManageSleepOutfitState(RE::Actor* a_actor, bool a_isEntering) {
    if (!a_actor) return;
    auto equipManager = RE::ActorEquipManager::GetSingleton();
    auto sleepOutfit = GetAppliedSleepOutfit(a_actor);
    if (!sleepOutfit || !equipManager) return;

    if (a_isEntering) {
        logger::info("[SleepSystem] Actor {} entrando na cama. Equipando traje de sono.", a_actor->GetName());
        // 1. Desequipar itens de armadura atuais
        auto inventory = a_actor->GetInventory();
        for (auto& [item, invData] : inventory) {
            if (item->IsArmor() && invData.second->IsWorn()) {
                equipManager->UnequipObject(a_actor, item);
            }
        }
        // 2. Equipar itens do Sleep Outfit
        for (auto* form : sleepOutfit->outfitItems) {
            if (auto* bound = form->As<RE::TESBoundObject>()) {
                equipManager->EquipObject(a_actor, bound, nullptr, 1, nullptr, false, false, false, true);
            }
        }
    }
    else {
        logger::info("[SleepSystem] Actor {} saindo da cama. Restaurando equipamento.", a_actor->GetName());
        // 1. Remover/Desequipar o traje de sono
        for (auto* form : sleepOutfit->outfitItems) {
            if (auto* bound = form->As<RE::TESBoundObject>()) {
                equipManager->UnequipObject(a_actor, bound);
            }
        }
        // 2. Equipar as melhores opções do inventário
        EquipBestInventoryItems(a_actor);
    }
}

struct PendingEquip {
    RE::TESBoundObject* object;
    int priority; // 0 para Outfit (Baixa), 1 para Recompensas Individuais (Alta)
};

void RemoveRuleRewards(RE::Actor* a_actor, const Rule& a_rule) {
    if (!a_actor) return;

    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return;
    bool isPlayer = a_actor->IsPlayer();
    for (const auto& group : a_rule.rewardGroups) {
        for (const auto& reward : group.rewards) {
            if (reward.isPersistent) continue;
            auto [plugin, fID] = reward.ParseFormID();
            auto rewardForm = RE::TESForm::LookupByID(fID);
            if (!rewardForm) continue;

            if (reward.typeReward == "Spell") {
                if (auto spell = rewardForm->As<RE::SpellItem>()) {

                    // 1. Dispel (Para functionOnType 1 ou 2) - SUGERIDO POR VOCÊ
                    if (reward.functionOnType == 1 || reward.functionOnType == 2) {
                        if (auto activeEffectList = a_actor->AsMagicTarget()->GetActiveEffectList()) {
                            for (auto& effect : *activeEffectList) {
                                if (effect && effect->spell == spell) {
                                    effect->Dispel(true);
                                    logger::debug("[Dispel] Efeito de '{}' removido de {}.", spell->GetName(), a_actor->GetName());
                                }
                            }
                        }
                    }

                    // 2. RemoveSpell (Para functionOnType 0 ou 2)
                    if (reward.functionOnType == 0 || reward.functionOnType == 2) {
                        // Se for NPC: Remove apenas se não for nativo do ActorBase
                        if (!isPlayer) {
                            if (!baseNPC->GetSpellList()->GetIndex(spell).has_value()) {
                                a_actor->RemoveSpell(spell);
                            }
                        }

                    }
                }
            }
            //else if (reward.typeReward == "Package") {
            //    if (auto pkg = rewardForm->As<RE::TESPackage>()) {
            //        auto& packageList = baseNPC->aiPackages.packages;

            //        // Verifica se o pacote já está na lista (conforme visto no Distribute.cpp)
            //        if (std::ranges::find(packageList, pkg) == packageList.end()) {
            //            // Adicionamos ao início (push_front) para que ele tenha prioridade sobre pacotes padrão
            //            packageList.push_front(pkg);

            //            // Força o NPC a reavaliar sua pilha de pacotes imediatamente
            //            a_actor->EvaluatePackage(false, false);
            //            logger::debug("[ApplyRules] Package {} adicionado ao BaseNPC {}", reward.formIDStr, baseNPC->GetName());
            //        }
            //    }
            //}
            else if (reward.typeReward == "Perk") {
                if (auto perk = rewardForm->As<RE::BGSPerk>()) {
                    if (!isPlayer) { // Proteção extra para o Player
                        bool isNative = false;
                        for (uint32_t i = 0; i < baseNPC->perkCount; i++) {
                            if (baseNPC->perks[i].perk == perk) { isNative = true; break; }
                        }
                        if (!isNative) a_actor->RemovePerk(perk);
                    }
                }
            }
            else if (reward.typeReward == "Outfit") {
                if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                    // 1: Sleep Outfit, 2: Both
                    if (reward.functionOnType == 1 || reward.functionOnType == 2) {
                        // Se o outfit de sono atual for o da regra, resetamos para null
                        if (a_actor->GetActorBase()->sleepOutfit == outfit) {
                            a_actor->SetSleepOutfit(nullptr, false);
                        }
                    }

                    // 0: Normal (Inventory), 2: Both
                    if (reward.functionOnType == 0 || reward.functionOnType == 2) {
                        for (auto* form : outfit->outfitItems) {
                            if (auto* bound = form->As<RE::TESBoundObject>()) {
                                // Remove apenas 1 unidade (assumindo que foi o que a regra deu)
                                a_actor->RemoveItem(bound, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                            }
                        }
                    }
                }
            }
            else {
                // Itens genéricos (Weapon, Armor, Ammo, etc)
                if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
                    // Itens são mais difíceis de rastrear se são "nativos",
                    // mas removemos a quantidade estipulada pela regra.
                    a_actor->RemoveItem(bound, reward.amount, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                }
            }
        }
    }
    logger::debug("[LocationUpdate] Regra '{}' desaplicada de {}. Atributos nativos preservados.", a_rule.name, a_actor->GetName());
}

void ApplyRewardPhysical(RE::Actor* a_actor, const Reward& reward, bool isPlayer, std::vector<PendingEquip>& equipQueue) {
    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    if (reward.typeReward == "Spell") {
        if (auto spell = rewardForm->As<RE::SpellItem>()) {
            if (reward.functionOnType == 0 || reward.functionOnType == 2) {
                if (!a_actor->HasSpell(spell)) a_actor->AddSpell(spell);
            }
            if (reward.functionOnType == 1 || reward.functionOnType == 2) {
                auto caster = a_actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (caster) caster->CastSpellImmediate(spell, false, a_actor, 1.0f, false, 0.0f, a_actor);
            }
        }
    }
    else if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            // Sempre adicionamos os itens ao inventário do NPC
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    a_actor->AddObjectToContainer(bound, nullptr, 1, nullptr);
                    // Só adicionamos à fila de equipagem imediata se o traje permitir
                    // uso padrão (Standard ou Both)
                    if (!isPlayer && (reward.functionOnType == 0 || reward.functionOnType == 2)) {
                        equipQueue.push_back({ bound, 0 });
                    }
                }
            }
        }
    }
    else if (reward.typeReward == "Perk") {
        if (auto perk = rewardForm->As<RE::BGSPerk>()) { if (!a_actor->HasPerk(perk)) a_actor->AddPerk(perk, 1); }
    }
    else {
        // Itens físicos (Weapon, Armor, etc)
        if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
            a_actor->AddObjectToContainer(bound, nullptr, reward.amount, nullptr);
            if (!isPlayer && (reward.typeReward == "Weapon" || reward.typeReward == "Armor" || reward.typeReward == "Ammo")) {
                equipQueue.push_back({ bound, 1 });
            }
        }
    }
}
void DebugLogInventory(RE::Actor* a_actor) {
    if (!a_actor) return;
    auto inventory = a_actor->GetInventory();
    logger::debug("  [Inventory Audit] '{}' ({:08X}) tem {} itens no container:",
        a_actor->GetName(), a_actor->GetFormID(), inventory.size());

    for (auto& [item, invData] : inventory) {
        auto count = invData.first;
        if (count > 0) {
            logger::debug("    - [{:08X}] {} (Qtd: {})", item->GetFormID(), item->GetName(), count);
        }
    }
}
void ApplyRulesToInstance(RE::Actor* a_actor, int a_forcedLevel) {
    if (!a_actor) return;

    // --- LOG DE INÍCIO ---
    std::string actorName = a_actor->GetName();
    RE::FormID actorID = a_actor->GetFormID();

    {
        std::lock_guard<std::mutex> lock(g_processingMutex);
        if (g_actorsInProcess.contains(actorID)) {
            // Se já estiver processando este ator, ignora a nova chamada para evitar conflitos
            // logger::debug("[ApplyRules] Ignorando chamada duplicada para {:08X} (em processamento)", actorID);
            return;
        }
        g_actorsInProcess.insert(actorID);
    }

    // Garantir que o ID seja removido do conjunto ao sair da função (RAII)
    struct ProcessCleaner {
        RE::FormID id;
        ~ProcessCleaner() {
            std::lock_guard<std::mutex> lock(g_processingMutex);
            g_actorsInProcess.erase(id);
        }
    } cleaner{ actorID };
    //logger::debug("[ApplyRules] INICIANDO processamento para {} (ID: {:08X})", actorName, actorID);

    if (a_actor->IsDead()) {
        //logger::info("[ApplyRules] FINALIZADO: Ator está morto.");
        return;
    }

    bool isPlayer = (actorID == 0x00000014) || a_actor->IsPlayer();
    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) {
        logger::debug("[ApplyRules] FINALIZADO: BaseNPC não encontrado.");
        return;
    }

    auto& context = SaveStateManager::GetSingleton()->GetCurrentContext();
    if (!context.isValid) {
        logger::debug("[ApplyRules] FINALIZADO: Contexto de save inválido.");
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
                //it = appliedRulesMap.erase(it);
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
        logger::debug("[ApplyRules] FINALIZADO: Nenhuma regra aplicável para {}", actorName);
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


        AppliedRuleState& state = session.npcRuleVersions[npcKey][ruleID];
        int oldVersion = state.version;

        if (oldVersion == 0) {
            logger::debug("  [Novo NPC] Aplicando regra '{}' pela primeira vez em {}.", currentRule.name, actorName);
            // --- 1. APLICAÇÃO INICIAL (NPC Novo) ---
            std::vector<RewardGroup> wonGroups = RuleManager::GetSingleton()->RollForGroups(baseNPC, currentRule);
            for (const auto& group : wonGroups) {
                state.appliedGroups.push_back(group.name);
                // Se o grupo for exclusivo, sorteia apenas UM item
                if (group.isExclusive) {
                    float roll = RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f);
                    float cumulative = 0.0f;
                    for (const auto& reward : group.rewards) {
                        cumulative += reward.chanceReward;
                        if (roll <= cumulative) {
                            ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                            break;
                        }
                    }
                }
                else {
                    // Grupo normal: sorteia cada item individualmente
                    for (const auto& reward : group.rewards) {
                        if (RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f) <= reward.chanceReward) {
                            ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                        }
                    }
                }
            }
            state.version = currentRule.version;
        }
        else if (oldVersion < currentRule.version) {

            // --- 2. ATUALIZAÇÃO INCREMENTAL (Versão subiu) ---
            auto oldRule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, oldVersion);
            logger::debug("  [Update] Atualizando {} da versão {} para {}.", actorName, oldVersion, currentRule.version);
            for (const auto& group : currentRule.rewardGroups) {
                auto it = std::find(state.appliedGroups.begin(), state.appliedGroups.end(), group.name);
                bool wasAlreadyApplied = (it != state.appliedGroups.end());

                if (wasAlreadyApplied) {
                    // LÓGICA UNIFICADA PARA GRUPO JÁ APLICADO
                    if (group.isExclusive) {
                        // Caso 1: Grupo Exclusivo - Nada novo entra, apenas mantém o estado visual
                        for (const auto& reward : group.rewards) {
                            if (!reward.isPersistent) ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                        }
                    }
                    else {
                        // Caso 2: Grupo NÃO Exclusivo - Permite novos itens da nova versão
                        for (const auto& reward : group.rewards) {
                            bool isNewReward = true;
                            if (oldRule) {
                                for (const auto& oldG : oldRule->rewardGroups) {
                                    if (oldG.name == group.name) {
                                        for (const auto& oldR : oldG.rewards) {
                                            if (oldR.formIDStr == reward.formIDStr) { isNewReward = false; break; }
                                        }
                                    }
                                }
                            }

                            if (isNewReward) {
                                if (RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f) <= reward.chanceReward) {
                                    ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                                }
                            }
                            else if (!reward.isPersistent) {
                                ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                            }
                        }
                    }
                }
                else if (!currentRule.isExclusive) {
                    // Grupo NOVO em regra NÃO exclusiva: Sorteia o grupo inteiro
                    if (RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f) <= group.chanceGroup) {
                        state.appliedGroups.push_back(group.name);
                        // Aplica itens do grupo novo (respeitando exclusividade do grupo se houver)
                        if (group.isExclusive) {
                            float roll = RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f);
                            float cumulative = 0.0f;
                            for (const auto& reward : group.rewards) {
                                cumulative += reward.chanceReward;
                                if (roll <= cumulative) {
                                    ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                                    break;
                                }
                            }
                        }
                        else {
                            for (const auto& reward : group.rewards) {
                                if (RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f) <= reward.chanceReward) {
                                    ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                                }
                            }
                        }
                    }
                }
            }
            state.version = currentRule.version;
        }
        else if (oldVersion == currentRule.version) {
            // --- 3. RE-ENTRADA (Mesma Versão) ---
            for (const auto& groupName : state.appliedGroups) {
                for (const auto& group : currentRule.rewardGroups) {
                    if (group.name == groupName) {
                        for (const auto& reward : group.rewards) {
                            if (!reward.isPersistent) {
                                ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                            }
                            else if (!isPlayer) {
                                auto [plugin, fID] = reward.ParseFormID();
                                auto rewardForm = RE::TESForm::LookupByID(fID);
                                if (!rewardForm) continue;

                                bool needsRestoration = false;
                                a_actor->InitInventoryIfRequired(); //

                                if (reward.typeReward == "Outfit") {
                                    auto outfit = rewardForm->As<RE::BGSOutfit>();
                                    if (outfit) {
                                        for (auto* item : outfit->outfitItems) {
                                            if (auto* bound = item->As<RE::TESBoundObject>()) {
                                                // LOG DE AUDITORIA: Mostra o que o motor do Skyrim está reportando
                                                int currentCount = 0;
                                                auto invChanges = a_actor->GetInventoryChanges(); // Busca as mudanças reais da instância

                                                if (invChanges && invChanges->entryList) {
                                                    for (auto* entry : *invChanges->entryList) {
                                                        if (entry && entry->object == bound) {
                                                            currentCount = entry->countDelta; // Quantos itens foram adicionados/removidos desta instância específica
                                                            break;
                                                        }
                                                    }
                                                }
                                                else {
                                                    // Se não há InventoryChanges, a instância está "limpa" (mesmo que a base diga que tem itens)
                                                    currentCount = 0;
                                                }

                                                logger::debug("  [Audit Real] NPC: {} | Item: '{}' | Delta Real: {}", actorName, bound->GetName(), currentCount);

                                                if (currentCount <= 0) {
                                                    // Se o Delta for 0, o NPC da instância atual NÃO tem o item, independente do que a base diga
                                                    needsRestoration = true;
                                                }
                                            }
                                        }
                                    }
                                }
                                else {
                                    auto boundObj = rewardForm->As<RE::TESBoundObject>();
                                    if (boundObj) {
                                        int currentCount = a_actor->GetInventoryCount(boundObj);
                                        logger::debug("  [Audit] NPC: {} | Item: '{}' | GetInventoryCount: {}",
                                            actorName, boundObj->GetName(), currentCount);

                                        if (currentCount <= 0) {
                                            logger::info("  [Re-entrada] NPC {} perdeu item persistente '{}'. Restaurando...",
                                                actorName, boundObj->GetName());
                                            needsRestoration = true;
                                        }
                                    }
                                }

                                if (needsRestoration) {
                                    ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
                                }
                            }
                        }
                    }
                }
            }
        }

        // Equipagem final
        if (!equipQueue.empty() && !isPlayer) {
            logger::debug("[EquipManager] Iniciando processamento da fila para {}. Itens na fila: {}", actorName, equipQueue.size());
            auto equipManager = RE::ActorEquipManager::GetSingleton();
            if (equipManager) {
                // 1. Ordena por prioridade
                std::sort(equipQueue.begin(), equipQueue.end(), [](const PendingEquip& a, const PendingEquip& b) {
                    return a.priority < b.priority;
                    });

                auto inventory = a_actor->GetInventory();

                for (auto& entry : equipQueue) {
                    if (entry.object->IsArmor()) {
                        auto newArmor = entry.object->As<RE::TESObjectARMO>();
                        auto newSlotMask = static_cast<uint32_t>(newArmor->GetSlotMask());

                        for (auto& [item, invData] : inventory) {
                            if (invData.second->IsWorn() && item->IsArmor()) {
                                auto wornArmor = item->As<RE::TESObjectARMO>();
                                if (wornArmor && (static_cast<uint32_t>(wornArmor->GetSlotMask()) & newSlotMask)) {
                                    if (item->GetFormID() != entry.object->GetFormID()) {
                                        logger::debug("  [EquipManager] Desequipando '{}' para liberar slot para '{}'", item->GetName(), entry.object->GetName());
                                        equipManager->UnequipObject(a_actor, item);
                                    }
                                }
                            }
                        }
                    }

                    logger::debug("  [EquipManager] Comandando motor: Equipar '{}' em {}", entry.object->GetName(), actorName);
                    // Usando force=true (penúltimo parâmetro) para garantir que NPCs como Athis/Njada ignorem o Outfit padrão
                    equipManager->EquipObject(a_actor, entry.object, nullptr, 1, nullptr, false, false, false, true);
                }
            }
        }

        if (IsActorSleeping(a_actor)) {
            auto sleepOutfit = GetAppliedSleepOutfit(a_actor);
            if (sleepOutfit) {
                // Verifica se já está vestindo para evitar equipagem redundante
                bool alreadyWearing = false;
                if (!sleepOutfit->outfitItems.empty()) {
                    if (auto item = sleepOutfit->outfitItems[0]->As<RE::TESBoundObject>()) {
                        auto inv = a_actor->GetInventory();
                        if (inv.contains(item) && inv[item].second->IsWorn()) alreadyWearing = true;
                    }
                }
                if (!alreadyWearing) {
                    logger::info("[SleepSystem] NPC {} carregado em estado de sono. Aplicando traje.", a_actor->GetName());
                    ManageSleepOutfitState(a_actor, true);
                }
            }
        }
        logger::debug(" [Regras aplicadas] {} (Save #{}) - Regra: '{}', Versão: {}, Grupos Aplicados: {}",
			actorName, session.saveNumber, currentRule.name, state.version, fmt::join(state.appliedGroups, ", "));
        // --- LOG DE FINALIZAÇÃO ---
        logger::debug("[ApplyRules] FINALIZADO com sucesso para {} ({:08X})", actorName, actorID);
    }
}
