#include "SaveState.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

// Conjunto estático para rastrear atores em processamento
static std::mutex g_processingMutex;
static std::set<RE::FormID> g_actorsInProcess;

namespace {
    std::mutex g_scheduledEvaluationsMutex;
    std::map<RE::FormID, std::uint64_t> g_scheduledEvaluations;
    std::atomic_uint64_t g_nextEvaluationToken{ 1 };

    struct PendingSleepUpdate {
        std::uint64_t token = 0;
        bool isEntering = false;
    };

    std::mutex g_sleepUpdatesMutex;
    std::map<RE::FormID, PendingSleepUpdate> g_sleepUpdates;
    std::atomic_uint64_t g_nextSleepToken{ 1 };
    std::map<RE::FormID, std::vector<RE::FormID>> g_sleepEquippedItems;

    constexpr char kRewardStateDelimiter = '\x1E';

    std::string BuildRewardStateKey(const std::string& a_groupName, const Reward& a_reward)
    {
        std::string key;
        const auto append = [&](std::string_view a_value) {
            if (!key.empty()) {
                key.push_back(kRewardStateDelimiter);
            }
            key.append(a_value);
        };

        append(a_groupName);
        append(a_reward.typeReward);
        append(a_reward.formIDStr);
        append(a_reward.editorID);
        append(std::to_string(a_reward.amount));
        append(std::to_string(a_reward.functionOnType));
        append(a_reward.isPersistent ? "1" : "0");
        return key;
    }

    bool ResolveRewardState(
        const Rule& a_rule,
        const std::string& a_rewardKey,
        const RewardGroup*& a_group,
        const Reward*& a_reward)
    {
        for (const auto& group : a_rule.rewardGroups) {
            for (const auto& reward : group.rewards) {
                if (BuildRewardStateKey(group.name, reward) == a_rewardKey) {
                    a_group = std::addressof(group);
                    a_reward = std::addressof(reward);
                    return true;
                }
            }
        }

        a_group = nullptr;
        a_reward = nullptr;
        return false;
    }

    const RE::TESFile* GetSourceFileByFormID(RE::TESForm* a_form)
    {
        if (!a_form) return nullptr;
        if (auto file = a_form->GetFile(0)) return file;

        auto dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return nullptr;

        const auto formID = a_form->GetFormID();
        const auto modIndex = static_cast<std::uint8_t>(formID >> 24);
        if (modIndex == 0xFE) {
            const auto lightIndex = static_cast<std::uint16_t>((formID >> 12) & 0xFFF);
            return dataHandler->LookupLoadedLightModByIndex(lightIndex);
        }
        if (modIndex != 0xFF) {
            return dataHandler->LookupLoadedModByIndex(modIndex);
        }
        return nullptr;
    }

    const rapidjson::Value* FindMember(const rapidjson::Value& obj, const char* key) {
        if (!obj.IsObject()) return nullptr;
        auto it = obj.FindMember(key);
        return it != obj.MemberEnd() ? &it->value : nullptr;
    }

    uint32_t GetJsonUint(const rapidjson::Value& value, uint32_t fallback = 0) {
        if (value.IsUint()) return value.GetUint();
        if (value.IsInt() && value.GetInt() >= 0) return static_cast<uint32_t>(value.GetInt());
        return fallback;
    }

    std::vector<std::string> ReadStringArray(const rapidjson::Value* value) {
        std::vector<std::string> result;
        if (!value || !value->IsArray()) return result;
        result.reserve(value->Size());
        for (const auto& item : value->GetArray()) {
            if (item.IsString()) result.emplace_back(item.GetString());
        }
        return result;
    }

    rapidjson::Value WriteStringArray(const std::vector<std::string>& values, rapidjson::Document::AllocatorType& alloc) {
        rapidjson::Value array(rapidjson::kArrayType);
        for (const auto& value : values) {
            rapidjson::Value jsonValue;
            jsonValue.SetString(value.c_str(), static_cast<rapidjson::SizeType>(value.size()), alloc);
            array.PushBack(jsonValue, alloc);
        }
        return array;
    }

    std::string SerializeJson(const rapidjson::Value& value) {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        value.Accept(writer);
        return buffer.GetString();
    }

    constexpr char kManagedItemKeyDelimiter = '\x1F';

    std::string GetManagedItemBaseKey(const std::string& a_managedKey)
    {
        const auto delimiter = a_managedKey.find(kManagedItemKeyDelimiter);
        return delimiter == std::string::npos ? a_managedKey : a_managedKey.substr(0, delimiter);
    }

    RE::TESBoundObject* LookupBoundObjectByKey(const std::string& a_key) {
        auto tokens = split(GetManagedItemBaseKey(a_key), '|');
        if (tokens.size() != 2) return nullptr;

        try {
            auto localID = static_cast<RE::FormID>(std::stoul(tokens[1], nullptr, 16));
            if (tokens[0] == "Dynamic" || tokens[0] == "Created") {
                return RE::TESForm::LookupByID<RE::TESBoundObject>(localID);
            }

            auto dataHandler = RE::TESDataHandler::GetSingleton();
            if (!dataHandler) return nullptr;

            auto formID = dataHandler->LookupFormID(localID, tokens[0]);
            return RE::TESForm::LookupByID<RE::TESBoundObject>(formID);
        }
        catch (...) {
            return nullptr;
        }
    }

    uint32_t GetInventoryCount(RE::Actor* a_actor, RE::TESBoundObject* a_item) {
        if (!a_actor || !a_item) return 0;
        const auto count = a_actor->GetInventoryCount(a_item);
        return count > 0 ? static_cast<uint32_t>(count) : 0;
    }

    std::string BuildManagedItemKey(const std::string& a_itemKey, const std::string& a_ruleID,
        const std::string& a_groupName, bool a_isPersistent)
    {
        return a_itemKey + kManagedItemKeyDelimiter + (a_isPersistent ? "P" : "T") +
            kManagedItemKeyDelimiter + a_ruleID + kManagedItemKeyDelimiter + a_groupName;
    }

    bool ManagedItemMatches(const std::string& a_managedKey, const std::string& a_itemKey,
        const PersistentItemState& a_state, const std::string& a_ruleID, const std::string& a_groupName,
        bool a_isPersistent)
    {
        return GetManagedItemBaseKey(a_managedKey) == a_itemKey &&
            a_state.ruleID == a_ruleID &&
            a_state.groupName == a_groupName &&
            a_state.isPersistent == a_isPersistent;
    }

    PersistentItemState* FindManagedItemState(std::map<std::string, PersistentItemState>& a_items,
        const std::string& a_itemKey, const std::string& a_ruleID, const std::string& a_groupName,
        bool a_isPersistent)
    {
        const auto managedKey = BuildManagedItemKey(a_itemKey, a_ruleID, a_groupName, a_isPersistent);
        if (auto it = a_items.find(managedKey); it != a_items.end()) {
            return std::addressof(it->second);
        }

        for (auto& [key, state] : a_items) {
            if (ManagedItemMatches(key, a_itemKey, state, a_ruleID, a_groupName, a_isPersistent)) {
                return std::addressof(state);
            }
        }

        return nullptr;
    }

    const PersistentItemState* FindManagedItemState(const std::map<std::string, PersistentItemState>& a_items,
        const std::string& a_itemKey, const std::string& a_ruleID, const std::string& a_groupName,
        bool a_isPersistent)
    {
        const auto managedKey = BuildManagedItemKey(a_itemKey, a_ruleID, a_groupName, a_isPersistent);
        if (auto it = a_items.find(managedKey); it != a_items.end()) {
            return std::addressof(it->second);
        }

        for (const auto& [key, state] : a_items) {
            if (ManagedItemMatches(key, a_itemKey, state, a_ruleID, a_groupName, a_isPersistent)) {
                return std::addressof(state);
            }
        }

        return nullptr;
    }
}

void ScheduleRuleEvaluation(RE::Actor* a_actor)
{
    if (!a_actor) {
        return;
    }

    auto taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        logger::error("[RuleScheduler] TaskInterface indisponível; avaliação de {:08X} ignorada.",
            a_actor->GetFormID());
        return;
    }

    const auto actorID = a_actor->GetFormID();
    const auto actorHandle = a_actor->GetHandle();
    if (!actorHandle) {
        logger::debug("[RuleScheduler] Ator {:08X} não possui handle válido.", actorID);
        return;
    }

    const auto token = g_nextEvaluationToken.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        if (g_scheduledEvaluations.contains(actorID)) {
            return;
        }
        g_scheduledEvaluations.emplace(actorID, token);
    }

    taskInterface->AddTask([actorHandle, actorID, token]() {
        {
            std::lock_guard lock(g_scheduledEvaluationsMutex);
            const auto it = g_scheduledEvaluations.find(actorID);
            if (it == g_scheduledEvaluations.end() || it->second != token) {
                return;
            }
            g_scheduledEvaluations.erase(it);
        }

        auto actorPtr = actorHandle.get();
        auto actor = actorPtr ? actorPtr.get() : nullptr;
        if (!actor || actor->IsDead()) {
            return;
        }

        const auto& context = SaveStateManager::GetSingleton()->GetCurrentContext();
        if (!context.isValid || !RuleManager::GetSingleton()->IsAffected(actor)) {
            return;
        }

        logger::debug("[RuleScheduler] Reavaliando regras para '{}' ({:08X}) no thread principal.",
            actor->GetName(), actorID);
        ApplyRulesToInstance(actor);
    });
}

void ScheduleSleepOutfitUpdate(RE::Actor* a_actor, bool a_isEntering)
{
    if (!a_actor) {
        return;
    }

    auto taskInterface = SKSE::GetTaskInterface();
    if (!taskInterface) {
        logger::error("[SleepScheduler] TaskInterface indisponível para {:08X}.", a_actor->GetFormID());
        return;
    }

    const auto actorID = a_actor->GetFormID();
    const auto actorHandle = a_actor->GetHandle();
    if (!actorHandle) {
        return;
    }

    const auto token = g_nextSleepToken.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_sleepUpdatesMutex);
        if (auto it = g_sleepUpdates.find(actorID); it != g_sleepUpdates.end()) {
            it->second.isEntering = a_isEntering;
            return;
        }
        g_sleepUpdates.emplace(actorID, PendingSleepUpdate{ token, a_isEntering });
    }

    taskInterface->AddTask([actorHandle, actorID, token]() {
        bool isEntering = false;
        {
            std::lock_guard lock(g_sleepUpdatesMutex);
            const auto it = g_sleepUpdates.find(actorID);
            if (it == g_sleepUpdates.end() || it->second.token != token) {
                return;
            }
            isEntering = it->second.isEntering;
            g_sleepUpdates.erase(it);
        }

        auto actorPtr = actorHandle.get();
        auto actor = actorPtr ? actorPtr.get() : nullptr;
        if (!actor || actor->IsDead()) {
            return;
        }

        ManageSleepOutfitState(actor, isEntering);
    });
}

void ForgetRuleEvaluationRuntimeState(RE::FormID a_actorID)
{
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        g_scheduledEvaluations.erase(a_actorID);
    }
    {
        std::lock_guard lock(g_sleepUpdatesMutex);
        g_sleepUpdates.erase(a_actorID);
        g_sleepEquippedItems.erase(a_actorID);
    }
}

void ResetRuleEvaluationRuntimeState()
{
    {
        std::lock_guard lock(g_scheduledEvaluationsMutex);
        g_scheduledEvaluations.clear();
    }
    {
        std::lock_guard lock(g_sleepUpdatesMutex);
        g_sleepUpdates.clear();
        g_sleepEquippedItems.clear();
    }
}

SaveStateManager* SaveStateManager::GetSingleton() {
    static SaveStateManager instance;
    return &instance;
}

int GetPoolIndex(std::vector<std::string>& pool, const std::string& value) {
    auto it = std::find(pool.begin(), pool.end(), value);
    if (it != pool.end()) return static_cast<int>(std::distance(pool.begin(), it));
    pool.push_back(value);
    return static_cast<int>(pool.size() - 1);
}

