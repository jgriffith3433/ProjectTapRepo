// Copyright 1998-2018 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ShooterGameInstance.cpp
=============================================================================*/

#include "ShooterGame.h"
#include "ShooterGameInstance.h"
#include "ShooterMainMenu.h"
#include "ShooterMessageMenu.h"
#include "ShooterGameLoadingScreen.h"
#include "OnlineKeyValuePair.h"
#include "ShooterStyle.h"
#include "ShooterMenuItemWidgetStyle.h"
#include "Player/ShooterPlayerController_Menu.h"
#include "Online/ShooterPlayerState.h"
#include "Online/ShooterGameSession.h"
#include "Online/ShooterOnlineSessionClient.h"
#include "OnlineFriendsInterface.h"
#include "Online.h"
#include "GameSparks/generated/GSMessages.h"
#include "GameSparks/generated/GSRequests.h"
#include "GameSparks/generated/GSResponses.h"
#include "GameSparks/GS.h"
#include "GameSparksModule.h"
#include <GameSparksRT/RTData.hpp>
#include <cassert>
#include "cjson/cJSON.h"

FAutoConsoleVariable CVarShooterGameTestEncryption(TEXT("ShooterGame.TestEncryption"), 0, TEXT("If true, clients will send an encryption token with their request to join the server and attempt to encrypt the connection using a debug key. This is NOT SECURE and for demonstration purposes only."));

void SShooterWaitDialog::Construct(const FArguments& InArgs)
{
	const FShooterMenuItemStyle* ItemStyle = &FShooterStyle::Get().GetWidgetStyle<FShooterMenuItemStyle>("DefaultShooterMenuItemStyle");
	const FButtonStyle* ButtonStyle = &FShooterStyle::Get().GetWidgetStyle<FButtonStyle>("DefaultShooterButtonStyle");
	ChildSlot
		.VAlign(VAlign_Center)
		.HAlign(HAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(20.0f)
			.VAlign(VAlign_Center)
			.HAlign(HAlign_Center)
			[
				SNew(SBorder)
				.Padding(50.0f)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				.BorderImage(&ItemStyle->BackgroundBrush)
				.BorderBackgroundColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f))
				[
					SNew(STextBlock)
					.TextStyle(FShooterStyle::Get(), "ShooterGame.MenuHeaderTextStyle")
					.ColorAndOpacity(this, &SShooterWaitDialog::GetTextColor)
					.Text(InArgs._MessageText)
					.WrapTextAt(500.0f)
				]
			]
		];

	//Setup a curve
	const float StartDelay = 0.0f;
	const float SecondDelay = 0.0f;
	const float AnimDuration = 2.0f;

	WidgetAnimation = FCurveSequence();
	TextColorCurve = WidgetAnimation.AddCurve(StartDelay + SecondDelay, AnimDuration, ECurveEaseFunction::QuadInOut);
	WidgetAnimation.Play(this->AsShared(), true);
}

FSlateColor SShooterWaitDialog::GetTextColor() const
{
	//instead of going from black -> white, go from white -> grey.
	float fAlpha = 1.0f - TextColorCurve.GetLerp();
	fAlpha = fAlpha * 0.5f + 0.5f;
	return FLinearColor(FColor(155, 164, 182, FMath::Clamp((int32)(fAlpha * 255.0f), 0, 255)));
}

using namespace GameSparks::Core;
using namespace GameSparks::Api::Requests;
using namespace GameSparks::Api::Responses;
using namespace GameSparks::Api::Types;
using namespace GameSparks::Api::Messages;

namespace ShooterGameInstanceState
{
	const FName None = FName(TEXT("None"));
	const FName LoginScreen = FName(TEXT("Login"));
	const FName PendingInvite = FName(TEXT("PendingInvite"));
	const FName MainMenu = FName(TEXT("MainMenu"));
	const FName MessageMenu = FName(TEXT("MessageMenu"));
	const FName Playing = FName(TEXT("Playing"));
}

UShooterGameInstance::UShooterGameInstance(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, OnlineMode(EOnlineMode::Online) // Default to online
	, bIsLicensed(true) // Default to licensed (should have been checked by OS on boot)
{
	CurrentState = ShooterGameInstanceState::None;
}

void UShooterGameInstance::Init() 
{
	Super::Init();

	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr const identity = OnlineSub->GetIdentityInterface();
	assert(identity);

	OnConnectionStatusChangedDelegate = FOnConnectionStatusChangedDelegate::CreateUObject(this, &UShooterGameInstance::OnConnectionStatusChanged);
	OnLoginCompleteDelegate = FOnLoginCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnLoginComplete);
	OnLogoutCompleteDelegate = FOnLogoutCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnLogoutComplete);

	OnConnectionStatusChangedDelegateHandle = OnlineSub->AddOnConnectionStatusChangedDelegate_Handle(OnConnectionStatusChangedDelegate);
	
	IgnorePairingChangeForControllerId = -1;

	LocalPlayerOnlineStatus.InsertDefaulted(0, MAX_LOCAL_PLAYERS);

	for (int i = 0; i < MAX_LOCAL_PLAYERS; ++i)
	{
		identity->AddOnLoginStatusChangedDelegate_Handle(i, FOnLoginStatusChangedDelegate::CreateUObject(this, &UShooterGameInstance::HandleUserLoginChanged));
	}

	identity->AddOnControllerPairingChangedDelegate_Handle(FOnControllerPairingChangedDelegate::CreateUObject(this, &UShooterGameInstance::HandleControllerPairingChanged));

	FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this, &UShooterGameInstance::HandleAppWillDeactivate);

	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UShooterGameInstance::HandleAppSuspend);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UShooterGameInstance::HandleAppResume);

	FCoreDelegates::OnSafeFrameChangedEvent.AddUObject(this, &UShooterGameInstance::HandleSafeFrameChanged);
	FCoreDelegates::OnControllerConnectionChange.AddUObject(this, &UShooterGameInstance::HandleControllerConnectionChange);
	FCoreDelegates::ApplicationLicenseChange.AddUObject(this, &UShooterGameInstance::HandleAppLicenseUpdate);

	FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &UShooterGameInstance::OnPreLoadMap);
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UShooterGameInstance::OnPostLoadMap);

	FCoreUObjectDelegates::PostDemoPlay.AddUObject(this, &UShooterGameInstance::OnPostDemoPlay);

	bPendingEnableSplitscreen = false;

	OnEndSessionCompleteDelegate = FOnEndSessionCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnEndSessionComplete);

	// Register delegate for ticker callback
	TickDelegate = FTickerDelegate::CreateUObject(this, &UShooterGameInstance::Tick);
	TickDelegateHandle = FTicker::GetCoreTicker().AddTicker(TickDelegate);

	// Initialize the debug key with a set value for AES256. This is not secure and for example purposes only.
	DebugTestEncryptionKey.SetNum(32);

	for (int32 i = 0; i < DebugTestEncryptionKey.Num(); ++i)
	{
		DebugTestEncryptionKey[i] = uint8(i);
	}
}

void UShooterGameInstance::OnConnectionStatusChanged(const FString& ServiceName, EOnlineServerConnectionStatus::Type LastConnectionState, EOnlineServerConnectionStatus::Type ConnectionState)
{
	CurrentConnectionStatus = ConnectionState;

	UE_LOG(LogOnlineGame, Log, TEXT("UShooterGameInstance::OnConnectionStatusChanged: %s"), EOnlineServerConnectionStatus::ToString(CurrentConnectionStatus));

#if SHOOTER_CONSOLE_UI
	// If we are disconnected from server, and not currently at (or heading to) the login screen
	// then display a message on consoles
	if (OnlineMode != EOnlineMode::Offline && PendingState != ShooterGameInstanceState::LoginScreen && CurrentState != ShooterGameInstanceState::LoginScreen && CurrentConnectionStatus != EOnlineServerConnectionStatus::Connected)
	{
		UE_LOG(LogOnlineGame, Log, TEXT("UShooterGameInstance::OnConnectionStatusChanged: Going to main menu"));

		// Display message on consoles
#if PLATFORM_XBOXONE
		const FText ReturnReason = NSLOCTEXT("NetworkFailures", "ServiceUnavailable", "Connection to Xbox LIVE has been lost.");
#elif PLATFORM_PS4
		const FText ReturnReason = NSLOCTEXT("NetworkFailures", "ServiceUnavailable", "Connection to \"PSN\" has been lost.");
#else
		const FText ReturnReason = NSLOCTEXT("NetworkFailures", "ServiceUnavailable", "Connection has been lost.");
#endif
		const FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");

		ShowMessageThenGotoState(ReturnReason, OKButton, FText::GetEmpty(), ShooterGameInstanceState::MainMenu);
	}

#endif
}

FReply UShooterGameInstance::OnConfirmGeneric()
{
	return FReply::Handled();
}

FReply UShooterGameInstance::OnContinueWithoutSavingConfirm()
{
	SetControllerAndAdvanceToMainMenu(0);
	return FReply::Handled();
}

void UShooterGameInstance::SetControllerAndAdvanceToMainMenu(const int ControllerIndex)
{
	ULocalPlayer * NewPlayerOwner = GetFirstGamePlayer();

	if (NewPlayerOwner != nullptr && ControllerIndex != -1)
	{
		NewPlayerOwner->SetControllerId(ControllerIndex);
		NewPlayerOwner->SetCachedUniqueNetId(NewPlayerOwner->GetUniqueNetIdFromCachedControllerId().GetUniqueNetId());

		GotoState(ShooterGameInstanceState::MainMenu);
	}
}

#pragma optimize( "", off )
void UShooterGameInstance::FindDeathmatches()
{
	GS& gs = UGameSparksModule::GetModulePtr()->GetGSInstance();
	
}

void UShooterGameInstance::HostQuickDeathmatch()
{
	AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (MenuPC)
	{
		GS& gs = UGameSparksModule::GetModulePtr()->GetGSInstance();
		gs.SetMessageListener<MatchFoundMessage>([&](GS& gs, const MatchFoundMessage& response) {
			UE_LOG(LogOnline, Log, TEXT("GSM| Match found! Fetching match details..."));
			MatchDetailsRequest request(gs);
			request.SetMatchId(response.GetMatchId().GetValue());
			request.Send(([&](GS& gs, const MatchDetailsResponse& matchDetailsResponse) {
				if (matchDetailsResponse.GetHasErrors())
				{
					UE_LOG(LogOnline, Log, TEXT("GSM| Match details not found."));
				}
				else
				{
					UE_LOG(LogOnline, Log, TEXT("GSM| Got match details!"));

					std::vector<std::string> PlayerIdsToChallenge;
					for (std::size_t i = 0; i < matchDetailsResponse.GetOpponents().size(); i++)
					{
						PlayerIdsToChallenge.push_back(matchDetailsResponse.GetOpponents()[i].GetId().GetValueOrDefault(""));
					}

					CreateChallengeRequest request(gs);
					request.SetAccessType("PRIVATE");
					//request.SetChallengeMessage("Bozo's challenge");
					request.SetChallengeShortCode("DEATHMATCH_CHALLENGE");
					request.SetMaxPlayers(14);
					request.SetEndTime(GSDateTime::Now().AddMinutes(30));
					request.SetExpiryTime(GSDateTime::Now().AddMinutes(2));
					request.SetUsersToChallenge(PlayerIdsToChallenge);
					request.Send([&](GS& gsInstance, const CreateChallengeResponse& response) {
						if (response.GetHasErrors())
						{
							FString JSONString = FString(UTF8_TO_TCHAR(response.GetErrors().GetValue().GetJSON().c_str()));
							UE_LOG(LogTemp, Warning, TEXT("GSM| Error in creating challenge: %s"), *JSONString);
						}
						else
						{
							UE_LOG(LogOnline, Log, TEXT("GSM| Created challenge!"));
							CurrentQuickDeathMatch->ChallengeInstanceId = FString(UTF8_TO_TCHAR(response.GetChallengeInstanceId().GetValueOrDefault("").c_str()));
							SessionInfo = MakeShareable(new RTSessionInfo(matchDetailsResponse));
						}
					}, 60);
				}
			}));
		});

		gs.SetMessageListener<MatchNotFoundMessage>([&](GS& gs, const MatchNotFoundMessage& response) {
			UE_LOG(LogOnline, Log, TEXT("GSM| Match not found..."));
		});

		gs.SetMessageListener<MatchUpdatedMessage>([&](GS& gs, const MatchUpdatedMessage& response) {
			UE_LOG(LogOnline, Log, TEXT("GSM| Match updated..."));
		});

		UE_LOG(LogOnline, Log, TEXT("GSM| Attempting Matchmaking..."));

		if (CurrentQuickDeathMatch == NULL)
		{
			CurrentQuickDeathMatch = NewObject<URTMatch>();
		}
		CurrentQuickDeathMatch->HostPlayerId = MenuPC->UserProfile->PlayerId;

		MatchmakingRequest request(gs);
		request.SetMatchShortCode("DEATHMATCH");
		request.SetSkill(0);
		cJSON* matchData = cJSON_CreateObject();
		if (matchData)
		{
			cJSON_AddItemToObject(matchData, "hostPlayerId", cJSON_CreateString(TCHAR_TO_UTF8(*CurrentQuickDeathMatch->HostPlayerId)));
			request.SetMatchData(GSRequestData(matchData));
		}

		request.Send([&](GS& gsInstance, const MatchmakingResponse& response) {
			if (response.GetHasErrors())
			{
				FString JSONString = FString(UTF8_TO_TCHAR(response.GetErrors().GetValue().GetJSON().c_str()));
				UE_LOG(LogTemp, Warning, TEXT("%s"), *JSONString);
			}
		}, 60);
	}
}

