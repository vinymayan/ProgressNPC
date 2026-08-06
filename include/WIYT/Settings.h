#pragma once

namespace WIYT
{
    class Settings
    {
    public:
        static Settings* GetSingleton();

        bool Load();
        bool Save() const;

        bool enabled = true;
        bool creditFollowerActions = true;
        bool creditSummonActions = true;
        bool ignoreWIYTRewardEvents = true;
        float minimumStatisticRefreshSeconds = 2.0f;
    };
}
