#include "QuickerBridge.h"
#include "QuickerBridgeScrollLockController.h"
#include "QuickerBridgeContextService.h"
#include "QuickerBridgeInstanceCoordinator.h"
#include "QuickerBridgeSettings.h"
#include "AssetRegistry/AssetData.h"
#include "Components/SceneComponent.h"
#include "ContentBrowserItemPath.h"
#include "ContentBrowserModule.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Commands/Commands.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/InputChord.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "InputCoreTypes.h"
#include "Misc/EngineVersion.h"
#include "Styling/AppStyle.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Layout/WidgetPath.h"
#include "Misc/Char.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "EditorModeManager.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Literal.h"
#include "K2Node_MacroInstance.h"
#include "SGraphPanel.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerResponse.h"
#include "HttpServerRequest.h"
#include "HttpPath.h"
#include "HttpRouteHandle.h"
#include "Async/Async.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "FQuickerBridgeModule"

static constexpr int32 QuickerBridgeHttpPort = 18080;
static const TCHAR* QuickerBridgeHttpHost = TEXT("127.0.0.1");
static const TCHAR* QuickerBridgeProbeShortcut = TEXT("Ctrl+Alt+P");
static const TCHAR* QuickerBridgeClipboardPythonShortcut = TEXT("Ctrl+Alt+Shift+V");

static FHttpRouteHandle HealthRouteHandle;
static FHttpRouteHandle GetContextRouteHandle;
static FHttpRouteHandle GetContextDebugRouteHandle;
static FHttpRouteHandle GetContextDetailRouteHandle;
static FHttpRouteHandle ExecutePythonRouteHandle;
static TSharedPtr<FUICommandList> QuickerBridgeCommandList;
static TSharedPtr<IInputProcessor> QuickerBridgeInputProcessor;

static FString GetHoveredContext();
static TSharedRef<FJsonObject> GetHoveredContextDetail();

class FQuickerBridgeCommands final : public TCommands<FQuickerBridgeCommands>
{
public:
    FQuickerBridgeCommands()
        : TCommands<FQuickerBridgeCommands>(
            TEXT("QuickerBridge"),
            NSLOCTEXT("Contexts", "QuickerBridge", "QuickerBridge"),
            NAME_None,
            FAppStyle::GetAppStyleSetName())
    {
    }

    virtual void RegisterCommands() override
    {
        UI_COMMAND(RunContextProbe, "QuickerBridge Probe", "Copy the current QuickerBridge context detail to the clipboard.", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control | EModifierKey::Alt, EKeys::P));
        UI_COMMAND(ExecuteClipboardPython, "Run Clipboard Python", "Execute Unreal Python code from the clipboard.", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control | EModifierKey::Alt | EModifierKey::Shift, EKeys::V));
    }

    TSharedPtr<FUICommandInfo> RunContextProbe;
    TSharedPtr<FUICommandInfo> ExecuteClipboardPython;
};

static TUniquePtr<FHttpServerResponse> PrepareQuickerBridgeResponse(TUniquePtr<FHttpServerResponse> Response)
{
    if (Response)
    {
        Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store, no-cache, must-revalidate, max-age=0") });
        Response->Headers.Add(TEXT("Pragma"), { TEXT("no-cache") });
        Response->Headers.Add(TEXT("Expires"), { TEXT("0") });
        Response->Headers.Add(TEXT("Connection"), { TEXT("close") });
        Response->Headers.Add(TEXT("X-QuickerBridge-ProcessId"), { FString::FromInt(FPlatformProcess::GetCurrentProcessId()) });
        Response->Headers.Add(TEXT("X-QuickerBridge-Project"), { FApp::GetProjectName() });
    }

    return Response;
}

static FString SerializeJsonObject(const TSharedRef<FJsonObject>& JsonObject)
{
    FString JsonText;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
    FJsonSerializer::Serialize(JsonObject, Writer);
    return JsonText;
}

static TUniquePtr<FHttpServerResponse> CreateJsonResponse(
    const TSharedRef<FJsonObject>& JsonObject,
    EHttpServerResponseCodes ResponseCode = EHttpServerResponseCodes::Ok)
{
    TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(SerializeJsonObject(JsonObject), TEXT("application/json; charset=utf-8"));
    Response->Code = ResponseCode;
    return PrepareQuickerBridgeResponse(MoveTemp(Response));
}

static TUniquePtr<FHttpServerResponse> CreatePythonErrorResponse(
    const FString& ErrorMessage,
    EHttpServerResponseCodes ResponseCode)
{
    const TSharedRef<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetBoolField(TEXT("success"), false);
    JsonObject->SetStringField(TEXT("error"), ErrorMessage);
    return CreateJsonResponse(JsonObject, ResponseCode);
}

static TUniquePtr<FHttpServerResponse> ExecutePythonCommand(const FString& Command, EPythonCommandExecutionMode ExecutionMode)
{
    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    if (!PythonPlugin || !PythonPlugin->IsPythonInitialized())
    {
        return CreatePythonErrorResponse(TEXT("PythonScriptPlugin is not initialized."), EHttpServerResponseCodes::ServiceUnavail);
    }

    FPythonCommandEx PythonCommand;
    PythonCommand.Command = Command;
    PythonCommand.ExecutionMode = ExecutionMode;
    PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;

    const bool bSuccess = PythonPlugin->ExecPythonCommandEx(PythonCommand);

    const TSharedRef<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetBoolField(TEXT("success"), bSuccess);
    JsonObject->SetStringField(TEXT("result"), PythonCommand.CommandResult);

    TArray<TSharedPtr<FJsonValue>> OutputEntries;
    OutputEntries.Reserve(PythonCommand.LogOutput.Num());
    for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
    {
        const TSharedRef<FJsonObject> OutputObject = MakeShared<FJsonObject>();
        OutputObject->SetStringField(TEXT("type"), LexToString(Entry.Type));
        OutputObject->SetStringField(TEXT("output"), Entry.Output);
        OutputEntries.Add(MakeShared<FJsonValueObject>(OutputObject));
    }
    JsonObject->SetArrayField(TEXT("output"), MoveTemp(OutputEntries));

    return CreateJsonResponse(
        JsonObject,
        bSuccess ? EHttpServerResponseCodes::Ok : EHttpServerResponseCodes::ServerError);
}

static void ShowQuickerBridgeNotification(const FText& Message, SNotificationItem::ECompletionState CompletionState)
{
    FNotificationInfo Info(Message);
    Info.ExpireDuration = 3.0f;
    Info.bUseSuccessFailIcons = true;

    TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(Info);
    if (Notification.IsValid())
    {
        Notification->SetCompletionState(CompletionState);
    }
}

static TSharedRef<FJsonObject> GetHealthPayload()
{
    const UQuickerBridgeSettings* Settings = GetDefault<UQuickerBridgeSettings>();
    IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
    const bool bMultiUESwitchingEnabled = Settings && Settings->bEnableMultiUEPortSwitching;

    const TSharedRef<FJsonObject> JsonObject = MakeShared<FJsonObject>();
    JsonObject->SetBoolField(TEXT("ok"), true);
    JsonObject->SetStringField(TEXT("service"), TEXT("QuickerBridge"));
    JsonObject->SetStringField(TEXT("project"), FApp::GetProjectName());
    JsonObject->SetNumberField(TEXT("processId"), FPlatformProcess::GetCurrentProcessId());
    JsonObject->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
    JsonObject->SetStringField(TEXT("host"), QuickerBridgeHttpHost);
    JsonObject->SetNumberField(TEXT("httpPort"), QuickerBridgeHttpPort);
    JsonObject->SetStringField(TEXT("baseUrl"), FString::Printf(TEXT("http://%s:%d"), QuickerBridgeHttpHost, QuickerBridgeHttpPort));
    JsonObject->SetBoolField(TEXT("pythonInitialized"), PythonPlugin && PythonPlugin->IsPythonInitialized());
    JsonObject->SetBoolField(TEXT("supportsClipboardPython"), true);
    JsonObject->SetBoolField(TEXT("multiUEPortSwitchingEnabled"), bMultiUESwitchingEnabled);
    JsonObject->SetBoolField(TEXT("multiUECoordinatorRunning"), QuickerBridgeInstanceCoordinator::IsRunning());
    JsonObject->SetBoolField(TEXT("ownsHttpPort"), bMultiUESwitchingEnabled ? QuickerBridgeInstanceCoordinator::OwnsPort() : true);
    JsonObject->SetStringField(TEXT("probeShortcut"), QuickerBridgeProbeShortcut);
    JsonObject->SetStringField(TEXT("clipboardPythonShortcut"), QuickerBridgeClipboardPythonShortcut);

    const TSharedRef<FJsonObject> RoutesObject = MakeShared<FJsonObject>();
    RoutesObject->SetBoolField(TEXT("health"), HealthRouteHandle.IsValid());
    RoutesObject->SetBoolField(TEXT("contextCurrent"), GetContextRouteHandle.IsValid());
    RoutesObject->SetBoolField(TEXT("contextDetail"), GetContextDetailRouteHandle.IsValid());
    RoutesObject->SetBoolField(TEXT("contextDebug"), GetContextDebugRouteHandle.IsValid());
    RoutesObject->SetBoolField(TEXT("pythonExecute"), ExecutePythonRouteHandle.IsValid());
    JsonObject->SetObjectField(TEXT("routesBound"), RoutesObject);

    const TSharedRef<FJsonObject> EndpointsObject = MakeShared<FJsonObject>();
    EndpointsObject->SetStringField(TEXT("health"), TEXT("GET /health"));
    EndpointsObject->SetStringField(TEXT("contextCurrent"), TEXT("GET /context/current"));
    EndpointsObject->SetStringField(TEXT("contextDetail"), TEXT("GET /context/detail"));
    EndpointsObject->SetStringField(TEXT("contextDebug"), TEXT("GET /context/debug"));
    EndpointsObject->SetStringField(TEXT("pythonExecute"), TEXT("POST /python/execute"));
    JsonObject->SetObjectField(TEXT("endpoints"), EndpointsObject);

    return JsonObject;
}

