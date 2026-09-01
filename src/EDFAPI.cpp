#include "EDFAPI.h"

#include "Rule.h"
#include "RulePackageStore.h"
#include "SaveState.h"
#include "logger.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace EDF::API
{
    namespace
    {
        struct CallbackTarget
        {
            Callback callback = nullptr;
            void* userData = nullptr;
        };

        std::atomic_bool g_ready{ false };

        void CopyText(char* destination, const std::size_t capacity,
            const std::string_view value)
        {
            if (!destination || capacity == 0) return;
            const auto count = std::min(capacity - 1, value.size());
            std::memcpy(destination, value.data(), count);
            destination[count] = '\0';
        }

        void Complete(
            const CallbackTarget target,
            Result result,
            const std::string& json = {})
        {
            if (!target.callback) return;
            result.ruleJson = json.empty() ? nullptr : json.c_str();
            result.ruleJsonLength =
                static_cast<std::uint32_t>(json.size());
            try {
                target.callback(std::addressof(result), target.userData);
            }
            catch (...) {
                logger::error("[EDF API] Consumer callback threw an exception.");
            }
        }

        Result MakeResult(const Operation operation)
        {
            Result result;
            result.operation = operation;
            return result;
        }

        bool ValidateRequester(
            const std::string_view requester,
            std::string& error)
        {
            if (requester.empty() || requester.size() > 64) {
                error = "requester must contain 1-64 characters";
                return false;
            }
            if (std::ranges::any_of(requester, [](const unsigned char ch) {
                    return ch < 0x20 || ch == '/' || ch == '\\';
                })) {
                error = "requester contains invalid characters";
                return false;
            }
            return true;
        }

        std::string PackageDisplayName(const std::string_view requester)
        {
            return std::format("EDF API - {}", requester);
        }

        std::string PackageID(const std::string_view requester)
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (const auto ch : requester) {
                hash ^= static_cast<unsigned char>(ch);
                hash *= 1099511628211ULL;
            }
            return std::format("edf.api.{:016x}", hash);
        }

        std::optional<std::string> FindRequesterPackage(
            const std::string_view requester)
        {
            const auto displayName = PackageDisplayName(requester);
            const auto packageID = PackageID(requester);
            for (const auto& package :
                 RuleManager::GetSingleton()->GetPackages()) {
                if (package.id == packageID &&
                    package.displayName == displayName) {
                    return package.id;
                }
            }
            return std::nullopt;
        }

        std::optional<std::string> EnsureRequesterPackage(
            const std::string_view requester)
        {
            if (auto package = FindRequesterPackage(requester)) {
                return package;
            }
            return RuleManager::GetSingleton()->CreatePackage(
                PackageDisplayName(requester), PackageID(requester));
        }

        bool OwnsRule(
            const std::string_view requester,
            const Rule& rule)
        {
            const auto package = FindRequesterPackage(requester);
            return package && *package == rule.packageID;
        }

        bool Queue(std::function<void()> work)
        {
            auto* tasks = SKSE::GetTaskInterface();
            if (!g_ready.load(std::memory_order_acquire) || !tasks) {
                return false;
            }
            tasks->AddTask(std::move(work));
            return true;
        }

        class Service final : public IEDFRuleAPI
        {
        public:
            std::uint32_t GetVersion() const noexcept override
            {
                return kInterfaceVersion;
            }

            bool IsReady() const noexcept override
            {
                return g_ready.load(std::memory_order_acquire);
            }

            bool QueueCreateRule(
                const CreateRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(CreateRuleRequest) ||
                    !request->requester || !request->ruleJson || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string json(request->ruleJson);
                return Queue([requester, json,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kCreate);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    Rule definition;
                    if (!ParseRuleDefinition(json, definition, error)) {
                        result.status = error.starts_with("invalid rule JSON") ?
                            Status::kInvalidJson : Status::kValidationFailed;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    const auto package = EnsureRequesterPackage(requester);
                    if (!package) {
                        result.status = Status::kPersistenceFailed;
                        CopyText(result.error, sizeof(result.error),
                            "could not create requester package");
                        Complete(target, result);
                        return;
                    }
                    auto* manager = RuleManager::GetSingleton();
                    auto& created = manager->CreateRule(*package);
                    const auto id = created.id;
                    const auto packageID = created.packageID;
                    created = std::move(definition);
                    created.id = id;
                    created.packageID = packageID;
                    created.version = 0;
                    created.lastSavedHash.clear();
                    if (!manager->SaveRule(id)) {
                        manager->DeleteRule(id);
                        result.status = Status::kPersistenceFailed;
                        CopyText(result.error, sizeof(result.error),
                            "could not persist rule");
                        Complete(target, result);
                        return;
                    }
                    const auto* saved = manager->FindRule(id);
                    result.status = Status::kSuccess;
                    result.version = saved ? saved->version : 1;
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    const auto output = saved ?
                        SerializeRuleDefinition(*saved) : std::string{};
                    ScheduleAllLoadedRuleEvaluations();
                    Complete(target, result, output);
                });
            }

            bool QueueUpdateRule(
                const UpdateRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(UpdateRuleRequest) ||
                    !request->requester || !request->ruleID ||
                    !request->ruleJson || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string id(request->ruleID);
                const std::string json(request->ruleJson);
                const auto expected = request->expectedVersion;
                return Queue([requester, id, json, expected,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kUpdate);
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    auto* manager = RuleManager::GetSingleton();
                    auto* current = manager->FindRule(id);
                    if (!current) {
                        result.status = Status::kNotFound;
                        Complete(target, result);
                        return;
                    }
                    if (!OwnsRule(requester, *current)) {
                        result.status = Status::kNotOwner;
                        Complete(target, result);
                        return;
                    }
                    if (expected != kAnyVersion &&
                        expected != current->version) {
                        result.status = Status::kVersionConflict;
                        result.version = current->version;
                        Complete(target, result);
                        return;
                    }
                    Rule replacement;
                    if (!ParseRuleDefinition(json, replacement, error)) {
                        result.status = error.starts_with("invalid rule JSON") ?
                            Status::kInvalidJson : Status::kValidationFailed;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    const Rule backup = *current;
                    replacement.id = backup.id;
                    replacement.packageID = backup.packageID;
                    replacement.version = backup.version;
                    replacement.lastSavedHash.clear();
                    *current = std::move(replacement);
                    if (!manager->SaveRule(id)) {
                        if (auto* restore = manager->FindRule(id)) {
                            *restore = backup;
                            manager->RebuildDependencyIndex();
                        }
                        result.status = Status::kPersistenceFailed;
                        Complete(target, result);
                        return;
                    }
                    current = manager->FindRule(id);
                    result.status = Status::kSuccess;
                    result.version = current ? current->version : 0;
                    const auto output = current ?
                        SerializeRuleDefinition(*current) : std::string{};
                    ScheduleAllLoadedRuleEvaluations();
                    Complete(target, result, output);
                });
            }

            bool QueueDeleteRule(
                const DeleteRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(DeleteRuleRequest) ||
                    !request->requester || !request->ruleID || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string id(request->ruleID);
                const auto expected = request->expectedVersion;
                return Queue([requester, id, expected,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kDelete);
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    auto* manager = RuleManager::GetSingleton();
                    const auto* current = manager->FindRule(id);
                    if (!current) {
                        result.status = Status::kNotFound;
                    }
                    else if (!OwnsRule(requester, *current)) {
                        result.status = Status::kNotOwner;
                    }
                    else if (expected != kAnyVersion &&
                        expected != current->version) {
                        result.status = Status::kVersionConflict;
                        result.version = current->version;
                    }
                    else if (!manager->DeleteRule(id)) {
                        result.status = Status::kPersistenceFailed;
                    }
                    else {
                        result.status = Status::kSuccess;
                    }
                    Complete(target, result);
                });
            }

            bool QueueLookupRule(
                const LookupRuleRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                if (!request ||
                    request->structSize < sizeof(LookupRuleRequest) ||
                    !request->requester || !request->ruleID || !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string id(request->ruleID);
                return Queue([requester, id,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(Operation::kLookup);
                    CopyText(result.ruleID, sizeof(result.ruleID), id);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    const auto* rule =
                        RuleManager::GetSingleton()->FindRule(id);
                    if (!rule) {
                        result.status = Status::kNotFound;
                        Complete(target, result);
                        return;
                    }
                    result.status = Status::kSuccess;
                    result.version = rule->version;
                    const auto output = SerializeRuleDefinition(*rule);
                    Complete(target, result, output);
                });
            }

            bool QueueReevaluateActor(
                const ActorRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                return QueueActor(
                    request, callback, userData, false);
            }

            bool QueueResetActor(
                const ActorRequest* request,
                const Callback callback,
                void* userData) noexcept override
            {
                return QueueActor(
                    request, callback, userData, true);
            }

        private:
            bool QueueActor(
                const ActorRequest* request,
                const Callback callback,
                void* userData,
                const bool reset) noexcept
            {
                if (!request ||
                    request->structSize < sizeof(ActorRequest) ||
                    !request->requester || request->actorFormID == 0 ||
                    !callback) {
                    return false;
                }
                const std::string requester(request->requester);
                const std::string ruleID =
                    request->ruleID ? request->ruleID : "";
                const auto actorID = request->actorFormID;
                return Queue([requester, ruleID, actorID, reset,
                              target = CallbackTarget{ callback, userData }] {
                    auto result = MakeResult(reset ?
                        Operation::kResetActor :
                        Operation::kReevaluateActor);
                    result.actorFormID = actorID;
                    CopyText(result.ruleID, sizeof(result.ruleID), ruleID);
                    std::string error;
                    if (!ValidateRequester(requester, error)) {
                        result.status = Status::kInvalidArgument;
                        CopyText(result.error, sizeof(result.error), error);
                        Complete(target, result);
                        return;
                    }
                    auto* actor =
                        RE::TESForm::LookupByID<RE::Actor>(actorID);
                    if (!actor || actor->IsDead() ||
                        !actor->GetParentCell()) {
                        result.status = Status::kActorUnavailable;
                        Complete(target, result);
                        return;
                    }
                    if (reset) {
                        result.status = ResetRuleActivationForActor(
                            actor, ruleID) ? Status::kSuccess :
                            Status::kNotFound;
                    }
                    else {
                        ApplyRulesToInstance(
                            actor, RuleEvaluationDelta::Full());
                        result.status = Status::kSuccess;
                    }
                    Complete(target, result);
                });
            }
        };
    }

    void SetReady(const bool ready) noexcept
    {
        g_ready.store(ready, std::memory_order_release);
    }

    IEDFRuleAPI* GetService() noexcept
    {
        static Service service;
        return std::addressof(service);
    }
}

extern "C" __declspec(dllexport) void* GetEDFRuleAPI()
{
    return EDF::API::GetService();
}
