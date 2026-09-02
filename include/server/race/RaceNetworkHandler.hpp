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

#ifndef ALICIA_SERVER_RACENETWORKHANDLER_HPP
#define ALICIA_SERVER_RACENETWORKHANDLER_HPP

#include "P2dIdPool.hpp"
#include "RaceInstance.hpp"

#include "server/Config.hpp"

#include "server/system/RoomSystem.hpp"
#include "server/tracker/RaceTracker.hpp"

#include "libserver/registry/MagicRegistry.hpp"
#include "libserver/network/command/CommandServer.hpp"
#include "libserver/network/command/proto/CommonMessageDefinitions.hpp"
#include "libserver/network/command/proto/RaceMessageDefinitions.hpp"
#include "libserver/network/command/proto/RanchMessageDefinitions.hpp"
#include "libserver/util/Scheduler.hpp"
#include "libserver/util/LogThrottle.hpp"
#include "server/race/RelayAuthz.hpp"

#include <random>
#include <unordered_map>

namespace server
{

class ServerInstance;

class RaceNetworkHandler final
  : public CommandServer::EventHandlerInterface
{
public:
  //!
  explicit RaceNetworkHandler(ServerInstance& serverInstance);

  void Initialize();
  void Terminate();
  void Tick();

  void NotifyRoomNameChanged(uint32_t roomUid) noexcept;

  //! Send a RequestUser notification to a character connected to this director.
  void NotifySummonCharacter(
    data::Uid characterUid,
    bool force,
    std::string characterName,
    uint32_t roomUid,
    uint32_t ranchUid) noexcept;

  void SendDailyQuestNotificationToCharacter(
    uint32_t characterUid,
    uint16_t questId,
    const protocol::ObjectiveProgress& objectiveProgress,
    uint32_t carrotsReward,
    protocol::QuestRewardType rewardType,
    uint32_t unk2,
    uint32_t mountExp);

  void HandleClientConnected(ClientId clientId) override;
  void HandleClientDisconnected(ClientId clientId) override;

  void DisconnectCharacter(data::Uid characterUid);

  //! LOA-fix (R12-1, round12, backlog #85): находится ли персонаж ПРЯМО СЕЙЧАС
  //! в стадии загрузки карты заезда. Зовётся из ЛОББИ-потока, поэтому трогает
  //! только `_raceInstances` под `_raceInstancesMutex` и НИКОГДА `_clients`
  //! (та карта принадлежит гоночному потоку и мьютексом не закрыта).
  [[nodiscard]] bool IsCharacterLoadingRace(data::Uid characterUid);

  //! Get room count.
  //! @return Room count.
  [[nodiscard]] size_t GetRoomCount();

  Config::Race& GetConfig();

  ServerInstance& GetServerInstance();
  CommandServer& GetCommandServer();

  template <WritableStruct C>
  void Broadcast(
    const RaceInstance& raceInstance,
    const C& command)
  {
    raceInstance.GetRoom(
      [this, command](const Room& room)
      {
        for (const auto& player : room.GetPlayers() | std::views::values)
          _commandServer.QueueCommand<C>(
            player.GetClientId(),
            [command]()
            {
              return command;
            });
      });
  }

  //! Отправляет команду ОДНОМУ клиенту по УЖЕ ИЗВЕСТНОМУ `ClientId`.
  //!
  //! ★СМЫСЛ МЕТОДА — В ТОМ, ЧЕГО В НЁМ НЕТ: обращения к `_clients`.
  //! `RaceInstance::Stop()` исполняется на потоке ГОНОЧНОГО ДИРЕКТОРА
  //! (`ServerInstance::_raceDirectorThread` → `RaceNetworkHandler::Tick` →
  //! `RaceInstance::Tick` → `TickFinishing` → `Stop`), а `_clients` мутирует
  //! СЕТЕВОЙ поток команд (`CommandServer::BeginHost` заводит собственный
  //! `_serverThread`, с него приходят `HandleClientConnected`/
  //! `HandleClientDisconnected` и все `Handle*`). Значит любой поиск по
  //! `_clients` из `Stop()` — гонка на `unordered_map` с параллельными
  //! `try_emplace`/`erase`, то есть UB, а не «редкая неточность». Первая
  //! редакция достижений R70 звала оттуда `GetClientIdByCharacterUid`, копируя
  //! форму `SendDailyQuestNotificationToCharacter`; ревью (итерация 2) поймало
  //! это как BLOCK, и утверждение «оба на одном потоке» оказалось ложным.
  //! ★ЧЕМ ЗАМЕНЕНО: `ClientId` берётся ИЗ КОМНАТЫ — `RoomSystem::GetRoom`
  //! держит per-room `std::mutex` на всё время колбэка, и ровно этим путём
  //! `Broadcast`/`BroadcastExceptCharacterUid` уже ходят с этого же потока на
  //! каждом пакете заезда. То есть это не новый механизм, а тот, что уже
  //! доказан в проде.
  //! ★УСТАРЕВШИЙ `ClientId` БЕЗОПАСЕН: `CommandServer::SendCommand` идёт в
  //! `Server::GetClient`, а тот ищет под разделяемым замком и БРОСАЕТ на
  //! неизвестном клиенте — здесь этот бросок глушится, потому что «игрок уже
  //! вышел» не должно стоить остальным ни заезда, ни достижений. Прогресс к
  //! этому моменту уже записан в БД, теряется только уведомление.
  template <WritableStruct C>
  void SendToClient(
    const ClientId clientId,
    const C& command) noexcept
  {
    try
    {
      _commandServer.QueueCommand<C>(
        clientId,
        [command]()
        {
          return command;
        });
    }
    catch (const std::exception&)
    {
      // Клиента уже нет — уведомление теряется, состояние нет.
    }
    catch (...)
    {
      // То же самое для не-std броска.
    }
  }

  template <WritableStruct C>
  void BroadcastExceptCharacterUid(
    const RaceInstance& raceInstance,
    const C& command,
    data::Uid skipCharacterUid)
  {
    raceInstance.GetRoom(
      [this, command, skipCharacterUid](const Room& room)
      {
        for (const auto& [characterUid, player] : room.GetPlayers())
        {
          if (characterUid == skipCharacterUid)
            continue;

          _commandServer.QueueCommand<C>(
            player.GetClientId(),
            [command]()
            {
              return command;
            });
        }
      });
  }

private:
  //! LOA-fix (R71-2, backlog #129-S2 / item 26): ПОТОЛОК СПИСКА ЦЕЛЕЙ ОДНОГО КАСТА.
  //!
  //! `AcCmdCRUseMagicItem::targetList` читается счётчиком `uint8_t`
  //! (RaceMessageDefinitions.cpp:1487-1493) — до 255 элементов. У списка два смысла,
  //! и оба малы: цели заклинания (их не больше, чем участников комнаты) и сосульки
  //! ледяной стены (1 у обычной, 3 у критической — комментарий там же,
  //! RaceMessageDefinitions.hpp:1942-1944).
  //!
  //! ★8 — это ТОЧНАЯ вместимость, а не запас с потолка: комната клампится восемью
  //! (`RaceNetworkHandler.cpp:1538`, `constexpr uint8_t MaxRoomPlayerCount = 8`), и
  //! ровно восемь участников в трекере у соло-заезда (1 живой + 7 ботов, :2362-2367
  //! + `SpawnAiRacers`). Поэтому сравнение строгое (`>`), а не `>=`. Число НЕ
  //! зависит от данных `magic.yaml` — конфиг его сдвинуть не может.
  static constexpr size_t MaxMagicTargetListSize = 8;

  enum class EffectVerdict : uint8_t
  {
    Shielded,
    Applied,
    Duplicated,
    Failed
  };

  struct ClientContext
  {
    data::Uid characterUid{data::InvalidUid};
    data::Uid roomUid{data::InvalidUid};
    bool isAuthenticated = false;
    std::string userName;
  };

  //! «Этот object id — бот в заезде ЭТОГО клиента?» — НЕБРОСАЮЩАЯ форма
  //! вопроса (R57, #195).
  //!
  //! Отличается от `GetRaceInstance(...).IsAiRacerOid(...)` ровно тем, что не
  //! бросает, когда клиент вообще не в заезде. Это нужно там, где вопрос задаётся
  //! РАНЬШЕ всех прочих проверок: прямой вызов завёл бы новую строку ошибки на
  //! пути, который сегодня до заезда не ходит.
  //! Отрицательный ответ означает «не бот ЛИБО спросить не у кого» — для
  //! единственного правила раунда («пакет за бота тихо игнорируем») этого
  //! достаточно: в сомнении обработка идёт прежним путём.
  [[nodiscard]] bool IsAiRacerOfClientRace(
    const ClientContext& clientContext,
    tracker::Oid oid) noexcept;

  race::P2dId GetOrCreateP2dId(ClientId clientId);

  //! Заводит AI-соперников для соло-заезда (R56, #61).
  //! Отказ мягкий: если ростер не удалось собрать, заезд едет БЕЗ ботов.
  void SpawnAiRacers(RaceInstance& raceInstance);

  //! Обеспечивает набор из `count` постоянных P2dId для ботов (R56, #61).
  //! @retval `true`  набор готов и лежит в `_aiP2dIds`
  //! @retval `false` пул исчерпан; НИ ОДИН id не изъят
  //!
  //! ★Почему ПОСТОЯННЫЕ, а не «на заезд». Апстрим писал ботам `p2dId = oid`;
  //! у нас так нельзя — пул раздаёт id С НУЛЯ, и бот столкнулся бы с живым
  //! клиентом. Берём нужное количество из ТОГО ЖЕ пула один раз за время жизни
  //! сервера и переиспользуем во всех комнатах: коллизия невозможна, а
  //! возвращать в пул нечего — значит, и утечь при разрушении комнаты нечему.
  //! Боты по ретрансляции не ездят, поэтому совпадение id между комнатами
  //! безвредно.
  //!
  //! ★ВСЁ ИЛИ НИЧЕГО (находка ревью R56-i1). Прошлая версия брала id по одному
  //! и при исчерпании пула на середине оставляла уже взятые у себя навсегда —
  //! ростер всё равно не собирался, а ёмкость для ЖИВЫХ клиентов таяла с
  //! каждой попыткой. Теперь неполный набор возвращается в пул целиком.
  [[nodiscard]] bool AcquireAiP2dIds(size_t count);

  ClientContext& GetClientContext(
    ClientId clientId,
    bool requireAuthorized = true);
  ClientId GetClientIdByCharacterUid(data::Uid characterUid);
  ClientContext& GetClientContextByCharacterUid(data::Uid characterUid);

  RaceInstance& GetRaceInstance(
    const ClientContext& clientContext,
    bool checkRacer = true);

  EffectVerdict ScheduleSkillEffect(
    RaceInstance& raceInstance,
    tracker::Oid attackerId, tracker::Oid targetId,
    const registry::Magic::SlotInfo& magicSlotInfo,
    uint16_t effectInstanceId = 0);

  //! Computes an effect's effective duration in milliseconds, applying any
  //! caster-stat duration bonus and any target-stat duration reduction from
  //! the spell's stat scaling (see magic.yaml statScalings).
  uint32_t ComputeEffectDurationMs(
    const registry::Magic::SlotInfo& magicSlotInfo,
    tracker::Oid attackerOid,
    const tracker::RaceTracker::Racer& targetRacer,
    const tracker::RaceTracker::RacerObjectMap& racers) const;

  void RemoveEffect(
    RaceInstance& raceInstance,
    tracker::RaceTracker::Racer& racer,
    uint32_t effectId);

  void HandleEnterRoom(
    ClientId clientId,
    const protocol::AcCmdCREnterRoom& command);

  void HandleChangeRoomOptions(
    ClientId clientId,
    const protocol::AcCmdCRChangeRoomOptions& command);

  void HandleChangeTeam(
    ClientId clientId,
    const protocol::AcCmdCRChangeTeam& command);

  void HandleLeaveRoom(
    ClientId clientId);

  void HandleReadyRace(
  ClientId clientId,
  const protocol::AcCmdCRReadyRace& command);

  void HandleStartRace(
    ClientId clientId,
    const protocol::AcCmdCRStartRace& command);

  void SendStartRaceCancel(
    ClientId clientId,
    protocol::AcCmdCRStartRaceCancel::Reason reason);

  void HandleRaceTimer(
    ClientId clientId,
    const protocol::AcCmdUserRaceTimer& command);

  void HandleLoadingComplete(
    ClientId clientId,
    const protocol::AcCmdCRLoadingComplete& command);

  void HandleUserRaceFinal(
    ClientId clientId,
    const protocol::AcCmdUserRaceFinal& command);

  void HandleRaceResult(
    ClientId clientId,
    const protocol::AcCmdCRRaceResult& command);

  void HandleP2PRaceResult(
    ClientId clientId,
    const protocol::AcCmdCRP2PResult& command);

  void HandleP2PUserRaceResult(
    ClientId clientId,
    const protocol::AcCmdUserRaceP2PResult& command);

  void HandleAwardStart(
    ClientId clientId,
    const protocol::AcCmdCRAwardStart& command);

  void HandleAwardEnd(
    ClientId clientId,
    const protocol::AcCmdCRAwardEnd& command);

  void HandleStarPointGet(
    ClientId clientId,
    const protocol::AcCmdCRStarPointGet& command);

  //! Обрабатывает шпору. Возвращает «пакет был от самого отправителя»
  //! (R57, #195): для пакета, присланного за AI-соперника, — `false`, и тогда
  //! вызывающий обязан пропустить и хвостовую обработку командной шкалы.
  [[nodiscard]] bool HandleRequestSpur(
    ClientId clientId,
    const protocol::AcCmdCRRequestSpur& command);

  void HandleHurdleClearResult(
    ClientId clientId,
    const protocol::AcCmdCRHurdleClearResult& command);

  void HandleStartingRate(
    ClientId clientId,
    const protocol::AcCmdCRStartingRate& command);

  void HandleRaceUserPos(
    ClientId clientId,
    const protocol::AcCmdUserRaceUpdatePos& command);

  void HandleChat(
    ClientId clientId,
    const protocol::AcCmdCRChat& command);

  void HandleRelayCommand(
    ClientId clientId,
    const protocol::AcCmdCRRelayCommand& command);

  void HandleRelay(
    ClientId clientId,
    const protocol::AcCmdCRRelay& command);

  void HandleUserRaceActivateInteractiveEvent(
    ClientId clientId,
    const protocol::AcCmdUserRaceActivateInteractiveEvent& command);

  void HandleUserRaceActivateEvent(
    ClientId clientId,
    const protocol::AcCmdUserRaceActivateEvent& command);

  void HandleUserRaceDeactivateEvent(
    ClientId clientId,
    const protocol::AcCmdUserRaceDeactivateEvent& command);

  void HandleRequestMagicItem(
    ClientId clientId,
    const protocol::AcCmdCRRequestMagicItem& command);

  void HandleUseMagicItem(
    ClientId clientId,
    const protocol::AcCmdCRUseMagicItem& command);

  void HandleUserRaceItemGet(
    ClientId clientId,
    const protocol::AcCmdUserRaceItemGet& command);

  // Magic Targeting Commands for Bolt System
  void HandleStartMagicTarget(
    ClientId clientId,
    const protocol::AcCmdCRStartMagicTarget& command);

  void HandleChangeMagicTarget(
    ClientId clientId,
    const protocol::AcCmdCRChangeMagicTarget& command);

  void HandleChangeSkillCardPresetId(
    ClientId clientId,
    const protocol::AcCmdCRChangeSkillCardPresetID& command);

  // Note: HandleActivateSkillEffect commented out due to build issues
  void HandleActivateSkillEffect(
    ClientId clientId,
    const protocol::AcCmdCRActivateSkillEffect& command);

  void HandleOpCmd(
    ClientId clientId,
    const protocol::AcCmdCROpCmd& command);

  //! Race clients can invite characters from ranch or other race rooms.
  void HandleInviteUser(
    ClientId clientId,
    const protocol::AcCmdCRInviteUser& command);

  void HandleRequestUser(
    ClientId clientId,
    const protocol::AcCmdCRRequestUser& command);

  void HandleKickUser(
    ClientId clientId,
    const protocol::AcCmdCRKick& command);

  //! Handles the team gauges in team races only.
  void HandleTeamGauge(ClientId clientId);

  void HandleTriggerizeAct(
    ClientId clientId,
    const protocol::AcCmdCRTriggerizeAct& command);

  void HandleGameCreateClientItem(
    ClientId clientId,
    const protocol::AcCmdCRGameCreateClientItem& command);

  //! A scheduler instance.
  Scheduler _scheduler;
  //! A server instance.
  ServerInstance& _serverInstance;
  //! A command server instance.
  CommandServer _commandServer;
  //! A map of all client contexts.
  std::unordered_map<ClientId, ClientContext> _clients;
  //! A map of all p2ds for UDP relay.
  std::unordered_map<ClientId, race::P2dId> _p2dIds;
  //! A pool for active race clients with P2dIds.
  race::P2dIdPool _p2dIdPool;
  //! Постоянные P2dId AI-соперников (R56, #61); см. `GetAiP2dId`.
  std::vector<race::P2dId> _aiP2dIds;

  std::mutex _raceInstancesMutex;
  //! A map of all race instanced indexed by room UIDs.
  std::unordered_map<uint32_t, RaceInstance> _raceInstances;

  //! LOA-fix (R71): по дросселю НА КАЖДУЮ ЗАЩИТУ — флуд одной не должен заглушать
  //! другую (у каждого своё окно и свой счётчик подавленных).
  //!
  //! ★ПОЛЯ ДОБАВЛЕНЫ В КОНЕЦ КЛАССА НАМЕРЕННО. Смещения всех существующих полей
  //! остаются прежними, поэтому код НЕтронутых методов кодируется так же — а значит
  //! «неподвижный контрольный символ» лесенки остаётся честной уликой, а не
  //! случайностью компоновки.
  util::LogThrottle _magicOwnershipThrottle;
  util::LogThrottle _magicTargetCountThrottle;
  util::LogThrottle _magicPayloadThrottle;
  util::LogThrottle _skillTargetThrottle;
  util::LogThrottle _skillEffectIdThrottle;
  util::LogThrottle _relayEnvelopeThrottle;
  util::LogThrottle _relayActorThrottle;
  util::LogThrottle _itemGetOwnershipThrottle;
  util::LogThrottle _relayPayloadTypeThrottle;
  util::LogThrottle _scheduleEffectRangeThrottle;
  util::LogThrottle _itemDeckUnknownThrottle;
  util::LogThrottle _relayReferenceThrottle;
  util::LogThrottle _iceWallInstanceThrottle;
};

} // namespace server

#endif // ALICIA_SERVER_RACENETWORKHANDLER_HPP
