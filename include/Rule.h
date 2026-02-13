#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Manager.h"

// Define types for JSON usage
using json = nlohmann::json;
std::vector<std::string> split(const std::string& s, char delimiter);
struct Reward {
    std::string typeReward; // "Spell", "Perk", "Weapon", "Keyword"
    std::string formIDStr;  // e.g. "Skyrim.esm|D8D4E" (Plugin|FormID) or "D8D4E" (FormID only - unsafe without plugin)
    uint32_t amount = 1;
    float chanceReward = 100.0f;
    bool lootable = true;
    // Helper to separate Plugin | FormID
    std::pair<std::string, RE::FormID> ParseFormID() const;
};

struct RewardGroup {
    std::string name = "New Group";
    bool isExclusive = false; // Modo Rolagem: Independente vs Exclusivo
    std::vector<Reward> rewards;
};

struct BlacklistFilter {
    std::string type;      // "NPC", "Faction", "Race", "Keyword"
    std::string formIDStr; // Plugin|FormID
};

void to_json(json& j, const Reward& p);
void from_json(const json& j, Reward& p);

void to_json(json& j, const RewardGroup& p); 
void from_json(const json& j, RewardGroup& p); 

void to_json(json& j, const BlacklistFilter& p);
void from_json(const json& j, BlacklistFilter& p);

struct Rule {
    std::string id;
    std::string name;
    std::string type = "NPC";
    int level = 1;
    int version = 0;
    // Novos campos de Alvos (Substituem type e filterFormIDs)
    int targetGender = 0;
    bool targetRequiresAll = false;
    std::vector<BlacklistFilter> targetFilters; // Usando a mesma struct de filtro
    std::vector<RewardGroup> rewardGroups;

    int blacklistedGender = 0;       // 0: Nenhum, 1: Male, 2: Female
    bool blacklistRequiresAll = false;
    std::vector<BlacklistFilter> blacklistFilters;

    mutable std::string lastSavedHash;

    // Calcula um hash baseado no conteúdo estrutural da regra
    std::string CalculateHash() const {
        nlohmann::json j;
        j["name"] = name;
        j["level"] = level;
        j["t_gender"] = targetGender;
        j["t_reqAll"] = targetRequiresAll;
        j["t_filters"] = targetFilters;
        j["groups"] = rewardGroups;
        j["b_gender"] = blacklistedGender;
        j["b_reqAll"] = blacklistRequiresAll;
        j["b_filters"] = blacklistFilters;
        return std::to_string(std::hash<std::string>{}(j.dump()));
    }

    bool IsModified() const {
        return lastSavedHash != CalculateHash();
    }
};

void to_json(json& j, const Rule& p);
void from_json(const json& j, Rule& p);

bool IsNPCMatchingTargets(RE::TESNPC* npc, const Rule& rule, bool isBlacklist);

struct AffectedNPC {
    RE::FormID npcFormID;
    std::string npcName;
    std::vector<std::string> ruleIDs; // IDs das regras que afetam este NPC
};

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
    
    // Adicione a declaração na classe RuleManager
    std::vector<Reward> GetRewardsForSpecificRule(RE::TESNPC* npc, const Rule& rule);

    void InitializeAffectedNPCsDatabase();
    float GetRandomFloat(float a_min, float a_max) {
        static thread_local std::mt19937 gen{ std::random_device{}() };
        std::uniform_real_distribution<float> dis(a_min, a_max);
        return dis(gen);
    }
    // Getter para o banco de dados
    const std::map<RE::FormID, AffectedNPC>& GetAffectedNPCsDatabase() { return _affectedNPCsDatabase; }

    Rule* GetRuleVersion(const std::string& ruleID, int version);
private:
    std::vector<Rule> _rules;
    std::map<std::string, std::vector<Rule>> _ruleHistories;
    std::map<RE::FormID, AffectedNPC> _affectedNPCsDatabase;
    std::map<std::string, std::string> _ruleIdToFileName;
    std::vector<std::string> _rulesToDelete;
    const std::string _rulesDir = "Data/SKSE/Plugins/EDF/Rules/";
};

std::string FormatLocalFormID(uint32_t a_formID, const std::string& a_pluginName);
