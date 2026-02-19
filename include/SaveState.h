#pragma once
#include "Rule.h"

struct SaveHistoryEntry {
    uint32_t saveNumber;
    std::map<std::string, std::map<std::string, int>> npcRuleVersions;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SaveHistoryEntry, saveNumber, npcRuleVersions)
};

struct CurrentSaveContext {
    uint32_t charID = 0;
    uint32_t saveNumber = 0;
    bool isValid = false;

};

class SaveStateManager {
public:
    static SaveStateManager* GetSingleton() {
        static SaveStateManager singleton;
        return &singleton;
    }

    // Carrega o arquivo do personagem (ex: 21C20337.json)
    void LoadCharacterData(uint32_t characterID);

    // Salva ou atualiza uma entrada de save para o personagem atual
    void UpdateSaveEntry(uint32_t characterID, const SaveHistoryEntry& newEntry);

    std::vector<SaveHistoryEntry>& GetCharacterHistory(uint32_t characterID);
    SaveHistoryEntry& GetSessionData() { return _sessionData; }
    void SetCurrentContext(uint32_t a_charID, uint32_t a_saveNum);

    void ClearContext() {
        _currentContext.isValid = false;
        _currentContext.saveNumber = 0;
        _currentContext.charID = 0;
        _sessionData = SaveHistoryEntry{};
    }

    CurrentSaveContext& GetCurrentContext() { return _currentContext; }

    void PersistCurrentSave(const std::string& a_saveName);

private:
    std::string GetCharacterPath(uint32_t characterID);
    std::map<uint32_t, std::vector<SaveHistoryEntry>> _characterHistory; // Cache em memória
    CurrentSaveContext _currentContext;
    SaveHistoryEntry _sessionData;
};

enum class OutfitConversionMode : int {
    kDisabled = 0,      // Desligado
    kOnlyEmpty = 1,     // Ligado (Apenas esvazia o Outfit original)
    kFullConversion = 2 // Ligado (Converte para inventário e equipa)
};

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
            std::ifstream i(settingsPath);
            nlohmann::json j;
            i >> j;
            outfitMode = static_cast<OutfitConversionMode>(j.value("outfitMode", 2));
        }
        catch (const std::exception& e) {
            SKSE::log::error("Falha ao carregar settings: {}", e.what());
        }
    }

    void Save() {
        try {
            std::filesystem::create_directories(std::filesystem::path(settingsPath).parent_path());
            nlohmann::json j;
            j["outfitMode"] = static_cast<int>(outfitMode);

            std::ofstream o(settingsPath);
            o << j.dump(4);
        }
        catch (const std::exception& e) {
            SKSE::log::error("Falha ao salvar settings: {}", e.what());
        }
    }

private:
    const std::string settingsPath = "Data/SKSE/Plugins/ProgressNPC/Settings.json";
};

void EquipBestInventoryItems(RE::Actor* a_actor);

void ApplyRulesToInstance(RE::Actor* a_actor,int a_forcedLevel = -1);



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
                    //EquipBestInventoryItems(actor);
                    return _ShouldBackgroundClone(a_this);
                }

                logger::debug("Rules encontradas para {} (ou seu Template), aplicando.", actor->GetName());
                ApplyRulesToInstance(actor);
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

        // 1. Tenta obter a referência do objeto que carregou
        auto form = RE::TESForm::LookupByID(a_event->formID);
        if (!form) return RE::BSEventNotifyControl::kContinue;
        auto actor = form->As<RE::Actor>();
        if (actor != RE::PlayerCharacter::GetSingleton()) return RE::BSEventNotifyControl::kContinue;
        auto npcConst = const_cast<RE::Actor*>(actor);

        if (actor == RE::PlayerCharacter::GetSingleton()) {
            auto baseNPC = npcConst->GetActorBase();

            if (baseNPC) {
                const auto& affectedDB = RuleManager::GetSingleton()->GetAffectedNPCsDatabase();
                // Se o FormID do NPC Base não estiver no banco de dados, ignoramos o ator imediatamente
                if (affectedDB.find(baseNPC->GetFormID()) == affectedDB.end()) {
                    //EquipBestInventoryItems(npcConst);
                    logger::debug("sem rules para aplicar para {}", npcConst->GetName());
                    return RE::BSEventNotifyControl::kContinue;
                }
                logger::debug("Rules encontradas para {}, iniciando processo de aplicacao.", npcConst->GetName());
                // Se chegou aqui, o NPC tem regras potenciais
                ApplyRulesToInstance(npcConst);

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
            ApplyRulesToInstance(player, static_cast<int>(a_event->newLevel));
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
                            ApplyRulesToInstance(npc);
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
    bool pendingPlayerUpdate = false;

    RE::BSEventNotifyControl ProcessEvent(const RE::TESCombatEvent* a_event, RE::BSTEventSource<RE::TESCombatEvent>*) override {
        if (!a_event || !a_event->actor || !a_event->actor->IsPlayer()) {
            return RE::BSEventNotifyControl::kContinue;
        }

        switch (a_event->newState.get()) {
        case RE::ACTOR_COMBAT_STATE::kNone:
            auto player = RE::PlayerCharacter::GetSingleton();
            if (player) {
                logger::info("saiu de combate");
                ApplyRulesToInstance(player);
            }
        }


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
    virtual RE::BSEventNotifyControl ProcessEvent(const RE::TESActorLocationChangeEvent* a_event, RE::BSTEventSource<RE::TESActorLocationChangeEvent>* a_source) override {
        if (a_event && a_event->newLoc) {
            logger::info("[LocationChangeHandler] Detected Location Change: {} moved from '{}' to '{}'",
                a_event->actor ? a_event->actor->GetName() : "Unknown Actor",
                a_event->oldLoc ? a_event->oldLoc->GetName() : "Unknown Location",
                a_event->newLoc->GetName());
			auto actor = a_event->actor->As<RE::Actor>();
            //RE::FormID actorID = a_event->actor->GetFormID();
            if (actor) {
                ApplyRulesToInstance(actor);
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