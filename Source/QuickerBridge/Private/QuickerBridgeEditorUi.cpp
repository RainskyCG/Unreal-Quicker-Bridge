#include "QuickerBridgeLibrary.h"
#include "QuickerBridgeContextService.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Layout/WidgetPath.h"
#include "SGraphPanel.h"
#include "GraphEditor.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "LevelEditor.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"

bool UQuickerBridgeLibrary::ControlEditorPanel(const FString& TabID, const FString& Command)
{
    if (!FSlateApplication::IsInitialized()) return false;

    FName TabName(*TabID);
    TSharedPtr<SDockTab> FoundTab;
    TSharedPtr<FTabManager> TargetTabManager = FGlobalTabmanager::Get(); 

    if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
    {
        FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
        TSharedPtr<FTabManager> LevelTabManager = LevelEditorModule.GetLevelEditorTabManager();
        
        if (LevelTabManager.IsValid())
        {
            FoundTab = LevelTabManager->FindExistingLiveTab(TabName);
            if (LevelTabManager->HasTabSpawner(TabName))
            {
                TargetTabManager = LevelTabManager;
            }
        }
    }

    if (!FoundTab.IsValid())
    {
        FoundTab = FGlobalTabmanager::Get()->FindExistingLiveTab(TabName);
    }

    bool bIsOpen = FoundTab.IsValid();

    if (Command.Equals(TEXT("Check"), ESearchCase::IgnoreCase)) return bIsOpen;
    else if (Command.Equals(TEXT("Open"), ESearchCase::IgnoreCase) || Command.Equals(TEXT("Focus"), ESearchCase::IgnoreCase))
    {
        TargetTabManager->TryInvokeTab(TabName);
        return true;
    }
    else if (Command.Equals(TEXT("Close"), ESearchCase::IgnoreCase))
    {
        if (bIsOpen) FoundTab->RequestCloseTab();
        return false;
    }
    else 
    {
        if (bIsOpen)
        {
            FoundTab->RequestCloseTab();
            return false;
        }
        TargetTabManager->TryInvokeTab(TabName);
        return true;
    }
}


// ==========================================
// 4. 获取图表绝对鼠标坐标 (带冻结坐标覆盖)
// ==========================================
FVector2D UQuickerBridgeLibrary::GetGraphMousePosition(float OverrideX, float OverrideY)
{
    if (!FSlateApplication::IsInitialized()) return FVector2D::ZeroVector;

    FVector2D MousePos = (OverrideX >= 0.0f && OverrideY >= 0.0f) ? FVector2D(OverrideX, OverrideY) : FSlateApplication::Get().GetCursorPos();
    FWidgetPath WidgetPath;
    if (!QuickerBridgeContextService::ResolveWidgetPath(MousePos, WidgetPath)) return FVector2D::ZeroVector;

    FGeometry PanelGeometry;
    bool bFoundPanel = false;
    SGraphEditor* GraphEditor = nullptr;

    for (int i = WidgetPath.Widgets.Num() - 1; i >= 0; --i)
    {
        FString WidgetType = WidgetPath.Widgets[i].Widget->GetTypeAsString();
        if (!bFoundPanel && WidgetType.Contains(TEXT("GraphPanel")))
        {
            PanelGeometry = WidgetPath.Widgets[i].Geometry;
            bFoundPanel = true;
        }
        if (WidgetType.Contains(TEXT("GraphEditor")))
        {
            GraphEditor = static_cast<SGraphEditor*>(&(WidgetPath.Widgets[i].Widget.Get()));
            break; 
        }
    }

    if (!bFoundPanel || !GraphEditor) return FVector2D::ZeroVector;

    FVector2D ViewLocation = FVector2D::ZeroVector;
    float ZoomAmount = 1.0f; 
    
    GraphEditor->GetViewLocation(ViewLocation, ZoomAmount);

    if (ZoomAmount <= 0.01f || !FMath::IsFinite(ZoomAmount) || FMath::IsNaN(ZoomAmount)) ZoomAmount = 1.0f;

    FVector2D LocalMouse = PanelGeometry.AbsoluteToLocal(MousePos);
    double FinalX = ViewLocation.X + (LocalMouse.X / ZoomAmount);
    double FinalY = ViewLocation.Y + (LocalMouse.Y / ZoomAmount);

    if (!FMath::IsFinite(FinalX) || FMath::IsNaN(FinalX)) FinalX = 0.0;
    if (!FMath::IsFinite(FinalY) || FMath::IsNaN(FinalY)) FinalY = 0.0;

    return FVector2D(FinalX, FinalY);
}


