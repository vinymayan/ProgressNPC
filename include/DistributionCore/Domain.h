#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace DistributionCore
{
    enum class Domain : std::uint8_t
    {
        kEDF = 1u << 0,
        kINLOS = 1u << 1
    };

    using DomainMask = std::uint8_t;

    constexpr DomainMask ToMask(const Domain a_domain)
    {
        return static_cast<DomainMask>(a_domain);
    }

    constexpr DomainMask kAllDomains =
        ToMask(Domain::kEDF) | ToMask(Domain::kINLOS);

    enum class EventType : std::uint8_t
    {
        kActorState = 0,
        kDeath = 1,
        kDefeat = 2
    };

    enum class RewardDestination : std::uint8_t
    {
        kSubject = 0,
        kPlayer = 1
    };

    enum class TypeCapability : std::uint32_t
    {
        kNone = 0,
        kRequiresForm = 1u << 0,
        kNumeric = 1u << 1,
        kEquipment = 1u << 2,
        kPhysicalItem = 1u << 3,
        kOneShotOnly = 1u << 4
    };

    using TypeCapabilityMask = std::uint32_t;

    constexpr TypeCapabilityMask ToMask(const TypeCapability a_capability)
    {
        return static_cast<TypeCapabilityMask>(a_capability);
    }

    struct TypeDescriptor
    {
        std::string id;
        std::string displayName;
        DomainMask domains = kAllDomains;
        TypeCapabilityMask capabilities = 0;
    };

    class TypeRegistry
    {
    public:
        bool Register(TypeDescriptor a_descriptor);
        const TypeDescriptor* Find(std::string_view a_id) const;
        std::vector<TypeDescriptor> AvailableFor(Domain a_domain) const;
        bool Supports(std::string_view a_id, Domain a_domain) const;

    private:
        mutable std::shared_mutex _lock;
        std::map<std::string, TypeDescriptor, std::less<>> _types;
    };

    TypeRegistry& FilterRegistry();
    TypeRegistry& RewardRegistry();
    void RegisterBuiltInTypes();

    struct EvaluationContext
    {
        RE::Actor* subject = nullptr;
        RE::Actor* instigator = nullptr;
        RE::PlayerCharacter* player = nullptr;
        RE::TESObjectCELL* cell = nullptr;
        RE::BGSLocation* location = nullptr;
        RE::TESForm* source = nullptr;
        EventType eventType = EventType::kActorState;
    };

    struct ResolvedItem
    {
        RE::TESBoundObject* item = nullptr;
        std::uint32_t count = 0;
    };

    std::vector<ResolvedItem> ResolveLeveledItems(
        RE::Actor* a_subject,
        RE::TESLevItem* a_list,
        std::uint32_t a_count);
}
