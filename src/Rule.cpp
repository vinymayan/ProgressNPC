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

// Novo Helper para formatar o FormID conforme a regra solicitada
std::string FormatLocalFormID(uint32_t a_formID, const std::string& a_pluginName) {
    std::string plugin = a_pluginName;
    std::transform(plugin.begin(), plugin.end(), plugin.begin(), ::tolower);

    auto dataHandler = RE::TESDataHandler::GetSingleton();
    auto file = dataHandler ? dataHandler->LookupModByName(plugin) : nullptr;

    char buf[10];
    // Plugins "Light" (ESL ou ESP com flag FE) usam os últimos 3 dígitos (12 bits)
    if (file && file->IsLight()) {
        sprintf_s(buf, "%03X", a_formID & 0x00000FFF);
    }
    // Plugins "Full" (ESM e ESP comuns) usam os últimos 6 dígitos (24 bits)
    else {
        sprintf_s(buf, "%06X", a_formID & 0x00FFFFFF);
    }
    return std::string(buf);
}

std::pair<std::string, RE::FormID> Reward::ParseFormID() const {
    auto tokens = split(formIDStr, '|');
    if (tokens.size() == 2) {
        // Plugin | HexID
        // Agora convertendo explicitamente de Hexadecimal (Base 16)
        try {
            uint32_t localID = std::stoul(tokens[1], nullptr, 16);
            auto dataHandler = RE::TESDataHandler::GetSingleton();
            if (dataHandler) {
                auto form = dataHandler->LookupFormID(localID, tokens[0]);
                return { tokens[0], form };
            }
        }
        catch (...) {
            return { tokens[0], 0 };
        }
    }
    return { "", 0 };
}
void to_json(json& j, const Reward& p) {
    j = json{ {"typeReward", p.typeReward}, {"FormID", p.formIDStr}, {"Amount", p.amount}, {"Chance", p.chanceReward} };
}

void from_json(const json& j, Reward& p) {
    j.at("typeReward").get_to(p.typeReward);
    j.at("FormID").get_to(p.formIDStr);
    p.amount = j.value("Amount", 1);
    p.chanceReward = j.value("Chance", 100.0f);
}

void to_json(json& j, const RewardGroup& p) {
    j = json{ {"name", p.name}, {"exclusive", p.isExclusive}, {"rewards", p.rewards} };
}

void from_json(const json& j, RewardGroup& p) {
    j.at("name").get_to(p.name);
    j.at("exclusive").get_to(p.isExclusive);
    j.at("rewards").get_to(p.rewards);
}

void to_json(json& j, const Rule& p) {
    j = json{
        {"id", p.id},
        {"name", p.name},
        {"type", p.type},
        {"level", p.level},
        {"filterFormIDs", p.filterFormIDs},
        {"RewardGroups", p.rewardGroups},
        {"versionHash", p.versionHash} 
    };
}

