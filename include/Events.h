#pragma once
#include "ClibUtil/editorID.hpp"
#include "QuickLootAPI.h"

namespace DynamicFormsGeneratorEvents {
    bool HasLoaded();
}
class DynamicFormsGeneratorListener : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
public:
    static DynamicFormsGeneratorListener* GetSingleton() {
        static DynamicFormsGeneratorListener singleton;
        return &singleton;
    }

    void Register() {
        auto dispatcher = SKSE::GetModCallbackEventSource();
        if (dispatcher) dispatcher->AddEventSink(this);
    }

    RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override;
};

class FollowerDialogueEventHandler :
    public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    static FollowerDialogueEventHandler* GetSingleton()
    {
        static FollowerDialogueEventHandler singleton;
        return &singleton;
    }

    static void Register();

    RE::BSEventNotifyControl ProcessEvent(
        const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
};

class EventSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    // O Singleton para acessar o handler
    static EventSink* GetSingleton() {
        static EventSink singleton;
        return &singleton;
    }

    std::vector<RE::TESForm*> _modifiedItems;
    RE::Actor* _lastActor = nullptr;

    void RestoreItemsPlayability();
    static void OnQuickLootOpen(QuickLoot::API::OpenLootMenuEvent* a_event);
    static void OnQuickLootClose(QuickLoot::API::CloseLootMenuEvent* a_event);
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;
};
