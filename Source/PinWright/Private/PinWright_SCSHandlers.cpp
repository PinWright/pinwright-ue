// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PinWright_SCSHandlers.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Async/Async.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
// SpawnMaterialUtils owns the resolve-then-mutate material contract shared with
// actor.spawn*; add_component consumes its FMaterialSpec/Resolve pair so both verbs
// sanitize and load a materialPath identically and emit the same two error codes.
#include "Handlers/Actor/SpawnMaterialUtils.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/ErrorCodes.h"
// The BodyInstance collision fields of an SCS component template are derived state that a
// reflection store cannot reach; set_property routes them through this shared module so the
// class-level write means the same thing actor.set_component_properties' instance write does.
#include "Utils/BodyInstanceCollisionPropertyWrite.h"
#include "Utils/ComponentReadFilter.h"
#include "Misc/ScopeExit.h"
#include "UObject/UnrealType.h"

#include "Camera/CameraComponent.h"
#include "Components/ActorComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UObjectIterator.h"



void FSCSHandlers::FinalizeBlueprintSCSChange(UBlueprint *Blueprint,
                                              const TSharedPtr<FJsonObject> &OutResult,
                                              bool &bOutCompiled,
                                              bool &bOutSaved) {
  bOutCompiled = false;
  bOutSaved = false;

  if (!Blueprint) {
    return;
  }

  FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
  const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
      BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
  bOutCompiled = CompileDiagnostics.bCompiled;
  BlueprintHandlerUtils::AddCompileDiagnosticsToJson(
      CompileDiagnostics, OutResult, TEXT("compileErrors"), TEXT("compileWarnings"));
  
  // UE 5.7+ Fix: Use McpSafeAssetSave instead of SaveLoadedAssetThrottled.
  // SaveLoadedAssetThrottled triggers UEditorAssetLibrary::SaveLoadedAsset() which
  // causes thumbnail generation and recursive FlushRenderingCommands calls (11+ times).
  // This corrupts render thread state and causes access violations in RenderCore.dll.
  // McpSafeAssetSave marks package dirty without triggering disk save operations.
  McpSafeAssetSave(Blueprint);

  // bOutSaved is published straight to the wire as `saved` by every caller of this
  // function, so it has to be a measurement. It used to be McpSafeAssetSave's return,
  // which was the literal true for any non-null Blueprint — the SCS edit was reported
  // as saved while it existed only as a dirty in-memory package, and a cold editor
  // restart discarded it. The deferral above is deliberate and stays; what changes is
  // that we no longer claim it persisted anything.
  bOutSaved = IsAssetPersistedToDisk(Blueprint);
  if (!bOutSaved) {
    UE_LOG(LogPinWrightSubsystem, Verbose,
           TEXT("FinalizeBlueprintSCSChange: '%s' is marked dirty but not on disk; "
                "reporting saved:false (an asset.save / editor.save_all is required "
                "before the SCS change survives a restart)"),
           *Blueprint->GetPathName());
  }
}

// Check if Play In Editor (PIE) is currently active
static bool IsPlayInEditorActive() {
  if (!GEditor) return false;
  if (GEditor->IsPlaySessionInProgress()) return true;
  for (const FWorldContext &Context : GEngine->GetWorldContexts()) {
    if (Context.WorldType == EWorldType::PIE ||
        Context.WorldType == EWorldType::Game) {
      return true;
    }
  }
  return false;
}

// Return PIE error result for SCS operations
static TSharedPtr<FJsonObject> PIEActiveError() {
  TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetBoolField(TEXT("success"), false);
  Result->SetStringField(
      TEXT("error"),
      TEXT("SCS operations cannot modify Blueprints during Play In Editor "
           "(PIE). Please stop the play session first."));
  Result->SetStringField(TEXT("errorCode"), TEXT("PIE_ACTIVE"));
  return Result;
}


// JSON values for the "source" field on each component entry.  Promoted to
// constants so any rename stays in lockstep with consumers.
static constexpr const TCHAR* KSourceScs       = TEXT("scs");
static constexpr const TCHAR* KSourceNative    = TEXT("native");
static constexpr const TCHAR* KSourceIch       = TEXT("inherited-override");
static constexpr const TCHAR* KSourceInherited = TEXT("inherited-scs");

template <typename FuncType>
static bool ForEachParentScsNode(UBlueprint* Blueprint, FuncType&& Func)
{
  UClass* ParentClass = Blueprint ? Blueprint->ParentClass : nullptr;
  while (ParentClass)
  {
    UBlueprint* ParentBP = Cast<UBlueprint>(ParentClass->ClassGeneratedBy);
    if (ParentBP && ParentBP->SimpleConstructionScript)
    {
      for (USCS_Node* Node : ParentBP->SimpleConstructionScript->GetAllNodes())
      {
        if (Func(Node))
        {
          return true;
        }
      }
    }
    ParentClass = ParentClass->GetSuperClass();
  }

  return false;
}

// Walk the parent Blueprint chain ONCE and collect every SCS node's
// (VariableName -> ComponentTemplate).  Closest parent wins on name collision
// so the diff base reflects the nearest inherited override.
static void BuildParentTemplateMap(UBlueprint* Blueprint, TMap<FName, UObject*>& OutMap)
{
  ForEachParentScsNode(Blueprint, [&OutMap](USCS_Node* Node) {
    if (!Node || !Node->ComponentTemplate) return false;
    const FName Name = Node->GetVariableName();
    if (Name.IsNone()) return false;
    if (!OutMap.Contains(Name)) OutMap.Add(Name, Node->ComponentTemplate);
    return false;
  });
}

static UObject* ResolveParentTemplate(const TMap<FName, UObject*>& ParentMap, FName Name, UClass* ComponentClass)
{
  if (UObject* const* Found = ParentMap.Find(Name)) return *Found;
  return ComponentClass ? ComponentClass->GetDefaultObject() : nullptr;
}

struct FInheritedScsTemplateResolution
{
  FComponentKey ComponentKey;
  UActorComponent* ParentTemplate = nullptr;
};

