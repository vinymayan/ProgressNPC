#include "Rule.h"
#include <miniz.h> // Inclua a biblioteca miniz
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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

namespace {
    using JsonAllocator = rapidjson::Document::AllocatorType;

    const rapidjson::Value* FindMember(const rapidjson::Value& obj, const char* key) {
        if (!obj.IsObject()) return nullptr;
        auto it = obj.FindMember(key);
        return it != obj.MemberEnd() ? &it->value : nullptr;
    }

    std::string GetString(const rapidjson::Value& obj, const char* key, const std::string& fallback = {}) {
        auto value = FindMember(obj, key);
        return value && value->IsString() ? value->GetString() : fallback;
    }

    bool GetBool(const rapidjson::Value& obj, const char* key, bool fallback = false) {
        auto value = FindMember(obj, key);
        return value && value->IsBool() ? value->GetBool() : fallback;
    }

    int GetInt(const rapidjson::Value& obj, const char* key, int fallback = 0) {
        auto value = FindMember(obj, key);
        return value && value->IsInt() ? value->GetInt() : fallback;
    }

    uint32_t GetUint(const rapidjson::Value& obj, const char* key, uint32_t fallback = 0) {
        auto value = FindMember(obj, key);
        return value && value->IsUint() ? value->GetUint() : fallback;
    }

    float GetFloat(const rapidjson::Value& obj, const char* key, float fallback = 0.0f) {
        auto value = FindMember(obj, key);
        return value && value->IsNumber() ? value->GetFloat() : fallback;
    }

    void AddString(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, const std::string& value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        rapidjson::Value jsonValue;
        jsonValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
        obj.AddMember(jsonKey, jsonValue, alloc);
    }

    void AddBool(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, bool value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    void AddInt(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, int value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    void AddUint(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, uint32_t value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    void AddFloat(rapidjson::Value& obj, JsonAllocator& alloc, const char* key, float value) {
        rapidjson::Value jsonKey;
        jsonKey.SetString(key, alloc);
        obj.AddMember(jsonKey, value, alloc);
    }

    rapidjson::Value WriteBlacklistFilter(const BlacklistFilter& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddString(obj, alloc, "type", p.type);
        AddString(obj, alloc, "formID", p.formIDStr);
        return obj;
    }

    BlacklistFilter ReadBlacklistFilter(const rapidjson::Value& value) {
        BlacklistFilter p;
        p.type = GetString(value, "type");
        p.formIDStr = GetString(value, "formID");
        return p;
    }

    rapidjson::Value WriteFilterArray(const std::vector<BlacklistFilter>& filters, JsonAllocator& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& filter : filters) {
            array.PushBack(WriteBlacklistFilter(filter, alloc).Move(), alloc);
        }
        return array;
    }

    std::vector<BlacklistFilter> ReadFilterArray(const rapidjson::Value* value) {
        std::vector<BlacklistFilter> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsObject()) result.push_back(ReadBlacklistFilter(item));
        }
        return result;
    }

    rapidjson::Value WriteReward(const Reward& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddString(obj, alloc, "typeReward", p.typeReward);
        AddString(obj, alloc, "FormID", p.formIDStr);
        AddUint(obj, alloc, "Amount", p.amount);
        AddFloat(obj, alloc, "Chance", p.chanceReward);
        AddInt(obj, alloc, "functionOnType", p.functionOnType);
        AddBool(obj, alloc, "isPersistent", p.isPersistent);
        return obj;
    }

    Reward ReadReward(const rapidjson::Value& value) {
        Reward p;
        p.typeReward = GetString(value, "typeReward");
        p.formIDStr = GetString(value, "FormID");
        p.amount = GetUint(value, "Amount", 1);
        p.chanceReward = GetFloat(value, "Chance", 100.0f);
        p.functionOnType = GetInt(value, "functionOnType", GetInt(value, "isSleepOutfit", 0));
        p.isPersistent = GetBool(value, "isPersistent", false);
        return p;
    }

