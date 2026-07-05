#include "QuickerBridgeLibrary.h"
#include "QuickerBridgeContextService.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "LevelEditor.h"
#include "SEditorViewport.h"
#include "SLevelViewport.h"
#include "ModelingToolsEditorMode.h"
#include "InteractiveToolManager.h"
#include "Containers/Ticker.h"
#include "SceneView.h"
#include "Engine/World.h"

namespace
{
    static FTSTicker::FDelegateHandle GQuickerToolActivationTickerHandle;

    struct FQuickerPendingToolActivation
    {
        FName TargetMode;
        FString SubToolID;

        double StartTime = 0.0;
        int32 ReadyFrameCount = 0;
        int32 AttemptCount = 0;

        bool bDidRequestMode = false;
        bool bDidShutdownExistingTool = false;
    };

    static void QuickerClearToolActivationTicker()
    {
        if (GQuickerToolActivationTickerHandle.IsValid())
        {
            FTSTicker::GetCoreTicker().RemoveTicker(GQuickerToolActivationTickerHandle);
            GQuickerToolActivationTickerHandle.Reset();
        }
    }

    static UInteractiveToolManager* QuickerResolveToolManagerForMode(const FName& TargetMode)
    {
        // 当前只稳定支持 Modeling Mode。
        // 不要在这里强行扩展 Landscape/Foliage/Fracture，
        // 因为它们不一定走 UInteractiveToolManager。
        static const FName ModelingModeID(TEXT("EM_ModelingToolsEditorMode"));

        if (TargetMode != ModelingModeID)
        {
            return nullptr;
        }

        UModelingToolsEditorMode* ModelingMode =
            Cast<UModelingToolsEditorMode>(
                GLevelEditorModeTools().GetActiveScriptableMode(ModelingModeID)
            );

        if (!ModelingMode)
        {
            return nullptr;
        }

        return ModelingMode->GetToolManager();
    }

    static bool QuickerIsEditorViewportWidgetType(const FString& WidgetType)
    {
        return WidgetType == TEXT("SEditorViewport")
            || WidgetType == TEXT("SAssetEditorViewport")
            || WidgetType == TEXT("SLevelViewport")
            || WidgetType == TEXT("SSCSEditorViewport")
            || WidgetType.Contains(TEXT("EditorViewport"))
            || WidgetType.Contains(TEXT("Editor3DPreviewViewport"));
    }

    static FEditorViewportClient* QuickerResolveViewportClientUnderMouse()
    {
        FWidgetPath WidgetPath;
        if (!QuickerBridgeContextService::ResolveCurrentWidgetPath(WidgetPath))
        {
            return nullptr;
        }

        for (int32 WidgetIndex = WidgetPath.Widgets.Num() - 1; WidgetIndex >= 0; --WidgetIndex)
        {
            const TSharedRef<SWidget>& Widget = WidgetPath.Widgets[WidgetIndex].Widget;
            if (!QuickerIsEditorViewportWidgetType(Widget->GetTypeAsString()))
            {
                continue;
            }

            const TSharedRef<SEditorViewport> EditorViewport = StaticCastSharedRef<SEditorViewport>(Widget);
            const TSharedPtr<FEditorViewportClient> ViewportClient = EditorViewport->GetViewportClient();
            return ViewportClient.Get();
        }

        // Some editor viewport implementations do not expose an identifiable
        // SEditorViewport type name. Only fall back when the mouse is still
        // confirmed to be over a viewport.
        if (QuickerBridgeContextService::IsWidgetPathViewport(WidgetPath)
            && GEditor
            && GEditor->GetActiveViewport())
        {
            return static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
        }

        return nullptr;
    }

