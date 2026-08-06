#include "WIYT/DFGBridge.h"

#include "WIYT/State.h"
#include "WIYT/Store.h"

#include <memory>

namespace WIYT
{
    namespace
    {
        constexpr std::size_t kBatchLimit = 10000;

        struct LookupContext
        {
            std::vector<std::string> titleIDs;
            std::vector<std::string> editorIDs;
            std::vector<DFG::LookupFormRequest> requests;
        };

        struct CreateContext
        {
            std::vector<std::string> titleIDs;
            std::vector<std::string> editorIDs;
            std::vector<std::string> json;
            std::vector<DFG::CreateFormRequest> requests;
        };

        void DispatchProgress(
            const char* a_eventName,
            const std::string_view a_argument,
            const float a_value,
            RE::TESGlobal* a_global)
        {
            auto* source = SKSE::GetModCallbackEventSource();
            if (!source) {
                return;
            }
            const std::string argument(a_argument);
            SKSE::ModCallbackEvent event{
                RE::BSFixedString(a_eventName),
                RE::BSFixedString(argument.c_str()),
                a_value,
                a_global
            };
            source->SendEvent(std::addressof(event));
        }

        void OnCreateBatch(
            const DFG::BatchOperationResult* a_result,
            void* a_userData)
        {
            std::unique_ptr<CreateContext> context(
                static_cast<CreateContext*>(a_userData));
            if (!a_result) {
                DFGBridge::GetSingleton()->
                    CompleteSynchronizationOperation();
                return;
            }
            const auto count = std::min<std::size_t>(
                a_result->resultCount,
                context->titleIDs.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto& result = a_result->results[index];
                if (result.status != DFG::Status::kSuccess ||
                    result.formID == 0) {
                    logger::error(
                        "[WIYT DFG] Could not create Global '{}': {}",
                        context->editorIDs[index],
                        result.error);
                    continue;
                }
                auto* global = result.form ?
                    result.form->As<RE::TESGlobal>() :
                    RE::TESForm::LookupByID<RE::TESGlobal>(
                        result.formID);
                if (!global) {
                    logger::error(
                        "[WIYT DFG] '{}' was created as a non-Global form.",
                        context->editorIDs[index]);
                    continue;
                }
                DFGBridge::GetSingleton()->RegisterResolvedGlobal(
                    context->titleIDs[index],
                    context->editorIDs[index],
                    result.formID);
            }
            DFGBridge::GetSingleton()->
                CompleteSynchronizationOperation();
        }

        void QueueCreates(
            std::vector<std::string> a_titleIDs,
            std::vector<std::string> a_editorIDs)
        {
            auto* api = DFG::GetAPI();
            if (!api || !api->IsReady() || a_titleIDs.empty()) {
                DFGBridge::GetSingleton()->
                    CompleteSynchronizationOperation();
                return;
            }
            auto context = std::make_unique<CreateContext>();
            context->titleIDs = std::move(a_titleIDs);
            context->editorIDs = std::move(a_editorIDs);
            context->json.reserve(context->editorIDs.size());
            context->requests.resize(context->editorIDs.size());
            for (std::size_t index = 0;
                 index < context->editorIDs.size();
                 ++index) {
                context->json.push_back(std::format(
                    "{{\"formKind\":\"Global\","
                    "\"sourceSignature\":\"GLOB\","
                    "\"editorId\":\"{}\","
                    "\"globalType\":\"float\","
                    "\"defaultValue\":0.0}}",
                    context->editorIDs[index]));
            }
            for (std::size_t index = 0;
                 index < context->requests.size();
                 ++index) {
                auto& request = context->requests[index];
                request.requester = "WIYT";
                request.packageName = "WIYT_Public_Globals";
                request.formJson = context->json[index].c_str();
            }
            DFG::CreateFormsRequest request;
            request.requests = context->requests.data();
            request.requestCount = static_cast<std::uint32_t>(
                context->requests.size());
            auto* raw = context.release();
            if (!api->QueueCreateForms(
                    &request,
                    OnCreateBatch,
                    raw)) {
                delete raw;
                logger::error(
                    "[WIYT DFG] DFG rejected the Global creation batch.");
                DFGBridge::GetSingleton()->
                    CompleteSynchronizationOperation();
            }
        }