static void RunQuickerBridgeContextProbe()
{
    const TSharedRef<FJsonObject> Detail = GetHoveredContextDetail();
    FPlatformApplicationMisc::ClipboardCopy(*SerializeJsonObject(Detail));

    FString CurrentContext;
    Detail->TryGetStringField(TEXT("current"), CurrentContext);
    ShowQuickerBridgeNotification(
        FText::Format(LOCTEXT("ContextProbeCopied", "QuickerBridge probe copied: {0}"), FText::FromString(CurrentContext)),
        SNotificationItem::CS_Success);
}

static void RunQuickerBridgeClipboardPython()
{
    FString ClipboardText;
    FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
    ClipboardText.TrimStartAndEndInline();

    if (ClipboardText.IsEmpty())
    {
        ShowQuickerBridgeNotification(
            LOCTEXT("ClipboardPythonEmpty", "Clipboard is empty."),
            SNotificationItem::CS_Fail);
        return;
    }

    TUniquePtr<FHttpServerResponse> Response = ExecutePythonCommand(ClipboardText, EPythonCommandExecutionMode::ExecuteFile);
    const bool bSuccess = Response && Response->Code == EHttpServerResponseCodes::Ok;
    ShowQuickerBridgeNotification(
        bSuccess ? LOCTEXT("ClipboardPythonExecuted", "Clipboard Python executed.") : LOCTEXT("ClipboardPythonFailed", "Clipboard Python failed."),
        bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
}

class FQuickerBridgeInputProcessor final : public IInputProcessor
{
public:
    virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override
    {
    }

    virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
    {
        return QuickerBridgeCommandList.IsValid() && QuickerBridgeCommandList->ProcessCommandBindings(InKeyEvent);
    }

    virtual const TCHAR* GetDebugName() const override
    {
        return TEXT("QuickerBridgeInputProcessor");
    }
};

static void RegisterQuickerBridgeEditorCommands()
{
    if (!FQuickerBridgeCommands::IsRegistered())
    {
        FQuickerBridgeCommands::Register();
    }

    if (!QuickerBridgeCommandList.IsValid())
    {
        QuickerBridgeCommandList = MakeShared<FUICommandList>();
        const FQuickerBridgeCommands& Commands = FQuickerBridgeCommands::Get();
        QuickerBridgeCommandList->MapAction(
            Commands.RunContextProbe,
            FExecuteAction::CreateStatic(&RunQuickerBridgeContextProbe));
        QuickerBridgeCommandList->MapAction(
            Commands.ExecuteClipboardPython,
            FExecuteAction::CreateStatic(&RunQuickerBridgeClipboardPython));

        FInputBindingManager::Get().RegisterCommandList(
            FQuickerBridgeCommands::Get().GetContextName(),
            QuickerBridgeCommandList.ToSharedRef());
    }

    if (!QuickerBridgeInputProcessor.IsValid() && FSlateApplication::IsInitialized())
    {
        QuickerBridgeInputProcessor = MakeShared<FQuickerBridgeInputProcessor>();
        FSlateApplication::Get().RegisterInputPreProcessor(QuickerBridgeInputProcessor);
    }
}

static void UnregisterQuickerBridgeEditorCommands()
{
    if (QuickerBridgeInputProcessor.IsValid())
    {
        if (FSlateApplication::IsInitialized())
        {
            FSlateApplication::Get().UnregisterInputPreProcessor(QuickerBridgeInputProcessor);
        }
        QuickerBridgeInputProcessor.Reset();
    }

    if (QuickerBridgeCommandList.IsValid())
    {
        if (FQuickerBridgeCommands::IsRegistered())
        {
            FInputBindingManager::Get().UnregisterCommandList(
                FQuickerBridgeCommands::Get().GetContextName(),
                QuickerBridgeCommandList.ToSharedRef());
        }
        QuickerBridgeCommandList.Reset();
    }

    if (FQuickerBridgeCommands::IsRegistered())
    {
        FQuickerBridgeCommands::Unregister();
    }
}

static FString CleanContextToken(FString Value)
{
    Value.ReplaceInline(TEXT(" "), TEXT(""));
    Value.ReplaceInline(TEXT("."), TEXT("_"));
    Value.ReplaceInline(TEXT("/"), TEXT("_"));
    Value.ReplaceInline(TEXT("\\"), TEXT("_"));
    Value.ReplaceInline(TEXT("'"), TEXT(""));
    Value.ReplaceInline(TEXT("\""), TEXT(""));
    Value.ReplaceInline(TEXT(":"), TEXT("_"));
    if (Value.EndsWith(TEXT("_C")))
    {
        Value.LeftChopInline(2);
    }
    return Value;
}
static bool GetMouseWindowVerticalInfo(const FWidgetPath& WidgetPath, const FVector2D& MousePos, float& OutLocalY, float& OutWindowHeight)
{
    if (TSharedPtr<SWindow> TopWindow = WidgetPath.GetWindow())
    {
        const FVector2D WindowPos = TopWindow->GetPositionInScreen();
        const FVector2D WindowSize = TopWindow->GetSizeInScreen();
        if (WindowSize.Y > 0.0f)
        {
            OutLocalY = MousePos.Y - WindowPos.Y;
            OutWindowHeight = WindowSize.Y;
            return true;
        }
    }

    return false;
}

static bool IsMouseInBottomWindowBand(const FWidgetPath& WidgetPath, const FVector2D& MousePos)
{
    float LocalY = 0.0f;
    float WindowHeight = 0.0f;
    if (!GetMouseWindowVerticalInfo(WidgetPath, MousePos, LocalY, WindowHeight))
    {
        return false;
    }

    const float BottomBandHeight = FMath::Clamp(WindowHeight * 0.18f, 120.0f, 220.0f);
    return LocalY >= WindowHeight - BottomBandHeight;
}

static bool IsWidgetInBottomWindowBand(const FWidgetPath& WidgetPath, const FGeometry& WidgetGeometry)
{
    if (TSharedPtr<SWindow> TopWindow = WidgetPath.GetWindow())
    {
        const FVector2D WindowPos = TopWindow->GetPositionInScreen();
        const FVector2D WindowSize = TopWindow->GetSizeInScreen();
        if (WindowSize.Y > 0.0f)
        {
            const float LocalTopY = WidgetGeometry.GetAbsolutePosition().Y - WindowPos.Y;
            const float LocalBottomY = LocalTopY + WidgetGeometry.GetAbsoluteSize().Y;
            return LocalTopY >= WindowSize.Y * 0.5f || LocalBottomY >= WindowSize.Y - 4.0f;
        }
    }

    return false;
}

static bool TryGetMouseInfoInLargestContainingWindow(const FVector2D& MousePos, float& OutLocalY, float& OutWindowHeight)
{
    float BestArea = 0.0f;
    bool bFoundWindow = false;

    for (const TSharedRef<SWindow>& WindowRef : FSlateApplication::Get().GetInteractiveTopLevelWindows())
    {
        const FVector2D WindowPos = WindowRef->GetPositionInScreen();
        const FVector2D WindowSize = WindowRef->GetSizeInScreen();
        if (WindowSize.X <= 0.0f || WindowSize.Y <= 0.0f)
        {
            continue;
        }

        const bool bContainsMouse =
            MousePos.X >= WindowPos.X &&
            MousePos.Y >= WindowPos.Y &&
            MousePos.X <= WindowPos.X + WindowSize.X &&
            MousePos.Y <= WindowPos.Y + WindowSize.Y;
        if (!bContainsMouse)
        {
            continue;
        }

        const float Area = WindowSize.X * WindowSize.Y;
        if (Area > BestArea)
        {
            BestArea = Area;
            OutLocalY = MousePos.Y - WindowPos.Y;
            OutWindowHeight = WindowSize.Y;
            bFoundWindow = true;
        }
    }

    return bFoundWindow;
}

static bool TryGetMouseInfoInLargestPathGeometry(const FWidgetPath& WidgetPath, const FVector2D& MousePos, float& OutLocalY, float& OutGeometryHeight)
{
    float BestArea = 0.0f;
    bool bFoundGeometry = false;

    for (int32 i = 0; i < WidgetPath.Widgets.Num(); ++i)
    {
        const FArrangedWidget& ArrangedWidget = WidgetPath.Widgets[i];
        const FVector2D GeometryPos = ArrangedWidget.Geometry.GetAbsolutePosition();
        const FVector2D GeometrySize = ArrangedWidget.Geometry.GetAbsoluteSize();
        if (GeometrySize.X <= 0.0f || GeometrySize.Y <= 0.0f)
        {
            continue;
        }

        const bool bContainsMouse =
            MousePos.X >= GeometryPos.X &&
            MousePos.Y >= GeometryPos.Y &&
            MousePos.X <= GeometryPos.X + GeometrySize.X &&
            MousePos.Y <= GeometryPos.Y + GeometrySize.Y;
        if (!bContainsMouse)
        {
            continue;
        }

        const float Area = GeometrySize.X * GeometrySize.Y;
        if (Area > BestArea)
        {
            BestArea = Area;
            OutLocalY = MousePos.Y - GeometryPos.Y;
            OutGeometryHeight = GeometrySize.Y;
            bFoundGeometry = true;
        }
    }

    return bFoundGeometry;
}

static FString ClassifyWindowTitleBarContext(const FWidgetPath& WidgetPath, const FVector2D& MousePos, const FGeometry& WidgetGeometry)
{
    float LocalY = 0.0f;
    float ReferenceHeight = 0.0f;
    if (TryGetMouseInfoInLargestPathGeometry(WidgetPath, MousePos, LocalY, ReferenceHeight))
    {
        const float BottomBandHeight = FMath::Clamp(ReferenceHeight * 0.20f, 140.0f, 260.0f);
        return LocalY >= ReferenceHeight - BottomBandHeight ? TEXT("BottomCommandLine") : TEXT("WindowTitleBar");
    }

    if (TryGetMouseInfoInLargestContainingWindow(MousePos, LocalY, ReferenceHeight))
    {
        const float BottomBandHeight = FMath::Clamp(ReferenceHeight * 0.20f, 140.0f, 260.0f);
        return LocalY >= ReferenceHeight - BottomBandHeight ? TEXT("BottomCommandLine") : TEXT("WindowTitleBar");
    }

    if (IsMouseInBottomWindowBand(WidgetPath, MousePos) || IsWidgetInBottomWindowBand(WidgetPath, WidgetGeometry))
    {
        return TEXT("BottomCommandLine");
    }

    return TEXT("WindowTitleBar");
}

static FString ParseGraphNodeName(UObject* NodeObj)
{
    if (!NodeObj) return TEXT("");

    FString NodeClass = NodeObj->GetClass()->GetName();

    auto CleanContextToken = [](FString Value) -> FString
    {
        Value.ReplaceInline(TEXT(" "), TEXT(""));
        Value.ReplaceInline(TEXT("."), TEXT("_"));
        Value.ReplaceInline(TEXT("/"), TEXT("_"));
        Value.ReplaceInline(TEXT("\\"), TEXT("_"));
        Value.ReplaceInline(TEXT("'"), TEXT(""));
        Value.ReplaceInline(TEXT("\""), TEXT(""));
        Value.ReplaceInline(TEXT(":"), TEXT("_"));
        if (Value.EndsWith(TEXT("_C")))
        {
            Value.LeftChopInline(2);
        }
        return Value;
    };

    if (NodeClass == TEXT("MaterialGraphNode"))
    {
        if (FObjectProperty* ExpProp = FindFProperty<FObjectProperty>(NodeObj->GetClass(), TEXT("MaterialExpression")))
        {
            if (UObject* ExpObj = ExpProp->GetObjectPropertyValue_InContainer(NodeObj))
            {
                NodeClass = ExpObj->GetClass()->GetName();
            }
        }
    }

    if (UK2Node_Literal* LiteralNode = Cast<UK2Node_Literal>(NodeObj))
    {
        FString Result = TEXT("Literal");
        if (UObject* ObjectRef = LiteralNode->GetObjectRef())
        {
            const FString RefClass = CleanContextToken(ObjectRef->GetClass()->GetName());
            if (!RefClass.IsEmpty())
            {
                Result += TEXT("_") + RefClass;
            }
        }
        return Result;
    }

    if (UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(NodeObj))
    {
        FString Result = TEXT("CallFunction");
        const FString FunctionName = CleanContextToken(CallFunctionNode->GetFunctionName().ToString());
        if (!FunctionName.IsEmpty() && FunctionName != TEXT("None"))
        {
            Result += TEXT("_") + FunctionName;
        }
        return Result;
    }

    if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(NodeObj))
    {
        FString Result = TEXT("MacroInstance");
        if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
        {
            const FString MacroName = CleanContextToken(MacroGraph->GetName());
            if (!MacroName.IsEmpty())
            {
                Result += TEXT("_") + MacroName;
            }
        }
        return Result;
    }

    if (NodeClass.StartsWith(TEXT("K2Node_"))) return NodeClass.Mid(7);
    if (NodeClass.StartsWith(TEXT("MaterialExpression"))) return NodeClass.Mid(18);
    if (NodeClass.StartsWith(TEXT("AnimGraphNode_"))) return NodeClass.Mid(14);
    if (NodeClass.StartsWith(TEXT("NiagaraNode"))) return NodeClass.Mid(11);

    return NodeClass;
}