    rapidjson::Value WriteRewardArray(const std::vector<Reward>& rewards, JsonAllocator& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& reward : rewards) {
            array.PushBack(WriteReward(reward, alloc).Move(), alloc);
        }
        return array;
    }

    std::vector<Reward> ReadRewardArray(const rapidjson::Value* value) {
        std::vector<Reward> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsObject()) result.push_back(ReadReward(item));
        }
        return result;
    }

    rapidjson::Value WriteRewardGroup(const RewardGroup& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddString(obj, alloc, "name", p.name);
        AddBool(obj, alloc, "exclusive", p.isExclusive);
        AddFloat(obj, alloc, "chanceGroup", p.chanceGroup);
        auto rewards = WriteRewardArray(p.rewards, alloc);
        obj.AddMember("rewards", rewards, alloc);
        return obj;
    }

    RewardGroup ReadRewardGroup(const rapidjson::Value& value) {
        RewardGroup p;
        p.name = GetString(value, "name", "New Group");
        p.isExclusive = GetBool(value, "exclusive", false);
        p.chanceGroup = GetFloat(value, "chanceGroup", 100.0f);
        p.rewards = ReadRewardArray(FindMember(value, "rewards"));
        return p;
    }

    rapidjson::Value WriteRewardGroupArray(const std::vector<RewardGroup>& groups, JsonAllocator& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& group : groups) {
            array.PushBack(WriteRewardGroup(group, alloc).Move(), alloc);
        }
        return array;
    }

    std::vector<RewardGroup> ReadRewardGroupArray(const rapidjson::Value* value) {
        std::vector<RewardGroup> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsObject()) result.push_back(ReadRewardGroup(item));
        }
        return result;
    }

    rapidjson::Value WriteCompactRule(const Rule& p, bool isLatest, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        if (isLatest) {
            AddString(obj, alloc, "id", p.id);
            AddString(obj, alloc, "n", p.name);
            AddBool(obj, alloc, "en", p.isEnabled);
        }
        AddInt(obj, alloc, "v", p.version);
        AddInt(obj, alloc, "l", p.level);
        AddInt(obj, alloc, "g", p.targetGender);
        AddBool(obj, alloc, "ra", p.targetRequiresAll);
        AddBool(obj, alloc, "ex", p.isExclusive);
        obj.AddMember("tf", WriteFilterArray(p.targetFilters, alloc), alloc);
        obj.AddMember("rg", WriteRewardGroupArray(p.rewardGroups, alloc), alloc);
        AddInt(obj, alloc, "bg", p.blacklistedGender);
        AddBool(obj, alloc, "bra", p.blacklistRequiresAll);
        obj.AddMember("bf", WriteFilterArray(p.blacklistFilters, alloc), alloc);
        return obj;
    }

    rapidjson::Value WriteHashRule(const Rule& p, JsonAllocator& alloc) {
        rapidjson::Value obj(rapidjson::kObjectType);
        AddBool(obj, alloc, "enabled", p.isEnabled);
        AddString(obj, alloc, "name", p.name);
        AddInt(obj, alloc, "level", p.level);
        AddInt(obj, alloc, "t_gender", p.targetGender);
        AddBool(obj, alloc, "t_reqAll", p.targetRequiresAll);
        obj.AddMember("t_filters", WriteFilterArray(p.targetFilters, alloc), alloc);
        obj.AddMember("groups", WriteRewardGroupArray(p.rewardGroups, alloc), alloc);
        AddInt(obj, alloc, "b_gender", p.blacklistedGender);
        AddBool(obj, alloc, "b_reqAll", p.blacklistRequiresAll);
        obj.AddMember("b_filters", WriteFilterArray(p.blacklistFilters, alloc), alloc);
        AddBool(obj, alloc, "isExclusive", p.isExclusive);
        for (const auto& group : p.rewardGroups) {
            const auto key = "g_chance_" + group.name;
            AddFloat(obj, alloc, key.c_str(), group.chanceGroup);
        }
        return obj;
    }

    std::string SerializeJson(const rapidjson::Value& value) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }
}

std::string Rule::CalculateHash() const {
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    auto hashValue = WriteHashRule(*this, alloc);
    return std::to_string(std::hash<std::string>{}(SerializeJson(hashValue)));
}

