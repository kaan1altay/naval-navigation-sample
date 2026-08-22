// Copyright (c) 2026 Kaan Altay. Licensed under the MIT License.
//
// Minimal stand-in for Unreal's CoreMinimal.h.
//
// This exists so the engine-free part of the navigation code (the grid data model, the A*
// pathfinder and the threat evaluator) can be compiled and unit-tested with a plain C++
// compiler on a machine that has no Unreal installation. It is a DEVELOPMENT AID ONLY and is
// never part of the NavalNav module: the shipping code always compiles against the real
// engine headers, and the same test cases also exist as UE automation tests under
// Source/NavalNav/Tests.
//
// Only the surface actually used by those three files is implemented, and each type mirrors
// the semantics of its engine counterpart closely enough that a divergence would show up as a
// compile error rather than as a silently different result.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

// --- Reflection macros: no-ops outside the engine ---------------------------------------
#define USTRUCT(...)
#define UCLASS(...)
#define UENUM(...)
#define UPROPERTY(...)
#define UFUNCTION(...)
#define GENERATED_BODY(...)
#define NAVALNAV_API
#define TEXT(x) x

// --- Scalar types -------------------------------------------------------------------------
using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

inline constexpr int32 INDEX_NONE = -1;
inline constexpr float UE_KINDA_SMALL_NUMBER = 1.0e-4f;
inline constexpr float UE_SMALL_NUMBER = 1.0e-8f;
inline constexpr float UE_PI = 3.1415926535897932f;

enum class EAllowShrinking : uint8 { No, Yes };

/**
 * Stand-in for Unreal's TNumericLimits. Only Max()/Min() are used by the navigation core (the
 * pathfinder seeds its best-cost search with Max()), so only those are mirrored here.
 */
template <typename T>
struct TNumericLimits
{
	static constexpr T Max() { return std::numeric_limits<T>::max(); }
	static constexpr T Min() { return std::numeric_limits<T>::lowest(); }
};

template <typename T>
inline void Swap(T& A, T& B)
{
	std::swap(A, B);
}

// --- Containers ---------------------------------------------------------------------------
template <typename T>
class TArray
{
public:
	TArray() = default;

	int32 Num() const { return static_cast<int32>(Storage.size()); }
	bool IsEmpty() const { return Storage.empty(); }

	T& operator[](int32 Index) { return Storage[static_cast<size_t>(Index)]; }
	const T& operator[](int32 Index) const { return Storage[static_cast<size_t>(Index)]; }

	T& Last(int32 IndexFromEnd = 0) { return Storage[Storage.size() - 1 - static_cast<size_t>(IndexFromEnd)]; }
	const T& Last(int32 IndexFromEnd = 0) const { return Storage[Storage.size() - 1 - static_cast<size_t>(IndexFromEnd)]; }

	int32 Add(const T& Item) { Storage.push_back(Item); return Num() - 1; }
	int32 Emplace(const T& Item) { return Add(Item); }

	int32 AddUnique(const T& Item)
	{
		for (int32 Index = 0; Index < Num(); ++Index)
		{
			if (Storage[static_cast<size_t>(Index)] == Item) { return Index; }
		}
		return Add(Item);
	}

	void Init(const T& Value, int32 Count) { Storage.assign(static_cast<size_t>(Count), Value); }
	void AddZeroed(int32 Count) { Storage.resize(Storage.size() + static_cast<size_t>(Count), T{}); }
	void SetNumUninitialized(int32 Count) { Storage.resize(static_cast<size_t>(Count)); }
	void SetNumZeroed(int32 Count) { Storage.assign(static_cast<size_t>(Count), T{}); }

	/** Engine semantics: keeps the allocation, drops the elements. */
	void Reset() { Storage.clear(); }
	void Reset(int32 ExpectedSize) { Storage.clear(); Storage.reserve(static_cast<size_t>(ExpectedSize)); }
	void Empty() { Storage.clear(); Storage.shrink_to_fit(); }
	void Reserve(int32 Count) { Storage.reserve(static_cast<size_t>(Count)); }

	void Pop(EAllowShrinking = EAllowShrinking::Yes) { Storage.pop_back(); }

	void RemoveAtSwap(int32 Index, EAllowShrinking = EAllowShrinking::Yes)
	{
		Storage[static_cast<size_t>(Index)] = Storage.back();
		Storage.pop_back();
	}

	int32 Remove(const T& Item)
	{
		const size_t Before = Storage.size();
		Storage.erase(std::remove(Storage.begin(), Storage.end(), Item), Storage.end());
		return static_cast<int32>(Before - Storage.size());
	}

	bool Contains(const T& Item) const
	{
		return std::find(Storage.begin(), Storage.end(), Item) != Storage.end();
	}

	auto begin() { return Storage.begin(); }
	auto end() { return Storage.end(); }
	auto begin() const { return Storage.begin(); }
	auto end() const { return Storage.end(); }

private:
	std::vector<T> Storage;
};

template <typename T>
using TFunctionRef = std::function<T>;

template <typename T>
using TFunction = std::function<T>;

