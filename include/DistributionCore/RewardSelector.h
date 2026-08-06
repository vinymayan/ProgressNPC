#pragma once

#include <Rule.h>

#include <functional>

namespace DistributionCore
{
    using RandomPercent = std::function<float()>;

    struct RewardSelection
    {
        std::size_t groupIndex = 0;
        std::vector<std::size_t> rewardIndices;
    };

    std::vector<std::size_t> RollGroupIndices(
        const Rule& a_rule,
        const RandomPercent& a_random);
    std::vector<std::size_t> RollRewardIndices(
        const RewardGroup& a_group,
        const RandomPercent& a_random);
    std::vector<RewardSelection> RollRuleRewards(
        const Rule& a_rule,
        const RandomPercent& a_random);
}
