//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013-2015 SuperTuxKart-Team
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "network/game_setup.hpp"

#include "config/player_manager.hpp"
#include "config/user_config.hpp"
#include "karts/abstract_kart.hpp"
#include "modes/world.hpp"
#include "network/network_config.hpp"
#include "network/network_player_profile.hpp"
#include "network/protocols/game_events_protocol.hpp"
#include "network/stk_host.hpp"
#include "race/race_manager.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <random>

//-----------------------------------------------------------------------------
/** Update and see if any player disconnects.
 *  \param remove_disconnected_players remove the disconnected players,
 *  otherwise eliminate the kart in world, so this function must be called
 *  in main thread.
 */
void GameSetup::update(bool remove_disconnected_players)
{
    std::unique_lock<std::mutex> lock(m_players_mutex);
    if (remove_disconnected_players)
    {
        m_players.erase(std::remove_if(m_players.begin(), m_players.end(), []
            (const std::weak_ptr<NetworkPlayerProfile> npp)->bool
            {
                return npp.expired();
            }), m_players.end());
        return;
    }
    lock.unlock();
    if (!World::getWorld() ||
        World::getWorld()->getPhase() < WorldStatus::MUSIC_PHASE)
        return;
    int red_count = 0;
    int blue_count = 0;
    for (uint8_t i = 0; i < (uint8_t)m_players.size(); i++)
    {
        bool disconnected = m_players[i].expired();
        if (race_manager->getKartInfo(i).getSoccerTeam() == SOCCER_TEAM_RED &&
            !disconnected)
            red_count++;
        else if (race_manager->getKartInfo(i).getSoccerTeam() ==
            SOCCER_TEAM_BLUE && !disconnected)
            blue_count++;

        if (!disconnected)
            continue;
        AbstractKart* k = World::getWorld()->getKart(i);
        if (!k->isEliminated())
        {
            World::getWorld()->eliminateKart(i,
                false/*notify_of_elimination*/);
            k->setPosition(
                World::getWorld()->getCurrentNumKarts() + 1);
            k->finishedRace(World::getWorld()->getTime());
            NetworkString p(PROTOCOL_GAME_EVENTS);
            p.setSynchronous(true);
            p.addUInt8(GameEventsProtocol::GE_PLAYER_DISCONNECT).addUInt8(i);
            STKHost::get()->sendPacketToAllPeers(&p, true);
        }
    }
    if (m_players.size() != 1 && World::getWorld()->hasTeam() &&
        (red_count == 0 || blue_count == 0))
        World::getWorld()->setUnfairTeam(true);
}   // removePlayer

//-----------------------------------------------------------------------------
void GameSetup::loadWorld()
{
    // Notice: for arena (battle / soccer) lap and reverse will be mapped to
    // goals / time limit and random item location
    assert(!m_tracks.empty());
    // Disable accidentally unlocking of a challenge
    if (PlayerManager::getCurrentPlayer())
        PlayerManager::getCurrentPlayer()->setCurrentChallenge("");
    race_manager->setTimeTarget(0.0f);
    if (race_manager->getMinorMode() == RaceManager::MINOR_MODE_SOCCER ||
        race_manager->getMinorMode() == RaceManager::MINOR_MODE_BATTLE)
    {
        const bool is_ctf = race_manager->getMajorMode() ==
            RaceManager::MAJOR_MODE_CAPTURE_THE_FLAG;
        bool prev_val = UserConfigParams::m_random_arena_item;
        if (is_ctf)
            UserConfigParams::m_random_arena_item = false;
        else
            UserConfigParams::m_random_arena_item = m_reverse;

        race_manager->setReverseTrack(false);
        if (race_manager->getMinorMode() == RaceManager::MINOR_MODE_SOCCER)
        {
            if (isSoccerGoalTarget())
                race_manager->setMaxGoal(m_laps);
            else
                race_manager->setTimeTarget((float)m_laps * 60.0f);
        }
        else
        {
            race_manager->setHitCaptureTime(m_hit_capture_limit,
                m_battle_time_limit);
        }
        race_manager->startSingleRace(m_tracks.back(), -1,
            false/*from_overworld*/);
        UserConfigParams::m_random_arena_item = prev_val;
    }
    else
    {
        race_manager->setReverseTrack(m_reverse);
        race_manager->startSingleRace(m_tracks.back(), m_laps,
            false/*from_overworld*/);
    }
}   // loadWorld

//-----------------------------------------------------------------------------
bool GameSetup::isGrandPrix() const
{
    return m_extra_server_info != -1 &&
        NetworkConfig::get()->getLocalGameMode().second ==
        RaceManager::MAJOR_MODE_GRAND_PRIX;
}   // isGrandPrix

//-----------------------------------------------------------------------------
void GameSetup::addServerInfo(NetworkString* ns)
{
    assert(NetworkConfig::get()->isServer());
    ns->encodeString(NetworkConfig::get()->getServerName());
    ns->addUInt8(race_manager->getDifficulty())
        .addUInt8((uint8_t)NetworkConfig::get()->getMaxPlayers())
        .addUInt8((uint8_t)NetworkConfig::get()->getServerMode());
    if (hasExtraSeverInfo())
    {
        if (isGrandPrix())
        {
            uint8_t cur_track = (uint8_t)m_tracks.size();
            if (!isGrandPrixStarted())
                cur_track = 0;
            ns->addUInt8((uint8_t)2).addUInt8(cur_track)
                .addUInt8(getExtraServerInfo());
        }
        else
        {
            // Soccer mode
            ns->addUInt8((uint8_t)1).addUInt8(getExtraServerInfo());
        }
    }
    else
    {
        // No extra server info
        ns->addUInt8((uint8_t)0);
    }
    if (NetworkConfig::get()->isOwnerLess())
    {
        ns->addFloat(UserConfigParams::m_start_game_threshold)
            .addFloat(UserConfigParams::m_start_game_counter);
    }
    else
        ns->addFloat(0.0f).addFloat(0.0f);
    ns->addUInt8(NetworkConfig::get()->getMaxPlayers());

    ns->encodeString16(NetworkConfig::get()->getMOTD());
}   // addServerInfo

//-----------------------------------------------------------------------------
void GameSetup::sortPlayersForGrandPrix()
{
    if (!isGrandPrix())
        return;
    std::lock_guard<std::mutex> lock(m_players_mutex);

    if (m_tracks.size() == 1)
    {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(m_players.begin(), m_players.end(), g);
        return;
    }

    std::sort(m_players.begin(), m_players.end(),
        [](const std::weak_ptr<NetworkPlayerProfile>& a,
        const std::weak_ptr<NetworkPlayerProfile>& b)
        {
            // They should be never expired
            auto c = a.lock();
            assert(c);
            auto d = b.lock();
            assert(d);
            return (c->getScore() < d->getScore()) ||
                (c->getScore() == d->getScore() &&
                c->getOverallTime() > d->getOverallTime());
        });
    if (UserConfigParams::m_gp_most_points_first)
    {
        std::reverse(m_players.begin(), m_players.end());
    }
}   // sortPlayersForGrandPrix

//-----------------------------------------------------------------------------
void GameSetup::sortPlayersForSoccer()
{
    if (race_manager->getMinorMode() != RaceManager::MINOR_MODE_SOCCER ||
        NetworkConfig::get()->hasTeamChoosing())
        return;
    std::lock_guard<std::mutex> lock(m_players_mutex);
    for (unsigned i = 0; i < m_players.size(); i++)
    {
        auto player = m_players[i].lock();
        assert(player);
        player->setTeam((SoccerTeam)(i % 2));
    }
}   // sortPlayersForSoccer