static bool ResolveInheritedScsParentTemplate(
    UBlueprint* Blueprint, const FString& ComponentName,
    FInheritedScsTemplateResolution& OutResolution)
{
  if (!Blueprint || ComponentName.IsEmpty())
  {
    return false;
  }

  bool bResolved = false;
  ForEachParentScsNode(Blueprint, [&ComponentName, &OutResolution, &bResolved](USCS_Node* Node) {
    if (!Node || !Node->ComponentTemplate || !Node->GetVariableName().IsValid())
    {
      return false;
    }

    if (!Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
    {
      return false;
    }

    const FComponentKey Key(Node);
    if (!Key.IsValid())
    {
      return true;
    }

    OutResolution.ComponentKey = Key;
    OutResolution.ParentTemplate = Node->ComponentTemplate;
    bResolved = true;
    return true;
  });

  return bResolved;
}

static UActorComponent* ResolveInheritedScsOverrideTemplate(
    UBlueprint* Blueprint, const FComponentKey& Key, bool& bOutCreated)
{
  bOutCreated = false;
  if (!Blueprint || !Key.IsValid())
  {
    return nullptr;
  }

  UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(true);
  if (!Handler)
  {
    return nullptr;
  }

  if (UActorComponent* ExistingOverride = Handler->GetOverridenComponentTemplate(Key))
  {
    return ExistingOverride;
  }

  UActorComponent* CreatedOverride = Handler->CreateOverridenComponentTemplate(Key);
  bOutCreated = CreatedOverride != nullptr;
  return CreatedOverride;
}

static TSharedPtr<FJsonObject> MakeTransformObj(const FTransform& Transform)
{
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  const FVector Loc = Transform.GetLocation();
  const FRotator Rot = Transform.GetRotation().Rotator();
  const FVector Scale = Transform.GetScale3D();
  Obj->SetStringField(TEXT("location"),
      FString::Printf(TEXT("X=%.2f Y=%.2f Z=%.2f"), Loc.X, Loc.Y, Loc.Z));
  Obj->SetStringField(TEXT("rotation"),
      FString::Printf(TEXT("P=%.2f Y=%.2f R=%.2f"), Rot.Pitch, Rot.Yaw, Rot.Roll));
  Obj->SetStringField(TEXT("scale"),
      FString::Printf(TEXT("X=%.2f Y=%.2f Z=%.2f"), Scale.X, Scale.Y, Scale.Z));
  return Obj;
}

// Extract properties that differ between Component and BaseTemplate and write
// them into PropsObj.  BaseTemplate should be the diff reference (parent
// template or class CDO).
static const FName GTransformSkipNames[] = {
    TEXT("RelativeLocation"), TEXT("RelativeRotation"), TEXT("RelativeScale3D")
};

static void AddOverriddenProperties(TSharedPtr<FJsonObject>& PropsObj,
                                    UObject* Component,
                                    UObject* BaseTemplate)
{
  if (!Component || !BaseTemplate)
  {
    return;
  }
  for (TFieldIterator<FProperty> PropIt(Component->GetClass()); PropIt; ++PropIt)
  {
    FProperty* Property = *PropIt;
    if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
      continue;

    bool bSkip = false;
    for (const FName& SkipName : GTransformSkipNames)
    {
      if (Property->GetFName() == SkipName) { bSkip = true; break; }
    }
    if (bSkip)
      continue;

    if (const FOmissionReason* Reason = IsKnownOversizedProperty(Property))
    {
      TSharedPtr<FJsonObject> Placeholder =
          BuildOmissionPlaceholder(Property, Component, *Reason);
      PropsObj->SetObjectField(Property->GetName(), Placeholder);
      continue;
    }

    // Use the base template's container only when its class matches; otherwise
    // fall back to the component class CDO to avoid type-mismatch crashes.
    const void* BaseContainer = BaseTemplate->GetClass()->IsChildOf(Component->GetClass())
        ? static_cast<const void*>(BaseTemplate)
        : static_cast<const void*>(Component->GetClass()->GetDefaultObject());

    if (!Property->Identical_InContainer(Component, BaseContainer))
    {
      TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(Component, Property);
      if (Value.IsValid())
      {
        PropsObj->SetField(Property->GetName(), Value);
      }
    }
  }
}

static UClass* GetScsNodeComponentClass(USCS_Node* Node)
{
  if (!Node)
  {
    return nullptr;
  }
  if (Node->ComponentClass)
  {
    return Node->ComponentClass;
  }
  return Node->ComponentTemplate ? Node->ComponentTemplate->GetClass() : nullptr;
}

// Build a child-variable-name -> parent-variable-name map from the top-down
// SCS structure. For a child attached under a *local* SCS root, UE stores the
// link only in the parent's GetChildNodes() (via AddChildNode); the child's
// ParentComponentOrVariableName stays None and is set only when the parent is
// inherited/native (USCS_Node::SetParent). Walking each node's child list here
// recovers the parent name so the readback can emit a `parent` field that is
// consistent with the parent's `child_count`, letting downstream consumers
// (e.g. SCSTextEmitter) reconstruct the nested tree.
static void BuildScsChildParentMap(const TArray<USCS_Node*>& Nodes,
                                   TMap<FName, FName>& OutChildToParent)
{
  for (USCS_Node* Node : Nodes)
  {
    if (!Node)
    {
      continue;
    }
    const FName ParentName = Node->GetVariableName();
    if (!ParentName.IsValid())
    {
      continue;
    }
    for (USCS_Node* Child : Node->GetChildNodes())
    {
      if (!Child)
      {
        continue;
      }
      const FName ChildName = Child->GetVariableName();
      if (ChildName.IsValid())
      {
        OutChildToParent.Add(ChildName, ParentName);
      }
    }
  }
}

// Get Blueprint SCS structure
TSharedPtr<FJsonObject>
FSCSHandlers::GetBlueprintSCS(const FString &BlueprintPath) {
  return GetBlueprintSCS(BlueprintPath, FComponentReadFilter());
}

TSharedPtr<FJsonObject>
FSCSHandlers::GetBlueprintSCS(
    const FString &BlueprintPath, const FComponentReadFilter &ComponentFilter) {
  TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

  // Load blueprint
  FString NormalizedPath;
  FString ErrorMsg;
  UBlueprint *Blueprint =
      LoadBlueprintAsset(BlueprintPath, NormalizedPath, ErrorMsg);
  if (!Blueprint) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        ErrorMsg.IsEmpty()
            ? FString::Printf(
                  TEXT(
                      "Blueprint not found or not a valid Blueprint asset: %s"),
                  *BlueprintPath)
            : ErrorMsg);
    return Result;
  }

  // SCS may be absent for data-only / function-library BPs; native, ICH,
  // and inherited-SCS loops below still emit anything from a C++ or BP parent.
  // Four sources total: local SCS, native CDO, ICH overrides, inherited parent SCS.
  USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript;

  TArray<TSharedPtr<FJsonValue>> Components;
  TSet<FName> ScsNodeNames;

  // Resolve parent-template diff bases for the whole BP up front (one chain walk).
  TMap<FName, UObject*> ParentTemplateMap;
  BuildParentTemplateMap(Blueprint, ParentTemplateMap);

  const TArray<USCS_Node *> EmptyNodes;
  const TArray<USCS_Node *> &AllNodes = SCS ? SCS->GetAllNodes() : EmptyNodes;

  // Recover child->parent links from the top-down structure so locally-attached
  // children (whose ParentComponentOrVariableName is None) still emit a `parent`
  // consistent with the parent's `child_count`. See BuildScsChildParentMap.
  TMap<FName, FName> LocalChildToParent;
  BuildScsChildParentMap(AllNodes, LocalChildToParent);

  for (USCS_Node *Node : AllNodes) {
    if (Node && Node->GetVariableName().IsValid()) {
      const FName VarName = Node->GetVariableName();
      ScsNodeNames.Add(VarName);
      if (!ComponentFilter.Matches(VarName.ToString(), GetScsNodeComponentClass(Node))) {
        continue;
      }

      TSharedPtr<FJsonObject> ComponentObj = MakeShareable(new FJsonObject);
      ComponentObj->SetStringField(TEXT("name"), VarName.ToString());
      ComponentObj->SetStringField(
          TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetName()
                                              : TEXT("Unknown"));
      ComponentObj->SetStringField(TEXT("source"), KSourceScs);
      if (!Node->ParentComponentOrVariableName.IsNone()) {
        ComponentObj->SetStringField(
            TEXT("parent"), Node->ParentComponentOrVariableName.ToString());
      } else if (const FName *DerivedParent = LocalChildToParent.Find(VarName)) {
        ComponentObj->SetStringField(TEXT("parent"), DerivedParent->ToString());
      }

      if (Node->ComponentTemplate) {
        if (USceneComponent *SceneComp =
                Cast<USceneComponent>(Node->ComponentTemplate)) {
          ComponentObj->SetObjectField(TEXT("transform"),
              MakeTransformObj(SceneComp->GetRelativeTransform()));
        }

        UObject* DiffBase = ResolveParentTemplate(ParentTemplateMap, VarName,
                                                  Node->ComponentTemplate->GetClass());
        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        AddOverriddenProperties(PropsObj, Node->ComponentTemplate, DiffBase);
        if (PropsObj->Values.Num() > 0)
        {
          ComponentObj->SetObjectField(TEXT("properties"), PropsObj);
        }
      }

      ComponentObj->SetNumberField(TEXT("child_count"),
                                   Node->GetChildNodes().Num());

      Components.Add(MakeShareable(new FJsonValueObject(ComponentObj)));
    }
  }

  // Emit native C++ components (CreateDefaultSubobject) from the actor CDO.
  // Skipped if already named by the SCS loop above.
  if (Blueprint->GeneratedClass)
  {
    UObject* CDOObj = Blueprint->GeneratedClass->GetDefaultObject(false);
    if (CDOObj)
    {
      TArray<UObject*> DefaultSubobjects;
      CDOObj->GetDefaultSubobjects(DefaultSubobjects);

      // Index parent CDO's subobjects by name once — avoids O(N²) per-component scan.
      TMap<FName, UObject*> ParentSubobjectByName;
      if (Blueprint->ParentClass)
      {
        if (UObject* ParentCDO = Blueprint->ParentClass->GetDefaultObject(false))
        {
          TArray<UObject*> ParentSubobjects;
          ParentCDO->GetDefaultSubobjects(ParentSubobjects);
          for (UObject* PS : ParentSubobjects)
          {
            if (PS) ParentSubobjectByName.Add(PS->GetFName(), PS);
          }
        }
      }

      for (UObject* Subobj : DefaultSubobjects)
      {
        if (!Subobj)
          continue;
        UActorComponent* NativeComp = Cast<UActorComponent>(Subobj);
        if (!NativeComp)
          continue;
        const FName NativeName = Subobj->GetFName();
        if (ScsNodeNames.Contains(NativeName))
          continue;
        ScsNodeNames.Add(NativeName);
        if (!ComponentFilter.Matches(NativeName.ToString(), NativeComp->GetClass()))
          continue;

        TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
        ComponentObj->SetStringField(TEXT("name"), NativeName.ToString());
        ComponentObj->SetStringField(TEXT("class"), NativeComp->GetClass()->GetName());
        ComponentObj->SetStringField(TEXT("source"), KSourceNative);

        if (USceneComponent* SceneComp = Cast<USceneComponent>(NativeComp))
        {
          ComponentObj->SetObjectField(TEXT("transform"),
              MakeTransformObj(SceneComp->GetRelativeTransform()));
        }

        UObject* DiffBase = NativeComp->GetClass()->GetDefaultObject();
        if (UObject* const* PS = ParentSubobjectByName.Find(NativeName))
        {
          if (*PS && (*PS)->IsA(NativeComp->GetClass())) DiffBase = *PS;
        }

        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        AddOverriddenProperties(PropsObj, NativeComp, DiffBase);
        if (PropsObj->Values.Num() > 0)
        {
          ComponentObj->SetObjectField(TEXT("properties"), PropsObj);
        }

        Components.Add(MakeShared<FJsonValueObject>(ComponentObj));
      }
    }
  }

  // Emit InheritableComponentHandler override records: child-BP overrides on
  // components inherited from a parent BP.  Skipped when the local SCS already
  // owns the name.
  UInheritableComponentHandler* ICH = Blueprint->GetInheritableComponentHandler(false);
  if (ICH)
  {
    // ICH->Records is private in UE 5.6; CreateRecordIterator is the public seam.
    for (auto It = ICH->CreateRecordIterator(); It; ++It)
    {
      const FComponentOverrideRecord& Record = *It;
      if (!Record.ComponentTemplate)
        continue;
      UActorComponent* ICHComp = Record.ComponentTemplate;
      const FName ICHName = Record.ComponentKey.GetSCSVariableName();
      if (ICHName.IsNone())
        continue;
      if (ScsNodeNames.Contains(ICHName))
        continue;
      ScsNodeNames.Add(ICHName);
      if (!ComponentFilter.Matches(ICHName.ToString(), ICHComp->GetClass()))
        continue;

      TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
      ComponentObj->SetStringField(TEXT("name"), ICHName.ToString());
      ComponentObj->SetStringField(TEXT("class"), ICHComp->GetClass()->GetName());
      ComponentObj->SetStringField(TEXT("source"), KSourceIch);

      if (USceneComponent* SceneComp = Cast<USceneComponent>(ICHComp))
      {
        ComponentObj->SetObjectField(TEXT("transform"),
            MakeTransformObj(SceneComp->GetRelativeTransform()));
      }

      UObject* DiffBase = ResolveParentTemplate(ParentTemplateMap, ICHName, ICHComp->GetClass());
      TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
      AddOverriddenProperties(PropsObj, ICHComp, DiffBase);
      if (PropsObj->Values.Num() > 0)
      {
        ComponentObj->SetObjectField(TEXT("properties"), PropsObj);
      }

      Components.Add(MakeShared<FJsonValueObject>(ComponentObj));
    }
  }

  // Emit SCS nodes from ancestor Blueprints that are not covered by local SCS,
  // native CDO, or ICH entries above.  Without this, child components that set
  // "parent": "DefaultSceneRoot" (or any other inherited root) produce dangling
  // references because the root name is absent from the components array.
  for (UClass* P = Blueprint->ParentClass; P; P = P->GetSuperClass())
  {
    UBlueprint* ParentBP = Cast<UBlueprint>(P->ClassGeneratedBy);
    if (!ParentBP || !ParentBP->SimpleConstructionScript)
      continue;

    const TArray<USCS_Node*>& InheritedNodes =
        ParentBP->SimpleConstructionScript->GetAllNodes();

    // Same top-down recovery as the local loop, scoped to this ancestor BP's SCS.
    TMap<FName, FName> InheritedChildToParent;
    BuildScsChildParentMap(InheritedNodes, InheritedChildToParent);

    for (USCS_Node* Node : InheritedNodes)
    {
      if (!Node)
        continue;
      const FName VarName = Node->GetVariableName();
      if (VarName.IsNone())
        continue;
      // Closest-wins: local SCS, native CDO, or ICH already emitted this name.
      if (ScsNodeNames.Contains(VarName))
        continue;
      ScsNodeNames.Add(VarName);
      if (!ComponentFilter.Matches(VarName.ToString(), GetScsNodeComponentClass(Node)))
        continue;

      TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
      ComponentObj->SetStringField(TEXT("name"), VarName.ToString());
      ComponentObj->SetStringField(TEXT("class"),
          Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("Unknown"));
      ComponentObj->SetStringField(TEXT("source"), KSourceInherited);
      ComponentObj->SetStringField(TEXT("inheritedFrom"), ParentBP->GetPathName());

      if (!Node->ParentComponentOrVariableName.IsNone())
      {
        ComponentObj->SetStringField(TEXT("parent"),
            Node->ParentComponentOrVariableName.ToString());
      }
      else if (const FName* DerivedParent = InheritedChildToParent.Find(VarName))
      {
        ComponentObj->SetStringField(TEXT("parent"), DerivedParent->ToString());
      }

      if (Node->ComponentTemplate)
      {
        if (USceneComponent* SceneComp = Cast<USceneComponent>(Node->ComponentTemplate))
        {
          ComponentObj->SetObjectField(TEXT("transform"),
              MakeTransformObj(SceneComp->GetRelativeTransform()));
        }

        UObject* DiffBase = ResolveParentTemplate(ParentTemplateMap, VarName,
                                                  Node->ComponentTemplate->GetClass());
        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        AddOverriddenProperties(PropsObj, Node->ComponentTemplate, DiffBase);
        if (PropsObj->Values.Num() > 0)
        {
          ComponentObj->SetObjectField(TEXT("properties"), PropsObj);
        }
      }

      ComponentObj->SetNumberField(TEXT("child_count"), Node->GetChildNodes().Num());
      Components.Add(MakeShared<FJsonValueObject>(ComponentObj));
    }
  }

  Result->SetBoolField(TEXT("success"), true);
  Result->SetArrayField(TEXT("components"), Components);
  Result->SetNumberField(TEXT("count"), Components.Num());
  Result->SetStringField(TEXT("blueprint_path"), BlueprintPath);
  AddAssetVerification(Result, Blueprint);

  return Result;
}

