#include "Manager.h"
#include "SaveState.h"

void Manager::PopulateAllLists(bool forceRefresh) {
    if (_isPopulated && !forceRefresh) return;

    logger::info("Iniciando escaneamento de FormTypes{}...", forceRefresh ? " (refresh forçado)" : "");

    PopulateList<RE::BGSKeyword>("Keyword");
    PopulateList<RE::TESFaction>("Faction");
    PopulateList<RE::TESRace>("Race");
    PopulateList<RE::BGSPerk>("Perk");
    PopulateList<RE::SpellItem>("Spell");
    PopulateList<RE::TESShout>("Shout");
    PopulateList<RE::TESNPC>("NPC");
    PopulateList<RE::TESObjectWEAP>("Weapon");
    PopulateList<RE::TESObjectARMO>("Armor");
    PopulateList<RE::BGSOutfit>("Outfit");


    // --- NOVOS TIPOS ADICIONADOS ---
    PopulateList<RE::AlchemyItem>("Potion");
    PopulateList<RE::IngredientItem>("Ingredient");
    PopulateList<RE::ScrollItem>("Scroll");
    PopulateList<RE::TESObjectBOOK>("Book");
    PopulateList<RE::TESAmmo>("Ammo");
    PopulateList<RE::TESObjectMISC>("Misc");
    PopulateList<RE::TESSoulGem>("SoulGem");
    PopulateList<RE::TESKey>("Key");
    // - v.1.2.0
    PopulateList<RE::TESCombatStyle>("Combat Style");
    PopulateList<RE::BGSVoiceType>("Voice Type");
    PopulateList<RE::TESClass>("Class");
    PopulateList<RE::BGSLocation>("Location");
    PopulateList<RE::BGSHeadPart>("Hair", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kHair;
        });
    PopulateList<RE::BGSHeadPart>("Facial Hair", [](RE::BGSHeadPart* hp) {
        return hp->type == RE::BGSHeadPart::HeadPartType::kFacialHair;
        });
    PopulateList<RE::TESLevCharacter>("Leveled NPC", [](RE::TESLevCharacter* lvnc) {
        return lvnc && !lvnc->entries.empty();
        });
    PopulateList<RE::TESPackage>("Package");

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

// Altere a implementaÃ§Ã£o:
std::string Manager::ToUTF8(std::string_view a_str) {
    if (a_str.empty()) return "";

    // Converte string_view para data temporÃ¡ria para o WinAPI
    int wlen = MultiByteToWideChar(CP_ACP, 0, a_str.data(), static_cast<int>(a_str.size()), nullptr, 0);
    if (wlen <= 0) return std::string(a_str);

    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, a_str.data(), static_cast<int>(a_str.size()), &wstr[0], wlen);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return std::string(a_str);

    std::string u8str(u8len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &u8str[0], u8len, nullptr, nullptr);

    if (!u8str.empty() && u8str.back() == '\0') u8str.pop_back();

    return u8str;
}

template <typename T>
void Manager::PopulateList(const std::string& a_typeName, std::function<bool(T*)> a_filter) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    auto& list = _dataStore[a_typeName];
    list.clear();

    const auto& forms = dataHandler->GetFormArray<T>();
    list.reserve(forms.size());

    for (const auto& form : forms) {
        if (!form) continue;

        if (a_filter && !a_filter(form)) {
            continue;
        }
        // VariÃ¡veis de auxÃ­lio para o log de erro caso o catch seja acionado
        RE::FormID currentID = 0;
        std::string currentPlugin = "Unknown";

        try {
            currentID = form->GetFormID();

            // ObtÃ©m o nome do plugin de origem antes de qualquer processamento complexo
            if (auto file = form->GetFile(0)) {
                currentPlugin = std::string(file->GetFilename());
            }
            else {
                currentPlugin = "Dynamic";
            }

            InternalFormInfo info;
            info.formID = currentID;
            info.formType = a_typeName;
            info.pluginName = ToUTF8(currentPlugin);

            // EditorID: clib_util pode lanÃ§ar exceÃ§Ãµes em contextos raros de memÃ³ria
            std::string rawEditorID = clib_util::editorID::get_editorID(form);
            info.editorID = ToUTF8(rawEditorID);

            std::string rawName = "";
            if (form->Is(RE::FormType::NPC)) {
                if (auto npc = form->As<RE::TESNPC>()) {
                    rawName = npc->fullName.c_str();
                }
            }
            else if (auto fullName = form->As<RE::TESFullName>()) {
                rawName = fullName->fullName.c_str();
            }

            // A conversÃ£o UTF-8 Ã© um ponto comum de falha se a string estiver corrompida
            info.name = ToUTF8(rawName);

            list.push_back(info);
        }
        catch (const std::exception& e) {
            // Log detalhado com FormID em Hexadecimal e o erro especÃ­fico
            logger::error("[PopulateList] Critical error on item {:08X} of plugin '{}' (Type: {}). Error: {}",
                currentID, currentPlugin, a_typeName, e.what());
        }
        catch (...) {
            // Captura erros desconhecidos que nÃ£o herdam de std::exception
            logger::error("[PopulateList] Uknown error on item {:08X} of plugin '{}' (Type: {})",
                currentID, currentPlugin, a_typeName);
        }
    }
    logger::info("Carregados {} itens do tipo {}", list.size(), a_typeName);
}

void Manager::ConvertAllNPCOutfitsToInventory() {
    auto settings = NPCSettings::GetSingleton();
    if (settings->outfitMode == OutfitConversionMode::kDisabled) {
        logger::info("[Outfit] ConversÃ£o desativada nas configuraÃ§Ãµes.");
        return;
    }
    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    // ObtÃ©m todos os NPCs carregados no jogo
    const auto& npcArray = dataHandler->GetFormArray<RE::TESNPC>();
    RE::BGSOutfit* rdoEmptyOutfit = dataHandler->LookupForm<RE::BGSOutfit>(0x800, "RDO.esp");
    uint32_t count = 0;
    for (auto* npc : npcArray) {
        // Verifica se o NPC existe e se possui um Outfit padrÃ£o (DOFT)
        if (npc && npc->defaultOutfit) {
            if (settings->outfitMode == OutfitConversionMode::kFullConversion) {
                RE::BGSOutfit* outfit = npc->defaultOutfit;
                for (auto* item : outfit->outfitItems) {
                    if (item) {
                        auto* boundItem = item->As<RE::TESBoundObject>();
                        if (boundItem && npc->GetObjectCount(boundItem) == 0) {
                            npc->AddObjectToContainer(boundItem, 1, npc);
                        }
                    }
                }
            }

            npc->SetDefaultOutfit(rdoEmptyOutfit);

            // npc->sleepOutfit = nullptr;

            count++;
        }
    }
    logger::info("Processados {} NPCs: Outfits convertidos em itens de inventÃ¡rio.", count);
}