void from_json(const json& j, Rule& p) {
    j.at("id").get_to(p.id);
    p.name = j.value("name", "Sem Nome");
    j.at("type").get_to(p.type);
    j.at("level").get_to(p.level);
    if (j.contains("filterFormIDs")) j.at("filterFormIDs").get_to(p.filterFormIDs);
    if (j.contains("versionHash")) j.at("versionHash").get_to(p.versionHash); 

    if (j.contains("RewardGroups")) {
        j.at("RewardGroups").get_to(p.rewardGroups);
    }
    else if (j.contains("Reward")) {
        RewardGroup defaultGroup;
        defaultGroup.name = "Migrated Rewards";
        j.at("Reward").get_to(defaultGroup.rewards);
        p.rewardGroups.push_back(defaultGroup);
    }
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
    int updatedCount = 0;
    for (auto& rule : _rules) {
        std::string newHash = rule.CalculateHash();
        if (rule.versionHash != newHash) {
            logger::info("[RuleManager] Regra '{}' modificada. Hash antigo: {} -> Novo: {}",
                rule.name, rule.versionHash, newHash);
            rule.versionHash = newHash;
            updatedCount++;
        }
    }

    std::filesystem::path path(_filename);
    std::filesystem::create_directories(path.parent_path());

    std::ofstream o(_filename);
    json j = _rules;
    o << std::setw(4) << j << std::endl;

    if (updatedCount > 0) {
        logger::info("Salvamento concluído: {} regras atualizadas no arquivo.", updatedCount);
    }
    else {
        logger::info("Salvamento concluído: Nenhuma alteração detectada nas regras.");
    }
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

    // 1. Obter o identificador do NPC no formato correto (Hexadecimal 5 ou 3 dígitos)
    std::string npcPlugin = "";
    if (auto file = npc->GetFile(0)) {
        npcPlugin = file->GetFilename();
    }

    // CORREÇÃO: Usar FormatLocalFormID em vez de std::to_string
    std::string npcIdentifier = npcPlugin + "|" + FormatLocalFormID(npc->GetFormID(), npcPlugin);

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 100.0f);

    for (const auto& rule : _rules) {
        bool isAffected = false;

        // Se a lista de filtros estiver vazia, a regra afeta todos os NPCs (Global)
        if (rule.filterFormIDs.empty()) {
            isAffected = true;
        }
        else {
            // CORREÇÃO: Lógica baseada no tipo da regra (NPC, Keyword ou Faction)
            if (rule.type == "NPC") {
                for (const auto& targetID : rule.filterFormIDs) {
                    if (targetID == npcIdentifier) {
                        isAffected = true;
                        break;
                    }
                }
            }
            else if (rule.type == "Keyword") {
                for (const auto& targetID : rule.filterFormIDs) {
                    auto tokens = split(targetID, '|');
                    if (tokens.size() == 2) {
                        uint32_t localID = std::stoul(tokens[1], nullptr, 16);
                        // CORREÇÃO: Primeiro resolve o FormID, depois busca o objeto
                        auto fullFormID = RE::TESDataHandler::GetSingleton()->LookupFormID(localID, tokens[0]);
                        auto kwd = RE::TESForm::LookupByID<RE::BGSKeyword>(fullFormID);

                        if (kwd && npc->HasKeyword(kwd)) {
                            isAffected = true;
                            break;
                        }
                    }
                }
            }
            else if (rule.type == "Faction") {
                for (const auto& targetID : rule.filterFormIDs) {
                    auto tokens = split(targetID, '|');
                    if (tokens.size() == 2) {
                        uint32_t localID = std::stoul(tokens[1], nullptr, 16);
                        // CORREÇÃO: Primeiro resolve o FormID, depois busca o objeto
                        auto fullFormID = RE::TESDataHandler::GetSingleton()->LookupFormID(localID, tokens[0]);
                        auto faction = RE::TESForm::LookupByID<RE::TESFaction>(fullFormID);

                        if (faction && npc->IsInFaction(faction)) {
                            isAffected = true;
                            break;
                        }
                    }
                }
            }
        }

        // Se o NPC passou no filtro, processa os grupos de recompensa
        if (isAffected) {
            for (const auto& group : rule.rewardGroups) {
                if (group.isExclusive) {
                    float roll = dis(gen);
                    float cumulative = 0.0f;
                    for (const auto& reward : group.rewards) {
                        cumulative += reward.chanceReward;
                        if (roll <= cumulative) {
                            applicable.push_back(reward);
                            break;
                        }
                    }
                }
                else {
                    for (const auto& reward : group.rewards) {
                        if (dis(gen) <= reward.chanceReward) {
                            applicable.push_back(reward);
                        }
                    }
                }
            }
        }
    }

    return applicable;
}


void RuleManager::GenerateDistributionReport() {
    logger::info("==================================================");
    logger::info("INICIANDO RELATÓRIO DE DISTRIBUIÇÃO DINÂMICA");
    logger::info("==================================================");

    auto npcList = Manager::GetSingleton()->GetList("NPC");
    size_t totalNpcs = npcList.size();
    size_t affectedCount = 0;
    size_t totalRewards = 0;

    logger::info("Total de NPCs indexados: {}", totalNpcs);
    logger::info("Total de Regras ativas: {}", _rules.size());

    for (const auto& npcInfo : npcList) {
        // Tenta obter o ponteiro real do NPC a partir do FormID
        auto npc = RE::TESForm::LookupByID<RE::TESNPC>(npcInfo.formID);
        if (!npc) continue;

        // Simula a aplicação das regras
        std::vector<Reward> results = GetRewardsForNPC(npc);

        if (!results.empty()) {
            affectedCount++;
            totalRewards += results.size();

            std::string rewardsStr = "";
            for (const auto& r : results) {
                rewardsStr += "[" + r.typeReward + ": " + r.formIDStr + "] ";
            }

            logger::info("NPC: {} ({:X}) | Plugin: {} -> Receberia: {}",
                npcInfo.name.empty() ? npcInfo.editorID : npcInfo.name,
                npcInfo.formID,
                npcInfo.pluginName,
                rewardsStr);
        }
    }

    logger::info("--------------------------------------------------");
    logger::info("RESUMO DO RELATÓRIO:");
    logger::info("NPCs que passaram nos filtros: {}", affectedCount);
    logger::info("Total de instâncias de itens distribuídas: {}", totalRewards);
    logger::info("==================================================");
}

