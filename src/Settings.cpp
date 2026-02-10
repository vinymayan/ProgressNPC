#include "Settings.h"
#include <fstream>
#include <filesystem>

std::vector<SaveHistoryEntry>& SaveStateManager::GetCharacterHistory(uint32_t characterID) {
    // Retorna a referência do histórico ou cria uma entrada vazia caso não exista
    return _characterHistory[characterID];
}

void SaveStateManager::PersistCurrentSave(const std::string& a_saveName) {
    auto& context = _currentContext;
    if (!context.isValid) return;

    // 1. Extração robusta do NOVO save number do nome do arquivo (ex: "Save5_...")
    uint32_t newSaveNumber = 0;
    try {
        std::string fileName = a_saveName;
        // Remove caminhos de diretório para focar apenas no nome do arquivo
        size_t lastSlash = fileName.find_last_of("\\/");
        std::string baseName = (lastSlash == std::string::npos) ? fileName : fileName.substr(lastSlash + 1);

        size_t savePos = baseName.find("Save");
        size_t underscorePos = baseName.find('_');

        if (savePos != std::string::npos && underscorePos != std::string::npos) {
            std::string numStr = baseName.substr(savePos + 4, underscorePos - (savePos + 4));
            newSaveNumber = std::stoul(numStr);
        }
        else {
            newSaveNumber = context.saveNumber; // Fallback para o atual se falhar
        }
    }
    catch (const std::exception& e) {
        logger::error("[Persist] Erro ao extrair save number de '{}': {}", a_saveName, e.what());
        newSaveNumber = context.saveNumber;
    }

    auto& history = _characterHistory[context.charID];
    auto currentRules = RuleManager::GetSingleton()->GetRules();

    // 2. Localiza o progresso da sessão atual (baseado no que foi carregado/processado até agora)
    SaveHistoryEntry sessionProgress;
    bool foundSession = false;
    for (const auto& h : history) {
        if (h.saveNumber == context.saveNumber) {
            sessionProgress = h;
            foundSession = true;
            break;
        }
    }

    // 3. Cria a nova entrada para o JSON herdando os NPCs já afetados nesta sessão
    SaveHistoryEntry newEntry;
    newEntry.saveNumber = newSaveNumber;
    newEntry.saveName = a_saveName;
    newEntry.appliedRulesSnapshot = currentRules;

    // Herança: Mantém o registro de quais NPCs já receberam quais regras
    if (foundSession) {
        newEntry.npcAppliedRules = sessionProgress.npcAppliedRules;
    }

    // 4. Persiste a nova entrada no histórico e grava no disco
    UpdateSaveEntry(context.charID, newEntry);

    // 5. ATUALIZAÇÃO DO CONTEXTO (Importante):
    // Agora o "save atual" da sessão passa a ser o novo número.
    // Isso evita que o plugin tente buscar no número antigo se o jogador continuar jogando.
    uint32_t oldNumber = context.saveNumber;
    context.saveNumber = newSaveNumber;

    logger::info("[SaveManager] Sincronização concluída: Transição Save {} -> {} registrada.", oldNumber, newSaveNumber);
}

std::string SaveStateManager::GetCharacterPath(uint32_t characterID) {
    char hexID[9];
    sprintf_s(hexID, "%08X", characterID);
    return "Data/SKSE/Plugins/ProgressNPC/Saves/" + std::string(hexID) + ".json";
}

void SaveStateManager::LoadCharacterData(uint32_t characterID) {
    std::string path = GetCharacterPath(characterID);
    std::ifstream i(path);
    if (!i.is_open()) return;

    try {
        nlohmann::json j;
        i >> j;
        _characterHistory[characterID] = j.get<std::vector<SaveHistoryEntry>>();
        //logger::info("Historico carregado para personagem {:X}: {} entradas.", characterID, _characterHistory[characterID].size());
    }
    catch (...) {
        logger::error("Erro ao ler dados do personagem {:X}", characterID);
    }
}

void SaveStateManager::UpdateSaveEntry(uint32_t characterID, const SaveHistoryEntry& newEntry) {
    auto& history = _characterHistory[characterID];

    // Tenta encontrar se este saveNumber já existe para atualizar, senão adiciona
    auto it = std::find_if(history.begin(), history.end(), [&](const SaveHistoryEntry& e) {
        return e.saveNumber == newEntry.saveNumber;
        });

    if (it != history.end()) *it = newEntry;
    else history.push_back(newEntry);

    // Salva o arquivo físico
    std::string path = GetCharacterPath(characterID);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream o(path);
    nlohmann::json j = history;
    o << j.dump(4) << std::endl;
}