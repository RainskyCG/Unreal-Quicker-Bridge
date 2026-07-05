#include "QuickerBridgeScrollLockController.h"
#include "QuickerBridgeSettings.h"
#include "QuickerBridgeContextService.h"

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace QuickerBridgeScrollLockController
{
    namespace
    {
        constexpr float BaseTickerIntervalSeconds = 0.01f;
        FTSTicker::FDelegateHandle TickerHandle;
        TSharedPtr<IInputProcessor> InputProcessor;
        float TimeSinceLastDetection = 0.0f;

        bool IsScrollLockEnabled()
        {
#if PLATFORM_WINDOWS
            return (::GetKeyState(VK_SCROLL) & 0x0001) != 0;
#else
            return false;
#endif
        }

        void SetScrollLockEnabled(bool bEnabled)
        {
#if PLATFORM_WINDOWS
            if (IsScrollLockEnabled() == bEnabled)
            {
                return;
            }

            INPUT Inputs[2] = {};
            Inputs[0].type = INPUT_KEYBOARD;
            Inputs[0].ki.wVk = VK_SCROLL;
            Inputs[1].type = INPUT_KEYBOARD;
            Inputs[1].ki.wVk = VK_SCROLL;
            Inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
            ::SendInput(UE_ARRAY_COUNT(Inputs), Inputs, sizeof(INPUT));
#endif
        }

        bool IsControlEnabled()
        {
            const UQuickerBridgeSettings* Settings = GetDefault<UQuickerBridgeSettings>();
            return Settings && Settings->bEnableScrollLockViewportControl;
        }

        void UpdateScrollLockForCurrentMousePosition()
        {
            if (!IsControlEnabled())
            {
                return;
            }

            FVector2D ScreenPosition;
            FWidgetPath WidgetPath;
            if (!QuickerBridgeContextService::ResolveCurrentWidgetPath(WidgetPath, &ScreenPosition) ||
                !QuickerBridgeContextService::IsScreenPositionOverCurrentProcessWindow(ScreenPosition))
            {
                // Do not let a background UE instance overwrite the global state.
                return;
            }

            SetScrollLockEnabled(!QuickerBridgeContextService::IsWidgetPathViewport(WidgetPath));
        }

        class FQuickerBridgeScrollLockInputProcessor final : public IInputProcessor
        {
        public:
            virtual void Tick(
                const float DeltaTime,
                FSlateApplication& SlateApp,
                TSharedRef<ICursor> Cursor) override
            {
            }

            virtual bool HandleMouseMoveEvent(
                FSlateApplication& SlateApp,
                const FPointerEvent& MouseEvent) override
            {
                QuickerBridgeContextService::Invalidate();
                UpdateScrollLockForCurrentMousePosition();
                return false;
            }
        };

        void EnsureInputProcessorRegistered()
        {
            if (!InputProcessor.IsValid() && FSlateApplication::IsInitialized())
            {
                InputProcessor = MakeShared<FQuickerBridgeScrollLockInputProcessor>();
                FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
            }
        }

        bool Tick(float DeltaTime)
        {
            EnsureInputProcessorRegistered();

            const UQuickerBridgeSettings* Settings = GetDefault<UQuickerBridgeSettings>();
            if (!Settings || !Settings->bEnableScrollLockViewportControl)
            {
                TimeSinceLastDetection = 0.0f;
                return true;
            }

            TimeSinceLastDetection += DeltaTime;
            const float DetectionInterval = FMath::Max(
                Settings->ScrollLockDetectionInterval,
                BaseTickerIntervalSeconds);

            if (TimeSinceLastDetection < DetectionInterval)
            {
                return true;
            }

            TimeSinceLastDetection = 0.0f;
            UpdateScrollLockForCurrentMousePosition();
            return true;
        }
    }

    void Start()
    {
        EnsureInputProcessorRegistered();
        UpdateScrollLockForCurrentMousePosition();

        if (!TickerHandle.IsValid())
        {
            TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateStatic(&Tick),
                BaseTickerIntervalSeconds);
        }
    }

    void Stop()
    {
        if (InputProcessor.IsValid())
        {
            if (FSlateApplication::IsInitialized())
            {
                FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
            }

            InputProcessor.Reset();
            QuickerBridgeContextService::Invalidate();
        }

        if (TickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
            TickerHandle.Reset();
            TimeSinceLastDetection = 0.0f;
        }
    }
}
