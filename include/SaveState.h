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

    void SetCurrentContext(uint32_t a_charID, uint32_t a_saveNum) {
        _currentContext.charID = a_charID;
        _currentContext.saveNumber = a_saveNum;
        _currentContext.isValid = true;
    }

    void ClearContext() {
        _currentContext.isValid = false;
        _currentContext.saveNumber = 0;
        _currentContext.charID = 0;
    }

    CurrentSaveContext& GetCurrentContext() { return _currentContext; }

    void PersistCurrentSave(const std::string& a_saveName);

private:
    std::string GetCharacterPath(uint32_t characterID);
    std::map<uint32_t, std::vector<SaveHistoryEntry>> _characterHistory; // Cache em memória
    CurrentSaveContext _currentContext;
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

void ApplyRulesToInstance(RE::Actor* a_actor);



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
        // Chama a função original
        auto npcConst = const_cast<RE::Actor*>(a_this->As<RE::Actor>());
        if (npcConst && !npcConst->IsDead() && npcConst != RE::PlayerCharacter::GetSingleton()) {
            // 1. MELHORIA: Verificação rápida antes de entrar na lógica pesada
            auto baseNPC = npcConst->GetActorBase();

            if (baseNPC) {
                const auto& affectedDB = RuleManager::GetSingleton()->GetAffectedNPCsDatabase();
                // Se o FormID do NPC Base não estiver no banco de dados, ignoramos o ator imediatamente
                if (affectedDB.find(baseNPC->GetFormID()) == affectedDB.end()) {
                    EquipBestInventoryItems(npcConst);
                    logger::debug("sem rules para aplicar para {}", npcConst->GetName());
                    return _ShouldBackgroundClone(a_this);
                }
                logger::debug("Rules encontradas para {}, iniciando processo de aplicacao.", npcConst->GetName());
                // Se chegou aqui, o NPC tem regras potenciais
                ApplyRulesToInstance(npcConst);

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
                    EquipBestInventoryItems(npcConst);
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