        void OnLookupBatch(
            const DFG::BatchLookupResult* a_result,
            void* a_userData)
        {
            std::unique_ptr<LookupContext> context(
                static_cast<LookupContext*>(a_userData));
            if (!a_result) {
                DFGBridge::GetSingleton()->
                    CompleteSynchronizationOperation();
                return;
            }
            std::vector<std::string> missingTitles;
            std::vector<std::string> missingEditors;
            const auto count = std::min<std::size_t>(
                a_result->resultCount,
                context->titleIDs.size());
            for (std::size_t index = 0; index < count; ++index) {
                const auto& result = a_result->results[index];
                if (result.status != DFG::Status::kSuccess) {
                    logger::error(
                        "[WIYT DFG] Lookup of '{}' failed: {}",
                        context->editorIDs[index],
                        result.error);
                    continue;
                }
                if (!result.exists) {
                    missingTitles.push_back(
                        context->titleIDs[index]);
                    missingEditors.push_back(
                        context->editorIDs[index]);
                    continue;
                }
                auto* global = result.form ?
                    result.form->As<RE::TESGlobal>() :
                    RE::TESForm::LookupByID<RE::TESGlobal>(
                        result.formID);
                if (!global) {
                    logger::error(
                        "[WIYT DFG] EditorID '{}' already belongs to a "
                        "non-Global form.",
                        context->editorIDs[index]);
                    continue;
                }
                DFGBridge::GetSingleton()->RegisterResolvedGlobal(
                    context->titleIDs[index],
                    context->editorIDs[index],
                    global->GetFormID());
            }
            QueueCreates(
                std::move(missingTitles),
                std::move(missingEditors));
        }
    }

    DFGBridge* DFGBridge::GetSingleton()
    {
        static DFGBridge singleton;
        return std::addressof(singleton);
    }

    bool DFGBridge::Initialize()
    {
        std::scoped_lock lock(_lock);
        _api = DFG::GetAPI();
        return _api && _api->IsReady();
    }

    bool DFGBridge::IsAvailable() const
    {
        std::scoped_lock lock(_lock);
        return _api && _api->IsReady();
    }

    void DFGBridge::SynchronizeAll()
    {
        if (!Initialize()) {
            return;
        }
        const auto& titles = Store::GetSingleton()->Titles();
        std::vector<std::string> missingTitles;
        std::vector<std::string> missingEditors;
        {
            std::scoped_lock lock(_lock);
            if (_synchronizationQueued) {
                return;
            }
            for (const auto& title : titles) {
                if (!title.enabled ||
                    title.publicGlobalEditorID.empty()) {
                    continue;
                }
                const auto found = _globals.find(title.id);
                if (found == _globals.end() ||
                    found->second.editorID !=
                        title.publicGlobalEditorID ||
                    !RE::TESForm::LookupByID<RE::TESGlobal>(
                        found->second.formID)) {
                    missingTitles.push_back(title.id);
                    missingEditors.push_back(
                        title.publicGlobalEditorID);
                }
            }
            if (!missingTitles.empty()) {
                _synchronizationQueued = true;
                _pendingSynchronizationOperations =
                    (missingTitles.size() + kBatchLimit - 1) /
                    kBatchLimit;
            }
        }

        if (!missingTitles.empty()) {
            for (std::size_t offset = 0;
                 offset < missingTitles.size();
                 offset += kBatchLimit) {
                const auto count = std::min(
                    kBatchLimit,
                    missingTitles.size() - offset);
                auto context = std::make_unique<LookupContext>();
                context->titleIDs.assign(
                    missingTitles.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    missingTitles.begin() +
                        static_cast<std::ptrdiff_t>(offset + count));
                context->editorIDs.assign(
                    missingEditors.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    missingEditors.begin() +
                        static_cast<std::ptrdiff_t>(offset + count));
                context->requests.resize(count);
                for (std::size_t index = 0;
                     index < count;
                     ++index) {
                    context->requests[index].requester = "WIYT";
                    context->requests[index].editorId =
                        context->editorIDs[index].c_str();
                }
                DFG::LookupFormsRequest request;
                request.requests = context->requests.data();
                request.requestCount =
                    static_cast<std::uint32_t>(count);
                auto* raw = context.release();
                if (!_api->QueueLookupForms(
                        &request,
                        OnLookupBatch,
                        raw)) {
                    delete raw;
                    logger::error(
                        "[WIYT DFG] DFG rejected the Global lookup batch.");
                    CompleteSynchronizationOperation();
                }
            }
            return;
        }

        {
            std::scoped_lock lock(_lock);
            _synchronizationQueued = false;
            _pendingSynchronizationOperations = 0;
        }
        for (const auto& title : titles) {
            const auto progress =
                State::GetSingleton()->GetTitleProgress(title.id);
            SynchronizeTitle(
                title.id,
                progress ? progress->overallProgress : 0.0f,
                false);
        }
    }