std::vector<SaveHistoryEntry>& SaveStateManager::GetCharacterHistory(uint32_t characterID) {
    // Retorna a referência do histórico ou cria uma entrada vazia caso não exista
    return _characterHistory[characterID];
}

void SaveStateManager::SetCurrentContext(uint32_t a_charID, uint32_t a_saveNum) {
    logger::info("[SaveManager] Definindo contexto: CharID {:08X}, Save #{}", a_charID, a_saveNum);
    ResetRuleEvaluationRuntimeState();

    _currentContext.charID = a_charID;
    _currentContext.saveNumber = a_saveNum;
    _currentContext.isValid = true; // Só é válido se tivermos um ID de personagem

    _sessionData = SaveHistoryEntry{};
    _sessionData.saveNumber = a_saveNum;

    if (a_charID != 0) {
        if (!_characterHistory.contains(a_charID)) {
            LoadCharacterData(a_charID);
        }

        auto& history = _characterHistory[a_charID];
        SaveHistoryEntry* bestSnapshot = nullptr;

        for (auto& entry : history) {
            if (entry.saveNumber == a_saveNum) {
                _sessionData.npcRuleVersions = entry.npcRuleVersions;
                _sessionData.persistentItems = entry.persistentItems;
                _sessionData.virtualKeywords = entry.virtualKeywords;
                _sessionData.addedFactions = entry.addedFactions;
                logger::info("[SaveManager] Contexto restaurado com sucesso do save #{}", a_saveNum);
                return;
            }
            if (entry.saveNumber < a_saveNum && (!bestSnapshot || entry.saveNumber > bestSnapshot->saveNumber)) {
                bestSnapshot = &entry;
            }
        }

        if (bestSnapshot) {
            _sessionData.npcRuleVersions = bestSnapshot->npcRuleVersions;
            _sessionData.persistentItems = bestSnapshot->persistentItems;
            _sessionData.virtualKeywords = bestSnapshot->virtualKeywords;
            _sessionData.addedFactions = bestSnapshot->addedFactions;
            logger::info("[SaveManager] Contexto herdado do save anterior #{}", bestSnapshot->saveNumber);
        }
    }
}

std::string SaveStateManager::BuildFormKey(RE::TESForm* a_form) {
    if (!a_form) return "";

    std::string fileNameStr = "Dynamic";
    if (auto file = GetSourceFileByFormID(a_form)) {
        fileNameStr = file->GetFilename();
    }
    else if (a_form->IsDynamicForm()) {
        fileNameStr = "Dynamic";
    }

    return fileNameStr + "|" + FormatLocalFormID(a_form->GetFormID(), fileNameStr);
}

void SaveStateManager::PersistCurrentSave(const std::string& a_saveName) {

    if (_currentContext.isValid && _currentContext.charID == 0) {
        try {
            std::filesystem::path p(a_saveName);
            std::string fileName = p.filename().string();
            size_t first_u = fileName.find('_');
            size_t second_u = fileName.find('_', first_u + 1);
            if (first_u != std::string::npos && second_u != std::string::npos) {
                std::string idStr = fileName.substr(first_u + 1, second_u - first_u - 1);
                _currentContext.charID = static_cast<uint32_t>(std::stoul(idStr, nullptr, 16));
                logger::info("[SaveManager] Migrando contexto de New Game para Personagem: {:X}", _currentContext.charID);
            }
        }
        catch (...) {}
    }
    auto& context = _currentContext;
    // Log de debug para rastrear por que pode estar falhando
    if (!context.isValid) {
        logger::warn("[SaveManager] PersistCurrentSave abortado: Contexto inválido (CharID: {:08X})", context.charID);
        return;
    }

    uint32_t newSaveNumber = context.saveNumber;
    try {
        // Extrai o nome do arquivo (ex: quiksave.ess ou Save1_...)
        std::filesystem::path p(a_saveName);
        std::string fileName = p.filename().string();

        // Tenta extrair o número do save se for o formato padrão do Skyrim
        size_t savePos = fileName.find("Save");
        size_t underscorePos = fileName.find('_');
        if (savePos != std::string::npos && underscorePos != std::string::npos) {
            std::string numStr = fileName.substr(savePos + 4, underscorePos - (savePos + 4));
            newSaveNumber = std::stoul(numStr);
        }
    }
    catch (...) {
        logger::error("[SaveManager] Erro ao processar número do save do nome: {}", a_saveName);
    }

    _sessionData.saveNumber = newSaveNumber;
    logger::info("[SaveManager] Sincronizando dados para o Save #{}...", newSaveNumber);

    RefreshPersistentItemsForLoadedActors();
    UpdateSaveEntry(context.charID, _sessionData);
    context.saveNumber = newSaveNumber; // Atualiza o contexto para o novo número
}

void SaveStateManager::ClearContext()
{
	logger::info("[SaveManager] Limpando contexto atual para CharID {:08X}", _currentContext.charID);
    ResetRuleEvaluationRuntimeState();
    _currentContext.isValid = false;
    _currentContext.saveNumber = 0;
    _currentContext.charID = 0;
    _sessionData = SaveHistoryEntry{};
}

std::string SaveStateManager::BuildNPCKey(RE::Actor* a_actor) {
    if (!a_actor) return "";
    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return "";

    std::string fileNameStr = "Dynamic";
    if (auto file = GetSourceFileByFormID(baseNPC)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Dynamic";
    }

    return fileNameStr + "|" + FormatLocalFormID(a_actor->GetFormID(), fileNameStr);
}

void SaveStateManager::TrackPersistentItemGrant(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_count,
    const std::string& a_ruleID, const std::string& a_groupName, bool a_isPersistent)
{
    if (!a_actor || !a_item || a_count == 0) return;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto itemKey = BuildFormKey(a_item);
    if (npcKey.empty() || itemKey.empty()) return;

    auto managedKey = BuildManagedItemKey(itemKey, a_ruleID, a_groupName, a_isPersistent);
    auto& itemState = _sessionData.persistentItems[npcKey][managedKey];
    itemState.expectedCount += a_count;
    itemState.ruleID = itemState.ruleID.empty() ? a_ruleID : itemState.ruleID;
    itemState.groupName = itemState.groupName.empty() ? a_groupName : itemState.groupName;
    itemState.isPersistent = a_isPersistent;

    const auto currentCount = GetInventoryCount(a_actor, a_item);
    itemState.missingCount = itemState.expectedCount > currentCount ? itemState.expectedCount - currentCount : 0;

    logger::debug("[PersistentLedger] Grant registrado: NPC '{}' Item '{}' Esperado {} Ausente {}",
        a_actor->GetName(), a_item->GetName(), itemState.expectedCount, itemState.missingCount);
}

void SaveStateManager::EnsurePersistentItemTracked(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_expectedCount,
    const std::string& a_ruleID, const std::string& a_groupName, bool a_isPersistent)
{
    if (!a_actor || !a_item || a_expectedCount == 0) return;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto itemKey = BuildFormKey(a_item);
    if (npcKey.empty() || itemKey.empty()) return;

    auto& npcItems = _sessionData.persistentItems[npcKey];
    if (FindManagedItemState(npcItems, itemKey, a_ruleID, a_groupName, a_isPersistent)) return;

    const auto currentCount = GetInventoryCount(a_actor, a_item);
    if (currentCount == 0 && a_isPersistent) return;

    auto managedKey = BuildManagedItemKey(itemKey, a_ruleID, a_groupName, a_isPersistent);
    auto& itemState = npcItems[managedKey];
    itemState.expectedCount = a_expectedCount;
    itemState.missingCount = a_isPersistent && itemState.expectedCount > currentCount ? itemState.expectedCount - currentCount : 0;
    itemState.ruleID = a_ruleID;
    itemState.groupName = a_groupName;
    itemState.isPersistent = a_isPersistent;

    logger::debug("[PersistentLedger] Item existente adotado: NPC '{}' Item '{}' Esperado {} Ausente {}",
        a_actor->GetName(), a_item->GetName(), itemState.expectedCount, itemState.missingCount);
}

void SaveStateManager::AuditPersistentItems(RE::Actor* a_actor)
{
    if (!a_actor) return;

    const auto npcKey = BuildNPCKey(a_actor);
    if (npcKey.empty()) return;

    auto npcIt = _sessionData.persistentItems.find(npcKey);
    if (npcIt == _sessionData.persistentItems.end()) return;

    for (auto& [itemKey, itemState] : npcIt->second) {
        auto item = LookupBoundObjectByKey(itemKey);
        if (!item || itemState.expectedCount == 0) continue;

        const auto currentCount = GetInventoryCount(a_actor, item);
        const auto protectedCount = itemState.expectedCount > itemState.missingCount ? itemState.expectedCount - itemState.missingCount : 0;

        if (currentCount < protectedCount) {
            logger::info("[PersistentLedger] NPC '{}' Item '{}' Esperado {} Atual {} Retirado {} Restauravel {}",
                a_actor->GetName(), item->GetName(), itemState.expectedCount, currentCount, itemState.missingCount, protectedCount - currentCount);
        }
    }
}

void SaveStateManager::RefreshPersistentItemsForLoadedActors()
{
    auto processLists = RE::ProcessLists::GetSingleton();
    if (!processLists) return;

    auto auditList = [&](auto& handles) {
        for (auto& handle : handles) {
            auto actorPtr = handle.get();
            if (actorPtr) {
                AuditPersistentItems(actorPtr.get());
            }
        }
        };

    if (auto player = RE::PlayerCharacter::GetSingleton()) {
        AuditPersistentItems(player);
    }

    auditList(processLists->highActorHandles);
    auditList(processLists->middleHighActorHandles);
    auditList(processLists->lowActorHandles);
}

void SaveStateManager::HandleContainerChanged(const RE::TESContainerChangedEvent* a_event)
{
    if (!a_event || !_currentContext.isValid || a_event->itemCount <= 0) return;

    auto item = RE::TESForm::LookupByID<RE::TESBoundObject>(a_event->baseObj);
    if (!item) return;

    auto applyTransfer = [&](RE::FormID a_containerID, bool a_enteringOwner) {
        if (a_containerID == 0) return;

        auto actor = RE::TESForm::LookupByID<RE::Actor>(a_containerID);
        if (!actor) return;

        const auto npcKey = BuildNPCKey(actor);
        const auto itemKey = BuildFormKey(item);
        auto npcIt = _sessionData.persistentItems.find(npcKey);
        if (npcIt == _sessionData.persistentItems.end()) return;

        auto remainingMovedCount = static_cast<uint32_t>(a_event->itemCount);
        for (auto& [managedKey, itemState] : npcIt->second) {
            if (remainingMovedCount == 0) break;
            if (GetManagedItemBaseKey(managedKey) != itemKey) continue;

            const auto oldMissing = itemState.missingCount;
            const auto movedCount = remainingMovedCount;

            if (a_enteringOwner) {
                const auto restoredCount = std::min(movedCount, itemState.missingCount);
                itemState.missingCount -= restoredCount;
                remainingMovedCount -= restoredCount;
            }
            else {
                const auto availableDebt = itemState.expectedCount > itemState.missingCount ? itemState.expectedCount - itemState.missingCount : 0;
                const auto claimedCount = std::min(movedCount, availableDebt);
                itemState.missingCount += claimedCount;
                remainingMovedCount -= claimedCount;
            }

            if (oldMissing != itemState.missingCount) {
                logger::info("[PersistentLedger] Transferencia: Dono '{}' Item '{}' Esperado {} Ausente {} -> {}",
                    actor->GetName(), item->GetName(), itemState.expectedCount, oldMissing, itemState.missingCount);
            }
        }
        };

    applyTransfer(a_event->oldContainer, false);
    applyTransfer(a_event->newContainer, true);
}

bool SaveStateManager::AddVirtualKeyword(RE::Actor* a_actor, RE::BGSKeyword* a_keyword)
{
    if (!a_actor || !a_keyword) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto keywordKey = BuildFormKey(a_keyword);
    if (npcKey.empty() || keywordKey.empty()) return false;

    auto& keywords = _sessionData.virtualKeywords[npcKey];
    const auto [it, inserted] = keywords.insert(keywordKey);
    if (inserted) {
        logger::info("[TagDelta] Virtual keyword '{}' adicionada para '{}'", a_keyword->GetFormEditorID(), a_actor->GetName());
    }
    return inserted;
}