void UShooterGameInstance::JoinQuickDeathmatch()
{
	GS& gs = UGameSparksModule::GetModulePtr()->GetGSInstance();
	gs.SetMessageListener<MatchFoundMessage>([&](GS& gs, const MatchFoundMessage& response) {
		UE_LOG(LogOnline, Log, TEXT("GSM| Joined match found!"));
		if (CurrentQuickDeathMatch == NULL)
		{
			CurrentQuickDeathMatch = NewObject<URTMatch>();
		}
		if (response.GetMatchData().HasValue())
		{
			cJSON* json = cJSON_Parse(response.GetMatchData().GetValue().GetJSON().c_str());

			if (json == NULL)
			{
				const char *error_ptr = cJSON_GetErrorPtr();
				if (error_ptr != NULL)
				{
					UE_LOG(LogTemp, Warning, TEXT("Error before: %s\n"), error_ptr);
				}
				return;
			}

			cJSON* hostPlayerIdJson = cJSON_GetObjectItem(json, "hostPlayerId");
			if (hostPlayerIdJson && hostPlayerIdJson->valuestring != NULL)
			{
				CurrentQuickDeathMatch->HostPlayerId = FString(UTF8_TO_TCHAR(hostPlayerIdJson->valuestring));
			}
			cJSON_Delete(json);
		}
	});

	gs.SetMessageListener<MatchNotFoundMessage>([&](GS& gs, const MatchNotFoundMessage& response) {
		UE_LOG(LogOnline, Log, TEXT("GSM| Joined match not found..."));
	});

	gs.SetMessageListener<MatchUpdatedMessage>([&](GS& gs, const MatchUpdatedMessage& response) {
		UE_LOG(LogOnline, Log, TEXT("GSM| Joined match updated..."));
	});

	gs.SetMessageListener<ChallengeIssuedMessage>([&](GS& gs, const ChallengeIssuedMessage& response) {
		UE_LOG(LogOnline, Log, TEXT("GSM| Got issued challenge!"));
		if (CurrentQuickDeathMatch == NULL)
		{
			CurrentQuickDeathMatch = NewObject<URTMatch>();
		}
		CurrentQuickDeathMatch->ChallengeInstanceId = FString(UTF8_TO_TCHAR(response.GetChallenge().GetChallengeId().GetValueOrDefault("").c_str()));

		JoinChallengeRequest request(gs);
		request.SetChallengeInstanceId(response.GetChallenge().GetChallengeId().GetValueOrDefault(""));
		request.Send([&](GS& gsInstance, const JoinChallengeResponse& response) {
			if (response.GetHasErrors())
			{
				FString JSONString = FString(UTF8_TO_TCHAR(response.GetErrors().GetValue().GetJSON().c_str()));
				UE_LOG(LogTemp, Warning, TEXT("GSM| Error in joining challenge: %s"), *JSONString);
			}
			else
			{
				bool joined = response.GetJoined().GetValueOrDefault(false);
				if (joined)
				{
					UE_LOG(LogOnline, Log, TEXT("GSM| Joined challenge!"));
				}
				else
				{
					UE_LOG(LogOnline, Log, TEXT("GSM| Could not join challenge."));
				}
			}
		});
	});

	UE_LOG(LogOnline, Log, TEXT("GSM| Attempting Matchmaking..."));
	MatchmakingRequest request(gs);
	request.SetMatchShortCode("DEATHMATCH");
	request.SetSkill(0);
	request.Send([&](GS& gsInstance, const MatchmakingResponse& response) {
		if (response.GetHasErrors())
		{
			FString JSONString = FString(UTF8_TO_TCHAR(response.GetErrors().GetValue().GetJSON().c_str()));
			UE_LOG(LogTemp, Warning, TEXT("%s"), *JSONString);
		}
	}, 60);
}

void UShooterGameInstance::StartMatch()
{
	if (CurrentQuickDeathMatch->HostPlayerId == MenuPC->UserProfile->PlayerId)
	{
		CreateNewRTSession();
	}
}

void UShooterGameInstance::Login(int32 LocalUserNum, const FString& UserName, const FString& Password)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);
	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(identity);

	OnLoginCompleteDelegateHandle = identity->AddOnLoginCompleteDelegate_Handle(LocalUserNum, OnLoginCompleteDelegate);

	FOnlineAccountCredentials AccountCredentials;
	AccountCredentials.Id = UserName;
	AccountCredentials.Token = Password;
	AccountCredentials.Type = "GSCredentials";
	identity->Login(0, AccountCredentials);
}

void UShooterGameInstance::OnLoginComplete(int32 LocalUserNum, bool bWasSuccessful, const FUniqueNetId& UserId, const FString& Error)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(identity);

	GS& gs = UGameSparksModule::GetModulePtr()->GetGSInstance();
	assert(gs);

	identity->ClearOnLoginCompleteDelegate_Handle(LocalUserNum, OnLoginCompleteDelegateHandle);

	if (bWasSuccessful)
	{
		TSharedPtr<const FUniqueNetId> playerId = identity->GetUniquePlayerId(LocalUserNum);
		TSharedPtr<FUserOnlineAccount> user = identity->GetUserAccount(*playerId);
		AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
		if (MenuPC)
		{
			MenuPC->UserProfile = NewObject<UUserProfile>();
			MenuPC->UserProfile->DisplayName = user->GetDisplayName();
			MenuPC->UserProfile->PlayerId = user->GetUserId().Get().ToString();
			MenuPC->AvailableChannels.Empty();
			MenuPC->AvailableChannels.Add(FString("General"));
			MenuPC->ShowMainMenu();
		}

		/*gs.SetMessageListener<ScriptMessage>([&](GS& gs, const ScriptMessage& message) {
			UE_LOG(LogOnline, Log, TEXT("GSM| Got script message!"));
		});*/

		ULocalPlayer * NewPlayerOwner = GetFirstGamePlayer();
		if (NewPlayerOwner != nullptr)
		{
			// If they don't currently have a license, let them know, but don't let them proceed
			if (!bIsLicensed)
			{
				return;
			}

			if (playerId.IsValid())
			{
				//IOnlineIdentity::FOnGetUserPrivilegeCompleteDelegate::CreateSP(this, &UShooterGameInstance::OnUserCanPlay)
				//identity->GetUserPrivilege(*playerId, EUserPrivileges::CanPlay, );
				StartOnlinePrivilegeTask(IOnlineIdentity::FOnGetUserPrivilegeCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnUserCanPlay), EUserPrivileges::CanPlay, playerId);
			}
			else
			{
				// Todo: Change this to not let you continue without signing in
				return;
			}
		}
		auto friendInterface = OnlineSub->GetFriendsInterface();

		FOnReadFriendsListComplete OnReadFriendsListComplete;
		OnReadFriendsListComplete.BindLambda([=](int32, bool, const FString&, const FString& ErrorStr) {
			TArray<TSharedRef<FOnlineFriend> > friends;

			if (!friendInterface->GetFriendsList(LocalUserNum, TEXT(""), friends))
			{
				GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("GetFriendsList failed"));
			}
			else
			{
				GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Yellow, TEXT("Your Friends:"));

				for (int i = 0; i != friends.Num(); ++i)
				{
					GEngine->AddOnScreenDebugMessage(-1, 5.f + 20 * i, FColor::Yellow, friends[i]->GetDisplayName());
				}
			}
		});

		if (!friendInterface->ReadFriendsList(LocalUserNum, "", OnReadFriendsListComplete))
		{
			GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("ReadFriendsList failed"));
		}
	}
	else
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("log-in failed with error: ") + Error);
	}
}

void UShooterGameInstance::OnChallengeInstanceStart(std::string ChallengeInstanceId)
{
	GS& gs = UGameSparksModule::GetModulePtr()->GetGSInstance();

	gs.SetMessageListener<ScriptMessage>([&](GS& gs, const ScriptMessage& message) {
		UE_LOG(LogOnline, Log, TEXT("GSM| Got script message!"));
		if (message.GetData().HasValue())
		{
			const FString eventType = FString(UTF8_TO_TCHAR(message.GetData().GetValue().GetString("eventType").GetValueOrDefault("").c_str()));
			if (eventType == "event_chat")
			{
				const FString fromDisplayName = FString(UTF8_TO_TCHAR(message.GetData().GetValue().GetString("fromDisplayName").GetValueOrDefault("").c_str()));
				const FString fromPlayerId = FString(UTF8_TO_TCHAR(message.GetData().GetValue().GetString("fromPlayerId").GetValueOrDefault("").c_str()));
				const FString chatMessage = FString(UTF8_TO_TCHAR(message.GetData().GetValue().GetString("chatMessage").GetValueOrDefault("").c_str()));
				const FString channel = FString(UTF8_TO_TCHAR(message.GetData().GetValue().GetString("channel").GetValueOrDefault("").c_str()));



			}
		}
	});
}

void UShooterGameInstance::OnUserCanPlay(const FUniqueNetId& UserId, EUserPrivileges::Type Privilege, uint32 PrivilegeResults)
{
	CleanupOnlinePrivilegeTask();

	if (PrivilegeResults == (uint32)IOnlineIdentity::EPrivilegeResults::NoFailures)
	{
		SetControllerAndAdvanceToMainMenu(0);
	}
	else
	{
		// Cannot play due to age restrictions
	}
}

void UShooterGameInstance::CreateNewRTSession()
{
	RTListener = MakeShareable(new RTSessionListener(this));
	RTSession = MakeShareable(GameSparksRT::SessionBuilder()
		.SetConnectToken(TCHAR_TO_UTF8(*SessionInfo->AccessToken))
		.SetHost(TCHAR_TO_UTF8(*SessionInfo->HostURL))
		.SetPort(TCHAR_TO_UTF8(*SessionInfo->PortID))
		.SetListener(RTListener.Get())
		.Build());

	RTSession->Start();
	AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (MenuPC)
	{
		MenuPC->PeerId = SessionInfo->PeerId;
		MenuPC->PlayerId = SessionInfo->PlayerId;
	}
}

void UShooterGameInstance::OnJoinRTSession(const FString& MapPath)
{
	if ((PendingState == CurrentState) || (PendingState == ShooterGameInstanceState::None))
	{
		// Go ahead and go into loading state now
		// If we fail, the delegate will handle showing the proper messaging and move to the correct state
		ShowLoadingScreen();
		GotoState(ShooterGameInstanceState::Playing);
		APlayerController * const PlayerController = GetFirstLocalPlayerController();

		if (PlayerController == nullptr)
		{
			FText ReturnReason = NSLOCTEXT("NetworkErrors", "InvalidPlayerController", "Invalid Player Controller");
			FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");
			RemoveNetworkFailureHandlers();
			ShowMessageThenGoMain(ReturnReason, OKButton, FText::GetEmpty());
			return;
		}

		//LoadFrontEndMap(MapPath);
		PlayerController->ClientTravelInternal(MapPath, TRAVEL_Absolute);
	}
}

