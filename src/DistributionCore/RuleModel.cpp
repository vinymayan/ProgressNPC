#include "Rule.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace
{
    bool IsValidActorValue(const RE::ActorValue a_actorValue)
    {
        const auto value = std::to_underlying(a_actorValue);
        return value >= 0 &&
            value < std::to_underlying(RE::ActorValue::kTotal);
    }

    std::string NormalizeActorValueName(const std::string_view a_name)
    {
        std::string normalized;
        normalized.reserve(a_name.size());
        for (const auto character : a_name) {
            const auto value = static_cast<unsigned char>(character);
            if (std::isalnum(value)) {
                normalized.push_back(static_cast<char>(std::tolower(value)));
            }
        }
        return normalized;
    }

}

RE::ActorValue ResolveActorValue(const std::string_view a_name)
{
    if (a_name.empty()) {
        return RE::ActorValue::kNone;
    }

    const std::string name(a_name);
    if (const auto resolved =
            RE::ActorValueList::LookupActorValueByName(name.c_str());
        IsValidActorValue(resolved)) {
        return resolved;
    }

    const auto normalized = NormalizeActorValueName(name);
    for (auto index = 0;
         index < std::to_underlying(RE::ActorValue::kTotal);
         ++index) {
        const auto actorValue =
            static_cast<RE::ActorValue>(index);
        const auto* actorValueName =
            RE::ActorValueList::GetActorValueName(actorValue);
        if (actorValueName &&
            NormalizeActorValueName(actorValueName) == normalized) {
            return actorValue;
        }
    }
    return RE::ActorValue::kNone;
}

bool IsMaximumActorValueSupported(
    const RE::ActorValue a_actorValue)
{
    return a_actorValue == RE::ActorValue::kHealth ||
        a_actorValue == RE::ActorValue::kMagicka ||
        a_actorValue == RE::ActorValue::kStamina;
}

bool IsActorValueFilterValid(const BlacklistFilter& a_filter)
{
    if (a_filter.type != "Actor Value") {
        return true;
    }
    const auto actorValue =
        ResolveActorValue(a_filter.actorValueName);
    return actorValue != RE::ActorValue::kNone &&
        (a_filter.actorValueMode != ActorValueMode::kMaximum ||
            IsMaximumActorValueSupported(actorValue));
}

bool IsActorValueRewardValid(const Reward& a_reward)
{
    if (a_reward.typeReward != "Actor Value") {
        return true;
    }

    const auto target = ResolveActorValue(a_reward.actorValueName);
    if (target == RE::ActorValue::kNone ||
        !std::isfinite(a_reward.sourceMultiplier) ||
        a_reward.sourceMultiplier == 0.0f ||
        (a_reward.numericOperation == NumericRewardOperation::kPercent &&
         a_reward.percentBaseMode == ActorValueMode::kMaximum &&
         !IsMaximumActorValueSupported(target))) {
        return false;
    }

    switch (a_reward.numericSource) {
    case NumericRewardSource::kFixed:
        return std::isfinite(a_reward.actorValueAmount) &&
            a_reward.actorValueAmount != 0.0f;
    case NumericRewardSource::kGlobal:
        return !a_reward.sourceGlobalFormID.empty() ||
            !a_reward.sourceGlobalEditorID.empty();
    case NumericRewardSource::kActorValue: {
        const auto source =
            ResolveActorValue(a_reward.sourceActorValueName);
        return source != RE::ActorValue::kNone && source != target;
    }
    default:
        return false;
    }
}

bool IsActorScaleRewardValid(const Reward& a_reward)
{
    if (a_reward.typeReward != "Actor Scale") {
        return true;
    }
    if (!std::isfinite(a_reward.sourceMultiplier) ||
        a_reward.sourceMultiplier == 0.0f) {
        return false;
    }
    switch (a_reward.numericSource) {
    case NumericRewardSource::kFixed:
        return std::isfinite(a_reward.actorValueAmount) &&
            a_reward.actorValueAmount != 0.0f;
    case NumericRewardSource::kGlobal:
        return !a_reward.sourceGlobalFormID.empty() ||
            !a_reward.sourceGlobalEditorID.empty();
    case NumericRewardSource::kActorValue:
        return ResolveActorValue(a_reward.sourceActorValueName) !=
            RE::ActorValue::kNone;
    default:
        return false;
    }
}

bool IsNumericValueFilterType(const std::string_view a_type)
{
    return a_type == "Inventory Count" ||
        a_type == "Gold" ||
        a_type == "Faction Rank" ||
        a_type == "Height" ||
        a_type == "Weight";
}

void NormalizeNumericValueFilter(BlacklistFilter& a_filter)
{
    if (!IsNumericValueFilterType(a_filter.type)) {
        return;
    }

    if (a_filter.type == "Height" || a_filter.type == "Weight") {
        if (a_filter.comparison != NumericComparison::kBetween) {
            a_filter.maximumValue = a_filter.minimumValue;
        }
        return;
    }

    if (a_filter.minimumValue == 0.0f &&
        a_filter.maximumValue == 0.0f) {
        std::vector<std::string> tokens;
        std::istringstream stream(a_filter.formIDStr);
        for (std::string token; std::getline(stream, token, '|');) {
            tokens.push_back(std::move(token));
        }
        if (tokens.size() >= 3) {
            try {
                a_filter.minimumValue =
                    static_cast<float>(std::stoi(tokens[2]));
            }
            catch (...) {
                a_filter.minimumValue =
                    a_filter.type == "Faction Rank" ?
                    0.0f :
                    1.0f;
            }
        }
        else {
            a_filter.minimumValue =
                a_filter.type == "Faction Rank" ?
                0.0f :
                1.0f;
        }
    }
    if (a_filter.comparison != NumericComparison::kBetween) {
        a_filter.maximumValue = a_filter.minimumValue;
    }
}

bool MatchesRuleLevel(const int a_actorLevel, const Rule& a_rule)
{
    const auto primary = std::max(1, a_rule.level);
    switch (a_rule.levelComparison) {
    case NumericComparison::kGreaterOrEqual:
        return a_actorLevel >= primary;
    case NumericComparison::kLessOrEqual:
        return a_actorLevel <= primary;
    case NumericComparison::kEqual:
        return a_actorLevel == primary;
    case NumericComparison::kBetween: {
        const auto [minimum, maximum] = std::minmax(
            primary,
            std::max(1, a_rule.maximumLevel));
        return a_actorLevel >= minimum &&
            a_actorLevel <= maximum;
    }
    default:
        return a_actorLevel >= primary;
    }
}
