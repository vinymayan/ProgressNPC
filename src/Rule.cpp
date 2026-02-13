#include "Rule.h"

namespace fs = std::filesystem;
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

std::string SanitizeFilename(std::string name) {
    if (name.empty()) return "Unnamed_Rule";

    // Caracteres proibidos em sistemas de arquivos
    std::string illegalChars = "<>:\"/\\|?*";
    for (char& c : name) {
        if (illegalChars.find(c) != std::string::npos) {
            c = '_'; // Substitui por underscore
        }
    }
    // Remove espaços no fim ou pontos que podem causar problemas
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) {
        name.pop_back();
    }
    return name;
}

void to_json(json& j, const Reward& p) {
    j = json{
        {"typeReward", p.typeReward},
        {"FormID", p.formIDStr},
        {"Amount", p.amount},
        {"Chance", p.chanceReward},
        {"Lootable", p.lootable} 
    };
}

void from_json(const json& j, Reward& p) {
    j.at("typeReward").get_to(p.typeReward);
    j.at("FormID").get_to(p.formIDStr);
    p.amount = j.value("Amount", 1);
    p.chanceReward = j.value("Chance", 100.0f);
    p.lootable = j.value("Lootable", true); 
}

void to_json(json& j, const RewardGroup& p) {
    j = json{ {"name", p.name}, {"exclusive", p.isExclusive}, {"rewards", p.rewards} };
}

void from_json(const json& j, RewardGroup& p) {
    j.at("name").get_to(p.name);
    j.at("exclusive").get_to(p.isExclusive);
    j.at("rewards").get_to(p.rewards);
}

void to_json(json& j, const BlacklistFilter& p) {
    j = json{ {"type", p.type}, {"formID", p.formIDStr} };
}

void from_json(const json& j, BlacklistFilter& p) {
    j.at("type").get_to(p.type);
    j.at("formID").get_to(p.formIDStr);
}

// Atualize o to_json e from_json da Rule:
void to_json(json& j, const Rule& p) {
    j = json{
        {"id", p.id}, {"name", p.name}, {"level", p.level}, {"version", p.version},
        {"targetGender", p.targetGender},
        {"targetRequiresAll", p.targetRequiresAll},
        {"targetFilters", p.targetFilters},
        {"RewardGroups", p.rewardGroups},
        {"blacklistedGender", p.blacklistedGender},
        {"blacklistRequiresAll", p.blacklistRequiresAll},
        {"blacklistFilters", p.blacklistFilters}
    };
}

void from_json(const json& j, Rule& p) {
    j.at("id").get_to(p.id);
    p.name = j.value("name", "Sem Nome");
    p.level = j.value("level", 1);
    p.version = j.value("version", 1);
    p.targetGender = j.value("targetGender", 0);
    p.targetRequiresAll = j.value("targetRequiresAll", false);
    if (j.contains("targetFilters")) j.at("targetFilters").get_to(p.targetFilters);
    if (j.contains("RewardGroups")) j.at("RewardGroups").get_to(p.rewardGroups);
    p.blacklistedGender = j.value("blacklistedGender", 0);
    p.blacklistRequiresAll = j.value("blacklistRequiresAll", false);
    if (j.contains("blacklistFilters")) j.at("blacklistFilters").get_to(p.blacklistFilters);
    p.lastSavedHash = p.CalculateHash();
}

bool IsNPCMatchingTargets(RE::TESNPC* npc, const Rule& rule, bool isBlacklist) {
    // 1. Seleciona os dados baseados no modo (Target vs Blacklist)
    int genderFilter = isBlacklist ? rule.blacklistedGender : rule.targetGender;
    const auto& filters = isBlacklist ? rule.blacklistFilters : rule.targetFilters;
    bool requiresAll = isBlacklist ? rule.blacklistRequiresAll : rule.targetRequiresAll;

    // 2. Verificação de Gênero (0: None, 1: Male, 2: Female)
    if (genderFilter != 0) {
        bool isFemale = npc->IsFemale();
        bool genderMatch = (genderFilter == 1 && !isFemale) || (genderFilter == 2 && isFemale);

        if (isBlacklist && genderMatch) return true;  // Se for blacklist e deu match no gênero, bloqueia
        if (!isBlacklist && !genderMatch) return false; // Se for target e NÃO deu match, descarta
    }

    // 3. Se não houver filtros de ID/Keyword/etc
    if (filters.empty()) {
        // Na Blacklist, vazio significa "não bloqueia ninguém". No Target, significa "afeta todos".
        return !isBlacklist;
    }

    int matches = 0;
    for (const auto& filter : filters) {
        bool match = false;
        auto tokens = split(filter.formIDStr, '|');
        if (tokens.size() < 2) continue;

        auto fID = RE::TESDataHandler::GetSingleton()->LookupFormID(std::stoul(tokens[1], nullptr, 16), tokens[0]);

        if (filter.type == "NPC") { if (npc->GetFormID() == fID) match = true; }
        else if (filter.type == "Keyword") {
            auto kwd = RE::TESForm::LookupByID<RE::BGSKeyword>(fID);
            if (kwd && npc->HasKeyword(kwd)) match = true;
        }
        else if (filter.type == "Faction") {
            auto fact = RE::TESForm::LookupByID<RE::TESFaction>(fID);
            if (fact && npc->IsInFaction(fact)) match = true;
        }
        else if (filter.type == "Race") {
            auto race = RE::TESForm::LookupByID<RE::TESRace>(fID);
            if (npc->race == race) match = true;
        }

        if (match) {
            matches++;
            if (!requiresAll) return true; // Se não exige todos, o primeiro match já valida
        }
    }

    return (requiresAll && matches == filters.size() && matches > 0);
}

