#pragma once

#include <string>
#include <vector>
#include <set>
#include <filesystem>
#include <optional>
#include <rapidjson/document.h>
#include "Manager.h"

std::vector<std::string> split(const std::string& s, char delimiter);
struct Reward {
    std::string typeReward; // "Spell", "Perk", "Weapon", "Keyword"
    std::string formIDStr;  // e.g. "Skyrim.esm|D8D4E" (Plugin|FormID) or "D8D4E" (FormID only - unsafe without plugin)
    std::string editorID;
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
    std::string editorID;
};


struct Rule {
    std::string id;
    std::string packageID = "edf.local-rules";
    std::string name;
    bool isEnabled = true;
    std::string type = "NPC";
    int level = 1;
    int version = 0;
    // Novos campos de Alvos (Substituem type e filterFormIDs)
    int targetGender = 0;
    int targetHumanoid = 0;  // 0: Both, 1: Only humanoids, 2: Only non-humanoids
    int targetChild = 0;     // 0: Both, 1: Only children, 2: Only non-children
    bool targetRequiresAll = false;
    bool isExclusive = false;
    std::vector<BlacklistFilter> targetFilters; // Usando a mesma struct de filtro
    std::vector<RewardGroup> rewardGroups;

    int blacklistedGender = 0;       // 0: Nenhum, 1: Male, 2: Female
    int blacklistedHumanoid = 0;     // 0: Nenhum, 1: Humanoid, 2: Non-humanoid
    int blacklistedChild = 0;        // 0: Nenhum, 1: Child, 2: Non-child
    bool blacklistRequiresAll = false;
    std::vector<BlacklistFilter> blacklistFilters;

    mutable std::string lastSavedHash;
    // Calcula um hash baseado no conteúdo estrutural da regra
    std::string CalculateHash() const;

    bool IsModified() const {
        return lastSavedHash != CalculateHash();
    }
};

struct RulePackage {
    std::string id;
    std::string displayName;
    bool enabled = true;
    std::filesystem::path path;
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
    bool SaveRules();
    bool ExportRule(const Rule& rule);
    bool ExportRulesPackage(const std::string& packageName, const std::set<std::string>& ruleIDs);
    std::vector<Rule>& GetRules() { return _rules; }
    const std::vector<RulePackage>& GetPackages() const;
    std::optional<std::string> CreatePackage(std::string_view displayName);

    // Create specific rule
    Rule& CreateRule(std::string_view packageID = "edf.local-rules");
    bool  DeleteRule(const std::string& id);

    bool CreateRulesPackageSnapshot(
        const std::string& packageName,
        const std::vector<Rule>& rules,
        const std::filesystem::path& stagingRoot,
        RulePackage& outPackage);

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
    std::map<std::string, std::string> _ruleOwners;
    const std::string _modDir = "Data/Viny Mods/EDF";
    const std::string _exportDir = "Data/Viny Mods/EDF/Export/";
};

bool ParseLegacyRuleFile(
    const std::filesystem::path& path,
    Rule& latest,
    std::vector<Rule>& history,
    std::string& error);

std::string FormatLocalFormID(uint32_t a_formID, const std::string& a_pluginName);
RE::TESForm* ResolveEDFForm(const std::string& a_type, const std::string& a_editorID, const std::string& a_formIDStr);
RE::FormID ResolveEDFFormID(const std::string& a_type, const std::string& a_editorID, const std::string& a_formIDStr);
