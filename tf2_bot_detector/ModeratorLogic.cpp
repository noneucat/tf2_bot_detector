#include "ModeratorLogic.h"
#include "Actions.h"
#include "ActionManager.h"
#include "Log.h"
#include "PlayerListJSON.h"
#include "Settings.h"
#include "WorldState.h"

#include <mh/text/string_insertion.hpp>

#include <iomanip>

using namespace tf2_bot_detector;
using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

void ModeratorLogic::OnPlayerStatusUpdate(WorldState& world, const IPlayer& player)
{
	ProcessDelayedBans(world.GetCurrentTime(), player);

	const auto name = player.GetName();
	const auto steamID = player.GetSteamID();

	if (name.find("MYG)T"sv) != name.npos)
	{
		if (SetPlayerAttribute(player, PlayerAttributes::Cheater))
			Log("Marked "s << steamID << " as a cheater due to name (mygot advertisement)");
	}
	if (name.ends_with("\xE2\x80\x8F"sv))
	{
		if (SetPlayerAttribute(player, PlayerAttributes::Cheater))
			Log("Marked "s << steamID << " as cheater due to name ending in common name-stealing characters");
	}
}

void ModeratorLogic::HandleFriendlyCheaters(uint8_t friendlyPlayerCount, const std::vector<const IPlayer*>& friendlyCheaters)
{
	if (friendlyCheaters.empty())
		return; // Nothing to do

	if ((friendlyPlayerCount / 2) <= friendlyCheaters.size())
	{
		Log("Impossible to pass a successful votekick against "s << friendlyCheaters.size()
			<< " friendly cheaters, but we're trying anyway :/", { 1, 0.5f, 0 });
	}

	// Votekick the first one that is actually connected
	for (const IPlayer* cheater : friendlyCheaters)
	{
		if (cheater->GetConnectionState() == PlayerStatusState::Active)
		{
			if (InitiateVotekick(*cheater, KickReason::Cheating))
				break;
		}
	}
}

void ModeratorLogic::HandleEnemyCheaters(uint8_t enemyPlayerCount,
	const std::vector<const IPlayer*>& enemyCheaters, const std::vector<IPlayer*>& connectingEnemyCheaters)
{
	if (enemyCheaters.empty() && connectingEnemyCheaters.empty())
		return;

	if (const auto cheaterCount = (enemyCheaters.size() + connectingEnemyCheaters.size()); (enemyPlayerCount / 2) <= cheaterCount)
	{
		Log("Impossible to pass a successful votekick against "s << cheaterCount << " enemy cheaters. Skipping all warnings.");
		return;
	}

	if (!enemyCheaters.empty())
	{
		// There are enough people on the other team to votekick the cheater(s)
		std::string logMsg;
		logMsg << "Telling the other team about " << enemyCheaters.size() << " cheater(s) named ";

		std::string chatMsg;
		chatMsg << "Attention! There ";
		if (enemyCheaters.size() == 1)
			chatMsg << "is a cheater ";
		else
			chatMsg << "are " << enemyCheaters.size() << " cheaters ";

		chatMsg << "on the other team named ";
		for (size_t i = 0; i < enemyCheaters.size(); i++)
		{
			const IPlayer* cheaterData = enemyCheaters[i];
			if (cheaterData->GetName().empty())
				continue; // Theoretically this should never happen, but don't embarass ourselves

			if (i != 0)
			{
				chatMsg << ", ";
				logMsg << ", ";
			}

			chatMsg << std::quoted(cheaterData->GetName());
			logMsg << std::quoted(cheaterData->GetName()) << " (" << enemyCheaters[i] << ')';
		}

		chatMsg << ". Please kick them!";

		if (const auto now = m_World->GetCurrentTime(); (now - m_LastCheaterWarningTime) > 10s)
		{
			if (m_ActionManager->QueueAction<ChatMessageAction>(chatMsg))
			{
				Log(logMsg, { 1, 0, 0, 1 });
				m_LastCheaterWarningTime = now;
			}
		}
	}
	else if (!connectingEnemyCheaters.empty())
	{
		bool needsWarning = false;
		for (const IPlayer* cheater : connectingEnemyCheaters)
		{
			auto cheaterData = cheater->GetData<PlayerExtraData>();
			if (cheaterData && !cheaterData->m_PreWarnedOtherTeam)
			{
				needsWarning = true;
				break;
			}
		}

		if (needsWarning)
		{
			std::string chatMsg;
			chatMsg << "Heads up! There ";
			if (connectingEnemyCheaters.size() == 1)
				chatMsg << "is a known cheater ";
			else
				chatMsg << "are " << connectingEnemyCheaters.size() << " known cheaters ";

			chatMsg << "joining the other team! Name";
			if (connectingEnemyCheaters.size() > 1)
				chatMsg << 's';

			chatMsg << " unknown until they fully join.";

			Log("Telling other team about "s << connectingEnemyCheaters.size() << " cheaters currently connecting");
			if (m_ActionManager->QueueAction<ChatMessageAction>(chatMsg))
			{
				for (IPlayer* cheater : connectingEnemyCheaters)
					cheater->GetOrCreateData<PlayerExtraData>().m_PreWarnedOtherTeam = true;
			}
		}
	}
}