// Add component to SCS
TSharedPtr<FJsonObject> FSCSHandlers::AddSCSComponent(
    const FString &BlueprintPath, const FString &ComponentClass,
    const FString &ComponentName, const FString &ParentComponentName,
    const FString &MeshPath, const FString &MaterialPath) {
  TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

  // Check for PIE - cannot modify Blueprints during play
  if (IsPlayInEditorActive()) {
    return PIEActiveError();
  }

  // Load blueprint
  FString NormalizedPath;
  FString ErrorMsg;
  UBlueprint *Blueprint =
      LoadBlueprintAsset(BlueprintPath, NormalizedPath, ErrorMsg);
  if (!Blueprint) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        ErrorMsg.IsEmpty()
            ? FString::Printf(TEXT("Blueprint asset not found at path: %s"),
                              *BlueprintPath)
            : ErrorMsg);
    return Result;
  }

  // Get or create SCS
  USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript;
  if (!SCS) {
    SCS = NewObject<USimpleConstructionScript>(Blueprint);
    Blueprint->SimpleConstructionScript = SCS;
  }

  // Find component class (UE 5.6: ANY_PACKAGE is deprecated, use nullptr)
  // Use the robust utility to resolve class (handles NiagaraComponent, Assets,
  // Native classes)
  UClass *CompClass = ResolveClassByName(ComponentClass);

  if (!CompClass) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"), FString::Printf(TEXT("Component class not found: %s"),
                                       *ComponentClass));
    return Result;
  }

  // Verify it's a component class
  if (!CompClass->IsChildOf(UActorComponent::StaticClass())) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Class is not a component: %s"), *ComponentClass));
    return Result;
  }

  // Populates Result as a typed failure. errorCode is what SCSHandler.cpp's
  // SendSCSResult forwards to Ctx.SendError, so it must be an ErrorCodes:: constant.
  auto FailWith = [&Result](const TCHAR *Code, const FString &Message) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(TEXT("error"), Message);
    Result->SetStringField(TEXT("errorCode"), Code);
    return Result;
  };

  // Resolve materialPath / meshPath BEFORE any mutation. Both consume only their own path
  // parameter - never the SCS node - so a bad path is knowable here, while the Blueprint is
  // still untouched. Previously the node was created, linked, compiled and SAVED first and
  // the caller's only signal was material_applied/mesh_applied:false on an otherwise
  // successful response, leaving a persisted half-applied edit with no way to tell a bad
  // path from an unloadable asset from a class that never had the slot. Same
  // resolve-then-mutate contract as actor.spawn* (SpawnMaterialUtils).
  // Trimmed because a padded path is unloadable today anyway, so accepting it can only turn
  // a silent false into a success, never a success into a failure.
  const FString TrimmedMaterialPath = MaterialPath.TrimStartAndEnd();
  const FString TrimmedMeshPath = MeshPath.TrimStartAndEnd();

  UMaterialInterface *PreflightMaterial = nullptr;
  if (!TrimmedMaterialPath.IsEmpty()) {
    // SetMaterial is a UPrimitiveComponent API; a class without the slot can never accept
    // the material, so this is a caller mistake rather than a transient miss.
    if (!CompClass->IsChildOf(UPrimitiveComponent::StaticClass())) {
      return FailWith(
          ErrorCodes::ERR_INVALID_PARAMS,
          FString::Printf(
              TEXT("Component class '%s' is not a primitive component and cannot hold a "
                   "material; omit materialPath or use a mesh/primitive component class"),
              *ComponentClass));
    }

    // Single-slot spec: blueprint.scs.add_component exposes only slot 0, matching
    // actor.spawn's materialPath (the multi-slot materialPaths form is spawn-only).
    SpawnMaterialUtils::FMaterialSpec MaterialSpec;
    MaterialSpec.SlotPaths.Add(TrimmedMaterialPath);
    SpawnMaterialUtils::FResolvedMaterials ResolvedMaterials;
    FString MaterialErrorCode;
    FString MaterialErrorMessage;
    if (!SpawnMaterialUtils::Resolve(MaterialSpec, ResolvedMaterials, MaterialErrorCode,
                                     MaterialErrorMessage)) {
      return FailWith(*MaterialErrorCode, MaterialErrorMessage);
    }
    PreflightMaterial = ResolvedMaterials.SlotMaterials.IsValidIndex(0)
                            ? ResolvedMaterials.SlotMaterials[0]
                            : nullptr;
  }

  // The mesh slot has no shared util (it is keyed off the component class, not a slot
  // index), so it repeats the same three checks inline against the concrete mesh type.
  UStaticMesh *PreflightStaticMesh = nullptr;
  USkeletalMesh *PreflightSkeletalMesh = nullptr;
  if (!TrimmedMeshPath.IsEmpty()) {
    const bool bStaticMeshClass =
        CompClass->IsChildOf(UStaticMeshComponent::StaticClass());
    const bool bSkeletalMeshClass =
        CompClass->IsChildOf(USkeletalMeshComponent::StaticClass());
    if (!bStaticMeshClass && !bSkeletalMeshClass) {
      return FailWith(
          ErrorCodes::ERR_INVALID_PARAMS,
          FString::Printf(
              TEXT("Component class '%s' is neither a static nor a skeletal mesh component "
                   "and cannot hold a mesh; omit meshPath"),
              *ComponentClass));
    }

    // SECURITY: constrain the mesh to a project-relative asset root, exactly as
    // SpawnMaterialUtils::Resolve does for the material.
    const FString SafeMeshPath = SanitizeProjectRelativePath(TrimmedMeshPath);
    if (SafeMeshPath.IsEmpty()) {
      return FailWith(
          ErrorCodes::ERR_SECURITY_VIOLATION,
          FString::Printf(TEXT("Invalid or unsafe mesh path: %s. Path must be relative to "
                               "project (e.g. /Game/...)"),
                          *TrimmedMeshPath));
    }

    if (bStaticMeshClass) {
      PreflightStaticMesh = LoadObject<UStaticMesh>(nullptr, *SafeMeshPath);
    } else {
      PreflightSkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *SafeMeshPath);
    }
    if (!PreflightStaticMesh && !PreflightSkeletalMesh) {
      return FailWith(
          ErrorCodes::ERR_MESH_NOT_FOUND,
          FString::Printf(TEXT("%s mesh not found: %s"),
                          bStaticMeshClass ? TEXT("Static") : TEXT("Skeletal"),
                          *SafeMeshPath));
    }
  }

  // Find parent node if specified
  USCS_Node *ParentNode = nullptr;
  if (!ParentComponentName.IsEmpty()) {
    for (USCS_Node *Node : SCS->GetAllNodes()) {
      if (Node && Node->GetVariableName().IsValid() &&
          Node->GetVariableName().ToString().Equals(ParentComponentName,
                                                    ESearchCase::IgnoreCase)) {
        ParentNode = Node;
        break;
      }
    }

    if (!ParentNode) {
      Result->SetBoolField(TEXT("success"), false);
      Result->SetStringField(
          TEXT("error"), FString::Printf(TEXT("Parent component not found: %s"),
                                         *ParentComponentName));
      return Result;
    }
  }

  // Check for duplicate name
  for (USCS_Node *Node : SCS->GetAllNodes()) {
    if (Node && Node->GetVariableName().IsValid() &&
        Node->GetVariableName().ToString().Equals(ComponentName,
                                                  ESearchCase::IgnoreCase)) {
      Result->SetBoolField(TEXT("success"), false);
      Result->SetStringField(
          TEXT("error"),
          FString::Printf(TEXT("Component with name '%s' already exists"),
                          *ComponentName));
      return Result;
    }
  }

  // Create new node
  USCS_Node *NewNode = SCS->CreateNode(CompClass, FName(*ComponentName));
  if (!NewNode) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(TEXT("error"), TEXT("Failed to create SCS node"));
    return Result;
  }

  if (ParentNode) {
    ParentNode->AddChildNode(NewNode);
  } else {
    SCS->AddNode(NewNode);
  }

  // Both slots now consume the pre-resolved pointers: nothing here can fail on a path.
  // The only remaining false is a null / unexpectedly-typed ComponentTemplate, which the
  // engine decides during CreateNode - post-mutation and not predictable, so it degrades to
  // a warning instead of an error and says which slot was dropped and why.
  TArray<FString> Warnings;

  bool bMeshApplied = false;
  if (PreflightStaticMesh || PreflightSkeletalMesh) {
    if (UStaticMeshComponent *SMC =
            Cast<UStaticMeshComponent>(NewNode->ComponentTemplate)) {
      if (PreflightStaticMesh) {
        SMC->SetStaticMesh(PreflightStaticMesh);
        bMeshApplied = true;
      }
    } else if (USkeletalMeshComponent *SkMC =
                   Cast<USkeletalMeshComponent>(NewNode->ComponentTemplate)) {
      if (PreflightSkeletalMesh) {
        SkMC->SetSkeletalMesh(PreflightSkeletalMesh, true);
        bMeshApplied = true;
      }
    }
    if (!bMeshApplied) {
      Warnings.Add(FString::Printf(
          TEXT("Component template for '%s' is missing or is not a mesh component, so mesh "
               "'%s' was not applied"),
          *ComponentName, *TrimmedMeshPath));
    }
  }

  bool bMaterialApplied = false;
  if (PreflightMaterial) {
    if (UPrimitiveComponent *PC =
            Cast<UPrimitiveComponent>(NewNode->ComponentTemplate)) {
      PC->SetMaterial(0, PreflightMaterial);
      bMaterialApplied = true;
    } else {
      Warnings.Add(FString::Printf(
          TEXT("Component template for '%s' is missing or is not a primitive component, so "
               "material '%s' was not applied"),
          *ComponentName, *TrimmedMaterialPath));
    }
  }

  // Finalize blueprint change (compile/save)
  bool bCompiled = false;
  bool bSaved = false;
  FinalizeBlueprintSCSChange(Blueprint, Result, bCompiled, bSaved);

  // Real test: Verify component exists in SCS
  bool bVerified = false;
  for (USCS_Node *Node : SCS->GetAllNodes()) {
    if (Node && Node->GetVariableName().IsValid() &&
        Node->GetVariableName().ToString().Equals(ComponentName,
                                                  ESearchCase::IgnoreCase)) {
      bVerified = true;
      break;
    }
  }

  if (!bVerified) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"), FString::Printf(TEXT("Verification failed: Component "
                                            "'%s' not found in SCS after add"),
                                       *ComponentName));
    return Result;
  }

  Result->SetBoolField(TEXT("success"), true);
  Result->SetStringField(
      TEXT("message"),
      FString::Printf(TEXT("Component '%s' added to SCS"), *ComponentName));
  Result->SetStringField(TEXT("component_name"), ComponentName);
  Result->SetStringField(TEXT("component_class"), CompClass->GetName());
  Result->SetStringField(TEXT("parent"), ParentComponentName.IsEmpty()
                                             ? TEXT("(root)")
                                             : ParentComponentName);
  Result->SetBoolField(TEXT("compiled"), bCompiled);
  Result->SetBoolField(TEXT("saved"), bSaved);
  Result->SetBoolField(TEXT("mesh_applied"), bMeshApplied);
  Result->SetBoolField(TEXT("material_applied"), bMaterialApplied);
  // Additive and emitted only when non-empty, so a call that asked for no material/mesh -
  // or got one applied - keeps a byte-identical response.
  if (Warnings.Num() > 0) {
    Result->SetArrayField(TEXT("warnings"), EmitStringArray(Warnings));
  }
  AddAssetVerification(Result, Blueprint);
  if (NewNode && NewNode->ComponentTemplate) {
    if (USceneComponent* SceneComp = Cast<USceneComponent>(NewNode->ComponentTemplate)) {
      AddComponentVerification(Result, SceneComp);
    }
  }

  return Result;
}

