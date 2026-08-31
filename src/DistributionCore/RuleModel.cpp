#include "Rule.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace
{
    bool EqualInsensitive(
        const std::string_view a_lhs,
        const std::string_view a_rhs)
    {
        return a_lhs.size() == a_rhs.size() &&
            std::equal(
                a_lhs.begin(),
                a_lhs.end(),
                a_rhs.begin(),
                [](const unsigned char a_left,
                   const unsigned char a_right) {
                    return std::tolower(a_left) ==
                        std::tolower(a_right);
                });
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
        resolved != RE::ActorValue::kNone) {
        return resolved;
    }

    for (auto index = 0;
         index < std::to_underlying(RE::ActorValue::kTotal);
         ++index) {
        const auto actorValue =
            static_cast<RE::ActorValue>(index);
        const auto* actorValueName =
            RE::ActorValueList::GetActorValueName(actorValue);
        if (actorValueName &&
            EqualInsensitive(name, actorValueName)) {
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
    return a_reward.typeReward != "Actor Value" ||
        (ResolveActorValue(a_reward.actorValueName) !=
             RE::ActorValue::kNone &&
         std::isfinite(a_reward.actorValueAmount) &&
         a_reward.actorValueAmount != 0.0f);
}

bool IsNumericValueFilterType(const std::string_view a_type)
{
    return a_type == "Inventory Count" ||
        a_type == "Gold" ||
        a_type == "Faction Rank";
}

void NormalizeNumericValueFilter(BlacklistFilter& a_filter)
{
    if (!IsNumericValueFilterType(a_filter.type)) {
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
