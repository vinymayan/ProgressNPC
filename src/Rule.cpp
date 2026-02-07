#include "Rule.h"
#include <fstream>
#include <filesystem>
#include <random>


// Helper to split string
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

std::pair<std::string, RE::FormID> Reward::ParseFormID() const {
    auto tokens = split(formIDStr, '|');
    if (tokens.size() == 2) {
        // Plugin | ID
        return { tokens[0], std::stoul(tokens[1], nullptr, 16) };
    } else if (tokens.size() == 1) {
        // ID only (Assumes Skyrim.esm or no load order index) -> Unsafe but handled
        return { "", std::stoul(tokens[0], nullptr, 16) };
    }
    return { "", 0 };
}

void to_json(json& j, const Reward& p) {
    j = json{{"typeReward", p.typeReward}, {"FormID", p.formIDStr}, {"Amount", p.amount}};
}

void from_json(const json& j, Reward& p) {
    j.at("typeReward").get_to(p.typeReward);
    j.at("FormID").get_to(p.formIDStr);
    if (j.contains("Amount")) {
        j.at("Amount").get_to(p.amount);
    } else {
        p.amount = 1;
    }
}

void to_json(json& j, const Rule& p) {
    j = json{ {"id", p.id}, {"name", p.name}, {"type", p.type}, {"level", p.level}, {"filterFormIDs", p.filterFormIDs}, {"Reward", p.rewards} };
}

void from_json(const json& j, Rule& p) {
    j.at("id").get_to(p.id);
    if (j.contains("name")) j.at("name").get_to(p.name); // Suporte a arquivos antigos
    j.at("type").get_to(p.type);
    j.at("level").get_to(p.level);
    if (j.contains("filterFormIDs")) j.at("filterFormIDs").get_to(p.filterFormIDs);
    j.at("Reward").get_to(p.rewards);
}

void RuleManager::LoadRules() {
    std::ifstream i(_filename);
    if (!i.is_open()) {
        logger::warn("Rules file not found: {}", _filename);
        return;
    }
    
    try {
        json j;
        i >> j;
        _rules = j.get<std::vector<Rule>>();
        logger::info("Loaded {} rules.", _rules.size());
    } catch (const json::parse_error& e) {
        logger::error("JSON Parse Error: {}", e.what());
    }
}

void RuleManager::SaveRules() {
    std::filesystem::path path(_filename);
    std::filesystem::create_directories(path.parent_path());

    std::ofstream o(_filename);
    json j = _rules;
    o << std::setw(4) << j << std::endl;
    logger::info("Saved rules to {}", _filename);
}

std::string GenerateUUID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    int i;
    ss << std::hex;
    for (i = 0; i < 8; i++) ss << dis(gen);
    return ss.str();
}

Rule& RuleManager::CreateRule() {
    Rule r;
    r.id = GenerateUUID();
    r.level = 1;
    r.type = "NPC"; // Agora é uma string simples para o Combo da UI
    r.filterFormIDs = {};
    _rules.push_back(r);
    return _rules.back();
}

void RuleManager::DeleteRule(const std::string& id) {
    std::erase_if(_rules, [&](const Rule& r) { return r.id == id; });
}

std::vector<Reward> RuleManager::GetRewardsForNPC(RE::TESNPC* npc) {
    std::vector<Reward> applicable;
    if (!npc) return applicable;

    // Obtém o identificador do NPC no formato "Plugin|FormID" para comparar com os filtros
    std::string npcPlugin = "";
    if (auto file = npc->GetFile(0)) {
        npcPlugin = file->GetFilename();
    }
    std::string npcIdentifier = npcPlugin + "|" + std::to_string(npc->GetFormID());

    for (const auto& rule : _rules) {
        // 1. Verifica se a regra é do tipo NPC
        if (rule.type != "NPC") continue;

        // 2. Lógica de Filtro:
        // Se a lista de filtros estiver vazia, a regra é GLOBAL (afeta todos os NPCs)
        // Se tiver itens, o NPC atual deve estar na lista.
        bool isAffected = rule.filterFormIDs.empty();

        if (!isAffected) {
            for (const auto& targetID : rule.filterFormIDs) {
                if (targetID == npcIdentifier) {
                    isAffected = true;
                    break;
                }
            }
        }

        if (isAffected) {
            // Nota: O check de level deve ser feito no Actor* em runtime usando SaveStateManager
            // Aqui apenas retornamos o que este NPC *poderia* ganhar.
            applicable.insert(applicable.end(), rule.rewards.begin(), rule.rewards.end());
        }
    }

    return applicable;
}