
//namespace DynamicHeadPartUtils
//{
//    using rapidjson Document
//
//    RE::BGSHeadPart* CreateHeadPartFromJson(const json& a_data)
//    {
//        // 1. Cria a inst�ncia utilizando o helper de template do ConcreteFormFactory.h
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
//        // 7. Configura as Ra�as V�lidas (RNAM)
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
//        // Obt�m o Base NPC para modificar o array de partes
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
//            // For�a a atualiza��o do modelo 3D do Actor para refletir a mudan�a
//            a_actor->Update3DModel();
//        }
//    }
//}



//namespace HeadPartCreator
//{
//    using rapidjson Document
//
//    RE::BGSHeadPart* CreateHeadPartFromJson(const json& a_data);
//
//    void TestCreateHeadPart();
//
//}
