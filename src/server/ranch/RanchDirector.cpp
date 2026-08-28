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

#include "server/ranch/RanchDirector.hpp"

#include "server/ServerInstance.hpp"
#include "server/system/ItemSystem.hpp"

#include <libserver/data/helper/ProtocolHelper.hpp>
#include <libserver/util/Cleanup.hpp>
#include <libserver/util/RecordAccess.hpp>
#include <libserver/util/Locale.hpp>
#include <libserver/util/QuietLog.hpp>
#include <libserver/util/Util.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <ranges>

#include <spdlog/spdlog.h>

namespace server
{

namespace
{

constexpr size_t MaxRanchHorseCount = 10;
constexpr size_t MaxRanchCharacterCount = 20;
constexpr size_t MaxRanchHousingCount = 13;

constexpr int16_t DoubleIncubatorId = 52;
constexpr int16_t SingleIncubatorId = 51;

constexpr uint16_t MaxCharm = 1000;
constexpr uint16_t MaxFriendliness = 1000;
constexpr uint16_t MaxAttachment = 1000;
constexpr uint16_t MaxPlenitude = 1200;

//! The item template ID of the instant grow-up item,
//! which matures a foal into an adult horse.
constexpr data::Tid InstantGrowUpItemTid = 43001;

//! How often the foal maturity sweep runs while players are on their ranch.
constexpr auto FoalMaturityCheckInterval = std::chrono::seconds(60);

//! How many times a deferred ranch entry is retried while waiting for horse
//! records to load before giving up and cancelling the entry.
constexpr uint32_t MaxEnterRanchDeferAttempts = 8;
constexpr uint32_t MaxTryBreedingDeferAttempts = 4;

BreedingMarket::SnapshotOrder ConvertProtocolStallionOrderToSnapshotOrder(
  const protocol::AcCmdCRSearchStallion::StallionOrder order)
{
  switch (order)
  {
    case protocol::AcCmdCRSearchStallion::StallionOrder::LineageDescending:
      return BreedingMarket::SnapshotOrder::LineageDescending;
    case protocol::AcCmdCRSearchStallion::StallionOrder::TimeLeftDescending:
      return BreedingMarket::SnapshotOrder::TimeLeftDescending;
    case protocol::AcCmdCRSearchStallion::StallionOrder::FeeDescending:
      return BreedingMarket::SnapshotOrder::FeeDescending;
    case protocol::AcCmdCRSearchStallion::StallionOrder::PregnancyChanceAscending:
      return BreedingMarket::SnapshotOrder::PregnancyChanceAscending;
    case protocol::AcCmdCRSearchStallion::StallionOrder::PregnancyChanceDescending:
      return BreedingMarket::SnapshotOrder::PregnancyChanceDescending;
    case protocol::AcCmdCRSearchStallion::StallionOrder::FeeAscending:
      return BreedingMarket::SnapshotOrder::FeeAscending;
    case protocol::AcCmdCRSearchStallion::StallionOrder::TimeLeftAscending:
      return BreedingMarket::SnapshotOrder::TimeLeftAscending;
    case protocol::AcCmdCRSearchStallion::StallionOrder::LineageAscending:
      return BreedingMarket::SnapshotOrder::LineageAscending;
    default:
      // todo: what should the default be?
      return BreedingMarket::SnapshotOrder::TimeLeftDescending;
  }
}

BreedingMarket::SnapshotFilter::Stat ConvertProtocolStallionStatToSnapshotStat(
  const protocol::AcCmdCRSearchStallion::Stat stat)
{
  switch (stat)
  {
    case protocol::AcCmdCRSearchStallion::Stat::Agility:
      return BreedingMarket::SnapshotFilter::Stat::Agility;
    case protocol::AcCmdCRSearchStallion::Stat::Ambition:
      return BreedingMarket::SnapshotFilter::Stat::Ambition;
    case protocol::AcCmdCRSearchStallion::Stat::Rush:
      return BreedingMarket::SnapshotFilter::Stat::Rush;
    case protocol::AcCmdCRSearchStallion::Stat::Endurance:
      return BreedingMarket::SnapshotFilter::Stat::Endurance;
    case protocol::AcCmdCRSearchStallion::Stat::Courage:
      return BreedingMarket::SnapshotFilter::Stat::Courage;
    case protocol::AcCmdCRSearchStallion::Stat::None:
    default:
      return BreedingMarket::SnapshotFilter::Stat::None;
  }
}

} // namespace anon

RanchDirector::RanchDirector(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
  , _commandServer(*this)
  , _breedingMarket(serverInstance)
  , _mountFamilyTreeDeferrer([this](const network::ClientId clientId, const protocol::AcCmdCRMountFamilyTree& command)
  {
    HandleMountFamilyTree(clientId, command);
  })
  , _enterRanchDeferrer([this](const network::ClientId clientId, const protocol::AcCmdCREnterRanch& command)
  {
    // The deferrer erases the command after each tick, so re-queue it here to
    // keep retrying until the required horse records have loaded. Give up after
    // a few attempts and cancel the entry so the client isn't stuck forever.
    const bool deferAgain = HandleEnterRanch(clientId, command);

    // LOA-fix (R13-11, round13, backlog #86): см. R13-8. Клиент мог уйти, пока
    // его вход лежал в отсрочке; GetClientContext ниже на пропавшем клиенте
    // кидает прямо в CommandDeferrer::Tick, а тот перебрасывает — гибнет весь
    // тик деферрера вместе с чужими командами. Отвечать некому, повторять
    // нечего: выходим без Defer, и команда уезжает из деферрера сама.
    if (not _clients.contains(clientId))
      return;

    auto& clientContext = GetClientContext(clientId, false);

    if (not deferAgain)
    {
      clientContext.enterRanchDeferAttempts = 0;
      return;
    }

    if (++clientContext.enterRanchDeferAttempts >= MaxEnterRanchDeferAttempts)
    {
      clientContext.enterRanchDeferAttempts = 0;

      server::util::QuietLogWarn(
        "Ranch entry for client {} gave up after {} deferred attempts; horse records unavailable",
        clientId,
        MaxEnterRanchDeferAttempts);

      // LOA-fix (R13-7, round13, backlog #86): ОТКАТ ПРИ ОТКАЗЕ ОТСРОЧКИ.
      // Тот же откат, что в catch R13-4b, только для второго выхода из входа на
      // ранчо.
      //
      // LOA-fix (R13-13, round13, ремонт 3): откат ТОЛЬКО ПО СВОЕМУ ЧЛЕНСТВУ.
      // Снимка из HandleEnterRanch здесь нет — лямбда работает позже и в другой
      // области видимости, — поэтому признак владения берём из самого
      // ClientContext: visitingRancherUid указывает на ЭТО ранчо только если
      // членство в нём завёл ЭТОТ клиент (поле присваивается вплотную к
      // tracker.AddCharacter, R13-9). Иначе снимать нечего, и трогать трекер
      // нельзя: запись там может принадлежать второму соединению того же
      // персонажа, а visitingRancherUid — предыдущему, законному ранчо.
      // ★Инвариант: сегодня это условие на give-up-пути НЕДОСТИЖИМО. Все выходы
      // «return true» в HandleEnterRanch лежат ВЫШЕ присвоения
      // visitingRancherUid и вставки в трекер (R13-1/R13-2c/R13-6/R13-9,
      // последнюю позднюю отсрочку убрал R13-10), а бросок после вставки
      // перехватывает catch R13-4b и возвращает false — до отказа отсрочки
      // дело не доходит. Блок оставлен «подтяжками» на случай, если будущая
      // правка вернёт отсрочку ниже вставки: тогда откат сработает и будет
      // корректно ограничен своим членством.
      if (clientContext.visitingRancherUid == command.rancherUid)
      {
        const auto ranchIter = _ranches.find(command.rancherUid);
        if (ranchIter != _ranches.cend())
        {
          ranchIter->second.tracker.RemoveCharacter(command.characterUid);
          ranchIter->second.clients.erase(clientId);
        }

        clientContext.visitingRancherUid = data::InvalidUid;
      }

      protocol::RanchCommandEnterRanchCancel cancel{};
      _commandServer.QueueCommand<decltype(cancel)>(
        clientId,
        [cancel]()
        {
          return cancel;
        });
      return;
    }

    _enterRanchDeferrer.Defer(clientId, command);
  })
  , _tryBreedingDeferrer([this](const network::ClientId clientId, const protocol::AcCmdCRTryBreeding& command)
  {
    if (HandleTryBreeding(clientId, command))
      _tryBreedingDeferrer.Defer(clientId, command);
  })
{
  _commandServer.RegisterCommandHandler<protocol::AcCmdCREnterRanch>(
    [this](ClientId clientId, const auto& message)
    {
      if (HandleEnterRanch(clientId, message))
        _enterRanchDeferrer.Defer(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRLeaveRanch>(
    [this](ClientId clientId, const auto&)
    {
      HandleRanchLeave(clientId);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRanchChat>(
    [this](ClientId clientId, const auto& command)
    {
      HandleChat(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRanchSnapshot>(
    [this](ClientId clientId, const auto& message)
    {
      HandleSnapshot(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCREnterBreedingMarket>(
    [this](ClientId clientId, auto& command)
    {
      HandleEnterBreedingMarket(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRSearchStallion>(
    [this](ClientId clientId, auto& command)
    {
      HandleSearchStallion(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRegisterStallion>(
    [this](ClientId clientId, auto& command)
    {
      HandleRegisterStallion(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUnregisterStallion>(
    [this](ClientId clientId, auto& command)
    {
      HandleUnregisterStallion(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUnregisterStallionEstimateInfo>(
    [this](ClientId clientId, auto& command)
    {
      HandleUnregisterStallionEstimateInfo(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRCheckStallionCharge>(
    [this](ClientId clientId, auto& command)
    {
      HandleCheckStallionCharge(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRStatusPointApply>(
    [this](ClientId clientId, auto& command)
    {
      HandleStatusPointApply(clientId, command);
    });

  // LOA (batch2): care-skill study/reset handlers.
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRStudyCareSkill>(
    [this](ClientId clientId, auto& command)
    {
      HandleStudyCareSkill(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRResetCareSkill>(
    [this](ClientId clientId, auto& command)
    {
      HandleResetCareSkill(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRTryBreeding>(
    [this](ClientId clientId, auto& command)
    {
      if (HandleTryBreeding(clientId, command))
        _tryBreedingDeferrer.Defer(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBreedingAbandon>(
    [this](ClientId clientId, auto& command)
    {
      HandleBreedingAbandon(clientId, command);
    });

  // AcCmdCLRequestFestivalResult

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBreedingWishlist>(
    [this](ClientId clientId, auto& command)
    {
      HandleBreedingWishlist(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBreedingFailureCard>(
    [this](ClientId clientId, auto& command)
    {
      HandleBreedingFailureCard(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBreedingFailureCardChoose>(
    [this](ClientId clientId, auto& command)
    {
      HandleBreedingFailureCardChoose(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRanchCmdAction>(
    [this](ClientId clientId, const auto& message)
    {
      HandleCmdAction(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandRanchStuff>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRanchStuff(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandUpdateBusyState>(
    [this](ClientId clientId, auto& command)
    {
      HandleUpdateBusyState(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUpdateMountNickname>(
    [this](ClientId clientId, auto& command)
    {
      HandleUpdateMountNickname(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestStorage>(
    [this](ClientId clientId, auto& command)
    {
      HandleRequestStorage(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRGetItemFromStorage>(
    [this](ClientId clientId, auto& command)
    {
      HandleGetItemFromStorage(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRWearEquipment>(
    [this](ClientId clientId, auto& command)
    {
      HandleWearEquipment(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRemoveEquipment>(
    [this](ClientId clientId, auto& command)
    {
      HandleRemoveEquipment(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUseItem>(
    [this](ClientId clientId, auto& command)
    {
      HandleUseItem(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandCreateGuild>(
    [this](ClientId clientId, auto& command)
    {
      HandleCreateGuild(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandRequestGuildInfo>(
    [this](ClientId clientId, auto& command)
    {
      HandleRequestGuildInfo(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUpdatePet>(
    [this](ClientId clientId, auto& command)
    {
      HandleUpdatePet(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandUserPetInfos>(
    [this](ClientId clientId, auto& command)
    {
      HandleUserPetInfos(clientId, command);
    });
  
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRIncubateEgg>(
    [this](ClientId clientId, auto& command)
    {
      HandleIncubateEgg(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBoostIncubateEgg>(
    [this](ClientId clientId, auto& command)
    {
      HandleBoostIncubateEgg(clientId, command);
    });
  
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestPetBirth>(
    [this](ClientId clientId, auto& command)
    {
      HandleRequestPetBirth(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRPetBornResult>(
    [this](ClientId clientId, auto& command)
    {
      HandlePetBornResult(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBoostIncubateInfoList>(
    [this](ClientId clientId, auto& command)
    {
      HandleBoostIncubateInfoList(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandRequestNpcDressList>(
    [this](ClientId clientId, const auto& message)
    {
      HandleRequestNpcDressList(clientId, message);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRHousingBuild>(
    [this](ClientId clientId, auto& command)
    {
      HandleHousingBuild(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRHousingRepair>(
    [this](ClientId clientId, auto& command)
    {
      HandleHousingRepair(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdRCMissionEvent>(
    [this](ClientId clientId, auto& command)
    {
      protocol::AcCmdRCMissionEvent event
      {
        .event = protocol::AcCmdRCMissionEvent::Event::EVENT_CALL_NPC_RESULT,
        .callerOid = command.callerOid,
        .calledOid = 0x40'00'00'00,
      };

      _commandServer.QueueCommand<decltype(event)>(clientId, [event](){return event;});
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandKickRanch>(
    [this](ClientId clientId, auto& command)
    {
      // LOA-fix (S9): реально выгоняем визитёра. Оригинал слал OK+Notify, но не
      // удалял цель — кикнутый оставался на ранчо. Зеркалим HandleRanchLeave
      // (tracker.RemoveCharacter + clients.erase + LeaveRanchNotify) и вводим
      // права: кикать может только владелец ранчо, владельца/себя — нельзя.
      const auto& clientContext = GetClientContext(clientId);
      const auto ownerUid = clientContext.visitingRancherUid;
      const auto kickerUid = clientContext.characterUid;
      const auto targetUid = static_cast<data::Uid>(command.characterUid);

      const auto rejectKick = [&]()
      {
        protocol::RanchCommandKickRanchCancel cancel{};
        _commandServer.QueueCommand<decltype(cancel)>(
          clientId, [cancel](){ return cancel; });
      };

      // Кикать вправе только владелец ранча; владельца/самого себя не кикнуть.
      if (kickerUid != ownerUid || targetUid == ownerUid || targetUid == kickerUid)
      {
        rejectKick();
        return;
      }

      const auto ranchIter = _ranches.find(ownerUid);
      if (ranchIter == _ranches.cend())
      {
        rejectKick();
        return;
      }
      auto& ranchInstance = ranchIter->second;

      // Ищем клиента цели среди визитёров ранча (GetClientIdByCharacterUid
      // кидает исключение, если персонаж не привязан к клиенту).
      ClientId targetClientId = clientId;
      bool targetFound = false;
      for (const ClientId& ranchClientId : ranchInstance.clients)
      {
        if (GetClientContext(ranchClientId).characterUid == targetUid)
        {
          targetClientId = ranchClientId;
          targetFound = true;
          break;
        }
      }

      if (not targetFound)
      {
        rejectKick();
        return;
      }

      // Подтверждаем кикеру.
      protocol::RanchCommandKickRanchOK response{};
      _commandServer.QueueCommand<decltype(response)>(
        clientId, [response](){ return response; });

      // Сообщаем кикнутому, что его выгнали (его клиент вернётся на своё ранчо).
      protocol::RanchCommandKickRanchNotify kickNotify{
        .characterUid = command.characterUid};
      _commandServer.QueueCommand<decltype(kickNotify)>(
        targetClientId, [kickNotify](){ return kickNotify; });

      // Реально убираем визитёра из инстанса ранча.
      ranchInstance.tracker.RemoveCharacter(targetUid);
      ranchInstance.clients.erase(targetClientId);
      // LOA-fix (SYNC-9g, adversarial round): кик — такой же выход из инстанса,
      // как LeaveRanch, значит и кэш положения (SYNC-9a) снимаем здесь тоже.
      ranchInstance.snapshots.erase(targetUid);
      // LOA-fix (SYNC-13d, adversarial round 2): кик тоже сбрасывает visitingRancherUid
      // ЦЕЛИ — иначе кикнутый застрянет: его повторный вход same-socket на своё ранчо
      // отвергнет гард R13-12 (тот же класс, что SYNC-13 закрыл для Leave). Цель ещё в
      // _clients (сокет жив), контекст мутабелен; requireAuthentication=false — на случай
      // редкой гонки со снятием аутентификации, чтобы не бросить исключение (ср. SYNC-4).
      GetClientContext(targetClientId, false).visitingRancherUid = data::InvalidUid;

      // LOA-fix (R21-2d, round21, backlog #95): кик — такой же выход с ранча,
      // как LeaveRanch, но он идёт МИМО HandleRanchLeave. Снимаем запись
      // активности здесь, иначе кикнутый до самого разрыва сокета таскал бы за
      // собой лишнюю запись. Лок ЛИСТОВОЙ, внутри только erase.
      {
        std::lock_guard lock(_ranchActivityMutex);
        _ranchActivity.erase(targetUid);
      }

      // Оставшимся на ранчо шлём LeaveRanchNotify — аватар исчезает (тот же
      // notify, что и при обычном выходе, HandleRanchLeave).
      protocol::AcCmdCRLeaveRanchNotify leaveNotify{
        .characterId = targetUid};
      for (const ClientId& ranchClientId : ranchInstance.clients)
      {
        _commandServer.QueueCommand<decltype(leaveNotify)>(
          ranchClientId,
          [leaveNotify]()
          {
            return leaveNotify;
          });
      }
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCROpCmd>(
    [this](ClientId clientId, auto& command)
    {
      HandleOpCmd(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::RanchCommandRequestLeagueTeamList>(
    [this](ClientId clientId, auto& command)
    {
      HandleRequestLeagueTeamList(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRMountFamilyTree>(
    [this](ClientId clientId, auto& command)
    {
      if (HandleMountFamilyTree(clientId, command))
        _mountFamilyTreeDeferrer.Defer(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRecoverMount>(
    [this](ClientId clientId, const auto& command)
    {
      HandleRecoverMount(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRWithdrawGuildMember>(
    [this](ClientId clientId, const auto& command)
    {
      HandleWithdrawGuild(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRCheckStorageItem>(
    [this](ClientId clientId, const auto& command)
    {
      HandleCheckStorageItem(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRGuildMemberList>(
    [this](ClientId clientId, const auto& command)
    {
      HandleGetGuildMemberList(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestGuildMatchInfo>(
    [this](ClientId clientId, const auto& command)
    {
      HandleRequestGuildMatchInfo(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUpdateGuildMemberGrade>(
    [this](ClientId clientId, const auto& command)
    {
      HandleUpdateGuildMemberGrade(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChangeAge>(
    [this](ClientId clientId, const auto& command)
    {
      HandleChangeAge(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRHideAge>(
    [this](ClientId clientId, const auto& command)
    {
      HandleHideAge(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChangeSkillCardPreset>(
    [this](ClientId clientId, const auto& command)
    {
      HandleChangeSkillCardPreset(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRInviteGuildJoin>(
    [this](ClientId clientId, const auto& command)
    {
      HandleInviteToGuild(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCREmblemList>(
    [this](ClientId clientId, const auto& command)
    {
      HandleGetEmblemList(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRChangeNickname>(
    [this](ClientId clientId, const auto& command)
    {
      HandleChangeNickname(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUpdateDailyQuest>(
    [this](ClientId clientId, const auto& command)
    {
      HandleUpdateDailyQuest(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRegisterDailyQuestGroup>(
    [this](ClientId clientId, const auto& command)
    {
      HandleRegisterDailyQuestGroup(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestDailyQuestReward>(
    [this](ClientId clientId, const auto& command)
    {
      HandleRequestDailyQuestReward(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRegisterQuest>(
    [this](ClientId clientId, const auto& command)
    {
      HandleRegisterQuest(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestQuestReward>(
    [this](ClientId clientId, const auto& command)
    {
      HandleRequestQuestReward(clientId, command);
      });
  
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRGiveupQuest>(
    [this](ClientId clientId, const auto& command)
    {
      HandleGiveupQuest(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRConfirmItem>(
    [this](ClientId clientId, const auto& command)
    {
      HandleConfirmItem(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRConfirmSetItem>(
    [this](ClientId clientId, const auto& command)
    {
      HandleConfirmSetItem(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBuyOwnItem>(
    [this](ClientId clientId, const auto& command)
    {
      HandleBuyOwnItem(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRSendGift>(
    [this](ClientId clientId, const auto& command)
    {
      HandleSendGift(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCROpenRandomBox>(
    [this](ClientId clientId, const auto& command)
    {
      HandleOpenRandomBox(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRPasswordAuth>(
    [this](ClientId clientId, const auto& command)
    {
      HandlePasswordAuth(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRUpdateMountInfo>(
    [this](ClientId clientId, const auto& command)
    {
      HandleUpdateMountInfo(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRInviteUser>(
    [this](ClientId clientId, const auto& command)
    {
      HandleInviteUser(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRRequestUser>(
    [this](ClientId clientId, const auto& command)
    {
      HandleRequestUser(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBreedingTakeMoney>(
    [this](ClientId clientId, const auto& command)
    {
      HandleBreedingTakeMoney(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRExpandMountSlot>(
    [this](ClientId clientId, const auto& command)
    {
      HandleExpandMountSlot(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBreedingWishlistAdd>(
    [this](ClientId clientId, const auto& command)
    {
      HandleBreedingWishlistAdd(clientId, command);
    });

  _commandServer.RegisterCommandHandler<protocol::AcCmdCRBreedingWishlistDel>(
    [this](ClientId clientId, const auto& command)
    {
      HandleBreedingWishlistDelete(clientId, command);
    });

  // LOA-fix (R44-4, #58/R1): ЗАМЕР 0x16b — ТОЛЬКО ЛОГ, НИКАКОГО ЭФФЕКТА.
  // Клиент репортит прогресс достижений сам, и что именно он шлёт (какие
  // события, абсолют или приращение, в каком виде) — главная неизвестная всего
  // #58: классы каталога размечены чтением кода, а не проводом. До этого
  // раунда команда молча отбрасывалась, причём в релизной сборке даже без
  // строки в логе (`debugCommands` = false), поэтому «ничего не приходит» было
  // неотличимо от «приходит, но молчит».
  // ★Значение рисует КЛИЕНТ, значит им можно спамить лог: держим окно в одну
  // секунду и не больше kAchievementProbeLinesPerSecond строк в нём, а саму
  // строку обрезаем. Прогресс, награды и ответы клиенту не трогаем вообще.
  _commandServer.RegisterCommandHandler<protocol::AcCmdCRAchievementUpdateProperty>(
    [this](ClientId clientId, const auto& command)
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
        "[achv-probe ranch] character {} event {} value '{}' ({} bytes, {}, {} dropped)",
        clientContext.characterUid,
        command.achievementEvent,
        command.propertyValue,
        command.propertyValue.size(),
        command.isPropertyValueTerminated ? "nul-ok" : "NO-NUL",
        command.rejectedPropertyValueBytes);
    });
}

void RanchDirector::Initialize()
{
  _breedingMarket.Initialize();

  ScheduleFoalMaturityCheck();

  server::util::QuietLogDebug(
    "Ranch server listening on {}:{}",
    GetConfig().listen.address.to_string(),
    GetConfig().listen.port);

  _commandServer.BeginHost(GetConfig().listen.address, GetConfig().listen.port);
}

void RanchDirector::Terminate()
{
  _breedingMarket.Terminate();
  _commandServer.EndHost();
}

void RanchDirector::Tick()
{
  _breedingMarket.Tick();
  _scheduler.Tick();
}

void RanchDirector::RefreshMaturingFoals(
  const data::Uid characterUid,
  ClientContext& clientContext)
{
  clientContext.maturingFoals = GetServerInstance().GetHorseSystem().PromoteMaturedFoals(characterUid);
}

// LOA-fix (batch1 task3): server-authoritative ежедневный сброс дейлик-квестов на
// входе на СВОЁ ранчо (06:00 UTC rollover). Также чинит латентный баг:
// carrotsClaimed никогда не сбрасывался → после первого клейма морковки игрок
// больше НИКОГДА не получал дейлик-морковки. Zero hand-migration: старые группы
// без lastResetDate грузятся как 0 → первый вход после деплоя = ровно один сброс.
void RanchDirector::ResetDailyQuestsIfNeeded(const data::Uid characterUid)
{
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    characterUid);

  // Читаем uid дейлик-группы персонажа; группы ещё нет → сбрасывать нечего
  // (зеркало InvalidUid-гарда из HandleUpdateDailyQuest).
  data::Uid groupUid = data::InvalidUid;
  characterRecord.Immutable(
    [&groupUid](const data::Character& character)
    {
      groupUid = character.dailyQuestGroupUid();
    });

  if (groupUid == data::InvalidUid)
    return;

  const auto groupRecord = _serverInstance.GetDataDirector().GetDailyQuestGroup(
    groupUid);
  if (not groupRecord.IsAvailable())
  {
    server::util::QuietLogWarn(
      "ResetDailyQuestsIfNeeded: daily quest group {} unavailable for character {}",
      groupUid, characterUid);
    return;
  }

  // LOA-fix (R63-4, round63, backlog #218): СБРОС ОБЯЗАН БЫТЬ ВИДЕН.
  // Он обнуляет три слота целей дня — то есть ровно то состояние, на которое
  // жалуется игрок («заданий нет»). Раньше он проходил молча, и по логу нельзя
  // было сказать даже, случился ли он вообще. Значения снимаем ИЗНУТРИ лямбды,
  // а печатаем СНАРУЖИ: под замком записи логировать нельзя.
  uint32_t resetFromDay = 0;
  uint32_t resetToDay = 0;

  groupRecord.Mutable(
    [&resetFromDay, &resetToDay](data::DailyQuestGroup& group)
    {
      const uint32_t today = util::CurrentGameDayIndex();
      // Сброс не чаще раза в игровой день: только если последний сброс раньше
      // сегодняшнего дня. Старые данные (lastResetDate=0) < today → один сброс.
      if (group.lastResetDate() >= today)
        return;

      // ★Снимаем ДО присваивания ниже: после него `lastResetDate` уже сегодняшняя,
      // и «откуда сбросили» было бы потеряно.
      resetFromDay = group.lastResetDate();
      resetToDay = today;

      // LOA-fix (batch1 fix-round1, BLOCK3): сбрасываем ТОЛЬКО carrotsClaimed и
      // ставим lastResetDate. progress 3 слотов и rewardPoints — client-authoritative:
      // клиент КАЖДЫЙ день заново регистрирует группу через fillGroup, перезаписывая
      // quests/progress/rewardPoints; единственное, что fillGroup НЕ трогает —
      // carrotsClaimed (это и есть баг: после первого клейма морковки больше НИКОГДА
      // не начисляются). Прежний код тут ещё обнулял rewardPoints — но rewardPoints
      // это СТАТИЧЕСКАЯ анти-чит-сумма rewardPoint трёх выбранных квестов
      // (RanchDirector.cpp:5967), и HandleRequestDailyQuestReward гейтит клейм на
      // group.rewardPoints() >= command.rewardPoints (:6680) → обнуление могло
      // ОТКАЗАТЬ в легитимной выдаче дейлик-награды. Поэтому un-stick carrotsClaimed
      // + штамп lastResetDate — полный и минимальный фикс. Присваивание Field'у
      // идентично group.carrotsClaimed = true в HandleUpdateDailyQuest — персист тот же.
      // LOA-fix (R1, round2): ПОЛНЫЙ сброс группы. Раунд 1 снимал только флаги,
      // оставляя questId/progress вчерашних слотов — из-за этого (а) гард F4
      // видел ненулевой прогресс и запрещал взять новые цели (дейлики умирали
      // навсегда со 2-го дня), (б) гейт F2 проходил на протухшем вчерашнем
      // прогрессе и отдавал награду дня бесплатно. Теперь группа возвращается в
      // состояние «целей на сегодня ещё нет»; rewardPoints обнуляется вместе со
      // слотами (это статическая сумма очков ИМЕННО этих трёх целей, без них она
      // бессмысленна), а регистрация нового набора пересчитает его в fillGroup.
      std::array<data::DailyQuestEntry, 3> clearedQuests{};
      group.quests = clearedQuests;
      group.rewardPoints = 0;
      group.carrotsClaimed = false;
      group.dailyRewardClaimed = false;
      group.lastResetDate = today;
      // LOA-fix (R42, #8 F2): счётчик dailyClassExpGranted БОЛЬШЕ НЕ сбрасывается здесь —
      // его владелец теперь SyncDailyClassExpBudget (QuestSystem, gated на отдельной
      // dailyClassExpResetDate). Прежний сброс по lastResetDate мог стереть spend,
      // случившийся между границей дня и этим ранч-входом → второй 6650 (F2-баг).
    });

  if (resetToDay != 0)
  {
    server::util::QuietLogDebug(
      "Daily goals: reset for character {} (group {}), game day {} -> {}; three slots "
      "cleared, waiting for the client to register today's set",
      characterUid, groupUid, resetFromDay, resetToDay);
  }
}

void RanchDirector::ScheduleFoalMaturityCheck() noexcept
{
  // LOA-fix (R55-8, round55, backlog #179 часть 5): постановка больше не
  // бросает. Отказ означает «таймер не заведён», а не «сервер умер».
  if (not util::TryQueue(
    _scheduler,
    [this]()
    {
      // LOA-fix (R35-3, round35, backlog #124): МАРШРУТИЗАЦИЯ ВМЕСТО ПРЯМОГО
      // ПРОХОДА. Эта лямбда крутится на потоке ранч-ДИРЕКТОРА
      // (_scheduler.Tick() ← RanchDirector::Tick ← RunDirectorTaskLoop), а
      // RunFoalMaturityCheck обходит и мутирует `_clients`, владелец которой —
      // ранч-СЕТЕВОЙ поток. Прямой вызов отсюда и был гонкой #124. Теперь
      // задача только звонит будильником; проход снимет HandleNetworkTick
      // (R35-4) в течение ≤1 c (период Server::TickLoop).
      // ★ПОРЯДОК ВАЖЕН: `store` стоит ДО перезавода и бросить не может, поэтому
      // таймер больше не может умереть навсегда из-за исключения внутри
      // прохода (раньше исключение из RunFoalMaturityCheck уносило нас мимо
      // перезавода — см. шапку раунда 35).
      _foalMaturityCheckDue.store(true);
      ScheduleFoalMaturityCheck();
    },
    Scheduler::Clock::now() + FoalMaturityCheckInterval))
  {
    // ★ОТКАЗ НЕ ИМЕЕТ ПРАВА БЫТЬ ТИХИМ. Непоставленная задача означает, что
    // периодическая проверка больше НИКОГДА не проснётся сама: жеребята
    // перестанут взрослеть в фоне (вход на ранчо их всё ещё подберёт).
    //
    // ★Пишем по ПЕРЕХОДУ, а не на каждой попытке. Повтор идёт с сетевого тика,
    // то есть раз в секунду: запись «в лоб» залила бы лог одной и той же
    // строкой, а под нехваткой памяти — ровно тогда, когда лог нужен читаемым.
    // `exchange` возвращает ПРЕЖНЕЕ значение, поэтому строка выходит один раз
    // на непрерывную полосу отказов.
    if (not _foalMaturityRearmFailed.exchange(true))
    {
      util::QuietLogError(
        "Failed to arm the foal maturity check; will retry on the ranch network tick");
    }
    return;
  }

  // ★Восстановление тоже обязано быть ВИДНЫМ: без этой строки полоса отказов
  // выглядела бы бесконечной, и по логу нельзя было бы отличить «таймер
  // вернулся» от «мы про него забыли».
  if (_foalMaturityRearmFailed.exchange(false))
    util::QuietLogInfo("Foal maturity check re-armed after an earlier failure");
}

void RanchDirector::RunFoalMaturityCheck()
{
  // LOA-fix (R35-5, round35, backlog #124): ★ЗВАТЬ ТОЛЬКО С РАНЧ-СЕТЕВОГО
  // ПОТОКА (сейчас — единственная точка вызова: HandleNetworkTick, R35-4).
  // Ниже идут ЧЕТЫРЕ операции, каждая из которых требует владения ранч-данными:
  //   1) обход всей `_clients` — гоночное чтение структуры при чужом rehash;
  //   2) мутация `clientContext.maturingFoals` (erase) — ту же map пишет
  //      RefreshMaturingFoals на входе на ранчо и хендлеры разведения;
  //   3) AnnounceFoalGrewUp → `_ranches[characterUid]` (operator[] ВСТАВЛЯЕТ) и
  //      GetClientContext(clientId) — второе чтение `_clients`;
  //   4) _commandServer.QueueCommand.
  // До раунда 35 всё это делал поток ранч-ДИРЕКТОРА (задача планировщика) —
  // это и была гонка #124, доказанная TSan.
  const auto now = data::Clock::now();

  for (auto& [clientId, clientContext] : _clients)
  {
    if (not clientContext.isAuthenticated
      || clientContext.maturingFoals.empty())
    {
      continue;
    }

    for (auto foalIter = clientContext.maturingFoals.begin();
      foalIter != clientContext.maturingFoals.end();)
    {
      if (now < foalIter->second)
      {
        ++foalIter;
        continue;
      }

      const auto horseUid = foalIter->first;
      const auto horseRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(horseUid);

      bool isFoal = false;
      if (horseRecord)
      {
        horseRecord->Immutable([&isFoal](const data::Horse& horse)
        {
          isFoal = horse.type() == data::Horse::Type::Foal;
        });
      }

      if (isFoal)
      {
        horseRecord->Mutable([](data::Horse& horse)
        {
          horse.type() = data::Horse::Type::Adult;
        });

        AnnounceFoalGrewUp(
          clientId,
          clientContext.characterUid,
          horseUid);
      }

      foalIter = clientContext.maturingFoals.erase(foalIter);
    }
  }
}

void RanchDirector::AnnounceFoalGrewUp(
  const ClientId clientId,
  const data::Uid characterUid,
  const data::Uid horseUid)
{
  const auto horseRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(horseUid);
  if (not horseRecord)
    return;

  protocol::AcCmdRCUpdateMountInfoNotify growUp{
      .characterUid = characterUid,
      .action = protocol::AcCmdRCUpdateMountInfoNotify::Action::PutHorseInRentOrBreedingSystem};
  horseRecord->Immutable([&growUp](const data::Horse& horse)
  {
    protocol::BuildProtocolHorse(growUp.horse, horse);
  });

  _commandServer.QueueCommand<decltype(growUp)>(
    clientId,
    [growUp]()
    {
      return growUp;
    });

  protocol::AcCmdRCAddIdleMountInfoNotify addNotify{};
  addNotify.horse.horseOid = _ranches[characterUid].tracker.GetHorseOid(horseUid);
  horseRecord->Immutable([&addNotify](const data::Horse& horse)
  {
    protocol::BuildProtocolHorse(addNotify.horse.horse, horse);
  });

  const auto& clientContext = GetClientContext(clientId);
  if (clientContext.visitingRancherUid == characterUid)
  {
    // The owner is on their own ranch; broadcast the new idle mount to everyone there.
    for (const ClientId& ranchClientId : _ranches[characterUid].clients)
    {
      _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
        ranchClientId,
        [addNotify]()
        {
          return addNotify;
        });
    }
  }
  else
  {
    _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
      clientId,
      [addNotify]()
      {
        return addNotify;
      });

    protocol::AcCmdRCMobDead mobDead{
      .mobOid = addNotify.horse.horseOid};
    _commandServer.QueueCommand<protocol::AcCmdRCMobDead>(
      clientId,
      [mobDead]()
      {
        return mobDead;
      });
  }
}

void RanchDirector::ReturnHorseToNature(
  data::Uid characterUid,
  data::Uid horseUid,
  std::string userName,
  [[maybe_unused]] bool breedingAbandon)
{
  bool isHorseValid = false;
  GetServerInstance().GetDataDirector().GetCharacter(characterUid).Mutable(
    [&isHorseValid, horseUid](data::Character& character)
    {
      const auto horseIter = std::ranges::find(character.horses(), horseUid);
      isHorseValid = horseIter != character.horses().end();
      if (not isHorseValid)
        return;
      
        // Remove horse from character
      character.horses().erase(horseIter);
    });

  if (not isHorseValid)
    // TODO: log?
    return;

  // Remove horse from ranch tracker
  auto& ranchInstance = _ranches[characterUid];
  const auto horseOid = ranchInstance.tracker.GetHorseOid(horseUid);
  ranchInstance.tracker.RemoveHorse(horseUid);
  if (horseOid != tracker::InvalidEntityOid)
  {
    const protocol::AcCmdRCMobDead mobDead{.mobOid = horseOid};
    for (const ClientId& ranchClientId : ranchInstance.clients)
    {
      _commandServer.QueueCommand<protocol::AcCmdRCMobDead>(
        ranchClientId,
        [mobDead]()
        {
          return mobDead;
        });
    }
  }

  // Keep horse record in cache for the family tree

  server::util::QuietLogInfo("User {} returned horse {} to nature",
    userName,
    horseUid);
}

std::vector<data::Uid> RanchDirector::GetOnlineCharacters()
{
  std::vector<data::Uid> onlineCharacterUids;

  for (const auto& clientContext : _clients | std::views::values)
  {
    if (not clientContext.isAuthenticated)
      continue;
    onlineCharacterUids.emplace_back(clientContext.characterUid);
  }

  return onlineCharacterUids;
}

void RanchDirector::HandleNetworkTick()
{
  try
  {
    // LOA-fix (R34-5, round34, backlog #96): ПЕРВЫМ ДЕЛОМ — отложенные разрывы.
    // Единственная точка, где ранч-сетевой поток исполняет просьбы чужих
    // потоков (лобби-таймаут, GM-бан/сброс). Порядок «сначала слив, потом
    // дефереры» намеренный: мёртвых клиентов убираем до того, как отложенные
    // команды снова к ним обратятся.
    // ★Бросить отсюда безопасно: обрамляющий catch на месте, а Server::TickLoop
    // помечен noexcept — исключение мимо него было бы std::terminate.
    DrainPendingDisconnects();

    // LOA-fix (R35-4, round35, backlog #124): ПРОХОД ПО СОЗРЕВШИМ ЖЕРЕБЯТАМ —
    // здесь и только здесь. Мы на РАНЧ-СЕТЕВОМ потоке, единственном владельце
    // `_clients`/`_ranches`, поэтому обход и мутация контекстов законны.
    // ★ПОСЛЕ слива отложенных разрывов намеренно: мёртвые клиенты уже убраны из
    // `_clients`, и уведомление о взрослении не адресуется трупу.
    // ★exchange(false) СНАЧАЛА: если проход бросит, флаг уже снят и следующий
    // тик не будет молотить тот же сбойный проход 60 раз в минуту — он просто
    // подождёт очередного звонка. Необработанные жеребята при этом не теряются:
    // их `maturingFoals` не тронут, а срок созревания уже прошёл, так что
    // следующий проход подберёт их целиком.
    // ★Бросить отсюда безопасно: обрамляющий catch на месте (см. R34-5).
    if (_foalMaturityCheckDue.exchange(false))
      RunFoalMaturityCheck();

    // LOA-fix (R55-9, round55, backlog #179 часть 5): ★ВОССТАНОВЛЕНИЕ БЕРЁМ
    // ДАРОМ, А НЕ СТРОИМ. Этот тик уже крутится не реже раза в секунду, уже
    // накрыт обрамляющим перехватом и уже опрашивает соседний атомарный флаг.
    // Память освободилась — таймер сам вернулся к жизни.
    //
    // ★Почему НЕ «пусть тик сам гоняет проход вместо таймера»: это переписало
    // бы УСПЕШНЫЙ путь ради пути отказа — ровно ошибка, за которую R51 получил
    // три NO-GO подряд ([[dont-trade-success-path-for-failure-path]]).
    //
    // Гонки нет: `Queue` берёт собственный мьютекс, а предупреждение R35 про
    // поток-владельца касается `_clients`, к которым перезавод не прикасается.
    if (_foalMaturityRearmFailed.load())
      ScheduleFoalMaturityCheck();

    _mountFamilyTreeDeferrer.Tick();
    _enterRanchDeferrer.Tick();
    _tryBreedingDeferrer.Tick();
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogError("Exception in a network tick of ranch director: {}", x.what());
  }
}

void RanchDirector::HandleClientConnected(ClientId clientId)
{
  server::util::QuietLogDebug(
    "Client {} connected to the ranch server from {}",
    clientId,
    _commandServer.GetClientAddress(clientId).to_string());
  const auto clientIterator = _clients.try_emplace(clientId).first;

  // LOA-fix (R34-7, round34, backlog #96): ★ШТАМП ПОКОЛЕНИЯ СОЕДИНЕНИЯ.
  // Каждое принятое ранч-соединение получает уникальный возрастающий номер
  // рождения. Он нужен отложенным разрывам (R34-2/R34-4): просьба порвать
  // соединение приходит с ЧУЖОГО потока и знает только characterUid, а один
  // персонаж может держать СРАЗУ ДВА соединения (переподключился быстрее, чем
  // отработал разрыв старого — дедупа по characterUid при входе нет: здесь
  // персонаж ещё неизвестен, он появляется позже, при авторизации по OTP).
  // Сравнение connectSeq с requestSeq и отличает старое соединение от того,
  // которое родилось уже ПОСЛЕ просьбы.
  // ★ПОТОК: это ранч-СЕТЕВОЙ поток — владелец _clients; поле пишется ровно
  // здесь и ровно один раз за жизнь соединения, читается тем же потоком в
  // DrainPendingDisconnects. Межпотоковый — только сам счётчик, он atomic.
  // ★fetch_add(1) + 1: нумерация с ЕДИНИЦЫ, чтобы ноль остался значением
  // «не проштамповано» и не выглядел как легальное поколение.
  clientIterator->second.connectSeq = _connectSeqCounter.fetch_add(1) + 1;
}

void RanchDirector::HandleClientDisconnected(ClientId clientId)
{
  server::util::QuietLogInfo("Client {} disconnected from the ranch server", clientId);

  // LOA-fix (R50-5, round50, backlog #180): УБОРКА РАНЧА ДОХОДИТ ДО КОНЦА.
  // `HandleRanchLeave` — самая бросающая работа во всём разрыве (реестры ранчо,
  // рассылки соседям), а `_clients.erase` стоял ПОСЛЕ неё. Осиротевший контекст
  // остаётся аутентифицированным и с `characterUid`, поэтому:
  //   • `GetClientContextByCharacterUid` отдаёт ПЕРВОЕ совпадение — то есть
  //     может вернуть мёртвый сокет вместо живого;
  //   • на следующем входе персонажа `DedupeStaleCharacterSessions` находит
  //     призрака и пытается разорвать его через `DisconnectClient`, а того уже
  //     нет в реестре сетевого сервера (R49-21 удаляет безусловно) → бросок →
  //     строка «Failed to disconnect the stale ranch client». Призрак остаётся
  //     навсегда, и пара строк повторяется на КАЖДОМ входе.
  const util::RegistryEraser eraser{_clients, clientId};

  const auto& clientContext = GetClientContext(clientId, false);

  // LOA-fix (R38-9, round38, backlog #131 SECURITY): был ли сокет РЕАЛЬНО на ранчо,
  // снимок ДО HandleRanchLeave (он сбрасывает visitingRancherUid). visitingRancherUid
  // (стр. ~2419) ставится РАНЬШЕ записи активности (стр. ~2434), поэтому владелец
  // записи активности гарантированно имел visitingRancherUid!=InvalidUid. Сокет,
  // отбитый по замку/переполнению/кросс-личностному гарду R38-7, приходит сюда с
  // привязанным (R38-6) characterUid, но БЕЗ своей записи активности → безусловный
  // erase ниже стёр бы запись ДРУГОЙ живой сессии того же персонажа (self-DoS).
  const bool wasOnRanch = clientContext.visitingRancherUid != data::InvalidUid;
  // LOA-fix (R50-5, round50, backlog #180): снимок UID делается ЗДЕСЬ, до
  // выхода с ранча. `HandleRanchLeave` его не меняет (сбрасывает только
  // visitingRancherUid), но уборка активности ниже больше не зависит от того,
  // осталась ли ссылка на контекст пригодной после сбоя.
  const auto characterUid = clientContext.characterUid;

  if (clientContext.isAuthenticated)
  {
    util::RunCleanupStep(
      "ranch leave",
      clientId,
      [&]()
      {
        HandleRanchLeave(clientId);
      });
  }

  // LOA-fix (R21-2b, round21, backlog #95): реестр активности чистим на разрыве
  // сокета — это безусловный путь, дающий верхнюю границу памяти. Обязательно
  // ДО `_clients.erase(clientId)`: после него ссылка clientContext мертва.
  // Лок ЛИСТОВОЙ, внутри только erase.
  // ★R38-9: только для сокета, который ВХОДИЛ на ранчо (иначе записи активности у
  // него нет — она у чужой живой сессии; #95 сохранён: владелец записи всё чистит).
  // ★Шаг НЕЗАВИСИМЫЙ: сбой выхода с ранча выше не имеет права оставить запись
  // активности. Иначе лобби продолжало бы откладывать таймаут по персонажу,
  // которого уже нет.
  if (wasOnRanch)
  {
    util::RunCleanupStep(
      "ranch activity cleanup",
      clientId,
      [&]()
      {
        std::lock_guard lock(_ranchActivityMutex);
        _ranchActivity.erase(characterUid);
      });
  }
}

void RanchDirector::HandleClientActivity(const ClientId clientId)
{
  // LOA-fix (R21-2a, round21, backlog #95): ШТАМП АКТИВНОСТИ РАНЧ-СОКЕТА.
  // Зовётся из CommandServer::NetworkEventHandler::OnClientData на КАЖДУЮ
  // входящую дейтаграмму ранч-канала — то есть в том числе на AcCmdCRHeartbeat
  // (0x12), у которой зарегистрированного хендлера нет вовсе.
  //
  // ★ШАГ 1 — БЕЗ activity-лока. Мы на ранч-потоке (единственный поток, который
  // вообще трогает _clients), поэтому чтение контекста здесь законно и НЕ
  // требует нашего мьютекса. Берём контекст через find, а не через
  // GetClientContext: тот бросает, а исключение отсюда обрушило бы клиенту
  // соединение в сетевом read-loop.
  const auto clientIter = _clients.find(clientId);
  if (clientIter == _clients.cend())
    return;

  const auto& clientContext = clientIter->second;

  // Гейт: штампуем ТОЛЬКО игроков, реально стоящих на ранчо. Клиент в меню
  // лобби записи не получает вовсе — значит и отсрочку лобби-таймаута получить
  // не может (см. шапку раунда 21).
  if (not clientContext.isAuthenticated
    || clientContext.visitingRancherUid == data::InvalidUid)
  {
    return;
  }

  // Копию UID снимаем ДО взятия лока — под локом не должно оставаться ничего,
  // кроме одной операции с map.
  const auto characterUid = clientContext.characterUid;

  // ★ШАГ 2 — ЛИСТОВОЙ лок, ровно одна операция с map, никаких вызовов наружу.
  // ★★UPDATE-ONLY, И ЭТО НЕ СТИЛЬ, А КОРРЕКТНОСТЬ. Здесь НЕЛЬЗЯ писать
  // `_ranchActivity[characterUid] = now`: teardown клиента умеет бежать на
  // ЛОББИ-потоке (Client::End() зовёт OnClientDisconnected синхронно, а
  // Disconnect дёргается из лобби-тика), поэтому между копированием UID выше и
  // взятием лока здесь запись могла быть законно стёрта. operator[] воскресил
  // бы её сиротой навсегда — сокет уже мёртв, второго teardown не будет, и на
  // каждом таком кике реестр рос бы на запись. find+присваивание такой сироты
  // создать не может: обновлять просто нечего. Создание записи живёт РОВНО в
  // одном месте — HandleEnterRanch (R21-2e).
  {
    std::lock_guard lock(_ranchActivityMutex);

    const auto activityIter = _ranchActivity.find(characterUid);
    if (activityIter != _ranchActivity.end())
      activityIter->second = std::chrono::steady_clock::now();
  }
}

void RanchDirector::SweepRanchActivity(const std::chrono::seconds maxAge)
{
  // LOA-fix (R21-2a, round21, backlog #95): ПЕРИОДИЧЕСКАЯ УБОРКА — верхняя
  // граница памяти реестра. Выходные пути (disconnect/leave/kick) чистят быстро,
  // но остаточная гонка enter-vs-disconnect (создание на ранч-потоке легло ПОСЛЕ
  // teardown-erase с лобби-потока) способна оставить запись, которую не подберёт
  // ни один выходной путь. Подбираем её здесь.
  //
  // ★ЖИВОГО ИГРОКА УБОРКА НЕ ТРОГАЕТ: он переставляет метку каждые ≤8.5 c
  // (худший наблюдённый по pcap разрыв ранч-пакетов), а maxAge = 90 c — втрое
  // больше окна свежести 30 c. Запись возрастом 30-90 c грейса уже не даёт,
  // вреда не несёт и просто ждёт ближайшей уборки.
  //
  // ★ПОТОК: зовётся с ЛОББИ-потока (тик). Законно ровно потому, что метод
  // трогает ТОЛЬКО свой мьютекс и свою map — ни _clients, ни _ranches.
  // ★Лок ЛИСТОВОЙ: обход + erase, ни одного вызова наружу (в том числе никакого
  // логирования под локом). O(n), n ≤ числа игроков на ранчо.
  const auto now = std::chrono::steady_clock::now();

  std::lock_guard lock(_ranchActivityMutex);

  for (auto activityIter = _ranchActivity.begin();
    activityIter != _ranchActivity.end();)
  {
    if (now - activityIter->second > maxAge)
      activityIter = _ranchActivity.erase(activityIter);
    else
      ++activityIter;
  }
}

bool RanchDirector::IsCharacterActiveOnRanch(
  const data::Uid characterUid,
  const std::chrono::seconds freshness) const
{
  // LOA-fix (R21-2a, round21, backlog #95): ЗАПРОС С ЧУЖОГО ПОТОКА (лобби-тик).
  // ★Единственное, что здесь можно трогать, — мьютекс и map. _clients НЕ
  // читается: он не защищён ничем и принадлежит ранч-потоку.
  std::lock_guard lock(_ranchActivityMutex);

  const auto activityIter = _ranchActivity.find(characterUid);
  if (activityIter == _ranchActivity.cend())
    return false;

  // Свежесть = анти-зомби: полумёртвый сокет перестаёт слать пакеты, запись
  // стареет, и лобби доводит кик до конца (см. шапку раунда 21).
  return std::chrono::steady_clock::now() - activityIter->second < freshness;
}

void RanchDirector::Disconnect(data::Uid characterUid)
{
  // LOA-fix (R34-4, round34, backlog #96): МАРШРУТИЗАЦИЯ ВМЕСТО ПРЯМОГО РАЗРЫВА.
  // Раньше здесь шёл обход _clients и вызов DisconnectClient — прямо на ЧУЖОМ
  // потоке (лобби-тик по сетевому таймауту, GM-команды из ChatSystem).
  // _clients директора ранчо не защищён ничем и принадлежит РАНЧ-СЕТЕВОМУ
  // потоку, который в тот же момент законно вставляет и стирает записи в
  // HandleClientConnected/HandleClientDisconnected. Это гонка по неатомарной
  // unordered_map: rehash под чужим обходом = порча памяти, а не «редкий сбой».
  // Хуже того, DisconnectClient синхронно доходил до Server::_clients — ВТОРОЙ
  // незащищённой map — и стирал запись и оттуда тоже.
  //
  // ★ПОЧЕМУ НЕ МЬЮТЕКС. Взять лок на _clients «по-честному» здесь нельзя:
  // GetClientContext возвращает ClientContext& НАРУЖУ, ссылка живёт дольше
  // любого лока внутри аксессора и держится хендлерами через вызовы. Лок в
  // аксессоре дал бы ЛОЖНУЮ безопасность — ровно та ошибка, из-за которой
  // раунд 21 пошёл через отдельную структуру, а не через «залочим _clients».
  //
  // Кладём UID в очередь под ЛИСТОВЫМ локом (одна операция с контейнером, ни
  // одного вызова наружу) и уходим. Разрыв сделает DrainPendingDisconnects на
  // ранч-сетевом тике — на том единственном потоке, которому _clients
  // принадлежит. Задержка ≤1 c (период Server::TickLoop).
  //
  // ★ШТАМП ПОКОЛЕНИЯ. Снимок счётчика рождений соединений — это ответ на
  // вопрос «какие ранч-соединения существовали на момент просьбы»: у всех у
  // них connectSeq <= requestSeq. Соединение, которое подключится ПОЗЖЕ
  // (реконнект того же персонажа внутри окна слива), получит строго больший
  // номер, и дренаж его по построению не выберет. Без этого штампа дренаж
  // искал бы по одному characterUid и рвал ПЕРВОЕ совпадение в unordered_map,
  // а libstdc++ отдаёт первой САМУЮ СВЕЖУЮ запись — то есть убивал бы
  // реконнект и оставлял в живых старую сессию (для GM-бана — обход санкции).
  // ★Читаем счётчик ДО взятия листового мьютекса: под ним должна оставаться
  // ровно одна операция с контейнером.
  const std::uint64_t requestSeq = _connectSeqCounter.load();

  std::lock_guard lock(_pendingDisconnectsMutex);

  // Повторные просьбы про один UID схлопываются в одну запись. Порог берём
  // ПОЗДНЕЙШИЙ: более свежая просьба видела больше соединений, и «омолаживать»
  // её ранним снимком нельзя — иначе соединение, родившееся между двумя
  // просьбами, выпало бы из зоны разрыва.
  const auto [pendingIterator, isInserted] =
    _pendingDisconnects.try_emplace(characterUid, requestSeq);
  if (not isInserted)
    pendingIterator->second = std::max(pendingIterator->second, requestSeq);
}

void RanchDirector::DedupeStaleCharacterSessions(
  const ClientId newClientId,
  const data::Uid characterUid)
{
  // LOA-fix (R38-2, round38, backlog #131): КОРЕНЬ СОСУЩЕСТВОВАНИЯ.
  // HandleClientConnected дедупа не делает и делать не может: на accept'е
  // персонаж ещё неизвестен. Поэтому дедуп живёт здесь — в первой точке, где
  // characterUid уже подтверждён OTP. Всё, что связано ПОЧЕМУ именно так,
  // расписано в шапке round38 и в док-комментарии объявления (R38-1).
  //
  // ★★ПОЧЕМУ ПРЯМОЙ DisconnectClient, А НЕ Disconnect(characterUid) ЧЕРЕЗ
  // ОЧЕРЕДЬ. Disconnect() кладёт просьбу в _pendingDisconnects со снимком
  // requestSeq = _connectSeqCounter.load(). connectSeq новой сессии B выдан ещё
  // на accept'е и УЖЕ учтён этим счётчиком, то есть B.connectSeq <= requestSeq.
  // Значит дренаж снёс бы КАЖДУЮ сессию этого персонажа с
  // connectSeq <= requestSeq — ВКЛЮЧАЯ саму легитимную B, — и сделал бы это
  // ПОЗЖЕ, отдельным тиком (асинхронно), когда B уже финализировала своё
  // ранч-состояние: ровно инверсия #96. Прямой синхронный разрыв рвёт ТОЛЬКО
  // заранее существовавший стейл-набор и целиком ДО первой записи B.
  if (characterUid == data::InvalidUid)
    return;

  // ★ШАГ 1 — ТОЛЬКО ПОИСК, БЕЗ ПОБОЧНЫХ ЭФФЕКТОВ. Ровно та же готча, из-за
  // которой в два шага написан DrainPendingDisconnects: DisconnectClient зовёт
  // Client::End(), тот СИНХРОННО уводит управление в OnClientDisconnected и
  // доходит до _clients.erase(clientId) — итератор обхода умирает прямо там.
  // Поэтому ClientId запоминаются ПО ЗНАЧЕНИЮ, а разрыв делается ниже, уже вне
  // обхода map.
  // ★СОБИРАЕМ ВСЕ СОВПАДЕНИЯ, а не первое: призраков могло накопиться больше
  // одного, а порядок unordered_map не специфицирован.
  std::vector<ClientId> staleClientIds;

  for (const auto& [clientId, clientContext] : _clients)
  {
    // Себя не дедупим. Гард нужен не только «на всякий случай»: повторный
    // заход в HandleEnterRanch по отсрочке (_enterRanchDeferrer) видит СВОЙ
    // контекст уже с выставленным characterUid — без этой строки сессия
    // разорвала бы сама себя.
    if (clientId == newClientId)
      continue;

    if (not clientContext.isAuthenticated
      or clientContext.characterUid != characterUid)
    {
      continue;
    }

    staleClientIds.emplace_back(clientId);
  }

  if (staleClientIds.empty())
    return;

  // ★ШАГ 2 — РАЗРЫВ УЖЕ ВНЕ ОБХОДА map. Каждый DisconnectClient синхронно
  // разворачивает полный teardown старой сессии (Client::End →
  // OnClientDisconnected → HandleClientDisconnected → HandleRanchLeave →
  // _clients.erase) и возвращается сюда УЖЕ ЗАВЕРШЁННЫМ. Именно поэтому
  // character-keyed уборка старой сессии не может задеть новую: та ещё ничего
  // о себе не записала.
  for (const ClientId staleClientId : staleClientIds)
  {
    // ★ЭТА СТРОКА — ПРИЁМКА #131 БЕЗ ИНСТРУМЕНТАЦИИ. Она печатается СИНХРОННО,
    // на входе нового клиента, и обязана стоять в логе ДО строк раскладки его
    // ранч-состояния. Если бы разрыв ушёл в очередь (Disconnect), teardown
    // появился бы отдельным тиком, до секунды спустя, — по логу это видно
    // сразу.
    server::util::QuietLogInfo(
      "Deduplicating ranch sessions of character {}: tearing down the stale "
      "client {} synchronously, before client {} finalises its ranch state "
      "(last login wins)",
      characterUid,
      staleClientId,
      newClientId);

    try
    {
      _commandServer.DisconnectClient(staleClientId);
    }
    catch (const std::exception& x)
    {
      // Реестры ранчо и сетевого сервера могли разъехаться. Не даём одной
      // осечке съесть остаток списка и тем более не роняем вход нового клиента.
      server::util::QuietLogWarn(
        "Failed to disconnect the stale ranch client {} of character {}: {}",
        staleClientId,
        characterUid,
        x.what());
    }
  }
}

void RanchDirector::DrainPendingDisconnects()
{
  // LOA-fix (R34-4, round34, backlog #96): исполнение отложенных разрывов.
  // ★ПОТОК: только РАНЧ-СЕТЕВОЙ (единственный вызов — из HandleNetworkTick).
  // Здесь и только здесь законно трогать _clients и _commandServer по просьбе
  // чужого потока.

  // Снимаем очередь ЦЕЛИКОМ и ОТПУСКАЕМ лок до любых действий: под листовым
  // локом не должно оставаться ни одного вызова наружу.
  std::unordered_map<data::Uid, std::uint64_t> pendingDisconnects;
  {
    std::lock_guard lock(_pendingDisconnectsMutex);
    if (_pendingDisconnects.empty())
      return;

    pendingDisconnects.swap(_pendingDisconnects);
  }

  for (const auto& [characterUid, requestSeq] : pendingDisconnects)
  {
    // ★ШАГ 1 — ТОЛЬКО ПОИСК, БЕЗ ПОБОЧНЫХ ЭФФЕКТОВ. Разрывать соединение прямо
    // внутри обхода нельзя: DisconnectClient зовёт Client::End(), тот СИНХРОННО
    // вызывает OnClientDisconnected в этом же стеке, а он доходит до
    // _clients.erase(clientId). И итератор обхода, и структурные привязки
    // [clientId, clientContext] умирают ровно там. Поэтому запоминаем ClientId
    // ПО ЗНАЧЕНИЮ и выходим из обхода.
    //
    // ★ШАГ 1 ТЕПЕРЬ ЕЩЁ И ОТБИРАЕТ ПОКОЛЕНИЕ. Одного characterUid мало: в
    // _clients могут одновременно лежать ДВЕ аутентифицированные записи одного
    // персонажа (дедупа по characterUid на входе нет — на момент
    // HandleClientConnected персонаж вообще неизвестен, а гард повторного
    // входа смотрит собственный контекст соединения). Поэтому берём только
    // записи с connectSeq <= requestSeq — родившиеся ДО просьбы. Реконнект,
    // случившийся внутри окна слива, имеет больший номер и пропускается: это и
    // закрывает ABA-окно «порвали свежую сессию, оставили старую» (для
    // GM-бана — обход санкции).
    // ★СОБИРАЕМ ВСЕ ПОДХОДЯЩИЕ, А НЕ ПЕРВОЕ СОВПАДЕНИЕ. Одного разрыва на
    // просьбу мало: если у персонажа ДО просьбы уже висели два соединения
    // (призрак прошлого реконнекта + текущее), то один разрыв оставил бы
    // второе живым — для GM-БАНА это обход санкции. Все они одинаково
    // «родились до просьбы», значит просьба была про них всех. Обход идём ДО
    // КОНЦА (без break) и без побочных эффектов; порядок unordered_map не
    // специфицирован, полагаться на «первое совпадение» нельзя.
    std::vector<std::pair<ClientId, std::uint64_t>> staleConnections;
    bool isNewerConnectionPresent = false;
    std::uint64_t newerConnectSeq = 0;

    for (const auto& [clientId, clientContext] : _clients)
    {
      if (clientContext.characterUid != characterUid
        or not clientContext.isAuthenticated)
      {
        continue;
      }

      // ★ГЕЙТ ПОКОЛЕНИЯ: соединение родилось ПОСЛЕ просьбы — просьба была не
      // про него. Это и есть закрытие ABA-окна реконнекта.
      if (clientContext.connectSeq > requestSeq)
      {
        isNewerConnectionPresent = true;
        newerConnectSeq = std::max(newerConnectSeq, clientContext.connectSeq);
        continue;
      }

      staleConnections.emplace_back(clientId, clientContext.connectSeq);
    }

    // ★★SKIP-IF-NEWER-SURVIVES (round34, ремонт по находке Codex-T3 о ПОРЧЕ
    // ПЕРСОНАЖА). Одного «пощадить сокет B» мало. Разрыв старого соединения A
    // идёт через DisconnectClient → Client::End() → OnClientDisconnected →
    // RanchDirector::HandleClientDisconnected, а тот для аутентифицированного
    // клиента синхронно зовёт HandleRanchLeave. И вот там уборка идёт НЕ по
    // ClientId, а по characterUid — то есть по ОБЩЕМУ ключу A и B:
    //   • _ranchActivity.erase(characterUid)            (HandleRanchLeave + HandleClientDisconnected)
    //   • ranchInstance.tracker.RemoveCharacter(uid)    (членство персонажа на ранчо)
    //   • ranchInstance.snapshots.erase(uid)            (кэш положения)
    //   • рассылка AcCmdCRLeaveRanchNotify{characterId=uid} ВСЕМ на ранчо
    // Итог до этого ремонта: сокет B цел (штамп поколения его спас), но
    // ЛОГИЧЕСКАЯ ранч-сессия B выпотрошена чужим teardown'ом — персонаж пропал
    // из трекера и снапшотов, остальные игроки получили ложное «B ушёл с
    // ранчо», а запись активности стёрта НАВСЕГДА: HandleClientActivity
    // намеренно update-only (см. R21-2a выше), воскресить её может только
    // повторный HandleEnterRanch. Без записи активности лобби перестаёт
    // отсрочивать таймаут осевшему на ранчо игроку (R21-4b) — и через минуту
    // кикает живого B «по сетевому таймауту».
    //
    // Поэтому: если у персонажа СУЩЕСТВУЕТ соединение НОВЕЕ просьбы, мы НЕ
    // рвём ни одного старого. Новая сессия B законно владеет ранч-состоянием
    // персонажа, а любой teardown A это состояние затрёт. Просьба относилась к
    // сессии, которой уже нет по смыслу: игрок переподключился.
    //
    // ★ПОЧЕМУ ЭТО НЕ ДЫРА В GM-БАНЕ. Для бана «новее» просто не появляется:
    // повторный вход забаненного отбивается проверкой санкции на логине, B
    // никогда не доходит до ранча, isNewerConnectionPresent остаётся false —
    // и все старые соединения рвутся как раньше. Для лобби-таймаута пропуск
    // корректен по определению: персонаж СНОВА онлайн, кикать нечего.
    //
    // ★ЦЕНА ПРОПУСКА — «висящий A»: мёртвое/залипшее соединение остаётся в
    // _clients до собственного разрыва сокета (EOF/RST даёт
    // HandleClientDisconnected) или до следующей просьбы, когда B уже уйдёт.
    // Это ограниченный по времени мусор, а не порча состояния живого игрока:
    // A ничего не рассылает и ни на что не влияет, тогда как его teardown
    // ломает игру B прямо сейчас. Разбор см. evidence/lingering-A-analysis.md.
    if (isNewerConnectionPresent)
    {
      server::util::QuietLogDebug(
        "Skipping the stale disconnect of character {} (request seq {}) — a "
        "newer session (seq {}) owns the ranch: {} stale ranch connection(s) "
        "left in place so the character-keyed ranch leave does not clobber it",
        characterUid,
        requestSeq,
        newerConnectSeq,
        staleConnections.size());
      continue;
    }

    // Клиент мог уйти сам, пока просьба лежала в очереди — это норма.
    if (staleConnections.empty())
      continue;

    // ★ШАГ 2 — РАЗРЫВ УЖЕ ВНЕ ОБХОДА map.
    for (const auto& [staleClientId, staleConnectSeq] : staleConnections)
    {
      // ★ЭТА СТРОКА — И ЕСТЬ ПРИЁМКА #96 БЕЗ ИНСТРУМЕНТАЦИИ. Форматтер лога
      // печатает [Thread %t] (main.cpp), поэтому до фикса teardown был виден с
      // лобби-потока, а после — с ранч-сетевого. Различие детерминированное и
      // читается в обычном логе стенда и прода. Номера поколений в строке —
      // приёмка реконнект-гейта: видно, что порвали именно СТАРОЕ соединение.
      server::util::QuietLogDebug(
        "Disconnecting ranch client {} (character {}, connect seq {} <= request "
        "seq {}) on the ranch network thread by request from another thread",
        staleClientId,
        characterUid,
        staleConnectSeq,
        requestSeq);

      try
      {
        _commandServer.DisconnectClient(staleClientId);
      }
      catch (const std::exception& x)
      {
        // Реестры ранчо и сетевого сервера могли разъехаться. Не даём одной
        // осечке съесть остаток очереди: она уже снята, второй попытки не будет.
        server::util::QuietLogWarn(
          "Failed to disconnect the ranch client of character {}: {}",
          characterUid,
          x.what());
      }
    }
  }
}

void RanchDirector::BroadcastSetIntroductionNotify(
  uint32_t characterUid,
  const std::string& introduction)
{
  const auto& clientContext = GetClientContextByCharacterUid(characterUid);

  protocol::RanchCommandSetIntroductionNotify notify{
    .characterUid = characterUid,
    .introduction = introduction};

  for (const ClientId& ranchClientId : _ranches[clientContext.visitingRancherUid].clients)
  {
    // Prevent broadcast to self.
    if (ranchClientId == clientContext.characterUid)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::BroadcastUpdateMountInfoNotify(
  const data::Uid characterUid,
  const data::Uid rancherUid,
  const data::Uid horseUid)
{
  const auto horseRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
    horseUid);
  // LOA-fix (SYNC-1): раньше запись брали без проверки и сразу разыменовывали.
  // Функция была мёртвым кодом, теперь её реально зовут — защищаемся.
  if (not horseRecord)
  {
    server::util::QuietLogWarn(
      "Horse record [{}] not available, skipping mount info broadcast",
      horseUid);
    return;
  }

  // LOA-fix (SYNC-1): заполняем characterUid и action. Без characterUid клиент
  // не знает, ЧЬЮ лошадь обновлять, и нотификация пропадает впустую.
  // Action::Default — тот же режим, в котором рассылается переименование
  // лошади (HandleUpdateMountNickname), т.е. проверенный на живых клиентах.
  protocol::AcCmdRCUpdateMountInfoNotify notify{
    .characterUid = characterUid,
    .action = protocol::AcCmdRCUpdateMountInfoNotify::Action::Default};
  horseRecord->Immutable([&notify](const data::Horse& horse)
  {
    protocol::BuildProtocolHorse(notify.horse, horse);
  });

  for (const ClientId& ranchClientId : _ranches[rancherUid].clients)
  {
    const auto& ranchClientContext = GetClientContext(ranchClientId);

    // Prevent broadcast to self.
    if (ranchClientContext.characterUid == characterUid)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::BroadcastMountChange(
  const ClientId clientId,
  const data::Uid previousMountUid,
  const data::Uid newMountUid)
{
  // LOA-fix (SYNC-1): игрок пересел на другую лошадь. Всем в ранче шлём
  // обновление лошади персонажа (готовый механизм, тот же, что у
  // переименования), а на СВОЁМ ранчо ещё и правим трекер лошадей инстанса:
  // новая лошадь перестаёт быть пасущимся мобом, старая — становится им.
  const auto& clientContext = GetClientContext(clientId);
  const auto characterUid = clientContext.characterUid;
  const auto rancherUid = clientContext.visitingRancherUid;

  BroadcastUpdateMountInfoNotify(characterUid, rancherUid, newMountUid);

  // Загон принадлежит владельцу ранчо: чужие лошади в трекере этого ранчо не
  // числятся, поэтому мобов трогаем только когда игрок у себя дома.
  if (rancherUid != characterUid)
    return;

  auto& ranchInstance = _ranches[rancherUid];

  // Лошадь, на которую сели, больше не пасётся — убираем моба у всех.
  const auto newMountOid = ranchInstance.tracker.GetHorseOid(newMountUid);
  if (newMountOid != tracker::InvalidEntityOid)
  {
    ranchInstance.tracker.RemoveHorse(newMountUid);

    const protocol::AcCmdRCMobDead mobDead{.mobOid = newMountOid};
    for (const ClientId& ranchClientId : ranchInstance.clients)
    {
      _commandServer.QueueCommand<protocol::AcCmdRCMobDead>(
        ranchClientId,
        [mobDead]()
        {
          return mobDead;
        });
    }
  }

  // Лошадь, с которой слезли, становится пасущейся — добавляем моба всем.
  if (previousMountUid == data::InvalidUid
    || ranchInstance.tracker.GetHorseOid(previousMountUid) != tracker::InvalidEntityOid)
  {
    return;
  }

  const auto previousMountRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
    previousMountUid);
  if (not previousMountRecord)
  {
    server::util::QuietLogWarn(
      "Previous mount record [{}] not available, skipping idle mount broadcast",
      previousMountUid);
    return;
  }

  protocol::AcCmdRCAddIdleMountInfoNotify addNotify{};
  addNotify.horse.horseOid = ranchInstance.tracker.AddHorse(previousMountUid);
  previousMountRecord->Immutable([&addNotify](const data::Horse& horse)
  {
    protocol::BuildProtocolHorse(addNotify.horse.horse, horse);
  });

  for (const ClientId& ranchClientId : ranchInstance.clients)
  {
    _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
      ranchClientId,
      [addNotify]()
      {
        return addNotify;
      });
  }
}

void RanchDirector::SummonCharacter(
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
  catch (const std::exception&)
  {
    // Dont care if the client is not found, we just won't send the notification
  }
}

void RanchDirector::SendStorageNotification(
  data::Uid characterUid,
  protocol::AcCmdCRRequestStorage::Category category)
{
  try
  {
    const auto& clientId = GetClientIdByCharacterUid(characterUid);

    // Setting pageCountAndNotification to 0b1 and category is enough
    protocol::AcCmdCRRequestStorageOK response{
      .category = category,
      .pageCountAndNotification = 0b1};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
  }
  catch (const std::exception&)
  {
  }
}

void RanchDirector::BroadcastChangeAgeNotify(
  const data::Uid characterUid,
  const data::Uid rancherUid,
  protocol::AcCmdCRChangeAge::Age age
)
{
  protocol::AcCmdRCChangeAgeNotify notify{
    .characterUid = characterUid,
    .age = age
  };

  for (const ClientId& ranchClientId : _ranches[rancherUid].clients)
  {
    const auto& ranchClientContext = GetClientContext(ranchClientId);

    // Prevent broadcast to self.
    if (ranchClientContext.characterUid == characterUid)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::BroadcastHideAgeNotify(
  const data::Uid characterUid,
  const data::Uid rancherUid,
  protocol::AcCmdCRHideAge::Option option
)
{
  protocol::AcCmdRCHideAgeNotify notify{
    .characterUid = characterUid,
    .option = option
  };

  for (const ClientId& ranchClientId : _ranches[rancherUid].clients)
  {
    const auto& ranchClientContext = GetClientContext(ranchClientId);

    // Prevent broadcast to self.
    if (ranchClientContext.characterUid == characterUid)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::BroadcastUpdateGuildMemberGradeNotify(
  data::Uid guildUid,
  data::Uid characterUid,
  protocol::GuildRole guildRole)
{
  // TODO: Identify fields
  protocol::AcCmdRCUpdateGuildMemberGradeNotify notify{
    .guildUid = guildUid,
    .unk1 = data::InvalidUid,
    .targetCharacterUid = characterUid,
    .unk3 = protocol::GuildRole::Member,
    .guildRole = guildRole
  };

  // Notify all (online) guild members
  GetServerInstance().GetDataDirector().GetGuild(guildUid).Immutable([this, &notify](const data::Guild& guild)
  {
    for (const auto& guildMember : guild.members())
    {
      // Self broadcast is needed, OK response is not sufficient
      for (auto& [clientId, clientContext] : _clients)
      {
        // Skip offline clients
        if (not clientContext.isAuthenticated)
          continue;

        // Client is not a guild member
        if (clientContext.characterUid != guildMember)
          continue;
        
        _commandServer.QueueCommand<decltype(notify)>(
          clientId,
          [notify]()
          {
            return notify;
          });
      }
    }
  });
}

void RanchDirector::SendGuildInviteDeclined(
  data::Uid characterUid,
  data::Uid inviterCharacterUid,
  std::string inviterCharacterName,
  data::Uid guildUid)
{
  // Send AcCmdCRInviteGuildJoinCancel?
  const protocol::AcCmdCRInviteGuildJoinCancel reply{
    .unk0 = characterUid,
    .unk1 = inviterCharacterUid,
    .unk2 = inviterCharacterName,
    .error = protocol::GuildError::InviteRejected,
    .unk4 = guildUid // is this true?
  };

  try
  {
    _commandServer.QueueCommand<decltype(reply)>(
      GetClientIdByCharacterUid(inviterCharacterUid),
      [reply]()
      {
        return reply;
      });
  }
  catch (const std::exception&)
  {
    // Inviter is no longer offline
    return;
  }
}

void RanchDirector::SendGuildInviteAccepted(
  const data::Uid guildUid,
  const data::Uid characterUid,
  const std::string& newMemberCharacterName)
{
  protocol::AcCmdRCAcceptGuildJoinNotify notify{
    .guildMemberCharacterUid = 0,
    .newMemberCharacterUid = characterUid,
    .newMemberCharacterName = newMemberCharacterName};
  
  // Notify (online) guild members that a new member is in
  for (const auto& client : _clients)
  {
    const auto& clientContext = client.second;
    // Notify online characters only
    if (not clientContext.isAuthenticated)
    {
      continue;
    }
    
    const auto& clientId = client.first;
    bool isCharacterInGuild = false;
    GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
      [guildUid, &isCharacterInGuild, &notify](const data::Character& character)
    {
      if (character.guildUid() == guildUid)
      {
        notify.guildMemberCharacterUid = character.uid();
        isCharacterInGuild = true;
      }
    });

    if (not isCharacterInGuild)
    {
      continue;
    }

    _commandServer.QueueCommand<decltype(notify)>(
      clientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::AddRanchHorse(
  const data::Uid rancherUid,
  const data::Uid horseUid)
{
  auto& ranchInstance = _ranches[rancherUid];
  ranchInstance.tracker.AddHorse(horseUid);
}

ServerInstance& RanchDirector::GetServerInstance()
{
  return _serverInstance;
}

Config::Ranch& RanchDirector::GetConfig()
{
  return GetServerInstance().GetSettings().ranch;
}

RanchDirector::ClientContext& RanchDirector::GetClientContext(
  const ClientId clientId,
  const bool requireAuthentication)
{
  const auto clientIter = _clients.find(clientId);
  if (clientIter == _clients.cend())
    throw std::runtime_error("Ranch client is not available");

  auto& clientContext = clientIter->second;
  if (requireAuthentication && not clientContext.isAuthenticated)
    throw std::runtime_error("Ranch client is not authenticated");

  return clientContext;
}

ClientId RanchDirector::GetClientIdByCharacterUid(data::Uid characterUid)
{
  for (auto& [clientId, clientContext] : _clients)
  {
    if (clientContext.characterUid == characterUid
      && clientContext.isAuthenticated)
      return clientId;
  }

  throw std::runtime_error("Character not associated with any client");
}

RanchDirector::ClientContext& RanchDirector::GetClientContextByCharacterUid(
  data::Uid characterUid)
{
  for (auto& clientContext : _clients | std::views::values)
  {
    if (clientContext.characterUid == characterUid
      && clientContext.isAuthenticated)
      return clientContext;
  }

  throw std::runtime_error("Character not associated with any client");
}

bool RanchDirector::HandleEnterRanch(
  ClientId clientId,
  const protocol::AcCmdCREnterRanch& command)
{
  // LOA-fix (R13-8, round13, backlog #86): КЛИЕНТ УШЁЛ, ПОКА ЖДАЛ ОТСРОЧКИ.
  // Вход на ранчо теперь штатно лежит в деферрере несколько секунд (R13-1/2/3),
  // и отключение за это время — рутина, а не аномалия. GetClientContext ниже на
  // пропавшем клиенте кидает, бросок уходит ВЫШЕ нашего try (R13-4a) прямо в
  // CommandDeferrer::Tick, а тот перебрасывает дальше — умирает весь тик
  // деферрера, включая чужие отложенные команды. Отвечать некому — выходим.
  if (not _clients.contains(clientId))
    return false;

  auto& clientContext = GetClientContext(clientId, false);

  // LOA-fix (R13-12, round13, ремонт 3): ПОВТОРНЫЙ ВХОД ОТКЛОНЯЕМ.
  // Клиент уже числится на каком-то ранчо, а просит войти снова. Штатный клиент
  // так не делает: смена ранчо идёт через разрыв сокета ранч-сервера и новое
  // соединение, а HandleClientDisconnected стирает ClientContext целиком —
  // значит на живом входе visitingRancherUid всегда InvalidUid. ★SYNC-13: и для
  // SAME-SOCKET Leave→Enter (без реконнекта) HandleRanchLeave теперь сам сбрасывает
  // visitingRancherUid→InvalidUid, так что легитимный повторный вход сюда не попадает.
  // Сюда попадают только дубликаты от битого/злонамеренного клиента, и пускать их нельзя:
  // откат упавшей второй попытки (R13-4b/R13-7) снял бы с трекера ПЕРВОЕ,
  // законное членство. Отклоняем тем же EnterRanchCancel, что и отказ по
  // замку/переполнению, — клиент получает ответ, инстанс не трогаем.
  if (clientContext.visitingRancherUid != data::InvalidUid)
  {
    server::util::QuietLogWarn(
      "Client {} requested entry into ranch {} while already registered in "
      "ranch {}; rejecting the re-entrant request",
      clientId,
      command.rancherUid,
      clientContext.visitingRancherUid);

    protocol::RanchCommandEnterRanchCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });

    return false;
  }

  const auto rancherRecord = GetServerInstance().GetDataDirector().GetCharacterCache().Get(
    command.rancherUid);
  if (not rancherRecord)
  {
    // LOA-fix (R13-6, round13, backlog #86): БЕЛЫЙ ЭКРАН ПРИ ПРЯМОМ ЗАХОДЕ.
    // Запись персонажа-ранчера тоже может быть холодной (Get на первом касании
    // ставит retrieve в очередь и возвращает nullopt). Бросок уводил управление
    // в транспортный catch — клиент не получал ни EnterRanchOK, ни Cancel.
    // Отсрочка: к следующей попытке запись обычно уже в кэше, а если ранчер
    // не читается вовсе — деферрер исчерпает попытки и пришлёт EnterRanchCancel.
    server::util::QuietLogDebug(
      "Deferring ranch entry of client {}: rancher record [{}] is not loaded yet",
      clientId,
      command.rancherUid);

    return true;
  }

  if (not clientContext.isAuthenticated)
  {
    const bool authorized = GetServerInstance().GetOtpSystem().AuthorizeCode(
      command.characterUid, command.otp);
    clientContext.isAuthenticated = authorized;

    // LOA-fix (R38-6, round38, backlog #131 SECURITY): ПРИВЯЗКА ЛИЧНОСТИ.
    // isAuthenticated — флаг УРОВНЯ СОЕДИНЕНИЯ и сам по себе НЕ доказывает, что
    // это соединение владеет command.characterUid — лишь что оно КОГДА-ТО
    // доказало владение неким UID по OTP. Привязываем проверенный UID к контексту
    // ПРЯМО ЗДЕСЬ (а не на строке `clientContext.characterUid = command.characterUid`
    // ниже, под гейтом): привязка обязана пережить даже отказ этой попытки по
    // замку/переполнению и быть готовой к гарду R38-7 и дедупу.
    if (authorized)
      clientContext.characterUid = command.characterUid;
  }

  // LOA-fix (R38-7, round38, backlog #131 SECURITY): ГАРД КРОСС-ЛИЧНОСТИ — без него
  // дедуп остаётся remote-kick primitive. Уже аутентифицированное соединение X
  // (LeaveRanch сохраняет isAuthenticated и characterUid, сбрасывая лишь
  // visitingRancherUid) могло бы прислать EnterRanch{characterUid=Y} на тёплое
  // НЕЗАЛОЧЕННОЕ ранчо: OTP-блок выше пропускается (not isAuthenticated == false),
  // reject-гейт проходит, и DedupeStaleCharacterSessions синхронно ВЫБИЛ БЫ живую
  // жертву Y. Поэтому: отклоняем любой аутентифицированный вход, чей
  // command.characterUid не совпадает с привязанной OTP-личностью. На легитимном
  // трафике одно ранч-соединение = один персонаж (даже смена ранчо через
  // Leave→Enter идёт с тем же characterUid), поэтому гард штатный трафик не трогает.
  if (clientContext.isAuthenticated
    && clientContext.characterUid != command.characterUid)
  {
    server::util::QuietLogWarn(
      "Client {} (bound identity {}) attempted ranch entry as foreign character "
      "{}; rejecting the cross-identity request",
      clientId,
      clientContext.characterUid,
      command.characterUid);

    protocol::RanchCommandEnterRanchCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });

    return false;
  }

  // Determine whether the ranch is locked.
  bool isRanchLocked = false;
  if (command.rancherUid != command.characterUid)
  {
    rancherRecord->Immutable(
      [&isRanchLocked](const data::Character& character)
      {
        isRanchLocked = character.isRanchLocked();
      });
  }

  auto& ranchInstance = _ranches[command.rancherUid];

  const bool isRanchFull = ranchInstance.clients.size() > MaxRanchCharacterCount;

  if (not clientContext.isAuthenticated
    || isRanchLocked
    || isRanchFull)
  {
    protocol::RanchCommandEnterRanchCancel response{};
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });

    return false;
  }

  // LOA-fix (R38-3, round38, backlog #131): ★ТОЧКА ДЕДУПА. Это первое место в
  // жизни ранч-соединения, где characterUid и известен, и ПОДТВЕРЖДЁН (OTP
  // проверен выше, гейт отказа уже пройден). Здесь и только здесь можно
  // схлопнуть сосуществование двух сессий одного персонажа.
  //
  // ★ПОЧЕМУ ИМЕННО ЗДЕСЬ, А НЕ ВЫШЕ. (1) Безопасность: до проверки
  // isAuthenticated дедуп стал бы remote-kick — любой сокет прислал бы
  // AcCmdCREnterRanch с чужим UID и выбил бы жертву из игры. (2) Мы НЕ рвём
  // старую сессию ради входа, который сами же отклоняем.
  //
  // ★ПОЧЕМУ ИМЕННО ЗДЕСЬ, А НЕ НИЖЕ. Ниже начинается раскладка состояния ЭТОГО
  // клиента: снимок R13-13 (wasTrackedBeforeEntry / wasClientBeforeEntry),
  // затем clientContext.characterUid, tracker.AddCharacter, clients.emplace,
  // snapshots и запись активности. Уборка старой сессии ключуется по
  // characterUid — по ОБЩЕМУ с нами ключу, — поэтому она обязана отработать
  // ЦЕЛИКОМ ДО первой нашей записи. Вызов синхронный, весь teardown
  // разворачивается внутри него, так что порядок гарантирован простым
  // расположением строки. (Именно эта инверсия и была багом #96: там teardown
  // старого приходил ПОСЛЕ раскладки нового и затирал её.)
  //
  // ★ПОБОЧНАЯ ВЫГОДА ДЛЯ СНИМКА R13-13: он считается уже ПОСЛЕ уборки, то есть
  // честно отражает «персонажа на ранчо нет», и откат в catch снимет ровно то,
  // что положила эта попытка.
  DedupeStaleCharacterSessions(clientId, command.characterUid);

  // LOA-fix (R13-4a, round13, backlog #86): РЕМЕНЬ БЕЗОПАСНОСТИ.
  // Ниже собирается ростер, который на недоступной записи (пользователь лобби,
  // персонаж, экипировка, лошадь, гильдия, питомец) кидает runtime_error. Без
  // перехвата это давало сразу две беды: клиент не получал ни OK, ни Cancel
  // (белый экран), а в инстансе оставался «призрак» — он есть в ростере у всех
  // следующих входящих, но команд не получает и держит OID.
  // Всё, что ниже, выполняется под try — откат и ответ клиенту см. в catch в
  // конце функции.
  //
  // LOA-fix (R13-13, round13, ремонт 3): СНИМОК ДО ВСТАВОК (BLOCK арбитра).
  // Откат в catch обязан снимать ТОЛЬКО то, что положила ЭТА попытка. Трекер
  // ключуется по characterUid, а список клиентов — по clientId, и оба могут
  // быть уже заняты чужой записью: второе соединение того же персонажа
  // (двойной логин, переподключение до того, как отработал разрыв старого)
  // видит СВОЙ, чистый ClientContext, поэтому гард R13-12 его не ловит.
  // Безусловный откат в таком случае выкинул бы с ранчо первое, живое
  // соединение. Снимок делается здесь, ДО try: переменные должны пережить
  // блок и быть видимы в catch. Между этой точкой и вставками
  // (tracker.AddCharacter / clients.emplace) ничего в эти контейнеры не
  // пишет — директор однопоточный, а все выходы по отсрочке лежат выше вставок.
  const bool wasTrackedBeforeEntry = ranchInstance.tracker.GetCharacters().contains(
    command.characterUid);
  const bool wasClientBeforeEntry = ranchInstance.clients.contains(clientId);

  try
  {

  // LOA-fix (R13-9, round13, ремонт 2): ЗДЕСЬ БЫЛА ЕЩЁ СТРОКА
  // `clientContext.visitingRancherUid = command.rancherUid;`. Она уехала вниз,
  // вплотную к tracker.AddCharacter. Причина: между этой точкой и вставкой в
  // ростер теперь штатно случаются отсрочки (R13-1/R13-2/R13-6) длиной в
  // секунды, и всё это время поле указывало на ранчо, в котором клиента нет.
  // Читатели поля этого не ждут: BroadcastSetIntroductionNotify и соседи берут
  // `_ranches[clientContext.visitingRancherUid]` через operator[], то есть на
  // каждом таком чтении МОЛЧА создавался пустой инстанс чужого ранчо.
  // characterUid оставлен здесь: он нужен уже следующей строкой
  // (GetUserByCharacterUid) и на чужие инстансы не указывает.
  clientContext.characterUid = command.characterUid;

  clientContext.userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;

  if (command.characterUid == command.rancherUid)
  {
    RefreshMaturingFoals(command.characterUid, clientContext);
    GetServerInstance().GetHorseSystem().RepairLineages(command.characterUid);
    // LOA-fix (R10-3, round10): ЗДЕСЬ БЫЛ ВЫЗОВ ResetDailyQuestsIfNeeded.
    // Убран: вход на ранчо происходит ПОЗЖЕ, чем клиент снял снапшот дневных
    // целей (AcCmdCLRequestDailyQuestListOK, 0x357), а сказать ему «набор
    // сброшен» протоколом нечем — сброс здесь молча расходил клиент с сервером
    // на весь день. Сброс переехал в LobbyNetworkHandler::SendLoginOK (R10-1) с
    // страховкой в HandleRequestDailyQuestList (R10-2) — обе точки ДО снапшота.
  }

  protocol::AcCmdCREnterRanchOK response{
    .rancherUid = command.rancherUid,
    .league = {
      .type = protocol::League::Type::Platinum,
      .rankingPercentile = 50}};

  // LOA-fix (R13-2a, round13, backlog #86): «жильё ранчера ещё не в кэше».
  // Решение о постройках принимается внутри лямбды (только там виден rancher),
  // а возврат из HandleEnterRanch возможен только снаружи — отсюда флаг.
  bool deferForColdHousing = false;

  rancherRecord->Immutable(
    [this, &response, &ranchInstance, &deferForColdHousing](
      const data::Character& rancher) mutable
    {
      const auto& rancherName = rancher.name();
      const bool endsWithPlural = rancherName.ends_with("s") || rancherName.ends_with("S");
      const std::string possessiveSuffix = endsWithPlural ? "'" : "'s";

      response.rancherName = rancherName;
      response.ranchName = std::format("Ранчо {}", rancherName);
      response.horseSlots = static_cast<uint8_t>(rancher.horseSlotCount());

      for (const auto& horseUid : rancher.horses())
      {
        ranchInstance.tracker.AddHorse(horseUid);
      }

      // Fill the housing info.
      const auto housingRecords = GetServerInstance().GetDataDirector().GetHousingCache().Get(
        rancher.housing());
      if (housingRecords)
      {
        for (const auto& housingRecord : *housingRecords)
        {
          housingRecord.Immutable([&response](const data::Housing& housing){

            // Certain types of housing have durability instead of expiration time.
            const bool hasDurability = (housing.housingId() == SingleIncubatorId || housing.housingId() == DoubleIncubatorId);
            if (hasDurability) 
            {
              response.incubatorUseCount = housing.durability();
              response.incubatorSlots = housing.housingId() == DoubleIncubatorId ? 2 : 1;
            }

            protocol::BuildProtocolHousing(response.housing.emplace_back(), housing, hasDurability);
          });
        }
      }
      else
      {
        // LOA-fix (R13-2b, round13, backlog #86): СИММЕТРИЯ С ЛОШАДЬМИ (R13-1).
        // Спан-перегрузка Get(KeySpan) отдаёт nullopt, если ХОТЯ БЫ ОДИН ключ
        // ещё не загружен, а жильё прелоадится только на логине владельца —
        // для оффлайнового ранчера это норма первого визита. Апстримное
        // «пропустить с предупреждением» рисовало ранчо БЕЗ построек и
        // инкубатора на весь визит; вместо этого откладываем вход — первое
        // касание Get уже поставило retrieve в очередь, к следующему тику
        // записи есть.
        // ИСКЛЮЧЕНИЕ, ЧТОБЫ БИТЫЙ ФАЙЛ НЕ ЗАПЕР РАНЧО НАВСЕГДА: упавший
        // retrieve сам НЕ повторяется (RequestRetrieve ставится только на
        // первом касании ключа), ждать нечего — пускаем игрока внутрь с
        // прежним предупреждением, как делал апстрим.
        bool hasFailedHousingRetrieval = false;
        for (const auto housingUid : rancher.housing())
        {
          if (GetServerInstance().GetDataDirector().GetHousingCache()
            .GetRetrieveFailureCount(housingUid) > 0)
          {
            hasFailedHousingRetrieval = true;
            break;
          }
        }

        if (hasFailedHousingRetrieval)
        {
          server::util::QuietLogWarn("Housing records not available for rancher {} ({})", rancherName, rancher.uid());
        }
        else
        {
          server::util::QuietLogDebug(
            "Deferring ranch entry: housing records of rancher {} ({}) are not loaded yet",
            rancherName,
            rancher.uid());

          deferForColdHousing = true;
        }
      }

      if (rancher.isRanchLocked())
        response.bitset = protocol::AcCmdCREnterRanchOK::Bitset::IsLocked;

      // Fill the incubator info.
      const auto eggRecords = GetServerInstance().GetDataDirector().GetEggCache().Get(
        rancher.eggs());
      if (eggRecords)
      {
        for (auto& eggRecord : *eggRecords)
        {
          eggRecord.Immutable(
            [this, &response](const data::Egg& egg)
            {
              // LOA-fix (R22-5, round22, SECURITY): guard BOTH the registry lookup and
              // the array index for pre-hardening data. An egg with an unregistered
              // itemTid makes GetEggInfo throw -> ranch entry aborts (permanent
              // lockout); an incubatorSlot >= 3 is an OOB write into response.incubator
              // (std::array<Egg,3>). Skip a poisoned egg instead of crashing.
              try
              {
                const registry::EggInfo eggTemplate = _serverInstance.GetPetRegistry().GetEggInfo(
                  egg.itemTid());
                const auto hatchingDuration = eggTemplate.hatchDuration;
                if (egg.incubatorSlot() < 3)
                  protocol::BuildProtocolEgg(response.incubator[egg.incubatorSlot()], egg, hatchingDuration );
                else
                  server::util::QuietLogWarn("EnterRanch: egg {} out-of-range incubator slot {}; skipping",
                    egg.uid(), egg.incubatorSlot());
              }
              catch (const std::exception&)
              {
                server::util::QuietLogWarn("EnterRanch: egg {} unregistered itemTid {}; skipping",
                  egg.uid(), egg.itemTid());
              }
            });
        }
      }
    });

  // LOA-fix (R13-2c, round13; порядок пересмотрен ремонтом 2): отсрочки по
  // холодному жилью ЗДЕСЬ НЕТ СПЕЦИАЛЬНО. Флаг deferForColdHousing едет вниз, в
  // единую фазу прогрева (R13-9), и решается одним условием вместе с лошадьми:
  // выход в этой точке делил подготовку на две последовательные фазы, а
  // деферрер отдаёт по одной попытке за тик — при нескольких гостях сразу это
  // множило ожидание белого экрана. Обе отсрочки по-прежнему срабатывают ДО
  // tracker.AddCharacter, так что «призрака» в инстансе не остаётся.

  // The character that is currently entering the ranch.
  protocol::RanchCharacter characterEnteringRanch;

  // LOA-fix (T3): раздаём лошадей в EnterRanchOK, чтобы клиент расставил их по
  // точкам спавна уровня с wander-AI. Кап на MaxRanchHorseCount (клиентский
  // стек = 10). Эти лошади пропускаются в idle-mount notify-цикле ниже.
  //
  // LOA-fix (R13-1, round13, backlog #86): БЕЛЫЙ ЭКРАН ПОРТАЛА + ПОЛНЫЙ ПРОГРЕВ.
  // Здесь был throw (унаследован от мёртвого апстримовского блока, который
  // оживил наш P3a): запись лошади оффлайнового ранчера ХОЛОДНАЯ, Get на первом
  // касании ставит retrieve в очередь и возвращает nullopt, а бросок уводил
  // управление в транспортный catch — клиент не получал ни EnterRanchOK, ни
  // Cancel и висел навсегда. Заменено на отсрочку (return true) — тот же
  // штатный контракт, которым уже пользуется idle-mount-цикл ниже.
  // КРИТИЧНО: выходим НЕ на первой холодной лошади, а ПОСЛЕ полного прохода.
  // Каждый Get() ставит СВОЙ retrieve в очередь, поэтому один проход греет весь
  // табун сразу; выход на первой холодной грел бы по одной лошади за попытку и
  // ранчо с 8+ лошадьми исчерпывало бы MaxEnterRanchDeferAttempts и получало
  // Cancel. Лошадь, чей retrieve уже упал НАВСЕГДА (счётчик неудач > 0, сам он
  // не ретраится), не ждём: исключаем её из ростера и снимаем с трекера, чтобы
  // один битый файл не запирал ранчо навечно — визит проходит без неё.
  bool deferForColdHorse = false;
  std::vector<data::Uid> unreadableHorses;

  for (auto [horseUid, horseOid] : ranchInstance.tracker.GetHorses())
  {
    auto horseRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(horseUid);
    if (not horseRecord)
    {
      if (GetServerInstance().GetDataDirector().GetHorseCache()
        .GetRetrieveFailureCount(horseUid) > 0)
      {
        // Битая насовсем. Ни лога, ни снятия с трекера ЗДЕСЬ: этот проход
        // может повториться ещё несколько раз (мы, возможно, всё равно уйдём в
        // отсрочку из-за жилья или соседней холодной лошади), и снимать/логать
        // на каждом заходе — значит жечь OID и сыпать spdlog::error пачками.
        // Обе операции делаются один раз, на финальном проходе (ниже).
        unreadableHorses.emplace_back(horseUid);
        continue;
      }

      // Холодная, но живая: касание выше уже поставило retrieve в очередь.
      // Продолжаем проход, чтобы прогреть и остальных.
      deferForColdHorse = true;
      continue;
    }

    // Кап клиентского стека. Прогрев при этом НЕ прекращаем: лошадей сверх капа
    // ниже разошлёт idle-mount-цикл, и им тоже нужен тёплый кэш.
    if (response.horses.size() >= MaxRanchHorseCount)
      continue;

    auto& ranchHorse = response.horses.emplace_back();
    ranchHorse.horseOid = horseOid;

    horseRecord->Immutable([&ranchHorse](const data::Horse& horse)
    {
      protocol::BuildProtocolHorse(ranchHorse.horse, horse);
    });
  }

  // LOA-fix (R13-9, round13, ремонт 2): МАУНТ РАНЧЕРА В ТОЙ ЖЕ ФАЗЕ ПРОГРЕВА.
  // Осёдланная лошадь лежит в character.mountUid() и в rancher.horses() НЕ
  // входит, то есть в трекер её кладут только сторонние пути (AddRanchHorse
  // при спешивании). Раз она может оказаться в GetHorses() холодной уже после
  // AddCharacter, греем её здесь же — тогда ни одна отсрочка не понадобится
  // ниже по функции.
  data::Uid rancherMountUid = data::InvalidUid;
  rancherRecord->Immutable([&rancherMountUid](const data::Character& rancher)
  {
    rancherMountUid = rancher.mountUid();
  });

  if (rancherMountUid != data::InvalidUid
    && not GetServerInstance().GetDataDirector().GetHorseCache().Get(rancherMountUid)
    && GetServerInstance().GetDataDirector().GetHorseCache()
      .GetRetrieveFailureCount(rancherMountUid) == 0)
  {
    deferForColdHorse = true;
  }

  // ЕДИНЫЙ ВЫХОД ОТСРОЧКИ (LOA-fix R13-9, ремонт 2). Жильё и лошади решаются
  // ОДНИМ условием и ОДНИМ заходом. Раньше выход по холодному жилью стоял выше
  // (сразу за лямбдой ранчера), и фазы шли последовательно: 2-3 захода
  // деферрера на прогрев жилья, потом ещё 2-3 на табун. Деферрер обрабатывает
  // ОДНУ отложенную команду за тик (1 c), поэтому при M одновременно входящих
  // гостях цена умножалась на M — 2M-3M секунд белого экрана вместо 2-3.
  if (deferForColdHousing || deferForColdHorse)
  {
    server::util::QuietLogDebug(
      "Deferring ranch entry of client {}: records of rancher {} are not loaded "
      "yet (housing cold: {}, horses cold: {})",
      clientId,
      command.rancherUid,
      deferForColdHousing,
      deferForColdHorse);

    return true;
  }

  // ФИНАЛЬНЫЙ ПРОХОД: дальше отсрочек нет, вход состоится. Только теперь
  // снимаем с трекера нечитаемых лошадей и пишем об этом в лог — по одному
  // разу на реальный вход, а не на каждую попытку.
  // Снятие ПОСЛЕ цикла: RemoveHorse рвал бы обход GetHorses(). Идемпотентно:
  // AddHorse в лямбде ранчера добавит лошадь снова на следующем входе, и она
  // снова отсеется здесь же.
  for (const auto unreadableHorseUid : unreadableHorses)
  {
    server::util::QuietLogError(
      "Ranch horse [{}] of rancher {} can not be read (retrieval failed "
      "permanently); excluding it from the ranch roster so the ranch stays "
      "visitable",
      unreadableHorseUid,
      command.rancherUid);

    ranchInstance.tracker.RemoveHorse(unreadableHorseUid);
  }

  // Add the character to the ranch.
  // LOA-fix (R13-1, round13): вставка в трекер переехала СЮДА, ниже прогрева.
  // Стоя выше, она оставляла в инстансе «призрака» на каждой отложенной
  // попытке: персонаж есть в ростере у всех следующих входящих, но команд не
  // получает и держит OID.
  // LOA-fix (R13-9, round13, ремонт 2): и «где я гощу» отмечаем ТОЖЕ здесь, а
  // не в начале функции. Пока вход лежал в деферрере, visitingRancherUid уже
  // указывал на ранчо, в котором клиента нет: широковещалки вроде
  // BroadcastSetIntroductionNotify лезли по нему в _ranches[...] и operator[]
  // плодил пустые инстансы чужих ранчо.
  clientContext.visitingRancherUid = command.rancherUid;

  // LOA-fix (R21-2e, round21, backlog #95): ЗАВОДИМ запись в реестре активности.
  // ★ЭТО ЕДИНСТВЕННОЕ МЕСТО, КОТОРОЕ СОЗДАЁТ ЗАПИСЬ. Штамп на входящих пакетах
  // (R21-2a) намеренно сделан update-only, чтобы не воскрешать записи, стёртые
  // teardown'ом с чужого потока (разбор гонки — в шапке раунда 21). Стоим сразу
  // после присвоения visitingRancherUid: выше лежат все выходы «отложить», то
  // есть вход уже зафиксирован, и гейт штампа (visitingRancherUid != InvalidUid)
  // с этого момента выполняется.
  // ПОТОК: ранч-поток и на прямом пути (обработчик AcCmdCREnterRanch в ранчевом
  // _commandServer), и на отложенном (_enterRanchDeferrer.Tick() из
  // RanchDirector::HandleNetworkTick, то есть из Server::TickLoop того же
  // ранчевого io_context). Лок ЛИСТОВОЙ, внутри одна операция с map.
  {
    std::lock_guard lock(_ranchActivityMutex);
    _ranchActivity[command.characterUid] = std::chrono::steady_clock::now();
  }

  ranchInstance.tracker.AddCharacter(
    command.characterUid);

  // Add the ranch characters.
  for (auto [characterUid, characterOid] : ranchInstance.tracker.GetCharacters())
  {
    auto& protocolCharacter = response.characters.emplace_back();
    protocolCharacter.oid = characterOid;

    auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(characterUid);
    if (not characterRecord)
      throw std::runtime_error(
        std::format("Ranch character [{}] not available", characterUid));

    characterRecord.Immutable([this, &protocolCharacter](const data::Character& character)
    {
      protocolCharacter.uid = character.uid();
      protocolCharacter.name = character.name();
      protocolCharacter.role = character.role() == data::Character::Role::GameMaster
        ? protocol::RanchCharacter::Role::GameMaster
        : character.role() == data::Character::Role::Op
          ? protocol::RanchCharacter::Role::Op
          : protocol::RanchCharacter::Role::User;
      protocolCharacter.introduction = character.introduction();

      protocol::BuildProtocolCharacter(protocolCharacter.character, character);

      // Character's equipment.
      const auto equipment = GetServerInstance().GetDataDirector().GetItemCache().Get(
        character.characterEquipment());
      if (not equipment)
      {
        throw std::runtime_error(
          std::format(
            "Ranch character's [{} ({})] equipment is not available",
            character.name(),
            character.uid()));
      }

      protocol::BuildProtocolItems(protocolCharacter.characterEquipment, *equipment);

      // Character's settings.
      const auto settingsRecord = GetServerInstance().GetDataDirector().GetSettings(
        character.settingsUid());

      if (settingsRecord)
      {
        settingsRecord.Immutable(
          [&protocolCharacter, &character](const data::Settings& settings)
        {
          if (settings.hideAge())
            return;

          protocolCharacter.age = static_cast<uint8_t>(settings.age());
          // todo: use model constant
          protocolCharacter.gender = character.parts.modelId() == 10
              ? protocol::RanchCharacter::Gender::Boy
              : protocol::RanchCharacter::Gender::Girl;
        });
      }

      // Character's mount.
      const auto mountRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
        character.mountUid());
      if (not mountRecord)
      {
        throw std::runtime_error(
          std::format(
            "Ranch character's [{} ({})] mount [{}] is not available",
            character.name(),
            character.uid(),
            character.mountUid()));
      }

      mountRecord->Immutable([&protocolCharacter](const data::Horse& horse)
      {
        protocol::BuildProtocolHorse(protocolCharacter.mount, horse);
        protocolCharacter.rent = {
          .mountUid = horse.uid(),
          .val1 = 0x12};
      });

      // Character's guild
      if (character.guildUid() != data::InvalidUid)
      {
        const auto guildRecord =  GetServerInstance().GetDataDirector().GetGuild(
          character.guildUid());
        if (not guildRecord)
        {
          throw std::runtime_error(
            std::format(
              "Ranch character's [{} ({})] guild [{}] is not available",
              character.name(),
              character.uid(),
              character.guildUid()));
        }

        guildRecord.Immutable([&protocolCharacter](const data::Guild& guild)
        {
          protocol::BuildProtocolGuild(protocolCharacter.guild, guild);
        });
      }

      // Character's pet
      if (character.petUid() != data::InvalidUid)
      {
        const auto petRecord =  GetServerInstance().GetDataDirector().GetPet(
          character.petUid());
        if (not petRecord)
        {
          throw std::runtime_error(
            std::format(
              "Ranch character's [{} ({})] pet [{}] is not available",
              character.name(),
              character.uid(),
              character.petUid()));
        }

        petRecord.Immutable([&protocolCharacter](const data::Pet& pet)
        {
          protocol::BuildProtocolPet(protocolCharacter.pet, pet);
        });
      }
    });

    // LOA-fix (SYNC-9): флаг «занят» уже находящихся на ранче. Он живёт только
    // в сессии (ClientContext::busyState, см. HandleUpdateBusyState) — в записи
    // персонажа его нет, поэтому без этого гость видит всех «свободными»
    // и, например, зовёт в заезд того, кто уже занят.
    for (const auto& otherClientContext : _clients | std::views::values)
    {
      if (otherClientContext.isAuthenticated
        && otherClientContext.characterUid == characterUid
        && otherClientContext.visitingRancherUid == command.rancherUid)
      {
        protocolCharacter.isBusy = otherClientContext.busyState;
        break;
      }
    }

    if (command.characterUid == characterUid)
    {
      characterEnteringRanch = protocolCharacter;
    }
  }

  // Build the idle-mount notifies for every ranch horse
  std::vector<protocol::AcCmdRCAddIdleMountInfoNotify> idleMountNotifies;
  for (auto [horseUid, horseOid] : ranchInstance.tracker.GetHorses())
  {
    // LOA-fix (T3): пропускаем лошадей, уже отданных в EnterRanchOK.horses
    // (у них точка спавна + wander-AI), иначе на ранчо будут дубли-призраки.
    bool alreadyPlaced = false;
    for (const auto& placedHorse : response.horses)
      if (placedHorse.horseOid == horseOid) { alreadyPlaced = true; break; }
    if (alreadyPlaced)
      continue;

    const auto horseRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(horseUid);
    if (not horseRecord)
    {
      // LOA-fix (R13-10, round13, backlog #86): БЫЛО `return true` — отсрочка
      // УЖЕ ПОСЛЕ tracker.AddCharacter, то есть каждая попытка оставляла в
      // инстансе «призрака». Прогрев всего табуна и маунта ранчера сделан выше
      // (R13-9), так что холодная запись здесь — только гонка с параллельным
      // AddRanchHorse. Пропускаем эту лошадь: вход уже состоялся, а клиент
      // получит её отдельным AddIdleMountInfoNotify.
      server::util::QuietLogWarn(
        "Ranch horse [{}] became cold while client {} was entering the ranch; "
        "skipping its idle-mount notify",
        horseUid,
        clientId);

      continue;
    }

    protocol::AcCmdRCAddIdleMountInfoNotify notify{};
    notify.horse.horseOid = horseOid;
    horseRecord->Immutable([&notify](const data::Horse& horse)
    {
      protocol::BuildProtocolHorse(notify.horse.horse, horse);
    });

    idleMountNotifies.emplace_back(std::move(notify));
  }

  // Todo: Roll the code for the connecting client.
  _commandServer.SetCode(clientId, {});
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  // Send all the ranch horses with AddIdleMountInfoNotify to the entering player.
  for (const auto& notify : idleMountNotifies)
  {
    _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
      clientId,
      [notify]()
      {
        return notify;
      });
  }

  // LOA-fix (SYNC-9): проигрываем вошедшему последний снапшот каждого, кто уже
  // на ранче. Канал тот же, что при обычном движении (RanchSnapshotNotify),
  // протокол не меняется — просто повтор последнего кадра. Без этого все
  // присутствующие висят в 0,0 до первого своего шага.
  for (const auto& [snapshotCharacterUid, snapshot] : ranchInstance.snapshots)
  {
    if (snapshotCharacterUid == command.characterUid)
      continue;

    // Снапшот адресуется по OID сущности. Если персонаж успел перезайти и
    // получил другой OID — кэш протух, пропускаем, чтобы не двинуть чужого.
    if (ranchInstance.tracker.GetCharacterOid(snapshotCharacterUid) != snapshot.ranchIndex)
      continue;

    _commandServer.QueueCommand<protocol::RanchCommandRanchSnapshotNotify>(
      clientId,
      [snapshot]()
      {
        return snapshot;
      });
  }

  // Notify to all other players of the entering player.
  protocol::RanchCommandEnterRanchNotify ranchJoinNotification{
    .character = characterEnteringRanch};

  // Iterate over all the clients connected
  // to the ranch and broadcast join notification.
  for (ClientId ranchClient : ranchInstance.clients)
  {
    _commandServer.QueueCommand<decltype(ranchJoinNotification)>(
      ranchClient,
      [ranchJoinNotification](){
        return ranchJoinNotification;
      });
  }

  ranchInstance.clients.emplace(clientId);

  // LOA-fix (R48-9, #58/R2-D): «В гости» (событие 48) — побывать на ЧУЖОМ
  // ранчо. Сервер знает это сам: в команде входа стоят и чьё ранчо
  // (rancherUid), и кто входит (characterUid), причём личность входящего
  // проверена по OTP выше по этой же функции. Стоим в самом конце — после всех
  // выходов «отложить вход»: событие обязано отмечаться на состоявшемся входе,
  // а не на каждой попытке прогреть кэш. Свой собственный вход, который
  // случается на каждом логине, не считается.
  if (command.rancherUid != command.characterUid)
    SendAchievementEvent(command.characterUid, 48);

  return false;

  }
  catch (const std::exception& x)
  {
    // LOA-fix (R13-4b, round13, backlog #86): ОТКАТ ЧАСТИЧНОГО ВХОДА.
    // Снимаем персонажа с трекера и из списка клиентов инстанса, сбрасываем
    // «где я гощу» и отвечаем клиенту штатным EnterRanchCancel — тем же
    // пакетом, которым отвечают отказ по замку/переполнению и исчерпанная
    // отсрочка. Возвращаем false: повторять нечего, ответ клиенту уже ушёл.
    //
    // LOA-fix (R13-13, round13, ремонт 3): откат СТРОГО ПО СНИМКУ. Снимаем
    // только те записи, которых до этой попытки НЕ БЫЛО (снимок взят перед
    // try). Раньше откат был безусловным и предполагал, что любая запись в
    // трекере/списке клиентов — наша: неверно, если тот же персонаж уже сидит
    // на этом ранчо со второго соединения (двойной логин). Тогда безусловный
    // откат выкидывал с ранчо живого игрока.
    if (not wasTrackedBeforeEntry)
      ranchInstance.tracker.RemoveCharacter(command.characterUid);
    if (not wasClientBeforeEntry)
      ranchInstance.clients.erase(clientId);

    // LOA-fix (R13-9, round13, ремонт 2): сброс «где я гощу» — ТОЛЬКО если поле
    // указывает именно на это ранчо (тот же гард, что в R13-7). Безусловный
    // сброс затирал бы предыдущее, полностью законное ранчо клиента, если бы
    // вход сюда упал ДО присвоения — тогда HandleRanchLeave и Disconnect
    // чистили бы уже не тот инстанс.
    if (clientContext.visitingRancherUid == command.rancherUid)
      clientContext.visitingRancherUid = data::InvalidUid;

    server::util::QuietLogError(
      "Ranch entry of client {} (character {}) into ranch {} failed: {}",
      clientId,
      command.characterUid,
      command.rancherUid,
      x.what());

    protocol::RanchCommandEnterRanchCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });

    return false;
  }
}

void RanchDirector::HandleRanchLeave(ClientId clientId)
{
  // LOA-fix (SYNC-13): контекст НЕ const — в конце сбрасываем visitingRancherUid.
  auto& clientContext = GetClientContext(clientId);

  // LOA-fix (R38-8, round38, backlog #131 SECURITY): КЛИЕНТ НИКОГДА НЕ ВХОДИЛ НА РАНЧО.
  // visitingRancherUid ставится ниже точки входа (рядом с tracker.AddCharacter), а
  // reject-гейт и гард R38-7 выходят ДО него — значит сокет, отбитый по замку/
  // переполнению или кросс-личностному гарду, приходит сюда с visitingRancherUid ==
  // InvalidUid. Такому клиенту нечего убирать, и — главное — у него НЕТ СВОЕЙ записи
  // активности: безусловный erase по characterUid ниже стёр бы запись ДРУГОЙ живой
  // сессии того же персонажа (это стало достижимо, когда R38-6 стал привязывать
  // characterUid к отбитому сокету) → лобби перестаёт откладывать таймаут → живого
  // игрока выкидывает ~через минуту. Выходим сразу: на ранчо этот сокет не значился.
  // ★#95/R21-2c СОХРАНЁН: клиент, который ВХОДИЛ (visitingRancherUid != InvalidUid),
  // проходит гард и чистит активность даже если инстанс ранчо уже снят.
  if (clientContext.visitingRancherUid == data::InvalidUid)
    return;

  // LOA-fix (R21-2c, round21, backlog #95): штатный выход с ранча снимает запись
  // активности. ★В НАЧАЛЕ функции, а не в конце: ниже есть early-return «ранчо не
  // инстанцировано», и стирание в конце этот путь бы пропустило.
  {
    std::lock_guard lock(_ranchActivityMutex);
    _ranchActivity.erase(clientContext.characterUid);
  }

  const auto ranchIter = _ranches.find(clientContext.visitingRancherUid);
  if (ranchIter == _ranches.cend())
  {
    server::util::QuietLogWarn(
      "Client {} tried to leave a ranch of {} which is not instanced",
      clientId,
      clientContext.visitingRancherUid);
    return;
  }

  auto& ranchInstance = ranchIter->second;

  ranchInstance.tracker.RemoveCharacter(clientContext.characterUid);
  ranchInstance.clients.erase(clientId);
  // LOA-fix (SYNC-9): снимаем кэш положения — иначе вошедшим позже проигрывался
  // бы снапшот уже ушедшего персонажа (и OID успел бы уехать другому).
  ranchInstance.snapshots.erase(clientContext.characterUid);

  protocol::AcCmdCRLeaveRanchOK response{};
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  protocol::AcCmdCRLeaveRanchNotify notify{
    .characterId = clientContext.characterUid};

  for (const ClientId& ranchClientId : ranchInstance.clients)
  {
    if (ranchClientId == clientId)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }

  // LOA-fix (SYNC-13): сбрасываем visitingRancherUid при выходе. Закрывает
  // SAME-CONNECTION Leave→Enter (иначе гард R13-12 отверг бы легитимный повторный
  // вход) и исходный баг SYNC-11. R13-12 полагается на wipe контекста в
  // HandleClientDisconnected — верно только для reconnect-пути, не для Leave→Enter
  // в одной сессии.
  clientContext.visitingRancherUid = data::InvalidUid;
}

void RanchDirector::HandleChat(
  ClientId clientId,
  const protocol::AcCmdCRRanchChat& chat)
{
  const auto& clientContext = GetClientContext(clientId);

  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);
  const auto rancherRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.visitingRancherUid);

  const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];

  std::string characterName;
  characterRecord.Immutable([&characterName](const data::Character& character)
  {
    characterName = character.name();
  });

  std::string ranchersName;
  rancherRecord.Immutable([&ranchersName](const data::Character& rancher)
  {
    ranchersName = rancher.name();
  });

  const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;

  const auto sendAllMessages = [this](
    const ClientId clientId,
    const std::string& sender,
    const bool isSystem,
    const std::vector<std::string>& messages)
  {
    protocol::AcCmdCRRanchChatNotify notify{
      .author = not isSystem ? sender : "",
      .isSystem = isSystem};

    for (const auto& resultMessage : messages)
    {
      notify.message = resultMessage;
      _commandServer.QueueCommand<decltype(notify)>(
        clientId,
        [notify](){ return notify; });
    }
  };

  // Perform moderation and check for any mute ban
  const auto chatVerdict = _serverInstance.GetChatSystem().ProcessChatMessage(
    clientContext.characterUid,
    chat.message);

  // LOA-fix (R55-6, round55, backlog #179 часть 5): см. R55-3.
  if (not chatVerdict)
    return;

  const auto& verdict = *chatVerdict;

  // Process commands, even if user has a mute ban
  if (verdict.commandVerdict)
  {
    sendAllMessages(clientId, characterName, true, verdict.commandVerdict->result);
    return;
  }

  // Message is not a command, check if user has been muted
  if (verdict.isMuted)
  {
    if (verdict.isPrevented)
    {
      server::util::QuietLogInfo("[{}'s ranch] (prevented) {} ({}): {}",
        ranchersName,
        characterName,
        userName,
        chat.message);
    }
    else
    {
      server::util::QuietLogInfo("[{}'s ranch] (muted) {} ({}): {}",
        ranchersName,
        characterName,
        userName,
        chat.message);
    }
    protocol::AcCmdCRRanchChatNotify notify{
      .author   = verdict.isPrevented ? "AutoMod" : "System",
      .message  = verdict.message,
      .isSystem = true};
    _commandServer.QueueCommand<decltype(notify)>(clientId, [notify](){ return notify; });
    return;
  }

  server::util::QuietLogInfo("[{}'s ranch] {} ({}): {}",
    ranchersName,
    characterName,
    userName,
    chat.message);

  for (const auto& ranchClientId : ranchInstance.clients)
  {
    sendAllMessages(ranchClientId, characterName, false, {verdict.message});
  }
}

// LOA-fix (R25, #104): троттл warn'а о стейл-снапшоте. 0x139 (ранчо-позиции) —
// поток ~10-30/с; при reconnect-storm незадросселированный warn был бы шумнее
// прежнего throw. Глобально одна строка / 5с (текст всё равно именует клиента и
// OID). Global static, НЕ per-client: per-client map рос бы неограниченно ровно
// на reconnect-churn (= триггере гарда) — плохой cost/benefit для P3-косметики.
static bool ShouldWarnStaleSnapshot()
{
  static std::atomic<std::time_t> lastWarn{0};
  const std::time_t now = std::time(nullptr);
  std::time_t prev = lastWarn.load(std::memory_order_relaxed);
  if (now - prev < 5)
    return false;
  return lastWarn.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}

void RanchDirector::HandleSnapshot(
  ClientId clientId,
  const protocol::AcCmdCRRanchSnapshot& command)
{
  const auto& clientContext = GetClientContext(clientId);
  // LOA-fix (SYNC-9): инстанс больше не const — в него пишется кэш снапшотов.
  auto& ranchInstance = _ranches[clientContext.visitingRancherUid];

  protocol::RanchCommandRanchSnapshotNotify notify{
    .ranchIndex = ranchInstance.tracker.GetCharacterOid(
      clientContext.characterUid),
    .type = command.type,
  };

  switch (command.type)
  {
    case protocol::AcCmdCRRanchSnapshot::Full:
    {
      if (command.full.ranchIndex != notify.ranchIndex)
      {
        // LOA-fix (R25, #104): стейл-снапшот (клиентский OID != серверный)
        // отклоняем ГРАЦИОЗНО. Прежний throw ловился общим catch в CommandServer
        // и печатался как "Unhandled exception" — шум уровня error на штатном
        // reconnect-окне (после reconnect'а клиент получил новый oid, а снапшот в
        // полёте нёс старый). Дропаем ровно этот пакет: трекер/инстанс не тронуты,
        // клиент самовосстановится на следующем снапшоте.
        if (ShouldWarnStaleSnapshot())
          server::util::QuietLogWarn(
            "Dropping stale ranch snapshot from client {} "
            "(claimed entity {}, server entity {})",
            clientId, command.full.ranchIndex, notify.ranchIndex);
        return;
      }
      notify.full = command.full;
      break;
    }
    case protocol::AcCmdCRRanchSnapshot::Partial:
    {
      // TODO(#105): Partial-ветка сверяет .full.ranchIndex (всегда 0 — Read()
      // заполняет только АКТИВНЫЙ вариант); правка на .partial активировала бы
      // никогда-не-исполнявшийся 0x13a partial-broadcast — SYNC-домен, оценивать
      // ОТДЕЛЬНЫМ деплоем. В R25 поле НЕ трогаем (zero behavior change).
      if (command.full.ranchIndex != notify.ranchIndex)
      {
        // LOA-fix (R25, #104): грациозный дроп вместо throw — см. Full-ветку.
        if (ShouldWarnStaleSnapshot())
          server::util::QuietLogWarn(
            "Dropping stale ranch snapshot from client {} "
            "(claimed entity {}, server entity {})",
            clientId, command.full.ranchIndex, notify.ranchIndex);
        return;
      }
      notify.partial = command.partial;
      break;
    }
  }

  // LOA-fix (SYNC-9): запоминаем последнее известное положение персонажа,
  // чтобы отдать его тем, кто войдёт на ранчо позже.
  //
  // LOA-fix (SYNC-9c, adversarial round): кадр с нулевым OID В КЭШ НЕ КЛАДЁМ.
  // notify.ranchIndex = tracker.GetCharacterOid(...), и ноль здесь — это
  // tracker::InvalidEntityOid, ровно тот же ответ, который трекер даёт для НЕ
  // отслеживаемого персонажа. Такая запись ломает гард протухания SYNC-9e
  // (сравнение GetCharacterOid == snapshot.ranchIndex превращается в 0 == 0 и
  // всегда проходит), и кадр «ни про кого» проигрывался бы каждому следующему
  // гостю ранча. Достижимо: кикнутого снимают с трекера, но его
  // visitingRancherUid не сбрасывают — его следующий снапшот приходит в тот же
  // инстанс уже с нулевым OID. Живую рассылку ниже не трогаем — она была такой
  // и до батча.
  if (notify.ranchIndex != tracker::InvalidEntityOid)
    ranchInstance.snapshots[clientContext.characterUid] = notify;

  for (const auto& ranchClient : ranchInstance.clients)
  {
    // Do not broadcast to the client that sent the snapshot.
    if (ranchClient == clientId)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClient,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::HandleEnterBreedingMarket(
  const ClientId clientId,
  const protocol::AcCmdCREnterBreedingMarket&)
{
  const auto& clientContext = GetClientContext(clientId);

  // The breeding market is where a short lineage is visible, so correct the listed
  // horses before building the response.
  GetServerInstance().GetHorseSystem().RepairLineages(clientContext.characterUid);

  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::RanchCommandEnterBreedingMarketOK response;

  characterRecord.Immutable(
    [this, &response](const data::Character& character)
    {
      // Include all horses in the response
      auto horses = character.horses();
      horses.emplace_back(character.mountUid());

      const auto horseRecords = GetServerInstance().GetDataDirector().GetHorseCache().Get(
        horses);

      for (const auto& horseRecord : *horseRecords)
      {
        auto& protocolHorse = response.stallions.emplace_back();

        // Get the horse data (EnterBreedingMarket has simpler struct)
        bool isRegistered = false;
        horseRecord.Immutable([this, &protocolHorse, &isRegistered](const data::Horse& horse)
        {
          protocolHorse.uid = horse.uid();
          protocolHorse.tid = horse.tid();
          protocolHorse.breedingCombo = static_cast<uint8_t>(
            horse.breedingCombo());
          protocolHorse.lineage = static_cast<uint8_t>(
            horse.lineage());

          // Keep track of whether this horse is a registered stallion
          isRegistered = _breedingMarket.IsRegistered(horse.uid());
        });

        if (not isRegistered)
          continue;

        // Get stallion data and populate the expiresAt field
        const auto& stallionData = _breedingMarket.GetStallionData(protocolHorse.uid);
        if (not stallionData.has_value())
          // Some fatal error occurred, this horse is a stallion but no stallion data
          throw std::runtime_error("Horse is a registered stallion but no stallion data");

        const uint32_t stallionUid = stallionData.value().stallionUid;
        GetServerInstance().GetDataDirector().GetStallion(stallionUid).Immutable(
          [&protocolHorse](const data::Stallion& stallion)
          {
            protocolHorse.expiresAt = util::TimePointToAliciaTime(stallion.expiresAt());
          });
      }
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleSearchStallion(
  const ClientId clientId,
  const protocol::AcCmdCRSearchStallion& command)
{
  const auto& clientContext = GetClientContext(clientId);
  clientContext;

  BreedingMarket::SnapshotFilter snapshotFilter{
    .grade = command.grade};

  for (const auto coatUid : command.filterCoats)
  {
    // todo: verify coat
    snapshotFilter.coats.insert(coatUid);
  }

  for (const auto maneUid : command.filterManes)
  {
    // todo: verify mane
    snapshotFilter.manes.insert(maneUid);
  }

  for (const auto tailUid : command.filterTails)
  {
    // todo: verify mane
    snapshotFilter.tails.insert(tailUid);
  }

  snapshotFilter.firstPreferredStat = ConvertProtocolStallionStatToSnapshotStat(
    command.firstRequiredStat);
  snapshotFilter.secondPreferred = ConvertProtocolStallionStatToSnapshotStat(
    command.secondRequiredStat);

  const auto snapshotOrder = ConvertProtocolStallionOrderToSnapshotOrder(
    command.order);

  // todo: cache this
  const auto result = _breedingMarket.CollectMarketSnapshot(
    snapshotOrder,
    snapshotFilter);

  constexpr size_t StallionsPerPage = 8;
  const auto pages = std::views::chunk(result.registrations, StallionsPerPage);

  // Client sends page number, convert that to an index and sanitize it within page bounds.
  //
  // LOA-fix (R53-X, round53, backlog #179 часть 3): верхней границей была
  // `pages.size()`, то есть индекс мог стать РАВНЫМ числу страниц, и
  // `pages[pageIndex]` ниже читал ЗА КОНЦОМ — `chunk_view` границы не
  // проверяет. Номер страницы приходит от клиента, так что это достижимо
  // крафтом пакета или устаревшей страницей после того, как список сократился.
  const size_t pageIndex = pages.empty()
    ? size_t{0}
    : std::min(static_cast<size_t>(pages.size()) - 1u, static_cast<size_t>(command.page - 1));

  protocol::RanchCommandSearchStallionOK response{
    .page = static_cast<uint32_t>(pageIndex + 1),
    .pageCount = static_cast<uint32_t>(pages.size())};

  if (not pages.empty())
  {
    const auto& page = pages[pageIndex];
    for (const auto& registration : page)
    {
      const auto horseRecord = _serverInstance.GetDataDirector().GetHorse(
        registration.horseUid);
      const auto stallionRecord = _serverInstance.GetDataDirector().GetStallion(
        registration.stallionUid);

      if (not horseRecord || not stallionRecord)
        continue;

      auto& protocolStallion = response.stallions.emplace_back();
      horseRecord.Immutable([&protocolStallion](const data::Horse& horse)
      {
        protocolStallion.uid = horse.uid();
        protocolStallion.tid = horse.tid();
        protocolStallion.name = horse.name();
        protocolStallion.grade = static_cast<uint8_t>(horse.grade());

        protocol::BuildProtocolHorseParts(protocolStallion.parts, horse.parts);
        protocol::BuildProtocolHorseAppearance(protocolStallion.appearance, horse.appearance);
        protocol::BuildProtocolHorseStats(protocolStallion.stats, horse.stats);

        protocolStallion.pregnancyChance = horse.breedingCount();
        protocolStallion.heritability = 0;
        // todo: figure out unk11
        protocolStallion.unk11 = 0;
        protocolStallion.lineage = static_cast<uint8_t>(horse.lineage());
      });

      data::Uid ownerUid = data::InvalidUid;
      stallionRecord.Immutable([&protocolStallion, &ownerUid](const data::Stallion& stallion)
      {
        protocolStallion.breedFee = stallion.breedingCharge();
        protocolStallion.expiresAt = stallion.expiresAt();
        ownerUid = stallion.ownerUid();
      });

      const auto ownerRecord = _serverInstance.GetDataDirector().GetCharacter(ownerUid);
      if (ownerRecord)
      {
        ownerRecord.Immutable([&protocolStallion](const data::Character& character)
        {
          protocolStallion.owner = character.name();
        });
      }
    }
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRegisterStallion(
  const ClientId clientId,
  const protocol::AcCmdCRRegisterStallion& command)
{
  const auto& clientContext = GetClientContext(clientId);

  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
      clientContext.characterUid);

  const bool isStallionRegistered = _breedingMarket.HandleRegisterStallion(
    clientContext.characterUid,
    command.horseUid,
    command.breedingFee);

  [[unlikely]] if (not isStallionRegistered)
  {
    SendRegisterStallionCancel(clientId);
    return;
  }

  // Get the current carrot balance of the character.
  int32_t carrotBalance{};
  characterRecord.Immutable([&carrotBalance](const data::Character& character)
  {
    carrotBalance = character.carrots();
  });

  protocol::AcCmdCRRegisterStallionOK response{
    .carrotBalance = carrotBalance};

  // LOA-fix (R48-7, #58/R2-D): «Отдать всего себя» (событие 13) — выставить
  // своего жеребца на рынок разведения. Решение принято сервером двумя шагами
  // выше (_breedingMarket.HandleRegisterStallion вернул true, иначе мы уже
  // ушли бы в Cancel), так что событие подтверждено фактом, а не запросом.
  SendAchievementEvent(clientContext.characterUid, 13);

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::SendRegisterStallionCancel(const ClientId clientId)
{
  _commandServer.QueueCommand<protocol::RanchCommandRegisterStallionCancel>(
    clientId,
    []()
    {
      return protocol::RanchCommandRegisterStallionCancel{};
    });
}

void RanchDirector::HandleUnregisterStallion(
  const ClientId clientId,
  const protocol::AcCmdCRUnregisterStallion& command)
{
  const auto& clientContext = GetClientContext(clientId);

  const bool isStallionUnregistered = _breedingMarket.HandleUnregisterStallion(
    clientContext.characterUid,
    command.horseUid);

  [[unlikely]] if (not isStallionUnregistered)
  {
    SendUnregisterStallionCancel(clientId);
    return;
  }

  protocol::AcCmdCRUnregisterStallionOK response{};
  _commandServer.QueueCommand<decltype(response)>(clientId, [response]()
  {
    return response;
  });
}

void RanchDirector::SendUnregisterStallionCancel(const ClientId clientId)
{
  _commandServer.QueueCommand<protocol::AcCmdCRUnregisterStallionCancel>(
    clientId,
    []()
    {
      return protocol::AcCmdCRUnregisterStallionCancel{};
    });
}

void RanchDirector::HandleUnregisterStallionEstimateInfo(
  const ClientId clientId,
  const protocol::AcCmdCRUnregisterStallionEstimateInfo& command)
{
  const auto estimate = _breedingMarket.CalculateUnregisterEarnings(
    command.horseUid);
  
  [[unlikely]] if (not estimate)
  {
    _commandServer.QueueCommand<protocol::AcCmdCRUnregisterStallionEstimateInfoCancel>(
      clientId,
      [] { return protocol::AcCmdCRUnregisterStallionEstimateInfoCancel{}; });
    return;
  }

  // todo: Figure out member1 and member4 of AcCmdCRUnregisterStallionEstimateInfoOK
  protocol::AcCmdCRUnregisterStallionEstimateInfoOK response{
    .timesMated = estimate->timesMated,
    .earnings = estimate->revenue,
    .breedingFee = estimate->breedingFee};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]() { return response; });
}

void RanchDirector::HandleCheckStallionCharge(
  const ClientId clientId,
  const protocol::AcCmdCRCheckStallionCharge& command)
{
  const auto horseRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
    command.horseUid);

  if (not horseRecord)
    return;

  uint32_t horseGrade = 0;
  uint32_t horseBreeds = 0;
  horseRecord->Immutable([&horseGrade, &horseBreeds](const data::Horse& horse)
  {
    horseGrade = horse.grade();
    horseBreeds = horse.breedingCount();
  });

  const auto gradeFeeRange = _breedingMarket.GetGradeFeeRange(horseGrade);

  [[unlikely]] if (not gradeFeeRange)
  {
    protocol::AcCmdCRCheckStallionChargeOK response{
      .hasFailed = true};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response] { return response; });
    return;
  }

  // Validate and return breeding charge information
  protocol::AcCmdCRCheckStallionChargeOK response{
    .hasFailed = false,
    .minFee = gradeFeeRange->min,
    .maxFee = gradeFeeRange->max,
    .breedCount = horseBreeds,
    .member5 = 0};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

bool RanchDirector::HandleTryBreeding(
  const ClientId clientId,
  const protocol::AcCmdCRTryBreeding& command)
{
  auto& clientContext = GetClientContext(clientId);
  auto& dataDirector = GetServerInstance().GetDataDirector();

  // Hard cancel (resultCode 0, i.e. not the consolation path) for validation failures,
  // so the client doesn't hang.
  using CancelReason = protocol::RanchCommandTryBreedingCancel::CancelReason;
  const auto sendBreedingCancel = [this, clientId](CancelReason reason)
  {
    const protocol::RanchCommandTryBreedingCancel cancel{.resultCode = reason};
    _commandServer.QueueCommand<protocol::RanchCommandTryBreedingCancel>(
      clientId, [cancel]() { return cancel; });
  };

  const auto mareRecord = dataDirector.GetHorseCache().Get(command.mareUid);
  const auto stallionRecord = dataDirector.GetHorseCache().Get(command.stallionUid);
  if (not mareRecord || not stallionRecord)
  {
    server::util::QuietLogWarn("TryBreeding: mare {} or stallion {} not found",
      command.mareUid, command.stallionUid);
    sendBreedingCancel(CancelReason::GenericError);
    clientContext.tryBreedingDeferAttempts = 0;
    return false;
  }

  if (not GetServerInstance().GetGenetics().IsAncestryResident(
    command.mareUid, command.stallionUid))
  {
    if (++clientContext.tryBreedingDeferAttempts < MaxTryBreedingDeferAttempts)
      return true;

    server::util::QuietLogWarn(
      "TryBreeding: ancestry of mare {} and stallion {} still incomplete after {} attempts, "
      "breeding with the records at hand",
      command.mareUid, command.stallionUid, MaxTryBreedingDeferAttempts);
  }
  clientContext.tryBreedingDeferAttempts = 0;

  // The stallion must be registered in the breeding market.
  const auto stallionData = _breedingMarket.GetStallionData(command.stallionUid);
  if (not stallionData)
  {
    server::util::QuietLogWarn("TryBreeding: stallion {} is not registered in the breeding market",
      command.stallionUid);
    sendBreedingCancel(CancelReason::StallionNotFound);
    return false;
  }

  // Charge the breeding fee.
  const auto characterRecord = dataDirector.GetCharacter(clientContext.characterUid);
  bool charged = false;
  bool sufficientHorseSlots = false;
  characterRecord.Mutable([&charged, &stallionData, &sufficientHorseSlots](data::Character& character)
  {
    // Check if this character has enough space for a new horse
    
    // Horses in inventory + current mount
    size_t currentHorseCount = character.horses().size() + 1;
    if (currentHorseCount + 1 > character.horseSlotCount())
      return;
    sufficientHorseSlots = true;

    const auto fee = static_cast<int32_t>(stallionData->breedingCharge);
    if (character.carrots() < fee)
      return;
    character.carrots() = character.carrots() - fee;
    charged = true;
  });

  if (not sufficientHorseSlots)
  {
    server::util::QuietLogWarn("TryBreeding: character {} has insufficient horse slots",
      clientContext.characterUid, stallionData->breedingCharge);
    sendBreedingCancel(CancelReason::InsufficientHorseSlots);
    return false;
  }

  if (not charged)
  {
    server::util::QuietLogWarn("TryBreeding: character {} cannot afford breeding fee {}",
      clientContext.characterUid, stallionData->breedingCharge);
    sendBreedingCancel(CancelReason::InsufficientBalance);
    return false;
  }

  // Read the stallion grade and breeding count needed for the success roll.
  uint32_t stallionGrade = 0;
  uint32_t stallionBreedingCount = 0;
  stallionRecord->Immutable([&stallionGrade, &stallionBreedingCount](const data::Horse& stallion)
  {
    stallionGrade = stallion.grade();
    stallionBreedingCount = stallion.breedingCount();
  });

  const protocol::BreedingBonus bonus = RollBreedingBonus(stallionGrade);
  const uint32_t successRate = CalculateBreedingSuccessRate(
    stallionGrade, stallionBreedingCount, bonus);

  std::uniform_int_distribution<uint32_t> successRoll(1, 100);
  const bool success = successRoll(server::util::GetRandomEngine()) <= successRate;

  if (not success)
  {
    server::util::QuietLogInfo("TryBreeding: failed (grade={}, count={}, rate={}%)",
      stallionGrade, stallionBreedingCount, successRate);
  }
  else
  {
    server::util::QuietLogInfo("TryBreeding: succeeded (grade={}, count={}, rate={}%)",
      stallionGrade, stallionBreedingCount, successRate);
  }

  const auto applyBreedingAttemptUpdates = [&]()
  {
    mareRecord->Mutable([success](data::Horse& mare)
    {
      mare.breedingCombo() = success ? mare.breedingCombo() + 1 : 0;
    });

    // The stallion's lifetime and market counters advance for any paid attempt.
    stallionRecord->Mutable([](data::Horse& stallion)
    {
      stallion.breedingCount() = stallion.breedingCount() + 1;
    });

    if (const auto stallionDbRecord = dataDirector.GetStallionCache().Get(stallionData->stallionUid))
    {
      stallionDbRecord->Mutable([](data::Stallion& stallion)
      {
        stallion.timesMated() = stallion.timesMated() + 1;
      });
    }
  };

  if (success)
  {
    protocol::RanchCommandTryBreedingOK response{};
    const data::Uid foalUid = CreateBredFoal(clientId, clientContext, command, bonus, response);

    characterRecord.Mutable([foalUid, &response](data::Character& character)
    {
      character.horses().emplace_back(foalUid);
      response.carrots = character.carrots();
    });

    clientContext.maturingFoals.emplace(
      foalUid, data::Clock::now() + HorseSystem::FoalGrowUpDuration);

    applyBreedingAttemptUpdates();

    server::util::QuietLogInfo("TryBreeding: created foal {}", foalUid);

    // LOA-fix (R48-5, #58/R2-D): удачная случка двигает ДВА достижения —
    // «Жажда любви» (событие 10 — сама оплаченная попытка) и «Доктор любовных
    // наук» (11 — успех). Оба измерены сервером целиком: плату он снял выше по
    // этой функции, бросок сделал сам, жеребёнка создал сам. Клиент в решении
    // не участвует и сообщить о нём не может.
    SendAchievementEvent(clientContext.characterUid, 10);
    SendAchievementEvent(clientContext.characterUid, 11);

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]() { return response; });
    return false;
  }

  applyBreedingAttemptUpdates();

  // LOA-fix (R48-6, #58/R2-D): неудачная случка — тоже попытка (событие 10) и
  // вдобавок «Горький вкус жизни» (12). Считаем именно ОПЛАЧЕННУЮ попытку:
  // сюда приходят только те, у кого списана плата и сделан бросок, а отказы по
  // деньгам, слотам и незарегистрированному производителю ушли выше по return.
  SendAchievementEvent(clientContext.characterUid, 10);
  SendAchievementEvent(clientContext.characterUid, 12);

  clientContext.hasPendingFailureCard = true;
  clientContext.pendingFailureCardSpend = stallionData->breedingCharge;

  protocol::RanchCommandTryBreedingCancel response{
    .resultCode = CancelReason::ShowBreedingFailureCards};
  characterRecord.Immutable([&response](const data::Character& character)
  {
    response.carrots = character.carrots();
  });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  return false;
}

protocol::BreedingBonus RanchDirector::RollBreedingBonus(const uint32_t stallionGrade)
{
  const auto& breedingRegistry = GetServerInstance().GetBreedingRegistry();
  const auto& smallBand = breedingRegistry.GetSmallGradeBonusBand();
  const auto& bigBand = breedingRegistry.GetBigGradeBonusBand();

  const bool isSmall = stallionGrade >= smallBand.minGrade && stallionGrade <= smallBand.maxGrade;
  const bool isBig = stallionGrade >= bigBand.minGrade && stallionGrade <= bigBand.maxGrade;
  if (not isSmall && not isBig)
    return {};

  // Roll whether a bonus activates at all.
  const int32_t activationChance = isSmall ? smallBand.activationChance : bigBand.activationChance;
  std::uniform_int_distribution<int32_t> activationRoll(1, 100);
  if (activationRoll(server::util::GetRandomEngine()) > activationChance)
    return {};

  // Build the selection weights for the active grade band.
  const auto& entries = breedingRegistry.GetBonusEntries();
  std::vector<int32_t> weights;
  weights.reserve(entries.size());
  int32_t weightSum = 0;
  for (const auto& entry : entries)
  {
    const int32_t weight = isSmall ? entry.ratioSmall : entry.ratioBig;
    weights.push_back(weight);
    weightSum += weight;
  }

  if (weightSum <= 0)
    return {};

  std::discrete_distribution<size_t> bonusDist(weights.begin(), weights.end());
  const auto& selected = entries[bonusDist(server::util::GetRandomEngine())];

  server::util::QuietLogInfo("TryBreeding: rolled bonus id {} (type {}, value {}) for grade {} ({} band)",
    selected.id, selected.type, selected.value, stallionGrade, isSmall ? "small" : "big");

  return protocol::BreedingBonus{
    .id = selected.id,
    .type = selected.type,
    .value = selected.value};
}

uint32_t RanchDirector::CalculateBreedingSuccessRate(
  const uint32_t stallionGrade,
  const uint32_t stallionBreedingCount,
  const protocol::BreedingBonus& bonus)
{
  const auto& horseRegistry = GetServerInstance().GetHorseRegistry();
  const auto& params = GetServerInstance().GetBreedingRegistry().GetBreedingParams();

  // Base success rate comes from the stallion grade (horses.yaml -> grades.pregnantValue).
  int32_t rate = params.minSuccessRate;
  if (const auto* gradeInfo = horseRegistry.GetGradeInfo(stallionGrade))
    rate = gradeInfo->pregnantValue;

  // Each prior breeding lowers the rate, floored at the configured minimum.
  rate -= static_cast<int32_t>(stallionBreedingCount) * params.successDecayPerBreeding;
  rate = std::max(rate, params.minSuccessRate);

  // A type-0 bonus increases the pregnancy success rate.
  if (bonus.type == 0)
    rate += static_cast<int32_t>(bonus.value);

  return static_cast<uint32_t>(std::clamp(rate, 0, 100));
}

data::Uid RanchDirector::CreateBredFoal(
  const ClientId clientId,
  const ClientContext& clientContext,
  const protocol::AcCmdCRTryBreeding& command,
  const protocol::BreedingBonus& bonus,
  protocol::RanchCommandTryBreedingOK& response)
{
  auto& serverInstance = GetServerInstance();
  auto& dataDirector = serverInstance.GetDataDirector();
  auto& genetics = serverInstance.GetGenetics();

  // A fertility-peak bonus (type 1) adds to the foal's grade; genetics owns the rest.
  const uint32_t gradeBonus = bonus.type == 1 ? bonus.value : 0;

  const auto foalRecord = dataDirector.CreateHorse();
  data::Uid foalUid = data::InvalidUid;

  foalRecord.Mutable([&](data::Horse& foal)
  {
    genetics.CreateFoal(foal, command.mareUid, command.stallionUid, gradeBonus);
    foalUid = foal.uid();

    // Populate the response from the freshly bred foal.
    response.item = protocol::Item{
      .uid = foal.uid(),
      .tid = foal.tid(),
      .expiresAt = 0,
      .count = 1};
    response.grade = static_cast<uint8_t>(foal.grade());
    protocol::BuildProtocolHorseParts(response.parts, foal.parts);
    protocol::BuildProtocolHorseAppearance(response.appearance, foal.appearance);
    protocol::BuildProtocolHorseStats(response.stats, foal.stats);
    response.breedingBonus = bonus;
    response.tendency = static_cast<uint8_t>(foal.tendency());
    response.potentialType = static_cast<uint8_t>(foal.potential.type());
    response.lineage = static_cast<uint8_t>(foal.lineage());
    response.emblemId = static_cast<uint16_t>(foal.emblemUid());
  });

  // Register the freshly bred foal with the ranch and spawn it for everyone
  // present (the owner included, since it isn't on their ranch view yet).
  AddRanchHorse(clientContext.characterUid, foalUid);

  protocol::AcCmdRCAddIdleMountInfoNotify addNotify{};
  addNotify.horse.horseOid =
    _ranches[clientContext.characterUid].tracker.GetHorseOid(foalUid);
  foalRecord.Immutable([&addNotify](const data::Horse& horse)
  {
    protocol::BuildProtocolHorse(addNotify.horse.horse, horse);
  });

  if (clientContext.visitingRancherUid == clientContext.characterUid)
  {
    for (const ClientId& ranchClientId : _ranches[clientContext.characterUid].clients)
    {
      _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
        ranchClientId,
        [addNotify]()
        {
          return addNotify;
        });
    }
  }
  else
  {
    _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
      clientId,
      [addNotify]()
      {
        return addNotify;
      });

    const protocol::AcCmdRCMobDead mobDead{.mobOid = addNotify.horse.horseOid};
    _commandServer.QueueCommand<protocol::AcCmdRCMobDead>(
      clientId,
      [mobDead]()
      {
        return mobDead;
      });
  }

  return foalUid;
}

void RanchDirector::HandleBreedingAbandon(
  const ClientId clientId,
  const protocol::AcCmdCRBreedingAbandon& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto& characterRecord = GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid);

  // Check if character owns the horse
  bool hasFoal = false;
  characterRecord.Immutable(
    [&hasFoal, foalUid = command.foalUid](const data::Character& character)
    {
      hasFoal = std::ranges::contains(character.horses(), foalUid);
    });

  const protocol::AcCmdCRBreedingAbandonCancel cancel{};
  if (not hasFoal)
  {
    _commandServer.QueueCommand<protocol::AcCmdCRBreedingAbandonCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  // Check if the horse is a foal
  bool isFoal = false;
  GetServerInstance().GetDataDirector().GetHorse(command.foalUid).Immutable(
    [&isFoal](const data::Horse& horse)
    {
      // Check if character owns the horse
      isFoal = horse.type() == data::Horse::Type::Foal;
    });

  if (not isFoal)
  {
    _commandServer.QueueCommand<protocol::AcCmdCRBreedingAbandonCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  ReturnHorseToNature(
    clientContext.characterUid,
    command.foalUid,
    clientContext.userName,
    true);

  const protocol::AcCmdCRBreedingAbandonOK response{};
  _commandServer.QueueCommand<protocol::AcCmdCRBreedingAbandonOK>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleBreedingWishlist(
  const ClientId clientId,
  const protocol::AcCmdCRBreedingWishlist&)
{
  const auto& clientContext = GetClientContext(clientId);

  std::vector<data::Uid> wishlist{};
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&wishlist](const data::Character& character)
    {
      wishlist = std::vector<data::Uid>{
        character.breedingWishlist().cbegin(),
        character.breedingWishlist().cend()};
    });

  const auto& horseRecords = GetServerInstance().GetDataDirector().GetHorseCache().Get(wishlist);
  if (not horseRecords.has_value())
  {
    const protocol::AcCmdCRBreedingWishlistCancel cancel{};
    _commandServer.QueueCommand<protocol::AcCmdCRBreedingWishlistCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  protocol::AcCmdCRBreedingWishlistOK response{};
  using FavouritedStallion = protocol::AcCmdCRBreedingWishlistOK::FavouritedStallion;

  size_t count = 0;
  for (const auto& horseRecord : *horseRecords)
  {
    // Max 8 stallions in a wishlist
    if (count >= 8)
      break;

    horseRecord.Immutable([this, &response](const data::Horse& horse)
    {
      auto& favouritedStallion = response.wishlist.emplace_back(FavouritedStallion{
        .uid = horse.uid(),
        .tid = horse.tid(),
        .grade = static_cast<uint8_t>(horse.grade()),
        .name = horse.name(),
        .heritability = 0, // Keep this 0, the client automatically derives it
        .breedingCount = horse.breedingCount(),
        .unk7 = 0,
        .unk8 = 0,
        .registrationEnded = true,
        .unk10 = 0,
        .lineage = static_cast<uint8_t>(horse.lineage())
      });

      protocol::BuildProtocolHorseStats(favouritedStallion.stats, horse.stats);
      protocol::BuildProtocolHorseParts(favouritedStallion.parts, horse.parts);
      protocol::BuildProtocolHorseAppearance(favouritedStallion.appearance, horse.appearance);

      // Check if this horse is a stallion, else we are done with this horse
      const auto& stallionDataResult = _breedingMarket.GetStallionData(horse.uid());
      if (not stallionDataResult.has_value())
        return;
      
      const BreedingMarket::StallionData& stallionData = stallionDataResult.value();
      favouritedStallion.registrationEnded = false;
      favouritedStallion.breedingFee = stallionData.breedingCharge;
      
      data::Uid ownerUid{data::InvalidUid};
      GetServerInstance().GetDataDirector().GetStallion(stallionData.stallionUid).Immutable(
        [&favouritedStallion, &ownerUid](const data::Stallion& stallion)
        {
          ownerUid = stallion.ownerUid();
          favouritedStallion.expiresAt = util::TimePointToAliciaTime(stallion.expiresAt());
        });

      GetServerInstance().GetDataDirector().GetCharacter(ownerUid).Immutable(
        [&favouritedStallion](const data::Character& character)
        {
          favouritedStallion.ownerName = character.name();
        });
    });

    count++;
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleBreedingFailureCard(
  const ClientId clientId,
  const protocol::AcCmdCRBreedingFailureCard&)
{
  auto& clientContext = GetClientContext(clientId);

  // Only show the card if there's a pending failure card from breeding
  [[unlikely]] if (not clientContext.hasPendingFailureCard)
    return;

  // Roll the card type (Chance/yellow vs Normal/red) now that the client is asking for it,
  // and remember it so the subsequent Choose draws from the matching reward table.
  const auto& params = GetServerInstance().GetBreedingRegistry().GetBreedingParams();
  std::uniform_int_distribution<int32_t> cardRoll(1, 100);
  clientContext.pendingCardType = cardRoll(server::util::GetRandomEngine()) <= params.chanceCardChance
    ? protocol::BreedingFailureCardType::Yellow
    : protocol::BreedingFailureCardType::Red;

  protocol::AcCmdCRBreedingFailureCardOK response{
    .cardType = clientContext.pendingCardType};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]
    {
      return response;
    });
}

void RanchDirector::HandleBreedingFailureCardChoose(
  const ClientId clientId,
  const protocol::AcCmdCRBreedingFailureCardChoose& command)
{
  server::util::QuietLogInfo("BreedingFailureCardChoose: statusOrFlag = {}", command.statusOrFlag);

  auto& clientContext = GetClientContext(clientId);

  // LOA-fix (R7 WARN-2, round7): награду карты разведения можно было получить
  // ПОВТОРНО, с приходом морковок каждый раз.
  // ЧТО БЫЛО НЕ ТАК: (а) обработчик вообще не проверял pending-флаг (в отличие
  // от HandleBreedingFailureCard, где гард есть), (б) флаг снимался в САМОМ
  // КОНЦЕ — уже ПОСЛЕ начисления морковок и выдачи предмета. Любой бросок между
  // ними оставлял флаг взведённым, а бросок реален: GetItem(InvalidUid)
  // .Immutable кидает std::runtime_error, а InvalidUid стал ЛЕГАЛЬНЫМ
  // результатом AddItem с раунда 3 (A6).
  // ТЕПЕРЬ: карта «расходуется» ДО любого начисления (fail-closed — тот же
  // приём, что у F2 с dailyRewardClaimed). Нет карты — нет выдачи; бросок при
  // выдаче максимум лишает награды, но НИКОГДА не даёт повторить приход.
  [[unlikely]] if (not clientContext.hasPendingFailureCard)
  {
    server::util::QuietLogWarn(
      "BreedingFailureCardChoose: character {} claimed a breeding failure card "
      "reward without having one pending",
      clientContext.characterUid);
    return;
  }
  clientContext.hasPendingFailureCard = false;

  // Reward grade scales with the fee paid for the failed breeding that earned this card.
  const uint32_t moneySpent = clientContext.pendingFailureCardSpend;
  const auto& probEntry = GetServerInstance().GetBreedingRegistry().GetFailureCardProb(moneySpent);

  std::uniform_int_distribution<int> gradeDist(1, 100);
  int gradeRoll = gradeDist(server::util::GetRandomEngine());

  int rewardGrade = 0;
  if (gradeRoll <= probEntry.probA) {
    rewardGrade = 0;
  } else if (gradeRoll <= probEntry.probA + probEntry.probB) {
    rewardGrade = 1;
  } else {
    rewardGrade = 2;
  }

  // Use the card type that was already determined in HandleBreedingFailureCard
  const bool isChanceCard = clientContext.pendingCardType == protocol::BreedingFailureCardType::Yellow;

  auto& breedingRegistry = GetServerInstance().GetBreedingRegistry();

  uint32_t rewardId = 0;
  const registry::FailureCardReward* rewardData = nullptr;

  if (isChanceCard) {
    const auto* gradeRange = breedingRegistry.GetChanceCardGradeRange(rewardGrade);
    if (gradeRange)
    {
      std::uniform_int_distribution<uint32_t> chanceDist(gradeRange->minId, gradeRange->maxId);
      rewardId = chanceDist(server::util::GetRandomEngine());
      rewardData = breedingRegistry.GetChanceCardReward(rewardId);
    }
  } else {
    const auto* gradeRange = breedingRegistry.GetNormalCardGradeRange(rewardGrade);
    if (gradeRange)
    {
      std::uniform_int_distribution<uint32_t> normalDist(gradeRange->minId, gradeRange->maxId);
      rewardId = normalDist(server::util::GetRandomEngine());
      rewardData = breedingRegistry.GetNormalCardReward(rewardId);
    }
  }

  static const registry::FailureCardReward fallbackReward = {45001, 1, 120};
  if (!rewardData) {
    rewardData = &fallbackReward;
  }

  data::Uid itemUid{data::InvalidUid};
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [this, &itemUid, &rewardData](data::Character& character)
    {
      character.carrots() += rewardData->gameMoney;
      itemUid = GetServerInstance().GetItemSystem().AddItem(
        character,
        rewardData->itemTid,
        rewardData->itemCount);
    });

  protocol::AcCmdCRBreedingFailureCardChooseOK response{
    .isChanceCard = isChanceCard,
    .rewardId = rewardId,
    .member4 = {},
    .rewardedCarrots = rewardData->gameMoney};

  // LOA-fix (R7 WARN-2, round7): AddItem легально возвращает data::InvalidUid
  // (раунд 3 / A6), а Record::Immutable на недоступной записи БРОСАЕТ
  // std::runtime_error — тот самый бросок, который раньше оставлял pending-флаг
  // взведённым и делал начисление повторяемым. Флаг теперь снят выше, а здесь
  // просто не заполняем поле предмета в ответе и пишем улику в лог.
  if (itemUid != data::InvalidUid)
  {
    GetServerInstance().GetDataDirector().GetItem(itemUid).Immutable(
      [&response](const data::Item& item)
      {
        protocol::BuildProtocolItem(response.item, item);
      });
  }
  else
  {
    server::util::QuietLogError(
      "BreedingFailureCardChoose: failed to grant item '{}' x{} to character {}; "
      "only the carrots were awarded",
      rewardData->itemTid,
      rewardData->itemCount,
      clientContext.characterUid);
  }

  server::util::QuietLogInfo("BreedingFailureCard: {} CARD (Grade {})! MoneySpent: {}, GradeRoll: {}, RewardId {}, gave {} carrots + item {} x{}",
    isChanceCard ? "CHANCE (YELLOW)" : "NORMAL (RED)",
    rewardGrade, moneySpent, gradeRoll, rewardId,
    rewardData->gameMoney, rewardData->itemTid, rewardData->itemCount);

  // LOA-fix (R7 WARN-2, round7): флаг снимается ВВЕРХУ обработчика, до любого
  // начисления (см. комментарий там). Здесь снимать поздно: бросок при выдаче
  // предмета оставлял бы состояние повторяемым, а повтор — с приходом морковок.

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleCmdAction(
  ClientId clientId,
  const protocol::AcCmdCRRanchCmdAction& command)
{
  // LOA-recon (SYNC-4): dump the raw emote/gesture request.
  //
  // Emotes are NOT broadcast yet, on purpose. The notify opcode exists
  // (AcCmdCRRanchCmdActionNotify, 0x1ca) but its struct is undecoded - three
  // unnamed fields {unk0, unk1, unk2} and a hardcoded {2, 3, 1} reply marked
  // "TODO: Actual implementation of it" (identical in current upstream master).
  // Critically, that struct carries NO actor identity (no ranch oid, no
  // characterUid), unlike every other ranch notify, so relaying it to guests
  // would animate whatever entity happens to own oid 2 - and RanchTracker hands
  // oids out to horses as well as characters, so on your own ranch that is a
  // HORSE, not the person who waved. Shipping that would be a louder regression
  // than the current silence.
  //
  // So instead we log the evidence needed to decode the packet: the leading
  // uint16 the client sends, plus the opaque trailing payload. Once a couple of
  // known gestures are captured here, the actual broadcast is ~15 lines - a copy
  // of the HandleSnapshot loop below.
  {
    // LOA-fix (SYNC-4, adversarial round): requireAuthentication=false.
    // По умолчанию GetClientContext БРОСАЕТ runtime_error для неаутентифициро-
    // ванного клиента, а этот обработчик достижим ДО входа на ранчо (глобального
    // auth-гейта нет, гейтом служит сам GetClientContext в каждом обработчике).
    // Исключение улетало бы в диспетчер, тот его глотает с error-логом и НЕ шлёт
    // ничего — то есть разведочный лог менял бы ответ сервера, хотя операция
    // заявлена как «поведение байт-в-байт как в проде». Разведке аутентификация
    // не нужна: пустой контекст просто даст InvalidUid в логе.
    const auto& reconContext = GetClientContext(clientId, false);

    // LOA-fix (SYNC-4, adversarial round): find(), а НЕ operator[]. На
    // unordered_map operator[] ВСТАВЛЯЕТ элемент: клиент без ранча
    // (visitingRancherUid == InvalidUid — например, аутентифицированный, но
    // отменённый на входе замком/переполнением) плодил бы фантомный
    // _ranches[InvalidUid] на каждом эмоут-пакете.
    auto reconCharacterOid = tracker::InvalidEntityOid;
    const auto reconRanchIter = _ranches.find(reconContext.visitingRancherUid);
    if (reconRanchIter != _ranches.cend())
    {
      reconCharacterOid = reconRanchIter->second.tracker.GetCharacterOid(
        reconContext.characterUid);
    }

    std::string payloadHex;
    payloadHex.reserve(command.snapshot.size() * 2);
    for (const auto payloadByte : command.snapshot)
      payloadHex += std::format("{:02x}", static_cast<unsigned>(payloadByte));

    server::util::QuietLogDebug(
      "SYNC-4 recon: RanchCmdAction from character {} (ranch oid {}) unk0={} payload[{}]={}",
      reconContext.characterUid,
      reconCharacterOid,
      command.unk0,
      command.snapshot.size(),
      payloadHex);
  }

  protocol::RanchCommandRanchCmdActionNotify response{
    .unk0 = 2,
    .unk1 = 3,
    .unk2 = 1,};

  // TODO: Actual implementation of it
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

// LOA-fix (R40-1, round40, backlog #129 S4b): троттл warn'а о запрошенном
// с провода начислении морковок. Идиома скопирована с ShouldWarnStaleSnapshot
// (:2543, R25/#104): глобально одна строка / 5 c, без per-client карты —
// иначе спам-пакетами растили бы карту ровно на триггере гарда.
static bool ShouldWarnRanchStuff()
{
  static std::atomic<std::time_t> lastWarn{0};
  const std::time_t now = std::time(nullptr);
  std::time_t prev = lastWarn.load(std::memory_order_relaxed);
  if (now - prev < 5)
    return false;
  return lastWarn.compare_exchange_strong(prev, now, std::memory_order_relaxed);
}

void RanchDirector::HandleRanchStuff(
  ClientId clientId,
  const protocol::RanchCommandRanchStuff& command)
{
  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  if (not characterRecord)
  {
    throw std::runtime_error(
      std::format("Character [{}] not available", clientContext.characterUid));
  }

  protocol::RanchCommandRanchStuffOK response{
    command.eventId,
    0};

  // LOA-fix (R40-1, round40, backlog #129 S4b, SECURITY/ECONOMY): ★ПРЯМОЕ
  // НАЧИСЛЕНИЕ ДЕНЕГ С ПРОВОДА. БЫЛО ровно `character.carrots() += command.value`
  // под апстримовым комментарием «Todo: needs validation». Хендлер
  // ЗАРЕГИСТРИРОВАН (RanchDirector.cpp:326-330, опкод AcCmdCRRanchStuff 0x1af),
  // проходит только гейт аутентификации — то есть ЛЮБОЙ вошедший игрок одним
  // пакетом выписывал себе произвольную сумму. Хуже: data::Character::carrots
  // это int32_t (DataDefinitions.hpp:204), а command.value — int32_t с провода,
  // поэтому `+=` это ещё и знаковое переполнение = UB.
  // Сервер НЕ ЗНАЕТ, сколько стоит ranch-событие: серверного реестра
  // ranch-событий не существует, авторитетной суммы вывести не из чего.
  // Поэтому fail-closed: обычному игроку начисление НЕ производится вовсе
  // (отвечаем OK с moneyIncrement = 0 и настоящим балансом — протокол не
  // ломается, клиент просто видит ноль). Для ПЕРСОНАЛА (role != User)
  // оставляем начисление как отладочный инструмент владельца, но с клампом в
  // int64 и потолком 2'000'000'000 < INT32_MAX — переполнения нет ни при каком
  // value.
  // ★УСЛОВИЕ ПРАВ = ровно `role() != Role::User`, БЕЗ roleRank. Это устоявшаяся
  // идиома admin-гейта в этой кодовой базе (ChatSystem.cpp, RanchDirector.cpp,
  // RaceNetworkHandler.cpp — везде `isAdmin = character.role() != Role::User`,
  // а LobbyNetworkHandler.cpp так же считает hasPermission; ни один из этих
  // гейтов roleRank не смотрит). И это единственный вариант, совместимый с
  // реальным аккаунтом владельца в проде: character 1 (Nmax) имеет role = 2
  // (GameMaster), а поля roleRank в его записи НЕТ вовсе → оно дефолтится в
  // RoleRank::None. Черновой вариант с `&& roleRank() == RoleRank::Admin`
  // обнулил бы 0x1af именно владельцу — то есть сломал бы ровно тот инструмент,
  // ради которого админская ветка и оставлена.
  // ★ЕСЛИ преflight-грепом прод-лога докажется, что реальный клиент шлёт 0x1af
  // в честной игре, — заменить эту ветку на серверный реестр ranch-событий
  // (eventId -> фиксированная награда), а не на «клампнутое доверие проводу».
  characterRecord.Mutable([&command, &response](data::Character& character)
  {
    const bool isAdmin = character.role() != data::Character::Role::User;

    if (isAdmin && command.value > 0)
    {
      // ★R40-1 iter2 (Codex-T3 BLOCK): грант считаем ТОЛЬКО на здравом текущем
      // балансе [0, 2e9]. Legacy-запись ВНЕ диапазона (испорчена до-R40 overflow'ом)
      // НЕ трогаем: иначе delta = clamped - current мог бы вылезти из int32 (напр.
      // current = INT32_MIN, value = 1 -> delta = 2^31 -> narrowing-overflow в
      // moneyIncrement; либо current = 2e9+1 -> отрицательный increment). Такой
      // баланс лечится вручную (//set carrots), а не этим путём — fail-closed.
      // Грант ограничиваем свободным местом до потолка: grant = min(value,
      // 2e9 - current) ∈ [0, 2e9] -> и moneyIncrement, и новый carrots влезают в
      // int32 ПО ПОСТРОЕНИЮ, без клампа-разности.
      const int64_t current = static_cast<int64_t>(character.carrots());
      if (current >= 0 && current <= 2000000000)
      {
        const int64_t grant = std::min<int64_t>(
          static_cast<int64_t>(command.value), 2000000000 - current);
        response.moneyIncrement = static_cast<int32_t>(grant);
        character.carrots() = static_cast<int32_t>(current + grant);
      }
    }

    response.totalMoney = character.carrots();
  });

  if (response.moneyIncrement == 0 && command.value != 0 && ShouldWarnRanchStuff())
  {
    server::util::QuietLogWarn(
      "Character {} requested an unvalidated ranch-stuff carrot grant "
      "(eventId {}, value {}) - refused",
      clientContext.characterUid,
      command.eventId,
      command.value);
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]
    {
      return response;
    });
}

void RanchDirector::HandleUpdateBusyState(
  ClientId clientId,
  const protocol::RanchCommandUpdateBusyState& command)
{
  auto& clientContext = GetClientContext(clientId);
  auto& ranchInstance = _ranches[clientContext.visitingRancherUid];

  protocol::RanchCommandUpdateBusyStateNotify response {
    .characterUid = clientContext.characterUid,
    .busyState = command.busyState};

  clientContext.busyState = command.busyState;

  for (auto ranchClientId : ranchInstance.clients)
  {
    // Do not broadcast to self.
    if (ranchClientId == clientId)
      continue;

    _commandServer.QueueCommand<decltype(response)>(
      ranchClientId,
      [response]()
      {
        return response;
      });
  }
}



void RanchDirector::HandleUpdateMountNickname(
  ClientId clientId,
  const protocol::AcCmdCRUpdateMountNickname& command)
{
  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // Collect the owned horses by the user's character.
  std::vector<data::Uid> ownedHorses;
  characterRecord.Mutable([&ownedHorses](data::Character& character)
  {
    ownedHorses.emplace_back(character.mountUid());
    std::ranges::copy(character.horses(), std::back_inserter(ownedHorses));
  });

  const bool isHorseOwned = std::ranges::contains(ownedHorses, command.horseUid);
  if (not isHorseOwned)
  {
    SendUpdateMountNicknameCancel(
      clientId,
      protocol::HorseNicknameUpdateError::ServerError);
    return;
  }

  const auto horseRecord = GetServerInstance().GetDataDirector().GetHorse(
    command.horseUid);
  if (not horseRecord)
  {
    SendUpdateMountNicknameCancel(
      clientId,
      protocol::HorseNicknameUpdateError::ServerError);
    return;
  }

  const bool isNameValid = locale::IsNameValid(command.name);
  const auto moderationVerdict = _serverInstance.GetModerationSystem().Moderate(
    command.name);

  if (not isNameValid || moderationVerdict.isPrevented)
  {
    SendUpdateMountNicknameCancel(
      clientId,
      protocol::HorseNicknameUpdateError::InvalidNickname);
    return;
  }

  bool requireItem = true;
  horseRecord.Immutable([&requireItem](const data::Horse& horse)
  {
    // If the horse name is empty we do not require item to rename the horse.
    // This only applies for prologue.
    requireItem = not horse.name().empty();
  });

  uint32_t remainingItemCount = 0;

  if (requireItem)
  {
    bool itemConsumed = false;
    characterRecord.Mutable([this, &itemConsumed, &remainingItemCount](data::Character& character)
    {
      constexpr data::Tid HorseRenameItemTid = 45003;

      // todo: To reconsider, the client sends us UID of the item that was used
      //       to rename the horse. This would allow us to not remember `HorseRenameItemTid` and
      //       to use the item UID to find the item.

      const auto consumeResult = GetServerInstance().GetItemSystem().ConsumeItem(
        character, HorseRenameItemTid, 1);
      itemConsumed = consumeResult.itemConsumed;
      remainingItemCount = consumeResult.remainingItemCount;
    });

    if (not itemConsumed)
    {
      SendUpdateMountNicknameCancel(
        clientId,
        protocol::HorseNicknameUpdateError::NoHorseRenameItem);
      // LOA-fix (R18-1, quest-batch-2): БЕСПЛАТНОЕ ПЕРЕИМЕНОВАНИЕ ЛОШАДИ.
      // Здесь не было `return;`: сервер отправлял клиенту Cancel «нет предмета
      // 45003» — и тут же переименовывал лошадь ниже по функции, рассылая новое
      // имя всему ранчу. То есть предмет за деньги был чисто декоративным.
      // Парная ветка переименования ПЕРСОНАЖА (HandleChangeNickname) в этом же
      // файле возвращается из точно такой же проверки — зеркалим её.
      return;
    }
  }

  protocol::AcCmdRCUpdateMountInfoNotify notify{
    .characterUid = clientContext.characterUid};

  std::string currentName{};
  horseRecord.Mutable(
    [&notify, &currentName, horseName = command.name](data::Horse& horse)
    {
      currentName = horse.name();
      horse.name() = horseName;
      protocol::BuildProtocolHorse(notify.horse, horse);
    });

  // LOA-fix (R48-8, #58/R2-D): «Первое имя» (событие 47) — именно ДАТЬ имя, а
  // не переименовать. Разницу сервер видит без клиента и уже ею пользуется:
  // пустое имя выше по функции освобождает от предмета переименования, и та же
  // улика — прежнее имя — отвечает, было ли оно вообще. Переименования не
  // считаем сознательно: иначе все четыре тира берутся кручением одной лошади.
  if (currentName.empty())
    SendAchievementEvent(clientContext.characterUid, 47);

  {
    const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;
    server::util::QuietLogInfo("User '{}' changed the name of a horse ({}) from '{}' to '{}'",
      userName,
      command.horseUid,
      currentName,
      command.name);
  }

  protocol::AcCmdCRUpdateMountNicknameOK response{
    .horseUid = command.horseUid,
    .nickname = command.name,
    .itemUid = command.itemUid,
    .itemCount = remainingItemCount};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  for (const ClientId& ranchClientId : _ranches[clientContext.visitingRancherUid].clients)
  {
    // Prevent broadcast to self.
    if (ranchClientId == clientId)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::SendUpdateMountNicknameCancel(
  const ClientId clientId,
  const protocol::HorseNicknameUpdateError reason)
{
  _commandServer.QueueCommand<protocol::AcCmdCRUpdateMountNicknameCancel>(
      clientId,
      [reason]()
      {
        return protocol::AcCmdCRUpdateMountNicknameCancel{
          .error = reason};
      });
}

void RanchDirector::HandleRequestStorage(
  ClientId clientId,
  const protocol::AcCmdCRRequestStorage& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCRRequestStorageOK response{
    .category = command.category,
    .page = command.page};

  const bool showPurchases = command.category == protocol::AcCmdCRRequestStorage::Category::Purchases;

  // Fill the stored items, either from the purchase category or the gift category.

  characterRecord.Immutable(
    [this, showPurchases, page = static_cast<size_t>(command.page), &response](
      const data::Character& character) mutable
    {
      const auto storedItemRecords = GetServerInstance().GetDataDirector().GetStorageItemCache().Get(
        showPurchases ? character.purchases() : character.gifts());
      if (not storedItemRecords || storedItemRecords->empty())
        return;

      const auto pagination = std::views::chunk(*storedItemRecords, 5);
      page = std::max(std::min(page - 1, pagination.size() - 1), size_t{0});

      response.pageCountAndNotification = static_cast<uint16_t>(
        pagination.size() << 2);

      protocol::BuildProtocolStorageItems(response.storedItems, pagination[page]);
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleGetItemFromStorage(
  ClientId clientId,
  const protocol::AcCmdCRGetItemFromStorage& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  bool isStorageItemValid = true;
  // LOA-fix (R7 WARN-1, round7): запоминаем, из КАКОГО списка отвязали посылку,
  // чтобы вернуть её на место, если выдача предметов не состоится (см. хвост
  // обработчика).
  bool takenFromGifts = false;

  // Try to remove the storage item from the character.
  characterRecord.Mutable(
    [&isStorageItemValid, &takenFromGifts, storageItemUid = command.storageItemUid](
      data::Character& character)
    {
      // The stored item is either a gift or a purchase.

      const auto storedGiftIter = std::ranges::find(character.gifts(), storageItemUid);
      if (storedGiftIter != character.gifts().cend())
      {
        character.gifts().erase(storedGiftIter);
        takenFromGifts = true;
        return;
      }

      const auto storedPurchaseIter = std::ranges::find(character.purchases(), storageItemUid);
      if (storedPurchaseIter != character.purchases().cend())
      {
        character.purchases().erase(storedPurchaseIter);
        return;
      }

      isStorageItemValid = false;
    });

  const auto storageItemRecord = GetServerInstance().GetDataDirector().GetStorageItemCache().Get(
      command.storageItemUid);
  if (isStorageItemValid)
    isStorageItemValid = storageItemRecord.has_value();

  // If the stored item is invalid cancel the takeout.
  if (not isStorageItemValid)
  {
    protocol::AcCmdCRGetItemFromStorageCancel response{
      .storageItemUid = command.storageItemUid,
      .status = 0};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  protocol::AcCmdCRGetItemFromStorageOK response{
    .storageItemUid = command.storageItemUid};

  std::vector<data::StorageItem::Item> collectedItems;
  int32_t collectedCarrots{};
  // LOA-fix (R7 WARN-1, round7): предметы посылки, которые выдать НЕ удалось
  // (AddItem вернул data::InvalidUid — легальный исход с раунда 3 / A6).
  std::vector<data::StorageItem::Item> undeliveredItems;

  storageItemRecord->Immutable(
    [&collectedItems, &collectedCarrots](const data::StorageItem& storedItem)
    {
      collectedItems = storedItem.items();
      collectedCarrots = storedItem.carrots();
    });

  characterRecord.Mutable(
    [this, &collectedItems, &collectedCarrots, &undeliveredItems, &response](
      data::Character& character)
    {
      // Add the collected items.
      std::vector<data::Uid> itemUids;
      for (const auto& collectedItem : collectedItems)
      {
        const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(
          collectedItem.tid);
        if (not itemTemplate)
        {
          // LOA-fix (R8-2, round8): здесь стоял голый `continue`, и предмет с
          // неизвестным сервером tid (снятым или переименованным в items.yaml)
          // исчезал вместе с посылкой — её удаление ниже было безусловным, а в
          // undeliveredItems (R7 WARN-1) такой предмет не попадал. Кладём его
          // туда: посылка сохранится с остатком и переживёт правку реестра.
          // LOA-fix (R9-3, round9): было spdlog::error на КАЖДЫЙ предмет
          // посылки с неизвестным tid. Это не поломка сервера, а расхождение
          // с items.yaml: при снятии/переименовании тиров одна правка конфига
          // заливает лог error'ами (по строке на предмет в каждой посылке), и
          // настоящие ошибки в нём тонут. Предмет при этом не теряется —
          // посылка сохраняется с остатком (R8-2). Уровень warn.
          server::util::QuietLogWarn(
            "Stored item '{}' of character '{}' is not in the item registry; "
            "keeping the parcel instead of dropping the item",
            collectedItem.tid,
            character.name());
          undeliveredItems.push_back(collectedItem);
          continue;
        }

        auto itemUid = data::InvalidUid;
        if (itemTemplate->type == registry::Item::Type::Temporary)
        {
          itemUid = _serverInstance.GetItemSystem().AddItem(
            character,
            collectedItem.tid,
            collectedItem.duration);
        }
        else
        {
          itemUid = _serverInstance.GetItemSystem().AddItem(
            character,
            collectedItem.tid,
            collectedItem.count);
        }

        // LOA-fix (WARN3, round6): AddItem с раунда 3 (A6) легально возвращает
        // data::InvalidUid при неудаче создания записи. Такой uid нельзя класть
        // в itemUids: GetItemCache().Get(...) вернёт nullopt на всю пачку, и
        // разыменование ниже станет UB (а игрок потерял бы и остальные предметы
        // из посылки).
        if (itemUid == data::InvalidUid)
        {
          server::util::QuietLogError(
            "Failed to grant stored item '{}' to character '{}'",
            collectedItem.tid,
            character.name());
          // LOA-fix (R7 WARN-1, round7): запоминаем невыданное. Раньше здесь
          // предмет просто исчезал: посылка ниже удалялась безусловно.
          undeliveredItems.push_back(collectedItem);
          continue;
        }

        itemUids.emplace_back(itemUid);
      }

      const auto itemRecords = _serverInstance.GetDataDirector().GetItemCache().Get(itemUids);
      if (itemRecords)
      {
        protocol::BuildProtocolItems(response.items, *itemRecords);
      }
      else
      {
        server::util::QuietLogError(
          "Failed to read back the stored items granted to character '{}'; "
          "the takeout response will not list them",
          character.name());
      }

      // Add the collected carrots.
      character.carrots() += collectedCarrots;
      response.updatedCarrots = character.carrots();
    });

  // LOA-fix (R7 WARN-1, round7): ПОРЯДОК ОПЕРАЦИЙ БЫЛ ОПАСЕН. Посылка
  // отвязывается от character.gifts()/purchases() в самом начале обработчика, а
  // storage-запись удаляется здесь — БЕЗУСЛОВНО, в том числе когда AddItem
  // вернул InvalidUid и предмет фактически не выдан (гард WARN3 раунда 6 такой
  // предмет пропускает через continue). Итог: предмет исчезал и из посылки, и
  // из инвентаря — чистая потеря у игрока.
  // ТЕПЕРЬ: всё выдано → удаляем запись как раньше. Что-то не выдано → посылка
  // ОСТАЁТСЯ в хранилище, но переписывается на ОСТАТОК (только невыданные
  // предметы; морковки обнуляются, они уже начислены выше), и uid возвращается
  // в тот же список, из которого его сняли. Ни потери, ни дубля: повторный
  // забор выдаст ровно то, что не доехало.
  // Гоночных условий нет: обе записи (StorageItem и Character) берутся
  // последовательно, вложенности блокировок нет.
  if (undeliveredItems.empty())
  {
    GetServerInstance().GetDataDirector().GetStorageItemCache().Delete(
      response.storageItemUid);
  }
  else
  {
    storageItemRecord->Mutable(
      [&undeliveredItems](data::StorageItem& storedItem)
      {
        // Форма записи — как везде в этом файле: dao::Field::operator()()
        // отдаёт ссылку на значение (сеттер-перегрузка от const-lvalue в
        // DataDefinitions.hpp некомпилируема, ею тут никто не пользуется).
        storedItem.items() = undeliveredItems;
        storedItem.carrots() = 0;
      });

    characterRecord.Mutable(
      [takenFromGifts, storageItemUid = command.storageItemUid](
        data::Character& character)
      {
        if (takenFromGifts)
          character.gifts().emplace_back(storageItemUid);
        else
          character.purchases().emplace_back(storageItemUid);
      });

    server::util::QuietLogError(
      "HandleGetItemFromStorage: {} stored item(s) could not be granted to "
      "character {}; the parcel {} is kept in the storage with the remainder",
      undeliveredItems.size(),
      clientContext.characterUid,
      response.storageItemUid);
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRequestNpcDressList(
  ClientId clientId,
  const protocol::RanchCommandRequestNpcDressList& requestNpcDressList)
{
  protocol::RanchCommandRequestNpcDressListOK response{
    .unk0 = requestNpcDressList.unk0,
    .dressList = {
    protocol::Item{
      .uid = 0xFFF,
      .tid = 10164,
      .count = 1}} // TODO: Fetch dress list from somewhere
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleWearEquipment(
  ClientId clientId,
  const protocol::AcCmdCRWearEquipment& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  bool isValidItem = false;
  bool isValidHorse = false;

  characterRecord.Immutable([&isValidItem, &isValidHorse, &command](
    const data::Character& character)
  {
    isValidItem = std::ranges::contains(
      character.inventory(), command.equipmentUid);
    isValidHorse = std::ranges::contains(
      character.horses(), command.equipmentUid);
  });

  if (isValidHorse)
  {
    const data::Uid equippedHorseUid = command.equipmentUid;
    // LOA-fix (SYNC-1): запоминаем прошлую лошадь и ФАКТ смены — рассылать и
    // править трекер нужно только если пересадка реально произошла (клиент
    // шлёт команду и при повторном выборе той же лошади).
    data::Uid previousMountUid{data::InvalidUid};
    bool mountChanged = false;

    characterRecord.Mutable([&equippedHorseUid, &previousMountUid, &mountChanged](
      data::Character& character)
    {
      const bool isHorseAlreadyMounted = character.mountUid() == equippedHorseUid;
      if (isHorseAlreadyMounted)
        return;

      previousMountUid = character.mountUid();
      mountChanged = true;

      // Add the mount back to the horse list.
      character.horses().emplace_back(character.mountUid());
      character.mountUid() = equippedHorseUid;

      // Remove the new mount from the horse list.
      character.horses().erase(
        std::ranges::find(character.horses(), equippedHorseUid));
    });

    // LOA-fix (SYNC-1): сообщаем соседям о новой лошади + чиним загон.
    if (mountChanged)
    {
      BroadcastMountChange(clientId, previousMountUid, equippedHorseUid);
    }
  }
  else if (isValidItem)
  {
    const data::Uid equippedItemUid = command.equipmentUid;
    auto equippedItemTid = data::InvalidTid;

    const auto equippedItemRecord = _serverInstance.GetDataDirector().GetItem(
      equippedItemUid);
    equippedItemRecord.Immutable([&equippedItemTid](const data::Item& item)
    {
      equippedItemTid = item.tid();
    });

    // Determine whether the newly equipped item is valid and can be equipped.
    const auto equippedItemTemplate = _serverInstance.GetItemRegistry().GetItem(
      equippedItemTid);

    if (not equippedItemTemplate.has_value())
    {
      throw std::runtime_error("Tried equipping item which is not recognized by the server");
    }

    if (not equippedItemTemplate->characterPartInfo.has_value()
      && not equippedItemTemplate->mountPartInfo.has_value())
    {
      throw std::runtime_error("Tried equipping item which is not a valid character or mount equipment");
    }

    characterRecord.Mutable(
      [this, &equippedItemTemplate, &equippedItemUid](
      data::Character& character)
    {
      const bool isCharacterEquipment = equippedItemTemplate->characterPartInfo.has_value();
      const bool isMountEquipment = equippedItemTemplate->mountPartInfo.has_value();

      // Retrieve the current equipment UIDs.
      std::vector<data::Uid> equipmentUids = character.characterEquipment();

      // Determine which equipment is to be replaced by the newly equipped item.
      std::vector<data::Uid> equipmentToReplace;
      const auto equipmentRecords = _serverInstance.GetDataDirector().GetItemCache().Get(
        equipmentUids);

      for (const auto& equipmentRecord : *equipmentRecords)
      {
        auto equipmentUid{data::InvalidUid};
        auto equipmentTid{data::InvalidTid};
        equipmentRecord.Immutable([&equipmentUid, &equipmentTid](const data::Item& item)
        {
          equipmentUid = item.uid();
          equipmentTid = item.tid();
        });

        // Replace equipment which occupies the same slots as the newly equipped item.
        const auto equipmentTemplate = _serverInstance.GetItemRegistry().GetItem(
          equipmentTid);

        if (isCharacterEquipment)
        {
          // Only compare character parts if the existing equipment template
          if (equipmentTemplate.has_value() && equipmentTemplate->characterPartInfo.has_value())
          {
            if (static_cast<uint32_t>(equipmentTemplate->characterPartInfo->slot)
              & static_cast<uint32_t>(equippedItemTemplate->characterPartInfo->slot))
            {
              equipmentToReplace.emplace_back(equipmentUid);
            }
          }
        }
        else if (isMountEquipment)
        {
          // Only compare mount parts if the existing equipment template
          if (equipmentTemplate.has_value() 
            && equipmentTemplate->mountPartInfo.has_value())
          {
            if (static_cast<uint32_t>(equipmentTemplate->mountPartInfo->slot)
              & static_cast<uint32_t>(equippedItemTemplate->mountPartInfo->slot))
            {
              equipmentToReplace.emplace_back(equipmentUid);
            }
          }
        }
      }

      // Remove equipment replaced with the newly equipped item.
      const auto replacedEquipment = std::ranges::remove_if(
        equipmentUids,
        [&equipmentToReplace](const data::Uid uid)
        {
          return std::ranges::contains(equipmentToReplace, uid);
        });

      // Erase them from the equipment.
      equipmentUids.erase(replacedEquipment.begin(), replacedEquipment.end());
      // Add the newly equipped item.
      equipmentUids.emplace_back(equippedItemUid);

      // Persist back into the unified character equipment list.
      character.characterEquipment = equipmentUids;

      // Remove the newly equipped item from the inventory.
      const auto equippedItemsToRemove = std::ranges::remove(
        character.inventory(), equippedItemUid);
      character.inventory().erase(equippedItemsToRemove.begin(), equippedItemsToRemove.end());

      // Add the replaced equipment back to the inventory.
      std::ranges::copy(equipmentToReplace, std::back_inserter(character.inventory()));
    });
  }

  // Make sure the equipment UID is either a valid item or a horse.
  const bool equipSuccessful = isValidItem || isValidHorse;
  if (equipSuccessful)
  {
    protocol::AcCmdCRWearEquipmentOK response{
      .itemUid = command.equipmentUid,
      .member = command.member};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });

    BroadcastEquipmentUpdate(clientId);
    return;
  }

  protocol::AcCmdCRWearEquipmentCancel response{
    .itemUid = command.equipmentUid,
    .member = command.member};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRemoveEquipment(
  ClientId clientId,
  const protocol::AcCmdCRRemoveEquipment& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  characterRecord.Mutable([&command](data::Character& character)
  {
    // Since mount equipment is combined into characterEquipment for
    // ranch logic, only search and operate on characterEquipment.
    const auto characterEquipmentItemIter = std::ranges::find(
      character.characterEquipment(),
      command.itemUid);

    // You can't really unequip a horse. You can only switch to a different one.
    // At least in Alicia 1.0.

    if (characterEquipmentItemIter != character.characterEquipment().cend())
    {
      const auto range = std::ranges::remove(
        character.characterEquipment(), command.itemUid);
      character.characterEquipment().erase(range.begin(), range.end());
      // LOA-fix (R22-7, round22, SECURITY): return the item to inventory ONLY if it
      // was actually equipped. The old UNCONDITIONAL emplace_back let any client
      // inject an arbitrary uid into its own inventory via AcCmdCRRemoveEquipment
      // (no ownership check) — voiding every std::ranges::contains(inventory,...)
      // membership check server-wide (root of the egg/pet ownership bypass).
      character.inventory().emplace_back(command.itemUid);
    }
  });

  // We really don't need to cancel the unequip. Always respond with OK.
  protocol::AcCmdCRRemoveEquipmentOK response{
    .uid = command.itemUid};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  BroadcastEquipmentUpdate(clientId);
}

void RanchDirector::HandleCreateGuild(
  const ClientId clientId,
  const protocol::RanchCommandCreateGuild& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  const bool isNameValid = locale::IsNameValid(command.name);

  const auto nameModerationVerdict = _serverInstance.GetModerationSystem().Moderate(
    command.name);
  const auto descriptionModerationVerdict = _serverInstance.GetModerationSystem().Moderate(
    command.description);

  if (not isNameValid || nameModerationVerdict.isPrevented || descriptionModerationVerdict.isPrevented)
  {
    protocol::AcCmdCRCreateGuildCancel response{
      .status = 23,
      .member2 = 0};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  bool canCreateGuild = true;
  // todo: configurable
  constexpr int32_t GuildCost = 3000;
  characterRecord.Immutable([&canCreateGuild, GuildCost](const data::Character& character)
  {
    // Check if character has sufficient carrots
    if (character.carrots() < GuildCost)
    {
      canCreateGuild = false;
    }
  });

  // Reject the guild if its name is already taken (case-insensitive). This checks
  // all guilds on the data source, including offline ones with no members online.
  if (canCreateGuild
    && not GetServerInstance().GetDataDirector().GetDataSource().IsGuildNameUnique(command.name))
  {
    canCreateGuild = false;
  }

  // If guild cannot be created, send cancel to client
  if (not canCreateGuild)
  {
    protocol::AcCmdCRCreateGuildCancel response{
      .status = 0,
      .member2 = 0}; // TODO: Unidentified

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    
    return;
  }

  protocol::RanchCommandCreateGuildOK response{
    .uid = 0};

  const auto guildRecord = GetServerInstance().GetDataDirector().CreateGuild();
  if (not guildRecord)
  {
    throw std::runtime_error(
      std::format("Failed to create guild for user '{}'", clientContext.userName));
  }

  guildRecord.Mutable([&response, command, characterUid = clientContext.characterUid](data::Guild& guild)
  {
    response.uid = guild.uid();
    guild.name = command.name;
    guild.description = command.description;
    guild.owner = characterUid;
    guild.members().emplace_back(characterUid);
  });

  characterRecord.Mutable([&response](data::Character& character)
  {
    character.carrots() -= GuildCost;
    response.updatedCarrots = character.carrots();
    character.guildUid = response.uid;
  });

  // Log for moderation
  const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;
  server::util::QuietLogInfo("User '{}' created a guild ({}) with the name '{}'",
    userName,
    response.uid,
    command.name);

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRequestGuildInfo(
  const ClientId clientId,
  const protocol::RanchCommandRequestGuildInfo&)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  auto guildUid = data::InvalidUid;
  characterRecord.Immutable([&guildUid](const data::Character& character)
  {
    guildUid = character.guildUid();
  });

  if (guildUid == data::InvalidUid)
  {
    protocol::RanchCommandRequestGuildInfoCancel response{
      .status = 2 // ERROR_FAIL_NOGUILD
    };

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  protocol::RanchCommandRequestGuildInfoOK response{};

  const auto guildRecord = GetServerInstance().GetDataDirector().GetGuild(guildUid);
  if (not guildRecord)
    throw std::runtime_error("Guild unavailable");

  guildRecord.Immutable([&response](const data::Guild& guild)
  {
    response.guildInfo = {
      .uid = guild.uid(),
      .member1 = 0,
      .member2 = 0,
      .member3 = 0,
      .memberCount = static_cast<uint8_t>(guild.members().size()),
      .member5 = 0,
      .name = guild.name(),
      .description = guild.description(),
      .inviteCooldown = 0,
      .member9 = 0,
      .member10 = 0,
      .member11 = 0};
  });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleWithdrawGuild(
  ClientId clientId,
  const protocol::AcCmdCRWithdrawGuildMember& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // If leave and characterUid is not self
  // If kick and characterUid is self (cannot kick self, only leave)
  using WithdrawOption = protocol::AcCmdCRWithdrawGuildMember::Option;
  if ((command.option == WithdrawOption::Leave and command.characterUid != clientContext.characterUid) or
       command.option == WithdrawOption::Kicked and command.characterUid == clientContext.characterUid)
  {
    protocol::AcCmdCRWithdrawGuildMemberCancel response{
      .status = protocol::GuildError::Unknown // ERROR_FAIL_UNKNOWN
    };
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  // If kick - use command.characterUid as target
  // If leave - use clientContext.characterUid as target
  const auto& characterUid = command.option == WithdrawOption::Kicked
    ? command.characterUid
    : clientContext.characterUid;

  // LOA-fix (R39-1, round39, backlog #129 S4a, iter2 после Codex-T3): fail-closed
  // на НЕДОСТУПНОЙ записи ЦЕЛИ. GetCharacter(uid).Immutable БРОСАЕТ, если запись
  // не в кэше/не существует (враждебный Kicked с несуществующим characterUid) ->
  // dispatch проглатывает бросок, клиент висит + давление на retrieve. Сохраняем
  // запись и проверяем доступность идиомой этого же файла (ср. :1407, :1484).
  const auto targetCharacterRecord =
    GetServerInstance().GetDataDirector().GetCharacter(characterUid);
  if (not targetCharacterRecord)
  {
    protocol::AcCmdCRWithdrawGuildMemberCancel noUser{
      .status = protocol::GuildError::NoUserOrOffline};
    _commandServer.QueueCommand<decltype(noUser)>(
      clientId,
      [noUser]()
      {
        return noUser;
      });
    return;
  }

  data::Uid guildUid{data::InvalidUid};
  targetCharacterRecord.Immutable(
    [&guildUid](const data::Character& character)
    {
      guildUid = character.guildUid();
    });

  // LOA-fix (R39-1, round39, backlog #129 S4a): graceful-отказ, когда цели
  // (или самому уходящему) гильдия не принадлежит. БЫЛО: guildUid остаётся
  // InvalidUid -> GetGuild(InvalidUid).Mutable бросает runtime_error ->
  // ловится dispatch'ем, клиент не получает НИЧЕГО (висит на «ожидании»), а
  // в лог падает строка на каждый пакет. Отдаём честный GuildError::NoGuild.
  if (guildUid == data::InvalidUid)
  {
    protocol::AcCmdCRWithdrawGuildMemberCancel noGuild{
      .status = protocol::GuildError::NoGuild};
    _commandServer.QueueCommand<decltype(noGuild)>(
      clientId,
      [noGuild]()
      {
        return noGuild;
      });
    return;
  }

  std::optional<protocol::GuildError> error{};
  const auto& guildRecord = GetServerInstance().GetDataDirector().GetGuild(guildUid);
  // LOA-fix (R39-2, round39, backlog #129 S4a, iter2 после Codex-T3): fail-closed
  // на НЕДОСТУПНОЙ записи ГИЛЬДИИ. Цель офлайн с валидным guildUid, но запись Guild
  // не загружена (холодный кэш) -> Mutable ниже БРОСИЛ БЫ (GetGuild noexcept, бросок
  // именно у Mutable), dispatch проглотил бы, клиент завис бы. Честный SystemError.
  if (not guildRecord)
  {
    protocol::AcCmdCRWithdrawGuildMemberCancel sysErr{
      .status = protocol::GuildError::SystemError};
    _commandServer.QueueCommand<decltype(sysErr)>(
      clientId,
      [sysErr]()
      {
        return sysErr;
      });
    return;
  }
  guildRecord.Mutable([&characterUid, &error, option = command.option,
                       kickerUid = clientContext.characterUid](data::Guild& guild)
  {
    // LOA-fix (R39-2, round39, backlog #129 S4a, SECURITY/AUTHZ): ПОЛНОМОЧИЯ
    // НА КИК. БЫЛО: при option == Kicked целью становился command.characterUid
    // (чужой персонаж), guildUid брался ИЗ ЦЕЛИ, и дальше гильдия молча
    // правилась — ни одной проверки, что кикающий вообще состоит в ЭТОЙ
    // гильдии, не говоря о праве кикать. То есть любой аутентифицированный
    // клиент одним пакетом выкидывал ЛЮБОГО персонажа из ЛЮБОЙ гильдии,
    // ВКЛЮЧАЯ её владельца (мутация персистится в записи Guild и Character).
    // Ветка Disband ниже свою проверку owner() имела — Kicked осталась без.
    // Правила (fail-closed): кикать может только владелец или офицер ТОЙ ЖЕ
    // гильдии; владельца кикнуть нельзя никому; офицер не может кикать
    // офицера (это может только владелец); цель обязана состоять в гильдии.
    if (option == WithdrawOption::Kicked)
    {
      const auto& members = guild.members();
      const auto& officers = guild.officers();

      const bool kickerIsOwner = guild.owner() == kickerUid;
      const bool kickerIsOfficer =
        std::ranges::find(officers, kickerUid) != officers.cend();
      const bool kickerIsMember =
        std::ranges::find(members, kickerUid) != members.cend();
      const bool targetIsMember =
        std::ranges::find(members, characterUid) != members.cend();
      const bool targetIsOfficer =
        std::ranges::find(officers, characterUid) != officers.cend();

      if (not kickerIsMember || not (kickerIsOwner || kickerIsOfficer))
      {
        error.emplace(protocol::GuildError::NoAuthority);
        server::util::QuietLogWarn("Character {} tried to kick {} from guild {} without authority",
          kickerUid,
          characterUid,
          guild.uid());
        return;
      }

      if (guild.owner() == characterUid)
      {
        error.emplace(protocol::GuildError::NoAuthority);
        server::util::QuietLogWarn("Character {} tried to kick the owner of guild {}",
          kickerUid,
          guild.uid());
        return;
      }

      if (targetIsOfficer && not kickerIsOwner)
      {
        error.emplace(protocol::GuildError::NoAuthority);
        server::util::QuietLogWarn("Officer {} tried to kick officer {} from guild {}",
          kickerUid,
          characterUid,
          guild.uid());
        return;
      }

      if (not targetIsMember)
      {
        error.emplace(protocol::GuildError::NoUserOrOffline);
        server::util::QuietLogWarn("Character {} tried to kick non-member {} from guild {}",
          kickerUid,
          characterUid,
          guild.uid());
        return;
      }
    }

    if (option == WithdrawOption::Disband)
    {
      if (guild.owner() != characterUid)
      {
        // Command was to disband guild but caller is not the owner, report
        error.emplace(protocol::GuildError::NoAuthority);
        server::util::QuietLogWarn("Character {} tried to disband guild {} but is not owner",
          characterUid,
          guild.uid());
        return;
      }

      const auto& guildMembers = guild.members();
      // Check that there is only 1 guild member and that member is the owner
      bool lastGuildMemberIsOwner = guildMembers.size() == 1 && guildMembers[0] == characterUid;
      if (not lastGuildMemberIsOwner || guild.officers().size() > 0)
      {
        // Command was to disband guild but guild has members (somehow)
        error.emplace(protocol::GuildError::NotAlone);
        server::util::QuietLogWarn("Character {} tried to disband guild {} with members and/or officers present",
          characterUid,
          guild.uid());
        return;
      }
    }
    
    // Make sure there is no trace of ex-member in the guild
    if (std::ranges::find(guild.members(), characterUid) != guild.members().cend())
      guild.members().erase(std::ranges::find(guild.members(), characterUid));
    if (std::ranges::find(guild.officers(), characterUid) != guild.officers().cend())
      guild.officers().erase(std::ranges::find(guild.officers(), characterUid));
  });

  if (error.has_value())
  {
    protocol::AcCmdCRWithdrawGuildMemberCancel cancel{
      .status = error.value()
    };
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  // Reset character guild uid
  GetServerInstance().GetDataDirector().GetCharacter(characterUid).Mutable(
    [&guildUid](data::Character& character)
    {
      character.guildUid() = data::InvalidUid;
    });

  // On disband the guild no longer has any members, so delete its record.
  if (command.option == WithdrawOption::Disband)
    GetServerInstance().GetDataDirector().GetGuildCache().Delete(guildUid);

  const protocol::AcCmdCRWithdrawGuildMemberOK response{
    .option = command.option};
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  const auto& authorityCharacterUid = clientContext.characterUid;
  for (const auto& [onlineClientId, onlineClientContext] : _clients)
  {
    // Notify online characters only
    if (not onlineClientContext.isAuthenticated)
      continue;

    // Leave option should not have a notify be sent to the leaver
    if (command.option == WithdrawOption::Leave and onlineClientContext.characterUid == characterUid)
      continue;

    // Check if this client is in the same guild as the withdrawn member
    // TODO: guild uid could be cached under client context for cheaper checks
    data::Uid onlineClientGuildUid{data::InvalidUid};
    GetServerInstance().GetDataDirector().GetCharacter(onlineClientContext.characterUid).Immutable(
      [&onlineClientGuildUid](const data::Character& character)
      {
        onlineClientGuildUid = character.guildUid();
      });

    if (onlineClientGuildUid != guildUid)
      continue;

    const protocol::AcCmdRCWithdrawGuildMemberNotify notify{
      .guildUid = guildUid,
      .guildMemberCharacterUid =
        command.option == WithdrawOption::Kicked ?
          authorityCharacterUid :
          onlineClientContext.characterUid,
      .withdrawnCharacterUid = characterUid,
      .option = command.option};

    _commandServer.QueueCommand<decltype(notify)>(
      onlineClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::HandleUpdatePet(
  ClientId clientId,
  const protocol::AcCmdCRUpdatePet& command)
{
  protocol::AcCmdRCUpdatePet response{
    .petInfo = command.petInfo
  };

  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  response.petInfo.characterUid = clientContext.characterUid;

  // petId of 0 means deequip the active pet.
  if (command.petInfo.pet.petId == 0)
  {
    characterRecord.Mutable(
      [](data::Character& character)
      {
        character.petUid = data::InvalidUid;
      });
  }
  else
  {
    auto petUid = data::InvalidUid;
    bool hasUsedItem = false;

    characterRecord.Mutable(
      [this, &command, &petUid, &hasUsedItem](data::Character& character)
      {
        // The pets of the character.
        const auto storedPetRecords = GetServerInstance().GetDataDirector().GetPetCache().Get(
          character.pets());

        if (not storedPetRecords || storedPetRecords->empty())
        {
          return;
        }

        // Find the pet record based on the item used.
        for (const auto& petRecord : *storedPetRecords)
        {
          petRecord.Immutable(
            [&command, &petUid](const data::Pet& pet)
            {
              if (pet.itemUid() == command.petInfo.itemUid)
              {
                petUid = pet.uid();
              }
            });
        }

        if (command.itemUid)
        {
          hasUsedItem = GetServerInstance().GetItemSystem().HasItemInstance(
           character,
           *command.itemUid);
        }

        if (not hasUsedItem && petUid != data::InvalidUid)
        {
          character.petUid = petUid;
        }
      });

    if (petUid == data::InvalidUid)
    {
      throw std::runtime_error(std::format(
        "Character {} has no pet with petId {}",
        clientContext.characterUid,
        command.petInfo.pet.petId));
    }

    const auto petRecord = GetServerInstance().GetDataDirector().GetPet(petUid);
    petRecord.Immutable(
      [&response](const data::Pet& pet)
      {
        response.petInfo.pet.name = pet.name();
        response.petInfo.pet.birthDate = util::TimePointToAliciaTime(pet.birthDate());
      });

    if (hasUsedItem)
    {
      const auto isNameValid = locale::IsNameValid(command.petInfo.pet.name);
      const auto moderationVerdict = _serverInstance.GetModerationSystem().Moderate(
        command.petInfo.pet.name);

      if (not isNameValid || moderationVerdict.isPrevented)
      {
        SendUpdatePetCancel(clientId, protocol::AcCmdRCUpdatePetCancel{
          .petInfo = response.petInfo,
          .error = protocol::ChangeNicknameError::InvalidNickname});
        return;
      }

      // LOA-fix (R29-3, #59 S21-b, ЭКОНОМИКА): жетон переименования питомца НЕ
      // списывался — это буквально апстримный TODO. Владение проверено выше
      // (HasItemInstance), но ConsumeItem не звался ни разу: ОДИН жетон 45002
      // (7000 морковок в лавке) давал БЕСКОНЕЧНЫЕ переименования. Достижимо
      // ВАНИЛЬНЫМ клиентом, без модов. Приводим к контракту «списать -> применить»:
      // имя меняется только если жетон реально ушёл.
      // ★ hasUsedItem выставлен через HasItemInstance, который считает своим и
      // НАДЕТЫЙ предмет (characterEquipment), а ConsumeItem списывает только из
      // inventory. Расхождение намеренно разрешаем В ПОЛЬЗУ ОТКАЗА (fail-closed).
      // ★ Урок R18-1 (там после Cancel забыли return): после Cancel обязателен ВЫХОД.
      bool renameTokenConsumed = false;
      characterRecord.Mutable(
        [this, &command, &renameTokenConsumed](data::Character& character)
        {
          const auto tokenRecord = GetServerInstance().GetDataDirector().GetItemCache().Get(
            *command.itemUid);
          if (not tokenRecord)
            return;

          data::Tid tokenTid = data::InvalidTid;
          tokenRecord->Immutable([&tokenTid](const data::Item& item) { tokenTid = item.tid(); });

          // LOA-fix (R29-3b, #59 S21-b, SECURITY): гейт TID жетона. command.itemUid
          // ВЫБИРАЕТ КЛИЕНТ из своего инвентаря — без проверки TID игрок укажет UID
          // дешёвого предмета (сахарный кубик 41007 = 20 морковок), тот спишется, а
          // ренейм пройдёт вместо жетона 45002 (Pet Rename Token, 7000 морковок).
          // Тот же forge-класс, что R29-1: списывать можно ТОЛЬКО настоящий жетон.
          if (tokenTid != 45002)
            return;

          renameTokenConsumed = GetServerInstance().GetItemSystem().ConsumeItem(
            character, tokenTid, 1).itemConsumed;
        });

      if (not renameTokenConsumed)
      {
        server::util::QuietLogWarn("UpdatePet: character {} could not consume rename token {}; refusing rename",
          clientContext.characterUid, *command.itemUid);
        SendUpdatePetCancel(clientId, protocol::AcCmdRCUpdatePetCancel{
          .petInfo = response.petInfo,
          .error = protocol::ChangeNicknameError::NoOrIncorrectItem});
        return;
      }

      std::string currentName{};
      petRecord.Mutable(
        [&command, &currentName](data::Pet& pet)
        {
          currentName = pet.name();
          pet.name() = command.petInfo.pet.name;
        });

      response.petInfo.pet.name = command.petInfo.pet.name;

      // Log for moderation
      const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
        clientContext.characterUid).userName;
      server::util::QuietLogInfo("User '{}' changed the name of a pet ({}) from '{}' to '{}'",
        userName,
        petUid,
        currentName,
        command.petInfo.pet.name);
    }
  }

  const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];
  for (const ClientId ranchClientId : ranchInstance.clients)
  {
    _commandServer.QueueCommand<decltype(response)>(ranchClientId, [response]()
      {
        return response;
      });
  }
}

void RanchDirector::SendUpdatePetCancel(
  ClientId clientId,
  const protocol::AcCmdRCUpdatePetCancel& command)
{
  _commandServer.QueueCommand<protocol::AcCmdRCUpdatePetCancel>(
    clientId,
    [command]()
    {
      return command;
    });
}

void RanchDirector::HandleUserPetInfos(
  ClientId clientId,
  const protocol::RanchCommandUserPetInfos& command)
{
  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::RanchCommandUserPetInfosOK response{};

  characterRecord.Mutable(
    [this, &command, &response](data::Character& character)
    {
      auto storedPetRecords = GetServerInstance().GetDataDirector().GetPetCache().Get(
        character.pets());
      if (!storedPetRecords || storedPetRecords->empty())
        return;

      protocol::BuildProtocolPets(response.pets,
        storedPetRecords.value());
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response](){
      return response;
    });
}

void RanchDirector::HandleIncubateEgg(
  ClientId clientId,
  const protocol::AcCmdCRIncubateEgg& command)
{
  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCRIncubateEggOK response{
    response.incubatorSlot = command.incubatorSlot,
  };

  bool incubated = false;
  characterRecord.Mutable(
    [this, &clientContext, &command, &response, &incubated, clientId](data::Character& character)
    {
      // LOA-fix (R22-4, round22, backlog #92-class / SECURITY): HandleIncubateEgg took
      // command.{itemUid,itemTid,incubatorSlot} verbatim. Validate all of it; each
      // failure sends AcCmdCRIncubateEggCancel and returns.
      const auto sendCancel = [this, &command, clientId]()
      {
        protocol::AcCmdCRIncubateEggCancel cancel{};
        cancel.cancel = 0;
        cancel.itemUid = command.itemUid;
        cancel.itemTid = command.itemTid;
        cancel.incubatorSlot = command.incubatorSlot;
        _commandServer.QueueCommand<decltype(cancel)>(
          clientId, [cancel]() { return cancel; });
      };

      // (1) ownership.
      if (not std::ranges::contains(character.inventory(), command.itemUid))
      {
        server::util::QuietLogWarn("IncubateEgg: character {} named item {} it does not own; refusing",
          clientContext.characterUid, command.itemUid);
        sendCancel();
        return;
      }

      // (2) tid FROM THE ITEM RECORD, not from the command.
      const auto itemRecord = GetServerInstance().GetDataDirector().GetItemCache().Get(
        command.itemUid);
      if (not itemRecord)
      {
        server::util::QuietLogWarn("IncubateEgg: character {} item {} record unavailable; refusing",
          clientContext.characterUid, command.itemUid);
        sendCancel();
        return;
      }
      data::Tid eggItemTid = data::InvalidTid;
      itemRecord->Immutable([&eggItemTid](const data::Item& item) { eggItemTid = item.tid(); });

      // (3) real tid must be a registered egg. GetEggInfo throws on unknown tid
      // (returns by value, not optional), so catch it and cancel — the old
      // `if (not optional)` was dead code.
      std::optional<registry::EggInfo> eggTemplate;
      try
      {
        eggTemplate = _serverInstance.GetPetRegistry().GetEggInfo(eggItemTid);
      }
      catch (const std::exception&)
      {
        server::util::QuietLogWarn("IncubateEgg: character {} item {} (tid {}) is not an egg; refusing",
          clientContext.characterUid, command.itemUid, eggItemTid);
        sendCancel();
        return;
      }

      // (4) slot in [0,3) — else a later response.incubator[slot] is an OOB write.
      if (command.incubatorSlot >= 3)
      {
        server::util::QuietLogWarn("IncubateEgg: character {} out-of-range incubator slot {}; refusing",
          clientContext.characterUid, command.incubatorSlot);
        sendCancel();
        return;
      }

      // (4b) LOA-fix (R28, #102, FAIRNESS): слот должен укладываться в ёмкость
      // инкубатора, которой персонаж РЕАЛЬНО владеет (housing 51=1 слот, 52=2,
      // нет жилья => 0 "locked"). R22-4 гейтит только границу массива (slot<3) —
      // модклиент пользуется слотом, который не покупал. Ёмкость выводим как в
      // EnterRanch (RanchDirector.cpp:1331-1337), но по MAX, а НЕ last-wins:
      // цикл там перезаписывает incubatorSlots, и у владельца ОБОИХ инкубаторов
      // (прод: Valoria housing [51,52,51]) last-wins мог бы отдать 1 слот.
      // КОНСЕРВАТИВНЫЙ вариант: гейт кусается только когда персонаж РЕАЛЬНО
      // владеет инкубатором (ownedIncubatorSlots > 0). Персонажи без жилья 51/52
      // остаются в статус-кво R22-4 (память уже закрыта slot<3), чтобы не убить
      // легитимную инкубацию, если у ванильного клиента есть путь slot=0 без
      // housing. Холодный housing-кэш => ПРОПУСКАЕМ гейт с warn (fail-open):
      // это fairness, а не security; ложный отказ легитимной инкубации хуже,
      // чем разовый необилеченный слот, а память уже защищена R22-4.
      {
        uint32_t ownedIncubatorSlots = 0;
        bool housingKnown = true;
        if (not character.housing().empty())
        {
          const auto housingRecords = GetServerInstance().GetDataDirector().GetHousingCache().Get(
            character.housing());
          if (not housingRecords)
          {
            housingKnown = false;
          }
          else
          {
            for (const auto& housingRecord : *housingRecords)
            {
              housingRecord.Immutable([&ownedIncubatorSlots](const data::Housing& housing)
              {
                if (housing.housingId() == DoubleIncubatorId)
                  ownedIncubatorSlots = std::max(ownedIncubatorSlots, uint32_t{2});
                else if (housing.housingId() == SingleIncubatorId)
                  ownedIncubatorSlots = std::max(ownedIncubatorSlots, uint32_t{1});
              });
            }
          }
        }

        if (not housingKnown)
        {
          server::util::QuietLogWarn("IncubateEgg: character {} housing cache cold; skipping capacity gate",
            clientContext.characterUid);
        }
        else if (ownedIncubatorSlots > 0 && command.incubatorSlot >= ownedIncubatorSlots)
        {
          server::util::QuietLogWarn("IncubateEgg: character {} used incubator slot {} but owns only {} slot(s); refusing",
            clientContext.characterUid, command.incubatorSlot, ownedIncubatorSlots);
          sendCancel();
          return;
        }
      }

      // (5) slot free and (6) item not already incubating. FAIL CLOSED: if the
      // character has eggs but the cache read is cold (nullopt), REFUSE rather than
      // skip — a cold cache must not bypass the duplicate/slot guards.
      if (not character.eggs().empty())
      {
        const auto existingEggs = GetServerInstance().GetDataDirector().GetEggCache().Get(
          character.eggs());
        if (not existingEggs)
        {
          server::util::QuietLogWarn("IncubateEgg: character {} egg cache cold; refusing (fail-closed)",
            clientContext.characterUid);
          sendCancel();
          return;
        }
        bool slotTaken = false;
        bool itemAlreadyIncubating = false;
        for (const auto& existing : *existingEggs)
        {
          existing.Immutable([&command, &slotTaken, &itemAlreadyIncubating](const data::Egg& e)
          {
            if (e.incubatorSlot() == command.incubatorSlot)
              slotTaken = true;
            if (e.itemUid() == command.itemUid)
              itemAlreadyIncubating = true;
          });
        }
        if (slotTaken)
        {
          server::util::QuietLogWarn("IncubateEgg: character {} incubator slot {} is occupied; refusing",
            clientContext.characterUid, command.incubatorSlot);
          sendCancel();
          return;
        }
        if (itemAlreadyIncubating)
        {
          server::util::QuietLogWarn("IncubateEgg: character {} item {} is already incubating; refusing",
            clientContext.characterUid, command.itemUid);
          sendCancel();
          return;
        }
      }

      const auto eggRecord = GetServerInstance().GetDataDirector().CreateEgg();
      if (not eggRecord)
      {
        throw std::runtime_error(
          std::format("Failed to create egg for user {}", clientContext.userName));
      }

      eggRecord.Mutable([&command, &response, &character, &eggTemplate, eggItemTid](data::Egg& egg)
        {
          egg.incubatorSlot = command.incubatorSlot;
          egg.incubatedAt = data::Clock::now();
          egg.boostsUsed = 0;
          egg.itemTid = eggItemTid;
          egg.itemUid = command.itemUid;

          character.eggs().emplace_back(egg.uid());

          protocol::BuildProtocolEgg(response.egg, egg, eggTemplate.value().hatchDuration);
        });

      incubated = true;
    });

  // Only a successful incubation responds/broadcasts. A refused request must NOT emit
  // an OK or notify a phantom (out-of-range) slot to peers.
  if (incubated)
  {
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });

    protocol::AcCmdCRIncubateEggNotify notify{
      .characterUid = clientContext.characterUid,
      .incubatorSlot = command.incubatorSlot,
      .egg = response.egg,
    };

    const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];
    // Broadcast the egg incubation to all ranch clients.
    for (ClientId ranchClient : ranchInstance.clients)
    {
      // Prevent broadcasting to self.
      if (ranchClient == clientId)
        continue;

      _commandServer.QueueCommand<decltype(notify)>(
        ranchClient,
        [notify]()
        {
          return notify;
        });
    }
  }
}

void RanchDirector::HandleBoostIncubateEgg(
  ClientId clientId,
  const protocol::AcCmdCRBoostIncubateEgg& command)
{
  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCRBoostIncubateEggOK response{
    .incubatorSlot = command.incubatorSlot};

  // LOA-fix (R22-1, round22, backlog #92): validate -> find egg -> cap -> consume ->
  // boost. Any failure early-outs with warn and NO OK; only a legitimate boost reaches
  // QueueCommand. Closes the boost leg of the instant-pet class.
  bool boostApplied = false;
  characterRecord.Mutable(
    [this, &command, &response, &boostApplied, &clientContext](data::Character& character)
    {
      // (#92 hole 0) ownership.
      if (not std::ranges::contains(character.inventory(), command.itemUid))
      {
        server::util::QuietLogWarn("BoostIncubateEgg: character {} named item {} it does not own; refusing",
          clientContext.characterUid, command.itemUid);
        return;
      }

      const auto itemRecord = GetServerInstance().GetDataDirector().GetItemCache().Get(
        command.itemUid);
      if (not itemRecord)
        return;
      data::Tid itemTid = data::InvalidTid;
      itemRecord->Immutable([&itemTid](const data::Item& item) { itemTid = item.tid(); });

      // (#92 hole 1; R22-8 regression fix) authoritative booster check by item IDENTITY.
      // The REAL purchasable egg booster is the Purified Crystal (tid 46018, itemIndex
      // 3/5, bought for carrots) — the pin handler's own comment named "Crystal". The
      // 97001/97003 potions (3/9) are the unobtainable legacy boosters. R22 wrongly
      // gated on subcategory 9, which EXCLUDED the crystal = a live regression (a real
      // crystal boost was refused). Fix: allowlist the known egg-booster tids. Subcategory
      // is NOT a usable signal — subcat 5 is a 58-item grab-bag (rename tokens, EXP potions,
      // horse permits, etc.), so gating on it would let junk items boost eggs. An explicit tid
      // set both restores the crystal AND keeps the economy gate (only a real booster is
      // accepted; fail-closed for the unknown). The set is derived MANUALLY — config has NO
      // egg-booster marker field, so it was built by a name/purpose sweep of every consumable:
      // subcat 9 == {97001,97003} exactly; 46018 is the only purchasable egg booster; all other
      // Crystal/Fortune/egg-named items serve other roles (crafting, cleaning, pouches, egg
      // detector, skill/EXP boosters, clothing). Add any new/seasonal egg-booster tid HERE.
      const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(itemTid);
      const bool isEggBooster =
        (itemTid == 46018u || itemTid == 97001u || itemTid == 97003u);
      if (not itemTemplate
        || itemTemplate->type != registry::Item::Type::Consumable
        || not isEggBooster)
      {
        server::util::QuietLogWarn("BoostIncubateEgg: character {} used non-booster item {} (tid {}); refusing",
          clientContext.characterUid, command.itemUid, itemTid);
        return;
      }

      // (#92 hole 2) find the target egg BEFORE consuming; capture its uid and whether it
      // is already ready at SECOND granularity. boostsUsed is clamped before the *8h to
      // keep the chrono arithmetic from overflowing int64 ns on inflated legacy data.
      const auto eggRecord = GetServerInstance().GetDataDirector().GetEggCache().Get(
        character.eggs());
      if (not eggRecord)
        return;
      data::Uid targetEggUid = data::InvalidUid;
      bool alreadyReady = false;
      for (const auto& egg : *eggRecord)
      {
        egg.Immutable([this, &command, &targetEggUid, &alreadyReady](const data::Egg& eggData)
        {
          if (eggData.incubatorSlot() != command.incubatorSlot)
            return;
          targetEggUid = eggData.uid();
          try
          {
            const registry::EggInfo eggTemplate = _serverInstance.GetPetRegistry().GetEggInfo(
              eggData.itemTid());
            const auto progress =
              (std::chrono::system_clock::now() - eggData.incubatedAt())
              + std::min<uint32_t>(eggData.boostsUsed(), 100000u) * std::chrono::hours(8);
            alreadyReady = std::chrono::duration_cast<std::chrono::seconds>(
              eggTemplate.hatchDuration - progress).count() <= 0;
          }
          catch (const std::exception&)
          {
            alreadyReady = true;  // unregistered legacy tid -> refuse the boost.
          }
        });
        if (targetEggUid != data::InvalidUid)
          break;
      }

      if (targetEggUid == data::InvalidUid)
      {
        server::util::QuietLogWarn("BoostIncubateEgg: character {} boosted empty slot {}; refusing",
          clientContext.characterUid, command.incubatorSlot);
        return;
      }
      if (alreadyReady)
      {
        server::util::QuietLogWarn("BoostIncubateEgg: character {} boosted already-ready egg in slot {}; refusing",
          clientContext.characterUid, command.incubatorSlot);
        return;
      }

      // (#92 hole 3) consume the booster and honor the verdict BEFORE granting.
      // ConsumeItem consumes the FIRST inventory stack of this tid and clears
      // verdict.itemUid when that stack empties. Capture the exact stack it will
      // consume (same first-match order) so the response identifies the right one
      // even with multiple stacks of the same tid or after deletion.
      data::Uid consumedUid = data::InvalidUid;
      if (const auto stacks = GetServerInstance().GetDataDirector().GetItemCache().Get(
            character.inventory()))
      {
        for (const auto& rec : *stacks)
        {
          bool matched = false;
          rec.Immutable([&itemTid, &matched, &consumedUid](const data::Item& it)
          {
            if (it.tid() == itemTid)
            {
              matched = true;
              consumedUid = it.uid();
            }
          });
          if (matched)
            break;
        }
      }
      const auto consumeVerdict = GetServerInstance().GetItemSystem().ConsumeItem(
        character, itemTid, 1);
      if (not consumeVerdict.itemConsumed)
      {
        server::util::QuietLogWarn("BoostIncubateEgg: character {} could not consume booster {} (tid {}); refusing",
          clientContext.characterUid, command.itemUid, itemTid);
        return;
      }

      response.item = {
        .uid = consumedUid,
        .tid = itemTid};
      response.item.count = consumeVerdict.remainingItemCount;

      // Apply the boost to EXACTLY the validated egg (by uid).
      bool applied = false;
      for (const auto& egg : *eggRecord)
      {
        egg.Mutable([this, &targetEggUid, &response, &applied](data::Egg& eggData)
        {
          if (eggData.uid() != targetEggUid)
            return;
          const registry::EggInfo eggTemplate = _serverInstance.GetPetRegistry().GetEggInfo(
            eggData.itemTid());
          eggData.boostsUsed() += 1;
          protocol::BuildProtocolEgg(response.egg, eggData, eggTemplate.hatchDuration);
          applied = true;
        });
        if (applied)
          break;
      }

      boostApplied = applied;
    });

  if (boostApplied)
  {
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
  }
};

void RanchDirector::HandleBoostIncubateInfoList(
  ClientId clientId,
  const protocol::AcCmdCRBoostIncubateInfoList&)
{
  protocol::AcCmdCRBoostIncubateInfoListOK response{
    .member1 = 0,
    .count = 0
  // for loop with a vector
  };
  
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRequestPetBirth(
  ClientId clientId,
  const protocol::AcCmdCRRequestPetBirth& command)
{
  // TODO: implement pity based on egg level provided by the client

  const auto& clientContext = GetClientContext(clientId);

  protocol::AcCmdCRRequestPetBirthOK response{
    .petBirthInfo = {
      .petInfo = {
        .characterUid = clientContext.characterUid,}
    },
  };

  bool petAlreadyExists = false;
  data::Tid petItemTid = data::InvalidTid;
  data::Uid petUid = data::InvalidUid;

  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);
  // LOA-fix (R22-2, round22, backlog #99 + SECURITY): gate birth on REAL readiness AND
  // re-verify the egg's item is still owned. Ownership re-check closes cross-account item
  // deletion + duplicate-mint from a pre-hardening egg referencing a foreign/dangling
  // itemUid (birth deletes egg.itemUid globally + mints a pet). Readiness at SECOND
  // granularity == the client's truncated countdown; boostsUsed clamped before *8h.
  // Refusal sends a real AcCmdCRRequestPetBirthCancel, not silence.
  {
    bool eggReady = false;
    bool eggOwned = false;
    data::Uid eggItemUid = data::InvalidUid;
    characterRecord.Immutable(
      [this, &command, &eggReady, &eggOwned, &eggItemUid](const data::Character& character)
      {
        const auto eggRecord = GetServerInstance().GetDataDirector().GetEggCache().Get(
          character.eggs());
        if (not eggRecord)
          return;
        for (const auto& egg : *eggRecord)
        {
          egg.Immutable([this, &command, &character, &eggReady, &eggOwned, &eggItemUid](const data::Egg& eggData)
          {
            if (eggData.incubatorSlot() != command.incubatorSlot)
              return;
            eggItemUid = eggData.itemUid();
            eggOwned = std::ranges::contains(character.inventory(), eggData.itemUid());
            try
            {
              const registry::EggInfo eggTemplate = _serverInstance.GetPetRegistry().GetEggInfo(
                eggData.itemTid());
              const auto progress =
                (std::chrono::system_clock::now() - eggData.incubatedAt())
                + std::min<uint32_t>(eggData.boostsUsed(), 100000u) * std::chrono::hours(8);
              eggReady = std::chrono::duration_cast<std::chrono::seconds>(
                eggTemplate.hatchDuration - progress).count() <= 0;
            }
            catch (const std::exception&)
            {
              eggReady = false;  // unregistered legacy tid -> not birthable.
            }
          });
        }
      });
    if (not eggOwned || not eggReady)
    {
      server::util::QuietLogWarn(
        "RequestPetBirth: character {} slot {} refused (owned={}, ready={})",
        clientContext.characterUid, command.incubatorSlot, eggOwned, eggReady);
      protocol::AcCmdCRRequestPetBirthCancel cancel{};
      cancel.petInfo.characterUid = clientContext.characterUid;
      cancel.petInfo.itemUid = eggItemUid;  // echo the egg's item uid so the client clears the slot UI
      _commandServer.QueueCommand<decltype(cancel)>(
        clientId,
        [cancel]()
        {
          return cancel;
        });
      return;
    }
  }

  characterRecord.Mutable(
    [this, &clientContext, &command, &response, &petAlreadyExists, &petItemTid, &petUid](data::Character& character)
    {
      auto hatchingEggUid{data::InvalidUid};
      auto hatchingEggItemUid{data::InvalidUid};
      auto hatchingEggTid{data::InvalidTid};

      const auto eggRecord = GetServerInstance().GetDataDirector().GetEggCache().Get(
        character.eggs());
      if (not eggRecord)
        throw std::runtime_error("Egg records not available");

      // Find the egg that has hatched.
      for (const auto& egg : *eggRecord)
      {
        egg.Immutable(
          [&command, &response, &hatchingEggTid, &hatchingEggItemUid, &hatchingEggUid](
            const data::Egg& eggData)
          {
            if (eggData.incubatorSlot() == command.incubatorSlot)
            {
              hatchingEggUid = eggData.uid();
              hatchingEggTid = eggData.itemTid();
              hatchingEggItemUid = eggData.itemUid();

              response.petBirthInfo.petInfo.itemUid = hatchingEggItemUid;
            };
          });
      }

      // TODO: reduce the incubator durability (if it is a double incubator)

      // Remove the hatched egg from the incubator and from the character's inventory.
      if (auto it = std::ranges::find(character.eggs(), hatchingEggUid);
        it != character.eggs().end())
      {
        character.eggs().erase(it);
      }

      if (auto it = std::ranges::find(character.inventory(), hatchingEggItemUid);
        it != character.inventory().end())
      {
        character.inventory().erase(it);
      }

      //Delete the Item and Egg records
      GetServerInstance().GetDataDirector().GetEggCache().Delete(hatchingEggUid);
      GetServerInstance().GetDataDirector().GetItemCache().Delete(hatchingEggItemUid);

      const registry::EggInfo eggTemplate = _serverInstance.GetPetRegistry().GetEggInfo(
        hatchingEggTid);

      const auto& hatchablePets = eggTemplate.hatchablePets;
      std::uniform_int_distribution<size_t> dist(0, hatchablePets.size() - 1);
      petItemTid = hatchablePets[dist(server::util::GetRandomEngine())];

      const registry::PetInfo petTemplate = _serverInstance.GetPetRegistry().GetPetInfo(
        petItemTid);
      const auto petId = petTemplate.petId;

      const auto petRecords = GetServerInstance().GetDataDirector().GetPetCache().Get(
        character.pets());

      // Figure out whether the character already has this pet. Guard the optional —
      // a cold pet cache must not dereference a disengaged optional (remote crash).
      if (petRecords)
      for (const auto& petRecord : *petRecords)
      {
        petRecord.Immutable([&petAlreadyExists, petId](const data::Pet& pet)
        {
          petAlreadyExists = (pet.petId() == petId);
        });

        if (petAlreadyExists == true)
          break;
      }

      if (petAlreadyExists)
        return;

      // Create the pet
      const auto bornPet = GetServerInstance().GetDataDirector().CreatePet();
      if (not bornPet)
      {
        throw std::runtime_error(
          std::format("Failed to create pet for user {}", clientContext.userName));
      }

      bornPet.Mutable([&response, &petUid, petId](data::Pet& pet)
      {
        pet.name() = "";
        pet.petId() = petId;
        pet.birthDate() = data::Clock::now();
    
        // Fill the response with the born pet.
        response.petBirthInfo.petInfo.pet = {
          .petId = pet.petId(),
          .name = pet.name(),
          .birthDate = util::TimePointToAliciaTime(pet.birthDate())};
        petUid = pet.uid();
      });

      character.pets().emplace_back(petUid);
    });

  // Determine which item to create based on whether pet already exists
  constexpr data::Tid PityItemTid = 46018;
  const data::Tid itemTidToCreate = petAlreadyExists ? PityItemTid : petItemTid;

  auto createdItemUid = data::InvalidUid;
  characterRecord.Mutable([this, &createdItemUid, &itemTidToCreate](data::Character& character)
  {
    createdItemUid = GetServerInstance().GetItemSystem().AddItem(
      character, itemTidToCreate, 1);
  });

  if (createdItemUid != data::InvalidUid)
  {
    // If it's a pet item (not pity), link the newly created pet to the item
    if (!petAlreadyExists)
    {
      auto petRecord = GetServerInstance().GetDataDirector().GetPet(petUid);
      petRecord.Mutable([createdItemUid](data::Pet& pet)
      {
        pet.itemUid() = createdItemUid;
      });
    }

    // Fill response with the created item
    const auto itemRecord = GetServerInstance().GetDataDirector().GetItem(createdItemUid);
    itemRecord.Immutable([&response](const data::Item& item)
    {
      response.petBirthInfo.eggItem = {
        .uid = item.uid(),
        .tid = item.tid(),
        .count = item.count()};
    });
  }

  protocol::AcCmdCRRequestPetBirthNotify notify{
    .petBirthInfo = response.petBirthInfo
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
  
  const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];
  // Broadcast the egg hatching to all ranch clients.
  for (ClientId ranchClient : ranchInstance.clients)
  {
    // Prevent broadcasting to self.
    if (ranchClient == clientId)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClient,
      [notify]()
      {
        return notify;
      });
  }
};

void RanchDirector::HandlePetBornResult(
  ClientId clientId,
  const protocol::AcCmdCRPetBornResult& command)
{
  // This command is sent by the client after receiving the pet birth notification,
  // this signals the clients to remove the pet from the incubator

  protocol::AcCmdCRPetBornResultNotify response{
    .member1 = command.member1,
    .member2 = command.member2
  };

  // broadcast to all the ranch clients.
  const auto& clientContext = GetClientContext(clientId);
  const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];
  for (ClientId ranchClient : ranchInstance.clients)
  {
    // Prevent broadcasting to self.
    if (ranchClient == clientId)
      continue;
    
    _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
  }
  
}

void RanchDirector::BroadcastEquipmentUpdate(ClientId clientId)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCRUpdateEquipmentNotify notify{
    .characterUid = clientContext.characterUid};

  characterRecord.Immutable([this, &notify](const data::Character& character)
  {
    // Character + mount equipment.
    // LOA-fix (SYNC-2): единый список character.characterEquipment() хранит И
    // одежду персонажа, И сбрую лошади. Раньше он целиком клался в ОБА поля
    // нотификации — соседи на ранчо видели перемешанный аватар. Раскладываем
    // по типу слота через ItemRegistry: characterPartInfo → characterEquipment,
    // mountPartInfo → mountEquipment. Предметы, неизвестные реестру, идут в
    // одежду (прежнее поведение, чтобы ничего не пропадало).
    const auto equipmentRecords = GetServerInstance().GetDataDirector().GetItemCache().Get(
      character.characterEquipment());
    if (equipmentRecords)
    {
      for (const auto& equipmentRecord : *equipmentRecords)
      {
        auto equipmentTid{data::InvalidTid};
        protocol::Item protocolItem{};

        equipmentRecord.Immutable([&equipmentTid, &protocolItem](const data::Item& item)
        {
          equipmentTid = item.tid();
          protocol::BuildProtocolItem(protocolItem, item);
        });

        const auto equipmentTemplate = GetServerInstance().GetItemRegistry().GetItem(
          equipmentTid);

        const bool isMountEquipment = equipmentTemplate.has_value()
          && equipmentTemplate->mountPartInfo.has_value()
          && not equipmentTemplate->characterPartInfo.has_value();

        if (isMountEquipment)
          notify.mountEquipment.emplace_back(protocolItem);
        else
          notify.characterEquipment.emplace_back(protocolItem);
      }
    }

    // Mount record
    const auto mountRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
      character.mountUid());

    mountRecord->Immutable([&notify](const data::Horse& mount)
    {
      protocol::BuildProtocolHorse(notify.mount, mount);
    });
  });

  // Broadcast to all the ranch clients.
  const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];
  for (ClientId ranchClientId : ranchInstance.clients)
  {
    // Prevent broadcasting to self.
    if (ranchClientId == clientId)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
}

bool RanchDirector::HandleUseFoodItem(
  const data::Uid characterUid,
  const data::Uid mountUid,
  const data::Tid usedItemTid,
  protocol::AcCmdCRUseItemOK& response)
{
  // This action type has 
  response.type = protocol::AcCmdCRUseItemOK::ActionType::Feed;

  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    characterUid);
  const auto mountRecord = _serverInstance.GetDataDirector().GetHorse(
    mountUid);
  const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(
    usedItemTid);
  assert(itemTemplate && itemTemplate->foodParameters);

  // Update plenitude and friendliness points according to the item used.
  // LOA (batch2 phase 2): effective care-skill ceilings for the ACTING
  // character. Read the learned {id,rank} set first (Immutable), release, THEN
  // compute the ceilings — this Immutable and the later accrual Mutable stay
  // SEQUENTIAL (never nested) so the non-recursive record lock can't deadlock.
  // EffectType 0 (Cook) raises the plenitude ceiling; EffectType 1 (One Body)
  // raises the attachment ceiling. Only op==Add ranks contribute (SumAddBonus).
  std::vector<std::pair<uint8_t, uint8_t>> learnedCareSkills;
  characterRecord.Immutable(
    [&learnedCareSkills](const data::Character& character)
    {
      for (const auto& learned : character.careSkills.learnedRanks())
        learnedCareSkills.emplace_back(learned.id, learned.rank);
    });
  const auto& careRegistry = _serverInstance.GetCareSkillRegistry();
  const uint16_t plenitudeCeiling = static_cast<uint16_t>(
    MaxPlenitude + careRegistry.SumAddBonus(learnedCareSkills, 0));
  const uint16_t attachmentCeiling = static_cast<uint16_t>(
    MaxAttachment + careRegistry.SumAddBonus(learnedCareSkills, 1));

  // LOA-fix (R48-10, #58/R2-D): признак «наелась досыта» снимаем ПРЯМО ЗДЕСЬ,
  // в момент кормления, а не догадкой после.
  bool isHorseStuffed = false;

  mountRecord.Mutable([&itemTemplate, &isHorseStuffed, plenitudeCeiling, attachmentCeiling](data::Horse& horse)
  {
    // TODO: there's a ranch skill which gives bonus to these points

    horse.mountCondition.plenitude() = std::min(
      static_cast<uint16_t>(
        horse.mountCondition.plenitude() + itemTemplate->foodParameters->plenitudePoints),
      plenitudeCeiling
    );

    // Условие «Сытно накормить» в оригинале звучало буквально так:
    // `Plenitude == MaxPlenitude` (ach_conditions.lua). Потолок берём тот же
    // самый, что и строкой выше, — вместе с надбавкой навыка ухода «Повар»,
    // иначе достижение стало бы недостижимым ровно у тех, кто этот навык учил.
    isHorseStuffed = horse.mountCondition.plenitude() >= plenitudeCeiling;
    
    horse.mountCondition.friendliness() = std::min(
      static_cast<uint16_t>(
        horse.mountCondition.friendliness() + itemTemplate->foodParameters->friendlinessPoints),
      MaxFriendliness
    );

    // TODO: confirm this behaviour
    // Rationale: friendliness/charm max = 1000, play activities unlock after ~111 and ~501
    // which roughly corresponds to attachment values
    horse.mountCondition.attachment() = std::min(
      static_cast<uint16_t>(
        horse.mountCondition.attachment() + itemTemplate->foodParameters->friendlinessPoints),
      attachmentCeiling
    );
  });

  // TODO: determine values
  response.experiencePoints = 1;
  response.playSuccessLevel = protocol::AcCmdCRUseItemOK::PlaySuccessLevel::Bad;

  // LOA (batch2 phase 2): accrue care-class ("Смотритель") progress for the
  // acting character. careProgress is CUMULATIVE xp (clamped at 2675);
  // careClassLevel is derived from CareSkillLevel thresholds; each level gained
  // grants CARE_POINTS_PER_LEVEL spendable carePoints. Separate Mutable AFTER the
  // Immutable read above → SEQUENTIAL, not nested (no record-lock self-deadlock).
  characterRecord.Mutable(
    [&careRegistry](data::Character& character)
    {
      constexpr uint32_t CARE_XP_PER_ACTION = 10;
      constexpr uint32_t CARE_POINTS_PER_LEVEL = 2;
      constexpr uint32_t CARE_PROGRESS_MAX = 2675;
      const uint32_t newProgress = std::min(
        character.careSkills.careProgress() + CARE_XP_PER_ACTION,
        CARE_PROGRESS_MAX);
      character.careSkills.careProgress() = newProgress;
      const uint8_t newLevel = careRegistry.GetLevelForProgress(newProgress);
      if (newLevel > character.careSkills.careClassLevel())
      {
        character.careSkills.carePoints() += CARE_POINTS_PER_LEVEL
          * static_cast<uint32_t>(newLevel - character.careSkills.careClassLevel());
        character.careSkills.careClassLevel() = newLevel;
      }
    });

  // todo: award experiences gained
  // todo: client-side update of plenitude and friendliness stats

  // LOA-fix (#25): инкремент прогресса count-квестов + live-notify (растёт при действии).
  characterRecord.Immutable([this, characterUid](const data::Character& character)
  {
    const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(
      character.quests());
    if (not questRecords)
      return;
    for (const auto& questRecord : *questRecords)
    {
      questRecord.Mutable([this, characterUid](data::Quest& quest)
      {
        if (quest.isCompleted() != data::Quest::Status::InProgress)
          return;
        const auto qid = static_cast<uint32_t>(quest.questId());
        bool match = false;
        for (uint32_t feedTid : {1003u, 1014u, 11039u, 14011u})
          if (feedTid == qid) { match = true; break; }
        if (not match)
          return;
        const auto tmpl = _serverInstance.GetQuestRegistry().GetQuest(qid);
        if (not tmpl.has_value())
          return;

        // LOA-fix (F5, quest-batch-1): ПОРЯДОК ПРОВЕРОК. Раньше здесь стояло
        // `if (progress >= successValue) return;` ДО перевода статуса, поэтому
        // запись, уже стоявшая на N/N со статусом InProgress, проваливалась в
        // return и не становилась ReadyToClaim НИКОГДА — игрок заперт (живой
        // пример в проде: data/quests/5.json, квест 11039, 10/10, InProgress).
        // Теперь кап и перевод статуса разделены: инкрементим только пока цель
        // не достигнута, а ReadyToClaim выставляем всегда по факту достижения.
        if (quest.progress() < tmpl->successValue)
          quest.progress() = quest.progress() + 1;

        if (quest.progress() >= tmpl->successValue)
          quest.isCompleted() = data::Quest::Status::ReadyToClaim;

        protocol::AcCmdRCUpdateQuestNotify notify{};
        notify.characterUid = static_cast<uint32_t>(characterUid);
        notify.questTid = static_cast<uint16_t>(qid);
        notify.objectiveProgress.progress = quest.progress();
        notify.objectiveProgress.isCompleted = quest.progress() >= tmpl->successValue;
        _commandServer.QueueCommand<protocol::AcCmdRCUpdateQuestNotify>(
          GetClientIdByCharacterUid(characterUid),
          [notify]() { return notify; });
      });
    }
  });

  // LOA-fix (F8, quest-batch-1): дейлики ухода «покорми лошадь» (1003 — 5 раз,
  // 1014 — 10 раз, 1022 — 1 раз). Они лежат в трёх слотах data::DailyQuestGroup,
  // цикл по character.quests() выше их не видит. Раньше их (ошибочно) двигал
  // финиш заезда — F8 это прекратил, поэтому кормление обязано слать событие само.
  {
    const auto dailyNotifies = _serverInstance.GetQuestSystem().OnQuestEvent(
      characterUid,
      QuestSystem::QuestEvent::Any,
      registry::Quest::GameModeFlag::None,
      0,
      {1003u, 1014u, 1022u});
    for (const auto& dailyNotify : dailyNotifies)
      SendDailyQuestNotificationToCharacter(characterUid, dailyNotify);
  }

  // LOA-fix (R48-10, #58/R2-D): кормление двигает «Сытно накормить» (событие
  // 53, условие Stuffed) — и ТОЛЬКО его.
  // ★Два соседних достижения того же события включать НЕЛЬЗЯ: «Только любимое»
  // и «Разнообразное питание» спрашивают, любит ли ЭТА лошадь ЭТОТ корм
  // (`IsPreferFood` оригинала), а такого признака у сервера нет. В данных есть
  // PreferType у корма (3/9/11/20/36/80/224 — похоже на битовую маску) и
  // tendency у лошади (шесть нравов), но связь между ними НЕ доказана. Пока не
  // доказана — условие не срабатывает: выданный по ошибке тир не отзывается.
  if (isHorseStuffed)
  {
    static constexpr std::string_view kStuffedCondition[] = {"Stuffed"};
    SendAchievementEvent(characterUid, 53, kStuffedCondition);
  }

  return true;
}

bool RanchDirector::HandleUseCleanItem(
  const data::Uid characterUid,
  const data::Uid mountUid,
  const data::Tid usedItemTid,
  protocol::AcCmdCRUseItemOK& response)
{
  response.type = protocol::AcCmdCRUseItemOK::ActionType::Wash;

  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    characterUid);
  const auto mountRecord = _serverInstance.GetDataDirector().GetHorse(
    mountUid);
  const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(
    usedItemTid);
  assert(itemTemplate && itemTemplate->careParameters);

  // Update clean and polish points according to the item used.
  // LOA (batch2 phase 2): effective attachment ceiling for the ACTING character
  // (EffectType 1 "One Body"). Wash has no server-side EffectType-0/charm skill
  // (Stylist = client EffectType 3, Beautician = RNG EffectType 2) → the charm
  // ceiling is left untouched. Read learned {id,rank} first (Immutable), release,
  // then compute — this Immutable and the later accrual Mutable stay SEQUENTIAL.
  std::vector<std::pair<uint8_t, uint8_t>> learnedCareSkills;
  characterRecord.Immutable(
    [&learnedCareSkills](const data::Character& character)
    {
      for (const auto& learned : character.careSkills.learnedRanks())
        learnedCareSkills.emplace_back(learned.id, learned.rank);
    });
  const auto& careRegistry = _serverInstance.GetCareSkillRegistry();
  const uint16_t attachmentCeiling = static_cast<uint16_t>(
    MaxAttachment + careRegistry.SumAddBonus(learnedCareSkills, 1));

  mountRecord.Mutable([&itemTemplate, attachmentCeiling](data::Horse& horse)
  {
    // todo: there's a ranch skill which gives bonus to these points

    switch (itemTemplate->careParameters->parts)
    {
      case registry::Item::CareParameters::Part::Body:
      {
        horse.mountCondition.bodyDirtiness() = 0;
        break;
      }
      case registry::Item::CareParameters::Part::Mane:
      {
        horse.mountCondition.maneDirtiness() = 0;
        break;
      }
      case registry::Item::CareParameters::Part::Tail:
      {
        horse.mountCondition.tailDirtiness() = 0;
        break;
      }
    }
    
    // Set horse charm (attractiveness) to new incremented value or max
    horse.mountCondition.charm() = std::min(
      static_cast<uint16_t>(
        horse.mountCondition.charm() + itemTemplate->careParameters->cleanPoints),
      MaxCharm
    );

    // TODO: confirm this behaviour
    // Rationale: friendliness/charm max = 1000, play activities unlock after ~111 and ~501
    // which roughly corresponds to attachment values
    // Set horse attachment (boredom) value to new incremented value or max
    horse.mountCondition.attachment() = std::min(
      static_cast<uint16_t>(
        horse.mountCondition.attachment() + itemTemplate->careParameters->cleanPoints),
      attachmentCeiling
    );
  });

  // TODO: determine values
  response.experiencePoints = 1;
  // TODO: is this needed? confirm
  response.playSuccessLevel = protocol::AcCmdCRUseItemOK::PlaySuccessLevel::Perfect;

  // LOA (batch2 phase 2): accrue care-class ("Смотритель") progress for the
  // acting character (same rule as feed). careProgress cumulative, clamped 2675;
  // careClassLevel derived from thresholds; +CARE_POINTS_PER_LEVEL per level up.
  // Separate Mutable AFTER the Immutable read above → SEQUENTIAL, not nested.
  characterRecord.Mutable(
    [&careRegistry](data::Character& character)
    {
      constexpr uint32_t CARE_XP_PER_ACTION = 10;
      constexpr uint32_t CARE_POINTS_PER_LEVEL = 2;
      constexpr uint32_t CARE_PROGRESS_MAX = 2675;
      const uint32_t newProgress = std::min(
        character.careSkills.careProgress() + CARE_XP_PER_ACTION,
        CARE_PROGRESS_MAX);
      character.careSkills.careProgress() = newProgress;
      const uint8_t newLevel = careRegistry.GetLevelForProgress(newProgress);
      if (newLevel > character.careSkills.careClassLevel())
      {
        character.careSkills.carePoints() += CARE_POINTS_PER_LEVEL
          * static_cast<uint32_t>(newLevel - character.careSkills.careClassLevel());
        character.careSkills.careClassLevel() = newLevel;
      }
    });

  // todo: client-side update of clean and polish stats

  // LOA-fix (#25): инкремент прогресса count-квестов + live-notify (растёт при действии).
  characterRecord.Immutable([this, characterUid](const data::Character& character)
  {
    const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(
      character.quests());
    if (not questRecords)
      return;
    for (const auto& questRecord : *questRecords)
    {
      questRecord.Mutable([this, characterUid](data::Quest& quest)
      {
        if (quest.isCompleted() != data::Quest::Status::InProgress)
          return;
        const auto qid = static_cast<uint32_t>(quest.questId());
        bool match = false;
        for (uint32_t washTid : {1004u, 1015u, 11040u, 14012u})
          if (washTid == qid) { match = true; break; }
        if (not match)
          return;
        const auto tmpl = _serverInstance.GetQuestRegistry().GetQuest(qid);
        if (not tmpl.has_value())
          return;

        // LOA-fix (F5, quest-batch-1): см. хук кормления выше — кап и перевод
        // статуса разделены, чтобы добитая до цели InProgress-запись становилась
        // ReadyToClaim, а не застревала навсегда.
        if (quest.progress() < tmpl->successValue)
          quest.progress() = quest.progress() + 1;

        if (quest.progress() >= tmpl->successValue)
          quest.isCompleted() = data::Quest::Status::ReadyToClaim;

        protocol::AcCmdRCUpdateQuestNotify notify{};
        notify.characterUid = static_cast<uint32_t>(characterUid);
        notify.questTid = static_cast<uint16_t>(qid);
        notify.objectiveProgress.progress = quest.progress();
        notify.objectiveProgress.isCompleted = quest.progress() >= tmpl->successValue;
        _commandServer.QueueCommand<protocol::AcCmdRCUpdateQuestNotify>(
          GetClientIdByCharacterUid(characterUid),
          [notify]() { return notify; });
      });
    }
  });

  // LOA-fix (F8, quest-batch-1): дейлики ухода «помой лошадь» (1004 — 4 раза,
  // 1015 — 9 раз, 1023 — 1 раз). См. комментарий в хуке кормления.
  {
    const auto dailyNotifies = _serverInstance.GetQuestSystem().OnQuestEvent(
      characterUid,
      QuestSystem::QuestEvent::Any,
      registry::Quest::GameModeFlag::None,
      0,
      {1004u, 1015u, 1023u});
    for (const auto& dailyNotify : dailyNotifies)
      SendDailyQuestNotificationToCharacter(characterUid, dailyNotify);
  }

  // LOA-fix (R47-7, #58/R2-C): мытьё двигает достижения «Чистый корпус» (50),
  // «Чистая грива» (51) и «Чистый хвост» (52). Часть тела сервер УЖЕ знает —
  // она пришла в самом предмете ухода (careParameters->parts) и выше по этому
  // же хендлеру решала, какую грязь обнулить. Событие берём оттуда же, а не
  // из слов клиента.
  {
    uint16_t achievementEvent = 0;
    switch (itemTemplate->careParameters->parts)
    {
      case registry::Item::CareParameters::Part::Body:
        achievementEvent = 50;
        break;
      case registry::Item::CareParameters::Part::Mane:
        achievementEvent = 51;
        break;
      case registry::Item::CareParameters::Part::Tail:
        achievementEvent = 52;
        break;
    }

    // LOA-fix (R48-4, #58/R2-D): та же отправка, что и была, но через общую
    // точку — вместе со всеми остальными хуками раунда.
    if (achievementEvent != 0)
      SendAchievementEvent(characterUid, achievementEvent);
  }

  return true;
}

bool RanchDirector::HandleUsePlayItem(
  const data::Uid characterUid,
  const data::Uid mountUid,
  const data::Tid usedItemTid,
  const protocol::AcCmdCRUseItem::PlaySuccessLevel successLevel,
  protocol::AcCmdCRUseItemOK& response)
{
  response.type = protocol::AcCmdCRUseItemOK::ActionType::Play;
  
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    characterUid);
  const auto mountRecord = _serverInstance.GetDataDirector().GetHorse(
    mountUid);
  const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(
    usedItemTid);
  assert(itemTemplate && itemTemplate->playParameters);

  // TODO: Make critical chance configurable. Currently 0->1 is 50% chance.
  std::uniform_int_distribution<uint32_t> critRandomDist(0, 1);
  auto crit = critRandomDist(server::util::GetRandomEngine());

  switch (successLevel)
  {
    case protocol::AcCmdCRUseItem::PlaySuccessLevel::Bad:
      response.playSuccessLevel = protocol::AcCmdCRUseItemOK::PlaySuccessLevel::Bad;
      break;
    case protocol::AcCmdCRUseItem::PlaySuccessLevel::Good:
      response.playSuccessLevel = crit ?
        protocol::AcCmdCRUseItemOK::PlaySuccessLevel::CriticalGood :
        protocol::AcCmdCRUseItemOK::PlaySuccessLevel::Good;
      break;
    case protocol::AcCmdCRUseItem::PlaySuccessLevel::Perfect:
      response.playSuccessLevel = crit ?
        protocol::AcCmdCRUseItemOK::PlaySuccessLevel::CriticalPerfect :
        protocol::AcCmdCRUseItemOK::PlaySuccessLevel::Perfect;
      break;
  }

  mountRecord.Mutable([&itemTemplate](data::Horse& horse)
  {
    // As dictated by the intimacy gauge in-game
    const auto& newFriendlinessValue = static_cast<uint16_t>(
      horse.mountCondition.friendliness() + itemTemplate->playParameters->friendlinessPoints);

    // TODO: do normal/crit good/perfect plays affect the increment value?
    // Set friendliness (intimacy) to incremented value or max
    horse.mountCondition.friendliness() = std::min(
      newFriendlinessValue,
      MaxFriendliness);

    // TODO: implement boredom mechanism
  });

  // TODO: determine values
  response.experiencePoints = 1;
  // TODO: is this needed? confirm
  response.playSuccessLevel = protocol::AcCmdCRUseItemOK::PlaySuccessLevel::Perfect;

  return true;
}

bool RanchDirector::HandleUseCureItem(
  [[maybe_unused]] const data::Uid characterUid,
  const data::Uid mountUid,
  const data::Tid usedItemTid,
  protocol::AcCmdCRUseItemOK& response)
{
  response.type = protocol::AcCmdCRUseItemOK::ActionType::Cure;

  // LOA-fix (S2): лечим травму лошади подходящим лекарством. Каждый cure-item
  // декларирует ровно один код травмы (items.yaml cureParameters.injury:
  // 17/18/33/34/65/66 == data::Horse::Injury). Если у лошади ровно эта травма —
  // обнуляем в 0 (None).
  //
  // ★ИСПРАВЛЕНО (R64-2, backlog #223): здесь стояло «инфликт травм на сервере не
  // реализован (лошади не травмируются) → на практике injury()==0 и лечение
  // инертно». ЭТО НЕПРАВДА, и неправда дорогая: инфликт РЕАЛИЗОВАН в
  // `RaceInstance.cpp:300` (блок «S2: износ-травма на финише») — шанс 10% за
  // финиш, лёгкие коды {17, 33, 65}, только на здоровую лошадь. Прод это
  // подтверждает: из 132 лошадей шесть травмированы, и все шесть — ровно этими
  // тремя кодами.
  // ★Комментарий, противоречащий соседнему файлу, ХУЖЕ отсутствующего: он
  // ОСТАНАВЛИВАЕТ поиск. На нём уже купились при разборе живого теста —
  // «лечение инертно» приняли за доказательство, что механики нет.
  const auto mountRecord = _serverInstance.GetDataDirector().GetHorse(mountUid);
  const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(usedItemTid);

  if (itemTemplate && itemTemplate->cureParameters)
  {
    const uint32_t curedInjury = itemTemplate->cureParameters->injury;
    mountRecord.Mutable([curedInjury](data::Horse& horse)
    {
      if (horse.mountCondition.injury() == curedInjury)
        horse.mountCondition.injury() = 0;
    });
  }

  response.experiencePoints = 1;

  return true;
}

void RanchDirector::HandleUseItem(
  ClientId clientId,
  const protocol::AcCmdCRUseItem& command)
{
  protocol::AcCmdCRUseItemOK response{
    response.itemUid = command.itemUid,
    response.remainingItemCount = command.always1,
    response.type = protocol::AcCmdCRUseItemOK::ActionType::Generic};

  auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  const auto usedItemUid = command.itemUid;
  const auto horseUid = command.horseUid;

  bool hasItem = false;
  bool hasHorse = false;
  uint32_t carrotCount = 0;
  std::string characterName;
  characterRecord.Immutable([&characterName, &usedItemUid, &horseUid, &hasItem, &hasHorse, &carrotCount](
    const data::Character& character)
  {
    hasItem = std::ranges::contains(character.inventory(), usedItemUid);;
    hasHorse = std::ranges::contains(character.horses(), horseUid)
      || character.mountUid() == horseUid;

    characterName = character.name();
    carrotCount = character.carrots();
  });

  if (not hasItem || not hasHorse)
    throw std::runtime_error("Item or horse not owned by the character");

  const auto mountRecord = GetServerInstance().GetDataDirector().GetHorse(
    command.horseUid);
  const auto itemRecord = GetServerInstance().GetDataDirector().GetItem(
    command.itemUid);

  auto usedItemTid = data::InvalidTid;
  itemRecord.Immutable([&usedItemTid](const data::Item& item)
  {
    usedItemTid = item.tid();
  });

  const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(
    usedItemTid);
  if (not itemTemplate)
    throw std::runtime_error("Item template not available");

  if (itemTemplate->type != registry::Item::Type::Consumable)
  {
    throw std::runtime_error(std::format(
      "Use of unconsumable item {} (tid: {})",
      command.itemUid,
      usedItemTid));
  }

  bool consumeItem = false;
  if (itemTemplate->foodParameters)
  {
    consumeItem = HandleUseFoodItem(
      clientContext.characterUid,
      horseUid,
      usedItemTid,
      response);
  }
  else if (itemTemplate->careParameters)
  {
    consumeItem = HandleUseCleanItem(
      clientContext.characterUid,
      horseUid,
      usedItemTid,
      response);
  }
  else if (itemTemplate->playParameters)
  {
    consumeItem = HandleUsePlayItem(
      clientContext.characterUid,
      horseUid,
      usedItemTid,
      command.playSuccessLevel,
      response);
  }
  else if (itemTemplate->cureParameters)
  {
    consumeItem = HandleUseCureItem(
      clientContext.characterUid,
      horseUid,
      usedItemTid,
      response);

    protocol::AcCmdCRMountInjuryHealOK cure{
      .horseUid = horseUid,
      .unk1 = 0,
      .unk2 = 0,
      .updatedCarrotCount = carrotCount
    };

    _commandServer.QueueCommand<decltype(cure)>(
      clientId,
      [cure]()
      {
        return cure;
      });
  }
  else if (usedItemTid == InstantGrowUpItemTid)
  {
    // Instantly matures a foal into an adult horse.
    protocol::AcCmdRCUpdateMountInfoNotify growUp{
      .characterUid = clientContext.characterUid,
      .action = protocol::AcCmdRCUpdateMountInfoNotify::Action::PutHorseInRentOrBreedingSystem};

    mountRecord.Mutable([&growUp](data::Horse& horse)
    {
      if (horse.type() == data::Horse::Type::Foal)
        horse.type() = data::Horse::Type::Adult;

      protocol::BuildProtocolHorse(growUp.horse, horse);
    });

    clientContext.maturingFoals.erase(command.horseUid);

    _commandServer.QueueCommand<decltype(growUp)>(
      clientId,
      [growUp]()
      {
        return growUp;
      });

    protocol::AcCmdRCAddIdleMountInfoNotify addNotify{};
    addNotify.horse.horseOid =
      _ranches[clientContext.characterUid].tracker.GetHorseOid(command.horseUid);
    mountRecord.Immutable([&addNotify](const data::Horse& horse)
    {
      protocol::BuildProtocolHorse(addNotify.horse.horse, horse);
    });

    if (clientContext.visitingRancherUid == clientContext.characterUid)
    {
      // The owner is on their own ranch; broadcast the new idle mount to everyone there.
      for (const ClientId& ranchClientId : _ranches[clientContext.characterUid].clients)
      {
        _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
          ranchClientId,
          [addNotify]()
          {
            return addNotify;
          });
      }
    }
    else
    {
      _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
        clientId,
        [addNotify]()
        {
          return addNotify;
        });

      protocol::AcCmdRCMobDead mobDead{
        .mobOid = addNotify.horse.horseOid};
      _commandServer.QueueCommand<protocol::AcCmdRCMobDead>(
        clientId,
        [mobDead]()
        {
          return mobDead;
        });
    }

    consumeItem = true;
  }
  else
  {
    server::util::QuietLogWarn(
      "Use of unhandled item {} (tid: {})",
      command.itemUid,
      usedItemTid);
  }

  if (consumeItem)
  {
    characterRecord.Mutable([this, &usedItemTid, &response](data::Character& character)
    {
      const auto consumeVerdict = GetServerInstance().GetItemSystem().ConsumeItem(
        character, usedItemTid, 1);

      response.remainingItemCount = static_cast<uint16_t>(
        consumeVerdict.remainingItemCount);
    });
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  // Perform a mount update
  constexpr uint32_t HorseRenameItemTid = 45003;
  const bool refreshMountInfo =
    usedItemTid == HorseRenameItemTid
    || response.type == protocol::AcCmdCRUseItemOK::ActionType::Feed
    || response.type == protocol::AcCmdCRUseItemOK::ActionType::Wash
    || response.type == protocol::AcCmdCRUseItemOK::ActionType::Play
    || response.type == protocol::AcCmdCRUseItemOK::ActionType::Cure;

  if (refreshMountInfo)
  {
    protocol::AcCmdCRUpdateMountInfoOK mountOk{
      .action = protocol::AcCmdCRUpdateMountInfo::Action::Rename,};

    const auto horseRecord = _serverInstance.GetDataDirector().GetHorse(horseUid);
    horseRecord.Immutable([&mountOk](const data::Horse& horse)
    {
      protocol::BuildProtocolHorse(mountOk.horse, horse);
    });

    _commandServer.QueueCommand<decltype(mountOk)>(
      clientId,
      [mountOk]()
      {
        return mountOk;
      });

    // LOA-fix (SYNC-5): let the rest of the ranch see the care result.
    //
    // Feeding / washing / playing / curing only ever answered the actor, so
    // guests standing right next to the horse kept a stale view of its
    // cleanliness, fullness, injury and experience until they re-entered the
    // ranch. We reuse the exact mechanism that already ships horse renames to
    // the whole ranch (HandleUpdateMountNickname): AcCmdRCUpdateMountInfoNotify
    // plus the ranch client loop with skip-self. No new opcode, no new struct.
    //
    // Guard: the notify is keyed by characterUid, so the client applies it to
    // THAT character's mount in the scene. Care can target any stabled horse,
    // and broadcasting a stabled horse under the character's identity could
    // swap the mount guests see. So we only broadcast when the cared-for horse
    // really is the character's current mount.
    bool isActiveMount = false;
    characterRecord.Immutable(
      [&isActiveMount, horseUid](const data::Character& character)
      {
        isActiveMount = character.mountUid() == horseUid;
      });

    if (isActiveMount)
    {
      protocol::AcCmdRCUpdateMountInfoNotify careNotify{
        .characterUid = clientContext.characterUid};

      horseRecord.Immutable([&careNotify](const data::Horse& horse)
      {
        protocol::BuildProtocolHorse(careNotify.horse, horse);
      });

      for (const ClientId& ranchClientId : _ranches[clientContext.visitingRancherUid].clients)
      {
        // Prevent broadcast to self - the actor already got the OK above.
        if (ranchClientId == clientId)
          continue;

        _commandServer.QueueCommand<decltype(careNotify)>(
          ranchClientId,
          [careNotify]()
          {
            return careNotify;
          });
      }
    }
  }
}

void RanchDirector::HandleHousingBuild(
  ClientId clientId,
  const protocol::AcCmdCRHousingBuild& command)
{
  //! The double incubator does not utilize the HousingRepair,
  //! instead it just creates a new double incubator
  //! TODO: make the check if the incubator already exists and set the durability back to 10

  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // LOA-fix (R29-5a, #59 S22, SECURITY+ЭКОНОМИКА): HandleHousingBuild строил ЧТО
  // УГОДНО и СКОЛЬКО УГОДНО. Ни цены, ни валидации command.housingTid, ни лимита:
  // клиент мог слать 0x25b с housingTid=52 (DoubleIncubatorId) бесконечно и получать
  // бесплатные слоты яиц — прямо в подсистему, которую закрывали R22/R28, — плюс
  // неограниченный рост character.housing() (раздувание EnterRanch-ответа и JSON).
  // Объявленный на :45 MaxRanchHousingCount = 13 НЕ ИСПОЛЬЗОВАЛСЯ НИГДЕ.
  //
  // ЦЕНУ НЕ БЕРЁМ: прайса на постройки нет ни в items.yaml, ни в разобранных
  // клиентских данных — это owner-decision + работа по каталогу, не этот раунд.
  // ALLOWLIST housingTid СОЗНАТЕЛЬНО НЕ ДЕЛАЕМ: реальный набор id, который шлёт
  // ванильный клиент, с pcap не снят, и «сузить» вслепую = убить легальную
  // декорацию (ровно ловушка R22-8). Отдельный пункт бэклога.
  // ЗДЕСЬ ТОЛЬКО ДВА ГЕЙТА, каждый лишь УБИРАЕТ эмиссию.
  bool housingLimitReached = false;
  bool housingCacheCold = false;
  data::Uid existingHousingUid = data::InvalidUid;
  characterRecord.Immutable(
    [this, &command, &housingLimitReached, &housingCacheCold, &existingHousingUid](const data::Character& character)
    {
      housingLimitReached = character.housing().size() >= MaxRanchHousingCount;

      // Ищем УЖЕ построенное жильё с тем же housingId — основа дубль-гейта (R29-5b).
      if (character.housing().empty())
        return;

      const auto housingRecords = GetServerInstance().GetDataDirector().GetHousingCache().Get(
        character.housing());
      if (not housingRecords)
      {
        housingCacheCold = true;
        return;
      }

      for (const auto& housingRecord : *housingRecords)
      {
        housingRecord.Immutable([&command, &existingHousingUid](const data::Housing& housing)
        {
          if (housing.housingId() == command.housingTid)
            existingHousingUid = housing.uid();
        });
      }
    });

  // LOA-fix (R29-5c, #59 S22, SECURITY): холодный housing-кэш => FAIL-CLOSED refuse
  // (дисциплина R23/#98). Если кэш не прогрет, дубль-гейт R29-5b слепнет и CREATE-ветка
  // плодит дубликат бесплатного инкубатора — ре-открывает эксплойт, что мы закрываем.
  // На холодном кэше отказываем, а не строим. Транзиентно; клиент повторит.
  if (housingCacheCold)
  {
    server::util::QuietLogWarn("HousingBuild: character {} housing cache cold; refusing (fail-closed)",
      clientContext.characterUid);
    return;
  }

  // (гейт а) cap. ★FAIL-OPEN по уже существующему переполнению: режем только СОЗДАНИЕ
  // НОВОЙ записи и НИКОГДА не удаляем существующие — у живых персонажей за годы могло
  // накопиться >13, и удаление было бы разрушительным. Перестройка уже имеющегося
  // жилья (existingHousingUid) размер character.housing() не увеличивает и проходит
  // даже при переполнении, иначе игрок с легаси-переполнением не смог бы даже
  // обновить свой инкубатор.
  if (housingLimitReached && existingHousingUid == data::InvalidUid)
  {
    server::util::QuietLogWarn("HousingBuild: character {} is at the housing cap ({}); refusing to build {}",
      clientContext.characterUid, MaxRanchHousingCount, command.housingTid);
    return;
  }

  auto housingUid = data::InvalidUid;

  // LOA-fix (R29-5b, #59 S22, SECURITY): апстримный TODO (он же комментарий в шапке
  // хендлера) закрыт. Если жильё с этим housingId у персонажа УЖЕ есть — перестройка
  // ОБНОВЛЯЕТ существующую запись (двойному инкубатору возвращаем durability = 10,
  // прочему — срок), а НЕ плодит новую. Без этого 0x25b с housingTid = 52 создавал
  // бесконечные бесплатные инкубаторы и неограниченно растил character.housing().
  // OK-ответ теперь шлётся НИЖЕ, после успешной мутации (R29-5d) — «rebuild = repair».
  if (existingHousingUid != data::InvalidUid)
  {
    const auto existingRecord = GetServerInstance().GetDataDirector().GetHousingCache(
      existingHousingUid);
    if (existingRecord)
    {
      existingRecord.Mutable([housingId = command.housingTid](data::Housing& housing)
      {
        if (housingId == DoubleIncubatorId)
          housing.durability = 10;
        else
          housing.expiresAt = std::chrono::system_clock::now() + std::chrono::days(20);
      });
    }
    else
    {
      // LOA-fix (R29-5e, #59 S22, SECURITY): запись жилья недоступна на перестройке
      // (после R29-5c кэш тёплый, поэтому практически недостижимо) — FAIL-CLOSED:
      // выходим ДО OK/notify, иначе клиент получил бы ложный OK без мутации. (Codex-T3 #2.)
      server::util::QuietLogWarn("HousingBuild: character {} rebuilt housing {} whose record is unavailable; refusing (fail-closed)",
        clientContext.characterUid, existingHousingUid);
      return;
    }
  }
  else
  {
    const auto housingRecord = GetServerInstance().GetDataDirector().CreateHousing();
    if (not housingRecord)
    {
      throw std::runtime_error(
        std::format("Failed to create housing for user {}", clientContext.userName));
    }

    housingRecord.Mutable([housingId = command.housingTid, &housingUid](data::Housing& housing)
    {
      housing.housingId = housingId;
      housingUid = housing.uid();

      if (housingId == DoubleIncubatorId)
        housing.durability = 10;
      else
        housing.expiresAt = std::chrono::system_clock::now() + std::chrono::days(20);
    });

    characterRecord.Mutable([&housingUid](data::Character& character)
    {
      character.housing().emplace_back(housingUid);
    });
  }

  // LOA-fix (R29-5d, #59 S22, SECURITY): OK-ответ ПОСЛЕ успешной мутации. Раньше OK
  // уезжал ДО create/update — при сбое мутации (или холодном кэше, ныне закрытом
  // R29-5c) клиент получал ложный OK. Теперь строим/обновляем, потом подтверждаем.
  // (закрывает Codex-T3 WARN #2.)
  protocol::AcCmdCRHousingBuildOK response{
    .member1 = clientContext.characterUid,
    .housingTid = command.housingTid,
    .member3 = 10,
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  assert(clientContext.visitingRancherUid == clientContext.characterUid);

  protocol::AcCmdCRHousingBuildNotify notify{
    .member1 = 1,
    .housingId = command.housingTid,
  };

  // Broadcast to all the ranch clients.
  const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];
  for (ClientId ranchClientId : ranchInstance.clients)
  {
    // Prevent broadcasting to self.
    if (ranchClientId == clientId)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void RanchDirector::HandleHousingRepair(
  ClientId clientId,
  const protocol::AcCmdCRHousingRepair& command)
{
  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);
  
  // LOA-fix (R29-6, #59 S22, SECURITY): HandleHousingRepair брал
  // GetHousingCache(command.housingUid) БЕЗ ЕДИНОЙ проверки владения — то есть
  // бесплатно чинил ЛЮБОЙ housing-uid в мире, включая ЧУЖОЙ (кросс-аккаунтная запись
  // в чужие данные). Плюс `uint16_t housingId;` читался неинициализированным, если
  // Mutable не выполнялся, и уезжал в broadcast-notify.
  uint16_t housingId = 0;

  bool ownsHousing = false;
  characterRecord.Immutable([&command, &ownsHousing](const data::Character& character)
  {
    ownsHousing = std::ranges::contains(character.housing(), command.housingUid);
  });

  if (not ownsHousing)
  {
    server::util::QuietLogWarn("HousingRepair: character {} named housing {} it does not own; refusing",
      clientContext.characterUid, command.housingUid);
    return;
  }

  const auto housingRecord = GetServerInstance().GetDataDirector().GetHousingCache(
    command.housingUid);
  if (not housingRecord)
  {
    server::util::QuietLogWarn("HousingRepair: housing {} record unavailable for character {}; refusing",
      command.housingUid, clientContext.characterUid);
    return;
  }

  housingRecord.Mutable([&housingId](data::Housing& housing){
    housing.expiresAt = std::chrono::system_clock::now() + std::chrono::days(20);
    housingId = static_cast<uint16_t>(housing.housingId());
  });

  // todo: implement transaction for the repair

  protocol::AcCmdCRHousingRepairOK response{
    .housingUid = command.housingUid,
    .member2 = 1,
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  assert(clientContext.visitingRancherUid == clientContext.characterUid);

  protocol::AcCmdCRHousingBuildNotify notify{
    .member1 = 1,
    .housingId = housingId,
  };

  // Broadcast to all the ranch clients.
  const auto& ranchInstance = _ranches[clientContext.visitingRancherUid];
  for (ClientId ranchClientId : ranchInstance.clients)
  {
    // Prevent broadcasting to self.
    if (ranchClientId == clientId)
      continue;

    _commandServer.QueueCommand<decltype(notify)>(
      ranchClientId,
      [notify]()
      {
        return notify;
      });
  }
};

void RanchDirector::HandleOpCmd(
  ClientId clientId,
  const protocol::AcCmdCROpCmd& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::vector<std::string> feedback;

  const auto chatVerdict = GetServerInstance().GetChatSystem().ProcessChatMessage(
    clientContext.characterUid, "//" + command.command);

  // LOA-fix (R55-7, round55, backlog #179 часть 5): см. R55-3.
  if (not chatVerdict)
    return;

  const auto& result = *chatVerdict;

  if (not result.commandVerdict)
  {
    return;
  }

  for (const auto response : result.commandVerdict->result)
  {
    _commandServer.QueueCommand<protocol::RanchCommandOpCmdOK>(
      clientId,
      [response = std::move(response)]()
      {
        return protocol::RanchCommandOpCmdOK{
          .feedback = response};
      });
  }
}

void RanchDirector::HandleRequestLeagueTeamList(
  ClientId clientId,
  const protocol::RanchCommandRequestLeagueTeamList&)
{
  protocol::RanchCommandRequestLeagueTeamListOK response{
    .season = 46,
    .league = 0,
    .group = 1,
    .points = 4,
    .rank = 10,
    .previousRank = 200,
    .breakPoints = 0,
    .unk7 = 0,
    .unk8 = 0,
    .lastWeekLeague = 1,
    .lastWeekGroup = 100,
    .lastWeekRank = 4,
    .lastWeekAvailable = 1,
    .unk13 = 1,
    .members = {
      protocol::RanchCommandRequestLeagueTeamListOK::Member{
        .uid = 1,
        .points = 4000,
        .name = "test"
      }}
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRecoverMount(
  ClientId clientId,
  const protocol::AcCmdCRRecoverMount command)
{
  protocol::AcCmdCRRecoverMountOK response{
    .horseUid = command.horseUid};

  bool horseValid = false;
  const auto& characterUid = GetClientContext(clientId).characterUid;
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(characterUid);
  
  characterRecord.Mutable([this, &response, &horseValid](data::Character& character)
  {
    const bool ownsHorse = character.mountUid() == response.horseUid ||
      std::ranges::contains(character.horses(), response.horseUid);

    const auto horseRecord = GetServerInstance().GetDataDirector().GetHorse(
      response.horseUid);

    // Check if the character owns the horse or exists in the data director
    if (not ownsHorse || character.carrots() <= 0 || not horseRecord.IsAvailable())
    {
      server::util::QuietLogWarn("Character {} unsuccessfully tried to recover horse {} stamina with {} carrots",
        character.name(), response.horseUid, character.carrots());
      return;
    }

    horseValid = true;
    horseRecord.Mutable([&character, &response](data::Horse& horse)
    {
      // Seems to always be 4000.
      constexpr uint16_t MaxHorseStamina = 4'000;
      // Each stamina point costs one carrot.
      constexpr double StaminaPointPrice = 1.0;
      
      // The stamina points the horse needs to recover to reach maximum stamina.
      const int32_t recoverableStamina = MaxHorseStamina - horse.mountCondition.stamina();
      
      // Recover as much required stamina as the user can afford with
      // the threshold being the max recoverable stamina.
      const int32_t staminaToRecover = std::min(
        recoverableStamina,
        static_cast<int32_t>(std::floor(character.carrots() / StaminaPointPrice)));
      
      horse.mountCondition.stamina() += staminaToRecover;
      character.carrots() -= static_cast<int32_t>(
        std::floor(staminaToRecover * StaminaPointPrice));
  
      response.stamina = static_cast<uint16_t>(
        horse.mountCondition.stamina());
      response.updatedCarrots = character.carrots();
    });
  });

  if (not horseValid)
  {
    const protocol::AcCmdCRRecoverMountCancel cancelResponse{
      .horseUid = command.horseUid};

    _commandServer.QueueCommand<decltype(cancelResponse)>(
      clientId,
      [cancelResponse]()
      {
        return cancelResponse;
      });
    
    return;
  }
  
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

bool RanchDirector::HandleMountFamilyTree(
  const ClientId clientId,
  const protocol::AcCmdCRMountFamilyTree& command)
{
  using HierarchyPosition = protocol::AcCmdCRMountFamilyTreeOK::MountFamilyTreeItem::Position;

  auto& dataDirector = GetServerInstance().GetDataDirector();
  protocol::AcCmdCRMountFamilyTreeOK response{};

  const auto sendResponse = [this, clientId, &response]()
  {
    _commandServer.QueueCommand<decltype(response)>(clientId, [response]()
    {
      return response;
    });
  };

  const auto mountRecord = dataDirector.GetHorse(command.horseUid);
  if (not mountRecord)
  {
    sendResponse();
    return false;
  }

  // todo: cache this

  // Set if a needed ancestor record isn't loaded yet; the command is then
  // deferred and retried once the record becomes available.
  bool defer = false;

  // Reads a horse's parent UIDs (InvalidUid when unknown).
  const auto getParents = [&dataDirector, &defer](const data::Uid horseUid) -> data::Horse::Ancestors
  {
    if (horseUid == data::InvalidUid)
      return {};

    const auto record = dataDirector.GetHorse(horseUid);
    if (not record)
    {
      defer = true;
      return {};
    }

    data::Horse::Ancestors parents;
    record.Immutable([&parents](const data::Horse& horse) { parents = horse.ancestors; });
    return parents;
  };

  const auto addAncestor = [&dataDirector, &response, &defer](
    const data::Uid horseUid, const HierarchyPosition position)
  {
    if (horseUid == data::InvalidUid)
      return;

    const auto record = dataDirector.GetHorse(horseUid);
    if (not record)
    {
      defer = true;
      return;
    }

    record.Immutable([&response, position](const data::Horse& horse)
    {
      response.ancestors.emplace_back(protocol::AcCmdCRMountFamilyTreeOK::MountFamilyTreeItem{
        .hierarchyPosition = position,
        .name = horse.name(),
        .grade = static_cast<uint8_t>(horse.grade()),
        .skinTid = static_cast<uint16_t>(horse.parts.skinTid())});
    });
  };

  data::Horse::Ancestors parents;
  mountRecord.Immutable([&parents](const data::Horse& horse) { parents = horse.ancestors; });

  const auto paternal = getParents(parents.father);
  const auto maternal = getParents(parents.mother);

  addAncestor(parents.father, HierarchyPosition::Father);
  addAncestor(paternal.father, HierarchyPosition::PaternalGrandfather);
  addAncestor(paternal.mother, HierarchyPosition::PaternalGrandmother);
  addAncestor(parents.mother, HierarchyPosition::Mother);
  addAncestor(maternal.father, HierarchyPosition::MaternalGrandfather);
  addAncestor(maternal.mother, HierarchyPosition::MaternalGrandmother);

  if (defer)
    return true;

  sendResponse();
  return false;
}

void RanchDirector::HandleCheckStorageItem(
  ClientId clientId,
  const protocol::AcCmdCRCheckStorageItem command)
{
  // No need to respond, only indicate to the server that
  // a stored item has been viewed
  const auto& characterUid = GetClientContext(clientId).characterUid;
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(characterUid);

  bool characterHasStoredItem = false;
  characterRecord.Immutable([&characterHasStoredItem, command](const data::Character& character)
  {
    characterHasStoredItem = 
      std::ranges::contains(character.purchases(), command.storedItemUid) ||
      std::ranges::contains(character.gifts(), command.storedItemUid);
  });

  if (not characterHasStoredItem)
  {
    server::util::QuietLogWarn("Character {} tried to check a stored item {} they do not have",
      characterUid, command.storedItemUid);
    return;
  }

  const auto& storedItemRecord = GetServerInstance().GetDataDirector().GetStorageItemCache(command.storedItemUid);
  storedItemRecord.Mutable([](data::StorageItem& storedItem)
  {
    storedItem.checked() = true;
  });
}

//! Changes the age of the calling character
//! If this is called, it implicitly means "hide age" is not selected on the client, so we show age
void RanchDirector::HandleChangeAge(
  const ClientId clientId,
  const protocol::AcCmdCRChangeAge command)
{
  const auto& clientContext = GetClientContext(clientId);

  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid)
    .Mutable([this, &clientContext, age = command.age](
      data::Character& character)
    {
      const auto settingsRecord = character.settingsUid() != data::InvalidUid
        ? GetServerInstance().GetDataDirector().GetSettings(character.settingsUid())
        : GetServerInstance().GetDataDirector().CreateSettings();

      if (not settingsRecord)
      {
        throw std::runtime_error(
          std::format("Failed to create or retrieve settings for user '{}'", clientContext.userName));
      }

      settingsRecord.Mutable(
        [&character, &age](data::Settings& settings)
        {
          // Age can only be changed if the "hide age and gender" option is not ticked
          settings.hideAge() = false;
          settings.age() = static_cast<uint8_t>(age);

          if (character.settingsUid() == data::InvalidUid)
            character.settingsUid = settings.uid();
        });
    });

  protocol::AcCmdCRChangeAgeOK response {
    .age = command.age};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  BroadcastChangeAgeNotify(
    clientContext.characterUid,
    clientContext.visitingRancherUid,
    command.age);
}

void RanchDirector::HandleHideAge(
  ClientId clientId,
  const protocol::AcCmdCRHideAge command)
{
  const auto& clientContext = GetClientContext(clientId);
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid)
    .Mutable([this, &clientContext, option = command.option](
      data::Character& character)
    {
      const auto settingsRecord = character.settingsUid() != data::InvalidUid
        ? GetServerInstance().GetDataDirector().GetSettings(character.settingsUid())
        : GetServerInstance().GetDataDirector().CreateSettings();

      if (not settingsRecord)
      {
        throw std::runtime_error(
          std::format("Failed to create or retrieve settings for user '{}'", clientContext.userName));
      }

      settingsRecord.Mutable(
        [&option, &character](data::Settings& settings)
        {
          settings.hideAge() = option == protocol::AcCmdCRHideAge::Option::Hidden;

          if (character.settingsUid() == data::InvalidUid)
            character.settingsUid = settings.uid();
        });
  });

  protocol::AcCmdCRHideAgeOK response {
    .option = command.option};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  BroadcastHideAgeNotify(
    clientContext.characterUid,
    clientContext.visitingRancherUid,
    command.option);
}

void RanchDirector::HandleStatusPointApply(
  ClientId clientId,
  const protocol::AcCmdCRStatusPointApply command)
{
  const auto& clientContext = GetClientContext(clientId);
  auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // Collect the owned horses by the user's character
  std::vector<data::Uid> ownedHorses;
  characterRecord.Mutable([&ownedHorses](data::Character& character)
  {
    ownedHorses.emplace_back(character.mountUid());
    std::ranges::copy(character.horses(), std::back_inserter(ownedHorses));
  });

  const bool isHorseOwned = std::ranges::contains(ownedHorses, command.horseUid);
  if (not isHorseOwned)
  {
    server::util::QuietLogWarn(
      "Character {} tried to apply status points to unowned horse {}",
      clientContext.characterUid, command.horseUid);

    _commandServer.QueueCommand<protocol::AcCmdCRStatusPointApplyCancel>(
      clientId,
      []()
      {
        return protocol::AcCmdCRStatusPointApplyCancel{};
      });
    return;
  }

  const auto horseRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
    command.horseUid);

  uint32_t horseGrade = 0;
  horseRecord->Immutable([&horseGrade](const data::Horse& horse)
  {
    horseGrade = horse.grade();
  });

  const auto* nextGradeInfo = GetServerInstance().GetHorseRegistry().GetGradeInfo(horseGrade + 1);

  bool applied = false;
  horseRecord->Mutable([&command, &applied, nextGradeInfo](data::Horse& horse)
  {
    if (horse.growthPoints() == 0)
      return;

    const int64_t agilityDelta = static_cast<int64_t>(command.stats.agility) - static_cast<int64_t>(horse.stats.agility());
    const int64_t ambitionDelta =  static_cast<int64_t>(command.stats.ambition) - static_cast<int64_t>(horse.stats.ambition());
    const int64_t rushDelta = static_cast<int64_t>(command.stats.rush) - static_cast<int64_t>(horse.stats.rush());
    const int64_t enduranceDelta = static_cast<int64_t>(command.stats.endurance) - static_cast<int64_t>(horse.stats.endurance());
    const int64_t courageDelta = static_cast<int64_t>(command.stats.courage) - static_cast<int64_t>(horse.stats.courage());

    // Decrease in any of the stats is not allowed.
    if (agilityDelta < 0
      || ambitionDelta < 0
      || rushDelta < 0
      || enduranceDelta < 0
      || courageDelta < 0)
    {
      return;
    }

   const auto totalPointsApplied = agilityDelta + ambitionDelta + rushDelta + enduranceDelta + courageDelta;

    // Increase  of  more than  one stat at a time is not allowed.
    if (totalPointsApplied > 1)
      return;

    const int32_t currentStatSum = horse.stats.agility() + horse.stats.ambition()
      + horse.stats.rush() + horse.stats.endurance() + horse.stats.courage();

    if (nextGradeInfo && currentStatSum >= nextGradeInfo->minStatSum)
      return;

    horse.stats.agility = command.stats.agility;
    horse.stats.ambition = command.stats.ambition;
    horse.stats.rush = command.stats.rush;
    horse.stats.endurance = command.stats.endurance;
    horse.stats.courage = command.stats.courage;
    horse.growthPoints() -= 1;

    if (nextGradeInfo && currentStatSum + 1 >= nextGradeInfo->minStatSum)
      horse.grade() += 1;

    applied = true;
  });

  if (not applied)
  {
    _commandServer.QueueCommand<protocol::AcCmdCRStatusPointApplyCancel>(
      clientId,
      []()
      {
        return protocol::AcCmdCRStatusPointApplyCancel{};
      });
    return;
  }

  protocol::AcCmdCRStatusPointApplyOK response{};
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

// LOA (batch2) phase 2: learn OR rank-up a care skill with server-authoritative
// validation. The client sends only {skillId} (1 byte); the target rank is
// server-derived = current rank + 1. Every number (point cost, required class
// level, prerequisite, max rank) comes from CareSkillRegistry — the client is
// never trusted for values. All state read + validation + mutation happen inside
// ONE characterRecord.Mutable (single exclusive lock, atomic, no TOCTOU and no
// Immutable/Mutable reentrancy). Registry lookups take no record lock, so calling
// them inside the Mutable is safe. Any refusal or error replies an empty Cancel.
void RanchDirector::HandleStudyCareSkill(
  ClientId clientId,
  const protocol::AcCmdCRStudyCareSkill& command)
{
  try
  {
    const auto& clientContext = GetClientContext(clientId);
    auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
      clientContext.characterUid);

    // Care-skill tables (read-only after startup; no locking of its own).
    const auto& careRegistry = GetServerInstance().GetCareSkillRegistry();

    bool applied = false;
    uint8_t newRankOut = 0;
    uint32_t pointsOut = 0;
    uint32_t progressOut = 0;
    characterRecord.Mutable(
      [&](data::Character& character)
      {
        auto& skills = character.careSkills;
        auto& learnedRanks = skills.learnedRanks();

        // Current rank of this skill (0 if not yet learned) → target rank.
        uint8_t currentRank = 0;
        for (const auto& entry : learnedRanks)
        {
          if (entry.id == command.skillId)
            currentRank = entry.rank;
        }
        const uint8_t newRank = static_cast<uint8_t>(currentRank + 1);

        // (skillId,newRank) must exist: nullptr = unknown skill OR already max.
        const auto* info = careRegistry.GetRankInfo(command.skillId, newRank);
        if (info == nullptr)
          return;  // no such rank / already at max → refuse (Cancel)
        if (skills.careClassLevel() < info->levelReq)
          return;  // care-class level too low → refuse
        if (skills.carePoints() < info->pointCost)
          return;  // not enough care points → refuse

        // Prerequisite (all zero in current data → no-op, but written generic):
        // player must own preSkillId at rank >= preSkillRank.
        if (info->preSkillId != 0)
        {
          bool prereqMet = false;
          for (const auto& entry : learnedRanks)
          {
            if (entry.id == info->preSkillId
              && entry.rank >= info->preSkillRank)
            {
              prereqMet = true;
              break;
            }
          }
          if (not prereqMet)
            return;  // prerequisite skill/rank missing → refuse
        }

        // Apply: bump the existing slot, or add a new one (12-entry cap kept —
        // LoginOK serialises the learned count as a single byte).
        for (auto& entry : learnedRanks)
        {
          if (entry.id == command.skillId)
          {
            entry.rank = newRank;
            skills.carePoints() = skills.carePoints() - info->pointCost;
            applied = true;
            newRankOut = newRank;
            pointsOut = skills.carePoints();
            progressOut = skills.careProgress();
            return;
          }
        }
        if (learnedRanks.size() >= 12)
          return;  // all 12 care skills already learned → refuse
        auto& slot = learnedRanks.emplace_back();
        slot.id = command.skillId;
        slot.rank = newRank;
        skills.carePoints() = skills.carePoints() - info->pointCost;
        applied = true;
        newRankOut = newRank;
        pointsOut = skills.carePoints();
        progressOut = skills.careProgress();
      });

    if (not applied)
    {
      // Any validation failure (or the 12-slot cap) → well-formed empty Cancel;
      // persisted data is left untouched.
      _commandServer.QueueCommand<protocol::AcCmdCRStudyCareSkillCancel>(
        clientId,
        []()
        {
          return protocol::AcCmdCRStudyCareSkillCancel{};
        });
      return;
    }

    // OK layout still best-guess (see design spec §9): echo the updated
    // single-skill state (points/progress are the real server values now).
    protocol::AcCmdCRStudyCareSkillOK response{};
    response.skillId = command.skillId;
    response.rank = newRankOut;
    response.points = static_cast<uint16_t>(pointsOut);
    response.progress = progressOut;
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
  }
  catch (const std::exception& e)
  {
    server::util::QuietLogWarn("HandleStudyCareSkill failed: {}", e.what());
    _commandServer.QueueCommand<protocol::AcCmdCRStudyCareSkillCancel>(
      clientId,
      []()
      {
        return protocol::AcCmdCRStudyCareSkillCancel{};
      });
  }
}

// LOA (batch2) phase 1: care-skill reset is a NON-DESTRUCTIVE no-op. The 0x27d
// request body and the 0x27e/0x27f reply layout are unverified guesses, so we
// never clear learnedRanks — clearing would be destructive if the client sends
// 0x27d for any reason other than a confirmed reset. We just log and reply an
// empty Cancel so the client gets a well-formed response instead of silence.
//
// NDEBUG/RelWithDebInfo dependency: the 0x27d body is Read as empty. The
// assert(cursor == size) at CommandServer.cpp:373 is compiled out in prod
// (RelWithDebInfo defines NDEBUG); a Debug build receiving a non-empty 0x27d
// body would abort at that assert.
void RanchDirector::HandleResetCareSkill(
  ClientId clientId,
  const protocol::AcCmdCRResetCareSkill&)
{
  const auto& clientContext = GetClientContext(clientId);
  server::util::QuietLogInfo(
    "Care-skill reset requested by {} — phase 1 no-op",
    clientContext.characterUid);
  _commandServer.QueueCommand<protocol::AcCmdCRResetCareSkillCancel>(
    clientId,
    []()
    {
      return protocol::AcCmdCRResetCareSkillCancel{};
    });
}

void RanchDirector::HandleGetGuildMemberList(
  ClientId clientId,
  const protocol::AcCmdCRGuildMemberList&)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto& characterRecord = GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid);

  // Get requesting character's guild
  auto guildUid = data::InvalidUid;
  characterRecord.Immutable([&guildUid](const data::Character& character)
  {
    guildUid = character.guildUid();
  });

  // Get and confirm guild exists
  const auto& guildRecord = GetServerInstance().GetDataDirector().GetGuild(guildUid);
  if (not guildRecord.IsAvailable())
  {
    protocol::AcCmdCRGuildMemberListCancel cancelResponse{
      .status = 2 // ERROR_FAIL_NOGUILD
    };

    _commandServer.QueueCommand<decltype(cancelResponse)>(
      clientId,
      [cancelResponse]()
      {
        return cancelResponse;
      });
    return;
  }

  // Build guild member list response
  protocol::AcCmdCRGuildMemberListOK response{};
  guildRecord.Immutable([this, &response](const data::Guild& guild)
  {
    for (const auto& member : guild.members())
    {
      const auto& characterRecord = GetServerInstance().GetDataDirector().GetCharacter(member);
      if (not characterRecord.IsAvailable())
      {
        server::util::QuietLogWarn("Character {} is not available but is guild {} member", 
          member, guild.uid());
        continue;
      }

      characterRecord.Immutable([&guild, &response](const data::Character& character)
      {
        protocol::AcCmdCRGuildMemberListOK::MemberInfo memberInfo{
          .memberUid = character.uid(),
          .nickname = character.name(),
          .unk0 = 1,
          .unk2 = 3
        };

        if (guild.owner() == character.uid())
        {
          memberInfo.guildRole = protocol::GuildRole::Owner;
        }
        else if (std::ranges::contains(guild.officers(), character.uid()))
        {
          memberInfo.guildRole = protocol::GuildRole::Officer;
        }
        else
        {
          memberInfo.guildRole = protocol::GuildRole::Member;
        }

        response.members.emplace_back(memberInfo);
      });
    }
  });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRequestGuildMatchInfo(
  ClientId clientId,
  const protocol::AcCmdCRRequestGuildMatchInfo& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto& guildRecord = GetServerInstance().GetDataDirector().GetGuild(command.guildUid);
  if (not guildRecord.IsAvailable())
  {
    server::util::QuietLogWarn("Character {} tried to request guild match info for guild {} that does not exist",
      clientContext.characterUid, command.guildUid);

    protocol::AcCmdCRRequestGuildMatchInfoCancel cancelResponse{};
    _commandServer.QueueCommand<decltype(cancelResponse)>(
      clientId,
      [cancelResponse]()
      {
        return cancelResponse;
      });
    return;
  }

  protocol::AcCmdCRRequestGuildMatchInfoOK response{
    .unk2 = 2,
    .unk3 = 3,
    .unk4 = 4,
    .unk5 = 5,
    .unk8 = 8,
    .unk10 = 10
  };

  guildRecord.Immutable([&response](const data::Guild& guild)
  {
    response.guildUid = guild.uid();
    response.name = guild.name(); 
    response.rank = guild.rank();
    response.totalWins = guild.totalWins();
    response.totalLosses = guild.totalLosses();
    response.seasonalWins = guild.seasonalWins();
    response.seasonalLosses = guild.seasonalLosses();
  });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleUpdateGuildMemberGrade(
  ClientId clientId,
  const protocol::AcCmdCRUpdateGuildMemberGrade& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto& characterRecord = GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid);
  
  // Get requesting character's guild
  auto guildUid = data::InvalidUid;
  characterRecord.Immutable([&guildUid](const data::Character& character)
  {
    guildUid = character.guildUid();
  });

  protocol::AcCmdCRUpdateGuildMemberGradeCancel response{};
  if (guildUid == data::InvalidUid)
  {
    response.unk0 = 2; // ERROR_FAIL_NOGUILD
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  const auto& guildRecord = GetServerInstance().GetDataDirector().GetGuild(guildUid);
  if (not guildRecord.IsAvailable())
  {
    response.unk0 = 0; // ERROR_FAIL_SYSTEMERROR
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  bool fail = true;
  uint8_t status = 0;
  guildRecord.Mutable([&command, callingCharacterUid = clientContext.characterUid, &fail, &status](data::Guild& guild)
  {
    // Check if calling character is owner
    if (guild.owner() != callingCharacterUid)
    {
      status = 7; // ERROR_FAIL_NOAUTHORITY
      server::util::QuietLogWarn("Character {}, who is not the owner of guild {}, tried to update member {} guild role to {}",
        callingCharacterUid, guild.uid(), command.characterUid, static_cast<uint8_t>(command.guildRole));
      return;
    }

    // Check if target character is in guild
    if (not std::ranges::contains(guild.members(), command.characterUid))
    {
      status = 1; // ERROR_FAIL_NOUSER
      server::util::QuietLogWarn("Character {} tried to update character {} guild role to {} but they are not in guild {}",
        callingCharacterUid, command.characterUid, static_cast<uint8_t>(command.guildRole), guild.uid());
      return;
    }

    // TODO: make this configurable
    constexpr uint8_t MaxOfficers = 2;

    // If promoting, check if there is enough space for officers to promote
    if (command.guildRole == protocol::GuildRole::Officer && guild.officers().size() >= MaxOfficers)
    {
      // TODO: Write in guild chat that max officer count has been reached
      server::util::QuietLogWarn("Character {} tried to update character {} guild role to officer but there are already max officers of {}",
        callingCharacterUid, command.characterUid, MaxOfficers);
      return;
    }

    // If promoting, check if target member is already an officer
    if (command.guildRole == protocol::GuildRole::Officer && std::ranges::contains(guild.officers(), command.characterUid))
    {
      // Tried to promote a guild member to officer but they are already an officer
      // TODO: Send a notify to the calling client of the target member's current guild role to update UI state 
      server::util::QuietLogWarn("Character {} tried to update character {} guild role to officer but they are already an officer",
        command.characterUid, static_cast<uint8_t>(command.guildRole));
      return;
    }

    // If currently owner, set new owner and ensure not present in officers list
    // If currently officer, get erased from the officers list
    // If currently member, get placed in officers list
    switch (command.guildRole)
    {
      case protocol::GuildRole::Owner:
      {
        // Transfer of ownership - swap roles (owner becomes member)
        // Since owner is already a member, just overwrite owner with new owner
        guild.owner() = command.characterUid;
        // Ensure previous owner is not somehow in officers list
        const auto& index = std::ranges::find(guild.officers(), guild.owner());
        if (index != guild.officers().end())
          guild.officers().erase(index);
        // Fall through to handle removal of officer role from the target user.
        [[fallthrough]];
      }
      case protocol::GuildRole::Member:
      {
        // Demotion - Find and erase officer from list of officers
        // Ensure an officer being transferred ownership is removed from officers list
        const auto& index = std::ranges::find(guild.officers(), command.characterUid);
        if (index != guild.officers().end())
          guild.officers().erase(index);
        break;
      }
      case protocol::GuildRole::Officer:
      {
        // Promotion - Previously checked if there is enough space for a new officer
        guild.officers().emplace_back(command.characterUid);
        break;
      }
    }

    fail = false;
  });

  if (fail)
  {
    response.unk0 = status;
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }
  
  // Broadcast to all online guild clients
  BroadcastUpdateGuildMemberGradeNotify(
    guildUid,
    command.characterUid,
    command.guildRole
  );

  // If ownership transfer
  if (command.guildRole == protocol::GuildRole::Owner)
  {
    // Broadcast ex-owner's new guild role as member
    BroadcastUpdateGuildMemberGradeNotify(
      guildUid,
      clientContext.characterUid,
      protocol::GuildRole::Member
    );
  }
}

void RanchDirector::HandleInviteToGuild(
  ClientId clientId,
  const protocol::AcCmdCRInviteGuildJoin& command)
{
  const auto& clientContext = GetClientContext(clientId);

  const auto inviterCharacterUid = clientContext.characterUid;
  auto inviterGuildUid = data::InvalidUid;
  _serverInstance.GetDataDirector().GetCharacter(inviterCharacterUid).Immutable(
    [&inviterGuildUid](const data::Character& character)
    {
      inviterGuildUid = character.guildUid();
    });

  auto inviteeCharacterUid = data::InvalidUid;
  auto inviteeGuildUid = data::InvalidUid;
  for (const auto& userInstance : _serverInstance.GetLobbyDirector().SnapshotUsers())
  {
    _serverInstance.GetDataDirector().GetCharacter(userInstance.characterUid).Immutable(
      [invitedCharacterName = command.characterName, &inviteeCharacterUid, &inviteeGuildUid](const data::Character& character)
      {
        if (character.name() != invitedCharacterName)
          return;
        inviteeCharacterUid = character.uid();
        inviteeGuildUid = character.guildUid();
      });

    if (inviteeCharacterUid != data::InvalidUid)
      break;
  }

  std::optional<protocol::GuildError> error;
  if (inviterGuildUid == data::InvalidUid)
  {
    // Inviter is not in a guild (should not be possible)
    error.emplace(protocol::GuildError::NoGuild);
    server::util::QuietLogWarn(
      "Character {} tried to invite {} to guild but inviter is not in a guild",
      clientContext.characterUid,
      command.characterName);
  }
  else if (inviteeCharacterUid == data::InvalidUid)
  {
    // Invitee is not found or offline
    error.emplace(protocol::GuildError::NoUserOrOffline);
  }
  else if (inviteeCharacterUid == inviterCharacterUid)
  {
    // Player is trying to invite themselves to the guild
    error.emplace(protocol::GuildError::CannotInviteSelf);
  }
  else if (inviteeGuildUid != data::InvalidUid)
  {
    // Character is already in the guild or is already in another guild
    error.emplace(protocol::GuildError::JoinedGuild);
  }

  if (error.has_value())
  {
    protocol::AcCmdCRInviteGuildJoinCancel response{.error = error.value()};
    _commandServer.QueueCommand<decltype(response)>(clientId, [response]()
    {
      return response;
    });
    return;
  }

  // Character is found, is not in (a) guild and is online
  GetServerInstance().GetLobbyDirector().InviteCharacterToGuild(
    inviteeCharacterUid,
    inviterGuildUid,
    clientContext.characterUid);
}

void RanchDirector::HandleGetEmblemList(
  ClientId clientId,
  const protocol::AcCmdCREmblemList&)
{
  const auto& clientContext = GetClientContext(clientId);
  
  auto guildUid = data::InvalidUid;
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&guildUid](const data::Character& character)
    {
      guildUid = character.guildUid();
    });

  if (guildUid == data::InvalidUid)
  {
    protocol::AcCmdCREmblemListCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  protocol::AcCmdCREmblemListOK response{};
  GetServerInstance().GetDataDirector().GetGuild(guildUid).Immutable(
    [](const data::Guild&)
    {
      // TODO: compile emblem list
    });
  
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
};

bool RanchDirector::BuildRanchCharacterInfo(
  const data::Uid characterUid,
  const data::Uid rancherUid,
  protocol::RanchCharacter& protocolCharacter)
{
  // LOA-fix (SYNC-3): собирает ростерную запись персонажа ровно так же, как
  // HandleEnterRanch собирает список персонажей ранча (та же последовательность
  // полей), но без бросков исключений — вызывается из рассылок, где падать
  // нельзя. Возвращает false, если персонажа нельзя показать (нет записи,
  // нет лошади, не в трекере этого ранча).
  auto& ranchInstance = _ranches[rancherUid];

  const auto characterOid = ranchInstance.tracker.GetCharacterOid(characterUid);
  if (characterOid == tracker::InvalidEntityOid)
    return false;

  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    characterUid);
  if (not characterRecord)
    return false;

  protocolCharacter.oid = characterOid;

  bool isComplete = true;
  characterRecord.Immutable([this, &protocolCharacter, &isComplete](
    const data::Character& character)
  {
    protocolCharacter.uid = character.uid();
    protocolCharacter.name = character.name();
    protocolCharacter.role = character.role() == data::Character::Role::GameMaster
      ? protocol::RanchCharacter::Role::GameMaster
      : character.role() == data::Character::Role::Op
        ? protocol::RanchCharacter::Role::Op
        : protocol::RanchCharacter::Role::User;
    protocolCharacter.introduction = character.introduction();

    protocol::BuildProtocolCharacter(protocolCharacter.character, character);

    // Character's equipment.
    const auto equipment = GetServerInstance().GetDataDirector().GetItemCache().Get(
      character.characterEquipment());
    if (equipment)
    {
      protocol::BuildProtocolItems(protocolCharacter.characterEquipment, *equipment);
    }

    // Character's settings (age/gender visibility).
    const auto settingsRecord = GetServerInstance().GetDataDirector().GetSettings(
      character.settingsUid());
    if (settingsRecord)
    {
      settingsRecord.Immutable(
        [&protocolCharacter, &character](const data::Settings& settings)
      {
        if (settings.hideAge())
          return;

        protocolCharacter.age = static_cast<uint8_t>(settings.age());
        protocolCharacter.gender = character.parts.modelId() == 10
          ? protocol::RanchCharacter::Gender::Boy
          : protocol::RanchCharacter::Gender::Girl;
      });
    }

    // Character's mount.
    const auto mountRecord = GetServerInstance().GetDataDirector().GetHorseCache().Get(
      character.mountUid());
    if (not mountRecord)
    {
      isComplete = false;
      return;
    }

    mountRecord->Immutable([&protocolCharacter](const data::Horse& horse)
    {
      protocol::BuildProtocolHorse(protocolCharacter.mount, horse);
      protocolCharacter.rent = {
        .mountUid = horse.uid(),
        .val1 = 0x12};
    });

    // Character's guild.
    if (character.guildUid() != data::InvalidUid)
    {
      const auto guildRecord = GetServerInstance().GetDataDirector().GetGuild(
        character.guildUid());
      if (guildRecord)
      {
        guildRecord.Immutable([&protocolCharacter](const data::Guild& guild)
        {
          protocol::BuildProtocolGuild(protocolCharacter.guild, guild);
        });
      }
    }

    // Character's pet.
    if (character.petUid() != data::InvalidUid)
    {
      const auto petRecord = GetServerInstance().GetDataDirector().GetPet(
        character.petUid());
      if (petRecord)
      {
        petRecord.Immutable([&protocolCharacter](const data::Pet& pet)
        {
          protocol::BuildProtocolPet(protocolCharacter.pet, pet);
        });
      }
    }
  });

  return isComplete;
}

void RanchDirector::BroadcastRanchCharacterRefresh(const ClientId clientId)
{
  // LOA-fix (SYNC-3): пересобираем ростерную запись игрока и переспавниваем её
  // у соседей готовыми опкодами Leave+Enter (своего notify «сменился ник» в
  // протоколе нет). Автору ничего не шлём — у него уже есть свой OK-ответ.
  const auto& clientContext = GetClientContext(clientId);
  const auto characterUid = clientContext.characterUid;
  const auto rancherUid = clientContext.visitingRancherUid;

  const auto ranchIter = _ranches.find(rancherUid);
  if (ranchIter == _ranches.cend())
    return;

  auto& ranchInstance = ranchIter->second;

  protocol::RanchCharacter protocolCharacter{};
  if (not BuildRanchCharacterInfo(characterUid, rancherUid, protocolCharacter))
  {
    server::util::QuietLogWarn(
      "Could not rebuild ranch roster entry for character {}, skipping refresh",
      characterUid);
    return;
  }

  protocolCharacter.isBusy = clientContext.busyState;

  const protocol::AcCmdCRLeaveRanchNotify leaveNotify{
    .characterId = characterUid};
  const protocol::RanchCommandEnterRanchNotify enterNotify{
    .character = protocolCharacter};

  // LOA-fix (SYNC-3, adversarial round): готовим повтор последнего снапшота.
  // Leave уничтожает сущность у соседа, Enter пересоздаёт её БЕЗ координат
  // (позиции в protocol::RanchCharacter нет — она ходит только снапшотами), а
  // стоящий на месте клиент новых снапшотов не шлёт. Без повтора соседи видели
  // бы переименованного игрока в точке спавна до его следующего шага.
  //
  // find(), а НЕ operator[]: последний вставил бы в кэш пустой notify для
  // персонажа, который с момента входа ни разу не двигался.
  //
  // Гард протухания — тот же, что в SYNC-9e: снапшот адресуется по OID сущности,
  // и если кэш относится к прошлому OID персонажа, повторять его нельзя. Если
  // записи нет вовсе — игрок с момента входа не двигался, соседи и так видят его
  // в стартовой точке, терять нечего.
  const auto snapshotIter = ranchInstance.snapshots.find(characterUid);
  const bool hasFreshSnapshot = snapshotIter != ranchInstance.snapshots.cend()
    && snapshotIter->second.ranchIndex
      == ranchInstance.tracker.GetCharacterOid(characterUid);

  for (const ClientId& ranchClientId : ranchInstance.clients)
  {
    // Prevent broadcast to self.
    if (ranchClientId == clientId)
      continue;

    _commandServer.QueueCommand<protocol::AcCmdCRLeaveRanchNotify>(
      ranchClientId,
      [leaveNotify]()
      {
        return leaveNotify;
      });

    _commandServer.QueueCommand<protocol::RanchCommandEnterRanchNotify>(
      ranchClientId,
      [enterNotify]()
      {
        return enterNotify;
      });

    // LOA-fix (SYNC-3, adversarial round): возвращаем позицию пересозданному
    // аватару. Канал тот же, что у обычного движения и у SYNC-9e — просто
    // повтор последнего кадра, протокол не меняется.
    if (hasFreshSnapshot)
    {
      const auto& snapshot = snapshotIter->second;
      _commandServer.QueueCommand<protocol::RanchCommandRanchSnapshotNotify>(
        ranchClientId,
        [snapshot]()
        {
          return snapshot;
        });
    }
  }
}

void RanchDirector::HandleChangeNickname(
  ClientId clientId,
  const protocol::AcCmdCRChangeNickname& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // Check if the new nickname is valid.
  const bool isNicknameValid = locale::IsNameValid(command.newNickname, 18);
  if (not isNicknameValid)
  {
    SendChangeNicknameCancel(
      clientId,
      protocol::ChangeNicknameError::InvalidNickname);
    return;
  }

  // Check if the new nickname is unique.
  const bool isUnique = _serverInstance.GetDataDirector().GetDataSource().IsCharacterNameUnique(
    command.newNickname);
  if (not isUnique)
  {
    SendChangeNicknameCancel(
      clientId,
      protocol::ChangeNicknameError::DuplicateNickname);
    return;
  }

  // todo: automod for the nickname

  bool itemConsumed = false;
  uint32_t remainingItemCount = 0;

  characterRecord.Mutable(
    [this, &itemConsumed, &remainingItemCount](data::Character& character)
    {
      const data::Tid CharacterRenameItem = 46002;

      // todo: To reconsider, the client sends us UID of the item that was used
      //       to rename the character. This would allow us to not remember `CharacterRenameItem` and
      //       to use the item UID to find the item.

      const auto consumeResult = GetServerInstance().GetItemSystem().ConsumeItem(
        character, CharacterRenameItem, 1);
      itemConsumed = consumeResult.itemConsumed;
      remainingItemCount = consumeResult.remainingItemCount;
    });

  if (not itemConsumed)
  {
    SendChangeNicknameCancel(
      clientId,
      protocol::ChangeNicknameError::NoOrIncorrectItem);
    return;
  }

  std::string previousName{};
  characterRecord.Mutable([newName = command.newNickname, &previousName](data::Character& character)
  {
    previousName = character.name();
    character.name() = newName;
  });

  const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;
  server::util::QuietLogInfo("User '{}' changed their character's name from '{}' to '{}'",
    userName,
    previousName,
    command.newNickname);

  protocol::AcCmdCRChangeNicknameOK response{
    .itemUid = command.itemUid,
    .remainingItemCount = static_cast<uint16_t>(remainingItemCount),
    .newNickname = command.newNickname};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  // LOA-fix (SYNC-3): у соседей по ранчо над аватаром висел старый ник до
  // перезахода. Переспавниваем ростерную запись игрока у всех остальных.
  BroadcastRanchCharacterRefresh(clientId);
}

void RanchDirector::SendChangeNicknameCancel(
  const ClientId clientId,
  const protocol::ChangeNicknameError reason)
{
  _commandServer.QueueCommand<protocol::AcCmdCRChangeNicknameCancel>(
    clientId,
    [reason]()
    {
      return protocol::AcCmdCRChangeNicknameCancel{
        .error =  reason};
    });
}

void RanchDirector::HandleChangeSkillCardPreset(
  ClientId clientId,
  const protocol::AcCmdCRChangeSkillCardPreset command)
{
  const auto& clientContext = GetClientContext(clientId);
  if (command.skillSet.setId > 2)
  {
    // TODO: character tried to update skill set exceeding range, return?
    server::util::QuietLogWarn("Character {} tried to update their skill set {} but character cannot have more than 2 skill sets",
      clientContext.characterUid, command.skillSet.setId);
    return;
  }
  // LOA-fix (R31-1, round31, backlog #126, SECURITY/REMOTE-CRASH): БЫЛО `> 2`,
  // стало `!= 2`. Верхняя граница проверялась, НИЖНЕЙ НЕ БЫЛО ВООБЩЕ, а ниже по
  // телу идут безусловные skills[0] и skills[1].
  // ★Размер вектора приходит С ПРОВОДА: SkillSet::Read читает uint8_t size и
  // делает skills.resize(size) (CommonStructureDefinitions.cpp) — то есть 0 и 1
  // полностью достижимы одним пакетом от аутентифицированного клиента.
  //   size == 0 → у вектора нулевой data() → skills[0] это разыменование
  //               НУЛЕВОГО указателя → SIGSEGV;
  //   size == 1 → skills[1] это чтение за концом буфера (OOB) → UB, в лучшем
  //               случае мусор уезжает в slot2 и СОХРАНЯЕТСЯ в персонажа.
  // ★ЭТО НЕ ЛОВИТСЯ. Диспетчер CommandServer оборачивает хендлер в
  // catch (const std::exception&), но нарушение памяти — не исключение: процесс
  // умирает целиком, без строки в логе. Один пакет = падение всего сервера.
  // ПОЧЕМУ `!= 2`, а не `< 2` вдобавок к `> 2`: легальный размер ровно один —
  // 2 (собственный SkillSet::Write утверждает assert(skills.size() == 2), и
  // клиент всегда шлёт два значения, пустые слоты передаются нулями). Одно
  // сравнение fail-closed закрывает оба конца диапазона и не даёт следующему
  // редактору снова забыть про нижнюю границу.
  else if (command.skillSet.skills.size() != 2)
  {
    server::util::QuietLogWarn("Character {} tried to save a skill set with {} skills, but a skill set holds exactly 2 skills",
      clientContext.characterUid, command.skillSet.skills.size());
    return;
  }

  const auto& characterRecord = GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid);
  characterRecord.Mutable(
    [&command](data::Character& character)
    {
      auto selectSkillSets = [&character](protocol::GameMode gamemode)
      { 
        switch (gamemode)
        {
          case protocol::GameMode::Magic:
            return &character.skills.magic();
          case protocol::GameMode::Speed:
            return &character.skills.speed();
          default:
            throw std::runtime_error("Gamemode is not recognised");
        }
      };

      const auto& skillSets = selectSkillSets(command.skillSet.gamemode);
      auto& skillSet = command.skillSet.setId == 0 ? skillSets->set1 : skillSets->set2;
      skillSet.slot1 = command.skillSet.skills[0];
      skillSet.slot2 = command.skillSet.skills[1];
    });
}

void RanchDirector::HandleUpdateDailyQuest(
  ClientId clientId,
  const protocol::AcCmdCRUpdateDailyQuest& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCRUpdateDailyQuestOK response{};

  // Get or create the daily quest group for this character.
  data::Uid groupUid = data::InvalidUid;
  characterRecord.Immutable(
    [&groupUid](const data::Character& character)
    {
      groupUid = character.dailyQuestGroupUid();
    });

  const auto groupRecord = groupUid != data::InvalidUid
    ? _serverInstance.GetDataDirector().GetDailyQuestGroup(groupUid)
    : _serverInstance.GetDataDirector().CreateDailyQuestGroup();

  // LOA-fix (R1, round2): прогресс, который в итоге лежит на сервере. Именно он
  // уезжает обратно в ответе, а не число из пакета — клиент должен видеть
  // серверную истину, иначе рассинхрон маскирует отказ записи.
  uint32_t serverProgress = 0;

  if (!groupRecord.IsAvailable())
  {
    server::util::QuietLogWarn(
      "HandleUpdateDailyQuest: daily quest group unavailable for character {}",
      clientContext.characterUid);
  }
  else groupRecord.Mutable(
    [this, &clientContext, &command, &characterRecord, &response, &serverProgress, groupUid](
      data::DailyQuestGroup& group)
    {
      // If this is a newly created group, link it back to the character.
      if (groupUid == data::InvalidUid)
      {
        const data::Uid newGroupUid = group.uid();
        characterRecord.Mutable(
          [newGroupUid](data::Character& character)
          {
            character.dailyQuestGroupUid() = newGroupUid;
          });
      }

      // LOA-fix (R1, round2): SERVER-AUTHORITATIVE прогресс дейликов.
      // Было: entry.progress = command.quest.progress — присланное клиентом число
      // как есть. Ни проверки, что questId вообще в трёх слотах, ни капа по
      // successValue, ни запрета УМЕНЬШАТЬ (обнулением прогресса обходился гард
      // реролла F4, а завышением — гейт награды дня F2).
      const auto& questRegistry = _serverInstance.GetQuestRegistry();
      auto quests = group.quests();
      bool slotFound = false;

      for (auto& entry : quests)
      {
        if (entry.questId != command.quest.questId)
          continue;

        slotFound = true;
        serverProgress = entry.progress;

        const auto questDef = questRegistry.GetQuest(entry.questId);
        if (not questDef.has_value())
          break;

        // Классы, у которых на сервере ЕСТЬ собственный счётчик: QuestSystem::
        // OnQuestEvent инкрементит слот сам (заезды 1002/1013 — RaceInstance,
        // кормление/мытьё — RanchDirector, рывок — RaceNetworkHandler, командная
        // победа и призовое место — RaceInstance). Клиентское число для них не
        // нужно вообще и является чистым каналом накрутки — игнорируем его.
        // LOA-fix (NEW-1, round3): клиентское число прогресса не пишется
        // БОЛЬШЕ НИКОГДА. Раунд 2 оставлял «подсказку» для классов, которых
        // сервер не считает, — и это был готовый бесплатный тир: слоты выбирает
        // сам клиент (fillGroup берёт questId из пакета), так что он набирал
        // 1005/1016 + 1018 и объявлял их выполненными, получая до 900 очков в
        // день, ни разу не сыграв.
        // Теперь PerfectJump и FireballAttack считает СЕРВЕР (хуки в
        // RaceNetworkHandler: HandleHurdleClearResult / HandleUseMagicItem), а
        // классы, которые сервер измерить не может (GlidingDistanceValue —
        // дистанция планирования не приезжает ни в одном пакете, CollectDropItem,
        // ClearMission), просто стоят на нуле. Чтобы такой слот не запирал день,
        // он исключён из гейта и из суммы очков награды
        // (см. HandleRequestDailyQuestReward + QuestSystem::IsServerTrackedFunction).
        if (not QuestSystem::IsServerTrackedFunction(questDef->function))
        {
          server::util::QuietLogDebug(
            "HandleUpdateDailyQuest: character {} reported progress {} for quest {} "
            "the server cannot verify; ignoring",
            clientContext.characterUid,
            command.quest.progress,
            entry.questId);
        }
        break;
      }

      if (not slotFound)
      {
        // Квест не входит в три слота группы: протухший пакет после суточного
        // сброса либо модифицированный клиент. Писать некуда — игнорируем.
        server::util::QuietLogDebug(
          "HandleUpdateDailyQuest: character {} reported progress for quest {} "
          "that is not in their daily slots; ignoring",
          clientContext.characterUid, command.quest.questId);
      }

      group.quests = quests;

      // LOA-fix (R3, round2): ежедневные 1000 морковок отсюда УБРАНЫ — они
      // выдаются при взятии целей дня (HandleRegisterDailyQuestGroup). Пакет
      // прогресса не должен быть валютным каналом: он приходит от клиента в любой
      // момент и с любым содержимым, включая questId, которого у игрока нет.
      // Баланс продолжаем возвращать, чтобы клиент не рассинхронился.
      characterRecord.Immutable(
        [&response](const data::Character& character)
        {
          response.newCarrotBalance = character.carrots();
        });
    });

  // LOA-fix (R1, round2): эхо серверного прогресса, а не клиентского.
  response.quest = {command.quest.questId, serverProgress, command.quest.rewardType, 1};
  response.unk_1 = 1;
  response.unk_2 = 1;

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRegisterDailyQuestGroup(
  ClientId clientId,
  const protocol::AcCmdCRRegisterDailyQuestGroup& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // LOA-fix (R63-3, round63, backlog #218): ФАКТ ПРИХОДА РЕГИСТРАЦИИ.
  //
  // ★ЗАЧЕМ ЛОГ РАНЬШЕ ФИКСА. Набор целей дня выбирает КЛИЕНТ: `fillGroup` берёт
  // `questId` прямо из этого пакета. Живой тест дал состояние «сброс сработал,
  // слоты пусты» (`lastResetDate` сегодняшний, три `questId = 0`), и по логам
  // отличить «клиент не прислал пакет» от «прислал, а мы его отвергли»
  // НЕВОЗМОЖНО: строки стоят только на ветках отказа, а на самом приходе и на
  // успехе — тишина. Это две РАЗНЫЕ задачи в разных каналах (клиент правится
  // только с разрешения владельца, сервер — нет), и выбирать между ними
  // догадкой мы уже пробовали: по одному клиентскому багу так умерло девять
  // гипотез подряд.
  //
  // ★Строка намеренно печатает РАЗМЕР пакета отдельно от идентификаторов:
  // пустой `dailyQuests` и три нулевых `questId` — разные симптомы с разными
  // причинами, и слитая формулировка «слоты пустые» их бы не различила.
  server::util::QuietLogDebug(
    "Daily goals: character {} registered a group, {} slot(s) in the packet: {} / {} / {}",
    clientContext.characterUid,
    command.dailyQuests.size(),
    command.dailyQuests.size() > 0 ? command.dailyQuests[0].questId : 0,
    command.dailyQuests.size() > 1 ? command.dailyQuests[1].questId : 0,
    command.dailyQuests.size() > 2 ? command.dailyQuests[2].questId : 0);

  data::Uid existingGroupUid = data::InvalidUid;
  characterRecord.Immutable(
    [&existingGroupUid](const data::Character& character)
    {
      existingGroupUid = character.dailyQuestGroupUid();
    });

  // LOA-fix (B3, round4): дедуп слотов дневных целей. Слоты выбирает клиент, а
  // очки награды дня суммируются по слотам — три одинаковых questId давали
  // тройной rewardPoint за одну-единственную выполненную цель (верхний тир за
  // треть работы). Пустой слот (questId == 0) дубликатом не считается.
  {
    bool duplicateSlots = false;
    for (size_t i = 0; i < 3 && i < command.dailyQuests.size() && not duplicateSlots; ++i)
    {
      if (command.dailyQuests[i].questId == 0)
        continue;
      for (size_t j = 0; j < i; ++j)
      {
        if (command.dailyQuests[j].questId == command.dailyQuests[i].questId)
        {
          duplicateSlots = true;
          break;
        }
      }
    }

    if (duplicateSlots)
    {
      server::util::QuietLogWarn(
        "HandleRegisterDailyQuestGroup: character {} sent duplicate daily goal "
        "slots ({}, {}, {}); refusing",
        clientContext.characterUid,
        command.dailyQuests.size() > 0 ? command.dailyQuests[0].questId : 0,
        command.dailyQuests.size() > 1 ? command.dailyQuests[1].questId : 0,
        command.dailyQuests.size() > 2 ? command.dailyQuests[2].questId : 0);

      // Отказ той же формы, что у гарда реролла (F4/R6).
      protocol::AcCmdCRRegisterDailyQuestGroupOK refuseResponse{};
      refuseResponse.status = 0;
      _commandServer.QueueCommand<decltype(refuseResponse)>(
        clientId,
        [refuseResponse]()
        {
          return refuseResponse;
        });
      return;
    }
  }

  const auto& questRegistry = _serverInstance.GetQuestRegistry();

  // LOA-fix (R3, round2): ежедневные 1000 морковок «от Дедушки Томаса» переехали
  // сюда из HandleUpdateDailyQuest (пакет прогресса 0x344). Там они выдавались за
  // ЛЮБОЙ первый пакет прогресса за день — независимо от questId и от того, брал
  // ли игрок вообще цели дня; валютный эффект висел на клиентском пакете без
  // единой проверки. Теперь они привязаны к реальному игровому действию «взять
  // цели дня у NPC» и по-прежнему гейтятся персистным carrotsClaimed — ровно один
  // раз в игровой день.
  bool dailyCarrotsAwarded = false;
  int32_t newCarrotBalance = 0;

  // Fills a group's fields from the command and calculates total possible rewardPoints.
  const auto fillGroup = [&command, &questRegistry, &characterRecord,
                          &dailyCarrotsAwarded, &newCarrotBalance](data::DailyQuestGroup& group)
  {
    if (!command.dailyQuests.empty())
    {
      group.rewardId   = command.dailyQuests[0].rewardId;
      group.rewardType = command.dailyQuests[0].rewardType;
    }

    std::array<data::DailyQuestEntry, 3> quests{};
    for (size_t i = 0; i < 3 && i < command.dailyQuests.size(); ++i)
    {
      quests[i].questId  = command.dailyQuests[i].questId;
      // LOA-fix (F4, quest-batch-1): прогресс НЕ берём из пакета клиента —
      // новый набор дневных целей всегда начинается с нуля. Иначе клиент мог
      // бы объявить цели сразу выполненными и забрать награду дня (гейт F2
      // сверяет именно progress).
      quests[i].progress = 0;
    }
    group.quests = quests;

    // Sum the rewardPoint of all 3 quest slots this determines the reward after completing the group of quests, 
    // not the individual quests themselves. Client sends this after completion too, safety net for us to ensure
    // that the client isnt cheating.
    uint32_t totalPoints = 0;
    for (const auto& entry : quests)
    {
      const auto questTemplate = questRegistry.GetQuest(entry.questId);
      if (questTemplate)
        totalPoints += questTemplate->rewardPoint;
    }
    group.rewardPoints = totalPoints;

    // LOA-fix (F3, quest-batch-1): штампуем день регистрации. Раньше новая группа
    // получала lastResetDate = 0, то есть «никогда не сбрасывалась», и ближайший
    // вход на своё ранчо обнулял её посреди дня (подтверждено живым клиентом
    // 2026-08-16). Схема дней — ровно та же, что в ResetDailyQuestsIfNeeded.
    const uint32_t today = util::CurrentGameDayIndex();
    if (group.lastResetDate() < today)
    {
      // Регистрация в НОВОМ игровом дне — это и есть точка суточного сброса для
      // игрока, который не заходит на своё ранчо: снимаем «морковки дня выданы»
      // и «награда дня забрана», иначе он потеряет их за этот день.
      group.carrotsClaimed = false;
      group.dailyRewardClaimed = false;
      // LOA-fix (R42, #8 F2): счётчик dailyClassExpGranted БОЛЬШЕ НЕ сбрасывается здесь —
      // владелец теперь SyncDailyClassExpBudget (QuestSystem, отдельная дата). См. R42-7.
    }
    group.lastResetDate = today;

    // LOA-fix (R3, round2): ежедневные 1000 морковок (см. комментарий выше).
    // Гейт — carrotsClaimed, который блок нового дня прямо над этим мог снять.
    if (not group.carrotsClaimed())
    {
      group.carrotsClaimed = true;
      characterRecord.Mutable(
        [&dailyCarrotsAwarded, &newCarrotBalance](data::Character& character)
        {
          character.carrots() += 1000;
          newCarrotBalance = character.carrots();
          dailyCarrotsAwarded = true;
        });
    }
  };

  if (existingGroupUid == data::InvalidUid)
  {
    const auto groupRecord = GetServerInstance().GetDataDirector().CreateDailyQuestGroup();
    groupRecord.Mutable(
      [&fillGroup, &characterRecord](data::DailyQuestGroup& group)
      {
        fillGroup(group);

        const data::Uid groupUid = group.uid();
        characterRecord.Mutable(
          [groupUid](data::Character& character)
          {
            character.dailyQuestGroupUid() = groupUid;
          });
      });
  }
  else
  {
    // If group exists, update all slots (there is no way to update individual slots)
    const auto groupRecord = _serverInstance.GetDataDirector().GetDailyQuestGroup(existingGroupUid);

    // LOA-fix (F4, quest-batch-1): гард повторной регистрации. Клиент оставляет
    // кнопку «Взять цель дня» активной и после выбора целей; upstream молча
    // перезаписывал все три слота → бесплатный реролл + потеря прогресса.
    if (not groupRecord.IsAvailable())
    {
      server::util::QuietLogWarn(
        "HandleRegisterDailyQuestGroup: daily quest group {} unavailable for character {}",
        existingGroupUid, clientContext.characterUid);
      return;
    }

    bool rerollAllowed = false;
    groupRecord.Immutable(
      [&rerollAllowed](const data::DailyQuestGroup& group)
      {
        // Новый игровой день — это штатный дневной реролл, он разрешён всегда.
        if (group.lastResetDate() < util::CurrentGameDayIndex())
        {
          rerollAllowed = true;
          return;
        }
        // Иначе — только если терять нечего: ни очка прогресса и награда дня
        // ещё не забрана.
        if (group.dailyRewardClaimed())
          return;
        for (const auto& entry : group.quests())
        {
          if (entry.progress != 0)
            return;
        }
        rerollAllowed = true;
      });

    if (not rerollAllowed)
    {
      server::util::QuietLogWarn(
        "HandleRegisterDailyQuestGroup: character {} tried to re-roll today's daily "
        "goals after making progress / claiming the reward; refusing",
        clientContext.characterUid);
      // LOA-fix (R6, round2): отвечаем OK со status = 0, а не Cancel (0x340).
      // Реакция клиента на 0x340 неизвестна: опкод в протоколе объявлен, но
      // структуры не существовало — значит его не слал никто и никогда, и мы
      // авторили её сами (F4). Отрицательный status у УЖЕ используемого OK-пакета
      // — заведомо более безопасный отказ. Структура Cancel оставлена в протоколе
      // как готовый резерв: если живой клиент ждёт именно её, откат — три строки.
      protocol::AcCmdCRRegisterDailyQuestGroupOK refuseResponse{};
      refuseResponse.status = 0;
      _commandServer.QueueCommand<decltype(refuseResponse)>(
        clientId,
        [refuseResponse]()
        {
          return refuseResponse;
        });
      return;
    }

    groupRecord.Mutable(
      [&fillGroup](data::DailyQuestGroup& group)
      {
        fillGroup(group);
      });
  }

  protocol::AcCmdCRRegisterDailyQuestGroupOK response{};
  response.status = 1;
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  // LOA-fix (R3, round2): если морковки дня начислены — освежаем баланс у клиента.
  // Пакет AcCmdCRRegisterDailyQuestGroupOK поля баланса не несёт (в нём только
  // status), поэтому используем ровно тот приём, который upstream уже применяет в
  // HandleRequestDailyQuestReward: досылаем AcCmdCRUpdateDailyQuestOK.
  if (dailyCarrotsAwarded)
  {
    protocol::AcCmdCRUpdateDailyQuestOK carrotsResponse{};
    carrotsResponse.newCarrotBalance = newCarrotBalance;

    protocol::DailyQuest questEcho{};
    if (not command.dailyQuests.empty())
    {
      questEcho.questId = command.dailyQuests[0].questId;
      questEcho.rewardType = command.dailyQuests[0].rewardType;
      questEcho.rewardId = command.dailyQuests[0].rewardId;
    }
    carrotsResponse.quest = questEcho;
    carrotsResponse.unk_1 = 1;
    carrotsResponse.unk_2 = 1;

    _commandServer.QueueCommand<decltype(carrotsResponse)>(
      clientId,
      [carrotsResponse]()
      {
        return carrotsResponse;
      });
  }
}
void RanchDirector::HandleConfirmItem(
  ClientId clientId,
  const protocol::AcCmdCRConfirmItem& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Get invoker's character name for logging
  std::string invokerCharacterName{};
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&invokerCharacterName](const data::Character& character)
    {
      invokerCharacterName = character.name();
    });

  // Get current shop list
  const auto& shopList = GetServerInstance().GetLobbyDirector().GetShopManager().GetShopList();

  // Get recipient character uid, if it even exists
  // TODO: this checks against the data source if character by that name exists but does not load character
  // into memory
  const data::Uid recipientCharacterUid = GetServerInstance()
    .GetDataDirector()
    .GetDataSource()
    .RetrieveCharacterUidByName(command.recipientCharacterName);

  // Check if character is gifting self or current shop list contains the goods
  bool error{false};
  if (command.recipientCharacterName == invokerCharacterName)
  {
    // Invoker cannot gift to themselves
    server::util::QuietLogWarn("Character '{}' ('{}') tried to confirm item (goods seq '{}') for themselves",
      clientContext.characterUid,
      invokerCharacterName,
      command.goodsSq);
    error = true;
  }
  else if (not shopList.goodsList.contains(command.goodsSq))
  {
    // Goods by that ID does not exist, return cancel
    server::util::QuietLogWarn("Character '{}' tried to confirm item (goods seq '{}') for another character but goods was not found.",
      clientContext.characterUid,
      command.goodsSq);
    error = true;
  }
  else if (recipientCharacterUid == data::InvalidUid)
  {
    // Character by that name does not exist
    // No need to log this
    error = true;
  }

  if (error)
  {
    // An error has occurred, return with cancel
    protocol::AcCmdCRConfirmItemCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Recipient character exists, goods is valid.
  const auto& goods = shopList.goodsList.at(command.goodsSq);

  // Check if recipient has the item
  bool hasItem{true};
  GetServerInstance().GetDataDirector().GetCharacter(recipientCharacterUid).Immutable(
    [this, &hasItem, itemTid = goods.itemUid](const data::Character& character)
    {
      if (not GetServerInstance().GetItemSystem().HasItem(character, itemTid))
        hasItem = false;
    });

  protocol::AcCmdCRConfirmItemOK response{
    .recipientCharacterName = command.recipientCharacterName,
    .goodsSq = command.goodsSq,
    .canPurchase = hasItem};
  _commandServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void RanchDirector::HandleConfirmSetItem(
  ClientId clientId,
  const protocol::AcCmdCRConfirmSetItem& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Get current shop list
  const auto& shopList = GetServerInstance().GetLobbyDirector().GetShopManager().GetShopList();

  // Check if current shop list contains the goods
  if (not shopList.goodsList.contains(command.goodsSq))
  {
    // Goods by that ID does not exist, return error
    protocol::AcCmdCRConfirmSetItemCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Get goods from the goods list
  const auto& goods = shopList.goodsList.at(command.goodsSq);
  // Get item TID from the goods
  const auto& requestedTid = goods.itemUid;

  // Validate shop item and ensure server has it in the item registry
  const auto& itemRegistryRecord = GetServerInstance().GetItemRegistry().GetItem(requestedTid);

  // Return cancel response if some server error happens
  if (itemRegistryRecord.has_value())
  {
    // Check if character owns the item
    bool hasItem = false;
    GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
      [this, &requestedTid, &hasItem](const data::Character& character)
      {
        // For now `shopItemUid` is the item TID (ref: GoodsSQ)
        hasItem = GetServerInstance().GetItemSystem().HasItem(
          character,
          requestedTid);
      });

    // Parse `hasItem` as result and return response
    protocol::AcCmdCRConfirmSetItemOK response{
      .goodsSq = command.goodsSq,
      .result = static_cast<protocol::AcCmdCRConfirmSetItemOK::Result>(hasItem)
    };

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
  }
  else
  {
    // Some server error happened here
    protocol::AcCmdCRConfirmSetItemCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
  }
}

void RanchDirector::HandleBuyOwnItem(
  ClientId clientId,
  const protocol::AcCmdCRBuyOwnItem& command)
{
  const auto& clientContext = GetClientContext(clientId);

  using OrderResult = protocol::AcCmdCRBuyOwnItemOK::OrderResult;
  using Purchase = protocol::AcCmdCRBuyOwnItemOK::Purchase;

  protocol::AcCmdCRBuyOwnItemOK response{};

  // Get current shop list
  const auto& shopList = GetServerInstance().GetLobbyDirector().GetShopManager().GetShopList();

  std::vector<data::Uid> newEquipmentUids{};
  std::vector<std::pair<data::Uid, protocol::Horse>> newHorseUids{};
  std::vector<std::pair<uint8_t, data::Uid>> expandMountSlotItems{};
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [this, &shopList, &command, &response, &newEquipmentUids, &newHorseUids, &expandMountSlotItems](data::Character& character)
    {
      for (const auto& order : command.orders)
      {
        // Create an order result entry in the response
        auto& orderResult = response.orderResults.emplace_back(
          OrderResult{
            .order = order});

        // Check if a goods by that `GoodsSQ` exists in the shop
        if (not shopList.goodsList.contains(order.goodsSq))
        {
          // Goods list does not contains this goods, return unknown error and process next order
          orderResult.result = OrderResult::Result::UnknownError;
          continue;
        }

        // Get the shop goods
        const auto& goods = shopList.goodsList.at(order.goodsSq);

        // Get the item cost from the selected price range
        std::optional<int32_t> costOpt{};
        uint32_t priceRange{0};

        // If goods info, get price from selected price range, else from set price
        if (goods.setType == 0)
        {
          // To determine the price of set of goods iterate over the items
          // and match the order price ID to the price ID of one of the items.
          for (const auto& price : goods.items)
          {
            if (price.priceId == order.priceId)
            {
              costOpt.emplace(price.goodsPrice);
              priceRange = price.priceRange;
              break;
            }
          }

          if (not costOpt.has_value())
          {
            // Goods item with that price range not found, continue onto the next order
            orderResult.result = OrderResult::Result::NotAvailable;
            continue;
          }
        }
        else if (goods.setType == 1)
        {
          // TODO: incomplete implementation
          costOpt.emplace(goods.setPrice);
          priceRange = 1;
        }
        else
        {
          // Set type is unknown, return unknown error and move onto the next order
          orderResult.result = OrderResult::Result::UnknownError;
          continue;
        }

        // Get the item from the registry by item TID
        // `itemUid` in the goods entry is actually the item TID
        const auto& itemRegistryRecord = GetServerInstance().GetItemRegistry().GetItem(goods.itemUid);

        const bool isCashItem = goods.moneyType == ShopList::Goods::MoneyType::Cash;
        const int32_t cost = costOpt.value();

        const bool hasSufficientCarrots = character.carrots() >= cost;
        const bool canPurchaseCarrotItem = not isCashItem and hasSufficientCarrots;
        const bool hasSufficientCash = character.cash() >= cost;
        const bool canPurchaseCashItem = isCashItem and hasSufficientCash;

        const bool hasItem = GetServerInstance().GetItemSystem().HasItem(
          character, 
          itemRegistryRecord.value().tid);

        if (not canPurchaseCarrotItem and not canPurchaseCashItem)
        {
          // Insufficient carrot or cash balance
          orderResult.result = OrderResult::Result::OutOfMoney;
          continue;
        }
        // TODO: implement other checks defined in `ShopItemResult::Result`

        // Deduct from character carrot/cash balance
        if (isCashItem)
          character.cash() -= cost;
        else
          character.carrots() -= cost;

        // Horse purchase — create a horse record and add it to the stable
        if (itemRegistryRecord.value().mountPartSetInfo.has_value())
        {
          const auto& partSetInfo = itemRegistryRecord.value().mountPartSetInfo.value();
          const auto& horseRecord = GetServerInstance().GetDataDirector().CreateHorse();
          if (not horseRecord)
          {
            // Refund and report error
            if (isCashItem)
              character.cash() += cost;
            else
              character.carrots() += cost;
            orderResult.result = OrderResult::Result::UnknownError;
            continue;
          }

          data::Uid horseUid{data::InvalidUid};
          const auto* mountAbility = itemRegistryRecord.value().mountAbility.has_value()
                                       ? &itemRegistryRecord.value().mountAbility.value()
                                       : nullptr;

          horseRecord.Mutable(
            [&horseUid, tid = itemRegistryRecord.value().tid, &partSetInfo, mountAbility](data::Horse& horse)
            {
              horse.tid() = tid;
              horse.dateOfBirth() = data::Clock::now();
              horse.mountCondition.stamina = 4000;
              horse.growthPoints() = 0;
              horse.clazz = 1;
              horse.tendency() = 1;
              horse.luckState = 4;
              if (mountAbility)
              {
                horse.grade() = mountAbility->grade;
                horse.stats.agility() = mountAbility->agility;
                horse.stats.ambition() = mountAbility->ambition;
                horse.stats.courage() = mountAbility->courage;
                horse.stats.endurance() = mountAbility->endurance;
                horse.stats.rush() = mountAbility->rush;
              }
              horse.parts.skinTid() = partSetInfo.skinId;
              horse.parts.faceTid() = partSetInfo.faceId;
              horse.parts.maneTid() = partSetInfo.maneId;
              horse.parts.tailTid() = partSetInfo.tailId;
              horse.appearance.scale() = partSetInfo.scale;
              horse.appearance.legLength() = partSetInfo.legLength;
              horse.appearance.legVolume() = partSetInfo.legVolume;
              horse.appearance.bodyLength() = partSetInfo.bodyLength;
              horse.appearance.bodyVolume() = partSetInfo.bodyVolume;
              horse.emblemUid() = partSetInfo.emblemId;
              horseUid = horse.uid();
            });

          character.horses().emplace_back(horseUid);

          // Add to the buy response so the client registers the horse purchase
          // (triggers horse naming dialog and stable update)
          auto& purchase = response.purchases.emplace_back(
            Purchase{.equipImmediately = false});
          purchase.item.uid = static_cast<uint32_t>(horseUid);
          purchase.item.tid = static_cast<uint32_t>(itemRegistryRecord.value().tid);
          purchase.item.count = 1;

          protocol::Horse protocolHorse{};
          horseRecord.Immutable([&protocolHorse](const data::Horse& horse)
            {
              protocol::BuildProtocolHorse(protocolHorse, horse);
            });
          newHorseUids.emplace_back(horseUid, protocolHorse);
          continue;
        }

        // Add item directly to character's inventory
        data::Uid itemUid{data::InvalidUid};
        if (itemRegistryRecord.value().type == registry::Item::Type::Temporary)
        {
          itemUid = GetServerInstance().GetItemSystem().AddItem(
            character,
            itemRegistryRecord.value().tid,
            std::chrono::hours(priceRange));
        }
        else
        {
          itemUid = GetServerInstance().GetItemSystem().AddItem(
            character,
            itemRegistryRecord.value().tid,
            priceRange);
        }

        // LOA-fix (R8-3, round8): AddItem легально возвращает data::InvalidUid
        // (раунд 3 / A6), а GetItem(InvalidUid).Immutable БРОСАЕТ
        // std::runtime_error. Бросок летел изнутри общего Character.Mutable и
        // уносил ВЕСЬ обработчик покупки: клиент не получал ни OK, ни Cancel,
        // остальные заказы пачки терялись, а списание в этой же лямбде было уже
        // сделано. ТЕПЕРЬ: возвращаем деньги за ЭТОТ заказ и переходим к
        // следующему — ровно тем приёмом, что апстрим применяет выше при
        // неудаче CreateHorse.
        if (itemUid == data::InvalidUid)
        {
          if (isCashItem)
            character.cash() += cost;
          else
            character.carrots() += cost;

          server::util::QuietLogError(
            "BuyItem: failed to grant item '{}' to character '{}'; the order is "
            "refunded",
            itemRegistryRecord.value().tid,
            character.name());

          orderResult.result = OrderResult::Result::UnknownError;
          continue;
        }

        // Append the purchase result into the response
        GetServerInstance().GetDataDirector().GetItem(itemUid).Immutable(
          [&order, &response](const data::Item& item)
          {
            auto& purchase = response.purchases.emplace_back(
              Purchase{
                .equipImmediately = order.equipImmediately});
            protocol::BuildProtocolItem(purchase.item, item);
          });

        if (itemRegistryRecord.value().prerequisiteLevel.has_value())
        {
          // This item is a horse slot expansion item, store it to send to the client
          // and instantly unlock the slots (bypasses AcCmdCRGetItemFromStorageOK handler logic)
          expandMountSlotItems.emplace_back(
            itemRegistryRecord.value().prerequisiteLevel.value(),
            itemUid);
        }

        // Queue for equipping only if the player requested it and doesn't own it yet
        if (order.equipImmediately && not hasItem)
          newEquipmentUids.emplace_back(itemUid);
      }

      // Update character's balance
      response.newCarrots = character.carrots();
      response.newCash = character.cash();
    });

  // All checks are completed and transaction can go ahead
  _commandServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });

  // Sort horse slot expansion items by prerequisite level to,
  // send it in the correct oder
  std::sort(
    expandMountSlotItems.begin(),
    expandMountSlotItems.end(),
    [](const auto& a, const auto& b)
    {
      return a.first < b.first;
    });

  // Handle horse slot expansion items
  for (const data::Uid itemUid : expandMountSlotItems | std::views::values)
  {
    HandleExpandMountSlot(clientId, protocol::AcCmdCRExpandMountSlot{
      .itemUid = itemUid});
  }

  // Register purchased horses with the ranch tracker and notify the client
  for (auto& [horseUid, protocolHorse] : newHorseUids)
  {
    AddRanchHorse(clientContext.characterUid, horseUid);

    protocol::AcCmdRCAddIdleMountInfoNotify notify{};
    notify.horse.horseOid = _ranches[clientContext.characterUid].tracker.GetHorseOid(horseUid);
    notify.horse.horse = std::move(protocolHorse);

    _commandServer.QueueCommand<protocol::AcCmdRCAddIdleMountInfoNotify>(
      clientId, [notify]()
      {
        return notify;
      });
  }

  // Process all the equipment marked for equipping
  for (const auto& equipmentUid : newEquipmentUids)
  {
    HandleWearEquipment(
      clientId,
      protocol::AcCmdCRWearEquipment{
        .equipmentUid = equipmentUid
      });
  }
}

void RanchDirector::HandleSendGift(
  ClientId clientId,
  const protocol::AcCmdCRSendGift& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::string invokerCharacterName{};
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&invokerCharacterName](const data::Character& character)
    {
      invokerCharacterName = character.name();
    });

  // Get current shop list
  const auto& shopList = GetServerInstance().GetLobbyDirector().GetShopManager().GetShopList();

  // Get recipient character uid, if it even exists
  // TODO: this checks against the data source if character by that name exists but does not load character
  //       into the memory
  const data::Uid recipientCharacterUid = GetServerInstance()
    .GetDataDirector()
    .GetDataSource()
    .RetrieveCharacterUidByName(command.recipientCharacterName);

  bool error{false};
  // Check if gifting self or current shop list contains the goods
  if (command.recipientCharacterName == invokerCharacterName)
  {
    // Invoker cannot gift themself
    server::util::QuietLogWarn("Character '{}' ('{}') tried to send gift (goods seq '{}') to themself.",
      clientContext.characterUid,
      invokerCharacterName,
      command.order.goodsSq);
    error = true;
  }
  else if (not shopList.goodsList.contains(command.order.goodsSq))
  {
    // Goods by that ID does not exist, return cancel
    server::util::QuietLogWarn("Character '{}' tried to send gift (goods seq '{}') to another character but goods was not found.",
      clientContext.characterUid,
      command.order.goodsSq);
    error = true;
  }
  else if (recipientCharacterUid == data::InvalidUid)
  {
    // Character by that name does not exist
    // No need to log this
    error = true;
  }

  protocol::AcCmdCRSendGiftCancel cancel{};
  if (error)
  {
    // An error has occurred, return with cancel
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Recipient character exists, goods is valid.
  const auto& goods = shopList.goodsList.at(command.order.goodsSq);

  // Get item information
  const auto& itemRegistryRecord = GetServerInstance().GetItemRegistry().GetItem(goods.itemUid);
  if (not itemRegistryRecord.has_value())
  {
    // Item does not exist in registry
    server::util::QuietLogWarn("Character '{}' tried to gift shop item (goods sq '{}') with invalid item tid '{}'.",
      clientContext.characterUid,
      command.order.goodsSq,
      goods.itemUid);
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // LOA-fix (R43-2, #159): дарить можно ТОЛЬКО то, что витрина помечает даримым.
  // Хендлер проверял «не себе / товар есть / получатель есть / tid в реестре», но
  // не смотрел на giftType, хотя магазин его считает и отдаёт клиенту (Shop.cpp:
  // CanGift выставляется лишь товарам с characterPartInfo, т.е. носимой косметике).
  // Честный клиент кнопку «подарить» на прочих товарах не покажет, но крафтом
  // AcCmdCRSendGift дарился ЛЮБОЙ товар витрины, включая «лошадиные» позиции
  // (mountPartSetInfo): подарочный путь выдал бы их плоским предметом вместо
  // лошади. Гейт fail-closed: не CanGift -> Cancel, как остальные проверки выше.
  if (goods.giftType != ShopList::Goods::GiftType::CanGift)
  {
    server::util::QuietLogWarn(
      "Character '{}' tried to gift non-giftable goods (goods sq '{}', item tid '{}').",
      clientContext.characterUid,
      command.order.goodsSq,
      goods.itemUid);
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Check if recipient has the item
  bool hasItem{true};
  GetServerInstance().GetDataDirector().GetCharacter(recipientCharacterUid).Immutable(
    [this, &hasItem, itemTid = goods.itemUid](const data::Character& character)
    {
      hasItem = GetServerInstance().GetItemSystem().HasItem(character, itemTid);
    });

  if (hasItem)
  {
    // TODO: prepare for the possibility that invoker is gifting an item that can stack
    // Like items with duration or consumables
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Recipient character exists, goods is valid and recipient does not have the item,
  // process the transaction.
  protocol::AcCmdCRSendGiftOK response{
    .giftOrderResult = protocol::AcCmdCRSendGiftOK::GiftOrderResult{
      .order = command.order,
      // LOA-fix (R43-3, #162): РАЗМОРОЗКА ПОДАРКОВ — они не работали у ВСЕХ.
      // Апстримная структура объявлена с дефолтом `bool error{true}` («если
      // хендлер забыл выставить результат — клиент увидит ошибку»). Этот
      // инициализатор поле не называл, поэтому флаг оставался true, а наш гейт
      // gift-affordability ниже (`not cost.has_value() ||
      // response.giftOrderResult.error`) читает его ДО того, как успешная ветка
      // в конце обработчика сбрасывает его в false. Итог: с деплоя
      // gift-affordability (~2026-08-11) КАЖДЫЙ подарок отбивался Cancel — ни
      // предмета, ни посылки, ни списания, ни строки в логе. Подтверждено
      // стендом A/B на прод-образе (обе арки), см. BACKLOG #162.
      // ТЕПЕРЬ: флаг стартует false и снова означает ровно «заказ малформный»
      // (его выставляют ветки bad priceId и unknown setType ниже), гейт
      // продолжает ловить именно их, анти-краш-часть (`not cost.has_value()`)
      // не тронута.
      // ★ЕДЕТ ВМЕСТЕ С R43-1: разморозка без parcel-only включила бы дупликацию
      // подарка (#159), которая сегодня недостижима только потому, что путь мёртв.
      .error = false
    }};

  // If set type is goods info, get price from selected price range, else from set price
  std::optional<uint32_t> cost{};
  uint32_t priceRange{0};
  if (goods.setType == 0)
  {
    // Loop through each price range
    for (const auto& price : goods.items)
    {
      // Check if price ID for the goods matches that of the one selected by the character
      if (price.priceId == command.order.priceId)
      {
        // Price found by price ID, store cost and price range
        cost.emplace(price.goodsPrice);
        priceRange = price.priceRange;
        break;
      }
    }

    if (not cost.has_value())
    {
      // Goods with that price range not found
      server::util::QuietLogWarn("Character '{}' tried to gift shop item (goods sq '{}') with invalid price id '{}'.",
        clientContext.characterUid,
        command.order.goodsSq,
        command.order.priceId);
      response.giftOrderResult.error = true;
    }
  }
  else if (goods.setType == 1)
  {
    // TODO: incomplete implementation
    cost.emplace(goods.setPrice);
    priceRange = 1;
  }
  else
  {
    // Set type is unknown, return unknown error and move onto the next order
    response.giftOrderResult.error = true;
  }

  // LOA-fix (gift-affordability): reject malformed orders BEFORE the deduct gate.
  // A crafted AcCmdCRSendGift (valid goodsSq but bad priceId under setType==0, or an
  // unknown setType) leaves `cost` empty with giftOrderResult.error set; the original
  // code fell through to cost.value() -> std::bad_optional_access (a crafted-packet
  // crash in the very handler we are hardening). Mirror the buy path's per-order skip:
  // cancel and return. Today error <=> empty cost, but guard on BOTH to stay robust.
  if (not cost.has_value() || response.giftOrderResult.error)
  {
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // LOA-fix (gift-affordability): gate the deduction on affordability, mirroring
  // HandleBuyOwnItem's OutOfMoney path. carrots/cash are int32_t, so an unchecked
  // deduct drives the sender NEGATIVE via a crafted/replayed AcCmdCRSendGift. Fold the
  // affordability CHECK + DEDUCT into ONE invoker Mutable; on shortfall, cancel and
  // return BEFORE any recipient grant (the refund below covers a later grant failure).
  bool canAffordGift{false};
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&response, &canAffordGift, moneyType = goods.moneyType, &cost](data::Character& character)
    {
      const int32_t giftCost = static_cast<int32_t>(cost.value());
      const bool isCashItem = moneyType == ShopList::Goods::MoneyType::Cash;
      const bool hasSufficientCarrots = character.carrots() >= giftCost;
      const bool hasSufficientCash = character.cash() >= giftCost;
      canAffordGift = isCashItem ? hasSufficientCash : hasSufficientCarrots;
      if (not canAffordGift)
        return;

      // Deduct from balance depending on goods money type
      if (moneyType == ShopList::Goods::MoneyType::Cash)
        character.cash() -= cost.value();
      else
        character.carrots() -= cost.value();

      // Set balance values in response
      response.carrots = character.carrots();
      response.cash = character.cash();
    });

  if (not canAffordGift)
  {
    // Invoker cannot afford the gift; cancel before any grant (mirrors OutOfMoney).
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // LOA-fix (gift-affordability): track whether the recipient grant produced a gift
  // record, so a failure after the deduction can refund the invoker (mirrors the
  // buy-path refund-on-CreateHorse-failure).
  bool giftGranted{false};
  // Add item to system
  GetServerInstance().GetDataDirector().GetCharacter(recipientCharacterUid).Mutable(
    [this, &goods, &priceRange, &command, &invokerCharacterName, &giftGranted, registryItem = itemRegistryRecord.value()]
      (data::Character& character)
    {
      // LOA-fix (R8-3, round8): ЗДЕСЬ СЪЕДАЛИСЬ ДЕНЬГИ ОТПРАВИТЕЛЯ.
      // AddItem с раунда 3 (A6) легально возвращает data::InvalidUid, а
      // CreateStorageItem() легально возвращает ПУСТУЮ запись (DataDirector.cpp
      // ловит исключение и отдаёт {}). И GetItem(InvalidUid).Immutable, и
      // .Mutable() на пустой записи БРОСАЮТ std::runtime_error (Record.hpp).
      // Бросок летел изнутри CreateStorageItem().Mutable внутри
      // Character.Mutable — то есть мимо рефанда, который стоит НИЖЕ по
      // обработчику (ветка `not giftGranted`). А списание к этому моменту уже
      // персистнуто отдельным, ранее закрытым Mutable отправителя: игрок терял
      // морковки/кэш и не получал ни подарка, ни ответа.
      // ТЕПЕРЬ: неудача создания записи — обычный выход из лямбды. giftGranted
      // остаётся false, и штатная ветка рефанда возвращает деньги и шлёт Cancel
      // (тот же контракт, что даёт гейт gift-affordability). ★С R43 (#159) это
      // ЕДИНСТВЕННАЯ неудача выдачи в этом хендлере: вторая, про AddItem,
      // исчезла вместе с самим вызовом AddItem.
      // ★УСТАРЕЛО С R43 (#159): рассуждение выше про ПОРЯДОК «storage-запись до
      // AddItem» и про осиротевшую запись описывало схему с ДВУМЯ выдачами —
      // её больше нет, AddItem в этом хендлере не вызывается вовсе. Оставлена
      // только история про съеденные деньги (она по-прежнему верна: неудача
      // создания записи = обычный выход, giftGranted остаётся false, рефанд
      // ниже возвращает списанное). ПОЧЕМУ ПРАВИМ КОММЕНТАРИЙ, А НЕ ОСТАВЛЯЕМ:
      // именно устаревший комментарий R8-3 («и предмет в инвентаре, И запись в
      // подарочном хранилище» как признак успеха) закодировал ошибочный
      // инвариант и помог багу пережить четыре раунда правок этого хендлера.
      const auto storageItemRecord =
        GetServerInstance().GetDataDirector().CreateStorageItem();
      if (not storageItemRecord)
      {
        server::util::QuietLogError(
          "SendGift: failed to create a storage record for the gift to '{}'; "
          "the gift is cancelled and the sender is refunded",
          character.name());
        return;
      }

      // LOA-fix (R43-1, #159): ПОДАРОК ДОСТАВЛЯЕТСЯ ТОЛЬКО ЧЕРЕЗ ЯЩИК.
      // БЫЛО (живой эксплойт экономики): AddItem клал предмет СРАЗУ В ИНВЕНТАРЬ
      // получателя — это побочный эффект ItemSystem::AddItem
      // (character.inventory().emplace_back), — а рядом создавалась посылка со
      // снимком того же предмета. При заборе посылки HandleGetItemFromStorage
      // звал AddItem ВТОРОЙ раз; предмет у получателя уже лежал, поэтому
      // срабатывала ветка СЛИЯНИЯ: count += count либо накопление duration
      // (ItemSystem.cpp). Один оплаченный подарок = двойное количество или
      // двойной срок у получателя. Гейт «у получателя уже есть этот предмет»
      // выше гарантировал, что первая выдача ВСЕГДА создаёт новый предмет, то
      // есть слияние срабатывало всегда — эксплойт достижим честным клиентом с
      // двух аккаунтов, без модификаций. Четыре прошлых правки этого хендлера
      // (GIFTFIX / R8-3 / R9-1) переставляли порядок и добавляли рефанд, но обе
      // выдачи сохраняли.
      // ТЕПЕРЬ: AddItem здесь не зовём вовсе — ровно как GM-путь //give
      // (ChatSystem создаёт StorageItem с tid напрямую). Снимок для посылки
      // собираем из реестра и выбранного priceRange; значения те же, что писал
      // AddItem: Temporary -> count 1 + duration hours(priceRange), иначе
      // count priceRange + duration 0. Предмет появляется у получателя РОВНО
      // ОДИН раз — на заборе посылки.
      const bool isTemporaryGift =
        registryItem.type == registry::Item::Type::Temporary;

      // Create storage item and populate with gift details
      data::Uid storageItemUid{data::InvalidUid};
      storageItemRecord.Mutable(
        [&storageItemUid, &command, &goods, &registryItem, &invokerCharacterName,
         isTemporaryGift, priceRangeValue = priceRange](data::StorageItem& storageItem)
        {
          storageItemUid = storageItem.uid();
          storageItem.goodsSq() = command.order.goodsSq;
          storageItem.priceId() = command.order.priceId;
          storageItem.carrots() = goods.bonusGameMoney;
          storageItem.duration() = std::chrono::days(7); // TODO: configurable?
          storageItem.createdAt() = util::Clock::now();
          storageItem.sender() = invokerCharacterName;
          storageItem.message() = command.message;

          storageItem.items() = {
            data::StorageItem::Item{
              .tid = registryItem.tid,
              .count = isTemporaryGift ? 1u : priceRangeValue,
              .duration = isTemporaryGift
                ? std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::hours(priceRangeValue))
                : std::chrono::seconds::zero()}};
        });

      if (storageItemUid == data::InvalidUid)
      {
        // Сюда попадаем, только если у СОЗДАННОЙ записи невалидный uid: сам
        // storageItemUid снят с неё же в Mutable выше. Значит удалить сироту
        // НЕЧЕМ — Delete адресуется по uid, а валидного uid здесь по
        // определению нет; перечитывать ту же запись бессмысленно (вернёт тот
        // же InvalidUid). ★Именно на этом поймал Codex-T3 (R43 iter1): унасле-
        // дованная от R9-1 пара «перечитать uid -> удалить» стала логически
        // недостижимой, потому что её прежний триггер (неудача AddItem, не
        // связанная с uid посылки) этим раундом убран вместе с AddItem.
        // Практически ветка недостижима и сама: FileDataSource раздаёт uid'ы
        // с единицы (++_storageItemSequentialUid), а InvalidUid == 0.
        // ★ЕДИНСТВЕННЫЙ теоретический путь сюда — переполнение счётчика uid
        // (uint32 wrap на UINT32_MAX даёт ключ 0). В этом случае запись с
        // ключом 0 останется в кэше неотнесённой ни к кому; гоняться за ней не
        // будем: Delete по ключу 0 в норме адресовал бы чужое/несуществующее,
        // а сам сценарий требует ~4 млрд посылок за жизнь мира. Экономического
        // эффекта нет — деньги отправителю возвращаются в любом случае.
        // Поведение fail-closed: подарок не состоялся, giftGranted остаётся
        // false, деньги вернёт штатная ветка рефанда ниже.
        server::util::QuietLogError(
          "SendGift: the gift parcel for '{}' has an invalid uid; the gift is "
          "cancelled and the sender is refunded",
          character.name());
        return;
      }

      // Add storage item to recipient's gift storage
      character.gifts().emplace_back(storageItemUid);

      // Подарок состоялся, когда посылка создана и положена в ящик получателя.
      // Предмета в инвентаре на этом шаге больше НЕТ (см. R43-1), поэтому и
      // трекинг рефанда теперь завязан на storageItemUid, а не на itemUid.
      giftGranted = storageItemUid != data::InvalidUid;
    });

  if (not giftGranted)
  {
    // Recipient grant failed AFTER the deduction — refund the invoker so money is
    // never lost silently. Follow-up Mutable on the invoker record: SEQUENTIAL after
    // the recipient Mutable, never nested (non-recursive record lock).
    GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
      [moneyType = goods.moneyType, &cost](data::Character& character)
      {
        if (moneyType == ShopList::Goods::MoneyType::Cash)
          character.cash() += cost.value();
        else
          character.carrots() += cost.value();
      });
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Gifting is successful, indicate and return response
  response.giftOrderResult.error = false;
  _commandServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });

  // Notify recipient of new item in gift box (if they are online)
  GetServerInstance().GetRanchDirector().SendStorageNotification(
    recipientCharacterUid,
    protocol::AcCmdCRRequestStorage::Category::Gifts);
}

void RanchDirector::HandlePasswordAuth(
  ClientId clientId,
  const protocol::AcCmdCRPasswordAuth)
{
  protocol::AcCmdCRPasswordAuthOK response {
    .action = protocol::AcCmdCRPasswordAuthOK::Action::Authenticated
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRequestDailyQuestReward(
  ClientId clientId,
  const protocol::AcCmdCRRequestDailyQuestReward& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCRRequestDailyQuestRewardOK response{};

  // Get character's daily quest group
  data::Uid groupUid = data::InvalidUid;
  characterRecord.Immutable([&groupUid](const data::Character& character)
  {
    groupUid = character.dailyQuestGroupUid();
  });

  if (groupUid == data::InvalidUid)
  {
    return;
  }

  const auto groupRecord = _serverInstance.GetDataDirector().GetDailyQuestGroup(groupUid);
  if (!groupRecord.IsAvailable())
  {
    return;
  }

  // Check if the command rewardPoints match the accumulated points in the group
  bool pointsMatch = false;
  groupRecord.Immutable([&pointsMatch, commandPoints = command.rewardPoints](const data::DailyQuestGroup& group)
  {
    pointsMatch = (group.rewardPoints() >= commandPoints);
  });

  if (!pointsMatch)
  {
    server::util::QuietLogWarn("HandleRequestDailyQuestReward: Character {} reward points do not match", clientContext.characterUid);
    return;
  }

  // LOA-fix (F2, quest-batch-1): АНТИ-ЭКСПЛОЙТ выдачи награды дня.
  // Было: единственная проверка — статическая сумма очков group.rewardPoints()
  // (считается при регистрации группы по трём выбранным квестам, RanchDirector
  // fillGroup) против числа из пакета. Фактический прогресс не смотрелся вообще,
  // признака «награда дня уже выдана» не существовало → модифицированный клиент
  // забирал предметы QuestRewardPoint при нулевом прогрессе и повторно, сколько
  // угодно раз. Стало: (1) повторная выдача за день отклоняется по персистному
  // флагу dailyRewardClaimed; (2) требуется фактическое выполнение ВСЕХ занятых
  // слотов (progress >= successValue из QuestRegistry по questId слота).
  // ЗАМЕЧАНИЕ по границам: сам progress пишется клиентом (AcCmdCRUpdateDailyQuest
  // 0x344 принимает присланное число как есть) — server-authoritative прогресс
  // дейликов это отдельная задача (см. CHANGES.md → «Отложено»). Здесь закрыты
  // ровно две дыры: клейм при нулевом прогрессе и повторный клейм.
  bool rewardAlreadyClaimed = false;
  bool objectivesMet = true;
  uint32_t occupiedSlots = 0;
  // LOA-fix (NEW-1, round3): очки, которые игрок ЗАРАБОТАЛ на слотах,
  // подтверждаемых сервером, и число таких слотов.
  uint32_t trackedSlots = 0;
  uint32_t earnedPoints = 0;
  groupRecord.Immutable(
    [this, &rewardAlreadyClaimed, &objectivesMet, &occupiedSlots, &trackedSlots,
      &earnedPoints](
      const data::DailyQuestGroup& group)
    {
      rewardAlreadyClaimed = group.dailyRewardClaimed();

      const auto& dailyRegistry = _serverInstance.GetQuestRegistry();
      for (const auto& entry : group.quests())
      {
        // Пустой слот (questId 0) в счёт не идёт: группа с <3 целями легальна,
        // блокировать по ней выдачу нельзя.
        if (entry.questId == 0)
          continue;
        ++occupiedSlots;

        const auto questTemplate = dailyRegistry.GetQuest(entry.questId);
        if (not questTemplate.has_value())
        {
          // Цель, которой нет в реестре, зачесть нельзя.
          objectivesMet = false;
          continue;
        }

        // LOA-fix (NEW-1, round3): слот класса, который сервер не измеряет
        // (гляйдинг и прочие), не участвует ни в гейте, ни в очках. Требовать
        // его выполнения нельзя — прогресс там навсегда 0 и день был бы заперт;
        // платить за него тоже нельзя — подтвердить его нечем.
        if (not QuestSystem::IsServerTrackedFunction(questTemplate->function))
          continue;

        ++trackedSlots;
        if (entry.progress < questTemplate->successValue)
        {
          objectivesMet = false;
          continue;
        }
        earnedPoints += questTemplate->rewardPoint;
      }
    });

  if (rewardAlreadyClaimed)
  {
    server::util::QuietLogWarn(
      "HandleRequestDailyQuestReward: Character {} already claimed today's daily "
      "reward; refusing",
      clientContext.characterUid);
    return;
  }

  // LOA-fix (B4, round4): требование «выполнены ВСЕ подтверждаемые слоты»
  // снято — оно обнуляло награду честному игроку, закрывшему 2 цели из 3.
  // Отказываем только когда подтверждать нечего вовсе.
  if (occupiedSlots == 0 || trackedSlots == 0 || earnedPoints == 0)
  {
    server::util::QuietLogWarn(
      "HandleRequestDailyQuestReward: Character {} requested the daily reward "
      "without a single completed server-tracked goal ({} occupied slots, {} of "
      "them server-tracked, all met: {}); refusing",
      clientContext.characterUid, occupiedSlots, trackedSlots, objectivesMet);
    return;
  }

  if (not objectivesMet)
  {
    // Частичный клейм — законен, но тир будет ниже: платим по фактически
    // набранным очкам.
    server::util::QuietLogInfo(
      "HandleRequestDailyQuestReward: Character {} claims a partial daily reward "
      "({} points earned on {} server-tracked slots)",
      clientContext.characterUid, earnedPoints, trackedSlots);
  }

  // Get the quest registry and find the appropriate reward
  const auto& questRegistry = _serverInstance.GetQuestRegistry();

  // Find the highest reward tier that doesn't exceed the command rewardPoints
  std::optional<registry::QuestRewardPoint> bestReward;
  uint32_t bestRewardPoints = 0;

  // LOA-fix (NEW-1, round3): потолок тира — очки, ФАКТИЧЕСКИ заработанные на
  // подтверждённых слотах. Раньше потолком была статическая сумма всех трёх
  // слотов (group.rewardPoints, проверка выше), поэтому слот класса, который
  // сервер измерить не может, всё равно приносил свои очки — «выполнял» его
  // клиент. Теперь такой слот приносит ноль и тир просто ниже.
  // (command.rewardPoints — uint16_t, поэтому явный тип у std::min.)
  const uint32_t claimablePoints = std::min<uint32_t>(command.rewardPoints, earnedPoints);

  for (const auto& [points, rewardPoint] : questRegistry.GetQuestRewardPoints())
  {
    // Only consider rewards that don't exceed the requested points
    if (points <= claimablePoints && points > bestRewardPoints)
    {
      bestReward = rewardPoint;
      bestRewardPoints = points;
    }
  }

  if (!bestReward.has_value())
  {
    return;
  }

  const auto& rewardPoint = bestReward.value();

  // LOA-fix (F2, quest-batch-1): помечаем награду дня выданной ДО начисления
  // (fail-closed: если начисление упадёт, повторно забрать всё равно нельзя).
  // Флаг снимается суточным сбросом (ResetDailyQuestsIfNeeded) и регистрацией
  // группы в новом игровом дне.
  groupRecord.Mutable(
    [](data::DailyQuestGroup& group)
    {
      group.dailyRewardClaimed = true;
    });

  // Award the items to the character
  characterRecord.Mutable([this, &response, &rewardPoint](data::Character& character)
    {
      for (const auto& rewardItem : rewardPoint.items)
      {
        // LOA-fix (R2, round2): ВРЕМЕННЫЕ предметы выдаём длительностью, а не
        // количеством. count у Temporary-предмета в таблице наград — это ЧАСЫ
        // (тиры так и называются: «그린리브 보호구 (36시간)»). count-перегрузка
        // AddItem ставила duration = 0 → 36 ВЕЧНЫХ копий дорогого бардинга за один
        // клейм. Форма проверки скопирована с HandleRequestQuestReward.
        data::Uid itemUid{};
        const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(rewardItem.tid);
        if (itemTemplate.has_value() && itemTemplate->type == registry::Item::Type::Temporary)
        {
          itemUid = _serverInstance.GetItemSystem().AddItem(
            character, rewardItem.tid, std::chrono::hours(rewardItem.count));
        }
        else
        {
          itemUid = _serverInstance.GetItemSystem().AddItem(
            character, rewardItem.tid, rewardItem.count);
        }

        // LOA-fix (A6, round3): см. тот же гард в HandleRequestQuestReward.
        const auto itemRecord = _serverInstance.GetDataDirector().GetItem(itemUid);
        if (not itemRecord.IsAvailable())
        {
          server::util::QuietLogError(
            "HandleRequestDailyQuestReward: failed to grant daily reward item {}",
            rewardItem.tid);
          continue;
        }

        itemRecord.Immutable(
          [&response](const data::Item& item)
          {
            auto& protocolItem = response.rewards.items.emplace_back();
            protocol::BuildProtocolItem(protocolItem, item);
          });
      }
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  protocol::AcCmdCRUpdateDailyQuestOK response2{};

  characterRecord.Mutable([&response2](data::Character& character)
    {
      response2.newCarrotBalance = character.carrots();
    });
  response2.quest = {command.questTid, 0, 0, 1};
  response2.unk_1 = 1;
  response2.unk_2 = 1;

  _commandServer.QueueCommand<decltype(response2)>(
    clientId,
    [response2]()
    {
      return response2;
    });
}

void RanchDirector::HandleUpdateMountInfo(
  ClientId clientId,
  const protocol::AcCmdCRUpdateMountInfo command)
{
  const auto& clientContext = GetClientContext(clientId);

  if (command.action == protocol::AcCmdCRUpdateMountInfo::Action::ReturnToNature)
  {
    ReturnHorseToNature(
      clientContext.characterUid,
      command.horse.uid,
      clientContext.userName,
      false);
  }

  const protocol::AcCmdCRUpdateMountInfoOK response{
    .action = command.action,
    .horse = command.horse};
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRegisterQuest(
  ClientId clientId,
  const protocol::AcCmdCRRegisterQuest& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // Check if the character already has this quest active.
  bool alreadyHasQuest = false;
  characterRecord.Immutable([this, &alreadyHasQuest, questId = command.questId](const data::Character& character)
  {
    const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(character.quests());
    if (not questRecords)
      return;
    for (const auto& questRecord : *questRecords)
    {
      questRecord.Immutable([&alreadyHasQuest, questId](const data::Quest& quest)
      {
        if (quest.questId() == questId
          && quest.isCompleted() != data::Quest::Status::Completed)
          alreadyHasQuest = true;
      });
    }
  });

  if (alreadyHasQuest)
  {
    server::util::QuietLogWarn("HandleRegisterQuest: Character {} already has quest {} active",
      clientContext.characterUid, command.questId);
    return;
  }

  // LOA-fix (R4, round2): ГЕЙТ ВЗЯТИЯ КВЕСТА. Upstream регистрировал любой tid,
  // который прислал клиент: ни уровня, ни цепочки preceding. Модифицированный
  // клиент брал сразу финальные квесты цепочек и сдавал их — у части квестов
  // successType == 0 («поговори с NPC»), их анти-эксплойт-гейт (F7) не касается,
  // так что это ~9600 морковок и десяток стеков предметов мимо всей игры.
  // Проверяем ровно то, что проверяет штатный клиент у NPC:
  //   • квест существует в реестре;
  //   • уровень персонажа >= quest.level (персонаж создаётся 40-м уровнем,
  //     LobbyNetworkHandler; максимум по quests.yaml — 13, живых это не задевает);
  //   • для СЮЖЕТНЫХ квестов (groupType 0 = Type::Main) все preceding уже
  //     Completed. Сверено по quests.yaml: висячих ссылок в preceding нет ни
  //     одной, а стартовые квесты цепочек (11030, 14010 и т.д.) preceding не
  //     имеют — честный игрок гейт не замечает.
  // Дейлики (groupType 2) сюда не попадают вообще: они регистрируются каналом
  // 0x33e, а их preceding ссылается на служебный квест 100, который игрок не
  // берёт никогда — гейт на них специально НЕ распространяется.
  const auto& questRegistry = _serverInstance.GetQuestRegistry();
  const auto questTemplate = questRegistry.GetQuest(command.questId);
  if (not questTemplate.has_value())
  {
    server::util::QuietLogWarn(
      "HandleRegisterQuest: Character {} requested unknown quest {}; refusing",
      clientContext.characterUid, command.questId);
    return;
  }

  uint32_t characterLevel = 0;
  std::vector<uint32_t> completedQuestTids;
  characterRecord.Immutable(
    [this, &characterLevel, &completedQuestTids](const data::Character& character)
    {
      characterLevel = character.level();

      const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(
        character.quests());
      if (not questRecords)
        return;

      for (const auto& questRecord : *questRecords)
      {
        questRecord.Immutable(
          [&completedQuestTids](const data::Quest& quest)
          {
            if (quest.isCompleted() == data::Quest::Status::Completed)
              completedQuestTids.push_back(quest.questId());
          });
      }
    });

  if (questTemplate->level > characterLevel)
  {
    server::util::QuietLogWarn(
      "HandleRegisterQuest: Character {} (level {}) requested quest {} requiring "
      "level {}; refusing",
      clientContext.characterUid, characterLevel, command.questId, questTemplate->level);
    return;
  }

  if (questTemplate->type == registry::Quest::Type::Main)
  {
    // LOA-fix (R19-2, quest-batch-2): СЮЖЕТНЫЙ КВЕСТ БЕРЁТСЯ ОДИН РАЗ.
    // Дедуп-гард в начале функции считает «квест уже есть» только для записи
    // со статусом != Completed. По пройденному сюжетному квесту клиент поэтому
    // может зарегистрировать ВТОРУЮ запись, и после R19-1 она рождается сразу
    // ReadyToClaim — над NPC навсегда повисает «?» у игрока, который этот шаг
    // давно сдал. Сюжет одноразовый по дизайну, так что отказываем.
    // ЗАУЖЕНИЕ: проверка живёт внутри блока Type::Main (== groupType 0,
    // QuestRegistry.cpp:61 + enum Main=0/Repeatable=1/Daily=2/Event=7), то есть
    // повторяемых и дейликов не касается вообще; дейлики к тому же ходят другим
    // каналом (HandleRegisterDailyQuestGroup, character.dailyQuestGroupUid()).
    // Отказ от квеста не ломается: HandleGiveupQuest удаляет InProgress-запись
    // целиком, Completed-записи после него не остаётся.
    for (const uint32_t completedTid : completedQuestTids)
    {
      if (completedTid != static_cast<uint32_t>(command.questId))
        continue;

      server::util::QuietLogWarn(
        "HandleRegisterQuest: Character {} requested main story quest {} which is "
        "already completed; refusing (main story is one-time)",
        clientContext.characterUid, command.questId);
      return;
    }

    for (const uint32_t precedingTid : questTemplate->preceding)
    {
      bool precedingCompleted = false;
      for (const uint32_t completedTid : completedQuestTids)
      {
        if (completedTid == precedingTid)
        {
          precedingCompleted = true;
          break;
        }
      }

      if (not precedingCompleted)
      {
        server::util::QuietLogWarn(
          "HandleRegisterQuest: Character {} requested quest {} without completing "
          "its preceding quest {}; refusing",
          clientContext.characterUid, command.questId, precedingTid);
        return;
      }
    }
  }

  // Create a new quest record, populate it, and attach it to the character.
  const auto questRecord = _serverInstance.GetDataDirector().CreateQuest();
  questRecord.Mutable([this, questId = command.questId](data::Quest& quest)
  {
    quest.questId() = questId;
    quest.isCompleted() = data::Quest::Status::InProgress;
    quest.progress() = 0;

    // LOA-fix (FLIGHT-ARC story-ready + R19-1): сюжетные шаги, которые сдаются
    // РАЗГОВОРОМ с NPC. Клиент предлагает сдачу только при ReadyToClaim, а
    // собственные счётчики этих квестов либо не существуют (successType 0),
    // либо принадлежат мёртвым классам функций. Обоснование и порядок отката —
    // в apply_patches.py рядом с этим патчем.
    //   • первые семь — арка «Полёт» (FLIGHT-ARC, играется в проде с flight-r2);
    //   • остальные восемь добавлены раундом 19 (quest-batch-2): те же MAIN
    //     (groupType 0) квесты с successType 0 и successValue 1, у которых
    //     прогресса нет и быть не может.
    // ★13012 сюда НЕ добавлен намеренно: у него successValue 1000
    // (GlidingDistanceValue) — настоящая цель, см. бэклог #7.
    for (const uint32_t storyTid : {// арка «Полёт» (FLIGHT-ARC)
                                    12010u, 11032u, 11033u, 13010u, 11041u,
                                    13011u, 13015u,
                                    // раунд 19 (quest-batch-2)
                                    11030u, 11034u, 11043u, 11045u, 12022u,
                                    14029u, 14031u, 14032u})
    {
      if (storyTid != static_cast<uint32_t>(questId))
        continue;
      const auto storyTemplate = _serverInstance.GetQuestRegistry().GetQuest(storyTid);
      quest.progress() = storyTemplate.has_value() ? storyTemplate->successValue : 1;
      quest.isCompleted() = data::Quest::Status::ReadyToClaim;
      break;
    }
  });

  data::Uid newQuestUid = data::InvalidUid;
  questRecord.Immutable([&newQuestUid](const data::Quest& quest)
  {
    newQuestUid = quest.uid();
  });

  characterRecord.Mutable([newQuestUid](data::Character& character)
  {
    character.quests().emplace_back(newQuestUid);
  });

  protocol::AcCmdCRRegisterQuestOK response{};
  questRecord.Immutable([&response](const data::Quest& quest)
  {
    response.questId    = static_cast<uint16_t>(quest.questId());
    response.progress   = quest.progress();
    response.isCompleted = static_cast<uint8_t>(quest.isCompleted());
  });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleOpenRandomBox(
  ClientId clientId,
  const protocol::AcCmdCROpenRandomBox& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid);

  // LOA-fix (R29-1, #59 S21-a, SECURITY): HandleOpenRandomBox был ПЕЧАТНЫМ СТАНКОМ
  // морковок. Три дыры разом:
  //   (1) command.itemUid НИГДЕ не сверялся с инвентарём (ср. HandleUseItem, где
  //       ownership есть: std::ranges::contains(character.inventory(), usedItemUid));
  //   (2) награда (200..1000 морковок ЛИБО случайный пакет из ВСЕХ 73 в packages.yaml,
  //       включая 43001 Growth Elixir = InstantGrowUpItemTid, ~1000 морковок в лавке)
  //       начислялась ДО и НЕЗАВИСИМО от списания;
  //   (3) списание шло ПОСЛЕДНИМ, по tid из НЕПРОВЕРЕННОГО uid, и его вердикт
  //       (itemConsumed) игнорировался.
  // Петля эксплойта без единого условия: слать 0x43a с uid НАДЕТОЙ вещи (она лежит в
  // characterEquipment, а не в inventory) — ConsumeItem ничего не находит и возвращает
  // {}, а морковки уже начислены. ~+300 морковок за пакет при стоимости 0.
  // Приводим к контракту R22-1/R22-4: ВАЛИДИРОВАТЬ -> СПИСАТЬ -> ВЫДАТЬ.
  // Канал отказа уже есть в протоколе и апстримом не использовался:
  // AcCmdCROpenRandomBoxCancel (0x43c) + OpenRandomBoxError::ItemNotExists.
  const auto sendOpenRandomBoxCancel = [this, clientId, &command]()
  {
    const protocol::AcCmdCROpenRandomBoxCancel cancel{
      .member1 = command.itemUid,
      .error = protocol::OpenRandomBoxError::ItemNotExists};
    _commandServer.QueueCommand<protocol::AcCmdCROpenRandomBoxCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
  };

  // Набор tid'ов НАСТОЯЩИХ коробок. Авторитетного флага «это random box» в items.yaml
  // НЕТ (все пять — type 2, category 3 / subcategory 5, ровно как кристаллы и жетоны),
  // поэтому список выведен ВРУЧНУЮ по именам/назначению: 45006 '주나의 깜짝상자',
  // 50005 "Juna's Mystery Box" (ЕДИНСТВЕННАЯ добываемая в игре — награда квеста id 160,
  // quests.yaml:381), 50006 '주나의 깜짝상자 더미', 50007/50008 'Christmas Mystery Box'.
  // ★Урок R22-8 (сужение гейта убило живую фичу): отказ по tid ЛОГИРУЕТСЯ С TID —
  // ложный негатив всплывёт строкой в логе, а не тишиной. Новую/сезонную коробку
  // добавлять СЮДА.
  static constexpr data::Tid RandomBoxTids[] = {45006, 50005, 50006, 50007, 50008};

  // Валидация и списание одним Mutable'ом ДО любой выдачи: всё, что ниже по функции
  // (морковки/пакет), исполняется только если коробка РЕАЛЬНО списана.
  uint32_t remainingBoxCount = 0;
  bool boxConsumed = false;
  characterRecord.Mutable(
    [this, &command, &clientContext, &remainingBoxCount, &boxConsumed](data::Character& character)
    {
      // (дыра 1) владение. СОЗНАТЕЛЬНО inventory(), а НЕ HasItemInstance: последний
      // считает своим и НАДЕТЫЙ предмет (characterEquipment), а ConsumeItem умеет
      // списывать ТОЛЬКО из inventory — гейт и списание обязаны смотреть на ОДИН
      // набор, иначе валидация проходит там, где списание невозможно (это и есть
      // исходная петля эксплойта).
      if (not std::ranges::contains(character.inventory(), command.itemUid))
      {
        server::util::QuietLogWarn("OpenRandomBox: character {} named item {} it does not own; refusing",
          clientContext.characterUid, command.itemUid);
        return;
      }

      // Резолв tid ИЗ ЗАПИСИ. ★Готча R22: Record::Immutable на недоступной записи
      // БРОСАЕТ (Record.hpp:115-118), поэтому доступность проверяем ЯВНО.
      const auto itemRecord = GetServerInstance().GetDataDirector().GetItemCache().Get(
        command.itemUid);
      if (not itemRecord)
      {
        server::util::QuietLogWarn("OpenRandomBox: character {} named item {} whose record is unavailable; refusing",
          clientContext.characterUid, command.itemUid);
        return;
      }
      data::Tid boxTid = data::InvalidTid;
      itemRecord->Immutable([&boxTid](const data::Item& item) { boxTid = item.tid(); });

      // (дыра 2) а это вообще коробка? Без гейта «открыть» можно было ЛЮБОЙ свой
      // предмет — например сахарный кубик 41007 за 20 морковок — и получить ~300.
      if (not std::ranges::contains(RandomBoxTids, boxTid))
      {
        server::util::QuietLogWarn("OpenRandomBox: character {} used non-box item {} (tid {}); refusing",
          clientContext.characterUid, command.itemUid, boxTid);
        return;
      }

      // (дыра 3) СПИСЫВАЕМ ПЕРВЫМ и уважаем вердикт. remainingItemCount забираем
      // СРАЗУ: ★готча R22 — ConsumeItem обнуляет verdict.itemUid, когда стопка
      // опустела, так что по itemUid судить о результате нельзя.
      const auto consumeVerdict = GetServerInstance().GetItemSystem().ConsumeItem(
        character, boxTid, 1);
      if (not consumeVerdict.itemConsumed)
      {
        server::util::QuietLogWarn("OpenRandomBox: character {} failed to consume box {} (tid {}); refusing",
          clientContext.characterUid, command.itemUid, boxTid);
        return;
      }

      remainingBoxCount = consumeVerdict.remainingItemCount;
      boxConsumed = true;
    });

  if (not boxConsumed)
  {
    sendOpenRandomBoxCancel();
    return;
  }

  protocol::AcCmdCROpenRandomBoxOK response{};

  std::uniform_int_distribution<uint32_t> booleanDistribution(0, 1);

  const bool isPackageReward = booleanDistribution(server::util::GetRandomEngine());

  if (isPackageReward)
  {
    std::uniform_int_distribution<uint32_t> carrotAmountDistribution(20, 100);
    const auto carrotAmount = carrotAmountDistribution(server::util::GetRandomEngine())*10;

    response = {
      .packageId = 0,
      .carrotsObtained = carrotAmount};

    characterRecord.Mutable(
    [this, carrotAmount, &response](data::Character& character)
      {
        character.carrots() += carrotAmount;
        response.newBalance = character.carrots();
      }
    );
  }
  else
  {
    data::Uid uid = data::InvalidUid;
    const auto packageKeysView = std::views::keys(_serverInstance.GetItemRegistry().GetPackages());
    std::vector<data::Tid> possiblePackages;
    std::ranges::copy(packageKeysView, std::back_inserter(possiblePackages));

    std::uniform_int_distribution<uint32_t> randomPackageDistribution(
      0,
      static_cast<uint32_t>(possiblePackages.size()) - 1);
    const auto randomPackageIdx = randomPackageDistribution(server::util::GetRandomEngine());

    const data::Tid PackageTid = possiblePackages[randomPackageIdx];

    const auto packageTemplate = _serverInstance.GetItemRegistry().GetPackage(PackageTid);

    response = {
      .packageId = packageTemplate->packageId,
    };
    //add package to inventory
    characterRecord.Mutable(
      [this, packageTemplate, &uid](data::Character& character)
      {
        // LOA-fix (R64-1, round64, backlog #203): ВРЕМЕННЫЕ предметы выдаём
        // ДЛИТЕЛЬНОСТЬЮ, а не количеством. Тот же класс, что R2 уже закрыл в
        // наградах дня и квестов, — здесь он остался: count-перегрузка ставит
        // `duration = 0`, то есть предмет либо не работает, либо истекает
        // мгновенно.
        //
        // ★РАДИУС ПОСЧИТАН, А НЕ ПРИКИНУТ: коробка берёт СЛУЧАЙНЫЙ пакет из
        // ВСЕГО реестра (73 пакета), и по items.yaml ровно **22 из них**
        // ссылаются на предмет `type = Temporary`. То есть примерно каждое
        // третье открытие выдавало пустышку. Остальные 51 (39 штучных type 2 +
        // 12 type 0) count-перегрузкой обслуживаются верно — их трогать нельзя.
        //
        // ★`count` у Temporary-предмета означает ЧАСЫ, а не штуки (тиры в
        // оригинале так и названы: «(36시간)»). Форма проверки скопирована с
        // `HandleRequestDailyQuestReward` (R2) — намеренно дословно: один класс
        // дефекта обязан лечиться одинаково, иначе следующий читатель решит,
        // что разница что-то значит.
        const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(
          packageTemplate->tid);
        if (itemTemplate.has_value()
          && itemTemplate->type == registry::Item::Type::Temporary)
        {
          uid = _serverInstance.GetItemSystem().AddItem(
            character, packageTemplate->tid,
            std::chrono::hours(packageTemplate->count));
        }
        else
        {
          uid = _serverInstance.GetItemSystem().AddItem(
            character, packageTemplate->tid, packageTemplate->count);
        }
      });
  }
  // TODO: figure out how to make the open box window appear after opening
  response.unk0 = command.itemUid;

  // LOA-fix (R29-2, #59 S21-a, SECURITY): коробка уже провалидирована и списана в
  // R29-1 ДО выдачи награды. Повторный ConsumeItem здесь означал бы ДВОЙНУЮ оплату
  // (стопка 2 -> 0 за одно открытие), поэтому списание убрано, а остаток берётся из
  // вердикта R29-1. Заодно уходит небезопасный резолв tid по непроверенному uid:
  // itemRecord.Immutable на недоступной записи БРОСАЕТ (Record.hpp:115-118) — прямо
  // внутри Mutable-лямбды, то есть под замком записи персонажа.
  // Список захвата: command здесь больше не нужен, вместо него remainingBoxCount
  // (иначе command стал бы неиспользуемым захватом).
  characterRecord.Mutable([this, &response, remainingBoxCount](data::Character& character)
    {
      response.unk1 = remainingBoxCount;
      // LOA-fix (WARN3, round6): Get(KeySpan) отдаёт nullopt, если хотя бы одна
      // запись инвентаря недоступна — разыменовывать optional без проверки
      // нельзя (UB). Раньше сюда было не дойти: AddItem падал в terminate.
      const auto itemRecords = _serverInstance.GetDataDirector().GetItemCache().Get(character.inventory());
      if (itemRecords)
      {
        protocol::BuildProtocolItems(response.items, *itemRecords);
      }
      else
      {
        server::util::QuietLogError(
          "Failed to read the inventory of character '{}' after opening a "
          "random box; the response will not list items",
          character.name());
      }
    }
  );

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleRequestQuestReward(
  ClientId clientId,
  const protocol::AcCmdCRRequestQuestReward& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCRRequestQuestRewardOK response{};
  response.questTid = command.questTid;
  response.carrotsRewarded = 0;

  // Get the quest registry and quest information
  const auto& questRegistry = _serverInstance.GetQuestRegistry();
  const auto questTemplate = questRegistry.GetQuest(command.questTid);

  if (!questTemplate.has_value())
  {
    server::util::QuietLogWarn("HandleRequestQuestReward: Quest {} not found in registry", command.questTid);
    return;
  }

  const auto& quest = questTemplate.value();

  // LOA-fix (batch1 task1): идемпотентность награды + проверка владения квестом.
  // ДО начисления читаем запись квеста игрока (зеркало HandleRegisterQuest):
  // уже Completed → пустой OK без лута; квест не взят → warn + отказ. Иначе
  // проваливаемся в начисление ниже, а P2-финализация выставит Completed один
  // раз — повторный запрос сюда уже упрётся в alreadyClaimed.
  bool questOwned = false;
  bool alreadyClaimed = false;
  uint32_t ownedProgress = 0;
  bool ownedReadyToClaim = false;
  characterRecord.Immutable(
    [this, &command, &questOwned, &alreadyClaimed, &ownedProgress, &ownedReadyToClaim](
      const data::Character& character)
    {
      const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(
        character.quests());
      if (not questRecords)
        return;
      for (const auto& questRecord : *questRecords)
      {
        questRecord.Immutable(
          [&command, &questOwned, &alreadyClaimed, &ownedProgress, &ownedReadyToClaim](
            const data::Quest& questData)
          {
            if (questData.questId() != command.questTid)
              return;
            questOwned = true;
            ownedProgress = questData.progress();
            if (questData.isCompleted() == data::Quest::Status::Completed)
              alreadyClaimed = true;
            if (questData.isCompleted() == data::Quest::Status::ReadyToClaim)
              ownedReadyToClaim = true;
          });
      }
    });

  // Награда уже получена — повторный запрос НЕ начисляет ничего. Возвращаем
  // минимальный OK (клиент закрывает диалог NPC), но БЕЗ лута.
  if (alreadyClaimed)
  {
    protocol::AcCmdCRRequestQuestRewardOK alreadyClaimedResponse{};
    alreadyClaimedResponse.questTid = command.questTid;
    alreadyClaimedResponse.carrotsRewarded = 0;
    alreadyClaimedResponse.npcEffects[0] = {command.npcId, 0};
    _commandServer.QueueCommand<decltype(alreadyClaimedResponse)>(
      clientId,
      [alreadyClaimedResponse]()
      {
        return alreadyClaimedResponse;
      });
    return;
  }

  // Квест не взят игроком — не выдаём награду за невзятый/чужой квест.
  if (not questOwned)
  {
    server::util::QuietLogWarn(
      "HandleRequestQuestReward: Character {} requested reward for quest {} "
      "they do not own; refusing",
      clientContext.characterUid, command.questTid);
    return;
  }

  // LOA-fix (batch1 fix-round1, BLOCK2): completion gate for SERVER-TRACKED count
  // quests. T1 previously paid any owned && !Completed quest, so a hacked client
  // could claim a count-quest reward at 0/N. For quests whose progress the server
  // authoritatively tracks — feed/wash 11039/11040 (P4a/P4b) and race/spur
  // 11035/11036 (T4), all successType==1 — require the objective be met
  // (progress >= successValue, or the record already ReadyToClaim) before paying.
  //
  // We DELIBERATELY restrict the gate to that tracked set instead of gating ALL
  // successType==1 quests: other successType==1 MAIN quests are NOT server-tracked
  // (verified in quests.yaml: 11037 = RunMap, 11031 = a count quest with no server
  // hook) — their record progress stays 0 forever, so a blanket successType gate
  // would PERMANENTLY block their legitimate NPC turn-in (a worse regression than
  // the exploit). successType==0 talk-to-NPC quests keep the old behaviour (claim
  // allowed while InProgress). Refuse path = empty OK: there is NO
  // AcCmdCRRequestQuestRewardCancel struct (only an enum id, no Write/Read), so we
  // mirror the alreadyClaimed empty-OK that closes the NPC dialog without loot.
  // LOA-fix (F7, quest-batch-1): гейт расширен на все классы, которые этот батч
  // сделал отслеживаемыми (см. список в apply_patches.py рядом с этим патчем).
  // Мёртвые классы (CollectDropItem/Gliding/PerfectJump/Fireball/ClearMission)
  // сюда НЕ входят: их прогресс навсегда 0, гейт заблокировал бы их насмерть.
  // Страховка от ошибки трекинга: квест всегда можно снять через
  // AcCmdCRGiveupQuest 0x3ec и взять заново — гейт не создаёт вечного тупика.
  bool serverTrackedCountQuest = false;
  for (const uint32_t trackedTid : {
         // уход: кормление/мытьё (P4a/P4b)
         11039u, 11040u, 14011u, 14012u,
         // рывок (T4)
         11036u,
         // «заездов в любом режиме: N» — финиш заезда (T4 + F1)
         11035u, 11031u, 11044u, 12021u, 14023u, 14025u, 14030u,
         // призовое место, топ-3 (F7)
         12012u, 12018u, 14015u, 14016u, 14027u,
         // «пройди карту N раз» (F7)
         // LOA-fix (FLIGHT-ARC): 11041 и 13015 СНЯТЫ с гейта — это сюжетные
         // шаги квестовой арки «Полёт», они сдаются разговором с NPC.
         // Прогресс им по-прежнему считается (F8 выше), просто перестал быть
         // обязательным. Обоснование — в apply_patches.py рядом с патчем.
         11037u, 13013u, 14013u, 14017u})
  {
    if (trackedTid == static_cast<uint32_t>(command.questTid))
    {
      serverTrackedCountQuest = true;
      break;
    }
  }
  // LOA-fix (R68, backlog #5/#99): СЮЖЕТНЫЕ КВЕСТЫ «СОБЕРИ N ПРЕДМЕТОВ».
  //
  // ★ЭТО НЕ ДОБАВКА, А ВТОРАЯ ПОЛОВИНА ОДНОГО ФИКСА, И РАЗДЕЛЯТЬ ИХ НЕЛЬЗЯ.
  // До этого раунда класс `CollectDropItem` был мёртв: прогресс не считался
  // НИКОГДА — именно поэтому эти 12 квестов в гейт и не входили (абзац выше:
  // гейт заблокировал бы их насмерть). Но `successType` у всех двенадцати
  // равен 1, то есть сервер ПЛАТИЛ за них при нулевом прогрессе. Как только
  // раунд оживил счётчик, отсутствие гейта превратилось бы из безобидного
  // долга в эксплойт «награда без квеста» — ровно класс R22.
  //
  // ★ИСТОЧНИК СПИСКА ОДИН (`QuestSystem::CollectDropItemMainQuestTids`) — тот
  // же, по которому предметы раскладываются и по которому двигается прогресс.
  // Три копии чисел разъехались бы молча.
  //
  // ★ИВЕНТОВЫХ 1035-1039 в списке НЕТ: их предметы кладёт дека 701, у которой
  // в courses.yaml нет ни одной координаты. Прогресс им недостижим, поэтому
  // гейт сделал бы их несдаваемыми навсегда — это была бы регрессия хуже
  // эксплойта.
  for (const uint32_t collectQuestTid : QuestSystem::CollectDropItemMainQuestTids)
  {
    if (collectQuestTid == static_cast<uint32_t>(command.questTid))
    {
      serverTrackedCountQuest = true;
      break;
    }
  }
  const bool objectiveMet =
    ownedReadyToClaim || ownedProgress >= quest.successValue;
  if (quest.successType == 1 && serverTrackedCountQuest && not objectiveMet)
  {
    server::util::QuietLogWarn(
      "HandleRequestQuestReward: Character {} requested reward for count-quest {} "
      "before completing it ({}/{}); refusing",
      clientContext.characterUid, command.questTid, ownedProgress, quest.successValue);
    protocol::AcCmdCRRequestQuestRewardOK refuseResponse{};
    refuseResponse.questTid = command.questTid;
    refuseResponse.carrotsRewarded = 0;
    refuseResponse.npcEffects[0] = {command.npcId, 0};
    _commandServer.QueueCommand<decltype(refuseResponse)>(
      clientId,
      [refuseResponse]()
      {
        return refuseResponse;
      });
    return;
  }

  // Award rewards to the character
  characterRecord.Mutable([this, &response, &quest, command](data::Character& character)
  {
    // LOA-fix (R42-1, round42, #8): выплата quest.rewardGameMoney на NPC-сдаче.
    // БЫЛО: платились только reward.carrots (таблица rewardId) + предметы, а
    // собственное поле quest.rewardGameMoney НЕ начислялось никогда. Единственный
    // расходящийся квест по всей базе — 100 «Разговор с дедушкой» (rewardGameMoney=1000,
    // rewardId=0): клиент рисует «+1000 Carrots», сервер платил 0. Дейлики исключены
    // (их деньги платит R17 через свой путь) гардом type != Daily — двойной оплаты нет.
    // int64-кламп 2e9 (класс R40-1/F4): carrots — int32, сырой += даёт overflow-UB.
    // Идемпотентность обеспечивает alreadyClaimed-гейт выше (повторная сдача = пустой OK).
    if (quest.rewardGameMoney > 0
      && quest.type != registry::Quest::Type::Daily)
    {
      const int64_t current = static_cast<int64_t>(character.carrots());
      if (current >= 0 && current <= 2000000000)
      {
        const int64_t grant = std::min<int64_t>(
          static_cast<int64_t>(quest.rewardGameMoney), 2000000000 - current);
        character.carrots() = static_cast<int32_t>(current + grant);
        response.carrotsRewarded += static_cast<uint32_t>(grant);
      }
    }

    // Award items from quest reward ID if it exists
    if (quest.rewardId > 0)
    {
      const auto questReward = _serverInstance.GetQuestRegistry().GetQuestReward(quest.rewardId);
      if (questReward.has_value())
      {
        const auto& reward = questReward.value();

        // Award additional carrots from reward
        if (reward.carrots > 0)
        {
          character.carrots() += reward.carrots;
          response.carrotsRewarded += reward.carrots;
        }

        // Award items from the reward
        for (const auto& rewardItem : reward.items)
        {
          data::Uid itemUid{};
          
          // Check item type to determine if we should use count or duration
          const auto itemTemplate = _serverInstance.GetItemRegistry().GetItem(rewardItem.tid);
          if (itemTemplate.has_value() && itemTemplate->type == registry::Item::Type::Temporary)
          {
            // For temporary items, count represents hours, so convert to duration
            itemUid = _serverInstance.GetItemSystem().AddItem(
              character, rewardItem.tid, std::chrono::hours(rewardItem.count));
          }
          else
          {
            itemUid = _serverInstance.GetItemSystem().AddItem(
              character, rewardItem.tid, rewardItem.count);
          }

          // LOA-fix (A6, round3): выдача могла не состояться (см. ItemSystem)
          // — на пустой записи Immutable бросает, и исключение уносит из-под
          // characterRecord.Mutable уже начисленные морковки.
          const auto itemRecord = _serverInstance.GetDataDirector().GetItem(itemUid);
          if (not itemRecord.IsAvailable())
          {
            server::util::QuietLogError(
              "HandleRequestQuestReward: failed to grant item {} for quest {}",
              rewardItem.tid,
              command.questTid);
            continue;
          }
          itemRecord.Immutable([&response](const data::Item& item)
          {
            auto& protocolItem = response.rewards.items.emplace_back();
            protocol::BuildProtocolItem(protocolItem, item);
          });
        }

        // Set NPC dress effect if specified
        if (reward.keyNpcDress > 0)
        {
          response.npcEffects[0] = {command.npcId, reward.keyNpcDress};
        }
        else
        {
          response.npcEffects[0] = {command.npcId, 0}; // Default effect
        }
      }
      else
      {
        server::util::QuietLogWarn("HandleRequestQuestReward: Quest reward {} not found for quest {}", 
          quest.rewardId, command.questTid);
        response.npcEffects[0] = {command.npcId, 0}; // Default effect
      }
    }
    else
    {
      // No reward ID, just use default effect
      response.npcEffects[0] = {command.npcId, 0};
    }
  });

  // LOA-fix: финализируем запись квеста, иначе она навечно InProgress и клиент
  // рисует незакрытый шаг (talk-to-NPC не засчитывается). Награда = turn-in.
  characterRecord.Immutable([this, &command, &questTemplate](const data::Character& character)
  {
    const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(
      character.quests());
    if (not questRecords)
      return;
    for (const auto& questRecord : *questRecords)
    {
      questRecord.Mutable([&command, &questTemplate](data::Quest& quest)
      {
        if (quest.questId() == command.questTid
          && quest.isCompleted() != data::Quest::Status::Completed)
        {
          quest.progress() = questTemplate->successValue;
          quest.isCompleted() = data::Quest::Status::Completed;
        }
      });
    }
  });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleGiveupQuest(
  ClientId clientId,
  const protocol::AcCmdCRGiveupQuest& command)
{
  // LOA-fix (batch1 task2): реальный отказ от квеста с гардом. Оригинал был
  // заглушкой (//TODO): слал OK, но НЕ трогал ни одной записи, так что «сдаться»
  // ничего не делало. Теперь находим запись квеста игрока и запрещаем отменять
  // уже завершённый/заработанный квест (Completed/ReadyToClaim), а InProgress
  // реально снимаем с персонажа.
  const auto& clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // Ищем ПЕРВУЮ запись квеста игрока по questId (зеркало HandleRegisterQuest):
  // фиксируем факт наличия, «завершён ли» и uid записи (это же значение лежит
  // в character.quests()).
  bool questFound = false;
  bool questFinished = false;
  data::Uid targetQuestUid = data::InvalidUid;
  characterRecord.Immutable(
    [this, &command, &questFound, &questFinished, &targetQuestUid](
      const data::Character& character)
    {
      const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(
        character.quests());
      if (not questRecords)
        return;
      for (const auto& questRecord : *questRecords)
      {
        questRecord.Immutable(
          [&command, &questFound, &questFinished, &targetQuestUid](
            const data::Quest& quest)
          {
            if (questFound)
              return;
            if (quest.questId() != command.questId)
              return;
            questFound = true;
            // Completed(3) и ReadyToClaim(1) — квест завершён/заработан: отменять
            // нечего и нельзя (иначе можно «отменить» готовую к сдаче награду).
            questFinished =
              quest.isCompleted() == data::Quest::Status::Completed
              || quest.isCompleted() == data::Quest::Status::ReadyToClaim;
            targetQuestUid = quest.uid();
          });
      }
    });

  // Нельзя отказаться от невзятого либо уже завершённого/готового к сдаче квеста —
  // шлём Cancel (клиент оставляет квест как есть).
  if (not questFound || questFinished)
  {
    protocol::AcCmdCRGiveupQuestCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  // InProgress — реально снимаем квест с персонажа: убираем его uid из
  // character.quests() (тот же паттерн, что DataRepair.cpp std::erase).
  characterRecord.Mutable([targetQuestUid](data::Character& character)
  {
    std::erase(character.quests(), targetQuestUid);
  });

  // Дополнительно удаляем осиротевшую запись квеста через чистый публичный API
  // кэша (DataStorage::Delete ставит удаление в очередь). Даже без этого запись
  // безвредна — на неё уже нет ссылки из персонажа.
  _serverInstance.GetDataDirector().GetQuestCache().Delete(targetQuestUid);

  protocol::AcCmdCRGiveupQuestOK response{
    .questId = command.questId
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleInviteUser(
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

  // By copy
  const auto recipientPresence = clientOpt.value().clientContext.presence;

  // Invites from ranch are more limited, you can only invite characters that are in
  // a ranch to your ranch
  const auto& recipientScene = recipientPresence.scene;
  if (recipientScene == protocol::Presence::Scene::Race)
  {
    // Invoker is in ranch, recipient is in race, should not be possible
    server::util::QuietLogWarn("Character '{}', who is in a ranch, tried to invite character '{}' to their ranch",
      clientContext.characterUid,
      command.recipientCharacterUid);
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Sanity check if character can be invited (is away, online or in waiting room)
  const auto& recipientStatus = clientOpt.value().clientContext.presence.status;
  bool canInvite = recipientStatus == protocol::Status::Away or
    recipientStatus == protocol::Status::Online or
    recipientStatus == protocol::Status::WaitingRoom;

  if (not canInvite)
  {
    // Cannot invite character
    server::util::QuietLogWarn("Character '{}' tried to invite character '{}' which is not in an invitable state",
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

void RanchDirector::HandleRequestUser(
  ClientId clientId,
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
  const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;

  if (not isAdmin)
  {
    server::util::QuietLogWarn("User '{}'('{}'), which is not an admin, tried to summon character '{}'",
      userName,
      invokerCharacterName,
      command.characterName);
    return;
  }

  protocol::AcCmdCRRequestUserCancel cancel{};
  cancel.force = command.force;
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

  GetServerInstance().GetRaceDirector().NotifySummonCharacter(characterUid, command.force, command.characterName, command.roomUid, command.ranchUid);
  GetServerInstance().GetRanchDirector().SummonCharacter(characterUid, command.force, command.characterName, command.roomUid, command.ranchUid);

  protocol::AcCmdCRRequestUserOK response{};
  response.force = command.force;
  response.characterName = command.characterName;
  response.roomUid = command.roomUid;
  response.ranchUid = command.ranchUid;

  _commandServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void RanchDirector::SendDailyQuestNotificationToCharacter(
  const data::Uid characterUid,
  const protocol::AcCmdRCUpdateDailyQuestNotify& updateNotify)
{
  for (const auto& [clientId, clientContext] : _clients)
  {
    if (clientContext.characterUid == characterUid)
    {
      _commandServer.QueueCommand<protocol::AcCmdRCUpdateDailyQuestNotify>(
        clientId, [updateNotify]() { return updateNotify; });

      return;
    }
  }
}

// LOA-fix (R48-3, #58/R2-D): ОДНА точка отправки достижений с ранча. В R47
// цикл «спросить систему — поставить нотификации в очередь» стоял прямо в хуке
// мытья; хуков стало восемь, и восемь копий одного цикла — это восемь мест, где
// можно ошибиться по-разному.
// ★ЗДЕСЬ ЖЕ ЖИВЁТ ПРАВИЛО ПОТОКА: систему зовём ВНЕ любого characterRecord.Mutable —
// она берёт тот же замок записи, а он нерекурсивный (Record.hpp), то есть
// вложенный вызов повесил бы ранч-поток намертво.
// ★И правило доставки: персонажа, который уже отключился, мы молча пропускаем.
// Прогресс к этому моменту УЖЕ записан в его данные, поэтому терять здесь
// нечего — а прежний GetClientIdByCharacterUid на отсутствующем клиенте кидал
// исключение прямо посреди хендлера, уже изменившего данные.
// ★НЕ БРОСАЕТ НИКОГДА (R48-11, находка ревью Codex-T3). Хук стоит ПОСЛЕ того,
// как действие уже совершено и данные уже изменены: жеребёнок создан, плата
// снята, вход на ранчо состоялся и клиенту отправлен EnterRanchOK. Исключение,
// улетевшее отсюда, било бы по действию, к которому достижение не имеет
// отношения: на неудачной случке — мимо выдачи карт неудачи, на входе на ранчо —
// прямо в откат уже состоявшегося входа (R13-4b снял бы гостя с ранча). Значок
// не имеет права стоить игроку того, что он уже сделал, поэтому здесь глухой
// перехват и строка в лог: прогресс к этому моменту записан, потеряна только
// нотификация, и клиент увидит достижение при следующем открытии списка.
void RanchDirector::SendAchievementEvent(
  const data::Uid characterUid,
  const uint16_t achievementEvent,
  const std::span<const std::string_view> provenConditions) noexcept
{
  // ★И САМА ДИАГНОСТИКА ОБЯЗАНА БЫТЬ БЕЗОПАСНОЙ (R48-12, находка ревью).
  // Наш spdlog на НЕИЗВЕСТНОМ исключении из приёмника лога его ПЕРЕБРАСЫВАЕТ
  // (`catch (...) { err_handler_(...); throw; }` — SPDLOG_LOGGER_CATCH,
  // 3rd-party/spdlog/include/spdlog/logger.h). Значит бросок из строки лога
  // вылетел бы наружу noexcept-функции и завершил процесс — ровно тем способом,
  // который мы этой функцией и закрываем. Поэтому запись в лог живёт в
  // собственном перехвате: не удалось даже пожаловаться — молчим.
  // LOA-fix (R49-11, round49, backlog #178): та же тихая жалоба, что завела
  // R48, переехала в общий заголовок — приём один на весь сервер, а не копия
  // лямбды в каждом файле.
  const auto reportQuietly = [characterUid, achievementEvent](const char* reason)
  {
    util::QuietLogError(
      "Achievement event {} of character {} was not processed: {}",
      achievementEvent,
      characterUid,
      reason);
  };

  try
  {
    const auto achievementNotifies =
      _serverInstance.GetAchievementSystem().OnServerEvent(
        characterUid, achievementEvent, 1, provenConditions);
    if (achievementNotifies.empty())
      return;

    for (const auto& [clientId, clientContext] : _clients)
    {
      if (clientContext.characterUid != characterUid
        || not clientContext.isAuthenticated)
        continue;

      for (const auto& achievementNotify : achievementNotifies)
      {
        _commandServer.QueueCommand<protocol::AcCmdRCAchievementUpdateNotify>(
          clientId, [achievementNotify]() { return achievementNotify; });
      }

      return;
    }
  }
  catch (const std::exception& x)
  {
    reportQuietly(x.what());
  }
  catch (...)
  {
    // noexcept без этой ветки означал бы terminate на любом не-std исключении —
    // то есть падение всего сервера ради значка.
    reportQuietly("unknown exception");
  }
}

void RanchDirector::HandleBreedingTakeMoney(
  ClientId clientId,
  const protocol::AcCmdCRBreedingTakeMoney& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Check if claim is validate and successful, by claimUid
  const bool claimSuccessful = GetServerInstance().GetRewardSystem().ClaimReward(
    command.claimUid,
    clientContext.characterUid);

  if (not claimSuccessful)
  {
    server::util::QuietLogError(
      "Character '{}' was unsuccessful at claiming '{}'",
      clientContext.characterUid,
      command.claimUid);
    const protocol::AcCmdCRBreedingTakeMoneyCancel cancel{};
    _commandServer.QueueCommand<protocol::AcCmdCRBreedingTakeMoneyCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  protocol::AcCmdCRBreedingTakeMoneyOK response{};
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&response](const data::Character& character)
    {
      response.carrotBalance = character.carrots();
    });

  _commandServer.QueueCommand<protocol::AcCmdCRBreedingTakeMoneyOK>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleExpandMountSlot(
  ClientId clientId,
  const protocol::AcCmdCRExpandMountSlot& command)
{
  const auto& clientContext = GetClientContext(clientId);
  const auto& characterRecord = GetServerInstance().GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // Check if character has expand slot item in inventory, track horse slots
  uint32_t horseSlotCount = 0;
  bool hasItem = false;
  characterRecord.Immutable([this, &hasItem, &horseSlotCount, itemUid = command.itemUid](const data::Character& character)
  {
    horseSlotCount = character.horseSlotCount();
    hasItem = GetServerInstance().GetItemSystem().HasItemInstance(character, itemUid);
  });

  const protocol::AcCmdCRExpandMountSlotCancel cancel{};
  if (not hasItem)
  {
    _commandServer.QueueCommand<protocol::AcCmdCRExpandMountSlotCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  data::Tid itemTid{data::InvalidTid};
  GetServerInstance().GetDataDirector().GetItem(command.itemUid).Immutable(
    [this, &itemTid](const data::Item& item)
    {
      itemTid = item.tid();
    });

  const auto& registryItemResult = GetServerInstance().GetItemRegistry().GetItem(itemTid);
  if (not registryItemResult.has_value())
  {
    _commandServer.QueueCommand<protocol::AcCmdCRExpandMountSlotCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  const registry::Item& registryItem = registryItemResult.value();

  bool isValidSlotExpansionItem =
    registryItem.prerequisiteLevel.has_value() and
    registryItem.prerequisiteLevel.value() == horseSlotCount;

  if (not isValidSlotExpansionItem)
  {
    _commandServer.QueueCommand<protocol::AcCmdCRExpandMountSlotCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  // LOA-fix (R29-4, #59 S21-c, ЭКОНОМИКА): пермит на слот НЕ списывался. Владение
  // (HasItemInstance) и prerequisiteLevel == horseSlotCount проверялись, но предмет
  // оставался в инвентаре: «Additional Horse Permit» 46006-46010 / 46111-46117 стоит
  // 72000 морковок и имеет type: 0 (Permanent) — то есть ОДИН купленный пермит
  // обслуживал ВЕСЬ сервер, его можно было передавать подарком следующему игроку
  // того же тира. Списываем ПЕРЕД инкрементом и инкрементим ТОЛЬКО при успешном
  // списании (контракт R22-1: валидировать -> списать -> выдать).
  // ★ Как и в R29-3: HasItemInstance считает своим НАДЕТЫЙ предмет, а ConsumeItem
  // списывает только из inventory — расхождение разрешаем в пользу отказа.
  uint8_t newHorseSlotCount = 0;
  bool permitConsumed = false;
  characterRecord.Mutable([this, itemTid, &permitConsumed, &newHorseSlotCount](data::Character& character)
  {
    if (not GetServerInstance().GetItemSystem().ConsumeItem(character, itemTid, 1).itemConsumed)
      return;

    permitConsumed = true;
    character.horseSlotCount() += 1;
    newHorseSlotCount = character.horseSlotCount();
  });

  if (not permitConsumed)
  {
    server::util::QuietLogWarn("ExpandMountSlot: character {} could not consume permit {} (tid {}); refusing",
      clientContext.characterUid, command.itemUid, itemTid);
    _commandServer.QueueCommand<protocol::AcCmdCRExpandMountSlotCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  const protocol::AcCmdCRExpandMountSlotOK response{
    .mountSlots = newHorseSlotCount};
  _commandServer.QueueCommand<protocol::AcCmdCRExpandMountSlotOK>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleBreedingWishlistAdd(
  ClientId clientId,
  const protocol::AcCmdCRBreedingWishlistAdd& command)
{
  const auto& clientContext = GetClientContext(clientId);

  const auto& cancelResponse = [this](ClientId clientId)
  {
    const protocol::AcCmdCRBreedingWishlistAddCancel cancel{};
    _commandServer.QueueCommand<protocol::AcCmdCRBreedingWishlistAddCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
  };

  // Confirm that the horse exists
  const auto& horseRecord = GetServerInstance().GetDataDirector().GetHorse(command.horseUid);
  if (not horseRecord)
  {
    cancelResponse(clientId);
    return;
  }

  // Confirm that the horse is a stallion
  // TODO: maybe check if a stallion record exists too?
  bool isHorseStallion = false;
  horseRecord.Immutable([&isHorseStallion](const data::Horse& horse)
  {
    isHorseStallion = horse.type() == data::Horse::Type::Stallion;
  });

  if (not isHorseStallion)
  {
    cancelResponse(clientId);
    return;
  }

  // Add horse to character's wishlist
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [horseUid = command.horseUid](data::Character& character)
    {
      character.breedingWishlist().insert(horseUid);
    });

  const protocol::AcCmdCRBreedingWishlistAddOK response{};
  _commandServer.QueueCommand<protocol::AcCmdCRBreedingWishlistAddOK>(
    clientId,
    [response]()
    {
      return response;
    });
}

void RanchDirector::HandleBreedingWishlistDelete(
  ClientId clientId,
  const protocol::AcCmdCRBreedingWishlistDel& command)
{
  const auto& clientContext = GetClientContext(clientId);
  
  // Check that the character has this stallion favourited and then delete
  // No need to check if the horse exists, better to remove from the list (implicit)
  bool success = false;
  GetServerInstance().GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&success, horseUid = command.horseUid](data::Character& character)
    {
      // Check if this character has the horse in the wishlist
      if (not std::ranges::contains(character.breedingWishlist(), horseUid))
        return;

      // Character has the horse in the wishlist, remove it
      character.breedingWishlist().erase(horseUid);
      success = true;
    });

  if (not success)
  {
    const protocol::AcCmdCRBreedingWishlistDelCancel cancel{};
    _commandServer.QueueCommand<protocol::AcCmdCRBreedingWishlistDelCancel>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  const protocol::AcCmdCRBreedingWishlistDelOK response{};
  _commandServer.QueueCommand<protocol::AcCmdCRBreedingWishlistDelOK>(
    clientId,
    [response]()
    {
      return response;
    });
}

} // namespace server
