#pragma once
#include "Settings.h"

void EquipBestInventoryItems(RE::Actor* a_actor);

void ApplyRulesToInstance(RE::Actor* a_actor);


//namespace DynamicHeadPartUtils
//{
//    using json = nlohmann::json;
//
//    RE::BGSHeadPart* CreateHeadPartFromJson(const json& a_data)
//    {
//        // 1. Cria a instância utilizando o helper de template do ConcreteFormFactory.h
//        // Isso resolve automaticamente a factory para FormType::HeadPart
//        auto newPart = RE::IFormFactory::Create<RE::BGSHeadPart>();
//        if (!newPart) return nullptr;
//
//        // 2. Configura o EditorID (EDID)
//        if (a_data.contains("editorID")) {
//            newPart->SetFormEditorID(a_data["editorID"].get<std::string>().c_str());
//        }
//
//        // 3. Define o Tipo (PNAM)
//        if (a_data.contains("type")) {
//            newPart->type = static_cast<RE::BGSHeadPart::HeadPartType>(a_data["type"].get<uint32_t>());
//        }
//
//        // 4. Configura o Modelo (herdado de TESModelTextureSwap)
//        if (a_data.contains("modelPath")) {
//            newPart->SetModel(a_data["modelPath"].get<std::string>().c_str());
//        }
//
//        // 5. Configura as Flags (DATA)
//        if (a_data.contains("flags")) {
//            newPart->flags = static_cast<RE::BGSHeadPart::Flag>(a_data["flags"].get<uint8_t>());
//        }
//
//        // 6. Configura o TextureSet (TNAM)
//        if (a_data.contains("textureSetID") && a_data.contains("textureSource")) {
//            auto dataHandler = RE::TESDataHandler::GetSingleton();
//            if (dataHandler) {
//                auto txSet = dataHandler->LookupForm<RE::BGSTextureSet>(
//                    std::stoul(a_data["textureSetID"].get<std::string>(), nullptr, 16),
//                    a_data["textureSource"].get<std::string>()
//                );
//                if (txSet) {
//                    newPart->textureSet = txSet;
//                }
//            }
//        }
//
//        // 7. Configura as Raças Válidas (RNAM)
//        if (a_data.contains("validRacesListID")) {
//            auto dataHandler = RE::TESDataHandler::GetSingleton();
//            if (dataHandler) {
//                auto raceList = dataHandler->LookupForm<RE::BGSListForm>(
//                    std::stoul(a_data["validRacesListID"].get<std::string>(), nullptr, 16),
//                    "Skyrim.esm"
//                );
//                if (raceList) {
//                    newPart->validRaces = raceList;
//                }
//            }
//        }
//
//        return newPart;
//    }
//
//    void ApplyDynamicPartToActor(RE::Actor* a_actor, const json& a_partData)
//    {
//        if (!a_actor) return;
//
//        // Obtém o Base NPC para modificar o array de partes
//        auto baseNPC = a_actor->GetActorBase();
//        if (!baseNPC) return;
//
//        // Cria a nova HeadPart em runtime
//        RE::BGSHeadPart* customPart = CreateHeadPartFromJson(a_partData);
//
//        if (customPart) {
//            // ChangeHeadPart substitui a parte existente do mesmo tipo no array headParts
//            baseNPC->ChangeHeadPart(customPart);
//
//            // Força a atualização do modelo 3D do Actor para refletir a mudança
//            a_actor->Update3DModel();
//        }
//    }
//}



namespace HeadPartCreator
{
    using json = nlohmann::json;

    RE::BGSHeadPart* CreateHeadPartFromJson(const json& a_data);

    void TestCreateHeadPart();

}

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
        auto npc = a_this->As<RE::Actor>();
        auto npcConst = const_cast<RE::Actor*>(npc);
        if (npcConst && !npcConst->IsDead()) {
            // 1. MELHORIA: Verificação rápida antes de entrar na lógica pesada
            auto baseNPC = npcConst->GetActorBase();
            auto dataHandler = RE::TESDataHandler::GetSingleton();
            RE::BGSOutfit* rdoEmptyOutfit = dataHandler ? dataHandler->LookupForm<RE::BGSOutfit>(0x800, "RDO.esp") : nullptr;

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



