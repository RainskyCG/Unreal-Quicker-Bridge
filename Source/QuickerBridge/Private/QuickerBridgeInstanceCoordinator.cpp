#include "QuickerBridgeInstanceCoordinator.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "HttpServerModule.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace QuickerBridgeInstanceCoordinator
{
    namespace
    {
        constexpr float RetryIntervalSeconds = 0.05f;
        constexpr int32 MaxRetryCount = 10;

#if PLATFORM_WINDOWS
        HWINEVENTHOOK ForegroundEventHook = nullptr;
        HANDLE PortOwnerMutex = nullptr;
#endif

        FTSTicker::FDelegateHandle RetryTickerHandle;
        TAtomic<bool> bRunning(false);
        bool bOwnsPortMutex = false;
        int32 RetryCount = 0;

        void CancelRetry()
        {
            if (RetryTickerHandle.IsValid())
            {
                FTSTicker::GetCoreTicker().RemoveTicker(RetryTickerHandle);
                RetryTickerHandle.Reset();
            }
            RetryCount = 0;
        }

        void StopHttpAndReleasePort()
        {
            CancelRetry();

            if (!bOwnsPortMutex)
            {
                return;
            }

            if (FHttpServerModule::IsAvailable())
            {
                FHttpServerModule::Get().StopAllListeners();
            }

#if PLATFORM_WINDOWS
            if (PortOwnerMutex)
            {
                ::ReleaseMutex(PortOwnerMutex);
            }
#endif

            bOwnsPortMutex = false;
            UE_LOG(LogTemp, Display, TEXT("QuickerBridge: Released shared HTTP port 18080."));
        }

        bool TryClaimPort()
        {
            if (!bRunning.Load())
            {
                return false;
            }

            if (bOwnsPortMutex)
            {
                return true;
            }

#if PLATFORM_WINDOWS
            if (!PortOwnerMutex)
            {
                return false;
            }

            const DWORD WaitResult = ::WaitForSingleObject(PortOwnerMutex, 0);
            if (WaitResult != WAIT_OBJECT_0 && WaitResult != WAIT_ABANDONED)
            {
                return false;
            }
#endif

            bOwnsPortMutex = true;
            FHttpServerModule::Get().StartAllListeners();
            UE_LOG(LogTemp, Display, TEXT("QuickerBridge: Claimed shared HTTP port 18080."));
            return true;
        }

        void StartLimitedRetry()
        {
            if (RetryTickerHandle.IsValid() || !bRunning.Load())
            {
                return;
            }

            RetryCount = 0;
            RetryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateLambda([](float DeltaTime)
                {
                    if (!bRunning.Load() || TryClaimPort())
                    {
                        RetryTickerHandle.Reset();
                        RetryCount = 0;
                        return false;
                    }

                    ++RetryCount;
                    if (RetryCount >= MaxRetryCount)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("QuickerBridge: Failed to claim shared HTTP port 18080 after %d retries."), RetryCount);
                        RetryTickerHandle.Reset();
                        RetryCount = 0;
                        return false;
                    }

                    return true;
                }),
                RetryIntervalSeconds);
        }

#if PLATFORM_WINDOWS
        bool IsUnrealEditorProcess(DWORD ProcessId)
        {
            if (ProcessId == ::GetCurrentProcessId())
            {
                return true;
            }

            HANDLE ProcessHandle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, ProcessId);
            if (!ProcessHandle)
            {
                return false;
            }

            WCHAR ProcessPath[32768] = {};
            DWORD PathLength = UE_ARRAY_COUNT(ProcessPath);
            const bool bFoundPath = ::QueryFullProcessImageNameW(ProcessHandle, 0, ProcessPath, &PathLength) != 0;
            ::CloseHandle(ProcessHandle);

            return bFoundPath &&
                FPaths::GetCleanFilename(FString(PathLength, ProcessPath)).Equals(TEXT("UnrealEditor.exe"), ESearchCase::IgnoreCase);
        }

        void HandleForegroundProcess(DWORD ForegroundProcessId, bool bIsUnrealEditor)
        {
            if (!bRunning.Load() || !bIsUnrealEditor)
            {
                return;
            }

            if (ForegroundProcessId == ::GetCurrentProcessId())
            {
                if (!TryClaimPort())
                {
                    StartLimitedRetry();
                }
            }
            else
            {
                StopHttpAndReleasePort();
            }
        }

        void CALLBACK OnForegroundWindowChanged(
            HWINEVENTHOOK Hook,
            DWORD Event,
            HWND Window,
            LONG ObjectId,
            LONG ChildId,
            DWORD EventThreadId,
            DWORD EventTime)
        {
            if (!bRunning.Load() || !Window)
            {
                return;
            }

            DWORD ForegroundProcessId = 0;
            ::GetWindowThreadProcessId(Window, &ForegroundProcessId);
            const bool bIsUnrealEditor = ForegroundProcessId != 0 && IsUnrealEditorProcess(ForegroundProcessId);

            AsyncTask(ENamedThreads::GameThread, [ForegroundProcessId, bIsUnrealEditor]()
            {
                HandleForegroundProcess(ForegroundProcessId, bIsUnrealEditor);
            });
        }
#endif
    }

    void Start()
    {
        if (bRunning.Exchange(true))
        {
            return;
        }

#if PLATFORM_WINDOWS
        PortOwnerMutex = ::CreateMutexW(nullptr, false, L"Local\\QuickerBridge_HTTP_18080_Owner");
        ForegroundEventHook = ::SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            &OnForegroundWindowChanged,
            0,
            0,
            WINEVENT_OUTOFCONTEXT);

        if (!PortOwnerMutex || !ForegroundEventHook)
        {
            UE_LOG(LogTemp, Warning, TEXT("QuickerBridge: Multi-UE coordinator initialization failed; using the original HTTP listener behavior."));

            if (ForegroundEventHook)
            {
                ::UnhookWinEvent(ForegroundEventHook);
                ForegroundEventHook = nullptr;
            }
            if (PortOwnerMutex)
            {
                ::CloseHandle(PortOwnerMutex);
                PortOwnerMutex = nullptr;
            }

            bRunning.Store(false);
            FHttpServerModule::Get().StartAllListeners();
            return;
        }

        // Ensure a lone editor starts serving immediately, even if another application
        // temporarily owns the foreground while Unreal Editor is starting.
        if (TryClaimPort())
        {
            return;
        }

        HWND ForegroundWindow = ::GetForegroundWindow();
        DWORD ForegroundProcessId = 0;
        if (ForegroundWindow)
        {
            ::GetWindowThreadProcessId(ForegroundWindow, &ForegroundProcessId);
        }

        HandleForegroundProcess(
            ForegroundProcessId,
            ForegroundProcessId != 0 && IsUnrealEditorProcess(ForegroundProcessId));
#else
        FHttpServerModule::Get().StartAllListeners();
#endif
    }

    void Stop()
    {
        if (!bRunning.Exchange(false))
        {
            return;
        }

        StopHttpAndReleasePort();

#if PLATFORM_WINDOWS
        if (ForegroundEventHook)
        {
            ::UnhookWinEvent(ForegroundEventHook);
            ForegroundEventHook = nullptr;
        }

        if (PortOwnerMutex)
        {
            ::CloseHandle(PortOwnerMutex);
            PortOwnerMutex = nullptr;
        }
#endif
    }
}
