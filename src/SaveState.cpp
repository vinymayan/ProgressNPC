#include "SaveState.h"

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

void ApplyRulesToInstance(RE::Actor* a_actor)
{
    if (!a_actor || a_actor->IsDead()) return;

    auto baseNPC = a_actor->GetActorBase();
    auto& context = SaveStateManager::GetSingleton()->GetCurrentContext();
    if (!context.isValid || !baseNPC) return;

    auto& history = SaveStateManager::GetSingleton()->GetCharacterHistory(context.charID);
    std::string actorName = a_actor->GetName();

    logger::debug("[ApplyInstance] Processando Ator: {} (Nivel: {})", actorName, a_actor->GetLevel());

    // 1. Busca Snapshot Retroativo
    SaveHistoryEntry* bestSnapshot = nullptr;
    for (auto& entry : history) {
        if (entry.saveNumber < context.saveNumber) {
            if (!bestSnapshot || entry.saveNumber > bestSnapshot->saveNumber) {
                bestSnapshot = &entry;
            }
        }
    }
    if (bestSnapshot) logger::debug("[ApplyInstance] Snapshot de referencia encontrado: Save {}", bestSnapshot->saveNumber);

    // 2. Garantir Entrada Atual
    SaveHistoryEntry* currentEntry = nullptr;
    for (auto& entry : history) {
        if (entry.saveNumber == context.saveNumber) { currentEntry = &entry; break; }
    }
    if (!currentEntry) {
        SaveHistoryEntry newE;
        newE.saveNumber = context.saveNumber;
        if (bestSnapshot) {
            newE.npcAppliedRules = bestSnapshot->npcAppliedRules;
            logger::debug("[ApplyInstance] Herdando {} registros de aplicacao do snapshot anterior.", newE.npcAppliedRules.size());
        }
        history.push_back(newE);
        currentEntry = &history.back();
    }

    std::string fileNameStr(baseNPC->GetFile(0)->GetFilename());
    std::string npcKey = fileNameStr + "|" + FormatLocalFormID(a_actor->GetFormID(), fileNameStr);

    const auto& affectedDB = RuleManager::GetSingleton()->GetAffectedNPCsDatabase();
    auto it = affectedDB.find(baseNPC->GetFormID());
    if (it == affectedDB.end()) {
        logger::debug("[ApplyInstance] NPC {} nao possui regras associadas no banco.", actorName);
        return;
    }

    for (const auto& ruleID : it->second.ruleIDs) {
        auto& rules = RuleManager::GetSingleton()->GetRules();
        auto ruleIt = std::find_if(rules.begin(), rules.end(), [&](const Rule& r) { return r.id == ruleID; });

        if (ruleIt == rules.end()) continue;

        // CHECKAGEM DE NÍVEL: O NPC é mapeado independente do nível, mas só recebe se o Ator tiver nível suficiente
        if (a_actor->GetLevel() < ruleIt->level) {
            logger::debug("[ApplyInstance] Ator {} nivel {} insuficiente para Regra '{}' (Req: {})",
                actorName, a_actor->GetLevel(), ruleIt->name, ruleIt->level);
            continue;
        }

        bool needsApplication = false;
        auto& appliedRulesInSave = currentEntry->npcAppliedRules[npcKey];
        bool alreadyApplied = std::find(appliedRulesInSave.begin(), appliedRulesInSave.end(), ruleID) != appliedRulesInSave.end();

        if (!alreadyApplied) {
            if (bestSnapshot) {
                auto oldRuleIt = std::find_if(bestSnapshot->appliedRulesSnapshot.begin(),
                    bestSnapshot->appliedRulesSnapshot.end(),
                    [&](const Rule& r) { return r.id == ruleID; });

                if (oldRuleIt != bestSnapshot->appliedRulesSnapshot.end()) {
                    // Usa o HASH e a estrutura de grupos para decidir se reaplica
                    if (RuleProcessor::ShouldReapplyRule(*oldRuleIt, *ruleIt)) {
                        needsApplication = true;
                        logger::debug("[ApplyInstance] REAPLICANDO: Regra '{}' foi modificada no banco de dados.", ruleIt->name);
                    }
                }
                else {
                    needsApplication = true; // Nova regra adicionada desde o último save
                }
            }
            else {
                needsApplication = true; // Novo personagem ou primeiro registro
            }
        }

        if (needsApplication) {
            // Aplica os rewards e registra
            logger::debug("[ApplyInstance] Aplicando Regra '{}' ao Ator '{}'", ruleIt->name, actorName);
            std::vector<Reward> rewards = RuleManager::GetSingleton()->GetRewardsForSpecificRule(baseNPC, *ruleIt);
            for (const auto& reward : rewards) {
                auto [plugin, fID] = reward.ParseFormID();
                auto rewardForm = RE::TESForm::LookupByID(fID);
                if (!rewardForm) continue;

                if (reward.typeReward == "Spell") {
                    if (auto spell = rewardForm->As<RE::SpellItem>()) a_actor->AddSpell(spell);
                }
                else if (reward.typeReward == "Perk") {
                    if (auto perk = rewardForm->As<RE::BGSPerk>()) a_actor->AddPerk(perk, 0);
                }
                else if (reward.typeReward == "Weapon" || reward.typeReward == "Armor") {
                    if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
                        // 1. Adiciona o item ao inventário
                        a_actor->AddObjectToContainer(bound, nullptr, reward.amount, nullptr);

                        // 2. FORÇA O NPC A EQUIPAR O ITEM IMEDIATAMENTE
                        auto equipManager = RE::ActorEquipManager::GetSingleton();
                        if (equipManager) {
                            // O parâmetro 'nullptr' usa o slot padrão do item
                            // O 'true' força a equipagem mesmo se ele tiver algo similar
                            equipManager->EquipObject(a_actor, bound, nullptr, 1, nullptr, true, false, false, true);

                            logger::debug("[ApplyInstance] Item {} equipado forçadamente em {}",
                                bound->GetName(), a_actor->GetName());
                        }
                    }
                }
            }

            if (std::find(appliedRulesInSave.begin(), appliedRulesInSave.end(), ruleID) == appliedRulesInSave.end()) {
                appliedRulesInSave.push_back(ruleID);
            }
        }
        else {
            logger::debug("[ApplyInstance] Regra '{}' ja consta como aplicada para {} no Save {}", ruleIt->name, actorName, context.saveNumber);
        }
    }
}

