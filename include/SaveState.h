#pragma once
#include "Settings.h"

class LoadEventHandler : public RE::BSTEventSink<RE::TESObjectLoadedEvent> {
public:
    // O Singleton para acessar o handler
    static LoadEventHandler* GetSingleton() {
        static LoadEventHandler singleton;
        return &singleton;
    }

    // A função que processa o evento
    RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;

        // 1. Tenta obter a referência do objeto que carregou
        auto form = RE::TESForm::LookupByID(a_event->formID);
        if (!form) return RE::BSEventNotifyControl::kContinue;

        // 2. Verifica se esse objeto é um Actor (NPC ou Player)
        auto actor = form->As<RE::Actor>();
        if (actor && !actor->IsDead()) {
            // 1. MELHORIA: Verificação rápida antes de entrar na lógica pesada
            auto baseNPC = actor->GetActorBase();
            auto dataHandler = RE::TESDataHandler::GetSingleton();
            RE::BGSOutfit* rdoEmptyOutfit = dataHandler ? dataHandler->LookupForm<RE::BGSOutfit>(0x800, "RDO.esp") : nullptr;

            // Se o NPC base já usa o outfit vazio, garantimos que a instância está vestida
            if (baseNPC->defaultOutfit == rdoEmptyOutfit) {
                EquipBestInventoryItems(actor);
            }
            if (baseNPC) {
                const auto& affectedDB = RuleManager::GetSingleton()->GetAffectedNPCsDatabase();
                // Se o FormID do NPC Base não estiver no banco de dados, ignoramos o ator imediatamente
                if (affectedDB.find(baseNPC->GetFormID()) == affectedDB.end()) {
                    return RE::BSEventNotifyControl::kContinue;
                }

                // Se chegou aqui, o NPC tem regras potenciais
                ApplyRulesToInstance(actor);
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    void EquipBestInventoryItems(RE::Actor* a_actor) {
        if (!a_actor) return;

        auto equipManager = RE::ActorEquipManager::GetSingleton();
        if (!equipManager) return;

        // Obtém o inventário atual da instância
        auto inventory = a_actor->GetInventory();

        for (auto& [item, invData] : inventory) {
            auto& [count, entry] = invData;

            // Se o item for uma armadura/roupa e NÃO estiver equipado, forçamos o uso
            if (count > 0 && !entry->IsWorn() && item->IsArmor()) {
                equipManager->EquipObject(a_actor, item, nullptr, 1, nullptr, true, false, false, false);

                logger::debug("[OutfitSync] Forçando equipagem de '{}' em '{}' (3D Loaded)",
                    item->GetName(), a_actor->GetName());
            }
        }
    }

    void ApplyRulesToInstance(RE::Actor* a_actor) {
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
        std::string npcKey = fileNameStr + "|" + FormatLocalFormID(baseNPC->GetFormID(), fileNameStr);

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
                                equipManager->EquipObject(a_actor, bound, nullptr, 1, nullptr, true, false, false, false);

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

    void ForceApplyToLoadedActors() {
        auto ruleManager = RuleManager::GetSingleton();
        const auto& affectedDB = ruleManager->GetAffectedNPCsDatabase();

        logger::info("======================================================");
        logger::info("[ApplyInstance] Sincronizando atores (Database size: {})", affectedDB.size());

        auto processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) return;

        int totalVerificados = 0;
        int totalProcessados = 0;

        // Percorre apenas atores em "High Process" (carregados perto do player)
        for (auto& handle : processLists->highActorHandles) {
            auto actorPtr = handle.get();
            if (!actorPtr || !actorPtr.get()) continue;

            RE::Actor* actor = actorPtr.get();
            auto baseNPC = actor->GetActorBase();
            if (!baseNPC) continue;

            totalVerificados++;

            // --- FILTRAGEM SOLICITADA ---
            // Só chama a função de aplicação se o FormID base estiver no banco de afetados
            if (affectedDB.find(baseNPC->GetFormID()) != affectedDB.end()) {
                logger::debug("[ApplyInstance] NPC Aplicavel encontrado: {} ({:X})", actor->GetName(), actor->GetFormID());
                ApplyRulesToInstance(actor);
                totalProcessados++;
            }
        }

        logger::info("[ApplyInstance] Varredura concluida. {} atores na vizinhanca, {} eram aplicaveis.",
            totalVerificados, totalProcessados);
        logger::info("======================================================");
    }
    static void Register() {
        auto scriptEventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (scriptEventSourceHolder) {
            scriptEventSourceHolder->AddEventSink(GetSingleton());
        }
    }
};