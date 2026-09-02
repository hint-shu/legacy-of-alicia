/**
 * Alicia Server - dedicated server software
 * Copyright (C) 2025-2026 Story Of Alicia
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

#include "server/lobby/LobbyNetworkHandler.hpp"
#include "libserver/util/QuietLog.hpp"

#include "libserver/util/Cleanup.hpp"
#include "server/ServerInstance.hpp"

#include <libserver/data/helper/ProtocolHelper.hpp>

#include <boost/container_hash/hash.hpp>
#include <spdlog/spdlog.h>
#include <zlib.h>

#include <random>

namespace server
{

namespace
{

//! A random device for random number generation.
std::random_device rd;

} // anon namespace

// LOA-fix (R72-1, round72, backlog #129-S1): РЕШЕНИЕ ОБ АУТЕНТИФИКАЦИИ ЖИВЁТ
// НА РЕГИСТРАЦИИ, А НЕ В ТЕЛЕ ХЕНДЛЕРА.
//
// Лобби отвечало ЛЮБОМУ сокету, завершившему TCP-рукопожатие: перепись по
// дереву дала 13 командных хендлеров из 36, которые не смотрели на контекст
// клиента вообще. Среди них `HandleRequestPersonalInfo` (имя гильдии, текст
// представления, уровень и опыт по ПРОИЗВОЛЬНОМУ characterUid, перечислимому
// инкрементом), подделка отказа от приглашения в гильдию от имени любого
// персонажа и два пред-логинных лог-флуда.
//
// ★КОРЕНЬ — НЕ ЗАБЫТЫЙ ВЫЗОВ, А ТО, ЧТО РЕШЕНИЕ ПРИНИМАЛ КАЖДЫЙ ХЕНДЛЕР САМ.
// Список мест обязательно отстанет от кода, поэтому решение перенесено туда,
// где оно принимается ровно один раз на команду и физически не может быть
// пропущено: в регистрацию.
//
// Арифметика этого конструктора: 36 = 33 аутентифицированных + 3
// пред-логинных. Пред-логинных ровно три, каждая с обоснованием рядом с
// вызовом, и весь их список виден отсюда, не собирается по файлу.
//
// ★Новый хендлер, зарегистрированный сырым RegisterCommandHandler, валит гард
// tools/check_lobby_auth_gate.sh: сырых регистраций в этом файле должно быть
// ноль, а множество пред-логинных команд сверяется ДОСЛОВНО, потому что числа
// сходятся и у неверного множества.
LobbyNetworkHandler::LobbyNetworkHandler(
  ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
  , _commandServer(*this)
{
  // ПРЕД-ЛОГИННАЯ #1 из ТРЁХ: это и есть сам логин. Гейт на ней означал бы,
  // что залогиниться не может никто.
  RegisterPreAuthHandler<protocol::AcCmdCLLogin>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleLogin(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRoomList>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRoomList(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLHeartbeat>(
    [this](const ClientId clientId, [[maybe_unused]] const auto& command)
    {
      HandleHeartbeat(clientId);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLMakeRoom>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleMakeRoom(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLEnterRoom>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleEnterRoom(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLLeaveRoom>(
    [this](const ClientId clientId, [[maybe_unused]] const auto& command)
    {
      HandleLeaveRoom(clientId);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLEnterChannel>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleEnterChannel(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLLeaveChannel>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleLeaveChannel(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLCreateNickname>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleCreateNickname(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLShowInventory>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleShowInventory(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLUpdateUserSettings>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleUpdateUserSettings(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLEnterRoomQuick>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleEnterRoomQuick(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLGoodsShopList>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleGoodsShopList(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLAchievementCompleteList>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleAchievementCompleteList(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestPersonalInfo>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestPersonalInfo(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLEnterRanch>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleEnterRanch(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLEnterRanchRandomly>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleEnterRanchRandomly(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLFeatureCommand>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleFeatureCommand(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestFestivalResult>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestFestivalResult(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLSetIntroduction>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleSetIntroduction(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLGetMessengerInfo>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleGetMessengerInfo(clientId, command);
    });

  // ПРЕД-ЛОГИННАЯ #2 из ТРЁХ: опрос ОЧЕРЕДИ логина. Ответ строится
  // `LobbyDirector::GetClientQueuePosition(clientId)`, то есть клиент по
  // определению ещё не аутентифицирован — гейт сделал бы очередь невидимой.
  RegisterPreAuthHandler<protocol::AcCmdCLCheckWaitingSeqno>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleCheckWaitingSeqno(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLUpdateSystemContent>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleUpdateSystemContent(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLEnterRoomQuickStop>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleEnterRoomQuickStop(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestFestivalPrize>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestFestivalPrize(clientId, command);
    });

  // ПРЕД-ЛОГИННАЯ #3 из ТРЁХ: отдаёт ТОЛЬКО системные часы, нулевое
  // разглашение. Оставлена пред-логинной СОЗНАТЕЛЬНО — как якорь живости
  // сокета для стенда и как страховка от регрессии, если клиент 2013 года
  // спрашивает время до логина. Единственная уступка риску совместимости:
  // не меняем успешный путь ради пути отказа.
  RegisterPreAuthHandler<protocol::AcCmdCLQueryServerTime>(
    [this](const ClientId clientId, [[maybe_unused]] const auto& command)
    {
      HandleQueryServerTime(clientId);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestMountInfo>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestMountInfo(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLInquiryTreecash>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleInquiryTreecash(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdLCInviteGuildJoinOK>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleAcceptInviteToGuild(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdLCInviteGuildJoinCancel>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleDeclineInviteToGuild(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdClientNotify>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleClientNotify(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLChangeRanchOption>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleChangeRanchOption(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestDailyQuestList>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestDailyQuestList(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestLeagueInfo>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestLeagueInfo(clientId, command);
    });

  // todo: AcCmdCLMakeGuildParty, AcCmdCLGuildPartyList, AcCmdCLEnterGuildParty,
  //       AcCmdCLLeaveGuildParty, AcCmdCLStartGuildPartyMatch, AcCmdCLStopGuildPartyMatch

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestQuestList>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestQuestList(clientId, command);
    });

  RegisterAuthenticatedHandler<protocol::AcCmdCLRequestSpecialEventList>(
    [this](const ClientId clientId, const auto& command)
    {
      HandleRequestSpecialEventList(clientId, command);
    });
}

void LobbyNetworkHandler::Initialize()
{
  const auto& lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();

  server::util::QuietLogDebug(
    "Lobby is advertising ranch server on {}:{}",
    lobbyConfig.advertisement.ranch.address.to_string(),
    lobbyConfig.advertisement.ranch.port);
  server::util::QuietLogDebug(
    "Lobby is advertising race server on {}:{}",
    lobbyConfig.advertisement.race.address.to_string(),
    lobbyConfig.advertisement.race.port);

  if (_serverInstance.GetMessengerDirector().GetConfig().enabled)
  {
    server::util::QuietLogDebug(
      "Lobby is advertising messenger server on {}:{}",
      lobbyConfig.advertisement.messenger.address.to_string(),
      lobbyConfig.advertisement.messenger.port);

    if (_serverInstance.GetAllChatDirector().GetConfig().enabled)
    {
      server::util::QuietLogDebug(
        "Lobby is advertising all chat server on {}:{}",
        lobbyConfig.advertisement.allChat.address.to_string(),
        lobbyConfig.advertisement.allChat.port);
    }

    if (_serverInstance.GetPrivateChatDirector().GetConfig().enabled)
    {
      server::util::QuietLogDebug(
        "Lobby is advertising private chat server on {}:{}",
        lobbyConfig.advertisement.privateChat.address.to_string(),
        lobbyConfig.advertisement.privateChat.port);
    }
  }

  server::util::QuietLogDebug(
    "Lobby server listening on {}:{}",
    lobbyConfig.listen.address.to_string(),
    lobbyConfig.listen.port);

  _commandServer.BeginHost(lobbyConfig.listen.address, lobbyConfig.listen.port);
}

void LobbyNetworkHandler::Terminate()
{
  _commandServer.EndHost();
}

void LobbyNetworkHandler::AcceptLogin(
  ClientId clientId,
  const bool sendToCharacterCreator)
{
  try
  {
    // LOA-fix (R64-3, round64, backlog #215): мутация под замком, вызовы наружу —
    // после него. ★Именно здесь ревью R59 поймало самозахват: `AcceptLogin`
    // возвращается в тот же директор, и замок, дотянутый до `SendLoginOK`
    // ниже, положил бы вход в игру целиком (0 успешных заходов из 24).
    // ★Семантику «клиента нет → бросок» сохраняем: прежний `GetClientContext`
    // бросал, снаружи стоит `catch`. `MutateClientContext` возвращает false,
    // поэтому бросаем сами — иначе отсутствие клиента прошло бы молча.
    if (not MutateClientContext(
          clientId,
          [](ClientContext& clientContext)
          {
            clientContext.isAuthenticated = true;
          }))
    {
      throw std::runtime_error("Lobby client is not available");
    }

    if (sendToCharacterCreator)
    {
      // Reset the waiting sequence number so the client does not soft lock.
      // SendWaitingSeqno(clientId, 0);
      SendCreateNicknameNotify(clientId);
    }
    else
    {
      SendLoginOK(clientId);
    }
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
  }
}

void LobbyNetworkHandler::RejectLogin(
  ClientId clientId,
  const protocol::AcCmdCLLoginCancel::Reason reason)
{
  try
  {
    // LOA-fix (R64-3, round64, backlog #215): контекст здесь не используется —
    // вызов нужен ТОЛЬКО ради проверки существования клиента (он бросает, если
    // клиента нет, и снаружи стоит `catch`). Метод теперь возвращает копию, так
    // что и проверка, и отсутствие ссылки в карту получаются даром.
    [[maybe_unused]] const auto clientContext = GetClientContext(clientId, false);

    SendLoginCancel(clientId, reason);
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
  }
}

void LobbyNetworkHandler::SendCharacterGuildInvitation(
  const data::Uid inviteeUid,
  const data::Uid guildUid,
  const data::Uid inviterUid)
{
  const ClientId inviteeClientId = GetClientIdByCharacterUid(inviteeUid);

  std::string inviterName;
  _serverInstance.GetDataDirector().GetCharacter(inviterUid).Immutable(
    [&inviterName](const data::Character& character)
    {
      inviterName = character.name();
    });

  std::string guildName, guildDescription;
  _serverInstance.GetDataDirector().GetGuild(guildUid).Immutable(
    [&guildName, &guildDescription](const data::Guild& guild)
    {
      guildName = guild.name();
      guildDescription = guild.description();
    });

  protocol::AcCmdLCInviteGuildJoin command{
    .characterUid = inviteeUid,
    .inviterCharacterUid = inviterUid, // clientContext.characterUid?
    .inviterCharacterName = inviterName,
    .unk3 = guildDescription,
    .guild = {
      .uid = guildUid,
      .val1 = 1,
      .val2 = 2,
      .name = guildName,
      .guildRole = protocol::GuildRole::Member,
      .val5 = 5,
      .val6 = 6}};

  _commandServer.QueueCommand<decltype(command)>(
    inviteeClientId,
    [command]()
    {
      return command;
    });
}

void LobbyNetworkHandler::SetCharacterVisitPreference(
  const data::Uid characterUid,
  const data::Uid rancherUid)
{
  try
  {
    const auto clientId = GetClientIdByCharacterUid(characterUid);
    // LOA-fix (R64-3, round64, backlog #215): запись предпочтения визита — под
    // замком. ★Этот метод зовёт ЛОББИ-ДИРЕКТОР, то есть чужой поток: без замка
    // он писал в структуру, которую сетевой поток мог в этот момент
    // перестраивать вставкой.
    if (not MutateClientContext(
          clientId,
          [rancherUid](ClientContext& clientContext)
          {
            clientContext.rancherVisitPreference = rancherUid;
          }))
    {
      throw std::runtime_error("Lobby client is not available");
    }
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
  }
}

void LobbyNetworkHandler::DisconnectCharacter(
  const data::Uid characterUid)
{
  try
  {
    const auto clientId = GetClientIdByCharacterUid(characterUid);
    _commandServer.DisconnectClient(clientId);
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
  }
}

void LobbyNetworkHandler::MuteCharacter(
  const data::Uid characterUid,
  const data::Clock::time_point expiration)
{
  try
  {
    const auto clientId = GetClientIdByCharacterUid(characterUid);

    protocol::AcCmdLCOpMute mute{
      .duration = util::TimePointToAliciaTime(expiration)};
    _commandServer.QueueCommand<decltype(mute)>(
      clientId,
      [mute]()
      {
        return mute;
      });
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
  }
}

void LobbyNetworkHandler::NotifyCharacter(
  const data::Uid characterUid,
  const std::string& message)
{
  try
  {
    const auto clientId = GetClientIdByCharacterUid(characterUid);

    protocol::AcCmdLCNotice notice{
      .notice = message};
    _commandServer.QueueCommand<decltype(notice)>(
      clientId,
      [notice]()
      {
        return notice;
      });
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
  }
}

void LobbyNetworkHandler::NotifyAchievementReward(
  const data::Uid characterUid)
{
  try
  {
    const auto clientId = GetClientIdByCharacterUid(characterUid);

    protocol::AcCmdLCAchievementRewardNotify notify{};
    _commandServer.QueueCommand<decltype(notify)>(
      clientId,
      [notify]()
      {
        return notify;
      });
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
  }
}

void LobbyNetworkHandler::NotifyMatchmakeResult(
  const data::Uid characterUid,
  const MatchmakingSystem::Result& result)
{
  // LOA-fix (R38-5, round38, backlog #90a-B4): клиент мог отвалиться, пока шёл
  // подбор комнаты. GetClientIdByCharacterUid в этом случае БРОСАЕТ, а зовут нас
  // из лямбды планировщика (MatchmakingSystem::Search через
  // LobbyDirector::NotifyMatchmakeResult): исключение уходит в Scheduler::Tick,
  // тот его ловит и пишет в лог — сервер НЕ падает, но остаток задания
  // обрывается, и в логе прода копится шум на каждом уходе игрока из подбора.
  // Саму очередь чистит R38-4.
  // Смысл ровно тот же, что у соседних Notify*-методов этого файла, и они уже
  // оформлены так же: «клиента нет — уведомлять некого».
  // ★ГАСИМ ТОЛЬКО ПОИСК КЛИЕНТА. Бросок из-за нереализованного вердикта
  // (R37-1/R37-2) обязан и дальше подниматься наверх: это дефект сервера, а не
  // штатный уход игрока. Поэтому try оборачивает одну строку, а не switch.
  ClientId characterClientId{};
  try
  {
    characterClientId = GetClientIdByCharacterUid(characterUid);
  }
  catch (const std::exception&)
  {
    // We really don't care if the user disconnected.
    return;
  }

  switch (result.verdict)
  {
    case MatchmakingSystem::Result::Verdict::NoRoom:
    {
      // Matchmaking unsuccessful, no room found
      const protocol::AcCmdCLEnterRoomQuickCancel cancel{};
      _commandServer.QueueCommand<decltype(cancel)>(characterClientId, [cancel](){ return cancel; });
      break;
    }
    case MatchmakingSystem::Result::Verdict::MakeRoom:
    {
      // LOA-fix (R37-1, round37, backlog #90a-B1): было `throw new` — бросался
      // УКАЗАТЕЛЬ std::runtime_error*, который НЕ ловится ни одним
      // `catch (const std::exception&)` в дереве. Любой заход сюда давал не
      // запись в логе, а std::terminate — падение всего сервера.
      // ★ЧЕСТНО: ветка сегодня НЕДОСТИЖИМА — Result::verdict нигде не
      // выставляется в MakeRoom (MatchmakingSystem::Search выдаёт только NoRoom
      // и FoundRoom). Это разминирование на будущее, а не живой баг.
      // ★Бинарное доказательство фикса: в PRE-объекте перед __cxa_throw стоит
      // вызов operator new, в POST его нет (objdump -dC).
      throw std::runtime_error("Matchmaking system verdict make room not implemented");

      // This response is partial, you also need to create a room on the serverside and then
      // enter room. The client does not send a make room request with some random details.
      const protocol::AcCmdCLEnterRoomQuickSuccess makeRoom{
        .result = protocol::AcCmdCLEnterRoomQuickSuccess::SuccessResult::MakeRoom};
      _commandServer.QueueCommand<decltype(makeRoom)>(characterClientId, [makeRoom](){ return makeRoom; });
      break;
    }
    case MatchmakingSystem::Result::Verdict::FoundRoom:
    {
      const protocol::AcCmdCLEnterRoomQuickSuccess quickJoin{
        .result = protocol::AcCmdCLEnterRoomQuickSuccess::SuccessResult::QuickJoin};
      _commandServer.QueueCommand<decltype(quickJoin)>(characterClientId, [quickJoin](){ return quickJoin; });

      // Leverage existing handler to trigger join for client
      const protocol::AcCmdCLEnterRoom enter{
        .roomUid = result.roomUid};
      this->HandleEnterRoom(characterClientId, enter);
      break;
    }
    default:
    {
      // LOA-fix (R37-2, round37, backlog #90a-B1): близнец R37-1 — бросок
      // указателя вместо объекта, мимо всех catch-ов, прямо в std::terminate.
      throw std::runtime_error("Unrecognised matchmaking system result verdict");
    }
  }
}

std::string LobbyNetworkHandler::GetClientAddress(const ClientId clientId) noexcept
{
  // LOA-fix (R72-fix-2, round72, находка Codex 3): наружу уходит СТРОКА, а не
  // диспетчер. `CommandServer::GetClientAddress` бросает, если клиента уже нет
  // (`Server::GetClient`), а единственный вызывающий — строка лога об успешном
  // входе. Терять из-за неё соединение или ронять поток директора нельзя.
  try
  {
    return _commandServer.GetClientAddress(clientId).to_string();
  }
  catch (const std::exception&)
  {
    return {};
  }
}

ClientId LobbyNetworkHandler::GetClientIdByUserName(
  const std::string& userName,
  const bool requiresAuthorization)
{
  // LOA-fix (R64-3, round64, backlog #215): перебор — под общим замком.
  //
  // ★ПЕРЕБОР ОПАСНЕЕ ОДИНОЧНОГО ПОИСКА. `find` под чужой вставкой рискует
  // прочитать перестроенную корзину; обход же держит итератор всё время цикла,
  // и достаточно ОДНОГО удаления, чтобы итератор стал недействительным прямо
  // посреди прохода. А удаление здесь двухпоточное: директор зовёт
  // `DisconnectCharacter` → `Client::End()`, и тот СИНХРОННО доходит до
  // `HandleClientDisconnected`, который запись снимает.
  //
  // ★Замок общий (`shared_lock`), потому что читателей может быть много и они
  // друг другу не мешают; исключительный нужен только тем, кто пишет.
  // Вызовов наружу в теле нет — иначе замок ушёл бы в чужой код.
  const std::shared_lock lock(_clientsMutex);

  for (const auto& [clientId, clientContext] : _clients)
  {
    if (clientContext.userName != userName)
      continue;

    if (clientContext.isAuthenticated || not requiresAuthorization)
      return clientId;
  }

  throw std::runtime_error(
    std::format(
      "Lobby client with the user name '{}' is not available or not authenticated",
      userName));
}

ClientId LobbyNetworkHandler::GetClientIdByCharacterUid(
  const data::Uid characterUid,
  const bool requiresAuthorization)
{
  // LOA-fix (R64-3, round64, backlog #215): зеркало предыдущего метода.
  // ★Этот перебор вызывается в том числе из `DisconnectCharacter`, то есть с
  // потока ДИРЕКТОРА — ровно с той стороны, откуда приходит удаление. Без
  // замка поток мог обходить карту, которую сам же вот-вот начнёт менять на
  // следующем шаге вызова.
  const std::shared_lock lock(_clientsMutex);

  for (const auto& [clientId, clientContext] : _clients)
  {
    if (clientContext.characterUid != characterUid)
      continue;

    if (clientContext.isAuthenticated || not requiresAuthorization)
      return clientId;
  }

  throw std::runtime_error(
    std::format(
      "Lobby client with the character uid '{}' is not available or not authenticated",
      characterUid));
}

// LOA-fix (R64-3, round64, backlog #215): доступ к карте клиентов лобби
// сериализован. Разбор — RESULTS-R64.md; коротко: два потока (сетевой лобби и
// лобби-директор), директор и читает, и ПИШЕТ поля, а удаление записи вдобавок
// исполняется на ЕГО потоке через синхронный `Client::End()`.

LobbyNetworkHandler::ClientContext&
LobbyNetworkHandler::GetClientContextLocked(
  const ClientId clientId,
  bool requireAuthentication)
{
  // ★ВНУТРЕННИЙ ПУТЬ: замок ОБЯЗАН быть уже взят вызывающим. Существует
  // ровно затем, чтобы уборка соединения (которая держит исключительный
  // замок через страж удаления) не звала публичный метод и не пыталась
  // взять `shared_mutex` повторно: он НЕ рекурсивный, и такой самозахват —
  // это не падение на ревью, а тихий дедлок в проде под нагрузкой.
  // Ровно этим был убит вход в игру в R59 (0 успешных заходов из 24).
  auto clientContextIter = _clients.find(clientId);
  if (clientContextIter == _clients.end())
    throw std::runtime_error("Lobby client is not available");

  auto& clientContext = clientContextIter->second;
  if (requireAuthentication && not clientContext.isAuthenticated)
    throw std::runtime_error("Lobby client is not authenticated");

  return clientContext;
}

LobbyNetworkHandler::ClientContext LobbyNetworkHandler::GetClientContext(
  const ClientId clientId,
  bool requireAuthentication)
{
  // ★КОПИЯ, А НЕ ССЫЛКА. Замок снимается на выходе, поэтому наружу нельзя
  // отдавать ничего, что указывает внутрь карты.
  const std::shared_lock lock(_clientsMutex);
  return GetClientContextLocked(clientId, requireAuthentication);
}

bool LobbyNetworkHandler::IsClientAuthenticated(const ClientId clientId) const
{
  // ★КОПИЯ ФЛАГА, А НЕ ССЫЛКА В КАРТУ: замок снимается на выходе.
  // ★И НЕ БРОСАЕТ. Этот вопрос задаётся на КАЖДОМ входящем пакете, в том
  // числе от сокета, который не логинился; бросок здесь означал бы строку
  // [error] на пакет — тот самый флуд, который раунд убирает.
  const std::shared_lock lock(_clientsMutex);
  const auto clientContextIter = _clients.find(clientId);
  return clientContextIter != _clients.cend()
    && clientContextIter->second.isAuthenticated;
}

void LobbyNetworkHandler::NoteRefusedPreAuthCommand(
  const ClientId clientId,
  const protocol::Command command) noexcept
{
  uint64_t suppressed = 0;
  uint64_t total = 0;
  if (not _preAuthRefusalThrottle.Allow(suppressed, total))
    return;

  // ★ЛОГИРУЕМ clientId, А НЕ АДРЕС. `CommandServer::GetClientAddress` идёт в
  // `_server.GetClient(...)` и УМЕЕТ БРОСИТЬ, а мы стоим в `noexcept` на пути
  // пакета. clientId — тот же идентификатор, которым пользуются остальные
  // строки лобби. Не «улучшать» это обратно на адрес.
  // ★`total` ПЕЧАТАЕТСЯ НЕ ДЛЯ КРАСОТЫ: дроссель глушит строки, и без
  // накопительного счёта «отказ случился и честному клиенту тоже» был бы
  // ненаблюдаем — оракул регрессии раунда считает именно по нему.
  server::util::QuietLogWarn(
    "Refused a lobby command(s) from unauthenticated clients: "
    "client {} sent '{}' (0x{:x}); {} more refusals suppressed since the "
    "previous line; {} refusals total since start",
    clientId,
    protocol::GetCommandName(command),
    static_cast<uint32_t>(command),
    suppressed,
    total);
}

bool LobbyNetworkHandler::MutateClientContext(
  const ClientId clientId,
  const std::function<void(ClientContext&)>& mutator)
{
  const std::unique_lock lock(_clientsMutex);

  const auto clientContextIter = _clients.find(clientId);
  if (clientContextIter == _clients.end())
    return false;

  // ★В лямбде допустимы ТОЛЬКО присваивания полей. Любой вызов наружу отсюда
  // уводит замок в чужой код — а именно так рождается перекрёстный дедлок
  // лобби↔ранчо (`IsCharacterActiveOnRanch` и соседи ходят в другой директор).
  mutator(clientContextIter->second);
  return true;
}

bool LobbyNetworkHandler::MutateClientContextIfSame(
  const ClientId clientId,
  const data::Uid expectedCharacterUid,
  const std::function<void(ClientContext&)>& mutator)
{
  const std::unique_lock lock(_clientsMutex);

  const auto clientContextIter = _clients.find(clientId);
  if (clientContextIter == _clients.end())
    return false;

  // ★ПРОВЕРКА ТОЖДЕСТВА, А НЕ ТОЛЬКО ПРИСУТСТВИЯ. Решение сюда приходит из
  // работы ПО СНИМКУ, снятому вне замка: за это время клиент мог отключиться,
  // а его `ClientId` — достаться новому подключению. Тогда `find` найдёт
  // запись, и без сверки мы применили бы чужое решение к постороннему игроку
  // (например, отключили бы только что зашедшего). Сверяем по `characterUid` —
  // он стабилен в пределах жизни записи и меняется только вместе с ней.
  if (clientContextIter->second.characterUid != expectedCharacterUid)
    return false;

  mutator(clientContextIter->second);
  return true;
}

// LOA-fix (R37-4, round37, backlog #123): ★ФУНКЦИОНАЛЬНЫЙ try-БЛОК НА ВЕСЬ
// ТИК. Нас зовут из Server::TickLoop(), помеченного noexcept
// (Server.cpp:463) — любое улетевшее отсюда исключение это не строка в логе, а
// std::terminate всего процесса. А бросающие вызовы в теле есть: сам слив
// `_commandServer.DisconnectClient` уходит в Server::GetClient с
// `throw std::runtime_error("Invalid client")`, плюс межпотоковые запросы к
// директорам ранчо/гонки. noexcept у TickLoop мы НЕ СНИМАЕМ — безопасным
// делаем ТЕЛО, ровно как RanchDirector::HandleNetworkTick (тот же catch, тот
// же spdlog::error) и RunDirectorTaskLoop. Молча не глотаем: пишем в лог.
// Форма — функциональный try-блок, чтобы не сдвигать ~270 строк тела на два
// пробела; для не-конструктора он эквивалентен обычному try вокруг тела.
void LobbyNetworkHandler::HandleNetworkTick()
try
{
  const auto now = std::chrono::steady_clock::now();

  // LOA-fix (R21-4g, round21, backlog #95): ПЕРИОДИЧЕСКАЯ УБОРКА реестра
  // активности ранчо — та самая верхняя граница его памяти. Быстрая уборка на
  // выходах с ранча (R21-2b/2c/2d) закрывает общий случай, но teardown клиента
  // умеет бежать на ЭТОМ, лобби-потоке (Client::End() зовёт OnClientDisconnected
  // синхронно; ★с раунда 34 RanchDirector::Disconnect ниже сокет УЖЕ НЕ рвёт —
  // он только ставит UID в очередь ранч-сетевого потока, см. R34), поэтому
  // редкая гонка enter-vs-disconnect способна оставить запись-сироту, которую не
  // подберёт ни один выходной путь. Разбор — в шапке раунда 21.
  //
  // ПОРОГ 90 c ЗАВЕДОМО БОЛЬШЕ ОКНА СВЕЖЕСТИ 30 c. Живой игрок переставляет
  // метку каждые ≤8.5 c, поэтому под уборку не попадает НИКОГДА; запись
  // возрастом 30-90 c уже не свежая (грейса не даёт, вреда не несёт) и просто
  // ждёт ближайшей уборки. Направление отказа безопасное: потерянная запись —
  // это отсутствие отсрочки в это окно (обычный таймаут 60 c) и самолечение на
  // следующем входе на ранчо, а НЕ бессмертная сессия.
  //
  // Раз в минуту, а не каждый тик: обход O(n) секунда в секунду — пустая
  // нагрузка, записи живут минутами. Сам метод межпотоково безопасен (берёт
  // только свой ЛИСТОВОЙ мьютекс, к _clients ранчо не прикасается).
  constexpr auto RanchActivitySweepInterval = std::chrono::seconds(60);
  constexpr auto RanchActivityMaxAge = std::chrono::seconds(90);

  if (now - _lastRanchActivitySweep >= RanchActivitySweepInterval)
  {
    _lastRanchActivitySweep = now;
    _serverInstance.GetRanchDirector().SweepRanchActivity(RanchActivityMaxAge);
  }
  // LOA-fix (R64-3, round64, backlog #215): ТИК РАБОТАЕТ ПО КОПИЯМ, А НЕ ПО КАРТЕ.
  //
  // ★ЗАЧЕМ. Прежний цикл шёл прямо по `_clients` и держал итератор всё время
  // прохода. Удаление записи здесь ДВУХПОТОЧНОЕ: директор зовёт
  // `DisconnectCharacter` → `Client::End()`, а тот СИНХРОННО доходит до
  // `HandleClientDisconnected`, который запись снимает. Одного такого удаления
  // достаточно, чтобы итератор обхода стал недействительным посреди тика.
  //
  // ★ПОЧЕМУ НЕ «ВЗЯТЬ ЗАМОК НА ВЕСЬ ЦИКЛ». В теле — ТРИ вызова в чужие
  // директоры (`IsCharacterActiveOnRanch`, `IsCharacterLoadingRace`, и пара
  // `Disconnect`/`DisconnectCharacter` в ветке кика). Замок, дотянутый до них,
  // ушёл бы в чужой код, а `Disconnect*` синхронно возвращается в нашу же
  // уборку за тем же нерекурсивным мьютексом — это самозахват, то есть тихий
  // дедлок под нагрузкой, а не падение на ревью. ★Их именно три: посчитано
  // грепом, а не замечено взглядом — с первого раза я насчитала один.
  //
  // ★ПОЧЕМУ КОПИИ, А НЕ ПЕРЕПИСЫВАНИЕ ТЕЛА. Тело — двести строк выверенной
  // логики отсрочек (грейс гонки R12-5, грейс ранчо R21-4b, решётка
  // 0/61/122/183/244 c, одноразовый хендофф). Работая по копии, оно остаётся
  // ДОСЛОВНЫМ и целиком уезжает из-под замка вместе со всеми тремя вызовами —
  // меняются только вход в цикл и применение результата.
  //
  // ★ОТБОР ДЕШЁВЫЙ И ОБЫЧНО ПУСТОЙ. В кандидаты попадает лишь тот, у кого уже
  // истёк порог пульса; в норме таких НОЛЬ, и тогда ни исключительный замок, ни
  // единая копия не берутся вовсе — при 50 тиках в секунду это важно.
  // Буфер — член класса: `clear()` сохраняет ёмкость, поэтому после первого
  // тика аллокаций нет.
  std::vector<std::pair<ClientId, data::Uid>> clientsToDisconnect;

  _tickSnapshot.clear();
  {
    const std::shared_lock lock(_clientsMutex);
    for (const auto& [clientId, clientContext] : _clients)
    {
      // Тот же порог, что и в теле ниже: клиент в создателе персонажа законно
      // молчит дольше (клиентский баг с пульсом).
      const auto timeout = clientContext.isInCharacterCreator
        ? std::chrono::seconds(15 * 60)
        : std::chrono::seconds(60);
      if (now - clientContext.lastHeartbeat > timeout)
        _tickSnapshot.emplace_back(clientId, clientContext);
    }
  }

  // LOA-fix (R12-4a, round12, backlog #85): было `const auto&` — грейс ниже
  // пишет в контекст клиента (lastHeartbeat + метка начала грейса).
  // ★Теперь пишет В КОПИЮ; изменения возвращаются в карту после цикла.
  for (auto& [clientId, clientContext] : _tickSnapshot)
  {
    // There's a bug in a client, where if the client is in the character creator,
    // they'll withdraw from sending heartbeats. Because of this we have to
    // ignore the lack of heartbeats for a while longer and not disconnect the client.
    const auto timeout = clientContext.isInCharacterCreator
      ? std::chrono::seconds(15 * 60)
      : std::chrono::seconds(60);

    const bool hasReachedTimeout = now - clientContext.lastHeartbeat > timeout;
    if (not hasReachedTimeout)
      continue;

    // LOA-fix (R21-4b, round21, backlog #95): ОТСРОЧКА ПО ЖИВОМУ РАНЧ-СОКЕТУ.
    // У игрока четыре независимых сокета. Осев на ранчо, реальный клиент
    // перестаёт качать ЛОББИ-пульс, продолжая говорить по ранч-каналу
    // (:10031) — снапшоты положения, чат, уход за лошадью. Тишина одного
    // сокета не означает мёртвого игрока, а кик отсюда рвёт ему и ранчо, и
    // гонку. Поэтому перед киком спрашиваем директора ранчо, говорил ли этот
    // персонаж по ранч-каналу за последние 30 c.
    //
    // ★ЗАПРОС МЕЖПОТОКОВЫЙ (та же дисциплина, что в R12): ранчо живёт на
    // другом потоке, его _clients ничем не защищён, поэтому лобби НЕ читает
    // состояние ранчо напрямую. IsCharacterActiveOnRanch трогает только свой
    // ЛИСТОВОЙ мьютекс и реестр активности — вложенных локов нет, deadlock
    // ранчо↔лобби невозможен.
    //
    // ★ПОТОЛКА НЕТ — И ЭТО НАМЕРЕННО. R12 ждал КОНЕЧНОГО события (загрузка
    // карты) и обязан был иметь потолок 210 c. Здесь состояние нормальное и
    // длительное: на ранчо стоят часами. Ограничитель — не время, а УСЛОВИЕ:
    // свежесть 30 c. Упавший клиент (полумёртвый TCP без FIN) перестаёт слать
    // пакеты, запись стареет, ветка перестаёт срабатывать, и кик доводится до
    // конца — призрачная сессия не живёт.
    //
    // ★ПОРЯДОК ВЕТОК: стоим ПЕРЕД веткой R12 и делаем `continue`, НЕ трогая
    // raceLoadingGraceSince — эпизодный инвариант R12 (метка живёт только
    // внутри одной загрузки карты) остаётся ровно таким, каким его завёл R12.
    // Обратная интерференция тоже безопасна: во время загрузки карты клиент не
    // качает и ранч-сокет, запись стареет за 30 c, управление уходит в R12.
    if (clientContext.isAuthenticated
      && _serverInstance.GetRanchDirector().IsCharacterActiveOnRanch(
        clientContext.characterUid,
        std::chrono::seconds(30)))
    {
      if (clientContext.ranchGraceSince
        == std::chrono::steady_clock::time_point{})
      {
        clientContext.ranchGraceSince = now;

        server::util::QuietLogInfo(
          "Client {} ('{}') is settled on a ranch (ranch socket active); "
          "lobby timeout DEFERRED",
          clientId,
          clientContext.userName);
      }
      else
      {
        server::util::QuietLogDebug(
          "Client {} ('{}') is still active on a ranch; "
          "lobby timeout deferred ({}s so far)",
          clientId,
          clientContext.userName,
          std::chrono::duration_cast<std::chrono::seconds>(
            now - clientContext.ranchGraceSince).count());
      }

      // Как и в R12: не «пропустить кик один раз», а признать клиента живым —
      // иначе накопленная тишина убьёт его на следующем же тике.
      clientContext.lastHeartbeat = now;
      continue;
    }

    // LOA-fix (R12-4b, round12, backlog #85): ОТСРОЧКА НА ВРЕМЯ ЗАГРУЗКИ КАРТЫ.
    // Клиент однопоточный: загружая тяжёлую карту заезда, он перестаёт качать
    // ВСЕ свои сокеты, в том числе лобби-пульс. Единственный в сервере таймер
    // простоя живёт здесь (у ранчо/гонки/мессенджера таймаутов нет вообще), и
    // он же рвёт игроку ранчо и гонку строками ниже — то есть на карте, которая
    // грузится дольше ~60 c, игрока выкидывало ДО того, как поднятый в R11
    // бюджет загрузки (LoadingStageTimeout, 150 c, задача #22) успевал сработать.
    // Живой случай: SmileMarlboro, старт 12:00:51.577, кик 12:02:04.257 (72.7 c).
    //
    // ПОЧЕМУ ГРЕЙС ОСВЕЖАЕТ lastHeartbeat (это не косметика, а суть ремонта 1).
    // Реальная каденция лобби-пульса клиента, снятая с прода, — НЕ «раз в
    // несколько секунд», а ~40 c, причём с дырами: наблюдалась пауза 57 c между
    // соседними пульсами, и дыра длиной 18.8 c приходилась ровно на момент
    // окончания загрузки. Если грейс только пропускает кик, не трогая
    // lastHeartbeat, то накопленная за загрузку тишина никуда не девается:
    // клиент рапортует LoadingComplete, гонщик мгновенно переходит в Racing
    // (RaceNetworkHandler.cpp, HandleLoadingComplete), запрос ниже начинает
    // возвращать false — и уже на СЛЕДУЮЩЕМ тике игрока убивает по старой
    // тишине, до того как успеет прийти первый послезагрузочный пульс. Поэтому
    // в грейсе мы ЯВНО двигаем lastHeartbeat. Одного этого, впрочем, мало:
    // движение происходит на редкой решётке (см. ниже), поэтому полное окно на
    // первый послезагрузочный пульс выдаёт отдельный одноразовый хендофф.
    //
    // ПОТОЛОК ЖИВЁТ НА ОТДЕЛЬНОЙ МЕТКЕ. Раз lastHeartbeat теперь двигается, по
    // нему нельзя мерить, сколько клиент уже сидит в грейсе. Для этого заведено
    // поле raceLoadingGraceSince (R12-5): ставится один раз при входе в грейс и
    // гаснет на реальном пульсе (R12-6), на хендоффе (ниже) либо при
    // переиспользовании контекста (R12-7/R12-8) — то есть каждая загрузка
    // получает свежий потолок, а не остаток от прошлой.
    //
    // ПОЧЕМУ НЕ ГЛОБАЛЬНО. Порог 60 c — единственная уборка мёртвых сессий:
    // повторный логин того же пользователя отбивается как Duplicated
    // (LobbyDirector.cpp:437-448), а зависший игрок держит слот в комнате и
    // никогда не станет ready. Глобальные 210 c означали бы «после краша клиента
    // не зайти 3.5 минуты» и мёртвые слоты в комнатах. Отсрочка адресная.
    //
    // РЕШЁТКА ПРОВЕРОК (ремонт 2; без этого цифры ниже врут). Грейс
    // переоценивается НЕ раз в секунду: он живёт ПОД гардом
    // `if (not hasReachedTimeout) continue;`, а каждое освежение lastHeartbeat
    // отодвигает следующее срабатывание таймаута ровно на 61 c (порог 60 c +
    // тик 1 c). Поэтому счётчик grace-elapsed принимает не любые значения, а
    // только 0 / 61 / 122 / 183 / 244 c.
    //
    // ОДНОРАЗОВЫЙ ХЕНДОФФ (★суть ремонта 2, ветка else ниже). Загрузка
    // кончается в произвольный момент МЕЖДУ точками решётки. Если на ближайшей
    // точке просто увидеть «клиент больше не грузит» и провалиться в кик, игрок
    // умирает уже ПОСЛЕ успешного LoadingComplete — в окне [0, 60] c после
    // него; две независимые симуляции дали так до 57% убитых на тяжёлых картах.
    // Поэтому, увидев погашенную загрузку при ещё живой метке грейса, мы ОДИН
    // раз выдаём клиенту полное штатное окно (двигаем lastHeartbeat) и гасим
    // метку. Тем же закрыт второй дефект: при штатном выходе из грейса метка
    // раньше вообще не сбрасывалась и доживала до первого пульса.
    //
    // ЧЕМ ОГРАНИЧЕНА (бессмертных зомби нет):
    //   1) клиент, залипший в стадии Loading навсегда: потолок 210 c впервые НЕ
    //      проходит на решётке при grace-elapsed 244 c — кик примерно на 305 c
    //      стены от последнего реального пульса;
    //   2) реалистичный случай — стадию Loading принудительно закрывает
    //      TickLoading по дедлайну R11 (150 c): таймаут после этого впервые
    //      срабатывает на ~183 c стены, там тратится одноразовый хендофф, и
    //      по-настоящему мёртвый клиент умирает на следующей точке решётки;
    //   3) реальный обрыв TCP идёт мимо этой ветки (HandleClientDisconnected).
    //
    // ПРИНЯТАЯ ЦЕНА. Зависший игрок держит слот в комнате дольше: дедлайн
    // загрузки 150 c плюс до 60 c смещения между его последним пульсом и
    // стартом загрузки — worst case ~211 c (раньше случайный кик на 60 c
    // освобождал слот быстрее). Осознанный размен: неслучившийся кик живого
    // игрока на медленной карте важнее пары минут занятого слота.
    constexpr auto RaceLoadingHeartbeatTimeout = std::chrono::seconds(210);

    const bool isLoadingRaceMap = clientContext.isAuthenticated
      && _serverInstance.GetRaceDirector().IsCharacterLoadingRace(
        clientContext.characterUid);

    if (isLoadingRaceMap)
    {
      if (clientContext.raceLoadingGraceSince
        == std::chrono::steady_clock::time_point{})
      {
        clientContext.raceLoadingGraceSince = now;

        server::util::QuietLogInfo(
          "Client {} ('{}') is loading a race map; lobby timeout grace engaged",
          clientId,
          clientContext.userName);
      }

      const auto graceElapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - clientContext.raceLoadingGraceSince);

      if (graceElapsed <= RaceLoadingHeartbeatTimeout)
      {
        // Прогресс-лог на КАЖДОЙ переоценке грейса, кроме первой (её печатает
        // spdlog::info выше). Спама нет по построению: соседние переоценки
        // отстоят на 61 c (см. «РЕШЁТКА ПРОВЕРОК»), то есть за весь потолок
        // 210 c клиент даёт максимум четыре строки. Прежнее условие
        // `graceElapsed % 15 == 0` было мёртвым кодом: 61/122/183/244 на 15 не
        // делятся, лог не мог напечататься НИ РАЗУ.
        if (graceElapsed.count() > 0)
        {
          server::util::QuietLogDebug(
            "Client {} ('{}') is still loading a race map ({}s of grace used)",
            clientId,
            clientContext.userName,
            graceElapsed.count());
        }

        // Ключевая строка ремонта: пока клиент грузит карту, сервер считает
        // его живым НА САМОМ ДЕЛЕ, а не «пропускает кик один раз».
        clientContext.lastHeartbeat = now;
        continue;
      }

      server::util::QuietLogWarn(
        "Client {} ('{}') exhausted the race-loading grace ({}s); "
        "falling through to the network timeout",
        clientId,
        clientContext.userName,
        graceElapsed.count());
    }
    else if (clientContext.raceLoadingGraceSince
      != std::chrono::steady_clock::time_point{})
    {
      // ХЕНДОФФ ПОСЛЕ ЗАГРУЗКИ (одноразовый). Мы здесь потому, что таймаут
      // сработал, клиент УЖЕ не грузит карту, но метка грейса ещё горит — то
      // есть загрузка закончилась где-то между двумя точками решётки, и
      // накопленная за неё тишина сейчас убила бы игрока через считанные
      // секунды после успешного LoadingComplete. Отдаём ему ровно одно полное
      // штатное окно (60 c) на первый послезагрузочный пульс и ГАСИМ метку:
      // окно выдаётся один раз, следующая проверка будет уже обычной.
      const auto graceElapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - clientContext.raceLoadingGraceSince);

      server::util::QuietLogInfo(
        "Client {} ('{}') is no longer loading a race map after {}s of grace; "
        "granting one full heartbeat window before the timeout applies again",
        clientId,
        clientContext.userName,
        graceElapsed.count());

      clientContext.lastHeartbeat = now;
      clientContext.raceLoadingGraceSince = {};
      continue;
    }

    server::util::QuietLogWarn(
       "Client {} ('{}') has reached a network timeout and is being disconnected",
       clientId,
       clientContext.userName);

    clientsToDisconnect.emplace_back(clientId, clientContext.characterUid);
  }

  // LOA-fix (R64-3, round64, backlog #215): ИЗМЕНЕНИЯ ИЗ КОПИЙ — ОБРАТНО В КАРТУ.
  //
  // Тело выше писало в копию; сюда возвращаются ровно те три поля, которые оно
  // могло изменить (метки пульса и двух отсрочек). Остальные поля не трогаем —
  // их владелец другой код, и слепое копирование всей структуры затёрло бы
  // чужие изменения, случившиеся, пока замок был отпущен.
  //
  // ★ПРОВЕРКА ТОЖДЕСТВА ОБЯЗАТЕЛЬНА. Между снятием копий и этой строкой замок
  // был отпущен — клиент мог отключиться, а его `ClientId` достаться новому
  // подключению. `MutateClientContextIfSame` сверяет `characterUid` и молча
  // ничего не делает, если запись уже не та: иначе мы применили бы решение о
  // старом игроке к только что вошедшему.
  for (const auto& [clientId, snapshotContext] : _tickSnapshot)
  {
    MutateClientContextIfSame(
      clientId,
      snapshotContext.characterUid,
      [&snapshotContext](ClientContext& liveContext)
      {
        liveContext.lastHeartbeat = snapshotContext.lastHeartbeat;
        liveContext.ranchGraceSince = snapshotContext.ranchGraceSince;
        liveContext.raceLoadingGraceSince = snapshotContext.raceLoadingGraceSince;
      });
  }

  // ★ОТКЛЮЧЕНИЯ — ПОСЛЕ ЦИКЛА И ВНЕ ЗАМКА. Прежде `Disconnect` и
  // `DisconnectCharacter` стояли ВНУТРИ тела; там они уехали бы под замок
  // вместе с ним. Здесь же замок не удерживается, а `DisconnectClient` ниже
  // синхронно доходит до нашей уборки, которой нужен исключительный замок, —
  // держать его в этот момент значило бы устроить самозахват.
  for (const auto& [clientId, characterUid] : clientsToDisconnect)
  {
    _serverInstance.GetRanchDirector().Disconnect(characterUid);
    _serverInstance.GetRaceDirector().DisconnectCharacter(characterUid);
    // LOA-fix (R37-5, round37, backlog #123): ПОКЛИЕНТНЫЙ ГАРД. DisconnectClient
    // доходит до Server::GetClient, а тот БРОСАЕТ «Invalid client», если реестр
    // сетевого сервера и реестр лобби разъехались. Без этой обёртки первая же
    // осечка съедала бы остаток списка — прочие таймаутнутые клиенты остались бы
    // висеть. Зеркало ранчёвого DrainPendingDisconnects (R34-2/R34-4).
    try
    {
      _commandServer.DisconnectClient(clientId);
    }
    catch (const std::exception& x)
    {
      server::util::QuietLogWarn(
        "Failed to disconnect the timed-out lobby client {}: {}",
        clientId,
        x.what());
    }
  }
}
catch (const std::exception& x)
{
  // Обработчик функционального try-блока R37-4. Выход за его конец у
  // void-функции равнозначен `return;` — тик просто заканчивается, сервер
  // продолжает жить, а причина остаётся в логе.
  server::util::QuietLogError("Exception in a network tick of lobby handler: {}", x.what());
}

void LobbyNetworkHandler::HandleClientConnected(
  const ClientId clientId)
{
  // LOA-fix (R64-3, round64, backlog #215): вставка и первичное заполнение —
  // под исключительным замком, одним блоком.
  //
  // ★ЗАМОК ЗАКРЫВАЕТСЯ ДО ВЫЗОВОВ НАРУЖУ, И ЭТО НЕ КОСМЕТИКА. Ниже идут
  // `GetClientAddress` и постановка задачи в планировщик директора — то есть
  // выход в чужой код. Замок, дотянутый до них, превращает локальную вставку в
  // заявку на перекрёстный дедлок; поэтому область явная, а не «до конца
  // функции».
  {
    const std::unique_lock lock(_clientsMutex);

    const auto iter = _clients.try_emplace(clientId).first;
    iter->second.lastHeartbeat = std::chrono::steady_clock::now();
    // LOA-fix (R12-7, round12, backlog #85): вместе с пульсом гасим и метку
    // грейса (R12-5) — ClientId переиспользуются, чужой недоеденный потолок
    // не должен достаться новому подключению.
    iter->second.raceLoadingGraceSince = {};
    // LOA-fix (R21-4d, round21, backlog #95): и логовую метку ранч-отсрочки —
    // ClientId переиспользуются (зеркало R12-7).
    iter->second.ranchGraceSince = {};
  }

  server::util::QuietLogDebug(
    "Client {} connected to the lobby server from {}",
    clientId,
    _commandServer.GetClientAddress(clientId).to_string());

  _serverInstance.GetLobbyDirector().GetScheduler().Queue(
    [this, clientId]()
    {
      _serverInstance.GetLobbyDirector().QueueClientConnect(clientId);
    });
}

void LobbyNetworkHandler::HandleClientDisconnected(ClientId clientId)
{
    // LOA-fix (R50-4, round50, backlog #180): УБОРКА ЛОББИ ДОХОДИТ ДО КОНЦА.
    // Запись реестра снималась ПОСЛЕДНЕЙ строкой, то есть только на успешном
    // пути, а до неё стояла бросающая работа (чтение контекста, снятие с
    // очереди подбора, постановка задачи с копией имени — аллокация).
    // ★ЦЕНА УТЕЧКИ ЗДЕСЬ ВИДНА ИГРОКУ: `HandleLogin` отказывает с `Duplicated`,
    // если в реестре найдётся аутентифицированная запись с тем же логином.
    // Осиротевшая запись = аккаунт, который НЕ МОЖЕТ ВОЙТИ В ИГРУ до
    // перезапуска сервера. Повторить уборку некому: гард `Client::End()` уже
    // снят, сокет закрыт, событий asio по нему больше не будет.
    // Страж снимает запись на выходе из функции — ровно там, где стоял `erase`,
    // то есть порядок операций прежний.
    // LOA-fix (R64-3, round64, backlog #215): СНЯТИЕ ЗАПИСИ — ПОД ЗАМКОМ,
    // РАБОТА НИЖЕ — БЕЗ НЕГО.
    //
    // ★ПОЧЕМУ НЕ ПРЕЖНИЙ `util::RegistryEraser`: он ничего не знает о замке и
    // удалил бы запись из карты голыми руками, ровно в тот момент, когда её
    // может перебирать тик. Гарантию R50 («уборка доходит до конца при любом
    // исходе») сохраняем — страж остаётся RAII и снимает запись на выходе, — но
    // теперь он делает это под исключительным замком.
    struct LockedContextEraser final
    {
      LobbyNetworkHandler& handler;
      ClientId clientId;

      ~LockedContextEraser() noexcept
      {
        const std::unique_lock lock(handler._clientsMutex);
        handler._clients.erase(clientId);
      }
    } const eraser{*this, clientId};

    // ★КОПИЯ ПОД ЗАМКОМ, А ДАЛЬШЕ РАБОТА ПО КОПИИ. Ниже по функции идут выходы
    // в чужой код (снятие с очереди подбора, планировщик директора), и держать
    // через них замок нельзя. А ссылку в карту держать нельзя тем более: запись
    // вот-вот снимет наш же страж.
    // ★Публичный `GetClientContext` здесь НЕ зовём намеренно: он берёт замок
    // сам, а `shared_mutex` не рекурсивный — вложенный захват дал бы тихий
    // дедлок в проде (класс R59, там это стоило 24 входов из 24).
    ClientContext clientContext;
    {
      const std::shared_lock lock(_clientsMutex);
      clientContext = GetClientContextLocked(clientId, false);
    }

    // LOA-fix (R38-4, round38, backlog #90a-B4): СНИМАЕМ ПЕРСОНАЖА С ОЧЕРЕДИ
    // БЫСТРОГО СТАРТА. Выход из игры «в поиске комнаты» запись в
    // MatchmakingSystem::_matchmakingQueue НЕ убирал: её подбирал только
    // собственный таймаут подбора, MatchmakingQueueTimeoutMs = 30 c.
    // ★ЧЕСТНО: запись НЕ вечная, окно ограничено этими 30 c. Но внутри окна
    // видны два дефекта:
    //   (1) ДЕТЕРМИНИРОВАННЫЙ СИМПТОМ (он же A/B-плечо приёмки): игрок
    //       перезаходит в пределах 30 c и жмёт «быстрый старт» — Queue видит
    //       старую запись, возвращает false, клиент получает
    //       AcCmdCLEnterRoomQuickCancel. «Быстрый старт не работает после
    //       релога» — это ровно оно;
    //   (2) осиротевшая цепочка Search продолжает тикать раз в секунду и
    //       ключуется по characterUid — то есть если она найдёт комнату уже
    //       ПОСЛЕ релога, свежий клиент будет затащен в комнату, которую не
    //       просил.
    // Dequeue сам берёт _matchmakingQueueMutex (после R36-2 это уже честный
    // замок) и молча возвращает false, если персонаж в очереди не стоял, —
    // звать безусловно безопасно, в том числе для неаутентифицированного
    // клиента с characterUid == data::InvalidUid. Возврат намеренно не
    // проверяем: false здесь — норма, а метод не [[nodiscard]].
    util::RunCleanupStep(
      "lobby matchmaking dequeue",
      clientId,
      [&]()
      {
        _serverInstance.GetMatchmakingSystem().Dequeue(clientContext.characterUid);
      });

    // Шаг НЕЗАВИСИМЫЙ от предыдущего: осечка снятия с очереди подбора не имеет
    // права отменить выход из игры. Раньше отменяла — и персонаж оставался
    // «в сети» для лобби-директора.
    util::RunCleanupStep(
      "lobby logout scheduling",
      clientId,
      [&]()
      {
        _serverInstance.GetLobbyDirector().GetScheduler().Queue(
          [this, isAuthenticated = clientContext.isAuthenticated, clientId, userName = clientContext.userName]()
          {
            // LOA-fix (R50-10, round50, backlog #180): ОТЛОЖЕННАЯ ПОЛОВИНА ТОЙ
            // ЖЕ УБОРКИ, и у неё повтора нет ровно так же. Задача исполняется
            // планировщиком через секунду и одна на два шага: выход из игры и
            // снятие клиента с очередей входа. Бросок первого шага съедал
            // второй — а планировщик к этому моменту уже вынул задачу из
            // списка, то есть никто её не повторит.
            if (isAuthenticated)
            {
              util::RunCleanupStep(
                "lobby user logout",
                clientId,
                [&]()
                {
                  _serverInstance.GetLobbyDirector().QueueClientLogout(
                    clientId,
                    userName);
                });
            }

            util::RunCleanupStep(
              "lobby login queue cleanup",
              clientId,
              [&]()
              {
                _serverInstance.GetLobbyDirector().QueueClientDisconnect(clientId);
              });
          });
      });
  server::util::QuietLogDebug("Client {} disconnected from the lobby server", clientId);
}

void LobbyNetworkHandler::HandleLogin(
  const ClientId clientId,
  const protocol::AcCmdCLLogin& command)
{
  // Alicia 1.0
  assert(command.constant0 == 50
    && command.constant1 == 281
    && "Game version mismatch");

  // Validate the command fields.
  if (command.loginId.empty() || command.authKey.empty())
  {
    SendLoginCancel(clientId, protocol::AcCmdCLLoginCancel::Reason::InvalidLoginId);
    return;
  }

  // LOA-fix (R64-3, round64, backlog #215): гард повторного входа — под замком,
  // но ОТВЕТ клиенту уже без него.
  //
  // ★НАЙДЕНО РЕВЬЮ, А НЕ МОИМИ ГЕЙТАМИ, и это важно записать. Мои сканеры
  // искали переборы вида `for (auto& [id, ctx] : _clients)` и вызовы наружу
  // под уже взятым замком — а здесь форма другая (`| std::views::values`), и
  // замка тут не было вовсе. Проверка ключилась на ФОРМУ обращения к карте
  // вместо СВОЙСТВА «любое обращение к `_clients` обязано быть под замком».
  //
  // ★Обернуть цикл замком целиком нельзя: `SendLoginCancel` внутри — выход в
  // чужой код. Поэтому под замком принимается РЕШЕНИЕ, а отправка идёт после.
  bool duplicateLogin = false;
  {
    const std::shared_lock lock(_clientsMutex);

    for (const auto& clientContext : _clients | std::views::values)
    {
      if (clientContext.userName != command.loginId
        || not clientContext.isAuthenticated)
      {
        continue;
      }

      duplicateLogin = true;
      break;
    }
  }

  if (duplicateLogin)
  {
    SendLoginCancel(clientId, protocol::AcCmdCLLoginCancel::Reason::Duplicated);
    return;
  }

  // LOA-fix (R64-3, round64, backlog #215): имя пользователя — под замком.
  // ★По нему ищет `GetClientIdByUserName`, перебирающий карту с ЧУЖОГО потока:
  // запись строки в контекст во время такого перебора — это гонка на самой
  // строке, а не только на структуре карты.
  if (not MutateClientContext(
        clientId,
        [&command](ClientContext& clientContext)
        {
          clientContext.userName = command.loginId;
        }))
  {
    throw std::runtime_error("Lobby client is not available");
  }

  _serverInstance.GetLobbyDirector().GetScheduler().Queue(
    [this, clientId, userName = command.loginId, userToken = command.authKey]()
    {
      [[maybe_unused]] const auto queuePosition = _serverInstance.GetLobbyDirector().QueueClientLogin(
        clientId,
        userName,
        userToken);

      //SendWaitingSeqno(clientId, queuePosition);
    });
}

void LobbyNetworkHandler::SendLoginOK(ClientId clientId)
{
  // LOA-fix (R64-3, round64, backlog #215): КОПИЯ для чтения имени; запись
  // characterUid ниже — отдельной мутацией под замком. Между ними идут выходы
  // в чужой код (кэш пользователей, конфиг директора), поэтому ссылку в карту
  // здесь держать нельзя: за это время карта может быть перестроена вставкой.
  const auto clientContext = GetClientContext(clientId);

  const auto userRecord = _serverInstance.GetDataDirector().GetUserCache().Get(
    clientContext.userName);
  if (not userRecord)
    throw std::runtime_error("User record unavailable");

  const auto& lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();

  // Get the character UID of the user.
  auto userCharacterUid{data::InvalidUid};
  userRecord->Immutable(
    [&userCharacterUid](
      const data::User& user)
    {
      userCharacterUid = user.characterUid();
    });

  // LOA-fix (R64-3, round64, backlog #215): привязка персонажа к сессии — под
  // замком. ★По этому полю ищет `GetClientIdByCharacterUid`, который зовут с
  // ЧУЖОГО потока (в том числе из `DisconnectCharacter`), так что запись сюда
  // обязана быть сериализована с тем перебором.
  MutateClientContext(
    clientId,
    [userCharacterUid](ClientContext& clientContext)
    {
      clientContext.characterUid = userCharacterUid;
    });

  // Promote any foals that matured while the player was offline before their
  // horses are sent, so the client shows them as adults from the start rather
  // than caching a foal it won't re-render on a later type change.
  _serverInstance.GetHorseSystem().PromoteMaturedFoals(userCharacterUid);

  // Get the character record and fill the protocol data.
  // Also get the UID of the horse mounted by the character.
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    userCharacterUid);
  if (not characterRecord)
    throw std::runtime_error("Character record unavailable");

  // LOA-fix (R10-1, round10): СУТОЧНЫЙ СБРОС ДЕЙЛИКОВ ДЕЛАЕМ ЗДЕСЬ, НА ЛОГИНЕ.
  // Раньше он жил в RanchDirector::HandleEnterRanch — то есть срабатывал ПОСЛЕ
  // того, как клиент уже получил и закешировал список дневных целей ответом
  // AcCmdCLRequestDailyQuestListOK (0x357). Протокол не умеет сказать клиенту
  // «набор сброшен» (0x35c/0x35d двигают ОДИН квест), поэтому игрок весь день
  // смотрел на вчерашние цели, которые сервером уже стёрты: прогресс не
  // капает, «Взять цель дня» молчит. Здесь снапшота ещё не было ни одного —
  // это первая точка после логина, где персонаж уже известен.
  // Тот же класс бага и то же лечение, что у PromoteMaturedFoals выше:
  // приводим данные в актуальное состояние ДО первой отправки клиенту.
  _serverInstance.GetRanchDirector().ResetDailyQuestsIfNeeded(userCharacterUid);

  protocol::LobbyCommandLoginOK response{
    .lobbyTime = util::TimePointToFileTime(util::Clock::now()),
    // .member0 = 0xCA794,
    .val3 = 0x0,

    .missions = {
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x18,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
          .id = 2,
          .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x1F,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x23,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x29,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x2A,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x2B,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x2C,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x2D,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x2E,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},
      protocol::LobbyCommandLoginOK::Mission{
        .id = 0x2F,
        .progress = {
          protocol::LobbyCommandLoginOK::Mission::Progress{
            .id = 2,
            .value = 1}}},},

    .ranchAddress = lobbyConfig.advertisement.ranch.address.to_uint(),
    .ranchPort = lobbyConfig.advertisement.ranch.port,
    .scramblingConstant = 0,

    // .managementSkills = {4, 0x2B, 4},
    // .skillRanks = {.values = {{1,1}}},
    // .val14 = 0xca1b87db,
    // .guild = {.val1 = 1},
    // .val16 = 4,
    // .val18 = 0x2a,
    // .val19 = 0x38d,
    //.val20 = 0x1c7
  };

  // Populate system content values
  response.systemContent.values = _serverInstance.GetSystemContentRegistry().GetSystemContent();
  
  data::Uid characterMountUid{
    data::InvalidUid};

  characterRecord.Immutable(
    [this, justCreatedCharacter = clientContext.justCreatedCharacter, &response, &characterMountUid](const data::Character& character)
    {
      response.uid = character.uid();
      response.name = character.name();
      response.introduction = character.introduction();
      // TODO: implement the storing of character creation date
      // response.characterCreationDate = util::TimePointToAliciaTime(character.creationDate()),

      // todo: model constant
      response.gender = character.parts.modelId() == 10
        ? protocol::Gender::Boy
        : protocol::Gender::Girl;

      response.level = static_cast<uint16_t>(character.level());
      response.levelProgress = character.experience();
      response.carrots = character.carrots();
      response.role = std::bit_cast<protocol::LobbyCommandLoginOK::Role>(
        character.role());

      if (not justCreatedCharacter)
        response.bitfield = protocol::LobbyCommandLoginOK::HasPlayedBefore;

      const auto equipmentItems = _serverInstance.GetDataDirector().GetItemCache().Get(
        character.characterEquipment());
      if (not equipmentItems)
        throw std::runtime_error("Equipment items unavailable");

      protocol::BuildProtocolItems(
        response.equipmentItems,
        *equipmentItems);

      const auto expiredItems = _serverInstance.GetDataDirector().GetItemCache().Get(
        character.expiredEquipment());
      if (not expiredItems)
        throw std::runtime_error("Expired items unavailable");

      protocol::BuildProtocolItems(
        response.expiredItems,
        *expiredItems);

      protocol::BuildProtocolCharacter(
        response.character,
        character);

      // LOA (batch2): populate care-skill state from the character so learned
      // skills + class/points survive relog. Mirrors the login model
      // ManagementSkills{val0=class, progress, points} + SkillRanks{{id,rank}}.
      response.managementSkills.val0 = character.careSkills.careClassLevel();
      response.managementSkills.progress = character.careSkills.careProgress();
      response.managementSkills.points = static_cast<uint16_t>(
        character.careSkills.carePoints());
      response.skillRanks.values.clear();
      for (const auto& learned : character.careSkills.learnedRanks())
      {
        auto& skill = response.skillRanks.values.emplace_back();
        skill.id = learned.id;
        skill.rank = learned.rank;
      }

      if (character.guildUid() != data::InvalidUid)
      {
        const auto guildRecord = _serverInstance.GetDataDirector().GetGuild(
          character.guildUid());
        if (not guildRecord)
          throw std::runtime_error("Character's guild not available");

        std::vector<uint32_t> guildMembers;
        guildRecord.Immutable([&response, &guildMembers](const data::Guild& guild)
        {
          guildMembers = guild.members();
          protocol::BuildProtocolGuild(response.guild, guild);
          const bool isOwner = guild.owner() == response.uid;
          const bool isOfficer = std::ranges::contains(guild.officers(), response.uid);
          const bool isMember = std::ranges::contains(guild.members(), response.uid);

          if (isOwner)
            response.guild.guildRole = protocol::GuildRole::Owner;
          else if (isOfficer)
            response.guild.guildRole = protocol::GuildRole::Officer;
          else if (isMember)
            response.guild.guildRole = protocol::GuildRole::Member;
          else
            throw std::runtime_error("Character is in a guild but not a member");
        });

        // FIXME: a patch to preload characters in the guild to memory
        // so the guild members list can compile and display fully
        for (const auto& guildMember : guildMembers)
        {
          // Just get character and don't do anything with it
          _serverInstance.GetDataDirector().GetCharacterCache().Get(guildMember, true);
        }
      }

      if (character.petUid() != data::InvalidUid)
      {
        const auto petRecord =  _serverInstance.GetDataDirector().GetPet(
          character.petUid());
        if (not petRecord)
          throw std::runtime_error("Character's pet not available");

        petRecord.Immutable([&response](const data::Pet& pet)
        {
          protocol::BuildProtocolPet(response.pet, pet);
        });
      }

      if (character.settingsUid() != data::InvalidUid)
      {
        const auto settingsRecord = _serverInstance.GetDataDirector().GetSettingsCache().Get(
          character.settingsUid());
        if (not settingsRecord)
          throw std::runtime_error("Character's settings not available");

        settingsRecord->Immutable([&response](const data::Settings& settings)
        {
          // We set the age despite if the hide age is set,
          // just so the user is able to see the last value set by them.
          response.settings.age = static_cast<uint8_t>(settings.age());
          response.settings.hideAge = static_cast<uint8_t>(settings.hideAge());

          protocol::BuildProtocolSettings(response.settings, settings);
        });
      }

      characterMountUid = character.mountUid();
    });

  // Get the mounted horse record and fill the protocol data.
  const auto mountRecord = _serverInstance.GetDataDirector().GetHorseCache().Get(
    characterMountUid);
  if (not mountRecord)
    throw std::runtime_error("Horse mount record unavailable");

  mountRecord->Immutable(
    [&response](const data::Horse& horse)
    {
      //    response.val17 = {
      //      .mountUid = horse.uid(),
      //      .tid = 0x12,
      //      .val2 = 0x16e67e4};

      protocol::BuildProtocolHorse(response.horse, horse);
    });

  constexpr std::string_view PlayersOnlinePlaceholder = "{players_online}";

  std::string notice = _serverInstance.GetSettings().general.notice;
  if (const auto placeholder = notice.find(PlayersOnlinePlaceholder); placeholder != std::string::npos)
  {
    notice = notice.replace(
      placeholder,
      PlayersOnlinePlaceholder.length(),
      std::format(
        "{}", _serverInstance.GetLobbyDirector().GetUserCount()));
  }

  if (!notice.empty())
  {
    response.notice = notice;
  }
  protocol::LobbyCommandLoginOK::TrainingProgression::MapProgressInfo mapProgressInfo{
    .mapBlockId= 1,
    .gameMode = protocol::GameMode::Speed,
    .clearStage = protocol::LobbyCommandLoginOK::TrainingProgression::MapProgressInfo::ClearStage::None,
  };

  response.trainingProgression.mapProggressInfos = {
    mapProgressInfo};

  _commandServer.SetCode(clientId, {});

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  protocol::AcCmdLCSkillCardPresetList skillPresetListResponse{};
  characterRecord.Immutable([&skillPresetListResponse](const data::Character& character)
  {
    const auto& speed = character.skills.speed();
    skillPresetListResponse.speedActiveSetId = static_cast<uint8_t>(
      speed.activeSetId);

    const auto& magic = character.skills.magic();
    skillPresetListResponse.magicActiveSetId = static_cast<uint8_t>(
      magic.activeSetId);

    skillPresetListResponse.skillSets = {
      protocol::SkillSet{.setId = 0, .gamemode = protocol::GameMode::Speed, .skills = {speed.set1.slot1, speed.set1.slot2}},
      protocol::SkillSet{.setId = 1, .gamemode = protocol::GameMode::Speed, .skills = {speed.set2.slot1, speed.set2.slot2}},
      protocol::SkillSet{.setId = 0, .gamemode = protocol::GameMode::Magic, .skills = {magic.set1.slot1, magic.set1.slot2}},
      protocol::SkillSet{.setId = 1, .gamemode = protocol::GameMode::Magic, .skills = {magic.set2.slot1, magic.set2.slot2}}
    };
  });

  _commandServer.QueueCommand<decltype(skillPresetListResponse)>(
    clientId,
    [skillPresetListResponse]()
    {
      return skillPresetListResponse;
    });
}

void LobbyNetworkHandler::SendLoginCancel(
  const ClientId clientId,
  const protocol::AcCmdCLLoginCancel::Reason reason)
{
  _commandServer.QueueCommand<protocol::AcCmdCLLoginCancel>(
    clientId,
    [reason]()
    {
      return protocol::AcCmdCLLoginCancel{
      .reason = reason };
    });
}

void LobbyNetworkHandler::HandleRoomList(
  const ClientId clientId,
  const protocol::AcCmdCLRoomList& command)
{
  constexpr uint32_t RoomsPerPage = 9;

  protocol::LobbyCommandRoomListOK response{
    .gameMode = command.gameMode,
    .teamMode = command.teamMode};

  // todo: update every x tick
  std::vector<server::Room::Snapshot> roomSnapshots{};
  std::ranges::copy_if(
    _serverInstance.GetRoomSystem().GetRoomsSnapshot(),
    std::back_inserter(roomSnapshots),
    [&command](const server::Room::Snapshot& roomSnapshot)
    {
      return
        roomSnapshot.details.gameMode == static_cast<server::Room::GameMode>(command.gameMode) &&
        roomSnapshot.details.teamMode == static_cast<server::Room::TeamMode>(command.teamMode);
    });

  // Sort race rooms
  // Priority ordering (ascending by player count):
  // - Unlocked and waiting
  // - Unlocked and racing
  // - Locked
  std::ranges::sort(
    roomSnapshots,
    [](const server::Room::Snapshot& a, const server::Room::Snapshot& b)
    {
      const auto getPriority = [](const server::Room::Snapshot& snapshot)
      {
        if (!snapshot.details.password.empty())
          // Locked
          return 2;
        if (snapshot.isPlaying)
          // Playing
          return 1;
        else
          // Waiting
          return 0;
      };

      const auto pA = getPriority(a);
      const auto pB = getPriority(b);

      // Check if snapshot A shares the same priority as snapshot B
      if (pA != pB)
        // Priorities are different, return
        // compared weight
        return pA < pB;

      // Both snapshots share the same priority, so
      // sort by player count.
      return a.playerCount < b.playerCount; // Ascending by player count
    });

  const auto roomChunks = std::views::chunk(
    roomSnapshots,
    RoomsPerPage);

  if (not roomChunks.empty())
  {
    // Clamp the page index to the last chunk
    const auto pageIndex = std::max(
      std::min(
        roomChunks.size() - 1,
        static_cast<size_t>(command.page)),
      size_t{0});

    // Set the response page index based on chunk index
    response.page = static_cast<uint8_t>(pageIndex);

    for (const auto& room : roomChunks[pageIndex])
    {
      auto& roomResponse = response.rooms.emplace_back();

      roomResponse.state = room.isPlaying ? 
        protocol::LobbyCommandRoomListOK::Room::State::Playing :
        protocol::LobbyCommandRoomListOK::Room::State::Waiting;

      roomResponse.uid = room.uid;
      if (not room.details.password.empty())
      {
        roomResponse.isLocked = true;
      }

      roomResponse.playerCount = static_cast<uint8_t>(room.playerCount);
      roomResponse.maxPlayerCount = static_cast<uint8_t>(room.details.maxPlayerCount);
      // todo: skill bracket
      roomResponse.skillBracket = protocol::LobbyCommandRoomListOK::Room::SkillBracket::Experienced;
      roomResponse.name = room.details.name;
      roomResponse.map = room.details.courseId;
    }
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleHeartbeat(
  const ClientId clientId)
{
  // LOA-fix (R64-3, round64, backlog #215): три поля — ОДНОЙ мутацией под
  // замком. ★Их важно применять вместе: обе метки гаснут именно потому, что
  // пришёл реальный пульс, и разрыв этой тройки между отдельными захватами
  // оставил бы окно, где пульс уже засчитан, а эпизоды отсрочек ещё открыты.
  // ★Если клиента уже нет — молча ничего не делаем: пульс от исчезнувшего
  // клиента не повод бросать, прежний код бросал лишь потому, что иначе не умел.
  MutateClientContext(
    clientId,
    [](ClientContext& clientContext)
    {
      clientContext.lastHeartbeat = std::chrono::steady_clock::now();

      // LOA-fix (R12-6, round12, backlog #85): реальный пульс закрывает эпизод
      // грейса — следующая загрузка карты получит свежий потолок 210 c (R12-4b).
      clientContext.raceLoadingGraceSince = {};

      // LOA-fix (R21-4c, round21, backlog #95): тем же движением закрываем эпизод
      // РАНЧ-отсрочки. Поле логовое, ничего не ограничивает: сброс нужен, чтобы
      // следующий эпизод снова напечатался info, а не потерялся в debug.
      clientContext.ranchGraceSince = {};
    });
}

void LobbyNetworkHandler::HandleMakeRoom(
  ClientId clientId,
  const protocol::AcCmdCLMakeRoom& command)
{
  const auto clientContext = GetClientContext(clientId);
  uint32_t createdRoomUid{0};

  const auto moderationVerdict = _serverInstance.GetModerationSystem().Moderate(
    command.name);
  if (moderationVerdict.isPrevented)
  {
    protocol::AcCmdCLMakeRoomCancel response{};
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  _serverInstance.GetRoomSystem().CreateRoom(
    [&createdRoomUid, &command, characterUid = clientContext.characterUid](
      Room& room)
    {
      const bool isTraining = command.playerCount == 1;

      // Only allow an empty room name in training/tutorial rooms.
      // todo: better way to detect this?
      if (command.name.empty() && not isTraining)
        return;

      room.GetRoomDetails().name = command.name;
      room.GetRoomDetails().password = command.password;
      room.GetRoomDetails().missionId = command.missionId;
      // todo: validate mission id

      room.GetRoomDetails().maxPlayerCount = std::max(
        std::min(command.playerCount,uint8_t{8}),
        uint8_t{0});

      switch (command.gameMode)
      {
        case protocol::GameMode::Speed:
          room.GetRoomDetails().gameMode = Room::GameMode::Speed;
          break;
        case protocol::GameMode::Magic:
          room.GetRoomDetails().gameMode = Room::GameMode::Magic;
          break;
        case protocol::GameMode::Tutorial:
          room.GetRoomDetails().gameMode = Room::GameMode::Tutorial;
          break;
        default:
          server::util::QuietLogError("Unknown game mode '{}'", static_cast<uint32_t>(command.gameMode));
      }

      switch (command.teamMode)
      {
        case protocol::TeamMode::FFA:
          room.GetRoomDetails().teamMode = Room::TeamMode::FFA;
          break;
        case protocol::TeamMode::Team:
          room.GetRoomDetails().teamMode = Room::TeamMode::Team;
          break;
        case protocol::TeamMode::Single:
          room.GetRoomDetails().teamMode = Room::TeamMode::Single;
          break;
        default:
          server::util::QuietLogError("Unknown team mode '{}'", static_cast<uint32_t>(command.gameMode));
      }

      room.GetRoomDetails().npcDifficulty = command.unk3;
      room.GetRoomDetails().skillBracket = command.unk4;
      // default to all courses
      room.GetRoomDetails().courseId = 10002;

      // Queue the master as a player.
      room.QueuePlayer(characterUid);
      createdRoomUid = room.GetUid();
    });

  if (createdRoomUid == 0)
  {
    protocol::AcCmdCLMakeRoomCancel response{};
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });

    return;
  }

  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [this, createdRoomUid, &command](const data::Character& character)
    {
      const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
        character.uid()).userName;
      server::util::QuietLogInfo("Room {} created by '{}' with the name '{}'", createdRoomUid, userName, command.name);
    });

  size_t identityHash = std::hash<uint32_t>()(clientContext.characterUid);
  boost::hash_combine(identityHash, createdRoomUid);

  const auto roomOtp = _serverInstance.GetOtpSystem().GrantCode(
    identityHash);

  const auto lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();
  protocol::AcCmdCLMakeRoomOK response{
    .roomUid = createdRoomUid,
    .oneTimePassword = roomOtp,
    .raceServerAddress = lobbyConfig.advertisement.race.address.to_uint(),
    .raceServerPort = lobbyConfig.advertisement.race.port,
    .unk2 = command.unk4};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  _serverInstance.GetLobbyDirector().GetScheduler().Queue(
    [this, userName = clientContext.userName, createdRoomUid]()
    {
      _serverInstance.GetLobbyDirector().SetUserRoom(userName, createdRoomUid);
    });
}

void LobbyNetworkHandler::HandleEnterRoom(
  const ClientId clientId,
  const protocol::AcCmdCLEnterRoom& command)
{
  const auto clientContext = GetClientContext(clientId);

  // Whether the room is valid.
  bool isRoomValid = true;
  // Whether the user is authorized to enter.
  bool isAuthorized = false;
  // Whether the room is full.
  bool isRoomFull = false;

  try
  {
    _serverInstance.GetRoomSystem().GetRoom(
      command.roomUid,
      [&isAuthorized, &isRoomFull, &command, characterUid = clientContext.characterUid](
        Room& room)
      {
        const auto& roomPassword = room.GetRoomDetails().password;
        if (not roomPassword.empty())
          isAuthorized = roomPassword == command.password;
        else
          isAuthorized = true;

        isRoomFull = room.IsRoomFull();
        if (isRoomFull)
          return;

        room.QueuePlayer(characterUid);
      });
  }
  catch (const std::exception&)
  {
    // The client requested to join a room which no longer exists.
    // We do care in this case.
    isRoomValid = false;
  }

  if (not isRoomValid)
  {
    protocol::AcCmdCLEnterRoomCancel response{
      .status = protocol::AcCmdCLEnterRoomCancel::Status::CR_INVALID_ROOM};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  if (not isAuthorized)
  {
    protocol::AcCmdCLEnterRoomCancel response{};

    switch (command.enterRoomType)
    {
      case protocol::AcCmdCLEnterRoom::EnterRoomType::RoomList:
        // Respond with a cancel indicating bad password
        response.status = protocol::AcCmdCLEnterRoomCancel::Status::CR_BAD_PASSWORD;
        break;
      case protocol::AcCmdCLEnterRoom::EnterRoomType::TournamentInvite:
      case protocol::AcCmdCLEnterRoom::EnterRoomType::RoomCode:
        // Indicates to the player that the room is locked and shows the password popup
        response.status = protocol::AcCmdCLEnterRoomCancel::Status::ShowRoomPassword;
        break;
      default:
        server::util::QuietLogWarn(
          "Unknown AcCmdCLEnterRoom::EnterRoomType type '{}'",
          static_cast<uint32_t>(command.enterRoomType));
        break;
    }

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  if (isRoomFull)
  {
    protocol::AcCmdCLEnterRoomCancel response{
      .status = protocol::AcCmdCLEnterRoomCancel::Status::CR_CROWDED_ROOM};

    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
    return;
  }

  size_t identityHash = std::hash<uint32_t>()(clientContext.characterUid);
  boost::hash_combine(identityHash, command.roomUid);

  const auto roomOtp = _serverInstance.GetOtpSystem().GrantCode(
    identityHash);

  const auto& lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();

  protocol::AcCmdCLEnterRoomOK response{
    .roomUid = command.roomUid,
    .oneTimePassword = roomOtp,
    .raceServerAddress = lobbyConfig.advertisement.race.address.to_uint(),
    .raceServerPort = lobbyConfig.advertisement.race.port,
    .member6 = 1};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  _serverInstance.GetLobbyDirector().GetScheduler().Queue(
    [this, userName = clientContext.userName, characterUid = clientContext.characterUid, roomUid = command.roomUid]()
    {
      bool hasEnteredRaceRoom = false;

      if (_serverInstance.GetRoomSystem().RoomExists(roomUid))
      {
        _serverInstance.GetRoomSystem().GetRoom(
          roomUid,
          [&hasEnteredRaceRoom, characterUid](Room& room)
          {
            const bool playerDequeued = room.DequeuePlayer(characterUid);

            // If the player was dequeued that means they did not enter the room.
            hasEnteredRaceRoom = not playerDequeued;
          });
      }

      if (hasEnteredRaceRoom)
        _serverInstance.GetLobbyDirector().SetUserRoom(userName, roomUid);
    },
    Scheduler::Clock::now() + std::chrono::seconds(7));
}

void LobbyNetworkHandler::HandleLeaveRoom(
  const ClientId clientId)
{
  const auto clientContext = GetClientContext(clientId);
  _serverInstance.GetLobbyDirector().GetScheduler().Queue(
    [this, userName = clientContext.userName]()
    {
      _serverInstance.GetLobbyDirector().SetUserRoom(userName, 0);
    });
}

void LobbyNetworkHandler::HandleEnterChannel(
  const ClientId clientId,
  const protocol::AcCmdCLEnterChannel& command)
{
  // todo: implement channels
  protocol::AcCmdCLEnterChannelOK response{
    .unk0 = command.channel,
    .unk1 = 557,
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleLeaveChannel(
  const ClientId clientId,
  const protocol::AcCmdCLLeaveChannel&)
{
  // todo: implement channels
  protocol::AcCmdCLLeaveChannelOK response{};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::SendCreateNicknameNotify(ClientId clientId)
{
  protocol::LobbyCommandCreateNicknameNotify notify{};

  // LOA-fix (R64-3, round64, backlog #215): флаг создателя персонажа — под
  // замком, а отправка уведомления ниже — уже без него.
  // ★Этот флаг читает ТИК (он даёт молчащему в создателе клиенту 15 минут
  // вместо 60 секунд), то есть его пишет один поток, а читает другой — ровно
  // тот стык, ради которого раунд и делается.
  MutateClientContext(
    clientId,
    [](ClientContext& clientContext)
    {
      clientContext.isInCharacterCreator = true;
    });

  _commandServer.QueueCommand<decltype(notify)>(
    clientId,
    [notify]()
    {
      return notify;
    });
}

void LobbyNetworkHandler::HandleCreateNickname(
  const ClientId clientId,
  const protocol::AcCmdCLCreateNickname& command)
{
  // LOA-fix (R64-3, round64, backlog #215): КОПИЯ для чтения; мутации ниже —
  // отдельной операцией под замком. Между ними идут выходы в чужой код
  // (модерация никнейма), поэтому единой ссылки здесь быть не должно.
  const auto clientContext = GetClientContext(clientId);

  constexpr uint32_t DefaultHorseTid = 20001;

  if (command.requestedHorseTid != DefaultHorseTid)
  {
    server::util::QuietLogWarn("Client {} ('{}') requested to create a character with an invalid horse TID '{}'",
      clientId,
      clientContext.userName,
      command.requestedHorseTid);

    SendCreateNicknameCancel(
      clientId,
      protocol::AcCmdCLCreateNicknameCancel::Reason::ServerError);
    return;
  }

  if (not locale::IsNameValid(command.nickname, 18)
    || _serverInstance.GetModerationSystem().Moderate(command.nickname).isPrevented)
  {
    SendCreateNicknameCancel(
      clientId,
      protocol::AcCmdCLCreateNicknameCancel::Reason::InvalidCharacterName);
    return;
  }

  // LOA-fix (R64-3, round64, backlog #215): пять полей — ОДНОЙ мутацией под
  // замком, уже ПОСЛЕ выходов в чужой код (модерация никнейма выше).
  // ★Тройку «пульс + две метки» разрывать нельзя по той же причине, что в
  // HandleHeartbeat: они закрывают один логический эпизод, и раздельные захваты
  // оставили бы окно, где создатель персонажа уже покинут, а отсрочки открыты.
  MutateClientContext(
    clientId,
    [](ClientContext& clientContext)
    {
      clientContext.justCreatedCharacter = true;

      // We update the last heartbeat too, so that the client does not get
      // kicked immediately after `isInCharacterCreator` immunity is withdrawn.
      clientContext.lastHeartbeat = std::chrono::steady_clock::now();
      // LOA-fix (R12-8, round12, backlog #85): пульс штампуется — гасим и метку
      // грейса (R12-5), чтобы инвариант «метка живёт только внутри эпизода
      // загрузки» держался во всех точках, а не только в HandleHeartbeat.
      clientContext.raceLoadingGraceSince = {};
      // LOA-fix (R21-4e, round21, backlog #95): и метку ранч-отсрочки — выход из
      // создателя персонажа штампует пульс, эпизод логически закрыт (зеркало R12-8).
      clientContext.ranchGraceSince = {};
      clientContext.isInCharacterCreator = false;
    });

  const auto userRecord = _serverInstance.GetDataDirector().GetUserCache().Get(
    clientContext.userName);
  if (not userRecord)
    throw std::runtime_error("User record does not exist");

  auto userCharacterUid{data::InvalidUid};
  userRecord->Immutable([&userCharacterUid](const data::User& user)
  {
    userCharacterUid = user.characterUid();
  });

  std::optional<Record<data::Character>> userCharacter;

  if (userCharacterUid == data::InvalidUid)
  {
    const bool isNameUnique = _serverInstance.GetDataDirector().GetDataSource().IsCharacterNameUnique(
      command.nickname);

    if (not isNameUnique)
    {
      SendCreateNicknameCancel(
        clientId,
        protocol::AcCmdCLCreateNicknameCancel::Reason::DuplicateCharacterName);
      return;
    }

    // Create a new mount for the character.
    const auto mountRecord  = _serverInstance.GetDataDirector().CreateHorse();
    if (not mountRecord)
    {
      throw std::runtime_error(
        std::format("Failed to create horse for user '{}'", clientContext.userName));
    }

    auto mountUid = data::InvalidUid;
    mountRecord.Mutable(
      [this, &mountUid, requestedHorseTid = command.requestedHorseTid](data::Horse& horse)
      {
        // The TID of the horse specifies which body mesh is used for that horse.
        // Can be found in the `MountPartInfo` table.
        registry::HorseRegistry::BuildDefaultHorse(horse, requestedHorseTid);
        mountUid = horse.uid();
      });

    // Create the new character.
    userCharacter = _serverInstance.GetDataDirector().CreateCharacter();
    if (not userCharacter)
    {
      throw std::runtime_error(
        std::format("Failed to create character for user '{}'", clientContext.userName));
    }

    userCharacter->Mutable(
      [&userCharacterUid,
        &mountUid,
        &command](data::Character& character)
      {
        if (character.name().empty())
          character.name = command.nickname;

        // todo: default level configured
        character.level = 40;
        character.experience() = 557300;
        // todo: default carrots configured
        character.carrots = 200'000;

        character.mountUid() = mountUid;

        constexpr uint8_t StartingHorseSlotCount = 5; 
        character.horseSlotCount() = StartingHorseSlotCount;

        // Create the default friend group.
        character.contacts.groups().try_emplace(0);

        userCharacterUid = character.uid();
      });

    // Assign the character to the user.
    userRecord->Mutable(
      [&userCharacterUid](data::User& user)
      {
        user.characterUid() = userCharacterUid;
      });

    _serverInstance.GetLobbyDirector().GetScheduler().Queue(
      [this, userCharacterUid, userName = clientContext.userName]()
      {
        // ★ЕДИНСТВЕННОЕ МЕСТО, КОТОРОЕ ПИСАЛО ЧЕРЕЗ ВОЗВРАЩЁННУЮ ССЫЛКУ.
        // Возврат копии молча потерял бы эту запись, поэтому запись переехала
        // внутрь директора, под исключительный замок.
        _serverInstance.GetLobbyDirector().SetUserCharacterUid(
          userName, userCharacterUid);
      });
  }
  else
  {
    // Retrieve the existing character.
    userCharacter = _serverInstance.GetDataDirector().GetCharacter(
      userCharacterUid);
  }

  assert(userCharacter.has_value());

  // Update the character's parts and appearance.
  userCharacter->Mutable(
    [&command](data::Character& character)
    {
      character.parts = data::Character::Parts{
        .modelId = command.character.parts.charId,
        .mouthId = command.character.parts.mouthSerialId,
        .faceId = command.character.parts.faceSerialId};
      character.appearance = data::Character::Appearance{
        .voiceId = command.character.appearance.voiceId,
        .headSize = command.character.appearance.headSize,
        .height = command.character.appearance.height,
        .thighVolume = command.character.appearance.thighVolume,
        .legVolume = command.character.appearance.legVolume,
        .emblemId = command.character.appearance.emblemId,
      };
    });

  // Log for moderation
  server::util::QuietLogInfo("User '{}' created a character ({}) with the name '{}'",
    clientContext.userName,
    userCharacterUid,
    command.nickname);

  SendLoginOK(clientId);
}

void LobbyNetworkHandler::SendCreateNicknameCancel(
  const ClientId clientId,
  const protocol::AcCmdCLCreateNicknameCancel::Reason reason)
{
  _commandServer.QueueCommand<protocol::AcCmdCLCreateNicknameCancel>(
    clientId, [reason]()
    {
      return protocol::AcCmdCLCreateNicknameCancel{.error = reason};
    });
}

void LobbyNetworkHandler::HandleShowInventory(
  const ClientId clientId,
  const protocol::AcCmdCLShowInventory&)
{
  const auto clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  if (not characterRecord)
    throw std::runtime_error("Character record unavailable");

  std::vector<protocol::LobbyCommandShowInventoryOK> responses{};

  characterRecord.Immutable(
    [this, &responses](const data::Character& character)
    {
      // 0xFA (250) is the protocol max per response
      constexpr uint32_t ItemsPerResponse = 250;
      const auto itemRecords = _serverInstance.GetDataDirector().GetItemCache().Get(
        character.inventory());
      const auto horseRecords = _serverInstance.GetDataDirector().GetHorseCache().Get(
        character.horses());
      // LOA-fix (R23, backlog #98): a cold/unavailable cache returns nullopt; the
      // std::views::chunk(*itemRecords)/(*horseRecords) derefs below would crash on a
      // disengaged optional. Check BOTH up front so a cold cache yields an EMPTY
      // response, never a partial one (item chunks are built before the horse section).
      if (not itemRecords || not horseRecords)
        return;

      // Produce chunked responses, by ItemsPerResponse
      const auto itemChunks = std::views::chunk(
        *itemRecords,
        ItemsPerResponse);
      
      // Create a response per chunk
      for (const auto& chunk : itemChunks)
      {
        auto& response = responses.emplace_back();
        for (const auto& item : chunk)
        {
          auto& protocolItem = response.items.emplace_back();
          item.Immutable([&protocolItem](const auto& item)
          {
            protocol::BuildProtocolItem(protocolItem, item);
          });
        }
      }

      // Create a separate response for horses
      // 0x0A (10) is the protocol max per response
      constexpr uint32_t HorsesPerResponse = 10;
      // Produce chunked responses, by HorsesPerResponse
      const auto horseChunks = std::views::chunk(
        *horseRecords,
        HorsesPerResponse);

      // Create a response per chunk
      for (const auto& horseChunk : horseChunks)
      {
        auto& response = responses.emplace_back();
        for (const auto& horse : horseChunk)
        {
          auto& protocolHorse = response.horses.emplace_back();
          horse.Immutable([&protocolHorse](const auto& horse)
          {
            protocol::BuildProtocolHorse(protocolHorse, horse);
          });
        }
      }
    });

  // If the character has no items or extra horses
  // then construct an empty response.
  // This is needed to prevent the client from soft-locking
  // and waiting for a response from the server.
  if (responses.empty())
    responses.emplace_back();

  for (auto response : responses)
  {
    _commandServer.QueueCommand<decltype(response)>(
      clientId,
      [response]()
      {
        return response;
      });
  }
}

void LobbyNetworkHandler::HandleUpdateUserSettings(
  const ClientId clientId,
  const protocol::AcCmdCLUpdateUserSettings& command)
{
  const auto clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
   clientContext.characterUid);

  auto settingsUid = data::InvalidUid;
  characterRecord.Immutable([&settingsUid](const data::Character& character)
  {
    settingsUid = character.settingsUid();
  });

  const bool wasCreated = settingsUid == data::InvalidUid;
  const auto settingsRecord = settingsUid != data::InvalidUid
    ? _serverInstance.GetDataDirector().GetSettings(settingsUid)
    : _serverInstance.GetDataDirector().CreateSettings();

  if (not settingsRecord)
  {
    throw std::runtime_error(
      std::format("Failed to create or retrieve settings for user '{}'", clientContext.userName));
  }

  settingsRecord.Mutable([&settingsUid, &command](data::Settings& settings)
  {
    // Copy the keyboard bindings if present in the command.
    if (command.settings.typeBitset.test(protocol::Settings::Keyboard))
    {
      settings.keyboardBindings().emplace();

      for (const auto& protocolBinding : command.settings.keyboardOptions.bindings)
      {
        auto& bindingRecord = settings.keyboardBindings()->emplace_back();

        bindingRecord.type = protocolBinding.type;
        bindingRecord.primaryKey = protocolBinding.primaryKey;
        bindingRecord.secondaryKey = protocolBinding.secondaryKey;
      }
    }

    // Copy the gamepad bindings if present in the command.
    if (command.settings.typeBitset.test(protocol::Settings::Gamepad))
    {
      settings.gamepadBindings().emplace();

      auto protocolBindings = command.settings.gamepadOptions.bindings;

      // The last binding is invalid, sends type 2 and overwrites real settings
      if (!protocolBindings.empty())
       protocolBindings.pop_back();

      for (const auto& protocolBinding : protocolBindings)
      {
        auto& bindingRecord = settings.gamepadBindings()->emplace_back();

        bindingRecord.type = protocolBinding.type;
        bindingRecord.primaryKey = protocolBinding.primaryButton;
        bindingRecord.secondaryKey = protocolBinding.secondaryButton;
      }
    }

    // Copy the macros if present in the command.
    if (command.settings.typeBitset.test(protocol::Settings::Macros))
    {
      settings.macros() = command.settings.macroOptions.macros;
    }

    settingsUid = settings.uid();
  });

  if (wasCreated)
  {
    characterRecord.Mutable([&settingsUid](data::Character& character)
    {
      character.settingsUid() = settingsUid;
    });
  }

  // We explicitly do not update the `age` and `hideAge` members,
  // as the client uses dedicated `AcCmdCRChangeAge` and `AcCmdCRHideAge` commands instead.

  protocol::AcCmdCLUpdateUserSettingsOK response{};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleEnterRoomQuick(
  const ClientId clientId,
  const protocol::AcCmdCLEnterRoomQuick& command)
{
  const auto clientContext = GetClientContext(clientId);

  const bool hasQueued = _serverInstance.GetMatchmakingSystem().Queue(
    clientContext.characterUid,
    command.gameMode,
    command.teamMode);

  if (hasQueued)
    // Queued successfully, no need to send a response
    return;

  // This character was not queued in the matchmaking system
  const protocol::AcCmdCLEnterRoomQuickCancel cancel{};
  _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
}

void LobbyNetworkHandler::HandleGoodsShopList(
  const ClientId clientId,
  const protocol::AcCmdCLGoodsShopList&)
{
  auto shopList = _serverInstance.GetLobbyDirector().GetShopManager().GetSerializedShopList();

  std::vector<std::byte> compressedXml;
  compressedXml.resize(shopList.size());

  uLongf compressedSize = static_cast<uLongf>(compressedXml.size());
  compress2(
    reinterpret_cast<Bytef*>(compressedXml.data()),
    &compressedSize,
    reinterpret_cast<const Bytef*>(shopList.c_str()),
    static_cast<uLongf>(shopList.length()),
    9);

  compressedXml.resize(compressedSize);

  // TODO: remove this, only used for testing protocol
  auto now = util::Clock::now() + std::chrono::days(1);

  //! Chunk size as defined in command handler.
  constexpr auto ChunkSize = 7168;

  // Fragment shop data and send it in parts for the client to reconstruct and store.
  const auto& dataParts = std::views::chunk(compressedXml, ChunkSize);
  const auto chunkCount = dataParts.size();
  const auto chunkedPartsSize = chunkCount * ChunkSize;

  //! Max shop data size as defined in the command handler.
  constexpr auto MaxShopDataSize = 0x8000;
  // Check if the total size of chunked parts exceed the size of limit defined in command handler
  if (chunkedPartsSize > MaxShopDataSize)
  {
    server::util::QuietLogError("Shop data chunking with {} chunks, totalling {} bytes, exceeds max game shop data size of {} bytes.",
      chunkCount,
      chunkedPartsSize,
      MaxShopDataSize);

    protocol::AcCmdCLGoodsShopListCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(
      clientId,
      [cancel]()
      {
        return cancel;
      });
    return;
  }

  // Send each chunk with the appropriate index and chunk count
  for (size_t index = 0; index < dataParts.size(); ++index)
  {
    const auto& dataPart = dataParts[index];
    protocol::AcCmdLCGoodsShopListData data{
      .timestamp = now,
      .index = static_cast<uint8_t>(index),
      .count = static_cast<uint8_t>(chunkCount),
      .data = std::vector<std::byte>(
        dataPart.cbegin(),
        dataPart.cend())};

    _commandServer.QueueCommand<decltype(data)>(
      clientId,
      [data]()
      {
        return data;
      });
  }

  // TODO: send date time that is now?
  protocol::AcCmdCLGoodsShopListOK response{
    .shopTimestamp = now
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleAchievementCompleteList(
  const ClientId clientId,
  const protocol::AcCmdCLAchievementCompleteList&)
{
  const auto clientContext = GetClientContext(clientId);
  auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCLAchievementCompleteListOK response{};

  uint32_t characterLevel = 0;
  // LOA-fix (R69, backlog #58): СНИМОК ЗАРАБОТАННЫХ ДОСТИЖЕНИЙ.
  //
  // Снимается ПОД ЗАМКОМ записи, а разбирается снаружи: под замком копируются
  // ровно два числа на запись, а обращение к реестру и сборка ответа идут уже
  // без него.
  struct EarnedAchievement
  {
    uint16_t tid;
    uint32_t progress;
  };
  std::vector<EarnedAchievement> earnedAchievements;

  characterRecord.Immutable(
    [&response, &characterLevel, &earnedAchievements](const data::Character& character)
    {
      response.unk0 = character.uid();
      characterLevel = character.level();

      // ★ПРИЗНАК «ПОЛУЧЕНО» — ВЗЯТЫЙ ТИР, А НЕ НЕНУЛЕВОЙ ПРОГРЕСС. Запись в
      // `character.achievements()` заводится при ПЕРВОМ же событии и живёт с
      // нулевым тиром до первого порога: судить по `progress > 0` значило бы
      // объявить полученным то, что только начато. Сам тир нигде не хранится —
      // он выводится из числа непустых отметок `tierEarnedAt` (см.
      // `data::Character::AchievementEntry`), поэтому «хотя бы одна отметка
      // непустая» и есть «хотя бы один тир взят». Пустая отметка = эпоха: так
      // её пишет и читает `FileDataSource` (ноль в json = тир не взят).
      for (const auto& achievementEntry : character.achievements())
      {
        bool anyTierEarned = false;
        for (const auto& tierEarnedAt : achievementEntry.tierEarnedAt)
        {
          if (tierEarnedAt != data::Clock::time_point{})
          {
            anyTierEarned = true;
            break;
          }
        }

        if (not anyTierEarned)
          continue;

        earnedAchievements.push_back(
          {.tid = achievementEntry.tid, .progress = achievementEntry.progress});
      }
    });

  // LOA-fix (achievements, вариант A): level-up достижения 20008-20012 из таблицы
  // Achievement (event id 75). Раньше слались БЕЗУСЛОВНО всем — клиент считал их
  // все полученными. Гейтим по уровню персонажа (level уже персистится): tid
  // добавляется, только если его порог уровня достигнут. Полный T6 (246 условий
  // с реальным начислением/попапом) — отдельный проект; это документированный
  // минимум, делающий список корректным по уровню.
  struct LevelUpAchievement { uint16_t tid; uint32_t requiredLevel; };
  static constexpr LevelUpAchievement kLevelUpAchievements[] = {
    {20008, 2}, {20009, 5}, {20010, 12}, {20011, 8}, {20012, 10}};
  for (const auto& achievement : kLevelUpAchievements)
  {
    if (characterLevel >= achievement.requiredLevel)
    {
      // LOA-fix (R44-1, #58/R0): статус ОБЯЗАН быть Finished. По умолчанию
      // protocol::Quest::status == InProgress(0), поэтому выданные достижения
      // уезжали клиенту как «в процессе»: список формально приходил, но ни одно
      // достижение не читалось как закрытое — а это единственное место во всей
      // игре, где достижение у нас реально что-то включает (диалоги NPC по
      // уровню). Значения enum: InProgress=0, ReadyToClaim=1, Finished=3.
      auto& completedAchievement = response.achievements.emplace_back();
      completedAchievement.tid = achievement.tid;
      completedAchievement.status = protocol::Quest::Status::Finished;
    }
  }

  // LOA-fix (R69, backlog #58): ЗАРАБОТАННЫЕ ДОСТИЖЕНИЯ — В СПИСОК.
  //
  // ДЕФЕКТ, КОТОРЫЙ ЭТО ЗАКРЫВАЕТ. Ответ 0xe6 состоял РОВНО из пяти уровневых
  // tid'ов и больше ни из чего. При этом подсистема достижений (R46/R47) уже
  // считает прогресс и проставляет тиры в `character.achievements()` — по проду
  // это ОДИННАДЦАТЬ разных tid'ов, до восьми у одного персонажа, — а окно
  // достижений показывало пять. Список врал: сервер знал больше, чем говорил.
  //
  // ★ЗАГЛУШКУ `neverAward` НЕ ОТДАЁМ НИКОГДА. Это заведомо невыполнимое
  // достижение (tid 20000): попади оно в список, клиент считал бы системную
  // книгу закрытой навсегда. `AchievementSystem` его тоже не двигает, но
  // правило здесь ВТОРОЕ и независимое: список строится из данных на диске, а
  // они переживут любую смену логики начисления.
  //
  // ★ДЕДУП ПО tid. Уровневая пятёрка выше и запись в данных умеют назвать один
  // и тот же tid; задвоенная строка — ложь о числе достижений.
  //
  // ★ПОТОЛОК ДЛИНЫ — ПРОТИВ ИСПОРЧЕННЫХ ДАННЫХ, А НЕ ПРОТИВ ИГРОКА. Тело
  // команды у клиента ограничено `protocol::BufferSize` за вычетом magic, одна
  // запись `Quest` весит на проводе 13 байт, шапка ответа — 6 (`unk0` +
  // счётчик). Каталог достижений — 246 записей, честный список в потолок не
  // упрётся никогда. Но читатель персиста принимает до 4096 записей, и такой
  // файл дал бы кадр в полсотни килобайт: переполнение вылезло бы НЕ ЗДЕСЬ, а
  // броском в потоке отправки (`CommandServer::SendCommand`) — класс #178.
  constexpr std::size_t kAchievementListHeaderSize = 6;
  constexpr std::size_t kAchievementListEntrySize = 13;
  constexpr std::size_t kMaxListedAchievements =
    (protocol::BufferSize - sizeof(protocol::MessageMagic)
      - kAchievementListHeaderSize) / kAchievementListEntrySize;

  const auto& achievementRegistry = _serverInstance.GetAchievementRegistry();
  for (const auto& earnedAchievement : earnedAchievements)
  {
    if (response.achievements.size() >= kMaxListedAchievements)
      break;

    // Запись, которой нет в каталоге, отдаётся как есть: каталог — серверное
    // зеркало клиентской таблицы, и его неполнота не отменяет того, что
    // достижение реально взято. Скрыть такую запись значило бы соврать в ту же
    // сторону, от которой раунд и лечит. Признак `neverAward` живёт В каталоге,
    // поэтому отсутствие записи в нём никогда не прячет заглушку.
    const auto* const achievementInfo = achievementRegistry.GetAchievement(
      earnedAchievement.tid);
    if (achievementInfo != nullptr and achievementInfo->neverAward)
      continue;

    bool alreadyListed = false;
    for (const auto& listedAchievement : response.achievements)
    {
      if (listedAchievement.tid == earnedAchievement.tid)
      {
        alreadyListed = true;
        break;
      }
    }

    if (alreadyListed)
      continue;

    auto& earnedEntry = response.achievements.emplace_back();
    earnedEntry.tid = earnedAchievement.tid;
    // Статус тот же, что у уровневых, и по той же причине (R44-1): список
    // 0xe6 — про ПОЛУЧЕННОЕ, а `InProgress` по умолчанию читался бы клиентом
    // как «ещё не закрыто».
    earnedEntry.status = protocol::Quest::Status::Finished;
    // Прогресс отдаём честный: клиент для `Finished` его не рисует, но врать
    // в поле, которое мы всё равно заполняем, незачем.
    earnedEntry.progress = earnedAchievement.progress;
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleRequestPersonalInfo(
  const ClientId clientId,
  const protocol::AcCmdCLRequestPersonalInfo& command)
{
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    command.characterUid);

  protocol::AcCmdLCPersonalInfo response{
    .characterUid = command.characterUid,
    .type = command.type,};

  characterRecord.Immutable([this, &response](const data::Character& character)
  {
    switch (response.type)
    {
      case protocol::AcCmdCLRequestPersonalInfo::Type::Basic:
      {
        const auto& guildRecord = _serverInstance.GetDataDirector().GetGuild(
          character.guildUid());
        if (guildRecord.IsAvailable())
        {
          guildRecord.Immutable([&response](const data::Guild& guild)
          {
            response.basic.guildName = guild.name();
          });
        }

        response.basic.introduction = character.introduction();
        response.basic.level = character.level();
        response.basic.levelProgress = character.experience();
        // TODO: implement other stats
        break;
      }
      case protocol::AcCmdCLRequestPersonalInfo::Type::Courses:
      {
        // TODO: implement
        break;
      }
      case protocol::AcCmdCLRequestPersonalInfo::Type::Eight:
      {
        // TODO: (what on earth uses "Eight")
        break;
      }
    }
  });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleEnterRanch(
  const ClientId clientId,
  const protocol::AcCmdCLEnterRanch& command)
{
  const auto clientContext = GetClientContext(clientId);
  const auto rancherRecord = _serverInstance.GetDataDirector().GetCharacter(
    command.rancherUid);

  bool isRanchLocked = true;
  if (rancherRecord)
  {
    rancherRecord.Immutable([&isRanchLocked](const data::Character& rancher)
    {
      isRanchLocked = rancher.isRanchLocked();
    });
  }

  const bool isEnteringOwnRanch = command.rancherUid == clientContext.characterUid;

  if (isRanchLocked && not isEnteringOwnRanch)
  {
    protocol::AcCmdCLEnterRanchCancel response{
      .reason = 3};

    _commandServer.QueueCommand<decltype(response)>(
      clientId, [response]()
      {
        return response;
      });
    return;
  }

  SendEnterRanchOK(clientId, command.rancherUid);
}

void LobbyNetworkHandler::HandleEnterRanchRandomly(
  const ClientId clientId,
  const protocol::AcCmdCLEnterRanchRandomly&)
{
  // LOA-fix (R64-3, round64, backlog #215 + #225): ЧТЕНИЕ И ПОТРЕБЛЕНИЕ
  // ПРЕДПОЧТЕНИЯ — ОДНОЙ АТОМАРНОЙ ОПЕРАЦИЕЙ.
  //
  // ★ЗДЕСЬ НЕЛЬЗЯ «КОПИЯ + ОТДЕЛЬНАЯ МУТАЦИЯ», И ЭТО ГЛАВНОЕ В ЭТОМ МЕСТЕ.
  // Пара «прочитал предпочтение → погасил его» — классический read-modify-write:
  // разложи её на два захвата замка, и между ними откроется окно, в котором
  // второй поток прочитает то же ненулевое значение до сброса. Тогда визит
  // сработает ДВАЖДЫ.
  // ★Пара неатомарна и СЕЙЧАС, до раунда (карта вообще без синхронизации) —
  // то есть это латентный дефект #225, а не поведение, которое надо сохранить.
  // Простой путь «как у всех остальных мест» аккуратно перенёс бы его в новую
  // обёртку; поэтому здесь чтение и запись живут под ОДНИМ захватом.
  data::Uid requestingCharacterUid = data::InvalidUid;
  data::Uid rancherUid = data::InvalidUid;

  if (not MutateClientContext(
        clientId,
        [&requestingCharacterUid, &rancherUid](ClientContext& clientContext)
        {
          requestingCharacterUid = clientContext.characterUid;

          if (clientContext.rancherVisitPreference != data::InvalidUid)
          {
            rancherUid = clientContext.rancherVisitPreference;
            clientContext.rancherVisitPreference = data::InvalidUid;
          }
        }))
  {
    throw std::runtime_error("Lobby client is not available");
  }

  // If the rancher's uid is invalid randomize it.
  if (rancherUid == data::InvalidUid)
  {
    std::vector<data::Uid> availableRanches;

    auto& characters = _serverInstance.GetDataDirector().GetCharacterCache();
    const auto& characterKeys = characters.GetKeys();

    for (const auto& randomRancherUid : characterKeys)
    {
      const auto character = characters.Get(randomRancherUid);
      // LOA-fix (R13-5, round13, backlog #86): ГАРД ХОЛОДНОГО КЛЮЧА.
      // GetKeys() отдаёт все известные хранилищу ключи, включая незагруженные
      // (Get на первом касании возвращает nullopt) и те, чей retrieve упал
      // навсегда. Разыменование пустого optional здесь — UB в лобби-потоке.
      if (not character)
        continue;

      character->Immutable([&availableRanches, requestingCharacterUid](const data::Character& character)
      {
        // Only consider ranches that are unlocked and that
        // do not belong to the character that requested the random ranch.
        if (character.isRanchLocked() || character.uid() == requestingCharacterUid)
          return;

        availableRanches.emplace_back(character.uid());
      });
    }

    // There must be at least the ranch the requesting character is the owner of.
    if (availableRanches.empty())
    {
      // LOA-fix (R64-3, round64, backlog #215): берём уже снятое значение, а не
      // поле контекста. ★Ссылки на контекст здесь больше нет — она жила бы через
      // выходы в чужой код ниже; `requestingCharacterUid` снят под тем же
      // замком, что и потребление предпочтения, то есть это ТО ЖЕ значение.
      availableRanches.emplace_back(requestingCharacterUid);
    }

    // Pick a random character from the available list to join the ranch of.
    std::uniform_int_distribution<size_t> uidDistribution(0, availableRanches.size() - 1);
    rancherUid = availableRanches[uidDistribution(rd)];
  }

  SendEnterRanchOK(clientId, rancherUid);
}

void LobbyNetworkHandler::SendEnterRanchOK(
  const ClientId clientId,
  const data::Uid rancherUid)
{
  const auto clientContext = GetClientContext(clientId);

  const auto& lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();

  protocol::AcCmdCLEnterRanchOK response{
    .rancherUid = rancherUid,
    .otp = _serverInstance.GetOtpSystem().GrantCode(clientContext.characterUid),
    .ranchAddress = lobbyConfig.advertisement.ranch.address.to_uint(),
    .ranchPort = lobbyConfig.advertisement.ranch.port};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleFeatureCommand(
  const ClientId,
  const protocol::AcCmdCLFeatureCommand& command)
{
  server::util::QuietLogWarn("Feature command: {}", command.command);
}

void LobbyNetworkHandler::HandleRequestFestivalResult(
  const ClientId,
  const protocol::AcCmdCLRequestFestivalResult&)
{
  // todo: implement festival
}

void LobbyNetworkHandler::HandleSetIntroduction(
  const ClientId clientId,
  const protocol::AcCmdCLSetIntroduction& command)
{
  const auto clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  characterRecord.Mutable(
    [&command](data::Character& character)
    {
      character.introduction() = command.introduction;
    });

  _serverInstance.GetRanchDirector().BroadcastSetIntroductionNotify(
    clientContext.characterUid, command.introduction);
}

void LobbyNetworkHandler::HandleGetMessengerInfo(
  const ClientId clientId,
  const protocol::AcCmdCLGetMessengerInfo&)
{
  const auto clientContext = GetClientContext(clientId);

  // Get messenger config and check if messenger is enabled
  const auto& messengerConfig = _serverInstance.GetMessengerDirector().GetConfig();

  if (not messengerConfig.enabled)
  {
    // Messenger is not enabled
    protocol::AcCmdCLGetMessengerInfoCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  const auto& lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();

  // Hash character uid with messenger director's otp constant for a unique key
  size_t identityHash = std::hash<uint32_t>()(clientContext.characterUid);
  boost::hash_combine(identityHash, MessengerOtpConstant);

  // Grant otp code to character
  const uint32_t code = _serverInstance.GetOtpSystem().GrantCode(identityHash);

  protocol::AcCmdCLGetMessengerInfoOK response{
    .code = code,
    .ip = static_cast<uint32_t>(htonl(lobbyConfig.advertisement.messenger.address.to_uint())),
    .port = lobbyConfig.advertisement.messenger.port,
  };

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleCheckWaitingSeqno(
  const ClientId clientId,
  [[maybe_unused]] const protocol::AcCmdCLCheckWaitingSeqno& command)
{
  _serverInstance.GetLobbyDirector().GetScheduler().Queue([this, clientId]()
  {
    SendWaitingSeqno(
      clientId,
      _serverInstance.GetLobbyDirector().GetClientQueuePosition(clientId));
  });
}

void LobbyNetworkHandler::SendWaitingSeqno(
  ClientId clientId,
  size_t queuePosition)
{
  protocol::AcCmdCLCheckWaitingSeqnoOK response{
    .position = static_cast<uint32_t>(queuePosition)};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleUpdateSystemContent(
  const ClientId clientId,
  const protocol::AcCmdCLUpdateSystemContent& command)
{
  const auto clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  bool hasPermission = false;
  characterRecord.Immutable([&hasPermission](const data::Character& character)
  {
    hasPermission = character.role() != data::Character::Role::User;
  });

  if (not hasPermission)
    return;

  // Set system content setting
  _serverInstance.GetSystemContentRegistry().SetValue(command.key, command.value);

  // Notify only the changed setting
  protocol::AcCmdLCUpdateSystemContent notify{};
  notify.systemContent.values = {{command.key, command.value}};

  // LOA-fix (R64-3, round64, backlog #215): рассылка по всем клиентам — СПИСОК
  // под замком, отправка после него.
  // ★Второе место той же формы (`| std::views::keys`), которое мои сканеры не
  // видели: они проверяли известные виды перебора, а не свойство «каждое
  // обращение к `_clients` под замком». Здесь вдобавок `QueueCommand` внутри
  // цикла — то есть замок, накинутый на цикл целиком, ушёл бы в чужой код.
  std::vector<ClientId> notifyTargets;
  {
    const std::shared_lock lock(_clientsMutex);
    notifyTargets.reserve(_clients.size());
    for (const auto& connectedClientId : _clients | std::views::keys)
      notifyTargets.emplace_back(connectedClientId);
  }

  for (const auto& connectedClientId : notifyTargets)
  {
    _commandServer.QueueCommand<decltype(notify)>(
      connectedClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void LobbyNetworkHandler::HandleEnterRoomQuickStop(
  const ClientId clientId,
  const protocol::AcCmdCLEnterRoomQuickStop&)
{
  const auto clientContext = GetClientContext(clientId);

  const bool dequeued = _serverInstance.GetMatchmakingSystem().Dequeue(
    clientContext.characterUid);

  if (not dequeued)
  {
    // Character was not dequeued from the matchmaking system,
    // maybe character was not queued in the first place?
    const protocol::AcCmdCLEnterRoomQuickStopCancel cancel{};
    _commandServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Successfully dequeued from the matchmaking system
  const protocol::AcCmdCLEnterRoomQuickStopOK response {};
  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleRequestFestivalPrize(
  const ClientId,
  const protocol::AcCmdCLRequestFestivalPrize&)
{
  // todo: implement festivals
}

void LobbyNetworkHandler::HandleQueryServerTime(
  const ClientId clientId)
{
  protocol::AcCmdCLQueryServerTimeOK response{
    .lobbyTime = util::TimePointToFileTime(std::chrono::system_clock::now())};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleRequestMountInfo(
  const ClientId clientId,
  const protocol::AcCmdCLRequestMountInfo& command)
{

  const auto& characterUid = GetClientContext(clientId).characterUid;
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(characterUid);

  protocol::AcCmdCLRequestMountInfoOK response{
    .characterUid = command.characterUid,};

  std::vector<data::Uid> mountUids;
  characterRecord.Immutable([&mountUids](const data::Character& character)
  {
    mountUids = character.horses();
    if (character.mountUid() != data::InvalidUid)
      mountUids.emplace_back(character.mountUid());
  });

  for (const auto mountUid : mountUids)
  {
    auto& mountInfo = response.mountInfos.emplace_back();
    mountInfo.horseUid = mountUid;

    const auto horseRecord = _serverInstance.GetDataDirector().GetHorse(mountUid);
    horseRecord.Immutable([&mountInfo](const data::Horse& horse)
    {
      mountInfo.boostsInARow = static_cast<uint16_t>(
        horse.mountInfo.boostsInARow());
      mountInfo.winsSpeedSingle = static_cast<uint16_t>(
        horse.mountInfo.winsSpeedSingle());
      mountInfo.winsSpeedTeam = static_cast<uint16_t>(
        horse.mountInfo.winsSpeedTeam());
      mountInfo.winsMagicSingle = static_cast<uint16_t>(
        horse.mountInfo.winsMagicSingle());
      mountInfo.winsMagicTeam = static_cast<uint16_t>(
        horse.mountInfo.winsMagicTeam());
      // LOA-fix (R24, #14 фаза 1): эти протокол-поля uint32 (LobbyMessageDefinitions),
      // но read-back резал их до uint16 → totalDistance оборачивался за ~19 заездов
      // (одометр «назад»); те же грабли ждали carnival participated/cumulativePrize/
      // biggestPrize (тоже uint32, Ф2). Расширяем ВСЕ шесть. Касты на winsSpeed*/
      // boostsInARow НЕ трогаем — те wire-поля реально uint16.
      mountInfo.totalDistance = static_cast<uint32_t>(
        horse.mountInfo.totalDistance());
      mountInfo.topSpeed = static_cast<uint32_t>(
        horse.mountInfo.topSpeed());
      mountInfo.longestGlideDistance = static_cast<uint32_t>(
        horse.mountInfo.longestGlideDistance());
      mountInfo.participated = static_cast<uint32_t>(
        horse.mountInfo.participated());
      mountInfo.cumulativePrize = static_cast<uint32_t>(
        horse.mountInfo.cumulativePrize());
      mountInfo.biggestPrize = static_cast<uint32_t>(
        horse.mountInfo.biggestPrize());
    });
  }

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleInquiryTreecash(
  const ClientId clientId,
  const protocol::AcCmdCLInquiryTreecash&)
{
  const auto clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::LobbyCommandInquiryTreecashOK response{};

  characterRecord.Immutable(
    [&response](const data::Character& character)
    {
      response.cash = character.cash();
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleAcceptInviteToGuild(
  const ClientId clientId,
  const protocol::AcCmdLCInviteGuildJoinOK& command)
{
  // TODO: command data check

  const auto clientContext = GetClientContext(clientId);

  // Pending invites for guild
  auto& pendingGuildInvites = _serverInstance.GetLobbyDirector().GetGuilds()[command.guild.uid].invites;

  // Check if the guild has outstanding character invite.
  const auto& guildInvite = std::ranges::find(
    pendingGuildInvites,
    clientContext.characterUid);

  if (guildInvite != pendingGuildInvites.end())
  {
    // Guild invite exists, erase and process
    pendingGuildInvites.erase(guildInvite);
  }
  else
  {
    // Character tried to join guild but has no pending (online) invite
    server::util::QuietLogWarn("Character {} tried to join a guild {} but does not have a valid invite",
      clientContext.characterUid, command.guild.uid);
    return;
  }

  std::string inviteeCharacterName;
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&inviteeCharacterName, guildUid = command.guild.uid](data::Character& character)
  {
    inviteeCharacterName = character.name();
    character.guildUid() = guildUid;
  });

  bool guildAddSuccess = false;
  _serverInstance.GetDataDirector().GetGuild(command.guild.uid).Mutable(
    [&guildAddSuccess, inviteeCharacterUid = command.characterUid](data::Guild& guild)
    {
      // Check if invitee who accepted is in the guild
      if (std::ranges::contains(guild.members(), inviteeCharacterUid) ||
          std::ranges::contains(guild.officers(), inviteeCharacterUid) ||
          guild.owner() == inviteeCharacterUid)
      {
        server::util::QuietLogWarn("Character {} tried to join guild {} that they are already a part of",
          inviteeCharacterUid, guild.uid());
        return;
      }

      guild.members().emplace_back(inviteeCharacterUid);
      guildAddSuccess = true;
    });

  if (not guildAddSuccess)
  {
    // TODO: return some error to the accepting client?
    return;
  }

  _serverInstance.GetRanchDirector().SendGuildInviteAccepted(
    command.guild.uid,
    command.characterUid,
    inviteeCharacterName
  );
}

void LobbyNetworkHandler::HandleDeclineInviteToGuild(
  const ClientId,
  const protocol::AcCmdLCInviteGuildJoinCancel& command)
{
  // TODO: command data check
  _serverInstance.GetRanchDirector().SendGuildInviteDeclined(
    command.characterUid,
    command.inviterCharacterUid,
    command.inviterCharacterName,
    command.guild.uid
  );
}

void LobbyNetworkHandler::HandleClientNotify(
  const ClientId,
  const protocol::AcCmdClientNotify& command)
{
  // todo: reset roll code?
  if (command.val0 != 1)
    server::util::QuietLogError("Client error notification: state[{}], value[{}]", command.val0, command.val1);
}

void LobbyNetworkHandler::HandleChangeRanchOption(
  const ClientId clientId,
  const protocol::AcCmdCLChangeRanchOption& command)
{
  const auto clientContext = GetClientContext(clientId);
  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);
  protocol::AcCmdCLChangeRanchOptionOK response{
    .unk0 = command.unk0,
    .unk1 = command.unk1,
    .unk2 = command.unk2};
  characterRecord.Mutable([](data::Character& character)
    {
      character.isRanchLocked() = !character.isRanchLocked();
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleRequestDailyQuestList(
  const ClientId clientId,
  const protocol::AcCmdCLRequestDailyQuestList&)
{
  const auto clientContext = GetClientContext(clientId);

  // LOA-fix (R10-2, round10): страховка «reset-on-read». Основной сброс стоит в
  // SendLoginOK (R10-1), но этот пакет — ЕДИНСТВЕННОЕ место, где клиент берёт
  // набор целей на день, и обновить его потом нечем. Поэтому сбрасываем ещё и
  // прямо перед сборкой ответа: если сессия почему-то пережила границу игрового
  // дня без нового логина (долгий онлайн, переоткрытие окна квестов), клиент
  // получит сегодняшнее состояние, а не вчерашнее.
  // Дублирования нет: сброс идемпотентен по lastResetDate — второй вызов в тот
  // же игровой день выходит по `if (group.lastResetDate() >= today) return;`.
  _serverInstance.GetRanchDirector().ResetDailyQuestsIfNeeded(
    clientContext.characterUid);

  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCLRequestDailyQuestListOK response{};
  data::Uid groupUid = data::InvalidUid;

  characterRecord.Immutable(
    [&response, &groupUid](const data::Character& character)
    {
      response.characterUid = character.uid();
      groupUid = character.dailyQuestGroupUid();
    });

  // Default all unk slots to 2 (not defined in enum, but used as a default/inactive state).
  for (auto& q : response.unk)
    q.status = static_cast<protocol::Quest::Status>(2);

  // Only populate quest data if the character has an assigned daily quest group.
  if (groupUid == data::InvalidUid)
  {
    _commandServer.QueueCommand<decltype(response)>(clientId, [response]() { return response; });
    return;
  }

  const auto groupRecord = _serverInstance.GetDataDirector().GetDailyQuestGroup(groupUid);
  if (not groupRecord)
  {
    _commandServer.QueueCommand<decltype(response)>(clientId, [response]() { return response; });
    return;
  }

  // Collect both Repeatable quest TIDs (TID 100 and 101) sorted ascending.
  std::vector<uint16_t> repeatableTids;
  for (const auto& [tid, quest] : _serverInstance.GetQuestRegistry().GetQuests())
  {
    if (quest.type == registry::Quest::Type::Repeatable)
      repeatableTids.push_back(static_cast<uint16_t>(tid));
  }
  std::sort(repeatableTids.begin(), repeatableTids.end());

  bool hasQuests = false;
  int completedCount = 0;
  bool carrotsClaimed = false;

  groupRecord.Immutable([&](const data::DailyQuestGroup& group)
  {
    carrotsClaimed = group.carrotsClaimed();

    const auto rewardId   = static_cast<uint8_t>(group.rewardId());
    const auto rewardType = static_cast<uint8_t>(group.rewardType());
    const auto& quests    = group.quests();

    // Only populate daily quest slots if there are actual quests assigned.
    for (size_t i = 0; i < 3; ++i)
    {
      if (quests[i].questId == 0)
        continue;

      hasQuests = true;

      const auto questId  = quests[i].questId;
      const auto progress = quests[i].progress;

      const auto questTemplate = _serverInstance.GetQuestRegistry().GetQuest(questId);
      const uint32_t successValue = questTemplate ? questTemplate->successValue : 0;
      const bool isDone = successValue > 0 && progress >= successValue;

      if (isDone)
        ++completedCount;

      response.dailyQuests[i] = protocol::DailyQuest{questId, progress, rewardType, rewardId};
      // Daily quests go into unk[2..4] (slots 0 and 1 are the two Repeatable quests).
      response.unk[i + 2] = protocol::Quest{
        /*tid=*/questId,
        /*member0=*/0,
        isDone ? protocol::Quest::Status::ReadyToClaim : protocol::Quest::Status::InProgress,
        /*progress=*/progress,
        /*member3=*/0,
        /*member4=*/0};
    }
  });

  // unk[0] = TID 100 (intro/activate) InProgress if carrots not yet claimed, ReadyToClaim if they have been.
  if (repeatableTids.size() >= 1)
    response.unk[0] = protocol::Quest{repeatableTids[0], 0,
      carrotsClaimed ? protocol::Quest::Status::ReadyToClaim : protocol::Quest::Status::InProgress,
      0, 0, 0};

  // unk[1] = TID 101 (collect reward) only shown when quests are present;
  // InProgress if not all done, ReadyToClaim if the quest rewards have been claimed (not indicated in the save data yet)
  if (hasQuests && repeatableTids.size() >= 2)
    response.unk[1] = protocol::Quest{repeatableTids[1], 0,
      protocol::Quest::Status::InProgress,
      0, 0, 0};

  _commandServer.QueueCommand<decltype(response)>(clientId, [response]() { return response; });
}

void LobbyNetworkHandler::HandleRequestLeagueInfo(
  const ClientId clientId,
  const protocol::AcCmdCLRequestLeagueInfo&)
{
  protocol::AcCmdCLRequestLeagueInfoOK response{};

  // todo: implement leagues

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleRequestQuestList(
  const ClientId clientId,
  const protocol::AcCmdCLRequestQuestList&)
{
  const auto clientContext = GetClientContext(clientId);
  auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  protocol::AcCmdCLRequestQuestListOK response{};

  characterRecord.Immutable(
    [this, &response](const data::Character& character)
    {
      response.unk0 = character.uid();

      const auto questRecords = _serverInstance.GetDataDirector().GetQuestCache().Get(
        character.quests());
      if (not questRecords)
        return;

      protocol::BuildProtocolQuests(response.quests, *questRecords);
    });

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

void LobbyNetworkHandler::HandleRequestSpecialEventList(
  const ClientId clientId,
  const protocol::AcCmdCLRequestSpecialEventList&)
{
  const auto clientContext = GetClientContext(clientId);
  auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // todo: figure this out

  protocol::AcCmdCLRequestSpecialEventListOK response{};

  _commandServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });
}

} // namespace server
