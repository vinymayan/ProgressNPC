#pragma once

#include <string>
#include <map>
#include <set>
#include <nlohmann/json.hpp>

// Helper structure to track applied rules
// Key: NPC FormID
// Value: Set of Rule IDs
class SaveStateManager {
public:
    static SaveStateManager* GetSingleton() {
        static SaveStateManager singleton;
        return &singleton;
    }

    void LoadData(const std::string& saveName);
    void SaveData(const std::string& saveName);

    bool IsRuleApplied(RE::FormID npcID, const std::string& ruleID);
    void MarkRuleApplied(RE::FormID npcID, const std::string& ruleID);

private:
    std::string GetSavePath(const std::string& saveName);

    // Map<NPC FormID, Set<RuleID>>
    std::map<RE::FormID, std::set<std::string>> _appliedRules;
};