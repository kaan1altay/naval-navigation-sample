// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
//
// Automation tests for the sailing model. Like the Slice 1 tests, they exercise a plain struct
// (FSailingModel) with no world, so the physics can be pinned down in isolation. Run them from
// the editor's Session Frontend, or headless with:
//   UnrealEditor-Cmd.exe NavalNavSample.uproject -ExecCmds="Automation RunTests NavalNav" -unattended -nopause
//
// The same scenarios also run without an engine via Tools/AlgoSelfTest/run_tests.sh.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Ship/SailingModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace NavalNavShipTest
{
	/** Default rig, tuned exactly as the pawn's inline defaults are. */
	static FSailingModel MakeModel()
	{
		FSailingModel Model;
		Model.Params = FSailingModelParams();
		return Model;
	}

	/** Sails a fresh ship on a fixed helm/trim for a while and returns the final state. */
	static FSailingState SailFor(const FSailingModel& Model, FSailingState Start, float Rudder, float Trim,
		float WindFromYaw, float WindStrength, float Seconds, float Dt = 0.05f)
	{
		const int32 Steps = FMath::Max(1, FMath::RoundToInt32(Seconds / Dt));
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			Model.Advance(Start, Rudder, Trim, WindFromYaw, WindStrength, Dt);
		}
		return Start;
	}
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavPolarCurveTest, "NavalNav.Sailing.PolarCurve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavPolarCurveTest::RunTest(const FString& Parameters)
{
	const FSailingModelParams P;

	// No-go zone: zero drive dead into the wind and anywhere inside the no-go half-angle.
	TestEqual(TEXT("Head to wind makes no drive"), FSailingModel::PolarThrustFactor(0.0f, P), 0.0f, 1.0e-4f);
	TestEqual(TEXT("Just inside the no-go makes no drive"),
		FSailingModel::PolarThrustFactor(P.NoGoAngleDegrees - 5.0f, P), 0.0f, 1.0e-4f);
	TestEqual(TEXT("The no-go edge is exactly zero"),
		FSailingModel::PolarThrustFactor(P.NoGoAngleDegrees, P), 0.0f, 1.0e-4f);

	// Peak sits at the beam-reach angle and reads as a full 1.
	const float Peak = FSailingModel::PolarThrustFactor(P.BeamReachAngleDegrees, P);
	TestEqual(TEXT("Drive peaks at the beam-reach angle"), Peak, 1.0f, 1.0e-3f);

	// It is symmetric in sign: a mirror-image angle off the other side is identical.
	TestEqual(TEXT("The polar is symmetric about the wind"),
		FSailingModel::PolarThrustFactor(70.0f, P), FSailingModel::PolarThrustFactor(-70.0f, P), 1.0e-4f);

	// A close reach makes less than the beam; a broad reach eases off but still beats close-hauled.
	const float CloseHauled = FSailingModel::PolarThrustFactor(P.NoGoAngleDegrees + 10.0f, P);
	const float BroadReach = FSailingModel::PolarThrustFactor(140.0f, P);
	const float DeadRun = FSailingModel::PolarThrustFactor(180.0f, P);
	TestTrue(TEXT("Close-hauled makes some drive"), CloseHauled > 0.0f);
	TestTrue(TEXT("Close-hauled is slower than the beam-reach peak"), CloseHauled < Peak);
	TestTrue(TEXT("A broad reach is slower than the beam-reach peak"), BroadReach < Peak);
	TestTrue(TEXT("A broad reach still beats close-hauled"), BroadReach > CloseHauled);
	TestEqual(TEXT("Dead downwind matches the configured downwind factor"), DeadRun, P.DownwindFactor, 1.0e-3f);

	// Monotone rise from the no-go edge up to the peak: no dips a helmsman could get stuck in.
	float Previous = -1.0f;
	for (float Angle = P.NoGoAngleDegrees; Angle <= P.BeamReachAngleDegrees; Angle += 2.0f)
	{
		const float Value = FSailingModel::PolarThrustFactor(Angle, P);
		TestTrue(TEXT("Drive rises monotonically toward the beam reach"), Value >= Previous - 1.0e-4f);
		Previous = Value;
	}

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavTerminalSpeedTest, "NavalNav.Sailing.ConvergesToBoundedSpeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavTerminalSpeedTest::RunTest(const FString& Parameters)
{
	const FSailingModel Model = NavalNavShipTest::MakeModel();
	const FSailingModelParams& P = Model.Params;

	// Wind from due south (yaw -90/270); a ship heading due east (yaw 0) is on a beam reach.
	const float WindFromYaw = -90.0f;
	FSailingState Beam;
	Beam.HeadingDegrees = 0.0f;

	// After a good while at full trim in full wind, the ship should sit at its terminal speed.
	const FSailingState Settled = NavalNavShipTest::SailFor(Model, Beam, 0.0f, 1.0f, WindFromYaw, 1.0f, 120.0f);
	const float Terminal = Model.TerminalSpeedForDrive(1.0f);

	TestEqual(TEXT("A beam reach converges to the terminal speed"), Settled.Speed, Terminal, Terminal * 0.02f);
	TestEqual(TEXT("Terminal speed is MaxSpeed on the best point of sail"), Terminal, P.MaxSpeed, 1.0e-2f);
	TestTrue(TEXT("Speed never exceeds MaxSpeed"), Settled.Speed <= P.MaxSpeed * 1.001f);
	TestTrue(TEXT("The ship actually built way"), Settled.Speed > P.MaxSpeed * 0.9f);

	// Half wind converges lower, and to sqrt(0.5) of the full-wind terminal, not half.
	const FSailingState Light = NavalNavShipTest::SailFor(Model, Beam, 0.0f, 1.0f, WindFromYaw, 0.5f, 120.0f);
	TestEqual(TEXT("Half wind converges to MaxSpeed*sqrt(0.5)"), Light.Speed,
		Model.TerminalSpeedForDrive(0.5f), Terminal * 0.03f);
	TestTrue(TEXT("Half wind is slower than full wind"), Light.Speed < Settled.Speed);

	// Head to wind, the ship makes no way however long it sits there.
	FSailingState IntoWind;
	IntoWind.HeadingDegrees = FSailingModel::NormalizeDegrees(WindFromYaw); // bow pointed at the wind's eye
	const FSailingState Stalled = NavalNavShipTest::SailFor(Model, IntoWind, 0.0f, 1.0f, WindFromYaw, 1.0f, 30.0f);
	TestTrue(TEXT("Head to wind the ship stays dead in the water"), Stalled.Speed < 1.0f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavRudderTest, "NavalNav.Sailing.RudderTurnsTheRightWay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavRudderTest::RunTest(const FString& Parameters)
{
	const FSailingModel Model = NavalNavShipTest::MakeModel();
	const float WindFromYaw = -90.0f; // beam reach on heading 0, so the ship keeps way on while turning

	// Give her way on first, then put the helm over one way and check the head swings that way.
	FSailingState Making;
	Making.HeadingDegrees = 0.0f;
	Making = NavalNavShipTest::SailFor(Model, Making, 0.0f, 1.0f, WindFromYaw, 1.0f, 40.0f);
	TestTrue(TEXT("The ship has way on before the turn"), Making.Speed > 100.0f);

	FSailingState Port = Making;
	FSailingState Starboard = Making;
	const float Before = Making.HeadingDegrees;

	// Short burst so the heading does not wrap past +/-180 and confuse the sign check.
	Port = NavalNavShipTest::SailFor(Model, Port, +1.0f, 1.0f, WindFromYaw, 1.0f, 3.0f);
	Starboard = NavalNavShipTest::SailFor(Model, Starboard, -1.0f, 1.0f, WindFromYaw, 1.0f, 3.0f);

	TestTrue(TEXT("Positive rudder swings the head positive"), Port.HeadingDegrees > Before + 1.0f);
	TestTrue(TEXT("Negative rudder swings the head negative"), Starboard.HeadingDegrees < Before - 1.0f);

	// A ship dead in the water cannot turn: no way on, no steerage.
	FSailingState Adrift;
	Adrift.HeadingDegrees = 30.0f;
	Adrift.Speed = 0.0f;
	const FSailingState Tried = NavalNavShipTest::SailFor(Model, Adrift, 1.0f, 0.0f, WindFromYaw, 0.0f, 2.0f);
	TestEqual(TEXT("With no way on and no wind the head does not move"), Tried.HeadingDegrees, 30.0f, 0.5f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavTurnHelpersTest, "NavalNav.Sailing.TurnPredictionHelpers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavTurnHelpersTest::RunTest(const FString& Parameters)
{
	const FSailingModel Model = NavalNavShipTest::MakeModel();

	// A stationary ship has an unbounded turn radius and cannot promise to complete a turn.
	TestTrue(TEXT("Turn radius is effectively infinite at rest"), Model.PredictTurnRadius(0.0f) > 1.0e9f);
	TestTrue(TEXT("Time to turn is effectively infinite at rest"), Model.EstimateTimeToTurn(90.0f, 0.0f) > 1.0e9f);

	// With way on the radius is finite and positive.
	const float Radius = Model.PredictTurnRadius(Model.Params.MaxSpeed);
	TestTrue(TEXT("Turn radius is finite with way on"), Radius > 0.0f && Radius < 1.0e6f);

	// Time to turn scales with the angle and is always positive.
	const float QuarterTurn = Model.EstimateTimeToTurn(90.0f, Model.Params.MaxSpeed);
	const float HalfTurn = Model.EstimateTimeToTurn(180.0f, Model.Params.MaxSpeed);
	TestTrue(TEXT("A quarter turn takes some time"), QuarterTurn > 0.0f);
	TestEqual(TEXT("A half turn takes twice as long as a quarter"), HalfTurn, QuarterTurn * 2.0f, 1.0e-3f);
	TestEqual(TEXT("Time to turn ignores the sign of the angle"),
		Model.EstimateTimeToTurn(-90.0f, Model.Params.MaxSpeed), QuarterTurn, 1.0e-4f);

	return true;
}

//---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNavalNavNoNaNTest, "NavalNav.Sailing.StableUnderRandomInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNavalNavNoNaNTest::RunTest(const FString& Parameters)
{
	const FSailingModel Model = NavalNavShipTest::MakeModel();

	// A deterministic PRNG so a failure reproduces. Hammer the model with garbage: random helm,
	// trim, wind and frame time, for 10k ticks, and assert nothing ever goes non-finite or
	// unbounded. Semi-implicit drag is what should keep the speed sane no matter the frame size.
	FRandomStream Rng(0x5A11);
	FSailingState State;
	State.HeadingDegrees = 10.0f;

	for (int32 Tick = 0; Tick < 10000; ++Tick)
	{
		const float Rudder = Rng.FRandRange(-1.5f, 1.5f);   // deliberately out of range
		const float Trim = Rng.FRandRange(-0.2f, 1.2f);     // deliberately out of range
		const float WindFrom = Rng.FRandRange(-720.0f, 720.0f);
		const float WindStrength = Rng.FRandRange(-0.2f, 1.5f);
		const float Dt = Rng.FRandRange(0.001f, 0.5f);      // includes frames past the internal clamp

		Model.Advance(State, Rudder, Trim, WindFrom, WindStrength, Dt);

		if (!FMath::IsFinite(State.HeadingDegrees) || !FMath::IsFinite(State.Speed)
			|| !FMath::IsFinite(State.RudderAngleDegrees) || !FMath::IsFinite(State.SailTrim))
		{
			AddError(FString::Printf(TEXT("Non-finite state on tick %d"), Tick));
			return false;
		}
	}

	TestTrue(TEXT("Heading stays normalised"),
		State.HeadingDegrees > -180.5f && State.HeadingDegrees <= 180.5f);
	TestTrue(TEXT("Speed stays bounded"), State.Speed >= 0.0f && State.Speed <= Model.Params.MaxSpeed * 1.01f);
	TestTrue(TEXT("Rudder stays within its stops"),
		FMath::Abs(State.RudderAngleDegrees) <= Model.Params.MaxRudderAngleDegrees + 0.5f);
	TestTrue(TEXT("Trim stays in range"), State.SailTrim >= 0.0f && State.SailTrim <= 1.0f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
