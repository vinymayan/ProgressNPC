#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Manager.h"

// Define types for JSON usage
using json = nlohmann::json;

struct Reward {
    std::string typeReward; // "Spell", "Perk", "Weapon", "Keyword"
    std::string formIDStr;  // e.g. "Skyrim.esm|D8D4E" (Plugin|FormID) or "D8D4E" (FormID only - unsafe without plugin)
    uint32_t amount = 1;

    // Helper to separate Plugin | FormID
    std::pair<std::string, RE::FormID> ParseFormID() const;
};

struct Rule {
    std::string id;
    std::string name;
    std::string type = "NPC"; // Alterado para string única para facilitar o combo
    int level = 1;
    std::vector<std::string> filterFormIDs; // Lista de "Plugin|ID" dos alvos selecionados
    std::vector<Reward> rewards;
};

void to_json(json& j, const Reward& p);
void from_json(const json& j, Reward& p);

void to_json(json& j, const Rule& p);
void from_json(const json& j, Rule& p);

class RuleManager {
public:
    static RuleManager* GetSingleton() {
        static RuleManager singleton;
        return &singleton;
    }

    void LoadRules();
    void SaveRules();
    
    std::vector<Rule>& GetRules() { return _rules; }
    
    // Create specific rule
    Rule& CreateRule();
    void  DeleteRule(const std::string& id);

    // Apply rules to an NPC (Validation logic)
    // Returns list of rewards to apply
    std::vector<Reward> GetRewardsForNPC(RE::TESNPC* npc);

private:
    std::vector<Rule> _rules;
    const std::string _filename = "Data/SKSE/Plugins/ProgressNPC/_DISTR.json";
};
