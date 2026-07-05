#include "QuickerBridgeLibrary.h"
#include "QuickerBridgeContextService.h"

#include "Framework/Application/SlateApplication.h"
#include "AssetDefinition.h"
#include "AssetDefinitionRegistry.h"
#include "Misc/AssetFilterData.h"
#include "AssetTypeCategories.h"
#include "SFilterList.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace
{
    TSharedPtr<SFilterList> FindFilterListRecursive(const TSharedRef<SWidget>& Widget)
    {
        if (Widget->GetTypeAsString() == TEXT("SFilterList"))
        {
            return StaticCastSharedRef<SFilterList>(Widget);
        }

        FChildren* Children = Widget->GetChildren();
        if (!Children)
        {
            return nullptr;
        }

        for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
        {
            if (TSharedPtr<SFilterList> Found = FindFilterListRecursive(Children->GetChildAt(ChildIndex)))
            {
                return Found;
            }
        }

        return nullptr;
    }

    TSharedPtr<SFilterList> FindVisibleContentBrowserFilterListRecursive(const TSharedRef<SWidget>& Widget)
    {
        if (!Widget->GetVisibility().IsVisible())
        {
            return nullptr;
        }

        if (Widget->GetTypeAsString() == TEXT("SContentBrowser"))
        {
            return FindFilterListRecursive(Widget);
        }

        FChildren* Children = Widget->GetChildren();
        if (!Children)
        {
            return nullptr;
        }

        for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
        {
            if (TSharedPtr<SFilterList> Found = FindVisibleContentBrowserFilterListRecursive(Children->GetChildAt(ChildIndex)))
            {
                return Found;
            }
        }

        return nullptr;
    }

    TSharedPtr<SFilterList> FindContentBrowserFilterListUnderMouse()
    {
        FWidgetPath WidgetPath;
        FVector2D ScreenPosition;
        const bool bHasWidgetPath = QuickerBridgeContextService::ResolveCurrentWidgetPath(WidgetPath, &ScreenPosition);

        if (bHasWidgetPath)
        {
            for (int32 WidgetIndex = WidgetPath.Widgets.Num() - 1; WidgetIndex >= 0; --WidgetIndex)
            {
                const TSharedRef<SWidget>& Widget = WidgetPath.Widgets[WidgetIndex].Widget;
                if (Widget->GetTypeAsString() == TEXT("SContentBrowser"))
                {
                    return FindFilterListRecursive(Widget);
                }
            }

            // Some UE 5.7 layouts do not expose SContentBrowser as an ancestor in
            // the hit-test path. Search the exact Slate window under the mouse.
            if (TSharedPtr<SWindow> MouseWindow = WidgetPath.GetWindow())
            {
                if (TSharedPtr<SFilterList> Found = FindVisibleContentBrowserFilterListRecursive(MouseWindow.ToSharedRef()))
                {
                    return Found;
                }
            }
        }

        // Filter changes can rebuild the Slate hit-test path between consecutive
        // Python calls. Fall back to current live top-level windows without caching.
        if (TSharedPtr<SWindow> ActiveWindow = FSlateApplication::Get().GetActiveTopLevelWindow())
        {
            if (TSharedPtr<SFilterList> Found = FindVisibleContentBrowserFilterListRecursive(ActiveWindow.ToSharedRef()))
            {
                return Found;
            }
        }

        for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetInteractiveTopLevelWindows())
        {
            if (TSharedPtr<SFilterList> Found = FindVisibleContentBrowserFilterListRecursive(Window))
            {
                return Found;
            }
        }

        return nullptr;
    }

    UClass* ResolveAssetFilterClass(const FString& FilterName)
    {
        FString CleanName = FilterName;
        CleanName.TrimStartAndEndInline();
        if (CleanName.IsEmpty())
        {
            return nullptr;
        }

        if (CleanName.StartsWith(TEXT("/")))
        {
            if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *CleanName))
            {
                return LoadedClass;
            }
        }

        return FindFirstObject<UClass>(
            *CleanName,
            EFindFirstObjectOptions::NativeFirst,
            ELogVerbosity::NoLogging,
            TEXT("QuickerBridge content browser asset filter"));
    }

    FString GetAssetFilterStatus(const TSharedRef<SFilterList>& FilterList, const FTopLevelAssetPath& ClassPath)
    {
        if (FilterList->GetAssetTypeFilterCheckState(ClassPath) != ECheckBoxState::Checked)
        {
            return TEXT("Removed");
        }

        return FilterList->IsAssetTypeFilterActive(ClassPath) ? TEXT("Active") : TEXT("Inactive");
    }
}