    static FEditorViewportClient* QuickerResolveActiveLevelViewportClient()
    {
        if (!FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
        {
            return nullptr;
        }

        FLevelEditorModule& LevelEditorModule =
            FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
        const TSharedPtr<SLevelViewport> LevelViewport = LevelEditorModule.GetFirstActiveLevelViewport();
        return LevelViewport.IsValid() ? LevelViewport->GetViewportClient().Get() : nullptr;
    }

    static FEditorViewportClient* QuickerResolveViewportClient(const FString& Target)
    {
        FString NormalizedTarget = Target;
        NormalizedTarget.TrimStartAndEndInline();

        if (NormalizedTarget.IsEmpty() ||
            NormalizedTarget.Equals(TEXT("UnderMouse"), ESearchCase::IgnoreCase))
        {
            return QuickerResolveViewportClientUnderMouse();
        }

        if (NormalizedTarget.Equals(TEXT("ActiveLevel"), ESearchCase::IgnoreCase))
        {
            return QuickerResolveActiveLevelViewportClient();
        }

        return nullptr;
    }

    static FString QuickerNormalizeViewModeName(const FString& ViewMode)
    {
        FString Normalized = ViewMode;
        Normalized.TrimStartAndEndInline();

        if (!Normalized.StartsWith(TEXT("VMI_"), ESearchCase::IgnoreCase))
        {
            Normalized = TEXT("VMI_") + Normalized;
        }

        return Normalized;
    }
}

bool UQuickerBridgeLibrary::ActivateEditorModeAndTool(const FString& ModeID, const FString& SubToolID)
{
    if (!GEditor)
    {
        UE_LOG(LogTemp, Error, TEXT("❌ QuickerBridge: GEditor 为空，无法激活模式。"));
        return false;
    }

    if (ModeID.IsEmpty())
    {
        UE_LOG(LogTemp, Error, TEXT("❌ QuickerBridge: ModeID 为空。"));
        return false;
    }

    const FName TargetMode(*ModeID);

    // 1. 先切换大模式
    if (!GLevelEditorModeTools().IsModeActive(TargetMode))
    {
        GLevelEditorModeTools().ActivateMode(TargetMode);
        const bool bModeActivated = GLevelEditorModeTools().IsModeActive(TargetMode);

        if (!bModeActivated)
        {
            UE_LOG(LogTemp, Error, TEXT("❌ QuickerBridge: ActivateMode 失败：[%s]"), *ModeID);
            return false;
        }
    }

    // 只切换模式，不激活工具
    if (SubToolID.IsEmpty())
    {
        UE_LOG(LogTemp, Display, TEXT("✅ QuickerBridge: 已请求进入模式：[%s]"), *ModeID);
        return true;
    }

    // 2. 当前版本只稳定支持 ModelingToolsEditorMode 的子工具
    if (TargetMode != FName(TEXT("EM_ModelingToolsEditorMode")))
    {
        UE_LOG(
            LogTemp,
            Error,
            TEXT("❌ QuickerBridge: 当前子工具激活模块只支持 EM_ModelingToolsEditorMode。收到 ModeID：[%s], ToolID：[%s]"),
            *ModeID,
            *SubToolID
        );
        return false;
    }

    // 3. 防止重复调用时多个 Ticker 抢 ToolManager
    QuickerClearToolActivationTicker();

    TSharedRef<FQuickerPendingToolActivation> State = MakeShared<FQuickerPendingToolActivation>();
    State->TargetMode = TargetMode;
    State->SubToolID = SubToolID;
    State->StartTime = FPlatformTime::Seconds();

    GQuickerToolActivationTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([State](float DeltaTime) -> bool
        {
            const double Elapsed = FPlatformTime::Seconds() - State->StartTime;

            // 3 秒熔断，防止永久轮询
            if (Elapsed > 3.0)
            {
                UE_LOG(
                    LogTemp,
                    Error,
                    TEXT("❌ QuickerBridge: 激活工具超时。Mode=[%s], Tool=[%s], Attempts=%d。可能原因：ToolID 错误、当前选择不满足工具要求、视口未激活。"),
                    *State->TargetMode.ToString(),
                    *State->SubToolID,
                    State->AttemptCount
                );

                GQuickerToolActivationTickerHandle.Reset();
                return false;
            }

            // 确保模式仍然 active
            if (!GLevelEditorModeTools().IsModeActive(State->TargetMode))
            {
                GLevelEditorModeTools().ActivateMode(State->TargetMode);
                return true;
            }

            UInteractiveToolManager* ToolManager = QuickerResolveToolManagerForMode(State->TargetMode);

            if (!ToolManager)
            {
                // Mode 刚切进去时，ToolManager 可能还没 ready
                return true;
            }

            // ToolManager 出现后，再等 2 帧，避免 Toolkit / Context 尚未稳定
            State->ReadyFrameCount++;

            if (State->ReadyFrameCount < 2)
            {
                return true;
            }

            const EToolSide ToolSide = EToolSide::Left;

            // 只 Cancel 一次，不要每帧都踢工具
            if (!State->bDidShutdownExistingTool)
            {
                if (ToolManager->HasActiveTool(ToolSide))
                {
                    ToolManager->DeactivateTool(ToolSide, EToolShutdownType::Cancel);
                    State->bDidShutdownExistingTool = true;

                    // 给 Deactivate 留一帧收尾时间
                    return true;
                }

                State->bDidShutdownExistingTool = true;
            }

            State->AttemptCount++;

            ToolManager->SelectActiveToolType(ToolSide, State->SubToolID);

            if (ToolManager->ActivateTool(ToolSide))
            {
                UE_LOG(
                    LogTemp,
                    Display,
                    TEXT("🎯 QuickerBridge: 已激活工具。Mode=[%s], Tool=[%s], Time=%.3fs, Attempts=%d"),
                    *State->TargetMode.ToString(),
                    *State->SubToolID,
                    Elapsed,
                    State->AttemptCount
                );

                GQuickerToolActivationTickerHandle.Reset();
                return false;
            }

            // 不每帧刷屏，只偶尔报一次
            if (State->AttemptCount == 1 || State->AttemptCount % 30 == 0)
            {
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("⚠️ QuickerBridge: 正在尝试激活工具。Mode=[%s], Tool=[%s], Attempts=%d"),
                    *State->TargetMode.ToString(),
                    *State->SubToolID,
                    State->AttemptCount
                );
            }

            return true;
        })
    );

    UE_LOG(
        LogTemp,
        Display,
        TEXT("🚀 QuickerBridge: 已提交工具激活请求。Mode=[%s], Tool=[%s]"),
        *ModeID,
        *SubToolID
    );

    return true;
}

