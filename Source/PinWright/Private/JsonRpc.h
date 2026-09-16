// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace JsonRpc
{
    static constexpr int32 kParseError     = -32700;
    static constexpr int32 kInvalidRequest = -32600;
    static constexpr int32 kMethodNotFound = -32601;
    static constexpr int32 kInvalidParams  = -32602;
    static constexpr int32 kInternalError  = -32603;

    struct FEnvelope
    {
        // Null for notifications; otherwise preserves the original id type (string, number, or explicit null).
        TSharedPtr<FJsonValue> Id;
        FString Method;
        TSharedPtr<FJsonObject> Params;
        bool bHasParams = false;
        bool bIsNotification = false;
    };

    inline bool ParseEnvelope(const TSharedPtr<FJsonObject>& Root, FEnvelope& Out, FString& OutParseError)
    {
        if (!Root.IsValid())
        {
            OutParseError = TEXT("Request object is null");
            return false;
        }

        FString JsonRpcVersion;
        if (!Root->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion) || JsonRpcVersion != TEXT("2.0"))
        {
            OutParseError = TEXT("Missing or invalid 'jsonrpc' field (must be \"2.0\")");
            return false;
        }

        if (!Root->TryGetStringField(TEXT("method"), Out.Method))
        {
            OutParseError = TEXT("Missing 'method' field");
            return false;
        }

        // Absent id = notification. An explicit null id is a malformed request per spec
        // for responses, but the spec permits any JSON value here, so we accept it.
        if (Root->HasField(TEXT("id")))
        {
            Out.Id = Root->TryGetField(TEXT("id"));
            Out.bIsNotification = false;
        }
        else
        {
            Out.Id = nullptr;
            Out.bIsNotification = true;
        }

        Out.bHasParams = Root->HasField(TEXT("params"));
        if (Out.bHasParams)
        {
            const TSharedPtr<FJsonValue> ParamsVal = Root->TryGetField(TEXT("params"));
            if (ParamsVal.IsValid() && ParamsVal->Type == EJson::Object)
            {
                Out.Params = ParamsVal->AsObject();
            }
            if (!Out.Params.IsValid())
            {
                Out.Params = MakeShared<FJsonObject>();
            }
        }
        else
        {
            Out.Params = MakeShared<FJsonObject>();
        }

        return true;
    }

    inline TSharedPtr<FJsonObject> BuildResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonValue>& Result)
    {
        // Notifications get no reply.
        if (!Id.IsValid())
        {
            return nullptr;
        }

        auto Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Resp->SetField(TEXT("id"), Id);
        Resp->SetField(TEXT("result"), Result.IsValid() ? Result : MakeShared<FJsonValueNull>());
        return Resp;
    }

    inline TSharedPtr<FJsonObject> BuildResponse(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& ResultObj)
    {
        const TSharedPtr<FJsonValue> Wrapped = ResultObj.IsValid()
            ? MakeShared<FJsonValueObject>(ResultObj)
            : MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
        return BuildResponse(Id, Wrapped);
    }

    inline TSharedPtr<FJsonObject> BuildErrorResponse(
        const TSharedPtr<FJsonValue>& Id,
        int32 Code,
        const FString& Message,
        const TSharedPtr<FJsonValue>& Data = nullptr)
    {
        auto ErrObj = MakeShared<FJsonObject>();
        ErrObj->SetNumberField(TEXT("code"), Code);
        ErrObj->SetStringField(TEXT("message"), Message);
        if (Data.IsValid())
        {
            ErrObj->SetField(TEXT("data"), Data);
        }

        auto Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        // Spec: id is null when the request id couldn't be determined (e.g. parse error).
        Resp->SetField(TEXT("id"), Id.IsValid() ? Id : MakeShared<FJsonValueNull>());
        Resp->SetObjectField(TEXT("error"), ErrObj);
        return Resp;
    }

    // CONDENSED, and the policy is written out rather than left to TJsonWriterFactory<>'s default.
    //
    // The default IS TPrettyJsonPrintPolicy - tab indent, CRLF line ends, a space after every
    // colon - and this function's output is not a file anyone opens. It becomes `content[0].text`
    // of an MCP tools/call result (Transport/McpRequestCore.cpp), where it is then JSON-ESCAPED
    // into a string: every tab becomes `\t` and every CRLF becomes `\r\n`, so the whitespace does
    // not merely inflate the payload, it inflates it at two characters per character.
    //
    // MEASURED, on 147 real spilled capture responses in a host project: a 4,618-character
    // response body was reaching the size gate as 14,228 characters, and 92 of 92 single-still
    // captures spilled to disk. Roughly a fifth of that excess was this line. The transport has
    // always written the wire body condensed (SocketHttpServer.cpp:72), so nothing downstream ever
    // wanted the pretty form - it was a default nobody chose.
    inline FString Serialize(const TSharedPtr<FJsonObject>& Obj)
    {
        FString Out;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
        Writer->Close();
        return Out;
    }
}