bool SaveStateManager::HasVirtualKeyword(RE::Actor* a_actor, RE::BGSKeyword* a_keyword) const
{
    if (!a_actor || !a_keyword) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto keywordKey = BuildFormKey(a_keyword);
    if (npcKey.empty() || keywordKey.empty()) return false;

    auto npcIt = _sessionData.virtualKeywords.find(npcKey);
    return npcIt != _sessionData.virtualKeywords.end() && npcIt->second.contains(keywordKey);
}

bool SaveStateManager::RemoveVirtualKeyword(RE::Actor* a_actor, RE::BGSKeyword* a_keyword)
{
    if (!a_actor || !a_keyword) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto keywordKey = BuildFormKey(a_keyword);
    auto npcIt = _sessionData.virtualKeywords.find(npcKey);
    if (npcIt == _sessionData.virtualKeywords.end()) return false;

    return npcIt->second.erase(keywordKey) > 0;
}

bool SaveStateManager::AddManagedFaction(RE::Actor* a_actor, RE::TESFaction* a_faction, int a_rank)
{
    if (!a_actor || !a_faction) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto factionKey = BuildFormKey(a_faction);
    if (npcKey.empty() || factionKey.empty()) return false;

    const auto rank = std::clamp(a_rank, -128, 127);
    auto& factions = _sessionData.addedFactions[npcKey];
    auto it = factions.find(factionKey);
    const bool changed = it == factions.end() || it->second != rank || !a_actor->IsInFaction(a_faction);

    if (changed) {
        a_actor->AddToFaction(a_faction, static_cast<std::int8_t>(rank));
        factions[factionKey] = rank;
        logger::info("[TagDelta] Faction '{}' rank {} adicionada para '{}'", a_faction->GetFormEditorID(), rank, a_actor->GetName());
    }

    return changed;
}

bool SaveStateManager::RemoveManagedFaction(RE::Actor* a_actor, RE::TESFaction* a_faction)
{
    if (!a_actor || !a_faction) return false;

    const auto npcKey = BuildNPCKey(a_actor);
    const auto factionKey = BuildFormKey(a_faction);
    auto npcIt = _sessionData.addedFactions.find(npcKey);
    if (npcIt == _sessionData.addedFactions.end() || !npcIt->second.contains(factionKey)) return false;

    a_actor->RemoveFromFaction(a_faction);
    npcIt->second.erase(factionKey);
    return true;
}

std::string SaveStateManager::GetCharacterPath(uint32_t characterID) {
    char hexID[9];
    sprintf_s(hexID, "%08X", characterID);
    const std::string fileName = std::string(hexID) + ".json";
    const std::filesystem::path newDir = "Data/Viny Mods/EDF/Saves";
    const std::filesystem::path legacyDir = "Data/SKSE/Plugins/EDF/Saves";
    std::filesystem::create_directories(newDir);

    const auto newPath = newDir / fileName;
    const auto legacyPath = legacyDir / fileName;
    if (!std::filesystem::exists(newPath) && std::filesystem::exists(legacyPath)) {
        std::error_code ec;
        std::filesystem::copy_file(legacyPath, newPath, std::filesystem::copy_options::skip_existing, ec);
        if (ec) {
            logger::warn("[SaveManager] Falha ao migrar save '{}' para '{}': {}", legacyPath.string(), newPath.string(), ec.message());
            return legacyPath.string();
        }
        logger::info("[SaveManager] Save migrado de '{}' para '{}'", legacyPath.string(), newPath.string());
    }

    return newPath.string();
}

void SaveStateManager::LoadCharacterData(uint32_t characterID) {
    std::string path = GetCharacterPath(characterID);
    logger::info("[SaveManager] Tentando carregar arquivo: {}", path);

    std::ifstream i(path);
    if (!i.is_open()) return;

    try {
        rapidjson::IStreamWrapper stream(i);
        rapidjson::Document doc;
        doc.ParseStream(stream);
        if (doc.HasParseError()) {
            logger::error("[SaveManager] JSON inválido em {}.", path);
            return;
        }

        // --- DETECÇÃO E CONVERSÃO DE FORMATO ANTIGO ---
        if (doc.IsArray()) {
            logger::info("[SaveManager] Formato antigo detectado para {:08X}. Iniciando migração...", characterID);

            std::vector<SaveHistoryEntry> legacyHistory;
            for (const auto& oldEntryJson : doc.GetArray()) {
                if (!oldEntryJson.IsObject()) continue;

                SaveHistoryEntry entry;
                if (auto saveNumber = FindMember(oldEntryJson, "saveNumber")) {
                    entry.saveNumber = GetJsonUint(*saveNumber);
                }

                // No formato antigo, npcRuleVersions mapeava ruleID para um int (versão)
                auto oldNpcMap = FindMember(oldEntryJson, "npcRuleVersions");
                if (oldNpcMap && oldNpcMap->IsObject()) {
                    for (auto npcIt = oldNpcMap->MemberBegin(); npcIt != oldNpcMap->MemberEnd(); ++npcIt) {
                        if (!npcIt->name.IsString() || !npcIt->value.IsObject()) continue;

                        const std::string npcKey = npcIt->name.GetString();
                        for (auto ruleIt = npcIt->value.MemberBegin(); ruleIt != npcIt->value.MemberEnd(); ++ruleIt) {
                            if (!ruleIt->name.IsString()) continue;

                            AppliedRuleState state;
                            state.version = static_cast<int>(GetJsonUint(ruleIt->value));
                            state.appliedGroups = {}; // Histórico de grupos não existia no formato antigo
                            entry.npcRuleVersions[npcKey][ruleIt->name.GetString()] = state;
                        }
                    }
                }
                legacyHistory.push_back(entry);
            }

            _characterHistory[characterID] = legacyHistory;

            if (!legacyHistory.empty()) {
                UpdateSaveEntry(characterID, legacyHistory.back());
                logger::info("[SaveManager] Migração concluída com sucesso para o disco.");
            }
            return;
        }

        if (!doc.IsObject()) return;

        // --- CARREGAMENTO DO FORMATO NOVO (EXISTENTE) ---
        auto plugins = ReadStringArray(FindMember(doc, "p_plugins"));
        auto rules = ReadStringArray(FindMember(doc, "p_rules"));
        auto groups = ReadStringArray(FindMember(doc, "p_groups"));
        auto items = ReadStringArray(FindMember(doc, "p_items"));
        auto tags = ReadStringArray(FindMember(doc, "p_tags"));
        auto rewardKeys = ReadStringArray(FindMember(doc, "p_rewards"));
        auto historyArray = FindMember(doc, "history");
        if (!historyArray || !historyArray->IsArray()) return;

        std::vector<SaveHistoryEntry> decodedHistory;
        for (const auto& h : historyArray->GetArray()) {
            if (!h.IsObject()) continue;

            SaveHistoryEntry entry;
            if (auto saveNumber = FindMember(h, "sn")) {
                entry.saveNumber = GetJsonUint(*saveNumber);
            }

            auto npcsJson = FindMember(h, "npcs");
            if (!npcsJson || !npcsJson->IsArray()) continue;

            for (const auto& npcItem : npcsJson->GetArray()) {
                if (!npcItem.IsArray() || npcItem.Size() < 3) continue;

                int pIdx = npcItem[0].IsInt() ? npcItem[0].GetInt() : -1;
                uint32_t fID = GetJsonUint(npcItem[1]);
                if (pIdx < 0 || static_cast<size_t>(pIdx) >= plugins.size()) continue;

                std::string npcKey = plugins[pIdx] + "|" + FormatLocalFormID(fID, plugins[pIdx]);
                const auto& rulesMap = npcItem[2];
                if (!rulesMap.IsObject()) continue;

                for (auto ruleIt = rulesMap.MemberBegin(); ruleIt != rulesMap.MemberEnd(); ++ruleIt) {
                    if (!ruleIt->name.IsString() || !ruleIt->value.IsArray() || ruleIt->value.Size() < 2) continue;

                    int rIdx = std::stoi(ruleIt->name.GetString());
                    if (rIdx < 0 || static_cast<size_t>(rIdx) >= rules.size()) continue;

                    const auto& rData = ruleIt->value;
                    AppliedRuleState state;
                    state.version = rData[0].IsInt() ? rData[0].GetInt() : 0;
                    if (rData[1].IsArray()) {
                        for (const auto& groupIndex : rData[1].GetArray()) {
                            int gIdx = groupIndex.IsInt() ? groupIndex.GetInt() : -1;
                            if (gIdx >= 0 && static_cast<size_t>(gIdx) < groups.size()) state.appliedGroups.push_back(groups[gIdx]);
                        }
                    }
                    if (rData.Size() >= 7 && rData[2].IsBool() && rData[3].IsBool() &&
                        rData[4].IsBool() && rData[5].IsArray() && rData[6].IsArray()) {
                        state.activationStateKnown = rData[2].GetBool();
                        state.isActive = rData[3].GetBool();
                        state.canRerollOnNextActivation = rData[4].GetBool();

                        for (const auto& rewardIndex : rData[5].GetArray()) {
                            int rewardIdx = rewardIndex.IsInt() ? rewardIndex.GetInt() : -1;
                            if (rewardIdx >= 0 && static_cast<size_t>(rewardIdx) < rewardKeys.size()) {
                                state.activeRewardKeys.push_back(rewardKeys[rewardIdx]);
                            }
                        }
                        for (const auto& rewardIndex : rData[6].GetArray()) {
                            int rewardIdx = rewardIndex.IsInt() ? rewardIndex.GetInt() : -1;
                            if (rewardIdx >= 0 && static_cast<size_t>(rewardIdx) < rewardKeys.size()) {
                                state.persistentRewardKeys.insert(rewardKeys[rewardIdx]);
                            }
                        }
                    }
                    entry.npcRuleVersions[npcKey][rules[rIdx]] = state;
                }

                if (npcItem.Size() >= 4 && npcItem[3].IsObject()) {
                    const auto& itemsMap = npcItem[3];
                    for (auto itemIt = itemsMap.MemberBegin(); itemIt != itemsMap.MemberEnd(); ++itemIt) {
                        if (!itemIt->name.IsString() || !itemIt->value.IsArray() || itemIt->value.Size() < 4) continue;

                        int itemIdx = std::stoi(itemIt->name.GetString());
                        if (itemIdx < 0 || static_cast<size_t>(itemIdx) >= items.size()) continue;

                        const auto& itemData = itemIt->value;
                        PersistentItemState itemState;
                        itemState.expectedCount = GetJsonUint(itemData[0]);
                        itemState.missingCount = GetJsonUint(itemData[1]);

                        int rIdx = itemData[2].IsInt() ? itemData[2].GetInt() : -1;
                        int gIdx = itemData[3].IsInt() ? itemData[3].GetInt() : -1;
                        if (rIdx >= 0 && static_cast<size_t>(rIdx) < rules.size()) itemState.ruleID = rules[rIdx];
                        if (gIdx >= 0 && static_cast<size_t>(gIdx) < groups.size()) itemState.groupName = groups[gIdx];
                        itemState.isPersistent = itemData.Size() >= 5 && itemData[4].IsBool() ? itemData[4].GetBool() : true;

                        if (itemState.expectedCount > 0) {
                            entry.persistentItems[npcKey][items[itemIdx]] = itemState;
                        }
                    }
                }

                if (npcItem.Size() >= 5 && npcItem[4].IsArray()) {
                    for (const auto& tagIndex : npcItem[4].GetArray()) {
                        int tIdx = tagIndex.IsInt() ? tagIndex.GetInt() : -1;
                        if (tIdx >= 0 && static_cast<size_t>(tIdx) < tags.size()) {
                            entry.virtualKeywords[npcKey].insert(tags[tIdx]);
                        }
                    }
                }

                if (npcItem.Size() >= 6 && npcItem[5].IsObject()) {
                    const auto& factionsMap = npcItem[5];
                    for (auto factIt = factionsMap.MemberBegin(); factIt != factionsMap.MemberEnd(); ++factIt) {
                        if (!factIt->name.IsString() || !factIt->value.IsInt()) continue;

                        int fIdx = std::stoi(factIt->name.GetString());
                        if (fIdx >= 0 && static_cast<size_t>(fIdx) < tags.size()) {
                            entry.addedFactions[npcKey][tags[fIdx]] = factIt->value.GetInt();
                        }
                    }
                }
            }
            decodedHistory.push_back(entry);
        }
        _characterHistory[characterID] = decodedHistory;
        logger::info("[SaveManager] Carregamento concluído. {} snapshots em memória.", decodedHistory.size());
    }
    catch (const std::exception& e) {
        logger::error("[SaveManager] Erro ao carregar/converter save: {}", e.what());
    }
}
void SaveStateManager::UpdateSaveEntry(uint32_t characterID, const SaveHistoryEntry& newEntry) {
    auto& history = _characterHistory[characterID];

    // Atualiza cache em memória
    auto it = std::find_if(history.begin(), history.end(), [&](const SaveHistoryEntry& e) {
        return e.saveNumber == newEntry.saveNumber;
        });
    if (it != history.end()) {
        *it = newEntry;
        logger::debug("[SaveManager] Atualizando snapshot existente #{} em memória.", newEntry.saveNumber);
    }
    else {
        history.push_back(newEntry);
        logger::debug("[SaveManager] Adicionando novo snapshot #{} ao histórico.", newEntry.saveNumber);
    }

    logger::info("[SaveManager] Iniciando compressão para escrita (Save #{})", newEntry.saveNumber);

    // --- COMPRESSÃO ---
    std::vector<std::string> p_plugins, p_rules, p_groups, p_items, p_tags, p_rewards;

    try {
        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();

        rapidjson::Value jHistory(rapidjson::kArrayType);
        for (const auto& entry : history) {
            rapidjson::Value jEntry(rapidjson::kObjectType);
            jEntry.AddMember("sn", entry.saveNumber, alloc);

            rapidjson::Value jNPCs(rapidjson::kArrayType);
            for (auto const& [npcKey, ruleMap] : entry.npcRuleVersions) {
                auto tokens = split(npcKey, '|');
                if (tokens.size() < 2) continue;

                rapidjson::Value jNPC(rapidjson::kArrayType);
                jNPC.PushBack(GetPoolIndex(p_plugins, tokens[0]), alloc);
                jNPC.PushBack(static_cast<uint32_t>(std::stoul(tokens[1], nullptr, 16)), alloc);

                rapidjson::Value jRules(rapidjson::kObjectType);
                for (auto const& [ruleID, state] : ruleMap) {
                    rapidjson::Value jState(rapidjson::kArrayType);
                    jState.PushBack(state.version, alloc);

                    rapidjson::Value jGrp(rapidjson::kArrayType);
                    for (const auto& gName : state.appliedGroups) {
                        jGrp.PushBack(GetPoolIndex(p_groups, gName), alloc);
                    }
                    jState.PushBack(jGrp, alloc);
                    jState.PushBack(state.activationStateKnown, alloc);
                    jState.PushBack(state.isActive, alloc);
                    jState.PushBack(state.canRerollOnNextActivation, alloc);

                    rapidjson::Value jActiveRewards(rapidjson::kArrayType);
                    for (const auto& rewardKey : state.activeRewardKeys) {
                        jActiveRewards.PushBack(GetPoolIndex(p_rewards, rewardKey), alloc);
                    }
                    jState.PushBack(jActiveRewards, alloc);

                    rapidjson::Value jPersistentRewards(rapidjson::kArrayType);
                    for (const auto& rewardKey : state.persistentRewardKeys) {
                        jPersistentRewards.PushBack(GetPoolIndex(p_rewards, rewardKey), alloc);
                    }
                    jState.PushBack(jPersistentRewards, alloc);

                    const auto ruleIndex = std::to_string(GetPoolIndex(p_rules, ruleID));
                    rapidjson::Value ruleKey;
                    ruleKey.SetString(ruleIndex.c_str(), static_cast<rapidjson::SizeType>(ruleIndex.size()), alloc);
                    jRules.AddMember(ruleKey, jState, alloc);
                }
                jNPC.PushBack(jRules, alloc);

                rapidjson::Value jItems(rapidjson::kObjectType);
                auto itemNpcIt = entry.persistentItems.find(npcKey);
                if (itemNpcIt != entry.persistentItems.end()) {
                    for (auto const& [itemKey, itemState] : itemNpcIt->second) {
                        rapidjson::Value jItemState(rapidjson::kArrayType);
                        jItemState.PushBack(itemState.expectedCount, alloc);
                        jItemState.PushBack(itemState.missingCount, alloc);
                        jItemState.PushBack(GetPoolIndex(p_rules, itemState.ruleID), alloc);
                        jItemState.PushBack(GetPoolIndex(p_groups, itemState.groupName), alloc);
                        jItemState.PushBack(itemState.isPersistent, alloc);

                        const auto itemIndex = std::to_string(GetPoolIndex(p_items, itemKey));
                        rapidjson::Value itemJsonKey;
                        itemJsonKey.SetString(itemIndex.c_str(), static_cast<rapidjson::SizeType>(itemIndex.size()), alloc);
                        jItems.AddMember(itemJsonKey, jItemState, alloc);
                    }
                }
                jNPC.PushBack(jItems, alloc);

                rapidjson::Value jVirtualKeywords(rapidjson::kArrayType);
                auto keywordNpcIt = entry.virtualKeywords.find(npcKey);
                if (keywordNpcIt != entry.virtualKeywords.end()) {
                    for (const auto& keywordKey : keywordNpcIt->second) {
                        jVirtualKeywords.PushBack(GetPoolIndex(p_tags, keywordKey), alloc);
                    }
                }
                jNPC.PushBack(jVirtualKeywords, alloc);

                rapidjson::Value jFactions(rapidjson::kObjectType);
                auto factionNpcIt = entry.addedFactions.find(npcKey);
                if (factionNpcIt != entry.addedFactions.end()) {
                    for (const auto& [factionKey, rank] : factionNpcIt->second) {
                        const auto factionIndex = std::to_string(GetPoolIndex(p_tags, factionKey));
                        rapidjson::Value factionJsonKey;
                        factionJsonKey.SetString(factionIndex.c_str(), static_cast<rapidjson::SizeType>(factionIndex.size()), alloc);
                        jFactions.AddMember(factionJsonKey, rank, alloc);
                    }
                }
                jNPC.PushBack(jFactions, alloc);

                jNPCs.PushBack(jNPC, alloc);
            }
            jEntry.AddMember("npcs", jNPCs, alloc);
            jHistory.PushBack(jEntry, alloc);
        }

        doc.AddMember("p_plugins", WriteStringArray(p_plugins, alloc), alloc);
        doc.AddMember("p_rules", WriteStringArray(p_rules, alloc), alloc);
        doc.AddMember("p_groups", WriteStringArray(p_groups, alloc), alloc);
        doc.AddMember("p_items", WriteStringArray(p_items, alloc), alloc);
        doc.AddMember("p_tags", WriteStringArray(p_tags, alloc), alloc);
        doc.AddMember("p_rewards", WriteStringArray(p_rewards, alloc), alloc);
        doc.AddMember("history", jHistory, alloc);

        std::string path = GetCharacterPath(characterID);
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());

        std::ofstream o(path);
        if (o.is_open()) {
            o << SerializeJson(doc);
            o.close();
            logger::info("[SaveManager] Arquivo de histórico atualizado com sucesso: {}", path);
        }
        else {
            logger::error("[SaveManager] Erro crítico: Não foi possível abrir o arquivo para escrita: {}", path);
        }
    }
    catch (const std::exception& e) {
        logger::error("[SaveManager] Exceção durante a compressão/escrita do JSON: {}", e.what());
    }
}
bool IsActorSleeping(RE::Actor* a_actor) {
    if (!a_actor) return false;
    auto actorState = a_actor->AsActorState();
    if (!actorState) return false;

    auto sitSleepState = actorState->GetSitSleepState();
    return (sitSleepState == RE::SIT_SLEEP_STATE::kIsSleeping);
}

