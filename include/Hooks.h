#pragma once
#include <shared_mutex>
#include "SaveState.h"

struct Hooks
{
	static void Install()
	{
		REL::Relocation<std::uintptr_t> vtbl{ REL::VariantID(261399, 207890, 0x16d7760) };
		_ProcessEvent = vtbl.write_vfunc(0x1, Hook_ProcessEvent);
	}

	static RE::BSEventNotifyControl Hook_ProcessEvent(
		RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this,
		RE::BSAnimationGraphEvent* a_event,
		RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_dispatcher)
	{
		if (a_event && a_event->holder) {
			std::string_view tag = a_event->tag.c_str();
			auto actor = const_cast<RE::Actor*>(a_event->holder->As<RE::Actor>());
			if (actor) {
				if (tag == "SBF_EnterBed" || tag == "SBF_EnterBedroll") {
					logger::debug("[Hooks] Detected sleep event: {} for actor {:08X}", tag, actor->GetFormID());
					ScheduleSleepOutfitUpdate(actor, true);

				}
				else if (tag == "SBF_ExitBed" || tag == "SBF_ExitBedroll") {
					logger::debug("[Hooks] Detected wake event: {} for actor {:08X}", tag, actor->GetFormID());
					ScheduleSleepOutfitUpdate(actor, false);

				}
			}


		}
		return _ProcessEvent(a_this, a_event, a_dispatcher);
	}

	static inline REL::Relocation<decltype(Hook_ProcessEvent)> _ProcessEvent;

};
