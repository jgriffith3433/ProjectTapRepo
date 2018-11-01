// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

#include "ShooterGame.h"
#include "Weapons/ShooterDamageType.h"

UShooterDamageType::UShooterDamageType(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	bKnockPlayerBack = true;
}