RE::BGSOutfit* GetAppliedSleepOutfit(RE::Actor* a_actor) {
    auto& manager = *SaveStateManager::GetSingleton();
    auto& session = manager.GetSessionData();

    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return nullptr;

    // Gerar npcKey consistente com a lógica de salvamento
    RE::FormID actorID = a_actor->GetFormID();
    std::string fileNameStr = "Dynamic";
    if (auto file = GetSourceFileByFormID(baseNPC)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Dynamic";
    }

    std::string npcKey = fileNameStr + "|" + FormatLocalFormID(actorID, fileNameStr);

    if (!session.npcRuleVersions.contains(npcKey)) return nullptr;

    // Considera somente rewards realmente sorteados na ativação atual.
    for (auto const& [ruleID, state] : session.npcRuleVersions[npcKey]) {
        if (!state.activationStateKnown || !state.isActive) continue;

        auto rule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, state.version);
        if (!rule) continue;

        for (const auto& rewardKey : state.activeRewardKeys) {
            const RewardGroup* group = nullptr;
            const Reward* reward = nullptr;
            if (!ResolveRewardState(*rule, rewardKey, group, reward) || !reward) continue;
            if (reward->typeReward != "Outfit" ||
                (reward->functionOnType != 1 && reward->functionOnType != 2)) {
                continue;
            }

            auto [plugin, fID] = reward->ParseFormID();
            if (auto outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(fID)) {
                return outfit;
            }
        }
    }
    return nullptr;
}

namespace {
    constexpr int kInventoryArmorPriority = 0;
    constexpr int kRuleOutfitArmorPriority = 1;
    constexpr int kRuleArmorPriority = 2;

    struct ArmorEquipCandidate {
        RE::TESObjectARMO* armor = nullptr;
        RE::InventoryEntryData* entry = nullptr;
        int priority = kInventoryArmorPriority;
        std::uint32_t slotMask = 0;
    };

    struct ActiveRuleArmorSelection {
        std::map<RE::FormID, int> priorities;
        std::set<RE::FormID> sleepOnlyItems;
    };

    bool HasCompatibleArmorModel(RE::Actor* a_actor, RE::TESObjectARMO* a_armor)
    {
        if (!a_actor || !a_armor) return false;

        auto race = a_actor->GetRace();
        auto baseNPC = a_actor->GetActorBase();
        if (!race || !baseNPC) return false;

        const auto sex = baseNPC->GetSex();
        if (sex != RE::SEXES::kMale && sex != RE::SEXES::kFemale) return false;

        const auto armorMask = a_armor->GetSlotMask().underlying();
        if (armorMask == 0) return false;

        std::set<RE::FormID> visitedTemplates;
        for (auto currentArmor = a_armor;
             currentArmor && visitedTemplates.insert(currentArmor->GetFormID()).second;
             currentArmor = currentArmor->templateArmor) {
            for (auto* addon : currentArmor->armorAddons) {
                if (!addon || !addon->IsValidRace(race) ||
                    (addon->GetSlotMask().underlying() & armorMask) == 0) {
                    continue;
                }

                const auto* model = addon->bipedModels[sex].GetModel();
                if (model && model[0] != '\0') {
                    return true;
                }
            }
        }

        return false;
    }

    void MarkPreferredArmor(
        std::map<RE::FormID, int>& a_priorities,
        RE::TESForm* a_form,
        int a_priority)
    {
        auto armor = a_form ? a_form->As<RE::TESObjectARMO>() : nullptr;
        if (!armor) return;

        auto& currentPriority = a_priorities[armor->GetFormID()];
        currentPriority = std::max(currentPriority, a_priority);
    }

