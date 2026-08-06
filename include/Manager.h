#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include "ClibUtil/editorID.hpp"

struct InternalFormInfo {
    RE::FormID formID;
    std::string editorID;
    std::string name;
    std::string pluginName;
    std::string formType;
    bool playable = true;

    // Helper for UI
    std::string GetDisplayName() const {
        if (!name.empty()) return name;
        if (!editorID.empty()) return editorID;
        return std::to_string(formID);
    }
};

class Manager {
public:
    static Manager* GetSingleton() {
        static Manager singleton;
        return &singleton;
    }

    void PopulateAllLists(bool forceRefresh = false);
    void RefreshLists(std::string_view a_signatures);
    std::uint64_t GetListRevision() const { return _listRevision; }
    static std::string ToUTF8(std::string_view a_str);
    // Data Store: Map of "TypeName" -> List of InternalFormInfo
    // We use this to feed the UI
    const std::vector<InternalFormInfo>& GetList(const std::string& typeName);
    std::optional<RE::FormID> FindFormIDByEditorID(
        std::string_view typeName,
        std::string_view editorID) const;

    // Register callback for when population is done
    void RegisterReadyCallback(std::function<void()> callback);

    void ConvertAllNPCOutfitsToInventory();

private:
    Manager() = default;

    template <typename T>
    void PopulateList(const std::string& a_typeName, std::function<bool(T*)> a_filter = nullptr);
    void PopulateCellList();
    void PopulateSpecialFilterLists();
    void RebuildEditorIDIndex(std::string_view typeName);
    static std::string NormalizeEditorID(std::string_view editorID);

    bool _isPopulated = false;
    std::uint64_t _listRevision = 0;
    std::map<std::string, std::vector<InternalFormInfo>> _dataStore;
    std::map<std::string, std::unordered_map<std::string, RE::FormID>> _formsByEditorID;
    std::vector<std::function<void()>> _readyCallbacks;
};
