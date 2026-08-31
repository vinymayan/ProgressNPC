#pragma once

namespace INLOS
{
    enum class VanillaLootMode : std::uint8_t
    {
        kDoNothing = 0,
        kAutoLoot = 1,
        kDiscard = 2
    };

    enum class LootRecipientMode : std::uint8_t
    {
        kPlayerOnly = 0,
        kAnyActor = 1,
        kPlayerAndFollowers = 2
    };

    class Settings
    {
    public:
        static Settings* GetSingleton();

        bool Load();
        bool Save() const;

        bool enableDeath = true;
        bool enableDefeat = true;
        float experienceMultiplier = 1.0f;
        VanillaLootMode vanillaLootMode =
            VanillaLootMode::kDoNothing;
        bool followerVanillaLootToPlayer = false;
        bool preserveQuestItemsWhenDiscarding = true;
        LootRecipientMode lootRecipientMode =
            LootRecipientMode::kPlayerOnly;
        bool giveSpellTomeWhenKnown = true;
        bool givePerkBookWhenOwned = true;
    };
}