static FString GetGraphSuffixFromGraphPanel(SGraphPanel* GraphPanel)
{
    if (!GraphPanel) return TEXT("");

    UEdGraph* GraphObj = GraphPanel->GetGraphObj();
    if (!GraphObj) return TEXT("");

    const FString GraphClass = GraphObj->GetClass()->GetName();
    const FString GraphName = GraphObj->GetName();
    const FString SchemaClass = GraphObj->GetSchema() ? GraphObj->GetSchema()->GetClass()->GetName() : TEXT("");

    if (GraphClass.Contains(TEXT("MaterialGraph"))) return TEXT("_Material");
    if (GraphClass.Contains(TEXT("AnimationStateMachineGraph"))) return TEXT("_Anim_StateMachine");
    if (GraphClass.Contains(TEXT("AnimationStateGraph"))) return TEXT("_Anim_State");
    if (GraphClass.Contains(TEXT("AnimationTransitionGraph"))) return TEXT("_Anim_Transition");
    if (GraphClass.Contains(TEXT("AnimationGraph")) || GraphClass.Contains(TEXT("AnimGraph"))) return TEXT("_Anim");
    if (GraphClass.Contains(TEXT("Niagara"))) return TEXT("_Niagara");
    if (GraphClass.Contains(TEXT("MetaSound"))) return TEXT("_MetaSound");

    if (GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase) ||
        GraphName.Contains(TEXT("EventGraph")))
    {
        return TEXT("_Event");
    }

    if (GraphName.Equals(TEXT("UserConstructionScript"), ESearchCase::IgnoreCase) ||
        GraphName.Contains(TEXT("ConstructionScript")) ||
        GraphName.Contains(TEXT("Construction Script")))
    {
        return TEXT("_ConstructionScript");
    }

    for (UObject* OuterObj = GraphObj->GetOuter(); OuterObj; OuterObj = OuterObj->GetOuter())
    {
        if (UBlueprint* Blueprint = Cast<UBlueprint>(OuterObj))
        {
            if (Blueprint->EventGraphs.Contains(GraphObj) || Blueprint->UbergraphPages.Contains(GraphObj)) return TEXT("_Event");
            if (Blueprint->FunctionGraphs.Contains(GraphObj)) return TEXT("_Function");
            if (Blueprint->MacroGraphs.Contains(GraphObj)) return TEXT("_Macro");
            if (Blueprint->DelegateSignatureGraphs.Contains(GraphObj)) return TEXT("_Delegate");
            break;
        }
    }

    if (SchemaClass.Contains(TEXT("WidgetGraphSchema"))) return TEXT("_Widget");
    if (SchemaClass.Contains(TEXT("BehaviorTree"))) return TEXT("_BehaviorTree");
    if (SchemaClass.Contains(TEXT("ControlRig"))) return TEXT("_Rig");
    if (SchemaClass.Contains(TEXT("AnimationStateGraph"))) return TEXT("_Anim_State");
    if (SchemaClass.Contains(TEXT("AnimationTransition"))) return TEXT("_Anim_Transition");
    if (SchemaClass.Contains(TEXT("AnimationGraph"))) return TEXT("_Anim");
    if (SchemaClass.Contains(TEXT("K2")))
    {
        if (GraphName.StartsWith(TEXT("NewMacro"), ESearchCase::IgnoreCase) ||
            GraphName.Contains(TEXT("Macro")))
        {
            return TEXT("_Macro");
        }

        if (GraphName.Contains(TEXT("Function")))
        {
            return TEXT("_Function");
        }

        return TEXT("_Blueprint");
    }

    for (UObject* OuterObj = GraphObj->GetOuter(); OuterObj; OuterObj = OuterObj->GetOuter())
    {
        const FString OuterClass = OuterObj->GetClass()->GetName();
        if (OuterClass.Contains(TEXT("Material"))) return TEXT("_Material");
        if (OuterClass.Contains(TEXT("WidgetBlueprint"))) return TEXT("_Widget");
        if (OuterClass.Contains(TEXT("AnimBlueprint"))) return TEXT("_Anim");
        if (OuterClass.Contains(TEXT("Blueprint"))) return TEXT("_Blueprint");
        if (OuterClass.Contains(TEXT("Niagara"))) return TEXT("_Niagara");
        if (OuterClass.Contains(TEXT("MetaSound"))) return TEXT("_MetaSound");
        if (OuterClass.Contains(TEXT("BehaviorTree"))) return TEXT("_BehaviorTree");
        if (OuterClass.Contains(TEXT("ControlRig"))) return TEXT("_Rig");
    }

    return TEXT("");
}

