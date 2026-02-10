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
    // Helper to separate Plugin | FormID
    std::pair<std::string, RE::FormID> ParseFormID() const;
};

struct RewardGroup {
    std::string name = "Novo Grupo";
    bool isExclusive = false; // Modo Rolagem: Independente vs Exclusivo
    std::vector<Reward> rewards;
};

void to_json(json& j, const Reward& p);
void from_json(const json& j, Reward& p);

void to_json(json& j, const RewardGroup& p); // Adicionado
void from_json(const json& j, RewardGroup& p); // Adicionado

struct Rule {
    std::string id;
    std::string name;
    std::string type = "NPC";
    int level = 1;
    std::vector<std::string> filterFormIDs;
    std::vector<RewardGroup> rewardGroups;

    // Novo: Armazena a versão atual da regra baseada no conteúdo
    std::string versionHash;

    // Calcula um hash baseado no conteúdo estrutural da regra
    std::string CalculateHash() const {
        nlohmann::json j;
        j["type"] = type;
        j["level"] = level;
        j["targets"] = filterFormIDs;
        j["groups"] = rewardGroups;
        return std::to_string(std::hash<std::string>{}(j.dump()));
    }
    bool IsModified() const {
        return versionHash != CalculateHash();
    }
};

void to_json(json& j, const Rule& p);
void from_json(const json& j, Rule& p);

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
    void GenerateDistributionReport();
    // Adicione a declaração na classe RuleManager
    std::vector<Reward> GetRewardsForSpecificRule(RE::TESNPC* npc, const Rule& rule);

    void InitializeAffectedNPCsDatabase();

    // Getter para o banco de dados
    const std::map<RE::FormID, AffectedNPC>& GetAffectedNPCsDatabase() { return _affectedNPCsDatabase; }

private:
    std::vector<Rule> _rules;
    std::map<RE::FormID, AffectedNPC> _affectedNPCsDatabase;
    const std::string _filename = "Data/SKSE/Plugins/ProgressNPC/_DISTR.json";
};

std::string FormatLocalFormID(uint32_t a_formID, const std::string& a_pluginName);

class RuleProcessor {
public:
    // Retorna true se a mudança na regra EXIGE que ela seja redistribuída
    static bool ShouldReapplyRule(const Rule& oldRule, const Rule& newRule) {
        if (oldRule.versionHash == newRule.versionHash) return false;

        // Se o nível mudou ou os alvos (filtros) mudaram, deve reaplicar
        if (oldRule.level != newRule.level || oldRule.filterFormIDs != newRule.filterFormIDs) return true;

        // Regra Especial: Grupos Exclusivos
        // Se a mudança foi apenas a adição de um item em um grupo exclusivo,
        // e o grupo já existia, não reaplicamos (conforme seu pedido).
        if (oldRule.rewardGroups.size() == newRule.rewardGroups.size()) {
            for (size_t i = 0; i < oldRule.rewardGroups.size(); i++) {
                if (newRule.rewardGroups[i].isExclusive && oldRule.rewardGroups[i].isExclusive) {
                    // Mudança em grupo exclusivo ignorada para NPCs que já a possuem
                    continue;
                }
                // Se um grupo NÃO exclusivo mudou, precisamos reaplicar
                if (oldRule.rewardGroups[i].rewards.size() != newRule.rewardGroups[i].rewards.size()) return true;
            }
        }
        else {
            return true; // Quantidade de grupos mudou
        }

        return false;
    }
};