FString UQuickerBridgeLibrary::GetViewportViewMode(const FString& Target)
{
    FEditorViewportClient* ViewportClient = QuickerResolveViewportClient(Target);
    if (!ViewportClient)
    {
        return FString();
    }

    const UEnum* ViewModeEnum = StaticEnum<EViewModeIndex>();
    if (!ViewModeEnum)
    {
        return FString();
    }

    FString ViewModeName = ViewModeEnum->GetNameStringByValue(static_cast<int64>(ViewportClient->GetViewMode()));
    ViewModeName.RemoveFromStart(TEXT("VMI_"), ESearchCase::IgnoreCase);
    return ViewModeName;
}

bool UQuickerBridgeLibrary::SetViewportViewMode(const FString& ViewMode, const FString& Target)
{
    FEditorViewportClient* ViewportClient = QuickerResolveViewportClient(Target);
    const UEnum* ViewModeEnum = StaticEnum<EViewModeIndex>();

    if (!ViewportClient || !ViewModeEnum)
    {
        return false;
    }

    const FString NormalizedViewMode = QuickerNormalizeViewModeName(ViewMode);
    const int64 ViewModeValue = ViewModeEnum->GetValueByNameString(NormalizedViewMode);

    if (ViewModeValue == INDEX_NONE || ViewModeValue < 0 || ViewModeValue >= static_cast<int64>(VMI_Max))
    {
        UE_LOG(LogTemp, Warning, TEXT("QuickerBridge: Unknown viewport view mode: [%s]"), *ViewMode);
        return false;
    }

    ViewportClient->SetViewMode(static_cast<EViewModeIndex>(ViewModeValue));
    return true;
}