static void ApplyEditorTypeFromGraphType(const FString& GraphType, FString& EditorType)
{
    if (GraphType == TEXT("Material")) EditorType = TEXT("Material");
    else if (GraphType.StartsWith(TEXT("Anim"))) EditorType = TEXT("Animation");
    else if (GraphType == TEXT("Niagara")) EditorType = TEXT("Niagara");
    else if (GraphType == TEXT("MetaSound")) EditorType = TEXT("MetaSound");
    else if (GraphType == TEXT("Widget")) EditorType = TEXT("Widget");
    else if (GraphType == TEXT("BehaviorTree")) EditorType = TEXT("BehaviorTree");
    else if (GraphType == TEXT("Rig")) EditorType = TEXT("ControlRig");
}

static TSharedRef<FJsonObject> MakeActorDetail(AActor* Actor)
{
    const TSharedRef<FJsonObject> ActorObject = MakeShared<FJsonObject>();
    if (!Actor)
    {
        return ActorObject;
    }

    ActorObject->SetStringField(TEXT("name"), Actor->GetName());
    ActorObject->SetStringField(TEXT("label"), Actor->GetActorLabel());
    ActorObject->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
    ActorObject->SetStringField(TEXT("path"), Actor->GetPathName());

    if (USceneComponent* RootComponent = Actor->GetRootComponent())
    {
        ActorObject->SetStringField(TEXT("rootComponentName"), RootComponent->GetName());
        ActorObject->SetStringField(TEXT("rootComponentClass"), RootComponent->GetClass()->GetName());
    }

    return ActorObject;
}

static void AddActorSelectionDetail(
    const TSharedRef<FJsonObject>& Detail,
    const TArray<AActor*>& SelectedActors,
    FString& Selection,
    FString& SelectionObjectType)
{
    Detail->SetNumberField(TEXT("selectedActorCount"), SelectedActors.Num());

    if (SelectedActors.Num() == 1 && SelectedActors[0])
    {
        Selection = TEXT("SingleActor");
        SelectionObjectType = SelectedActors[0]->GetClass()->GetName();
        Detail->SetStringField(TEXT("selectionObjectName"), SelectedActors[0]->GetName());
        Detail->SetStringField(TEXT("selectionObjectLabel"), SelectedActors[0]->GetActorLabel());
        Detail->SetStringField(TEXT("selectionObjectPath"), SelectedActors[0]->GetPathName());
    }
    else if (SelectedActors.Num() > 1)
    {
        Selection = TEXT("MultipleActors");
    }

    TArray<TSharedPtr<FJsonValue>> ActorEntries;
    const int32 MaxActorsToReport = FMath::Min(SelectedActors.Num(), 16);
    ActorEntries.Reserve(MaxActorsToReport);
    for (int32 ActorIndex = 0; ActorIndex < MaxActorsToReport; ++ActorIndex)
    {
        if (SelectedActors[ActorIndex])
        {
            ActorEntries.Add(MakeShared<FJsonValueObject>(MakeActorDetail(SelectedActors[ActorIndex])));
        }
    }

    Detail->SetArrayField(TEXT("selectedActors"), MoveTemp(ActorEntries));
    Detail->SetBoolField(TEXT("selectedActorsTruncated"), SelectedActors.Num() > MaxActorsToReport);
}

static TSharedRef<FJsonObject> MakeAssetDataDetail(const FAssetData& AssetData)
{
    const TSharedRef<FJsonObject> AssetObject = MakeShared<FJsonObject>();
    AssetObject->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
    AssetObject->SetStringField(TEXT("assetClass"), AssetData.AssetClassPath.GetAssetName().ToString());
    AssetObject->SetStringField(TEXT("packageName"), AssetData.PackageName.ToString());
    AssetObject->SetStringField(TEXT("packagePath"), AssetData.PackagePath.ToString());
    AssetObject->SetStringField(TEXT("objectPath"), AssetData.GetSoftObjectPath().ToString());
    return AssetObject;
}

static void AddContentBrowserDetail(
    const TSharedRef<FJsonObject>& Detail,
    FString& Selection,
    FString& SelectionObjectType)
{
    IContentBrowserSingleton& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get();

    const FContentBrowserItemPath CurrentPath = ContentBrowser.GetCurrentPath();
    if (CurrentPath.HasInternalPath())
    {
        Detail->SetStringField(TEXT("contentBrowserPath"), CurrentPath.GetInternalPathString());
    }
    Detail->SetStringField(TEXT("contentBrowserVirtualPath"), CurrentPath.GetVirtualPathString());

    TArray<FAssetData> SelectedAssets;
    ContentBrowser.GetSelectedAssets(SelectedAssets);
    TArray<FString> SelectedFolders;
    ContentBrowser.GetSelectedFolders(SelectedFolders);

    Detail->SetNumberField(TEXT("contentBrowserSelectedAssetCount"), SelectedAssets.Num());
    Detail->SetNumberField(TEXT("contentBrowserSelectedFolderCount"), SelectedFolders.Num());

    if (SelectedAssets.Num() == 1)
    {
        Selection = TEXT("SingleAsset");
        SelectionObjectType = SelectedAssets[0].AssetClassPath.GetAssetName().ToString();
        Detail->SetStringField(TEXT("selectionObjectName"), SelectedAssets[0].AssetName.ToString());
        Detail->SetStringField(TEXT("selectionObjectPath"), SelectedAssets[0].GetSoftObjectPath().ToString());
    }
    else if (SelectedAssets.Num() > 1)
    {
        Selection = TEXT("MultipleAssets");
    }
    else if (SelectedFolders.Num() == 1)
    {
        Selection = TEXT("SingleFolder");
        SelectionObjectType = TEXT("Folder");
        Detail->SetStringField(TEXT("selectionObjectName"), SelectedFolders[0]);
        Detail->SetStringField(TEXT("selectionObjectPath"), SelectedFolders[0]);
    }
    else if (SelectedFolders.Num() > 1)
    {
        Selection = TEXT("MultipleFolders");
        SelectionObjectType = TEXT("Folder");
    }

    TArray<TSharedPtr<FJsonValue>> AssetEntries;
    const int32 MaxAssetsToReport = FMath::Min(SelectedAssets.Num(), 16);
    AssetEntries.Reserve(MaxAssetsToReport);
    for (int32 AssetIndex = 0; AssetIndex < MaxAssetsToReport; ++AssetIndex)
    {
        AssetEntries.Add(MakeShared<FJsonValueObject>(MakeAssetDataDetail(SelectedAssets[AssetIndex])));
    }
    Detail->SetArrayField(TEXT("contentBrowserSelectedAssets"), MoveTemp(AssetEntries));
    Detail->SetBoolField(TEXT("contentBrowserSelectedAssetsTruncated"), SelectedAssets.Num() > MaxAssetsToReport);

    TArray<TSharedPtr<FJsonValue>> FolderEntries;
    const int32 MaxFoldersToReport = FMath::Min(SelectedFolders.Num(), 16);
    FolderEntries.Reserve(MaxFoldersToReport);
    for (int32 FolderIndex = 0; FolderIndex < MaxFoldersToReport; ++FolderIndex)
    {
        FolderEntries.Add(MakeShared<FJsonValueString>(SelectedFolders[FolderIndex]));
    }
    Detail->SetArrayField(TEXT("contentBrowserSelectedFolders"), MoveTemp(FolderEntries));
    Detail->SetBoolField(TEXT("contentBrowserSelectedFoldersTruncated"), SelectedFolders.Num() > MaxFoldersToReport);
}