void UShooterGameInstance::OnChatMessageReceived(FString Channel, FString FromDisplayName, FString FromPlayerId, FString MessageText)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(OnlineSub);

	AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (MenuPC)
	{
		MenuPC->OnChatMessageReceived(Channel, FromPlayerId, FromDisplayName, MessageText);
	}
}

void UShooterGameInstance::SendFriendChatMessage(std::string FriendPlayerId, std::string MessageText)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(OnlineSub);


}

void UShooterGameInstance::SendChallengeChatMessage(std::string MessageText)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(OnlineSub);

	ChatOnChallengeRequest request(UGameSparksModule::GetModulePtr()->GetGSInstance());
	request.SetChallengeInstanceId(TCHAR_TO_UTF8(*CurrentQuickDeathMatch->ChallengeInstanceId));
	request.SetMessage(MessageText);
	request.Send([&](GS& gsInstance, const ChatOnChallengeResponse& response) {
		if (response.GetHasErrors())
		{
			FString JSONString = FString(UTF8_TO_TCHAR(response.GetErrors().GetValue().GetJSON().c_str()));
			UE_LOG(LogTemp, Warning, TEXT("GSM| Error sending challenge chat message: %s"), *JSONString);
		}
	});
	
	/*LogEventRequest request(UGameSparksModule::GetModulePtr()->GetGSInstance());
	request.SetEventKey("CHAT");
	request.SetEventAttribute("Channel", Channel);
	request.SetEventAttribute("Message", MessageText);
	request.Send([&](GS& gsInstance, const LogEventResponse& response) {
		if (response.GetHasErrors())
		{
			UE_LOG(LogOnline, Log, TEXT("GSM| Error sending chat."));
		}
	});*/
}

void UShooterGameInstance::SendTeamChatMessage(std::string MessageText)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(OnlineSub);

	
}

/*RTData data;
data.SetString(1, MessageText);
if (RTSession->SendRTData(1, GameSparksRT::DeliveryIntent::RELIABLE, data, PeerIds))
{
	UE_LOG(LogOnlineGame, Warning, TEXT("Sent message: %s"), UTF8_TO_TCHAR(MessageText.c_str()));
}*/
void UShooterGameInstance::OnPacket(const RTPacket& packet)
{
	AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (MenuPC)
	{
		switch (packet.OpCode)
		{
		case 1:
			/*for (auto it = SessionInfo->PlayerList.CreateIterator(); it; ++it)
			{
				RTPlayer rtPlayer = *it;
				if (rtPlayer.PeerID == packet.Sender)
				{
					MenuPC->OnChatMessageReceived(rtPlayer.PeerID, rtPlayer.DisplayName, FString(UTF8_TO_TCHAR(packet.Data.GetString(1).GetValueOrDefault("").c_str())));
					break;
				}
			}*/
			break;
		default:
			break;
		}
	}
}

void UShooterGameInstance::OnPlayerConnect(int PeerId)
{
	TSharedPtr<RTPlayer> player = GetRTPlayerFromPeerId(PeerId);
	if (player)
	{
		OnChatMessageReceived("System", "System", player->DisplayName, "has connected");
	}
}

void UShooterGameInstance::OnPlayerDisconnect(int PeerId)
{
	TSharedPtr<RTPlayer> player = GetRTPlayerFromPeerId(PeerId);
	if (player)
	{
		OnChatMessageReceived("System", "System", player->DisplayName, "has disconnected");
	}
}

TSharedPtr<RTPlayer> UShooterGameInstance::GetRTPlayerFromPeerId(int PeerId)
{
	if (SessionInfo.IsValid())
	{
		for (auto it = SessionInfo->PlayerList.CreateIterator(); it; ++it)
		{
			TSharedPtr<RTPlayer> rtPlayer = *it;
			if (rtPlayer->PeerID == PeerId)
			{
				return rtPlayer;
			}
		}
	}
	return NULL;
}

void UShooterGameInstance::Logout(int32 LocalUserNum)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(identity);

	OnLogoutCompleteDelegateHandle = identity->AddOnLogoutCompleteDelegate_Handle(LocalUserNum, OnLogoutCompleteDelegate);
	identity->Logout(LocalUserNum);
}

void UShooterGameInstance::OnLogoutComplete(int32 LocalUserNum, bool bWasSuccessful)
{
	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);

	IOnlineIdentityPtr identity = OnlineSub->GetIdentityInterface();
	assert(identity);

	identity->ClearOnLogoutCompleteDelegate_Handle(LocalUserNum, OnLogoutCompleteDelegateHandle);

	if (bWasSuccessful)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, FString::Printf(TEXT("Logged out user %d"), LocalUserNum));
		AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
		if (MenuPC)
		{
			MenuPC->ShowLoginScreen();
		}
	}
	else
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, TEXT("log-out failed"));
	}
}

void UShooterGameInstance::Shutdown()
{
	if (RTSession.IsValid())
	{
		RTSession->Stop();
		RTSession.Reset();
	}
	//UGameSparksModule::GetModulePtr()->GetGSInstance().Disconnect();
	
	Super::Shutdown();

	IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	assert(OnlineSub);
	
	OnlineSub->ClearOnConnectionStatusChangedDelegate_Handle(OnConnectionStatusChangedDelegateHandle);

	// Unregister ticker delegate
	FTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);
}
#pragma optimize( "", on )

void UShooterGameInstance::HandleSessionFailure( const FUniqueNetId& NetId, ESessionFailure::Type FailureType )
{
	UE_LOG( LogOnlineGame, Warning, TEXT( "UShooterGameInstance::HandleSessionFailure: %u" ), (uint32)FailureType );

#if SHOOTER_CONSOLE_UI
	// If we are not currently at (or heading to) the login screen then display a message on consoles
	if (	OnlineMode != EOnlineMode::Offline &&
			PendingState != ShooterGameInstanceState::LoginScreen &&
			CurrentState != ShooterGameInstanceState::LoginScreen )
	{
		UE_LOG( LogOnlineGame, Log, TEXT( "UShooterGameInstance::HandleSessionFailure: Going to main menu" ) );

		// Display message on consoles
#if PLATFORM_XBOXONE
		const FText ReturnReason	= NSLOCTEXT( "NetworkFailures", "ServiceUnavailable", "Connection to Xbox LIVE has been lost." );
#elif PLATFORM_PS4
		const FText ReturnReason	= NSLOCTEXT( "NetworkFailures", "ServiceUnavailable", "Connection to PSN has been lost." );
#else
		const FText ReturnReason	= NSLOCTEXT( "NetworkFailures", "ServiceUnavailable", "Connection has been lost." );
#endif
		const FText OKButton		= NSLOCTEXT( "DialogButtons", "OKAY", "OK" );
		
		ShowMessageThenGotoState( ReturnReason, OKButton,  FText::GetEmpty(), ShooterGameInstanceState::MainMenu );
	}
#endif
}

void UShooterGameInstance::OnPreLoadMap(const FString& MapName)
{
	if (bPendingEnableSplitscreen)
	{
		// Allow splitscreen
		UGameViewportClient* GameViewportClient = GetGameViewportClient();
		if (GameViewportClient != nullptr)
		{
			GameViewportClient->SetDisableSplitscreenOverride(false);

			bPendingEnableSplitscreen = false;
		}
	}
}

void UShooterGameInstance::OnPostLoadMap(UWorld*)
{
	// Make sure we hide the loading screen when the level is done loading
	HideLoadingScreen();
}

void UShooterGameInstance::OnUserCanPlayInvite(const FUniqueNetId& UserId, EUserPrivileges::Type Privilege, uint32 PrivilegeResults)
{
	CleanupOnlinePrivilegeTask();

	if (PrivilegeResults == (uint32)IOnlineIdentity::EPrivilegeResults::NoFailures)	
	{
		if (UserId == *PendingInvite.UserId)
		{
			PendingInvite.bPrivilegesCheckedAndAllowed = true;
		}		
	}
	else
	{
		DisplayOnlinePrivilegeFailureDialogs(UserId, Privilege, PrivilegeResults);
		GotoState(ShooterGameInstanceState::LoginScreen);
	}
}

void UShooterGameInstance::OnUserCanPlayTogether(const FUniqueNetId& UserId, EUserPrivileges::Type Privilege, uint32 PrivilegeResults)
{
	CleanupOnlinePrivilegeTask();

	if (PrivilegeResults == (uint32)IOnlineIdentity::EPrivilegeResults::NoFailures)
	{
		GotoState(ShooterGameInstanceState::MainMenu);
	}
	else
	{
		DisplayOnlinePrivilegeFailureDialogs(UserId, Privilege, PrivilegeResults);
		GotoState(ShooterGameInstanceState::LoginScreen);
	}
}

void UShooterGameInstance::OnPostDemoPlay()
{
	GotoState( ShooterGameInstanceState::Playing );
}

void UShooterGameInstance::HandleDemoPlaybackFailure( EDemoPlayFailure::Type FailureType, const FString& ErrorString )
{
	if (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::PIE)
	{
		UE_LOG(LogEngine, Warning, TEXT("Demo failed to play back correctly, got error %s"), *ErrorString);
		return;
	}

	ShowMessageThenGotoState(FText::Format(NSLOCTEXT("UShooterGameInstance", "DemoPlaybackFailedFmt", "Demo playback failed: {0}"), FText::FromString(ErrorString)), NSLOCTEXT("DialogButtons", "OKAY", "OK"), FText::GetEmpty(), ShooterGameInstanceState::MainMenu);
}

#if WITH_EDITOR
FGameInstancePIEResult UShooterGameInstance::StartPlayInEditorGameInstance(ULocalPlayer* LocalPlayer, const FGameInstancePIEParameters& Params)
{
	UWorld* const World = GetWorld();
	if (World)
	{
		FString CurrentMapName = World->PersistentLevel->GetOutermost()->GetName();
		if (CurrentMapName.Find(TEXT("Entry")) != -1)
		{
			GotoInitialState();
		}
	}

	return Super::StartPlayInEditorGameInstance(LocalPlayer, Params);
}
#endif

void UShooterGameInstance::StartGameInstance()
{
#if PLATFORM_PS4 == 0
	TCHAR Parm[4096] = TEXT("");

	const TCHAR* Cmd = FCommandLine::Get();

	// Catch the case where we want to override the map name on startup (used for connecting to other MP instances)
	if (FParse::Token(Cmd, Parm, ARRAY_COUNT(Parm), 0) && Parm[0] != '-')
	{
		// if we're 'overriding' with the default map anyway, don't set a bogus 'playing' state.
		if (!MainMenuMap.Contains(Parm))
		{
			FURL DefaultURL;
			DefaultURL.LoadURLConfig(TEXT("DefaultPlayer"), GGameIni);

			FURL URL(&DefaultURL, Parm, TRAVEL_Partial);

			if (URL.Valid)
			{
				UEngine* const Engine = GetEngine();

				FString Error;

				const EBrowseReturnVal::Type BrowseRet = Engine->Browse(*WorldContext, URL, Error);

				if (BrowseRet == EBrowseReturnVal::Success)
				{
					// Success, we loaded the map, go directly to playing state
					GotoState(ShooterGameInstanceState::Playing);
					return;
				}
				else if (BrowseRet == EBrowseReturnVal::Pending)
				{
					// Assume network connection
					LoadFrontEndMap(MainMenuMap);
					AddNetworkFailureHandlers();
					ShowLoadingScreen();
					GotoState(ShooterGameInstanceState::Playing);
					return;
				}
			}
		}
	}
#endif

	GotoInitialState();
}

FName UShooterGameInstance::GetInitialState()
{
	auto Identity = Online::GetIdentityInterface();
	if (Identity.IsValid() && Identity->GetLoginStatus(0) == ELoginStatus::LoggedIn)
	{
		return ShooterGameInstanceState::MainMenu;
	}
	return ShooterGameInstanceState::LoginScreen;
//#if SHOOTER_CONSOLE_UI	
	// Start in the login screen state on consoles
	//return ShooterGameInstanceState::LoginScreen;
//#else
	// On PC, go directly to the main menu
	//return ShooterGameInstanceState::MainMenu;
//#endif
}

void UShooterGameInstance::GotoInitialState()
{
	GotoState(GetInitialState());
}