Rule ProcessRuleVersion(const rapidjson::Value& j, const std::string& fallbackId, const std::string& fallbackName, bool fallbackEnabled) {
    Rule p;
    p.id = GetString(j, "id", fallbackId);
    p.name = GetString(j, "n", GetString(j, "name", fallbackName));
    p.isEnabled = GetBool(j, "en", GetBool(j, "enabled", fallbackEnabled));

    p.version = GetInt(j, "v", GetInt(j, "version", 1));
    p.level = GetInt(j, "l", GetInt(j, "level", 1));
    p.targetGender = GetInt(j, "g", GetInt(j, "targetGender", 0));
    p.targetRequiresAll = GetBool(j, "ra", GetBool(j, "targetRequiresAll", false));
    p.isExclusive = GetBool(j, "ex", GetBool(j, "ruleExclusive", false));

    if (auto value = FindMember(j, "tf")) p.targetFilters = ReadFilterArray(value);
    else p.targetFilters = ReadFilterArray(FindMember(j, "targetFilters"));

    if (auto value = FindMember(j, "rg")) p.rewardGroups = ReadRewardGroupArray(value);
    else p.rewardGroups = ReadRewardGroupArray(FindMember(j, "RewardGroups"));

    p.blacklistedGender = GetInt(j, "bg", GetInt(j, "blacklistedGender", 0));
    p.blacklistRequiresAll = GetBool(j, "bra", GetBool(j, "blacklistRequiresAll", false));

    if (auto value = FindMember(j, "bf")) p.blacklistFilters = ReadFilterArray(value);
    else p.blacklistFilters = ReadFilterArray(FindMember(j, "blacklistFilters"));

    p.lastSavedHash = p.CalculateHash();
    return p;
}
bool RuleManager::IsAffected(RE::Actor* actor) {
    if (!actor) return false;
    auto baseNPC = actor->GetActorBase();
	//logger::debug("Verificando NPC: {} (FormID: {:08X})", baseNPC ? baseNPC->GetName() : "Unknown", baseNPC->GetFormID());
    if (!baseNPC) return false;

    // 1. Identificar os 3 IDs principais
    RE::FormID actorID = actor->GetFormID();
    RE::FormID baseID = baseNPC->GetFormID();

    // Acessa o baseTemplateForm (TPLT) definido em TESActorBaseData
    RE::TESForm* baseTemplate = actor->GetTemplateBase();
    RE::FormID templateID = baseTemplate ? baseTemplate->GetFormID() : 0;
	logger::debug("IDs para verificação - ActorID: {:08X}, BaseID: {:08X}, TemplateID: {:08X}", actorID, baseID, templateID);
    // 2. Busca "em massa": Se qualquer um dos IDs estiver no banco, o ator é afetado
    if (_affectedNPCsDatabase.contains(actorID)) return true;
    if (_affectedNPCsDatabase.contains(baseID)) return true;

    if (templateID != 0 && _affectedNPCsDatabase.contains(templateID)) {
        logger::debug("NPC {} identificado via Template: {:08X}", baseNPC->GetName(), templateID);
        return true;
    }

    return false;
}

bool IsNPCInLeveledList(RE::TESNPC* a_npc, RE::TESLevCharacter* a_levList) {
    if (!a_npc || !a_levList) return false;

    for (auto& entry : a_levList->entries) {
        auto form = entry.form;
        if (!form) continue;

        // Se a entrada for o próprio NPC, encontramos o match
        if (form->Is(RE::FormType::NPC)) {
            if (form->GetFormID() == a_npc->GetFormID()) return true;
        }
        // Se a entrada for outra Leveled List, entra nela recursivamente
        else if (form->Is(RE::FormType::LeveledNPC)) {
            if (IsNPCInLeveledList(a_npc, form->As<RE::TESLevCharacter>())) return true;
        }
    }
    return false;
}