FString UQuickerBridgeLibrary::ControlContentBrowserAssetFilter(const FString& FilterName, const FString& Command)
{
    if (!IsInGameThread())
    {
        return TEXT("WrongThread");
    }

    const TSharedPtr<SFilterList> FilterList = FindContentBrowserFilterListUnderMouse();
    if (!FilterList.IsValid())
    {
        return TEXT("NoContentBrowserUnderMouse");
    }

    UClass* AssetClass = ResolveAssetFilterClass(FilterName);
    if (!AssetClass)
    {
        return TEXT("ClassNotFound");
    }

    const FTopLevelAssetPath ClassPath = AssetClass->GetClassPathName();
    if (!FilterList->DoesAssetTypeFilterExist(ClassPath))
    {
        return TEXT("FilterNotFound");
    }

    FString NormalizedCommand = Command;
    NormalizedCommand.TrimStartAndEndInline();

    if (NormalizedCommand.Equals(TEXT("Enable"), ESearchCase::IgnoreCase))
    {
        FilterList->SetAssetTypeFilterCheckState(ClassPath, ECheckBoxState::Checked, true);
        FilterList->ToggleAssetTypeFilterEnabled(ClassPath, true);
    }
    else if (NormalizedCommand.Equals(TEXT("Disable"), ESearchCase::IgnoreCase) ||
             NormalizedCommand.Equals(TEXT("PinInactive"), ESearchCase::IgnoreCase))
    {
        FilterList->SetAssetTypeFilterCheckState(ClassPath, ECheckBoxState::Checked, false);
        FilterList->ToggleAssetTypeFilterEnabled(ClassPath, false);
    }
    else if (NormalizedCommand.Equals(TEXT("Remove"), ESearchCase::IgnoreCase))
    {
        FilterList->SetAssetTypeFilterCheckState(ClassPath, ECheckBoxState::Unchecked, false);
    }
    else if (NormalizedCommand.Equals(TEXT("Toggle"), ESearchCase::IgnoreCase))
    {
        if (FilterList->GetAssetTypeFilterCheckState(ClassPath) != ECheckBoxState::Checked)
        {
            FilterList->SetAssetTypeFilterCheckState(ClassPath, ECheckBoxState::Checked, true);
        }
        else
        {
            FilterList->ToggleAssetTypeFilterEnabled(ClassPath, !FilterList->IsAssetTypeFilterActive(ClassPath));
        }
    }
    else if (!NormalizedCommand.Equals(TEXT("Check"), ESearchCase::IgnoreCase))
    {
        return TEXT("InvalidCommand");
    }

    return GetAssetFilterStatus(FilterList.ToSharedRef(), ClassPath);
}

bool UQuickerBridgeLibrary::ClearContentBrowserFilters()
{
    if (!IsInGameThread())
    {
        return false;
    }

    const TSharedPtr<SFilterList> FilterList = FindContentBrowserFilterListUnderMouse();
    if (!FilterList.IsValid())
    {
        return false;
    }

    FilterList->RemoveAllFilters();
    return true;
}

FString UQuickerBridgeLibrary::ToggleContentBrowserCommonFilters(const FString&)
{
    if (!IsInGameThread())
    {
        return TEXT("WrongThread");
    }

    const TSharedPtr<SFilterList> FilterList = FindContentBrowserFilterListUnderMouse();
    if (!FilterList.IsValid())
    {
        return TEXT("ContentBrowserNotFound");
    }

    if (FilterList->HasAnyFilters())
    {
        FilterList->RemoveAllFilters();
        return TEXT("Cleared");
    }

    int32 EnabledCount = 0;
    for (UAssetDefinition* AssetDefinition : UAssetDefinitionRegistry::Get()->GetAllAssetDefinitions())
    {
        if (!AssetDefinition)
        {
            continue;
        }

        bool bIsCommonFilter = false;
        for (const FAssetFilterData& FilterData : AssetDefinition->GetFilters()->Filters)
        {
            if (FilterData.FilterCategories.ContainsByPredicate([](const FAssetCategoryPath& CategoryPath)
                {
                    return CategoryPath.GetCategory() == EAssetCategoryPaths::Basic.GetCategory();
                }))
            {
                bIsCommonFilter = true;
                break;
            }
        }

        if (!bIsCommonFilter)
        {
            continue;
        }

        const FTopLevelAssetPath ClassPath = AssetDefinition->GetAssetClass().ToSoftObjectPath().GetAssetPath();
        if (FilterList->DoesAssetTypeFilterExist(ClassPath))
        {
            FilterList->SetAssetTypeFilterCheckState(ClassPath, ECheckBoxState::Checked, false);
            FilterList->ToggleAssetTypeFilterEnabled(ClassPath, false);
            ++EnabledCount;
        }
    }

    return EnabledCount > 0
        ? FString::Printf(TEXT("PinnedInactive:%d"), EnabledCount)
        : TEXT("NoCommonFiltersFound");
}
