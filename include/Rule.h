#pragma once

#include <string>
#include <vector>
#include <rapidjson/document.h>
#include "Manager.h"

std::vector<std::string> split(const std::string& s, char delimiter);
struct Reward {
    std::string typeReward; // "Spell", "Perk", "Weapon", "Keyword"
    std::string formIDStr;  // e.g. "Skyrim.esm|D8D4E" (Plugin|FormID) or "D8D4E" (FormID only - unsafe without plugin)
    uint32_t amount = 1;
    float chanceReward = 100.0f;
    int functionOnType = 0;
    bool isPersistent = true;
    //bool lootable = true;
    // Helper to separate Plugin | FormID
    std::pair<std::string, RE::FormID> ParseFormID() const;
};

struct RewardGroup {
    std::string name = "New Group";
    bool isExclusive = false; // Modo Rolagem: Independente vs Exclusivo
    float chanceGroup = 100.0f;
    std::vector<Reward> rewards;
};

struct BlacklistFilter {
    std::string type;      // "NPC", "Faction", "Race", "Keyword"
    std::string formIDStr; // Plugin|FormID
};


struct Rule {
    std::string id;
    std::string name;
    bool isEnabled = true;
    std::string type = "NPC";
    int level = 1;
    int version = 0;
    // Novos campos de Alvos (Substituem type e filterFormIDs)
    int targetGender = 0;
    bool targetRequiresAll = false;
    bool isExclusive = false;
    std::vector<BlacklistFilter> targetFilters; // Usando a mesma struct de filtro
    std::vector<RewardGroup> rewardGroups;

    int blacklistedGender = 0;       // 0: Nenhum, 1: Male, 2: Female
    bool blacklistRequiresAll = false;
    std::vector<BlacklistFilter> blacklistFilters;

    mutable std::string lastSavedHash;
    // Calcula um hash baseado no conteúdo estrutural da regra
    std::string CalculateHash() const;

    bool IsModified() const {
        return lastSavedHash != CalculateHash();
    }
};


bool IsNPCMatchingTargets(RE::TESNPC* npc, const Rule& rule, bool isBlacklist, RE::Actor* actor = nullptr);

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

    bool IsAffected(RE::Actor* actor);

    void LoadRules();
    void SaveRules();
    void ExportRule(const Rule& rule);
    std::vector<Rule>& GetRules() { return _rules; }

    // Create specific rule
    Rule& CreateRule();
    void  DeleteRule(const std::string& id);

    // Apply rules to an NPC (Validation logic)
    // Returns list of rewards to apply
    std::vector<Reward> GetRewardsForNPC(RE::TESNPC* npc);

    std::vector<RewardGroup> RollForGroups(RE::TESNPC* npc, const Rule& rule);
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