static void AddViewportDetail(
    const TSharedRef<FJsonObject>& Detail,
    FEditorViewportClient* ViewportClient,
    FString& Selection,
    FString& SelectionObjectType)
{
    if (ViewportClient)
    {
        if (UWorld* World = ViewportClient->GetWorld())
        {
            Detail->SetStringField(TEXT("worldName"), World->GetName());
            Detail->SetStringField(TEXT("worldPath"), World->GetPathName());
            Detail->SetStringField(TEXT("worldType"), FString(LexToString(World->WorldType)));
        }
    }

    if (GEditor)
    {
        if (UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
        {
            AddActorSelectionDetail(Detail, EditorActorSubsystem->GetSelectedLevelActors(), Selection, SelectionObjectType);
        }
    }
}

static TSharedRef<FJsonObject> MakeGraphNodeDetail(UObject* NodeObject)
{
    const TSharedRef<FJsonObject> NodeObjectJson = MakeShared<FJsonObject>();
    if (!NodeObject)
    {
        return NodeObjectJson;
    }

    NodeObjectJson->SetStringField(TEXT("name"), NodeObject->GetName());
    NodeObjectJson->SetStringField(TEXT("class"), NodeObject->GetClass()->GetName());
    NodeObjectJson->SetStringField(TEXT("type"), ParseGraphNodeName(NodeObject));
    NodeObjectJson->SetStringField(TEXT("path"), NodeObject->GetPathName());

    if (UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(NodeObject))
    {
        NodeObjectJson->SetStringField(TEXT("nodeType"), TEXT("CallFunction"));
        NodeObjectJson->SetStringField(TEXT("functionName"), CallFunctionNode->GetFunctionName().ToString());
        if (UFunction* TargetFunction = CallFunctionNode->GetTargetFunction())
        {
            if (UClass* OwnerClass = TargetFunction->GetOwnerClass())
            {
                NodeObjectJson->SetStringField(TEXT("functionOwner"), OwnerClass->GetName());
            }
        }
    }
    else if (UK2Node_Literal* LiteralNode = Cast<UK2Node_Literal>(NodeObject))
    {
        NodeObjectJson->SetStringField(TEXT("nodeType"), TEXT("Literal"));
        if (UObject* ObjectRef = LiteralNode->GetObjectRef())
        {
            NodeObjectJson->SetStringField(TEXT("literalObjectName"), ObjectRef->GetName());
            NodeObjectJson->SetStringField(TEXT("literalObjectClass"), ObjectRef->GetClass()->GetName());
            NodeObjectJson->SetStringField(TEXT("literalObjectPath"), ObjectRef->GetPathName());
        }
    }
    else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(NodeObject))
    {
        NodeObjectJson->SetStringField(TEXT("nodeType"), TEXT("MacroInstance"));
        if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
        {
            NodeObjectJson->SetStringField(TEXT("macroName"), MacroGraph->GetName());
            NodeObjectJson->SetStringField(TEXT("macroPath"), MacroGraph->GetPathName());
        }
    }
    else
    {
        NodeObjectJson->SetStringField(TEXT("nodeType"), ParseGraphNodeName(NodeObject));
    }

    return NodeObjectJson;
}

static void AddGraphPanelDetail(
    const TSharedRef<FJsonObject>& Detail,
    SGraphPanel* GraphPanel,
    FString& GraphType,
    FString& EditorType,
    FString& Selection,
    FString& SelectionObjectType,
    FString& NodeType,
    FString& FunctionName,
    FString& FunctionOwner,
    FString& NodeClass)
{
    if (!GraphPanel)
    {
        return;
    }

    const FString GraphSuffix = GetGraphSuffixFromGraphPanel(GraphPanel);
    GraphType = GraphSuffix.StartsWith(TEXT("_")) ? GraphSuffix.RightChop(1) : GraphSuffix;
    ApplyEditorTypeFromGraphType(GraphType, EditorType);

    if (UEdGraph* GraphObject = GraphPanel->GetGraphObj())
    {
        Detail->SetStringField(TEXT("graphName"), GraphObject->GetName());
        Detail->SetStringField(TEXT("graphClass"), GraphObject->GetClass()->GetName());
        Detail->SetStringField(TEXT("graphPath"), GraphObject->GetPathName());
        if (const UEdGraphSchema* Schema = GraphObject->GetSchema())
        {
            Detail->SetStringField(TEXT("graphSchema"), Schema->GetClass()->GetName());
        }
        if (UPackage* Package = GraphObject->GetOutermost())
        {
            Detail->SetBoolField(TEXT("graphPackageDirty"), Package->IsDirty());
        }

        bool bOwnerSet = false;
        bool bAssetSet = false;
        for (UObject* OuterObject = GraphObject->GetOuter(); OuterObject; OuterObject = OuterObject->GetOuter())
        {
            const FString OuterClass = OuterObject->GetClass()->GetName();
            if (!bOwnerSet)
            {
                Detail->SetStringField(TEXT("graphOwnerName"), OuterObject->GetName());
                Detail->SetStringField(TEXT("graphOwnerClass"), OuterClass);
                Detail->SetStringField(TEXT("graphOwnerPath"), OuterObject->GetPathName());
                bOwnerSet = true;
            }

            if (UBlueprint* Blueprint = Cast<UBlueprint>(OuterObject))
            {
                Detail->SetStringField(TEXT("blueprintName"), Blueprint->GetName());
                Detail->SetStringField(TEXT("blueprintPath"), Blueprint->GetPathName());
                if (Blueprint->ParentClass)
                {
                    Detail->SetStringField(TEXT("blueprintParentClass"), Blueprint->ParentClass->GetName());
                }
                if (Blueprint->GeneratedClass)
                {
                    Detail->SetStringField(TEXT("blueprintGeneratedClass"), Blueprint->GeneratedClass->GetName());
                }
                if (UPackage* BlueprintPackage = Blueprint->GetOutermost())
                {
                    Detail->SetBoolField(TEXT("blueprintDirty"), BlueprintPackage->IsDirty());
                }
            }

            if (!bAssetSet &&
                (OuterClass.Contains(TEXT("Material")) ||
                 OuterClass.Contains(TEXT("Niagara")) ||
                 OuterClass.Contains(TEXT("MetaSound")) ||
                 OuterClass.Contains(TEXT("BehaviorTree")) ||
                 OuterClass.Contains(TEXT("ControlRig"))))
            {
                Detail->SetStringField(TEXT("graphAssetName"), OuterObject->GetName());
                Detail->SetStringField(TEXT("graphAssetClass"), OuterClass);
                Detail->SetStringField(TEXT("graphAssetPath"), OuterObject->GetPathName());
                bAssetSet = true;
            }
        }
    }

    const auto& SelectedNodes = GraphPanel->SelectionManager.SelectedNodes;
    Detail->SetNumberField(TEXT("selectedNodeCount"), SelectedNodes.Num());

    TArray<TSharedPtr<FJsonValue>> NodeEntries;
    const int32 MaxNodesToReport = FMath::Min(SelectedNodes.Num(), 16);
    NodeEntries.Reserve(MaxNodesToReport);

    int32 NodeIndex = 0;
    for (UObject* NodeObject : SelectedNodes)
    {
        if (!NodeObject)
        {
            continue;
        }

        if (NodeIndex < MaxNodesToReport)
        {
            NodeEntries.Add(MakeShared<FJsonValueObject>(MakeGraphNodeDetail(NodeObject)));
        }
        ++NodeIndex;

        if (SelectedNodes.Num() == 1)
        {
            NodeClass = NodeObject->GetClass()->GetName();
            SelectionObjectType = NodeClass;
            if (UK2Node_CallFunction* CallFunctionNode = Cast<UK2Node_CallFunction>(NodeObject))
            {
                NodeType = TEXT("CallFunction");
                FunctionName = CallFunctionNode->GetFunctionName().ToString();
                if (UFunction* TargetFunction = CallFunctionNode->GetTargetFunction())
                {
                    if (UClass* OwnerClass = TargetFunction->GetOwnerClass())
                    {
                        FunctionOwner = OwnerClass->GetName();
                    }
                }
            }
            else if (Cast<UK2Node_Literal>(NodeObject))
            {
                NodeType = TEXT("Literal");
            }
            else if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(NodeObject))
            {
                NodeType = TEXT("MacroInstance");
                if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
                {
                    Detail->SetStringField(TEXT("macroName"), MacroGraph->GetName());
                }
            }
            else
            {
                NodeType = ParseGraphNodeName(NodeObject);
            }
        }
    }

    Detail->SetArrayField(TEXT("selectedNodes"), MoveTemp(NodeEntries));
    Detail->SetBoolField(TEXT("selectedNodesTruncated"), SelectedNodes.Num() > MaxNodesToReport);

    if (SelectedNodes.Num() > 1)
    {
        Selection = TEXT("MultipleNodes");
    }
    else if (SelectedNodes.Num() == 1)
    {
        Selection = TEXT("SingleNode");
    }
}
static FString GetLevelViewportPlaySessionSuffix()
{
    if (!GEditor)
    {
        return TEXT("");
    }

    if (GEditor->IsSimulateInEditorInProgress())
    {
        return TEXT("_SimulateInEditor");
    }

    if (GEditor->IsPlaySessionInProgress())
    {
        return TEXT("_PlayInEditor");
    }

    return TEXT("");
}

