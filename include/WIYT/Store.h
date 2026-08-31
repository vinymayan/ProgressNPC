#pragma once

#include "WIYT/Model.h"

#include <mutex>
#include <set>
#include <unordered_map>

namespace WIYT
{
    class Store
    {
    public:
        static constexpr std::string_view kLocalPackageID =
            "wiyt.local-titles";
        static constexpr int kSchemaVersion = 2;

        static Store* GetSingleton();

        bool Load();
        bool SaveAll();
        std::optional<std::string> CreatePackage(
            std::string_view a_displayName);
        TitleDefinition& CreateTitle(std::string_view a_packageID);
        bool MarkTitleForDeletion(std::string_view a_titleID);
        bool CancelTitleDeletion(std::string_view a_titleID);
        bool MarkPackageForDeletion(std::string_view a_packageID);
        bool CancelPackageDeletion(std::string_view a_packageID);
        bool IsTitlePendingDeletion(std::string_view a_titleID) const;
        bool IsPackagePendingDeletion(std::string_view a_packageID) const;

        std::vector<TitleDefinition>& Titles() { return _titles; }
        const std::vector<TitleDefinition>& Titles() const { return _titles; }
        const std::vector<Package>& Packages() const { return _packages; }
        TitleDefinition* FindTitle(std::string_view a_titleID);
        const TitleDefinition* FindTitle(
            std::string_view a_titleID) const;

    private:
        bool EnsurePackage(Package& a_package);
        bool LoadPackage(const Package& a_package);
        bool SaveTitle(TitleDefinition& a_title);
        bool DeleteTitleNow(std::string_view a_titleID);
        bool DeletePackageNow(std::string_view a_packageID);
        void RebuildIndices();

        mutable std::mutex _lock;
        std::vector<Package> _packages;
        std::vector<TitleDefinition> _titles;
        std::unordered_map<std::string, std::size_t> _titleIndices;
        std::set<std::string> _titlesToDelete;
        std::set<std::string> _packagesToDelete;
    };
}