void UShooterGameInstance::ShowMessageThenGotoState( const FText& Message, const FText& OKButtonString, const FText& CancelButtonString, const FName& NewState, const bool OverrideExisting, TWeakObjectPtr< ULocalPlayer > PlayerOwner )
{
	UE_LOG( LogOnline, Log, TEXT( "ShowMessageThenGotoState: Message: %s, NewState: %s" ), *Message.ToString(), *NewState.ToString() );
	
	const bool bAtLoginScreen = PendingState == ShooterGameInstanceState::LoginScreen || CurrentState == ShooterGameInstanceState::LoginScreen;

	// Never override the login
	if (bAtLoginScreen)
	{
		UE_LOG( LogOnline, Log, TEXT( "ShowMessageThenGotoState: Ignoring due to higher message priority in queue." ) );
		return;
	}

	const bool bAlreadyAtMessageMenu = PendingState == ShooterGameInstanceState::MessageMenu || CurrentState == ShooterGameInstanceState::MessageMenu;
	const bool bAlreadyAtDestState = PendingState == NewState || CurrentState == NewState;

	// If we are already going to the message menu, don't override unless asked to
	if ( bAlreadyAtMessageMenu && PendingMessage.NextState == NewState && !OverrideExisting )
	{
		UE_LOG( LogOnline, Log, TEXT( "ShowMessageThenGotoState: Ignoring due to higher message priority in queue (check 1)." ) );
		return;
	}

	// If we are already going to the message menu, and the next dest is login screen, don't override
	if ( bAlreadyAtMessageMenu && PendingMessage.NextState == ShooterGameInstanceState::LoginScreen )
	{
		UE_LOG( LogOnline, Log, TEXT( "ShowMessageThenGotoState: Ignoring due to higher message priority in queue (check 2)." ) );
		return;
	}

	// If we are already at the dest state, don't override unless asked
	if ( bAlreadyAtDestState && !OverrideExisting )
	{
		UE_LOG( LogOnline, Log, TEXT( "ShowMessageThenGotoState: Ignoring due to higher message priority in queue (check 3)" ) );
		return;
	}

	PendingMessage.DisplayString		= Message;
	PendingMessage.OKButtonString		= OKButtonString;
	PendingMessage.CancelButtonString	= CancelButtonString;
	PendingMessage.NextState			= NewState;
	PendingMessage.PlayerOwner			= PlayerOwner;

	if ( CurrentState == ShooterGameInstanceState::MessageMenu )
	{
		UE_LOG( LogOnline, Log, TEXT( "ShowMessageThenGotoState: Forcing new message" ) );
		EndMessageMenuState();
		BeginMessageMenuState();
	}
	else
	{
		GotoState(ShooterGameInstanceState::MessageMenu);
	}
}

void UShooterGameInstance::ShowLoadingScreen()
{
	// This can be confusing, so here is what is happening:
	//	For LoadMap, we use the IShooterGameLoadingScreenModule interface to show the load screen
	//  This is necessary since this is a blocking call, and our viewport loading screen won't get updated.
	//  We can't use IShooterGameLoadingScreenModule for seamless travel though
	//  In this case, we just add a widget to the viewport, and have it update on the main thread
	//  To simplify things, we just do both, and you can't tell, one will cover the other if they both show at the same time
	/*IShooterGameLoadingScreenModule* const LoadingScreenModule = FModuleManager::LoadModulePtr<IShooterGameLoadingScreenModule>("ShooterGameLoadingScreen");
	if (LoadingScreenModule != nullptr)
	{
		LoadingScreenModule->StartInGameLoadingScreen();
	}*/
}

void UShooterGameInstance::HideLoadingScreen()
{
}

void UShooterGameInstance::ShowLoginScreen()
{
	AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (MenuPC)
	{
		MenuPC->ShowLoginScreen();
	}
}

void UShooterGameInstance::HideLoginScreen()
{
	AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (MenuPC)
	{
		MenuPC->HideLoginScreen();
	}
}

bool UShooterGameInstance::LoadFrontEndMap(const FString& MapName)
{
	bool bSuccess = true;

	// if already loaded, do nothing
	UWorld* const World = GetWorld();
	if (World)
	{
		FString const CurrentMapName = *World->PersistentLevel->GetOutermost()->GetName();
#if WITH_EDITOR
		// This solves the problem where we load the same map in the editor
		// For some reason GetName above DOES return the full path?
		if (CurrentMapName.Find("Entry") != -1 && MapName.Find("Entry") != -1)
		{
			return bSuccess;
		}
#else
		if (CurrentMapName == MapName)
		{
			return bSuccess;
		}
#endif
	}

	FString Error;
	EBrowseReturnVal::Type BrowseRet = EBrowseReturnVal::Failure;
	FURL URL(*FString::Printf(TEXT("%s"), *MapName));

	if (URL.Valid && !HasAnyFlags(RF_ClassDefaultObject)) //CastChecked<UEngine>() will fail if using Default__ShooterGameInstance, so make sure that we're not default
	{
		BrowseRet = GetEngine()->Browse(*WorldContext, URL, Error);

		// Handle failure.
		if (BrowseRet != EBrowseReturnVal::Success)
		{
			UE_LOG(LogLoad, Fatal, TEXT("%s"), *FString::Printf(TEXT("Failed to enter %s: %s. Please check the log for errors."), *MapName, *Error));
			bSuccess = false;
		}
	}
	return bSuccess;
}

AShooterGameSession* UShooterGameInstance::GetGameSession() const
{
	UWorld* const World = GetWorld();
	if (World)
	{
		AGameModeBase* const Game = World->GetAuthGameMode();
		if (Game)
		{
			return Cast<AShooterGameSession>(Game->GameSession);
		}
	}

	return nullptr;
}

void UShooterGameInstance::TravelLocalSessionFailure(UWorld *World, ETravelFailure::Type FailureType, const FString& ReasonString)
{
	AShooterPlayerController_Menu* const FirstPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (FirstPC != nullptr)
	{
		FText ReturnReason = NSLOCTEXT("NetworkErrors", "JoinSessionFailed", "Join Session failed.");
		if (ReasonString.IsEmpty() == false)
		{
			ReturnReason = FText::Format(NSLOCTEXT("NetworkErrors", "JoinSessionFailedReasonFmt", "Join Session failed. {0}"), FText::FromString(ReasonString));
		}

		FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");
		ShowMessageThenGoMain(ReturnReason, OKButton, FText::GetEmpty());
	}
}

void UShooterGameInstance::ShowMessageThenGoMain(const FText& Message, const FText& OKButtonString, const FText& CancelButtonString)
{
	ShowMessageThenGotoState(Message, OKButtonString, CancelButtonString, ShooterGameInstanceState::MainMenu);
}

void UShooterGameInstance::SetPendingInvite(const FShooterPendingInvite& InPendingInvite)
{
	PendingInvite = InPendingInvite;
}

void UShooterGameInstance::GotoState(FName NewState)
{
	UE_LOG( LogOnline, Log, TEXT( "GotoState: NewState: %s" ), *NewState.ToString() );

	PendingState = NewState;
}

void UShooterGameInstance::MaybeChangeState()
{
	if ( (PendingState != CurrentState) && (PendingState != ShooterGameInstanceState::None) )
	{
		FName const OldState = CurrentState;

		// end current state
		EndCurrentState(PendingState);

		// begin new state
		BeginNewState(PendingState, OldState);

		// clear pending change
		PendingState = ShooterGameInstanceState::None;
	}
}

void UShooterGameInstance::EndCurrentState(FName NextState)
{
	// per-state custom ending code here
	if (CurrentState == ShooterGameInstanceState::PendingInvite)
	{
		EndPendingInviteState();
	}
	else if (CurrentState == ShooterGameInstanceState::LoginScreen)
	{
		EndLoginScreenState();
	}
	else if (CurrentState == ShooterGameInstanceState::MainMenu)
	{
		EndMainMenuState();
	}
	else if (CurrentState == ShooterGameInstanceState::MessageMenu)
	{
		EndMessageMenuState();
	}
	else if (CurrentState == ShooterGameInstanceState::Playing)
	{
		EndPlayingState();
	}

	CurrentState = ShooterGameInstanceState::None;
}

void UShooterGameInstance::BeginNewState(FName NewState, FName PrevState)
{
	// per-state custom starting code here

	if (NewState == ShooterGameInstanceState::PendingInvite)
	{
		BeginPendingInviteState();
	}
	else if (NewState == ShooterGameInstanceState::LoginScreen)
	{
		BeginLoginState();
	}
	else if (NewState == ShooterGameInstanceState::MainMenu)
	{
		BeginMainMenuState();
	}
	else if (NewState == ShooterGameInstanceState::MessageMenu)
	{
		BeginMessageMenuState();
	}
	else if (NewState == ShooterGameInstanceState::Playing)
	{
		BeginPlayingState();
	}

	CurrentState = NewState;
}

void UShooterGameInstance::BeginPendingInviteState()
{	
	if (LoadFrontEndMap(MainMenuMap))
	{				
		StartOnlinePrivilegeTask(IOnlineIdentity::FOnGetUserPrivilegeCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnUserCanPlayInvite), EUserPrivileges::CanPlayOnline, PendingInvite.UserId);
	}
	else
	{
		GotoState(ShooterGameInstanceState::LoginScreen);
	}
}

void UShooterGameInstance::EndPendingInviteState()
{
	// cleanup in case the state changed before the pending invite was handled.
	CleanupOnlinePrivilegeTask();
}

void UShooterGameInstance::BeginLoginState()
{
	//this must come before split screen player removal so that the OSS sets all players to not using online features.
	SetOnlineMode(EOnlineMode::Offline);

	// Remove any possible split-screen players
	RemoveSplitScreenPlayers();

	LoadFrontEndMap(LoginScreenMap);

	ULocalPlayer* const LocalPlayer = GetFirstGamePlayer();
	LocalPlayer->SetCachedUniqueNetId(nullptr);
	if (LocalPlayer->PlayerController)
	{
		LocalPlayer->PlayerController->bShowMouseCursor = true;
	}
	ShowLoginScreen();
	

	// Disallow split-screen (we will allow while in the playing state)
	GetGameViewportClient()->SetDisableSplitscreenOverride( true );
}

void UShooterGameInstance::EndLoginScreenState()
{
	HideLoginScreen();
}

void UShooterGameInstance::SetPresenceForLocalPlayers(const FString& StatusStr, const FVariantData& PresenceData)
{
	const auto Presence = Online::GetPresenceInterface();
	if (Presence.IsValid())
	{
		for (int i = 0; i < LocalPlayers.Num(); ++i)
		{
			FUniqueNetIdRepl UserId = LocalPlayers[i]->GetPreferredUniqueNetId();

			if (UserId.IsValid())
			{
				FOnlineUserPresenceStatus PresenceStatus;
				PresenceStatus.StatusStr = StatusStr;
				PresenceStatus.Properties.Add(DefaultPresenceKey, PresenceData);

				Presence->SetPresence(*UserId, PresenceStatus);
			}
		}
	}
}

void UShooterGameInstance::BeginMainMenuState()
{
	// Make sure we're not showing the loading screen
	HideLoadingScreen();

	SetOnlineMode(EOnlineMode::Offline);

	// Disallow splitscreen
	UGameViewportClient* GameViewportClient = GetGameViewportClient();
	
	if (GameViewportClient)
	{
		GetGameViewportClient()->SetDisableSplitscreenOverride(true);
	}

	// Remove any possible splitscren players
	RemoveSplitScreenPlayers();

	// Set presence to menu state for the owning player
	SetPresenceForLocalPlayers(FString(TEXT("In Menu")), FVariantData(FString(TEXT("OnMenu"))));

	// load startup map
	LoadFrontEndMap(MainMenuMap);

	// player 0 gets to own the UI
	ULocalPlayer* const Player = GetFirstGamePlayer();

	/*MainMenuUI = MakeShareable(new FShooterMainMenu());
	MainMenuUI->Construct(this, Player);
	MainMenuUI->AddMenuToGameViewport();

	// It's possible that a play together event was sent by the system while the player was in-game or didn't
	// have the application launched. The game will automatically go directly to the main menu state in those cases
	// so this will handle Play Together if that is why we transitioned here.
	if (PlayTogetherInfo.UserIndex != -1)
	{
		MainMenuUI->OnPlayTogetherEventReceived();
	}*/

	auto Identity = Online::GetIdentityInterface();
	if (Identity.IsValid() && Identity->GetLoginStatus(0) == ELoginStatus::LoggedIn)
	{
		TSharedPtr<const FUniqueNetId> playerId = Identity->GetUniquePlayerId(0);
		TSharedPtr<FUserOnlineAccount> user = Identity->GetUserAccount(*playerId);
		AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
		if (MenuPC)
		{
			MenuPC->UserProfile = NewObject<UUserProfile>();
			MenuPC->UserProfile->DisplayName = user->GetDisplayName();
			MenuPC->UserProfile->PlayerId = user->GetUserId().Get().ToString();

			MenuPC->ShowMainMenu();
		}
	}

#if !SHOOTER_CONSOLE_UI
	// The cached unique net ID is usually set on the login screen, but there isn't
	// one on PC/Mac, so do it here.
	if (Player != nullptr)
	{
		//Todo:cache the gamesparks uniquenetid
		//Player->SetControllerId(0);
		//Player->SetCachedUniqueNetId(Player->GetUniqueNetIdFromCachedControllerId().GetUniqueNetId());
	}
#endif

	RemoveNetworkFailureHandlers();
}