static FEditorViewportClient* GetLevelViewportClientFromWidgetPath(const FWidgetPath& WidgetPath)
{
    for (int32 WidgetIndex = WidgetPath.Widgets.Num() - 1; WidgetIndex >= 0; --WidgetIndex)
    {
        const TSharedRef<SWidget>& Widget = WidgetPath.Widgets[WidgetIndex].Widget;
        if (!Widget->GetTypeAsString().Contains(TEXT("LevelViewport")))
        {
            continue;
        }

        const TSharedRef<SEditorViewport> EditorViewport = StaticCastSharedRef<SEditorViewport>(Widget);
        return EditorViewport->GetViewportClient().Get();
    }

    return nullptr;
}

static FString GetViewportContextViewMode(FEditorViewportClient* ViewportClient)
{
    if (!ViewportClient)
    {
        return TEXT("");
    }

    const UEnum* ViewModeEnum = StaticEnum<EViewModeIndex>();
    if (!ViewModeEnum)
    {
        return TEXT("");
    }

    FString ViewMode = ViewModeEnum->GetNameStringByValue(static_cast<int64>(ViewportClient->GetViewMode()));
    ViewMode.RemoveFromStart(TEXT("VMI_"), ESearchCase::IgnoreCase);
    ViewMode.ReplaceInline(TEXT("_"), TEXT(""));
    ViewMode.ReplaceInline(TEXT(" "), TEXT(""));
    return ViewMode;
}

static FString GetHoveredContext()
{
    if (!FSlateApplication::IsInitialized() || !GIsEditor) return TEXT("None");

    FVector2D MousePos;
    FWidgetPath WidgetPath;
    if (!QuickerBridgeContextService::ResolveCurrentWidgetPath(WidgetPath, &MousePos)) return TEXT("None");

    const int32 PathNum = WidgetPath.Widgets.Num();
    TArray<FString> WNames;
    WNames.Reserve(PathNum);
    for (int32 i = 0; i < PathNum; ++i)
    {
        WNames.Add(WidgetPath.Widgets[i].Widget->GetTypeAsString());
    }

    for (const FString& WName : WNames)
    {
        if (WName.Contains(TEXT("ConsoleInput")) ||
            WName.Contains(TEXT("ConsoleCommand")) ||
            WName.Contains(TEXT("StatusBar")))
        {
            return TEXT("BottomCommandLine");
        }
    }

    FString GraphSuffix;
    if (TSharedPtr<SWindow> TopWindow = WidgetPath.GetWindow())
    {
        FString WindowTitle = TopWindow->GetTitle().ToString();
        if (WindowTitle.Contains(TEXT("Control Rig")) || WindowTitle.Contains(TEXT("控制绑定")))
        {
            GraphSuffix = TEXT("_Rig");
        }
    }

    for (const FString& WName : WNames)
    {
        if (WName.Contains(TEXT("SettingsEditor")) || WName.Contains(TEXT("ProjectSettings"))) return TEXT("SettingsEditor");
        if (WName.Contains(TEXT("PluginBrowser")) || WName.Contains(TEXT("PluginCategory")) || WName.Contains(TEXT("PluginTile")) || WName.Contains(TEXT("IPlugin"))) return TEXT("Plugin_Setting");
        
        if (WName.Contains(TEXT("WidgetBlueprint")) || WName.Contains(TEXT("UMGEditor"))) GraphSuffix = TEXT("_Widget");
        else if (WName.Contains(TEXT("BehaviorTree"))) GraphSuffix = TEXT("_BehaviorTree");
        else if (WName.Contains(TEXT("ControlRig"))) GraphSuffix = TEXT("_Rig");
    }

    for (int32 i = 0; i < PathNum; ++i)
    {
        const FString& WName = WNames[i];

        if (WName.Contains(TEXT("LevelViewport")))
        {
            FString Context = TEXT("Viewport_Level");
            FEditorViewportClient* LevelViewportClient = GetLevelViewportClientFromWidgetPath(WidgetPath);
            if (GEditor)
            {
                const FString PlaySessionSuffix = GetLevelViewportPlaySessionSuffix();
                if (!PlaySessionSuffix.IsEmpty()) Context += PlaySessionSuffix;
                else if (GLevelEditorModeTools().IsModeActive(FName("EM_Default"))) Context += TEXT("_SelectMode");
                else if (GLevelEditorModeTools().IsModeActive(FName("EM_Landscape"))) Context += TEXT("_LandscapeMode");
                else if (GLevelEditorModeTools().IsModeActive(FName("EM_Foliage"))) Context += TEXT("_FoliageMode");
                else if (GLevelEditorModeTools().IsModeActive(FName("EM_MeshPaint"))) Context += TEXT("_MeshPaintMode");
                else if (GLevelEditorModeTools().IsModeActive(FName("EM_ModelingToolsEditorMode"))) Context += TEXT("_ModelingMode");
                else if (GLevelEditorModeTools().IsModeActive(FName("EM_FractureEditorMode"))) Context += TEXT("_FractureMode");
                else if (GLevelEditorModeTools().IsModeActive(FName("EM_AnimationEditorMode"))) Context += TEXT("_AnimationMode");

                if (UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>())
                {
                    TArray<AActor*> SelectedActors = EditorActorSubsystem->GetSelectedLevelActors();
                    if (SelectedActors.Num() == 1 && SelectedActors[0])
                    {
                        FString ActorClass = SelectedActors[0]->GetClass()->GetName();
                        if (ActorClass.EndsWith(TEXT("_C"))) ActorClass.RemoveFromEnd(TEXT("_C"));
                        else if (ActorClass.Len() > 5 && ActorClass.EndsWith(TEXT("Actor"))) ActorClass.RemoveFromEnd(TEXT("Actor"));
                        Context += TEXT("_") + ActorClass;
                    }
                    else if (SelectedActors.Num() > 1)
                    {
                        Context += TEXT("_Multiple");
                    }
                }
            }

            if (LevelViewportClient && LevelViewportClient->IsOrtho())
            {
                Context += TEXT("_Ortho");
            }

            const FString ViewMode = GetViewportContextViewMode(LevelViewportClient);
            if (!ViewMode.IsEmpty())
            {
                Context += TEXT("_") + ViewMode;
            }

            return Context;
        }

        if (WName.Contains(TEXT("Outliner"))) return TEXT("Outliner");
        if (WName.Contains(TEXT("Placement")) || WName.Contains(TEXT("PlaceActors"))) return TEXT("PlaceActors");

        if (WName.Contains(TEXT("Graph")) || WName.Contains(TEXT("NodePanel")))
        {
            if (GraphSuffix.IsEmpty())
            {
                for (int32 j = i; j < PathNum; ++j)
                {
                    if (WNames[j].Contains(TEXT("GraphPanel")))
                    {
                        GraphSuffix = GetGraphSuffixFromGraphPanel(static_cast<SGraphPanel*>(&(WidgetPath.Widgets[j].Widget.Get())));
                        if (!GraphSuffix.IsEmpty())
                        {
                            break;
                        }
                    }
                }
            }

            if (GraphSuffix.IsEmpty())
            {
                if (FChildren* Children = WidgetPath.Widgets[i].Widget->GetChildren())
                {
                    const int32 MaxCheckCount = FMath::Min(Children->Num(), 5);
                    for (int32 ChildIdx = 0; ChildIdx < MaxCheckCount; ++ChildIdx)
                    {
                        FString ChildName = Children->GetChildAt(ChildIdx)->GetTypeAsString();
                        if (ChildName.Contains(TEXT("AnimGraph")) || ChildName.Contains(TEXT("AnimState")) || ChildName.Contains(TEXT("StateMachine")) || ChildName.Contains(TEXT("StateResult"))) { GraphSuffix = TEXT("_Anim"); break; }
                        else if (ChildName.Contains(TEXT("Material"))) { GraphSuffix = TEXT("_Material"); break; }
                        else if (ChildName.Contains(TEXT("Niagara"))) { GraphSuffix = TEXT("_Niagara"); break; }
                        else if (ChildName.Contains(TEXT("MetaSound"))) { GraphSuffix = TEXT("_MetaSound"); break; }
                    }
                }
            }

            FString Context = TEXT("Graph") + GraphSuffix;
            for (int32 j = i; j < PathNum; ++j)
            {
                if (WNames[j].Contains(TEXT("GraphPanel")))
                {
                    if (SGraphPanel* GraphPanel = static_cast<SGraphPanel*>(&(WidgetPath.Widgets[j].Widget.Get())))
                    {
                        const auto& SelectedNodes = GraphPanel->SelectionManager.SelectedNodes;
                        if (SelectedNodes.Num() == 1)
                        {
                            for (UObject* NodeObj : SelectedNodes)
                            {
                                Context += TEXT("_") + ParseGraphNodeName(NodeObj);
                            }
                        }
                        else if (SelectedNodes.Num() > 1)
                        {
                            Context += TEXT("_MultipleNodes");
                        }
                    }
                    break;
                }
            }
            return Context;
        }

        if (WName.Contains(TEXT("Detail"))) return TEXT("Details");
        if (WName.Contains(TEXT("AssetView")) || WName.Contains(TEXT("ContentBrowser")))
        {
            return TEXT("ContentBrowser");
        }
        if (WName.Contains(TEXT("Fab"))) return TEXT("Fab");
    }

    for (const FString& WName : WNames)
    {
        if (WName.Contains(TEXT("Viewport"))) return TEXT("Viewport_Asset");
    }

    bool bSawWindowTitleBar = false;
    for (int32 i = PathNum - 1; i >= 0; --i)
    {
        const FString& WName = WNames[i];
        if (WName.Contains(TEXT("StatusBar")) || WName.Contains(TEXT("ConsoleInput")) || WName.Contains(TEXT("ConsoleCommand"))) return TEXT("BottomCommandLine");
        if (WName.Contains(TEXT("ToolBar")) || WName.Contains(TEXT("MultiBox"))) return TEXT("TopBar");
        if (WName.Contains(TEXT("WindowTitleBar")))
        {
            const FString TitleBarContext = ClassifyWindowTitleBarContext(WidgetPath, MousePos, WidgetPath.Widgets[i].Geometry);
            if (TitleBarContext == TEXT("BottomCommandLine"))
            {
                return TitleBarContext;
            }

            bSawWindowTitleBar = true;
            continue;
        }
        if (WName.Contains(TEXT("DockTab")) || WName.Contains(TEXT("DockingTabStack")) || WName.Contains(TEXT("DockingTabWell"))) return TEXT("TopTab");
        if (WName.Contains(TEXT("MenuAnchor"))) return TEXT("LevelEditor");
        if (WName.Contains(TEXT("Row")) || WName.Contains(TEXT("Splitter")) || WName.Contains(TEXT("Box")) || 
            WName.Contains(TEXT("Button")) || WName.Contains(TEXT("Text")) || WName.Contains(TEXT("Border")) || 
            WName.Contains(TEXT("Image")) || WName.Contains(TEXT("Overlay")) || WName.Contains(TEXT("Canvas")) || 
            WName.Contains(TEXT("Spacer"))) continue;

        if (WName.Len() > 1 && WName[0] == TEXT('S') && FChar::IsUpper(WName[1])) return WName.RightChop(1);
    }

    if (bSawWindowTitleBar)
    {
        return TEXT("WindowTitleBar");
    }

    return TEXT("None");
}

