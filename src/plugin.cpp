#include "logger.h"
#include "SaveState.h"
#include "UI.h"



void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        // Init Rules
        RuleManager::GetSingleton()->LoadRules();
        Manager::GetSingleton()->PopulateAllLists();
        RuleManager::GetSingleton()->InitializeAffectedNPCsDatabase();
		Manager::GetSingleton()->ConvertAllNPCOutfitsToInventory();
       // CellAttachHandler::Register();
        //CellFullyLoadedHandler::Register();
        //LoadEventHandler::Register();
        
        //LocationChangeHandler::Register();
        //HeadPartCreator::TestCreateHeadPart();
        SPIDUI::Register();
    }
    if (message->type == SKSE::MessagingInterface::kPreLoadGame) {
        SaveStateManager::GetSingleton()->ClearContext();
        const char* saveName = static_cast<const char*>(message->data);
        
        if (!saveName) {
            logger::warn("PreLoadGame: Nome do save é nulo.");
            return;
        }

        logger::info("Iniciando pré-carregamento para o save: {}", saveName);

        RE::BGSSaveLoadFileEntry tempEntry{};
        tempEntry.fileName = saveName;

        bool success = false;
        for (int i = 0; i < 5; ++i) {
            if (tempEntry.PopulateFileEntryData()) {
                // 3. CORREÇÃO: Use .data() em vez de .c_str() para evitar o assert !wide()
                logger::debug("Sucesso ao ler cabeçalho na tentativa {}: {}", i + 1, tempEntry.fileName.data());
                success = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (success) {
            std::string fileName = saveName;
            uint32_t charID = 0;

            try {
                size_t firstUnderscore = fileName.find('_');
                size_t secondUnderscore = fileName.find('_', firstUnderscore + 1);

                if (firstUnderscore != std::string::npos && secondUnderscore != std::string::npos) {
                    std::string charIdStr = fileName.substr(firstUnderscore + 1, secondUnderscore - firstUnderscore - 1);
                    charID = static_cast<uint32_t>(std::stoul(charIdStr, nullptr, 16));
                }
                else {
                    charID = tempEntry.characterID;
                    logger::warn("Padrão de nome não detectado. Usando ID interno: {:X}", charID);
                }
            }
            catch (const std::exception& e) {
                charID = tempEntry.characterID;
                logger::error("Erro no processamento do nome: {}", e.what());
            }

            SaveStateManager::GetSingleton()->LoadCharacterData(charID);
            SaveStateManager::GetSingleton()->SetCurrentContext(charID, tempEntry.saveNumber, "");

            logger::info("Contexto carregado: Personagem {:X}, Save {}", charID, tempEntry.saveNumber);
        }
        else {
            logger::error("Falha crítica ao ler o cabeçalho do arquivo: {}", saveName);
        }


    }
    if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
        //LoadEventHandler::GetSingleton()->ForceApplyToLoadedActors();
        //AplicaGeral();
    }
    if (message->type == SKSE::MessagingInterface::kSaveGame) {
        // O data do kSaveGame contém o nome do arquivo de save
        const char* saveName = static_cast<const char*>(message->data);
        
        logger::info("Salvamento detectado: {}. Sincronizando banco de dados JSON...", saveName);
        SaveStateManager::GetSingleton()->PersistCurrentSave(saveName);
    }

    
    if (message->type == SKSE::MessagingInterface::kNewGame) {

    }
}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {

    SetupLog();
    logger::info("Plugin loaded");
    SKSE::Init(skse);
    BackgroundCloneHook::Install();
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
