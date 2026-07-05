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

    UPROPERTY(config, EditAnywhere, Category = "多 UE 实例", meta = (DisplayName = "启用多 UE 实例端口切换", ToolTip = "启用后，不同 Unreal Editor 进程成为前台时会自动切换固定 HTTP 端口 18080。需要在所有同时运行的 UE 项目中启用，并重启编辑器。"))
    bool bEnableMultiUEPortSwitching = false;

    UPROPERTY(config, EditAnywhere, Category = "Scroll Lock 控制", meta = (DisplayName = "启用 Scroll Lock 视口控制", ToolTip = "启用后，鼠标位于当前 UE 实例的任意 3D 视口时解除 Scroll Lock，位于当前 UE 的其他区域时锁定 Scroll Lock。"))
    bool bEnableScrollLockViewportControl = true;

    UPROPERTY(config, EditAnywhere, Category = "Scroll Lock 控制", meta = (DisplayName = "保底检测间隔", ToolTip = "鼠标移动时会立即检测；此间隔用于鼠标未移动或状态被外部修改时的保底检测。", ClampMin = "0.01", ClampMax = "2.0", UIMin = "0.01", UIMax = "1.0", ForceUnits = "s"))
    float ScrollLockDetectionInterval = 0.5f;
};