static TSharedRef<FJsonObject> GetHoveredContextDetail()
{
    const TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
    const FString CurrentContext = GetHoveredContext();
    Detail->SetStringField(TEXT("current"), CurrentContext);

    FVector2D MousePos;
    FWidgetPath WidgetPath;
    if (!QuickerBridgeContextService::ResolveCurrentWidgetPath(WidgetPath, &MousePos))
    {
        Detail->SetStringField(TEXT("area"), TEXT("None"));
        return Detail;
    }

    FString Area = TEXT("Other");
    FString EditorType;
    FString GraphType;
    FString Selection = TEXT("None");
    FString SelectionObjectType;
    FString EditorMode;
    FString PlayState;
    FString ViewMode;
    FString NodeType;
    FString FunctionName;
    FString FunctionOwner;
    FString NodeClass;

    if (CurrentContext.StartsWith(TEXT("Graph")))
    {
        Area = TEXT("Graph");
        EditorType = TEXT("Blueprint");
    }
    else if (CurrentContext.StartsWith(TEXT("Viewport")))
    {
        Area = TEXT("Viewport");
        if (CurrentContext.StartsWith(TEXT("Viewport_Level")))
        {
            EditorType = TEXT("Level");
            FEditorViewportClient* LevelViewportClient = GetLevelViewportClientFromWidgetPath(WidgetPath);
            ViewMode = GetViewportContextViewMode(LevelViewportClient);
            if (CurrentContext.Contains(TEXT("_PlayInEditor"))) PlayState = TEXT("PlayInEditor");
            else if (CurrentContext.Contains(TEXT("_SimulateInEditor"))) PlayState = TEXT("SimulateInEditor");

            if (CurrentContext.Contains(TEXT("_SelectMode"))) EditorMode = TEXT("SelectMode");
            else if (CurrentContext.Contains(TEXT("_LandscapeMode"))) EditorMode = TEXT("LandscapeMode");
            else if (CurrentContext.Contains(TEXT("_FoliageMode"))) EditorMode = TEXT("FoliageMode");
            else if (CurrentContext.Contains(TEXT("_MeshPaintMode"))) EditorMode = TEXT("MeshPaintMode");
            else if (CurrentContext.Contains(TEXT("_ModelingMode"))) EditorMode = TEXT("ModelingMode");
            else if (CurrentContext.Contains(TEXT("_FractureMode"))) EditorMode = TEXT("FractureMode");
            else if (CurrentContext.Contains(TEXT("_AnimationMode"))) EditorMode = TEXT("AnimationMode");

            AddViewportDetail(Detail, LevelViewportClient, Selection, SelectionObjectType);
        }
        else
        {
            EditorType = TEXT("Asset");
        }
    }
    else if (CurrentContext.StartsWith(TEXT("ContentBrowser")))
    {
        Area = TEXT("ContentBrowser");
        AddContentBrowserDetail(Detail, Selection, SelectionObjectType);
    }

    for (int32 WidgetIndex = WidgetPath.Widgets.Num() - 1; WidgetIndex >= 0; --WidgetIndex)
    {
        if (!WidgetPath.Widgets[WidgetIndex].Widget->GetTypeAsString().Contains(TEXT("GraphPanel")))
        {
            continue;
        }

        AddGraphPanelDetail(
            Detail,
            static_cast<SGraphPanel*>(&(WidgetPath.Widgets[WidgetIndex].Widget.Get())),
            GraphType,
            EditorType,
            Selection,
            SelectionObjectType,
            NodeType,
            FunctionName,
            FunctionOwner,
            NodeClass);
        break;
    }

    Detail->SetStringField(TEXT("area"), Area);
    Detail->SetStringField(TEXT("editorType"), EditorType);
    Detail->SetStringField(TEXT("graphType"), GraphType);
    Detail->SetStringField(TEXT("selection"), Selection);
    Detail->SetStringField(TEXT("selectionObjectType"), SelectionObjectType);
    Detail->SetStringField(TEXT("nodeType"), NodeType);
    Detail->SetStringField(TEXT("nodeClass"), NodeClass);
    Detail->SetStringField(TEXT("functionName"), FunctionName);
    Detail->SetStringField(TEXT("functionOwner"), FunctionOwner);
    Detail->SetStringField(TEXT("editorMode"), EditorMode);
    Detail->SetStringField(TEXT("playState"), PlayState);
    Detail->SetStringField(TEXT("viewMode"), ViewMode);

    TArray<TSharedPtr<FJsonValue>> Tags;
    auto AddTag = [&Tags](const FString& Tag)
    {
        if (!Tag.IsEmpty() && Tag != TEXT("None"))
        {
            Tags.Add(MakeShared<FJsonValueString>(Tag));
        }
    };
    AddTag(Area);
    AddTag(EditorType);
    AddTag(GraphType);
    AddTag(Selection);
    AddTag(SelectionObjectType);
    AddTag(NodeType);
    AddTag(EditorMode);
    AddTag(PlayState);
    AddTag(ViewMode);
    Detail->SetArrayField(TEXT("tags"), MoveTemp(Tags));
    Detail->SetNumberField(TEXT("mouseX"), MousePos.X);
    Detail->SetNumberField(TEXT("mouseY"), MousePos.Y);
    return Detail;
}

static FString GetHoveredContextDebug()
{
    if (!FSlateApplication::IsInitialized() || !GIsEditor) return TEXT("SlateNotInitialized");

    FVector2D MousePos;
    FWidgetPath WidgetPath;
    QuickerBridgeContextService::ResolveCurrentWidgetPath(WidgetPath, &MousePos);

    FString Result = FString::Printf(
        TEXT("Context=%s\nMouse=(%.1f, %.1f)\nPathValid=%s\n"),
        *GetHoveredContext(),
        MousePos.X,
        MousePos.Y,
        WidgetPath.IsValid() ? TEXT("true") : TEXT("false")
    );
    if (!WidgetPath.IsValid())
    {
        return Result;
    }

    Result += FString::Printf(TEXT("PathNum=%d\n"), WidgetPath.Widgets.Num());
    for (int32 i = WidgetPath.Widgets.Num() - 1; i >= 0; --i)
    {
        const FArrangedWidget& ArrangedWidget = WidgetPath.Widgets[i];
        const FVector2D Pos = ArrangedWidget.Geometry.GetAbsolutePosition();
        const FVector2D Size = ArrangedWidget.Geometry.GetAbsoluteSize();
        Result += FString::Printf(
            TEXT("[%d] %s Pos=(%.1f, %.1f) Size=(%.1f, %.1f)\n"),
            i,
            *ArrangedWidget.Widget->GetTypeAsString(),
            Pos.X,
            Pos.Y,
            Size.X,
            Size.Y
        );
    }

    Result += TEXT("Windows:\n");
    for (const TSharedRef<SWindow>& WindowRef : FSlateApplication::Get().GetInteractiveTopLevelWindows())
    {
        const FVector2D Pos = WindowRef->GetPositionInScreen();
        const FVector2D Size = WindowRef->GetSizeInScreen();
        Result += FString::Printf(
            TEXT("%s Pos=(%.1f, %.1f) Size=(%.1f, %.1f)\n"),
            *WindowRef->GetTitle().ToString(),
            Pos.X,
            Pos.Y,
            Size.X,
            Size.Y
        );
    }

    return Result;
}

