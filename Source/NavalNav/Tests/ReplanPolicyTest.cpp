// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
//
// Automation tests for the Slice 4 replanning core: the replan policy, the escape ring-search, and
// the mid-voyage splice. All plain structs, so these run with no world. Mirrored in Tools/AlgoSelfTest.
//
//   UnrealEditor-Cmd.exe NavalNavSample.uproject -ExecCmds="Automation RunTests NavalNav" -unattended -nopause

#include "CoreMinimal.h"
#include "Grid/SeaGridPathfinder.h"
#include "Grid/SeaGridTypes.h"
#include "Misc/AutomationTest.h"
#include "Navigation/PredictiveHelmsman.h"
#include "Navigation/ReplanPolicy.h"
#include "Ship/SailingModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NavalNavReplanTest
{
	static FNavalPath MakePath(const TArray<FVector>& Points)
	{
		FNavalPath Path;
		float Accumulated = 0.0f;
		for (int32 Index = 0; Index < Points.Num(); ++Index)
		{
			if (Index > 0)
			{
				Accumulated += static_cast<float>(FVector::Dist2D(Points[Index - 1], Points[Index]));
			}
			Path.Waypoints.Add(Points[Index]);
			Path.Costs.Add(Accumulated);
		}
		Path.bSuccess = Points.Num() > 0;
		return Path;
	}

	/** A square grid of CellsPerSide cells at 100 uu, lower corner at the origin (as the other tests use). */
	static FSeaGridData MakeGrid(int32 CellsPerSide)
	{
		FSeaGridConfig Config;
		Config.CellSize = 100.0f;
		Config.Extent = FVector2D(CellsPerSide * 50.0, CellsPerSide * 50.0);
		Config.Center = FVector2D(Config.Extent.X, Config.Extent.Y);
		FSeaGridData Grid;
		Grid.Rebuild(Config);
		return Grid;
	}
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavReplanOffRouteTest, "NavalNav.Replan.OffRouteRespectsGrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavReplanOffRouteTest::RunTest(const FString& Parameters)
{
	FReplanPolicy Policy;
	// Isolate the off-route trigger.
	Policy.Params.bEnablePathBlocked = false;
	Policy.Params.bEnableThreatChanged = false;
	Policy.Params.bEnablePowerChanged = false;
	Policy.Params.bEnablePeriodic = false;
	Policy.Params.OffRouteDistance = 1500.0f;
	Policy.Params.OffRouteGraceSeconds = 1.5f;
	Policy.Params.MinReplanInterval = 1.0f;

	const FNavalPath Path = NavalNavReplanTest::MakePath({ FVector(0, 0, 0), FVector(10000, 0, 0) });
	auto OpenWater = [](const FVector&) { return NavalNav::OpenWaterCost; };

	FReplanSituation Sit;
	Sit.DeltaSeconds = 0.1f;
	Sit.bFollowing = true;
	Sit.ShipLocation = FVector(5000, 2000, 0); // 2000 uu off the line, beyond the threshold
	Sit.Path = &Path;
	Sit.ActiveWaypoint = 1;

	bool bFiredEarly = false;
	for (int32 Tick = 0; Tick < 12; ++Tick) // 1.2 s < grace
	{
		bFiredEarly |= Policy.Evaluate(Sit, OpenWater).bShouldReplan;
	}
	TestFalse(TEXT("No replan before the grace time elapses"), bFiredEarly);

	bool bFiredAfter = false;
	for (int32 Tick = 0; Tick < 6; ++Tick) // now past 1.5 s of being off-route
	{
		bFiredAfter |= Policy.Evaluate(Sit, OpenWater).bShouldReplan;
	}
	TestTrue(TEXT("A replan fires once the ship has been off-route past the grace time"), bFiredAfter);
	TestEqual(TEXT("The reason is off-route"), Policy.GetLastReason(), EReplanReason::OffRoute);

	// A ship on the line never trips the trigger, however long it sails.
	FReplanPolicy OnLine = Policy;
	FReplanSituation OnSit = Sit;
	OnSit.ShipLocation = FVector(5000, 100, 0); // within the threshold
	bool bFalsePositive = false;
	for (int32 Tick = 0; Tick < 50; ++Tick)
	{
		bFalsePositive |= OnLine.Evaluate(OnSit, OpenWater).bShouldReplan;
	}
	TestFalse(TEXT("On the line, off-route never fires"), bFalsePositive);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavReplanBlockedTest, "NavalNav.Replan.PathBlockedIsPerShip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavReplanBlockedTest::RunTest(const FString& Parameters)
{
	const FNavalPath Path = NavalNavReplanTest::MakePath({ FVector(0, 0, 0), FVector(10000, 0, 0) });

	FReplanSituation Sit;
	Sit.DeltaSeconds = 0.5f;
	Sit.bFollowing = true;
	Sit.ShipLocation = FVector(0, 0, 0);
	Sit.Path = &Path;
	Sit.ActiveWaypoint = 1;

	// A lethal patch sits on the route between x=4000 and x=6000.
	auto WeakShipCost = [](const FVector& P)
	{
		return (P.X > 4000.0 && P.X < 6000.0) ? NavalNav::ImpassableCost : NavalNav::OpenWaterCost;
	};
	// The same water, but this ship outguns the zone, so it reads as open water.
	auto StrongShipCost = [](const FVector&) { return NavalNav::OpenWaterCost; };

	FReplanPolicy Weak;
	Weak.Params.bEnableOffRoute = false;
	Weak.Params.bEnableThreatChanged = false;
	Weak.Params.bEnablePowerChanged = false;
	Weak.Params.bEnablePeriodic = false;
	const FReplanDecision WeakDecision = Weak.Evaluate(Sit, WeakShipCost);
	TestTrue(TEXT("A blocking cell on the route triggers a replan for the weak ship"), WeakDecision.bShouldReplan);
	TestEqual(TEXT("The reason is path-blocked"), WeakDecision.Reason, EReplanReason::PathBlocked);

	FReplanPolicy Strong;
	Strong.Params.bEnableOffRoute = false;
	Strong.Params.bEnableThreatChanged = false;
	Strong.Params.bEnablePowerChanged = false;
	Strong.Params.bEnablePeriodic = false;
	const FReplanDecision StrongDecision = Strong.Evaluate(Sit, StrongShipCost);
	TestFalse(TEXT("The same water does not block a ship that outguns the zone"), StrongDecision.bShouldReplan);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavReplanHysteresisTest, "NavalNav.Replan.HysteresisPreventsStorms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavReplanHysteresisTest::RunTest(const FString& Parameters)
{
	FReplanPolicy Policy;
	Policy.Params.bEnableOffRoute = false;
	Policy.Params.bEnablePathBlocked = false;
	Policy.Params.bEnablePowerChanged = false;
	Policy.Params.bEnablePeriodic = false;
	Policy.Params.MinReplanInterval = 1.0f;

	const FNavalPath Path = NavalNavReplanTest::MakePath({ FVector(0, 0, 0), FVector(10000, 0, 0) });
	auto OpenWater = [](const FVector&) { return NavalNav::OpenWaterCost; };

	FReplanSituation Sit;
	Sit.DeltaSeconds = 0.1f;
	Sit.bFollowing = true;
	Sit.ShipLocation = FVector(3000, 0, 0);
	Sit.Path = &Path;
	Sit.ActiveWaypoint = 1;

	// A zone jitters every single tick for 10 seconds: 100 change events.
	for (int32 Tick = 0; Tick < 100; ++Tick)
	{
		Policy.NotifyThreatChanged();
		Policy.Evaluate(Sit, OpenWater);
	}

	// The min-interval of 1 s should collapse 100 jitters into about ten replans, not a hundred.
	TestTrue(TEXT("Jitters do not cause a replan storm"), Policy.GetValidationCount() <= 12);
	TestTrue(TEXT("But the relevant change is still acted on periodically"), Policy.GetValidationCount() >= 8);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavReplanAdoptTest, "NavalNav.Replan.AdoptOnlyMeaningfulImprovements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavReplanAdoptTest::RunTest(const FString& Parameters)
{
	FReplanPolicy Policy;
	Policy.Params.ImprovementCostRatio = 0.9f;

	TestTrue(TEXT("An invalid old path is always replaced"), Policy.ShouldAdoptNewPath(100.0f, 120.0f, /*bOldInvalid=*/true));
	TestFalse(TEXT("A barely-cheaper route is not worth switching to"), Policy.ShouldAdoptNewPath(100.0f, 95.0f, false));
	TestTrue(TEXT("A meaningfully cheaper route is adopted"), Policy.ShouldAdoptNewPath(100.0f, 80.0f, false));
	TestFalse(TEXT("A more expensive route is kept out"), Policy.ShouldAdoptNewPath(100.0f, 130.0f, false));

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavEscapeNearestTest, "NavalNav.Replan.EscapePicksNearestSafeCell",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavEscapeNearestTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavReplanTest::MakeGrid(60);
	const FIntPoint Origin(30, 30);

	// A hostile disc of radius 5 cells around the origin; everything outside it is open water.
	const int32 HostileRadius = 5;
	auto Cost = [Origin, HostileRadius](const FIntPoint& Cell)
	{
		const int32 Cheb = FMath::Max(FMath::Abs(Cell.X - Origin.X), FMath::Abs(Cell.Y - Origin.Y));
		return Cheb <= HostileRadius ? 40.0f : NavalNav::OpenWaterCost;
	};

	FIntPoint Out;
	const bool bFound = FSeaGridPathfinder::FindNearestCellBelowCost(Grid, Origin, Cost,
		NavalNav::OpenWaterCost + 0.01f, /*MaxRing=*/30, Out);

	TestTrue(TEXT("An escape cell is found"), bFound);
	TestTrue(TEXT("The escape cell is open water"), Cost(Out) <= NavalNav::OpenWaterCost + 0.01f);
	const int32 OutRing = FMath::Max(FMath::Abs(Out.X - Origin.X), FMath::Abs(Out.Y - Origin.Y));
	TestEqual(TEXT("It is the nearest safe ring, just outside the hostile disc"), OutRing, HostileRadius + 1);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavEscapeEnclosedTest, "NavalNav.Replan.EscapeTakesLeastBadExitWhenEnclosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavEscapeEnclosedTest::RunTest(const FString& Parameters)
{
	FSeaGridData Grid = NavalNavReplanTest::MakeGrid(60);
	const FIntPoint Origin(30, 30);

	// Everything in range is hostile, but a +X corridor is only half as costly: the least-bad exit.
	auto Cost = [Origin](const FIntPoint& Cell)
	{
		const bool bGap = (Cell.X > Origin.X) && (FMath::Abs(Cell.Y - Origin.Y) <= 1);
		return bGap ? 20.0f : 50.0f;
	};

	FIntPoint Out;
	const bool bFound = FSeaGridPathfinder::FindNearestCellBelowCost(Grid, Origin, Cost,
		/*AcceptCost=*/NavalNav::OpenWaterCost + 0.01f, /*MaxRing=*/8, Out);

	TestTrue(TEXT("A least-bad exit is returned even when nothing is safe"), bFound);
	TestTrue(TEXT("The exit is through the weaker corridor"), Out.X > Origin.X);
	TestEqual(TEXT("The exit cost is the corridor's, not the wall's"), Cost(Out), 20.0f, 1.0e-3f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavSpliceContinuityTest, "NavalNav.Replan.SpliceKeepsHeadingContinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavSpliceContinuityTest::RunTest(const FString& Parameters)
{
	FSailingModelParams Sail;
	FPredictiveHelmsman Helmsman;

	const FVector ShipPos(3000, 0, 0);
	FHelmsmanInput In;
	In.ShipLocation = ShipPos;
	In.ShipHeadingDeg = 0.0f;
	In.ShipSpeed = 800.0f;
	In.WindFromDeg = -90.0f;

	// Following the old route east.
	const FNavalPath OldPath = NavalNavReplanTest::MakePath({ FVector(0, 0, 0), FVector(10000, 0, 0) });
	const float DesiredOld = Helmsman.Update(OldPath, In, Sail, 0.05f).DesiredHeadingDeg;

	// The replan is spliced from the ship's CURRENT position toward a slightly different goal, and the
	// helmsman is reset for the new path. Because waypoint 0 is the ship, the desired heading barely
	// moves — there is no snap back toward the old start.
	const FNavalPath NewPath = NavalNavReplanTest::MakePath({ ShipPos, FVector(10000, 1500, 0) });
	Helmsman.Reset();
	const float DesiredNew = Helmsman.Update(NewPath, In, Sail, 0.05f).DesiredHeadingDeg;

	const float Jump = FMath::Abs(FSailingModel::NormalizeDegrees(DesiredNew - DesiredOld));
	TestTrue(TEXT("The spliced route does not snap the desired heading around"), Jump < 45.0f);

	// Contrast: a route that (wrongly) started back at the origin would demand a near-U-turn.
	const FNavalPath BadPath = NavalNavReplanTest::MakePath({ FVector(0, 0, 0), FVector(0, -100, 0) });
	Helmsman.Reset();
	const float DesiredBad = Helmsman.Update(BadPath, In, Sail, 0.05f).DesiredHeadingDeg;
	TestTrue(TEXT("A reset-to-start would have jerked the helm hard over (sanity of the test)"),
		FMath::Abs(FSailingModel::NormalizeDegrees(DesiredBad - DesiredOld)) > 90.0f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavReplanRelevanceTest, "NavalNav.Replan.RelevanceFiltersDistantChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavReplanRelevanceTest::RunTest(const FString& Parameters)
{
	const FNavalPath Path = NavalNavReplanTest::MakePath({ FVector(0, 0, 0), FVector(20000, 0, 0) });
	const FVector Ship(1000, 0, 0);

	TestTrue(TEXT("A change on the route ahead is relevant"),
		FReplanPolicy::IsChangeRelevant(FVector(10000, 500, 0), 2000.0f, Ship, Path, 1, 6000.0f));
	TestFalse(TEXT("A change far off the route is ignored"),
		FReplanPolicy::IsChangeRelevant(FVector(10000, 40000, 0), 2000.0f, Ship, Path, 1, 6000.0f));
	TestTrue(TEXT("A change right on top of the ship is relevant"),
		FReplanPolicy::IsChangeRelevant(FVector(1200, 300, 0), 500.0f, Ship, Path, 1, 6000.0f));

	// Cross-track distance is measured to the active leg.
	TestEqual(TEXT("Cross-track equals the perpendicular offset"),
		FReplanPolicy::CrossTrackDistance(Path, 1, FVector(5000, 1200, 0)), 1200.0f, 1.0f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