// Remove component from SCS
TSharedPtr<FJsonObject>
FSCSHandlers::RemoveSCSComponent(const FString &BlueprintPath,
                                 const FString &ComponentName) {
  TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

  FString NormalizedPath;
  FString ErrorMsg;
  UBlueprint *Blueprint =
      LoadBlueprintAsset(BlueprintPath, NormalizedPath, ErrorMsg);
  if (!Blueprint) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        ErrorMsg.IsEmpty()
            ? FString::Printf(TEXT("Blueprint asset not found at path: %s"),
                              *BlueprintPath)
            : ErrorMsg);
    Result->SetStringField(TEXT("errorCode"), TEXT("ASSET_NOT_FOUND"));
    return Result;
  }

  if (!Blueprint->SimpleConstructionScript) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Blueprint has no SimpleConstructionScript: %s"),
                        *BlueprintPath));
    Result->SetStringField(TEXT("errorCode"), TEXT("SCS_NOT_FOUND"));
    return Result;
  }

  USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript;

  // Find node to remove
  USCS_Node *NodeToRemove = nullptr;
  for (USCS_Node *Node : SCS->GetAllNodes()) {
    if (Node && Node->GetVariableName().IsValid() &&
        Node->GetVariableName().ToString().Equals(ComponentName,
                                                  ESearchCase::IgnoreCase)) {
      NodeToRemove = Node;
      break;
    }
  }

  if (!NodeToRemove) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    Result->SetStringField(TEXT("errorCode"), TEXT("SCS_COMPONENT_NOT_FOUND"));
    return Result;
  }

  SCS->RemoveNode(NodeToRemove);

  bool bCompiled = false;
  bool bSaved = false;
  FinalizeBlueprintSCSChange(Blueprint, Result, bCompiled, bSaved);

  Result->SetBoolField(TEXT("success"), true);
  Result->SetStringField(
      TEXT("message"),
      FString::Printf(TEXT("Component '%s' removed from SCS"), *ComponentName));
  Result->SetBoolField(TEXT("compiled"), bCompiled);
  Result->SetBoolField(TEXT("saved"), bSaved);
  AddAssetVerification(Result, Blueprint);

  return Result;
}

