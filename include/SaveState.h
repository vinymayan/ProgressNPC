#pragma once
#include "Rule.h"
#include <set>
#include <rapidjson/document.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/filewritestream.h>
#include <rapidjson/prettywriter.h>

struct SaveFileRoot {
    std::vector<std::string> pool_plugins;
    std::vector<std::string> pool_rules;
    std::vector<std::string> pool_groups;
    // npcKey_PluginIdx | npcKey_FormID | { ruleIdx: { v: version, g: [groupIdx] } }
    std::string denseHistoryJson;
};

struct AppliedRuleState {
    int version = 0;
    std::vector<std::string> appliedGroups; // Nomes dos grupos que passaram no sorteio
    bool activationStateKnown = false;
    bool isActive = false;
    bool canRerollOnNextActivation = true;
    std::vector<std::string> activeRewardKeys;
    std::set<std::string> persistentRewardKeys;
};

struct PersistentItemState {
    uint32_t expectedCount = 0;
    uint32_t missingCount = 0;
    std::string ruleID;
    std::string groupName;
    bool isPersistent = true;
};

//inline void to_json(rapidjson::Document& j, const AppliedRuleState& p) {
//    j = rapidjson::Document{ {"version", p.version}, {"appliedGroups", p.appliedGroups} };
//}
//
//inline void from_json(const rapidjson::Document& j, AppliedRuleState& p) {
//    p.version = j.value("version", 0);
//    // Se appliedGroups não existir no save, inicializa como vetor vazio
//    p.appliedGroups = j.value("appliedGroups", std::vector<std::string>{});
//}

struct SaveHistoryEntry {
    uint32_t saveNumber;
    std::map<std::string, std::map<std::string, AppliedRuleState>> npcRuleVersions;
    std::map<std::string, std::map<std::string, PersistentItemState>> persistentItems;
    std::map<std::string, std::set<std::string>> virtualKeywords;
    std::map<std::string, std::map<std::string, int>> addedFactions;
};

struct CurrentSaveContext {
    uint32_t charID = 0;
    uint32_t saveNumber = 0;
    bool isValid = false;

};

class SaveStateManager {
public:

    static SaveStateManager* GetSingleton();


    void LoadCharacterData(uint32_t characterID);
    void UpdateSaveEntry(uint32_t characterID, const SaveHistoryEntry& newEntry);
    void SetCurrentContext(uint32_t a_charID, uint32_t a_saveNum);
    void PersistCurrentSave(const std::string& a_saveName);
    void ClearContext();
    std::vector<SaveHistoryEntry>& GetCharacterHistory(uint32_t characterID);
    SaveHistoryEntry& GetSessionData() { return _sessionData; }
    void TrackPersistentItemGrant(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_count,
        const std::string& a_ruleID, const std::string& a_groupName, bool a_isPersistent = true);
    void EnsurePersistentItemTracked(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_expectedCount,
        const std::string& a_ruleID, const std::string& a_groupName, bool a_isPersistent = true);
    void AuditPersistentItems(RE::Actor* a_actor);
    void RefreshPersistentItemsForLoadedActors();
    void HandleContainerChanged(const RE::TESContainerChangedEvent* a_event);
    bool AddVirtualKeyword(
        RE::Actor* a_actor,
        RE::BGSKeyword* a_keyword,
        std::string_view a_owner = "legacy");
    bool RemoveVirtualKeyword(
        RE::Actor* a_actor,
        RE::BGSKeyword* a_keyword,
        std::string_view a_owner = "legacy");
    bool HasVirtualKeyword(RE::Actor* a_actor, RE::BGSKeyword* a_keyword) const;
    bool AddManagedFaction(
        RE::Actor* a_actor,
        RE::TESFaction* a_faction,
        int a_rank,
        std::string_view a_owner = "legacy");
    bool RemoveManagedFaction(
        RE::Actor* a_actor,
        RE::TESFaction* a_faction,
        std::string_view a_owner = "legacy");
    static std::string BuildFormKey(RE::TESForm* a_form);

