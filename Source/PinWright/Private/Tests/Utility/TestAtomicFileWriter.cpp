// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && (PLATFORM_WINDOWS || PLATFORM_LINUX)

#include "Utils/AtomicFileWriter.h"
#include "Utils/AtomicFileWriterInternal.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#else
#include "HAL/PlatformFileManager.h"
#include <sys/stat.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicFileWriterFailurePreservesOriginalTest,
    "PinWright.utils.atomic_file_writer.FailurePreservesOriginal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAtomicFileWriterFailurePreservesOriginalTest::RunTest(const FString& Parameters)
{
    using namespace AtomicFileWriter;
    using namespace AtomicFileWriter::Private;

    FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()
        / TEXT("AtomicFileWriterTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FPaths::NormalizeDirectoryName(Root);
    const FString FinalPath = Root / TEXT("config.bin");
    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*Root, /*Tree=*/true);
    ON_SCOPE_EXIT
    {
        FileManager.DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const TArray<uint8> OriginalBytes = {
        0x7B, 0x0D, 0x0A, 0x20, 0x20, 0x22, 0x6B, 0x65, 0x65, 0x70, 0x22,
        0x3A, 0x20, 0x22, 0xE2, 0x9C, 0x93, 0x22, 0x0D, 0x0A, 0x7D
    };
    const TArray<uint8> ReplacementBytes = {
        0x7B, 0x0A, 0x20, 0x20, 0x22, 0x70, 0x69, 0x6E, 0x77, 0x72, 0x69,
        0x67, 0x68, 0x74, 0x22, 0x3A, 0x20, 0x74, 0x72, 0x75, 0x65, 0x0A, 0x7D
    };

    if (!TestTrue(TEXT("sentinel destination was seeded"),
            FFileHelper::SaveArrayToFile(OriginalBytes, *FinalPath)))
    {
        return false;
    }

    FString PartialTempPath;
    bool bPartialStageWasWritten = false;
    int32 PreReplaceCallsAfterStageFailure = 0;
    FOperations PartialStageFailure;
    PartialStageFailure.Stage = [&PartialTempPath, &bPartialStageWasWritten](
        const FString& TempPath, TArrayView<const uint8> Bytes)
    {
        PartialTempPath = TempPath;
        const int32 PartialSize = FMath::Max(1, Bytes.Num() / 2);
        bPartialStageWasWritten = FFileHelper::SaveArrayToFile(
            TArrayView64<const uint8>(Bytes.GetData(), PartialSize), *TempPath);
        return FStageResult { EStageStatus::IoError, TEXT("Injected partial stage failure") };
    };
    PartialStageFailure.PreReplace = [&PreReplaceCallsAfterStageFailure](
        const FString&, TFunction<void()>&)
    {
        ++PreReplaceCallsAfterStageFailure;
    };

    const FResult StageFailureResult = WriteBytesWithOperationsForTests(
        FinalPath, ReplacementBytes, EExistingFilePolicy::ReplaceExisting,
        PartialStageFailure);
    TestTrue(TEXT("partial stage failure returns IoError"),
        StageFailureResult.Status == EStatus::IoError);
    TestFalse(TEXT("partial stage failure reports no replacement"),
        StageFailureResult.bReplaced);
    TestFalse(TEXT("partial stage failure includes an error"),
        StageFailureResult.Error.IsEmpty());
    TestTrue(TEXT("injected stage wrote partial bytes before failing"),
        bPartialStageWasWritten);
    TestEqual(TEXT("pre-replacement is not reached after stage failure"),
        PreReplaceCallsAfterStageFailure, 0);

    TArray<uint8> ActualBytes;
    TestTrue(TEXT("destination remains readable after stage failure"),
        FFileHelper::LoadFileToArray(ActualBytes, *FinalPath));
    TestTrue(TEXT("stage failure preserves the exact original bytes"),
        ActualBytes == OriginalBytes);
    TestFalse(TEXT("partial temporary file is cleaned up"),
        FileManager.FileExists(*PartialTempPath));
    TestTrue(TEXT("partial temporary file is a sibling of the destination"),
        FPaths::GetPath(PartialTempPath) == FPaths::GetPath(FinalPath));
    TestTrue(TEXT("partial temporary path differs from the destination"),
        PartialTempPath != FinalPath);

    FString PublishTempPath;
    bool bPublishSawClosedCompleteStage = false;
    bool bPublishLockAcquired = false;
    bool bPublishLockReleased = false;
    int32 PreReplaceCalls = 0;
    FOperations PublishFailure;
#if PLATFORM_WINDOWS
    HANDLE LockedTempHandle = INVALID_HANDLE_VALUE;
    PublishFailure.PreReplace = [&PublishTempPath, &bPublishSawClosedCompleteStage,
        &bPublishLockAcquired, &bPublishLockReleased, &PreReplaceCalls, &LockedTempHandle,
        &ReplacementBytes](const FString& TempPath, TFunction<void()>& OutRelease)
    {
        ++PreReplaceCalls;
        PublishTempPath = TempPath;
        TArray<uint8> StagedBytes;
        bPublishSawClosedCompleteStage =
            FFileHelper::LoadFileToArray(StagedBytes, *TempPath)
            && StagedBytes == ReplacementBytes;

        LockedTempHandle = CreateFileW(*TempPath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, /*lpSecurityAttributes=*/nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, /*hTemplateFile=*/nullptr);
        bPublishLockAcquired = LockedTempHandle != INVALID_HANDLE_VALUE;
        OutRelease = [&bPublishLockReleased, &LockedTempHandle]()
        {
            if (LockedTempHandle != INVALID_HANDLE_VALUE)
            {
                bPublishLockReleased = CloseHandle(LockedTempHandle) != 0;
                LockedTempHandle = INVALID_HANDLE_VALUE;
            }
        };
    };
#else
    // rename(2) cannot put a file over a directory (EISDIR) for any user, root included, so
    // parking the destination and leaving a directory in its place fails the real publisher.
    const FString ParkedDestinationPath = FinalPath + TEXT(".parked");
    PublishFailure.PreReplace = [&PublishTempPath, &bPublishSawClosedCompleteStage,
        &bPublishLockAcquired, &bPublishLockReleased, &PreReplaceCalls, &ReplacementBytes,
        &FileManager, &FinalPath, &ParkedDestinationPath](
            const FString& TempPath, TFunction<void()>& OutRelease)
    {
        ++PreReplaceCalls;
        PublishTempPath = TempPath;
        TArray<uint8> StagedBytes;
        bPublishSawClosedCompleteStage =
            FFileHelper::LoadFileToArray(StagedBytes, *TempPath)
            && StagedBytes == ReplacementBytes;

        bPublishLockAcquired = FileManager.Move(*ParkedDestinationPath, *FinalPath)
            && FileManager.MakeDirectory(*FinalPath);
        OutRelease = [&bPublishLockReleased, &FileManager, &FinalPath, &ParkedDestinationPath]()
        {
            bPublishLockReleased =
                FileManager.DeleteDirectory(*FinalPath, /*RequireExists=*/true, /*Tree=*/false)
                && FileManager.Move(*FinalPath, *ParkedDestinationPath);
        };
    };
#endif

    const FResult PublishFailureResult = WriteBytesWithOperationsForTests(
        FinalPath, ReplacementBytes, EExistingFilePolicy::ReplaceExisting,
        PublishFailure);
    TestTrue(TEXT("publish failure returns IoError"),
        PublishFailureResult.Status == EStatus::IoError);
    TestFalse(TEXT("publish failure reports no replacement"),
        PublishFailureResult.bReplaced);
    TestFalse(TEXT("publish failure includes an error"),
        PublishFailureResult.Error.IsEmpty());
    TestEqual(TEXT("replacement hook runs once after the destination collision"),
        PreReplaceCalls, 1);
    TestTrue(TEXT("publish sees a closed and byte-complete staged file"),
        bPublishSawClosedCompleteStage);
#if PLATFORM_WINDOWS
    TestTrue(TEXT("failure comes from the real Win32 publisher"),
        PublishFailureResult.Error.Contains(TEXT("(error ")));
    TestTrue(TEXT("pre-replacement hook acquires a no-delete-sharing source lock"),
        bPublishLockAcquired);
    TestTrue(TEXT("source lock is released before temporary cleanup"),
        bPublishLockReleased);
#else
    TestTrue(TEXT("failure comes from the real rename(2) publisher"),
        PublishFailureResult.Error.Contains(TEXT("(error ")));
    TestTrue(TEXT("pre-replacement hook parks the destination behind a directory"),
        bPublishLockAcquired);
    TestTrue(TEXT("destination is restored before temporary cleanup"),
        bPublishLockReleased);
#endif

    ActualBytes.Reset();
    TestTrue(TEXT("destination remains readable after publish failure"),
        FFileHelper::LoadFileToArray(ActualBytes, *FinalPath));
    TestTrue(TEXT("publish failure preserves the exact original bytes"),
        ActualBytes == OriginalBytes);
    TestFalse(TEXT("publish-failure temporary file is cleaned up"),
        FileManager.FileExists(*PublishTempPath));
    TestTrue(TEXT("publish-failure temporary file is a sibling of the destination"),
        FPaths::GetPath(PublishTempPath) == FPaths::GetPath(FinalPath));
    TestTrue(TEXT("each attempt receives a unique temporary path"),
        PublishTempPath != PartialTempPath);

    FString CreateRetryTempPath;
    bool bDestinationDeletedBeforeReplacement = false;
    int32 CreateRetryPreReplaceCalls = 0;
    FOperations CreateRetry;
    CreateRetry.PreReplace = [&CreateRetryTempPath, &bDestinationDeletedBeforeReplacement,
        &CreateRetryPreReplaceCalls, &FileManager, &FinalPath](
            const FString& TempPath, TFunction<void()>&)
    {
        ++CreateRetryPreReplaceCalls;
        CreateRetryTempPath = TempPath;
        bDestinationDeletedBeforeReplacement = FileManager.Delete(
            *FinalPath, /*RequireExists=*/true, /*EvenReadOnly=*/false, /*Quiet=*/true);
    };

    const FResult CreateRetryResult = WriteBytesWithOperationsForTests(
        FinalPath, ReplacementBytes, EExistingFilePolicy::ReplaceExisting, CreateRetry);
    TestTrue(TEXT("destination disappearance retries as a successful creation"),
        CreateRetryResult.IsSuccess());
    TestFalse(TEXT("fresh-create retry does not report replacement"),
        CreateRetryResult.bReplaced);
    TestTrue(TEXT("pre-replacement hook deleted the collided destination"),
        bDestinationDeletedBeforeReplacement);
    TestEqual(TEXT("fresh-create retry needed one replacement hook call"),
        CreateRetryPreReplaceCalls, 1);
    TestFalse(TEXT("fresh-create retry consumes its staged temporary file"),
        FileManager.FileExists(*CreateRetryTempPath));
    ActualBytes.Reset();
    TestTrue(TEXT("fresh-create retry output is readable"),
        FFileHelper::LoadFileToArray(ActualBytes, *FinalPath));
    TestTrue(TEXT("fresh-create retry publishes the exact replacement bytes"),
        ActualBytes == ReplacementBytes);

    const FResult EmptyPathResult = WriteBytes(
        FString(), ReplacementBytes, EExistingFilePolicy::ReplaceExisting);
    TestTrue(TEXT("empty destination path returns IoError before normalization"),
        EmptyPathResult.Status == EStatus::IoError);
    TestFalse(TEXT("empty destination path includes an error"),
        EmptyPathResult.Error.IsEmpty());

    const FString BinaryPath = Root / TEXT("raw.bin");
    const TArray<uint8> FirstBinary = { 0x00, 0xFF, 0x7F, 0x00, 0x42 };
    const TArray<uint8> SecondBinary = { 0x13, 0x00, 0x37, 0xFE };

    const FResult FirstBinaryResult = WriteBytes(
        BinaryPath, FirstBinary, EExistingFilePolicy::FailIfExists);
    TestTrue(TEXT("raw binary create succeeds"), FirstBinaryResult.IsSuccess());
    TestFalse(TEXT("raw binary create did not replace a file"), FirstBinaryResult.bReplaced);

#if PLATFORM_WINDOWS
    const DWORD CreatedBinaryAttributes = GetFileAttributesW(*BinaryPath);
    TestTrue(TEXT("created raw binary has readable attributes"),
        CreatedBinaryAttributes != INVALID_FILE_ATTRIBUTES);
    if (CreatedBinaryAttributes != INVALID_FILE_ATTRIBUTES)
    {
        TestFalse(TEXT("created file does not retain FILE_ATTRIBUTE_TEMPORARY"),
            (CreatedBinaryAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0);
    }
#endif

    const FResult ExistingBinaryResult = WriteBytes(
        BinaryPath, SecondBinary, EExistingFilePolicy::FailIfExists);
    TestTrue(TEXT("fail-if-exists reports AlreadyExists"),
        ExistingBinaryResult.Status == EStatus::AlreadyExists);
    ActualBytes.Reset();
    TestTrue(TEXT("raw binary destination remains readable after AlreadyExists"),
        FFileHelper::LoadFileToArray(ActualBytes, *BinaryPath));
    TestTrue(TEXT("fail-if-exists preserves the first raw binary"),
        ActualBytes == FirstBinary);

    const FResult ReplaceBinaryResult = WriteBytes(
        BinaryPath, SecondBinary, EExistingFilePolicy::ReplaceExisting);
    TestTrue(TEXT("raw binary replacement succeeds"), ReplaceBinaryResult.IsSuccess());
    TestTrue(TEXT("raw binary replacement reports bReplaced"),
        ReplaceBinaryResult.bReplaced);
    ActualBytes.Reset();
    TestTrue(TEXT("replaced raw binary is readable"),
        FFileHelper::LoadFileToArray(ActualBytes, *BinaryPath));
    TestTrue(TEXT("raw binary bytes round-trip exactly"),
        ActualBytes == SecondBinary);

    const FString Utf8Path = Root / TEXT("utf8.txt");
    const TArray<uint8> ExpectedUtf8 = { 0x41, 0xE2, 0x9C, 0x93 };
    const FResult Utf8Result = WriteUtf8(
        Utf8Path, FStringView(TEXT("A\u2713")), EExistingFilePolicy::FailIfExists);
    TestTrue(TEXT("UTF-8 without BOM write succeeds"), Utf8Result.IsSuccess());
    ActualBytes.Reset();
    TestTrue(TEXT("UTF-8 output is readable"),
        FFileHelper::LoadFileToArray(ActualBytes, *Utf8Path));
    TestTrue(TEXT("UTF-8 output has exact bytes and no BOM"),
        ActualBytes == ExpectedUtf8);

    const FString EmptyUtf8Path = Root / TEXT("empty-utf8.txt");
    const FResult EmptyUtf8Result = WriteUtf8(
        EmptyUtf8Path, FStringView(), EExistingFilePolicy::FailIfExists);
    TestTrue(TEXT("empty UTF-8 write succeeds"), EmptyUtf8Result.IsSuccess());
    ActualBytes.Reset();
    TestTrue(TEXT("empty UTF-8 output is readable"),
        FFileHelper::LoadFileToArray(ActualBytes, *EmptyUtf8Path));
    TestEqual(TEXT("empty UTF-8 output contains zero bytes"), ActualBytes.Num(), 0);

    return true;
}

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#else

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicFileWriterReplacementKeepsPermissionsTest,
    "PinWright.utils.atomic_file_writer.ReplacementKeepsDestinationPermissions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAtomicFileWriterReplacementKeepsPermissionsTest::RunTest(const FString& Parameters)
{
    using namespace AtomicFileWriter;

    FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()
        / TEXT("AtomicFileWriterTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FPaths::NormalizeDirectoryName(Root);
    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*Root, /*Tree=*/true);
    ON_SCOPE_EXIT
    {
        FileManager.DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString PrivatePath = Root / TEXT("private.json");
    const TArray<uint8> FirstBytes = { 0x7B, 0x7D };
    const TArray<uint8> SecondBytes = { 0x7B, 0x0A, 0x7D };
    if (!TestTrue(TEXT("private destination was created"),
            WriteBytes(PrivatePath, FirstBytes, EExistingFilePolicy::FailIfExists).IsSuccess()))
    {
        return false;
    }
    TestEqual(TEXT("destination narrowed to 0600"),
        chmod(TCHAR_TO_UTF8(*PrivatePath), S_IRUSR | S_IWUSR), 0);

    const FResult ReplaceResult = WriteBytes(
        PrivatePath, SecondBytes, EExistingFilePolicy::ReplaceExisting);
    TestTrue(TEXT("private destination replacement succeeds"), ReplaceResult.IsSuccess());
    TestTrue(TEXT("private destination replacement reports bReplaced"), ReplaceResult.bReplaced);

    struct stat ReplacedInfo;
    if (TestEqual(TEXT("replaced destination can be stat'ed"),
            stat(TCHAR_TO_UTF8(*PrivatePath), &ReplacedInfo), 0))
    {
        TestEqual(TEXT("replacement keeps the destination's 0600 permission bits"),
            static_cast<int32>(ReplacedInfo.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)),
            static_cast<int32>(S_IRUSR | S_IWUSR));
    }
    TArray<uint8> ActualBytes;
    TestTrue(TEXT("replaced private destination is readable"),
        FFileHelper::LoadFileToArray(ActualBytes, *PrivatePath));
    TestTrue(TEXT("replaced private destination has the new bytes"), ActualBytes == SecondBytes);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtomicFileWriterReadOnlyDestinationTest,
    "PinWright.utils.atomic_file_writer.ReadOnlyDestinationIsNotReplaced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAtomicFileWriterReadOnlyDestinationTest::RunTest(const FString& Parameters)
{
    using namespace AtomicFileWriter;

    FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir()
        / TEXT("AtomicFileWriterTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FPaths::NormalizeDirectoryName(Root);
    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*Root, /*Tree=*/true);
    const FString ReadOnlyPath = Root / TEXT("locked.bin");
    ON_SCOPE_EXIT
    {
        FileManager.Delete(*ReadOnlyPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
        FileManager.DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const TArray<uint8> OriginalBytes = { 0x01, 0x02, 0x03 };
    const TArray<uint8> ReplacementBytes = { 0x04 };
    if (!TestTrue(TEXT("read-only destination was created"),
            WriteBytes(ReadOnlyPath, OriginalBytes, EExistingFilePolicy::FailIfExists).IsSuccess()))
    {
        return false;
    }
    TestTrue(TEXT("destination marked read-only"),
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ReadOnlyPath, true));

    const FResult ReplaceResult = WriteBytes(
        ReadOnlyPath, ReplacementBytes, EExistingFilePolicy::ReplaceExisting);
    TestTrue(TEXT("read-only destination returns IoError"),
        ReplaceResult.Status == EStatus::IoError);
    TestFalse(TEXT("read-only destination reports no replacement"), ReplaceResult.bReplaced);
    TestTrue(TEXT("refusal names the errno"), ReplaceResult.Error.Contains(TEXT("(error ")));

    TArray<uint8> ActualBytes;
    TestTrue(TEXT("read-only destination stays readable"),
        FFileHelper::LoadFileToArray(ActualBytes, *ReadOnlyPath));
    TestTrue(TEXT("read-only destination keeps its original bytes"), ActualBytes == OriginalBytes);

    TArray<FString> Leftovers;
    FileManager.FindFiles(Leftovers, *(Root / TEXT("*.tmp")), /*Files=*/true, /*Directories=*/false);
    TestEqual(TEXT("refused replacement leaves no temporary file"), Leftovers.Num(), 0);
    return true;
}

#endif

#endif // WITH_DEV_AUTOMATION_TESTS && (PLATFORM_WINDOWS || PLATFORM_LINUX)
