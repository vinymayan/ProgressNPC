#include "WIYT/State.h"

#include "WIYT/DFGBridge.h"
#include "WIYT/Store.h"

#include <cmath>

namespace WIYT
{
    namespace
    {
        constexpr std::uint32_t kSerializationID =
            0x54594957;  // WIYT
        constexpr std::uint32_t kStateRecord =
            0x47525054;  // TPRG
        constexpr std::uint32_t kSerializationVersion = 1;
        constexpr std::uint32_t kMaximumTitles = 100000;
        constexpr std::uint32_t kMaximumRequirements = 100000;
        constexpr std::uint32_t kMaximumUniqueKeys = 1000000;

        bool WriteString(
            SKSE::SerializationInterface* a_interface,
            const std::string_view a_value)
        {
            const auto size =
                static_cast<std::uint32_t>(a_value.size());
            return a_interface->WriteRecordData(size) &&
                (size == 0 ||
                    a_interface->WriteRecordData(
                        a_value.data(),
                        size));
        }

        bool ReadString(
            SKSE::SerializationInterface* a_interface,
            std::string& a_value)
        {
            std::uint32_t size = 0;
            if (!a_interface->ReadRecordData(size) ||
                size > 1024 * 1024) {
                return false;
            }
            a_value.resize(size);
            return size == 0 ||
                a_interface->ReadRecordData(
                    a_value.data(),
                    size);
        }
    }

    State* State::GetSingleton()
    {
        static State singleton;
        return std::addressof(singleton);
    }

    RequirementProgress& State::GetRequirementLocked(
        const TitleDefinition& a_title,
        const Requirement& a_requirement)
    {
        auto& title = _titles[a_title.id];
        auto& progress = title.requirements[a_requirement.id];
        const auto fingerprint =
            RequirementFingerprint(a_requirement);
        if (progress.definitionFingerprint != fingerprint &&
            !title.completed) {
            if (progress.definitionFingerprint ==
                LegacyRequirementFingerprintV1(a_requirement)) {
                progress.definitionFingerprint = fingerprint;
            }
            else {
                progress = {};
                progress.definitionFingerprint = fingerprint;
            }
        }
        else if (progress.definitionFingerprint.empty()) {
            progress.definitionFingerprint = fingerprint;
        }
        return progress;
    }

    void State::ReconcileDefinitions(
        const std::vector<TitleDefinition>& a_titles)
    {
        std::scoped_lock lock(_lock);
        for (const auto& title : a_titles) {
            auto& titleProgress = _titles[title.id];
            for (const auto& requirement : title.requirements) {
                auto& progress =
                    GetRequirementLocked(title, requirement);
                progress.prerequisitesMet =
                    requirement.playerPrerequisiteFilters.empty();
            }
            RecomputeLocked(
                title,
                titleProgress.overallProgress,
                false);
        }
    }

    ProgressChange State::AddEventProgress(
        const TitleDefinition& a_title,
        const Requirement& a_requirement,
        const float a_amount,
        const std::uint64_t a_uniqueKey)
    {
        if (!std::isfinite(a_amount) || a_amount < 0.0f) {
            return {};
        }
        std::scoped_lock lock(_lock);
        auto& title = _titles[a_title.id];
        if (title.completed) {
            return {
                false,
                false,
                title.overallProgress,
                title.overallProgress
            };
        }
        auto& progress =
            GetRequirementLocked(a_title, a_requirement);
        const auto previous = progress.value;
        switch (a_requirement.aggregation) {
        case Aggregation::kCount:
            progress.value += 1.0f;
            break;
        case Aggregation::kSum:
            progress.value += a_amount;
            break;
        case Aggregation::kUniqueCount:
            if (a_uniqueKey == 0 ||
                progress.uniqueKeys.size() >= kMaximumUniqueKeys ||
                !progress.uniqueKeys.emplace(a_uniqueKey).second) {
                return {
                    false,
                    false,
                    title.overallProgress,
                    title.overallProgress
                };
            }
            progress.value =
                static_cast<float>(progress.uniqueKeys.size());
            break;
        case Aggregation::kHighestValue:
            progress.value =
                std::max(progress.value, a_amount);
            break;
        }
        progress.highest =
            std::max(progress.highest, progress.value);
        return RecomputeLocked(
            a_title,
            title.overallProgress,
            std::abs(progress.value - previous) > 0.0001f);
    }

