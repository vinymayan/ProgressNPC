#include "WIYT/Settings.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <fstream>
#include <sstream>

namespace WIYT
{
    namespace
    {
        constexpr auto kSettingsPath =
            "Data/Viny Mods/WIYT/Settings.json";
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
            logger::error("[WIYT] Settings.json is invalid.");
            return false;
        }
        const auto readBool = [&](
                                  const char* a_name,
                                  bool& a_value) {
            const auto found = document.FindMember(a_name);
            if (found != document.MemberEnd() &&
                found->value.IsBool()) {
                a_value = found->value.GetBool();
            }
        };
        readBool("enabled", enabled);
        readBool(
            "creditFollowerActions",
            creditFollowerActions);
        readBool(
            "creditSummonActions",
            creditSummonActions);
        readBool(
            "ignoreWIYTRewardEvents",
            ignoreWIYTRewardEvents);
        if (const auto found = document.FindMember(
                "minimumStatisticRefreshSeconds");
            found != document.MemberEnd() &&
            found->value.IsNumber()) {
            minimumStatisticRefreshSeconds = std::clamp(
                found->value.GetFloat(),
                0.25f,
                60.0f);
        }
        return true;
    }

    bool Settings::Save() const
    {
        const auto path = std::filesystem::path(kSettingsPath);
        std::error_code error;
        std::filesystem::create_directories(
            path.parent_path(),
            error);
        if (error) {
            return false;
        }
        rapidjson::Document document;
        document.SetObject();
        auto& allocator = document.GetAllocator();
        document.AddMember("enabled", enabled, allocator);
        document.AddMember(
            "creditFollowerActions",
            creditFollowerActions,
            allocator);
        document.AddMember(
            "creditSummonActions",
            creditSummonActions,
            allocator);
        document.AddMember(
            "ignoreWIYTRewardEvents",
            ignoreWIYTRewardEvents,
            allocator);
        document.AddMember(
            "minimumStatisticRefreshSeconds",
            minimumStatisticRefreshSeconds,
            allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
        document.Accept(writer);
        std::ofstream file(
            path,
            std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(
            buffer.GetString(),
            static_cast<std::streamsize>(buffer.GetSize()));
        return file.good();
    }
}
