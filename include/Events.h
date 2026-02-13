#pragma once
#include "ClibUtil/editorID.hpp"
#include "QuickLootAPI.h"

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

