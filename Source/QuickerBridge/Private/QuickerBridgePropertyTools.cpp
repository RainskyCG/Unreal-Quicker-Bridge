#include "QuickerBridgeLibrary.h"
#include "Editor.h"
#include "UObject/UnrealType.h"

FString UQuickerBridgeLibrary::ForceGetEditorProperty(UObject* TargetObject, const FString& PropertyName)
{
    if (!TargetObject) return TEXT("");

    if (FProperty* Prop = TargetObject->GetClass()->FindPropertyByName(FName(*PropertyName)))
    {
        FString OutValue;
        Prop->ExportTextItem_Direct(OutValue, Prop->ContainerPtrToValuePtr<void>(TargetObject), nullptr, TargetObject, PPF_None);
        return OutValue;
    }
    return TEXT("");
}

bool UQuickerBridgeLibrary::ForceSetEditorProperty(UObject* TargetObject, const FString& PropertyName, const FString& PropertyValue)
{
    if (!TargetObject) return false;

    if (FProperty* Prop = TargetObject->GetClass()->FindPropertyByName(FName(*PropertyName)))
    {
        TargetObject->Modify();
        
        Prop->ImportText_Direct(*PropertyValue, Prop->ContainerPtrToValuePtr<void>(TargetObject), TargetObject, PPF_None);
        
        TargetObject->PostEditChange();
        TargetObject->SaveConfig();
        if (GEditor)
        {
            GEditor->RedrawLevelEditingViewports();
        }
        return true;
    }
    return false;
}