void UShooterGameInstance::EndMainMenuState()
{
	/*if (MainMenuUI.IsValid())
	{
		MainMenuUI->RemoveMenuFromGameViewport();
		MainMenuUI = nullptr;
	}*/
}

void UShooterGameInstance::BeginMessageMenuState()
{
	if (PendingMessage.DisplayString.IsEmpty())
	{
		UE_LOG(LogOnlineGame, Warning, TEXT("UShooterGameInstance::BeginMessageMenuState: Display string is empty"));
		GotoInitialState();
		return;
	}

	// Make sure we're not showing the loading screen
	HideLoadingScreen();

	/*check(!MessageMenuUI.IsValid());
	MessageMenuUI = MakeShareable(new FShooterMessageMenu);
	MessageMenuUI->Construct(this, PendingMessage.PlayerOwner, PendingMessage.DisplayString, PendingMessage.OKButtonString, PendingMessage.CancelButtonString, PendingMessage.NextState);
	*/
	PendingMessage.DisplayString = FText::GetEmpty();
}

void UShooterGameInstance::EndMessageMenuState()
{
	/*if (MessageMenuUI.IsValid())
	{
		MessageMenuUI->RemoveFromGameViewport();
		MessageMenuUI = nullptr;
	}*/
}

void UShooterGameInstance::BeginPlayingState()
{
	bPendingEnableSplitscreen = true;

	// Set presence for playing in a map
	SetPresenceForLocalPlayers(FString(TEXT("In Game")), FVariantData(FString(TEXT("InGame"))));

	// Make sure viewport has focus
	FSlateApplication::Get().SetAllUserFocusToGameViewport();
	AShooterPlayerController_Menu* const MenuPC = Cast<AShooterPlayerController_Menu>(UGameplayStatics::GetPlayerController(GetWorld(), 0));
	if (MenuPC)
	{
		MenuPC->HideMainMenu();
	}
}

void UShooterGameInstance::EndPlayingState()
{
	// Disallow splitscreen
	GetGameViewportClient()->SetDisableSplitscreenOverride( true );

	// Clear the players' presence information
	SetPresenceForLocalPlayers(FString(TEXT("In Menu")), FVariantData(FString(TEXT("OnMenu"))));

	UWorld* const World = GetWorld();
	AShooterGameState* const GameState = World != NULL ? World->GetGameState<AShooterGameState>() : NULL;

	if (GameState)
	{
		// Send round end events for local players
		for (int i = 0; i < LocalPlayers.Num(); ++i)
		{
			auto ShooterPC = Cast<AShooterPlayerController>(LocalPlayers[i]->PlayerController);
			if (ShooterPC)
			{
				// Assuming you can't win if you quit early
				ShooterPC->ClientSendRoundEndEvent(false, GameState->ElapsedTime);
			}
		}

		// Give the game state a chance to cleanup first
		GameState->RequestFinishAndExitToMainMenu();
	}
	else
	{
		// If there is no game state, make sure the session is in a good state
		CleanupSessionOnReturnToMenu();
	}
}

void UShooterGameInstance::OnEndSessionComplete( FName SessionName, bool bWasSuccessful )
{
	UE_LOG(LogOnline, Log, TEXT("UShooterGameInstance::OnEndSessionComplete: Session=%s bWasSuccessful=%s"), *SessionName.ToString(), bWasSuccessful ? TEXT("true") : TEXT("false") );

	IOnlineSubsystem* OnlineSub = IOnlineSubsystem::Get();
	if (OnlineSub)
	{
		//IOnlineSessionPtr Sessions = OnlineSub->GetSessionInterface();
		//if (Sessions.IsValid())
		//{
		//	Sessions->ClearOnStartSessionCompleteDelegate_Handle  (OnStartSessionCompleteDelegateHandle);
		//	Sessions->ClearOnEndSessionCompleteDelegate_Handle    (OnEndSessionCompleteDelegateHandle);
		//	Sessions->ClearOnDestroySessionCompleteDelegate_Handle(OnDestroySessionCompleteDelegateHandle);
		//}
	}

	// continue
	CleanupSessionOnReturnToMenu();
}

void UShooterGameInstance::CleanupSessionOnReturnToMenu()
{
	bool bPendingOnlineOp = false;

	// end online game and then destroy it
	IOnlineSubsystem * OnlineSub = IOnlineSubsystem::Get();
	//IOnlineSessionPtr Sessions = ( OnlineSub != NULL ) ? OnlineSub->GetSessionInterface() : NULL;
	//
	//if ( Sessions.IsValid() )
	//{
	//	FName GameSession(NAME_GameSession);
	//	EOnlineSessionState::Type SessionState = Sessions->GetSessionState(NAME_GameSession);
	//	UE_LOG(LogOnline, Log, TEXT("Session %s is '%s'"), *GameSession.ToString(), EOnlineSessionState::ToString(SessionState));

	//	if ( EOnlineSessionState::InProgress == SessionState )
	//	{
	//		UE_LOG(LogOnline, Log, TEXT("Ending session %s on return to main menu"), *GameSession.ToString() );
	//		OnEndSessionCompleteDelegateHandle = Sessions->AddOnEndSessionCompleteDelegate_Handle(OnEndSessionCompleteDelegate);
	//		Sessions->EndSession(NAME_GameSession);
	//		bPendingOnlineOp = true;
	//	}
	//	else if ( EOnlineSessionState::Ending == SessionState )
	//	{
	//		UE_LOG(LogOnline, Log, TEXT("Waiting for session %s to end on return to main menu"), *GameSession.ToString() );
	//		OnEndSessionCompleteDelegateHandle = Sessions->AddOnEndSessionCompleteDelegate_Handle(OnEndSessionCompleteDelegate);
	//		bPendingOnlineOp = true;
	//	}
	//	else if ( EOnlineSessionState::Ended == SessionState || EOnlineSessionState::Pending == SessionState )
	//	{
	//		UE_LOG(LogOnline, Log, TEXT("Destroying session %s on return to main menu"), *GameSession.ToString() );
	//		OnDestroySessionCompleteDelegateHandle = Sessions->AddOnDestroySessionCompleteDelegate_Handle(OnEndSessionCompleteDelegate);
	//		Sessions->DestroySession(NAME_GameSession);
	//		bPendingOnlineOp = true;
	//	}
	//	else if ( EOnlineSessionState::Starting == SessionState || EOnlineSessionState::Creating == SessionState)
	//	{
	//		UE_LOG(LogOnline, Log, TEXT("Waiting for session %s to start, and then we will end it to return to main menu"), *GameSession.ToString() );
	//		OnStartSessionCompleteDelegateHandle = Sessions->AddOnStartSessionCompleteDelegate_Handle(OnEndSessionCompleteDelegate);
	//		bPendingOnlineOp = true;
	//	}
	//}

	if ( !bPendingOnlineOp )
	{
		//GEngine->HandleDisconnect( GetWorld(), GetWorld()->GetNetDriver() );
	}
}

void UShooterGameInstance::LabelPlayerAsQuitter(ULocalPlayer* LocalPlayer) const
{
	AShooterPlayerState* const PlayerState = LocalPlayer && LocalPlayer->PlayerController ? Cast<AShooterPlayerState>(LocalPlayer->PlayerController->PlayerState) : nullptr;	
	if(PlayerState)
	{
		PlayerState->SetQuitter(true);
	}
}

void UShooterGameInstance::RemoveNetworkFailureHandlers()
{
	// Remove the local session/travel failure bindings if they exist
	if (GEngine->OnTravelFailure().IsBoundToObject(this) == true)
	{
		GEngine->OnTravelFailure().Remove(TravelLocalSessionFailureDelegateHandle);
	}
}

void UShooterGameInstance::AddNetworkFailureHandlers()
{
	// Add network/travel error handlers (if they are not already there)
	if (GEngine->OnTravelFailure().IsBoundToObject(this) == false)
	{
		TravelLocalSessionFailureDelegateHandle = GEngine->OnTravelFailure().AddUObject(this, &UShooterGameInstance::TravelLocalSessionFailure);
	}
}

TSubclassOf<UOnlineSession> UShooterGameInstance::GetOnlineSessionClass()
{
	return UShooterOnlineSessionClient::StaticClass();
}

bool UShooterGameInstance::HostQuickSession(ULocalPlayer& LocalPlayer, const FOnlineSessionSettings& SessionSettings)
{
	// This function is different from BeginHostingQuickMatch in that it creates a session and then starts a quick match,
	// while BeginHostingQuickMatch assumes a session already exists

	if (AShooterGameSession* const GameSession = GetGameSession())
	{
		// Add callback delegate for completion
		OnCreatePresenceSessionCompleteDelegateHandle = GameSession->OnCreatePresenceSessionComplete().AddUObject(this, &UShooterGameInstance::OnCreatePresenceSessionComplete);

		TravelURL = GetQuickMatchUrl();

		FOnlineSessionSettings HostSettings = SessionSettings;

		const FString GameType = UGameplayStatics::ParseOption(TravelURL, TEXT("game"));

		// Determine the map name from the travelURL
		const FString MapNameSubStr = "/Game/Maps/";
		const FString ChoppedMapName = TravelURL.RightChop(MapNameSubStr.Len());
		const FString MapName = ChoppedMapName.LeftChop(ChoppedMapName.Len() - ChoppedMapName.Find("?game"));

		HostSettings.Set(SETTING_GAMEMODE, GameType, EOnlineDataAdvertisementType::ViaOnlineService);
		HostSettings.Set(SETTING_MAPNAME, MapName, EOnlineDataAdvertisementType::ViaOnlineService);
		HostSettings.NumPublicConnections = 16;

		if (GameSession->HostSession(LocalPlayer.GetPreferredUniqueNetId().GetUniqueNetId(), NAME_GameSession, SessionSettings))
		{
			// If any error occurred in the above, pending state would be set
			if (PendingState == CurrentState || PendingState == ShooterGameInstanceState::None)
			{
				// Go ahead and go into loading state now
				// If we fail, the delegate will handle showing the proper messaging and move to the correct state
				ShowLoadingScreen();
				GotoState(ShooterGameInstanceState::Playing);
				return true;
			}
		}
	}

	return false;
}

bool UShooterGameInstance::LoadMissionLevel(const FString& MapPath)
{
	if ((PendingState == CurrentState) || (PendingState == ShooterGameInstanceState::None))
	{
		// Go ahead and go into loading state now
		// If we fail, the delegate will handle showing the proper messaging and move to the correct state
		ShowLoadingScreen();
		GotoState(ShooterGameInstanceState::Playing);
		return LoadFrontEndMap(MapPath);
	}
	return false;
}

