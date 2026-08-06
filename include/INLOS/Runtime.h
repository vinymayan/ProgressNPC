#pragma once

#include "INLOS/Rule.h"

#include <mutex>

namespace INLOS
{
    struct LifecycleState
    {
        std::uint32_t generation = 0;
        bool dead = false;
        bool deathProcessed = false;
        bool defeatProcessed = false;
        std::set<std::string> appliedRuleIDs;
    };

    class State
    {
    public:
        static State* GetSingleton();

        bool BeginEvent(
            RE::FormID a_actorID,
            Trigger a_trigger,
            std::vector<std::string>& a_previouslyApplied);
        void CompleteRule(RE::FormID a_actorID, std::string_view a_ruleID);
        void MarkAlive(RE::FormID a_actorID);
        void MarkLoadedAlive(RE::FormID a_actorID);
        void Revert();

        void AddExperience(float a_amount);
        float GetExperience() const;
        std::vector<std::pair<RE::FormID, LifecycleState>>
            GetLifecycleSnapshot() const;

        static void InstallSerialization();
        bool Save(SKSE::SerializationInterface* a_interface) const;
        bool Load(
            SKSE::SerializationInterface* a_interface,
            std::uint32_t a_version);

    private:
        mutable std::mutex _lock;
        std::unordered_map<RE::FormID, LifecycleState> _actors;
        float _experience = 0.0f;
    };

    class DeathEventHandler :
        public RE::BSTEventSink<RE::TESDeathEvent>,
        public RE::BSTEventSink<RE::TESObjectLoadedEvent>
    {
    public:
        static DeathEventHandler* GetSingleton();
        static void Register();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESDeathEvent* a_event,
            RE::BSTEventSource<RE::TESDeathEvent>*) override;
        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESObjectLoadedEvent* a_event,
            RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override;
    };

    class DefeatEventHandler :
        public RE::BSTEventSink<SKSE::ModCallbackEvent>
    {
    public:
        static DefeatEventHandler* GetSingleton();
        static void Register();

        RE::BSEventNotifyControl ProcessEvent(
            const SKSE::ModCallbackEvent* a_event,
            RE::BSTEventSource<SKSE::ModCallbackEvent>*) override;
    };

    void QueueEvaluation(
        RE::ActorHandle a_subject,
        RE::ActorHandle a_instigator,
        Trigger a_trigger);
}