void RuleManager::LoadRules() {
    _rules.clear();
    _ruleHistories.clear();
    _ruleIdToFileName.clear();

    if (!fs::exists(_rulesDir)) {
        fs::create_directories(_rulesDir);
        return;
    }

    for (const auto& entry : fs::directory_iterator(_rulesDir)) {
        if (entry.path().extension() == ".json") {
            std::ifstream i(entry.path());
            try {
                json j;
                i >> j;
                // Cada arquivo agora é uma lista (histórico)
                auto history = j.get<std::vector<Rule>>();
                if (!history.empty()) {
                    // A primeira posição [0] deve ser sempre a versão mais recente
                    std::string id = history[0].id;
                    _ruleHistories[id] = history;
                    _rules.push_back(history[0]);
                    _ruleIdToFileName[id] = entry.path().stem().string();
                }
            }
            catch (const std::exception& e) {
                logger::error("Erro ao carregar regra {}: {}", entry.path().string(), e.what());
            }
        }
    }
    logger::info("Carregadas {} regras com seus históricos.", _rules.size());
}

void RuleManager::SaveRules() {
    int updatedTotal = 0;

    for (const auto& filePath : _rulesToDelete) {
        if (fs::exists(filePath)) {
            fs::remove(filePath);
            logger::info("Arquivo deletado permanentemente: {}", filePath);
            updatedTotal++;
        }
    }
    _rulesToDelete.clear(); // Limpa a fila de espera

    for (auto& currentRule : _rules) {
        std::string currentContentHash = currentRule.CalculateHash();

        // Se o hash atual for diferente do último salvo
        if (currentRule.lastSavedHash != currentContentHash) {

            std::string newFileName = SanitizeFilename(currentRule.name);
            std::string oldFileName = _ruleIdToFileName[currentRule.id];

            // 2. Se o nome mudou, deleta o arquivo antigo para evitar duplicatas
            if (!oldFileName.empty() && oldFileName != newFileName) {
                std::string oldPath = _rulesDir + oldFileName + ".json";
                if (fs::exists(oldPath)) {
                    fs::remove(oldPath);
                    logger::info("Renomeando regra: deletando arquivo antigo '{}'", oldFileName);
                }
            }

            // 1. Incrementa a versão numérica
            currentRule.version++;
            currentRule.lastSavedHash = currentContentHash;

            // 2. Atualiza o histórico em memória
            auto& history = _ruleHistories[currentRule.id];
            history.insert(history.begin(), currentRule); // Adiciona a nova versão no topo

            // 3. Salva o arquivo individual com o histórico completo
            std::string filePath = _rulesDir + newFileName + ".json";
            std::ofstream o(filePath);
            json j = history; // O arquivo contém o array de versões
            o << std::setw(4) << j << std::endl;

            updatedTotal++;
            logger::info("Regra '{}' salva. Nova Versão: {}", currentRule.name, currentRule.version);
        }
    }

    if (updatedTotal > 0) {
        InitializeAffectedNPCsDatabase();
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
    _rules.push_back(r);
    return _rules.back();
}

void RuleManager::DeleteRule(const std::string& id) {
    // 1. Localiza o nome do arquivo antes de limpar os mapas
    if (_ruleIdToFileName.contains(id)) {
        std::string fileName = _ruleIdToFileName[id];
        std::string filePath = _rulesDir + fileName + ".json";

        // Adiciona à lista de pendências para deletar do disco no Save
        _rulesToDelete.push_back(filePath);

        // Limpa o rastro nos mapas
        _ruleIdToFileName.erase(id);
    }

    // 2. Remove do histórico
    _ruleHistories.erase(id);

    // 3. Remove do vetor principal
    std::erase_if(_rules, [&](const Rule& r) { return r.id == id; });

    logger::info("Regra {} marcada para deleção física.", id);
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
        if (IsNPCMatchingTargets(npc, rule, false) && !IsNPCMatchingTargets(npc, rule, true)) {
            // 3. Processa os grupos de recompensa da regra
            for (const auto& group : rule.rewardGroups) {
                if (group.isExclusive) {
                    // Lógica de Sorteio Único (Exclusivo)
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
                    // Lógica de Sorteio Independente
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




Rule* RuleManager::GetRuleVersion(const std::string& ruleID, int version) {
    if (_ruleHistories.contains(ruleID)) {
        for (auto& rule : _ruleHistories[ruleID]) {
            if (rule.version == version) return &rule;
        }
    }
    return nullptr;
}

std::vector<Reward> RuleManager::GetRewardsForSpecificRule(RE::TESNPC* npc, const Rule& rule) {
    std::vector<Reward> applicable;
    if (!npc) return applicable;

    std::string npcName = npc->GetName();
    logger::debug("[Sorteio] --- Iniciando processamento para: {} (Regra: {}) ---", npcName, rule.name);

    for (const auto& group : rule.rewardGroups) {
        if (group.rewards.empty()) {
            logger::debug("  [Grupo: {}] Vazio, pulando.", group.name);
            continue;
        }

        if (group.isExclusive) {
            // Lógica de Sorteio Único (Exclusivo)
            float roll = GetRandomFloat(0.0f, 100.0f);
            float cumulative = 0.0f;
            bool found = false;

            logger::debug("  [Grupo Exclusivo: {}] Roll: {:.2f}/100.00", group.name, roll);

            for (const auto& reward : group.rewards) {
                float startRange = cumulative;
                cumulative += reward.chanceReward;

                // Busca o nome do item para o log
                auto [plugin, fID] = reward.ParseFormID();
                auto form = RE::TESForm::LookupByID(fID);
                std::string itemName = form ? form->GetName() : reward.formIDStr;

                logger::debug("    - Item: {} | Range: {:.2f} a {:.2f}", itemName, startRange, cumulative);

                if (!found && roll <= cumulative) {
                    logger::debug("      >> SELECIONADO: {:.2f} caiu dentro do range!", roll);
                    applicable.push_back(reward);
                    found = true;
                    // Não damos 'break' aqui se você quiser ver o log dos outros ranges, 
                    // mas a lógica garante que apenas o primeiro que satisfaz a condição é pego.
                }
            }
            if (!found) logger::debug("    - Nenhum item selecionado (Roll foi maior que a soma total).");
        }
        else {
            // Lógica de Sorteio Independente
            logger::debug("  [Grupo Independente: {}]", group.name);
            for (const auto& reward : group.rewards) {
                float roll = GetRandomFloat(0.0f, 100.0f);

                auto [plugin, fID] = reward.ParseFormID();
                auto form = RE::TESForm::LookupByID(fID);
                std::string itemName = form ? form->GetName() : reward.formIDStr;

                bool success = (roll <= reward.chanceReward);
                logger::debug("    - Item: {} | Roll: {:.2f} vs Chance: {:.2f} | {}",
                    itemName, roll, reward.chanceReward, success ? "SUCESSO" : "FALHA");

                if (success) {
                    applicable.push_back(reward);
                }
            }
        }
    }
    logger::debug("[Sorteio] --- Fim do processamento para {} ---\n", npcName);
    return applicable;
}

void RuleManager::InitializeAffectedNPCsDatabase() {


    _affectedNPCsDatabase.clear();
    auto& rules = GetRules();
    auto npcList = Manager::GetSingleton()->GetList("NPC");

    for (const auto& npcInfo : npcList) {
        auto npc = RE::TESForm::LookupByID<RE::TESNPC>(npcInfo.formID);
        if (!npc) continue;

        AffectedNPC affectedInfo;
        affectedInfo.npcFormID = npcInfo.formID;
        affectedInfo.npcName = npc->GetName();

        for (const auto& rule : rules) {
            if (IsNPCMatchingTargets(npc, rule, false) && !IsNPCMatchingTargets(npc, rule, true)) {
                affectedInfo.ruleIDs.push_back(rule.id);
            }
        }

        if (!affectedInfo.ruleIDs.empty()) {
            _affectedNPCsDatabase[npcInfo.formID] = affectedInfo;
        }
    }
}