bool IsNPCMatchingTargets(RE::TESNPC* npc, const Rule& rule, bool isBlacklist, RE::Actor* actor) {
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

        if (filter.type == "NPC") {
            if (npc->GetFormID() == fID) {
                match = true;
            }
        }
        else if (filter.type == "Leveled NPC") {
            auto levList = RE::TESForm::LookupByID<RE::TESLevCharacter>(fID);
            if (levList && IsNPCInLeveledList(npc, levList)) {
                match = true;
            }
        }
        else if (filter.type == "Keyword") {
            auto kwd = RE::TESForm::LookupByID<RE::BGSKeyword>(fID);
            if (kwd && (npc->HasKeyword(kwd))) {
                match = true;
            }
        }
        else if (filter.type == "Faction") {
            auto fact = RE::TESForm::LookupByID<RE::TESFaction>(fID);
            if (fact && (npc->IsInFaction(fact))) {
                match = true;
            }
        }
        else if (filter.type == "Race") {
            auto race = RE::TESForm::LookupByID<RE::TESRace>(fID);
            if (race && (npc->race == race)) {
                match = true;
            }
        }
        else if (filter.type == "Combat Style") {
            if (npc->combatStyle && npc->combatStyle->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Voice Type") {
            if (npc->voiceType && npc->voiceType->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Class") {
            if (npc->npcClass && npc->npcClass->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Skin") {
            // A Skin do NPC é um ponteiro para um TESObjectARMO (Armor)
            if (npc->skin && npc->skin->GetFormID() == fID) match = true;
        }
        else if (filter.type == "Package") {
            auto pkg = RE::TESForm::LookupByID<RE::TESPackage>(fID);
            if (pkg) {
                // Verifica na lista de pacotes do NPC base
                for (auto* pak : npc->aiPackages.packages) {
                    if (pak && pak->GetFormID() == fID) {
                        match = true;
                        break;
                    }
                }
            }
        }
        else if (filter.type == "Hair" || filter.type == "Facial Hair") {
            if (npc->headParts && npc->numHeadParts > 0) {
                for (std::int8_t i = 0; i < npc->numHeadParts; i++) {
                    if (npc->headParts[i] && npc->headParts[i]->GetFormID() == fID) {
                        match = true;
                        break;
                    }
                }
            }
        }
        else if (filter.type == "Location") {
            if (actor) {
                // Estamos em tempo de execução com um Actor real.
                auto targetLoc = RE::TESForm::LookupByID<RE::BGSLocation>(fID);
                auto currentLoc = actor->GetCurrentLocation();
                if (targetLoc && currentLoc) {
                    if (currentLoc == targetLoc || currentLoc->IsParent(targetLoc)) {
                        match = true;
                    }
                }
            }
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
                rapidjson::IStreamWrapper stream(i);
                rapidjson::Document j;
                j.ParseStream(stream);

                if (!j.HasParseError() && j.IsArray() && !j.Empty()) {
                    std::vector<Rule> history;

                    // A primeira entrada [0] é a mais recente e contém os metadados completos
                    const rapidjson::Value& latestJson = j[0];
                    Rule latest = ProcessRuleVersion(latestJson, "", "Sem Nome", true);

                    std::string ruleId = latest.id;
                    std::string ruleName = latest.name;
                    bool ruleEnabled = latest.isEnabled;

                    history.push_back(latest);

                    // Processa o restante do histórico usando os metadados da versão mais recente como fallback
                    for (rapidjson::SizeType idx = 1; idx < j.Size(); ++idx) {
                        history.push_back(ProcessRuleVersion(j[idx], ruleId, ruleName, ruleEnabled));
                    }

                    _ruleHistories[ruleId] = history;
                    _rules.push_back(latest);
                    _ruleIdToFileName[ruleId] = entry.path().stem().string();
                }
            }
            catch (const std::exception& e) {
                logger::error("Erro ao carregar regra {}: {}", entry.path().string(), e.what());
            }
        }
    }
    logger::info("Carregadas {} regras com seus históricos (Suporte a Formato Compacto ativado).", _rules.size());
}

void RuleManager::SaveRules() {
    const size_t MAX_HISTORY = 100;
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
            _ruleIdToFileName[currentRule.id] = newFileName;
            // 1. Incrementa a versão numérica
            currentRule.version++;
            currentRule.lastSavedHash = currentContentHash;

            // 2. Atualiza o histórico em memória
            auto& history = _ruleHistories[currentRule.id];
            history.insert(history.begin(), currentRule);

            // PODA: Mantém apenas as últimas X versões
            if (history.size() > MAX_HISTORY) {
                history.resize(MAX_HISTORY);
            }

            // SALVAMENTO OTIMIZADO
            rapidjson::Document historyDoc;
            historyDoc.SetArray();
            auto& alloc = historyDoc.GetAllocator();
            for (size_t i = 0; i < history.size(); ++i) {
                historyDoc.PushBack(WriteCompactRule(history[i], i == 0, alloc).Move(), alloc);
            }

            std::string filePath = _rulesDir + newFileName + ".json";
            std::ofstream o(filePath);
            // Salva sem indentação (dump) para velocidade e espaço
            o << SerializeJson(historyDoc) << std::endl;

            updatedTotal++;
            logger::info("Regra '{}' otimizada e salva. Versão: {}", currentRule.name, currentRule.version);
        }
    }

    if (updatedTotal > 0) {
        InitializeAffectedNPCsDatabase();
    }
}


