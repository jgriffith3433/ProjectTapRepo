// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameSparks/generated/GSResponses.h"

using namespace GameSparks::Api::Types;
/**
 * 
 */
class SHOOTERGAME_API RTSessionPlayerInfo
{
public:
	RTSessionPlayerInfo(Participant& participant);
	RTSessionPlayerInfo(Player& gsPlayer);
	FString DisplayName;
	FString ID;
	int32 PeerID;
	bool bIsOnline;
	~RTPlayer();
};
