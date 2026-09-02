/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2024 Story Of Alicia
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 **/

#include "server/race/MagicSystem.hpp"
#include "libserver/util/QuietLog.hpp"
#include "server/race/RaceNetworkHandler.hpp"

#include "libserver/util/Cleanup.hpp"

#include "server/ServerInstance.hpp"

#include <libserver/data/helper/ProtocolHelper.hpp>
#include <libserver/util/Util.hpp>

#include <boost/container_hash/hash.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <limits>
#include <ranges>
#include <algorithm> // std::ranges::all_of / none_of — сегодня приезжает транзитивно (ревью N3)
#include <cmath>     // std::isfinite

namespace server
{

RaceNetworkHandler::RaceNetworkHandler(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
  , _commandServer(*this)
{
  _commandServer.RegisterCommandHandler<protocol::AcCmdCREnterRoom>(
    [this](ClientId clientId, const auto& message)
    {
      HandleEnterRoom(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChangeRoomOptions>(
    [this](ClientId clientId, const auto& message)
    {
      HandleChangeRoomOptions(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChangeTeam>(
    [this](ClientId clientId, const auto& message)
    {
      HandleChangeTeam(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRLeaveRoom>(
    [this](ClientId clientId, const auto&)
    {
      HandleLeaveRoom(clientId);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRStartRace>(
    [this](ClientId clientId, const auto& message)
    {
      HandleStartRace(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceTimer>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRaceTimer(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRLoadingComplete>(
    [this](ClientId clientId, const auto& message)
    {
      HandleLoadingComplete(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRReadyRace>(
    [this](ClientId clientId, const auto& message)
    {
      HandleReadyRace(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceFinal>(
    [this](ClientId clientId, const auto& message)
    {
      HandleUserRaceFinal(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRaceResult>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRaceResult(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRP2PResult>(
    [this](ClientId clientId, const auto& message)
    {
      HandleP2PRaceResult(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceP2PResult>(
    [this](ClientId clientId, const auto& message)
    {
      HandleP2PUserRaceResult(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRAwardStart>(
    [this](ClientId clientId, const auto& message)
    {
      HandleAwardStart(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRAwardEnd>(
    [this](ClientId clientId, const auto& message)
    {
      HandleAwardEnd(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRStarPointGet>(
    [this](ClientId clientId, const auto& message)
    {
      HandleStarPointGet(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestSpur>(
    [this](ClientId clientId, const auto& message)
    {
      // LOA-fix (R57-3c, round57, backlog #195): хвост зовём ТОЛЬКО если шпора
      // была своя. Пакет за бота обязан не дойти ни до шкалы, ни до чего-либо
      // ещё — раньше это обеспечивал бросок, теперь обеспечивает контракт.
      if (HandleRequestSpur(clientId, message))
        HandleTeamGauge(clientId);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRHurdleClearResult>(
    [this](ClientId clientId, const auto& message)
    {
      HandleHurdleClearResult(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRStartingRate>(
    [this](ClientId clientId, const auto& message)
    {
      HandleStartingRate(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceUpdatePos>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRaceUserPos(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChat>(
    [this](ClientId clientId, const auto& message)
    {
      HandleChat(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRelayCommand>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRelayCommand(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRelay>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRelay(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceActivateInteractiveEvent>(
    [this](ClientId clientId, const auto& message)
    {
      HandleUserRaceActivateInteractiveEvent(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceActivateEvent>(
    [this](ClientId clientId, const auto& message)
     {
       HandleUserRaceActivateEvent(clientId, message);
     });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceDeactivateEvent>(
    [this](ClientId clientId, const auto& message)
    {
      HandleUserRaceDeactivateEvent(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestMagicItem>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRequestMagicItem(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUseMagicItem>(
    [this](ClientId clientId, const auto& message)
    {
      HandleUseMagicItem(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdUserRaceItemGet>(
    [this](ClientId clientId, const auto& message)
    {
      HandleUserRaceItemGet(clientId, message);
    });

  // Magic Targeting Commands for Bolt System
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRStartMagicTarget>(
    [this](ClientId clientId, const auto& message)
    {
      HandleStartMagicTarget(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChangeMagicTarget>(
    [this](ClientId clientId, const auto& message)
    {
      HandleChangeMagicTarget(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRActivateSkillEffect>(
    [this](ClientId clientId, const auto& message)
    {
      HandleActivateSkillEffect(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChangeSkillCardPresetID>(
    [this](ClientId clientId, const auto& message)
    {
      HandleChangeSkillCardPresetId(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCROpCmd>(
    [this](ClientId clientId, const auto& message)
    {
      HandleOpCmd(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRInviteUser>(
    [this](ClientId clientId, const auto& message)
    {
      HandleInviteUser(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestUser>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRequestUser(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRKick>(
    [this](ClientId clientId, const auto& message)
    {
      HandleKickUser(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRTriggerizeAct>(
    [this](ClientId clientId, const auto& message)
    {
      HandleTriggerizeAct(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRGameCreateClientItem>(
    [this](ClientId clientId, const auto& message)
    {
      HandleGameCreateClientItem(clientId, message);
    });

  // LOA-fix (R44-5, #58/R1): вторая половина замера из R44-4. Ранч-коннект на
  // время заезда закрыт, поэтому ВСЁ, что клиент репортит во время гонки
  // (скорость, заносы, прыжки), приходит сюда, а не на ранч. Условия те же:
  // только лог, троттл окном в секунду, никакого эффекта.
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRAchievementUpdateProperty>(
    [this](ClientId clientId, const auto& message)
    {
      auto& clientContext = GetClientContext(clientId, false);
      if (not clientContext.isAuthenticated)
        return;

      constexpr int64_t kAchievementProbeLinesPerSecond = 20;
      static std::atomic<int64_t> probeWindowSecond{0};
      static std::atomic<int64_t> probeWindowLines{0};

      const auto nowSecond = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
      int64_t windowSecond = probeWindowSecond.load();
      if (windowSecond != nowSecond
        && probeWindowSecond.compare_exchange_strong(windowSecond, nowSecond))
      {
        probeWindowLines.store(0);
      }
      if (probeWindowLines.fetch_add(1) >= kAchievementProbeLinesPerSecond)
        return;

      server::util::QuietLogInfo(
        "[achv-probe race] character {} event {} value '{}' ({} bytes, {}, {} dropped)",
        clientContext.characterUid,
        message.achievementEvent,
        message.propertyValue,
        message.propertyValue.size(),
        message.isPropertyValueTerminated ? "nul-ok" : "NO-NUL",
        message.rejectedPropertyValueBytes);
    });
}

void RaceNetworkHandler::Initialize()
{
  server::util::QuietLogDebug(
    "Race server listening on {}:{}",
    GetConfig().listen.address.to_string(),
    GetConfig().listen.port);

  _commandServer.BeginHost(GetConfig().listen.address, GetConfig().listen.port);
}

void RaceNetworkHandler::Terminate()
{
  _commandServer.EndHost();
}

void RaceNetworkHandler::Tick()
{
  try
  {
    _scheduler.Tick();
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogError("Exception ticking a race scheduler: {}", x.what());
  }

  std::scoped_lock lock(_raceInstancesMutex);
  for (auto& raceInstance : _raceInstances | std::views::values)
  {
    try
    {
      raceInstance.Tick();
    }
    catch (const std::exception& x)
    {
      server::util::QuietLogError("Exception ticking a race scheduler: {}", x.what());
    }
  }
}

void RaceNetworkHandler::NotifySummonCharacter(
    data::Uid characterUid,
    bool force,
    std::string characterName,
    uint32_t roomUid,
    uint32_t ranchUid) noexcept
{
  protocol::AcCmdRCRequestUser notify{};
  notify.force = force;
  notify.characterName = characterName;
  notify.roomUid = roomUid;
  notify.ranchUid = ranchUid;

  try
  {
     const auto targetClientId = GetClientIdByCharacterUid(characterUid);
     _commandServer.QueueCommand<protocol::AcCmdRCRequestUser>(targetClientId, [notify](){ return notify; });
  }
  catch(const std::exception&)
  {
    // Dont care if the client is not found, we just won't send the notification
  }
}

void RaceNetworkHandler::NotifyRoomNameChanged(
  const uint32_t roomUid) noexcept
{
  _serverInstance.GetRoomSystem().GetRoom(
    roomUid,
    [this](const Room& room)
    {
      const protocol::AcCmdCRChangeRoomOptionsNotify notify{
        .optionsBitfield = protocol::RoomOptionType::Name,
        .name = room.GetRoomSnapshot().details.name};

      for (const auto& player : room.GetPlayers() | std::views::values)
      {
        try
        {
          _commandServer.QueueCommand<protocol::AcCmdCRChangeRoomOptionsNotify>(
            player.GetClientId(),
            [notify]()
            {
              return notify;
            });
        }
        catch (const std::exception&)
        {
          // the player disconnected
        }
      }
    });
}

void RaceNetworkHandler::SendDailyQuestNotificationToCharacter(
  uint32_t characterUid,
  uint16_t questId,
  const protocol::ObjectiveProgress& objectiveProgress,
  uint32_t carrotsReward,
  protocol::QuestRewardType rewardType,
  uint32_t unk2,
  uint32_t mountExp)
{
  const protocol::AcCmdRCUpdateDailyQuestNotify updateNotify{
    .characterUid = characterUid,
    .questId = questId,
    .objectiveProgress = objectiveProgress,
    .carrotsReward = carrotsReward,
    .rewardType = rewardType,
    .unk2 = unk2,
    .mountExp = mountExp};

  try
  {
    const ClientId clientId = GetClientIdByCharacterUid(characterUid);
    _commandServer.QueueCommand<protocol::AcCmdRCUpdateDailyQuestNotify>(
      clientId,
      [updateNotify]()
      {
        return updateNotify;
      });
  }
  catch (const std::exception&)
  {
    // Ignore
  }
}

void RaceNetworkHandler::HandleClientConnected(ClientId clientId)
{
  _clients.try_emplace(clientId);

  server::util::QuietLogDebug(
    "Client {} connected to the race server from {}",
    clientId,
    _commandServer.GetClientAddress(clientId).to_string());
}

void RaceNetworkHandler::HandleClientDisconnected(ClientId clientId)
{
  // LOA-fix (R50-6, round50, backlog #180): уборка гонки доходит до конца.
  // `HandleLeaveRoom` бросает (комната, рассылки), а `_clients.erase` стоял
  // после него — осиротевший контекст оставался с `roomUid` навсегда.
  const util::RegistryEraser eraser{_clients, clientId};

  const auto& clientContext = GetClientContext(clientId, false);

  bool roomLeaveSucceeded = true;
  if (clientContext.isAuthenticated)
  {
    std::unique_lock lock(_raceInstancesMutex);
    const auto raceIter = _raceInstances.find(clientContext.roomUid);
    if (raceIter != _raceInstances.cend())
    {
      lock.unlock();
      roomLeaveSucceeded = util::RunCleanupStep(
        "race room leave",
        clientId,
        [&]()
        {
          HandleLeaveRoom(clientId);
        });
    }
  }

  // If client had a P2dId, erase it from client map and release it from the pool
  //
  // ★ЭТОТ ШАГ НАМЕРЕННО ЗАВИСИМЫЙ, в отличие от прочих. Слепая изоляция здесь
  // была бы хуже болезни: вернуть билет в пул, когда выход из комнаты НЕ
  // удался, значит выдать тот же билет другому клиенту, пока комната всё ещё
  // ссылается на прежнего. Видимая утечка билета честнее тихой коллизии,
  // поэтому при осечке билет остаётся занятым — и мы говорим об этом в лог.
  if (_p2dIds.contains(clientId))
  {
    if (roomLeaveSucceeded)
    {
      util::RunCleanupStep(
        "race p2d ticket release",
        clientId,
        [&]()
        {
          // Erase client P2dId and release it
          const race::P2dId p2dId = _p2dIds.at(clientId);
          _p2dIds.erase(clientId);
          _p2dIdPool.Release(p2dId);
        });
    }
    else
    {
      server::util::QuietLogWarn(
        "Keeping the p2d ticket of client {} reserved: its room leave failed, "
        "so the room may still reference it",
        clientId);
    }
  }

  server::util::QuietLogInfo("Client {} disconnected from the race server", clientId);
}

void RaceNetworkHandler::DisconnectCharacter(data::Uid characterUid)
{
  try
  {
    _commandServer.DisconnectClient(GetClientIdByCharacterUid(characterUid));
  }
  catch (const std::exception&)
  {
    // We really don't care.
  }
}

bool RaceNetworkHandler::IsCharacterLoadingRace(const data::Uid characterUid)
{
  if (characterUid == data::InvalidUid)
    return false;

  std::scoped_lock lock(_raceInstancesMutex);
  for (auto& raceInstance : _raceInstances | std::views::values)
  {
    if (raceInstance.GetStage() != RaceInstance::Stage::Loading)
      continue;

    auto& racers = raceInstance.GetTracker().GetRacers();
    const auto racerIter = racers.find(characterUid);
    if (racerIter == racers.cend())
      continue;

    // State::Loading ставится в HandleStartRace вместе с AddRacer и снимается
    // либо HandleLoadingComplete (Racing), либо TickLoading по дедлайну R11
    // (Disconnected). То есть «true» здесь = «клиент грузит карту прямо сейчас».
    return racerIter->second.state
      == tracker::RaceTracker::Racer::State::Loading;
  }

  return false;
}

size_t RaceNetworkHandler::GetRoomCount()
{
  std::scoped_lock lock(_raceInstancesMutex);
  return _raceInstances.size();
}

Config::Race& RaceNetworkHandler::GetConfig()
{
  return GetServerInstance().GetSettings().race;
}

ServerInstance& RaceNetworkHandler::GetServerInstance()
{
  return _serverInstance;
}

CommandServer& RaceNetworkHandler::GetCommandServer()
{
  return _commandServer;
}

uint16_t RaceNetworkHandler::GetOrCreateP2dId(ClientId clientId)
{
  const auto existingP2dIdIter = _p2dIds.find(clientId);
  if (existingP2dIdIter != _p2dIds.end())
    return existingP2dIdIter->second;

  const std::optional<race::P2dId> p2dId = _p2dIdPool.Acquire();
  if (not p2dId.has_value())
    throw std::runtime_error("P2dId pool has been exhausted.");

  _p2dIds.emplace(clientId, p2dId.value());
  return p2dId.value();
}

namespace
{

//! Ростер AI-соперников соло-заезда (R56, #61).
//!
//! ★Имена не выдуманы: они сверены с таблицами вождения САМОГО КЛИЕНТА.
//! `logic_aiparam.lua` объявляет 21 блок `SAI_*Param`, и корейские имена в
//! комментариях идут ровно этим порядком, поэтому индекс в списке и есть
//! `personality`, которым клиент выбирает блок.
//!
//! Пока заведён только «обычный» тир. Апстрим захардкодил сложность и НЕ
//! передавал её клиенту вовсе, а чем именно клиент выбирает тир — открытый
//! вопрос (индекс внутри тира против Type из `AIParam.csv`). Списки easy/hard
//! не заводим, пока это не проверено вживую: пустая заготовка честнее
//! неработающего переключателя.
constexpr const char* AiRacerNames[]{
  "Karim", "Eden", "Warren", "Tien", "Dains", "Glen", "Meirin"};

constexpr size_t AiRacerCount = sizeof(AiRacerNames) / sizeof(AiRacerNames[0]);

//! LOA-fix (R75, #14): ЕДИНОЕ определение «идёт сам заезд» для ВСЕХ
//! пер-заездных счётчиков раунда.
//!
//! ★ЗАЧЕМ ФУНКЦИЯ, А НЕ ПОВТОРЁННОЕ УСЛОВИЕ. `racer.state == Racing` НЕ
//! является признаком зелёного света: state выставляется по завершении
//! ЗАГРУЗКИ, а `GetRaceStartTimePoint()` лежит на waitTime карты в БУДУЩЕМ
//! (mapBlockId 1 -> 10 с обратного отсчёта). Именно поэтому телеметрия R24 уже
//! стоит под этим гейтом. Раунд заводит ДВА новых счётчика, которые уезжают в
//! ВЕЧНЫЕ поля лошади; повтори условие дважды — и завтра третий счётчик заведут
//! с третьим смыслом «во время заезда». Одна функция = одно определение, и
//! негативная арка снимает его РАЗОМ у всех потребителей.
[[nodiscard]] bool IsRaceUnderway(
  const RaceInstance& raceInstance,
  const tracker::RaceTracker::Racer& racer,
  const std::chrono::steady_clock::time_point now) noexcept
{
  return racer.state == tracker::RaceTracker::Racer::State::Racing
    && not racer.finishCounted
    && now >= raceInstance.GetRaceStartTimePoint();
}

//! LOA-fix (R71-3): «координаты стены — числа, а не мусор?»
//!
//! Границы карты сервер не знает (в `courses.yaml` геометрии трасс нет вообще — об
//! этом прямо написано в RaceTracker.hpp:49-52), поэтому проверяется РОВНО то, что
//! можно проверить, не выдумывая порогов: конечность. NaN/Inf, разосланные каждому
//! клиенту как позиция препятствия, — это не «странное значение», это порча
//! состояния у всех сразу.
//! LOA-fix (R71-12, находка ревью 2 #1): «ЭФФЕКТ ЕСТЬ В РЕЕСТРЕ» И «ЭФФЕКТ МОЖНО
//! ПОВЕСИТЬ» — РАЗНЫЕ УТВЕРЖДЕНИЯ.
//!
//! `Racer::effects` — массив на 24 слота (RaceTracker.hpp:259). `magic.yaml` же
//! хранит `skillEffectId` СВОБОДНЫМ числом, и в поставляемом конфиге лежит запись
//! `type: 27` со `skillEffectId: 99999` (magic.yaml:748) — она в реестре ЕСТЬ, но
//! слота под неё не существует. Отсюда две беды, и обе лечит одно предикатное имя:
//! жалоба «out of range» на каждый пакет (лог-флуд, ради которого заведён
//! `LogThrottle`) и индексация `effects[99999]` — чтение за границами `std::array`.
//!
//! ★СЕГОДНЯ ЗА ГРАНИЦУ НЕ ХОДЯТ, НО ПО ВЕЗЕНИЮ: у записи 27 `adjustMotionSpeed: 0`,
//! поэтому короткое замыкание в цикле снятия бафов не доходит до индекса. Меняется
//! одно число в КОНФИГЕ — и «повезло» кончается. Проверяем свойство, а не запись.
[[nodiscard]] constexpr bool IsSchedulableEffectId(const uint32_t effectId) noexcept
{
  return effectId < tracker::RaceTracker::Racer::EffectCount;
}

[[nodiscard]] bool IsFiniteIceWallPlacement(
  const protocol::AcCmdCRUseMagicItem::IceWallProperties& properties) noexcept
{
  return std::ranges::all_of(properties.member1, [](const float axis) { return std::isfinite(axis); })
    && std::ranges::all_of(properties.member2, [](const float axis) { return std::isfinite(axis); });
}

} // namespace

bool RaceNetworkHandler::AcquireAiP2dIds(const size_t count)
{
  if (_aiP2dIds.size() >= count)
    return true;

  // Берём недостающее во ВРЕМЕННЫЙ набор: пока он не полон, взятое принадлежит
  // не нам, а пулу, и обязано вернуться туда при первом же отказе.
  std::vector<race::P2dId> acquired;
  acquired.reserve(count - _aiP2dIds.size());

  while (_aiP2dIds.size() + acquired.size() < count)
  {
    const std::optional<race::P2dId> p2dId = _p2dIdPool.Acquire();
    if (not p2dId.has_value())
    {
      // Пул исчерпан. Возвращаем ВСЁ взятое в этой попытке: иначе каждая
      // следующая попытка отъедала бы у живых клиентов ещё несколько id
      // ради ростера, который всё равно не собрался.
      for (const race::P2dId reserved : acquired)
        _p2dIdPool.Release(reserved);
      return false;
    }

    acquired.push_back(p2dId.value());
  }

  _aiP2dIds.insert(_aiP2dIds.end(), acquired.begin(), acquired.end());
  return true;
}

void RaceNetworkHandler::SpawnAiRacers(RaceInstance& raceInstance)
{
  // ★ПОРЯДОК ВАЖЕН: сначала p2dId, потом oid. P2dId можно вернуть в пул, а oid
  // вернуть НЕКУДА — счётчик трекера только растёт. Поэтому необратимый шаг
  // делается последним и только тогда, когда обратимый уже удался.
  if (not AcquireAiP2dIds(AiRacerCount))
  {
    // ★МЯГКИЙ ОТКАЗ, а не бросок. Соперники — украшение соло-заезда; ронять
    // из-за них сам заезд нельзя. И ростер собирается либо целиком, либо
    // никак: половина ботов в пакете — это половина ботов на трассе.
    server::util::QuietLogWarn(
      "The P2dId pool cannot supply {} AI racers; the solo race in room {} "
      "will run without AI opponents",
      AiRacerCount,
      raceInstance.GetRoomUid());
    return;
  }

  // ★Object id ботов выдаются ОДИН РАЗ НА КОМНАТУ и дальше переиспользуются —
  // ровно так же, как трекер переиспользует id живого игрока. Свежий набор на
  // каждый заезд сжигал бы по семь id за заезд, и `uint16_t`-счётчик обернулся
  // бы примерно на девятитысячном заезде, выдав боту id живого игрока.
  auto& aiOids = raceInstance.GetAiOids();
  if (aiOids.empty())
  {
    aiOids.reserve(AiRacerCount);
    for (size_t index = 0; index < AiRacerCount; ++index)
      aiOids.push_back(raceInstance.GetTracker().ReserveOid());
  }

  auto& aiRacers = raceInstance.GetAiRacers();
  aiRacers.reserve(AiRacerCount);

  for (size_t index = 0; index < AiRacerCount; ++index)
  {
    aiRacers.push_back(RaceInstance::AiRacer{
      .oid = aiOids[index],
      .p2dId = _aiP2dIds[index],
      .name = AiRacerNames[index],
      .personality = static_cast<uint8_t>(index + 1),
      .courseTime = tracker::InvalidCourseTime});
  }

  server::util::QuietLogDebug(
    "Spawned {} AI racers for the solo race in room {}",
    aiRacers.size(),
    raceInstance.GetRoomUid());
}

RaceNetworkHandler::ClientContext& RaceNetworkHandler::GetClientContext(ClientId clientId, bool requireAuthorized)
{
  auto clientContextIter = _clients.find(clientId);
  if (clientContextIter == _clients.end())
    throw std::runtime_error("Race client is not available");

  auto& clientContext = clientContextIter->second;
  if (requireAuthorized && not clientContext.isAuthenticated)
    throw std::runtime_error("Race client is not authenticated");

  return clientContext;
}

ClientId RaceNetworkHandler::GetClientIdByCharacterUid(data::Uid characterUid)
{
  for (auto& [clientId, clientContext] : _clients)
  {
    if (clientContext.characterUid == characterUid
      && clientContext.isAuthenticated)
      return clientId;
  }

  throw std::runtime_error("Character not associated with any client");
}

RaceNetworkHandler::ClientContext& RaceNetworkHandler::GetClientContextByCharacterUid(
  const data::Uid characterUid)
{
  for (auto& clientContext : _clients | std::views::values)
  {
    if (clientContext.characterUid == characterUid
      && clientContext.isAuthenticated)
      return clientContext;
  }

  throw std::runtime_error("Character not associated with any client");
}

bool RaceNetworkHandler::IsAiRacerOfClientRace(
  const ClientContext& clientContext,
  const tracker::Oid oid) noexcept
{
  try
  {
    if (clientContext.roomUid == data::InvalidUid)
      return false;

    std::scoped_lock lock(_raceInstancesMutex);

    const auto raceInstance = _raceInstances.find(clientContext.roomUid);
    if (raceInstance == _raceInstances.cend())
      return false;

    return raceInstance->second.IsAiRacerOid(oid);
  }
  catch (...)
  {
    // Захват замка умеет бросить на системной ошибке. Пометка `noexcept`
    // обязывает погасить это здесь; ответ «не бот» безопасен — обработка
    // просто пойдёт прежним путём.
    return false;
  }
}

RaceInstance& RaceNetworkHandler::GetRaceInstance(
  const ClientContext& clientContext,
  const bool checkRacer)
{
  // Check if the client has an invalid room UID
  if (clientContext.roomUid == data::InvalidUid)
    throw std::runtime_error(
      std::format("Tried to get race instance for character '{}' but room uid is invalid",
        clientContext.characterUid));

  // Sanity check if a race instance by that room UID exists
  if (not _raceInstances.contains(clientContext.roomUid))
    throw std::runtime_error(
      std::format("Tried to get race instance for character '{}' but room '{}' does not exist",
        clientContext.characterUid,
        clientContext.roomUid));

  auto& raceInstance = _raceInstances.at(clientContext.roomUid);

  // If not racing cqommand then we are done here
  // HurdleClearResult, HandleSpur etc.
  if (not checkRacer)
    return raceInstance;

  // Check if the character is a racer
  // Protects against characters waiting in the waiting room but emitting racing commands
  if (not raceInstance.GetTracker().IsRacer(clientContext.characterUid))
    throw std::runtime_error(
      std::format("Tried to get race instance '{}' but character '{}' is not a racer",
        clientContext.roomUid,
        clientContext.characterUid));

  return raceInstance;
}

void RaceNetworkHandler::HandleEnterRoom(
  ClientId clientId,
  const protocol::AcCmdCREnterRoom& command)
{
  auto& clientContext = _clients[clientId];

  size_t identityHash = std::hash<uint32_t>()(command.characterUid);
  boost::hash_combine(identityHash, command.roomUid);

  clientContext.isAuthenticated = _serverInstance.GetOtpSystem().AuthorizeCode(
    identityHash,
    command.oneTimePassword);

  const bool doesRoomExist = _serverInstance.GetRoomSystem().RoomExists(
    command.roomUid);

  // Determine the racer count and whether the room is full.
  bool isOvercrowded = false;
  if (clientContext.isAuthenticated)
  {
    _serverInstance.GetRoomSystem().GetRoom(
      command.roomUid,
      [&isOvercrowded, clientId, characterUid = command.characterUid](Room& room)
      {
        // If the player is not able to be added, the room is full.
        isOvercrowded = not room.AddPlayer(clientId, characterUid);
      });
  }

  // Cancel the enter room if the client is not authenticated,
  // the room does not exist or the room is full.
  if (not clientContext.isAuthenticated
    || not doesRoomExist
    || isOvercrowded)
  {
    const protocol::AcCmdCREnterRoomCancel response{};
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  // The client is authorized so we can trust the identifiers
  // that were provided.
  clientContext.characterUid = command.characterUid;
  clientContext.roomUid = command.roomUid;
  clientContext.userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;

  std::scoped_lock lock(_raceInstancesMutex);
  // Try to emplace the room instance.
  const auto& [raceInstanceIter, inserted] = _raceInstances.try_emplace(
    command.roomUid,
    *this,
    command.roomUid);

  auto& raceInstance = raceInstanceIter->second;

  // LOA-fix (R11-5a, round11, backlog #19 п.5): ШТАТНЫЙ ОТКАЗ ВО ВХОДЕ.
  // ЕДИНСТВЕННАЯ реализация отказа входа в комнату; ею пользуются все пять
  // веток: недоступная запись персонажа заходящего на апстримном логирующем
  // доступе (R11-5c) и в петле ростера (R11-5), нет записи его лошади (R11-7b),
  // переросший ответ (R11-4), гонщик так и не собрался (R11-5b).
  // ★ ИНВАРИАНТ, РАДИ КОТОРОГО ЭТОТ ХЕЛПЕР СУЩЕСТВУЕТ: после ЛЮБОГО отказа
  // член-список комнаты обязан совпадать с состоянием ДО попытки входа. Голый
  // return в ветке отказа запрещён — он оставляет полудобавленного игрока
  // (Room::AddPlayer отработал в начале обработчика), то есть призрака, который
  // вечно занимает слот и никогда не станет ready, из-за чего комната больше
  // никогда не сможет стартовать заезд.
  // Зовётся, когда ростер нельзя собрать или отправить именно для ЗАХОДЯЩЕГО.
  // Слать в этом случае EnterRoomOK нечем,
  // а рассылать комнате EnterRoomNotify с пустым гонщиком нельзя: писатель
  // упадёт на avatar.value() внутри write-supplier и Client::WriteLoop разорвёт
  // соединение ВСЕМ получателям. Поэтому отвечаем заходящему тем же
  // AcCmdCREnterRoomCancel, что и при «комната полна», и убираем занятый слот —
  // иначе комната останется с призраком и никогда не удалится.
  // ГДЕ ОН ОПРЕДЕЛЁН: сразу после взятия ссылки на raceInstance, то есть ВЫШЕ
  // первого апстримного доступа к записи заходящего. Иначе ветка отказа по
  // недоступной записи недостижима — Immutable бросает раньше неё (WARN третьей
  // панели 2026-08-17).
  // ВЫЗВАЛ — СРАЗУ return. Хелпер не рассчитан на повторный вызов и после него
  // нельзя трогать ни raceInstance (ссылка могла быть инвалидирована erase'ом
  // ниже), ни комнату (её могло не стать).
  // МАСТЕРА НЕ ПЕРЕИЗБИРАЕМ — СОЗНАТЕЛЬНО. Хвост HandleLeaveRoom кроме уборки
  // слота ещё и передаёт masterUid следующему игроку; здесь этого блока нет и он
  // не нужен: masterUid проставляется ТОЛЬКО на inserted (первый вошедший в
  // race-инстанс), заходящий в НЕПУСТУЮ комнату мастером быть не может, а отказ,
  // который опустошает комнату, её тут же и удаляет вместе с инстансом. Условие
  // корректности — однопоточная обработка команд (единственный io_context.run()).
  // Если AddPlayer уедет под _raceInstancesMutex или появится второй поток
  // обработки, сюда обязан приехать блок переизбрания мастера из HandleLeaveRoom,
  // иначе комната останется без мастера и HandleStartRace никого не пустит.
  const auto refuseRoomEntry = [this, clientId, &clientContext](
    const char* reason)
  {
    // Ранний выход делает хелпер безопасным при повторном заходе: ниже он сам
    // сбрасывает roomUid в InvalidUid, а GetRoomSystem().GetRoom() на
    // несуществующей комнате БРОСАЕТ (RoomSystem: «Room does not exist»). No-op —
    // это Room::RemovePlayer (erase по ключу), а не хелпер целиком, поэтому
    // страховку ставим явно. Правило «после вызова — немедленный return» она НЕ
    // отменяет, см. ★-блок в шапке раунда.
    if (clientContext.roomUid == data::InvalidUid)
      return;

    server::util::QuietLogError(
      "Refusing character {} entry to room {}: {}",
      clientContext.characterUid,
      clientContext.roomUid,
      reason);

    bool isRoomEmpty = false;
    _serverInstance.GetRoomSystem().GetRoom(
      clientContext.roomUid,
      [&isRoomEmpty, characterUid = clientContext.characterUid](Room& room)
      {
        room.RemovePlayer(characterUid);
        isRoomEmpty = room.GetPlayerCount() == 0;
      });

    // Хвост HandleLeaveRoom: опустевшая комната удаляется вместе с инстансом.
    if (isRoomEmpty)
    {
      _serverInstance.GetRoomSystem().DeleteRoom(clientContext.roomUid);
      _raceInstances.erase(clientContext.roomUid);
    }

    clientContext.roomUid = data::InvalidUid;

    const protocol::AcCmdCREnterRoomCancel cancelResponse{};
    _commandServer.QueueCommand<decltype(cancelResponse)>(
      clientId,
      [cancelResponse]()
      {
        return cancelResponse;
      });
  };

  // If the room instance was just created, set it up.
  if (inserted)
  {
    raceInstance.GetRoom([masterUid = command.characterUid](Room& room)
    {
      auto& roomDetails = room.GetRoomDetails();
      roomDetails.masterUid = masterUid;
    });
  }

  // LOA-fix (R11-5c, round11, backlog #19 п.5): ЗАПИСЬ ЗАХОДЯЩЕГО ПРОВЕРЯЕМ
  // ДО ЧТЕНИЯ, А НЕ ЛОВИМ БРОСОК.
  // ЧТО БЫЛО НЕ ТАК: апстрим читал запись персонажа заходящего сразу, ради
  // одной строки лога «has created/joined [Room]». Record::Immutable на
  // недоступной записи бросает std::runtime_error (Record.hpp), а к этому
  // моменту уже отработали Room::AddPlayer и try_emplace race-инстанса —
  // исключение улетало в CommandServer («Unhandled exception handling command»)
  // и оставляло в комнате полудобавленного игрока ПЛЮС свежесозданный инстанс.
  // Ровно тот призрак #20, ради которого писан весь ★-инвариант отказа.
  // ПОЧЕМУ ЭТО ВАЖНО ИМЕННО СЕЙЧАС: ветка «запись заходящего недоступна» в
  // R11-5 (петля ростера) физически недостижима, пока этот доступ бросает
  // раньше неё, — то есть без этого гарда инвариант на данном сценарии не
  // держался вовсе (WARN третьей панели 2026-08-17).
  // Условие ровно то же, что в R11-5, и лечение то же: штатный отказ во входе
  // через refuseRoomEntry (слот снимается, опустевшая комната удаляется,
  // клиенту уходит AcCmdCREnterRoomCancel) и немедленный return.
  const auto joiningCharacterRecord =
    _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid);

  if (not joiningCharacterRecord.IsAvailable())
  {
    refuseRoomEntry(
      "the character record of the joining player is unavailable "
      "(upstream logging access)");
    return;
  }

  joiningCharacterRecord.Immutable(
    [inserted, clientContext](const data::Character& character)
    {
      if (inserted)
        server::util::QuietLogInfo("Player {} ({}) has created [Room {}]",
          clientContext.userName,
          character.name(),
          clientContext.roomUid);
      else
        server::util::QuietLogInfo("Player {} ({}) has joined [Room {}]",
          clientContext.userName,
          character.name(),
          clientContext.roomUid);
    });

  // LOA-fix (R11-18, round11, backlog #19 п.1): сброс роллинг-кода ПЕРЕЕХАЛ
  // отсюда вниз, вплотную к отправке AcCmdCREnterRoomOK. Здесь он стоял ДО
  // сборки ростера, поэтому каждый наш отказ во входе уходил клиенту уже после
  // ресета кода, и клиент с сервером расходились по _rollingCode (подробности —
  // в комментарии операции R11-18 патчера и в ★-блоке шапки раунда).
  protocol::AcCmdCREnterRoomOK response{
    .isRoomWaiting = raceInstance.GetStage() == RaceInstance::Stage::Waiting,
    .uid = command.roomUid};

  // If race instance exists and race is not waiting then
  // set the elapsed time since loading started
  if (not inserted and raceInstance.GetStage() != RaceInstance::Stage::Waiting)
  {
    response.elapsedTime = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - raceInstance.GetLoadingStartTimePoint()).count());
  }

  try
  {
    _serverInstance.GetRoomSystem().GetRoom(
      command.roomUid,
      [&response](Room& room)
      {
        const auto& roomDetails = room.GetRoomDetails();
        response.roomDescription = {
          .name = roomDetails.name,
          .maxPlayerCount = static_cast<uint8_t>(roomDetails.maxPlayerCount),
          .password = roomDetails.password,
          .gameModeMaps = static_cast<uint8_t>(roomDetails.gameMode),
          .gameMode = static_cast<protocol::GameMode>(roomDetails.gameMode),
          .mapBlockId = roomDetails.courseId,
          .teamMode = static_cast<protocol::TeamMode>(roomDetails.teamMode),
          .missionId = roomDetails.missionId,
          .unk6 = roomDetails.npcDifficulty,
          .skillBracket = roomDetails.skillBracket};
      });
  }
  catch (const std::exception&)
  {
    throw std::runtime_error("Client tried entering a deleted room");
  }

  protocol::Racer joiningRacer;

  // Collect the room players.
  std::vector<data::Uid> characterUids;
  data::Uid roomMasterUid{data::InvalidUid};
  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    [&characterUids, &roomMasterUid](Room& room)
    {
      roomMasterUid = room.GetRoomDetails().masterUid;
      for (const auto& characterUid : room.GetPlayers() | std::views::keys)
      {
        characterUids.emplace_back(characterUid);
      }
    });

  // Build the room players.
  for (const auto& characterUid : characterUids)
  {
    auto& protocolRacer = response.racers.emplace_back();
    protocolRacer.isMaster = characterUid == roomMasterUid;

    bool isPlayerReady = false;
    Room::Player::Team team;

    _serverInstance.GetRoomSystem().GetRoom(
      clientContext.roomUid,
      [&isPlayerReady, &team, characterUid](Room& room)
      {
        const auto& player = room.GetPlayer(characterUid);
        isPlayerReady = player.IsReady();
        team = player.GetTeam();
      });

    // Fill data from the character record.
    const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
      characterUid);

    // LOA-fix (R11-5, round11, backlog #19 п.5): битого участника ПРОПУСКАЕМ.
    // Record::Immutable() на недоступной записи бросает std::runtime_error, а
    // бросок отсюда стоит дорого: исключение ловится в CommandServer, и в
    // результате заходящий не получает AcCmdCREnterRoomOK, а комната —
    // AcCmdCREnterRoomNotify. Один игрок с неподгруженной записью (холодный кэш
    // после рестарта) вешал вход в комнату ВСЕМ. Лучше отдать список без него.
    // РАЗВИЛКА (правка после ревью панели 2026-08-17): «пропустить» законно
    // только для ЧУЖОЙ записи. Если недоступна запись САМОГО заходящего, то
    // joiningRacer останется пустым и уедет всей комнате — это разрыв соединения
    // у всех (подробности в R11-5a). Такому входу отказываем целиком.
    if (not characterRecord.IsAvailable())
    {
      if (characterUid == clientContext.characterUid)
      {
        refuseRoomEntry("the character record of the joining player is unavailable");
        return;
      }

      server::util::QuietLogError(
        "Character record {} is unavailable while building the room roster for "
        "room {}; skipping this racer",
        characterUid,
        clientContext.roomUid);

      response.racers.pop_back();
      continue;
    }

    // LOA-fix (R11-7b, round11, backlog #19 п.5): ПРИГОДНОСТЬ ДАННЫХ ГОНЩИКА.
    // Лямбда ниже не может ни пропустить участника, ни отказать во входе —
    // поэтому она лишь выставляет флаг, а решение принимается сразу после неё.
    bool isRacerDataUsable = true;

    characterRecord.Immutable(
      [this, isPlayerReady, team, &protocolRacer, &isRacerDataUsable](
        const data::Character& character)
      {
        const auto& settingsRecord = GetServerInstance().GetDataDirector().GetSettings(character.settingsUid());
        if (settingsRecord.IsAvailable())
        {
          settingsRecord.Immutable(
            [&protocolRacer, modelId = character.parts.modelId()](const data::Settings& settings)
            {
              if (not settings.hideAge())
              {
                // TODO: Add age here (find if it is even possible)
                // todo: model constants
                // LOA-fix (R11-6, round11, backlog #19 п.6): неизвестный modelId
                // больше НЕ бросает. modelId по умолчанию 0
                // (DataDefinitions.hpp), поэтому любой недосозданный или
                // починенный DataRepair'ом персонаж ронял сборку ВСЕГО ростера
                // комнаты — и вход подвисал не у него, а у того, кто заходит.
                // protocol::Gender::Unspecified (=0) — легальное значение
                // протокола, ровно для этого и заведено.
                if (modelId == 10)
                {
                  protocolRacer.gender = protocol::Gender::Boy;
                }
                else if (modelId == 20)
                {
                  protocolRacer.gender = protocol::Gender::Girl;
                }
                else
                {
                  protocolRacer.gender = protocol::Gender::Unspecified;
                  server::util::QuietLogWarn(
                    "Character gender not recognised by model ID {}; "
                    "sending Unspecified",
                    modelId);
                }
              }
            });
        }
        else
        {
          server::util::QuietLogWarn("Settings record for character {} was not found, skipping role/gender assignment...",
            character.uid());
        }

        protocolRacer.level = character.level();
        protocolRacer.uid = character.uid();
        protocolRacer.name = character.name();
        protocolRacer.role = static_cast<protocol::Racer::Role>(character.role());
        protocolRacer.isHidden = false;
        protocolRacer.isNPC = false;
        protocolRacer.isReady = isPlayerReady;

        switch (team)
        {
          case Room::Player::Team::Red:
            protocolRacer.teamColor = protocol::TeamColor::Red;
            break;
          case Room::Player::Team::Blue:
            protocolRacer.teamColor = protocol::TeamColor::Blue;
            break;
          default:
            protocolRacer.teamColor = protocol::TeamColor::None;
            break;
        }

        protocolRacer.avatar = protocol::Avatar{};

        protocol::BuildProtocolCharacter(
          protocolRacer.avatar->character, character);

        // Build the character equipment.
        // LOA-fix (R11-7, round11, backlog #19 п.2/3/5): ОДИН аппенд, с
        // проверкой доступности и потолком длины.
        // (п.2) Здесь был ВТОРОЙ аппенд из character.expiredEquipment() под
        // комментарием «Build the mount equipment». Комментарий врал:
        // protocol::Avatar имеет ровно один список equipment, сбруя после
        // регрессии 79d32bd лежит в characterEquipment, а expiredEquipment —
        // легаси-список horseEquipment (FileDataSource.cpp читает его из
        // JSON-ключа "horseEquipment" с пометкой «todo: rename»). У старых
        // сейвов он не пуст, поэтому аватар уезжал клиенту удвоенным — это один
        // из трёх факторов переполнения клиентского буфера (#19).
        // (п.5) DataStorage::Get(KeySpan) отдаёт nullopt, если хотя бы ОДИН
        // предмет ещё не в кэше (типично сразу после рестарта), поэтому прежнее
        // разыменование `*...Get(...)` было UB. Эталон проверки — лобби
        // (LobbyNetworkHandler.cpp), но бросать здесь нельзя (см. R11-5),
        // поэтому логируем и отдаём гонщика без экипировки.
        // (п.3) Клиент показывает максимум 16 предметов экипировки (upstream
        // issue #119), длина в протоколе пишется uint8_t. ЭТО СТРАХОВКА ОТ БИТЫХ
        // ДАННЫХ, А НЕ СРЕДСТВО УДЕРЖАНИЯ ПАКЕТА ПОД 4096 (уточнено ревью панели
        // 2026-08-17): экипировка слотовая — у персонажа шляпа/голова/тело/ноги/
        // серьги, у лошади седло/подковы/протектор/щит, то есть до ~10 предметов,
        // и до 16 список в норме не дорастает; protocol::Item — фиксированные 16
        // байт без строк. Размер EnterRoomOK держат имена (EUC-KR, два байта на
        // символ) и число гонщиков, а реальную экономию дал снятый двойной
        // аппенд (п.2), а не этот потолок.
        constexpr std::size_t MaxRacerEquipmentItems = 16;

        const auto equipmentItems = _serverInstance.GetDataDirector().GetItemCache().Get(
          character.characterEquipment());
        if (equipmentItems)
        {
          protocol::BuildProtocolItems(
            protocolRacer.avatar->equipment,
            *equipmentItems);

          if (protocolRacer.avatar->equipment.size() > MaxRacerEquipmentItems)
          {
            server::util::QuietLogWarn(
              "Character {} has {} equipment items, clamping to {} for the race "
              "roster",
              character.uid(),
              protocolRacer.avatar->equipment.size(),
              MaxRacerEquipmentItems);

            protocolRacer.avatar->equipment.resize(MaxRacerEquipmentItems);
          }
        }
        else
        {
          server::util::QuietLogError(
            "Equipment items of character {} are not available yet; "
            "sending the racer without equipment",
            character.uid());
        }

        const auto mountRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
          character.mountUid());
        if (mountRecord)
        {
          mountRecord->Immutable(
            [&protocolRacer](const data::Horse& mount)
            {
              protocol::BuildProtocolHorse(protocolRacer.avatar->mount, mount);
            });
        }
        else
        {
          // LOA-fix (R11-7b, round11, backlog #19 п.5): БЕЗ ЛОШАДИ ГОНЩИКА НЕ
          // ШЛЁМ (правка после ревью панели 2026-08-17). Раньше в этой ветке в
          // пакете оставалась ДЕФОЛТНАЯ лошадь — uid 0, tid 0, пустая кличка, —
          // то есть клиенту уезжала модель коня, которой не существует. Это
          // ровно тот класс «плохих данных в EnterRoomOK», ради которого писан
          // весь #19, а поведение клиента на несуществующем tid непроверено.
          // Пустой список экипировки безопасен, отсутствующая лошадь — нет,
          // поэтому помечаем гонщика непригодным: чужого пропустим (pop_back),
          // заходящему откажем во входе (R11-5a). Симметрично R11-5.
          server::util::QuietLogError(
            "Mount {} of character {} is not available yet; "
            "the racer cannot be put into the room roster",
            character.mountUid(),
            character.uid());

          isRacerDataUsable = false;
        }

        if (character.guildUid() != data::InvalidUid)
        {
          GetServerInstance().GetDataDirector().GetGuild(character.guildUid()).Immutable(
            [&protocolRacer, characterUid = character.uid()](const data::Guild& guild)
            {
              protocol::BuildProtocolGuild(protocolRacer.guild, guild);

              if (guild.owner() == characterUid)
              {
                protocolRacer.guild.guildRole = protocol::GuildRole::Owner;
              }
              else if (std::ranges::contains(guild.officers(), characterUid))
              {
                protocolRacer.guild.guildRole = protocol::GuildRole::Officer;
              }
              else
              {
                protocolRacer.guild.guildRole = protocol::GuildRole::Member;
              }
            });
        }

        if (character.petUid() != data::InvalidUid)
        {
          const auto& petRecord = GetServerInstance().GetDataDirector().GetPet(character.petUid());
          if (petRecord.IsAvailable())
          {
            petRecord.Immutable(
              [&protocolRacer](const data::Pet& pet)
              {
                protocol::BuildProtocolPet(protocolRacer.pet, pet);
              });
          }
          else
          {
            server::util::QuietLogWarn("Character {} tried to load pet {} but it is not available.",
              character.uid(),
              character.petUid());
          }
        }
      });

    // LOA-fix (R11-7b, round11, backlog #19 п.5): гонщик с непригодными
    // данными в пакет не попадает. Чужого просто вычёркиваем из ростера (лучше
    // короткий список, чем модель коня, которой нет), а заходящему отказываем во
    // входе штатным AcCmdCREnterRoomCancel — тем же путём, что и R11-5.
    if (not isRacerDataUsable)
    {
      if (characterUid == clientContext.characterUid)
      {
        refuseRoomEntry("the mount record of the joining player is unavailable");
        return;
      }

      response.racers.pop_back();
      continue;
    }

    if (characterUid == clientContext.characterUid)
    {
      joiningRacer = protocolRacer;
    }
  }

  // LOA-fix (R11-5b, round11, backlog #19 п.5): ГОНЩИК НЕ СОБРАЛСЯ — ОТКАЗ ДО
  // ЛЮБОГО ПАКЕТА О ВХОДЕ.
  // Если joiningRacer так и не собрался (заходящего не оказалось в списке
  // игроков комнаты — апстримный краевой случай), рассылать EnterRoomNotify
  // нельзя: writer вызовет racer.avatar.value() и std::bad_optional_access из
  // write-supplier обернётся End() — разрывом соединения у ВСЕХ получателей.
  // ГДЕ СТОИТ И ПОЧЕМУ ИМЕННО ЗДЕСЬ (BLOCK-1 панели Codex T2 2026-08-17).
  // Сразу за концом петли ростера: joiningRacer тут уже окончателен (последнее
  // присваивание — в конце петли, ниже его никто не трогает), а клиенту ещё не
  // ушло НИ ОДНОГО пакета о входе. Значит отказ уходит ДО замера размера
  // (R11-4), ДО _commandServer.SetCode(clientId, {}) и ДО QueueCommand с
  // AcCmdCREnterRoomOK. Прежняя редакция держала гард перед рассылкой notify,
  // то есть ЗА уже отправленным OK: сработай он — клиент получил бы OK, а
  // следом Cancel (пары терминалов, которой апстрим не производит никогда), плюс
  // разошёлся бы с сервером по роллинг-коду, потому что SetCode уже отработал.
  // Теперь ★ инвариант отказа держится и на этой ветке без всяких оговорок, а
  // правило ★-блока «отказ обязан уходить до SetCode» выполняется без
  // исключений.
  // ОТКАЗЫВАЕМ ВО ВХОДЕ, А НЕ ПРОСТО ПРОПУСКАЕМ РАССЫЛКУ. Молчаливый выход
  // оставлял бы призрака: Room::AddPlayer отработал в начале обработчика, слот
  // занят, комната уже никогда не станет ready. refuseRoomEntry снимает слот
  // (если он есть — erase по несуществующему uid это no-op), убирает опустевшую
  // комнату и шлёт заходящему AcCmdCREnterRoomCancel. Так член-список комнаты
  // после неудачной попытки входа совпадает с состоянием до неё.
  // СТАТУС ЭТОЙ ВЕТКИ — АВАРИЙНЫЙ ГАРД АПСТРИМНОГО КРАЯ, А НЕ РАБОЧИЙ СЦЕНАРИЙ
  // (уточнено третьей панелью 2026-08-17). Заходящий попадает в
  // room.GetPlayers() синхронно, ещё в AddPlayer в начале обработчика, а все
  // ранние отказы (R11-5, R11-5c, R11-7b) делают return ещё выше, — то есть на
  // известных путях сюда не приходят. Но стоит гард теперь там, где его
  // срабатывание не нарушает ни порядок пакетов, ни роллинг-код.
  if (not joiningRacer.avatar.has_value())
  {
    server::util::QuietLogError(
      "Room {}: the joining racer {} was not built, refusing the entry before "
      "sending AcCmdCREnterRoomOK (broadcasting AcCmdCREnterRoomNotify with an "
      "empty racer would drop the whole room)",
      clientContext.roomUid,
      clientContext.characterUid);

    refuseRoomEntry("the joining player was not built into the room roster");
    return;
  }

  // LOA-fix (R11-4, round11, backlog #19 п.1): ОТВЕТ, КОТОРЫЙ НЕ ВЛЕЗАЕТ В
  // КЛИЕНТСКИЙ БУФЕР, НЕ ОТПРАВЛЯЕМ — И ОТКАЗЫВАЕМ ВО ВХОДЕ.
  // ЧТО БЫЛО НЕ ТАК: исходящий путь размер не проверял вообще. AcCmdCREnterRoomOK
  // несёт полные аватары всех игроков комнаты, а кириллица в никах/кличках/
  // гильдиях уходит в EUC-KR по два байта на символ — поэтому у нас этот пакет
  // перерастает клиентский приёмный буфер (protocol::BufferSize = 4096) там, где
  // у англоязычного апстрима не перерастал. Переросток уходил молча и ронял
  // ПРИНИМАЮЩИЙ клиент: «краш ровно у того, кто заходит в комнату» (#19).
  // ПОЧЕМУ ПРОВЕРКА ЗДЕСЬ, А НЕ В CommandServer (BLOCK второй панели 2026-08-17).
  // В write-supplier общей воронки записи нет ни комнаты, ни клиентского
  // контекста. Отказ, принятый там, лечит клиент, но оставляет на сервере
  // ПОЛУДОБАВЛЕННОГО игрока: Room::AddPlayer отработал в начале этого
  // обработчика, clientContext.roomUid проставлен. Такой призрак вечно занимает
  // слот, никогда не станет ready (CanRoomStart → NotAllPlayersReady, комната
  // больше не стартует), да ещё и попадает в ростер СЛЕДУЮЩЕГО EnterRoomOK, то
  // есть делает его ещё больше. Здесь же комната видна, поэтому отказ идёт через
  // общий refuseRoomEntry и член-список комнаты возвращается ровно в то
  // состояние, в котором был до попытки входа (★ инвариант отказа).
  // КАК МЕРЯЕМ. Прогоняем готовый response через AcCmdCREnterRoomOK::Write в
  // СКРЕТЧ-поток. Это чистая функция от response (const&), у реального пути
  // записи свой SinkStream со своим курсором, поэтому замер ничего не портит и
  // ничего не переупорядочивает. Скретч намеренно больше клиентского буфера: нам
  // нужно УЗНАТЬ размер, а не уместить пакет.
  // ЧТО ЕЩЁ ЛОВИТ ЭТОТ TRY. AcCmdCREnterRoomOK::Write бросает std::logic_error
  // при > 10 гонщиков (RaceMessageDefinitions.cpp). Раньше этот бросок прилетал
  // из write-supplier, Client::WriteLoop ловил его и вызывал End() — то есть РВАЛ
  // соединение заходящему. Теперь он ловится здесь и превращается в штатный отказ.
  // ПОЧЕМУ ПОРОГ СЧИТАЕТСЯ КАК payload + magic. Клиент читает в свой
  // 4096-байтный буфер и заголовок, и нагрузку, поэтому «влезает» = payload плюс
  // sizeof(MessageMagic) не больше protocol::BufferSize.
  // EnterRoomNotify НЕ ПРОВЕРЯЕМ намеренно: он несёт ровно одного гонщика
  // (~455 байт) и до 4096 физически не дотягивает — проверка была бы мёртвым кодом.
  {
    // Скретч-буфер заведомо больше и клиентского потолка (4096), и серверного
    // MaxCommandDataSize (8192): переросток надо ИЗМЕРИТЬ, а не обрезать.
    constexpr std::size_t EnterRoomOkProbeBufferSize = 16384;

    // Мягкий порог: с него начинаем ПРЕДУПРЕЖДАТЬ, ещё ничего не отказывая.
    // Зачем (замечание панели 2026-08-17): жёсткий потолок 4096 виден в логе
    // ТОЛЬКО в момент отказа живому игроку, то есть узнать «насколько мы близко»
    // можно было бы лишь постфактум, уже испортив кому-то вход. ~88% от потолка
    // даёт запас примерно в одного гонщика (450-650 Б на человека по писателю
    // WriteRacer), поэтому первый же прод-лог покажет распределение размеров
    // ДО того, как порог начнёт стрелять. Это же измерение — вход для решения
    // «4096 правда предел клиента или нет»: магазин штатно шлёт тому же клиенту
    // куски по 7168 Б (ChunkSize в LobbyNetworkHandler), то есть универсальным
    // 4096 не является, и предпосылка проверяется на стенде (см. смоук раунда 11
    // в CHANGES.md).
    constexpr std::size_t EnterRoomOkSoftLimit = 3600;

    std::size_t responseCommandSize = 0;
    bool isResponseSizeKnown = false;
    bool doesResponseFitClientBuffer = false;

    try
    {
      std::vector<std::byte> probeBuffer(EnterRoomOkProbeBufferSize);
      SinkStream probeSink{std::span<std::byte>{probeBuffer}};

      protocol::AcCmdCREnterRoomOK::Write(response, probeSink);

      responseCommandSize = probeSink.GetCursor() + sizeof(protocol::MessageMagic);
      isResponseSizeKnown = true;
      doesResponseFitClientBuffer =
        responseCommandSize <= static_cast<std::size_t>(protocol::BufferSize);
    }
    catch (const std::exception& serializationError)
    {
      // Либо ростер не влез даже в скретч, либо сработал апстримный logic_error
      // на > 10 гонщиков. И то и другое означает «этот ответ отправить нельзя».
      // РАЗМЕР ЗДЕСЬ НЕИЗВЕСТЕН, поэтому эта ветка — единственная, кто пишет про
      // причину, и ниже мы её не дублируем строкой про «N байт» (NIT третьей
      // панели: раньше следом печаталось «is 0 bytes … does not fit», то есть
      // заведомая неправда ровно в самом интересном случае).
      server::util::QuietLogError(
        "Room {}: the AcCmdCREnterRoomOK roster for character {} with {} racers "
        "cannot be serialized: {}; refusing the entry instead of crashing the "
        "client",
        clientContext.roomUid,
        clientContext.characterUid,
        response.racers.size(),
        serializationError.what());

      isResponseSizeKnown = false;
      doesResponseFitClientBuffer = false;
    }

    if (not doesResponseFitClientBuffer)
    {
      // Про размер печатаем ТОЛЬКО когда он реально измерен; в catch-ветке
      // причина уже напечатана выше.
      if (isResponseSizeKnown)
      {
        server::util::QuietLogError(
          "Room {}: the AcCmdCREnterRoomOK roster for character {} is {} bytes "
          "with {} racers, which exceeds the client buffer of {} bytes; "
          "refusing the entry instead of crashing the client",
          clientContext.roomUid,
          clientContext.characterUid,
          responseCommandSize,
          response.racers.size(),
          static_cast<std::size_t>(protocol::BufferSize));
      }

      refuseRoomEntry("the room roster does not fit into the client buffer");
      return;
    }

    // Ответ влезает, но уже подбирается к потолку — сообщаем заранее.
    if (responseCommandSize > EnterRoomOkSoftLimit)
    {
      server::util::QuietLogWarn(
        "Room {}: the AcCmdCREnterRoomOK roster for character {} is {} bytes "
        "with {} racers, approaching the client buffer of {} bytes",
        clientContext.roomUid,
        clientContext.characterUid,
        responseCommandSize,
        response.racers.size(),
        static_cast<std::size_t>(protocol::BufferSize));
    }
  }

  // LOA-fix (R11-18, round11, backlog #19 п.1): СБРОС РОЛЛИНГ-КОДА — ТОЛЬКО НА
  // УСПЕШНОМ ПУТИ. Апстримные две строки todo стояли выше по обработчику, ещё до
  // сборки ростера, поэтому КАЖДЫЙ наш отказ во входе уходил клиенту уже после
  // ресета _rollingCode: сервер на нуле, клиент — нет, и следующая команда
  // клиента разбиралась чужим кодом («Malformed command … Bad command data size»
  // в read-loop = разрыв). Апстримный отказ по «комната полна» кода не трогает,
  // и наши теперь тоже: до этой точки доходит только успешный вход.
  // Todo: Roll the code for the connecting client.
  // Todo: The response contains the code, somewhere.
  _commandServer.SetCode(clientId, {});

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  const protocol::AcCmdCREnterRoomNotify notify{
    .racer = joiningRacer,
    .averageTimeRecord = clientContext.characterUid};

  // Player should be added to the room at this point,
  // broadcast to room except joining player
  this->BroadcastExceptCharacterUid(
    raceInstance,
    notify,
    clientContext.characterUid);
}

void RaceNetworkHandler::HandleChangeRoomOptions(
  const ClientId clientId,
  const protocol::AcCmdCRChangeRoomOptions& command)
{
  // todo: validate command fields
  const auto& clientContext = GetClientContext(clientId);

  if (command.optionsBitfield == protocol::RoomOptionType::None)
    // If no options have been changed then do not broadcast notify
    // This prevents a bug with the race elapsed time from occurring
    return;

  const std::bitset<6> options(
    static_cast<uint16_t>(command.optionsBitfield));

  // LOA-fix (R11-8, round11, backlog #19 п.4): КЛАМП ЧИСЛА ИГРОКОВ ДО 8 —
  // ОДИН РАЗ И ДО МУТАЦИИ КОМНАТЫ. Лобби клампит на 8 при СОЗДАНИИ комнаты
  // (LobbyNetworkHandler::MakeRoom), а смена опций принимала сырое uint8_t из
  // пакета: модифицированный клиент ставил 9-10 и больше, комната пухла,
  // AcCmdCREnterRoomOK рос вместе с ней, а на 11 гонщиках его writer бросал
  // logic_error: в пине — прямо из write-supplier, то есть с разрывом соединения
  // заходящему. Теперь тот же бросок перехватывает замер R11-4 в HandleEnterRoom
  // и превращает в штатный отказ входа, но первопричину лечит именно этот кламп.
  // Переменная заведена ЗДЕСЬ, а не внутри лямбды мутации (как было в первой
  // редакции), потому что тем же числом обязан пользоваться и эхо-notify —
  // иначе сервер держит 8, а все клиенты рисуют 10 пустых слотов (это нашла
  // панель 2026-08-17). Notify ниже собирается из roomDetails, куда попадает
  // ровно это значение.
  constexpr uint8_t MaxRoomPlayerCount = 8;
  const uint8_t clampedPlayerCount = std::min(
    command.playerCount,
    MaxRoomPlayerCount);

  // LOA-fix (R11-16, round11, backlog #23): КАРТА — ТОЛЬКО ИЗ ПУЛА РЕЖИМА.
  // Апстримный `// todo: validate command fields` выше — ровно про это: courseId
  // клался из пакета как есть, и модифицированный клиент мог назначить комнате
  // ЛЮБУЮ карту, включая ранчо и тестовые сцены без гоночной геометрии. Заезд на
  // такой карте роняет клиент у ВСЕХ участников, а не только у автора.
  // ПОЧЕМУ ПУЛ РЕЖИМА, А НЕ ВЕСЬ РЕЕСТР (правка после ревью панели 2026-08-17):
  // первая редакция считала «карта известна CourseRegistry» достаточным
  // условием, но в mapBlockInfo courses.yaml лежат 55 записей, среди которых
  // ровно те, ради которых заведена задача #23 — 20000 'ranch_00', 20001
  // 'readyroom01', 20002 'award', 610 'b_ranch_01', 35 'test_small', 44
  // 'town_test01', 71 'b_tuto_test', 777 'test_bboo01', 879 'running_test'…
  // Гоночные пулы заметно уже: mapPool режима Speed — 28 карт, Magic — 16.
  // Авторитетен именно пул, поэтому сверяемся с ним.
  // ПОЧЕМУ РЕЖИМ БЕРЁМ ИТОГОВЫЙ. Тот же пакет может менять и режим (бит 3), и
  // карту (бит 4). Сверять карту с ТЕКУЩИМ режимом комнаты значит пропустить
  // связку «переключи на Magic + поставь спид-карту». Считаем режим таким, каким
  // он станет ПОСЛЕ применения этого же пакета (повторяя switch мутации ниже:
  // неизвестный режим не применяется, значит и не участвует).
  // ПОЧЕМУ РЕЖИМ БЕЗ ПУЛА = ОТКАЗ. В courses.yaml есть gameModeInfo только для
  // 0/1/2; Guild (3) и Tutorial (6) реестру неизвестны, GetCourseGameModeInfo на
  // них бросает. Такие комнаты и стартовать не могут (PrepareGameMode бросит то
  // же исключение, Start() вернёт false), поэтому конкретную карту им не
  // разрешаем — иначе через «сначала Guild + карта 20000, потом Speed» обход
  // проверки становится тривиальным.
  // Три псевдокурса (10000/10001/10002 = «все карты» / «новые» / «горячие»)
  // обязаны проходить всегда: в CourseRegistry их НЕТ, они разворачиваются в
  // случайную карту уже в RaceInstance::PrepareMap, и каждая комната создаётся
  // именно с 10002 (LobbyNetworkHandler). Без этого исключения не стартовала бы
  // ни одна комната.
  // ЧТО ДЕЛАЕМ НА ОТКАЗЕ — НЕ голый return (это тоже находка панели): остальные
  // биты применяются, у комнаты остаётся ПРЕЖНИЙ courseId, а notify в конце
  // обработчика собирается из ФАКТИЧЕСКОГО roomDetails и уходит всем, включая
  // инициатора. Клиент выбирает карту оптимистично, поэтому без notify его UI
  // остался бы на отклонённой карте — то есть ранний return лечил бы краш
  // рассинхроном, ровно тем, что чинит этот же батч.
  bool isMapChangeAllowed = true;
  if (options.test(4))
  {
    constexpr uint16_t AllMapsCourseId = 10000;
    constexpr uint16_t NewMapsCourseId = 10001;
    constexpr uint16_t HotMapsCourseId = 10002;

    // Режим, который будет у комнаты ПОСЛЕ этого пакета.
    registry::GameModeId resultingGameModeId{};
    _serverInstance.GetRoomSystem().GetRoom(
      clientContext.roomUid,
      [&resultingGameModeId](Room& room)
      {
        resultingGameModeId = static_cast<registry::GameModeId>(
          room.GetRoomDetails().gameMode);
      });

    if (options.test(3))
    {
      switch (command.gameMode)
      {
        case protocol::GameMode::Speed:
          resultingGameModeId = static_cast<registry::GameModeId>(
            Room::GameMode::Speed);
          break;
        case protocol::GameMode::Magic:
          resultingGameModeId = static_cast<registry::GameModeId>(
            Room::GameMode::Magic);
          break;
        case protocol::GameMode::Tutorial:
          resultingGameModeId = static_cast<registry::GameModeId>(
            Room::GameMode::Tutorial);
          break;
        default:
          // Неизвестный режим мутация ниже не применит (там default пишет
          // ERROR и оставляет прежний) — значит и здесь он ничего не меняет.
          break;
      }
    }

    bool isMapBlockAllowed =
      command.mapBlockId == AllMapsCourseId
      || command.mapBlockId == NewMapsCourseId
      || command.mapBlockId == HotMapsCourseId;

    if (not isMapBlockAllowed)
    {
      try
      {
        const auto& gameModeInfo = _serverInstance.GetCourseRegistry()
          .GetCourseGameModeInfo(resultingGameModeId);

        isMapBlockAllowed = std::ranges::contains(
          gameModeInfo.mapBlockPool,
          static_cast<registry::MapBlockId>(command.mapBlockId));
      }
      catch (const std::exception&)
      {
        // Режима нет в courses.yaml — сверять не с чем, карту не разрешаем.
        isMapBlockAllowed = false;
      }
    }

    if (not isMapBlockAllowed)
    {
      server::util::QuietLogWarn(
        "AcCmdCRChangeRoomOptions: character {} requested map block {} outside "
        "the map pool of game mode {} for room {}; keeping the current map and "
        "applying the remaining options",
        clientContext.characterUid,
        command.mapBlockId,
        resultingGameModeId,
        clientContext.roomUid);

      isMapChangeAllowed = false;
    }
  }

  if (options.test(0))
  {
    _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
      [&command, clientContext](const data::Character& character)
      {
        server::util::QuietLogInfo("Room {}'s name changed by '{}' ('{}') to '{}'",
          clientContext.roomUid,
          clientContext.userName,
          character.name(),
          command.name);
      });
  }

  // Change the room options.
  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    // LOA-fix (R11-8 + R11-16b, round11): в захвате появились
    // clampedPlayerCount (кламп посчитан ВЫШЕ, до мутации, чтобы им же
    // пользовался эхо-notify) и isMapChangeAllowed (решение по карте принято
    // выше, здесь оно только применяется).
    [&options, &command, clampedPlayerCount, isMapChangeAllowed](Room& room)
    {
      auto& roomDetails = room.GetRoomDetails();

      if (options.test(0))
      {
        roomDetails.name = command.name;
      }
      if (options.test(1))
      {
        // LOA-fix (R11-8, round11, backlog #19 п.4): кладём КЛАМПНУТОЕ число.
        // Обоснование и сам кламп — выше по обработчику. Уже сидящих в комнате
        // это не выселяет: Room::AddPlayer лишь перестаёт пускать новых.
        // Эхо-notify читает это же значение из roomDetails, поэтому сервер и
        // клиенты показывают одно и то же количество слотов.
        roomDetails.maxPlayerCount = clampedPlayerCount;
      }
      if (options.test(2))
      {
        roomDetails.password = command.password;
      }
      if (options.test(3))
      {
        switch (command.gameMode)
        {
          case protocol::GameMode::Speed:
            roomDetails.gameMode = Room::GameMode::Speed;
            break;
          case protocol::GameMode::Magic:
            roomDetails.gameMode = Room::GameMode::Magic;
            break;
          case protocol::GameMode::Tutorial:
            roomDetails.gameMode = Room::GameMode::Tutorial;
            break;
          default:
            server::util::QuietLogError("Unknown game mode '{}'", static_cast<uint32_t>(command.gameMode));
        }
      }
      // LOA-fix (R11-16b, round11, backlog #23): карта применяется, только если
      // прошла проверку по пулу игрового режима (выше по обработчику).
      // Отклонённая карта НЕ отбивает остальные опции пакета — комната просто
      // остаётся на прежней, а notify ниже расскажет об этом всем клиентам.
      if (options.test(4) && isMapChangeAllowed)
      {
        roomDetails.courseId = command.mapBlockId;
      }
      if (options.test(5))
      {
        roomDetails.npcDifficulty = command.npcDifficulty;
      }
    });

  // LOA-fix (R11-16c, round11, backlog #23 + #19 п.4): NOTIFY ИЗ СОСТОЯНИЯ
  // КОМНАТЫ, А НЕ ИЗ ПОЛЕЙ ПАКЕТА.
  // Апстрим пересылал клиентам ровно то, что прислал инициатор, и это работало
  // только пока сервер применял пакет дословно. Теперь сервер вправе НЕ принять
  // часть пакета — кламп числа игроков (R11-8) и отказ по карте (R11-16) — и
  // эхо сырых полей развело бы UI всей комнаты с сервером: у всех 10 слотов при
  // серверных 8, у инициатора выбранная тестовая карта при серверной прежней.
  // Собираем notify из roomDetails ПОСЛЕ мутации: тогда сервер и клиенты сходятся
  // по определению, а UI инициатора откатывается на то, что реально применилось.
  // Биты optionsBitfield оставляем как пришли — иначе клиент просто не прочитает
  // поле, которое мы и хотим откатить.
  // Бонусом чинится и апстримный край: неизвестный gameMode (default в switch
  // мутации) раньше уезжал клиентам как принятый, хотя комната его не приняла.
  protocol::AcCmdCRChangeRoomOptionsNotify notify{
    .optionsBitfield = command.optionsBitfield};

  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    [&notify](Room& room)
    {
      const auto& roomDetails = room.GetRoomDetails();
      notify.name = roomDetails.name;
      notify.playerCount = static_cast<uint8_t>(roomDetails.maxPlayerCount);
      notify.password = roomDetails.password;
      notify.gameMode = static_cast<protocol::GameMode>(roomDetails.gameMode);
      notify.mapBlockId = roomDetails.courseId;
      notify.npcDifficulty = roomDetails.npcDifficulty;
    });

  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    [this, &notify](const Room& room)
    {
      for (const auto& player : room.GetPlayers() | std::views::values)
      {
        try
        {
          _commandServer.QueueCommand<protocol::AcCmdCRChangeRoomOptionsNotify>(
            player.GetClientId(),
            [notify]()
            {
              return notify;
            });
        }
        catch (const std::exception&)
        {
          // the player disconnected
        }
      }
    });
}

void RaceNetworkHandler::HandleChangeTeam(
  const ClientId clientId,
  const protocol::AcCmdCRChangeTeam& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // LOA-fix (R66-5a, backlog #137): ГАРД ВЛАДЕНИЯ.
  //
  // До него ключ брался прямо из пакета и ничем не сверялся: любой клиент,
  // сидящий в комнате ожидания, мог назвать идентификатор СОСЕДА и переставить
  // его между командами — сервер послушно применял, а жертва узнавала об этом
  // из notify, который ей же и рассылали.
  //
  // ★СРАВНИВАЕМ С `characterUid`, А НЕ С oid ГОНЩИКА. Соседний гард R57
  // (`HandleRaceUserPos`: `command.oid != racer.oid`) выглядит готовым образцом,
  // и здесь он НЕ РАБОТАЕТ: oid'ы раздаёт `RaceTracker::AddRacer` только на
  // старте заезда, а в комнате ожидания их не существует вовсе — сравнивать
  // было бы не с чем, и гард либо не сработал бы, либо отверг всех.
  // Величина, которая тут реально ходит по проводу, — characterUid: ростер
  // комнаты сервер САМ заполняет им (`protocolRacer.uid = character.uid()`,
  // `HandleEnterRoom`), другого идентификатора игрока комнаты у клиента нет.
  // ★Отсюда и имя поля `AcCmdCRChangeTeam.characterOid` — оно лжёт, несёт
  // characterUid. Переименование сознательно НЕ делаем (обёртка зеркалит
  // wire-имена клиента); пометка стоит у объявления поля.
  //
  // ★ОТКАЗ МОЛЧАЛИВЫЙ, И ЭТО ВЫБОР, А НЕ НЕДОДЕЛКА. Во-первых, такова уже
  // существующая семантика отказа этого обработчика (ниже дословно: «No
  // response needed, client does not change until it receives an OK») — без OK
  // клиентский UI не двигается. Во-вторых, строка лога здесь была бы
  // удалённо-управляемой: ровно этот класс дал 15 350 строк [error] за час
  // живой игры и лечился в R57 (#195). Наблюдаемость гарда даёт ПОВЕДЕНИЕ
  // (нет OK, нет notify, команда соседа не изменилась), а не запись в журнал.
  if (command.characterOid != clientContext.characterUid)
    return;

  // LOA-fix (R66-5b, backlog #137): СТАДИЯ ПРОВЕРЯЕТСЯ ДО ВСЯКОЙ МУТАЦИИ.
  //
  // Раньше блок смены команды стоял ВЫШЕ этой проверки: вне комнаты ожидания
  // изменение всё равно ПРИМЕНЯЛОСЬ к комнате, подавлялись только OK и notify.
  // То есть сервер тихо расходился с тем, что видят все клиенты, и расхождение
  // доживало до старта заезда, где состав команд читается из комнаты
  // (`HandleStartRace` переносит `roomPlayer.GetTeam()` в трекер).
  //
  // ★ПОРЯДОК ЗАМКОВ ПРИ ПЕРЕНОСЕ НЕ МЕНЯЕТСЯ. Пара «_raceInstancesMutex →
  // замок комнаты» уже берётся В ЭТОМ ЖЕ обработчике на успешном пути
  // (`BroadcastExceptCharacterUid` → `RaceInstance::GetRoom` →
  // `RoomSystem::GetRoom`), и в этом файле она берётся ещё на 19 участках
  // (`HandleEnterRoom` ×5, `HandleLeaveRoom` ×4, `HandleStartRace` ×6,
  // `HandleKickUser` ×2, `HandleAwardStart`, `HandleUserRaceItemGet`),
  // тогда как обратной пары («замок комнаты → _raceInstancesMutex»)
  // нет ни одной — ни прямо в лямбдах-потребителях `GetRoom`, ни через вызов
  // метода, который берёт этот замок. До правки инверсии не было лишь потому,
  // что замки не пересекались во времени; теперь порядок стал ЯВНЫМ и
  // совпадает с господствующим в файле.
  // ★Побочно чинится вторая половина того же дефекта: `GetRaceInstance` умеет
  // бросить (комнаты нет / uid невалиден) — раньше бросок случался ПОСЛЕ
  // мутации, то есть комната успевала измениться под исключение.
  std::scoped_lock lock(_raceInstancesMutex);
  const auto& raceInstance = GetRaceInstance(clientContext, false);

  if (raceInstance.GetStage() != RaceInstance::Stage::Waiting)
  {
    // A racer tried to change teams when not in the waiting room
    // No response needed, client does not change until it receives an OK
    return;
  }

  // LOA-fix (R66-5c, backlog #137): НЕБРОСАЮЩИЙ ПОИСК ИГРОКА.
  //
  // `Room::GetPlayer` бросает `std::runtime_error("Room player does not
  // exist")` на неизвестном ключе, а ключ приходил из пакета — удалённый
  // клиент одной строкой вызывал исключение из-под замка комнаты.
  //
  // ★В КАКОМ СОСТОЯНИИ ОСТАВАЛАСЬ КОМНАТА (вопрос R49/R50) — ОТВЕТ: ЦЕЛОЙ, и
  // это ровно тот вопрос, который нельзя было не задать. Бросок случался ДО
  // единственной мутации (`SetTeam`), поэтому полуприменённого состояния не
  // возникало; замок комнаты снимался раскруткой стека, потому что
  // `RoomSystem::GetRoom` держит его через `scoped_lock` (RAII), а
  // `_raceInstancesMutex` на тот момент ещё не был взят. Ущерб был не в порче
  // состояния, а в том, что удалённый ввод порождал исключение и строку
  // [error] «Unhandled exception handling command» на границе `CommandServer`.
  // Заменяем на чистый отказ: ни исключения, ни ответа.
  //
  // ★ЧЕСТНО О ДОСТИЖИМОСТИ (NIT ревью, итерация 1 — прежнее обоснование здесь
  // было ЛОЖНЫМ, и в этом проекте лживый комментарий сам по себе дефект, #139).
  // Я утверждала, что клиент может стоять в `Room::_queuedPlayers` при уже
  // выставленном `roomUid`. Это неверно: `HandleEnterRoom` вызывает
  // `Room::AddPlayer` РАНЬШЕ присвоения `clientContext.roomUid`, а сам
  // `AddPlayer` стирает запись очереди ДО вставки в `_players`; на пути
  // «комната переполнена» roomUid вообще не присваивается.
  // ПРОВЕРЕНО И НЕ НАЙДЕНО достижимой комбинации «roomUid валиден, гонка
  // существует, а игрока в `_players` нет»: `Room::RemovePlayer` зовётся ровно
  // из двух мест, и оба в том же обработчике гасят roomUid.
  // ★ПОЭТОМУ ЭТО ЭШЕЛОНИРОВАНИЕ, А НЕ ЗАКРЫТИЕ ИЗВЕСТНОЙ ДЫРЫ, и оставлено
  // сознательно: `Room::GetPlayer` бросает ПО КОНТРАКТУ, а этот обработчик не
  // должен держаться на инварианте, который поддерживают три функции в стороне
  // и которого никто не проверяет ([[total-invariant-beats-list-of-sites]]).
  // Цена — одна проверка хеш-карты; цена ошибки в инварианте — исключение с
  // удалённым триггером.
  bool playerFound = false;
  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    [&command, &clientContext, &playerFound](Room& room)
    {
      if (not room.HasPlayer(clientContext.characterUid))
        return;
      playerFound = true;

      // ★КЛЮЧ БЕРЁМ ИЗ КОНТЕКСТА СЕРВЕРА, А НЕ ИЗ ПАКЕТА. После гарда выше
      // значения равны, поэтому поведение не меняется ни на байт; но код
      // перестаёт зависеть от клиентского числа, и если гард когда-нибудь
      // ослабнет, поиск всё равно не уйдёт на чужую строку.
      auto& player = room.GetPlayer(clientContext.characterUid);
      switch (command.teamColor)
      {
        case protocol::TeamColor::Red:
          player.SetTeam(Room::Player::Team::Red);
          break;
        case protocol::TeamColor::Blue:
          player.SetTeam(Room::Player::Team::Blue);
          break;
        default: {}
      }
    });

  // ★`default: {}` ВЫШЕ ОСТАВЛЕН КАК БЫЛ, И ЭТО ОСОЗНАННО. `TeamColor::Solo`
  // равен `None` (= 0) и приходит легитимно, а сегодня такой запрос получает
  // OK без мутации. Превратить его в отказ значило бы поменять УСПЕШНЫЙ путь
  // ради пути отказа, не имея наблюдения о том, как на это реагирует
  // настоящий клиент ([[dont-trade-success-path-for-failure-path]]). Поэтому
  // `playerFound` означает «игрок в комнате нашёлся», а не «команда
  // изменилась»: OK и notify уходят ровно в тех же случаях, что и до R66.
  // Отдельная запись бэклога, не R66.
  if (not playerFound)
    return;

  const protocol::AcCmdCRChangeTeamOK response{
    .characterOid = command.characterOid,
    .teamColor = command.teamColor};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  // Notify all other clients in the room
  const protocol::AcCmdCRChangeTeamNotify notify{
    .characterOid = command.characterOid,
    .teamColor = command.teamColor};
  this->BroadcastExceptCharacterUid(
    raceInstance,
    notify,
    clientContext.characterUid);
}

void RaceNetworkHandler::HandleLeaveRoom(ClientId clientId)
{
  protocol::AcCmdCRLeaveRoomOK response{};

  auto& clientContext = GetClientContext(clientId);
  if (clientContext.roomUid == 0)
    return;

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext, false);

  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [clientContext](const data::Character& character)
    {
      server::util::QuietLogInfo("Player {} ({}) has left [Room {}]",
        clientContext.userName,
        character.name(),
        clientContext.roomUid);
    });

  if (raceInstance.GetTracker().IsRacer(clientContext.characterUid))
  {
    auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);
    racer.state = tracker::RaceTracker::Racer::State::Disconnected;

    // Notify all the other racers that the client has disconnected
    const protocol::AcCmdUserRaceDeleteNotify deleteNotify{
      .racerOid = racer.oid};
    this->BroadcastExceptCharacterUid(
      raceInstance,
      deleteNotify,
      clientContext.characterUid);
  }

  data::Uid roomMasterUid{data::InvalidUid};
  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    [&roomMasterUid, characterUid = clientContext.characterUid](Room& room)
    {
      roomMasterUid = room.GetRoomDetails().masterUid;
      room.RemovePlayer(characterUid);
    });

  // Check if the leaving player was the leader
  const bool wasMaster = roomMasterUid == clientContext.characterUid;

  {
    // Notify other clients in the room about the character leaving.
    const protocol::AcCmdCRLeaveRoomNotify notify{
      .characterId = clientContext.characterUid,
      .unk0 = 1};
    // No need to prevent self broadcast, player should be
    // removed from the room
    this->Broadcast(raceInstance, notify);
  }

  if (wasMaster)
  {
    std::vector<data::Uid> candidates;

    // If the race room is waiting pick from room users,
    // otherwise we have to pick a player from the race.
    // This prevents the new leader from being able to start
    // next race and cause confusion.
    if (raceInstance.GetStage() == RaceInstance::Stage::Waiting)
    {
      _serverInstance.GetRoomSystem().GetRoom(
        clientContext.roomUid,
        [&candidates](const Room& room)
        {
          std::ranges::copy(
            room.GetPlayers() | std::views::keys,
            std::back_inserter(candidates));
        });
    }
    else
    {
      // Get active racers (that are still connected)
      auto& tracker = raceInstance.GetTracker();
      std::ranges::copy_if(
        tracker.GetRacers() | std::views::keys,
        std::back_inserter(candidates),
        [&tracker](const data::Uid characterUid)
        {
          const auto& racer = tracker.GetRacer(characterUid);
          return racer.state != tracker::RaceTracker::Racer::State::Disconnected;
        });
    }

    // Pick a candidate
    // For now, we pick the first racer
    // todo: sort by performance
    if (not candidates.empty())
    {
      const data::Uid candidateUid = candidates.front();
      const auto& newMasterClientContext = GetClientContextByCharacterUid(candidateUid);

      std::string newMasterCharacterName;
      _serverInstance.GetDataDirector().GetCharacter(newMasterClientContext.characterUid).Immutable(
        [&newMasterCharacterName](const data::Character& character)
        {
          newMasterCharacterName = character.name();
        });

      server::util::QuietLogInfo("Player {} ({}) became the master of [Room {}] after the previous master left",
        newMasterClientContext.userName,
        newMasterCharacterName,
        clientContext.roomUid);

      // Update the room details to make the new master official
      _serverInstance.GetRoomSystem().GetRoom(
        clientContext.roomUid,
        [masterUid = newMasterClientContext.characterUid](Room& room)
        {
          room.GetRoomDetails().masterUid = masterUid;
        });

      // Notify other clients in the room about the new master.
      const protocol::AcCmdCRChangeMasterNotify notify{
        .masterUid = newMasterClientContext.characterUid};
      this->Broadcast(raceInstance, notify);
    }
  }

  {
    // Delete room if empty
    bool roomEmpty{false};
    raceInstance.GetRoom(
      [this, &roomEmpty](const Room& room)
      {
        if (room.GetPlayerCount() != 0)
          // Room is not empty
          return;

        roomEmpty = true;
      });

    if (roomEmpty)
    {
      _serverInstance.GetRoomSystem().DeleteRoom(clientContext.roomUid);
      _raceInstances.erase(clientContext.roomUid);
    }
  }

  clientContext.roomUid = data::InvalidUid;

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RaceNetworkHandler::HandleReadyRace(
  const ClientId clientId,
  const protocol::AcCmdCRReadyRace&)
{
  const auto& clientContext = GetClientContext(clientId);

  bool isPlayerReady = false;
  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    [&isPlayerReady, characterUid = clientContext.characterUid](Room& room)
    {
      isPlayerReady = room.GetPlayer(characterUid).ToggleReady();
    });

  const protocol::AcCmdCRReadyRaceNotify notify{
    .characterUid = clientContext.characterUid,
    .isReady = isPlayerReady};

  std::scoped_lock lock(_raceInstancesMutex);
  const auto& raceInstance = GetRaceInstance(clientContext, false);
  this->Broadcast(raceInstance, notify);
}

void RaceNetworkHandler::HandleStartRace(
  const ClientId clientId,
  [[maybe_unused]] const protocol::AcCmdCRStartRace& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext, false);

  // LOA-fix (R8-1b, round8): ГАРД ПОВТОРНОГО СТАРТА (re-entrant StartRace).
  // ЧТО БЫЛО НЕ ТАК: единственными проверками были «отправитель — мастер
  // комнаты» и Room::CanRoomStart() (готовность игроков + баланс команд).
  // Стадию самого заезда не смотрел никто, поэтому мастер мог прислать
  // AcCmdCRStartRace ПОСРЕДИ заезда. Последствия две штуки:
  //   1) Tracker::Clear() ниже сносил всех гонщиков, AddRacer заводил их заново
  //      с finishCounted = false — латч идемпотентности финиша (R7 BLOCK-1)
  //      обнулялся, и цикл StartRace → RaceFinal накручивал сюжетные счётчики
  //      заездов (вместе с R8-1a это был полноценный эксплойт: метка старта
  //      протухала, гейт правдоподобности B1 проходил мгновенно);
  //   2) апстримный грифинг: Clear() посреди заезда выбивал остальных игроков
  //      комнаты из трекера, и их пакеты падали в «Character is not a racer».
  // ТЕПЕРЬ: стартовать можно только из Stage::Waiting — состояния «заезда нет».
  // Оно восстанавливается штатно: TickFinishing зовёт Stop() и ставит Waiting.
  // Честную игру не задевает — легальный StartRace приходит из комнаты, где
  // предыдущий заезд уже завершён.
  // Отвечаем существующим StartRaceCancel (Generic), а не throw: бросок здесь
  // унёс бы соединение мастера из-за одного лишнего/дублированного пакета.
  // LOA-fix (R11-15, round11, backlog #20 п.6): СМЯГЧЕНИЕ ГАРДА R8-1b —
  // ПРЕДОХРАНИТЕЛЬ, А НЕ ОСНОВНОЕ ЛЕЧЕНИЕ.
  // R8-1b (выше) закрыл re-entrant StartRace, но вместе с ним отнял апстримный
  // аварийный рестарт застрявшей комнаты: любая комната вне Waiting становилась
  // неразбираемой.
  // ЧЕСТНАЯ ОЦЕНКА ПОЛЬЗЫ (уточнено ревью панели 2026-08-17). Конкретный залип,
  // с которого начинали, — соло-комната без дедлайна — закрыт самим R11-14, и
  // каждая стадия теперь самолечится за один Tick (Loading → Racing по
  // дедлайну, Racing → Finishing, Finishing → Stop → Waiting), причём Tick
  // прогоняется по ВСЕМ инстансам каждый тик. Так что эта ветка — не «расшивка
  // соло-залипа», её больше нет, а СТРАХОВКА НА ОБЩИЙ СЛУЧАЙ: если самолечение
  // Tick по какой-то причине не сработало (например, Tick стабильно бросает
  // исключение на битой карте — RaceNetworkHandler::Tick его ловит и логирует,
  // комната остаётся в своей стадии навсегда), у мастера остаётся способ
  // разобрать комнату руками, не перезапуская сервер.
  // Разрешаем старт, если стадия ПРОСРОЧИЛА собственный дедлайн с запасом.
  // Инвариант раунда 8 сохраняется: пока заезд идёт нормально, Tracker::Clear()
  // недостижим. Эксплойтом это не становится — один цикл стоил бы (лимит стадии
  // + запас) реального времени, то есть минуты на каждую накрутку, и упирается
  // в гейт правдоподобности B1 и в латч finishCounted.
  // time_point::max() трактуем как «дедлайна нет» — просрочить его нельзя.
  constexpr auto StuckRoomGracePeriod = std::chrono::seconds(30);

  const bool isRaceInProgress =
    raceInstance.GetStage() != RaceInstance::Stage::Waiting;
  const auto stageDeadline = raceInstance.GetStageTimeoutTimePoint();
  const bool isStageOverdue = isRaceInProgress
    && stageDeadline != RaceInstance::Clock::time_point::max()
    && RaceInstance::Clock::now() > stageDeadline + StuckRoomGracePeriod;

  if (isStageOverdue)
  {
    server::util::QuietLogWarn(
      "AcCmdCRStartRace: room {} is stuck at stage {} past its deadline; "
      "allowing character {} to restart it",
      clientContext.roomUid,
      static_cast<uint32_t>(raceInstance.GetStage()),
      clientContext.characterUid);
  }

  if (isRaceInProgress && not isStageOverdue)
  {
    server::util::QuietLogWarn(
      "AcCmdCRStartRace: character {} tried to start room {} while a race is "
      "already in progress (stage {}); ignoring",
      clientContext.characterUid,
      clientContext.roomUid,
      static_cast<uint32_t>(raceInstance.GetStage()));

    SendStartRaceCancel(clientId, protocol::AcCmdCRStartRaceCancel::Reason::Generic);
    return;
  }

  // Check if all race requirements are met to start the race
  data::Uid roomMasterUid{data::InvalidUid};
  Room::PreventStartReason preventStartReason{};
  _serverInstance.GetRoomSystem().GetRoom(
    clientContext.roomUid,
    [&preventStartReason, &roomMasterUid, invokerCharacterUid = clientContext.characterUid](Room& room)
    {
      roomMasterUid = room.GetRoomDetails().masterUid;
      if (invokerCharacterUid != roomMasterUid)
        throw std::runtime_error("Client tried to start the race even though they're not the master");

      preventStartReason = room.CanRoomStart();
    });

  // Check if there is a reason why race cannot start
  switch (preventStartReason)
  {
    case Room::PreventStartReason::None:
      // No reason to prevent race start, continue
      break;
    case Room::PreventStartReason::NotAllPlayersReady:
      SendStartRaceCancel(clientId, protocol::AcCmdCRStartRaceCancel::Reason::NotReady);
      return;
    case Room::PreventStartReason::TeamImbalance:
      SendStartRaceCancel(clientId, protocol::AcCmdCRStartRaceCancel::Reason::NotTeamBalance);
      return;
    default:
      throw std::runtime_error("Prevent start reason not implemented");
  }

  const auto roomUid = clientContext.roomUid;

  RaceInstance::Parameters parameters;
  _serverInstance.GetRoomSystem().GetRoom(
    roomUid,
    [&parameters](Room& room)
    {
      auto& details = room.GetRoomDetails();

      parameters.gameMode = static_cast<protocol::GameMode>(details.gameMode);
      parameters.teamMode = static_cast<protocol::TeamMode>(details.teamMode);
      parameters.missionId = details.missionId;
      parameters.mapBlockId = details.courseId;
    });

  parameters.masterUid = roomMasterUid;

  // Clear the tracker before the race.
  raceInstance.GetTracker().Clear();

  if (not raceInstance.Start(parameters))
  {
    SendStartRaceCancel(clientId, protocol::AcCmdCRStartRaceCancel::Reason::Generic);
    return;
  }

  constexpr uint32_t GameCountdownKey = 17;
  constexpr uint32_t DefaultCountdownMs = 5310;

  const auto countdown = GetServerInstance()
    .GetSystemContentRegistry()
    .GetValue(GameCountdownKey);

  protocol::AcCmdRCRoomCountdown roomCountdown{
    .countdown = countdown.has_value()
      ? countdown.value()
      : DefaultCountdownMs,
    .mapBlockId = static_cast<uint16_t>(raceInstance.GetMapBlockId())};

  // Start with bonus course set to none by default
  raceInstance.SetBonusCourseType(protocol::BonusCourseType::None);

  // Randomly assign a bonus course if the room has 8 players
  {
    constexpr size_t RequiredPlayerCount = 8;
    bool hasRequiredPlayerCount = false;
    _serverInstance.GetRoomSystem().GetRoom(
      roomUid,
      [&hasRequiredPlayerCount](const Room& room)
      {
        hasRequiredPlayerCount = room.GetPlayerCount() == RequiredPlayerCount;
      });

    if (hasRequiredPlayerCount)
    {
      auto& gen = server::util::GetRandomEngine();
      std::uniform_int_distribution<uint32_t> chanceDist(1, 100);

      constexpr uint32_t BonusCourseChance = 25;
      const bool isBonusCourse = chanceDist(gen) <= BonusCourseChance;
      if (isBonusCourse)
      {
        std::uniform_int_distribution<uint32_t> typeDist(1, 3);
        const auto selectedType = static_cast<protocol::BonusCourseType>(typeDist(gen));
        roomCountdown.bonusCourseType = selectedType;
        raceInstance.SetBonusCourseType(selectedType);
      }
    }
  }

  // Broadcast room countdown.
  this->Broadcast(raceInstance, roomCountdown);

  // Add the racers.
  _serverInstance.GetRoomSystem().GetRoom(
    roomUid,
    [&raceInstance](Room& room)
    {
      // todo: observers
      for (const auto& [characterUid, roomPlayer] : room.GetPlayers())
      {
        auto& racer = raceInstance.GetTracker().AddRacer(characterUid);
        racer.state = tracker::RaceTracker::Racer::State::Loading;
        // LOA-fix (R7 BLOCK-1, round7): явный сброс латча идемпотентности
        // финиша. AddRacer и так отдаёт свежий Racer (Tracker::Clear() стоит
        // выше по функции), но состояние заезда обязано инициализироваться там
        // же, где ставится state — иначе любое будущее переиспользование
        // трекера (пул гонщиков / рестарт заезда без Clear) тихо унесло бы
        // квестовые счётчики в накрутку.
        racer.finishCounted = false;
        // LOA-fix (R24, #14 фаза 1): пер-заездная телеметрия обнуляется ЗДЕСЬ же,
        // где ставится state — по той же причине, что finishCounted: переиспользование
        // трекера без Clear() иначе тихо унесло бы чужой пробег в лошадь.
        racer.topSpeedKph = 0.0f;
        racer.distanceMetres = 0.0;
        racer.hasPositionSample = false;
        racer.lastPositionTimePoint =
          std::chrono::steady_clock::time_point::max();
        // LOA-fix (R75, #14 Ф2): планирование и цепочка рывков — пер-заездные,
        // обнуляем там же, где телеметрию R24 (причина та же).
        racer.previousAirborne = false;
        racer.currentAirborneMetres = 0.0f;
        racer.currentStretchIsGlide = false;
        racer.lastStretchMetres = 0.0f;
        racer.lastLandingTimePoint = std::chrono::steady_clock::time_point::max();
        racer.glideMarkTimePoint = std::chrono::steady_clock::time_point::max();
        racer.longestGlideMetres = 0.0f;
        racer.boostCombo = 0;
        racer.boostComboMax = 0;
        racer.lastSpurTimePoint = std::chrono::steady_clock::time_point::max();
        // LOA-fix (R-revenge, #13): состояние мести — пер-заездное, обнуляем
        // ровно там же, где остальное состояние заезда (та же причина, что у
        // finishCounted: переиспользование трекера без Clear() иначе перенесло
        // бы чужие зачёты в следующий заезд = бесплатные морковки).
        racer.trustedProgress = 0.0f;
        racer.trustedProgressTimePoint =
          std::chrono::steady_clock::time_point::max();
        racer.revengeRows.clear();
        racer.revengeCredits = 0;
        // LOA-fix (R76, #30 этап 1): журнал трассы — пер-заездный, обнуляем в
        // ТОМ ЖЕ списке, что finishCounted / topSpeedKph / trustedProgress.
        // Инвариант раунда: НОВОЕ ПОЛЕ ОБЯЗАНО ПОЯВИТЬСЯ ЗДЕСЬ. Сегодня это
        // защита «на будущее» (Clear() выше по функции сносит гонщиков целиком,
        // и AddRacer отдаёт свежего) — но ровно та же защита у finishCounted и
        // R24 стоит по той же причине, и снимать её из «оптимизации» нельзя.
        racer.progressSplits.fill(
          tracker::RaceTracker::Racer::InvalidSplitMs);
        racer.splitsReached = 0;
        racer.posSampleCount = 0;
        racer.positionJumps = 0;
        racer.discardedMetres = 0.0;
        racer.maxDiscardedStepMetres = 0.0f;
        racer.progressClipped = 0;
        racer.maxDeclaredProgress = 0.0f;
        switch (roomPlayer.GetTeam())
        {
          case Room::Player::Team::Solo:
            racer.team = tracker::RaceTracker::Racer::Team::Solo;
            break;
          case Room::Player::Team::Red:
            racer.team = tracker::RaceTracker::Racer::Team::Red;
            break;
          case Room::Player::Team::Blue:
            racer.team = tracker::RaceTracker::Racer::Team::Blue;
            break;
        }
      }
    });

  // LOA-fix (R68, backlog #5/#99): РАСКЛАДКА КВЕСТОВЫХ ПРЕДМЕТОВ.
  // ★Место выбрано не «поближе». Раскладка обязана идти ПОСЛЕ `AddRacer`
  // (иначе гонщиков ещё нет и класть предметы некому) и ДО первого прохода
  // спавнеров: `TickItemSpawners` рассылает всё, что лежит в трекере, на
  // ПЕРВОМ же тике — флаг `firstPassItemSpawn` после `Clear()` ещё поднят,
  // и проверка близости на этом проходе не применяется. Оба условия
  // выполняются ровно здесь.
  raceInstance.PrepareQuestItems();

  // === R56 (#61): соло-заезд получает AI-соперников ========================
  // Отдельного игрового режима «Race Alone» в протоколе нет
  // (`GameMode` = Speed / Magic / Unk4 / Tutorial), поэтому одиночество
  // выводится из состава заезда. Считаем ПО ТРЕКЕРУ, а не по комнате: в
  // трекере ровно те, кто реально поехал (наблюдатели и не готовые отсеяны
  // выше). Обучение исключаем — там пустая трасса часть сценария.
  {
    const bool isSoloRace =
      raceInstance.GetTracker().GetRacers().size() == 1
      && parameters.gameMode != protocol::GameMode::Tutorial;

    if (isSoloRace)
      SpawnAiRacers(raceInstance);
  }

  _serverInstance.GetRoomSystem().GetRoom(
    roomUid,
    [](Room& room)
    {
      room.SetRoomPlaying(true);
    });

  // Queue race start after room countdown.
  _scheduler.Queue(
    [this, roomUid]()
    {
      std::scoped_lock raceInstanceLock(_raceInstancesMutex);

      const auto raceInstanceIter = _raceInstances.find(roomUid);;
      if (raceInstanceIter == _raceInstances.cend())
        return;

      auto& raceInstance = raceInstanceIter->second;
      const auto& parameters = raceInstance.GetParameters();

      // LOA-fix (R11-3c, round11, backlog #20 п.4): дедлайн загрузки взводим
      // ЗДЕСЬ. Эта лямбда выполняется по истечении countdown комнаты и прямо
      // ниже рассылает AcCmdCRStartRaceNotify — то есть это и есть момент,
      // когда клиенты начинают грузить карту. Дедлайн, взведённый в
      // RaceInstance::Start(), съедал длину countdown (дефолт 5310 мс) из
      // бюджета загрузки, и «60 секунд» на деле были ~54.7. Теперь написанное в
      // LoadingStageTimeout совпадает с реальностью при любом countdown с прода.
      raceInstance.ArmLoadingDeadline();

      const auto& lobbyConfig = GetServerInstance().GetLobbyDirector().GetConfig();
      protocol::AcCmdCRStartRaceNotify notify{
        .raceGameMode = parameters.gameMode,
        .raceTeamMode = parameters.teamMode,
        .raceMapBlockId = static_cast<uint16_t>(raceInstance.GetMapBlockId()),
        .p2pRelayAddress = lobbyConfig.advertisement.udpRaceRelay.address.to_uint(),
        .p2pRelayPort = lobbyConfig.advertisement.udpRaceRelay.port,
        .raceMissionId = parameters.missionId,
        // LOA-fix (S2): включаем per-race флаг травм — как в оригинальной игре.
        // Эмулятор жёстко слал false → клиент считал травмы выключенными. true =
        // клиент снова сам применяет свою логику травмированной лошади. Пара к
        // серверному инфликту в RaceInstance::Stop и лечению в RanchDirector.
        .isHorseInjuryEnabled = true,};

      // Build the racers.
      for (const auto& [characterUid, racer] : raceInstance.GetTracker().GetRacers())
      {
        if (racer.state == tracker::RaceTracker::Racer::State::Disconnected)
          continue;

        std::string characterName;
        GetServerInstance().GetDataDirector().GetCharacter(characterUid).Immutable(
          [&characterName](const data::Character& character)
          {
            characterName = character.name();
          });

        auto& protocolRacer = notify.racers.emplace_back(
          protocol::AcCmdCRStartRaceNotify::Player{
            .oid = racer.oid,
            .name = characterName});

        // Assign the racer P2dId
        const ClientId racerClientId = GetClientIdByCharacterUid(characterUid);
        protocolRacer.p2dId = GetOrCreateP2dId(racerClientId);

        switch (racer.team)
        {
          case tracker::RaceTracker::Racer::Team::Solo:
            protocolRacer.teamColor = protocol::TeamColor::None;
            break;
          case tracker::RaceTracker::Racer::Team::Red:
            protocolRacer.teamColor = protocol::TeamColor::Red;
            break;
          case tracker::RaceTracker::Racer::Team::Blue:
            protocolRacer.teamColor = protocol::TeamColor::Blue;
            break;
        }
      }

      // === R56 (#61): боты в ростере старта ==============================
      // Это ВЕСЬ контракт с клиентом по ботам: он водит их сам, ему достаточно
      // увидеть их в ростере с признаком «это AI» и номером личности — дальше
      // он берёт свою таблицу вождения и едет. Сервер за них не считает ни
      // метра. Структура пакета не менялась: поля `unk2/unk3/unk6/unk7` уже
      // были на проводе и до сих пор оставались нулями.
      for (const auto& aiRacer : raceInstance.GetAiRacers())
      {
        notify.racers.emplace_back(
          protocol::AcCmdCRStartRaceNotify::Player{
            .oid = aiRacer.oid,
            .name = aiRacer.name,
            // 1 = «этот участник — AI, веди его сам».
            .unk2 = 1,
            // Номер личности внутри тира: выбор таблицы вождения.
            .unk3 = aiRacer.personality,
            .p2dId = aiRacer.p2dId,
            .teamColor = protocol::TeamColor::None,
            // Значения апстрима. Смысл полей не установлен, поэтому
            // повторяем известное работающее, а не изобретаем своё.
            .unk6 = 1,
            .unk7 = 1});
      }

      const bool isEligibleForSkills = (notify.raceGameMode == protocol::GameMode::Speed
        || notify.raceGameMode == protocol::GameMode::Magic)
        && notify.raceTeamMode == protocol::TeamMode::FFA;

      // Send to all clients participating in the race.
      raceInstance.GetRoom(
        [this, &raceInstance, &notify, isEligibleForSkills](const Room& room)
        {
          for (const auto& [characterUid, player] : room.GetPlayers())
          {
            if (not raceInstance.GetTracker().IsRacer(characterUid))
              continue;

            auto& racer = raceInstance.GetTracker().GetRacer(characterUid);
            notify.hostOid = racer.oid;

            // Skills only apply for speed single or magic single
            if (isEligibleForSkills)
            {
              // Notify racer of confirmed selection of skills
              GetServerInstance().GetDataDirector().GetCharacter(characterUid).Immutable(
                [&notify](const data::Character& character)
                {
                  // Get skill set by gamemode
                  const auto& skillSets =
                    notify.raceGameMode == protocol::GameMode::Speed ? character.skills.speed() :
                    notify.raceGameMode == protocol::GameMode::Magic ? character.skills.magic() :
                      throw std::runtime_error("Unknown game mode");

                  // LOA-fix (R11-10, round11, backlog #21): ФОЛБЭК ВМЕСТО БРОСКА.
                  // Прежний тернарник бросал на любом activeSetId > 1. Бросок
                  // отсюда стоит дорого: мы внутри отложенной лямбды
                  // планировщика И внутри цикла рассылки по всем игрокам
                  // комнаты — исключение обрывает цикл, и AcCmdCRStartRaceNotify
                  // не получает ни виновник, ни все, кто стоял после него.
                  // Комната зависает на загрузке целиком. R11-9 закрывает
                  // источник, но в данных уже сохранённых персонажей значение 2
                  // остаётся, поэтому нужен и приёмник.
                  // В notify уходит УЖЕ исправленный id: иначе клиент получил бы
                  // несуществующий у него набор.
                  const auto activeSetId = skillSets.activeSetId <= 1
                    ? skillSets.activeSetId
                    : 0u;
                  if (activeSetId != skillSets.activeSetId)
                  {
                    server::util::QuietLogWarn(
                      "Character {} has an out-of-range active skill set {}; "
                      "falling back to set 1",
                      character.uid(),
                      skillSets.activeSetId);
                  }

                  // Get racer's active skill set ID and set it in notify
                  notify.racerActiveSkillSet.setId = static_cast<uint8_t>(activeSetId);

                  const auto& skillSet =
                    activeSetId == 1 ? skillSets.set2 : skillSets.set1;

                  // Slot 1, slot 2, bonus (calculated after)
                  notify.racerActiveSkillSet.skills[0] = skillSet.slot1;
                  notify.racerActiveSkillSet.skills[1] = skillSet.slot2;
                });

              // Bonus skills are unique for each racer in the racer
              // TODO: put these in a skill registry table
              std::vector<uint32_t> speedOnlyBonusSkills = {59, 32, 31};
              std::vector<uint32_t> magicOnlyBonusSkills = {34, 35, 36, 57, 58};
              std::vector<uint32_t> bonusSkillIds = {43, 29, 30}; // Speed + magic

              // Append to list depending on gamemode
              if (notify.raceGameMode == protocol::GameMode::Speed)
              {
                bonusSkillIds.insert(
                  bonusSkillIds.end(),
                  speedOnlyBonusSkills.begin(),
                  speedOnlyBonusSkills.end());
              }
              else if (notify.raceGameMode == protocol::GameMode::Magic)
              {
                bonusSkillIds.insert(
                  bonusSkillIds.end(),
                  magicOnlyBonusSkills.begin(),
                  magicOnlyBonusSkills.end());
              }

              std::uniform_int_distribution<uint32_t> bonusSkillDist(
                0,
                static_cast<uint32_t>(bonusSkillIds.size()) - 1);

              const auto bonusSkillIdx = bonusSkillDist(server::util::GetRandomEngine());
              notify.racerActiveSkillSet.skills[2] = bonusSkillIds[bonusSkillIdx];
            }

            _commandServer.QueueCommand<decltype(notify)>(
              player.GetClientId(),
              [notify]()
              {
                return notify;
              });
          }
        });
    },
    Scheduler::Clock::now() + std::chrono::milliseconds(roomCountdown.countdown));
}

void RaceNetworkHandler::SendStartRaceCancel(
  ClientId clientId,
  protocol::AcCmdCRStartRaceCancel::Reason reason)
{
  _commandServer.QueueCommand<protocol::AcCmdCRStartRaceCancel>(
    clientId,
    [reason]()
    {
      return protocol::AcCmdCRStartRaceCancel{
        .reason = reason};
    });
}

void RaceNetworkHandler::HandleRaceTimer(
  ClientId clientId,
  const protocol::AcCmdUserRaceTimer& command)
{
  protocol::AcCmdUserRaceTimerOK response{
    .clientRaceClock = command.clientClock,
    .serverRaceClock = util::TimePointToRaceTimePoint(
      std::chrono::steady_clock::now()),};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RaceNetworkHandler::HandleLoadingComplete(
  ClientId clientId,
  const protocol::AcCmdCRLoadingComplete&)
{
  auto& clientContext = GetClientContext(clientId);
  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();

  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  // LOA-fix (R11-12, round11, backlog #20 п.2): ГАРД СТАДИИ — ЧАСТЬ
  // СТЕЙТ-МАШИНЫ, А НЕ АНТИЧИТ. ВЫПЛАТ НЕ КАСАЕТСЯ: ни наград, ни счётчиков
  // квестов эта ветка не трогает, латч finishCounted (R7 BLOCK-1) живёт
  // отдельно и здесь не читается.
  // ЧТО БЫЛО НЕ ТАК: обработчик возвращал гонщика в Racing БЕЗУСЛОВНО — из
  // любого его состояния и на любой стадии заезда. Опоздавший, которого
  // TickLoading уже вычеркнул и о котором клиентам ушёл DeleteNotify (R11-11),
  // тем самым «воскресал» посреди чужого заезда: сервер снова считал его
  // участником и рассылал по нему LoadingCompleteNotify, а на клиентах его уже
  // нет. Ровно отсюда расходятся ростеры.
  // ЧТО РАЗРЕШЕНО ТЕПЕРЬ:
  //   Stage::Loading — всё как раньше, это штатный путь;
  //   Stage::Racing  — принимаем от того, кого ЕЩЁ НЕ вычеркнули. Так честный
  //                    игрок, чей LoadingComplete пришёл на тик-другой позже
  //                    перехода комнаты в Racing, не страдает — а именно за
  //                    это в раунде 6 откатили прежний гард;
  //   Finishing/Waiting — отбиваем: заезда для этого клиента уже нет.
  const auto raceStage = raceInstance.GetStage();
  const bool isLoadingCompleteAcceptable =
    raceStage == RaceInstance::Stage::Loading
    || (raceStage == RaceInstance::Stage::Racing
      && racer.state != tracker::RaceTracker::Racer::State::Disconnected);

  if (not isLoadingCompleteAcceptable)
  {
    server::util::QuietLogWarn(
      "AcCmdCRLoadingComplete: character {} reported loading complete in room "
      "{} at stage {} with racer state {}; ignoring to keep the roster "
      "consistent",
      clientContext.characterUid,
      clientContext.roomUid,
      static_cast<uint32_t>(raceStage),
      static_cast<uint32_t>(racer.state));

    // LOA-fix (R11-12b, round11, backlog #20 п.2): ОТБИТЬ — НЕ ЗНАЧИТ ПРОМОЛЧАТЬ
    // (найдено панелью 2026-08-17). У AcCmdCRLoadingComplete нет ответного
    // пакета, поэтому «просто return» оставлял опоздавшего висеть на экране
    // загрузки: клиент ждёт отсчёта, а его для него уже не будет. Отвечаем тем
    // же штатным AcCmdCRStartRaceCancel(Generic), что и TickLoading при
    // вычёркивании (R11-11b), — это единственное в протоколе «заезд для тебя не
    // состоится», и клиент выходит из загрузки сразу, а не через чужой заезд.
    // НЕ ШЛЁМ ВТОРОЙ РАЗ (NIT второй панели 2026-08-17). Ровно одна комбинация
    // означает «этому клиенту Cancel уже ушёл из TickLoading»: стадия Racing +
    // состояние Disconnected + выданный oid — это в точности условие отправки в
    // R11-11b (он шлёт тем, кого вычеркнул именно сейчас и у кого oid валиден).
    // Тогда молчим: реакция клиента на повторный Cancel не проверена, а лишний
    // диалог/двойной выход в лобби — видимый дефект. Во всех остальных ветках
    // отбоя (стадии Finishing/Waiting) Cancel уходит, потому что там его никто
    // не слал.
    // ОСТАТОЧНЫЙ СЛУЧАЙ, ЗАФИКСИРОВАН ЧЕСТНО: гонщик мог стать Disconnected не
    // от TickLoading, а от HandleLeaveRoom — тогда Cancel не слался, и мы его
    // тоже не пошлём. Это безвредно: игрок уже вышел из комнаты сам, терминальный
    // пакет ему не нужен.
    const bool wasStartRaceCancelAlreadySent =
      raceStage == RaceInstance::Stage::Racing
      && racer.state == tracker::RaceTracker::Racer::State::Disconnected
      && racer.oid != tracker::InvalidEntityOid;

    if (not wasStartRaceCancelAlreadySent)
    {
      SendStartRaceCancel(
        clientId,
        protocol::AcCmdCRStartRaceCancel::Reason::Generic);
    }

    return;
  }

  // Switch the racer to the racing state.
  //
  // LOA-fix (R66-3b, backlog #78, ИТЕРАЦИЯ 1 РЕВЬЮ): прежнее состояние
  // запоминается ДО перезаписи — телеметрия ниже печатается только на
  // НАСТОЯЩЕМ переходе «грузился → едет», см. её комментарий.
  const auto previousRacerState = racer.state;
  racer.state = tracker::RaceTracker::Racer::State::Racing;

  // LOA-fix (R66-3, backlog #78): сколько РЕАЛЬНО заняла загрузка у этого игрока.
  //
  // ★МАРКЕР `loaded` ЗДЕСЬ НЕ УКРАШЕНИЕ. Соседний путь — списание по дедлайну
  // (`RaceInstance::TickLoading`) — печатает СВОЮ строку «did not load in time».
  // Две ветки обязаны быть различимы В ТЕКСТЕ, иначе сводка перемешает «доехал за
  // 4 секунды» и «не доехал вовсе, засчитан выбывшим» — и телеметрия соберёт
  // мусор ровно в том месте, ради которого её и заводят.
  //
  // ★Новых полей не понадобилось: `_loadingStartTimePoint` в RaceInstance уже
  // существует (им же считается дедлайн стадии), поэтому длительность берётся
  // разностью, а не отдельным замером, который мог бы разъехаться с дедлайном.
  //
  // ★ПЕЧАТАЕМ ПЕРЕХОД, А НЕ ФАКТ ПРИЁМА ПАКЕТА (BLOCK ревью, итерация 1).
  // Гард приёма выше СОЗНАТЕЛЬНО пускает `AcCmdCRLoadingComplete` и на стадии
  // Racing — ради честного игрока, чей пакет опоздал на тик (R11-12). Значит
  // клиент, уже перешедший в Racing, может слать этот пакет СКОЛЬКО УГОДНО РАЗ,
  // и каждый повтор давал бы новую строку «loaded in» со всё бо́льшим временем.
  // Это разом две беды: удалённо-управляемый флуд журнала (тот самый класс, что
  // дал 15 350 строк за час и лечился в R57/#195) и отравление ИМЕННО ТОЙ
  // сводки, ради которой строка и заводится, — «время загрузки» превратилось бы
  // во «время с начала загрузки до последнего повтора».
  // ★Условие `previousRacerState == Loading` даёт ровно одну строку на заезд на
  // игрока: `Loading` ставится единожды в `HandleStartRace`, и первый же
  // принятый LoadingComplete его снимает.
  // ★Ветка `Disconnected → Racing` (ушедший вернулся, пока стадия Loading) не
  // печатается намеренно: это не измерение загрузки, а воскрешение гонщика.
  if (previousRacerState == tracker::RaceTracker::Racer::State::Loading)
  {
    const auto loadingElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - raceInstance.GetLoadingStartTimePoint());
    server::util::QuietLogInfo(
      "Room {}: racer {} (oid {}) loaded in {} ms",
      raceInstance.GetRoomUid(),
      clientContext.characterUid,
      racer.oid,
      loadingElapsed.count());
  }

  // Snapshot mount stats so per-tick magic calculations don't hit the data store on every pos-update.
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [this, &racer](const data::Character& character)
    {
      GetServerInstance().GetDataDirector().GetHorse(character.mountUid()).Immutable(
        [&racer](const data::Horse& horse)
        {
          racer.mountStats = {
            .agility = horse.stats.agility(),
            .ambition = horse.stats.ambition(),
            .rush = horse.stats.rush(),
            .endurance = horse.stats.endurance(),
            .courage = horse.stats.courage(),
          };
        });

      auto& itemRegistry = GetServerInstance().GetItemRegistry();
      const auto equipmentRecords = GetServerInstance().GetDataDirector().GetItemCache().Get(
        character.characterEquipment());

      std::vector<uint32_t> equippedMountTids;
      if (equipmentRecords)
      {
        for (const auto& equipmentRecord : *equipmentRecords)
        {
          data::Tid itemTid{data::InvalidTid};
          equipmentRecord.Immutable([&itemTid](const data::Item& item)
          {
            itemTid = item.tid();
          });

          const auto itemTemplate = itemRegistry.GetItem(itemTid);
          if (not itemTemplate.has_value()
            || not itemTemplate->mountPartInfo.has_value())
          {
            continue;
          }

          equippedMountTids.emplace_back(itemTid);

          if (itemTemplate->mountAbility.has_value())
          {
            const auto& ability = itemTemplate->mountAbility.value();
            racer.mountStats.agility += ability.agility;
            racer.mountStats.ambition += ability.ambition;
            racer.mountStats.rush += ability.rush;
            racer.mountStats.endurance += ability.endurance;
            racer.mountStats.courage += ability.courage;
          }
        }
      }

      // A racer can only benefit from a single set bonus at a time, so the first
      // fully-equipped set wins.
      racer.activeSetEffect = registry::SetEquipEffect::None;
      const auto activeSets = itemRegistry.GetActiveSets(equippedMountTids);
      if (not activeSets.empty())
        racer.activeSetEffect = activeSets.front()->equipEffect;
    });

  // Notify all clients in the room that this player's loading is complete
  const protocol::AcCmdCRLoadingCompleteNotify notify{
    .oid = racer.oid};
  this->Broadcast(raceInstance, notify);

  // === R56 (#61): боты «догружаются» сразу ================================
  // Серверный гейт старта их не ждёт — в трекере их нет, и это правильно. Но
  // КЛИЕНТ ждёт отметку по каждому oid из ростера, который сам же получил, и
  // без неё соло-заезд с ботами завис бы на экране загрузки до таймаута.
  for (const auto& aiRacer : raceInstance.GetAiRacers())
  {
    const protocol::AcCmdCRLoadingCompleteNotify aiNotify{
      .oid = aiRacer.oid};
    this->Broadcast(raceInstance, aiNotify);
  }

  // Egg spawning mechanism

  // Character eligibility check
  const auto& isCharacterEligible = [this](data::Uid characterUid) -> bool
  {
    // Get character level to check min level
    uint32_t characterLevel{};
    GetServerInstance().GetDataDirector().GetCharacter(characterUid).Immutable(
      [&characterLevel](const data::Character& character)
      {
        characterLevel = character.level();
      });

    // Get configured minimum level required for egg spawning
    constexpr uint32_t MinCharLevelForEggSpawningKey = 61u;
    constexpr uint32_t DefaultMinCharLevelForEggSpawning = 12u;
    const auto& minCharacterLevelOpt = GetServerInstance().GetSystemContentRegistry().GetValue(
      MinCharLevelForEggSpawningKey);

    // Simple existence check in the system content registry, fallback to default
    const uint32_t minCharacterLevel = minCharacterLevelOpt.has_value() ?
      minCharacterLevelOpt.value() :
      DefaultMinCharLevelForEggSpawning;

    // If character level is above minimum level then character is eligible
    return characterLevel > minCharacterLevel;
  };

  // Randomness check
  const auto& shouldEggSpawn = []() -> bool
  {
    // TODO: verify if egg spawning probability is truly 50%
    return std::uniform_int_distribution<uint32_t>(0, 1)(server::util::GetRandomEngine()) != 0;
  };

  // Check gamemode eligibility
  // All teammodes including single (training, level 1 eggs only) can spawn eggs
  const bool isGameModeEligible =
    parameters.gameMode == protocol::GameMode::Speed or
    parameters.gameMode == protocol::GameMode::Magic;

  // If gamemode and character is eligible, and egg should spawn (chance)
  // then spawn egg
  const bool isEggSpawnEligible =
    isGameModeEligible and
    isCharacterEligible(clientContext.characterUid) and
    shouldEggSpawn();

  if (not isEggSpawnEligible)
    // Egg spawn not eligible, we are done here
    return;

  const protocol::AcCmdRCGameCreateClientItem spawnClientItem{
    .racerOid = racer.oid,
    .unk1 = 0};

  _commandServer.QueueCommand<decltype(spawnClientItem)>(
    clientId,
    [spawnClientItem]()
    {
      return spawnClientItem;
    });
}

void RaceNetworkHandler::HandleUserRaceFinal(
  ClientId clientId,
  const protocol::AcCmdUserRaceFinal& command)
{
  // todo: this should be verified as part of the anti cheat -
  //       we should track the race track progress and make sure it's linear
  //       and was done within reasonable timespan.

  const bool didNotFinish = command.raceTrackProgress > 0;

  // debug
  {
    const std::chrono::hh_mm_ss raceTime{
      command.courseTime};
    server::util::QuietLogDebug("[{}] AcCmdUserRaceFinal: {} {} {}",
      clientId,
      command.oid,
      didNotFinish
        ? "DNF"
        : std::format("{}:{}.{}",
            raceTime.minutes().count(),
            raceTime.seconds().count(),
            raceTime.subseconds().count()),
      command.raceTrackProgress);
  }

  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);

  // LOA-fix (R57-10, round57, backlog #195): «финишировал бот» — не финиш
  // человека. Проверки владения здесь НЕТ ВООБЩЕ: `command.oid` уходит только
  // в отладочную строку, а весь эффект применяется к отправителю. Без этого
  // гарда первый же пакет финиша за бота защёлкнул бы человеку `finishCounted`,
  // выставил ему courseTime, разослал времена ботов и засчитал сюжетные квесты.
  // Времена ботов раунд 56 выставляет сам — на честном финише живого игрока.
  if (raceInstance.IsAiRacerOid(command.oid))
    return;

  // todo: sanity check for course time
  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  // LOA-fix (batch1 task4, fix-round1; ПЕРЕДЕЛАНО R7 BLOCK-1, round7):
  // идемпотентность засчёта финиша в СЮЖЕТНЫЕ счётчики заездов
  // (11031/11035/11044/12021/14023/14025/14030).
  // БЫЛО: `alreadyFinishing = (racer.state == Finishing)` — дедуп по состоянию
  // гонщика.
  // ПОЧЕМУ СЛОМАЛОСЬ: раунд 6 откатил гард C1, и HandleLoadingComplete снова
  // ставит State::Racing БЕЗУСЛОВНО из любого состояния. Модифицированный
  // клиент чередует AcCmdCRLoadingComplete → AcCmdUserRaceFinal: каждый раз
  // состояние уходит из Finishing, alreadyFinishing == false, счётчик растёт.
  // Гейт B1 это не ловит: он меряет время ОТ СТАРТА ЗАЕЗДА, которое только
  // растёт — порог MinPlausibleCourseTime пройден один раз и навсегда.
  // СТАЛО: латч racer.finishCounted (RaceTracker.hpp) — поле состояния ЗАЕЗДА,
  // а не состояния гонщика. Ни один клиентский пакет его не сбрасывает; сброс
  // только на старте заезда (Tracker::Clear + AddRacer + явный сброс в
  // HandleStartRace).
  // ПОЧЕМУ НЕ РАННИЙ `if (state == Finishing) return;`: он ломается ровно тем же
  // чередованием (LoadingComplete вернул Racing → проверка пропускает), то есть
  // эксплойт не закрывает, зато обрывает остальную обработку пакета (серверный
  // кламп courseTime A3/B5 и рассылку AcCmdUserRaceFinalNotify) на честном
  // дубле. Латч же дедуплицирует ровно то, что должно быть одноразовым.
  // ⚠️ ВЫПЛАТ НЕ КАСАЕТСЯ: RaceInstance::Stop платит по scores/courseTime и это
  // поле не читает — выплатной античит остаётся откаченным (раунд 6).
  const bool alreadyFinishing = racer.finishCounted;
  racer.finishCounted = true;

  racer.state = tracker::RaceTracker::Racer::State::Finishing;

  // LOA-fix (A3, round3): courseTime приезжает В ПАКЕТЕ КЛИЕНТА и принимался как
  // есть (в апстриме тут так и написано «todo: sanity check for course time»).
  // По нему считаются призовые места (RaceInstance::Stop) и квестовые гейты
  // (B1 и родственные), поэтому доверять ему нельзя: подложное время давало и
  // первое место, и сюжетный прогресс.
  // ⚠️ АКТУАЛЬНОСТЬ (раунды 6-7): «право на награду за заезд» этим клампом
  // БОЛЬШЕ НЕ управляется. Выплатной античит (A1 / A3-множитель / B2 / C1)
  // откачен раундом 6 как нерабочий — 2500 морковок и 420 опыта платятся по
  // апстримному правилу, безусловно. Кламп остаётся ради рекордов, призовых
  // мест и квестовых счётчиков. Не читать его как «деньги под защитой».
  // Сервер знает фактическое время сам: _raceStartTimePoint — момент зелёного
  // света, который сервер же и рассылает в AcCmdUserRaceCountdown, и клиентский
  // таймер стартует ровно с этой метки. В честной игре расхождение — это
  // задержка сети (десятки мс). Всё, что вышло за допуск, физически невозможно
  // (нельзя ехать дольше, чем идёт заезд, и нельзя финишировать до старта) —
  // в этом случае берём серверное измерение.
  uint32_t finishCourseTime = static_cast<uint32_t>(command.courseTime.count());

  // LOA-fix (R70 итерация 2, backlog #58): СЕРВЕРНЫЙ ЗАМЕР — ОДИН НА ОБЕ ВЕТКИ
  // ПАКЕТА. Раньше он жил ВНУТРИ ветки заявленного финиша, и ветка схода
  // (`raceTrackProgress > 0`) не проверялась сервером ВООБЩЕ — ни на «заезд
  // вообще стартовал», ни на «сколько он идёт». Ревью (итерация 2) показало
  // цену: модклиент, войдя в Racing, шлёт мгновенный DNF и чеканит «Обидный
  // сход» (10036, 4 тира, 10 очков), а напарнику этим же пакетом печатает
  // `PerfectWin` (10008). Форма пакета у первой правки поменялась, а чеканка —
  // нет.
  // ЧТО ЭТО ЗА ВЕЛИЧИНЫ: `raceHasStarted` — зелёный свет уже был (метка
  // выставлена И не лежит в будущем: предстартовый отсчёт держит её ВПЕРЕДИ,
  // см. R15-1); `serverElapsedMs` — сколько миллисекунд заезд идёт ПО ЧАСАМ
  // СЕРВЕРА. Оба считаются от ОДНОГО `now()` — второго вызова нет, дрейфа
  // между проверкой и вычислением тоже (инвариант R15-1).
  const auto nowTp = std::chrono::steady_clock::now();
  const auto raceStartTimePoint = raceInstance.GetRaceStartTimePoint();
  const bool raceHasStarted =
    raceStartTimePoint != std::chrono::steady_clock::time_point::max()
    && nowTp >= raceStartTimePoint;
  const int64_t serverElapsedMs = raceHasStarted
    ? std::chrono::duration_cast<std::chrono::milliseconds>(
        nowTp - raceStartTimePoint).count()
    : 0;

  if (not didNotFinish)
  {
    // LOA-fix (B5, round4): было 3000 мс. Честное расхождение — задержка
    // сети (десятки мс; сотни на плохом канале), а трёхсекундное окно
    // позволяло молча «подрезать» своё время и красть подиум. ТЮНИНГ: если в
    // логе пойдут строки «reported course time … while the server measured …»
    // у честных игроков, порог поднять.
    constexpr int64_t CourseTimeToleranceMs = 1000;
    // LOA-fix (R15-1, quest-batch-2): ОДИН замер now() на обе ветки. Иначе
    // проверка «финиш до старта» и вычисление elapsedMs смотрят на РАЗНЫЕ
    // моменты времени, и на границе зелёного света можно получить
    // отрицательный elapsedMs уже ПОСЛЕ пройденной проверки. ★R70 итерация 2:
    // тот же замер поднят выше по функции и теперь обслуживает и ветку схода
    // (`raceHasStarted`/`serverElapsedMs`) — вычисление то же самое, место
    // одно, значение одно.

    // LOA-fix (R15-1, quest-batch-2): БЫЛО `if (raceStartTimePoint == max())`, а
    // внутри `finishCourseTime = 0`. ДВЕ ошибки в одной ветке.
    // (1) УСЛОВИЕ БЫЛО УЖЕ, ЧЕМ ДЫРА. Значение max() покрывает только окно между
    //     RaceInstance::Start() и переходом Loading -> Racing (инвариант R8-1a).
    //     Но метку старта TickLoading ставит В БУДУЩЕЕ:
    //     `_raceStartTimePoint = now + seconds(mapBlockTemplate.waitTime)`
    //     (RaceInstance.cpp ~883) — весь предстартовый отсчёт метка КОНЕЧНА и
    //     лежит впереди. Заявленный финиш, присланный в это окно, уходил в ELSE,
    //     где elapsedMs выходил ОТРИЦАТЕЛЬНЫМ, ветка `elapsedMs <= 0 ? 0u`
    //     давала serverCourseTime == 0, и кламп записывал в finishCourseTime
    //     ноль — ровно то, от чего эта правка и должна была защитить.
    // (2) НОЛЬ — ЭТО САМОЕ БЫСТРОЕ ВОЗМОЖНОЕ ВРЕМЯ: RaceInstance::Stop раздаёт
    //     призовые места по courseTime ПО ВОЗРАСТАНИЮ, то есть пакет «я уже
    //     финишировал», присланный во время отсчёта, давал первое место.
    // ТЕПЕРЬ обе двери закрыты ОДНИМ условием: финиш, заявленный ДО зелёного
    // света (метка не выставлена ИЛИ сейчас раньше метки), физически невозможен
    // и трактуется как НЕ ДОЕХАЛ — InvalidCourseTime, тот же маркер, что у
    // честного DNF (его RaceInstance::Stop уже умеет ставить в конец таблицы).
    // ЧЕСТНЫЙ ИГРОК НЕ ЗАДЕТ: его клиентский таймер стартует по
    // AcCmdUserRaceCountdown, который сервер рассылает В ТОТ ЖЕ МОМЕНТ, когда
    // ставит метку (TickLoading), — легального финиша раньше зелёного света не
    // бывает.
    // ЛАТЧ ФИНИША ТРОГАТЬ НЕ НАДО (проверено): racer.finishCounted и
    // racer.state = Finishing выставляются ВЫШЕ по функции, ДО этого блока, и
    // ровно так же — на любом честном DNF (didNotFinish == raceTrackProgress > 0,
    // тоже поле клиентского пакета). То есть «финиш до старта» ведёт себя
    // ИДЕНТИЧНО настоящему DNF, а не заводит третий вид состояния; подставить
    // его можно только САМОМУ СЕБЕ — racer берётся по
    // clientContext.characterUid, чужой заезд этим пакетом не испортить.
    // ⚠️ ПАРНАЯ ОПЕРАЦИЯ — R15-2 НИЖЕ. InvalidCourseTime == UINT32_MAX, то
    // есть он БОЛЬШЕ порога MinPlausibleCourseTime; без R15-2 эта правка
    // закрыла бы кражу подиума, но открыла бы бесплатный сюжетный прогресс.
    if (not raceHasStarted)
    {
      server::util::QuietLogWarn(
        "AcCmdUserRaceFinal: character {} reported a finish before the race has "
        "started; the finish is treated as a DNF",
        clientContext.characterUid);
      finishCourseTime = tracker::InvalidCourseTime;
    }
    else
    {
      // Сюда попадаем, только когда заезд ГАРАНТИРОВАННО стартовал
      // (nowTp >= raceStartTimePoint), то есть elapsedMs >= 0. Берём ТОТ ЖЕ
      // nowTp, что и в проверке выше: второго вызова now() нет, дрейфа между
      // условием и вычислением тоже.
      const int64_t elapsedMs = serverElapsedMs;
      const uint32_t serverCourseTime = elapsedMs <= 0
        ? 0u
        : static_cast<uint32_t>(std::min<int64_t>(
            elapsedMs, static_cast<int64_t>(tracker::InvalidCourseTime) - 1));

      // LOA-fix (R15-1b, quest-batch-2): СЕРВЕРНЫЙ ПОЛ ПРАВДОПОДОБИЯ.
      // ДЫРА, КОТОРУЮ ЭТО ЗАКРЫВАЕТ. Проверка выше ловит только финиш ДО
      // зелёного света. Финиш, заявленный ЧЕРЕЗ ПОЛСЕКУНДЫ ПОСЛЕ зелёного,
      // проходил насквозь: клиент присылал courseTime = 0, сервер намерял ~500
      // мс, расхождение 500 мс укладывалось в допуск CourseTimeToleranceMs
      // (1000 мс) — и КЛИЕНТСКИЙ НОЛЬ оставался в силе. А ноль — самое быстрое
      // возможное время: RaceInstance::Stop раздаёт призовые места по
      // courseTime ПО ВОЗРАСТАНИЮ, то есть чит забирал первое место, не проехав
      // ни метра. Гейт правдоподобности до этой правки смотрел ТОЛЬКО на
      // клиентское значение и такой ноль не видел.
      // ЧТО ТЕПЕРЬ: для НИЖНЕЙ границы авторитетен СЕРВЕРНЫЙ замер. Если по
      // часам сервера от зелёного света прошло меньше порога — финиша не было,
      // что бы ни прислал клиент и как бы он ни укладывался в допуск.
      // ⚠️ ПОРЯДОК ВАЖЕН: гейт стоит ДО проверки допуска и уводит её в else.
      // Если поставить его рядом/после, следующий шаг увидит расхождение
      // (UINT32_MAX - serverCourseTime), сочтёт его накруткой и «починит»
      // finishCourseTime обратно в маленькое серверное число.
      // ДОПУСК ±1000 мс НЕ ТРОГАЕМ — это отдельная ручка (B5, round4).
      // ПОЧЕМУ ПОРОГ = MinPlausibleCourseTime (30000 мс), А НЕ НОВАЯ КОНСТАНТА:
      //  (1) КОНСТАНТА НЕ НОВАЯ. Она уже развёрнута в проде как античит-пол:
      //      RaceInstance::Stop гейтит ею призовые места и квестовый кредит
      //      (`courseTime == InvalidCourseTime || courseTime <
      //      MinPlausibleCourseTime` -> continue, две штуки), и B1-гейт
      //      сюжетных счётчиков в этом же файле (raceCountsForQuests).
      //  (2) КОНСИСТЕНТНОСТЬ: тот же порог и тот же смысл, только применённый к
      //      СЕРВЕРНОМУ замеру, а не к числу из пакета. Новой поверхности
      //      ложных срабатываний не появляется: честный финиш, который зафолсил
      //      бы этот гейт (serverCourseTime < 30 с), УЖЕ СЕЙЧАС не получает ни
      //      призового места, ни квестового прогресса по гейтам выше.
      //  (3) ДАННЫЕ. В courses.yaml у ВСЕХ 52 гоночных карт timeLimit = 300 с;
      //      30 с стоит только у ranch_00 / ranch_w / town_test01, и ни одна из
      //      них не входит НИ В ОДИН mapPool (режимы 0/1/2, а в прод-конфиге ещё
      //      Tutorial(6), тянут mapBlockId 1..46). Трассы, которая честно
      //      закрывается быстрее 30 с, в игре нет — включая обучающие заезды,
      //      которые едут по тем же картам 1..10.
      // ЧТО НЕ ЗАДЕТО: базовая выплата (2500 морковок / 420 опыта) начисляется
      // безусловно, ВНЕ ветки `racer.courseTime != InvalidCourseTime`
      // (RaceInstance::Stop), поэтому DNF её не отнимает — пропускаются только
      // событийные множители.
      // ТЮНИНГ: если в логе пойдут ЭТИ warn'ы у честных игроков — опускать надо
      // саму MinPlausibleCourseTime, она одна на все гейты.
      if (serverCourseTime < tracker::MinPlausibleCourseTime)
      {
        server::util::QuietLogWarn(
          "AcCmdUserRaceFinal: character {} claimed a finish {} ms after the "
          "green light (client course time {} ms, plausible minimum {} ms); the "
          "finish is treated as a DNF",
          clientContext.characterUid,
          serverCourseTime,
          finishCourseTime,
          tracker::MinPlausibleCourseTime);
        finishCourseTime = tracker::InvalidCourseTime;
      }
      else
      {
        // Дальше — прежняя проверка расхождения клиент/сервер (A3 + B5). Она
        // работает только НАД полом: сюда мы попадаем, когда серверный замер уже
        // признан правдоподобным, и спор идёт лишь о точности числа.
        const int64_t deviationMs = static_cast<int64_t>(finishCourseTime)
          - static_cast<int64_t>(serverCourseTime);
        if (deviationMs > CourseTimeToleranceMs || deviationMs < -CourseTimeToleranceMs)
        {
          server::util::QuietLogWarn(
            "AcCmdUserRaceFinal: character {} reported course time {} ms while the "
            "server measured {} ms; using the server value",
            clientContext.characterUid,
            finishCourseTime,
            serverCourseTime);
          finishCourseTime = serverCourseTime;
        }
      }
    }
  }

  // LOA-fix (R9-2, round9): ВРЕМЯ ЗАЕЗДА ПИШЕТ ТОЛЬКО ПЕРВЫЙ ПАКЕТ ФИНИША.
  // ЧТО БЫЛО НЕ ТАК: присвоение стояло безусловно, на каждом AcCmdUserRaceFinal.
  // Накрутить время им нельзя (серверный кламп A3/B5 выше уже отработал), но
  // СТЕРЕТЬ — можно: гонщик честно финиширует, следом прилетает второй пакет с
  // raceTrackProgress > 0 (DNF) — и racer.courseTime становится
  // InvalidCourseTime. А по нему RaceInstance::Stop считает призовые места и
  // множитель — доехавший терял подиум одним лишним пакетом (свой же клиент мог
  // прислать его и случайно, при разрыве).
  // ТЕПЕРЬ: пишем под тем же латчем finishCounted, который выше уже
  // дедуплицирует засчёт заезда в сюжетные счётчики (R7 BLOCK-1) — одно
  // состояние заезда, одна точка истины, лишний гард не заводим.
  // ЧЕСТНЫЙ ПЕРВЫЙ ФИНИШ НЕ ЗАДЕТ: alreadyFinishing вычислен ДО установки латча,
  // на первом пакете он false — присвоение проходит ровно как раньше.
  // Рассылка AcCmdUserRaceFinalNotify остаётся безусловной: на повторном пакете
  // она просто повторит уже зафиксированное время вместо того, чтобы разослать
  // затёртое.
  if (not alreadyFinishing)
  {
    racer.courseTime = didNotFinish
      ? tracker::InvalidCourseTime
      : finishCourseTime;

    // LOA-fix (R70, backlog #58): ИСХОД ЗАЕЗДА ЗАПИСЫВАЕТСЯ ТАМ, ГДЕ ОН ИЗВЕСТЕН.
    // Ниже по стеку (RaceInstance::Stop, достижения) виден только `courseTime`,
    // а он равен InvalidCourseTime сразу в трёх случаях: честный сход, ОТВЕРГНУТЫЙ
    // АНТИЧИТОМ финиш и «гонщик не прислал ничего». Различить их там уже нечем —
    // значит различать надо здесь. Без этого «сход» (10036) чеканился бы попытками
    // мгновенного финиша: античит превращает их в InvalidCourseTime, и достижение
    // читало бы попытку обмана как обидный сход.
    // ПОРЯДОК ВЕТОК: `didNotFinish` — поле пакета, оно первично; античит правит
    // только `finishCourseTime` и только на ветке заявленного финиша.
    //
    // ★СХОД ТОЖЕ ПРОВЕРЯЕТ СЕРВЕР (R70 итерация 2). `didNotFinish` — это
    // `command.raceTrackProgress > 0`, то есть ЧИСТО КЛИЕНТСКОЕ поле, и первая
    // редакция верила ему на слово: мгновенный DNF сразу после входа в Racing
    // давал `Retired` и чеканил 10036 (4 тира, 10 очков), а через `retireCount`
    // — ещё и `PerfectWin` (10008) напарнику. Пакет невозможно подтвердить по
    // содержимому, но МОЖНО потребовать, чтобы заезд, с которого «сходят», к
    // этому моменту РЕАЛЬНО ШЁЛ: тот же пол правдоподобия
    // `MinPlausibleCourseTime`, что стоит на финише, только применённый к
    // серверному замеру. Заявленный сход, пришедший раньше, — не сход, а
    // `Rejected`: он не движет ни `Retire`, ни `retireCount`.
    // ПОЧЕМУ ТОТ ЖЕ ПОРОГ, А НЕ НОВАЯ РУЧКА: см. R15-1b — константа одна на все
    // гейты правдоподобия заезда, и «сход» не может быть правдоподобнее финиша.
    // ЦЕНА, ЗАПИСАННАЯ ЧЕСТНО: честный игрок, вышедший из заезда в первые 30
    // секунд, за ЭТОТ заезд «Обидного схода» не получит. Он его получит в
    // следующем — а чеканка четырёх тиров пакетом без езды закрыта.
    //
    // ★★ЧАСЫ — НЕ УЛИКА ЕЗДЫ (R70 итерация 3, находка ревью). Пол
    // `MinPlausibleCourseTime` доказывает, что ЗАЕЗД шёл тридцать секунд, — но
    // ровно то же самое он доказывает и про гонщика, который эти тридцать
    // секунд ПРОСТОЯЛ. Время идёт у всех одинаково, ехал ты или нет: молчун,
    // выждавший полминуты и приславший ОДИН пакет, получал либо «сход» (10036,
    // четыре тира), либо ПРИНЯТЫЙ финиш — а с ним час, мастерство карты и место
    // в ранговых условиях. Поэтому оба исхода теперь требуют ещё и улику,
    // которую сервер собрал САМ, — пройденный путь (`HasProvenTraversal`,
    // разбор в `RaceTracker.hpp`).
    // ★ЧТО ЭТА ПРАВКА НЕ ТРОГАЕТ, И ЭТО НАМЕРЕННО: `racer.courseTime`,
    // призовые места, квестовые счётчики, деньги и телеметрию лошади. Их гейтит
    // прежний античит (R15-1/R15-1b/B5) и его радиус раунд не расширяет:
    // `finishOutcome` читают ТОЛЬКО достижения. Ложное срабатывание здесь стоит
    // достижения, а не заезда.
    const bool traversalIsProven = racer.HasProvenTraversal();

    const bool retirementIsProven = raceHasStarted
      && serverElapsedMs >= static_cast<int64_t>(tracker::MinPlausibleCourseTime)
      && traversalIsProven;

    // ★ПОЧЕМУ У КОНЪЮНКТА `traversalIsProven` НА ВЕТКЕ ФИНИША НЕТ НЕГАТИВА
    // (R70 итерация 4, решение лида — записано, чтобы следующий не искал
    // пропавшую ступень лесенки). Снять его поведением НЕЛЬЗЯ НАБЛЮДАТЬ:
    // `racer.finishOutcome` читает единственное место — блок достижений в
    // `RaceInstance::Stop`, и оба его цикла начинаются с `participated(racer)`,
    // то есть с ТОГО ЖЕ предиката. Гонщик, чей исход снятие конъюнкта изменило
    // бы, до чтения исхода просто не доходит. Развести две величины во времени
    // тоже нельзя: `finishCounted` и `state = Finishing` ставятся ЭТИМ пакетом
    // ДО вычисления исхода, а обе улики копятся только под `not finishCounted`
    // и только в состоянии `Racing`, — значит в `Stop()` предикат ровно тот же.
    // Конъюнкт оставлен как защита в глубину: если исход когда-нибудь начнут
    // читать вторым местом БЕЗ гарда участия, дыра не откроется молча.
    racer.finishOutcome = didNotFinish
      ? (retirementIsProven
          ? tracker::RaceTracker::Racer::FinishOutcome::Retired
          : tracker::RaceTracker::Racer::FinishOutcome::Rejected)
      : (finishCourseTime != tracker::InvalidCourseTime && traversalIsProven
          ? tracker::RaceTracker::Racer::FinishOutcome::Finished
          : tracker::RaceTracker::Racer::FinishOutcome::Rejected);

    // Один раз на гонщика за заезд (мы под латчем `alreadyFinishing`) —
    // per-packet логирования здесь не появляется ни в одной ветке.
    if (not traversalIsProven)
    {
      server::util::QuietLogWarn(
        "AcCmdUserRaceFinal: character {} declared an outcome ({}) after the "
        "server measured only {} m of travel and {} track progress (minimum {} m "
        "and {}); the outcome does not count as participation",
        clientContext.characterUid,
        didNotFinish ? "retirement" : "finish",
        static_cast<uint64_t>(racer.distanceMetres),
        racer.trustedProgress,
        static_cast<uint64_t>(tracker::MinMeaningfulTraversalMetres),
        tracker::MinMeaningfulRaceProgress);
    }
    else if (didNotFinish && not retirementIsProven)
    {
      server::util::QuietLogWarn(
        "AcCmdUserRaceFinal: character {} declared a retirement {} ms after the "
        "green light (plausible minimum {} ms); the retirement is not counted",
        clientContext.characterUid,
        raceHasStarted ? serverElapsedMs : 0,
        tracker::MinPlausibleCourseTime);
    }
  }

  const protocol::AcCmdUserRaceFinalNotify notify{
    .oid = racer.oid,
    .courseTime = racer.courseTime};

  this->Broadcast(raceInstance, notify);

  // === R56 (#61): времена финиша ботов ====================================
  // Считаем РОВНО ОДИН РАЗ, на первом честном пакете финиша — под тем же
  // латчем `alreadyFinishing`, под которым выше пишется время игрока. Иначе
  // повторный пакет перекатывал бы уже показанные результаты.
  //
  // База — `racer.courseTime` ПОСЛЕ наших клампов A3/B5, а не поле из пакета
  // клиента: иначе бот унаследовал бы время, которое сервер только что признал
  // недостоверным, и чит игрока размножился бы на семерых.
  //
  // ★Если живой НЕ доехал, боты тоже остаются «не доехали». Апстрим этого не
  // различал; у нас DNF — это `InvalidCourseTime` (UINT32_MAX), и арифметика
  // от него дала бы не время, а мусор. Нулевое время (ветка «финиш до старта
  // заезда») исключено по той же причине.
  //
  // ★ПОПРАВКА РАУНДА 62 (#196). Времена ниже БОЛЬШЕ НЕ СИММЕТРИЧНЫ вокруг
  // времени игрока. Симметрия и была багом: бросок ±5 % давал пять «быстрее»
  // из одиннадцати исходов, то есть в среднем 7·5/11 ≈ 3,2 бота получали время
  // ЛУЧШЕ человека, который на экране пересёк черту первым, и табло
  // (`RaceInstance::Stop`, сортировка по courseTime) ставило его четвёртым.
  // Соло-заезд с ботами было НЕЛЬЗЯ ВЫИГРАТЬ — при любой езде.
  if (not alreadyFinishing
    && not raceInstance.GetAiRacers().empty()
    && racer.courseTime != tracker::InvalidCourseTime
    && racer.courseTime > 0)
  {
    // ★ЗА ЧТО ЦЕПЛЯЕМСЯ ВМЕСТО БРОСКА КУБИКА. Сервер не моделирует движение
    // ботов, но клиент за них ОТЧИТЫВАЕТСЯ позициями, и в каждом таком пакете
    // есть `progress` — доля трассы, нормированная клиентом в 0..1. R62-1
    // складывает максимум этой величины в ростер. На момент, когда человек
    // пересёк черту, отношение «прогресс бота / прогресс игрока» — это и есть
    // отношение их темпов, то есть ЕДИНСТВЕННОЕ, что связывает табло с тем,
    // что игрок видел на экране.
    //
    // Знаменатель берём ДОВЕРЕННЫЙ (`trustedProgress`, R13), а не сырой
    // `raceProgress`: он NaN-безопасен, монотонен и ограничен правдоподобным
    // темпом. Мусор в знаменателе отравил бы весь ростер разом.
    const double playerTime = static_cast<double>(racer.courseTime);
    const double playerProgress = static_cast<double>(racer.trustedProgress);
    const bool haveReference = playerProgress > 0.0;

    // Мёртвая зона фотофиниша: последний замер игрока сделан ДО черты, поэтому
    // преимущество бота в пределах пары процентов — артефакт дискретизации, а
    // не обгон. Спорную полосу отдаём человеку: он черту пересёк, бот — нет.
    constexpr double PhotoFinishMargin = 0.02;
    // Границы фантазии. Без верхней бот с прогрессом 0.05 получил бы час, без
    // нижней — «быстрее в разы». Итог: бот быстрее игрока не более чем на ~9 %
    // и медленнее не более чем вдвое.
    constexpr double FastestRatio = 1.10;
    constexpr double SlowestRatio = 0.50;
    // Шаг «данных нет»: боты выстраиваются за игроком с интервалом в секунду.
    constexpr int64_t NoDataStepMs = 1000;

    int64_t aiIndex = 0;
    for (auto& aiRacer : raceInstance.GetAiRacers())
    {
      ++aiIndex;

      int64_t aiTime;
      if (haveReference)
      {
        double ratio = static_cast<double>(aiRacer.raceProgress) / playerProgress;

        if (ratio > 1.0 - PhotoFinishMargin && ratio < 1.0 + PhotoFinishMargin)
          ratio = 1.0 - PhotoFinishMargin;

        ratio = ratio < SlowestRatio ? SlowestRatio : ratio;
        ratio = ratio > FastestRatio ? FastestRatio : ratio;

        // Знаменатель здесь заведомо >= SlowestRatio, деления на ноль нет.
        // ★ФИКС АПСТРИМ-БАГА B2 СОХРАНЁН ПО СУЩЕСТВУ: вся арифметика идёт в
        // типах со знаком (у них `uint32 += uint32 * int / 100` заворачивал
        // отрицательный бросок в ~12 часов), а результат клампится снизу.
        aiTime = static_cast<int64_t>(playerTime / ratio + 0.5);
      }
      else
      {
        // Опорной величины нет: клиент не отчитался ни за одного бота, либо
        // заезд закончился раньше, чем прогресс игрока успел вырасти. Тогда
        // единственное, что сервер знает достоверно, — что человек черту
        // ПЕРЕСЁК. Ставим ботов за ним, а не вокруг него.
        aiTime = static_cast<int64_t>(racer.courseTime) + aiIndex * NoDataStepMs;
      }

      // Разводим совпадения на миллисекунды. Клампы выше умеют схлопнуть
      // нескольких ботов в одно и то же число, а `RaceInstance::Stop`
      // сортирует НЕустойчивой сортировкой — семь одинаковых времён на табло
      // выглядели бы как ошибка, да и порядок между ними был бы случайным от
      // запуска к запуску. Смещение на индекс детерминировано и не может
      // перевести бота через границу «быстрее/медленнее игрока»: минимальный
      // зазор до времени игрока — два процента дистанции, то есть секунды.
      aiTime += aiIndex;

      const int64_t clamped = std::min<int64_t>(
        std::max<int64_t>(aiTime, 1),
        static_cast<int64_t>(tracker::InvalidCourseTime) - 1);
      aiRacer.courseTime = static_cast<uint32_t>(clamped);

      const protocol::AcCmdUserRaceFinalNotify aiNotify{
        .oid = aiRacer.oid,
        .courseTime = aiRacer.courseTime};

      this->Broadcast(raceInstance, aiNotify);
    }

    // ★ОДНА СТРОКА НА ЗАЕЗД, И ОНА НУЖНА. Раунд опирается на утверждение
    // «клиент отчитывается за ботов ПОЛЕМ progress». Позиции за ботов измерены
    // на проде (R57: 15 350 пакетов за 90 минут), а вот заполнено ли в них
    // `progress` — из логов не видно, потому что до R62 сервер их выбрасывал.
    // Строка ниже делает ответ наблюдаемым с первого же живого заезда:
    // `reference=0` или сплошные нули у ботов означают, что опоры нет и работает
    // запасная ветка ([[verify-the-oracle-before-believing-red]]).
    std::string aiProgressReport;
    for (const auto& aiRacer : raceInstance.GetAiRacers())
    {
      aiProgressReport += std::format(" {}={:.3f}/{}",
        aiRacer.name,
        aiRacer.raceProgress,
        aiRacer.courseTime);
    }

    server::util::QuietLogDebug(
      "AI finish times for character {} (course time {} ms, reference progress "
      "{:.3f}):{}",
      clientContext.characterUid,
      racer.courseTime,
      racer.trustedProgress,
      aiProgressReport);
  }

  // LOA-fix (batch1 task4, fix-round1): server-authoritative progress for MAIN
  // story quest 11035 «finish races: 10» on the Finishing transition — once per
  // race per participant (incl. DNF: counts race PARTICIPATION, not placement).
  // 11035 is a Main quest (groupType 0) turned in at an NPC, so we advance its
  // data::Quest record here mirroring HandleUseFoodItem (P4a): guard InProgress,
  // ++progress capped at successValue, ReadyToClaim on completion. PERSIST-ONLY:
  // no notify from the RACE channel — AcCmdRCUpdateQuestNotify is a ranch/journal
  // packet and the client is on the race director here; progress is reflected on
  // the next ranch-enter / quest-window open. Deadlock-safe: we hold only
  // _raceInstancesMutex (not a record lock); Character shared + Quest unique are
  // DIFFERENT records (no same-record nesting), same shape as P4a.
  // LOA-fix (B1, round4): см. комментарий у патча в apply_patches.py —
  // сюжетные счётчики заездов идут под тот же гейт правдоподобности, что и
  // награда/дейлики в RaceInstance::Stop.
  const bool raceCountsForQuests =
    not didNotFinish
    // LOA-fix (R15-2, quest-batch-2): ОБЯЗАТЕЛЬНАЯ ПАРА К R15-1. После R15-1
    // «финиш до старта» приходит сюда со временем InvalidCourseTime ==
    // UINT32_MAX, а это ЗАВЕДОМО БОЛЬШЕ MinPlausibleCourseTime (30000) — гейт
    // правдоподобности он проходит сам собой. Без этой строки правка R15-1 не
    // закрывала бы эксплойт, а меняла его форму: подиум перестал бы красться,
    // зато сюжетные счётчики заездов начали бы расти с одного пакета.
    && finishCourseTime != tracker::InvalidCourseTime
    && finishCourseTime >= tracker::MinPlausibleCourseTime;

  if (not alreadyFinishing && not raceCountsForQuests)
  {
    // DNF — штатное событие (тайм-аут карты в командном заезде), поэтому debug.
    // А вот ЗАЯВЛЕННЫЙ финиш за неправдоподобное время — улика, поэтому warn.
    if (didNotFinish)
    {
      server::util::QuietLogDebug(
        "AcCmdUserRaceFinal: character {} did not finish; story race counters "
        "not advanced",
        clientContext.characterUid);
    }
    else if (finishCourseTime == tracker::InvalidCourseTime)
    {
      // LOA-fix (R15-3, quest-batch-2): косметика к R15-1. У «финиша до старта»
      // время теперь InvalidCourseTime, и общая ветка ниже напечатала бы
      // «course time 4294967295 ms» — улика читалась бы как накрутка времени,
      // хотя причина другая. Отдельная строка = отдельный диагноз в логе.
      server::util::QuietLogWarn(
        "AcCmdUserRaceFinal: character {} reported a finish before the race "
        "started; treated as a DNF, story race counters not advanced",
        clientContext.characterUid);
    }
    else
    {
      server::util::QuietLogWarn(
        "AcCmdUserRaceFinal: character {} finished implausibly (course time {} "
        "ms, plausible minimum {} ms); story race counters not advanced",
        clientContext.characterUid,
        finishCourseTime,
        tracker::MinPlausibleCourseTime);
    }
  }

  if (not alreadyFinishing && raceCountsForQuests)
  {
    const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
      clientContext.characterUid);
    characterRecord.Immutable(
      [this, clientId, characterUid = clientContext.characterUid](
        const data::Character& character)
    {
      const auto questRecords = GetServerInstance().GetDataDirector().GetQuestCache().Get(
        character.quests());
      if (not questRecords)
        return;
      for (const auto& questRecord : *questRecords)
      {
        questRecord.Mutable([this, clientId, characterUid](data::Quest& quest)
        {
          if (quest.isCompleted() != data::Quest::Status::InProgress)
            return;
          // LOA-fix (F1, quest-batch-1): раньше здесь был захардкожен один 11035.
          // Шесть MAIN-квестов с ТЕМ ЖЕ условием «заездов в любом режиме: N»
          // (function TRUE, gameModeFlag 111, successType 1 — сверено по
          // quests.yaml и registry.jsonl) были мертвы только потому, что им не
          // отдали tid'ы: 11031 (5), 11044 (5), 12021 (10), 14023 (3),
          // 14025 (10), 14030 (8). За одним 11031 висит 30 сюжетных квестов.
          const auto qid = static_cast<uint32_t>(quest.questId());
          bool raceCountQuest = false;
          for (uint32_t raceTid : {11031u, 11035u, 11044u, 12021u, 14023u, 14025u, 14030u})
            if (raceTid == qid) { raceCountQuest = true; break; }
          if (not raceCountQuest)
            return;
          const auto tmpl = GetServerInstance().GetQuestRegistry().GetQuest(qid);
          if (not tmpl.has_value())
            return;
          // LOA-fix (F5): кап и перевод статуса разделены — добитая до цели
          // InProgress-запись становится ReadyToClaim, а не застревает.
          if (quest.progress() < tmpl->successValue)
            quest.progress() = quest.progress() + 1;
          if (quest.progress() >= tmpl->successValue)
            quest.isCompleted() = data::Quest::Status::ReadyToClaim;

          // LOA-fix (R16-1, quest-batch-2): ЖИВОЙ NOTIFY ПРОГРЕССА С РЕЙС-КАНАЛА.
          // БЫЛО «PERSIST-ONLY»: прогресс сюжетных счётчиков заездов писался в
          // запись, но клиент об этом не узнавал до следующего входа на ранчо
          // или переоткрытия журнала — игрок доезжал десять заездов и видел
          // 0/10. Причина отказа от notify была в допущении «AcCmdRCUpdateQuestNotify
          // (0x3fe) — ранчовый пакет», но оно неверно: сам апстрим документирует
          // структуру как «Can be used in either ranch or race», а на рейс-сокете
          // у нас уже штатно ходит RC-пакет того же семейства —
          // AcCmdRCUpdateDailyQuestNotify (0x35c) из SendDailyQuestNotificationToCharacter.
          // Шлём тем же способом, что и хук кормления на ранчо (RanchDirector),
          // только адресата не ищем: clientId — это и есть автор пакета финиша.
          // ⚠️ FOLLOW-UP (R16-3, ОТЛОЖЕН): маршрутизация notify для MAIN-квестов,
          // которые двигает диспатч PrizeWinner/RunMap в RaceInstance::Stop, —
          // ей нужен отдельный метод SendQuestProgressNotificationToCharacter и
          // поиск клиента по characterUid. Не делаем до живой проверки, что
          // клиент действительно РИСУЕТ 0x3fe, придя по рейс-сокету.
          protocol::AcCmdRCUpdateQuestNotify questNotify{};
          questNotify.characterUid = static_cast<uint32_t>(characterUid);
          questNotify.questTid = static_cast<uint16_t>(qid);
          questNotify.objectiveProgress.progress = quest.progress();
          questNotify.objectiveProgress.isCompleted =
            quest.progress() >= tmpl->successValue;
          _commandServer.QueueCommand<protocol::AcCmdRCUpdateQuestNotify>(
            clientId,
            [questNotify]() { return questNotify; });
        });
      }
    });
  }
}

void RaceNetworkHandler::HandleRaceResult(
  const ClientId clientId,
  [[maybe_unused]] const protocol::AcCmdCRRaceResult& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // todo:
  //  - record replays,
  //  - mount emblem unlocked
  //  - implement mount fatigue
  protocol::AcCmdCRRaceResultOK response{};
  protocol::AcCmdRCUpdateMountInfoNotify potentialNotify{
    .characterUid = clientContext.characterUid,
    .action = protocol::AcCmdRCUpdateMountInfoNotify::Action::ProgressHorsePotential};
  bool potentialProgressed = false;

  // LOA-fix (R20-2, #93): СЕРВЕРНЫЙ КЛАМП КЛАСС-ОПЫТА. gainedClassProgress
  // приходит сырым uint32 из клиентского AcCmdCRRaceResult; без капа один
  // крафт-пакет выводит лошадь на класс 30. Режем к правдоподобному потолку
  // (обоснование порога — в шапке раунда 20), честный клиент под него не
  // попадает. Кламп, не отказ: прогресс усекаем, игрока не наказываем.
  uint32_t clampedClassProgress = command.gainedClassProgress;
  if (clampedClassProgress > tracker::MaxPlausibleClassProgress)
  {
    server::util::QuietLogWarn(
      "AcCmdCRRaceResult: character {} reported gainedClassProgress {} exceeding "
      "the plausible per-race maximum {}; clamping (possible client tampering)",
      clientContext.characterUid,
      clampedClassProgress,
      tracker::MaxPlausibleClassProgress);
    clampedClassProgress = tracker::MaxPlausibleClassProgress;
  }

  characterRecord.Immutable(
    [this, &response, &potentialNotify, &potentialProgressed,
      gainedClassProgress = clampedClassProgress](const data::Character& character)
    {
      response.currentCarrots = character.carrots();

      GetServerInstance().GetDataDirector().GetHorse(character.mountUid()).Mutable(
        [this, &response, &potentialNotify, &potentialProgressed, gainedClassProgress](data::Horse& horse)
        {
          response.horseFatigue = static_cast<uint16_t>(
            horse.fatigue());

          GetServerInstance().GetHorseRegistry().ApplyClassProgress(
            horse, gainedClassProgress);

          potentialProgressed =
            GetServerInstance().GetHorseRegistry().ApplyPotentialGrowth(horse) > 0;
          if (potentialProgressed)
            protocol::BuildProtocolHorse(potentialNotify.horse, horse);
        });
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  if (potentialProgressed)
  {
    _commandServer.QueueCommand<decltype(potentialNotify)>(
      clientId,
      [potentialNotify]()
      {
        return potentialNotify;
      });
  }
}

void RaceNetworkHandler::HandleP2PRaceResult(
  ClientId,
  const protocol::AcCmdCRP2PResult&)
{
  // const auto& clientContext = GetClientContext(clientId);

  // std::scoped_lock lock(_raceInstancesMutex);
  // auto& raceInstance = GetRaceInstance(clientContext);

  // protocol::AcCmdGameRaceP2PResult result{};
  // for (const auto & [uid, racer] : raceInstance.GetTracker().GetRacers())
  // {
  //   auto& protocolRacer = result.member1.emplace_back();
  //   protocolRacer.oid = racer.oid;
  // }

  // _commandServer.QueueCommand<decltype(result)>(clientId, [result](){return result;});
}

void RaceNetworkHandler::HandleP2PUserRaceResult(
  ClientId,
  const protocol::AcCmdUserRaceP2PResult&)
{
}

void RaceNetworkHandler::HandleAwardStart(
  ClientId clientId,
  const protocol::AcCmdCRAwardStart& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);

  protocol::AcCmdRCAwardNotify notify{
    .member1 = command.member1};

  // Send to clients not participating in races.
  raceInstance.GetRoom(
    [this, &notify, &raceInstance](const Room& room)
    {
      for (const auto& [characterUid, player] : room.GetPlayers())
      {
        // Whether the client is a participating racer that did not disconnect.
        bool isParticipatingRacer = false;
        if (raceInstance.GetTracker().IsRacer(characterUid))
        {
          auto& racer = raceInstance.GetTracker().GetRacer(
            characterUid);
          // todo: handle player reconnect instead of ignoring them here
          isParticipatingRacer = racer.state != tracker::RaceTracker::Racer::State::Disconnected;
        }

        if (isParticipatingRacer)
          continue;

        _commandServer.QueueCommand<decltype(notify)>(
          player.GetClientId(),
          [notify]()
          {
            return notify;
          });
      }
    });
}

void RaceNetworkHandler::HandleAwardEnd(
  ClientId,
  const protocol::AcCmdCRAwardEnd&)
{
  // todo: this always crashes everyone

  // const auto& clientContext = GetClientContext(clientId);
  // auto& raceInstance = GetRaceInstance(clientContext);
  //
  // protocol::AcCmdCRAwardEndNotify notify{};
  //
  // // Send to clients not participating in races.
  // for (const auto raceClientId : raceInstance.clients)
  // {
  //   const auto& roomClientContext = _clients[raceClientId];
  //
  //   // Whether the client is a participating racer that did not disconnect.
  //   bool isParticipatingRacer = false;
  //   if (raceInstance.GetTracker().IsRacer(roomClientContext.characterUid))
  //   {
  //     auto& racer = raceInstance.GetTracker().GetRacer(
  //       roomClientContext.characterUid);
  //     isParticipatingRacer = racer.state != tracker::RaceTracker::Racer::State::Disconnected;
  //   }
  //
  //   if (isParticipatingRacer)
  //     continue;
  //
  //   _commandServer.QueueCommand<decltype(notify)>(
  //     raceClientId,
  //     [notify]()
  //     {
  //       return notify;
  //     });
  // }
}

void RaceNetworkHandler::HandleStarPointGet(
  ClientId clientId,
  const protocol::AcCmdCRStarPointGet& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();

  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  // LOA-fix (R57-3, round57, backlog #195): TODO апстрима выше — про этот
  // самый случай, и он наступил вместе с R56. Клиент ведёт AI-соперников сам и
  // отчитывается за них; их oid — не подлог. Выход ТИХИЙ и ДО всякого действия:
  // тело ниже считает по `racer`, то есть по ЖИВОМУ игроку.
  if (command.characterOid != racer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.characterOid))
      return;

    throw std::runtime_error(
      "Client tried to perform action on behalf of different racer");
  }

  const auto& gameModeTemplate = GetServerInstance().GetCourseRegistry().GetCourseGameModeInfo(
    static_cast<uint8_t>(parameters.gameMode));

  uint32_t gainedStarPoints = command.gainedStarPoints;
  if (racer.effects[20] || racer.effects[21]) {
    // TODO: Something sensible, idk what the bonus does
    gainedStarPoints *= 2;
  }

  racer.starPointValue = std::min(
    racer.starPointValue + gainedStarPoints,
    gameModeTemplate.starPointsMax);

  // Star point get (boost get) is only called in speed, should never give magic item
  protocol::AcCmdCRStarPointGetOK response{
    .characterOid = command.characterOid,
    .starPointValue = racer.starPointValue,
    .giveMagicItem = false
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

bool RaceNetworkHandler::HandleRequestSpur(
  ClientId clientId,
  const protocol::AcCmdCRRequestSpur& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();

  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  // LOA-fix (R57-3a, round57, backlog #195): пакет за бота законен, но обработку
  // надо прервать ЦЕЛИКОМ, вместе с хвостом лямбды-регистрации. Возврат `false`
  // именно это и означает: «пакет был не от отправителя, дальше ничего не делаем».
  if (command.characterOid != racer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.characterOid))
      return false;

    throw std::runtime_error(
      "Client tried to perform action on behalf of different racer");
  }

  const auto& gameModeTemplate = GetServerInstance().GetCourseRegistry().GetCourseGameModeInfo(
    static_cast<uint8_t>(parameters.gameMode));

  if (racer.starPointValue < gameModeTemplate.spurConsumeStarPoints)
    throw std::runtime_error("Client is dead ass cheating (or is really desynced)");

  racer.starPointValue -= gameModeTemplate.spurConsumeStarPoints;

  // === LOA-fix (R75, #14 Ф2): ЦЕПОЧКА ПЛАТНЫХ РЫВКОВ =======================
  // Стоим ПОСЛЕ списания звёздных очков и ПОСЛЕ AI-гарда R57 — значит считаем
  // только рывки, за которые списано, и которые прислал сам отправитель.
  // ★ГЕЙТ — ТОТ ЖЕ IsRaceUnderway, а не `state == Racing`: рывки в обратном
  // отсчёте цепочку не начинают, рывки в финиш-окне её не удлиняют.
  // ★ОБА ПРИЗНАКА ОБРЫВА УМЕЮТ ТОЛЬКО УМЕНЬШАТЬ ЧИСЛО (см. BoostComboWindow):
  // клиентский comboBreak и серверное окно устаревания. Ни один не даёт
  // модифицированному клиенту НАКРУТИТЬ значение — накрутку ограничивает
  // потолок MaxPlausibleBoostChain на записи в лошадь, потому что «оплата»
  // рывка объявляется самим клиентом (HandleStarPointGet).
  {
    const auto now = std::chrono::steady_clock::now();
    if (IsRaceUnderway(raceInstance, racer, now))
    {
      const bool chainStale =
        racer.lastSpurTimePoint == std::chrono::steady_clock::time_point::max()
        || (now - racer.lastSpurTimePoint) > tracker::BoostComboWindow;
      if (command.comboBreak != 0 || chainStale)
        racer.boostCombo = 1;
      else if (racer.boostCombo < std::numeric_limits<uint32_t>::max())
        racer.boostCombo += 1;
      racer.lastSpurTimePoint = now;
      racer.boostComboMax = std::max(racer.boostComboMax, racer.boostCombo);
    }
  }

  protocol::AcCmdCRRequestSpurOK response{
    .characterOid = command.characterOid,
    .activeBoosters = command.activeBoosters,
    .startPointValue = racer.starPointValue,
    .comboBreak = command.comboBreak};

  protocol::AcCmdCRStarPointGetOK starPointResponse{
    .characterOid = command.characterOid,
    .starPointValue = racer.starPointValue,
    .giveMagicItem = false
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  _commandServer.QueueCommand<decltype(starPointResponse)>(
    clientId,
    [starPointResponse]()
    {
      return starPointResponse;
    });

  // LOA-fix (batch1 task4, fix-round1): server-authoritative progress for MAIN
  // story quest 11036 «use spur in speed races: 30» (quests.yaml groupType 0, NOT
  // a daily). Per-EVENT (every valid spur; no dedup) — we only reach here after
  // the star-point cost check+deduct above, so this is a real, paid spur. Gate on
  // Speed SOLO only (11036's def flag = SpeedSoloAction; speed-TEAM spurs must NOT
  // count). 11036 is a Main quest turned in at an NPC, so we advance its
  // data::Quest record here mirroring HandleUseFoodItem (P4a). PERSIST-ONLY (same
  // race-channel reasoning + deadlock-safety as the finish hook): progress shows
  // on next ranch-enter / quest-window open.
  if (parameters.gameMode == protocol::GameMode::Speed
      && parameters.teamMode != protocol::TeamMode::Team
      // ⚠️ (N4, round7) ЭТО ГЕЙТ КВЕСТ-СЧЁТЧИКА, А НЕ ДЕНЕГ. Выплата за заезд
      // никакого racer.state не читает: выплатной античит по состоянию гонщика
      // откачен раундом 6 (см. CHANGES «Раунд 6»). Худшее, что делает ошибка
      // здесь, — квест не двигается; денег игрок не теряет.
      // Codex re-review WARN: считать ТОЛЬКО спуры во время самой гонки. Без этого
      // модифицированный клиент мог бы дослать спуры остатком старпоинтов в
      // finishing-окне (racer уже Finishing) и накрутить 11036 несуществующими рывками.
      && racer.state == tracker::RaceTracker::Racer::State::Racing)
  {
    const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
      clientContext.characterUid);
    characterRecord.Immutable(
      [this, clientId, characterUid = clientContext.characterUid](
        const data::Character& character)
    {
      const auto questRecords = GetServerInstance().GetDataDirector().GetQuestCache().Get(
        character.quests());
      if (not questRecords)
        return;
      for (const auto& questRecord : *questRecords)
      {
        questRecord.Mutable([this, clientId, characterUid](data::Quest& quest)
        {
          if (quest.isCompleted() != data::Quest::Status::InProgress)
            return;
          if (quest.questId() != 11036u)
            return;
          const auto tmpl = GetServerInstance().GetQuestRegistry().GetQuest(11036u);
          if (not tmpl.has_value())
            return;
          // LOA-fix (F5, quest-batch-1): кап и перевод статуса разделены (см.
          // хук кормления в RanchDirector) — иначе запись 30/30 со статусом
          // InProgress застревает навсегда.
          if (quest.progress() < tmpl->successValue)
            quest.progress() = quest.progress() + 1;
          if (quest.progress() >= tmpl->successValue)
            quest.isCompleted() = data::Quest::Status::ReadyToClaim;

          // LOA-fix (R16-2, quest-batch-2): тот же живой notify, что и в хуке
          // финиша (см. развёрнутое обоснование там) — 11036 «покажи рывок в
          // скоростных заездах: 30» раньше рос молча, счётчик в журнале стоял
          // на месте до следующего входа на ранчо.
          protocol::AcCmdRCUpdateQuestNotify questNotify{};
          questNotify.characterUid = static_cast<uint32_t>(characterUid);
          questNotify.questTid = 11036u;
          questNotify.objectiveProgress.progress = quest.progress();
          questNotify.objectiveProgress.isCompleted =
            quest.progress() >= tmpl->successValue;
          _commandServer.QueueCommand<protocol::AcCmdRCUpdateQuestNotify>(
            clientId,
            [questNotify]() { return questNotify; });
        });
      }
    });
  }

  // LOA-fix (R27, #10): дейлики «покажи рывок» 1006 (40) и 1017 (90) ВЫНЕСЕНЫ
  // из блока MAIN-квеста 11036 в СОБСТВЕННЫЙ гейт БЕЗ условия по teamMode.
  // ПОЧЕМУ: F8 поставил дейлик-диспатч ВНУТРЬ гейта 11036, у которого есть
  // `teamMode != Team` (11036 соло-по-дизайну, подтверждено владельцем), и
  // командные рывки не засчитывались дейликам — та же недоплата, что R26
  // починил у прыжков (1005/1016) и фаерболов (1008/1019): их диспатчи
  // teamMode никогда не гейтили, поэтому одной маски в IsModeMatch им хватило.
  // Гейт 11036 выше ОСТАЛСЯ ДОСЛОВНО ПРЕЖНИМ (Speed && teamMode != Team &&
  // Racing) и ЗАКРЫТ ДО этого блока — 11036 остаётся соло-only.
  // Здесь ровно два условия: Speed (1006/1017 — «в скоростных заездах»; magic
  // всё равно отсёк бы IsModeMatch: 35 & 4 == 0, 35 & 8 == 0) и
  // racer.state == Racing (анти-накрутка: рывки в finishing-окне не считаем —
  // тот же гард, что и был). Личный приз в команде НЕ протекает: это другой
  // канал (ToPrizeGameModeFlag, 33 & 2 == 0), его арбитрит negative-test R26.
  if (parameters.gameMode == protocol::GameMode::Speed
      && racer.state == tracker::RaceTracker::Racer::State::Racing)
  {
    const auto spurDailyNotifies = GetServerInstance().GetQuestSystem().OnQuestEvent(
      clientContext.characterUid,
      QuestSystem::QuestEvent::Any,
      QuestSystem::ToGameModeFlag(parameters.gameMode, parameters.teamMode),
      0,
      {1006u, 1017u});
    for (const auto& spurNotify : spurDailyNotifies)
    {
      SendDailyQuestNotificationToCharacter(
        spurNotify.characterUid,
        spurNotify.questId,
        spurNotify.objectiveProgress,
        spurNotify.carrotsReward,
        spurNotify.rewardType,
        spurNotify.unk2,
        spurNotify.mountExp);
    }
  }

  // Пакет был от самого отправителя — хвост лямбды (командная шкала) уместен.
  return true;
}

void RaceNetworkHandler::HandleHurdleClearResult(
  ClientId clientId,
  const protocol::AcCmdCRHurdleClearResult& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();

  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  // LOA-fix (R57-3, round57, backlog #195): TODO апстрима выше — про этот
  // самый случай, и он наступил вместе с R56. Клиент ведёт AI-соперников сам и
  // отчитывается за них; их oid — не подлог. Выход ТИХИЙ и ДО всякого действия:
  // тело ниже считает по `racer`, то есть по ЖИВОМУ игроку.
  if (command.characterOid != racer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.characterOid))
      return;

    throw std::runtime_error(
      "Client tried to perform action on behalf of different racer");
  }

  protocol::AcCmdCRHurdleClearResultOK response{
    .characterOid = command.characterOid,
    .hurdleClearType = command.hurdleClearType,
    .jumpCombo = 0,
    .unk3 = 0
  };

  // Give magic item is calculated later
  protocol::AcCmdCRStarPointGetOK starPointResponse{
    .characterOid = command.characterOid,
    .starPointValue = racer.starPointValue,
    .giveMagicItem = false
  };

  const auto& gameModeTemplate = GetServerInstance().GetCourseRegistry().GetCourseGameModeInfo(
    static_cast<uint8_t>(parameters.gameMode));

  switch (command.hurdleClearType)
  {
    case protocol::AcCmdCRHurdleClearResult::HurdleClearType::Perfect:
    {
      // Perfect jump over the hurdle.
      racer.jumpComboValue = std::min(
        static_cast<uint32_t>(99),
        racer.jumpComboValue + 1);

      if (parameters.gameMode == protocol::GameMode::Speed)
      {
        // Only send jump combo if it is a speed race
        response.jumpCombo = racer.jumpComboValue;
      }

      // Calculate max applicable combo
      const auto& applicableComboCount = std::min(
        gameModeTemplate.perfectJumpMaxBonusCombo,
        racer.jumpComboValue);
      // Calculate max combo count * perfect jump boost unit points
      const auto& gainedStarPointsFromCombo = applicableComboCount * gameModeTemplate.perfectJumpUnitStarPoints;
      // Add boost points to character boost tracker
      racer.starPointValue = std::min(
        racer.starPointValue + gameModeTemplate.perfectJumpStarPoints + gainedStarPointsFromCombo,
        gameModeTemplate.starPointsMax);

      // Update boost gauge
      starPointResponse.starPointValue = racer.starPointValue;

      // LOA-fix (NEW-1, round3): дейлики «перфект-прыжков N раз» — 1005 (100) и
      // 1016 (250). До этого их прогресс писал КЛИЕНТ пакетом 0x344, то есть
      // 500 очков награды дня выдавались за одно сообщение, без единого прыжка.
      // Событие сервер видит: перфект-прыжок приходит СЮДА ЖЕ, где начисляется
      // буст-шкала, — тот же уровень доверия, что и у всей гоночной механики.
      // gameModeFlag дейликов = 35 (SpeedSoloAction), лишние режимы отсеет
      // IsModeMatch. Гейт racer.state == Racing — как у рывка (F8): вне самой
      // гонки прыжки не считаем.
      // ⚠️ (N4, round7) ЭТО ГЕЙТ КВЕСТ-СЧЁТЧИКА, А НЕ ДЕНЕГ: выплатной античит
      // по racer.state откачен раундом 6, награда за заезд состояние не читает.
      if (racer.state == tracker::RaceTracker::Racer::State::Racing)
      {
        const auto jumpDailyNotifies = GetServerInstance().GetQuestSystem().OnQuestEvent(
          clientContext.characterUid,
          QuestSystem::QuestEvent::PerfectJump,
          QuestSystem::ToGameModeFlag(parameters.gameMode, parameters.teamMode),
          0,
          {1005u, 1016u});
        for (const auto& jumpNotify : jumpDailyNotifies)
        {
          SendDailyQuestNotificationToCharacter(
            jumpNotify.characterUid,
            jumpNotify.questId,
            jumpNotify.objectiveProgress,
            jumpNotify.carrotsReward,
            jumpNotify.rewardType,
            jumpNotify.unk2,
            jumpNotify.mountExp);
        }
      }
      break;
    }
    case protocol::AcCmdCRHurdleClearResult::HurdleClearType::Good:
    case protocol::AcCmdCRHurdleClearResult::HurdleClearType::DoubleJumpOrGlide:
    {
      // LOA-fix (R75, #14 Ф2): ОТМЕТКА ПЛАНИРОВАНИЯ. `Good` и `DoubleJumpOrGlide`
      // делят ветку, поэтому разбираем тип явно — планирование это ТОЛЬКО тип 2.
      // ★ГЕЙТ — ТОТ ЖЕ IsRaceUnderway, ЧТО У ПОЗИЦИИ И У ЦЕПОЧКИ: отметка в
      // обратном отсчёте не должна пометить первый пост-стартовый отрезок, а
      // отметка в финиш-окне — вообще ничего.
      // Гонщик в момент отметки может быть в трёх состояниях:
      //   * в воздухе        -> помечаем ТЕКУЩИЙ отрезок;
      //   * только что сел   -> засчитываем ТОЛЬКО ЧТО закончившийся;
      //   * на земле давно   -> отметка ждёт взлёта (допуск проверит его).
      //
      // ★ОДНА ОТМЕТКА ПОМЕЧАЕТ РОВНО ОДИН ОТРЕЗОК (R75 ит.2, Codex #5).
      // ЧТО БЫЛО НЕ ТАК: `glideMarkTimePoint = now` стояло БЕЗУСЛОВНО, в том
      // числе на двух ветках, где отметка уже ИЗРАСХОДОВАНА — помечен текущий
      // полёт или засчитан только что закончившийся. Отметка оставалась
      // «свежей» ещё GlideMarkTolerance (500 мс), а каденция позиции 3.83 Гц
      // (0.26 с) вполне позволяет за это время сесть и снова взлететь. Тогда
      // взлёт СЛЕДУЮЩЕГО, ОБЫЧНОГО барьера видел ту же отметку и помечался
      // планированием — путь обычного прыжка уезжал в ВЕЧНЫЙ рекорд лошади.
      // ТЕПЕРЬ отметка ЛИБО израсходована здесь (гасим её), ЛИБО остаётся
      // ждать взлёта; взлёт, воспользовавшись ею, гасит её сам. Ждать нового
      // отрезка может только НЕизрасходованная отметка.
      if (command.hurdleClearType
            == protocol::AcCmdCRHurdleClearResult::HurdleClearType::DoubleJumpOrGlide)
      {
        const auto now = std::chrono::steady_clock::now();
        if (IsRaceUnderway(raceInstance, racer, now))
        {
          if (racer.previousAirborne)
          {
            racer.currentStretchIsGlide = true;
            // Израсходована: пометила ТЕКУЩИЙ отрезок.
            racer.glideMarkTimePoint = std::chrono::steady_clock::time_point::max();
          }
          else if (racer.lastLandingTimePoint
                     != std::chrono::steady_clock::time_point::max()
                   && (now - racer.lastLandingTimePoint) <= tracker::GlideMarkTolerance)
          {
            racer.longestGlideMetres =
              std::max(racer.longestGlideMetres, racer.lastStretchMetres);
            // Один отрезок не может быть засчитан дважды.
            racer.lastStretchMetres = 0.0f;
            // Израсходована: засчитала ПРЕДЫДУЩИЙ отрезок.
            racer.glideMarkTimePoint = std::chrono::steady_clock::time_point::max();
          }
          else
          {
            // Не израсходована — ждёт взлёта, который случится в пределах
            // допуска. Это единственная ветка, оставляющая отметку живой.
            racer.glideMarkTimePoint = now;
          }
        }
      }

      // Not a perfect jump over the hurdle, reset the jump combo.
      racer.jumpComboValue = 0;
      response.jumpCombo = racer.jumpComboValue;

      uint32_t gainedStarPoints = gameModeTemplate.goodJumpStarPoints;
      if (racer.effects[20] || racer.effects[21]) {
        // TODO: Something sensible, idk what the bonus does
        gainedStarPoints *= 2;
      }

      // Increment boost gauge by a good jump
      racer.starPointValue = std::min(
        racer.starPointValue + gainedStarPoints,
        gameModeTemplate.starPointsMax);

      // Update boost gauge
      starPointResponse.starPointValue = racer.starPointValue;
      break;
    }
    case protocol::AcCmdCRHurdleClearResult::HurdleClearType::Collision:
    {
      // A collision with hurdle, reset the jump combo.
      racer.jumpComboValue = 0;
      response.jumpCombo = racer.jumpComboValue;
      break;
    }
    default:
    {
      server::util::QuietLogWarn("Unhandled hurdle clear type {}",
        static_cast<uint8_t>(command.hurdleClearType));
      return;
    }
  }

  // Needs to be assigned after hurdle clear result calculations
  // Triggers magic item request when set to true (if gamemode is magic and magic gauge is max)
  starPointResponse.giveMagicItem =
    parameters.gameMode == protocol::GameMode::Magic &&
    racer.starPointValue >= gameModeTemplate.starPointsMax &&
    not racer.magicItem.has_value() &&
    command.hurdleClearType == protocol::AcCmdCRHurdleClearResult::HurdleClearType::Perfect;

  // Update the star point value if the jump was not a collision.
  if (command.hurdleClearType != protocol::AcCmdCRHurdleClearResult::HurdleClearType::Collision)
  {
    _commandServer.QueueCommand<decltype(starPointResponse)>(
      clientId,
      [clientId, starPointResponse]()
      {
        return starPointResponse;
      });
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [clientId, response]()
    {
      return response;
    });
}

void RaceNetworkHandler::HandleStartingRate(
  ClientId clientId,
  const protocol::AcCmdCRStartingRate& command)
{
  // TODO: check for sensible values
  if (command.unk1 < 1 && command.boostGained < 1)
  {
    // Velocity and boost gained is not valid
    // TODO: throw?
    return;
  }

  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();

  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  // LOA-fix (R57-3, round57, backlog #195): TODO апстрима выше — про этот
  // самый случай, и он наступил вместе с R56. Клиент ведёт AI-соперников сам и
  // отчитывается за них; их oid — не подлог. Выход ТИХИЙ и ДО всякого действия:
  // тело ниже считает по `racer`, то есть по ЖИВОМУ игроку.
  if (command.characterOid != racer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.characterOid))
      return;

    throw std::runtime_error(
      "Client tried to perform action on behalf of different racer");
  }

  const auto& gameModeTemplate = GetServerInstance().GetCourseRegistry().GetCourseGameModeInfo(
    static_cast<uint8_t>(parameters.gameMode));

  // TODO: validate boost gained against a table and determine good/perfect start
  racer.starPointValue = std::min(
    racer.starPointValue + command.boostGained,
    gameModeTemplate.starPointsMax);

  // Only send this on good/perfect starts
  protocol::AcCmdCRStarPointGetOK response{
    .characterOid = command.characterOid,
    .starPointValue = racer.starPointValue,
    .giveMagicItem = false // TODO: this would never give a magic item on race start, right?
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [clientId, response]()
    {
      return response;
    });
}

void RaceNetworkHandler::HandleRaceUserPos(
  const ClientId clientId,
  const protocol::AcCmdUserRaceUpdatePos& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  // LOA-fix (R62-1, round62, backlog #196): ЕДИНСТВЕННЫЙ наблюдаемый сигнал о
  // том, где на трассе бот. Ботов ведёт клиент, и он же присылает их позиции —
  // это и есть тот трафик, который R57 научился игнорировать молча. Игнорировать
  // его ЦЕЛИКОМ оказалось слишком: без него сервер выдумывал времена финиша
  // ботов симметрично вокруг времени игрока, и человек, пересёкший черту первым,
  // получал в среднем четвёртое место (#196).
  //
  // ★ВЫЗОВ СТОИТ ДО СРАВНЕНИЯ ВЛАДЕНИЯ И САМ СЕБЯ ГАРДИТ. Для oid живого
  // гонщика это пустая операция, поэтому «свой» путь не меняется ни на байт, а
  // дословная форма гарда R57 ниже остаётся нетронутой — её проверяет оракул
  // #195 по числу совпадений.
  raceInstance.NoteAiRacerProgress(command.oid, command.progress);

  // LOA-fix (R57-4, round57, backlog #195). Именно это место дало 15 350 из
  // 15 589 строк [error] за час живой игры: клиент шлёт позицию за каждого из
  // семи ботов на каждом тике. Пропустить пакет ДАЛЬШЕ было бы хуже флуда —
  // ниже копится телеметрия лошади (R24) и доверенный прогресс мести (R13).
  if (command.oid != racer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.oid))
      return;

    throw std::runtime_error(
      "Client tried to perform action on behalf of different racer");
  }

  // LOA-fix (R24, #14 фаза 1): копим topSpeed / totalDistance. Скорость уже в
  // пакете (member4, км/ч). Мир: 1 юнит ≈ 1 метр. finishCounted — латч «уже
  // отфинишировал». Читаем прошлую позицию ДО перезаписи worldPosition ниже.
  if (racer.state == tracker::RaceTracker::Racer::State::Racing
    && not racer.finishCounted)
  {
    const auto now = std::chrono::steady_clock::now();

    // ★Гейт зелёного света: racer переходит в Racing уже на ЗАГРУЗКЕ, а старт
    // (GetRaceStartTimePoint) ставится на 1-10с в БУДУЩЕЕ (обратный отсчёт). Без
    // этого гейта клиент писал бы пре-гоночную скорость и до ~833м фейк-движения
    // на отсчёте. До старта GetRaceStartTimePoint в будущем/max() → не копим и НЕ
    // сеем: worldPosition ниже всё равно обновится, поэтому первый пост-старт пакет
    // просто сеет заново (первая дельта не считается).
    if (now >= raceInstance.GetRaceStartTimePoint())
    {
      // Клиентская скорость недоверенная → кламп по потолку.
      if (command.member4 > racer.topSpeedKph
        && command.member4 <= tracker::MaxPlausibleSpeedKph)
      {
        racer.topSpeedKph = command.member4;
      }

      // LOA-fix (R75): принятый шаг нужен и планированию — объявляем ДО ветки,
      // чтобы отброшенный телепорт не мог стать «полётом» (тот же кламп, что у
      // R24: одна величина, один фильтр, два потребителя).
      float acceptedStep = 0.0f;
      if (racer.hasPositionSample)
      {
        // Бюджет перемещения за прошедшее время: телепорт/респавн/лаг-скачок
        // ОТБРАСЫВАЕМ, а не копим. dt режем 2 с — пауза сервера не разрешение.
        const auto elapsedMs = std::chrono::duration_cast<
          std::chrono::milliseconds>(now - racer.lastPositionTimePoint).count();
        const float dtSeconds = elapsedMs <= 0
          ? 0.0f
          : std::min(2.0f, static_cast<float>(elapsedMs) / 1000.0f);
        const float budget = std::min(
          tracker::MaxPlausiblePositionDeltaMetres,
          (tracker::MaxPlausibleSpeedKph / 3.6f) * dtSeconds);

        const float step = (command.position - racer.worldPosition).Length();
        if (step <= budget)
        {
          racer.distanceMetres += static_cast<double>(step);
          acceptedStep = step;
        }
        else
        {
          // LOA-fix (R76, #30 этап 1): отброшенный шаг перестаёт исчезать
          // молча. Ровно та же ветка, ровно то же условие — меняется только
          // то, что мы про неё ПОМНИМ. Ни одного лога здесь: путь
          // удалённо-управляемый, R57/#195 дал 15 350 строк [error] за час
          // именно на этом хендлере.
          racer.positionJumps += 1;
          racer.discardedMetres += static_cast<double>(step);
          if (step > racer.maxDiscardedStepMetres)
            racer.maxDiscardedStepMetres = step;
        }
      }

      // LOA-fix (R76, #30 этап 1): считаем ПОСТ-СТАРТОВЫЕ пакеты позиции.
      // Место выбрано внутри гейта зелёного света и рядом с hasPositionSample:
      // это ровно те пакеты, по которым сервер что-то накапливает. Тот же
      // тройной гейт, что у splits ниже (state == Racing && !finishCounted &&
      // now >= GetRaceStartTimePoint) — окна samples и splits СОВПАДАЮТ.
      racer.posSampleCount += 1;
      racer.hasPositionSample = true;
      racer.lastPositionTimePoint = now;

      // === LOA-fix (R75, #14 Ф2): ПЛАНИРОВАНИЕ ==============================
      // Считаем ТОЛЬКО отрезки полёта, помеченные 0xe7 DoubleJumpOrGlide.
      // `member5 == 1` = «в воздухе» и для ОБЫЧНОГО прыжка через барьер тоже,
      // поэтому «любой airtime» превратил бы рекорд планирования в счётчик
      // барьеров — и откатить это было бы нечем: поле вечное.
      //
      // ★В РЕКОРД ИДЁТ ТОЛЬКО ОТРЕЗОК, ЗАКРЫВШИЙСЯ ПРИЗЕМЛЕНИЕМ.
      // Клиент, держащий member5 = 1 до конца заезда, не приземляется никогда,
      // и весь остаток пути свернулся бы в ВЕЧНЫЙ рекорд. Теперь величина
      // ограничена сверху самим фактом приземления (плюс потолком в Stop()).
      // Цена — гонщик, пересёкший черту в воздухе, свой последний полёт не
      // досчитает. НЕДОСЧЁТ ДЛЯ ВЕЧНОГО ПОЛЯ ДЕШЕВЛЕ, ЧЕМ НЕОГРАНИЧЕННЫЙ ПЕРЕБОР.
      const bool airborne = command.member5 == 1;
      if (airborne)
      {
        if (not racer.previousAirborne)
        {
          // ВЗЛЁТ: начинается новый отрезок. Он планирующий, если отметка пришла
          // только что (случай «0xe7 на пакет раньше первого воздушного кадра»).
          racer.currentAirborneMetres = 0.0f;
          racer.currentStretchIsGlide =
            racer.glideMarkTimePoint != std::chrono::steady_clock::time_point::max()
            && (now - racer.glideMarkTimePoint) <= tracker::GlideMarkTolerance;
          // ★ОТМЕТКА ОДНОРАЗОВАЯ (R75 ит.2, Codex #5). Гасим её ВСЕГДА, а не
          // только когда она сработала: просроченная отметка тоже не должна
          // дожить до следующего взлёта, а сработавшая — тем более. Иначе
          // обычный барьер, взлетевший вторым внутри допуска, унаследовал бы
          // чужую пометку и его путь ушёл бы в ВЕЧНЫЙ рекорд лошади.
          racer.glideMarkTimePoint = std::chrono::steady_clock::time_point::max();
        }
        else
        {
          racer.currentAirborneMetres += acceptedStep;
        }
      }
      else if (racer.previousAirborne)
      {
        // ПРИЗЕМЛЕНИЕ: отрезок закончился. Помеченный — сворачиваем в рекорд
        // заезда; непомеченный — запоминаем на случай, если 0xe7 придёт
        // следующим пакетом (случай «отметка уже на земле»).
        if (racer.currentStretchIsGlide)
          racer.longestGlideMetres =
            std::max(racer.longestGlideMetres, racer.currentAirborneMetres);
        racer.lastStretchMetres = racer.currentAirborneMetres;
        racer.lastLandingTimePoint = now;
        racer.currentAirborneMetres = 0.0f;
        racer.currentStretchIsGlide = false;
      }
      racer.previousAirborne = airborne;
    }
  }

  // === LOA-fix (R-revenge, #13): ДОВЕРЕННЫЙ ПРОГРЕСС + ДЕТЕКТОР МЕСТИ ========
  // Полностью серверное определение бонуса «Неплохая месть». Клиент НЕ участвует:
  // опкод 0x206 (AcCmdCRRevengeAssign) не реализован и реализован не будет.
  // Анти-форж стоит на том, что строку прогресса гонщика X пишет ТОЛЬКО
  // соединение самого X — гард `command.oid != racer.oid` в начале этой функции.
  // Соло-читер не может сфабриковать «обидчика»: обидчик обязан прислать свои
  // пакеты сам. raceProgress НЕ трогаем (по нему ранжирует MagicSystem).
  // Локи: всё под уже взятым _raceInstancesMutex, новых локов нет.
  // Стоимость: O(гонщиков) на пакет позиции (<= 8), карта revengeRows крошечная.
  {
    const auto revengeNow = std::chrono::steady_clock::now();

    // Гейт зелёного света — тот же, что у телеметрии R24: до старта
    // GetRaceStartTimePoint лежит в будущем (обратный отсчёт), «прогресс» на
    // отсчёте — не прогресс.
    if (revengeNow >= raceInstance.GetRaceStartTimePoint()
      && racer.state == tracker::RaceTracker::Racer::State::Racing
      && not racer.finishCounted)
    {
      // --- G2: свой прогресс через бюджет правдоподобия + храповик ----------
      // NaN-безопасная санитизация: `NaN > 0.0f` ложно, значение уходит в 0.
      float clientProgress = 0.0f;
      if (command.progress > 0.0f)
        clientProgress = command.progress > 1.0f ? 1.0f : command.progress;

      if (racer.trustedProgressTimePoint
          == std::chrono::steady_clock::time_point::max())
      {
        // Первый пост-стартовый пакет ТОЛЬКО сеет отсчёт времени: доверенный
        // прогресс стартует с нуля, что бы клиент ни объявил. Модклиент,
        // объявивший progress = 1.0 на старте, выглядит ровно как самый быстрый
        // ЧЕСТНЫЙ гонщик — не быстрее.
        racer.trustedProgressTimePoint = revengeNow;
      }
      else
      {
        const auto elapsedMs = std::chrono::duration_cast<
          std::chrono::milliseconds>(
            revengeNow - racer.trustedProgressTimePoint).count();
        const float budget = elapsedMs <= 0
          ? 0.0f
          : static_cast<float>(elapsedMs)
              / static_cast<float>(tracker::MinPlausibleCourseTime);
        const float capped = std::min(clientProgress,
          racer.trustedProgress + budget);
        // LOA-fix (R76, #30 этап 1): помним, что бюджет темпа сработал. Считаем
        // ДО присвоения — после него разница уже неотличима.
        if (clientProgress > racer.trustedProgress + budget)
          racer.progressClipped += 1;
        // ★ХРАПОВИК: только вверх. Понижение своего прогресса — это попытка
        // сфабриковать «меня обогнали», а не движение по трассе.
        if (capped > racer.trustedProgress)
          racer.trustedProgress = std::min(capped, 1.0f);
        racer.trustedProgressTimePoint = revengeNow;
      }

      // LOA-fix (R76, backlog #30 этап 1): ОТМЕТКИ ВРЕМЕНИ ПО ТРАССЕ.
      // Порог k взят, когда ДОВЕРЕННЫЙ прогресс впервые достиг 0.1*(k+1).
      // Цикл, а не одно сравнение: один пакет после долгой паузы законно
      // перекрывает несколько порогов сразу (бюджет темпа пропорционален
      // elapsedMs), и такой заезд обязан отличаться от честного НЕ числом
      // сплитов, а числом пакетов — их считает posSampleCount.
      // ★МЕСТО: ПОСЛЕ всего блока if/else, а не внутри ветки храповика —
      // чтобы будущая перестановка веток не оставила сплиты слепыми. На самом
      // ПЕРВОМ пакете (ветка «только сеет») trustedProgress ещё 0.0f и цикл
      // законно не выполняется ни разу: 0.0f >= 0.1f ложно.
      // Стоимость: O(1) амортизированно, тело выполняется суммарно 10 раз за заезд.
      // Времени берём УЖЕ ИЗМЕРЕННОЕ revengeNow — второй вызов now() дал бы две
      // разные шкалы в одной функции.
      while (racer.splitsReached < tracker::RaceTracker::Racer::ProgressSplitCount
        && racer.trustedProgress
             >= 0.1f * static_cast<float>(racer.splitsReached + 1))
      {
        const auto sinceGreenLight = std::chrono::duration_cast<
          std::chrono::milliseconds>(
            revengeNow - raceInstance.GetRaceStartTimePoint()).count();
        racer.progressSplits[racer.splitsReached] = sinceGreenLight <= 0
          ? 0u
          : static_cast<uint32_t>(std::min<int64_t>(sinceGreenLight, 0xFFFFFFFEll));
        racer.splitsReached += 1;
      }

      // --- G7: только командный заезд и только ЧУЖАЯ команда ----------------
      if (racer.team != tracker::RaceTracker::Racer::Team::Solo
        && racer.revengeCredits < tracker::RevengeMaxCredits)
      {
        for (const auto& [rivalUid, rival] : raceInstance.GetTracker().GetRacers())
        {
          if (rivalUid == clientContext.characterUid)
            continue;
          if (rival.team == tracker::RaceTracker::Racer::Team::Solo
            || rival.team == racer.team)
            continue;
          // --- G8: соперник обязан быть ЖИВЫМ ---------------------------
          // Замороженный прогресс отвалившегося/залагавшего игрока обогнать
          // «легко» — такой обгон местью не считается.
          if (rival.state != tracker::RaceTracker::Racer::State::Racing
            || rival.finishCounted)
            continue;
          if (rival.trustedProgressTimePoint
              == std::chrono::steady_clock::time_point::max()
            || (revengeNow - rival.trustedProgressTimePoint)
                 > tracker::RevengeRivalFreshness)
            continue;

          auto& revengeRow = racer.revengeRows[rivalUid];
          // --- G5: один зачёт на соперника (терминальное состояние) -------
          if (revengeRow.state
              == tracker::RaceTracker::Racer::RevengeState::Revenged)
            continue;

          // --- G3: гистерезис -------------------------------------------
          const float margin = racer.trustedProgress - rival.trustedProgress;
          const bool relationHolds =
            revengeRow.state == tracker::RaceTracker::Racer::RevengeState::Idle
              ? margin <= -tracker::RevengeOvertakeHysteresis   // он впереди
              : margin >= tracker::RevengeOvertakeHysteresis;   // мы впереди

          if (not relationHolds)
          {
            revengeRow.since = std::chrono::steady_clock::time_point::max();
            continue;
          }

          // --- G4: выдержка ---------------------------------------------
          if (revengeRow.since == std::chrono::steady_clock::time_point::max())
          {
            revengeRow.since = revengeNow;
            continue;
          }
          if ((revengeNow - revengeRow.since) < tracker::RevengeDwellDuration)
            continue;

          revengeRow.since = std::chrono::steady_clock::time_point::max();
          if (revengeRow.state
              == tracker::RaceTracker::Racer::RevengeState::Idle)
          {
            // Обидчик зафиксирован — теперь ждём ответного устойчивого обгона.
            revengeRow.state = tracker::RaceTracker::Racer::RevengeState::Passed;
          }
          else
          {
            revengeRow.state =
              tracker::RaceTracker::Racer::RevengeState::Revenged;
            racer.revengeCredits += 1;
            server::util::QuietLogDebug(
              "Revenge: character {} revenged rival {} ({}/{})",
              clientContext.characterUid,
              rivalUid,
              racer.revengeCredits,
              tracker::RevengeMaxCredits);
            if (racer.revengeCredits >= tracker::RevengeMaxCredits)
              break;
          }
        }
      }
    }
  }

  racer.worldPosition = command.position;
  racer.raceProgress = command.progress;
  // LOA-fix (R76, #30 этап 1): максимум СЫРОГО объявления за заезд. Место —
  // ЕДИНСТВЕННОЕ на функцию, рядом с единственной записью raceProgress: это
  // тотальный инвариант «что клиент объявил», а не список мест
  // ([[total-invariant-beats-list-of-sites]]). NaN не проходит: `>` с NaN ложно.
  // ★ОКНО ШИРЕ, ЧЕМ У ОСТАЛЬНЫХ ПОЛЕЙ: здесь нет ни гейта зелёного света, ни
  // `state == Racing`, ни `!finishCounted` — намеренно, чтобы предстартовое или
  // постфинишное «999» тоже попало в журнал. Раунд 2 обязан это учитывать.
  if (command.progress > racer.maxDeclaredProgress)
    racer.maxDeclaredProgress = command.progress;
}

void RaceNetworkHandler::HandleChat(
  const ClientId clientId,
  const protocol::AcCmdCRChat& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Perform moderation before proceeding with chat processing
  const auto chatVerdict = _serverInstance.GetChatSystem().ProcessChatMessage(
    clientContext.characterUid, command.message);

  // LOA-fix (R55-4, round55, backlog #179 часть 5): см. R55-3.
  if (not chatVerdict)
    return;

  const auto& verdict = *chatVerdict;

  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  std::string characterName;
  characterRecord.Immutable([&characterName](const data::Character& character)
  {
    characterName = character.name();
  });

  const auto& userName = clientContext.userName;

  std::vector<protocol::AcCmdCRChatNotify> response;
  const bool isCommand = verdict.commandVerdict.has_value();

  if (isCommand)
  {
    for (const auto& line : verdict.commandVerdict->result)
    {
      response.emplace_back(protocol::AcCmdCRChatNotify{
        .message = line,
        .author = "",
        .isSystem = true});
    }
  }
  else
  {
    if (verdict.isMuted)
    {
      if (verdict.isPrevented)
      {
        server::util::QuietLogInfo("[Room {}] (prevented) {} ({}): {}",
          clientContext.roomUid,
          characterName,
          userName,
          command.message);
      }
      else
      {
        server::util::QuietLogInfo("[Room {}] (muted) {} ({}): {}",
          clientContext.roomUid,
          characterName,
          userName,
          command.message);
      }
      protocol::AcCmdCRChatNotify notify{
        .message  = verdict.message,
        .author   = verdict.isPrevented ? "AutoMod" : "System",
        .isSystem = true};
      _commandServer.QueueCommand<decltype(notify)>(clientId, [notify](){ return notify; });
      return;
    }

    server::util::QuietLogInfo("[Room {}] {} ({}): {}",
      clientContext.roomUid,
      characterName,
      userName,
      command.message);

    response.emplace_back(protocol::AcCmdCRChatNotify{
      .message = verdict.message,
      .author = characterName,
      .isSystem = false,});
  }

  if (isCommand)
  {
    for (const auto& notify : response)
    {
      _commandServer.QueueCommand<protocol::AcCmdCRChatNotify>(
        clientId,
        [notify]{ return notify; });
    }
  }
  else
  {
    std::scoped_lock lock(_raceInstancesMutex);
    // Don't check racer since chat can be sent either
    // in the waiting room or during a race.
    const auto& raceInstance = GetRaceInstance(clientContext, false);
    for (const auto& notify : response)
    {
      this->Broadcast(raceInstance, notify);
    }
  }
}

void RaceNetworkHandler::HandleRelayCommand(
  ClientId clientId,
  const protocol::AcCmdCRRelayCommand& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Create relay notify message
  protocol::AcCmdCRRelayCommandNotify notify{
    .member1 = command.member1,
    .member2 = command.member2};

  std::scoped_lock lock(_raceInstancesMutex);
  // Get the room instance for this client
  const auto& raceInstance = GetRaceInstance(clientContext);

  // Relay the command to all other clients in the room
  this->BroadcastExceptCharacterUid(
    raceInstance,
    notify,
    clientContext.characterUid);
}

void RaceNetworkHandler::HandleRelay(
  ClientId clientId,
  const protocol::AcCmdCRRelay& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Create relay notify message
  protocol::AcCmdCRRelayNotify notify{
    .fromOid = command.fromOid,
    .toOid = command.toOid,
    .payloadType = command.payloadType,
    .data = std::move(command.data),};

  switch (command.payloadType)
  {
    case protocol::relay::RelayCommandId::Snapshot:
    {
      // Do anything related to `command.snapshot`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::SyncProgress:
    {
      // Do anything related to `command.syncProgress`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::SetTargetStateEnabled:
    case protocol::relay::RelayCommandId::SetTargetStateDisabled:
    {
      // Do anything related to `command.setTargetState`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::NetSetState:
    {
      // Do anything related to `command.netSetState`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::NetSetLayerAnimation:
    {
      // Do anything related to `command.netSetLayerAnimation`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::SyncGoalIn:
    {
      // Do anything related to `command.syncGoalIn`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::SpurLevel:
    {
      // Do anything related to `command.spurLevel`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::SlidingMotion:
    {
      // Do anything related to `command.slidingMotion`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::BroadcastCharacterUid:
    {
      // Do anything related to `command.broadcastCharacterUid`, if needed
      break;
    }
    case protocol::relay::RelayCommandId::ResetPosOther:
    {
      // Do anything related to `command.resetPosOther`, if needed
      break;
    }
    default:
    {
      const std::string header = command.toOid == 0 ?
        std::format("{:#x}->Broadcast", command.fromOid) :
        std::format("{:#x}->{:#x}",
          command.fromOid,
          command.toOid);

      // LOA-fix (R71-8): ЖАЛОБА, КОТОРУЮ ЗАКАЗЫВАЕТ КЛИЕНТ, ОБЯЗАНА БЫТЬ ЗАДРОССЕЛЕНА.
      // Строка писалась на КАЖДЫЙ пакет и несёт hex-дамп: клиент, шлющий неизвестный
      // `payloadType` четыре раза в секунду, получал готовый лог-флуд того же класса,
      // что R57 нашёл на проде. Аргументы и формат-строка НЕ меняются — иначе поехал
      // бы маркер лесенки прошлого раунда.
      // ★Эта ветка стоит ДО захвата `_raceInstancesMutex` — именно поэтому у
      // `LogThrottle` собственный замок-лист (LogThrottle.hpp).
      uint64_t suppressed = 0;
      if (_relayPayloadTypeThrottle.Allow(suppressed))
        server::util::QuietLogWarn("Relay payload from client '{}', with oids {}, sent an unrecognised relay payload type '{:#04x}': {:02X} (suppressed {})",
          clientId,
          header,
          static_cast<uint16_t>(command.payloadType),
          spdlog::to_hex(command.data),
          suppressed);

      // LOA-fix (R71-14, находка ревью 2 #4): НЕРАЗОБРАННАЯ НАГРУЗКА НЕ РЕТРАНСЛИРУЕТСЯ.
      //
      // До этой строки ветка только ЖАЛОВАЛАСЬ и падала дальше — на широковещание.
      // Правило раунда «кто действует, тот и отправитель» проверяется по действующему
      // лицу ВНУТРИ нагрузки (`GetRelayClaim`), а у неразобранного типа его нет: класс
      // `Unparsed` не может быть авторизован в принципе. Значит канал оставался
      // фейл-оупен: `fromOid = свой` + неизвестный `payloadType` + до 65 535 байт
      // (размер читается `uint16`, RaceMessageDefinitions.cpp:1201-1209) = усиление
      // одного пакета в семь по числу соседей, с содержимым, которое сервер ни разу
      // не посмотрел.
      //
      // ★ЧТО МЫ ЛОМАЕМ, ЕСЛИ ОШИБЛИСЬ, И ПОЧЕМУ ДУМАЕМ, ЧТО НЕ ЛОМАЕМ. Отбрасывание
      // видно клиенту как «фича по неизвестному типу перестала работать». Улика против
      // этого — ЖИВОЙ ЗАХВАТ настоящего клиента: 28 000 датаграмм за один 188-секундный
      // заезд несут ровно опкоды 0x03, 0x07, 0x0d, 0x14, 0x16 (плюс транспортные 0x07d1
      // и 0x07d5 СВОЕГО слоя, не payloadType) — все классифицированы. Исторические
      // «неизвестные» типы из логов апстрима (0x0c, 0x12) с тех пор в перечислении.
      // Проверяемо на проде и откатывается одной строкой: жалоба выше печатает
      // `payloadType`, и если в логе пойдут одинаковые типы от РАЗНЫХ честных клиентов
      // — это новый тип клиента, его место в `RelayCommandId` и в классификаторе.
      return;
    }
  }

  std::scoped_lock lock(_raceInstancesMutex);
  // Get the room instance for this client
  // ★НЕконстантная ссылка (LOA-fix, R71-6a): гарду нужен `GetTracker().GetRacer(...)`,
  // а у него нет const-перегрузки (RaceTracker.hpp:364). `GetRaceInstance` и так
  // возвращает `RaceInstance&` — снятие const ничего не расширяет.
  auto& raceInstance = GetRaceInstance(clientContext);

  // LOA-fix (R71-6a, backlog #31 пререквизит): РЕТРАНСЛЯЦИЯ НЕСЁТ ТОЛЬКО СВОЙ oid.
  //
  // Конверт `AcCmdCRRelayNotify` уходит всем в комнате с `fromOid` ИЗ ПАКЕТА (:4363,
  // :4446; запись — RaceMessageDefinitions.cpp:1325-1338).
  //
  // ★ЭТОГО ОДНОГО ГАРДА МАЛО, И ЭТО ГЛАВНАЯ ПОПРАВКА РЕДАКЦИИ 2 СПЕКИ. Настоящее
  // авторство лежит ВНУТРИ нагрузки (см. R71-6b) — конверт закрывается здесь только
  // потому, что он тоже уходит наружу, а не потому, что он что-то решает.
  //
  // ★`GetRacer` здесь БЕЗОПАСЕН: `GetRaceInstance` вызван с `checkRacer = true`
  // (умолчание, RaceNetworkHandler.hpp:199-201) и уже бросил бы, не будь отправитель
  // гонщиком (:779-783).
  //
  // ★oid БОТА — ЗАКОННЫЙ ВХОД, НО РЕТРАНСЛИРОВАТЬ ЕГО НЕЧЕГО. Ботов ведёт клиент, и он
  // шлёт за них relay. Боты существуют ТОЛЬКО в соло-заезде (`isSoloRace` = ровно один
  // гонщик в трекере, :2362-2367), а `BroadcastExceptCharacterUid` в соло не доходит ни
  // до кого — то есть отбрасывание таких кадров не меняет ни одного экрана. Выход
  // ТИХИЙ: жалоба на законное поведение и есть тот самый флуд, который лечил R57.
  const auto& senderRacer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);
  if (command.fromOid != senderRacer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.fromOid))
      return;

    uint64_t suppressed = 0;
    if (_relayEnvelopeThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Relay from racer {} claimed foreign oid {} in the envelope (suppressed {})",
        senderRacer.oid,
        command.fromOid,
        suppressed);
    return;
  }

  // LOA-fix (R71-6b, находка ревью R1, backlog #31): АВТОРСТВО ЖИВЁТ В НАГРУЗКЕ.
  //
  // Сервер разбирает вложенный идентификатор (RaceMessageDefinitions.cpp:1211-1320) и
  // ВЫБРАСЫВАЕТ разбор: наружу уходят те же байты (`:1325-1338`), и принимающий клиент
  // читает oid ИЗ НАГРУЗКИ. Поэтому гард на конверте закрывает только вывеску:
  // `fromOid = свой` + `snapshot.racerOid = чужой` телепортирует чужую лошадь на всех
  // экранах, `syncGoalIn.racerOid = чужой` объявляет чужой финиш, и так по всем
  // самоотчётным типам.
  //
  // ★ПРАВИЛО ТОТАЛЬНО ПО ТИПАМ, А НЕ ПО СПИСКУ МЕСТ: классификация — в
  // `race::GetRelayClaim` (RelayAuthz.hpp), её полноту доказывает
  // `tools/check_relay_authz.sh` (каждый элемент перечисления назван) и юнит-тест
  // (взято ТО поле). Неизвестный тип нагрузки сюда уже не доходит: с R71-14 такой кадр
  // отбрасывается выше, в самом switch'е, — авторизовать `Unparsed` нечем.
  const auto relayClaim = race::GetRelayClaim(command);
  const bool relayActorMismatch =
    (relayClaim.actorKind == race::RelayActorKind::RacerOid
      && relayClaim.actorId != senderRacer.oid)
    || (relayClaim.actorKind == race::RelayActorKind::CharacterUid
      && relayClaim.actorId != clientContext.characterUid);

  if (relayActorMismatch)
  {
    // oid бота — тот же законный вход, что и в конверте, и так же нечего вещать.
    if (relayClaim.actorKind == race::RelayActorKind::RacerOid
      && raceInstance.IsAiRacerOid(static_cast<tracker::Oid>(relayClaim.actorId)))
      return;

    uint64_t suppressed = 0;
    if (_relayActorThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Relay from racer {} carried payload type {:#04x} acting as {} (suppressed {})",
        senderRacer.oid,
        static_cast<uint16_t>(command.payloadType),
        relayClaim.actorId,
        suppressed);
    return;
  }

  // LOA-fix (R71-15, находка ревью 2 #3): «С ЧЕМ» — ВТОРАЯ ПОЛОВИНА ПРАВИЛА.
  //
  // Гард выше отвечает только на «кто». У `SetTargetState` рядом с проверенным
  // `invokerRacerOid` лежит `magicEffectId` (RelayMessageDefinitions.hpp:153-162), и он
  // уходил соседям как есть: `fromOid = свой`, `invoker = свой`, `magicEffectId =
  // 0xDEADBEEF` — конверт чист, действующее лицо своё, а названная величина выдумана.
  // Сервер этот тип нагрузки не интерпретирует, поэтому проверяется РОВНО ТО, ЧТО
  // СЕРВЕР ЗНАЕТ: идентификаторы экземпляров эффектов выдаёт он сам, значит честный
  // клиент может назвать только уже выданный (`HasIssuedEffectInstanceId`).
  //
  // ★НЕ БОЛЬШЕ ТОГО: «выдан» — не «жив» и не «твой». Сильная проверка живого
  // экземпляра есть там, где сервер знает семантику (ледяная стена, R71-17); здесь
  // выдумывать её значило бы поставить гард на догадку.
  if (relayClaim.referenceKind == race::RelayReferenceKind::EffectInstanceId
    && not raceInstance.GetTracker().HasIssuedEffectInstanceId(relayClaim.referencedId))
  {
    uint64_t suppressed = 0;
    if (_relayReferenceThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Relay from racer {} named an unissued effect instance {} (suppressed {})",
        senderRacer.oid,
        relayClaim.referencedId,
        suppressed);
    return;
  }

  // Relay the command to all other clients in the room

  // TODO: potential improvement - instead of blindly broadcasting to room,
  // forward packet to recepient if `toOid` is non-zero.
  this->BroadcastExceptCharacterUid(raceInstance, notify, clientContext.characterUid);
}

void RaceNetworkHandler::HandleUserRaceActivateInteractiveEvent(
  ClientId clientId,
  const protocol::AcCmdUserRaceActivateInteractiveEvent& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);

  // Get the sender's OID from the room tracker
  auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  protocol::AcCmdUserRaceActivateInteractiveEvent notify{
    .member1 = command.member1,
    .characterOid = racer.oid, // sender oid
    .member3 = command.member3
  };

  // Broadcast to all clients in the room
  this->Broadcast(raceInstance, notify);
}

void RaceNetworkHandler::HandleUserRaceActivateEvent(
  ClientId clientId,
  const protocol::AcCmdUserRaceActivateEvent& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  // Check if event is throttled, or add event if it is a new one
  if (raceInstance.GetTracker().IsEventThrottled(command.eventId))
  {
    // Event throttled
    return;
  }

  // Schedule a deactivate event notify
  _scheduler.Queue([this, clientId, eventId = command.eventId]()
  {
    protocol::AcCmdUserRaceDeactivateEvent deactivateCommand{
      .eventId = eventId};
    this->HandleUserRaceDeactivateEvent(clientId, deactivateCommand);
  }, std::chrono::steady_clock::now() + tracker::EventThrottleDuration);

  // Broadcast to all active racers in the race
  const protocol::AcCmdUserRaceActivateEventNotify notify{
    .eventId = command.eventId,
    .characterOid = racer.oid};
  this->Broadcast(raceInstance, notify);
}

void RaceNetworkHandler::HandleUserRaceDeactivateEvent(
  ClientId clientId,
  const protocol::AcCmdUserRaceDeactivateEvent& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  // Check if event is throttled, or add event if it is a new one
  if (raceInstance.GetTracker().IsEventThrottled(command.eventId))
  {
    // Event throttled
    return;
  }

  // Broadcast to all active racers in the race
  const protocol::AcCmdUserRaceDeactivateEventNotify notify{
    .eventId = command.eventId,
    .characterOid = racer.oid};
  this->Broadcast(raceInstance, notify);
}

void RaceNetworkHandler::HandleRequestMagicItem(
  const ClientId clientId,
  const protocol::AcCmdCRRequestMagicItem& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();
  auto& tracker = raceInstance.GetTracker();
  auto& racer = tracker.GetRacer(clientContext.characterUid);

  // TODO: Revise this on NPC races
  if (command.characterOid != racer.oid)
  {
    // LOA-fix (R57-5, round57, backlog #195): oid бота — законен, молчим.
    if (raceInstance.IsAiRacerOid(command.characterOid))
      return;

    server::util::QuietLogWarn("Client tried to perform action on behalf of different racer");
    return;
  }

  const auto& gameModeTemplate = GetServerInstance().GetCourseRegistry().GetCourseGameModeInfo(
    static_cast<uint8_t>(parameters.gameMode));

  // Only assign + respond if the gauge is full and the racer is empty-handed.
  // Anything else (stale request, duplicate after assignment, request while holding an item) drops.
  if (racer.magicItem.has_value()
    || racer.starPointValue < gameModeTemplate.starPointsMax)
  {
    return;
  }

  const auto& magicItemSlotInfo = race::MagicSystem::RandomMagicItem(
    _serverInstance.GetMagicRegistry(),
    tracker,
    clientContext.characterUid);
  racer.magicItem.emplace(magicItemSlotInfo.type);
  racer.starPointValue = 0;

  protocol::AcCmdCRStarPointGetOK starPointResponse{
    .characterOid = command.characterOid,
    .starPointValue = racer.starPointValue,
    .giveMagicItem = false};

  _commandServer.QueueCommand<decltype(starPointResponse)>(
    clientId,
    [starPointResponse]
    {
      return starPointResponse;
    });

  protocol::AcCmdCRRequestMagicItemOK response{
    .characterOid = command.characterOid,
    .magicItemId = racer.magicItem.value(),
    .member3 = 0};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]
    {
      return response;
    });

  // Notify other racers that racer is holding the magic item
  const protocol::AcCmdCRRequestMagicItemNotify notify{
    .magicItemId = response.magicItemId,
    .characterOid = response.characterOid};
  this->BroadcastExceptCharacterUid(raceInstance, notify, clientContext.characterUid);
}

void RaceNetworkHandler::HandleUseMagicItem(
  const ClientId clientId,
  const protocol::AcCmdCRUseMagicItem& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  // TODO: Revise this in NPC races
  if (command.characterOid != racer.oid)
  {
    // LOA-fix (R57-5, round57, backlog #195): oid бота — законен, молчим.
    if (raceInstance.IsAiRacerOid(command.characterOid))
      return;

    server::util::QuietLogWarn("Client tried to perform action on behalf of different racer");
    return;
  }

  if (not racer.magicItem.has_value() || command.magicItemId == 0)
  {
    racer.magicItem.reset();

    // Still acknowledge the (empty) usage so the client clears the held
    // magic item indicator.
    const protocol::AcCmdCRUseMagicItemOK response{
      .characterOid = command.characterOid};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]
      {
        return response;
      });
    return;
  }

  // LOA-fix (R71-1, backlog #129-S2): КАСТУЕТСЯ ТО, ЧТО ЛЕЖИТ НА РУКАХ.
  //
  // Предмет выдаёт СЕРВЕР — двумя путями, и оба сообщают клиенту точный номер:
  // `HandleRequestMagicItem` (:4565 `racer.magicItem.emplace(...)`, ответ :4580-4583) и
  // подбор из деки (:5081-5086). Значит честный клиент физически не может назвать
  // другой номер. До этой строки `GetSlotInfo(command.magicItemId)` (:4640) брал
  // КЛИЕНТСКОЕ число как есть, и весь эффект строился из него: гонщик, держащий
  // самый дешёвый предмет, кастовал любое заклинание из `magic.yaml`.
  //
  // ★ЗАОДНО ЗАКРЫВАЕТСЯ БРОСОК. `MagicRegistry::GetSlotInfo` на неизвестном номере
  // бросает `std::runtime_error` (MagicRegistry.cpp:192-198), а бросок из хендлера
  // ловится в CommandServer.cpp:515-527 и печатает строку `[error]` НА КАЖДЫЙ ПАКЕТ
  // без всякого дросселя. После этой проверки до :4640 доходит только номер, который
  // сервер сам и выдал, — то есть заведомо существующий.
  //
  // ★СРАВНЕНИЕ ВЕРНО И ДЛЯ КРИТА. Крит-подмену делает СЕРВЕР строкой ниже (:4642-4652)
  // по своим эффектам 18/19; клиент всегда присылает БАЗОВЫЙ номер. А если сервер выдал
  // крит-вариант сразу (`RandomMagicItem` умеет вернуть `criticalType`,
  // MagicSystem.cpp:130-133), то он же его и назвал — сравнение снова сходится.
  //
  // ★ГАРДА «а вдруг это бот» здесь НЕ НУЖНО: пакет за бота уже отсечён выше (:4610-4618,
  // R57-5), сюда доходит только собственный oid отправителя.
  if (command.magicItemId != racer.magicItem.value())
  {
    uint64_t suppressed = 0;
    if (_magicOwnershipThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Racer {} tried to cast magic {} while holding {} (suppressed {})",
        racer.oid,
        command.magicItemId,
        racer.magicItem.value(),
        suppressed);
    return;
  }

  // LOA-fix (R71-2, backlog item 26): УСИЛЕНИЕ ОДНИМ ПАКЕТОМ.
  //
  // Ледяная стена рассылает по ОДНОМУ `AcCmdRCMagicExpire` НА ЭЛЕМЕНТ списка КАЖДОМУ
  // в комнате (:4739-4761, `obstacleInstanceCount = command.targetList.size()`, цикл
  // Broadcast на :4750-4759): 255 x 8 = 2040 кадров с одного клиентского пакета. Тем
  // же числом двигается счётчик идентификаторов эффектов (:4655-4656), то есть
  // 16-битный счётчик выкручивается за десяток пакетов.
  //
  // ★ОТКАЗ, А НЕ ОБРЕЗКА. Обрезка спрятала бы попытку: честный максимум мал и известен
  // (см. `MaxMagicTargetListSize`), поэтому список длиннее — это не «клиент прислал
  // лишнее», а «клиент прислал невозможное».
  if (command.targetList.size() > MaxMagicTargetListSize)
  {
    uint64_t suppressed = 0;
    if (_magicTargetCountThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Racer {} named {} magic targets, max is {} (suppressed {})",
        racer.oid,
        command.targetList.size(),
        MaxMagicTargetListSize,
        suppressed);
    return;
  }

  auto targetList = command.targetList;

  auto magicSlotInfo = GetServerInstance().GetMagicRegistry().GetSlotInfo(command.magicItemId);

  // LOA-fix (R71-11, находка ревью 2 #7): НЕОБРАТИМОЕ ПОСЛЕДСТВИЕ — ПОСЛЕ ВСЕХ ГАРДОВ.
  //
  // Крит-подмена состоит из ДВУХ шагов: выбрать крит-вариант (чистый расчёт) и СПИСАТЬ
  // баф 18/19 (`RemoveEffect` — запись в трекер плюс широковещательный
  // `AcCmdRCRemoveSkillEffect`). Оба шага стояли ДО гарда R71-3, поэтому ледяная стена
  // с NaN-координатами съедала крит-баф, объявляла его снятие всей комнате и только
  // потом отбрасывалась — предмет при этом оставался на руках. То есть гард, который
  // «ничего не разрешает», всё равно давал читеру списать чужой (свой) баф пакетом,
  // который сервер сам же и признал невалидным.
  //
  // ★ВЫБОР ОСТАЁТСЯ ЗДЕСЬ, СПИСАНИЕ УЕЗЖАЕТ ВНИЗ. Гард R71-3 сравнивает наличие
  // `iceWallProperties` с РАЗРЕШЁННЫМ типом (`magicSlotInfo.type` после подмены), так
  // что выбор обязан быть сделан до него. А `RemoveEffect` не влияет ни на один гард —
  // его законное место сразу после последнего из них.
  const bool consumesCritBuff =
    (racer.effects[18] || racer.effects[19]) && (magicSlotInfo.criticalType != 0);

  if (consumesCritBuff)
    magicSlotInfo = GetServerInstance().GetMagicRegistry().GetSlotInfo(magicSlotInfo.criticalType);

  const bool isIceWall = magicSlotInfo.type == 10 || magicSlotInfo.type == 11;

  // LOA-fix (R71-3, SYNTHESIS item 10 + item 26): ОТВЕТ СОБИРАЕТСЯ ИЗ ДВУХ РАЗНЫХ ЧИСЕЛ.
  //
  // `iceWallProperties` ЧИТАЕТСЯ по сырому `command.magicItemId`
  // (RaceMessageDefinitions.cpp:1457-1474), а ПИШЕТСЯ по `magicSlotInfo.type`,
  // положенному в ответ (:1538-1546 для OK, :1766-1775 для Notify) — и там стоит
  // `assert(command.iceWallProperties.has_value())`, который в боевом образе выключен
  // (`Dockerfile` собирает RelWithDebInfo -> -DNDEBUG). Разойдись эти два числа —
  // `.value()` бросит `std::bad_optional_access` ИЗ ПОСТАВЩИКА ЗАПИСИ, а бросок оттуда
  // роняет соединение (Server.cpp:191-212): воспроизводимый кик игрока.
  //
  // ★ЧЕСТНО: СЕГОДНЯ РАСХОЖДЕНИЕ ОПЦИОНАЛА НЕДОСТИЖИМО, И ЭТО ДОКАЗАНО СТРУКТУРНО.
  // После R71-1 клиент называет только выданный сервером номер, а при ЛЮБОЙ
  // крит-подмене base->crit обе половины switch совпадают: 2<->3, 12<->13, 14<->15,
  // 16<->17, 18<->19 читают и пишут targetList; 4<->5, 6<->7, 8<->9, 20<->21, 22<->23,
  // 24<->25 — ни одна; 10<->11 — обе со стеной (сверено по `magic.yaml`, 25 записей;
  // `criticalType == 11` стоит ТОЛЬКО у типа 10 — рекон приводил обратный пример как
  // живой, он неверен). Гард стоит не «на баг», а на ИНВАРИАНТ: `magic.yaml` — конфиг,
  // он лежит на прод-хосте bind-mount'ом и правится отдельно от кода. Поставщик записи
  // не имеет права бросать — проверяем ДО очереди.
  //
  // Вторая половина того же вопроса — числовой мусор в координатах: они уходят каждому
  // клиенту как есть, серверной проверки размещения нет нигде (item 26). Эта половина
  // достижима СЕГОДНЯ и именно она судится на стенде.
  if (isIceWall != command.iceWallProperties.has_value()
    || (isIceWall && not IsFiniteIceWallPlacement(command.iceWallProperties.value())))
  {
    uint64_t suppressed = 0;
    if (_magicPayloadThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Racer {} sent an ice-wall payload inconsistent with resolved magic type {} "
        "(payload present: {}, suppressed {})",
        racer.oid,
        magicSlotInfo.type,
        command.iceWallProperties.has_value(),
        suppressed);
    return;
  }

  // LOA-fix (R71-21, находка ревью 2 #3): МЕСТО ПОД УЛИКУ СПРАШИВАЕТСЯ ДО КАСТА.
  //
  // Реестр выданных экземпляров — единственное, чем сервер потом отличит честный
  // отчёт «на мне сработало» от выдуманного. Прежняя редакция гарантировала место
  // ВЫТЕСНЕНИЕМ самой старой записи, то есть уничтожала улику о живой стене: девять
  // кастов по восемь сегментов давали 72 живых экземпляра при потолке 64, и честный
  // слом первых восьми стен после этого отбрасывался. Теперь наоборот: если места
  // нет — отказывает КАСТ, и отказ стоит ЗДЕСЬ, до списания крит-бафа, до выдачи
  // номеров и до любой рассылки. Ёмкость считается НА КАСТЕРА, поэтому флудер
  // отказывает в кастах только самому себе, а чужую улику вытеснить не может.
  const uint16_t issuedInstanceCount = isIceWall
    ? static_cast<uint16_t>(command.targetList.size())
    : uint16_t{1};

  if (not raceInstance.GetTracker().CanIssueEffectInstances(racer.oid, issuedInstanceCount))
  {
    uint64_t suppressed = 0;
    if (_effectInstanceCapacityThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Racer {} exhausted its effect-instance budget ({} per race), cast of magic {} "
        "refused (suppressed {})",
        racer.oid,
        tracker::RaceTracker::MaxEffectInstancesPerRacer,
        magicSlotInfo.type,
        suppressed);
    return;
  }

  // LOA-fix (R71-11): вот теперь баф списывается — ни один гард выше уже не может
  // отбросить пакет, значит списание и его широковещательное объявление больше не
  // случаются «в никуда».
  if (consumesCritBuff)
  {
    // Consume the crit chance buff immediately
    for (const uint32_t critEffectId : {18u, 19u})
    {
      if (racer.effects[critEffectId])
        RemoveEffect(raceInstance, racer, critEffectId);
    }
  }

  const uint16_t effectInstanceId = raceInstance.GetTracker().GetNextEffectInstanceIdAndIncrementBy(
    issuedInstanceCount);

  // LOA-fix (R71-17, находка ревью 2 #2; РАСШИРЕНО R71-20 по находке ревью 2 #1):
  // СЕРВЕР ЗАПОМИНАЕТ ВСЁ, ЧТО САМ ВЫДАЛ.
  //
  // Отчёт «на мне сработал эффект» объявляет КЛИЕНТ, и номер экземпляра в его пакете
  // до этого раунда ничем не сверялся. Здесь — единственное место, где экземпляры
  // рождаются, поэтому здесь же они и записываются: тип берётся РАЗРЕШЁННЫЙ
  // (`magicSlotInfo.type`, уже после крит-подмены), владелец — отправитель, чей oid
  // сверен гардом R57-5 выше.
  //
  // ★ЗАПИСЫВАЮТСЯ ВСЕ ТИПЫ, А НЕ ОДНИ СТЕНЫ. Первая редакция вела реестр только для
  // ледяной стены — то есть вела СПИСОК МЕСТ вместо правила, и всё остальное
  // (`effectId = 2` + выдуманный `effectInstanceId` = бесплатный водяной щит) ехало
  // мимо. Правило теперь одно: экземпляр эффекта существует ровно тогда, когда его
  // выдал сервер.
  //
  // Снимаются записи только по событию, которое сервер объявил сам: по слому стены
  // (`HandleActivateSkillEffect`) и по её истечению — тем же отложенным вызовом,
  // который рассылает `AcCmdRCMagicExpire`. Остальные живут до конца заезда
  // (`RaceTracker::Clear`): у них нет объявленного сервером срока, и выдумывать его
  // значило бы выбрасывать честные отчёты о попадании.
  raceInstance.GetTracker().AddEffectInstances(
    effectInstanceId,
    issuedInstanceCount,
    magicSlotInfo.type,
    racer.oid);

  // Darkfire should only affect one target
  // Client sends all targets infront of them but we should only apply the effect to the targeted one (the arrow above their head)
  if (magicSlotInfo.type == 14)
    targetList.resize(1);

  // Dragon handling
  if (magicSlotInfo.basicType == 16)
  {
    if (!targetList.empty())
    {
      auto& racers = raceInstance.GetTracker().GetRacers();

      const auto targetOid = targetList[0];
      const auto targetIter = std::ranges::find_if(
        racers,
        [targetOid](const auto& entry)
        {
          return entry.second.oid == targetOid;
        });

      if (targetIter == racers.end())
      {
        targetList.clear();
      }
      else
      {
        auto& targetRacer = targetIter->second;

        // If target has already a dragon, miss
        if (targetRacer.pendingMagicTarget.has_value())
        {
          targetList.clear();
        }
      }
    }
  }

  protocol::AcCmdCRUseMagicItemOK response{
    .characterOid = command.characterOid,
    .magicItemId = magicSlotInfo.type,
    .iceWallProperties = command.iceWallProperties,
    .targetList = targetList,
    .effectInstanceId = effectInstanceId,
    .unk4 = magicSlotInfo.castingTime
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]
    {
      return response;
    });

  // Notify other players that this player used their magic item
  protocol::AcCmdCRUseMagicItemNotify usageNotify{
    .characterOid = command.characterOid,
    .magicItemId = magicSlotInfo.type,
    .iceWallProperties = command.iceWallProperties,
    .targetList = targetList,
    .effectInstanceId = effectInstanceId,
    .unk4 = magicSlotInfo.castingTime};

  // Send usage notification to other players
  this->BroadcastExceptCharacterUid(raceInstance, usageNotify, clientContext.characterUid);

  // Send effect for items that have instant effects
  switch (magicSlotInfo.type)
  {
    // Shield, Booster, Phoenix
    case 4:
    case 5:
    case 6:
    case 7:
    case 8:
    case 9:
      this->ScheduleSkillEffect(raceInstance, command.characterOid, racer.oid, magicSlotInfo, effectInstanceId);
      break;
    // IceWall
    case 10:
    case 11:
    {
      const uint16_t obstacleInstanceCount = static_cast<uint16_t>(command.targetList.size());
      _scheduler.Queue(
        [this, effectInstanceId, obstacleInstanceCount, magicType = magicSlotInfo.type, roomUid = raceInstance.GetRoomUid()]()
        {
          std::scoped_lock lock(_raceInstancesMutex);
          const auto raceInstanceIter = _raceInstances.find(roomUid);
          if (raceInstanceIter == _raceInstances.cend())
            return;

          auto& raceInstance = raceInstanceIter->second;

          for (uint16_t i = 0; i < obstacleInstanceCount; ++i)
          {
            this->Broadcast(
              raceInstance,
              protocol::AcCmdRCMagicExpire{
                .magicType = magicType,
                .firstObstacleInstanceId = static_cast<uint16_t>(effectInstanceId + i),
                .obstacleInstanceCount = 1,
                .breakdown = 0});
          }

          // LOA-fix (R71-17): стена истекла — экземпляров больше нет. Снятие стоит
          // ЗДЕСЬ, а не по таймеру в реестре: живым экземпляр считается ровно столько,
          // сколько сервер сам объявил его живым, и выдумывать второй срок жизни (а с
          // ним и запас на задержку сети) не приходится.
          raceInstance.GetTracker().RemoveEffectInstances(
            effectInstanceId, obstacleInstanceCount);
        },
        Scheduler::Clock::now() + std::chrono::seconds(4)); // TODO: Change to 4 seconds
      break;
    }
    // BufPower, BufGauge, BufSpeed
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    {
      for (auto& otherRacer : raceInstance.GetTracker().GetRacers() | std::views::values)
      {
        if (racer.oid == otherRacer.oid
        || (racer.team != tracker::RaceTracker::Racer::Team::Solo && racer.team == otherRacer.team))
        {
          this->ScheduleSkillEffect(raceInstance, command.characterOid, otherRacer.oid, magicSlotInfo, effectInstanceId);
        }
      }
      break;
    }
  }

  // LOA-fix (NEW-1, round3): дейлики «попаданий магическим шаром» — 1008 (20) и
  // 1019 (50). basicType 2 = FireBall (magic.yaml: type 2 обычный, type 3 крит,
  // basicType у обоих 2). Прогресс раньше писал клиент. Строгого «попадания»
  // сервер не считает — targetList приезжает от клиента, — но засчитываем только
  // РЕАЛЬНЫЙ каст: предмет должен был лежать на руках (гейт выше по функции), он
  // тут же расходуется (magicItem.reset() ниже), цель непустая и гонщик в гонке.
  // Это несравнимо строже прежнего «клиент прислал число».
  // gameModeFlag дейликов = 76 (MagicSoloAction), остальное отсеет IsModeMatch.
  // ⚠️ (N4, round7) ЭТО ГЕЙТ КВЕСТ-СЧЁТЧИКА, А НЕ ДЕНЕГ: выплатной античит по
  // racer.state откачен раундом 6, награда за заезд состояние не читает.
  if (magicSlotInfo.basicType == 2
      && not targetList.empty()
      && racer.state == tracker::RaceTracker::Racer::State::Racing)
  {
    const auto& magicParameters = raceInstance.GetParameters();
    const auto fireballDailyNotifies = GetServerInstance().GetQuestSystem().OnQuestEvent(
      clientContext.characterUid,
      QuestSystem::QuestEvent::FireballAttack,
      QuestSystem::ToGameModeFlag(magicParameters.gameMode, magicParameters.teamMode),
      0,
      {1008u, 1019u});
    for (const auto& fireballNotify : fireballDailyNotifies)
    {
      SendDailyQuestNotificationToCharacter(
        fireballNotify.characterUid,
        fireballNotify.questId,
        fireballNotify.objectiveProgress,
        fireballNotify.carrotsReward,
        fireballNotify.rewardType,
        fireballNotify.unk2,
        fireballNotify.mountExp);
    }
  }

  racer.magicItem.reset();
}

void RaceNetworkHandler::HandleUserRaceItemGet(
  const ClientId clientId,
  const protocol::AcCmdUserRaceItemGet& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);

  auto& raceInstance = GetRaceInstance(clientContext);

  // LOA-fix (R57-11, round57, backlog #195): «предмет подобрал бот» — не выдача
  // человеку. Проверки владения здесь нет: `characterOid` идёт только эхом в
  // исходящие пакеты, а выдача (яйцо, магический предмет, звёздные точки)
  // делается ОТПРАВИТЕЛЮ. Семь ботов, катающихся по предметам, выдавали бы их
  // живому игроку — и ни одной строки в логе.
  if (raceInstance.IsAiRacerOid(command.characterOid))
    return;

  auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  // LOA-fix (R71-7, находка ревью W2): «ПРЕДМЕТ ПОДОБРАЛ X» — ТОЛЬКО ПРО СЕБЯ.
  //
  // Сравнения владения здесь не было вовсе: единственной проверкой был отсев ботов
  // (:4836, R57-11), а `command.characterOid` уходил ЭХОМ в три широковещательных
  // кадра — `AcCmdGameRaceItemGet` на :4866-4870 (квестовая/яичная ветка) и :5132-5137
  // (общая дека), плюс `AcCmdCRRequestMagicItemNotify` на :5116-5118. Живой чужой oid
  // проходил насквозь: A объявлял комнате «предмет подобрал B».
  //
  // ★ЭТО ТО ЖЕ ПРАВИЛО РАУНДА, А НЕ СОСЕДНЕЕ. Выдача и так делается ОТПРАВИТЕЛЮ (по
  // `clientContext.characterUid`), поэтому чинится ровно расхождение «кому выдали» и
  // «про кого сказали». Оракул #195 УЖЕ считает этот хендлер накрытым по полю
  // `characterOid` (oracle.py:142-143) — до этого коммита это утверждение оракула было
  // неверным; теперь оно верно.
  if (command.characterOid != racer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.characterOid))
      return;

    uint64_t suppressed = 0;
    if (_itemGetOwnershipThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Racer {} claimed an item pickup for racer {} (suppressed {})",
        racer.oid,
        command.characterOid,
        suppressed);
    return;
  }

  // Check event items first (eggs, etc.)
  const auto eventItemOid = raceInstance.GetTracker().FindEventItem(
    clientContext.characterUid,
    command.itemDeckId);

  if (eventItemOid != tracker::InvalidEntityOid)
  {
    auto& eventItem = raceInstance.GetTracker().GetEventItem(clientContext.characterUid, eventItemOid);
    const auto eggInfo = _serverInstance.GetPetRegistry().GetEggInfoByDeckId(eventItem.itemType);
    auto itemUid = data::InvalidUid;
    const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
      clientContext.characterUid);

    characterRecord.Mutable([this, &eggInfo, &itemUid](data::Character& character)
    {
      itemUid = _serverInstance.GetItemSystem().AddItem(character, eggInfo.tid, 1);
    });

    // Notify racers that invoker got the egg
    const protocol::AcCmdRCObtainEgg obtainEgg{
      .characterUid = clientContext.characterUid,
      .ItemUid = itemUid,
      .ItemTid = eggInfo.tid};
    this->Broadcast(raceInstance, obtainEgg);

    const protocol::AcCmdGameRaceItemGet itemGet{
      .characterOid = command.characterOid,
      .itemId = command.itemDeckId,
      .itemType = eventItem.itemType};
    this->Broadcast(raceInstance, itemGet);

    raceInstance.GetTracker().RemoveEventItem(clientContext.characterUid, command.itemDeckId);
    racer.trackedDecks.erase(command.itemDeckId);

    return;
  }

  // LOA-fix (R68, backlog #5/#99): ПОДБОР КВЕСТОВОГО ПРЕДМЕТА.
  //
  // Ветка стоит ЗДЕСЬ — после яиц и ДО общих деков — по двум причинам:
  // квестовые предметы пер-гонщиковые (как яйца), и в общей карте деков их нет
  // вовсе, поэтому ниже они упёрлись бы в жалобу «picked up untracked item
  // deck» и подбор бы просто терялся.
  //
  // ★СНАЧАЛА СНИМАЕМ ПРЕДМЕТ, ПОТОМ ЗАСЧИТЫВАЕМ. Повторный пакет с тем же oid
  // (лаг, дубликат, модклиент) обязан не найти предмета и уйти молча: иначе
  // один предмет двигал бы счётчик квеста сколько угодно раз.
  if (const auto* questItem = raceInstance.GetTracker().FindQuestItem(
        clientContext.characterUid, command.itemDeckId))
  {
    // ★ГЕЙТ «ЗАЕЗД ИДЁТ ПРЯМО СЕЙЧАС». Три условия закрывают три РАЗНЫХ окна,
    // в которых предмет физически лежит в трекере, а подобрать его честно
    // нельзя:
    //   * стадия — предметы раскладываются в `HandleStartRace`, то есть ещё в
    //     `Loading`, и НЕ снимаются в `Stop()`: трекер чистит только следующий
    //     `HandleStartRace`. Без этой проверки модклиент собирал бы весь
    //     квест, сидя в комнате ожидания ПОСЛЕ заезда;
    //   * зелёный свет — стадия `Racing` наступает за отсчёт до старта
    //     (`waitTime` карты, 10 с), и всё это время игрок стоит. Тот же гейт
    //     `now >= GetRaceStartTimePoint()` стоит на телеметрии R24 и на
    //     детекторе мести — берём его же, а не изобретаем свой;
    //   * состояние гонщика — уже финишировавший стоит на финише и подбирать
    //     ничего не может.
    // ★МОЛЧА, БЕЗ ЛОГА: путь удалённо-управляемый, а логирование на таком пути
    // уже давало 15 350 строк за час живой игры (R57/#195).
    // ★ЧЕГО ЭТОТ ГЕЙТ НЕ ДЕЛАЕТ: он не проверяет, что гонщик РЯДОМ с
    // предметом. Серверной проверки близости в этом коде нет НИ У ОДНОГО вида
    // предметов (подковы, магия, яйца), а `racer.worldPosition` приходит от
    // клиента и не клампится — проверка по нему обходилась бы враньём о
    // позиции и при этом умела бы отказать честному игроку на лаге. Это
    // названный остаток раунда, а не недосмотр.
    const auto raceStage = raceInstance.GetStage();
    const bool raceIsLive =
      (raceStage == RaceInstance::Stage::Racing
        || raceStage == RaceInstance::Stage::Finishing)
      && std::chrono::steady_clock::now() >= raceInstance.GetRaceStartTimePoint()
      && racer.state == tracker::RaceTracker::Racer::State::Racing;

    if (not raceIsLive)
      return;

    // Копии ДО удаления: `RemoveQuestItem` инвалидирует указатель.
    const uint32_t questItemType = questItem->itemType;
    const uint32_t questItemId = questItem->questItemId;
    const uint32_t questItemTid = questItem->questTid;

    raceInstance.GetTracker().RemoveQuestItem(
      clientContext.characterUid, command.itemDeckId);
    racer.trackedDecks.erase(command.itemDeckId);

    // Клиенты в комнате убирают предмет с карты.
    const protocol::AcCmdGameRaceItemGet questItemGet{
      .characterOid = command.characterOid,
      .itemId = command.itemDeckId,
      .itemType = questItemType};
    this->Broadcast(raceInstance, questItemGet);

    // ★ЗАСЧИТЫВАЕМ РОВНО В ТОТ КВЕСТ, РАДИ КОТОРОГО ПРЕДМЕТ ЛЕЖАЛ, и только в
    // него: список из ОДНОГО tid. Передать сюда все 12 значило бы отдать +1
    // каждому активному квесту того же класса — пять сюжетных делят QTemID 3,
    // три делят QTemID 6, — то есть напечатать прогресс из одного объекта.
    // ★Сверка значения (`matchFunctionValue = true`) остаётся ПОВЕРХ этого как
    // инвариант: предмет положен ради этого квеста, значит его `QTemID` обязан
    // совпасть с `Quest::functionValue`. Если когда-нибудь разойдётся —
    // прогресс не двинется, а не двинется НЕ ТОТ.
    const auto advancedQuests = GetServerInstance().GetQuestSystem().AdvanceMainQuests(
      clientContext.characterUid,
      std::vector<uint32_t>{questItemTid},
      true,
      questItemId);

    // ЖИВОЙ NOTIFY ПРОГРЕССА С РЕЙС-КАНАЛА — тем же способом, что доказан
    // R16-1 для сюжетных счётчиков заездов: 0x3fe клиент рисует и придя по
    // рейс-сокету. Без него игрок собирает предметы весь заезд и видит 0/20
    // до следующего входа на ранчо.
    for (const auto& advancedQuest : advancedQuests)
    {
      protocol::AcCmdRCUpdateQuestNotify questNotify{};
      questNotify.characterUid = static_cast<uint32_t>(clientContext.characterUid);
      questNotify.questTid = static_cast<uint16_t>(advancedQuest.questTid);
      questNotify.objectiveProgress.progress = advancedQuest.progress;
      questNotify.objectiveProgress.isCompleted = advancedQuest.completed;
      _commandServer.QueueCommand<protocol::AcCmdRCUpdateQuestNotify>(
        clientId,
        [questNotify]() { return questNotify; });
    }

    return;
  }

  auto& items = raceInstance.GetTracker().GetItemDecks();
  const auto deckIter = items.find(command.itemDeckId);
  if (deckIter == items.end())
  {
    // LOA-fix (R71-13, находка ревью 2 #5): ЭТА СТРОКА ПИШЕТСЯ НА ЧЕСТНОМ ПОВЕДЕНИИ.
    //
    // Сюда приходят три разных случая, и только один из них — попытка: подделанный
    // `itemDeckId` (гард R71-7 закрывает чужой oid, но не выдуманный номер деки),
    // ДУБЛЬ честного пакета подбора (дека уже снята первым) и подбор деки, снятой
    // соседом миллисекундой раньше. Два последних — обычная гонка, а строка писалась
    // на каждый пакет: соседний комментарий про «тихую обработку дубля» относится к
    // ветке кулдауна НИЖЕ и этой ветки не касался. Свой дроссель, а не общий: флуд
    // подделанным номером не должен глушить жалобы других гардов раунда.
    uint64_t suppressed = 0;
    if (_itemDeckUnknownThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Client {} picked up untracked item deck {} (suppressed {})",
        clientId,
        command.itemDeckId,
        suppressed);
    return;
  }

  auto& deck = deckIter->second;

  const auto now = std::chrono::steady_clock::now();

  const auto deckCooldownIter = racer.deckCooldown.find(
    command.itemDeckId);

  if (deckCooldownIter != racer.deckCooldown.end()
    && deckCooldownIter->second > now)
  {
    // Picker's client predictively hid the item on collision; untrack it
    // so processItemSpawn re-broadcasts the spawn for them next tick.
    racer.trackedDecks.erase(command.itemDeckId);
    return;
  }

  // On pickup, clear the spawner cooldown for all other racers
  // so it remains available for them immediately
  for (auto& [otherUid, otherRacer] : raceInstance.GetTracker().GetRacers())
  {
    if (otherUid != clientContext.characterUid)
    {
      otherRacer.deckCooldown.erase(command.itemDeckId);
    }
  }

  // Set the pickup cooldown exclusively for the collecting racer
  racer.deckCooldown[command.itemDeckId] = now + deck.respawnTime;

  Room::GameMode gameMode;
  registry::Course::GameModeInfo gameModeInfo;
  _serverInstance.GetRoomSystem().GetRoom(clientContext.roomUid, [this, &gameMode, &gameModeInfo](const Room& room)
  {
    gameMode = room.GetRoomSnapshot().details.gameMode;
    gameModeInfo = this->GetServerInstance().GetCourseRegistry().GetCourseGameModeInfo(static_cast<uint8_t>(gameMode));
  });

  switch(gameMode)
  {
    // TODO: Deduplicate from StarPointGet
    case Room::GameMode::Speed:
      {
        switch (deck.currentItem)
        {
          case 101: // Gold horseshoe. Get star points until the next boost
            racer.starPointValue = std::min(((racer.starPointValue/40000)+1) * 40000, gameModeInfo.starPointsMax);
            break;
          case 102: // Silver horseshoe. Get 10k star points
            racer.starPointValue = std::min(racer.starPointValue+10000, gameModeInfo.starPointsMax);
            break;
          default:
            server::util::QuietLogWarn("Player {} picked up unknown item type {}",
              clientId, deck.currentItem);
            break;
        }

        // Only send this on good/perfect starts
        protocol::AcCmdCRStarPointGetOK starPointResponse{
          .characterOid = command.characterOid,
          .starPointValue = racer.starPointValue,
          .giveMagicItem = false
        };

        _commandServer.QueueCommand<decltype(starPointResponse)>(
          clientId,
          [clientId, starPointResponse]()
          {
            return starPointResponse;
          });
      }
      break;

    // TODO: Deduplicate from RequestMagicItem
    case Room::GameMode::Magic:
    {
      uint32_t magicItem{};
      if (not racer.magicItem.has_value())
      {
        // Racer is empty handed

        // Get the item type of the picked up item (408, 409 etc)
        const uint32_t magicItemType = deck.currentItem;

        // Get the magic slot index to indicate to the racer that they
        // have the item (water shield, ice wall etc).
        magicItem = _serverInstance.GetCourseRegistry()
          .GetDeckItemInfo(magicItemType).magicSlot;

        // Get the magic item's slot info and check if it gives positional magic
        const auto& slotInfo = _serverInstance.GetMagicRegistry().GetSlotInfo(magicItem);
        if (slotInfo.givePositionalMagic != 0)
        {
          const auto& magicItemSlotInfo = race::MagicSystem::RandomMagicItem(
            _serverInstance.GetMagicRegistry(),
            raceInstance.GetTracker(),
            clientContext.characterUid);
          magicItem = magicItemSlotInfo.type;
        }

        // Response with OK to the client that they have a new item in hand
        protocol::AcCmdCRRequestMagicItemOK magicItemOk{
          .characterOid = command.characterOid,
          .magicItemId = racer.magicItem.emplace(magicItem),
          .member3 = 0};

        _commandServer.QueueCommand<decltype(magicItemOk)>(
          clientId,
          [clientId, magicItemOk]()
          {
            return magicItemOk;
          });

        racer.starPointValue = 0;

        protocol::AcCmdCRStarPointGetOK starPointResponse{
          .characterOid = command.characterOid,
          .starPointValue = 0,
          .giveMagicItem = false};

        _commandServer.QueueCommand<decltype(starPointResponse)>(
          clientId,
          [starPointResponse]()
          {
            return starPointResponse;
          });
      }
      else
      {
        // Racer is already holding the item, do not replace it
        magicItem = racer.magicItem.value();
      }

      // The item was picked up, generate a new item.
      raceInstance.PickRandomItemFromDeck(deck);

      // Notify racers in the race room that the invoking racer is now
      // holding a new magic item
      const protocol::AcCmdCRRequestMagicItemNotify notify{
        .magicItemId = magicItem,
        .characterOid = command.characterOid,};

      // Prevent self broadcast,
      // this prevents the double pickup UI bug for the invoker)
      this->BroadcastExceptCharacterUid(
        raceInstance,
        notify,
        clientContext.characterUid);

      break;
    }
  }

  // Notify all clients in the room that this item has been picked up
  const protocol::AcCmdGameRaceItemGet get{
    .characterOid = command.characterOid,
    .itemId = command.itemDeckId,
    .itemType = deck.currentItem,
  };
  this->Broadcast(raceInstance, get);

  // Erase the item from item instances of each client.
  for (auto& raceRacer : raceInstance.GetTracker().GetRacers() | std::views::values)
  {
    raceRacer.trackedDecks.erase(deck.oid);
  }
}

// Magic Targeting System Implementation for Bolt
void RaceNetworkHandler::HandleStartMagicTarget(
  const ClientId clientId,
  const protocol::AcCmdCRStartMagicTarget& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  // TODO: Revise this in NPC races
  if (command.casterOid != racer.oid)
  {
    // LOA-fix (R57-6, round57, backlog #195): oid бота — законен, молчим.
    if (raceInstance.IsAiRacerOid(command.casterOid))
      return;

    server::util::QuietLogWarn("Character OID mismatch in HandleStartMagicTarget");
    return;
  }

  auto& racers = raceInstance.GetTracker().GetRacers();
  const auto targetIter = std::ranges::find_if(
    racers,
    [&command](const auto& entry)
    {
      return entry.second.oid == command.targetOid;
    });

  if (targetIter == racers.end())
  {
    // LOA-fix (R57-8, round57, backlog #195): цель-бот — не ошибка. Живой
    // игрок вправе навести магию на AI-соперника; у сервера просто нет для
    // него состояния, поэтому наводить нечего и жаловаться не на что.
    if (not raceInstance.IsAiRacerOid(command.targetOid))
      server::util::QuietLogWarn("Target OID {} not found in HandleStartMagicTarget", command.targetOid);

    return;
  }

  auto& targetRacer = targetIter->second;

  if (targetRacer.pendingMagicTarget.has_value())
    return;

  targetRacer.dragonReceivedAt = std::chrono::steady_clock::now();
  targetRacer.pendingMagicTarget = {command.casterOid, command.effectInstanceId};
}

void RaceNetworkHandler::HandleChangeMagicTarget(
  const ClientId clientId,
  const protocol::AcCmdCRChangeMagicTarget& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  auto& racer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  if (command.targetOid!= racer.oid)
  {
    // LOA-fix (R57-7, round57, backlog #195): здесь поле названо targetOid, но
    // проверяется им ОТПРАВИТЕЛЬ (наименование апстрима). oid бота — законен.
    if (raceInstance.IsAiRacerOid(command.targetOid))
      return;

    server::util::QuietLogWarn("Character OID mismatch in HandleChangeMagicTarget");
    return;
  }

  if (!racer.pendingMagicTarget.has_value())
  {
    server::util::QuietLogWarn("Caster does not have dragon in HandleChangeMagicTarget");
    return;
  }

  // Find the target racer based on targetOid2
  auto& racers = raceInstance.GetTracker().GetRacers();
  const auto targetIter = std::ranges::find_if(
    racers,
    [&command](const auto& entry)
    {
      return entry.second.oid == command.targetOid2;
    });

  if (targetIter == racers.end())
  {
    // LOA-fix (R57-9, round57, backlog #195): цель-бот — не ошибка (текст
    // жалобы апстрима здесь ошибочно называет чужую функцию и чужое поле;
    // трогать его не стали, чтобы правка осталась про один дефект).
    if (not raceInstance.IsAiRacerOid(command.targetOid2))
      server::util::QuietLogWarn("Target OID {} not found in HandleStartMagicTarget", command.targetOid);

    return;
  }

  auto& targetRacer = targetIter->second;

  // Enforce cooldown: dragon cannot be passed until 5s after it was received
  constexpr auto DragonPassCooldown = std::chrono::milliseconds(500);
  if (std::chrono::steady_clock::now() - racer.dragonReceivedAt < DragonPassCooldown)
  {
    protocol::AcCmdCRChangeMagicTargetCancel response{
      .effectInstanceId = command.effectInstanceId,
      .casterOid = command.casterOid,
      .targetOid = command.targetOid,
      .targetOid2 = command.targetOid2
    };

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]() { return response; });
    return;
  }

  // Send Cancel if the target already has dragon, otherwise send OK and update the target's dragon status
  if (targetRacer.pendingMagicTarget.has_value())
  {
    // Send Cancel response
    protocol::AcCmdCRChangeMagicTargetCancel response{
      .effectInstanceId = command.effectInstanceId,
      .casterOid = command.casterOid,
      .targetOid = command.targetOid,
      .targetOid2 = command.targetOid2
    };

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]() { return response; });

    return;
  }

  targetRacer.dragonReceivedAt = std::chrono::steady_clock::now();
  targetRacer.pendingMagicTarget = {command.casterOid, command.effectInstanceId};
  racer.pendingMagicTarget.reset();

  // Send OK response
  protocol::AcCmdCRChangeMagicTargetOK response{
    .effectInstanceId = command.effectInstanceId,
    .casterOid = command.casterOid,
    .targetOid = command.targetOid,
    .targetOid2 = command.targetOid2
  };
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]() { return response; });

  // Send targeting notification to the target
  const protocol::AcCmdCRChangeMagicTargetNotify targetNotify{
    .effectInstanceId = command.effectInstanceId,
    .casterOid = command.casterOid,
    .targetOid = command.targetOid,
    .targetOid2 = command.targetOid2
  };
  this->Broadcast(raceInstance, targetNotify);
}

void RaceNetworkHandler::HandleActivateSkillEffect(
  const ClientId clientId,
  const protocol::AcCmdCRActivateSkillEffect& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);

  // LOA-fix (R57-12, round57, backlog #195). ★ГАРД СТОИТ НА `targetOid`, И ЭТО
  // НЕ ОЧЕВИДНО: хендлер target-reported — отправитель и есть ЦЕЛЬ, клиент
  // сообщает «на МНЕ сработал эффект от attackerOid», поэтому `targetRacer`
  // ищется по characterUid отправителя. Наш собственный разбор PLAN-129 §4
  // фиксирует ту же семантику.
  //
  // Отсюда два вывода, оба важные:
  //  * `targetOid` = кого объявили задетым. Назвали бота — у сервера для него
  //    состояния нет, делать нечего. Без этого гарда пакет всё равно доходит до
  //    хвоста, где `targetRacer.pendingMagicTarget.reset()` выполняется
  //    БЕЗУСЛОВНО, то есть чужой «эффект по боту» сбивал бы наводку ЖИВОМУ;
  //  * `attackerOid` гардить НЕЛЬЗЯ. Бот, ударивший игрока, — законное событие,
  //    и клиент честно о нём сообщает. Гард там выбрасывал бы эти пакеты, и
  //    магия ботов перестала бы действовать на человека: правка ради тишины
  //    сломала бы игру. Поймано ревью — первая редакция раунда гардила именно
  //    `attackerOid`.
  if (raceInstance.IsAiRacerOid(command.targetOid))
    return;

  auto& targetRacer = raceInstance.GetTracker().GetRacer(clientContext.characterUid);

  // LOA-fix (R71-4, backlog #129-S3): ЭФФЕКТ ОБЪЯВЛЯЕТСЯ ТОЛЬКО НА СЕБЕ.
  //
  // Хендлер target-reported — это разобрано прямо выше (R57-12): отправитель И ЕСТЬ
  // цель, поэтому `targetRacer` ищется по characterUid отправителя. Но
  // `command.targetOid` уходит НЕ только в эхо: он идёт в `ScheduleSkillEffect` (:5353),
  // который ищет жертву ПО ЭТОМУ oid (:5549-5550) и вешает эффект на найденного
  // (:5634, `targetRacer.effects[effectId] = true`), разослав всем
  // `AcCmdRCAddSkillEffect` с `characterOid = targetOid` (:5611-5626). До этой строки
  // любой гонщик мог объявить «на игроке Y сработала молния» — и она реально вешалась.
  //
  // ★ГАРД НА `targetOid`, А НЕ НА `attackerOid` — ровно по причине R57-12: бот, ударивший
  // игрока, законен, клиент честно о нём сообщает, и гард на `attackerOid` выбросил бы
  // эти события (первая редакция R57 сломала именно это; арка 3 стенда это стережёт).
  //
  // ★ВНУТРЕННЯЯ ВЕТКА «а вдруг это бот» СЕГОДНЯ МЕРТВА: та же проверка стоит на :5330 и
  // возвращает раньше. Она здесь намеренно, и по двум причинам: форма гарда одна на все
  // места раунда и не должна зависеть от того, что стоит рядом (уберут ту строку — эта
  // останется корректной), и ровно эту форму сверяет оракул #195 (`GUARDED_BRANCH`,
  // oracle.py:354-357). Читатель, не ищи здесь обработку ботовых эффектов — её тут нет.
  if (command.targetOid != targetRacer.oid)
  {
    if (raceInstance.IsAiRacerOid(command.targetOid))
      return;

    uint64_t suppressed = 0;
    if (_skillTargetThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Racer {} declared a skill effect on foreign racer {} (suppressed {})",
        targetRacer.oid,
        command.targetOid,
        suppressed);
    return;
  }

  // LOA-fix (R71-5, находка ревью W1): ГАРД, КОТОРЫЙ ОБХОДИТСЯ СОСЕДНИМ ПОЛЕМ ТОГО ЖЕ
  // ПАКЕТА, — не гард.
  //
  // `MagicRegistry::GetSlotInfoByEffectId` на неизвестном id БРОСАЕТ
  // (MagicRegistry.cpp:200-208). Бросок ловится в CommandServer.cpp:515-527 и печатает
  // `QuietLogError("Unhandled exception handling command …")` — ОДНУ СТРОКУ НА КАЖДЫЙ
  // ПАКЕТ, без дросселя. То есть после R71-4 читеру достаточно перестать подделывать
  // `targetOid` (тихо отброшено) и начать слать `effectId = 0xDEADBEEF` на частоте тика,
  // чтобы получить ровно тот лог-флуд, ради которого заведён `LogThrottle` (R57: 15 350
  // строк за час). Цель раунда побеждалась полем, лежащим в том же пакете.
  //
  // ★ПРОВЕРКА ТОЙ ЖЕ ВЕЛИЧИНОЙ, ЧТО И БРОСОК. `GetSlotInfoByEffectId` линейно ищет
  // `skillEffectId == effectId` по всей карте (25 записей); здесь тот же поиск, только
  // без исключения. Не «похожая» проверка по своему списку — иначе она разошлась бы с
  // реестром при первом же изменении `magic.yaml`.
  //
  // ★ДОПОЛНЕНО ПО РЕВЬЮ 2 (находка #1): «ЕСТЬ В РЕЕСТРЕ» ЕЩЁ НЕ ЗНАЧИТ «РАБОТАЕТ».
  // Первая редакция гарда спрашивала только про реестр — и пропускала
  // `effectId = 99999`, который в поставляемом `magic.yaml` РЕАЛЬНО ЛЕЖИТ (запись
  // type 27, :748). Пакет проходил гард, доезжал до `ScheduleSkillEffect` и печатал
  // там `[error] skillEffectId 99999 out of range` — на КАЖДЫЙ пакет, без дросселя.
  // То есть гард закрывал выдуманный номер и оставлял открытым настоящий: ровно тот
  // класс «обход соседним полем», ради которого он и заводился. Спрашиваем ОБА
  // условия — существование И наличие слота (`IsSchedulableEffectId`).
  const auto& magicSlotInfoMap = GetServerInstance().GetMagicRegistry().GetSlotInfoMap();
  const bool effectIdRegistered = std::ranges::any_of(
    magicSlotInfoMap,
    [&command](const auto& entry) { return entry.second.skillEffectId == command.effectId; });

  if (not effectIdRegistered || not IsSchedulableEffectId(command.effectId))
  {
    uint64_t suppressed = 0;
    if (_skillEffectIdThrottle.Allow(suppressed))
      server::util::QuietLogWarn(
        "Racer {} activated unknown skill effect id {} (registered: {}, suppressed {})",
        targetRacer.oid,
        command.effectId,
        effectIdRegistered,
        suppressed);
    return;
  }

  auto magicSlotInfo = GetServerInstance().GetMagicRegistry().GetSlotInfoByEffectId(command.effectId);

  // LOA-fix (R71-20, находка ревью 2 #1): ОТЧЁТ ССЫЛАЕТСЯ НА КАСТ, КОТОРЫЙ СЕРВЕР
  // ДЕЙСТВИТЕЛЬНО ОБСЛУЖИЛ.
  //
  // ★ЧТО БЫЛО ОТКРЫТО. Проверка выданности стояла ТОЛЬКО внутри ветки ледяной стены,
  // а всякий другой зарегистрированный эффект ехал прямо в `ScheduleSkillEffect`.
  // Пакет `targetOid = свой, attackerOid = свой, effectId = 2, effectInstanceId =
  // 0xBEEF`, посланный до единого каста, проходил гарды R71-4 и R71-5 (цель своя, id
  // в реестре и планируемый) — и сервер выдавал водяной щит. То есть гард был
  // СПИСКОМ МЕСТ (одна ветка), а не правилом; ровно тот класс дефекта, ради которого
  // затеян раунд.
  //
  // ★ПРАВИЛО ОДНО И ТОТАЛЬНОЕ: экземпляр эффекта существует ровно тогда, когда его
  // выдал сервер (реестр наполняется в единственном месте рождения — при касте), он
  // ТОГО ЖЕ ТИПА, что называет отчёт, и его кастовал ТОТ, кого отчёт называет
  // атакующим. Ни одно из трёх условий клиент подделать не может: номер выдаёт
  // сервер, тип и кастера он записывает сам.
  //
  // ★ТИП СВЕРЯЕТСЯ ДО КРИТ-ПОДМЕНЫ ПО DARKFIRE — иначе гард бил бы по честной игре:
  // подмену 2->3 и 18->19 делает СЕРВЕР строкой ниже, глядя на эффекты ЦЕЛИ, а
  // клиент присылает базовый `effectId`. Сравнение с уже подменённым типом отвергало
  // бы каждое честное попадание по игроку под тёмным огнём.
  //
  // ★`attackerOid` БОТА — ЗАКОННЫЙ ВХОД. Пакеты `UseMagicItem` за бота отброшены ещё
  // R57-5, значит и записи о его касте в реестре нет по построению; гард на такой
  // отчёт означал бы, что магия ботов перестала действовать на человека — поломка,
  // которую поймало ревью R57-12. Боты живут только в соло-заезде.
  //
  // ★ДРАКОН (`basicType` 16) ПЕРЕДАЁТСЯ ИЗ РУК В РУКИ, И «КАСТЕР» В ОТЧЁТЕ УЖЕ НЕ ТОТ.
  // `HandleChangeMagicTarget` переписывает `pendingMagicTarget` полем `casterOid` ИЗ
  // ПАКЕТА передающего, поэтому взрыв дракона у последнего держателя честно называет
  // атакующим не первого кастера. Сверяем то, что сервер про дракона действительно
  // знает: цель держит наведение ИМЕННО С ЭТИМ номером экземпляра. Номер и тип при
  // этом всё равно обязаны быть выданными сервером — послабление касается только
  // личности кастера.
  //
  // ★ЧЕГО ЭТО НЕ ЛОВИТ, СКАЗАНО ПРЯМО: гонщик, которого магия ДЕЙСТВИТЕЛЬНО не
  // задела, всё ещё может объявить попадание по себе — геометрии трасс у сервера нет
  // (RaceTracker.hpp:49-52), проверить контакт нечем. Закрыты выдуманный номер, чужой
  // тип, чужой кастер и отчёт до всякого каста.
  //
  // ★ПОБОЧНЫЙ ЭФФЕКТ, НАЗВАННЫЙ ВСЛУХ: у Booster'а обычный и критический варианты
  // делят один `skillEffectId` (5) в `magic.yaml`, поэтому отчёт о КРИТИЧЕСКОМ
  // бустере разрешается в обычный тип и гардом отвергается. Игру это не меняет:
  // эффекты 4-9 и 20-25 сервер вешает САМ в момент каста (`HandleUseMagicItem`,
  // switch по типу), отчёт клиента для них — повтор. Все типы, для которых отчёт
  // ЕДИНСТВЕННЫЙ путь применения (2/3, 12/13, 14/15, 16/17, 18/19), имеют различимые
  // идентификаторы эффектов.
  const uint16_t reportedMagicType = magicSlotInfo.type;
  const bool attackerIsAiRacer = raceInstance.IsAiRacerOid(command.attackerOid);

  if (not attackerIsAiRacer)
  {
    const auto* effectInstance = raceInstance.GetTracker().FindEffectInstance(
      command.effectInstanceId);
    const bool holdsThisDragon = magicSlotInfo.basicType == 16
      && targetRacer.pendingMagicTarget.has_value()
      && targetRacer.pendingMagicTarget->effectInstanceId == command.effectInstanceId;
    const bool instanceAuthorized = effectInstance != nullptr
      && effectInstance->magicType == reportedMagicType
      && (effectInstance->casterOid == command.attackerOid || holdsThisDragon);

    if (not instanceAuthorized)
    {
      uint64_t suppressed = 0;
      if (_effectInstanceThrottle.Allow(suppressed))
        server::util::QuietLogWarn(
          "Racer {} reported effect instance {} the server never issued for magic {} "
          "by racer {} (issued: {}, suppressed {})",
          targetRacer.oid,
          command.effectInstanceId,
          reportedMagicType,
          command.attackerOid,
          effectInstance != nullptr,
          suppressed);
      return;
    }
  }

  // If the target has a DarkFire effect active and the magic crits by dark fire, use the critical type instead
  if ((targetRacer.effects[12] || targetRacer.effects[13]) && magicSlotInfo.criticalByDarkFire)
  {
    magicSlotInfo = GetServerInstance().GetMagicRegistry().GetSlotInfo(magicSlotInfo.criticalType);
  }

  // only send the magic expire for icewall. other magic cant do anything with it.
  if (magicSlotInfo.type == 10 || magicSlotInfo.type == 11)
  {
    // LOA-fix (R71-17, находка ревью 2 #2): СЛОМ ОДНОРАЗОВЫЙ.
    //
    // Проверку «стена выдана сервером, того же типа, от названного кастера» делает
    // общее правило выше — отдельного гарда для стены больше нет и быть не должно:
    // два одинаковых правила рядом расходятся при первой же правке. Здесь остаётся
    // то, что есть ТОЛЬКО у стены, — ПОТРЕБЛЕНИЕ: запись снимается, поэтому повтор
    // того же номера (дубль пакета или попытка) тихо отбрасывается уже общим
    // правилом, и `AcCmdRCMagicExpire` не размножается.
    //
    // ★ЧЕГО ЭТО НЕ ЛОВИТ: гонщик, который стену ВИДИТ, но не касался, всё ещё может
    // объявить слом — геометрии трасс у сервера нет.
    //
    // ★СТЕНА БОТА снимать нечего: её каст в реестр не попадал (R57-5), и общее
    // правило бота не судит.
    if (not attackerIsAiRacer)
      raceInstance.GetTracker().RemoveEffectInstance(command.effectInstanceId);

    const auto magicExpire = protocol::AcCmdRCMagicExpire{
      .magicType = magicSlotInfo.type,
      .firstObstacleInstanceId = command.effectInstanceId,
      .obstacleInstanceCount = 1,
      .breakdown = 1};
    this->Broadcast(raceInstance, magicExpire);
  }

  EffectVerdict verdict = this->ScheduleSkillEffect(raceInstance, command.attackerOid, command.targetOid, magicSlotInfo, command.effectInstanceId);

  if (verdict == EffectVerdict::Applied && magicSlotInfo.attackRank > 1 && targetRacer.pendingMagicTarget)
  {
    const protocol::AcCmdRCRemoveMagicTarget removeMagicTarget{
      .effectInstanceId = targetRacer.pendingMagicTarget->effectInstanceId,
      .casterOid = targetRacer.pendingMagicTarget->casterOid,
      .targetOid = command.targetOid,
      .targetOid2 = command.targetOid};
    this->Broadcast(raceInstance, removeMagicTarget);
  }

  // TODO:: Add a Conditional for the SystemContent that can enable/disable this behavior
  if (verdict == EffectVerdict::Applied && magicSlotInfo.removeMagic == 1 && targetRacer.magicItem.has_value())
  {
    protocol::AcCmdCRUseItemSlotOK response{
      .magicItemId = 0,
      .characterOid = command.targetOid};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });

    const protocol::AcCmdCRUseItemSlotNotify notify{
      .magicItemId = 0,
      .characterOid = command.targetOid,
      .unk = 1};
    this->Broadcast(raceInstance, notify);

    targetRacer.magicItem.reset();
  }

  if (magicSlotInfo.basicType == 16)
    targetRacer.pendingMagicTarget.reset();
}

void RaceNetworkHandler::HandleOpCmd(
  ClientId clientId,
  const protocol::AcCmdCROpCmd& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::vector<std::string> feedback;

  const auto chatVerdict = GetServerInstance().GetChatSystem().ProcessChatMessage(
    clientContext.characterUid, "//" + command.command);

  // LOA-fix (R55-5, round55, backlog #179 часть 5): см. R55-3.
  if (not chatVerdict)
    return;

  const auto& result = *chatVerdict;

  if (not result.commandVerdict)
  {
    return;
  }

  for (const auto& response : result.commandVerdict->result)
  {
    _commandServer.QueueCommand<protocol::RanchCommandOpCmdOK>(
      clientId,
      [response = std::move(response)]()
      {
        return protocol::RanchCommandOpCmdOK{
          .feedback = response,
          .observerState = protocol::RanchCommandOpCmdOK::Observer::Disabled};
      });
  }
}

void RaceNetworkHandler::HandleChangeSkillCardPresetId(
  const ClientId clientId,
  const protocol::AcCmdCRChangeSkillCardPresetID& command)
{
  // LOA-fix (R11-9, round11, backlog #21): диапазон 0..1, а не 0..2.
  // Наборов скиллов у персонажа ФИЗИЧЕСКИ два (skills.<mode>.set1 и .set2), но
  // приём пропускал ещё и setId = 2 и сохранял его в activeSetId. На старте
  // заезда это значение уводило сборку AcCmdCRStartRaceNotify в throw, и notify
  // не получал никто из тех, кто стоял в обходе после виновника — вся комната
  // висла на загрузке. Отбиваем на входе, там где это дёшево и без последствий.
  if (command.setId > 1)
  {
    server::util::QuietLogWarn(
      "AcCmdCRChangeSkillCardPresetID: client {} requested out-of-range skill "
      "preset {}; ignoring (valid range is 0..1)",
      clientId,
      command.setId);
    return;
  }

  if (command.gamemode != protocol::GameMode::Speed && command.gamemode != protocol::GameMode::Magic)
  {
    // TODO: throw? return?
    // Gamemode can either be speed (1) or magic (2)
    return;
  }

  const auto& clientContext = GetClientContext(clientId);
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&command](data::Character& character)
    {
      // Get skill sets by gamemode
      auto& skillSets =
        command.gamemode == protocol::GameMode::Speed ? character.skills.speed() :
        command.gamemode == protocol::GameMode::Magic ? character.skills.magic() :
        throw std::runtime_error("Invalid gamemode");
      // Set character's active skill set in the record
      skillSets.activeSetId = command.setId;
    }
  );

  // No response command
}

void RaceNetworkHandler::RemoveEffect(
  RaceInstance& raceInstance,
  tracker::RaceTracker::Racer& racer,
  uint32_t effectId)
{
  if (effectId >= tracker::RaceTracker::Racer::EffectCount)
  {
    server::util::QuietLogError("RemoveEffect: effectId {} out of range", effectId);
    return;
  }
  racer.effects[effectId] = false;
  ++racer.effectGenerations[effectId];

  const protocol::AcCmdRCRemoveSkillEffect removeSkillEffect{
    .characterOid = racer.oid,
    .effectId = effectId,
    .targetOid = racer.oid,
    .unk1 = 0};
  this->Broadcast(raceInstance, removeSkillEffect);
}

uint32_t RaceNetworkHandler::ComputeEffectDurationMs(
  const registry::Magic::SlotInfo& magicSlotInfo,
  tracker::Oid attackerOid,
  const tracker::RaceTracker::Racer& targetRacer,
  const tracker::RaceTracker::RacerObjectMap& racers) const
{
  uint32_t effectDurationMs = static_cast<uint32_t>(magicSlotInfo.effectDelay * 1000.0f);

  const auto* scaling = _serverInstance.GetMagicRegistry().GetStatScaling(
    magicSlotInfo.basicType);
  if (scaling == nullptr)
    return effectDurationMs;

  // Caster-side bonus, capped at +115% to prevent runaway durations on high stats.
  if (scaling->durationScaleBp > 0)
  {
    const auto attackerRacerIter = std::ranges::find_if(
      racers, [attackerOid](const auto& pair) { return pair.second.oid == attackerOid; });

    if (attackerRacerIter != racers.cend())
    {
      const uint32_t statValue = race::MagicSystem::GetMountStatValue(
        attackerRacerIter->second.mountStats,
        scaling->stat);

      constexpr uint32_t MaxDurationBonusBp = 1150;
      const uint32_t bonusBp = std::min(
        scaling->durationScaleBp * statValue,
        MaxDurationBonusBp);

      effectDurationMs = effectDurationMs * (1000u + bonusBp) / 1000u;
    }
  }

  // Target-side reduction (e.g. IceWall shock mitigation), clamped to 100%.
  if (scaling->targetDurationReductionBp > 0)
  {
    const uint32_t statValue = race::MagicSystem::GetMountStatValue(
      targetRacer.mountStats,
      scaling->stat);

    const uint32_t reductionBp = std::min<uint32_t>(
      scaling->targetDurationReductionBp * statValue, 1000u);

    effectDurationMs = effectDurationMs * (1000u - reductionBp) / 1000u;
  }

  return effectDurationMs;
}

RaceNetworkHandler::EffectVerdict RaceNetworkHandler::ScheduleSkillEffect(
  RaceInstance& raceInstance,
  tracker::Oid attackerOid, tracker::Oid targetOid,
  const registry::Magic::SlotInfo& magicSlotInfo,
  const uint16_t effectInstanceId)
{
  auto& racers = raceInstance.GetTracker().GetRacers();
  const auto targetRacerIter = std::ranges::find_if(
    racers, [targetOid](const auto& pair) { return pair.second.oid == targetOid; });

  // Target racer not found
  if (targetRacerIter == racers.cend())
    return EffectVerdict::Failed;

  // Guard against misconfigured skillEffectId crashing the server
  //
  // LOA-fix (R71-12, находка ревью 2 #1): ЖАЛОБА ЗАДРОССЕЛЕНА, ПОТОМУ ЧТО ЕЁ УМЕЕТ
  // ЗАКАЗАТЬ КЛИЕНТ. Гард R71-5 закрывает клиентский путь `HandleActivateSkillEffect`,
  // но сюда ведут и серверные вызовы (`HandleUseMagicItem`, крит-подмена по DarkFire
  // ниже по функции), а величина берётся из КОНФИГА — то есть строка остаётся
  // достижимой без единой правки кода, стоит появиться ещё одной записи вроде
  // `skillEffectId: 99999`. Дроссель ставится на месте самой жалобы: так она не
  // зависит от того, какой из путей до неё дошёл.
  if (not IsSchedulableEffectId(magicSlotInfo.skillEffectId))
  {
    uint64_t suppressed = 0;
    if (_scheduleEffectRangeThrottle.Allow(suppressed))
      server::util::QuietLogError(
        "ScheduleSkillEffect: skillEffectId {} out of range (max {}, suppressed {})",
        magicSlotInfo.skillEffectId,
        tracker::RaceTracker::Racer::EffectCount - 1,
        suppressed);
    return EffectVerdict::Failed;
  }

  const data::Uid targetCharacterUid = targetRacerIter->first;
  auto& targetRacer = targetRacerIter->second;

  const bool isAttack = magicSlotInfo.attackValue > 0;

  // Shield check: effectId 2 = WaterShield Normal (threshold 100), effectId 3 = WaterShield Critical (threshold 200)
  const uint32_t shieldThreshold =
    targetRacer.effects[3] ? 200u :
    targetRacer.effects[2] ? 100u : 0u;
  const bool shieldBlocks = isAttack && magicSlotInfo.attackValue < shieldThreshold;

  // Any removeHotRodding attack is considered part of the lightning family.
  // For the current registry, critical variants have criticalType == 0.
  const bool isLightning = isAttack && magicSlotInfo.removeHotRodding;
  const bool isCritLightning = isLightning && magicSlotInfo.criticalType == 0;

  // Normal hotrodding (effectId 6): blocked by non-lightning attacks, canceled by any lightning.
  // Crit hotrodding (effectId 7): blocks everything except crit lightning.
  const bool hotroddingBlocks =
    (targetRacer.effects[6] && isAttack && !isLightning) ||
    (targetRacer.effects[7] && isAttack && !isCritLightning);

  const uint32_t effectId = shieldBlocks
    ? (targetRacer.effects[3] ? 3u : 2u)
    : magicSlotInfo.skillEffectId;

  // For removeMagic attacks: blocked if an equal-or-higher-rank attack is already active.
  // For other attacks (attackValue > 0): blocked if the same effect is already active.
  // For pure buffs: blocked only if already active and not replaceable (replaceEffect == 0 means no extension).
  // Duplication is checked against the basic-type effect slot so crit variants share occupancy with their base,
  // except for replaceEffect spells which track their own slot independently.
  // Attacks with rank < 2 are blocked if a rank-2+ attack (fireball/lightning) is already active.
  const uint32_t checkEffectId = magicSlotInfo.replaceEffect
    ? magicSlotInfo.skillEffectId
    : GetServerInstance().GetMagicRegistry().GetSlotInfo(magicSlotInfo.basicType).skillEffectId;
  // LOA-fix (R71-12): `checkEffectId` приходит из ДРУГОЙ записи реестра (basicType), и
  // проверенный выше `magicSlotInfo.skillEffectId` про неё ничего не говорит. Слота
  // нет — значит эффект не занят: индексировать нечего, а `effects[99999]` было бы
  // чтением за границами массива.
  const bool checkEffectActive =
    IsSchedulableEffectId(checkEffectId) && targetRacer.effects[checkEffectId];
  const bool isDuplicated = hotroddingBlocks
    || (isAttack && magicSlotInfo.attackRank < 2 && targetRacer.attackRank >= 2)
    || (magicSlotInfo.attackRank > 0
      ? targetRacer.attackRank >= magicSlotInfo.attackRank
      : checkEffectActive && (isAttack || !magicSlotInfo.replaceEffect));

  const uint32_t effectDurationMs = ComputeEffectDurationMs(
    magicSlotInfo, attackerOid, targetRacer, racers);

  // TODO: Verify if characterOid and targetOid should be the same once we have NPCs
  const protocol::AcCmdRCAddSkillEffect addSkillEffect{
    .characterOid = targetOid,
    .effectId = effectId,
    .targetOid = targetOid,
    .attackerOid = attackerOid,
    .unk2 = effectInstanceId,
    .unk3 = isDuplicated ? 1u : 0u,
    .shieldEffect = protocol::AcCmdRCAddSkillEffect::ShieldEffect{
      .unk0 = shieldBlocks ? 2u : 0u,
      .unk1 = 0,
    },
    .boostEffectMs = effectDurationMs,
  };

  // Broadcast
  this->Broadcast(raceInstance, addSkillEffect);

  if (shieldBlocks)
    return EffectVerdict::Shielded;

  if (isDuplicated)
    return EffectVerdict::Duplicated;

  targetRacer.effects[effectId] = true;
  const uint32_t generation = ++targetRacer.effectGenerations[effectId];
  if (magicSlotInfo.attackRank > 0)
    targetRacer.attackRank = magicSlotInfo.attackRank;

  // Cancel any active adjustMotionSpeed buffs only when a removeMagic attack lands.
  // HotRodding (effectIds 6 and 7), crit chance buffs (18 and 19), and BufGauge buffs (20 and 21) are excluded.
  if (isAttack && magicSlotInfo.removeMagic)
  {
    for (const auto& [type, slot] : GetServerInstance().GetMagicRegistry().GetSlotInfoMap())
    {
      // LOA-fix (R71-12): `IsSchedulableEffectId` СТОИТ ПЕРЕД ИНДЕКСОМ, а не после —
      // порядок здесь и есть защита: `effects[]` индексируется числом из конфига.
      if (slot.adjustMotionSpeed && slot.attackValue == 0
        && slot.skillEffectId != 6 && slot.skillEffectId != 7
        && slot.skillEffectId != 18 && slot.skillEffectId != 19
        && slot.skillEffectId != 20 && slot.skillEffectId != 21
        && IsSchedulableEffectId(slot.skillEffectId)
        && targetRacer.effects[slot.skillEffectId])
      {
        RemoveEffect(raceInstance, targetRacer, slot.skillEffectId);
      }
    }
  }

  _scheduler.Queue(
    [this, roomUid = raceInstance.GetRoomUid(), targetOid, targetCharacterUid, effectId,
      attackRank = magicSlotInfo.attackRank, generation,
      clearMagicTarget = magicSlotInfo.attackRank > 1]()
    {
      std::scoped_lock raceInstanceLock(_raceInstancesMutex);
      const auto raceInstanceIter = _raceInstances.find(roomUid);
      if (raceInstanceIter == _raceInstances.cend())
        return;

      auto& raceInstance = raceInstanceIter->second;

      if (!raceInstance.GetTracker().IsRacer(targetCharacterUid))
        return;

      auto& racer = raceInstance.GetTracker().GetRacer(targetCharacterUid);

      // If the generation has changed, this effect was extended — skip the removal
      if (racer.effectGenerations[effectId] != generation)
        return;

      racer.effects[effectId] = false;
      // Only clear attackRank if a higher-rank attack hasn't replaced this one
      if (attackRank > 0 && racer.attackRank == attackRank)
        racer.attackRank = 0;
      if (clearMagicTarget)
        racer.pendingMagicTarget.reset();

      const protocol::AcCmdRCRemoveSkillEffect removeSkillEffect{
        .characterOid = targetOid,
        .effectId = effectId,
        .targetOid = targetOid,
        .unk1 = 0,
      };
      this->Broadcast(raceInstance, removeSkillEffect);
    },
    Scheduler::Clock::now() + std::chrono::milliseconds(effectDurationMs));
  return EffectVerdict::Applied;
}

void RaceNetworkHandler::HandleInviteUser(
  ClientId clientId,
  const protocol::AcCmdCRInviteUser& command)
{
  const auto& clientContext = GetClientContext(clientId);

  protocol::AcCmdCRInviteUserCancel cancel{};
  cancel.recipientCharacterUid = command.recipientCharacterUid;
  cancel.recipientCharacterName = command.recipientCharacterName;

  // Check if character by that uid is online
  const auto clientOpt = GetServerInstance().GetMessengerDirector().GetClientByCharacterUid(
    command.recipientCharacterUid);
  if (not clientOpt.has_value())
  {
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Check if there's a name mismatch
  // TODO: this could benefit from caching the character name within the messenger client context
  bool isNameMatch{false};
  GetServerInstance().GetDataDirector().GetCharacter(command.recipientCharacterUid).Immutable(
    [&isNameMatch, recipientCharacterName = command.recipientCharacterName](const data::Character& character)
    {
      isNameMatch = character.name() == recipientCharacterName;
    });

  if (not isNameMatch)
  {
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Race director invites are generally more relaxed, you can invite characters that are in
  // either a ranch or race waiting room

  // Sanity check if character can be invited (is away, online or in waiting room)
  const auto& recipientStatus = clientOpt.value().clientContext.presence.status;
  bool canInvite = recipientStatus == protocol::Status::Away or
    recipientStatus == protocol::Status::Online or
    recipientStatus == protocol::Status::WaitingRoom;

  if (not canInvite)
  {
    // Cannot invite character
    server::util::QuietLogWarn("Character '{}', which is in a race waiting room, tried to invite character '{}' who is not in an invitable state",
      clientContext.characterUid,
      command.recipientCharacterUid);
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  protocol::AcCmdCRInviteUserOK response{};
  response.recipientCharacterUid = command.recipientCharacterUid;
  response.recipientCharacterName = command.recipientCharacterName;

  _commandServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void RaceNetworkHandler::HandleRequestUser(
  const ClientId clientId,
  const protocol::AcCmdCRRequestUser& command)
{
  const auto& clientContext = GetClientContext(clientId);

  const auto& invokerCharacterUid = clientContext.characterUid;

  const auto invokerRecord = _serverInstance.GetDataDirector().GetCharacter(invokerCharacterUid);
  if (not invokerRecord)
    return;

  bool isAdmin = false;
  std::string invokerCharacterName{};
  invokerRecord.Immutable([&isAdmin, &invokerCharacterName](const data::Character& character)
    {

      isAdmin = character.role() != data::Character::Role::User;
      invokerCharacterName = character.name();
    });
  const auto& userName = clientContext.userName;

  if (not isAdmin)
  {
    server::util::QuietLogWarn("User '{}'('{}'), which is not an admin, tried to summon character '{}'",
      userName,
      invokerCharacterName,
      command.characterName);
    return;
  }

  protocol::AcCmdCRRequestUserCancel cancel{};
  cancel.force= command.force;
  cancel.characterName = command.characterName;
  cancel.roomUid = command.roomUid;
  cancel.ranchUid = command.ranchUid;

  const data::Uid characterUid = GetServerInstance()
    .GetDataDirector()
    .GetDataSource()
    .RetrieveCharacterUidByName(command.characterName);

  if (characterUid == data::InvalidUid)
  {
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  try
  {
    const auto clientOpt = GetServerInstance()
      .GetLobbyDirector().GetUserByCharacterUid(characterUid);
  }
  catch (const std::exception&)
  {
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  GetServerInstance().GetRaceDirector().NotifySummonCharacter(
    characterUid,
    command.force,
    command.characterName,
    command.roomUid,
    command.ranchUid);

  GetServerInstance().GetRanchDirector().SummonCharacter(
    characterUid,
    command.force,
    command.characterName,
    command.roomUid,
    command.ranchUid);

  protocol::AcCmdCRRequestUserOK response{
    {
      .force= command.force,
      .characterName = command.characterName,
      .roomUid = command.roomUid,
      .ranchUid = command.ranchUid,}};


  _commandServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void RaceNetworkHandler::HandleKickUser(
  ClientId clientId,
  const protocol::AcCmdCRKick& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::unique_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext, false);

  std::string kickerCharacterName;
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&kickerCharacterName](const data::Character& character)
    {
      kickerCharacterName = character.name();
    });

  std::string targetCharacterName;
  _serverInstance.GetDataDirector().GetCharacter(command.characterUid).Immutable(
    [&targetCharacterName](const data::Character& character)
    {
      targetCharacterName = character.name();
    });

  const auto& kickerUserName = clientContext.userName;
  const auto targetUserName = GetClientContextByCharacterUid(command.characterUid).userName;

  // Only the room master may kick players.
  data::Uid roomMasterUid{data::InvalidUid};
  raceInstance.GetRoom([&roomMasterUid](Room& room)
  {
    roomMasterUid = room.GetRoomDetails().masterUid;
  });

  if (clientContext.characterUid != roomMasterUid)
  {
    server::util::QuietLogWarn(
      "Player {} ({}) tried to kick Player {} ({}) but is not the room master.",
      kickerUserName,
      kickerCharacterName,
      targetUserName,
      targetCharacterName);
    return;
  }

  // Prevent self-kick.
  if (command.characterUid == clientContext.characterUid)
  {
    server::util::QuietLogWarn(
      "Player {} ({}) tried to kick themselves.",
      kickerUserName,
      kickerCharacterName);
    return;
  }

  // Verify the target character is actually in this room.
  bool isTargetInRoom{false};
  raceInstance.GetRoom(
    [&isTargetInRoom, targetCharacterUid = command.characterUid](const Room& room)
    {
      isTargetInRoom = room.GetPlayers().contains(targetCharacterUid);
    });

  if (!isTargetInRoom)
  {
    server::util::QuietLogWarn(
      "Player {} ({}) tried to kick Player {} ({}) who is not in the room.",
      kickerUserName,
      kickerCharacterName,
      targetUserName,
      targetCharacterName);
    return;
  }

  // GameMasters (role 2) cannot be kicked.
  bool targetIsGameMaster = false;
  _serverInstance.GetDataDirector().GetCharacter(command.characterUid).Immutable(
    [&targetIsGameMaster](const data::Character& character)
    {
      targetIsGameMaster = character.role() == data::Character::Role::GameMaster;
    });

  if (targetIsGameMaster)
  {
    server::util::QuietLogInfo(
      "Player {} ({}) tried to kick Player {} ({}) who is a GameMaster.",
      kickerUserName,
      kickerCharacterName,
      targetUserName,
      targetCharacterName);
    return;
  }

  // Retrieve the clientId of the targeted player (IMPORTANT)
  ClientId targetClientId{};
  try
  {
    targetClientId = GetClientIdByCharacterUid(command.characterUid);
  }
  catch (const std::exception& ex)
  {
    server::util::QuietLogWarn(
      "Player {} ({}) tried to kick Player {} ({}) but no active client was found: {}",
      kickerUserName,
      kickerCharacterName,
      targetUserName,
      targetCharacterName,
      ex.what());
    return;
  }

  server::util::QuietLogInfo(
    "Player {} ({}) kicked Player {} ({}) from [Room {}].",
    kickerUserName,
    kickerCharacterName,
    targetUserName,
    targetCharacterName,
    clientContext.roomUid);

  // Broadcast the kick notification to all clients in the room.
  const protocol::AcCmdCRKickNotify notify{
    .characterUid = command.characterUid};
  this->Broadcast(raceInstance, notify);

  lock.unlock();
  HandleLeaveRoom(targetClientId);
}

//! Handles team gauge-related logic, including speed and theoretically guild battles.
//! Primary logic reference: `TeamSpurGaugeInfo` in libconfig
void RaceNetworkHandler::HandleTeamGauge(const ClientId clientId)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();

  // If race teammode is not team then we are done here.
  // This is necessary to ensure no team-related logic is handled when spur logic is handled.
  // Sanity check for speed gamemode
  bool isTeamMode = parameters.teamMode == protocol::TeamMode::Team;
  bool isSpeedGameMode = parameters.gameMode == protocol::GameMode::Speed;
  if (not isTeamMode or not isSpeedGameMode)
    return;

  auto& racer = raceInstance.GetTracker().GetRacer(
    clientContext.characterUid);

  auto& blueTeam = raceInstance.GetTracker().blueTeam;
  auto& redTeam = raceInstance.GetTracker().redTeam;
  auto& team =
    racer.team == tracker::RaceTracker::Racer::Team::Red ? redTeam :
    racer.team == tracker::RaceTracker::Racer::Team::Blue ? blueTeam :
    throw std::runtime_error(
      std::format(
        "Racer character uid {} is on unrecognised team {}",
        clientContext.characterUid,
        static_cast<uint32_t>(racer.team)));

  // If the invoker's team gauge is locked (beaten by opposing team's spur), reject gauge fill.
  if (team.gaugeLocked)
    return;

  // Track team boost count for gauge fill rate calculation.
  team.boostCount += 1;

  //! Boost fill rates, scaled with team count, iterated with boost count.
  //! Reference: `TeamSpurGaugeInfo` in libconfig
  // TODO: put this in the config somewhere
  const std::vector<float> baseFillRates{
    1.25f,
    2.50f,
    3.00f,
    3.75f,
    5.50f,
    6.50f};

  // Get team size from the racer tracker (immutable for the race duration).
  // Use the max of the two team sizes to handle potentially unbalanced teams.
  uint32_t redTeamCount = 0;
  uint32_t blueTeamCount = 0;
  for (const auto& _ : raceInstance.GetTracker().GetRacers() | std::views::values)
  {
    if (_.team == tracker::RaceTracker::Racer::Team::Red)
      ++redTeamCount;
    else if (_.team == tracker::RaceTracker::Racer::Team::Blue)
      ++blueTeamCount;
  }
  const auto teamSize = std::max(redTeamCount, blueTeamCount);

  const auto fillRateIndex = std::min(
    team.boostCount,
    static_cast<uint32_t>(baseFillRates.size() - 1));
  protocol::AcCmdRCTeamSpurGauge spur{
    .team = racer.team,
    .markerSpeed = baseFillRates[fillRateIndex] * teamSize, // Base fill rate * boost count * team size
    .unk5 = 0 // TODO: identify use
  };

  //! Base point for a successful boost.
  constexpr uint32_t BaseBoostPoints = 50;
  //! Base point difference per team member in a team.
  constexpr uint32_t BoostPointsDiffBase = 20;

  //! Scale points per boost, based on team size.
  //! Scale = team size - 1 for the formula.
  const auto scale = teamSize - 1;
  //! Final points per boost = base boost + additional boost points.
  const auto additionalBoostPoints = (BoostPointsDiffBase * scale) + (10 * scale);

  //! Base max points.
  constexpr uint32_t BaseMaxPoints = 250;
  //! Max points difference per team member.
  constexpr uint32_t MaxPointsDiffBase = 150;
  //! Final max points for team size.
  const uint32_t maxPoints = BaseMaxPoints + (MaxPointsDiffBase * scale);

  auto& blueTeamPoints = blueTeam.points;
  auto& redTeamPoints = redTeam.points;
  auto& teamPoints =
    racer.team == tracker::RaceTracker::Racer::Team::Red ? redTeamPoints :
    racer.team == tracker::RaceTracker::Racer::Team::Blue ? blueTeamPoints :
    throw std::runtime_error(
      std::format(
        "Racer character uid {} is on unrecognised team {}",
        clientContext.characterUid,
        static_cast<uint32_t>(racer.team)));

  spur.currentPoints = teamPoints / 10.0f;
  teamPoints = std::min(
    maxPoints,
    teamPoints + BaseBoostPoints + additionalBoostPoints);
  spur.newPoints = teamPoints / 10.0f;

  // If any of the teams got max points to spur, reset points and broadcast team spur
  bool isTeamRed = racer.team == tracker::RaceTracker::Racer::Team::Red;
  bool isTeamBlue = racer.team == tracker::RaceTracker::Racer::Team::Blue;

  // Can invoker's team spur
  bool isTeamSpur = false;
  // Check if either red or blue team points have hit max
  if (redTeamPoints >= maxPoints or blueTeamPoints >= maxPoints)
  {
    // If any (red or blue) team can spur.
    // Team check is added for additional validation.
    isTeamSpur = (isTeamRed and redTeamPoints >= maxPoints) or
      (isTeamBlue and blueTeamPoints >= maxPoints);

    // Reset points
    redTeamPoints = 0;
    blueTeamPoints = 0;
  }

  // If any of the teams can spur, schedule a spur/reset event.
  if (isTeamSpur)
  {
    // Reset team boost counters
    redTeam.boostCount = 0;
    blueTeam.boostCount = 0;

    // Lock the spurring team's gauge so it cannot fill during the spur.
    auto& spurringTeamInfo =
      racer.team == tracker::RaceTracker::Racer::Team::Red ? redTeam :
      racer.team == tracker::RaceTracker::Racer::Team::Blue ? blueTeam :
      throw std::runtime_error(
        std::format(
          "Unrecognised racer team '{}'",
          static_cast<uint32_t>(racer.team)));
    spurringTeamInfo.gaugeLocked = true;

    // TODO: put this into the config somewhere
    // When to begin the spur/reset event.
    // Reference: `TeamSpurGaugeInfo`/`ReduceWaitTime` in libconfig
    constexpr auto SpurStartDelay = std::chrono::milliseconds(1500);

    // LOA-fix (R30-1, round30, backlog #128, SECURITY/LIFETIME): захват
    // ПО ЗНАЧЕНИЮ вместо ссылок. БЫЛО `&racer, &spurringTeamInfo` — обе ссылки
    // указывают ВНУТРЬ RaceInstance::_tracker, то есть в узел
    // `_raceInstances` (RaceNetworkHandler.hpp:363). Джоб исполняется через
    // SpurStartDelay=1500 мс на потоке директора; за это окно
    // HandleLeaveRoom (:1770) или refuseRoomEntry (:710) успевают сделать
    // `_raceInstances.erase(roomUid)` — узел уничтожен, ссылки повисли.
    // Проверка существования ниже спасает `raceInstance`, но НЕ `racer`:
    // `Tracker::Clear()` (:1935, HandleStartRace) сносит `_racers` У ЖИВОГО
    // инстанса — find() проходит, а `racer.team` читается из освобождённой
    // памяти (UAF-read). `racer.team` неизменен на всю длительность заезда
    // (RaceTracker.hpp:112, ставится при AddRacer/ChangeTeam до старта),
    // поэтому копия по значению эквивалентна по смыслу и безопасна по
    // времени жизни. `spurringTeamInfo` не захватываем вовсе — внутренний
    // джоб (R30-3) переищет инстанс сам.
    // LOA-fix (R67-6, backlog #128b): ВНЕШНИЙ ДЖОБ ЗАХВАТЫВАЕТ ЭПОХУ ЗАЕЗДА.
    //
    // `roomUid` отвечает на вопрос «та ли это КОМНАТА», и до сих пор только на
    // него и отвечали. Вопрос «тот ли это ЗАЕЗД» не задавался вовсе, а
    // `RaceInstance` комната переиспользует из заезда в заезд. Между
    // планированием и исполнением проходит SpurStartDelay = 1500 мс: за это
    // окно комната успевает финишировать и стартовать заново (`HandleStartRace`
    // → `Tracker::Clear()` → `RaceInstance::Start()`), и джоб ПРОШЛОГО заезда
    // рассылал спур участникам НОВОГО и ставил в очередь разблокировку против
    // чужого трекера.
    // ★Эпоха берётся ПО ЗНАЧЕНИЮ ровно там же и так же, как `roomUid`: копия
    // uint32_t не бросает, то есть захват не умеет «не установиться»
    // ([[obligation-that-can-fail-to-install]]).
    _scheduler.Queue(
      [this,
       roomUid = raceInstance.GetRoomUid(),
       raceEpoch = raceInstance.GetRaceEpoch(),
       spurringTeam = racer.team,
       maxPoints,
       teamSize]()
      {
        std::scoped_lock lock(_raceInstancesMutex);
        const auto raceInstanceIter = _raceInstances.find(roomUid);;
        if (raceInstanceIter == _raceInstances.cend())
          return;

        const auto& raceInstance = raceInstanceIter->second;

        // ★ГАРД ЭПОХИ (R67-6). Комната та же, а заезд уже другой — значит
        // этот спур принадлежит прошлому заезду. Тихий no-op: ни рассылки,
        // ни постановки джоба разблокировки. Проверка стоит ДО всякого
        // побочного действия — после него гасить было бы уже нечего.
        if (raceInstance.GetRaceEpoch() != raceEpoch)
          return;

        const float BaseLoseTeamSpurConsumeRate = -10.0f;
        const float BaseWinTeamSpurConsumeRate = -2.5f;

        // Reset boost gauge for the team that lost it.
        // LOA-fix (R30-2, round30, backlog #128): читаем КОПИЮ `spurringTeam`
        // вместо `racer.team` — сам `racer` больше не захвачен (см. R30-1).
        // Тернарник с throw оставлен ДОСЛОВНО как апстримный стиль; он теперь
        // мёртвый (Solo/None отсекает throw при выборе `spurringTeamInfo`
        // ВЫШЕ, до Queue), но если когда-нибудь оживёт — Scheduler::Tick
        // стирает джоб и перебрасывает, а RaceNetworkHandler::Tick (:288-297)
        // ловит `const std::exception&`. Терминации не будет.
        protocol::AcCmdRCTeamSpurGauge beatenSpur{
          .team =
            // This red/blue swap is intentional, if team A wins, team B is punished and reset.
            spurringTeam == tracker::RaceTracker::Racer::Team::Red ? tracker::RaceTracker::Racer::Team::Blue :
            spurringTeam == tracker::RaceTracker::Racer::Team::Blue ? tracker::RaceTracker::Racer::Team::Red :
            throw std::runtime_error(
              std::format(
                "Unrecognised racer team '{}'",
                static_cast<uint32_t>(spurringTeam))),
          .currentPoints = 0.0f,
          .newPoints = 0.0f,
          .markerSpeed = BaseLoseTeamSpurConsumeRate * teamSize, // Scales with `LoseTeamSpurConsumeRate`
          .unk5 = 3 // Reset gauge and markers.
        };

        // Trigger spur for the team that has won it.
        protocol::AcCmdRCTeamSpurGauge successfulSpur{
          .team = spurringTeam,
          .currentPoints = maxPoints / 10.0f,
          .newPoints = 0.0f,
          .markerSpeed = BaseWinTeamSpurConsumeRate * teamSize, // Scales with `WinTeamSpurConsumeRate`
          .unk5 = 0
        };

        // Spur duration = (maxPoints / 10.0f) / (abs(consumeRate) * teamSize)
        // For example: 25.0f / (2.5f * 1) = 10s for a team of 1.
        const float spurDurationSeconds =
          (maxPoints / 10.0f) / (std::abs(BaseWinTeamSpurConsumeRate) * teamSize);

        // Schedule unlock of the spurring team's gauge after the spur completes.
        // LOA-fix (R30-3, round30, backlog #128, SECURITY/UAF-WRITE): ЯДРО ФИКСА.
        // БЫЛО: `[&spurringTeamInfo]() { spurringTeamInfo.gaugeLocked = false; }`
        // — захват ССЫЛКИ на `RaceInstance::_tracker.redTeam/.blueTeam` с
        // задержкой spurDurationSeconds = (maxPoints/10)/(2.5*teamSize) = 7-10 с,
        // БЕЗ гарда живости и БЕЗ блокировки. Scheduler (Scheduler.hpp) не умеет
        // отменять джобы и не отдаёт хендл — джоб выстрелит В ЛЮБОМ СЛУЧАЕ.
        // Если за эти 7-10 с комната опустела, `_raceInstances.erase(roomUid)`
        // (:710 refuseRoomEntry, :1770 HandleLeaveRoom) уничтожает RaceInstance,
        // и джоб пишет `bool` в освобождённую кучу = USE-AFTER-FREE WRITE.
        // Это не исключение — ни один catch этого не видит, порча памяти тихая.
        // Достижимо в ЧЕСТНОЙ игре: командный Speed-заезд, калибр дошёл до
        // максимума, все вышли в лобби в течение ~10 с.
        // СТАЛО: захват ПО ЗНАЧЕНИЮ (`roomUid`, `spurringTeam`) + переиск
        // инстанса под `_raceInstancesMutex` + гард живости + no-op, если
        // инстанса уже нет. Ровно тот же шаблон, что у джоба истечения
        // эффекта (:4767-4802) и у джоба стены льда (:3997-4018).
        // Побочно закрывается ВТОРОЙ дефект: старый джоб не брал мьютекс
        // вообще, то есть писал `gaugeLocked` с потока директора, пока
        // сетевой поток читает его в HandleTeamGauge (:5111) — гонка данных
        // даже на ЖИВОМ инстансе. Теперь запись под тем же мьютексом.
        _scheduler.Queue(
          [this, roomUid, raceEpoch, spurringTeam]()
          {
            // Fail-closed: команда обязана быть Red или Blue. Сюда мы попадаем
            // только после успешного выбора `spurringTeamInfo` выше (Solo/None
            // там бросает ДО Queue), но джоб не должен полагаться на то, что
            // произошло 1.5 с назад в другом кадре — Solo молча ушёл бы в
            // blueTeam. Бросать здесь нельзя (мы на потоке директора), поэтому
            // тихий no-op.
            if (spurringTeam != tracker::RaceTracker::Racer::Team::Red
              and spurringTeam != tracker::RaceTracker::Racer::Team::Blue)
              return;

            std::scoped_lock unlockLock(_raceInstancesMutex);
            const auto unlockRaceInstanceIter = _raceInstances.find(roomUid);
            if (unlockRaceInstanceIter == _raceInstances.cend())
              return;

            // LOA-fix (R67-7, backlog #128b): ГАРД ЭПОХИ У ДЖОБА
            // РАЗБЛОКИРОВКИ — ИМЕННО ОН ДЕЛАЕТ ВОЗВРАТ R30-4 БЕЗОПАСНЫМ.
            //
            // Задержка здесь spurDurationSeconds = (maxPoints/10)/(2.5*teamSize)
            // = 7-10 с — самое широкое окно во всём обработчике. Заезд за это
            // время успевает кончиться и начаться заново, а `Tracker::Clear()`
            // (R67-5) отдаёт новому заезду СВЕЖИЙ незалоченный калибр. Без
            // этой проверки джоб прошлого заезда снял бы блокировку, которую
            // новый заезд поставил честно, — команда получила бы второй спур
            // подряд.
            // ★Гард внешнего джоба этого НЕ покрывает: между ним и этим
            // остаются те самые 7-10 с, в которые рестарт и попадает чаще
            // всего. Две проверки закрывают два РАЗНЫХ окна, а не одно
            // дважды.
            if (unlockRaceInstanceIter->second.GetRaceEpoch() != raceEpoch)
              return;

            auto& unlockTracker = unlockRaceInstanceIter->second.GetTracker();
            auto& unlockTeamInfo =
              spurringTeam == tracker::RaceTracker::Racer::Team::Red
                ? unlockTracker.redTeam
                : unlockTracker.blueTeam;
            unlockTeamInfo.gaugeLocked = false;
          },
          Scheduler::Clock::now() + std::chrono::milliseconds(
            static_cast<int64_t>(spurDurationSeconds * 1000)));

        // Broadcast losing team's gauge status
        this->Broadcast(raceInstance, beatenSpur);
        // Broadcast winning team's gauge status
        this->Broadcast(raceInstance, successfulSpur);
      },
      Scheduler::Clock::now() + SpurStartDelay);
  }

  // Broadcast invoker's team gauge status
  this->Broadcast(raceInstance, spur);
}

void RaceNetworkHandler::HandleTriggerizeAct(
  ClientId clientId,
  const protocol::AcCmdCRTriggerizeAct& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::scoped_lock lock(_raceInstancesMutex);
  auto& raceInstance = GetRaceInstance(clientContext);
  const auto& parameters = raceInstance.GetParameters();

  const bool isSpeedGameMode = parameters.gameMode == protocol::GameMode::Speed;

  const auto& mapBlockInfo = _serverInstance.GetCourseRegistry()
    .GetMapBlockInfo(
      raceInstance.GetMapBlockId());

  const bool isAdvMap = mapBlockInfo.trainingFee > 0;

  // The racer is neither in a speed mode or adv map
  if (not isSpeedGameMode or not isAdvMap)
  {
    server::util::QuietLogWarn("Character '{}' tried to trigger an interactive object but is not in a speed adv map race.",
      clientContext.characterUid);
    return;
  }

  // TODO: check if the object ID is within range
  // TODO: check if the event ID is valid

  const protocol::AcCmdCRTriggerizeAct response{
    .unk0 = 1, // Setting this to either 1 or 2 satisfies the conditional in the handler
    .unk1 = command.unk1,
    .unk2 = command.unk2};
  this->BroadcastExceptCharacterUid(raceInstance, response, clientContext.characterUid);
}

void RaceNetworkHandler::HandleGameCreateClientItem(
  ClientId clientId,
  const protocol::AcCmdCRGameCreateClientItem& command)
{
  server::util::QuietLogDebug(
    "AcCmdCRGameCreateClientItem: {} {} [{}, {}, {}] [{}, {}, {}, {}]",
    command.someonesOid,
    command.unk1,
    command.position.x, command.position.y, command.position.z,
    command.unk3[0], command.unk3[1], command.unk3[2], command.unk3[3]);

  // LOA-fix (R57-13, round57, backlog #195): «предмет создал бот» — не спавн для
  // человека. `someonesOid` здесь уходит только в лог, а сам спавн привязывается
  // к отправителю; проверки владения нет. Гард стоит ДО всякой жалобы, потому
  // что пакет за бота обязан быть тихим на ЛЮБОЙ ветке этого обработчика.
  if (IsAiRacerOfClientRace(GetClientContext(clientId), command.someonesOid))
    return;

  if (command.unk1 != 0)
  {
    // LOA-fix (#125, SECURITY/AVAILABILITY): unk1 приходит с ПРОВОДА (client-controlled).
    // `throw new` бросал УКАЗАТЕЛЬ → catch(const std::exception&) в диспетчере команд
    // (CommandServer.cpp:363-376) НЕ ловит → std::terminate = REMOTE CRASH
    // (модклиент шлёт unk1 != 0 → сервер падает одним пакетом).
    // Реализован только спавн яиц (unk1==0); прочие случаи ГРАЦИОЗНО игнорируем
    // (R25/R104-прецедент: throw → warn + return), не роняя сервер.
    server::util::QuietLogWarn(
      "HandleGameCreateClientItem: oid {} sent unk1={} (only egg-spawn unk1==0 implemented); ignoring",
      command.someonesOid, command.unk1);
    return;
  }

  const auto& clientContext = GetClientContext(clientId);

  // LOA-fix (R33-1, round33, backlog #125b, SECURITY/CONCURRENCY + DoS).
  // Тело хендлера разбито на ТРИ фазы: дёшево-под-локом → дорого-без-лока →
  // дёшево-под-локом. Подробный разбор «почему именно так» — в шапке раунда в
  // apply_patches.py и в plans/PLAN-125b-createitem-split-critsec.md.
  //
  // КАП = максимум ОДНОВРЕМЕННО живых event-item'ов у гонщика (НЕ «одно яйцо за
  // заезд»): подбор зовёт RemoveEventItem, вектор пустеет, легальный повторный
  // спавн проходит. Проектное значение — сервер предлагает спавн один раз за
  // заезд (HandleLoadingComplete), живой размер вектора проектно 0 или 1.
  constexpr std::size_t MaxEventItemsPerRacer = 1;

  // Снимок идентити ПО ЗНАЧЕНИЮ: дальше фазы 1 никакие ссылки в _raceInstances
  // (инстанс, трекер, гонщик) не переживают снятие мьютекса — только эти скаляры.
  const data::Uid characterUid = clientContext.characterUid;
  const data::Uid roomUid = clientContext.roomUid;

  // ---- ФАЗА 1: дёшево, ПОД _raceInstancesMutex -----------------------------
  // Только дешёвые, room-bounded операции: лукап инстанса, mapBlockId, размер
  // eventItems. ★ТОЧНОСТЬ (NIT ревью round33): это НЕ O(1) — _raceInstances
  // это unordered_map (O(1) в среднем), а IsRacer/GetRacer ходят в
  // RaceTracker::_racers = std::map<data::Uid, Racer>, то есть O(log R) по
  // числу гонщиков комнаты (R ≤ 8). Существенно не «константа», а то, что под
  // этим локом НЕТ ни реестров, ни RNG, ни record-локов, ни I/O.
  registry::MapBlockId mapBlockId{};
  std::size_t heldEventItems = 0;
  bool capHit = false;
  {
    std::scoped_lock lock(_raceInstancesMutex);

    // checkRacer = true по умолчанию → бросает, если персонаж не гонщик.
    // Бросок caught-noisy в диспетчере (std::runtime_error по ЗНАЧЕНИЮ), как и до фикса.
    auto& raceInstance = GetRaceInstance(clientContext);
    mapBlockId = raceInstance.GetMapBlockId();

    const auto& racer = raceInstance.GetTracker().GetRacer(characterUid);
    heldEventItems = racer.eventItems.size();
    capHit = heldEventItems >= MaxEventItemsPerRacer;
  }

  // ★warn СНАРУЖИ критической секции: spdlog делает файловый/консольный I/O, и
  // под директор-глобальным мьютексом это готовый рычаг для флуда.
  if (capHit)
  {
    server::util::QuietLogWarn(
      "HandleGameCreateClientItem: character {} already holds {} event item(s) (cap {}); "
      "ignoring extra client-item spawn",
      characterUid, heldEventItems, MaxEventItemsPerRacer);
    return;
  }

  // ---- ФАЗА 2: дорого, БЕЗ ЛОКА -------------------------------------------
  // Реестры (read-only, грузятся на старте), взвешенный RNG-выбор яйца и
  // record-lock персонажа. Именно этот участок под _raceInstancesMutex и был
  // регрессом по доступности, из-за которого раунд переделан.

  // Get region for this map.
  const auto& mapBlockInfo = _serverInstance.GetCourseRegistry().GetMapBlockInfo(
    mapBlockId);
  const auto regionEggs = _serverInstance.GetPetRegistry().GetEggsByRegion(mapBlockInfo.region);
  if (regionEggs.empty())
    return;

  // Weighted random selection using ObtainRatio (owned eggs still included in weight pool).
  std::vector<uint32_t> weights;
  weights.reserve(regionEggs.size());
  for (const auto& egg : regionEggs)
    weights.push_back(egg.obtainRatio);

  std::discrete_distribution<size_t> dist(weights.begin(), weights.end());
  const auto& selectedEgg = regionEggs[dist(server::util::GetRandomEngine())];

  // Check if the player already owns this egg.
  bool alreadyOwned = false;
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    characterUid);
  characterRecord.Immutable([&](const data::Character& character)
  {
    alreadyOwned = _serverInstance.GetItemSystem().HasItem(character, selectedEgg.tid);
  });

  // If player already owns the egg, do nothing.
  if (alreadyOwned)
    return;

  // Решение фазы 2 — ПО ЗНАЧЕНИЮ (regionEggs/selectedEgg локальны, но в фазу 3
  // сознательно не переносим ни одной ссылки).
  const uint32_t selectedItemType = selectedEgg.deckItemId;
  const auto selectedPosition = command.position;

  // ---- ФАЗА 3: дёшево, СНОВА ПОД _raceInstancesMutex -----------------------
  // Ре-лукап и ре-валидация: за время фазы 2 комната могла закрыться
  // (HandleLeaveRoom стирает из _raceInstances), гонщик — выйти, а параллельный
  // пакет — успеть занять кап. Прецедент ре-лукапа по roomUid — R30-3.
  bool capHitLate = false;
  {
    std::scoped_lock lock(_raceInstancesMutex);

    const auto raceInstanceIter = _raceInstances.find(roomUid);
    if (raceInstanceIter == _raceInstances.cend())
      return;

    auto& tracker = raceInstanceIter->second.GetTracker();
    if (not tracker.IsRacer(characterUid))
      return;

    auto& racer = tracker.GetRacer(characterUid);
    // ★АВТОРИТЕТНАЯ проверка капа (TOCTOU-гард): без неё два одновременных
    // пакета оба прошли бы фазу 1 при size() == 0 и оба сделали emplace_back.
    if (racer.eventItems.size() >= MaxEventItemsPerRacer)
    {
      capHitLate = true;
    }
    else
    {
      // Add to per-racer event item tracker regardless of ownership.
      // ★Запись под тем же мьютексом, под которым директор-тред ИТЕРИРУЕТ
      // racer.eventItems в TickItemSpawners → data-race UAF закрыт.
      auto& item = tracker.AddEventItem(characterUid);
      item.position = selectedPosition;
      item.itemType = selectedItemType;
    }
  }

  // ★Снова: I/O только после снятия лока.
  if (capHitLate)
    server::util::QuietLogWarn(
      "HandleGameCreateClientItem: character {} lost the cap race (cap {}); "
      "ignoring extra client-item spawn",
      characterUid, MaxEventItemsPerRacer);
}

} // namespace server