void FQuickerBridgeModule::StartupModule()
{
    RegisterQuickerBridgeEditorCommands();
    QuickerBridgeScrollLockController::Start();

    FModuleManager::LoadModuleChecked<IModuleInterface>("HTTPServer");
    TSharedPtr<IHttpRouter> HttpRouter = FHttpServerModule::Get().GetHttpRouter(QuickerBridgeHttpPort);

    if (HttpRouter.IsValid())
    {
        if (HealthRouteHandle.IsValid())
        {
            HttpRouter->UnbindRoute(HealthRouteHandle);
            HealthRouteHandle.Reset();
        }
        if (GetContextRouteHandle.IsValid())
        {
            HttpRouter->UnbindRoute(GetContextRouteHandle);
            GetContextRouteHandle.Reset();
        }
        if (GetContextDebugRouteHandle.IsValid())
        {
            HttpRouter->UnbindRoute(GetContextDebugRouteHandle);
            GetContextDebugRouteHandle.Reset();
        }
        if (GetContextDetailRouteHandle.IsValid())
        {
            HttpRouter->UnbindRoute(GetContextDetailRouteHandle);
            GetContextDetailRouteHandle.Reset();
        }
        if (ExecutePythonRouteHandle.IsValid())
        {
            HttpRouter->UnbindRoute(ExecutePythonRouteHandle);
            ExecutePythonRouteHandle.Reset();
        }

        HealthRouteHandle = HttpRouter->BindRoute(
            FHttpPath(TEXT("/health")),
            EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
            {
                Async(EAsyncExecution::TaskGraphMainThread, [OnComplete]()
                {
                    OnComplete(CreateJsonResponse(GetHealthPayload()));
                });
                return true;
            })
        );
        UE_LOG(LogTemp, Display, TEXT("QuickerBridge: /health route %s"), HealthRouteHandle.IsValid() ? TEXT("bound") : TEXT("failed"));
        GetContextRouteHandle = HttpRouter->BindRoute(
            FHttpPath(TEXT("/context/current")), 
            EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
            {
                Async(EAsyncExecution::TaskGraphMainThread, [OnComplete]()
                {
                    TUniquePtr<FHttpServerResponse> Response = PrepareQuickerBridgeResponse(
                        FHttpServerResponse::Create(GetHoveredContext(), TEXT("text/plain")));
                    OnComplete(MoveTemp(Response));
                });
                return true; 
            })
        );
        UE_LOG(LogTemp, Display, TEXT("QuickerBridge: /context/current route %s"), GetContextRouteHandle.IsValid() ? TEXT("bound") : TEXT("failed"));

        GetContextDebugRouteHandle = HttpRouter->BindRoute(
            FHttpPath(TEXT("/context/debug")),
            EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
            {
                Async(EAsyncExecution::TaskGraphMainThread, [OnComplete]()
                {
                    TUniquePtr<FHttpServerResponse> Response = PrepareQuickerBridgeResponse(
                        FHttpServerResponse::Create(GetHoveredContextDebug(), TEXT("text/plain")));
                    OnComplete(MoveTemp(Response));
                });
                return true;
            })
        );
        UE_LOG(LogTemp, Display, TEXT("QuickerBridge: /context/debug route %s"), GetContextDebugRouteHandle.IsValid() ? TEXT("bound") : TEXT("failed"));

        GetContextDetailRouteHandle = HttpRouter->BindRoute(
            FHttpPath(TEXT("/context/detail")),
            EHttpServerRequestVerbs::VERB_GET,
            FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
            {
                Async(EAsyncExecution::TaskGraphMainThread, [OnComplete]()
                {
                    OnComplete(CreateJsonResponse(GetHoveredContextDetail()));
                });
                return true;
            })
        );
        UE_LOG(LogTemp, Display, TEXT("QuickerBridge: /context/detail route %s"), GetContextDetailRouteHandle.IsValid() ? TEXT("bound") : TEXT("failed"));

        ExecutePythonRouteHandle = HttpRouter->BindRoute(
            FHttpPath(TEXT("/python/execute")),
            EHttpServerRequestVerbs::VERB_PUT | EHttpServerRequestVerbs::VERB_POST,
            FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
            {
                if (Request.Body.IsEmpty())
                {
                    OnComplete(CreatePythonErrorResponse(TEXT("Request body is empty."), EHttpServerResponseCodes::BadRequest));
                    return true;
                }

                const FUTF8ToTCHAR ConvertedBody(
                    reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()),
                    Request.Body.Num());
                const FString RequestBody(ConvertedBody.Length(), ConvertedBody.Get());

                TSharedPtr<FJsonObject> JsonObject;
                const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
                if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
                {
                    OnComplete(CreatePythonErrorResponse(TEXT("Request body must be valid JSON."), EHttpServerResponseCodes::BadRequest));
                    return true;
                }

                FString Command;
                if (!JsonObject->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
                {
                    OnComplete(CreatePythonErrorResponse(TEXT("Missing non-empty string field: command."), EHttpServerResponseCodes::BadRequest));
                    return true;
                }

                EPythonCommandExecutionMode ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
                FString Mode;
                if (JsonObject->TryGetStringField(TEXT("mode"), Mode) && !LexTryParseString(ExecutionMode, *Mode))
                {
                    OnComplete(CreatePythonErrorResponse(TEXT("Invalid mode. Use ExecuteFile, ExecuteStatement, or EvaluateStatement."), EHttpServerResponseCodes::BadRequest));
                    return true;
                }

                Async(EAsyncExecution::TaskGraphMainThread, [OnComplete, Command = MoveTemp(Command), ExecutionMode]()
                {
                    OnComplete(ExecutePythonCommand(Command, ExecutionMode));
                });
                return true;
            })
        );
        UE_LOG(LogTemp, Display, TEXT("QuickerBridge: /python/execute route %s"), ExecutePythonRouteHandle.IsValid() ? TEXT("bound") : TEXT("failed"));

        const UQuickerBridgeSettings* Settings = GetDefault<UQuickerBridgeSettings>();
        if (Settings && Settings->bEnableMultiUEPortSwitching)
        {
            QuickerBridgeInstanceCoordinator::Start();
        }
        else
        {
            FHttpServerModule::Get().StartAllListeners();
        }
    }
}

void FQuickerBridgeModule::ShutdownModule()
{
    UnregisterQuickerBridgeEditorCommands();
    QuickerBridgeInstanceCoordinator::Stop();
    QuickerBridgeScrollLockController::Stop();

    if (HealthRouteHandle.IsValid() || GetContextRouteHandle.IsValid() || GetContextDebugRouteHandle.IsValid() || GetContextDetailRouteHandle.IsValid() || ExecutePythonRouteHandle.IsValid())
    {
        if (FModuleManager::Get().IsModuleLoaded("HTTPServer"))
        {
            if (TSharedPtr<IHttpRouter> HttpRouter = FHttpServerModule::Get().GetHttpRouter(QuickerBridgeHttpPort))
            {
                if (HealthRouteHandle.IsValid())
                {
                    HttpRouter->UnbindRoute(HealthRouteHandle);
                    HealthRouteHandle.Reset();
                }
                if (GetContextRouteHandle.IsValid())
                {
                    HttpRouter->UnbindRoute(GetContextRouteHandle);
                    GetContextRouteHandle.Reset();
                }
                if (GetContextDebugRouteHandle.IsValid())
                {
                    HttpRouter->UnbindRoute(GetContextDebugRouteHandle);
                    GetContextDebugRouteHandle.Reset();
                }
                if (GetContextDetailRouteHandle.IsValid())
                {
                    HttpRouter->UnbindRoute(GetContextDetailRouteHandle);
                    GetContextDetailRouteHandle.Reset();
                }
                if (ExecutePythonRouteHandle.IsValid())
                {
                    HttpRouter->UnbindRoute(ExecutePythonRouteHandle);
                    ExecutePythonRouteHandle.Reset();
                }
            }
        }
    }
}

#undef LOCTEXT_NAMESPACE
IMPLEMENT_MODULE(FQuickerBridgeModule, QuickerBridge)
