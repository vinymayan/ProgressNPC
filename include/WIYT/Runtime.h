#pragma once

#include "WIYT/Model.h"

#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace WIYT
{
    void RebuildRequirementIndex();
    void ReportProgressEvent(const ProgressEvent& a_event);
    void RefreshProgressSources(bool a_force = false);
    void ProcessPendingRewards();
    void InstallDamageTracking();
    void ResetTransientTracking();
    float GetCachedStatistic(std::string_view a_name);
    const std::vector<std::string>& KnownStatistics();
    void SuppressNextAcquisition(
        RE::FormID a_formID,
        std::int32_t a_count);

    class EventHandler :
        public RE::BSTEventSink<RE::TESDeathEvent>,
        public RE::BSTEventSink<RE::TESContainerChangedEvent>,
        public RE::BSTEventSink<RE::TESActivateEvent>,
        public RE::BSTEventSink<RE::TESQuestStageEvent>,
        public RE::BSTEventSink<RE::TESHitEvent>,
        public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
        public RE::BSTEventSink<RE::ItemCrafted::Event>,
        public RE::BSTEventSink<RE::LocationDiscovery::Event>,
        public RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
    public:
        static EventHandler* GetSingleton();
        static void Register();
        void ResetTransient();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESDeathEvent* a_event,
            RE::BSTEventSource<RE::TESDeathEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESContainerChangedEvent* a_event,
            RE::BSTEventSource<RE::TESContainerChangedEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESActivateEvent* a_event,
            RE::BSTEventSource<RE::TESActivateEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESQuestStageEvent* a_event,
            RE::BSTEventSource<RE::TESQuestStageEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESHitEvent* a_event,
            RE::BSTEventSource<RE::TESHitEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::ItemCrafted::Event* a_event,
            RE::BSTEventSource<RE::ItemCrafted::Event>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::LocationDiscovery::Event* a_event,
            RE::BSTEventSource<RE::LocationDiscovery::Event>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const SKSE::ModCallbackEvent* a_event,
            RE::BSTEventSource<SKSE::ModCallbackEvent>*) override;

    private:
        struct PendingHarvest
        {
            RE::FormID itemID = 0;
            RE::FormID cellID = 0;
            std::chrono::steady_clock::time_point timestamp{};
        };

        std::mutex _lock;
        PendingHarvest _pendingHarvest;
    };
}
