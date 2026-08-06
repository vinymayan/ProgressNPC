#include "DistributionCore/Domain.h"
#include "INLOS/NewSkillMenu.h"
#include "Manager.h"
#include "WIYT/DFGBridge.h"
#include "WIYT/Runtime.h"
#include "WIYT/Settings.h"
#include "WIYT/State.h"
#include "WIYT/Store.h"
#include "WIYT/UI.h"
#include "logger.h"

namespace
{
    void OnWIYTMessage(
        SKSE::MessagingInterface::Message* a_message)
    {
        if (!a_message) {
            return;
        }
        if (a_message->type ==
            SKSE::MessagingInterface::kPostLoad) {
            INLOS::NewSkillMenu::Initialize();
            WIYT::DFGBridge::GetSingleton()->Initialize();
            return;
        }
        if (a_message->type ==
            SKSE::MessagingInterface::kDataLoaded) {
            WIYT::Settings::GetSingleton()->Load();
            Manager::GetSingleton()->PopulateAllLists();
            if (!WIYT::Store::GetSingleton()->Load()) {
                logger::error(
                    "[WIYT] One or more title packages could not be loaded.");
            }
            WIYT::RebuildRequirementIndex();
            WIYT::InstallDamageTracking();
            WIYT::EventHandler::Register();
            WIYT::UI::Register();
            INLOS::NewSkillMenu::RefreshSkills();
            WIYT::DFGBridge::GetSingleton()->SynchronizeAll();
            WIYT::RefreshProgressSources(true);
            WIYT::ProcessPendingRewards();
            logger::info(
                "[WIYT] DataLoaded initialization complete.");
            return;
        }
        if (a_message->type ==
                SKSE::MessagingInterface::kPostLoadGame ||
            a_message->type ==
                SKSE::MessagingInterface::kNewGame) {
            WIYT::ResetTransientTracking();
            WIYT::State::GetSingleton()->ReconcileDefinitions(
                WIYT::Store::GetSingleton()->Titles());
            WIYT::DFGBridge::GetSingleton()->SynchronizeAll();
            WIYT::RefreshProgressSources(true);
            WIYT::ProcessPendingRewards();
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SetupLog();
    SKSE::Init(a_skse);
    DistributionCore::RegisterBuiltInTypes();
    WIYT::State::InstallSerialization();
    SKSE::GetMessagingInterface()->RegisterListener(
        OnWIYTMessage);
    logger::info("WIYT loaded.");
    return true;
}