void ModeratorLogic::ProcessPlayerActions()
{
	const auto now = m_World->GetCurrentTime();
	if ((now - m_LastPlayerActionsUpdate) < 1s)
	{
		return;
	}
	else
	{
		m_LastPlayerActionsUpdate = now;
	}

	// Don't process actions if we're way out of date
	[[maybe_unused]] const auto dbgDeltaTime = to_seconds(clock_t::now() - now);
	if ((clock_t::now() - now) > 15s)
		return;

	const auto myTeam = TryGetMyTeam();
	if (!myTeam)
		return; // We don't know what team we're on, so we can't really take any actions.

	uint8_t totalEnemyPlayers = 0;
	uint8_t totalFriendlyPlayers = 0;
	std::vector<const IPlayer*> enemyCheaters;
	std::vector<const IPlayer*> friendlyCheaters;
	std::vector<IPlayer*> connectingEnemyCheaters;

	m_World->ForEachLobbyMember([&](const LobbyMember& lobbyMember)
		{
			IPlayer* playerPtr = m_World->FindPlayer(lobbyMember.m_SteamID);
			assert(playerPtr);
			IPlayer& player = *playerPtr;

			const auto teamShareResult = m_World->GetTeamShareResult(*myTeam, lobbyMember.m_SteamID);
			switch (teamShareResult)
			{
			case TeamShareResult::SameTeams:      totalFriendlyPlayers++; break;
			case TeamShareResult::OppositeTeams:  totalEnemyPlayers++; break;
			}

			if (m_PlayerList.HasPlayerAttribute(lobbyMember.m_SteamID, PlayerAttributes::Cheater))
			{
				switch (teamShareResult)
				{
				case TeamShareResult::SameTeams:
					friendlyCheaters.push_back(&player);
					break;
				case TeamShareResult::OppositeTeams:
					if (!player.GetName().empty())
						connectingEnemyCheaters.push_back(&player);
					else
						enemyCheaters.push_back(&player);

					break;
				}
			}
		});

	HandleEnemyCheaters(totalEnemyPlayers, enemyCheaters, connectingEnemyCheaters);
	HandleFriendlyCheaters(totalFriendlyPlayers, friendlyCheaters);
}

bool ModeratorLogic::SetPlayerAttribute(const IPlayer& player, PlayerAttributes attribute, bool set)
{
	bool attributeChanged = false;

	m_PlayerList.ModifyPlayer(player.GetSteamID(), [&](PlayerListData& data)
		{
			data.m_Attributes.SetAttribute(attribute, set);

			if (!data.m_LastSeen)
				data.m_LastSeen.emplace();

			data.m_LastSeen->m_Time = m_World->GetCurrentTime();

			if (const auto& name = player.GetName(); !name.empty())
				data.m_LastSeen->m_PlayerName = name;

			return ModifyPlayerAction::Modified;
		});

	return attributeChanged;
}

std::optional<LobbyMemberTeam> ModeratorLogic::TryGetMyTeam() const
{
	return m_World->FindLobbyMemberTeam(m_Settings->m_LocalSteamID);
}

bool ModeratorLogic::InitiateVotekick(const IPlayer& player, KickReason reason)
{
	const auto userID = player.GetUserID();
	if (!userID)
	{
		Log("Wanted to kick "s << player << ", but could not find userid");
		return false;
	}

	if (m_ActionManager->QueueAction<KickAction>(userID.value(), reason))
		Log("InitiateVotekick on "s << player << ": " << reason);

	return true;
}

void ModeratorLogic::ProcessDelayedBans(time_point_t timestamp, const IPlayer& updatedStatus)
{
	for (size_t i = 0; i < m_DelayedBans.size(); i++)
	{
		const auto& ban = m_DelayedBans[i];
		const auto timeSince = timestamp - ban.m_Timestamp;

		const auto RemoveBan = [&]()
		{
			m_DelayedBans.erase(m_DelayedBans.begin() + i);
			i--;
		};

		if (timeSince > 10s)
		{
			RemoveBan();
			Log("Expiring delayed ban for user with name "s << std::quoted(ban.m_PlayerName)
				<< " (" << to_seconds(timeSince) << " second delay)");
			continue;
		}

		if (ban.m_PlayerName == updatedStatus.GetName())
		{
			if (SetPlayerAttribute(updatedStatus, PlayerAttributes::Cheater))
			{
				Log("Applying delayed ban ("s << to_seconds(timeSince) << " second delay) to player " << updatedStatus);
			}

			RemoveBan();
			break;
		}
	}
}