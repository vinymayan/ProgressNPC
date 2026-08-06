#include "DistributionCore/Domain.h"
#include "INLOS/NewSkillMenu.h"
#include "INLOS/Runtime.h"
#include "INLOS/Settings.h"
#include "INLOS/Store.h"
#include "INLOS/UI.h"
#include "Manager.h"
#include "logger.h"

namespace
{
    void OnINLOSMessage(SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message) {
            return;
        }
        if (a_message->type ==
            SKSE::MessagingInterface::kPostLoad) {
            INLOS::NewSkillMenu::Initialize();
            return;
        }
        if (a_message->type ==
            SKSE::MessagingInterface::kDataLoaded) {
            INLOS::Settings::GetSingleton()->Load();
            Manager::GetSingleton()->PopulateAllLists();
            if (!INLOS::Store::GetSingleton()->Load()) {
                logger::error(
                    "[INLOS] Rule packages could not be loaded.");
            }
            INLOS::DeathEventHandler::Register();
            INLOS::DefeatEventHandler::Register();
            INLOS::UI::Register();
            INLOS::NewSkillMenu::RefreshSkills();
            logger::info("[INLOS] DataLoaded initialization complete.");
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SetupLog();
    SKSE::Init(a_skse);
    DistributionCore::RegisterBuiltInTypes();
    INLOS::State::InstallSerialization();
    SKSE::GetMessagingInterface()->RegisterListener(
        OnINLOSMessage);
    logger::info("INLOS loaded.");
    return true;
}
