#pragma once

#include "WIYT/Model.h"

#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace WIYT
{
    struct RequirementProgress
    {
        std::string definitionFingerprint;
        float value = 0.0f;
        float baseline = 0.0f;
        float highest = 0.0f;
        bool baselineSet = false;
        std::set<std::uint64_t> uniqueKeys;
    };

    struct RewardKey
    {
        std::uint32_t group = 0;
        std::uint32_t reward = 0;

        auto operator<=>(const RewardKey&) const = default;
    };

    struct TitleProgress
    {
        std::map<std::string, RequirementProgress, std::less<>>
            requirements;
        float overallProgress = 0.0f;
        bool completed = false;
        bool rewardsSelected = false;
        bool rewarded = false;
        std::vector<RewardKey> selectedRewards;
        std::set<RewardKey> deliveredRewards;
    };

    struct ProgressChange
    {
        bool changed = false;
        bool newlyCompleted = false;
        float previousOverall = 0.0f;
        float overall = 0.0f;
    };

    class State
    {
    public:
        static State* GetSingleton();

        void ReconcileDefinitions(
            const std::vector<TitleDefinition>& a_titles);
        ProgressChange AddEventProgress(
            const TitleDefinition& a_title,
            const Requirement& a_requirement,
            float a_amount,
            std::uint64_t a_uniqueKey);
        ProgressChange SetAbsoluteProgress(
            const TitleDefinition& a_title,
            const Requirement& a_requirement,
            float a_value);

        std::optional<TitleProgress> GetTitleProgress(
            std::string_view a_titleID) const;
        std::vector<std::pair<std::string, TitleProgress>>
            GetSnapshot() const;
        void SetSelectedRewards(
            std::string_view a_titleID,
            std::vector<RewardKey> a_rewards);
        bool IsRewardDelivered(
            std::string_view a_titleID,
            RewardKey a_reward) const;
        void MarkRewardDelivered(
            std::string_view a_titleID,
            RewardKey a_reward);
        void MarkRewarded(std::string_view a_titleID);
        void ResetTitle(std::string_view a_titleID);
        void Revert();

        static void InstallSerialization();
        bool Save(SKSE::SerializationInterface* a_interface) const;
        bool Load(
            SKSE::SerializationInterface* a_interface,
            std::uint32_t a_version);

    private:
        ProgressChange RecomputeLocked(
            const TitleDefinition& a_title,
            float a_previousOverall,
            bool a_changed);
        RequirementProgress& GetRequirementLocked(
            const TitleDefinition& a_title,
            const Requirement& a_requirement);

        mutable std::mutex _lock;
        std::unordered_map<std::string, TitleProgress> _titles;
    };
}
