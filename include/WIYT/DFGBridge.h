#pragma once

#include "WIYT/DFGAPI.h"

#include <mutex>
#include <unordered_map>

namespace WIYT
{
    class DFGBridge
    {
    public:
        static DFGBridge* GetSingleton();

        bool Initialize();
        bool IsAvailable() const;
        void SynchronizeAll();
        void SynchronizeTitle(
            std::string_view a_titleID,
            float a_progress,
            bool a_dispatch = true);
        void ClearRuntimeForms();
        std::optional<RE::FormID> GetGlobalFormID(
            std::string_view a_titleID) const;

        void RegisterResolvedGlobal(
            std::string_view a_titleID,
            std::string_view a_editorID,
            RE::FormID a_formID);
        void CompleteSynchronizationOperation();

    private:
        struct GlobalEntry
        {
            std::string editorID;
            RE::FormID formID = 0;
            float lastValue = -1.0f;
        };

        mutable std::mutex _lock;
        DFG::IDynamicFormsGenerator* _api = nullptr;
        std::unordered_map<std::string, GlobalEntry> _globals;
        bool _synchronizationQueued = false;
        std::size_t _pendingSynchronizationOperations = 0;
    };
}