// Reparent component within SCS
TSharedPtr<FJsonObject>
FSCSHandlers::ReparentSCSComponent(const FString &BlueprintPath,
                                   const FString &ComponentName,
                                   const FString &NewParentName) {
  TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

  FString NormalizedPath;
  FString ErrorMsg;
  UBlueprint *Blueprint =
      LoadBlueprintAsset(BlueprintPath, NormalizedPath, ErrorMsg);
  if (!Blueprint) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        ErrorMsg.IsEmpty()
            ? FString::Printf(TEXT("Blueprint asset not found at path: %s"),
                              *BlueprintPath)
            : ErrorMsg);
    return Result;
  }

  if (!Blueprint->SimpleConstructionScript) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Blueprint has no SimpleConstructionScript: %s"),
                        *BlueprintPath));
    return Result;
  }

  USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript;

  // Find component to reparent
  USCS_Node *ComponentNode = nullptr;
  for (USCS_Node *Node : SCS->GetAllNodes()) {
    if (Node && Node->GetVariableName().IsValid() &&
        Node->GetVariableName().ToString().Equals(ComponentName,
                                                  ESearchCase::IgnoreCase)) {
      ComponentNode = Node;
      break;
    }
  }

  if (!ComponentNode) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Component not found: %s"), *ComponentName));
    return Result;
  }

  // Find new parent (empty string means root)
  USCS_Node *NewParentNode = nullptr;
  if (!NewParentName.IsEmpty()) {
    // Accept common root synonyms
    const bool bRootSynonym =
        NewParentName.Equals(TEXT("RootComponent"), ESearchCase::IgnoreCase) ||
        NewParentName.Equals(TEXT("DefaultSceneRoot"),
                             ESearchCase::IgnoreCase) ||
        NewParentName.Equals(TEXT("Root"), ESearchCase::IgnoreCase);
    if (bRootSynonym) {
      const TArray<USCS_Node *> &Roots = SCS->GetRootNodes();
      // Prefer an explicit DefaultSceneRoot if present
      for (USCS_Node *R : Roots) {
        if (R && R->GetVariableName().IsValid() &&
            R->GetVariableName().ToString().Equals(TEXT("DefaultSceneRoot"),
                                                   ESearchCase::IgnoreCase)) {
          NewParentNode = R;
          break;
        }
      }
      // Fallback: first root that is not the component itself
      if (!NewParentNode) {
        for (USCS_Node *R : Roots) {
          if (R && R != ComponentNode) {
            NewParentNode = R;
            break;
          }
        }
      }
    }

    if (!NewParentNode) {
      for (USCS_Node *Node : SCS->GetAllNodes()) {
        if (Node && Node->GetVariableName().IsValid() &&
            Node->GetVariableName().ToString().Equals(
                NewParentName, ESearchCase::IgnoreCase)) {
          NewParentNode = Node;
          break;
        }
      }
    }

  if (!NewParentNode) {
      // If caller asked for RootComponent and we can't resolve it, treat as a
      // benign no-op
      if (bRootSynonym) {
        Result->SetBoolField(TEXT("success"), true);
        Result->SetStringField(
            TEXT("message"),
            TEXT("Requested RootComponent not found; component remains at "
                 "current hierarchy (treated as success)."));
        AddAssetVerification(Result, Blueprint);
        return Result;
      }
      Result->SetBoolField(TEXT("success"), false);
      Result->SetStringField(
          TEXT("error"),
          FString::Printf(TEXT("New parent not found: %s"), *NewParentName));
      return Result;
    }
  }

  // Helper: check if B is a descendant of A (prevent cycles)
  auto IsDescendantOf = [](USCS_Node *A, USCS_Node *B) -> bool {
    if (!A || !B)
      return false;
    TArray<USCS_Node *> Stack;
    Stack.Add(A);
    while (Stack.Num() > 0) {
      USCS_Node *Cur = Stack.Pop(EAllowShrinking::No);
      if (!Cur)
        continue;
      const TArray<USCS_Node *> &Kids = Cur->GetChildNodes();
      for (USCS_Node *K : Kids) {
        if (!K)
          continue;
        if (K == B)
          return true;
        Stack.Add(K);
      }
    }
    return false;
  };

  // Remove from current parent (UE 5.6: find parent manually)
  USCS_Node *OldParent = nullptr;
  for (USCS_Node *Candidate : SCS->GetAllNodes()) {
    if (Candidate && Candidate->GetChildNodes().Contains(ComponentNode)) {
      OldParent = Candidate;
      break;
    }
  }

  // No-op checks (already under desired parent)
  if ((OldParent == nullptr && NewParentNode && SCS->GetRootNodes().Num() > 0 &&
       NewParentNode == SCS->GetRootNodes()[0]) ||
      (OldParent != nullptr && NewParentNode == OldParent)) {
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(
        TEXT("message"),
        TEXT("Component already under requested parent; no changes made"));
    AddAssetVerification(Result, Blueprint);
    return Result;
  }

  // Prevent cycles: new parent cannot be a descendant of the component
  if (NewParentNode && IsDescendantOf(ComponentNode, NewParentNode)) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        TEXT("Cannot create circular parent-child relationship"));
    return Result;
  }

  // Detach from old parent
  if (OldParent) {
    OldParent->RemoveChildNode(ComponentNode);
  } else {
    // Was a root node; remove from root listing when reparenting to non-root
    const bool bReparentingToRoot = (NewParentNode == nullptr);
    if (!bReparentingToRoot) {
      SCS->RemoveNode(ComponentNode);
    }
    // else already at root and staying root would have been returned above
  }

  // Attach to new parent or root
  if (NewParentNode) {
    NewParentNode->AddChildNode(ComponentNode);
  } else {
    SCS->AddNode(ComponentNode);
  }

  // Mark blueprint as modified and finalize change
  FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
  bool bCompiled = false;
  bool bSaved = false;
  FinalizeBlueprintSCSChange(Blueprint, Result, bCompiled, bSaved);

  Result->SetBoolField(TEXT("success"), true);
  Result->SetStringField(
      TEXT("message"),
      FString::Printf(TEXT("Component '%s' reparented to '%s'"), *ComponentName,
                      NewParentName.IsEmpty() ? TEXT("(root)")
                                              : *NewParentName));
  Result->SetBoolField(TEXT("compiled"), bCompiled);
  Result->SetBoolField(TEXT("saved"), bSaved);
  AddAssetVerification(Result, Blueprint);

  return Result;
}

