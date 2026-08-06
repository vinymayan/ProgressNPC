#include "WIYT/Model.h"

#include <cctype>
#include <random>
#include <sstream>

namespace WIYT
{
    namespace
    {
        std::string DeterministicHash(const std::string_view a_value)
        {
            constexpr std::uint64_t offset = 14695981039346656037ull;
            constexpr std::uint64_t prime = 1099511628211ull;
            std::uint64_t hash = offset;
            for (const unsigned char character : a_value) {
                hash ^= character;
                hash *= prime;
            }
            return std::format("{:016x}", hash);
        }

        void AppendFilter(
            std::ostringstream& a_stream,
            const BlacklistFilter& a_filter)
        {
            a_stream << "|F:" << a_filter.type << ':' <<
                a_filter.formIDStr << ':' << a_filter.editorID << ':' <<
                a_filter.actorValueName << ':' << a_filter.optionMode << ':' <<
                a_filter.optionValue << ':' << a_filter.optionText << ':' <<
                static_cast<int>(a_filter.actorValueMode) << ':' <<
                static_cast<int>(a_filter.comparison) << ':' <<
                a_filter.minimumValue << ':' << a_filter.maximumValue;
        }
    }

    std::string GenerateUUID()
    {
        static thread_local std::mt19937_64 generator{
            std::random_device{}()
        };
        std::uniform_int_distribution<std::uint32_t> distribution(
            0,
            0xFFFFFFFFu);
        const auto a = distribution(generator);
        const auto b = distribution(generator);
        const auto c = distribution(generator);
        const auto d = distribution(generator);
        return std::format(
            "{:08x}-{:04x}-4{:03x}-{:01x}{:03x}-{:08x}{:04x}",
            a,
            b >> 16,
            b & 0x0FFF,
            8 + ((c >> 28) & 0x3),
            (c >> 16) & 0x0FFF,
            d,
            c & 0xFFFF);
    }

    std::string SanitizePublicGlobalName(
        const std::string_view a_titleName)
    {
        std::string result = "WIYT_";
        bool uppercaseNext = true;
        for (const unsigned char character : a_titleName) {
            if (std::isalnum(character)) {
                result.push_back(
                    uppercaseNext ?
                        static_cast<char>(std::toupper(character)) :
                        static_cast<char>(character));
                uppercaseNext = false;
            }
            else {
                uppercaseNext = true;
            }
            if (result.size() >= 119) {
                break;
            }
        }
        if (result == "WIYT_") {
            result += "Title";
        }
        return result;
    }

    std::string RequirementFingerprint(
        const Requirement& a_requirement)
    {
        std::ostringstream stream;
        stream << static_cast<int>(a_requirement.source) << '|' <<
            static_cast<int>(a_requirement.activity) << '|' <<
            static_cast<int>(a_requirement.trackingMode) << '|' <<
            static_cast<int>(a_requirement.aggregation) << '|' <<
            a_requirement.statisticName << '|' <<
            a_requirement.referenceFormID << '|' <<
            a_requirement.referenceEditorID << '|' <<
            a_requirement.graphVariableName << '|' <<
            a_requirement.graphVariableType << '|' <<
            a_requirement.filtersRequireAll;
        const auto append = [&](const auto& a_filters) {
            for (const auto& filter : a_filters) {
                AppendFilter(stream, filter);
            }
        };
        append(a_requirement.creditedActorFilters);
        append(a_requirement.targetActorFilters);
        append(a_requirement.sourceFormFilters);
        append(a_requirement.environmentFilters);
        return DeterministicHash(stream.str());
    }

    std::string TitleDefinition::CalculateHash() const
    {
        std::ostringstream stream;
        stream << name << '|' << description << '|' <<
            publicGlobalEditorID << '|' << enabled;
        for (const auto& requirement : requirements) {
            stream << "|Q:" << requirement.id << ':' <<
                requirement.name << ':' <<
                RequirementFingerprint(requirement) << ':' <<
                requirement.targetAmount;
        }
        for (const auto& group : rewardGroups) {
            stream << "|G:" << group.name << ':' << group.isExclusive <<
                ':' << group.chanceGroup;
            for (const auto& reward : group.rewards) {
                stream << "|R:" << reward.typeReward << ':' <<
                    reward.formIDStr << ':' << reward.editorID << ':' <<
                    reward.amount << ':' << reward.chanceReward << ':' <<
                    reward.functionOnType << ':' << reward.isPersistent;
            }
        }
        return DeterministicHash(stream.str());
    }

    const char* ToString(const ProgressSource a_value)
    {
        switch (a_value) {
        case ProgressSource::kVanillaStatistic:
            return "Vanilla Statistic";
        case ProgressSource::kEventCounter:
            return "WIYT Event Counter";
        case ProgressSource::kGlobal:
            return "Global Variable";
        case ProgressSource::kGraphVariable:
            return "Player Graph Variable";
        default:
            return "Unknown";
        }
    }

    const char* ToString(const ActivityType a_value)
    {
        switch (a_value) {
        case ActivityType::kActorKilled:
            return "Actor Killed";
        case ActivityType::kActorDefeated:
            return "Actor Defeated";
        case ActivityType::kItemHarvested:
            return "Item Harvested";
        case ActivityType::kItemAcquired:
            return "Item Acquired";
        case ActivityType::kDamageDealt:
            return "Damage Dealt";
        case ActivityType::kSpellDamageDealt:
            return "Spell Damage Dealt";
        case ActivityType::kItemCrafted:
            return "Item Crafted";
        case ActivityType::kLocationDiscovered:
            return "Location Discovered";
        case ActivityType::kQuestCompleted:
            return "Quest Completed";
        case ActivityType::kGoldEarned:
            return "Gold Earned";
        case ActivityType::kGoldSpent:
            return "Gold Spent";
        case ActivityType::kCustom:
            return "Custom";
        default:
            return "Unknown";
        }
    }

    const char* ToString(const TrackingMode a_value)
    {
        switch (a_value) {
        case TrackingMode::kLifetimeTotal:
            return "Lifetime Total";
        case TrackingMode::kSinceActivated:
            return "Since Title Activated";
        case TrackingMode::kHighestReached:
            return "Highest Reached";
        default:
            return "Unknown";
        }
    }

    const char* ToString(const Aggregation a_value)
    {
        switch (a_value) {
        case Aggregation::kCount:
            return "Count";
        case Aggregation::kSum:
            return "Sum";
        case Aggregation::kUniqueCount:
            return "Unique Count";
        case Aggregation::kHighestValue:
            return "Highest Value";
        default:
            return "Unknown";
        }
    }
}
