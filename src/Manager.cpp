#include "Manager.h"


void Manager::PopulateAllLists() {
    if (_isPopulated) return;

    logger::info("Iniciando escaneamento de FormTypes...");

    PopulateList<RE::BGSKeyword>("Keyword");
    PopulateList<RE::TESFaction>("Faction");
    PopulateList<RE::BGSPerk>("Perk");
    PopulateList<RE::SpellItem>("Spell");
    PopulateList<RE::TESShout>("Shout");
    PopulateList<RE::TESNPC>("NPC");
    PopulateList<RE::TESObjectWEAP>("Weapon");
    PopulateList<RE::TESObjectARMO>("Armor");

    _isPopulated = true;
    for (auto cb : _readyCallbacks) {
        if (cb) cb();
    }
    _readyCallbacks.clear();
}

const std::vector<InternalFormInfo>& Manager::GetList(const std::string& typeName) {
    static std::vector<InternalFormInfo> empty;
    auto it = _dataStore.find(typeName);
    if (it != _dataStore.end()) {
        return it->second;
    }
    return empty;
}

void Manager::RegisterReadyCallback(std::function<void()> callback) {
    if (_isPopulated) {
        callback();
    } else {
        _readyCallbacks.push_back(callback);
    }
}

template <typename T>
void Manager::PopulateList(const std::string& a_typeName) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    auto& list = _dataStore[a_typeName];
    list.clear();

    const auto& forms = dataHandler->GetFormArray<T>();
    list.reserve(forms.size());

    for (const auto& form : forms) {
        if (!form) continue;

        InternalFormInfo info;
        info.formID = form->GetFormID();
        info.formType = a_typeName;

        // Atribuição segura de string
        // editorID pode não existir em build pública, mas clib_util tenta pegar
        info.editorID = clib_util::editorID::get_editorID(form);

        info.name = "";

        // 1. Verifica se o form é um NPC
        if (form->Is(RE::FormType::NPC)) {
            if (auto npc = form->As<RE::TESNPC>()) {
                info.name = npc->fullName.c_str();
            }
        }
        // 2. Opcional: Se não for NPC, tenta pegar o nome de qualquer objeto que tenha TESFullName (Spells, Itens, etc)
        else if (auto fullName = form->As<RE::TESFullName>()) {
            info.name = fullName->fullName.c_str();
        }

        if (auto file = form->GetFile(0)) {
            info.pluginName = file->GetFilename();
        } else {
             info.pluginName = "Dynamic"; // Created at runtime
        }
       
        list.push_back(info);
    }
    logger::info("Carregados {} itens do tipo {}", list.size(), a_typeName);
}

void Manager::ConvertAllNPCOutfitsToInventory() {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    // Obtém todos os NPCs carregados no jogo
    const auto& npcArray = dataHandler->GetFormArray<RE::TESNPC>();

    uint32_t count = 0;
    for (auto* npc : npcArray) {
        // Verifica se o NPC existe e se possui um Outfit padrão (DOFT)
        if (npc && npc->defaultOutfit) {
            RE::BGSOutfit* outfit = npc->defaultOutfit;
            std::string editorID = clib_util::editorID::get_editorID(outfit);
            // Transfere cada item do Outfit para o container fixo do NPC
            for (auto* item : outfit->outfitItems) {
                if (item) {
                    auto* boundItem = item->As<RE::TESBoundObject>();
                    if (boundItem) {
                        if (npc->GetObjectCount(boundItem) == 0) {
                            npc->AddObjectToContainer(boundItem, 1, nullptr);
                        }
                    }
                }
            }
            RE::BGSOutfit* rdoEmptyOutfit = nullptr;

            rdoEmptyOutfit = dataHandler->LookupForm<RE::BGSOutfit>(0x800, "RDO.esp");
            // Remove o Outfit padrão para evitar que o jogo sobrescreva o inventário
            npc->SetDefaultOutfit(rdoEmptyOutfit);
            RE::BGSOutfit* newoutFit = npc->defaultOutfit;
            std::string novoOut = clib_util::editorID::get_editorID(newoutFit);
            //logger::debug("[NOVO] '{}': Outfit '{}'",npc->GetName(), novoOut);
            // Opcional: Repetir para o Sleep Outfit (SOFT) se desejar
            // npc->sleepOutfit = nullptr; 

            count++;
        }
    }
    //logger::info("Processados {} NPCs: Outfits convertidos em itens de inventário.", count);
}