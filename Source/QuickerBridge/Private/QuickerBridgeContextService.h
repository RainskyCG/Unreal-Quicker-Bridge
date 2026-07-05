#pragma once

#include "CoreMinimal.h"
#include "Layout/WidgetPath.h"

namespace QuickerBridgeContextService
{
    bool ResolveWidgetPath(const FVector2D& ScreenPosition, FWidgetPath& OutWidgetPath);
    bool ResolveCurrentWidgetPath(FWidgetPath& OutWidgetPath, FVector2D* OutScreenPosition = nullptr);
    bool IsWidgetPathViewport(const FWidgetPath& WidgetPath);
    bool IsScreenPositionOverCurrentProcessWindow(const FVector2D& ScreenPosition);
    void Invalidate();
}
