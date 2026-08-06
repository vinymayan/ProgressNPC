#pragma once

#include "RE/Skyrim.h"

#include <Windows.h>
#include <cstdint>

namespace WIYT::DFG
{
    inline constexpr std::uint32_t kInterfaceVersion = 1;

    enum class Operation : std::uint32_t
    {
        kCreate = 1,
        kUpdate = 2,
        kDelete = 3,
        kLookup = 4
    };

    enum class Status : std::uint32_t
    {
        kSuccess = 0,
        kNotReady,
        kInvalidArgument,
        kInvalidJson,
        kMissingEditorId,
        kInvalidEditorId,
        kMissingPackageName,
        kInvalidPackageName,
        kMissingFormKind,
        kUnsupportedFormKind,
        kEditorIdAlreadyExists,
        kEditorIdReserved,
        kEditorIdNotFound,
        kEditorIdMismatch,
        kFormKindMismatch,
        kProtectedField,
        kDPFUnavailable,
        kDPFCreateFailed,
        kConfigureFailed,
        kPersistenceFailed,
        kDPFReleaseFailed,
        kInternalError,
        kBatchPartialSuccess,
        kBatchFailed
    };

    struct CreateFormRequest
    {
        std::uint32_t structSize{ sizeof(CreateFormRequest) };
        const char* requester = nullptr;
        const char* packageName = nullptr;
        const char* formJson = nullptr;
    };

    struct UpdateFormRequest
    {
        std::uint32_t structSize{ sizeof(UpdateFormRequest) };
        const char* requester = nullptr;
        const char* editorId = nullptr;
        const char* patchJson = nullptr;
    };

    struct DeleteFormRequest
    {
        std::uint32_t structSize{ sizeof(DeleteFormRequest) };
        const char* requester = nullptr;
        const char* editorId = nullptr;
    };

    struct LookupFormRequest
    {
        std::uint32_t structSize{ sizeof(LookupFormRequest) };
        const char* requester = nullptr;
        const char* editorId = nullptr;
    };

    struct CreateFormsRequest
    {
        std::uint32_t structSize{ sizeof(CreateFormsRequest) };
        const CreateFormRequest* requests = nullptr;
        std::uint32_t requestCount = 0;
    };

    struct UpdateFormsRequest
    {
        std::uint32_t structSize{ sizeof(UpdateFormsRequest) };
        const UpdateFormRequest* requests = nullptr;
        std::uint32_t requestCount = 0;
    };

    struct DeleteFormsRequest
    {
        std::uint32_t structSize{ sizeof(DeleteFormsRequest) };
        const DeleteFormRequest* requests = nullptr;
        std::uint32_t requestCount = 0;
    };

    struct LookupFormsRequest
    {
        std::uint32_t structSize{ sizeof(LookupFormsRequest) };
        const LookupFormRequest* requests = nullptr;
        std::uint32_t requestCount = 0;
    };

    struct FormOperationResult
    {
        std::uint32_t structSize{ sizeof(FormOperationResult) };
        Operation operation{ Operation::kCreate };
        Status status{ Status::kInternalError };
        RE::TESForm* form = nullptr;
        RE::FormID formID = 0;
        std::uint32_t pluginNumber = 0;
        std::uint32_t localId = 0;
        std::uint8_t recoveredExistingSlot = 0;
        char editorId[128]{};
        char packageName[128]{};
        char pluginName[64]{};
        char error[256]{};
    };

    using FormOperationCallback =
        void (*)(const FormOperationResult*, void*);

    struct BatchOperationResult
    {
        std::uint32_t structSize{ sizeof(BatchOperationResult) };
        Operation operation{ Operation::kCreate };
        Status status{ Status::kBatchFailed };
        const FormOperationResult* results = nullptr;
        std::uint32_t resultCount = 0;
        std::uint32_t successCount = 0;
        std::uint32_t failureCount = 0;
        char updatedSignatures[256]{};
        char error[256]{};
    };

    using BatchOperationCallback =
        void (*)(const BatchOperationResult*, void*);

    struct FormLookupResult
    {
        std::uint32_t structSize{ sizeof(FormLookupResult) };
        Status status{ Status::kInternalError };
        std::uint8_t exists = 0;
        RE::TESForm* form = nullptr;
        RE::FormID formID = 0;
        std::uint32_t pluginNumber = 0;
        std::uint32_t localId = 0;
        char editorId[128]{};
        char packageName[128]{};
        char pluginName[64]{};
        char formKind[64]{};
        char sourceSignature[16]{};
        const char* formJson = nullptr;
        std::uint32_t formJsonLength = 0;
        char error[256]{};
    };

    struct BatchLookupResult
    {
        std::uint32_t structSize{ sizeof(BatchLookupResult) };
        Status status{ Status::kBatchFailed };
        const FormLookupResult* results = nullptr;
        std::uint32_t resultCount = 0;
        std::uint32_t foundCount = 0;
        std::uint32_t missingCount = 0;
        std::uint32_t failureCount = 0;
        char error[256]{};
    };

    using FormLookupCallback =
        void (*)(const FormLookupResult*, void*);
    using BatchLookupCallback =
        void (*)(const BatchLookupResult*, void*);

    class IDynamicFormsGenerator
    {
    public:
        virtual ~IDynamicFormsGenerator() = default;
        virtual std::uint32_t GetVersion() const noexcept = 0;
        virtual bool IsReady() const noexcept = 0;
        virtual bool QueueCreateForm(
            const CreateFormRequest*,
            FormOperationCallback,
            void*) noexcept = 0;
        virtual bool QueueUpdateForm(
            const UpdateFormRequest*,
            FormOperationCallback,
            void*) noexcept = 0;
        virtual bool QueueDeleteForm(
            const DeleteFormRequest*,
            FormOperationCallback,
            void*) noexcept = 0;
        virtual bool QueueCreateForms(
            const CreateFormsRequest*,
            BatchOperationCallback,
            void*) noexcept = 0;
        virtual bool QueueUpdateForms(
            const UpdateFormsRequest*,
            BatchOperationCallback,
            void*) noexcept = 0;
        virtual bool QueueDeleteForms(
            const DeleteFormsRequest*,
            BatchOperationCallback,
            void*) noexcept = 0;
        virtual bool QueueLookupForm(
            const LookupFormRequest*,
            FormLookupCallback,
            void*) noexcept = 0;
        virtual bool QueueLookupForms(
            const LookupFormsRequest*,
            BatchLookupCallback,
            void*) noexcept = 0;
    };

    inline IDynamicFormsGenerator* GetAPI() noexcept
    {
        const auto module =
            GetModuleHandleA("DynamicFormsGenerator.dll");
        if (!module) {
            return nullptr;
        }
        using Getter = void* (*)();
        const auto getter = reinterpret_cast<Getter>(
            GetProcAddress(
                module,
                "GetDynamicFormsGeneratorAPI"));
        if (!getter) {
            return nullptr;
        }
        auto* api =
            static_cast<IDynamicFormsGenerator*>(getter());
        return api &&
                       api->GetVersion() == kInterfaceVersion ?
            api :
            nullptr;
    }
}