// ==========================================
// 9. 全息 3D 射线雷达 (最难啃的模块，涉及屏幕缩放与坐标映射)
// ==========================================

void UQuickerBridgeLibrary::GetViewportMouseRaycast(bool& bIsHit, FVector& HitLocation, FVector& HitNormal, AActor*& HitActor, float OverrideX, float OverrideY)
{
    bIsHit = false;
    HitLocation = FVector::ZeroVector;
    HitNormal = FVector(0, 0, 1); 
    HitActor = nullptr;

    if (!GEditor || !GEditor->GetActiveViewport()) return;

    FEditorViewportClient* Client = static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient());
    if (!Client || !Client->Viewport) return;

    // 默认坐标：视口中心
    int32 MouseX = Client->Viewport->GetSizeXY().X / 2;
    int32 MouseY = Client->Viewport->GetSizeXY().Y / 2;
    bool bFoundViewport = false;

    if (FSlateApplication::IsInitialized())
    {
        TArray<FVector2D> TestPositions;

        if (OverrideX >= 0.0f && OverrideY >= 0.0f)
        {
            // 尝试应用 DPI 缩放前后的坐标，保证命中 UI
            float DPIScale = FSlateApplication::Get().GetApplicationScale();
            TestPositions.Add(FVector2D(OverrideX, OverrideY));
            if (DPIScale > 0) TestPositions.Add(FVector2D(OverrideX / DPIScale, OverrideY / DPIScale));
        }
        else
        {
            TestPositions.Add(FSlateApplication::Get().GetCursorPos());
        }

        for (FVector2D AbsPos : TestPositions)
        {
            FWidgetPath WidgetPath;
            if (QuickerBridgeContextService::ResolveWidgetPath(AbsPos, WidgetPath))
            {
                for (int i = WidgetPath.Widgets.Num() - 1; i >= 0; --i)
                {
                    if (WidgetPath.Widgets[i].Widget->GetTypeAsString().Contains(TEXT("Viewport")))
                    {
                        FGeometry Geom = WidgetPath.Widgets[i].Geometry;
                        
                        FVector2D LocalPos = Geom.AbsoluteToLocal(AbsPos);
                        FVector2D LocalSize = Geom.GetLocalSize();
                        FIntPoint ViewportSize = Client->Viewport->GetSizeXY();

                        // 比例映射，解决 4K 或视口缩放下的坐标漂移
                        if (LocalSize.X > 0.1f && LocalSize.Y > 0.1f)
                        {
                            MouseX = FMath::RoundToInt((LocalPos.X / LocalSize.X) * ViewportSize.X);
                            MouseY = FMath::RoundToInt((LocalPos.Y / LocalSize.Y) * ViewportSize.Y);
                            bFoundViewport = true;
                            break; 
                        }
                    }
                }
            }
            if (bFoundViewport) break; 
        }
    }

    FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
        Client->Viewport,
        Client->GetScene(),
        Client->EngineShowFlags)
        .SetRealtimeUpdate(Client->IsRealtime()));

    if (FSceneView* View = Client->CalcSceneView(&ViewFamily))
    {
        FViewportCursorLocation CursorLocation(View, Client, MouseX, MouseY);
        FVector Origin = CursorLocation.GetOrigin();
        FVector Direction = CursorLocation.GetDirection();

        if (UWorld* World = Client->GetWorld())
        {
            FVector End = Origin + Direction * 100000.0f; 
            FHitResult HitResult;
            FCollisionQueryParams Params;
            Params.bTraceComplex = true; 

            if (World->LineTraceSingleByChannel(HitResult, Origin, End, ECC_Visibility, Params))
            {
                bIsHit = true;
                HitLocation = HitResult.Location;
                HitNormal = HitResult.Normal;
                HitActor = HitResult.GetActor();
                return;
            }
            
            HitLocation = Origin + Direction * 1000.0f;
        }
    }
}