bool UShooterGameInstance::HostGame(ULocalPlayer* LocalPlayer, const FString& GameType, const FString& InTravelURL)
{
	if (GetOnlineMode() == EOnlineMode::Offline)
	{
		//
		// Offline game, just go straight to map
		//

		ShowLoadingScreen();
		GotoState(ShooterGameInstanceState::Playing);

		// Travel to the specified match URL
		TravelURL = InTravelURL;
		GetWorld()->ServerTravel(TravelURL);
		return true;
	}

	//
	// Online game
	//

	AShooterGameSession* const GameSession = GetGameSession();
	if (GameSession)
	{
		// add callback delegate for completion
		OnCreatePresenceSessionCompleteDelegateHandle = GameSession->OnCreatePresenceSessionComplete().AddUObject(this, &UShooterGameInstance::OnCreatePresenceSessionComplete);

		TravelURL = InTravelURL;
		bool const bIsLanMatch = InTravelURL.Contains(TEXT("?bIsLanMatch"));

		//determine the map name from the travelURL
		const FString& MapNameSubStr = "/Game/Maps/";
		const FString& ChoppedMapName = TravelURL.RightChop(MapNameSubStr.Len());
		const FString& MapName = ChoppedMapName.LeftChop(ChoppedMapName.Len() - ChoppedMapName.Find("?game"));

		if (GameSession->HostSession(LocalPlayer->GetPreferredUniqueNetId().GetUniqueNetId(), NAME_GameSession, GameType, MapName, bIsLanMatch, true, AShooterGameSession::DEFAULT_NUM_PLAYERS))
		{
			// If any error occurred in the above, pending state would be set
			if ( (PendingState == CurrentState) || (PendingState == ShooterGameInstanceState::None) )
			{
				// Go ahead and go into loading state now
				// If we fail, the delegate will handle showing the proper messaging and move to the correct state
				ShowLoadingScreen();
				GotoState(ShooterGameInstanceState::Playing);
				return true;
			}
		}
	}

	return false;
}

bool UShooterGameInstance::JoinSession(ULocalPlayer* LocalPlayer, int32 SessionIndexInSearchResults)
{
	// needs to tear anything down based on current state?

	AShooterGameSession* const GameSession = GetGameSession();
	if (GameSession)
	{
		AddNetworkFailureHandlers();

		OnJoinSessionCompleteDelegateHandle = GameSession->OnJoinSessionComplete().AddUObject(this, &UShooterGameInstance::OnJoinSessionComplete);
		if (GameSession->JoinSession(LocalPlayer->GetPreferredUniqueNetId().GetUniqueNetId(), NAME_GameSession, SessionIndexInSearchResults))
		{
			// If any error occurred in the above, pending state would be set
			if ( (PendingState == CurrentState) || (PendingState == ShooterGameInstanceState::None) )
			{
				// Go ahead and go into loading state now
				// If we fail, the delegate will handle showing the proper messaging and move to the correct state
				ShowLoadingScreen();
				GotoState(ShooterGameInstanceState::Playing);
				return true;
			}
		}
	}

	return false;
}

bool UShooterGameInstance::JoinSession(ULocalPlayer* LocalPlayer, const FOnlineSessionSearchResult& SearchResult)
{
	// needs to tear anything down based on current state?
	AShooterGameSession* const GameSession = GetGameSession();
	if (GameSession)
	{
		AddNetworkFailureHandlers();

		OnJoinSessionCompleteDelegateHandle = GameSession->OnJoinSessionComplete().AddUObject(this, &UShooterGameInstance::OnJoinSessionComplete);
		if (GameSession->JoinSession(LocalPlayer->GetPreferredUniqueNetId().GetUniqueNetId(), NAME_GameSession, SearchResult))
		{
			// If any error occured in the above, pending state would be set
			if ( (PendingState == CurrentState) || (PendingState == ShooterGameInstanceState::None) )
			{
				// Go ahead and go into loading state now
				// If we fail, the delegate will handle showing the proper messaging and move to the correct state
				ShowLoadingScreen();
				GotoState(ShooterGameInstanceState::Playing);
				return true;
			}
		}
	}

	return false;
}

bool UShooterGameInstance::PlayDemo(ULocalPlayer* LocalPlayer, const FString& DemoName)
{
	ShowLoadingScreen();

	// Play the demo
	PlayReplay(DemoName);
	
	return true;
}

/** Callback which is intended to be called upon finding sessions */
void UShooterGameInstance::OnJoinSessionComplete(EOnJoinSessionCompleteResult::Type Result)
{
	// unhook the delegate
	AShooterGameSession* const GameSession = GetGameSession();
	if (GameSession)
	{
		GameSession->OnJoinSessionComplete().Remove(OnJoinSessionCompleteDelegateHandle);
	}

	// Add the splitscreen player if one exists
	if (Result == EOnJoinSessionCompleteResult::Success && LocalPlayers.Num() > 1)
	{
		//auto Sessions = Online::GetSessionInterface();
		//if (Sessions.IsValid() && LocalPlayers[1]->GetPreferredUniqueNetId().IsValid())
		//{
		//	Sessions->RegisterLocalPlayer(*LocalPlayers[1]->GetPreferredUniqueNetId(), NAME_GameSession,
		//		FOnRegisterLocalPlayerCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnRegisterJoiningLocalPlayerComplete));
		//}
	}
	else
	{
		// We either failed or there is only a single local user
		FinishJoinSession(Result);
	}
}

void UShooterGameInstance::FinishJoinSession(EOnJoinSessionCompleteResult::Type Result)
{
	if (Result != EOnJoinSessionCompleteResult::Success)
	{
		FText ReturnReason;
		switch (Result)
		{
		case EOnJoinSessionCompleteResult::SessionIsFull:
			ReturnReason = NSLOCTEXT("NetworkErrors", "JoinSessionFailed", "Game is full.");
			break;
		case EOnJoinSessionCompleteResult::SessionDoesNotExist:
			ReturnReason = NSLOCTEXT("NetworkErrors", "JoinSessionFailed", "Game no longer exists.");
			break;
		default:
			ReturnReason = NSLOCTEXT("NetworkErrors", "JoinSessionFailed", "Join failed.");
			break;
		}

		FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");
		RemoveNetworkFailureHandlers();
		ShowMessageThenGoMain(ReturnReason, OKButton, FText::GetEmpty());
		return;
	}

	InternalTravelToSession(NAME_GameSession);
}

void UShooterGameInstance::OnRegisterJoiningLocalPlayerComplete(const FUniqueNetId& PlayerId, EOnJoinSessionCompleteResult::Type Result)
{
	FinishJoinSession(Result);
}

void UShooterGameInstance::InternalTravelToSession(const FName& SessionName)
{
	APlayerController * const PlayerController = GetFirstLocalPlayerController();

	if ( PlayerController == nullptr )
	{
		FText ReturnReason = NSLOCTEXT("NetworkErrors", "InvalidPlayerController", "Invalid Player Controller");
		FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");
		RemoveNetworkFailureHandlers();
		ShowMessageThenGoMain(ReturnReason, OKButton, FText::GetEmpty());
		return;
	}

	// travel to session
	IOnlineSubsystem* OnlineSub = IOnlineSubsystem::Get();

	if ( OnlineSub == nullptr )
	{
		FText ReturnReason = NSLOCTEXT("NetworkErrors", "OSSMissing", "OSS missing");
		FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");
		RemoveNetworkFailureHandlers();
		ShowMessageThenGoMain(ReturnReason, OKButton, FText::GetEmpty());
		return;
	}

	FString URL;
	//IOnlineSessionPtr Sessions = OnlineSub->GetSessionInterface();

	//if ( !Sessions.IsValid() || !Sessions->GetResolvedConnectString( SessionName, URL ) )
	//{
	//	FText FailReason = NSLOCTEXT("NetworkErrors", "TravelSessionFailed", "Travel to Session failed.");
	//	FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");
	//	ShowMessageThenGoMain(FailReason, OKButton, FText::GetEmpty());
	//	UE_LOG(LogOnlineGame, Warning, TEXT("Failed to travel to session upon joining it"));
	//	return;
	//}

	// Add debug encryption token if desired.
	if (CVarShooterGameTestEncryption->GetInt() != 0)
	{
		// This is just a value for testing/debugging, the server will use the same key regardless of the token value.
		// But the token could be a user ID and/or session ID that would be used to generate a unique key per user and/or session, if desired.
		URL += TEXT("?EncryptionToken=1");
	}

	PlayerController->ClientTravel(URL, TRAVEL_Absolute);
}

/** Callback which is intended to be called upon session creation */
void UShooterGameInstance::OnCreatePresenceSessionComplete(FName SessionName, bool bWasSuccessful)
{
	AShooterGameSession* const GameSession = GetGameSession();
	if (GameSession)
	{
		GameSession->OnCreatePresenceSessionComplete().Remove(OnCreatePresenceSessionCompleteDelegateHandle);

		// Add the splitscreen player if one exists
		if (bWasSuccessful && LocalPlayers.Num() > 1)
		{
			/*auto Sessions = Online::GetSessionInterface();
			if (Sessions.IsValid() && LocalPlayers[1]->GetPreferredUniqueNetId().IsValid())
			{
				Sessions->RegisterLocalPlayer(*LocalPlayers[1]->GetPreferredUniqueNetId(), NAME_GameSession,
					FOnRegisterLocalPlayerCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnRegisterLocalPlayerComplete));
			}*/
		}
		else
		{
			// We either failed or there is only a single local user
			FinishSessionCreation(bWasSuccessful ? EOnJoinSessionCompleteResult::Success : EOnJoinSessionCompleteResult::UnknownError);
		}
	}
}

/** Initiates the session searching */
bool UShooterGameInstance::FindSessions(ULocalPlayer* PlayerOwner, bool bIsDedicatedServer, bool bFindLAN)
{
	bool bResult = false;

	check(PlayerOwner != nullptr);
	if (PlayerOwner)
	{
		AShooterGameSession* const GameSession = GetGameSession();
		if (GameSession)
		{
			GameSession->OnFindSessionsComplete().RemoveAll(this);
			OnSearchSessionsCompleteDelegateHandle = GameSession->OnFindSessionsComplete().AddUObject(this, &UShooterGameInstance::OnSearchSessionsComplete);

			GameSession->FindSessions(PlayerOwner->GetPreferredUniqueNetId().GetUniqueNetId(), NAME_GameSession, bFindLAN, !bIsDedicatedServer);

			bResult = true;
		}
	}

	return bResult;
}

/** Callback which is intended to be called upon finding sessions */
void UShooterGameInstance::OnSearchSessionsComplete(bool bWasSuccessful)
{
	AShooterGameSession* const Session = GetGameSession();
	if (Session)
	{
		Session->OnFindSessionsComplete().Remove(OnSearchSessionsCompleteDelegateHandle);
	}
}

bool UShooterGameInstance::Tick(float DeltaSeconds)
{
	if (RTSession.IsValid())
	{
		RTSession->Update();
	}

	// Dedicated server doesn't need to worry about game state
	if (IsRunningDedicatedServer() == true)
	{
		return true;
	}

	MaybeChangeState();

	if (CurrentState != ShooterGameInstanceState::LoginScreen)
	{
		// If at any point we aren't licensed (but we are after login) bounce them back to the login screen
		if (!bIsLicensed && CurrentState != ShooterGameInstanceState::None)
		{
			const FText ReturnReason	= NSLOCTEXT( "ProfileMessages", "NeedLicense", "The signed in users do not have a license for this game. Please purchase ShooterGame from the Xbox Marketplace or sign in a user with a valid license." );
			const FText OKButton		= NSLOCTEXT( "DialogButtons", "OKAY", "OK" );

			ShowMessageThenGotoState( ReturnReason, OKButton, FText::GetEmpty(), ShooterGameInstanceState::LoginScreen );
		}

		// Show controller disconnected dialog if any local players have an invalid controller
	}

	// If we have a pending invite, and we are at the login screen, and the session is properly shut down, accept it
	if (PendingInvite.UserId.IsValid() && PendingInvite.bPrivilegesCheckedAndAllowed && CurrentState == ShooterGameInstanceState::PendingInvite)
	{
		IOnlineSubsystem * OnlineSub = IOnlineSubsystem::Get();
		/*IOnlineSessionPtr Sessions = (OnlineSub != NULL) ? OnlineSub->GetSessionInterface() : NULL;

		if (Sessions.IsValid())
		{
			EOnlineSessionState::Type SessionState = Sessions->GetSessionState(NAME_GameSession);

			if (SessionState == EOnlineSessionState::NoSession)
			{
				ULocalPlayer * NewPlayerOwner = GetFirstGamePlayer();

				if (NewPlayerOwner != nullptr)
				{
					NewPlayerOwner->SetControllerId(PendingInvite.ControllerId);
					NewPlayerOwner->SetCachedUniqueNetId(PendingInvite.UserId);
					SetOnlineMode(EOnlineMode::Online);

					const bool bIsLocalPlayerHost = PendingInvite.UserId.IsValid() && PendingInvite.InviteResult.Session.OwningUserId.IsValid() && *PendingInvite.UserId == *PendingInvite.InviteResult.Session.OwningUserId;
					if (bIsLocalPlayerHost)
					{
						HostQuickSession(*NewPlayerOwner, PendingInvite.InviteResult.Session.SessionSettings);
					}
					else
					{
						JoinSession(NewPlayerOwner, PendingInvite.InviteResult);
					}
				}

				PendingInvite.UserId.Reset();
			}
		}*/
	}

	return true;
}