    ActiveRuleArmorSelection GetActiveRuleArmorSelection(RE::Actor* a_actor)
    {
        ActiveRuleArmorSelection selection;
        if (!a_actor) return selection;

        const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
        auto& session = SaveStateManager::GetSingleton()->GetSessionData();
        const auto npcIt = session.npcRuleVersions.find(npcKey);
        if (npcIt == session.npcRuleVersions.end()) return selection;

        for (const auto& [ruleID, state] : npcIt->second) {
            if (!state.activationStateKnown) continue;

            auto rule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, state.version);
            if (!rule) continue;

            std::set<std::string> relevantRewardKeys{
                state.persistentRewardKeys.begin(), state.persistentRewardKeys.end()
            };
            if (state.isActive) {
                relevantRewardKeys.insert(
                    state.activeRewardKeys.begin(), state.activeRewardKeys.end());
            }

            for (const auto& rewardKey : relevantRewardKeys) {
                const RewardGroup* group = nullptr;
                const Reward* reward = nullptr;
                if (!ResolveRewardState(*rule, rewardKey, group, reward) || !reward) continue;

                auto [plugin, formID] = reward->ParseFormID();
                auto rewardForm = RE::TESForm::LookupByID(formID);
                if (!rewardForm) continue;

                if (reward->typeReward == "Armor") {
                    MarkPreferredArmor(selection.priorities, rewardForm, kRuleArmorPriority);
                }
                else if (reward->typeReward == "Outfit") {
                    if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                        for (auto* outfitItem : outfit->outfitItems) {
                            auto armor = outfitItem ? outfitItem->As<RE::TESObjectARMO>() : nullptr;
                            if (!armor) continue;

                            if (reward->functionOnType == 0 || reward->functionOnType == 2) {
                                MarkPreferredArmor(
                                    selection.priorities, armor, kRuleOutfitArmorPriority);
                            }
                            else if (reward->functionOnType == 1) {
                                selection.sleepOnlyItems.insert(armor->GetFormID());
                            }
                        }
                    }
                }
            }
        }

        for (const auto& preferred : selection.priorities) {
            selection.sleepOnlyItems.erase(preferred.first);
        }
        return selection;
    }

    std::vector<RE::TESObjectARMO*> SelectCompatibleArmorSet(
        RE::Actor* a_actor,
        std::vector<RE::TESObjectARMO*> a_armors)
    {
        std::erase_if(a_armors, [&](RE::TESObjectARMO* a_armor) {
            return !a_armor || a_armor->IsShield() ||
                a_actor->GetInventoryCount(a_armor) <= 0 ||
                !HasCompatibleArmorModel(a_actor, a_armor);
        });

        std::ranges::sort(a_armors, [](RE::TESObjectARMO* a_lhs, RE::TESObjectARMO* a_rhs) {
            if (a_lhs->GetArmorRating() != a_rhs->GetArmorRating()) {
                return a_lhs->GetArmorRating() > a_rhs->GetArmorRating();
            }
            return a_lhs->GetFormID() < a_rhs->GetFormID();
        });

        std::vector<RE::TESObjectARMO*> selected;
        std::set<RE::FormID> seen;
        std::uint32_t occupiedSlots = 0;
        for (auto* armor : a_armors) {
            if (!seen.insert(armor->GetFormID()).second) continue;

            const auto slotMask = armor->GetSlotMask().underlying();
            if ((occupiedSlots & slotMask) != 0) continue;

            selected.push_back(armor);
            occupiedSlots |= slotMask;
        }
        return selected;
    }
}

void EquipBestInventoryItems(RE::Actor* a_actor)
{
    if (!a_actor || a_actor->IsPlayer()) return;

    auto equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) return;

    auto inventory = a_actor->GetInventory();
    const auto activeRuleSelection = GetActiveRuleArmorSelection(a_actor);

    std::vector<ArmorEquipCandidate> candidates;
    candidates.reserve(inventory.size());
    for (auto& [item, inventoryData] : inventory) {
        auto& [count, entry] = inventoryData;
        auto armor = item ? item->As<RE::TESObjectARMO>() : nullptr;
        if (count <= 0 || !armor || armor->IsShield()) continue;
        if (activeRuleSelection.sleepOnlyItems.contains(armor->GetFormID())) continue;

        const auto slotMask = armor->GetSlotMask().underlying();
        if (slotMask == 0 || !HasCompatibleArmorModel(a_actor, armor)) {
            logger::debug(
                "[EquipBest] Ignorando '{}' para '{}': raça, sexo, modelo ou slots incompatíveis.",
                armor->GetName(), a_actor->GetName());
            continue;
        }

        const auto priorityIt = activeRuleSelection.priorities.find(armor->GetFormID());
        const auto priority = priorityIt != activeRuleSelection.priorities.end() ?
            priorityIt->second :
            kInventoryArmorPriority;
        candidates.push_back({ armor, entry.get(), priority, slotMask });
    }

    std::ranges::sort(candidates, [](const ArmorEquipCandidate& a_lhs, const ArmorEquipCandidate& a_rhs) {
        if (a_lhs.priority != a_rhs.priority) {
            return a_lhs.priority > a_rhs.priority;
        }
        if (a_lhs.armor->GetArmorRating() != a_rhs.armor->GetArmorRating()) {
            return a_lhs.armor->GetArmorRating() > a_rhs.armor->GetArmorRating();
        }
        return a_lhs.armor->GetFormID() < a_rhs.armor->GetFormID();
    });

    std::vector<const ArmorEquipCandidate*> selected;
    std::set<RE::FormID> selectedIDs;
    std::uint32_t occupiedSlots = 0;
    for (const auto& candidate : candidates) {
        if ((occupiedSlots & candidate.slotMask) != 0) continue;

        selected.push_back(std::addressof(candidate));
        selectedIDs.insert(candidate.armor->GetFormID());
        occupiedSlots |= candidate.slotMask;
    }

    for (auto& [item, inventoryData] : inventory) {
        auto armor = item ? item->As<RE::TESObjectARMO>() : nullptr;
        auto entry = inventoryData.second.get();
        if (!armor || !entry || !entry->IsWorn() || selectedIDs.contains(armor->GetFormID())) {
            continue;
        }

        if ((armor->GetSlotMask().underlying() & occupiedSlots) != 0) {
            equipManager->UnequipObject(a_actor, armor);
        }
    }

    for (const auto* candidate : selected) {
        if (!candidate || !candidate->armor || !candidate->entry || candidate->entry->IsWorn()) {
            continue;
        }

        auto extraData =
            candidate->entry->extraLists && !candidate->entry->extraLists->empty() ?
            candidate->entry->extraLists->front() :
            nullptr;
        equipManager->EquipObject(
            a_actor, candidate->armor, extraData, 1, nullptr, false, false, false, true);

        logger::debug(
            "[EquipBest] Equipando '{}' em '{}' (prioridade {}, rating {}, slots {:08X}).",
            candidate->armor->GetName(), a_actor->GetName(), candidate->priority,
            candidate->armor->GetArmorRating(), candidate->slotMask);
    }
}

// Gerencia apenas as peças controladas pelo ciclo de sono da EDF.
void ManageSleepOutfitState(RE::Actor* a_actor, bool a_isEntering) {
    if (!a_actor) return;
    auto equipManager = RE::ActorEquipManager::GetSingleton();
    if (!equipManager) return;

    const auto actorID = a_actor->GetFormID();

    if (a_isEntering) {
        auto sleepOutfit = GetAppliedSleepOutfit(a_actor);
        if (!sleepOutfit) return;

        logger::info("[SleepSystem] Actor {} entrando na cama. Equipando traje de sono.", a_actor->GetName());
        auto inventory = a_actor->GetInventory();
        for (auto& [item, invData] : inventory) {
            if (item->IsArmor() && invData.second->IsWorn()) {
                equipManager->UnequipObject(a_actor, item);
            }
        }

        std::vector<RE::TESObjectARMO*> sleepArmorCandidates;
        std::vector<RE::TESBoundObject*> sleepNonArmorItems;
        for (auto* form : sleepOutfit->outfitItems) {
            if (auto* bound = form ? form->As<RE::TESBoundObject>() : nullptr) {
                if (a_actor->GetInventoryCount(bound) <= 0) continue;

                if (auto* armor = bound->As<RE::TESObjectARMO>()) {
                    sleepArmorCandidates.push_back(armor);
                }
                else {
                    sleepNonArmorItems.push_back(bound);
                }
            }
        }

        std::vector<RE::FormID> equippedItems;
        for (auto* armor : SelectCompatibleArmorSet(a_actor, std::move(sleepArmorCandidates))) {
            equipManager->EquipObject(a_actor, armor, nullptr, 1, nullptr, false, false, false, true);
            equippedItems.push_back(armor->GetFormID());
        }
        for (auto* item : sleepNonArmorItems) {
            equipManager->EquipObject(a_actor, item, nullptr, 1, nullptr, false, false, false, true);
            equippedItems.push_back(item->GetFormID());
        }

        std::lock_guard lock(g_sleepUpdatesMutex);
        g_sleepEquippedItems[actorID] = std::move(equippedItems);
    }
    else {
        logger::info("[SleepSystem] Actor {} saindo da cama. Liberando traje de sono.", a_actor->GetName());

        std::vector<RE::FormID> equippedItems;
        {
            std::lock_guard lock(g_sleepUpdatesMutex);
            if (auto it = g_sleepEquippedItems.find(actorID); it != g_sleepEquippedItems.end()) {
                equippedItems = std::move(it->second);
                g_sleepEquippedItems.erase(it);
            }
        }

        for (const auto formID : equippedItems) {
            if (auto* bound = RE::TESForm::LookupByID<RE::TESBoundObject>(formID)) {
                equipManager->UnequipObject(a_actor, bound);
            }
        }

        // Recompõe os slots após retirar o traje de sono. Rewards Armor têm
        // precedência sobre Outfits Normal/Both e sobre o restante do inventário.
        EquipBestInventoryItems(a_actor);
    }
}

struct PendingEquip {
    RE::TESBoundObject* object;
    int priority; // 0 para Outfit (Baixa), 1 para Recompensas Individuais (Alta)
};