std::vector<Reward> RuleManager::GetRewardsForSpecificRule(RE::TESNPC* npc, const Rule& rule) {
    std::vector<Reward> applicable;
    if (!npc) return applicable;

    static std::random_device rd;
    static std::mt19937 gen(rd());
    std::uniform_real_distribution<float> dis(0.0f, 100.0f);

    // Como o RuleProcessor já validou o "quem" (NPC) e o "quando" (Nível),
    // aqui focamos apenas no "o quê" (Sorteio dos Grupos)
    for (const auto& group : rule.rewardGroups) {
        if (group.rewards.empty()) continue;

        if (group.isExclusive) {
            // Lógica de Sorteio Único (Exclusivo)
            float roll = dis(gen);
            float cumulative = 0.0f;
            for (const auto& reward : group.rewards) {
                cumulative += reward.chanceReward;
                if (roll <= cumulative) {
                    applicable.push_back(reward);
                    break; // Sai do grupo após ganhar um item
                }
            }
        }
        else {
            // Lógica de Sorteio Independente
            for (const auto& reward : group.rewards) {
                if (dis(gen) <= reward.chanceReward) {
                    applicable.push_back(reward);
                }
            }
        }
    }

    return applicable;
}

void RuleManager::InitializeAffectedNPCsDatabase() {
    logger::info("======================================================");
    logger::info("Iniciando Mapeamento de NPCs Afetados por Regras...");
    logger::info("======================================================");

    _affectedNPCsDatabase.clear();
    auto& rules = GetRules();
    auto npcList = Manager::GetSingleton()->GetList("NPC");

    if (rules.empty()) {
        logger::warn("[Database] Nenhuma regra carregada. Abortando mapeamento.");
        return;
    }

    logger::info("[Database] Analisando {} NPCs contra {} regras.", npcList.size(), rules.size());

    for (const auto& npcInfo : npcList) {
        auto npc = RE::TESForm::LookupByID<RE::TESNPC>(npcInfo.formID);
        if (!npc) continue;

        std::string npcIdentifier = npcInfo.pluginName + "|" + FormatLocalFormID(npcInfo.formID, npcInfo.pluginName);
        AffectedNPC affectedInfo;
        affectedInfo.npcFormID = npcInfo.formID;
        affectedInfo.npcName = npc->GetName();

        for (const auto& rule : rules) {
            bool isAffected = false;

            // 1. Regra Global (Sem filtros = afeta todos)
            if (rule.filterFormIDs.empty()) {
                isAffected = true;
                logger::debug("  [Match] NPC '{}' ({:X}) afetado por Regra Global: {}", affectedInfo.npcName, npcInfo.formID, rule.name);
            }
            else {
                // 2. Filtro por FormID direto (Tipo NPC)
                if (rule.type == "NPC") {
                    if (std::find(rule.filterFormIDs.begin(), rule.filterFormIDs.end(), npcIdentifier) != rule.filterFormIDs.end()) {
                        isAffected = true;
                    }
                }
                // 3. Filtros Complexos (Keyword/Faction)
                else {
                    for (const auto& filterStr : rule.filterFormIDs) {
                        auto tokens = split(filterStr, '|');
                        if (tokens.size() < 2) continue;

                        auto filterFormID = RE::TESDataHandler::GetSingleton()->LookupFormID(std::stoul(tokens[1], nullptr, 16), tokens[0]);

                        if (rule.type == "Keyword") {
                            auto kwd = RE::TESForm::LookupByID<RE::BGSKeyword>(filterFormID);
                            if (kwd && npc->HasKeyword(kwd)) isAffected = true;
                        }
                        else if (rule.type == "Faction") {
                            auto fact = RE::TESForm::LookupByID<RE::TESFaction>(filterFormID);
                            if (fact && npc->IsInFaction(fact)) isAffected = true;
                        }

                        if (isAffected) break;
                    }
                }
            }

            if (isAffected) {
                affectedInfo.ruleIDs.push_back(rule.id);
            }
        }

        // Se o NPC foi afetado por pelo menos uma regra, adiciona ao banco
        if (!affectedInfo.ruleIDs.empty()) {
            _affectedNPCsDatabase[npcInfo.formID] = affectedInfo;
            logger::debug("[Database] NPC Adicionado: {} | Regras: {}", affectedInfo.npcName, affectedInfo.ruleIDs.size());
        }
    }

    logger::info("======================================================");
    logger::info("Mapeamento concluído. NPCs afetados encontrados: {}", _affectedNPCsDatabase.size());
    logger::info("======================================================");
}