bool UShooterGameInstance::HandleOpenCommand(const TCHAR* Cmd, FOutputDevice& Ar, UWorld* InWorld)
{
	bool const bOpenSuccessful = Super::HandleOpenCommand(Cmd, Ar, InWorld);
	if (bOpenSuccessful)
	{
		GotoState(ShooterGameInstanceState::Playing);
	}

	return bOpenSuccessful;
}

void UShooterGameInstance::HandleSignInChangeMessaging()
{
	// Master user signed out, go to initial state (if we aren't there already)
	if ( CurrentState != GetInitialState() )
	{
#if SHOOTER_CONSOLE_UI
		// Display message on consoles
		const FText ReturnReason	= NSLOCTEXT( "ProfileMessages", "SignInChange", "Sign in status change occurred." );
		const FText OKButton		= NSLOCTEXT( "DialogButtons", "OKAY", "OK" );

		ShowMessageThenGotoState(ReturnReason, OKButton, FText::GetEmpty(), GetInitialState());
#else								
		GotoInitialState();
#endif
	}
}

void UShooterGameInstance::HandleUserLoginChanged(int32 GameUserIndex, ELoginStatus::Type PreviousLoginStatus, ELoginStatus::Type LoginStatus, const FUniqueNetId& UserId)
{
	// On Switch, accounts can play in LAN games whether they are signed in online or not. 
#if PLATFORM_SWITCH
	const bool bDowngraded = LoginStatus == ELoginStatus::NotLoggedIn || (GetOnlineMode() == EOnlineMode::Online && LoginStatus == ELoginStatus::UsingLocalProfile);
#else
	const bool bDowngraded = (LoginStatus == ELoginStatus::NotLoggedIn && GetOnlineMode() == EOnlineMode::Offline) || (LoginStatus != ELoginStatus::LoggedIn && GetOnlineMode() != EOnlineMode::Offline);
#endif

	UE_LOG( LogOnline, Log, TEXT( "HandleUserLoginChanged: bDownGraded: %i" ), (int)bDowngraded );

	TSharedPtr<GenericApplication> GenericApplication = FSlateApplication::Get().GetPlatformApplication();
	bIsLicensed = GenericApplication->ApplicationLicenseValid();

	// Find the local player associated with this unique net id
	ULocalPlayer * LocalPlayer = FindLocalPlayerFromUniqueNetId( UserId );

	LocalPlayerOnlineStatus[GameUserIndex] = LoginStatus;

	// If this user is signed out, but was previously signed in, punt to login (or remove split-screen if that makes sense)
	if ( LocalPlayer != NULL )
	{
		if (bDowngraded)
		{
			UE_LOG( LogOnline, Log, TEXT( "HandleUserLoginChanged: Player logged out: %s" ), *UserId.ToString() );

			LabelPlayerAsQuitter(LocalPlayer);

			// Check to see if this was the master, or if this was a split-screen player on the client
			if ( LocalPlayer == GetFirstGamePlayer() || GetOnlineMode() != EOnlineMode::Offline )
			{
				HandleSignInChangeMessaging();
			}
			else
			{
				// Remove local split-screen players from the list
				RemoveExistingLocalPlayer( LocalPlayer );
			}
		}
	}
}

void UShooterGameInstance::HandleAppWillDeactivate()
{
	if (CurrentState == ShooterGameInstanceState::Playing)
	{
		// Just have the first player controller pause the game.
		UWorld* const GameWorld = GetWorld();
		if (GameWorld)
		{
			// protect against a second pause menu loading on top of an existing one if someone presses the Jewel / PS buttons.
			bool bNeedsPause = true;
			for (FConstControllerIterator It = GameWorld->GetControllerIterator(); It; ++It)
			{
				AShooterPlayerController* Controller = Cast<AShooterPlayerController>(*It);
				if (Controller && (Controller->IsPaused() || Controller->IsGameMenuVisible()))
				{
					bNeedsPause = false;
					break;
				}
			}

			if (bNeedsPause)
			{
				AShooterPlayerController* const Controller = Cast<AShooterPlayerController>(GameWorld->GetFirstPlayerController());
				if (Controller)
				{
					Controller->ShowInGameMenu();
				}
			}
		}
	}
}

void UShooterGameInstance::HandleAppSuspend()
{
	// Players will lose connection on resume. However it is possible the game will exit before we get a resume, so we must kick off round end events here.
	UE_LOG( LogOnline, Warning, TEXT( "UShooterGameInstance::HandleAppSuspend" ) );
	UWorld* const World = GetWorld(); 
	AShooterGameState* const GameState = World != NULL ? World->GetGameState<AShooterGameState>() : NULL;

	if ( CurrentState != ShooterGameInstanceState::None && CurrentState != GetInitialState() )
	{
		UE_LOG( LogOnline, Warning, TEXT( "UShooterGameInstance::HandleAppSuspend: Sending round end event for players" ) );

		// Send round end events for local players
		for (int i = 0; i < LocalPlayers.Num(); ++i)
		{
			auto ShooterPC = Cast<AShooterPlayerController>(LocalPlayers[i]->PlayerController);
			if (ShooterPC && GameState)
			{
				// Assuming you can't win if you quit early
				ShooterPC->ClientSendRoundEndEvent(false, GameState->ElapsedTime);
			}
		}
	}
}

void UShooterGameInstance::HandleAppResume()
{
	UE_LOG( LogOnline, Log, TEXT( "UShooterGameInstance::HandleAppResume" ) );

	if ( CurrentState != ShooterGameInstanceState::None && CurrentState != GetInitialState() )
	{
		UE_LOG( LogOnline, Warning, TEXT( "UShooterGameInstance::HandleAppResume: Attempting to sign out players" ) );

		for ( int32 i = 0; i < LocalPlayers.Num(); ++i )
		{
			if ( LocalPlayers[i]->GetCachedUniqueNetId().IsValid() && LocalPlayerOnlineStatus[i] == ELoginStatus::LoggedIn && !IsLocalPlayerOnline( LocalPlayers[i] ) )
			{
				UE_LOG( LogOnline, Log, TEXT( "UShooterGameInstance::HandleAppResume: Signed out during resume." ) );
				HandleSignInChangeMessaging();
				break;
			}
		}
	}
}

void UShooterGameInstance::HandleAppLicenseUpdate()
{
	TSharedPtr<GenericApplication> GenericApplication = FSlateApplication::Get().GetPlatformApplication();
	bIsLicensed = GenericApplication->ApplicationLicenseValid();
}

void UShooterGameInstance::HandleSafeFrameChanged()
{
	UCanvas::UpdateAllCanvasSafeZoneData();
}

void UShooterGameInstance::RemoveExistingLocalPlayer(ULocalPlayer* ExistingPlayer)
{
	check(ExistingPlayer);
	if (ExistingPlayer->PlayerController != NULL)
	{
		// Kill the player
		AShooterCharacter* MyPawn = Cast<AShooterCharacter>(ExistingPlayer->PlayerController->GetPawn());
		if ( MyPawn )
		{
			MyPawn->KilledBy(NULL);
		}
	}

	// Remove local split-screen players from the list
	RemoveLocalPlayer( ExistingPlayer );
}

void UShooterGameInstance::RemoveSplitScreenPlayers()
{
	// if we had been split screen, toss the extra players now
	// remove every player, back to front, except the first one
	while (LocalPlayers.Num() > 1)
	{
		ULocalPlayer* const PlayerToRemove = LocalPlayers.Last();
		RemoveExistingLocalPlayer(PlayerToRemove);
	}
}

FReply UShooterGameInstance::OnPairingUsePreviousProfile()
{
	return FReply::Handled();
}

FReply UShooterGameInstance::OnPairingUseNewProfile()
{
	HandleSignInChangeMessaging();
	return FReply::Handled();
}

void UShooterGameInstance::HandleControllerPairingChanged( int GameUserIndex, const FUniqueNetId& PreviousUser, const FUniqueNetId& NewUser )
{
	UE_LOG(LogOnlineGame, Log, TEXT("UShooterGameInstance::HandleControllerPairingChanged GameUserIndex %d PreviousUser '%s' NewUser '%s'"),
		GameUserIndex, *PreviousUser.ToString(), *NewUser.ToString());
	
	if ( CurrentState == ShooterGameInstanceState::LoginScreen )
	{
		// Don't care about pairing changes at login screen
		return;
	}

#if SHOOTER_CONSOLE_UI && PLATFORM_XBOXONE
	if ( IgnorePairingChangeForControllerId != -1 && GameUserIndex == IgnorePairingChangeForControllerId )
	{
		// We were told to ignore
		IgnorePairingChangeForControllerId = -1;	// Reset now so there there is no chance this remains in a bad state
		return;
	}

	if ( PreviousUser.IsValid() && !NewUser.IsValid() )
	{
		// Treat this as a disconnect or signout, which is handled somewhere else
		return;
	}

	if ( !PreviousUser.IsValid() && NewUser.IsValid() )
	{
		// Treat this as a signin
		ULocalPlayer * ControlledLocalPlayer = FindLocalPlayerFromControllerId( GameUserIndex );

		if ( ControlledLocalPlayer != NULL && !ControlledLocalPlayer->GetCachedUniqueNetId().IsValid() )
		{
			// If a player that previously selected "continue without saving" signs into this controller, move them back to login screen
			HandleSignInChangeMessaging();
		}
		
		return;
	}

	// Find the local player currently being controlled by this controller
	ULocalPlayer * ControlledLocalPlayer	= FindLocalPlayerFromControllerId( GameUserIndex );

	// See if the newly assigned profile is in our local player list
	ULocalPlayer * NewLocalPlayer			= FindLocalPlayerFromUniqueNetId( NewUser );

	// If the local player being controlled is not the target of the pairing change, then give them a chance 
	// to continue controlling the old player with this controller
	if ( ControlledLocalPlayer != nullptr && ControlledLocalPlayer != NewLocalPlayer )
	{
		// Controller is paired to another profile
	}
#endif
}

void UShooterGameInstance::HandleControllerConnectionChange( bool bIsConnection, int32 Unused, int32 GameUserIndex )
{
	UE_LOG(LogOnlineGame, Log, TEXT("UShooterGameInstance::HandleControllerConnectionChange bIsConnection %d GameUserIndex %d"),
		bIsConnection, GameUserIndex);

	if(!bIsConnection)
	{
		// Controller was disconnected

		// Find the local player associated with this user index
		ULocalPlayer * LocalPlayer = FindLocalPlayerFromControllerId( GameUserIndex );

		if ( LocalPlayer == NULL )
		{
			return;		// We don't care about players we aren't tracking
		}

		// Invalidate this local player's controller id.
		LocalPlayer->SetControllerId(-1);
	}
}

FReply UShooterGameInstance::OnControllerReconnectConfirm()
{
	return FReply::Handled();
}

TSharedPtr< const FUniqueNetId > UShooterGameInstance::GetUniqueNetIdFromControllerId( const int ControllerId )
{
	IOnlineIdentityPtr OnlineIdentityInt = Online::GetIdentityInterface();

	if ( OnlineIdentityInt.IsValid() )
	{
		TSharedPtr<const FUniqueNetId> UniqueId = OnlineIdentityInt->GetUniquePlayerId( ControllerId );

		if ( UniqueId.IsValid() )
		{
			return UniqueId;
		}
	}

	return nullptr;
}

void UShooterGameInstance::SetOnlineMode(EOnlineMode InOnlineMode)
{
	OnlineMode = InOnlineMode;
	UpdateUsingMultiplayerFeatures(InOnlineMode == EOnlineMode::Online);
}

