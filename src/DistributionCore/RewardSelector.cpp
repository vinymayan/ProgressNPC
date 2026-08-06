#include "DistributionCore/RewardSelector.h"

namespace DistributionCore
{
    std::vector<std::size_t> RollGroupIndices(
        const Rule& a_rule,
        const RandomPercent& a_random)
    {
        std::vector<std::size_t> result;
        if (!a_random) {
            return result;
        }
        if (a_rule.isExclusive) {
            const auto roll = a_random();
            float cumulative = 0.0f;
            for (std::size_t index = 0;
                 index < a_rule.rewardGroups.size();
                 ++index) {
                cumulative +=
                    a_rule.rewardGroups[index].chanceGroup;
                if (roll <= cumulative) {
                    result.push_back(index);
                    break;
                }
            }
            return result;
        }
        for (std::size_t index = 0;
             index < a_rule.rewardGroups.size();
             ++index) {
            if (a_random() <=
                a_rule.rewardGroups[index].chanceGroup) {
                result.push_back(index);
            }
        }
        return result;
    }

    std::vector<std::size_t> RollRewardIndices(
        const RewardGroup& a_group,
        const RandomPercent& a_random)
    {
        std::vector<std::size_t> result;
        if (!a_random) {
            return result;
        }
        if (a_group.isExclusive) {
            const auto roll = a_random();
            float cumulative = 0.0f;
            for (std::size_t index = 0;
                 index < a_group.rewards.size();
                 ++index) {
                cumulative +=
                    a_group.rewards[index].chanceReward;
                if (roll <= cumulative) {
                    result.push_back(index);
                    break;
                }
            }
            return result;
        }
        for (std::size_t index = 0;
             index < a_group.rewards.size();
             ++index) {
            if (a_random() <=
                a_group.rewards[index].chanceReward) {
                result.push_back(index);
            }
        }
        return result;
    }

    std::vector<RewardSelection> RollRuleRewards(
        const Rule& a_rule,
        const RandomPercent& a_random)
    {
        std::vector<RewardSelection> result;
        for (const auto groupIndex :
             RollGroupIndices(a_rule, a_random)) {
            RewardSelection selection;
            selection.groupIndex = groupIndex;
            selection.rewardIndices = RollRewardIndices(
                a_rule.rewardGroups[groupIndex], a_random);
            result.push_back(std::move(selection));
        }
        return result;
    }
}
