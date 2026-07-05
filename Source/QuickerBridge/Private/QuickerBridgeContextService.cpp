#include "QuickerBridgeContextService.h"

#include "Framework/Application/SlateApplication.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace QuickerBridgeContextService
{
    bool ResolveWidgetPath(const FVector2D& ScreenPosition, FWidgetPath& OutWidgetPath)
    {
        if (!FSlateApplication::IsInitialized())
        {
            OutWidgetPath = FWidgetPath();
            return false;
        }

        // Do not retain FWidgetPath across calls. It owns Slate widget references
        // that can become stale while tabs are rebuilt or destroyed.
        OutWidgetPath = FSlateApplication::Get().LocateWindowUnderMouse(
            ScreenPosition,
            FSlateApplication::Get().GetInteractiveTopLevelWindows());
        return OutWidgetPath.IsValid();
    }

    bool ResolveCurrentWidgetPath(FWidgetPath& OutWidgetPath, FVector2D* OutScreenPosition)
    {
        if (!FSlateApplication::IsInitialized())
        {
            OutWidgetPath = FWidgetPath();
            return false;
        }

        const FVector2D ScreenPosition = FSlateApplication::Get().GetCursorPos();
        if (OutScreenPosition)
        {
            *OutScreenPosition = ScreenPosition;
        }

        return ResolveWidgetPath(ScreenPosition, OutWidgetPath);
    }

    bool IsWidgetPathViewport(const FWidgetPath& WidgetPath)
    {
        for (int32 WidgetIndex = 0; WidgetIndex < WidgetPath.Widgets.Num(); ++WidgetIndex)
        {
            const FArrangedWidget& ArrangedWidget = WidgetPath.Widgets[WidgetIndex];
            if (ArrangedWidget.Widget->GetTypeAsString().Contains(TEXT("Viewport")))
            {
                return true;
            }
        }

        return false;
    }

    bool IsScreenPositionOverCurrentProcessWindow(const FVector2D& ScreenPosition)
    {
#if PLATFORM_WINDOWS
        POINT Point;
        Point.x = FMath::RoundToInt(ScreenPosition.X);
        Point.y = FMath::RoundToInt(ScreenPosition.Y);

        HWND Window = ::WindowFromPoint(Point);
        if (!Window)
        {
            return false;
        }

        DWORD ProcessId = 0;
        ::GetWindowThreadProcessId(Window, &ProcessId);
        return ProcessId == ::GetCurrentProcessId();
#else
        return true;
#endif
    }

    void Invalidate()
    {
        // Paths are resolved per call, so there is no retained state to invalidate.
    }
}
