#include "Settings.h"
#include <fstream>
#include <filesystem>

using json = nlohmann::json;

std::string SaveStateManager::GetSavePath(const std::string& saveName) {
    // Sanitize saveName just in case (e.g. remove path separators)
    std::string safeName = std::filesystem::path(saveName).filename().string();
    return "Data/SKSE/Plugins/ProgressNPC/Saves/" + safeName + ".json";
}

void SaveStateManager::LoadData(const std::string& saveName) {
    _appliedRules.clear();
    std::string path = GetSavePath(saveName);

    std::ifstream i(path);
    if (!i.is_open()) return;

    try {
        json j;
        i >> j;

        // Format: { "FormID (Decimal or Hex String)" : ["RuleID1", "RuleID2"] }
        for (auto& [key, value] : j.items()) {
            RE::FormID formID = std::stoul(key);
            std::set<std::string> rules;
            for (const auto& r : value) {
                rules.insert(r.get<std::string>());
            }
            _appliedRules[formID] = rules;
        }
        logger::info("Loaded Save State for {}: {} NPCs affected.", saveName, _appliedRules.size());
    }
    catch (...) {
        logger::error("Failed to load save state: {}", path);
    }
}

void SaveStateManager::SaveData(const std::string& saveName) {
    std::string path = GetSavePath(saveName);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());

    json j;
    for (const auto& [formID, rules] : _appliedRules) {
        j[std::to_string(formID)] = rules;
    }

    std::ofstream o(path);
    o << j << std::endl;
    logger::info("Saved state to {}", path);
}

bool SaveStateManager::IsRuleApplied(RE::FormID npcID, const std::string& ruleID) {
    if (_appliedRules.find(npcID) == _appliedRules.end()) return false;
    return _appliedRules[npcID].contains(ruleID);
}

void SaveStateManager::MarkRuleApplied(RE::FormID npcID, const std::string& ruleID) {
    _appliedRules[npcID].insert(ruleID);
}