// Set component transform in SCS
TSharedPtr<FJsonObject> FSCSHandlers::SetSCSComponentTransform(
    const FString &BlueprintPath, const FString &ComponentName,
    const TSharedPtr<FJsonObject> &TransformData) {
  TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

  FString NormalizedPath;
  FString ErrorMsg;
  UBlueprint *Blueprint =
      LoadBlueprintAsset(BlueprintPath, NormalizedPath, ErrorMsg);
  if (!Blueprint) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        ErrorMsg.IsEmpty()
            ? FString::Printf(TEXT("Blueprint asset not found at path: %s"),
                              *BlueprintPath)
            : ErrorMsg);
    Result->SetStringField(TEXT("errorCode"), TEXT("ASSET_NOT_FOUND"));
    return Result;
  }

  if (!Blueprint->SimpleConstructionScript) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Blueprint has no SimpleConstructionScript: %s"),
                        *BlueprintPath));
    Result->SetStringField(TEXT("errorCode"), TEXT("SCS_NOT_FOUND"));
    return Result;
  }

  USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript;

  // Find component
  USCS_Node *ComponentNode = nullptr;
  for (USCS_Node *Node : SCS->GetAllNodes()) {
    if (Node && Node->GetVariableName().IsValid() &&
        Node->GetVariableName().ToString().Equals(ComponentName,
                                                  ESearchCase::IgnoreCase)) {
      ComponentNode = Node;
      break;
    }
  }

  if (!ComponentNode || !ComponentNode->ComponentTemplate) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Component or template not found: %s"),
                        *ComponentName));
    Result->SetStringField(TEXT("errorCode"),
                           TEXT("SCS_COMPONENT_TEMPLATE_NOT_FOUND"));
    return Result;
  }

  // Parse transform from JSON
  FVector Location(0, 0, 0);
  FRotator Rotation(0, 0, 0);
  FVector Scale(1, 1, 1);

  // Parse location array [x, y, z]
  const TArray<TSharedPtr<FJsonValue>> *LocArray;
  if (TransformData->TryGetArrayField(TEXT("location"), LocArray) &&
      LocArray->Num() >= 3) {
    Location.X = (*LocArray)[0]->AsNumber();
    Location.Y = (*LocArray)[1]->AsNumber();
    Location.Z = (*LocArray)[2]->AsNumber();
  }

  // Parse rotation array [pitch, yaw, roll]
  const TArray<TSharedPtr<FJsonValue>> *RotArray;
  if (TransformData->TryGetArrayField(TEXT("rotation"), RotArray) &&
      RotArray->Num() >= 3) {
    Rotation.Pitch = (*RotArray)[0]->AsNumber();
    Rotation.Yaw = (*RotArray)[1]->AsNumber();
    Rotation.Roll = (*RotArray)[2]->AsNumber();
  }

  // Parse scale array [x, y, z]
  const TArray<TSharedPtr<FJsonValue>> *ScaleArray;
  if (TransformData->TryGetArrayField(TEXT("scale"), ScaleArray) &&
      ScaleArray->Num() >= 3) {
    Scale.X = (*ScaleArray)[0]->AsNumber();
    Scale.Y = (*ScaleArray)[1]->AsNumber();
    Scale.Z = (*ScaleArray)[2]->AsNumber();
  }

  // Apply transform to component template
  FTransform NewTransform(Rotation, Location, Scale);

  if (USceneComponent *SceneComp =
          Cast<USceneComponent>(ComponentNode->ComponentTemplate)) {
    SceneComp->SetRelativeTransform(NewTransform);

    bool bCompiled = false;
    bool bSaved = false;
    FinalizeBlueprintSCSChange(Blueprint, Result, bCompiled, bSaved);

    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(
        TEXT("message"),
        FString::Printf(TEXT("Transform set for component '%s'"),
                        *ComponentName));
    Result->SetBoolField(TEXT("compiled"), bCompiled);
    Result->SetBoolField(TEXT("saved"), bSaved);
    AddAssetVerification(Result, Blueprint);
  } else {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        TEXT("Component is not a SceneComponent (no transform)"));
    Result->SetStringField(TEXT("errorCode"), TEXT("SCS_NOT_SCENE_COMPONENT"));
  }

  return Result;
}

