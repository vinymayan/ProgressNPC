#pragma once
#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>
#include "Rule.h"

struct SaveHistoryEntry {
    uint32_t saveNumber;
    std::string playTime;
    std::string saveName;
    std::vector<Rule> appliedRulesSnapshot; // Como as regras eram no momento do save
    std::vector<RE::FormID> affectedNPCs;
    std::map<std::string, std::vector<std::string>> npcAppliedRules;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SaveHistoryEntry, saveNumber, playTime, saveName, appliedRulesSnapshot, affectedNPCs, npcAppliedRules)
};

struct CurrentSaveContext {
    uint32_t charID = 0;
    uint32_t saveNumber = 0;
    std::string playTime = "";
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

    // Verifica se uma regra mudou comparando com o snapshot salvo
    bool DidRuleChange(const std::string& ruleID, const SaveHistoryEntry& lastEntry);

    std::vector<SaveHistoryEntry>& GetCharacterHistory(uint32_t characterID);

    void SetCurrentContext(uint32_t a_charID, uint32_t a_saveNum, std::string a_time) {
        _currentContext.charID = a_charID;
        _currentContext.saveNumber = a_saveNum;
        _currentContext.playTime = a_time;
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