#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Math/Vector2D.h"
#include "Math/Vector.h" 
#include "LevelEditor.h" 
#include "QuickerBridgeLibrary.generated.h"

UCLASS()
class QUICKERBRIDGE_API UQuickerBridgeLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static class UMaterialExpression* SpawnMaterialNodeAtMouse(TSubclassOf<class UMaterialExpression> NodeClass, float OffsetX = 0.0f, float OffsetY = 0.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static UObject* SpawnBlueprintNodeAtMouse(const FString& NodeName, float OffsetX = 0.0f, float OffsetY = 0.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static UObject* SpawnBlueprintActionNodeAtMouse(const FString& NodeName, float OffsetX = 0.0f, float OffsetY = 0.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static UObject* SpawnBlueprintClassNodeAtMouse(TSubclassOf<UObject> NodeClass, float OffsetX = 0.0f, float OffsetY = 0.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static UObject* SpawnBlueprintMacroNodeAtMouse(const FString& MacroGraphPath, float OffsetX = 0.0f, float OffsetY = 0.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static UObject* SpawnBlueprintFunctionNodeAtMouse(TSubclassOf<UObject> FunctionOwnerClass, const FString& FunctionName, float OffsetX = 0.0f, float OffsetY = 0.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static void RedrawGraphWires(class UMaterialExpression* Node);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static bool ControlEditorPanel(const FString& TabID, const FString& Command = TEXT("Toggle"));

    // === 【核心修改】：加上坐标覆盖参数 ===
    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static FVector2D GetGraphMousePosition(float OverrideX = -1.0f, float OverrideY = -1.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static void GetViewportMouseRaycast(bool& bIsHit, FVector& HitLocation, FVector& HitNormal, AActor*& HitActor, float OverrideX = -1.0f, float OverrideY = -1.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static UObject* GetCurrentlyEditedMaterial(float OverrideX = -1.0f, float OverrideY = -1.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static void RefreshCurrentGraphUI(float OverrideX = -1.0f, float OverrideY = -1.0f);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static bool ActivateEditorModeAndTool(const FString& ModeID, const FString& SubToolID);

    UFUNCTION(BlueprintPure, Category = "QuickerBridge|Viewport")
    static FString GetViewportViewMode(const FString& Target = TEXT("UnderMouse"));

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge|Viewport")
    static bool SetViewportViewMode(const FString& ViewMode, const FString& Target = TEXT("UnderMouse"));

    UFUNCTION(BlueprintPure, Category = "QuickerBridge|Editor UI")
    static float GetEditorUIScale();

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge|Editor UI")
    static bool SetEditorUIScale(float Scale);

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge|Content Browser")
    static FString ControlContentBrowserAssetFilter(const FString& FilterName, const FString& Command = TEXT("Toggle"));

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge|Content Browser")
    static bool ClearContentBrowserFilters();

    UFUNCTION(BlueprintCallable, Category = "QuickerBridge|Content Browser")
    static FString ToggleContentBrowserCommonFilters(const FString& ContentBrowserId = TEXT("ContentBrowser"));

    // 万能钥匙：强制读取任何未暴露的属性（返回字符串）
    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static FString ForceGetEditorProperty(UObject* TargetObject, const FString& PropertyName);

    // 万能钥匙：强制修改任何未暴露的属性，并刷新 UI
    UFUNCTION(BlueprintCallable, Category = "QuickerBridge")
    static bool ForceSetEditorProperty(UObject* TargetObject, const FString& PropertyName, const FString& PropertyValue);

};