void RemoveManagedTemporaryItemOnInvalidation(RE::Actor* a_actor, RE::TESBoundObject* a_item,
    uint32_t a_fallbackCount, const std::string& a_ruleID, const std::string& a_groupName)
{
    if (!a_actor || !a_item || a_fallbackCount == 0) return;

    auto saveManager = SaveStateManager::GetSingleton();
    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    const auto itemKey = SaveStateManager::BuildFormKey(a_item);
    auto& session = saveManager->GetSessionData();
    auto npcIt = session.persistentItems.find(npcKey);

    uint32_t removeCount = a_fallbackCount;
    bool eraseManagedState = false;
    std::string managedKeyToErase;

    if (npcIt != session.persistentItems.end()) {
        for (auto& [managedKey, state] : npcIt->second) {
            if (!ManagedItemMatches(managedKey, itemKey, state, a_ruleID, a_groupName, false)) continue;

            const auto protectedCount = state.expectedCount > state.missingCount ? state.expectedCount - state.missingCount : 0;
            removeCount = protectedCount;
            eraseManagedState = state.missingCount == 0;
            managedKeyToErase = managedKey;
            break;
        }
    }

    const auto currentCount = GetInventoryCount(a_actor, a_item);
    removeCount = std::min(removeCount, currentCount);
    if (removeCount > 0) {
        a_actor->RemoveItem(a_item, static_cast<std::int32_t>(removeCount), RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    }

    if (eraseManagedState && npcIt != session.persistentItems.end()) {
        npcIt->second.erase(managedKeyToErase);
        if (npcIt->second.empty()) {
            session.persistentItems.erase(npcIt);
        }
    }
}

void RemoveRuleRewards(RE::Actor* a_actor, const Rule& a_rule) {
    if (!a_actor) return;

    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) return;
    bool isPlayer = a_actor->IsPlayer();
    for (const auto& group : a_rule.rewardGroups) {
        for (const auto& reward : group.rewards) {
            if (reward.isPersistent) continue;
            auto [plugin, fID] = reward.ParseFormID();
            auto rewardForm = RE::TESForm::LookupByID(fID);
            if (!rewardForm) continue;

            if (reward.typeReward == "Spell") {
                if (auto spell = rewardForm->As<RE::SpellItem>()) {

                    // 1. Dispel (Para functionOnType 1 ou 2) - SUGERIDO POR VOCÊ
                    if (reward.functionOnType == 1 || reward.functionOnType == 2) {
                        if (auto activeEffectList = a_actor->AsMagicTarget()->GetActiveEffectList()) {
                            for (auto& effect : *activeEffectList) {
                                if (effect && effect->spell == spell) {
                                    effect->Dispel(true);
                                    logger::debug("[Dispel] Efeito de '{}' removido de {}.", spell->GetName(), a_actor->GetName());
                                }
                            }
                        }
                    }

                    // 2. RemoveSpell (Para functionOnType 0 ou 2)
                    if (reward.functionOnType == 0 || reward.functionOnType == 2) {
                        // Se for NPC: Remove apenas se não for nativo do ActorBase
                        if (!isPlayer) {
                            if (!baseNPC->GetSpellList()->GetIndex(spell).has_value()) {
                                a_actor->RemoveSpell(spell);
                            }
                        }

                    }
                }
            }
            else if (reward.typeReward == "Shout") {
                if (auto shout = rewardForm->As<RE::TESShout>()) {
                    if (!isPlayer) {
                        if (auto spellList = baseNPC->GetSpellList(); spellList && spellList->GetIndex(shout).has_value()) {
                            spellList->RemoveShout(shout);
                        }
                    }
                }
            }
            else if (reward.typeReward == "Keyword") {
                if (auto keyword = rewardForm->As<RE::BGSKeyword>()) {
                    SaveStateManager::GetSingleton()->RemoveVirtualKeyword(a_actor, keyword);
                }
            }
            else if (reward.typeReward == "Faction") {
                if (auto faction = rewardForm->As<RE::TESFaction>()) {
                    SaveStateManager::GetSingleton()->RemoveManagedFaction(a_actor, faction);
                }
            }
            //else if (reward.typeReward == "Package") {
            //    if (auto pkg = rewardForm->As<RE::TESPackage>()) {
            //        auto& packageList = baseNPC->aiPackages.packages;

            //        // Verifica se o pacote já está na lista (conforme visto no Distribute.cpp)
            //        if (std::ranges::find(packageList, pkg) == packageList.end()) {
            //            // Adicionamos ao início (push_front) para que ele tenha prioridade sobre pacotes padrão
            //            packageList.push_front(pkg);

            //            // Força o NPC a reavaliar sua pilha de pacotes imediatamente
            //            a_actor->EvaluatePackage(false, false);
            //            logger::debug("[ApplyRules] Package {} adicionado ao BaseNPC {}", reward.formIDStr, baseNPC->GetName());
            //        }
            //    }
            //}
            else if (reward.typeReward == "Perk") {
                if (auto perk = rewardForm->As<RE::BGSPerk>()) {
                    if (!isPlayer) { // Proteção extra para o Player
                        bool isNative = false;
                        for (uint32_t i = 0; i < baseNPC->perkCount; i++) {
                            if (baseNPC->perks[i].perk == perk) { isNative = true; break; }
                        }
                        if (!isNative) a_actor->RemovePerk(perk);
                    }
                }
            }
            else if (reward.typeReward == "Outfit") {
                if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
                    // Todos os modos adicionam os itens ao inventário. Na
                    // invalidação, removemos apenas a quantidade ainda
                    // atribuída a esta regra pelo ledger.
                    for (auto* form : outfit->outfitItems) {
                        if (auto* bound = form->As<RE::TESBoundObject>()) {
                            RemoveManagedTemporaryItemOnInvalidation(a_actor, bound, 1, a_rule.id, group.name);
                        }
                    }
                }
            }
            else {
                // Itens genéricos (Weapon, Armor, Ammo, etc)
                if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
                    // Itens são mais difíceis de rastrear se são "nativos",
                    // mas removemos a quantidade estipulada pela regra.
                    RemoveManagedTemporaryItemOnInvalidation(a_actor, bound, reward.amount, a_rule.id, group.name);
                }
            }
        }
    }
    logger::debug("[LocationUpdate] Regra '{}' desaplicada de {}. Atributos nativos preservados.", a_rule.name, a_actor->GetName());
}

void ApplyRewardPhysical(RE::Actor* a_actor, const Reward& reward, bool isPlayer, std::vector<PendingEquip>& equipQueue) {
    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    if (reward.typeReward == "Spell") {
        if (auto spell = rewardForm->As<RE::SpellItem>()) {
            if (reward.functionOnType == 0 || reward.functionOnType == 2) {
                if (!a_actor->HasSpell(spell)) a_actor->AddSpell(spell);
            }
            if (reward.functionOnType == 1 || reward.functionOnType == 2) {
                auto caster = a_actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (caster) caster->CastSpellImmediate(spell, false, a_actor, 1.0f, false, 0.0f, a_actor);
            }
        }
    }
    else if (reward.typeReward == "Shout") {
        if (auto shout = rewardForm->As<RE::TESShout>()) {
            if (!a_actor->HasShout(shout)) a_actor->AddShout(shout);
        }
    }
    else if (reward.typeReward == "Keyword") {
        if (auto keyword = rewardForm->As<RE::BGSKeyword>()) {
            SaveStateManager::GetSingleton()->AddVirtualKeyword(a_actor, keyword);
        }
    }
    else if (reward.typeReward == "Faction") {
        if (auto faction = rewardForm->As<RE::TESFaction>()) {
            SaveStateManager::GetSingleton()->AddManagedFaction(a_actor, faction, static_cast<int>(reward.amount));
        }
    }
    else if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            // Sempre adicionamos os itens ao inventário do NPC
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    a_actor->AddObjectToContainer(bound, nullptr, 1, nullptr);
                    // Só adicionamos à fila de equipagem imediata se o traje permitir
                    // uso padrão (Standard ou Both)
                    if (!isPlayer && (reward.functionOnType == 0 || reward.functionOnType == 2)) {
                        equipQueue.push_back({ bound, 0 });
                    }
                }
            }
        }
    }
    else if (reward.typeReward == "Perk") {
        if (auto perk = rewardForm->As<RE::BGSPerk>()) { if (!a_actor->HasPerk(perk)) a_actor->AddPerk(perk, 1); }
    }
    else {
        // Itens físicos (Weapon, Armor, etc)
        if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
            a_actor->AddObjectToContainer(bound, nullptr, reward.amount, nullptr);
            if (!isPlayer && (reward.typeReward == "Weapon" || reward.typeReward == "Armor" || reward.typeReward == "Ammo")) {
                equipQueue.push_back({ bound, 1 });
            }
        }
    }
}

void TrackPersistentRewardItems(RE::Actor* a_actor, const Reward& reward, const std::string& ruleID, const std::string& groupName)
{
    if (!a_actor) return;

    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    auto saveManager = SaveStateManager::GetSingleton();
    if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    saveManager->TrackPersistentItemGrant(a_actor, bound, 1, ruleID, groupName, reward.isPersistent);
                }
            }
        }
    }
    else if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
        saveManager->TrackPersistentItemGrant(a_actor, bound, reward.amount, ruleID, groupName, reward.isPersistent);
    }
}

void EnsurePersistentRewardTracked(RE::Actor* a_actor, const Reward& reward, const std::string& ruleID, const std::string& groupName)
{
    if (!a_actor) return;

    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    auto saveManager = SaveStateManager::GetSingleton();
    if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    saveManager->EnsurePersistentItemTracked(a_actor, bound, 1, ruleID, groupName, reward.isPersistent);
                }
            }
        }
    }
    else if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
        saveManager->EnsurePersistentItemTracked(a_actor, bound, reward.amount, ruleID, groupName, reward.isPersistent);
    }
}

uint32_t GetPersistentRestoreCount(RE::Actor* a_actor, RE::TESBoundObject* a_item,
    const std::string& ruleID, const std::string& groupName, bool isPersistent)
{
    if (!a_actor || !a_item) return 0;

    auto saveManager = SaveStateManager::GetSingleton();
    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    const auto itemKey = SaveStateManager::BuildFormKey(a_item);
    if (npcKey.empty() || itemKey.empty()) return 0;

    auto& session = saveManager->GetSessionData();
    auto npcIt = session.persistentItems.find(npcKey);
    if (npcIt == session.persistentItems.end()) return 0;

    auto itemState = FindManagedItemState(npcIt->second, itemKey, ruleID, groupName, isPersistent);
    if (!itemState) return 0;

    const auto protectedCount = itemState->expectedCount > itemState->missingCount ? itemState->expectedCount - itemState->missingCount : 0;
    const auto currentCount = GetInventoryCount(a_actor, a_item);
    return protectedCount > currentCount ? protectedCount - currentCount : 0;
}

void RestorePersistentBoundObject(RE::Actor* a_actor, RE::TESBoundObject* a_item, uint32_t a_count,
    bool isPlayer, std::vector<PendingEquip>& equipQueue)
{
    if (!a_actor || !a_item || a_count == 0) return;

    a_actor->AddObjectToContainer(a_item, nullptr, static_cast<std::int32_t>(a_count), nullptr);
    if (!isPlayer && (a_item->IsWeapon() || a_item->IsArmor() || a_item->IsAmmo())) {
        equipQueue.push_back({ a_item, 1 });
    }

    logger::info("[PersistentLedger] Restaurado '{}' x{} para '{}'", a_item->GetName(), a_count, a_actor->GetName());
}

void RestorePersistentRewardIfNeeded(RE::Actor* a_actor, const Reward& reward, bool isPlayer,
    std::vector<PendingEquip>& equipQueue, const std::string& ruleID, const std::string& groupName)
{
    if (!a_actor) return;

    EnsurePersistentRewardTracked(a_actor, reward, ruleID, groupName);

    auto [plugin, fID] = reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return;

    if (reward.typeReward == "Outfit") {
        if (auto outfit = rewardForm->As<RE::BGSOutfit>()) {
            for (auto* form : outfit->outfitItems) {
                if (auto* bound = form->As<RE::TESBoundObject>()) {
                    RestorePersistentBoundObject(a_actor, bound,
                        GetPersistentRestoreCount(a_actor, bound, ruleID, groupName, reward.isPersistent), isPlayer, equipQueue);
                }
            }
        }
    }
    else if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
        RestorePersistentBoundObject(a_actor, bound,
            GetPersistentRestoreCount(a_actor, bound, ruleID, groupName, reward.isPersistent), isPlayer, equipQueue);
    }
}

void ApplyRewardPhysicalTracked(RE::Actor* a_actor, const Reward& reward, bool isPlayer, std::vector<PendingEquip>& equipQueue,
    const std::string& ruleID, const std::string& groupName)
{
    ApplyRewardPhysical(a_actor, reward, isPlayer, equipQueue);
    TrackPersistentRewardItems(a_actor, reward, ruleID, groupName);
}