    CurrentSaveContext& GetCurrentContext() { return _currentContext; }
    static std::string BuildNPCKey(RE::Actor* a_actor);
private:
    std::string GetCharacterPath(uint32_t characterID);
    std::map<uint32_t, std::vector<SaveHistoryEntry>> _characterHistory; // Cache em memória
    CurrentSaveContext _currentContext;
    SaveHistoryEntry _sessionData;
    std::map<std::string, std::map<std::string, std::set<std::string>>> _virtualKeywordOwners;
    std::set<std::string> _virtualKeywordOwnersReady;
    std::map<std::string, std::map<std::string, std::map<std::string, int>>> _managedFactionOwners;
    std::set<std::string> _managedFactionOwnersReady;
    void EnsureVirtualKeywordOwners(RE::Actor* a_actor);
    void EnsureManagedFactionOwners(RE::Actor* a_actor);
};

enum class OutfitConversionMode : int {
    kDisabled = 0,      // Desligado
    kOnlyEmpty = 1,     // Ligado (Apenas esvazia o Outfit original)
    kFullConversion = 2 // Ligado (Converte para inventário e equipa)
};

void ManageSleepOutfitState(RE::Actor* a_actor, bool a_isEntering);
void ScheduleSleepOutfitUpdate(RE::Actor* a_actor, bool a_isEntering);
void EquipBestInventoryItems(RE::Actor* a_actor);
void ReconcileEquipmentContext(RE::Actor* a_actor);

class NPCSettings {
public:
    static NPCSettings* GetSingleton() {
        static NPCSettings singleton;
        return &singleton;
    }

    // Valor padrão inicial
    OutfitConversionMode outfitMode = OutfitConversionMode::kFullConversion;
    void Load() {
        if (!std::filesystem::exists(settingsPath)) {
            Save(); // Cria o padrão se não existir
            return;
        }

        try {
            FILE* fp = nullptr;
            fopen_s(&fp, settingsPath.c_str(), "rb");
            if (!fp) return;

            char readBuffer[4096];
            rapidjson::FileReadStream stream(fp, readBuffer, sizeof(readBuffer));
            rapidjson::Document doc;
            doc.ParseStream(stream);
            fclose(fp);

            if (!doc.HasParseError() && doc.IsObject()) {
                auto it = doc.FindMember("outfitMode");
                if (it != doc.MemberEnd() && it->value.IsInt()) {
                    outfitMode = static_cast<OutfitConversionMode>(it->value.GetInt());
                }
            }
        }
        catch (const std::exception& e) {
            SKSE::log::error("Falha ao carregar settings: {}", e.what());
        }
    }

    void Save() {
        try {
            std::filesystem::create_directories(std::filesystem::path(settingsPath).parent_path());

            rapidjson::Document doc;
            doc.SetObject();
            doc.AddMember("outfitMode", static_cast<int>(outfitMode), doc.GetAllocator());

            FILE* fp = nullptr;
            fopen_s(&fp, settingsPath.c_str(), "wb");
            if (!fp) return;

            char writeBuffer[4096];
            rapidjson::FileWriteStream stream(fp, writeBuffer, sizeof(writeBuffer));
            rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(stream);
            doc.Accept(writer);
            fclose(fp);
        }
        catch (const std::exception& e) {
            SKSE::log::error("Falha ao salvar settings: {}", e.what());
        }
    }
private:
    const std::string settingsPath = "Data/SKSE/Plugins/ProgressNPC/Settings.json";
};



void ApplyRulesToInstance(RE::Actor* a_actor, int a_forcedLevel = -1);
void ApplyRulesToInstance(
    RE::Actor* a_actor,
    const RuleEvaluationDelta& a_delta,
    int a_forcedLevel = -1);
void ScheduleRuleEvaluation(
    RE::Actor* a_actor,
    RuleEvaluationDelta a_delta = RuleEvaluationDelta::Full());
void ForgetRuleEvaluationRuntimeState(RE::FormID a_actorID);
void ResetRuleEvaluationRuntimeState();
bool IsActorInCombatContext(RE::Actor* actor);
void UpdateActorCombatContext(
    RE::Actor* actor,
    RE::ACTOR_COMBAT_STATE state);