// One property write onto a resolved SCS component template.
//
// The collision sub-fields of UPrimitiveComponent::BodyInstance are the one group where a
// reflection store onto a TEMPLATE is not the write that matters: instancing the template
// duplicates it and routes ConditionalPostLoad -> UPrimitiveComponent::PostLoad ->
// FBodyInstance::FixupData -> LoadProfileData -> UCollisionProfile::ReadConfig on the copy,
// which re-derives CollisionEnabled, ObjectType and the responses from CollisionProfileName
// AFTER the archetype values arrive. The store lands on the template, serialises, reads back
// correctly - and every spawned instance overwrites it. Those fields therefore go through
// the same typed-setter route actor.set_component_properties uses; the full chain with
// engine line numbers is in Utils/BodyInstanceCollisionPropertyWrite.h. Every other property
// keeps the plain reflection store it has today.
static bool ApplySCSTemplatePropertyValue(UObject *Target, void *ContainerPtr,
                                          FProperty *TargetProp,
                                          const FString &PropertyName,
                                          const TSharedPtr<FJsonValue> &PropertyValue,
                                          bool &bOutCollisionRouted, FString &OutError) {
  bOutCollisionRouted = false;
  if (UActorComponent *Component = Cast<UActorComponent>(Target)) {
    PinWright::FBodyInstanceCollisionMeasurement Measurement;
    const PinWright::EBodyInstanceCollisionWrite Routed =
        PinWright::ApplyBodyInstanceCollisionPropertyPath(
            Component, PropertyName, PropertyValue, Measurement, OutError);
    if (Routed == PinWright::EBodyInstanceCollisionWrite::Applied) {
      bOutCollisionRouted = Measurement.bCollisionWritten;
      return true;
    }
    if (Routed == PinWright::EBodyInstanceCollisionWrite::Failed) {
      return false;
    }
  }
  return ApplyJsonValueToProperty(ContainerPtr, TargetProp, PropertyValue, OutError);
}