void UShooterGameInstance::UpdateUsingMultiplayerFeatures(bool bIsUsingMultiplayerFeatures)
{
	IOnlineSubsystem* OnlineSub = IOnlineSubsystem::Get();

	if (OnlineSub)
	{
		for (int32 i = 0; i < LocalPlayers.Num(); ++i)
		{
			ULocalPlayer* LocalPlayer = LocalPlayers[i];

			FUniqueNetIdRepl PlayerId = LocalPlayer->GetPreferredUniqueNetId();
			if (PlayerId.IsValid())
			{
				OnlineSub->SetUsingMultiplayerFeatures(*PlayerId, bIsUsingMultiplayerFeatures);
			}
		}
	}
}

void UShooterGameInstance::TravelToSession(const FName& SessionName)
{
	// Added to handle failures when joining using quickmatch (handles issue of joining a game that just ended, i.e. during game ending timer)
	AddNetworkFailureHandlers();
	ShowLoadingScreen();
	GotoState(ShooterGameInstanceState::Playing);
	InternalTravelToSession(SessionName);
}

void UShooterGameInstance::SetIgnorePairingChangeForControllerId( const int32 ControllerId )
{
	IgnorePairingChangeForControllerId = ControllerId;
}

bool UShooterGameInstance::IsLocalPlayerOnline(ULocalPlayer* LocalPlayer)
{
	if (LocalPlayer == NULL)
	{
		return false;
	}
	const auto OnlineSub = IOnlineSubsystem::Get();
	if(OnlineSub)
	{
		const auto IdentityInterface = OnlineSub->GetIdentityInterface();
		if(IdentityInterface.IsValid())
		{
			auto UniqueId = LocalPlayer->GetCachedUniqueNetId();
			if (UniqueId.IsValid())
			{
				const auto LoginStatus = IdentityInterface->GetLoginStatus(*UniqueId);
				if(LoginStatus == ELoginStatus::LoggedIn)
				{
					return true;
				}
			}
		}
	}

	return false;
}

bool UShooterGameInstance::IsLocalPlayerSignedIn(ULocalPlayer* LocalPlayer)
{
	if (LocalPlayer == NULL)
	{
		return false;
	}

	const auto OnlineSub = IOnlineSubsystem::Get();
	if (OnlineSub)
	{
		const auto IdentityInterface = OnlineSub->GetIdentityInterface();
		if (IdentityInterface.IsValid())
		{
			auto UniqueId = LocalPlayer->GetCachedUniqueNetId();
			if (UniqueId.IsValid())
			{
				return true;
			}
		}
	}

	return false;
}

bool UShooterGameInstance::ValidatePlayerForOnlinePlay(ULocalPlayer* LocalPlayer)
{
#if PLATFORM_XBOXONE
	if (CurrentConnectionStatus != EOnlineServerConnectionStatus::Connected)
	{
		// Don't let them play online if they aren't connected to Xbox LIVE
		return false;
	}
#endif

	if (!IsLocalPlayerOnline(LocalPlayer))
	{
		// Don't let them play online if they aren't online
		return false;
	}

	return true;
}

bool UShooterGameInstance::ValidatePlayerIsSignedIn(ULocalPlayer* LocalPlayer)
{
	if (!IsLocalPlayerSignedIn(LocalPlayer))
	{
		// Don't let them play online if they aren't online
		return false;
	}

	return true;
}

void UShooterGameInstance::StartOnlinePrivilegeTask(const IOnlineIdentity::FOnGetUserPrivilegeCompleteDelegate& Delegate, EUserPrivileges::Type Privilege, TSharedPtr< const FUniqueNetId > UserId)
{
	WaitMessageWidget = SNew(SShooterWaitDialog)
		.MessageText(NSLOCTEXT("NetworkStatus", "CheckingPrivilegesWithServer", "Checking privileges with server.  Please wait..."));

	if (GEngine && GEngine->GameViewport)
	{
		UGameViewportClient* const GVC = GEngine->GameViewport;
		GVC->AddViewportWidgetContent(WaitMessageWidget.ToSharedRef());
	}

	auto Identity = Online::GetIdentityInterface();
	if (Identity.IsValid() && UserId.IsValid())
	{		
		Identity->GetUserPrivilege(*UserId, Privilege, Delegate);
	}
	else
	{
		// Can only get away with faking the UniqueNetId here because the delegates don't use it
		Delegate.ExecuteIfBound(FUniqueNetIdString(), Privilege, (uint32)IOnlineIdentity::EPrivilegeResults::NoFailures);
	}
}

void UShooterGameInstance::CleanupOnlinePrivilegeTask()
{
	if (GEngine && GEngine->GameViewport && WaitMessageWidget.IsValid())
	{
		UGameViewportClient* const GVC = GEngine->GameViewport;
		GVC->RemoveViewportWidgetContent(WaitMessageWidget.ToSharedRef());
	}
}

void UShooterGameInstance::DisplayOnlinePrivilegeFailureDialogs(const FUniqueNetId& UserId, EUserPrivileges::Type Privilege, uint32 PrivilegeResults)
{	
	// Show warning that the user cannot play due to age restrictions
	TWeakObjectPtr<ULocalPlayer> OwningPlayer;
	if (GEngine)
	{
		for (auto It = GEngine->GetLocalPlayerIterator(GetWorld()); It; ++It)
		{
			FUniqueNetIdRepl OtherId = (*It)->GetPreferredUniqueNetId();
			if (OtherId.IsValid())
			{
				if (UserId == (*OtherId))
				{
					OwningPlayer = *It;
				}
			}
		}
	}
	
	if (OwningPlayer.IsValid())
	{
		if ((PrivilegeResults & (uint32)IOnlineIdentity::EPrivilegeResults::AccountTypeFailure) != 0)
		{
			IOnlineExternalUIPtr ExternalUI = Online::GetExternalUIInterface();
			if (ExternalUI.IsValid())
			{
				ExternalUI->ShowAccountUpgradeUI(UserId);
			}
		}
		else if ((PrivilegeResults & (uint32)IOnlineIdentity::EPrivilegeResults::RequiredSystemUpdate) != 0)
		{
			
		}
		else if ((PrivilegeResults & (uint32)IOnlineIdentity::EPrivilegeResults::RequiredPatchAvailable) != 0)
		{
			
		}
		else if ((PrivilegeResults & (uint32)IOnlineIdentity::EPrivilegeResults::AgeRestrictionFailure) != 0)
		{
			
		}
		else if ((PrivilegeResults & (uint32)IOnlineIdentity::EPrivilegeResults::UserNotFound) != 0)
		{
			
		}
		else if ((PrivilegeResults & (uint32)IOnlineIdentity::EPrivilegeResults::GenericFailure) != 0)
		{
			
		}
	}
}

void UShooterGameInstance::OnRegisterLocalPlayerComplete(const FUniqueNetId& PlayerId, EOnJoinSessionCompleteResult::Type Result)
{
	FinishSessionCreation(Result);
}

void UShooterGameInstance::FinishSessionCreation(EOnJoinSessionCompleteResult::Type Result)
{
	if (Result == EOnJoinSessionCompleteResult::Success)
	{
		// This will send any Play Together invites if necessary, or do nothing.
		SendPlayTogetherInvites();

		// Travel to the specified match URL
		GetWorld()->ServerTravel(TravelURL);
	}
	else
	{
		FText ReturnReason = NSLOCTEXT("NetworkErrors", "CreateSessionFailed", "Failed to create session.");
		FText OKButton = NSLOCTEXT("DialogButtons", "OKAY", "OK");
		ShowMessageThenGoMain(ReturnReason, OKButton, FText::GetEmpty());
	}
}

FString UShooterGameInstance::GetQuickMatchUrl()
{
	static const FString QuickMatchUrl(TEXT("/Game/Maps/AlienLab/Lab_Deathmatch?game=TDM?listen"));
	return QuickMatchUrl;
}

void UShooterGameInstance::BeginHostingQuickMatch()
{
	ShowLoadingScreen();
	GotoState(ShooterGameInstanceState::Playing);

	// Travel to the specified match URL
	GetWorld()->ServerTravel(GetQuickMatchUrl());	
}

void UShooterGameInstance::OnPlayTogetherEventReceived(const int32 UserIndex, const TArray<TSharedPtr<const FUniqueNetId>>& UserIdList)
{
	PlayTogetherInfo = FShooterPlayTogetherInfo(UserIndex, UserIdList);

	const IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	check(OnlineSub);

	/*const IOnlineSessionPtr SessionInterface = OnlineSub->GetSessionInterface();
	check(SessionInterface.IsValid());

	// If we have available slots to accomedate the whole party in our current sessions, we should send invites to the existing one
	// instead of a new one according to Sony's best practices.
	const FNamedOnlineSession* const Session = SessionInterface->GetNamedSession(NAME_GameSession);
	
	if (Session != nullptr && Session->NumOpenPrivateConnections + Session->NumOpenPublicConnections >= UserIdList.Num())
	{
		SendPlayTogetherInvites();
	}
	// Always handle Play Together in the main menu since the player has session customization options.
	else if (CurrentState == ShooterGameInstanceState::MainMenu)
	{
		MainMenuUI->OnPlayTogetherEventReceived();
	}
	else if (CurrentState == ShooterGameInstanceState::LoginScreen)
	{
		StartOnlinePrivilegeTask(IOnlineIdentity::FOnGetUserPrivilegeCompleteDelegate::CreateUObject(this, &UShooterGameInstance::OnUserCanPlayTogether), EUserPrivileges::CanPlayOnline, PendingInvite.UserId);
	}
	else
	{
		GotoState(ShooterGameInstanceState::MainMenu);
	}*/
}

void UShooterGameInstance::SendPlayTogetherInvites()
{
	const IOnlineSubsystem* const OnlineSub = IOnlineSubsystem::Get();
	check(OnlineSub);

	/*const IOnlineSessionPtr SessionInterface = OnlineSub->GetSessionInterface();
	check(SessionInterface.IsValid());

	if (PlayTogetherInfo.UserIndex != -1)
	{
		for (const ULocalPlayer* LocalPlayer : LocalPlayers)
		{
			if (LocalPlayer->GetControllerId() == PlayTogetherInfo.UserIndex)
			{
				FUniqueNetIdRepl PlayerId = LocalPlayer->GetPreferredUniqueNetId();
				if (PlayerId.IsValid())
				{
					// Automatically send invites to friends in the player's PS4 party to conform with Play Together requirements
					for (const TSharedPtr<const FUniqueNetId>& FriendId : PlayTogetherInfo.UserIdList)
					{
						SessionInterface->SendSessionInviteToFriend(*PlayerId, NAME_GameSession, *FriendId.ToSharedRef());
					}
				}

			}
		}

		PlayTogetherInfo = FShooterPlayTogetherInfo();
	}*/
}

void UShooterGameInstance::ReceivedNetworkEncryptionToken(const FString& EncryptionToken, const FOnEncryptionKeyResponse& Delegate)
{
	// This is a simple implementation to demonstrate using encryption for game traffic using a hardcoded key.
	// For a complete implementation, you would likely want to retrieve the encryption key from a secure source,
	// such as from a web service over HTTPS. This could be done in this function, even asynchronously - just
	// call the response delegate passed in once the key is known. The contents of the EncryptionToken is up to the user,
	// but it will generally contain information used to generate a unique encryption key, such as a user and/or session ID.

	FEncryptionKeyResponse Response(EEncryptionResponse::Failure, TEXT("Unknown encryption failure"));

	if (EncryptionToken.IsEmpty())
	{
		Response.Response = EEncryptionResponse::InvalidToken;
		Response.ErrorMsg = TEXT("Encryption token is empty.");
	}
	else
	{
		Response.Response = EEncryptionResponse::Success;
		Response.EncryptionKey = DebugTestEncryptionKey;
	}

	Delegate.ExecuteIfBound(Response);

}

void UShooterGameInstance::ReceivedNetworkEncryptionAck(const FOnEncryptionKeyResponse& Delegate)
{
	// This is a simple implementation to demonstrate using encryption for game traffic using a hardcoded key.
	// For a complete implementation, you would likely want to retrieve the encryption key from a secure source,
	// such as from a web service over HTTPS. This could be done in this function, even asynchronously - just
	// call the response delegate passed in once the key is known.

	FEncryptionKeyResponse Response;

	TArray<uint8> FakeKey;
	
	Response.Response = EEncryptionResponse::Success;
	Response.EncryptionKey = DebugTestEncryptionKey;

	Delegate.ExecuteIfBound(Response);
}
