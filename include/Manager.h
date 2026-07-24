#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include "ClibUtil/editorID.hpp"

struct InternalFormInfo {
    RE::FormID formID;
    std::string editorID;
    std::string name;
    std::string pluginName;
    std::string formType;

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
    static std::string ToUTF8(std::string_view a_str);
    // Data Store: Map of "TypeName" -> List of InternalFormInfo
    // We use this to feed the UI
    const std::vector<InternalFormInfo>& GetList(const std::string& typeName);

    // Register callback for when population is done
    void RegisterReadyCallback(std::function<void()> callback);

    void ConvertAllNPCOutfitsToInventory();

private:
    Manager() = default;

    template <typename T>
    void PopulateList(const std::string& a_typeName, std::function<bool(T*)> a_filter = nullptr);

    bool _isPopulated = false;
    std::map<std::string, std::vector<InternalFormInfo>> _dataStore;
    std::vector<std::function<void()>> _readyCallbacks;
};