class BackgroundCloneHook {
public:
    static void Install() {
        // Localiza a VTable principal de TESObjectREFR
        REL::Relocation<std::uintptr_t> vtable{ RE::Character::VTABLE[0] };

        // Realiza o hook no índice 0x6D conforme definido no header
        _ShouldBackgroundClone = vtable.write_vfunc(0x6D, &Hook_ShouldBackgroundClone);

        SKSE::log::info("Hook de ShouldBackgroundClone instalado no índice 0x6D");
    }

private:
    // Nota: Como a função original é 'const', o ponteiro 'this' também deve ser const
    static bool Hook_ShouldBackgroundClone(const RE::TESObjectREFR* a_this) {
        auto actor = const_cast<RE::Actor*>(a_this->As<RE::Actor>());
        if (actor && !actor->IsDead() && actor != RE::PlayerCharacter::GetSingleton()) {
            auto baseNPC = actor->GetActorBase();
            if (baseNPC) {
                // USAR O NOVO HELPER: Ele verifica o NPC e o Template dele no banco de dados
                if (!RuleManager::GetSingleton()->IsAffected(actor)) {
                    logger::debug("[OFF] Rules encontradas para {}, aplicando.", actor->GetName());
                    return _ShouldBackgroundClone(a_this);
                }

                logger::info("[ShouldBackgroundClone] Rules encontradas para {}, aplicando.", actor->GetName());
                ScheduleRuleEvaluation(actor);
            }
        }

        return _ShouldBackgroundClone(a_this);
    }

    static inline REL::Relocation<decltype(&Hook_ShouldBackgroundClone)> _ShouldBackgroundClone;
};


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

        if (!a_event->loaded) {
            ForgetRuleEvaluationRuntimeState(a_event->formID);
            return RE::BSEventNotifyControl::kContinue;
        }

        // 1. Tenta obter a referência do objeto que carregou
        auto form = RE::TESForm::LookupByID(a_event->formID);
        if (!form) return RE::BSEventNotifyControl::kContinue;
        auto actor = form->As<RE::Actor>();

        if (actor) {
            ScheduleRuleEvaluation(actor);
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    static void Register() {
        auto scriptEventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (scriptEventSourceHolder) {
            scriptEventSourceHolder->AddEventSink(GetSingleton());
        }
    }

};