    ProgressChange State::SetAbsoluteProgress(
        const TitleDefinition& a_title,
        const Requirement& a_requirement,
        const float a_value)
    {
        if (!std::isfinite(a_value)) {
            return {};
        }
        std::scoped_lock lock(_lock);
        auto& title = _titles[a_title.id];
        if (title.completed) {
            return {
                false,
                false,
                title.overallProgress,
                title.overallProgress
            };
        }
        auto& progress =
            GetRequirementLocked(a_title, a_requirement);
        const auto previous = progress.value;
        const auto current = std::max(0.0f, a_value);
        switch (a_requirement.trackingMode) {
        case TrackingMode::kLifetimeTotal:
            progress.value = current;
            break;
        case TrackingMode::kSinceActivated:
            if (!progress.baselineSet) {
                progress.baseline = current;
                progress.baselineSet = true;
            }
            progress.value =
                std::max(0.0f, current - progress.baseline);
            break;
        case TrackingMode::kHighestReached:
            progress.highest =
                std::max(progress.highest, current);
            progress.value = progress.highest;
            break;
        }
        progress.highest =
            std::max(progress.highest, progress.value);
        return RecomputeLocked(
            a_title,
            title.overallProgress,
            std::abs(progress.value - previous) > 0.0001f);
    }

    ProgressChange State::SetPrerequisiteState(
        const TitleDefinition& a_title,
        const Requirement& a_requirement,
        const bool a_met)
    {
        std::scoped_lock lock(_lock);
        auto& title = _titles[a_title.id];
        if (title.completed) {
            return {
                false,
                false,
                title.overallProgress,
                title.overallProgress
            };
        }
        auto& progress =
            GetRequirementLocked(a_title, a_requirement);
        const auto changed =
            progress.prerequisitesMet != a_met;
        progress.prerequisitesMet = a_met;
        return RecomputeLocked(
            a_title,
            title.overallProgress,
            changed);
    }

    ProgressChange State::RecomputeLocked(
        const TitleDefinition& a_title,
        const float a_previousOverall,
        const bool a_changed)
    {
        auto& title = _titles[a_title.id];
        if (title.completed) {
            for (const auto& requirement : a_title.requirements) {
                auto& progress = title.requirements[requirement.id];
                progress.progressTargetReached = true;
                progress.waitingForPrerequisites = false;
            }
            title.rawOverallProgress = 1.0f;
            title.overallProgress = 1.0f;
            return {
                a_changed,
                false,
                a_previousOverall,
                1.0f
            };
        }
        if (a_title.requirements.empty()) {
            title.rawOverallProgress = 0.0f;
            title.overallProgress = 0.0f;
            return {
                a_changed,
                false,
                a_previousOverall,
                0.0f
            };
        }
        float total = 0.0f;
        bool complete = true;
        for (const auto& requirement : a_title.requirements) {
            const auto found =
                title.requirements.find(requirement.id);
            const auto value =
                found != title.requirements.end() ?
                found->second.value :
                0.0f;
            const auto ratio = std::clamp(
                value / std::max(0.0001f, requirement.targetAmount),
                0.0f,
                1.0f);
            auto& requirementProgress =
                title.requirements[requirement.id];
            requirementProgress.progressTargetReached =
                ratio >= 1.0f;
            requirementProgress.waitingForPrerequisites =
                requirementProgress.progressTargetReached &&
                requirement.prerequisiteMode ==
                    PrerequisiteMode::kRequiredToComplete &&
                !requirementProgress.prerequisitesMet;
            total += ratio;
            complete = complete &&
                requirementProgress.progressTargetReached &&
                !requirementProgress.waitingForPrerequisites;
        }
        title.rawOverallProgress =
            total / static_cast<float>(a_title.requirements.size());
        if (complete) {
            title.completed = true;
            title.rawOverallProgress = 1.0f;
            title.overallProgress = 1.0f;
        }
        else {
            // A public title Global reaches exactly 1.0 only when every
            // completion prerequisite is satisfied.
            title.overallProgress = std::min(
                title.rawOverallProgress,
                0.999f);
        }
        return {
            a_changed ||
                std::abs(
                    title.overallProgress -
                    a_previousOverall) >
                    0.0001f,
            complete,
            a_previousOverall,
            title.overallProgress
        };
    }

    std::optional<TitleProgress> State::GetTitleProgress(
        const std::string_view a_titleID) const
    {
        std::scoped_lock lock(_lock);
        const auto found =
            _titles.find(std::string(a_titleID));
        return found != _titles.end() ?
            std::optional<TitleProgress>(found->second) :
            std::nullopt;
    }