// --- Math types ---------------------------------------------------------------------------
struct FIntPoint
{
	int32 X = 0;
	int32 Y = 0;

	FIntPoint() = default;
	FIntPoint(int32 InX, int32 InY) : X(InX), Y(InY) {}

	friend FIntPoint operator-(const FIntPoint& A, const FIntPoint& B) { return FIntPoint(A.X - B.X, A.Y - B.Y); }
	friend FIntPoint operator+(const FIntPoint& A, const FIntPoint& B) { return FIntPoint(A.X + B.X, A.Y + B.Y); }
	friend bool operator==(const FIntPoint& A, const FIntPoint& B) { return A.X == B.X && A.Y == B.Y; }
	friend bool operator!=(const FIntPoint& A, const FIntPoint& B) { return !(A == B); }
};

struct FVector2D
{
	double X = 0.0;
	double Y = 0.0;

	FVector2D() = default;
	FVector2D(double InX, double InY) : X(InX), Y(InY) {}

	static const FVector2D ZeroVector;
};

inline const FVector2D FVector2D::ZeroVector = FVector2D(0.0, 0.0);

struct FVector
{
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;

	FVector() = default;
	FVector(double InX, double InY, double InZ) : X(InX), Y(InY), Z(InZ) {}

	static const FVector ZeroVector;

	friend FVector operator-(const FVector& A, const FVector& B) { return FVector(A.X - B.X, A.Y - B.Y, A.Z - B.Z); }
	friend FVector operator+(const FVector& A, const FVector& B) { return FVector(A.X + B.X, A.Y + B.Y, A.Z + B.Z); }
	friend FVector operator*(const FVector& A, double S) { return FVector(A.X * S, A.Y * S, A.Z * S); }

	bool IsNearlyZero(double Tolerance = 1.0e-4) const
	{
		return std::abs(X) <= Tolerance && std::abs(Y) <= Tolerance && std::abs(Z) <= Tolerance;
	}

	FVector GetSafeNormal(double Tolerance = 1.0e-8) const
	{
		const double SquareSum = X * X + Y * Y + Z * Z;
		if (SquareSum <= Tolerance) { return ZeroVector; }
		const double Scale = 1.0 / std::sqrt(SquareSum);
		return FVector(X * Scale, Y * Scale, Z * Scale);
	}

	static double Dist2D(const FVector& A, const FVector& B)
	{
		const double DX = A.X - B.X;
		const double DY = A.Y - B.Y;
		return std::sqrt(DX * DX + DY * DY);
	}

	static double DistSquared2D(const FVector& A, const FVector& B)
	{
		const double DX = A.X - B.X;
		const double DY = A.Y - B.Y;
		return DX * DX + DY * DY;
	}
};

inline const FVector FVector::ZeroVector = FVector(0.0, 0.0, 0.0);

struct FMath
{
	template <typename T> static T Max(T A, T B) { return A > B ? A : B; }
	template <typename T> static T Min(T A, T B) { return A < B ? A : B; }
	template <typename T> static T Abs(T A) { return A < T(0) ? -A : A; }
	template <typename T> static T Clamp(T Value, T Low, T High) { return Value < Low ? Low : (Value > High ? High : Value); }
	template <typename T> static T Square(T A) { return A * A; }

	static float Sqrt(float A) { return std::sqrt(A); }
	static double Sqrt(double A) { return std::sqrt(A); }
	static float Pow(float A, float B) { return std::pow(A, B); }
	static double Pow(double A, double B) { return std::pow(A, B); }
	static float Sin(float A) { return std::sin(A); }
	static double Sin(double A) { return std::sin(A); }
	static float Cos(float A) { return std::cos(A); }
	static double Cos(double A) { return std::cos(A); }
	static float Tan(float A) { return std::tan(A); }
	static double Tan(double A) { return std::tan(A); }
	static float Atan2(float Y, float X) { return std::atan2(Y, X); }
	static double Atan2(double Y, double X) { return std::atan2(Y, X); }

	static float DegreesToRadians(float A) { return A * (UE_PI / 180.0f); }
	static double DegreesToRadians(double A) { return A * (UE_PI / 180.0); }
	static float RadiansToDegrees(float A) { return A * (180.0f / UE_PI); }
	static double RadiansToDegrees(double A) { return A * (180.0 / UE_PI); }

	static int32 FloorToInt32(float A) { return static_cast<int32>(std::floor(A)); }
	static int32 FloorToInt32(double A) { return static_cast<int32>(std::floor(A)); }
	static int32 CeilToInt32(float A) { return static_cast<int32>(std::ceil(A)); }
	static int32 CeilToInt32(double A) { return static_cast<int32>(std::ceil(A)); }
	static int32 RoundToInt32(float A) { return static_cast<int32>(std::lround(A)); }
	static int32 RoundToInt32(double A) { return static_cast<int32>(std::lround(A)); }

	static bool IsNearlyZero(float A, float Tolerance = UE_KINDA_SMALL_NUMBER) { return std::abs(A) <= Tolerance; }
	static bool IsNearlyEqual(float A, float B, float Tolerance = UE_KINDA_SMALL_NUMBER) { return std::abs(A - B) <= Tolerance; }
};
