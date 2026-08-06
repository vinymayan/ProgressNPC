#pragma once

#include <Rule.h>

namespace INLOS
{
    enum class Trigger : std::uint8_t
    {
        kDeath = 0,
        kDefeat = 1,
        kBoth = 2
    };

    enum class Destination : std::uint8_t
    {
        kVictim = 0,
        kPlayer = 1
    };

    struct LootRule
    {
        Rule criteria;
        Trigger trigger = Trigger::kDeath;
        Destination destination = Destination::kVictim;
        bool requirePlayerKiller = false;
    };

    struct Package
    {
        std::string id;
        std::string displayName;
        bool enabled = true;
        std::filesystem::path path;
        int schemaVersion = 1;
    };
}
