// credits: NoahBoddie <3<3<3
namespace RE
{
    class TESObjectREFR;

    enum class PackageState : std::int32_t
    {
        Start = 0,
        Change = 1,
        End = 2
    };

    struct TESPackageEvent
    {
    public:
        // members
        RE::TESObjectREFR* actor;        // 0x0
        RE::FormID      package;    // 0x8
        PackageState    state;        // 0xC
    };
    static_assert(sizeof(TESPackageEvent) == 0x10);
}

#include "RE/S/ScriptEventSourceHolder.h"

//// 1. Defina sua classe que herdará do Sink de eventos
//class MyPackageEventHandler : public RE::BSTEventSink<RE::TESPackageEvent>
//{
//public:
//    // O Singleton para garantir que só exista uma instância do seu ouvinte
//    static MyPackageEventHandler* GetSingleton()
//    {
//        static MyPackageEventHandler singleton;
//        return &singleton;
//    }
//
//    // 2. Implemente o ProcessEvent (o que seria o seu "Hook")
//    RE::BSEventNotifyControl ProcessEvent(const RE::TESPackageEvent* a_event, RE::BSTEventSource<RE::TESPackageEvent>* a_source) override
//    {
//        if (!a_event || !a_event->actor) {
//            return RE::BSEventNotifyControl::kContinue;
//        }
//
//        auto actor = a_event->actor;
//        if (!actor->IsHumanoid()) {
//            return RE::BSEventNotifyControl::kContinue;
//        }
//        // Lógica de filtro (ex: apenas quando o pacote de sono começa)
//        auto packagePtr = RE::TESForm::LookupByID<RE::TESPackage>(a_event->package);
//        if (packagePtr) {
//            // 1. Obtemos as flags gerais e o tipo de procedimento
//            auto flags = packagePtr->packData.packFlags.get();
//            auto pType = packagePtr->packData.packType.get();
//            auto procType = packagePtr->procedureType.get(); // Offset 0xD8 no TESPackage
//
//            // 3. Verificação por Flag (kWearSleepOutfit = 1 << 29)
//            using GFlag = RE::PACKAGE_DATA::GeneralFlag;
//            bool isSleepingOutfit = packagePtr->packData.packFlags.any(GFlag::kWearSleepOutfit);
//
//            // 4. Verificação por Procedimento (kSleep = 4)
//            bool isSleepProc = (procType == RE::PACKAGE_PROCEDURE_TYPE::kSleep);
//
//            if (isSleepingOutfit || isSleepProc) {
//                SKSE::log::info("FUNCIONOU {}", actor->GetName());
//                if (a_event->state != RE::PackageState::End) {
//                    SKSE::log::info("-> Identificado estado de sono (Flag ou Proc) para {}", actor->GetName());
//                    //EquipBestInventoryItems(actor);
//                }
//            }
//        }
//        return RE::BSEventNotifyControl::kContinue;
//    }
//
//    // 3. Função para registrar o ouvinte no motor do jogo
//    static void Register()
//    {
//        auto sourceHolder = RE::ScriptEventSourceHolder::GetSingleton();
//        if (sourceHolder) {
//            sourceHolder->AddEventSink<RE::TESPackageEvent>(GetSingleton());
//            SKSE::log::info("Registrado: MyPackageEventHandler como sink de TESPackageEvent.");
//        }
//    }
//};