RE::BGSHeadPart* HeadPartCreator::CreateHeadPartFromJson(const json& a_data)
{
    std::string targetEDID = a_data.value("editorID", "MISSING_EDID");
    SKSE::log::info("==============================================");
    SKSE::log::info("Iniciando criação dinâmica de HeadPart: '{}'", targetEDID);

    // 1. Criação da Instância (Heap)
    // Utiliza o template helper do ConcreteFormFactory.h
    auto newPart = RE::IFormFactory::Create<RE::BGSHeadPart>();

    if (!newPart) {
        SKSE::log::critical("CRITICO: Falha ao obter a ConcreteFormFactory para HeadPart ou falha na alocação de memória.");
        return nullptr;
    }
    SKSE::log::info("Sucesso: Instância base de BGSHeadPart alocada no heap (FormID temporário: {:X})", newPart->GetFormID());


    // 2. Configuração do EditorID
    if (a_data.contains("editorID")) {
        std::string edid = a_data["editorID"];
        newPart->SetFormEditorID(edid.c_str());
        SKSE::log::info(" - EditorID definido para: {}", edid);
    }
    else {
        SKSE::log::warn(" - AVISO: JSON sem 'editorID'. A parte não terá nome interno.");
    }


    // 3. Configuração do Tipo (Hair, Eyes, etc.)
    if (a_data.contains("type")) {
        uint32_t typeVal = a_data["type"].get<uint32_t>();
        newPart->type = static_cast<RE::BGSHeadPart::HeadPartType>(typeVal);
        // Enum simples para log legível
        std::string typeStr = "Unknown";
        switch (newPart->type.get()) {
        case RE::BGSHeadPart::HeadPartType::kMisc: typeStr = "Misc"; break;
        case RE::BGSHeadPart::HeadPartType::kFace: typeStr = "Face"; break;
        case RE::BGSHeadPart::HeadPartType::kEyes: typeStr = "Eyes"; break;
        case RE::BGSHeadPart::HeadPartType::kHair: typeStr = "Hair"; break;
        case RE::BGSHeadPart::HeadPartType::kFacialHair: typeStr = "Beard"; break;
        case RE::BGSHeadPart::HeadPartType::kEyebrows: typeStr = "Eyebrows"; break;
        }
        SKSE::log::info(" - Tipo definido para: {} ({})", typeVal, typeStr);
    }
    else {
        SKSE::log::error(" - ERRO: JSON faltando campo obrigatório 'type'.");
    }


    // 4. Configuração do Modelo (.nif)
    if (a_data.contains("modelPath")) {
        std::string path = a_data["modelPath"];
        newPart->SetModel(path.c_str());
        SKSE::log::info(" - Model Path definido para: '{}'", path);
        // Verificação básica se o caminho parece válido (não verifica existência do arquivo)
        if (path.length() < 5 || path.find(".nif") == std::string::npos) {
            SKSE::log::warn("   - AVISO: O caminho do modelo parece suspeito ou incompleto.");
        }
    }
    else {
        SKSE::log::error(" - ERRO: JSON faltando campo obrigatório 'modelPath'. A parte será invisível.");
    }


    // 5. Configuração das Flags
    if (a_data.contains("flags")) {
        uint8_t flagsVal = a_data["flags"].get<uint8_t>();
        newPart->flags = static_cast<RE::BGSHeadPart::Flag>(flagsVal);
        SKSE::log::info(" - Flags bitmask definida para: {:X} (decimal: {})", flagsVal, flagsVal);
    }


    // 6. Configuração da Lista de Raças Válidas
    if (a_data.contains("validRacesListID") && a_data.contains("validRacesSource")) {
        std::string formIdStr = a_data["validRacesListID"];
        std::string sourceMod = a_data["validRacesSource"];

        SKSE::log::info(" - Tentando resolver Lista de Raças: ID '{}' em '{}'", formIdStr, sourceMod);

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (dataHandler) {
            // Converte string hex para numérico e busca
            RE::FormID localID = std::stoul(formIdStr, nullptr, 16);
            auto raceList = dataHandler->LookupForm<RE::BGSListForm>(localID, sourceMod);

            if (raceList) {
                newPart->validRaces = raceList;
                SKSE::log::info("   - Sucesso: Lista de raças encontrada e vinculada ({:X}).", raceList->GetFormID());
            }
            else {
                SKSE::log::error("   - FALHA: Não foi possível encontrar o FormList com ID {:X} no plugin '{}'.", localID, sourceMod);
            }
        }
        else {
            SKSE::log::critical("   - CRITICO: TESDataHandler não está disponível.");
        }
    }
    else {
        SKSE::log::warn(" - AVISO: Informações de 'validRacesListID' ou 'validRacesSource' ausentes. A parte pode não aparecer para nenhuma raça.");
    }

    SKSE::log::info("HeadPart dinâmica '{}' criada com sucesso.", targetEDID);
    SKSE::log::info("==============================================");

    return newPart;
}

