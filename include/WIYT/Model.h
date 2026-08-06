#pragma once

#include "Rule.h"

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace WIYT
{
    enum class ProgressSource : std::uint8_t
    {
        kVanillaStatistic = 0,
        kEventCounter = 1,
        kGlobal = 2,
        kGraphVariable = 3
    };

    enum class ActivityType : std::uint8_t
    {
        kActorKilled = 0,
        kActorDefeated = 1,
        kItemHarvested = 2,
        kItemAcquired = 3,
        kDamageDealt = 4,
        kSpellDamageDealt = 5,
        kItemCrafted = 6,
        kLocationDiscovered = 7,
        kQuestCompleted = 8,
        kGoldEarned = 9,
        kGoldSpent = 10,
        kCustom = 11
    };

    enum class TrackingMode : std::uint8_t
    {
        kLifetimeTotal = 0,
        kSinceActivated = 1,
        kHighestReached = 2
    };

    enum class Aggregation : std::uint8_t
    {
        kCount = 0,
        kSum = 1,
        kUniqueCount = 2,
        kHighestValue = 3
    };

    enum class FilterScope : std::uint8_t
    {
        kCreditedActor = 0,
        kTargetActor = 1,
        kSourceForm = 2,
        kEnvironment = 3
    };

    enum class EventProvenance : std::uint8_t
    {
        kGameplay = 0,
        kWIYTReward = 1,
        kExternalMod = 2
    };

    struct Requirement
    {
        std::string id;
        std::string name = "New Requirement";
        ProgressSource source = ProgressSource::kEventCounter;
        ActivityType activity = ActivityType::kActorKilled;
        TrackingMode trackingMode = TrackingMode::kLifetimeTotal;
        Aggregation aggregation = Aggregation::kCount;
        float targetAmount = 1.0f;
        std::string statisticName;
        std::string referenceFormID;
        std::string referenceEditorID;
        std::string graphVariableName;
        int graphVariableType = 2;
        bool filtersRequireAll = true;
        std::vector<BlacklistFilter> creditedActorFilters;
        std::vector<BlacklistFilter> targetActorFilters;
        std::vector<BlacklistFilter> sourceFormFilters;
        std::vector<BlacklistFilter> environmentFilters;
    };

    struct TitleDefinition
    {
        std::string id;
        std::string packageID = "wiyt.local-titles";
        std::string name = "New Title";
        std::string description;
        std::string publicGlobalEditorID;
        bool enabled = true;
        int version = 0;
        std::vector<Requirement> requirements;
        std::vector<RewardGroup> rewardGroups;
        mutable std::string lastSavedHash;

        [[nodiscard]] std::string CalculateHash() const;
        [[nodiscard]] bool IsModified() const
        {
            return lastSavedHash != CalculateHash();
        }
    };

    struct Package
    {
        std::string id;
        std::string displayName;
        bool enabled = true;
        std::filesystem::path path;
        int schemaVersion = 1;
    };

    struct ProgressEvent
    {
        ActivityType activity = ActivityType::kCustom;
        RE::Actor* creditedActor = nullptr;
        RE::Actor* sourceActor = nullptr;
        RE::Actor* targetActor = nullptr;
        RE::TESForm* sourceForm = nullptr;
        RE::TESObjectCELL* cell = nullptr;
        RE::BGSLocation* location = nullptr;
        float amount = 1.0f;
        std::uint64_t uniqueKey = 0;
        EventProvenance provenance = EventProvenance::kGameplay;
    };

    [[nodiscard]] std::string GenerateUUID();
    [[nodiscard]] std::string SanitizePublicGlobalName(
        std::string_view a_titleName);
    [[nodiscard]] std::string RequirementFingerprint(
        const Requirement& a_requirement);
    [[nodiscard]] const char* ToString(ProgressSource a_value);
    [[nodiscard]] const char* ToString(ActivityType a_value);
    [[nodiscard]] const char* ToString(TrackingMode a_value);
    [[nodiscard]] const char* ToString(Aggregation a_value);
}
