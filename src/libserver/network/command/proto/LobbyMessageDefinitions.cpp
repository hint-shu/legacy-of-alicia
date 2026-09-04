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

#include "libserver/network/command/proto/LobbyMessageDefinitions.hpp"

#include "libserver/util/BoundedList.hpp"

#include <algorithm>
#include <cassert>
#include <span>
#include <utility>
#include <vector>

namespace server::protocol
{

void AcCmdCLLogin::Write(
  const AcCmdCLLogin&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLLogin::Read(
  AcCmdCLLogin& command,
  SourceStream& stream)
{
  stream.Read(command.constant0)
    .Read(command.constant1)
    .Read(command.loginId)
    .Read(command.memberNo)
    .Read(command.authKey)
    .Read(command.val0);
}

void LobbyCommandLoginOK::SystemContent::Write(const SystemContent& command, SinkStream& stream)
{
  // ★R74. ЯВНЫЙ ПИСАТЕЛЬ ОБЯЗАТЕЛЕН: элемент здесь — `std::pair`, а для пары у
  // `SinkStream` перегрузки `Write` нет вовсе. Заодно снимается вторая беда:
  // обход `std::unordered_map` НЕ ОПРЕДЕЛЁН, то есть порядок этих записей на
  // проводе уже сегодня недетерминирован. Сортировка по ключу превращает
  // неопределённый порядок в определённый — это не смена поведения, а его
  // фиксация. Кламп недостижим: каталог системного контента даёт 69 записей
  // при потолке 255.
  std::vector<std::pair<uint32_t, uint32_t>> orderedValues{
    command.values.begin(), command.values.end()};
  std::sort(orderedValues.begin(), orderedValues.end());

  util::WriteBoundedList<uint8_t>(
    stream,
    orderedValues,
    {.name = "LobbyCommandLoginOK.SystemContent.values"},
    [](SinkStream& sink, const auto& entry)
    {
      sink.Write(entry.first)
        .Write(entry.second);
    });
}

void LobbyCommandLoginOK::SystemContent::Read(
  SystemContent&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void LobbyCommandLoginOK::Write(
  const LobbyCommandLoginOK& command,
  SinkStream& stream)
{
  stream.Write(command.lobbyTime.dwLowDateTime)
    .Write(command.lobbyTime.dwHighDateTime)
    .Write(command.member0);

  // Profile
  stream.Write(command.uid)
    .Write(command.name)
    .Write(command.notice)
    .Write(static_cast<uint8_t>(command.gender))
    .Write(command.introduction);

  // ★R74. ЭТИ ДВА `throw` И БЫЛИ МЕХАНИЗМОМ ВЕЧНОЙ БЛОКИРОВКИ ВХОДА.
  //
  // `LobbyCommandLoginOK::Write` исполняется в поставщике записи, то есть на
  // потоке `Client::WriteLoop`; бросок оттуда ловит `Server.cpp` и зовёт
  // `End()`. Семнадцатый предмет экипировки означал «этот персонаж не входит в
  // игру НИКОГДА» — и путь этот самоинфлицируемый: `characterEquipment` хранит
  // и одежду, и сбрую одним списком, а замена по слоту происходит ТОЛЬКО для
  // предметов, чей tid знает реестр; выпавший из реестра tid остаётся в списке
  // навсегда, а реестр мы правим каждой волной локализации.
  //
  // Потолки переехали в `maxCount` теми же числами и теперь КЛАМПЯТ: игрок
  // входит и видит 16 предметов вместо «не входит вовсе».
  constexpr size_t MaxExpiredItemCount = 250;

  util::WriteBoundedList<uint8_t>(
    stream,
    command.equipmentItems,
    {.maxCount = MaxCharacterEquipmentCount, .name = "LobbyCommandLoginOK.equipmentItems"});

  // Mount equipment
  util::WriteBoundedList<uint8_t>(
    stream,
    command.expiredItems,
    {.maxCount = MaxExpiredItemCount, .name = "LobbyCommandLoginOK.expiredItems"});

  //
  stream.Write(command.level)
    .Write(command.carrots)
    .Write(command.levelProgress)
    .Write(command.role)
    .Write(command.val3);

  //
  stream.Write(command.settings);

  //
  util::WriteBoundedList<uint8_t>(
    stream,
    command.missions,
    {.name = "LobbyCommandLoginOK.missions"},
    [](SinkStream& sink, const auto& mission)
    {
      sink.Write(mission.id);

      util::WriteBoundedList<uint8_t>(
        sink,
        mission.progress,
        {.name = "LobbyCommandLoginOK.missions.progress"},
        [](SinkStream& nested, const auto& entry)
        {
          nested.Write(entry.id)
            .Write(entry.value);
        });
    });

  stream.Write(command.val6);

  stream.Write(command.ranchAddress)
    .Write(command.ranchPort)
    .Write(command.scramblingConstant);

  stream.Write(command.character)
    .Write(command.horse);

  stream.Write(command.systemContent)
    .Write(command.bitfield);

  // Struct2
  const auto& struct1 = command.val9;
  stream.Write(struct1.val0)
    .Write(struct1.val1)
    .Write(struct1.val2);

  stream.Write(command.val10);

  const auto& managementSkills = command.managementSkills;
  stream.Write(managementSkills.val0)
    .Write(managementSkills.progress)
    .Write(managementSkills.points);

  const auto& skillRanks = command.skillRanks;
  util::WriteBoundedList<uint8_t>(
    stream,
    skillRanks.values,
    {.name = "LobbyCommandLoginOK.skillRanks.values"},
    [](SinkStream& sink, const auto& value)
    {
      sink.Write(value.id)
        .Write(value.rank);
    });

  util::WriteBoundedList<uint8_t>(
    stream,
    command.trainingProgression.mapProggressInfos,
    {.name = "LobbyCommandLoginOK.trainingProgression.mapProggressInfos"},
    [](SinkStream& sink, const auto& value)
    {
      sink.Write(value.mapBlockId)
        .Write(value.gameMode)
        .Write(value.clearStage);
    });

  stream.Write(command.characterCreationDate);

  // Guild
  const auto& struct5 = command.guild;
  stream.Write(struct5.uid)
    .Write(struct5.val1)
    .Write(struct5.val2)
    .Write(struct5.name)
    .Write(struct5.guildRole)
    .Write(struct5.val5)
    .Write(struct5.val6);

  stream.Write(command.val16);

  // Rent
  const auto& struct6 = command.val17;
  stream.Write(struct6.mountUid)
    .Write(struct6.val1)
    .Write(struct6.val2);

  stream.Write(command.val18)
    .Write(command.val19)
    .Write(command.val20);

  // Pet
  stream.Write(command.pet);
}

void LobbyCommandLoginOK::Read(
  LobbyCommandLoginOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLLoginCancel::Write(
  const AcCmdCLLoginCancel& command,
  SinkStream& stream)
{
  stream.Write(static_cast<uint8_t>(command.reason));
}

void AcCmdCLLoginCancel::Read(
  AcCmdCLLoginCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLShowInventory::Write(
  const AcCmdCLShowInventory&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLShowInventory::Read(
  AcCmdCLShowInventory&,
  SourceStream&)
{
  // Empty,
}

void LobbyCommandShowInventoryOK::Write(
  const LobbyCommandShowInventoryOK& command,
  SinkStream& stream)
{
  // ★R74. Два `throw` УДАЛЕНЫ, а их числа переехали в потолки хелпера.
  //
  // Бросок отсюда исполнялся В ПОСТАВЩИКЕ ЗАПИСИ, то есть на потоке
  // `Client::WriteLoop`, где `Server.cpp` ловит его и зовёт `End()` — то есть
  // «список длиннее протокольного предела» означало «выкинуть игрока», и так
  // на каждой попытке. Наблюдаемого поведения замена не меняет вовсе:
  // `HandleShowInventory` уже нарезает ответ ровно по 250 предметов и 10
  // лошадей (`std::views::chunk`), поэтому кламп здесь недостижим — он стоит
  // как последняя, не убивающая соединение линия.
  constexpr std::size_t MaxInventoryItemCount = 250;
  constexpr std::size_t MaxInventoryHorseCount = 10;

  util::WriteBoundedList<uint8_t>(
    stream,
    command.items,
    {.maxCount = MaxInventoryItemCount, .name = "LobbyCommandShowInventoryOK.items"});

  util::WriteBoundedList<uint8_t>(
    stream,
    command.horses,
    {.maxCount = MaxInventoryHorseCount, .name = "LobbyCommandShowInventoryOK.horses"});
}

void LobbyCommandShowInventoryOK::Read(
  LobbyCommandShowInventoryOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void LobbyCommandCreateNicknameNotify::Write(
  const LobbyCommandCreateNicknameNotify&,
  SinkStream&)
{
  // Empty.
}

void LobbyCommandCreateNicknameNotify::Read(
  LobbyCommandCreateNicknameNotify&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLCreateNickname::Write(
  const AcCmdCLCreateNickname&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLCreateNickname::Read(
  AcCmdCLCreateNickname& command,
  SourceStream& stream)
{
  stream.Read(command.nickname)
    .Read(command.character)
    .Read(command.requestedHorseTid);
}

void AcCmdCLCreateNicknameCancel::Write(
  const AcCmdCLCreateNicknameCancel& command,
  SinkStream& stream)
{
  stream.Write(command.error);
}

void AcCmdCLCreateNicknameCancel::Read(
  AcCmdCLCreateNicknameCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void LobbyCommandShowInventoryCancel::Write(
  const LobbyCommandShowInventoryCancel&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void LobbyCommandShowInventoryCancel::Read(
  LobbyCommandShowInventoryCancel&,
  SourceStream&)
{
}

void AcCmdCLRequestLeagueInfo::Write(
  const AcCmdCLRequestLeagueInfo&,
  SinkStream&)
{
}

void AcCmdCLRequestLeagueInfo::Read(
  AcCmdCLRequestLeagueInfo&,
  SourceStream&)
{
}

void AcCmdCLRequestLeagueInfoOK::Write(
  const AcCmdCLRequestLeagueInfoOK& command,
  SinkStream& stream)
{
  stream.Write(command.season)
    .Write(command.league)
    .Write(command.unk2)
    .Write(command.unk3)
    .Write(command.rankingPercentile)
    .Write(command.unk5)
    .Write(command.unk6)
    .Write(command.unk7)
    .Write(command.unk8)
    .Write(command.leagueReward)
    .Write(command.place)
    .Write(command.rank)
    .Write(command.claimedReward)
    .Write(command.unk13);
}

void AcCmdCLRequestLeagueInfoOK::Read(
  AcCmdCLRequestLeagueInfoOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestLeagueInfoCancel::Write(
  const AcCmdCLRequestLeagueInfoCancel&,
  SinkStream&)
{
}

void AcCmdCLRequestLeagueInfoCancel::Read(
  AcCmdCLRequestLeagueInfoCancel&,
  SourceStream&)
{
}

void AcCmdCLAchievementCompleteList::Write(
  const AcCmdCLAchievementCompleteList&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLAchievementCompleteList::Read(
  AcCmdCLAchievementCompleteList& command,
  SourceStream& stream)
{
  stream.Read(command.unk0);
}

void AcCmdCLAchievementCompleteListOK::Write(
  const AcCmdCLAchievementCompleteListOK& command,
  SinkStream& stream)
{
  stream.Write(command.unk0);
  util::WriteBoundedList<uint16_t>(
    stream, command.achievements, {.name = "AcCmdCLAchievementCompleteListOK.achievements"});
}

void AcCmdCLAchievementCompleteListOK::Read(
  AcCmdCLAchievementCompleteListOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterChannel::Write(
  const AcCmdCLEnterChannel&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterChannel::Read(
  AcCmdCLEnterChannel& command,
  SourceStream& stream)
{
  stream.Read(command.channel);
}

void AcCmdCLEnterChannelOK::Write(
  const AcCmdCLEnterChannelOK& command,
  SinkStream& stream)
{
  stream.Write(command.unk0)
    .Write(command.unk1);
}

void AcCmdCLEnterChannelOK::Read(
  AcCmdCLEnterChannelOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterChannelCancel::Write(
  const AcCmdCLEnterChannelCancel&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLEnterChannelCancel::Read(
  AcCmdCLEnterChannelCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLLeaveChannel::Write(
  const AcCmdCLLeaveChannel&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLLeaveChannel::Read(
  AcCmdCLLeaveChannel&,
  SourceStream&)
{
  // Empty.
}

void AcCmdCLLeaveChannelOK::Write(
  const AcCmdCLLeaveChannelOK&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLLeaveChannelOK::Read(
  AcCmdCLLeaveChannelOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRoomList::Write(
  const AcCmdCLRoomList&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRoomList::Read(
  AcCmdCLRoomList& command,
  SourceStream& stream)
{
  stream.Read(command.page)
    .Read(command.gameMode)
    .Read(command.teamMode);
}

void LobbyCommandRoomListOK::Room::Write(
  const Room& value,
  SinkStream& stream)
{
  stream.Write(value.uid)
    .Write(value.name)
    .Write(value.playerCount)
    .Write(value.maxPlayerCount)
    .Write(value.isLocked)
    .Write(value.unk0)
    .Write(value.unk1)
    .Write(value.map)
    .Write(value.state)
    .Write(value.unk2)
    .Write(value.unk3)
    .Write(value.skillBracket)
    .Write(value.unk4);
}

void LobbyCommandRoomListOK::Room::Read(
  Room&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void LobbyCommandRoomListOK::Write(
  const LobbyCommandRoomListOK& command,
  SinkStream& stream)
{
  stream.Write(command.page)
    .Write(command.gameMode)
    .Write(command.teamMode);
  util::WriteBoundedList<uint8_t>(
    stream, command.rooms, {.name = "LobbyCommandRoomListOK.rooms"});
  stream.Write(command.unk3.unk0)
    .Write(command.unk3.unk1)
    .Write(command.unk3.unk2);
}

void LobbyCommandRoomListOK::Read(
  LobbyCommandRoomListOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLMakeRoom::Write(
  const AcCmdCLMakeRoom&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLMakeRoom::Read(
  AcCmdCLMakeRoom& command,
  SourceStream& stream)
{
  stream.Read(command.name)
    .Read(command.password)
    .Read(command.playerCount)
    .Read(command.gameMode)
    .Read(command.teamMode)
    .Read(command.missionId)
    .Read(command.unk3)
    .Read(command.bitset)
    .Read(command.unk4);
}

void AcCmdCLMakeRoomOK::Write(
  const AcCmdCLMakeRoomOK& command,
  SinkStream& stream)
{
  stream.Write(command.roomUid)
    .Write(command.oneTimePassword)
    .Write(htonl(command.raceServerAddress))
    .Write(command.raceServerPort)
    .Write(command.unk2);
}

void AcCmdCLMakeRoomOK::Read(
  AcCmdCLMakeRoomOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLMakeRoomCancel::Write(
  const AcCmdCLMakeRoomCancel& command,
  SinkStream& stream)
{
  stream.Write(command.unk0);
}

void AcCmdCLMakeRoomCancel::Read(
  AcCmdCLMakeRoomCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterRoom::Write(
  const AcCmdCLEnterRoom&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterRoom::Read(
  AcCmdCLEnterRoom& command,
  SourceStream& stream)
{
  stream.Read(command.roomUid)
    .Read(command.password)
    .Read(command.enterRoomType);
}

void AcCmdCLEnterRoomOK::Write(
  const AcCmdCLEnterRoomOK& command,
  SinkStream& stream)
{
  stream.Write(command.roomUid)
    .Write(command.oneTimePassword)
    .Write(htonl(command.raceServerAddress))
    .Write(command.raceServerPort)
    .Write(command.member6);
}

void AcCmdCLEnterRoomOK::Read(
  AcCmdCLEnterRoomOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterRoomCancel::Write(
  const AcCmdCLEnterRoomCancel& command,
  SinkStream& stream)
{
  stream.Write(command.status);
}

void AcCmdCLEnterRoomCancel::Read(
  AcCmdCLEnterRoomCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLLeaveRoom::Write(
  const AcCmdCLLeaveRoom&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLLeaveRoom::Read(
  AcCmdCLLeaveRoom&,
  SourceStream&)
{
  // Empty.
}

void AcCmdCLLeaveRoomOK::Write(
  const AcCmdCLLeaveRoomOK&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLLeaveRoomOK::Read(
  AcCmdCLLeaveRoomOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestQuestList::Write(
  const AcCmdCLRequestQuestList&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestQuestList::Read(
  AcCmdCLRequestQuestList& command,
  SourceStream& stream)
{
  stream.Read(command.unk0);
}

void AcCmdCLRequestQuestListOK::Write(
  const AcCmdCLRequestQuestListOK& command,
  SinkStream& stream)
{
  stream.Write(command.unk0);
  util::WriteBoundedList<uint16_t>(
    stream, command.quests, {.name = "AcCmdCLRequestQuestListOK.quests"});
}

void AcCmdCLRequestQuestListOK::Read(
  AcCmdCLRequestQuestListOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestDailyQuestList::Write(
  const AcCmdCLRequestDailyQuestList&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestDailyQuestList::Read(
  AcCmdCLRequestDailyQuestList& command,
  SourceStream& stream)
{
  stream.Read(command.val0);
}

void AcCmdCLRequestDailyQuestListOK::Write(
  const AcCmdCLRequestDailyQuestListOK& command,
  SinkStream& stream)
{
  stream.Write(command.characterUid);
  util::WriteBoundedList<uint16_t>(
    stream, command.unk, {.name = "AcCmdCLRequestDailyQuestListOK.unk"});

  util::WriteBoundedList<uint16_t>(
    stream, command.dailyQuests, {.name = "AcCmdCLRequestDailyQuestListOK.dailyQuests"});
}

void AcCmdCLRequestDailyQuestListOK::Read(
  AcCmdCLRequestDailyQuestListOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterRanch::Write(
  const AcCmdCLEnterRanch&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterRanch::Read(
  AcCmdCLEnterRanch& command,
  SourceStream& stream)
{
  stream.Read(command.rancherUid)
    .Read(command.unk1)
    .Read(command.unk2);
}

void AcCmdCLEnterRanchOK::Write(
  const AcCmdCLEnterRanchOK& command,
  SinkStream& stream)
{
  stream.Write(command.rancherUid)
    .Write(command.otp)
    .Write(htonl(command.ranchAddress))
    .Write(command.ranchPort);
}

void AcCmdCLEnterRanchOK::Read(
  AcCmdCLEnterRanchOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLEnterRanchCancel::Write(
  const AcCmdCLEnterRanchCancel& command,
  SinkStream& stream)
{
  stream.Write(command.reason);
}

void AcCmdCLEnterRanchCancel::Read(
  AcCmdCLEnterRanchCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLGetMessengerInfo::Write(
  const AcCmdCLGetMessengerInfo&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLGetMessengerInfo::Read(
  AcCmdCLGetMessengerInfo&,
  SourceStream&)
{
  // Empty.
}

void AcCmdCLGetMessengerInfoOK::Write(
  const AcCmdCLGetMessengerInfoOK& command,
  SinkStream& stream)
{
  stream.Write(command.code)
    .Write(command.ip)
    .Write(command.port);
}

void AcCmdCLGetMessengerInfoOK::Read(
  AcCmdCLGetMessengerInfoOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLGetMessengerInfoCancel::Write(
  const AcCmdCLGetMessengerInfoCancel&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLGetMessengerInfoCancel::Read(
  AcCmdCLGetMessengerInfoCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLCheckWaitingSeqno::Write(
  const AcCmdCLCheckWaitingSeqno&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLCheckWaitingSeqno::Read(
  AcCmdCLCheckWaitingSeqno& command,
  SourceStream& stream)
{
  stream.Read(command.uid);
}

void AcCmdCLCheckWaitingSeqnoOK::Write(
  const AcCmdCLCheckWaitingSeqnoOK& command,
  SinkStream& stream)
{
  stream.Write(command.time)
    .Write(command.position);
}

void AcCmdCLCheckWaitingSeqnoOK::Read(
  AcCmdCLCheckWaitingSeqnoOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestSpecialEventList::Write(
  const AcCmdCLRequestSpecialEventList&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestSpecialEventList::Read(
  AcCmdCLRequestSpecialEventList& command,
  SourceStream& stream)
{
  stream.Read(command.unk0);
}

void AcCmdCLRequestSpecialEventListOK::Write(
  const AcCmdCLRequestSpecialEventListOK& command,
  SinkStream& stream)
{
  stream.Write(command.unk0);

  util::WriteBoundedList<uint16_t>(
    stream, command.quests, {.name = "AcCmdCLRequestSpecialEventListOK.quests"});

  util::WriteBoundedList<uint16_t>(
    stream,
    command.events,
    {.name = "AcCmdCLRequestSpecialEventListOK.events"},
    [](SinkStream& sink, const auto& event)
    {
      sink.Write(event.unk0)
        .Write(event.unk1);
    });
}

void AcCmdCLRequestSpecialEventListOK::Read(
  AcCmdCLRequestSpecialEventListOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLHeartbeat::Write(
  const AcCmdCLHeartbeat&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLHeartbeat::Read(
  AcCmdCLHeartbeat&,
  SourceStream&)
{
  // Empty.
}

void AcCmdCLGoodsShopList::Write(
  const AcCmdCLGoodsShopList&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLGoodsShopList::Read(
  AcCmdCLGoodsShopList& command,
  SourceStream& stream)
{
  std::array<uint32_t, 3> cachedShopTimestamp;
  stream.Read(cachedShopTimestamp.data(), 12);

  command.cachedShopTimestamp = util::AliciaShopTimeToTimePoint(cachedShopTimestamp);
}

void AcCmdCLGoodsShopListOK::Write(
  const AcCmdCLGoodsShopListOK& command,
  SinkStream& stream)
{
  const std::array<uint32_t, 3>& timePoint = util::TimePointToAliciaShopTime(command.shopTimestamp);
  stream.Write(timePoint.data(), 12);
}

void AcCmdCLGoodsShopListOK::Read(
  AcCmdCLGoodsShopListOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLGoodsShopListCancel::Write(
  const AcCmdCLGoodsShopListCancel&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLGoodsShopListCancel::Read(
  AcCmdCLGoodsShopListCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdLCGoodsShopListData::Write(
  const AcCmdLCGoodsShopListData& command,
  SinkStream& stream)
{
  const std::array<uint32_t, 3>& timePoint = util::TimePointToAliciaShopTime(command.timestamp);
  stream.Write(timePoint.data(), 12);

  stream.Write(command.index)
    .Write(command.count);

  util::WriteBoundedBytes<uint32_t>(
    stream, std::span{command.data}, {.name = "AcCmdLCGoodsShopListData.data"});
}

void AcCmdLCGoodsShopListData::Read(
  AcCmdLCGoodsShopListData&,
  SourceStream&)
{
}

void AcCmdCLInquiryTreecash::Write(
  const AcCmdCLInquiryTreecash&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLInquiryTreecash::Read(
  AcCmdCLInquiryTreecash&,
  SourceStream&)
{
  // Empty.
}

void LobbyCommandInquiryTreecashOK::Write(
  const LobbyCommandInquiryTreecashOK& command,
  SinkStream& stream)
{
  stream.Write(command.cash);
}

void LobbyCommandInquiryTreecashOK::Read(
  LobbyCommandInquiryTreecashOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void LobbyCommandInquiryTreecashCancel::Write(
  const LobbyCommandInquiryTreecashCancel&,
  SinkStream&)
{
  // Empty.
}

void LobbyCommandInquiryTreecashCancel::Read(
  LobbyCommandInquiryTreecashCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdClientNotify::Write(
  const AcCmdClientNotify&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdClientNotify::Read(
  AcCmdClientNotify& command,
  SourceStream& stream)
{
  stream.Read(command.val0).Read(command.val1);
}

void LobbyCommandGuildPartyList::Write(
  const LobbyCommandGuildPartyList&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void LobbyCommandGuildPartyList::Read(
  LobbyCommandGuildPartyList&,
  SourceStream&)
{
  // Empty.
}

void LobbyCommandGuildPartyListOK::Write(
  const LobbyCommandGuildPartyListOK& command,
  SinkStream& stream)
{
  // `assert` ОСТАЁТСЯ: сообщение не реализовано для непустого списка, и кламп
  // это не заменяет — он объявит меньше, но тела всё равно не будет.
  assert(command.members.empty());
  // todo: Write members
  util::WriteBoundedList<uint8_t>(
    stream,
    command.members,
    {.maxCount = 0, .name = "LobbyCommandGuildPartyListOK.members"},
    [](SinkStream&, const auto&) {});
}

void LobbyCommandGuildPartyListOK::Read(
  LobbyCommandGuildPartyListOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRanchRandomly::Write(
  const AcCmdCLEnterRanchRandomly&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRanchRandomly::Read(
  AcCmdCLEnterRanchRandomly&,
  SourceStream&)
{
  // Empty.
}

void AcCmdCLFeatureCommand::Write(
  const AcCmdCLFeatureCommand&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLFeatureCommand::Read(
  AcCmdCLFeatureCommand& command,
  SourceStream& stream)
{
  stream.Read(command.command);
}

void AcCmdCLRequestFestivalResult::Write(
  const AcCmdCLRequestFestivalResult&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestFestivalResult::Read(
  AcCmdCLRequestFestivalResult& command,
  SourceStream& stream)
{
  stream.Read(command.member1);
}

void AcCmdCLRequestFestivalResultOK::Write(
  const AcCmdCLRequestFestivalResultOK&,
  SinkStream&)
{
  throw std::runtime_error("Command needs to be discovered");
}

void AcCmdCLRequestFestivalResultOK::Read(
  AcCmdCLRequestFestivalResultOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestPersonalInfo::Write(
  const AcCmdCLRequestPersonalInfo&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestPersonalInfo::Read(
  AcCmdCLRequestPersonalInfo& command,
  SourceStream& stream)
{
  stream.Read(command.characterUid)
    .Read(command.type);
}

void AcCmdLCPersonalInfo::Basic::Write(
  const Basic& command,
  SinkStream& stream)
{
  stream.Write(command.distanceTravelled)
    .Write(command.topSpeed)
    .Write(command.longestGlidingDistance)
    .Write(command.jumpSuccessRate)
    .Write(command.perfectJumpSuccessRate)
    .Write(command.speedSingleWinCombo)
    .Write(command.speedTeamWinCombo)
    .Write(command.magicSingleWinCombo)
    .Write(command.magicTeamWinCombo)
    .Write(command.averageRank)
    .Write(command.completionRate)
    .Write(command.member12)
    .Write(command.highestCarnivalPrize)
    .Write(command.member14)
    .Write(command.member15)
    .Write(command.member16)
    .Write(command.introduction)
    .Write(command.level)
    .Write(command.levelProgress)
    .Write(command.member20)
    .Write(command.perfectBoostCombo)
    .Write(command.perfectJumpCombo)
    .Write(command.magicDefenseCombo)
    .Write(command.member24)
    .Write(command.member25)
    .Write(command.member26)
    .Write(command.guildName)
    .Write(command.member28)
    .Write(command.member29);
}

void AcCmdLCPersonalInfo::Basic::Read(
  Basic&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCPersonalInfo::CourseInformation::Write(const CourseInformation& command, SinkStream& stream)
{
  stream.Write(command.totalGames)
    .Write(command.totalSpeedGames)
    .Write(command.totalMagicGames);

  util::WriteBoundedList<uint8_t>(
    stream,
    command.courses,
    {.name = "AcCmdLCPersonalInfo.CourseInformation.courses"},
    [](SinkStream& sink, const auto& entry)
    {
      sink.Write(entry.courseId)
        .Write(entry.timesRaced)
        .Write(entry.recordTime);

      // Фиксированный std::array без счётчика на проводе — не список.
      for (const auto& byte : entry.member4)
      {
        sink.Write(byte);
      }
    });
}

void AcCmdLCPersonalInfo::CourseInformation::Read(
  CourseInformation&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCPersonalInfo::Eight::Write(
  const Eight& command,
  SinkStream& stream)
{
  util::WriteBoundedList<uint8_t>(
    stream,
    command.member1,
    {.name = "AcCmdLCPersonalInfo.Eight.member1"},
    [](SinkStream& sink, const auto& entry)
    {
      sink.Write(entry.member1)
        .Write(entry.member2);
    });
}

void AcCmdLCPersonalInfo::Eight::Read(
  Eight&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCPersonalInfo::Write(
  const AcCmdLCPersonalInfo& command,
  SinkStream& stream)
{
  stream.Write(command.characterUid)
    .Write(command.type);

  switch (command.type)
  {
    case AcCmdCLRequestPersonalInfo::Type::Basic:
      {
        stream.Write(command.basic);
        break;
      }
    case AcCmdCLRequestPersonalInfo::Type::Courses:
      {
        stream.Write(command.courseInformation);
        break;
      }
    case AcCmdCLRequestPersonalInfo::Type::Eight:
      {
        stream.Write(command.eight);
        break;
      }
  }
}

void AcCmdLCPersonalInfo::Read(
  AcCmdLCPersonalInfo&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLSetIntroduction::Write(
  const AcCmdCLSetIntroduction&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLSetIntroduction::Read(
  AcCmdCLSetIntroduction& command,
  SourceStream& stream)
{
  stream.Read(command.introduction);
}

void AcCmdCLUpdateSystemContent::Write(
  const AcCmdCLUpdateSystemContent&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLUpdateSystemContent::Read(
  AcCmdCLUpdateSystemContent& command,
  SourceStream& stream)
{
  stream.Read(command.member1)
    .Read(command.key)
    .Read(command.value);
}

void AcCmdLCUpdateSystemContent::Write(
  const AcCmdLCUpdateSystemContent& command,
  SinkStream& stream)
{
  stream.Write(command.systemContent);
}

void AcCmdLCUpdateSystemContent::Read(
  AcCmdLCUpdateSystemContent&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRoomQuickStop::Write(
  const AcCmdCLEnterRoomQuickStop&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRoomQuickStop::Read(
  AcCmdCLEnterRoomQuickStop&,
  SourceStream&)
{
  // Empty.
}

void AcCmdCLEnterRoomQuickStopOK::Write(
  const AcCmdCLEnterRoomQuickStopOK&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLEnterRoomQuickStopOK::Read(
  AcCmdCLEnterRoomQuickStopOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRoomQuickStopCancel::Write(
  const AcCmdCLEnterRoomQuickStopCancel&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLEnterRoomQuickStopCancel::Read(
  AcCmdCLEnterRoomQuickStopCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestFestivalPrize::Write(
  const AcCmdCLRequestFestivalPrize&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestFestivalPrize::Read(
  AcCmdCLRequestFestivalPrize& command,
  SourceStream& stream)
{
  stream.Read(command.member1);
}

void AcCmdCLRequestFestivalPrizeOK::Write(
  const AcCmdCLRequestFestivalPrizeOK&,
  SinkStream&)
{
  // todo: discover
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestFestivalPrizeOK::Read(
  AcCmdCLRequestFestivalPrizeOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestFestivalPrizeCancel::Write(
  const AcCmdCLRequestFestivalPrizeCancel&,
  SinkStream&)
{
  // todo: discover
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestFestivalPrizeCancel::Read(
  AcCmdCLRequestFestivalPrizeCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLQueryServerTime::Write(
  const AcCmdCLQueryServerTime&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLQueryServerTime::Read(
  AcCmdCLQueryServerTime&,
  SourceStream&)
{
  // Empty.
}

void AcCmdCLQueryServerTimeOK::Write(
  const AcCmdCLQueryServerTimeOK& command,
  SinkStream& stream)
{
  stream.Write(command.lobbyTime.dwLowDateTime)
    .Write(command.lobbyTime.dwHighDateTime);
}

void AcCmdCLQueryServerTimeOK::Read(
  AcCmdCLQueryServerTimeOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLChangeRanchOption::Read(
  AcCmdCLChangeRanchOption& command,
  SourceStream& stream)
{
  stream.Read(command.unk0)
    .Read(command.unk1)
    .Read(command.unk2);
}

void AcCmdCLChangeRanchOptionOK::Write(
  const AcCmdCLChangeRanchOptionOK& command,
  SinkStream& stream)
{
  stream.Write(command.unk0)
    .Write(command.unk1)
    .Write(command.unk2);
}

void AcCmdLCOpKick::Write(
  const AcCmdLCOpKick&,
  SinkStream&)
{
  // Empty.
}

void AcCmdLCOpKick::Read(
  AcCmdLCOpKick&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCOpMute::Write(
  const AcCmdLCOpMute& command,
  SinkStream& stream)
{
  stream.Write(command.duration);
}

void AcCmdLCOpMute::Read(
  AcCmdLCOpMute&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCNotice::Write(
  const AcCmdLCNotice& command,
  SinkStream& stream)
{
  stream.Write(command.notice);
}

void AcCmdLCNotice::Read(
  AcCmdLCNotice&,
  SourceStream&)
{
    throw std::runtime_error("Not implemented");
}

void AcCmdCLUpdateUserSettings::Write(
  const AcCmdCLUpdateUserSettings&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLUpdateUserSettings::Read(
  AcCmdCLUpdateUserSettings& command,
  SourceStream& stream)
{
  stream.Read(command.settings);
}

void AcCmdCLUpdateUserSettingsOK::Write(
  const AcCmdCLUpdateUserSettingsOK&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLUpdateUserSettingsOK::Read(
  AcCmdCLUpdateUserSettingsOK&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRoomQuick::Write(
  const AcCmdCLEnterRoomQuick&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRoomQuick::Read(
  AcCmdCLEnterRoomQuick& command,
  SourceStream& stream)
{
  stream.Read(command.gameMode)
    .Read(command.teamMode);
}

void AcCmdCLEnterRoomQuickCancel::Write(
  const AcCmdCLEnterRoomQuickCancel&,
  SinkStream&)
{
  // Empty.
}

void AcCmdCLEnterRoomQuickCancel::Read(
  AcCmdCLEnterRoomQuickCancel&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLRequestMountInfo::Write(
  const AcCmdCLRequestMountInfo&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdCLRequestMountInfo::Read(
  AcCmdCLRequestMountInfo& command,
  SourceStream& stream)
{
  stream.Read(command.characterUid);
}

void AcCmdCLRequestMountInfoOK::Write(
  const AcCmdCLRequestMountInfoOK& command,
  SinkStream& stream)
{
  stream.Write(command.characterUid);
  util::WriteBoundedList<uint8_t>(
    stream,
    command.mountInfos,
    {.name = "AcCmdCLRequestMountInfoOK.mountInfos"},
    [](SinkStream& sink, const auto& mountInfo)
    {
      sink.Write(mountInfo.horseUid)
        .Write(mountInfo.boostsInARow)
        .Write(mountInfo.winsSpeedSingle)
        .Write(mountInfo.winsSpeedTeam)
        .Write(mountInfo.winsMagicSingle)
        .Write(mountInfo.winsMagicTeam)
        .Write(mountInfo.totalDistance)
        .Write(mountInfo.topSpeed)
        .Write(mountInfo.longestGlideDistance)
        .Write(mountInfo.participated)
        .Write(mountInfo.cumulativePrize)
        .Write(mountInfo.biggestPrize);
    });
}

void AcCmdLCSkillCardPresetList::Read(
  AcCmdLCSkillCardPresetList&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented.");
}

void AcCmdLCSkillCardPresetList::Write(
  const AcCmdLCSkillCardPresetList& command,
  SinkStream& stream)
{
  stream.Write(command.speedActiveSetId)
    .Write(command.magicActiveSetId);

  util::WriteBoundedList<uint8_t>(
    stream, command.skillSets, {.name = "AcCmdLCSkillCardPresetList.skillSets"});
}

void AcCmdLCInviteGuildJoin::Read(
  AcCmdLCInviteGuildJoin&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCInviteGuildJoin::Write(
  const AcCmdLCInviteGuildJoin& command,
  SinkStream& stream)
{
  stream.Write(command.characterUid)
    .Write(command.inviterCharacterUid)
    .Write(command.inviterCharacterName)
    .Write(command.unk3)
    .Write(command.guild);
}

void AcCmdLCInviteGuildJoinCancel::Read(
  AcCmdLCInviteGuildJoinCancel& command,
  SourceStream& stream)
{
  stream.Read(command.characterUid)
    .Read(command.inviterCharacterUid)
    .Read(command.inviterCharacterName)
    .Read(command.unk3)
    .Read(command.guild)
    .Read(command.error);
}

void AcCmdLCInviteGuildJoinCancel::Write(
  const AcCmdLCInviteGuildJoinCancel&,
  SinkStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCInviteGuildJoinOK::Read(
  AcCmdLCInviteGuildJoinOK& command,
  SourceStream& stream)
{
  stream.Read(command.characterUid)
    .Read(command.inviterCharacterUid)
    .Read(command.inviterCharacterName)
    .Read(command.unk3)
    .Read(command.guild);
}

void AcCmdLCInviteGuildJoinOK::Write(
  const AcCmdLCInviteGuildJoinOK&,
  SinkStream&)
{
  // TODO: Return this back to the client to confirm join?
  throw std::runtime_error("Not implemented");
}

void AcCmdLCAchievementRewardNotify::Read(
  AcCmdLCAchievementRewardNotify&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdLCAchievementRewardNotify::Write(
  const AcCmdLCAchievementRewardNotify&,
  SinkStream&)
{
  // Empty
}

void AcCmdCLEnterRoomQuickSuccess::Read(
  AcCmdCLEnterRoomQuickSuccess&,
  SourceStream&)
{
  throw std::runtime_error("Not implemented");
}

void AcCmdCLEnterRoomQuickSuccess::Write(
  const AcCmdCLEnterRoomQuickSuccess& command,
  SinkStream& stream)
{
  stream.Write(command.result);
}

} // namespace server::protocol
