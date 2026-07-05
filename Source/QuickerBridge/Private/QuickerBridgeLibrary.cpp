#include "QuickerBridgeLibrary.h"
#include "QuickerBridgeContextService.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Layout/WidgetPath.h"
#include "SGraphPanel.h"
#include "GraphEditor.h"
#include "Math/UnrealMathUtility.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "ModelingToolsEditorMode.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "BlueprintActionFilter.h"
#include "BlueprintActionMenuBuilder.h"
#include "BlueprintActionMenuUtils.h"
#include "BlueprintNodeSpawner.h"
#include "K2Node_CallFunction.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Timeline.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/FieldIterator.h"
#include "ScopedTransaction.h"
#include "SceneView.h" 
#include "InteractiveToolManager.h"
#include "Containers/Ticker.h" // 引入 UE 的核心异步队列模块


// ==========================================
// 辅助函数：提取鼠标下方的画布
// ==========================================
static SGraphPanel* GetGraphPanelUnderMouse(FVector2D& OutGraphCoord)
{
    if (!FSlateApplication::IsInitialized()) return nullptr;

    FVector2D MousePos;
    FWidgetPath Path;
    if (!QuickerBridgeContextService::ResolveCurrentWidgetPath(Path, &MousePos)) return nullptr;

    for (int i = Path.Widgets.Num() - 1; i >= 0; --i)
    {
        if (Path.Widgets[i].Widget->GetTypeAsString().Contains(TEXT("GraphPanel")))
        {
            if (SGraphPanel* Panel = static_cast<SGraphPanel*>(&(Path.Widgets[i].Widget.Get())))
            {
                OutGraphCoord = Panel->PanelCoordToGraphCoord(Path.Widgets[i].Geometry.AbsoluteToLocal(MousePos));
                return Panel;
            }
        }
    }
    return nullptr;
}

static UBlueprint* QuickerFindBlueprintForGraph(UEdGraph* Graph)
{
    for (UObject* OuterObj = Graph; OuterObj; OuterObj = OuterObj->GetOuter())
    {
        if (UBlueprint* Blueprint = Cast<UBlueprint>(OuterObj))
        {
            return Blueprint;
        }
    }

    return nullptr;
}

static bool QuickerIsK2Graph(UEdGraph* Graph)
{
    return Graph && Graph->GetSchema() && Graph->GetSchema()->IsA(UEdGraphSchema_K2::StaticClass());
}

static UEdGraph* QuickerGetK2GraphUnderMouse(FVector2D& OutSpawnLocation, float OffsetX, float OffsetY, SGraphPanel** OutGraphPanel = nullptr)
{
    if (OutGraphPanel)
    {
        *OutGraphPanel = nullptr;
    }

    FVector2D GraphCoord;
    SGraphPanel* FoundPanel = GetGraphPanelUnderMouse(GraphCoord);
    if (!FoundPanel)
    {
        return nullptr;
    }

    UEdGraph* Graph = FoundPanel->GetGraphObj();
    if (!QuickerIsK2Graph(Graph))
    {
        return nullptr;
    }

    OutSpawnLocation = FVector2D(GraphCoord.X + OffsetX, GraphCoord.Y + OffsetY);
    if (OutGraphPanel)
    {
        *OutGraphPanel = FoundPanel;
    }
    return Graph;
}

static UEdGraphNode* QuickerGetSingleSelectedGraphNode(SGraphPanel* GraphPanel, UEdGraph* Graph)
{
    if (!GraphPanel || !Graph)
    {
        return nullptr;
    }

    const TArray<UEdGraphNode*> SelectedNodes = GraphPanel->GetSelectedGraphNodes();
    if (SelectedNodes.Num() != 1)
    {
        return nullptr;
    }

    UEdGraphNode* SelectedNode = SelectedNodes[0];
    return SelectedNode && SelectedNode->GetGraph() == Graph ? SelectedNode : nullptr;
}

static bool QuickerIsPinAvailableForAutoConnect(const UEdGraphPin* Pin, EEdGraphPinDirection Direction, bool bExecOnly)
{
    if (!Pin || Pin->Direction != Direction || Pin->bHidden || Pin->bNotConnectable)
    {
        return false;
    }

    const bool bIsExecPin = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    return bExecOnly ? bIsExecPin : !bIsExecPin;
}