    std::vector<std::pair<std::string, TitleProgress>>
    State::GetSnapshot() const
    {
        std::scoped_lock lock(_lock);
        std::vector<std::pair<std::string, TitleProgress>> result;
        result.reserve(_titles.size());
        for (const auto& entry : _titles) {
            result.push_back(entry);
        }
        std::ranges::sort(result, {}, &decltype(result)::value_type::first);
        return result;
    }

    void State::SetSelectedRewards(
        const std::string_view a_titleID,
        std::vector<RewardKey> a_rewards)
    {
        std::scoped_lock lock(_lock);
        auto& progress = _titles[std::string(a_titleID)];
        if (!progress.rewardsSelected) {
            progress.selectedRewards = std::move(a_rewards);
            progress.rewardsSelected = true;
        }
    }

    bool State::IsRewardDelivered(
        const std::string_view a_titleID,
        const RewardKey a_reward) const
    {
        std::scoped_lock lock(_lock);
        const auto found =
            _titles.find(std::string(a_titleID));
        return found != _titles.end() &&
            found->second.deliveredRewards.contains(a_reward);
    }

    void State::MarkRewardDelivered(
        const std::string_view a_titleID,
        const RewardKey a_reward)
    {
        std::scoped_lock lock(_lock);
        _titles[std::string(a_titleID)].
            deliveredRewards.emplace(a_reward);
    }

    void State::MarkRewarded(const std::string_view a_titleID)
    {
        std::scoped_lock lock(_lock);
        _titles[std::string(a_titleID)].rewarded = true;
    }

    void State::ResetTitle(const std::string_view a_titleID)
    {
        std::scoped_lock lock(_lock);
        _titles.erase(std::string(a_titleID));
    }

    void State::Revert()
    {
        std::scoped_lock lock(_lock);
        _titles.clear();
    }