void HeadPartCreator::TestCreateHeadPart()
{
    SKSE::log::info("Iniciando Teste de Criação de HeadPart...");

    // Simulando a leitura do arquivo JSON (aqui definido inline para o exemplo)
    std::string jsonContent = R"(
        {
            "editorID": "TESTARONE123A",
            "type": 3,
            "modelPath": "Actors\\Character\\Hair\\KS Hairdo's\\Male\\ExampleHair.nif",
            "flags": 15,
            "validRacesListID": "0x00013746",
            "validRacesSource": "Skyrim.esm"
        }
    )";

    try {
        // Parse da string para objeto JSON
        nlohmann::json jsonData = nlohmann::json::parse(jsonContent);

        // Chama a função criadora
        RE::BGSHeadPart* myNewPart = HeadPartCreator::CreateHeadPartFromJson(jsonData);

        if (myNewPart) {
            SKSE::log::info("Teste bem sucedido! O ponteiro para a nova parte é válido.");
            // Aqui você poderia aplicar 'myNewPart' a um Actor usando a lógica anterior.
        }
        else {
            SKSE::log::error("Teste falhou! A função retornou um ponteiro nulo.");
        }

    }
    catch (const nlohmann::json::parse_error& e) {
        SKSE::log::error("Erro ao fazer parse do JSON de teste: {}", e.what());
    }
}
