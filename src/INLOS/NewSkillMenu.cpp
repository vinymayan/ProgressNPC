#include "INLOS/NewSkillMenu.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <windows.h>

namespace INLOS::NewSkillMenu
{
    namespace
    {
        constexpr std::uint32_t kRequiredVersion = 5;

        struct SkillListView
        {
            const char* const* items;
            std::uint32_t count;
        };

        struct Interface
        {
            std::uint32_t interfaceVersion;
            int (*GetCustomSkillLevel)(const char*);
            void (*AddCustomSkillXP)(const char*, float);
            float (*GetCustomSkillXP)(const char*);
            float (*GetSkillFormulaValue)(const char*, int);
            int (*GetCustomSkillTotalLevel)(const char*);
            int (*GetCustomSkillBonus)(const char*);
            void (*ModCustomSkillBonus)(const char*, int);
            void (*SetCustomSkillBonus)(const char*, int);
            void (*AddCustomSkillXPForActor)(
                RE::FormID,
                const char*,
                float);
            int (*GetCustomSkillLevelForActor)(
                RE::FormID,
                const char*);
            float (*GetCustomSkillXPForActor)(
                RE::FormID,
                const char*);
            int (*GetCustomSkillTotalLevelForActor)(
                RE::FormID,
                const char*);
            int (*GetCustomSkillBonusForActor)(
                RE::FormID,
                const char*);
            void (*ModCustomSkillBonusForActor)(
                RE::FormID,
                const char*,
                int);
            void (*SetCustomSkillBonusForActor)(
                RE::FormID,
                const char*,
                int);
            bool (*HasCustomPerkForActor)(
                RE::FormID,
                const char*);
            bool (*AddCustomPerkForActor)(
                RE::FormID,
                const char*);
            bool (*RemoveCustomPerkForActor)(
                RE::FormID,
                const char*);
            int (*GetActorPerkPoints)(RE::FormID);
            int (*ModActorPerkPoints)(RE::FormID, int);
            float (*GetActorResource)(
                RE::FormID,
                const char*);
            bool (*ModActorResource)(
                RE::FormID,
                const char*,
                float);
            SkillListView (*GetAvailableSkills)();
        };

        using GetInterface = void* (*)();

        std::mutex g_lock;
        Interface* g_interface = nullptr;
        std::vector<std::string> g_skills;

        bool HasSkillLocked(const std::string_view a_skillID)
        {
            return !a_skillID.empty() &&
                std::ranges::binary_search(
                    g_skills,
                    std::string(a_skillID));
        }

        bool RefreshSkillsLocked()
        {
            if (!g_interface ||
                g_interface->interfaceVersion <
                    kRequiredVersion ||
                !g_interface->GetAvailableSkills) {
                return false;
            }
            const auto view =
                g_interface->GetAvailableSkills();
            std::vector<std::string> skills;
            skills.reserve(view.count);
            for (std::uint32_t index = 0;
                 index < view.count;
                 ++index) {
                if (view.items && view.items[index] &&
                    view.items[index][0] != '\0') {
                    skills.emplace_back(view.items[index]);
                }
            }
            std::ranges::sort(skills);
            skills.erase(
                std::unique(skills.begin(), skills.end()),
                skills.end());
            g_skills = std::move(skills);
            return true;
        }
    }

    bool Initialize()
    {
        std::scoped_lock lock(g_lock);
        if (g_interface) {
            return true;
        }
        auto* module = GetModuleHandleA("SkillMenu.dll");
        if (!module) {
            return false;
        }
        const auto getter = reinterpret_cast<GetInterface>(
            GetProcAddress(module, "GetSkillMenuAPI"));
        if (!getter) {
            logger::warn(
                "[INLOS] SkillMenu.dll does not export GetSkillMenuAPI.");
            return false;
        }
        auto* candidate =
            static_cast<Interface*>(getter());
        if (!candidate ||
            candidate->interfaceVersion <
                kRequiredVersion) {
            logger::warn(
                "[INLOS] New Skill Menu API v{} or newer is required.",
                kRequiredVersion);
            return false;
        }
        g_interface = candidate;
        RefreshSkillsLocked();
        logger::info(
            "[INLOS] New Skill Menu API v{} connected ({} custom skills).",
            g_interface->interfaceVersion,
            g_skills.size());
        return true;
    }

    bool IsAvailable()
    {
        std::scoped_lock lock(g_lock);
        return g_interface != nullptr;
    }

    std::uint32_t InterfaceVersion()
    {
        std::scoped_lock lock(g_lock);
        return g_interface ?
            g_interface->interfaceVersion :
            0;
    }

    bool RefreshSkills()
    {
        if (!Initialize()) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        return RefreshSkillsLocked();
    }

    const std::vector<std::string>& AvailableSkills()
    {
        if (!IsAvailable()) {
            Initialize();
        }
        return g_skills;
    }

    bool HasSkill(const std::string_view a_skillID)
    {
        if (!Initialize()) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (g_skills.empty()) {
            RefreshSkillsLocked();
        }
        return HasSkillLocked(a_skillID);
    }

    bool AddSkillExperience(
        const RE::FormID a_actorID,
        const std::string_view a_skillID,
        const float a_amount)
    {
        if (!std::isfinite(a_amount) ||
            a_amount <= 0.0f ||
            !HasSkill(a_skillID)) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->AddCustomSkillXPForActor) {
            return false;
        }
        const std::string skillID(a_skillID);
        g_interface->AddCustomSkillXPForActor(
            a_actorID,
            skillID.c_str(),
            a_amount);
        return true;
    }

    bool AddSkillBonus(
        const RE::FormID a_actorID,
        const std::string_view a_skillID,
        const int a_amount)
    {
        if (a_amount == 0 || !HasSkill(a_skillID)) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->ModCustomSkillBonusForActor) {
            return false;
        }
        const std::string skillID(a_skillID);
        g_interface->ModCustomSkillBonusForActor(
            a_actorID,
            skillID.c_str(),
            a_amount);
        return true;
    }

    bool AddPerkPoints(
        const RE::FormID a_actorID,
        const int a_amount)
    {
        if (a_amount == 0 || !Initialize()) {
            return false;
        }
        std::scoped_lock lock(g_lock);
        if (!g_interface ||
            !g_interface->ModActorPerkPoints) {
            return false;
        }
        g_interface->ModActorPerkPoints(
            a_actorID,
            a_amount);
        return true;
    }
}
