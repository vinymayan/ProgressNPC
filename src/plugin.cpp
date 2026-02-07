#include "logger.h"
#include "Rule.h"
#include "UI.h"


void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        // Init Rules
        RuleManager::GetSingleton()->LoadRules();
        
        // Populate Forms (Async? Or direct?)
        // Manager::GetSingleton()->PopulateAllLists(); // Maybe wait for user request or thread it?
        // For now, let's just populate.
        Manager::GetSingleton()->PopulateAllLists();

        // Register UI
        SPIDUI::Register();
    }
    if (message->type == SKSE::MessagingInterface::kPostLoadGame) {
        auto taskInterface = SKSE::GetTaskInterface();
        if (taskInterface) {
            taskInterface->AddTask([]() {
                auto manager = RE::BGSSaveLoadManager::GetSingleton();
                if (!manager || manager->lastFileName.empty()) {
                    return;
                }

                // Criamos o objeto na stack e limpamos a memória inicial
                RE::BGSSaveLoadFileEntry tempEntry;
                std::memset(&tempEntry, 0, sizeof(RE::BGSSaveLoadFileEntry));
                tempEntry.fileName = manager->lastFileName;

                bool success = false;
                // Tenta até 5 vezes caso o arquivo esteja bloqueado pela engine
                for (int i = 0; i < 5; ++i) {
                    if (tempEntry.PopulateFileEntryData()) {
						logger::debug("Tentativa {}: Sucesso ao ler o cabeçalho do save: {}", i + 1, tempEntry.fileName.c_str());
                        success = true;
                        break;
                    }
                    // Se falhar, aguarda 200ms antes da próxima tentativa
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }

                if (success) {
                    SKSE::log::info("--- Save Atual Detectado (Sucesso na tentativa) ---");
                    SKSE::log::info("Arquivo: {}", tempEntry.fileName.c_str());
                    SKSE::log::info("Personagem: {}", tempEntry.characterName.c_str());
                    SKSE::log::info("Nível: {}", tempEntry.characterLevel);
                    SKSE::log::info("Número do Save: {}", tempEntry.saveNumber);
                    SKSE::log::info("Tempo de Jogo: {}", tempEntry.playTime.c_str());
                    SKSE::log::info("ID do Personagem: {:X}", tempEntry.characterID);
                }
                else {
                    SKSE::log::error("Erro persistente: Falha ao ler o cabeçalho após várias tentativas: {}", manager->lastFileName.c_str());
                }

                // Limpeza de segurança para evitar o crash !wide() no destruidor
                std::memset(&tempEntry, 0, sizeof(RE::BGSSaveLoadFileEntry));
                });
        }
    }

    
    if (message->type == SKSE::MessagingInterface::kNewGame) {
        SaveStateManager::GetSingleton()->LoadData("NewGame");
        const char* saveName = (const char*)message->data; // kSaveGame data is the save name string?
        // Actually kSaveGame data consists of the save name string (const char*)
        if (saveName) {
            SaveStateManager::GetSingleton()->SaveData(saveName);
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface *skse) {

    SetupLog();
    logger::info("Plugin loaded");
    SKSE::Init(skse);
    SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
    return true;
}