    void DFGBridge::SynchronizeTitle(
        const std::string_view a_titleID,
        const float a_progress,
        const bool a_dispatch)
    {
        RE::FormID formID = 0;
        std::string editorID;
        float previous = -1.0f;
        {
            std::scoped_lock lock(_lock);
            const auto found =
                _globals.find(std::string(a_titleID));
            if (found == _globals.end()) {
                return;
            }
            formID = found->second.formID;
            editorID = found->second.editorID;
            previous = found->second.lastValue;
            found->second.lastValue =
                std::clamp(a_progress, 0.0f, 1.0f);
        }
        auto* global =
            RE::TESForm::LookupByID<RE::TESGlobal>(formID);
        if (!global) {
            return;
        }
        const auto value = std::clamp(a_progress, 0.0f, 1.0f);
        global->value = value;
        if (a_dispatch &&
            std::abs(previous - value) > 0.0001f) {
            DispatchProgress(
                "WIYTTitleProgressUpdated",
                editorID,
                value,
                global);
            if (value >= 1.0f && previous < 1.0f) {
                DispatchProgress(
                    "WIYTTitleEarned",
                    a_titleID,
                    1.0f,
                    global);
            }
        }
    }

    void DFGBridge::RegisterResolvedGlobal(
        const std::string_view a_titleID,
        const std::string_view a_editorID,
        const RE::FormID a_formID)
    {
        {
            std::scoped_lock lock(_lock);
            _globals[std::string(a_titleID)] = {
                std::string(a_editorID),
                a_formID,
                -1.0f
            };
        }
        const auto progress =
            State::GetSingleton()->GetTitleProgress(a_titleID);
        SynchronizeTitle(
            a_titleID,
            progress ? progress->overallProgress : 0.0f,
            false);
    }

    void DFGBridge::CompleteSynchronizationOperation()
    {
        bool finished = false;
        {
            std::scoped_lock lock(_lock);
            if (_pendingSynchronizationOperations > 0) {
                --_pendingSynchronizationOperations;
            }
            if (_pendingSynchronizationOperations == 0) {
                _synchronizationQueued = false;
                finished = true;
            }
        }
        if (!finished) {
            return;
        }
        for (const auto& title :
             Store::GetSingleton()->Titles()) {
            const auto progress =
                State::GetSingleton()->GetTitleProgress(title.id);
            SynchronizeTitle(
                title.id,
                progress ? progress->overallProgress : 0.0f,
                false);
        }
    }

    void DFGBridge::ClearRuntimeForms()
    {
        std::scoped_lock lock(_lock);
        _globals.clear();
        _synchronizationQueued = false;
        _pendingSynchronizationOperations = 0;
    }

    std::optional<RE::FormID> DFGBridge::GetGlobalFormID(
        const std::string_view a_titleID) const
    {
        std::scoped_lock lock(_lock);
        const auto found =
            _globals.find(std::string(a_titleID));
        return found != _globals.end() ?
            std::optional<RE::FormID>(found->second.formID) :
            std::nullopt;
    }
}
