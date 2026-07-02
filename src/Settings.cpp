

//#include "Settings.h"
//#include <fstream>
//#include <filesystem>
//
//RE::BGSHeadPart* HeadPartCreator::CreateHeadPartFromJson(const json& a_data)
//{
//    std::string targetEDID = a_data.value("editorID", "MISSING_EDID");
//    SKSE::log::info("==============================================");
//    SKSE::log::info("Iniciando cria��o din�mica de HeadPart: '{}'", targetEDID);
//
//    // 1. Cria��o da Inst�ncia (Heap)
//    // Utiliza o template helper do ConcreteFormFactory.h
//    auto newPart = RE::IFormFactory::Create<RE::BGSHeadPart>();
//
//    if (!newPart) {
//        SKSE::log::critical("CRITICO: Falha ao obter a ConcreteFormFactory para HeadPart ou falha na aloca��o de mem�ria.");
//        return nullptr;
//    }
//    SKSE::log::info("Sucesso: Inst�ncia base de BGSHeadPart alocada no heap (FormID tempor�rio: {:X})", newPart->GetFormID());
//
//
//    // 2. Configura��o do EditorID
//    if (a_data.contains("editorID")) {
//        std::string edid = a_data["editorID"];
//        newPart->SetFormEditorID(edid.c_str());
//        SKSE::log::info(" - EditorID definido para: {}", edid);
//    }
//    else {
//        SKSE::log::warn(" - AVISO: JSON sem 'editorID'. A parte n�o ter� nome interno.");
//    }
//
//
//    // 3. Configura��o do Tipo (Hair, Eyes, etc.)
//    if (a_data.contains("type")) {
//        uint32_t typeVal = a_data["type"].get<uint32_t>();
//        newPart->type = static_cast<RE::BGSHeadPart::HeadPartType>(typeVal);
//        // Enum simples para log leg�vel
//        std::string typeStr = "Unknown";
//        switch (newPart->type.get()) {
//        case RE::BGSHeadPart::HeadPartType::kMisc: typeStr = "Misc"; break;
//        case RE::BGSHeadPart::HeadPartType::kFace: typeStr = "Face"; break;
//        case RE::BGSHeadPart::HeadPartType::kEyes: typeStr = "Eyes"; break;
//        case RE::BGSHeadPart::HeadPartType::kHair: typeStr = "Hair"; break;
//        case RE::BGSHeadPart::HeadPartType::kFacialHair: typeStr = "Beard"; break;
//        case RE::BGSHeadPart::HeadPartType::kEyebrows: typeStr = "Eyebrows"; break;
//        }
//        SKSE::log::info(" - Tipo definido para: {} ({})", typeVal, typeStr);
//    }
//    else {
//        SKSE::log::error(" - ERRO: JSON faltando campo obrigat�rio 'type'.");
//    }
//
//
//    // 4. Configura��o do Modelo (.nif)
//    if (a_data.contains("modelPath")) {
//        std::string path = a_data["modelPath"];
//        newPart->SetModel(path.c_str());
//        SKSE::log::info(" - Model Path definido para: '{}'", path);
//        // Verifica��o b�sica se o caminho parece v�lido (n�o verifica exist�ncia do arquivo)
//        if (path.length() < 5 || path.find(".nif") == std::string::npos) {
//            SKSE::log::warn("   - AVISO: O caminho do modelo parece suspeito ou incompleto.");
//        }
//    }
//    else {
//        SKSE::log::error(" - ERRO: JSON faltando campo obrigat�rio 'modelPath'. A parte ser� invis�vel.");
//    }
//
//
//    // 5. Configura��o das Flags
//    if (a_data.contains("flags")) {
//        uint8_t flagsVal = a_data["flags"].get<uint8_t>();
//        newPart->flags = static_cast<RE::BGSHeadPart::Flag>(flagsVal);
//        SKSE::log::info(" - Flags bitmask definida para: {:X} (decimal: {})", flagsVal, flagsVal);
//    }
//
//
//    // 6. Configura��o da Lista de Ra�as V�lidas
//    if (a_data.contains("validRacesListID") && a_data.contains("validRacesSource")) {
//        std::string formIdStr = a_data["validRacesListID"];
//        std::string sourceMod = a_data["validRacesSource"];
//
//        SKSE::log::info(" - Tentando resolver Lista de Ra�as: ID '{}' em '{}'", formIdStr, sourceMod);
//
//        auto dataHandler = RE::TESDataHandler::GetSingleton();
//        if (dataHandler) {
//            // Converte string hex para num�rico e busca
//            RE::FormID localID = std::stoul(formIdStr, nullptr, 16);
//            auto raceList = dataHandler->LookupForm<RE::BGSListForm>(localID, sourceMod);
//
//            if (raceList) {
//                newPart->validRaces = raceList;
//                SKSE::log::info("   - Sucesso: Lista de ra�as encontrada e vinculada ({:X}).", raceList->GetFormID());
//            }
//            else {
//                SKSE::log::error("   - FALHA: N�o foi poss�vel encontrar o FormList com ID {:X} no plugin '{}'.", localID, sourceMod);
//            }
//        }
//        else {
//            SKSE::log::critical("   - CRITICO: TESDataHandler n�o est� dispon�vel.");
//        }
//    }
//    else {
//        SKSE::log::warn(" - AVISO: Informa��es de 'validRacesListID' ou 'validRacesSource' ausentes. A parte pode n�o aparecer para nenhuma ra�a.");
//    }
//
//    SKSE::log::info("HeadPart din�mica '{}' criada com sucesso.", targetEDID);
//    SKSE::log::info("==============================================");
//
//    return newPart;
//}
//
//void HeadPartCreator::TestCreateHeadPart()
//{
//    SKSE::log::info("Iniciando Teste de Cria��o de HeadPart...");
//
//    // Simulando a leitura do arquivo JSON (aqui definido inline para o exemplo)
//    std::string jsonContent = R"(
//        {
//            "editorID": "TESTARONE123A",
//            "type": 3,
//            "modelPath": "Actors\\Character\\Hair\\KS Hairdo's\\Male\\ExampleHair.nif",
//            "flags": 15,
//            "validRacesListID": "0x00013746",
//            "validRacesSource": "Skyrim.esm"
//        }
//    )";
//
//    try {
//        // Parse da string para objeto JSON
//        rapidjson::Document jsonData = rapidjson::Document::parse(jsonContent);
//
//        // Chama a fun��o criadora
//        RE::BGSHeadPart* myNewPart = HeadPartCreator::CreateHeadPartFromJson(jsonData);
//
//        if (myNewPart) {
//            SKSE::log::info("Teste bem sucedido! O ponteiro para a nova parte � v�lido.");
//            // Aqui voc� poderia aplicar 'myNewPart' a um Actor usando a l�gica anterior.
//        }
//        else {
//            SKSE::log::error("Teste falhou! A fun��o retornou um ponteiro nulo.");
//        }
//
//    }
//    catch (const rapidjson::Document::parse_error& e) {
//        SKSE::log::error("Erro ao fazer parse do JSON de teste: {}", e.what());
//    }
//}