bool IsRewardRepresentedInCurrentState(
    RE::Actor* a_actor,
    const Reward& a_reward,
    const std::string& a_ruleID,
    const std::string& a_groupName)
{
    if (!a_actor) return false;

    auto saveManager = SaveStateManager::GetSingleton();
    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    auto& session = saveManager->GetSessionData();

    auto [plugin, fID] = a_reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return false;

    const auto hasTrackedBoundObject = [&](RE::TESBoundObject* a_item) {
        if (!a_item) return false;
        const auto npcIt = session.persistentItems.find(npcKey);
        if (npcIt == session.persistentItems.end()) return false;
        const auto itemKey = SaveStateManager::BuildFormKey(a_item);
        return FindManagedItemState(
            npcIt->second, itemKey, a_ruleID, a_groupName, a_reward.isPersistent) != nullptr;
    };

    if (a_reward.typeReward == "Outfit") {
        auto outfit = rewardForm->As<RE::BGSOutfit>();
        if (!outfit) return false;
        return std::ranges::any_of(outfit->outfitItems, [&](RE::TESForm* a_form) {
            return hasTrackedBoundObject(a_form ? a_form->As<RE::TESBoundObject>() : nullptr);
        });
    }
    if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
        return hasTrackedBoundObject(bound);
    }
    if (a_reward.typeReward == "Keyword") {
        auto keyword = rewardForm->As<RE::BGSKeyword>();
        return keyword && saveManager->HasVirtualKeyword(a_actor, keyword);
    }
    if (a_reward.typeReward == "Faction") {
        auto faction = rewardForm->As<RE::TESFaction>();
        const auto factionKey = SaveStateManager::BuildFormKey(faction);
        const auto npcIt = session.addedFactions.find(npcKey);
        return faction && npcIt != session.addedFactions.end() && npcIt->second.contains(factionKey);
    }
    if (a_reward.typeReward == "Spell") {
        auto spell = rewardForm->As<RE::SpellItem>();
        if (!spell) return false;
        if ((a_reward.functionOnType == 0 || a_reward.functionOnType == 2) && a_actor->HasSpell(spell)) {
            return true;
        }
        if (a_reward.functionOnType == 1 || a_reward.functionOnType == 2) {
            if (auto effects = a_actor->AsMagicTarget()->GetActiveEffectList()) {
                return std::ranges::any_of(*effects, [&](const auto& a_effect) {
                    return a_effect && a_effect->spell == spell;
                });
            }
        }
        return false;
    }
    if (a_reward.typeReward == "Shout") {
        auto shout = rewardForm->As<RE::TESShout>();
        return shout && a_actor->HasShout(shout);
    }
    if (a_reward.typeReward == "Perk") {
        auto perk = rewardForm->As<RE::BGSPerk>();
        return perk && a_actor->HasPerk(perk);
    }

    return false;
}

bool HasOutstandingNonPersistentReward(
    RE::Actor* a_actor,
    const Reward& a_reward,
    const std::string& a_ruleID,
    const std::string& a_groupName)
{
    if (!a_actor || a_reward.isPersistent) return false;

    auto [plugin, fID] = a_reward.ParseFormID();
    auto rewardForm = RE::TESForm::LookupByID(fID);
    if (!rewardForm) return false;

    const auto npcKey = SaveStateManager::BuildNPCKey(a_actor);
    auto& session = SaveStateManager::GetSingleton()->GetSessionData();
    const auto npcIt = session.persistentItems.find(npcKey);

    const auto hasMissingManagedCopies = [&](RE::TESBoundObject* a_item) {
        if (!a_item || npcIt == session.persistentItems.end()) return false;
        const auto itemKey = SaveStateManager::BuildFormKey(a_item);
        const auto itemState = FindManagedItemState(
            npcIt->second, itemKey, a_ruleID, a_groupName, false);
        return itemState && itemState->missingCount > 0;
    };

    if (a_reward.typeReward == "Outfit") {
        auto outfit = rewardForm->As<RE::BGSOutfit>();
        return outfit && std::ranges::any_of(outfit->outfitItems, [&](RE::TESForm* a_form) {
            return hasMissingManagedCopies(a_form ? a_form->As<RE::TESBoundObject>() : nullptr);
        });
    }
    if (auto bound = rewardForm->As<RE::TESBoundObject>()) {
        return hasMissingManagedCopies(bound);
    }

    // Para rewards não físicos, permanecer no ator após a tentativa de
    // remoção significa que o resultado anterior ainda está presente.
    return IsRewardRepresentedInCurrentState(
        a_actor, a_reward, a_ruleID, a_groupName);
}

void MigrateLegacyActivationState(
    RE::Actor* a_actor,
    const Rule& a_rule,
    AppliedRuleState& a_state)
{
    a_state.activeRewardKeys.clear();
    a_state.persistentRewardKeys.clear();

    for (const auto& groupName : a_state.appliedGroups) {
        const auto groupIt = std::ranges::find_if(a_rule.rewardGroups, [&](const RewardGroup& a_group) {
            return a_group.name == groupName;
        });
        if (groupIt == a_rule.rewardGroups.end()) continue;

        for (const auto& reward : groupIt->rewards) {
            if (!IsRewardRepresentedInCurrentState(a_actor, reward, a_rule.id, groupIt->name)) continue;

            const auto rewardKey = BuildRewardStateKey(groupIt->name, reward);
            a_state.activeRewardKeys.push_back(rewardKey);
            if (reward.isPersistent) {
                a_state.persistentRewardKeys.insert(rewardKey);
            }
        }
    }

    a_state.activationStateKnown = true;
    a_state.isActive = true;
    a_state.canRerollOnNextActivation = true;
    logger::info("[ActivationMigration] Regra '{}' migrada para '{}' com {} rewards identificados.",
        a_rule.name, a_actor->GetName(), a_state.activeRewardKeys.size());
}

void RemoveAppliedActivationRewards(
    RE::Actor* a_actor,
    const Rule& a_rule,
    AppliedRuleState& a_state)
{
    if (!a_actor) return;

    bool hasOutstandingRewards = false;
    if (!a_state.activationStateKnown) {
        RemoveRuleRewards(a_actor, a_rule);
    }
    else if (!a_state.activeRewardKeys.empty()) {
        Rule removalRule = a_rule;
        removalRule.rewardGroups.clear();

        for (const auto& rewardKey : a_state.activeRewardKeys) {
            const RewardGroup* group = nullptr;
            const Reward* reward = nullptr;
            if (!ResolveRewardState(a_rule, rewardKey, group, reward) || !group || !reward || reward->isPersistent) {
                continue;
            }

            auto targetGroup = std::ranges::find_if(removalRule.rewardGroups, [&](const RewardGroup& a_group) {
                return a_group.name == group->name;
            });
            if (targetGroup == removalRule.rewardGroups.end()) {
                RewardGroup selectedGroup = *group;
                selectedGroup.rewards.clear();
                removalRule.rewardGroups.push_back(std::move(selectedGroup));
                targetGroup = std::prev(removalRule.rewardGroups.end());
            }
            targetGroup->rewards.push_back(*reward);
        }

        if (!removalRule.rewardGroups.empty()) {
            RemoveRuleRewards(a_actor, removalRule);
        }

        for (const auto& rewardKey : a_state.activeRewardKeys) {
            const RewardGroup* group = nullptr;
            const Reward* reward = nullptr;
            if (!ResolveRewardState(a_rule, rewardKey, group, reward) || !group || !reward) continue;
            if (HasOutstandingNonPersistentReward(
                    a_actor, *reward, a_rule.id, group->name)) {
                hasOutstandingRewards = true;
                break;
            }
        }
    }

    a_state.activationStateKnown = true;
    a_state.isActive = false;
    a_state.canRerollOnNextActivation = !hasOutstandingRewards;
    if (!hasOutstandingRewards) {
        a_state.activeRewardKeys.clear();
        a_state.appliedGroups.clear();
    }
}

void QueuePersistentOutfitForNormalUse(
    RE::Actor* a_actor,
    const Reward& a_reward,
    bool a_isPlayer,
    std::vector<PendingEquip>& a_equipQueue)
{
    if (!a_actor || a_isPlayer || a_reward.typeReward != "Outfit" ||
        (a_reward.functionOnType != 0 && a_reward.functionOnType != 2)) {
        return;
    }

    auto [plugin, fID] = a_reward.ParseFormID();
    auto outfit = RE::TESForm::LookupByID<RE::BGSOutfit>(fID);
    if (!outfit) return;

    for (auto* form : outfit->outfitItems) {
        auto bound = form ? form->As<RE::TESBoundObject>() : nullptr;
        if (bound && a_actor->GetInventoryCount(bound) > 0) {
            a_equipQueue.push_back({ bound, 0 });
        }
    }
}

void StartRuleActivation(
    RE::Actor* a_actor,
    RE::TESNPC* a_baseNPC,
    const Rule& a_rule,
    AppliedRuleState& a_state,
    bool a_isPlayer,
    std::vector<PendingEquip>& a_equipQueue)
{
    a_state.activationStateKnown = true;
    a_state.isActive = true;
    a_state.canRerollOnNextActivation = true;
    a_state.version = a_rule.version;
    a_state.appliedGroups.clear();
    a_state.activeRewardKeys.clear();

    const auto wonGroups = RuleManager::GetSingleton()->RollForGroups(a_baseNPC, a_rule);
    for (const auto& group : wonGroups) {
        a_state.appliedGroups.push_back(group.name);

        std::vector<const Reward*> selectedRewards;
        if (group.isExclusive) {
            const auto roll = RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f);
            float cumulative = 0.0f;
            for (const auto& reward : group.rewards) {
                cumulative += reward.chanceReward;
                if (roll <= cumulative) {
                    selectedRewards.push_back(std::addressof(reward));
                    break;
                }
            }
        }
        else {
            for (const auto& reward : group.rewards) {
                if (RuleManager::GetSingleton()->GetRandomFloat(0.0f, 100.0f) <= reward.chanceReward) {
                    selectedRewards.push_back(std::addressof(reward));
                }
            }
        }

        for (const auto* reward : selectedRewards) {
            if (!reward) continue;

            const auto rewardKey = BuildRewardStateKey(group.name, *reward);
            a_state.activeRewardKeys.push_back(rewardKey);

            if (reward->isPersistent && a_state.persistentRewardKeys.contains(rewardKey)) {
                RestorePersistentRewardIfNeeded(
                    a_actor, *reward, a_isPlayer, a_equipQueue, a_rule.id, group.name);
                QueuePersistentOutfitForNormalUse(a_actor, *reward, a_isPlayer, a_equipQueue);
                continue;
            }

            ApplyRewardPhysicalTracked(
                a_actor, *reward, a_isPlayer, a_equipQueue, a_rule.id, group.name);
            if (reward->isPersistent) {
                a_state.persistentRewardKeys.insert(rewardKey);
            }
        }
    }
}

enum class RuleEvaluationPhase
{
    kStatic = 0,
    kTag = 1,
    kFactionRank = 2,
    kAbility = 3,
    kInventory = 4
};

RuleEvaluationPhase MaxPhase(RuleEvaluationPhase a_lhs, RuleEvaluationPhase a_rhs)
{
    return static_cast<int>(a_lhs) >= static_cast<int>(a_rhs) ? a_lhs : a_rhs;
}

RuleEvaluationPhase GetFilterEvaluationPhase(const std::string& a_type)
{
    if (a_type == "Keyword" || a_type == "Faction") {
        return RuleEvaluationPhase::kTag;
    }
    if (a_type == "Faction Rank") {
        return RuleEvaluationPhase::kFactionRank;
    }
    if (a_type == "Perk" || a_type == "Spell" || a_type == "Shout") {
        return RuleEvaluationPhase::kAbility;
    }
    if (a_type == "Inventory Item" || a_type == "Inventory Count" ||
        a_type == "Gold" || a_type == "Equipped Item") {
        return RuleEvaluationPhase::kInventory;
    }
    return RuleEvaluationPhase::kStatic;
}

RuleEvaluationPhase GetRuleEvaluationPhase(const Rule& a_rule)
{
    auto phase = RuleEvaluationPhase::kStatic;
    for (const auto& filter : a_rule.targetFilters) {
        phase = MaxPhase(phase, GetFilterEvaluationPhase(filter.type));
    }
    for (const auto& filter : a_rule.blacklistFilters) {
        phase = MaxPhase(phase, GetFilterEvaluationPhase(filter.type));
    }
    return phase;
}

const char* RuleEvaluationPhaseName(RuleEvaluationPhase a_phase)
{
    switch (a_phase) {
    case RuleEvaluationPhase::kStatic:
        return "Static";
    case RuleEvaluationPhase::kTag:
        return "Keyword/Faction";
    case RuleEvaluationPhase::kFactionRank:
        return "Faction Rank";
    case RuleEvaluationPhase::kAbility:
        return "Spell/Perk/Shout";
    case RuleEvaluationPhase::kInventory:
        return "Inventory";
    default:
        return "Unknown";
    }
}

