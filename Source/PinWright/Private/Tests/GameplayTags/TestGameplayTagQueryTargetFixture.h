// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "TestGameplayTagQueryTargetFixture.generated.h"

// Fixture UCLASS for TestGameplayTagBuildQuery.cpp. gameplay_tags.build_query returns no
// representation of the FGameplayTagQuery it builds — `description` is synthesized from the
// caller's own JSON and `tokenStreamBytes` is a length — so the only way to observe what
// GameplayTagBuildQueryHandler.cpp's ApplyOpKind actually produced is to have the handler
// write the query into a real property and evaluate it with FGameplayTagQuery::Matches.
// Nothing else in the repo carries an FGameplayTagQuery UPROPERTY, and the handler's target
// path resolves via LoadObject, so this must be a loadable asset in a real package rather
// than a transient object. A UDataAsset keeps the asset-registry / force-delete teardown
// path (TestUtils.h CleanupTestAsset) on its normal asset codepath.
UCLASS()
class UTestGameplayTagQueryTarget : public UDataAsset
{
    GENERATED_BODY()

public:
    // The FStructProperty of type FGameplayTagQuery that gameplay_tags.build_query's
    // `target: { assetPath, propertyPath: "Query" }` writes into. Any other property type
    // would be rejected with PROPERTY_WRONG_TYPE.
    UPROPERTY()
    FGameplayTagQuery Query;
};