// Set component property in SCS
TSharedPtr<FJsonObject> FSCSHandlers::SetSCSComponentProperty(
    const FString &BlueprintPath, const FString &ComponentName,
    const FString &PropertyName, const TSharedPtr<FJsonValue> &PropertyValue) {
  TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject);

  FString NormalizedPath;
  FString ErrorMsg;
  UBlueprint *Blueprint =
      LoadBlueprintAsset(BlueprintPath, NormalizedPath, ErrorMsg);
  if (!Blueprint) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        ErrorMsg.IsEmpty()
            ? FString::Printf(TEXT("Blueprint asset not found at path: %s"),
                              *BlueprintPath)
            : ErrorMsg);
    Result->SetStringField(TEXT("errorCode"), TEXT("ASSET_NOT_FOUND"));
    return Result;
  }

  if (!Blueprint->SimpleConstructionScript) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Blueprint has no SimpleConstructionScript: %s"),
                        *BlueprintPath));
    return Result;
  }

  USimpleConstructionScript *SCS = Blueprint->SimpleConstructionScript;

  // Find component — try local SCS first, then child overrides for inherited SCS, then CDO subobjects
  USCS_Node *ComponentNode = nullptr;
  UObject *ComponentTemplate = nullptr;
  FComponentKey InheritedComponentKey;
  FString LookupSource;
  bool bUsesInheritedScsParentTemplate = false;

  // Fallback 0: Local SCS
  for (USCS_Node *Node : SCS->GetAllNodes()) {
    if (Node && Node->GetVariableName().IsValid() &&
        Node->GetVariableName().ToString().Equals(ComponentName,
                                                  ESearchCase::IgnoreCase)) {
      ComponentNode = Node;
      break;
    }
  }

  if (ComponentNode && ComponentNode->ComponentTemplate) {
    ComponentTemplate = ComponentNode->ComponentTemplate;
    LookupSource = TEXT("local");
  }

  // Fallback 1: Parent Blueprint SCS hierarchy
  if (!ComponentTemplate) {
    FInheritedScsTemplateResolution InheritedResolution;
    if (ResolveInheritedScsParentTemplate(Blueprint, ComponentName, InheritedResolution)) {
      ComponentTemplate = InheritedResolution.ParentTemplate;
      InheritedComponentKey = InheritedResolution.ComponentKey;
      bUsesInheritedScsParentTemplate = true;
      LookupSource = TEXT("inherited_scs");
    }
  }

  // Fallback 2: CDO default subobjects
  if (!ComponentTemplate) {
    if (Blueprint->GeneratedClass) {
      UObject *CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
      if (CDO) {
        ForEachObjectWithOuter(CDO, [&](UObject *SubObj) {
          if (!ComponentTemplate &&
              SubObj->GetName().Equals(ComponentName,
                                       ESearchCase::IgnoreCase)) {
            ComponentTemplate = SubObj;
          }
        });
        if (ComponentTemplate) {
          LookupSource = TEXT("default_subobject");
        }
      }
    }
  }

  if (!ComponentTemplate) {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(
        TEXT("error"),
        FString::Printf(TEXT("Component or template not found: %s"),
                        *ComponentName));
    Result->SetStringField(TEXT("errorCode"),
                           TEXT("SCS_COMPONENT_TEMPLATE_NOT_FOUND"));
    return Result;
  }

  bool bCollisionRouted = false;
  if (PropertyValue.IsValid()) {
    void *ContainerPtr = nullptr;
    FString ResolveError;
    FString FailureMessage;
    FString FailureCode;
    bool bAppliedValue = false;
    FProperty *TargetProp = ResolveNestedPropertyPath(
        ComponentTemplate, PropertyName, ContainerPtr, ResolveError);

    if (!TargetProp || !ContainerPtr) {
      Result->SetBoolField(TEXT("success"), false);
      Result->SetStringField(
          TEXT("error"),
          ResolveError.IsEmpty()
              ? FString::Printf(TEXT("Property not found: %s"), *PropertyName)
              : ResolveError);
      Result->SetStringField(TEXT("errorCode"), TEXT("SCS_PROPERTY_NOT_FOUND"));
      return Result;
    }

    if (bUsesInheritedScsParentTemplate) {
      UObject* ValidationTemplate = DuplicateObject<UObject>(
          ComponentTemplate, GetTransientPackage());
      if (!ValidationTemplate) {
        Result->SetBoolField(TEXT("success"), false);
        Result->SetStringField(
            TEXT("error"),
            TEXT("Failed to prepare inherited component property validation"));
        Result->SetStringField(TEXT("errorCode"),
                               TEXT("SCS_PROPERTY_APPLY_FAILED"));
        return Result;
      }

      void* ValidationContainerPtr = nullptr;
      FString ValidationResolveError;
      FProperty* ValidationTargetProp = ResolveNestedPropertyPath(
          ValidationTemplate, PropertyName, ValidationContainerPtr,
          ValidationResolveError);
      if (!ValidationTargetProp || !ValidationContainerPtr) {
        Result->SetBoolField(TEXT("success"), false);
        Result->SetStringField(
            TEXT("error"),
            ValidationResolveError.IsEmpty()
                ? FString::Printf(TEXT("Property not found: %s"), *PropertyName)
                : ValidationResolveError);
        Result->SetStringField(TEXT("errorCode"), TEXT("SCS_PROPERTY_NOT_FOUND"));
        return Result;
      }

      if (!ApplyJsonValueToProperty(ValidationContainerPtr, ValidationTargetProp,
                                    PropertyValue, FailureMessage)) {
        FailureCode = TEXT("SCS_PROPERTY_APPLY_FAILED");
      } else {
        bool bCreatedOverride = false;
        UActorComponent* OverrideTemplate = ResolveInheritedScsOverrideTemplate(
            Blueprint, InheritedComponentKey, bCreatedOverride);
        if (!OverrideTemplate) {
          Result->SetBoolField(TEXT("success"), false);
          Result->SetStringField(
              TEXT("error"),
              FString::Printf(TEXT("Component override template not found: %s"),
                              *ComponentName));
          Result->SetStringField(TEXT("errorCode"),
                                 TEXT("SCS_COMPONENT_TEMPLATE_NOT_FOUND"));
          return Result;
        }

        void* OverrideContainerPtr = nullptr;
        FString OverrideResolveError;
        FProperty* OverrideTargetProp = ResolveNestedPropertyPath(
            OverrideTemplate, PropertyName, OverrideContainerPtr,
            OverrideResolveError);
        if (!OverrideTargetProp || !OverrideContainerPtr) {
          if (bCreatedOverride) {
            if (UInheritableComponentHandler* Handler =
                    Blueprint->GetInheritableComponentHandler(false)) {
              Handler->RemoveOverridenComponentTemplate(InheritedComponentKey);
            }
          }
          Result->SetBoolField(TEXT("success"), false);
          Result->SetStringField(
              TEXT("error"),
              OverrideResolveError.IsEmpty()
                  ? FString::Printf(TEXT("Property not found: %s"), *PropertyName)
                  : OverrideResolveError);
          Result->SetStringField(TEXT("errorCode"), TEXT("SCS_PROPERTY_NOT_FOUND"));
          return Result;
        }

        if (ApplySCSTemplatePropertyValue(OverrideTemplate, OverrideContainerPtr,
                                          OverrideTargetProp, PropertyName,
                                          PropertyValue, bCollisionRouted,
                                          FailureMessage)) {
          bAppliedValue = true;
        } else {
          FailureCode = TEXT("SCS_PROPERTY_APPLY_FAILED");
          if (bCreatedOverride) {
            if (UInheritableComponentHandler* Handler =
                    Blueprint->GetInheritableComponentHandler(false)) {
              Handler->RemoveOverridenComponentTemplate(InheritedComponentKey);
            }
          }
        }
      }
    } else {
      if (ApplySCSTemplatePropertyValue(ComponentTemplate, ContainerPtr, TargetProp,
                                        PropertyName, PropertyValue,
                                        bCollisionRouted, FailureMessage)) {
        bAppliedValue = true;
      } else {
        FailureCode = TEXT("SCS_PROPERTY_APPLY_FAILED");
      }
    }

    if (!bAppliedValue) {
      Result->SetBoolField(TEXT("success"), false);
      Result->SetStringField(TEXT("error"),
                             FailureMessage.IsEmpty()
                                 ? TEXT("Failed to apply property value")
                                 : FailureMessage);
      if (!FailureCode.IsEmpty()) {
        Result->SetStringField(TEXT("errorCode"), FailureCode);
      }
      return Result;
    }
  } else {
    Result->SetBoolField(TEXT("success"), false);
    Result->SetStringField(TEXT("error"), TEXT("Property value is invalid"));
    Result->SetStringField(TEXT("errorCode"),
                           TEXT("SCS_PROPERTY_INVALID_VALUE"));
    return Result;
  }

  FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
  bool bCompiled = false;
  bool bSaved = false;
  FinalizeBlueprintSCSChange(Blueprint, Result, bCompiled, bSaved);

  Result->SetBoolField(TEXT("success"), true);
  Result->SetStringField(
      TEXT("message"),
      FString::Printf(TEXT("Property '%s' set on component '%s'"),
                      *PropertyName, *ComponentName));
  Result->SetStringField(TEXT("source"), LookupSource);
  Result->SetBoolField(TEXT("compiled"), bCompiled);
  Result->SetBoolField(TEXT("saved"), bSaved);
  if (bCollisionRouted) {
    // Present only when a BodyInstance collision field actually changed and went through
    // the engine setters. The ticket this closes was reported because a raw store and a
    // real write returned byte-identical responses; this is the one bit that tells them
    // apart, and it is a measurement of which path ran, not a claim about a spawned body.
    Result->SetBoolField(TEXT("collisionRouted"), true);
  }
  AddAssetVerification(Result, Blueprint);

  return Result;
}