void ApplyRulesToInstance(RE::Actor* a_actor, int a_forcedLevel) {
    if (!a_actor) return;

    // --- LOG DE INÍCIO ---
    std::string actorName = a_actor->GetName();
    RE::FormID actorID = a_actor->GetFormID();

    {
        std::lock_guard<std::mutex> lock(g_processingMutex);
        if (g_actorsInProcess.contains(actorID)) {
            // Se já estiver processando este ator, ignora a nova chamada para evitar conflitos
            // logger::debug("[ApplyRules] Ignorando chamada duplicada para {:08X} (em processamento)", actorID);
            return;
        }
        g_actorsInProcess.insert(actorID);
    }

    // Garantir que o ID seja removido do conjunto ao sair da função (RAII)
    struct ProcessCleaner {
        RE::FormID id;
        ~ProcessCleaner() {
            std::lock_guard<std::mutex> lock(g_processingMutex);
            g_actorsInProcess.erase(id);
        }
    } cleaner{ actorID };
    //logger::debug("[ApplyRules] INICIANDO processamento para {} (ID: {:08X})", actorName, actorID);

    if (a_actor->IsDead()) {
        //logger::info("[ApplyRules] FINALIZADO: Ator está morto.");
        return;
    }

    bool isPlayer = (actorID == 0x00000014) || a_actor->IsPlayer();
    auto baseNPC = a_actor->GetActorBase();
    if (!baseNPC) {
        logger::debug("[ApplyRules] FINALIZADO: BaseNPC não encontrado.");
        return;
    }

    auto& context = SaveStateManager::GetSingleton()->GetCurrentContext();
    if (!context.isValid) {
        logger::debug("[ApplyRules] FINALIZADO: Contexto de save inválido.");
        return;
    }

    auto& session = SaveStateManager::GetSingleton()->GetSessionData();
    std::string fileNameStr = "Dynamic";
    if (auto file = GetSourceFileByFormID(baseNPC)) {
        fileNameStr = file->GetFilename();
    }
    else if (baseNPC->IsDynamicForm()) {
        fileNameStr = "Dynamic";
    }

    std::string npcKey = fileNameStr + "|" + FormatLocalFormID(actorID, fileNameStr);

    // --- LÓGICA DE REMOÇÃO DE REGRAS INVÁLIDAS ---
    if (session.npcRuleVersions.contains(npcKey)) {
        auto& appliedRulesMap = session.npcRuleVersions[npcKey];
        auto& allRules = RuleManager::GetSingleton()->GetRules();

        for (auto it = appliedRulesMap.begin(); it != appliedRulesMap.end();) {
            const std::string& ruleID = it->first;
            auto& state = it->second;
            auto ruleDefIt = std::find_if(allRules.begin(), allRules.end(), [&](const Rule& r) { return r.id == ruleID; });

            bool shouldRemove = false;
            if (ruleDefIt == allRules.end()) {
                shouldRemove = true;
            }
            else {
                const Rule& rule = *ruleDefIt;
                if (!rule.isEnabled ||
                    !IsNPCMatchingTargets(baseNPC, rule, false, a_actor) ||
                    IsNPCMatchingTargets(baseNPC, rule, true, a_actor)) {
                    shouldRemove = true;
                }
            }

            if (shouldRemove) {
                if (!state.activationStateKnown || state.isActive) {
                    const Rule* appliedRule = RuleManager::GetSingleton()->GetRuleVersion(ruleID, state.version);
                    if (!appliedRule && ruleDefIt != allRules.end()) {
                        appliedRule = std::addressof(*ruleDefIt);
                    }

                    if (appliedRule) {
                        RemoveAppliedActivationRewards(a_actor, *appliedRule, state);
                    }
                    else {
                        state.activationStateKnown = true;
                        state.isActive = false;
                        state.canRerollOnNextActivation = true;
                        state.activeRewardKeys.clear();
                        state.appliedGroups.clear();
                    }
                }
                ++it;
            }
            else {
                ++it;
            }
        }
    }

    const auto& affectedDB = RuleManager::GetSingleton()->GetAffectedNPCsDatabase();

    // --- COLETA DE REGRAS ---
    std::vector<std::string> rulesToProcess;
    auto addRulesFromID = [&](RE::FormID a_id) {
        if (affectedDB.contains(a_id)) {
            for (const auto& id : affectedDB.at(a_id).ruleIDs) {
                if (std::find(rulesToProcess.begin(), rulesToProcess.end(), id) == rulesToProcess.end()) {
                    rulesToProcess.push_back(id);
                }
            }
        }
        };

    addRulesFromID(actorID);
    addRulesFromID(baseNPC->GetFormID());
    auto templateBase = a_actor->GetTemplateBase();
    if (templateBase) addRulesFromID(templateBase->GetFormID());

    auto& allRulesForTags = RuleManager::GetSingleton()->GetRules();
    for (const auto& rule : allRulesForTags) {
        if (std::find(rulesToProcess.begin(), rulesToProcess.end(), rule.id) == rulesToProcess.end()) {
            rulesToProcess.push_back(rule.id);
        }
    }

    if (rulesToProcess.empty()) {
        if (!isPlayer && !IsActorSleeping(a_actor)) {
            EquipBestInventoryItems(a_actor);
        }
        logger::debug("[ApplyRules] FINALIZADO: Nenhuma regra aplicável para {}", actorName);
        return;
    }

    // --- PROCESSAMENTO E APLICAÇÃO ---
    std::vector<PendingEquip> equipQueue;
    constexpr std::array rulePhases{
        RuleEvaluationPhase::kStatic,
        RuleEvaluationPhase::kTag,
        RuleEvaluationPhase::kFactionRank,
        RuleEvaluationPhase::kAbility,
        RuleEvaluationPhase::kInventory
    };

    for (const auto phase : rulePhases) {
        logger::debug("[RulePhase] Processando fase '{}' para '{}'", RuleEvaluationPhaseName(phase), actorName);

        std::vector<std::string> phaseRuleIDs;
        for (const auto& ruleID : rulesToProcess) {
            auto& allRules = RuleManager::GetSingleton()->GetRules();
            auto ruleIt = std::find_if(allRules.begin(), allRules.end(), [&](const Rule& r) { return r.id == ruleID; });
            if (ruleIt == allRules.end()) continue;

            const Rule& currentRule = *ruleIt;
            if (!currentRule.isEnabled) continue;
            if (GetRuleEvaluationPhase(currentRule) != phase) continue;
            if (!IsNPCMatchingTargets(baseNPC, currentRule, false, a_actor) ||
                IsNPCMatchingTargets(baseNPC, currentRule, true, a_actor)) {
                continue;
            }

            int level = (a_forcedLevel != -1) ? a_forcedLevel : a_actor->GetLevel();
            if (level < currentRule.level) continue;

            phaseRuleIDs.push_back(ruleID);
        }

        for (const auto& ruleID : phaseRuleIDs) {
            auto& allRules = RuleManager::GetSingleton()->GetRules();
            auto ruleIt = std::find_if(allRules.begin(), allRules.end(), [&](const Rule& r) { return r.id == ruleID; });
            if (ruleIt == allRules.end()) continue;

            const Rule& currentRule = *ruleIt;
            if (!currentRule.isEnabled) continue;

            int level = (a_forcedLevel != -1) ? a_forcedLevel : a_actor->GetLevel();
            if (level < currentRule.level) continue;
            AppliedRuleState& state = session.npcRuleVersions[npcKey][ruleID];
            int oldVersion = state.version;

            const Rule* previousRule = oldVersion > 0 ?
                RuleManager::GetSingleton()->GetRuleVersion(ruleID, oldVersion) :
                nullptr;

            if (!state.activationStateKnown && oldVersion > 0) {
                MigrateLegacyActivationState(
                    a_actor, previousRule ? *previousRule : currentRule, state);
            }

            if (oldVersion > 0 && oldVersion != currentRule.version) {
                if (state.isActive) {
                    RemoveAppliedActivationRewards(
                        a_actor, previousRule ? *previousRule : currentRule, state);
                }
                state.isActive = false;
                state.canRerollOnNextActivation = true;
                state.activeRewardKeys.clear();
                state.appliedGroups.clear();
            }

            if (!state.isActive) {
                if (state.canRerollOnNextActivation) {
                    logger::debug("[Activation] Nova ativação da regra '{}' para '{}'.",
                        currentRule.name, actorName);
                    StartRuleActivation(
                        a_actor, baseNPC, currentRule, state, isPlayer, equipQueue);
                }
                else {
                    // Algum reward não persistente saiu do ator e continua
                    // documentado pelo ledger. Reativamos sem novo sorteio
                    // para impedir duplicação/farming.
                    state.isActive = true;
                    state.version = currentRule.version;
                    logger::debug(
                        "[Activation] Regra '{}' reativada para '{}' sem reroll; rewards anteriores continuam documentados.",
                        currentRule.name, actorName);
                }
            }
            else {
                // A regra continua na mesma ativação: não há reroll. Apenas
                // rewards persistentes podem ser reconciliados pelo ledger.
                for (const auto& rewardKey : state.activeRewardKeys) {
                    const RewardGroup* group = nullptr;
                    const Reward* reward = nullptr;
                    if (!ResolveRewardState(currentRule, rewardKey, group, reward) ||
                        !group || !reward || !reward->isPersistent) {
                        continue;
                    }

                    RestorePersistentRewardIfNeeded(
                        a_actor, *reward, isPlayer, equipQueue, ruleID, group->name);
                    QueuePersistentOutfitForNormalUse(
                        a_actor, *reward, isPlayer, equipQueue);
                }
                state.version = currentRule.version;
            }


        // Equipagem final
        if (!equipQueue.empty() && !isPlayer) {
            logger::debug("[EquipManager] Iniciando processamento da fila para {}. Itens na fila: {}", actorName, equipQueue.size());
            auto equipManager = RE::ActorEquipManager::GetSingleton();
            if (equipManager) {
                // 1. Ordena por prioridade
                std::sort(equipQueue.begin(), equipQueue.end(), [](const PendingEquip& a, const PendingEquip& b) {
                    return a.priority < b.priority;
                    });

                for (auto& entry : equipQueue) {
                    if (entry.object->IsArmor()) {
                        // Armaduras são selecionadas em conjunto por
                        // EquipBestInventoryItems para respeitar slots,
                        // precedência das rules e ArmorAddons compatíveis.
                        continue;
                    }

                    logger::debug("  [EquipManager] Comandando motor: Equipar '{}' em {}", entry.object->GetName(), actorName);
                    // Usando force=true (penúltimo parâmetro) para garantir que NPCs como Athis/Njada ignorem o Outfit padrão
                    equipManager->EquipObject(a_actor, entry.object, nullptr, 1, nullptr, false, false, false, true);
                }
            }
        }
        equipQueue.clear();

        if (IsActorSleeping(a_actor)) {
            auto sleepOutfit = GetAppliedSleepOutfit(a_actor);
            if (sleepOutfit) {
                // Verifica se já está vestindo para evitar equipagem redundante
                bool alreadyWearing = false;
                auto inventory = a_actor->GetInventory();
                for (auto* form : sleepOutfit->outfitItems) {
                    auto item = form ? form->As<RE::TESBoundObject>() : nullptr;
                    if (item && inventory.contains(item) && inventory[item].second->IsWorn()) {
                        alreadyWearing = true;
                        break;
                    }
                }
                if (!alreadyWearing) {
                    logger::info("[SleepSystem] NPC {} carregado em estado de sono. Aplicando traje.", a_actor->GetName());
                    ManageSleepOutfitState(a_actor, true);
                }
            }
        }
        logger::debug(" [Regras aplicadas] {} (Save #{}) - Regra: '{}', Versão: {}, Grupos Aplicados: {}",
			actorName, session.saveNumber, currentRule.name, state.version, fmt::join(state.appliedGroups, ", "));
        // --- LOG DE FINALIZAÇÃO ---
        logger::debug("[ApplyRules] FINALIZADO com sucesso para {} ({:08X})", actorName, actorID);
    }
    }

    // Também recompõe quando nenhuma regra da fase correspondeu (por
    // exemplo, após sair de uma Cell/Location e remover o outfit anterior).
    if (!isPlayer && !IsActorSleeping(a_actor)) {
        EquipBestInventoryItems(a_actor);
    }
}