    bool State::Save(
        SKSE::SerializationInterface* a_interface) const
    {
        std::scoped_lock lock(_lock);
        if (!a_interface->OpenRecord(
                kStateRecord,
                kSerializationVersion)) {
            return false;
        }
        const auto titleCount =
            static_cast<std::uint32_t>(_titles.size());
        if (!a_interface->WriteRecordData(titleCount)) {
            return false;
        }
        for (const auto& [titleID, title] : _titles) {
            const std::uint8_t flags =
                (title.completed ? 1u : 0u) |
                (title.rewarded ? 2u : 0u) |
                (title.rewardsSelected ? 4u : 0u);
            const auto requirementCount =
                static_cast<std::uint32_t>(
                    title.requirements.size());
            const auto selectedCount =
                static_cast<std::uint32_t>(
                    title.selectedRewards.size());
            const auto deliveredCount =
                static_cast<std::uint32_t>(
                    title.deliveredRewards.size());
            if (!WriteString(a_interface, titleID) ||
                !a_interface->WriteRecordData(
                    title.overallProgress) ||
                !a_interface->WriteRecordData(flags) ||
                !a_interface->WriteRecordData(requirementCount)) {
                return false;
            }
            for (const auto& [requirementID, progress] :
                 title.requirements) {
                const auto uniqueCount =
                    static_cast<std::uint32_t>(
                        progress.uniqueKeys.size());
                const std::uint8_t progressFlags =
                    progress.baselineSet ? 1u : 0u;
                if (!WriteString(a_interface, requirementID) ||
                    !WriteString(
                        a_interface,
                        progress.definitionFingerprint) ||
                    !a_interface->WriteRecordData(progress.value) ||
                    !a_interface->WriteRecordData(progress.baseline) ||
                    !a_interface->WriteRecordData(progress.highest) ||
                    !a_interface->WriteRecordData(progressFlags) ||
                    !a_interface->WriteRecordData(uniqueCount)) {
                    return false;
                }
                for (const auto key : progress.uniqueKeys) {
                    if (!a_interface->WriteRecordData(key)) {
                        return false;
                    }
                }
            }
            if (!a_interface->WriteRecordData(selectedCount)) {
                return false;
            }
            for (const auto& reward : title.selectedRewards) {
                if (!a_interface->WriteRecordData(reward.group) ||
                    !a_interface->WriteRecordData(reward.reward)) {
                    return false;
                }
            }
            if (!a_interface->WriteRecordData(deliveredCount)) {
                return false;
            }
            for (const auto& reward : title.deliveredRewards) {
                if (!a_interface->WriteRecordData(reward.group) ||
                    !a_interface->WriteRecordData(reward.reward)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool State::Load(
        SKSE::SerializationInterface* a_interface,
        const std::uint32_t a_version)
    {
        if (a_version != kSerializationVersion) {
            return false;
        }
        std::uint32_t titleCount = 0;
        if (!a_interface->ReadRecordData(titleCount) ||
            titleCount > kMaximumTitles) {
            return false;
        }
        std::unordered_map<std::string, TitleProgress> loaded;
        for (std::uint32_t titleIndex = 0;
             titleIndex < titleCount;
             ++titleIndex) {
            std::string titleID;
            TitleProgress title;
            std::uint8_t flags = 0;
            std::uint32_t requirementCount = 0;
            if (!ReadString(a_interface, titleID) ||
                !a_interface->ReadRecordData(
                    title.overallProgress) ||
                !a_interface->ReadRecordData(flags) ||
                !a_interface->ReadRecordData(requirementCount) ||
                requirementCount > kMaximumRequirements) {
                return false;
            }
            title.completed = (flags & 1u) != 0;
            title.rewarded = (flags & 2u) != 0;
            title.rewardsSelected = (flags & 4u) != 0;
            for (std::uint32_t requirementIndex = 0;
                 requirementIndex < requirementCount;
                 ++requirementIndex) {
                std::string requirementID;
                RequirementProgress progress;
                std::uint8_t progressFlags = 0;
                std::uint32_t uniqueCount = 0;
                if (!ReadString(a_interface, requirementID) ||
                    !ReadString(
                        a_interface,
                        progress.definitionFingerprint) ||
                    !a_interface->ReadRecordData(progress.value) ||
                    !a_interface->ReadRecordData(progress.baseline) ||
                    !a_interface->ReadRecordData(progress.highest) ||
                    !a_interface->ReadRecordData(progressFlags) ||
                    !a_interface->ReadRecordData(uniqueCount) ||
                    uniqueCount > kMaximumUniqueKeys) {
                    return false;
                }
                progress.baselineSet =
                    (progressFlags & 1u) != 0;
                for (std::uint32_t uniqueIndex = 0;
                     uniqueIndex < uniqueCount;
                     ++uniqueIndex) {
                    std::uint64_t key = 0;
                    if (!a_interface->ReadRecordData(key)) {
                        return false;
                    }
                    progress.uniqueKeys.emplace(key);
                }
                title.requirements.emplace(
                    std::move(requirementID),
                    std::move(progress));
            }
            std::uint32_t selectedCount = 0;
            if (!a_interface->ReadRecordData(selectedCount) ||
                selectedCount > kMaximumRequirements) {
                return false;
            }
            title.selectedRewards.resize(selectedCount);
            for (auto& reward : title.selectedRewards) {
                if (!a_interface->ReadRecordData(reward.group) ||
                    !a_interface->ReadRecordData(reward.reward)) {
                    return false;
                }
            }
            std::uint32_t deliveredCount = 0;
            if (!a_interface->ReadRecordData(deliveredCount) ||
                deliveredCount > kMaximumRequirements) {
                return false;
            }
            for (std::uint32_t rewardIndex = 0;
                 rewardIndex < deliveredCount;
                 ++rewardIndex) {
                RewardKey reward;
                if (!a_interface->ReadRecordData(reward.group) ||
                    !a_interface->ReadRecordData(reward.reward)) {
                    return false;
                }
                title.deliveredRewards.emplace(reward);
            }
            loaded.emplace(std::move(titleID), std::move(title));
        }
        std::scoped_lock lock(_lock);
        _titles = std::move(loaded);
        return true;
    }

    void State::InstallSerialization()
    {
        auto* serialization = SKSE::GetSerializationInterface();
        serialization->SetUniqueID(kSerializationID);
        serialization->SetSaveCallback(
            [](SKSE::SerializationInterface* a_interface) {
                if (!GetSingleton()->Save(a_interface)) {
                    logger::error(
                        "[WIYT] Failed to save title progress.");
                }
            });
        serialization->SetLoadCallback(
            [](SKSE::SerializationInterface* a_interface) {
                GetSingleton()->Revert();
                std::uint32_t type = 0;
                std::uint32_t version = 0;
                std::uint32_t length = 0;
                while (a_interface->GetNextRecordInfo(
                    type,
                    version,
                    length)) {
                    if (type == kStateRecord &&
                        !GetSingleton()->Load(
                            a_interface,
                            version)) {
                        logger::error(
                            "[WIYT] Failed to load title progress.");
                    }
                }
                GetSingleton()->ReconcileDefinitions(
                    Store::GetSingleton()->Titles());
                DFGBridge::GetSingleton()->SynchronizeAll();
            });
        serialization->SetRevertCallback(
            [](SKSE::SerializationInterface*) {
                GetSingleton()->Revert();
                DFGBridge::GetSingleton()->ClearRuntimeForms();
            });
    }
}