static bool QuickerTryConnectPins(UEdGraph* Graph, UEdGraphNode* SourceNode, UEdGraphNode* NewNode, bool bExecOnly)
{
    if (!Graph || !SourceNode || !NewNode || SourceNode == NewNode || !Graph->GetSchema())
    {
        return false;
    }

    const UEdGraphSchema* Schema = Graph->GetSchema();
    for (UEdGraphPin* SourcePin : SourceNode->Pins)
    {
        if (!QuickerIsPinAvailableForAutoConnect(SourcePin, EGPD_Output, bExecOnly))
        {
            continue;
        }

        for (UEdGraphPin* TargetPin : NewNode->Pins)
        {
            if (!QuickerIsPinAvailableForAutoConnect(TargetPin, EGPD_Input, bExecOnly))
            {
                continue;
            }

            const FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
            const bool bCanTryConnection =
                Response.Response != CONNECT_RESPONSE_DISALLOW &&
                Response.Response != CONNECT_RESPONSE_BREAK_OTHERS_A &&
                Response.Response != CONNECT_RESPONSE_BREAK_OTHERS_AB;

            if (bCanTryConnection && Schema->TryCreateConnection(SourcePin, TargetPin))
            {
                return true;
            }
        }
    }

    return false;
}

static bool QuickerAutoConnectSelectedNodeToNewNode(UEdGraph* Graph, UEdGraphNode* SourceNode, UEdGraphNode* NewNode)
{
    return QuickerTryConnectPins(Graph, SourceNode, NewNode, true) ||
        QuickerTryConnectPins(Graph, SourceNode, NewNode, false);
}

static UObject* QuickerFinishSpawnedBlueprintNode(UEdGraph* Graph, UEdGraphNode* NewNode, UEdGraphNode* SourceNode = nullptr)
{
    if (!Graph || !NewNode)
    {
        return nullptr;
    }

    if (SourceNode)
    {
        QuickerAutoConnectSelectedNodeToNewNode(Graph, SourceNode, NewNode);
    }

    Graph->NotifyGraphChanged();
    if (UBlueprint* Blueprint = QuickerFindBlueprintForGraph(Graph))
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    return NewNode;
}