// ==========================================
// 5. 穿透外壳获取当前真实材质
// ==========================================
UObject* UQuickerBridgeLibrary::GetCurrentlyEditedMaterial(float OverrideX, float OverrideY)
{
    if (!FSlateApplication::IsInitialized()) return nullptr;

    FVector2D MousePos = (OverrideX >= 0.0f && OverrideY >= 0.0f) ? FVector2D(OverrideX, OverrideY) : FSlateApplication::Get().GetCursorPos();
    FWidgetPath WidgetPath;
    if (!QuickerBridgeContextService::ResolveWidgetPath(MousePos, WidgetPath)) return nullptr;

    for (int i = WidgetPath.Widgets.Num() - 1; i >= 0; --i)
    {
        if (WidgetPath.Widgets[i].Widget->GetTypeAsString().Contains(TEXT("GraphPanel")))
        {
            SGraphPanel* GraphPanel = static_cast<SGraphPanel*>(&(WidgetPath.Widgets[i].Widget.Get()));
            if (!GraphPanel) continue;

            UEdGraph* Graph = GraphPanel->GetGraphObj();
            if (!Graph) continue;

            UObject* OuterObj = Graph->GetOuter();
            while (OuterObj)
            {
                if (OuterObj->IsA<UMaterial>() || OuterObj->IsA<UMaterialFunction>())
                {
                    return OuterObj;
                }
                OuterObj = OuterObj->GetOuter();
            }
        }
    }
    return nullptr;
}


// ==========================================
// 6. 刷新节点 UI 视图
// ==========================================
void UQuickerBridgeLibrary::RefreshCurrentGraphUI(float OverrideX, float OverrideY)
{
    if (!FSlateApplication::IsInitialized()) return;

    FVector2D MousePos = (OverrideX >= 0.0f && OverrideY >= 0.0f) ? FVector2D(OverrideX, OverrideY) : FSlateApplication::Get().GetCursorPos();
    FWidgetPath WidgetPath;
    if (!QuickerBridgeContextService::ResolveWidgetPath(MousePos, WidgetPath)) return;

    for (int i = WidgetPath.Widgets.Num() - 1; i >= 0; --i)
    {
        if (WidgetPath.Widgets[i].Widget->GetTypeAsString().Contains(TEXT("GraphPanel")))
        {
            SGraphPanel* GraphPanel = static_cast<SGraphPanel*>(&(WidgetPath.Widgets[i].Widget.Get()));
            if (GraphPanel && GraphPanel->GetGraphObj())
            {
                GraphPanel->GetGraphObj()->NotifyGraphChanged();
                return;
            }
        }
    }
}


// ==========================================
// 7. 强制修改编辑器配置属性 (忽略权限锁)
// ==========================================

float UQuickerBridgeLibrary::GetEditorUIScale()
{
    if (!FSlateApplication::IsInitialized())
    {
        return 1.0f;
    }

    return FSlateApplication::Get().GetApplicationScale();
}

bool UQuickerBridgeLibrary::SetEditorUIScale(float Scale)
{
    if (!FSlateApplication::IsInitialized())
    {
        return false;
    }

    const float ClampedScale = FMath::Clamp(Scale, 0.5f, 3.0f);
    FSlateApplication::Get().SetApplicationScale(ClampedScale);
    return true;
}
