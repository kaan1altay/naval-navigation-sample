// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
//
// Automation tests for the predictive helmsman. Like the sailing model, it is a plain struct, so
// these run with no world — including a full closed-loop sim of FSailingModel + FPredictiveHelmsman
// sailing a zigzag to its goal. The same scenarios also run in Tools/AlgoSelfTest.
//
//   UnrealEditor-Cmd.exe NavalNavSample.uproject -ExecCmds="Automation RunTests NavalNav" -unattended -nopause

#include "CoreMinimal.h"
#include "Grid/SeaGridTypes.h"
#include "Misc/AutomationTest.h"
#include "Navigation/PredictiveHelmsman.h"
#include "Ship/SailingModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NavalNavHelmsmanTest
{
	/** Builds a successful FNavalPath through the given world-space points. */
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
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavHelmsmanStraightTest, "NavalNav.Helmsman.HoldsAStraightLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavHelmsmanStraightTest::RunTest(const FString& Parameters)
{
	FPredictiveHelmsman Helmsman;
	FSailingModelParams Sail;

	const FNavalPath Path = NavalNavHelmsmanTest::MakePath({ FVector(0, 0, 0), FVector(10000, 0, 0) });

	FHelmsmanInput In;
	In.ShipLocation = FVector(2000, 0, 0);   // on the line
	In.ShipHeadingDeg = 0.0f;                // pointed straight down it
	In.ShipSpeed = 800.0f;
	In.WindFromDeg = -90.0f;                 // beam reach on heading 0

	const FHelmsmanOutput Out = Helmsman.Update(Path, In, Sail, 0.05f);

	TestTrue(TEXT("Dead on course the helm stays near amidships"), FMath::Abs(Out.RudderInput) < 0.05f);
	TestEqual(TEXT("Desired heading is straight down the line"), Out.DesiredHeadingDeg, 0.0f, 1.0f);
	TestFalse(TEXT("Not tacking on a beam reach"), Out.bTacking);
	TestFalse(TEXT("Not arrived yet"), Out.bArrived);

	// A small heading disturbance is corrected back the right way.
	In.ShipHeadingDeg = 10.0f;
	const FHelmsmanOutput Corrected = Helmsman.Update(Path, In, Sail, 0.05f);
	TestTrue(TEXT("A positive heading error orders negative rudder"), Corrected.RudderInput < 0.0f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavHelmsmanTurnInTest, "NavalNav.Helmsman.TurnsInBeforeTheCorner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavHelmsmanTurnInTest::RunTest(const FString& Parameters)
{
	FSailingModelParams Sail;

	// Turn-in distance is positive with way on, zero without, and grows with speed above the knee.
	TestEqual(TEXT("No way on, no turn-in"), FPredictiveHelmsman::TurnInDistance(90.0f, 0.0f, Sail), 0.0f, 1.0e-3f);
	const float TurnInSlow = FPredictiveHelmsman::TurnInDistance(90.0f, 600.0f, Sail);
	const float TurnInFast = FPredictiveHelmsman::TurnInDistance(90.0f, 1200.0f, Sail);
	TestTrue(TEXT("Turn-in is positive with way on"), TurnInSlow > 0.0f);
	TestTrue(TEXT("Turn-in grows with speed"), TurnInFast > TurnInSlow + 1.0f);
	TestTrue(TEXT("A sharper corner needs more turn-in than a gentle one"),
		FPredictiveHelmsman::TurnInDistance(120.0f, 1200.0f, Sail) > FPredictiveHelmsman::TurnInDistance(45.0f, 1200.0f, Sail));

	// A right-angle corner: straight east, then north.
	const FNavalPath Path = NavalNavHelmsmanTest::MakePath(
		{ FVector(0, 0, 0), FVector(10000, 0, 0), FVector(10000, 10000, 0) });

	FHelmsmanInput In;
	In.ShipHeadingDeg = 0.0f;
	In.ShipSpeed = 1200.0f;
	In.WindFromDeg = -90.0f;

	// Well before the corner (beyond both look-ahead and turn-in) the helm is idle.
	{
		FPredictiveHelmsman Helmsman;
		In.ShipLocation = FVector(3000, 0, 0);   // 7000 uu from the corner
		const FHelmsmanOutput Out = Helmsman.Update(Path, In, Sail, 0.05f);
		TestTrue(TEXT("Far from the corner the helm is near amidships"), FMath::Abs(Out.RudderInput) < 0.05f);
	}

	// Inside the turn-in distance the helm is already ordered toward the next leg (a left turn,
	// i.e. positive yaw / positive rudder), before the ship has reached the corner.
	{
		FPredictiveHelmsman Helmsman;
		In.ShipLocation = FVector(8500, 0, 0);   // 1500 uu from the corner, inside turn-in
		const FHelmsmanOutput Out = Helmsman.Update(Path, In, Sail, 0.05f);
		TestTrue(TEXT("Inside turn-in the corner is already being anticipated"), Out.RudderInput > 0.05f);
		TestTrue(TEXT("The desired heading has begun swinging toward the next leg"), Out.DesiredHeadingDeg > 5.0f);
	}

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavHelmsmanMissedWaypointTest, "NavalNav.Helmsman.AdvancesPastAMissedWaypoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavHelmsmanMissedWaypointTest::RunTest(const FString& Parameters)
{
	FPredictiveHelmsman Helmsman;
	FSailingModelParams Sail;

	const FNavalPath Path = NavalNavHelmsmanTest::MakePath(
		{ FVector(0, 0, 0), FVector(5000, 0, 0), FVector(10000, 0, 0) });

	FHelmsmanInput In;
	// The ship is past the middle waypoint (x > 5000) and off to the side: it was never within the
	// accept radius, but it must not turn back for it.
	In.ShipLocation = FVector(6000, 800, 0);
	In.ShipHeadingDeg = 0.0f;
	In.ShipSpeed = 900.0f;
	In.WindFromDeg = -90.0f;

	const FHelmsmanOutput Out = Helmsman.Update(Path, In, Sail, 0.05f);
	TestEqual(TEXT("The missed middle waypoint is skipped"), Out.ActiveWaypoint, 2);
	TestEqual(TEXT("The active waypoint persists"), Helmsman.GetActiveWaypoint(), 2);

	// Hammering Update must never walk the index backwards or spin.
	for (int32 Iteration = 0; Iteration < 100; ++Iteration)
	{
		const FHelmsmanOutput Repeat = Helmsman.Update(Path, In, Sail, 0.05f);
		if (Repeat.ActiveWaypoint != 2)
		{
			AddError(FString::Printf(TEXT("Active waypoint drifted to %d on iteration %d"), Repeat.ActiveWaypoint, Iteration));
			return false;
		}
	}

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavHelmsmanTackTest, "NavalNav.Helmsman.TacksWhenTheCourseIsUpwind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavHelmsmanTackTest::RunTest(const FString& Parameters)
{
	FPredictiveHelmsman Helmsman;
	FSailingModelParams Sail;

	// Wind straight down +X (blows from bearing 0). A goal due east is therefore dead upwind.
	const FNavalPath Path = NavalNavHelmsmanTest::MakePath({ FVector(0, 0, 0), FVector(10000, 0, 0) });

	FHelmsmanInput In;
	In.ShipLocation = FVector(0, 0, 0);
	In.ShipHeadingDeg = 0.0f;
	In.ShipSpeed = 600.0f;
	In.WindFromDeg = 0.0f;

	const FHelmsmanOutput Out = Helmsman.Update(Path, In, Sail, 0.05f);

	TestTrue(TEXT("A dead-upwind course triggers a tack"), Out.bTacking);
	const float TackOffWind = FSailingModel::AngleOffWind(Out.DesiredHeadingDeg, In.WindFromDeg);
	TestTrue(TEXT("The tack heading is outside the no-go cone"), TackOffWind >= Sail.NoGoAngleDegrees);
	TestTrue(TEXT("The tack heading only just clears the no-go, to make ground to windward"),
		TackOffWind <= Sail.NoGoAngleDegrees + Helmsman.Params.TackMarginDeg + 1.0f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavHelmsmanClosedLoopTest, "NavalNav.Helmsman.ClosedLoopArrivesWithoutNaN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavHelmsmanClosedLoopTest::RunTest(const FString& Parameters)
{
	FPredictiveHelmsman Helmsman;

	FSailingModel Model;
	Model.Params.bEnableLeeway = false; // keep the closed loop deterministic and axis-aligned

	// A zigzag that always makes ground east, so with the wind blowing east (from bearing 180) no
	// leg is upwind and the ship can sail the whole thing.
	const TArray<FVector> Points = {
		FVector(0, 0, 0), FVector(4000, 3000, 0), FVector(8000, -1000, 0),
		FVector(12000, 3000, 0), FVector(16000, 0, 0) };
	const FNavalPath Path = NavalNavHelmsmanTest::MakePath(Points);
	const float WindFromDeg = 180.0f;

	FSailingState State;
	State.HeadingDegrees = FPredictiveHelmsman::BearingDegrees(Points[0], Points[1]);
	FVector Position = Points[0];
	float LastHeading = State.HeadingDegrees;

	const float Dt = 0.05f;
	bool bArrived = false;
	bool bFinite = true;

	for (int32 Tick = 0; Tick < 10000 && !bArrived; ++Tick)
	{
		FHelmsmanInput In;
		In.ShipLocation = Position;
		In.ShipHeadingDeg = State.HeadingDegrees;
		In.ShipSpeed = State.Speed;
		In.ShipYawRateDeg = FSailingModel::NormalizeDegrees(State.HeadingDegrees - LastHeading) / Dt;
		In.WindFromDeg = WindFromDeg;
		In.WindStrength = 1.0f;

		const FHelmsmanOutput Out = Helmsman.Update(Path, In, Model.Params, Dt);
		LastHeading = State.HeadingDegrees;

		Model.Advance(State, Out.RudderInput, Out.SailTrim, WindFromDeg, 1.0f, Dt);

		const float HeadingRad = FMath::DegreesToRadians(State.HeadingDegrees);
		Position += FVector(FMath::Cos(HeadingRad), FMath::Sin(HeadingRad), 0.0) * (State.Speed * Dt);

		if (!FMath::IsFinite(State.HeadingDegrees) || !FMath::IsFinite(State.Speed)
			|| !FMath::IsFinite(Position.X) || !FMath::IsFinite(Position.Y))
		{
			bFinite = false;
			break;
		}

		bArrived = Out.bArrived;
	}

	TestTrue(TEXT("The closed loop never went non-finite"), bFinite);
	TestTrue(TEXT("The ship arrived at the goal"), bArrived);
	TestTrue(TEXT("It finished within arrival tolerance of the goal"),
		FVector::Dist2D(Position, Points.Last()) < Helmsman.Params.ArrivalRadius * 1.5f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
