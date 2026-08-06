#include "INLOS/Settings.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <fstream>
#include <sstream>

namespace INLOS
{
    namespace
    {
        constexpr auto kSettingsPath =
            "Data/Viny Mods/INLOS/Settings.json";
    }

    Settings* Settings::GetSingleton()
    {
        static Settings singleton;
        return std::addressof(singleton);
    }

    bool Settings::Load()
    {
        std::ifstream file(kSettingsPath, std::ios::binary);
        if (!file) {
            return Save();
        }
        std::stringstream content;
        content << file.rdbuf();
        rapidjson::Document document;
        document.Parse(content.str().c_str());
        if (document.HasParseError() || !document.IsObject()) {
            logger::error("[INLOS] Settings.json is invalid.");
            return false;
        }
        if (const auto found = document.FindMember("enableDeath");
            found != document.MemberEnd() && found->value.IsBool()) {
            enableDeath = found->value.GetBool();
        }
        if (const auto found = document.FindMember("enableDefeat");
            found != document.MemberEnd() && found->value.IsBool()) {
            enableDefeat = found->value.GetBool();
        }
        if (const auto found =
                document.FindMember("experienceMultiplier");
            found != document.MemberEnd() &&
            found->value.IsNumber()) {
            experienceMultiplier = std::clamp(
                found->value.GetFloat(), 0.0f, 1000.0f);
        }
        if (const auto found =
                document.FindMember("vanillaLootMode");
            found != document.MemberEnd() &&
            found->value.IsInt()) {
            vanillaLootMode = static_cast<VanillaLootMode>(
                std::clamp(found->value.GetInt(), 0, 2));
        }
        if (const auto found =
                document.FindMember("followerVanillaLootToPlayer");
            found != document.MemberEnd() &&
            found->value.IsBool()) {
            followerVanillaLootToPlayer =
                found->value.GetBool();
        }
        if (const auto found =
                document.FindMember(
                    "preserveQuestItemsWhenDiscarding");
            found != document.MemberEnd() &&
            found->value.IsBool()) {
            preserveQuestItemsWhenDiscarding =
                found->value.GetBool();
        }
        if (const auto found =
                document.FindMember("lootRecipientMode");
            found != document.MemberEnd() &&
            found->value.IsInt()) {
            lootRecipientMode = static_cast<LootRecipientMode>(
                std::clamp(found->value.GetInt(), 0, 2));
        }
        return true;
    }

    bool Settings::Save() const
    {
        std::error_code error;
        const auto path = std::filesystem::path(kSettingsPath);
        std::filesystem::create_directories(
            path.parent_path(), error);
        if (error) {
            return false;
        }

        rapidjson::Document document;
        document.SetObject();
        auto& allocator = document.GetAllocator();
        document.AddMember(
            "enableDeath", enableDeath, allocator);
        document.AddMember(
            "enableDefeat", enableDefeat, allocator);
        document.AddMember(
            "experienceMultiplier",
            experienceMultiplier,
            allocator);
        document.AddMember(
            "vanillaLootMode",
            static_cast<int>(vanillaLootMode),
            allocator);
        document.AddMember(
            "followerVanillaLootToPlayer",
            followerVanillaLootToPlayer,
            allocator);
        document.AddMember(
            "preserveQuestItemsWhenDiscarding",
            preserveQuestItemsWhenDiscarding,
            allocator);
        document.AddMember(
            "lootRecipientMode",
            static_cast<int>(lootRecipientMode),
            allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);
        std::ofstream file(
            path, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(
            buffer.GetString(),
            static_cast<std::streamsize>(buffer.GetSize()));
        return file.good();
    }
}