class PersistentItemTransferHandler : public RE::BSTEventSink<RE::TESContainerChangedEvent> {
public:
    static PersistentItemTransferHandler* GetSingleton() {
        static PersistentItemTransferHandler singleton;
        return &singleton;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* a_event,
        RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
        SaveStateManager::GetSingleton()->HandleContainerChanged(a_event);
        if (a_event) {
            auto delta = RuleEvaluationDelta::For(
                RuleDependency::kInventory, a_event->baseObj);
            if (auto oldOwner = RE::TESForm::LookupByID<RE::Actor>(a_event->oldContainer)) {
                ScheduleRuleEvaluation(oldOwner, delta);
            }
            if (a_event->newContainer != a_event->oldContainer) {
                if (auto newOwner = RE::TESForm::LookupByID<RE::Actor>(a_event->newContainer)) {
                    ScheduleRuleEvaluation(newOwner, delta);
                }
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    static void Register() {
        auto scriptEventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (scriptEventSourceHolder) {
            scriptEventSourceHolder->AddEventSink(GetSingleton());
            logger::info("PersistentItemTransferHandler registrado.");
        }
    }
};

class PlayerLevel : public RE::BSTEventSink<RE::LevelIncrease::Event> {
public:
    static PlayerLevel* GetSingleton() {
        static PlayerLevel singleton;
        return &singleton;
    }

    // A assinatura deve receber 'RE::LevelIncrease::Event'
    RE::BSEventNotifyControl ProcessEvent(const RE::LevelIncrease::Event* a_event, RE::BSTEventSource<RE::LevelIncrease::Event>*) override {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        logger::info("Player subiu para o nível {}. Iniciando scan de regras para atores próximos.", a_event->newLevel);

        // 1. Aplicar regras ao próprio Player
        auto player = RE::PlayerCharacter::GetSingleton();
        if (player && !player->IsInCombat()) {
            //logger::debug("[LevelUp] Verificando regras para o Player.");
            ApplyRulesToInstance(
                player,
                RuleEvaluationDelta::For(RuleDependency::kLevel),
                static_cast<int>(a_event->newLevel));
        }

        // 2. Aplicar regras aos NPCs carregados (próximos)
        auto processLists = RE::ProcessLists::GetSingleton();
        if (processLists) {
            // A lista 'highActorHandles' contém os atores que o motor está processando ativamente perto do player
            for (auto& handle : processLists->highActorHandles) {
                auto actorPtr = handle.get();
                if (actorPtr) {
                    RE::Actor* npc = actorPtr.get();

                    // Pula o player (já processado acima) e mortos
                    if (npc && npc != player && !npc->IsDead()) {
                        if (RuleManager::GetSingleton()->IsAffected(npc)) {
                            logger::debug("[LevelUp] NPC '{}' detectado com regras aplicáveis. Aplicando.", npc->GetName());
                            ScheduleRuleEvaluation(
                                npc,
                                RuleEvaluationDelta::For(RuleDependency::kLevel));
                        }
                    }
                }
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }

    static void Register() {
        // CORREÇÃO: Usar a fonte dedicada definida em LevelIncrease.h, não o ScriptEventSourceHolder
        auto eventSource = RE::LevelIncrease::GetEventSource();
        if (eventSource) {
            eventSource->AddEventSink(GetSingleton());
            logger::info("PlayerLevel sink registrado com sucesso.");
        }
        else {
            logger::error("Falha ao obter RE::LevelIncrease::GetEventSource()!");
        }
    }
};

class CombatEventHandler : public RE::BSTEventSink<RE::TESCombatEvent> {
public:
    static CombatEventHandler* GetSingleton() {
        static CombatEventHandler singleton;
        return &singleton;
    }

    // Flag para controlar se o player deve receber itens após o combate
    RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_event, RE::BSTEventSource<RE::TESCombatEvent>*) override {
        if (!a_event || !a_event->actor) {
            return RE::BSEventNotifyControl::kContinue;
        }
        auto* actorRef = a_event->actor.get();
        auto* actor = actorRef ? actorRef->As<RE::Actor>() : nullptr;
        if (!actor) {
            return RE::BSEventNotifyControl::kContinue;
        }
        UpdateActorCombatContext(actor, a_event->newState.get());
        ScheduleRuleEvaluation(
            actor,
            RuleEvaluationDelta::For(RuleDependency::kCombat));
        return RE::BSEventNotifyControl::kContinue;
    }

    static void Register() {
        auto scriptEventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (scriptEventSourceHolder) {
            scriptEventSourceHolder->AddEventSink(GetSingleton());
            logger::info("CombatEventHandler registrado.");
        }
    }
};

class LocationChangeHandler : public RE::BSTEventSink<RE::TESActorLocationChangeEvent> {
public:
    static LocationChangeHandler* GetSingleton() {
        static LocationChangeHandler singleton;
        return &singleton;
    }
    RE::BSEventNotifyControl ProcessEvent(
        const RE::TESActorLocationChangeEvent* a_event,
        RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override {
        if (a_event && a_event->actor) {
            auto actor = const_cast<RE::Actor*>(a_event->actor->As<RE::Actor>());
            if (actor) {
                ScheduleRuleEvaluation(
                    actor,
                    RuleEvaluationDelta::For(RuleDependency::kEnvironment));
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
    static void Register() {
        auto scriptEventSourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
        if (scriptEventSourceHolder) {
            scriptEventSourceHolder->AddEventSink(GetSingleton());
        }
    }
};

class ActorCellChangeHandler : public RE::BSTEventSink<RE::BGSActorCellEvent> {
public:
    static ActorCellChangeHandler* GetSingleton() {
        static ActorCellChangeHandler singleton;
        return &singleton;
    }

    RE::BSEventNotifyControl ProcessEvent(
        const RE::BGSActorCellEvent* a_event,
        RE::BSTEventSource<RE::BGSActorCellEvent>*) override {
        if (!a_event) {
            return RE::BSEventNotifyControl::kContinue;
        }

        auto actorPtr = a_event->actor.get();
        auto actor = actorPtr ? actorPtr.get() : nullptr;
        if (actor) {
            ScheduleRuleEvaluation(
                actor,
                RuleEvaluationDelta::For(RuleDependency::kEnvironment));
        }
        return RE::BSEventNotifyControl::kContinue;
    }

    static void Register() {
        auto player = RE::PlayerCharacter::GetSingleton();
        auto source = player ? player->AsBGSActorCellEventSource() : nullptr;
        if (source) {
            source->AddEventSink(GetSingleton());
            logger::info("ActorCellChangeHandler registrado.");
        }
        else {
            logger::warn("Não foi possível registrar ActorCellChangeHandler.");
        }
    }
};