static UFunction* QuickerFindFunctionByName(UClass* OwnerClass, const FString& FunctionName)
{
    if (!OwnerClass || FunctionName.IsEmpty())
    {
        return nullptr;
    }

    if (UFunction* ExactFunction = OwnerClass->FindFunctionByName(FName(*FunctionName)))
    {
        return ExactFunction;
    }

    const FString NormalizedInput = FunctionName.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
    for (TFieldIterator<UFunction> It(OwnerClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
    {
        UFunction* Function = *It;
        const FString NormalizedFunctionName = Function->GetName().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
        if (NormalizedFunctionName.Equals(NormalizedInput, ESearchCase::IgnoreCase))
        {
            return Function;
        }
    }

    return nullptr;
}

static UK2Node_CallFunction* QuickerSpawnK2FunctionNode(UEdGraph* Graph, const FVector2D& Location, UFunction* Function)
{
    if (!Graph || !Function)
    {
        return nullptr;
    }

    Graph->Modify();
    UK2Node_CallFunction* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_CallFunction>(
        Graph,
        Location,
        EK2NewNodeFlags::SelectNewNode,
        [Function](UK2Node_CallFunction* Node)
        {
            Node->SetFromFunction(Function);
        }
    );

    return NewNode;
}

static UEdGraphNode* QuickerSpawnBlueprintClassNode(UEdGraph* Graph, const FVector2D& Location, UClass* NodeClass)
{
    if (!Graph || !NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
    {
        return nullptr;
    }

    Graph->Modify();
    TSubclassOf<UEdGraphNode> GraphNodeClass(NodeClass);
    UBlueprintNodeSpawner* Spawner = UBlueprintNodeSpawner::Create(GraphNodeClass);
    if (!Spawner)
    {
        return nullptr;
    }

    IBlueprintNodeBinder::FBindingSet Bindings;
    return Spawner->Invoke(Graph, Bindings, Location);
}

static UEdGraph* QuickerResolveMacroGraph(const FString& MacroGraphPath)
{
    if (MacroGraphPath.IsEmpty())
    {
        return nullptr;
    }

    UEdGraph* MacroGraph = Cast<UEdGraph>(StaticLoadObject(UEdGraph::StaticClass(), nullptr, *MacroGraphPath));
    if (MacroGraph)
    {
        return MacroGraph;
    }

    if (!MacroGraphPath.Contains(TEXT("/")) && !MacroGraphPath.Contains(TEXT(".")) && !MacroGraphPath.Contains(TEXT(":")))
    {
        const FString StandardMacroPath = FString::Printf(TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros:%s"), *MacroGraphPath);
        MacroGraph = Cast<UEdGraph>(StaticLoadObject(UEdGraph::StaticClass(), nullptr, *StandardMacroPath));
    }

    return MacroGraph;
}

static UEdGraphNode* QuickerSpawnBlueprintMacroNode(UEdGraph* Graph, const FVector2D& Location, UEdGraph* MacroGraph)
{
    if (!Graph || !MacroGraph)
    {
        return nullptr;
    }

    Graph->Modify();
    UK2Node_MacroInstance* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_MacroInstance>(
        Graph,
        Location,
        EK2NewNodeFlags::SelectNewNode,
        [MacroGraph](UK2Node_MacroInstance* Node)
        {
            Node->SetMacroGraph(MacroGraph);
        }
    );

    if (NewNode)
    {
        NewNode->ReconstructNode();
    }

    return NewNode;
}

static UEdGraphNode* QuickerSpawnBlueprintTimelineNode(UEdGraph* Graph, const FVector2D& Location)
{
    if (!Graph)
    {
        return nullptr;
    }

    UBlueprint* Blueprint = QuickerFindBlueprintForGraph(Graph);
    if (!Blueprint || !FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
    {
        return nullptr;
    }

    const FName TimelineName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint);
    UTimelineTemplate* TimelineTemplate = FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName);
    if (!TimelineTemplate)
    {
        return nullptr;
    }

    Graph->Modify();
    UK2Node_Timeline* NewNode = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_Timeline>(
        Graph,
        Location,
        EK2NewNodeFlags::SelectNewNode,
        [TimelineName](UK2Node_Timeline* Node)
        {
            Node->TimelineName = TimelineName;
            Node->TimelineGuid = FGuid::NewGuid();
            Node->ErrorMsg.Empty();
            Node->bHasCompilerMessage = false;
        }
    );

    if (!NewNode)
    {
        FBlueprintEditorUtils::RemoveTimeline(Blueprint, TimelineTemplate, false);
        return nullptr;
    }

    NewNode->ReconstructNode();
    return NewNode;
}

static FString QuickerNormalizeNodeSearchText(const FString& Text)
{
    FString Result = Text;
    Result.ReplaceInline(TEXT(" "), TEXT(""));
    Result.ReplaceInline(TEXT("_"), TEXT(""));
    Result.ReplaceInline(TEXT("-"), TEXT(""));
    Result.ReplaceInline(TEXT("\t"), TEXT(""));
    Result.ReplaceInline(TEXT("\r"), TEXT(""));
    Result.ReplaceInline(TEXT("\n"), TEXT(""));
    return Result.ToLower();
}

static bool QuickerIsTimelineActionName(const FString& NodeName)
{
    const FString Normalized = QuickerNormalizeNodeSearchText(NodeName).Replace(TEXT("."), TEXT(""));
    return Normalized == TEXT("timeline") ||
        Normalized == TEXT("addtimeline") ||
        Normalized == TEXT("k2nodetimeline") ||
        Normalized == TEXT("时间轴");
}

static void QuickerGetActionSearchCandidates(const FEdGraphSchemaAction& Action, TArray<FString>& OutCandidates)
{
    OutCandidates.Add(Action.GetMenuDescription().ToString());
    OutCandidates.Add(Action.GetFullSearchText());

    OutCandidates.Append(Action.GetMenuDescriptionArray());
    OutCandidates.Append(Action.GetLocalizedMenuDescriptionArray());
    OutCandidates.Append(Action.GetSearchTitleArray());
    OutCandidates.Append(Action.GetLocalizedSearchTitleArray());
    OutCandidates.Append(Action.GetSearchKeywordsArray());
    OutCandidates.Append(Action.GetLocalizedSearchKeywordsArray());
}

struct FQuickerBlueprintActionCache
{
    FString ContextKey;
    TMap<FString, TSharedPtr<FEdGraphSchemaAction>> ActionsByName;
};

static FQuickerBlueprintActionCache GQuickerBlueprintActionCache;

static FString QuickerGetBlueprintActionContextKey(UEdGraph* Graph, UBlueprint* Blueprint)
{
    const FString GraphPath = Graph ? Graph->GetPathName() : TEXT("None");
    const FString BlueprintPath = Blueprint ? Blueprint->GetPathName() : TEXT("None");
    const FString ParentClassPath = Blueprint && Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT("None");
    const FString SchemaClassPath = Graph && Graph->GetSchema() ? Graph->GetSchema()->GetClass()->GetPathName() : TEXT("None");

    return FString::Printf(TEXT("%s|%s|%s|%s"), *GraphPath, *BlueprintPath, *ParentClassPath, *SchemaClassPath);
}

static void QuickerCacheBlueprintAction(TSharedPtr<FEdGraphSchemaAction> Action)
{
    if (!Action.IsValid())
    {
        return;
    }

    TArray<FString> Candidates;
    QuickerGetActionSearchCandidates(*Action, Candidates);

    for (const FString& Candidate : Candidates)
    {
        const FString NormalizedCandidate = QuickerNormalizeNodeSearchText(Candidate);
        if (!NormalizedCandidate.IsEmpty() && !GQuickerBlueprintActionCache.ActionsByName.Contains(NormalizedCandidate))
        {
            GQuickerBlueprintActionCache.ActionsByName.Add(NormalizedCandidate, Action);
        }
    }
}

static TSharedPtr<FEdGraphSchemaAction> QuickerFindCachedBlueprintAction(UEdGraph* Graph, UBlueprint* Blueprint, const FString& NodeName)
{
    if (!Graph || !Blueprint || NodeName.IsEmpty())
    {
        return nullptr;
    }

    const FString ContextKey = QuickerGetBlueprintActionContextKey(Graph, Blueprint);
    if (GQuickerBlueprintActionCache.ContextKey != ContextKey)
    {
        GQuickerBlueprintActionCache.ContextKey = ContextKey;
        GQuickerBlueprintActionCache.ActionsByName.Reset();

        FBlueprintActionContext Context;
        Context.Blueprints.Add(Blueprint);
        Context.Graphs.Add(Graph);

        FBlueprintActionMenuBuilder MenuBuilder(FBlueprintActionMenuBuilder::DefaultConfig);
        const uint32 ClassTargetMask =
            EContextTargetFlags::TARGET_Blueprint |
            EContextTargetFlags::TARGET_BlueprintLibraries |
            EContextTargetFlags::TARGET_NonImportedTypes;

        // Build the same broad action set used by the Blueprint right-click menu once per graph context.
        FBlueprintActionMenuUtils::MakeContextMenu(Context, false, ClassTargetMask, MenuBuilder);
        MenuBuilder.RebuildActionList();

        for (int32 ActionIndex = 0; ActionIndex < MenuBuilder.GetNumActions(); ++ActionIndex)
        {
            QuickerCacheBlueprintAction(MenuBuilder.GetSchemaAction(ActionIndex));
        }
    }

    return GQuickerBlueprintActionCache.ActionsByName.FindRef(QuickerNormalizeNodeSearchText(NodeName));
}

static UEdGraphNode* QuickerSpawnBlueprintActionByName(UEdGraph* Graph, const FString& NodeName, const FVector2D& Location)
{
    if (!Graph || NodeName.IsEmpty())
    {
        return nullptr;
    }

    UBlueprint* Blueprint = QuickerFindBlueprintForGraph(Graph);
    if (!Blueprint)
    {
        return nullptr;
    }

    if (QuickerIsTimelineActionName(NodeName))
    {
        return QuickerSpawnBlueprintTimelineNode(Graph, Location);
    }

    TSharedPtr<FEdGraphSchemaAction> BestAction = QuickerFindCachedBlueprintAction(Graph, Blueprint, NodeName);

    if (!BestAction.IsValid())
    {
        return nullptr;
    }

    Graph->Modify();
    UEdGraphNode* NewNode = BestAction->PerformAction(Graph, nullptr, FVector2f(static_cast<float>(Location.X), static_cast<float>(Location.Y)), true);
    if (NewNode)
    {
        Graph->NotifyGraphChanged();
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    }

    return NewNode;
}

enum class EQuickerBlueprintSpawnKind : uint8
{
    Action,
    Class,
    Macro,
    Function
};

struct FQuickerBlueprintSpawnRequest
{
    EQuickerBlueprintSpawnKind Kind = EQuickerBlueprintSpawnKind::Action;
    FString Target;
    UClass* TargetClass = nullptr;
    float OffsetX = 0.0f;
    float OffsetY = 0.0f;
};

static UObject* QuickerDispatchBlueprintNodeAtMouse(const FQuickerBlueprintSpawnRequest& Request)
{
    FVector2D SpawnLocation;
    SGraphPanel* GraphPanel = nullptr;
    UEdGraph* Graph = QuickerGetK2GraphUnderMouse(
        SpawnLocation,
        Request.OffsetX,
        Request.OffsetY,
        &GraphPanel);
    if (!Graph)
    {
        return nullptr;
    }

    UEdGraphNode* SourceNode = QuickerGetSingleSelectedGraphNode(GraphPanel, Graph);
    UEdGraphNode* NewNode = nullptr;

    FScopedTransaction Transaction(FText::FromString(TEXT("QuickerBridge: Spawn Blueprint Node")));
    switch (Request.Kind)
    {
    case EQuickerBlueprintSpawnKind::Action:
        NewNode = QuickerSpawnBlueprintActionByName(Graph, Request.Target, SpawnLocation);
        break;

    case EQuickerBlueprintSpawnKind::Class:
        if (Request.TargetClass && Request.TargetClass->IsChildOf(UEdGraphNode::StaticClass()))
        {
            NewNode = QuickerSpawnBlueprintClassNode(Graph, SpawnLocation, Request.TargetClass);
        }
        break;

    case EQuickerBlueprintSpawnKind::Macro:
        if (UEdGraph* MacroGraph = QuickerResolveMacroGraph(Request.Target))
        {
            NewNode = QuickerSpawnBlueprintMacroNode(Graph, SpawnLocation, MacroGraph);
        }
        break;

    case EQuickerBlueprintSpawnKind::Function:
        if (UFunction* Function = QuickerFindFunctionByName(Request.TargetClass, Request.Target))
        {
            NewNode = QuickerSpawnK2FunctionNode(Graph, SpawnLocation, Function);
        }
        break;
    }

    return QuickerFinishSpawnedBlueprintNode(Graph, NewNode, SourceNode);
}


// ==========================================
// 1. 鼠标处生成材质节点
// ==========================================
UMaterialExpression* UQuickerBridgeLibrary::SpawnMaterialNodeAtMouse(TSubclassOf<UMaterialExpression> NodeClass, float OffsetX, float OffsetY)
{
    if (!NodeClass) return nullptr;

    FVector2D GraphCoord;
    SGraphPanel* FoundPanel = GetGraphPanelUnderMouse(GraphCoord);
    if (!FoundPanel) return nullptr;

    UEdGraph* Graph = FoundPanel->GetGraphObj();
    if (!Graph) return nullptr;

    UObject* TargetAsset = nullptr;
    UObject* Outer = Graph->GetOuter();
    while (Outer)
    {
        if (Outer->IsA<UMaterial>() || Outer->IsA<UMaterialFunction>())
        {
            TargetAsset = Outer;
            break;
        }
        Outer = Outer->GetOuter();
    }

    if (!IsValid(TargetAsset)) return nullptr;

    const float FinalX = GraphCoord.X + OffsetX;
    const float FinalY = GraphCoord.Y + OffsetY;

    FScopedTransaction Transaction(FText::FromString(TEXT("QuickerBridge: Quick Spawn Node")));
    TargetAsset->Modify();

    UMaterialExpression* NewExpression = nullptr;
    UMaterialGraph* TargetGraph = nullptr;

    if (UMaterial* Material = Cast<UMaterial>(TargetAsset))
    {
        NewExpression = NewObject<UMaterialExpression>(Material, NodeClass);
        NewExpression->Material = Material;
        if (Material->GetEditorOnlyData())
        {
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(NewExpression);
        }
        TargetGraph = Cast<UMaterialGraph>(Material->MaterialGraph);
    }
    else if (UMaterialFunction* MatFunction = Cast<UMaterialFunction>(TargetAsset))
    {
        NewExpression = NewObject<UMaterialExpression>(MatFunction, NodeClass);
        NewExpression->Function = MatFunction;
        MatFunction->GetExpressionCollection().Expressions.Add(NewExpression);
        TargetGraph = Cast<UMaterialGraph>(MatFunction->MaterialGraph);
    }

    if (NewExpression && IsValid(TargetGraph))
    {
        TargetGraph->Modify();
        if (UMaterialGraphNode* GraphNode = TargetGraph->AddExpression(NewExpression, true))
        {
            GraphNode->NodePosX = FMath::RoundToInt(FinalX);
            GraphNode->NodePosY = FMath::RoundToInt(FinalY);
            GraphNode->SnapToGrid(16); 
        }
        
        TargetAsset->PostEditChange();
        TargetGraph->NotifyGraphChanged();
        
        return NewExpression;
    }

    return nullptr;
}


// ==========================================
// 2. 刷新连线
// ==========================================
UObject* UQuickerBridgeLibrary::SpawnBlueprintFunctionNodeAtMouse(TSubclassOf<UObject> FunctionOwnerClass, const FString& FunctionName, float OffsetX, float OffsetY)
{
    FQuickerBlueprintSpawnRequest Request;
    Request.Kind = EQuickerBlueprintSpawnKind::Function;
    Request.Target = FunctionName;
    Request.TargetClass = FunctionOwnerClass.Get();
    Request.OffsetX = OffsetX;
    Request.OffsetY = OffsetY;
    return QuickerDispatchBlueprintNodeAtMouse(Request);
}

UObject* UQuickerBridgeLibrary::SpawnBlueprintNodeAtMouse(const FString& NodeName, float OffsetX, float OffsetY)
{
    return SpawnBlueprintActionNodeAtMouse(NodeName, OffsetX, OffsetY);
}

UObject* UQuickerBridgeLibrary::SpawnBlueprintActionNodeAtMouse(const FString& NodeName, float OffsetX, float OffsetY)
{
    FQuickerBlueprintSpawnRequest Request;
    Request.Kind = EQuickerBlueprintSpawnKind::Action;
    Request.Target = NodeName;
    Request.OffsetX = OffsetX;
    Request.OffsetY = OffsetY;
    return QuickerDispatchBlueprintNodeAtMouse(Request);
}

UObject* UQuickerBridgeLibrary::SpawnBlueprintClassNodeAtMouse(TSubclassOf<UObject> NodeClass, float OffsetX, float OffsetY)
{
    FQuickerBlueprintSpawnRequest Request;
    Request.Kind = EQuickerBlueprintSpawnKind::Class;
    Request.TargetClass = NodeClass.Get();
    Request.OffsetX = OffsetX;
    Request.OffsetY = OffsetY;
    return QuickerDispatchBlueprintNodeAtMouse(Request);
}

UObject* UQuickerBridgeLibrary::SpawnBlueprintMacroNodeAtMouse(const FString& MacroGraphPath, float OffsetX, float OffsetY)
{
    FQuickerBlueprintSpawnRequest Request;
    Request.Kind = EQuickerBlueprintSpawnKind::Macro;
    Request.Target = MacroGraphPath;
    Request.OffsetX = OffsetX;
    Request.OffsetY = OffsetY;
    return QuickerDispatchBlueprintNodeAtMouse(Request);
}

void UQuickerBridgeLibrary::RedrawGraphWires(UMaterialExpression* Node)
{
    if (!Node || !Node->GraphNode) return;

    if (UEdGraph* Graph = Node->GraphNode->GetGraph())
    {
        if (UMaterialGraph* MatGraph = Cast<UMaterialGraph>(Graph))
        {
            MatGraph->LinkGraphNodesFromMaterial();
        }
        Graph->NotifyGraphChanged();
    }
}