void RuleManager::ExportRule(const Rule& rule) {
    namespace fs = std::filesystem;

    if (!_ruleIdToFileName.contains(rule.id)) {
        logger::error("Export: ID da regra não encontrado no mapeamento de arquivos.");
        return;
    }
    // 1. Caminhos de origem e destino
    std::string ruleFileName = _ruleIdToFileName[rule.id] + ".json";
    std::string sourcePath = _rulesDir + ruleFileName;

    fs::path exportDir = "Data/SKSE/Plugins/EDF/Exports";
    fs::create_directories(exportDir);

    std::string zipPath = (exportDir / (SanitizeFilename(rule.name) + ".zip")).string();

    // 2. Inicializa o arquivo ZIP
    mz_zip_archive zip_archive;
    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_writer_init_file(&zip_archive, zipPath.c_str(), 0)) {
        logger::error("Export: Falha ao inicializar arquivo ZIP em {}", zipPath);
        return;
    }

    // 3. Define o caminho interno (onde o arquivo ficará dentro do ZIP)
    // O usuário quer: SKSE\Plugins\EDF\Rules\nome.json
    std::string internalZipPath = "SKSE/Plugins/EDF/Rules/" + ruleFileName;

    // 4. Adiciona o arquivo ao ZIP
    // mz_zip_writer_add_file(arquivo_zip, nome_dentro_do_zip, caminho_no_disco, ...)
    if (!mz_zip_writer_add_file(&zip_archive, internalZipPath.c_str(), sourcePath.c_str(), nullptr, 0, MZ_BEST_COMPRESSION)) {
        logger::error("Export: Falha ao adicionar arquivo {} ao ZIP", ruleFileName);
        mz_zip_writer_finalize_archive(&zip_archive);
        mz_zip_writer_end(&zip_archive);
        return;
    }

    // 5. Finaliza e fecha
    mz_zip_writer_finalize_archive(&zip_archive);
    mz_zip_writer_end(&zip_archive);

    logger::info("Regra '{}' exportada com sucesso para: {}", rule.name, zipPath);
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

std::vector<RewardGroup> RuleManager::RollForGroups(RE::TESNPC* npc, const Rule& rule) {
    std::vector<RewardGroup> wonGroups;
    if (!npc) return wonGroups;

    if (rule.isExclusive) {
        // Lógica: Escolhe apenas UM grupo da regra baseado nas chances (chanceGroup)
        float roll = GetRandomFloat(0.0f, 100.0f);
        float cumulative = 0.0f;
        for (const auto& group : rule.rewardGroups) {
            cumulative += group.chanceGroup;
            if (roll <= cumulative) {
                wonGroups.push_back(group);
                break;
            }
        }
    }
    else {
        // Lógica: Testa cada grupo independentemente
        for (const auto& group : rule.rewardGroups) {
            if (GetRandomFloat(0.0f, 100.0f) <= group.chanceGroup) {
                wonGroups.push_back(group);
            }
        }
    }
    return wonGroups;
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

    auto processGroup = [&](const RewardGroup& group) {
        if (group.isExclusive) {
            float roll = GetRandomFloat(0.0f, 100.0f);
            float cumulative = 0.0f;
            for (const auto& reward : group.rewards) {
                cumulative += reward.chanceReward;
                if (roll <= cumulative) { applicable.push_back(reward); break; }
            }
        }
        else {
            for (const auto& reward : group.rewards) {
                if (GetRandomFloat(0.0f, 100.0f) <= reward.chanceReward) applicable.push_back(reward);
            }
        }
        };

    if (rule.isExclusive) {
        // LÓGICA: Escolhe apenas UM grupo da regra baseado nas chances
        float roll = GetRandomFloat(0.0f, 100.0f);
        float cumulative = 0.0f;
        for (const auto& group : rule.rewardGroups) {
            cumulative += group.chanceGroup;
            if (roll <= cumulative) {
                logger::debug("  >> Grupo Selecionado Exclusivamente: {}", group.name);
                processGroup(group);
                break;
            }
        }
    }
    else {
        // LÓGICA: Testa cada grupo independentemente
        for (const auto& group : rule.rewardGroups) {
            if (GetRandomFloat(0.0f, 100.0f) <= group.chanceGroup) {
                processGroup(group);
            }
        }
    }
    return applicable;
}


void RuleManager::InitializeAffectedNPCsDatabase() {
    _affectedNPCsDatabase.clear();
    auto& rules = GetRules();
    auto npcList = Manager::GetSingleton()->GetList("NPC");

    logger::info("--- Iniciando Inicialização do Database de NPCs Afetados ---");

    for (const auto& npcInfo : npcList) {
        auto npc = RE::TESForm::LookupByID<RE::TESNPC>(npcInfo.formID);
        if (!npc) continue;

        AffectedNPC affectedInfo;
        for (const auto& rule : rules) {
            if (!rule.isEnabled) continue;
            // Verifica se o NPC passa nos filtros de Target e não está na Blacklist
            if (IsNPCMatchingTargets(npc, rule, false) && !IsNPCMatchingTargets(npc, rule, true)) {
                affectedInfo.ruleIDs.push_back(rule.id);
            }
        }

        if (!affectedInfo.ruleIDs.empty()) {
            _affectedNPCsDatabase[npcInfo.formID] = affectedInfo;
        }
    }

    logger::info("--- Inicialização Concluída. Total de NPCs no Database: {} ---", _affectedNPCsDatabase.size());
}
