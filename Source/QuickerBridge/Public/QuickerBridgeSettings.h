#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "QuickerBridgeSettings.generated.h"

UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "QuickerBridge"))
class QUICKERBRIDGE_API UQuickerBridgeSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    virtual FName GetContainerName() const override { return FName("Editor"); }
    virtual FName GetCategoryName() const override { return FName("Plugins"); }
    virtual FName GetSectionName() const override { return FName("QuickerBridge"); }

    UPROPERTY(VisibleAnywhere, Category = "HTTP Service", meta = (DisplayName = "UE Service Address", ToolTip = "Local HTTP service exposed by QuickerBridge inside Unreal Editor."))
    FString HttpServiceEndpoint = TEXT("http://127.0.0.1:18080");

    UPROPERTY(VisibleAnywhere, Category = "HTTP Service", meta = (DisplayName = "Main Ports", MultiLine = true, ToolTip = "Port notes for local Quicker integration."))
    FString PortSummary = TEXT("UE QuickerBridge: 127.0.0.1:18080 for UE context probing and Python execution.\nBlender QuickerBridge reference port: 127.0.0.1:18765, used only by the Blender add-on.\nWith multi-UE switching enabled, external tools still call 18080; the foreground UE instance owns that port.");

    UPROPERTY(VisibleAnywhere, Category = "HTTP Service", meta = (DisplayName = "Context Endpoints", MultiLine = true, ToolTip = "Endpoints used by Quicker or other local tools to read the current UE context."))
    FString ContextEndpointSummary = TEXT("GET /context/current: lightweight text context.\nGET /context/detail: structured JSON including context, graph, viewport, and Content Browser selection details.\nGET /context/debug: Slate widget path debug text.");

    UPROPERTY(VisibleAnywhere, Category = "HTTP Service", meta = (DisplayName = "Action Endpoints", MultiLine = true, ToolTip = "Connectivity and local Python execution endpoints."))
    FString ActionEndpointSummary = TEXT("GET /health: plugin status, port, process, Python initialization, route binding state.\nPOST /python/execute: JSON body with command, optional mode ExecuteFile, ExecuteStatement, or EvaluateStatement.");

    UPROPERTY(VisibleAnywhere, Category = "Shortcuts", meta = (DisplayName = "Built-in Shortcuts", MultiLine = true, ToolTip = "Registered as Unreal Editor commands; defaults mirror the Blender add-on where practical."))
    FString ShortcutSummary = TEXT("Ctrl+Alt+P: QuickerBridge probe, copies current /context/detail JSON to clipboard.\nCtrl+Alt+Shift+V: execute Unreal Python from clipboard.");

    UPROPERTY(config, EditAnywhere, Category = "Multi UE Instances", meta = (DisplayName = "Enable Multi-UE Port Switching", ToolTip = "When enabled, Unreal Editor processes switch the fixed HTTP port 18080 as they become the foreground app. Enable in every simultaneously running UE project, then restart the editor."))
    bool bEnableMultiUEPortSwitching = false;

    UPROPERTY(config, EditAnywhere, Category = "Scroll Lock Control", meta = (DisplayName = "Enable Scroll Lock Viewport Control", ToolTip = "When enabled, Scroll Lock is released while the mouse is over any 3D viewport in the current UE instance and locked elsewhere in the current UE instance."))
    bool bEnableScrollLockViewportControl = true;

    UPROPERTY(config, EditAnywhere, Category = "Scroll Lock Control", meta = (DisplayName = "Fallback Detection Interval", ToolTip = "Mouse movement triggers immediate detection; this interval is a fallback for idle mouse state or external Scroll Lock changes.", ClampMin = "0.01", ClampMax = "2.0", UIMin = "0.01", UIMax = "1.0", ForceUnits = "s"))
    float ScrollLockDetectionInterval = 0.5f;
};
