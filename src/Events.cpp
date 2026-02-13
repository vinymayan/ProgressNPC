#include "Events.h"


RE::TESObjectREFR* GetContainerFromMenu() {
    auto ui = RE::UI::GetSingleton();
    if (!ui) return nullptr;

    // 1. Tenta o ContainerMenu padrão (Skyrim Vanilla/VR)
    if (const auto ui_menu = ui->GetMenu<RE::ContainerMenu>()) {
        auto ui_refid = ui_menu->GetTargetRefHandle();
        if (ui_refid) {
            if (const auto ui_ref = RE::TESObjectREFR::LookupByHandle(ui_refid)) {
                return ui_ref.get();
            }
        }
    }

    return nullptr;
}
    
void InjectNoGoldSWF(RE::IMenu* a_menu) {
    if (!a_menu || !a_menu->uiMovie) return;

    auto view = a_menu->uiMovie.get();
    RE::GFxValue root;

    // 1. Obtém o _root do Flash
    if (view->GetVariable(&root, "_root")) {

        // Gerar profundidade aleatória entre 2000 e 4000
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(2000, 4000);
        int randomDepth = dis(gen);

        // 2. Preparar argumentos para createEmptyMovieClip("NoGold", depth)
        RE::GFxValue args[2];
        args[0].SetString("NoGold");
        args[1].SetNumber(randomDepth);

        // Invoca o comando no _root
        root.Invoke("createEmptyMovieClip", nullptr, args, 2);

        // 3. Agora acessamos o novo MovieClip criado para carregar o SWF
        RE::GFxValue noGoldClip;
        if (view->GetVariable(&noGoldClip, "_root.NoGold")) {
            RE::GFxValue swfPath;
            swfPath.SetString("nogold_inject.swf");

            noGoldClip.Invoke("loadMovie", nullptr, &swfPath, 1);

            logger::debug("SKSE: SWF 'nogold_inject.swf' injetado com sucesso em _root.NoGold (Depth: {})", randomDepth);
        }
    }
}

// Exemplo de como a função Testarone poderia ser declarada (ou deve ser importada de outro lugar)
void Testarone(RE::Actor* a_deadActor) {
    if (!a_deadActor) return;

    auto sink = EventSink::GetSingleton();
    // Obtemos o inventário atual do ator
    auto inventory = a_deadActor->GetInventory();

    logger::debug("Testarone: Iniciando processamento para: {} (ID: {:x})",
        a_deadActor->GetName(), a_deadActor->GetFormID());

    uint32_t count = 0;
    for (auto& [item, data] : inventory) {
        if (!item) continue;

        // 1. Aplicar flag na TESForm base (afeta todos os itens por padrão)
       
        if (item->IsGold()) {



        }
        item->formFlags |= RE::TESForm::RecordFlags::kNonPlayable;
        // 3. Tratar flags específicas para outros tipos
        if (auto weapon = item->As<RE::TESObjectWEAP>()) {
            weapon->weaponData.flags |= RE::TESObjectWEAP::Data::Flag::kNonPlayable;
        }
        else if (auto ammo = item->As<RE::TESAmmo>()) {
            ammo->GetRuntimeData().data.flags |= RE::AMMO_DATA::Flag::kNonPlayable;
        }
        else if (auto key = item->As<RE::TESKey>()) {
            key->formFlags |= RE::TESKey::RecordFlags::kNonPlayable;
        }
        else if (auto misc = item->As<RE::TESObjectMISC>()) {
            misc->formFlags |= RE::TESObjectMISC::RecordFlags::kIgnored;
        }

        sink->_modifiedItems.push_back(item);

        logger::debug("Item processado: {} | Playable: {}",
            item->GetName() ? item->GetName() : "Sem Nome",
            item->GetPlayable());

        count++;
    }
    RE::SendUIMessage::SendInventoryUpdateMessage(a_deadActor,nullptr);
    logger::debug("Testarone: Concluído. {} itens processados. Estado final Playable verificado.", count);
}

void EventSink::RestoreItemsPlayability() {
    if (_modifiedItems.empty()) {
        logger::debug("RestoreItemsPlayability: Nenhum item para restaurar.");
        return;
    }

    size_t count = _modifiedItems.size();
    for (auto* item : _modifiedItems) {
        if (item) {
            item->formFlags &= ~RE::TESForm::RecordFlags::kNonPlayable;
        }
    }
    _modifiedItems.clear();
    logger::debug("RestoreItemsPlayability: Inventário restaurado. {} itens voltaram a ser Playable.", count);
}

void EventSink::OnQuickLootOpen(QuickLoot::API::OpenLootMenuEvent* a_event) {
    if (!a_event || !a_event->container) return;

    logger::debug("QuickLoot detectado: Abrindo container {:x}", a_event->container->GetFormID());

    if (auto actor = a_event->container->As<RE::Actor>()) {
        if (actor->IsDead()) {
            Testarone(actor);
        }
    }
}

void EventSink::OnQuickLootClose(QuickLoot::API::CloseLootMenuEvent* a_event) {
    logger::debug("QuickLoot detectado: Fechando menu.");
    EventSink::GetSingleton()->RestoreItemsPlayability();
}

RE::BSEventNotifyControl EventSink::ProcessEvent(const RE::MenuOpenCloseEvent* a_event,
    RE::BSTEventSource<RE::MenuOpenCloseEvent>*) {

    if (!a_event) return RE::BSEventNotifyControl::kContinue;

    if (a_event->menuName == RE::ContainerMenu::MENU_NAME) {
        if (a_event->opening) {
            
            auto containerRef = GetContainerFromMenu();
            if (containerRef) {
                if (auto actor = containerRef->As<RE::Actor>()) {
                    if (actor->IsDead()) {
                        auto ui = RE::UI::GetSingleton();
                        if (auto menu = ui->GetMenu<RE::ContainerMenu>()) {
                            InjectNoGoldSWF(menu.get());
                        }
                        Testarone(actor);
                    }
                }
            }
        }
        else {
            RestoreItemsPlayability();
        }
    }

    return RE::BSEventNotifyControl::kContinue;
}