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

#include "server/chat/AllChatDirector.hpp"
#include "libserver/util/QuietLog.hpp"

#include "server/ServerInstance.hpp"

#include <boost/container_hash/hash.hpp>

#include <mutex>
#include <shared_mutex>
#include <unordered_set>

namespace server
{

AllChatDirector::AllChatDirector(ServerInstance& serverInstance)
  : _chatterServer(*this)
  , _serverInstance(serverInstance)
{
  // Register chatter command handlers
  _chatterServer.RegisterCommandHandler<protocol::ChatCmdEnterRoom>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterEnterRoom(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdChat>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterChat(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdInputState>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterInputState(clientId, command);
    });
}

void AllChatDirector::Initialize()
{
  server::util::QuietLogDebug(
    "All chat server listening on {}:{}",
    GetConfig().listen.address.to_string(),
    GetConfig().listen.port);

  _chatterServer.BeginHost(GetConfig().listen.address, GetConfig().listen.port);
}

void AllChatDirector::Terminate()
{
  _chatterServer.EndHost();
}

AllChatDirector::ClientContext& AllChatDirector::GetClientContext(
  const network::ClientId clientId,
  bool requireAuthentication)
{
  auto clientContextIter = _clients.find(clientId);
  if (clientContextIter == _clients.end())
    throw std::runtime_error("All chat client is not available");

  auto& clientContext = clientContextIter->second;
  if (requireAuthentication && not clientContext.isAuthenticated)
    throw std::runtime_error("All chat client is not authenticated");

  return clientContext;
}

void AllChatDirector::Tick()
{
}

Config::AllChat& AllChatDirector::GetConfig()
{
  return _serverInstance.GetSettings().allChat;
}

void AllChatDirector::HandleClientConnected(network::ClientId clientId)
{
  server::util::QuietLogDebug("Client {} connected to the all chat server from {}",
    clientId,
    _chatterServer.GetClientAddress(clientId).to_string());

  // LOA-fix (R80-4, round80, backlog #235): ВСТАВКА — ПОД ИСКЛЮЧИТЕЛЬНЫМ
  // ЗАМКОМ, и обе метки времени ставятся при рождении записи. Вставка способна
  // вызвать РЕХЭШ, то есть для обходящего карту с чужого потока она опаснее
  // правки значений (класс R61-2/#202).
  {
    const std::unique_lock lock(_clientsMutex);
    const auto now = chat::ReapClock::now();
    const auto [clientIter, inserted] = _clients.try_emplace(clientId);
    clientIter->second.connectedAt = now;
    clientIter->second.lastActivity = now;
  }
}

void AllChatDirector::HandleClientDisconnected(network::ClientId clientId)
{
  server::util::QuietLogDebug("Client {} disconnected from the all chat server", clientId);

  // LOA-fix (R80-4, round80, backlog #235): УДАЛЕНИЕ — ПОД ИСКЛЮЧИТЕЛЬНЫМ
  // ЗАМКОМ. До раунда сюда приходили только настоящие разрывы с собственного
  // потока; теперь этот путь синхронно зовёт САМ раунд (фаза 2 вытеснения и
  // слив), а карту читает и правит ещё и поток лобби.
  {
    const std::unique_lock lock(_clientsMutex);
    _clients.erase(clientId);
  }
}

void AllChatDirector::HandleClientActivity(const network::ClientId clientId)
{
  // LOA-fix (R80-4, round80, backlog #235): ШТАМП АКТИВНОСТИ ЧАТ-СОКЕТА.
  // ★UPDATE-ONLY: `operator[]` воскресил бы уже убранную запись сиротой
  // навсегда. ★`find`, а не `GetClientContext`: тот бросает, а бросок отсюда
  // уходит в сетевой read-loop и рвёт клиенту соединение.
  const std::unique_lock lock(_clientsMutex);
  const auto clientIter = _clients.find(clientId);
  if (clientIter != _clients.end())
    clientIter->second.lastActivity = chat::ReapClock::now();
}

chat::ReapThresholds AllChatDirector::GetReapThresholds() const
{
  const auto& cfg = _serverInstance.GetSettings().chatReap;
  return chat::ReapThresholds{
    .handshakeTimeout = std::chrono::seconds(cfg.handshakeTimeoutSeconds),
    .orphanGrace = std::chrono::seconds(cfg.orphanGraceSeconds),
    .absoluteIdle = std::chrono::seconds(cfg.absoluteIdleSeconds)};
}

void AllChatDirector::HandleNetworkTick()
{
  // ★ЕДИНСТВЕННАЯ ТОЧКА, ПРИХОДЯЩАЯ С ПОТОКА ALL-CHAT. `Server::TickLoop`
  // армируется на `io_context` того же сервера, поэтому тик приходит ровно с
  // того потока, что accept, чтение пакетов и разрывы — то есть с того,
  // которому карта клиентов принадлежит. `Tick()` директора идёт с ЧУЖОГО
  // потока задач и остаётся ПУСТЫМ (гейт `tools/check_chat_sweep_thread.sh`).
  //
  // LOA-fix (R80-4, round80, backlog #235): развёртка идёт ПЕРЕД сливом — то,
  // что она поставит в очередь, закрывается ТЕМ ЖЕ тиком, а не следующим.
  SweepChatSockets();
  DrainPendingDisconnects();
}

void AllChatDirector::EvictOtherSessionsOfCharacter(
  const network::ClientId keepClientId,
  const data::Uid characterUid)
{
  // ★ГАРД `InvalidUid` — НЕСУЩИЙ, А НЕ ГИГИЕНА. Свежее соединение до входа
  // лежит в карте с `characterUid == InvalidUid`; без гарда вызов с
  // невыясненной личностью совпал бы КО ВСЕМ таким записям и закрыл бы
  // соединения клиентов, которые как раз подключаются.
  if (characterUid == data::InvalidUid)
    return;

  // Фаза 1 — правка значений НА МЕСТЕ под исключительным замком: ни вставок,
  // ни удалений, ни рехэша.
  std::vector<network::ClientId> unbound;
  {
    const std::unique_lock lock(_clientsMutex);
    for (auto& [otherClientId, otherContext] : _clients)
    {
      if (otherClientId == keepClientId)
        continue;
      if (otherContext.characterUid != characterUid)
        continue;

      otherContext.isAuthenticated = false;
      otherContext.characterUid = data::InvalidUid;
      unbound.emplace_back(otherClientId);
    }
  }

  if (unbound.empty())
    return;

  // Фаза 2 — закрытие СТРОГО ВНЕ обхода и ВНЕ замка. `Client::End()` синхронно
  // зовёт `OnClientDisconnected`, тот приходит в наш `HandleClientDisconnected`,
  // а он стирает запись из `_clients` под тем же замком: закрытие внутри обхода
  // было бы и инвалидацией итератора, и самозахватом нерекурсивного замка.
  // ★Мы уже на своём сетевом потоке (путь входа), поэтому фаза 2 синхронна и
  // очередь не нужна — тот же довод, по которому так устроен R78.
  size_t closed = 0;
  for (const network::ClientId staleClientId : unbound)
  {
    try
    {
      _chatterServer.DisconnectClient(staleClientId);
      ++closed;
    }
    catch (const std::exception&)
    {
      // Соединения уже нет — ровно та цель, которой добивались.
    }
  }

  server::util::QuietLogInfo(
    "Evicted {} stale all chat session(s) of character {} on {}",
    closed,
    characterUid,
    "re-login");
}

void AllChatDirector::CloseSessionsOfCharacter(const data::Uid characterUid)
{
  if (characterUid == data::InvalidUid)
    return;

  // Фаза 1 — отвязать ВСЕ сессии персонажа, не щадя ни одной: игрок вышел из
  // игры, держать нечего.
  std::vector<PendingDisconnect> condemned;
  {
    const std::unique_lock lock(_clientsMutex);
    for (auto& [clientId, clientContext] : _clients)
    {
      if (clientContext.characterUid != characterUid)
        continue;

      clientContext.isAuthenticated = false;
      clientContext.characterUid = data::InvalidUid;
      condemned.emplace_back(
        PendingDisconnect{.clientId = clientId, .characterUid = characterUid});
    }
  }

  if (condemned.empty())
    return;

  // ★ФАЗА 2 НЕ ЗДЕСЬ. Этот метод зовёт ЧУЖОЙ поток — лобби на выходе игрока.
  // Закрыть соединение отсюда значит синхронно уйти в `Client::End()` →
  // `HandleClientDisconnected` и стирать записи из трёх карт (наша `_clients`,
  // `Server::_clients`, `Server::_addressStates` вовсе без замка), пока поток
  // all-chat их законно читает. Кладём в очередь и уходим; закроет
  // `DrainPendingDisconnects()` с тика СВОЕГО сервера, не позже 1 с.
  {
    const std::lock_guard lock(_pendingDisconnectsMutex);
    for (const PendingDisconnect& entry : condemned)
      _pendingDisconnects.emplace_back(entry);
  }
}

void AllChatDirector::DrainPendingDisconnects()
{
  // ★ИСПОЛНЯЕТСЯ НА ПОТОКЕ ALL-CHAT (`HandleNetworkTick`). Только здесь законно
  // звать `DisconnectClient` и позволять уборке стирать записи.
  std::vector<PendingDisconnect> pending;
  {
    const std::lock_guard lock(_pendingDisconnectsMutex);
    if (_pendingDisconnects.empty())
      return;
    pending.swap(_pendingDisconnects);
  }

  size_t closed = 0;
  size_t rebound = 0;
  for (const PendingDisconnect& entry : pending)
  {
    // ★ПОЯС НА ЭЛЕМЕНТ СТОИТ С САМОГО НАЧАЛА — не потому, что рассылка может
    // бросить (её здесь нет вовсе: all-chat присутствие никому не вещает), а
    // потому что очередь уже снята `swap`-ом и один бросок посреди цикла унёс
    // бы весь остаток: те сокеты никто не закроет и вернуть их некому.
    try
    {
      // ★ПЕРЕПРОВЕРКА ПЕРЕД РАЗРЫВОМ. Пока запись лежала в очереди, сокет мог
      // пройти повторный вход и СНОВА стать законной сессией персонажа. Рвать
      // такую сессию значило бы бить по живому входу.
      bool stillUnbound = false;
      {
        const std::shared_lock lock(_clientsMutex);
        const auto clientIter = _clients.find(entry.clientId);
        stillUnbound = clientIter != _clients.cend()
          && clientIter->second.characterUid == data::InvalidUid;
      }
      if (not stillUnbound)
      {
        ++rebound;
        continue;
      }

      try
      {
        _chatterServer.DisconnectClient(entry.clientId);
        ++closed;
      }
      catch (const std::exception&)
      {
        // Соединения уже нет — ровно та цель, которой добивались.
      }
    }
    catch (const std::exception& x)
    {
      uint64_t suppressed = 0;
      uint64_t total = 0;
      if (_drainStepThrottle.Allow(suppressed, total))
      {
        server::util::QuietLogWarn(
          "Deferred chat session teardown failed for client {}: {}"
          " (suppressed {}, total {})",
          entry.clientId,
          x.what(),
          suppressed,
          total);
      }
    }
  }

  server::util::QuietLogInfo(
    "Closed {} deferred all chat session(s) on the all chat thread"
    " ({} rebound and spared)",
    closed,
    rebound);
}

void AllChatDirector::SweepChatSockets()
{
  // LOA-fix (R80-4, round80, backlog #235): РАЗВЁРТКА-BACKSTOP. Форма — та же,
  // что у мессенджера; разбор «почему здесь, а не в `Tick()`» — там же и в
  // `HandleNetworkTick` выше.
  //
  // ★ОТЛИЧИЕ ОТ МЕССЕНДЖЕРА РОВНО ОДНО, И ОНО НЕ КОСМЕТИЧЕСКОЕ: у
  // `AllChatDirector::ClientContext` НЕТ поля `otpCode`, поэтому отвязка — это
  // ровно два присваивания, а не три.
  const auto now = chat::ReapClock::now();
  const auto& reapConfig = _serverInstance.GetSettings().chatReap;
  const auto sweepInterval = std::chrono::seconds(reapConfig.sweepIntervalSeconds);
  if (now - _lastChatSweep < sweepInterval)
    return;
  _lastChatSweep = now;
  const chat::ReapThresholds thresholds = GetReapThresholds();

  // ★СНИМОК ЛОББИ — ОДИН НА РАЗВЁРТКУ. ★ЗАМКИ НЕ ПЕРЕСЕКАЮТСЯ ВО ВРЕМЕНИ:
  // копия снимка целиком перекладывается в `charactersInGame`, и только ПОСЛЕ
  // этого берётся `_clientsMutex`.
  // ★СБОЙ СНИМКА ВЫКЛЮЧАЕТ ТОЛЬКО P2/P3, НЕ ВЕСЬ РАУНД: жать «сироту», не
  // сумев спросить, в игре ли персонаж, — катастрофический ложно-зелёный.
  std::unordered_set<data::Uid> charactersInGame;
  bool lobbyKnown = false;
  try
  {
    for (const auto& user : _serverInstance.GetLobbyDirector().SnapshotUsers())
    {
      if (user.characterUid != data::InvalidUid)
        charactersInGame.insert(user.characterUid);
    }
    lobbyKnown = true;
  }
  catch (const std::exception& x)
  {
    uint64_t suppressed = 0;
    uint64_t total = 0;
    if (_lobbySnapshotThrottle.Allow(suppressed, total))
    {
      server::util::QuietLogWarn(
        "Chat reaper could not read the lobby snapshot: {}"
        " (suppressed {}, total {})",
        x.what(),
        suppressed,
        total);
    }
  }

  // Фаза 1 — ПОД ЗАМКОМ: решить, отвязать, собрать. Ни одного вызова наружу.
  std::vector<PendingDisconnect> condemned;
  size_t handshake = 0;
  size_t orphan = 0;
  size_t idle = 0;
  {
    const std::unique_lock lock(_clientsMutex);
    for (auto& [clientId, clientContext] : _clients)
    {
      const chat::ChatSocketState state{
        .isAuthenticated = clientContext.isAuthenticated,
        .characterUid = clientContext.characterUid,
        .connectedAt = clientContext.connectedAt,
        .lastActivity = clientContext.lastActivity};

      // ★НЕИЗВЕСТНОСТЬ — В ПОЛЬЗУ ИГРОКА.
      const bool inGame = not lobbyKnown
        || charactersInGame.contains(clientContext.characterUid);

      const auto verdict = chat::DecideChatSocketReap(
        state, inGame, now, thresholds);
      if (verdict == chat::ReapVerdict::Keep)
        continue;

      switch (verdict)
      {
        case chat::ReapVerdict::Handshake: ++handshake; break;
        case chat::ReapVerdict::Orphan:    ++orphan;    break;
        case chat::ReapVerdict::Idle:      ++idle;      break;
        case chat::ReapVerdict::Keep:      break;
      }

      // ★ОТВЯЗКА ДО ЗАКРЫТИЯ — ОБЯЗАТЕЛЬНА, И СЛИВ НА НЕЁ ОПИРАЕТСЯ: он
      // распознаёт «успела перевязаться» именно по `characterUid`.
      condemned.emplace_back(
        PendingDisconnect{
          .clientId = clientId,
          .characterUid = clientContext.characterUid});
      clientContext.isAuthenticated = false;
      clientContext.characterUid = data::InvalidUid;
    }
  }

  if (condemned.empty())
    return;

  // Фаза 2 — постановка в ту же очередь, которую сливает `HandleNetworkTick`.
  // ★Второго пути к закрытию сокета раунд не заводит.
  {
    const std::lock_guard lock(_pendingDisconnectsMutex);
    for (const PendingDisconnect& entry : condemned)
      _pendingDisconnects.emplace_back(entry);
  }

  // ★ОДНА СТРОКА НА РАЗВЁРТКУ, И ТОЛЬКО КОГДА ДЕЙСТВИТЕЛЬНО ЖАЛИ. Формат-строка
  // намеренно ТА ЖЕ, что у мессенджера: компилятор сольёт литералы, маркер
  // лесенки считается один раз, а службы различает АРГУМЕНТ.
  server::util::QuietLogInfo(
    "Reaped {} chat socket(s) on {}: {} handshake, {} orphan, {} idle",
    condemned.size(),
    "the all chat",
    handshake,
    orphan,
    idle);
}

void AllChatDirector::HandleChatterEnterRoom(
  network::ClientId clientId,
  const protocol::ChatCmdEnterRoom& command)
{
  auto& clientContext = GetClientContext(clientId, false);

  // Generate identity hash based on the character uid from the command and
  // the chat otp constant
  size_t identityHash = std::hash<uint32_t>()(command.characterUid);
  boost::hash_combine(identityHash, AllChatOtpConstant);

  // Authorise the code received in the command against the calculated identity hash
  //
  // LOA-fix (R80-4, round80, backlog #235): РЕШЕНИЕ ПРИНИМАЕТСЯ БЕЗ ЗАМКА,
  // ЗАПИСЬ ИДЁТ ПОД ЗАМКОМ — дословно форма R78. `AuthorizeLtk` берёт СВОЙ
  // мьютекс (`OtpSystem`), и тянуть его внутрь нашего значило бы завести
  // порядок захвата, которого больше нигде нет.
  const bool authorized = _serverInstance.GetOtpSystem().AuthorizeLtk(
    identityHash,
    command.code,
    _chatterServer.GetClientAddress(clientId).to_uint());

  {
    const std::unique_lock lock(_clientsMutex);
    clientContext.isAuthenticated = authorized;
  }

  if (not authorized)
  {
    // Client failed chat authentication
    // Do not log with `command.name` (character name) to prevent some form of string manipulation in spdlog
    server::util::QuietLogWarn("Client '{}' tried to login to all chat as character '{}' but failed authentication with auth code '{}'",
      clientId,
      command.characterUid,
      command.code);

    protocol::ChatCmdEnterRoomAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::ChatLoginFailed};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });

    // TODO: confirm the cancel command is sent before disconnecting the client
    _chatterServer.DisconnectClient(clientId);
    return;
  }

  // Sets client context character uid to the one provided by the client
  // This value is assured by the server to be correct (if it passes authentication) as
  // the server hashes the character uid and then the director's otp constant to compute the code.
  {
    const std::unique_lock lock(_clientsMutex);
    clientContext.characterUid = command.characterUid;
  }

  // LOA-fix (R80-4, round80, backlog #235): УСПЕШНЫЙ ВХОД ВЫТЕСНЯЕТ ПРЕЖНИЕ
  // СЕССИИ ТОГО ЖЕ ПЕРСОНАЖА.
  //
  // ★ЭТО ЛЕЧЕНИЕ ИЗМЕРЕННОГО СИМПТОМА, А НЕ ТЕОРИИ. По T55 клиент открывает
  // НОВЫЙ all-chat-сокет на каждом заезде, не закрыв старый: четыре захода дали
  // четыре сокета, и ни один не закрылся сам.
  //
  // ★ЗАМОК ОТПУЩЕН. Фаза 2 вытеснения синхронно уходит в `Client::End()` →
  // `HandleClientDisconnected` → `erase` ПОД ЭТИМ ЖЕ замком, а `shared_mutex`
  // не рекурсивный: вытеснять под замком значило бы получить самозахват —
  // тихий дедлок потока all-chat (класс R59).
  EvictOtherSessionsOfCharacter(clientId, command.characterUid);

  // TODO: discover response ack
  protocol::ChatCmdEnterRoomAckOk response{
    .unk1 = {
      protocol::ChatCmdEnterRoomAckOk::Struct0{
        .unk0 = 0,
        .unk1 = "All"
      },
      protocol::ChatCmdEnterRoomAckOk::Struct0{
        .unk0 = 1,
        .unk1 = "Guild"
      }
    },
  };
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void AllChatDirector::HandleChatterChat(
  network::ClientId clientId,
  const protocol::ChatCmdChat& command)
{
  const auto& clientContext = GetClientContext(clientId);

  const auto characterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  std::string characterName{};
  bool isGameMaster = false;
  characterRecord.Immutable(
    [&characterName, &isGameMaster](const data::Character& character)
    {
      characterName = character.name();
      isGameMaster = character.role() == data::Character::Role::GameMaster;
    });

  const auto userName = _serverInstance.GetLobbyDirector().GetUserByCharacterUid(
    clientContext.characterUid).userName;

  server::util::QuietLogInfo("[Global] {} ({}): {}",
    characterName,
    userName,
    command.message);

  const auto chatVerdict = _serverInstance.GetChatSystem().ProcessChatMessage(
    clientContext.characterUid, command.message);

  // LOA-fix (R55-3, round55, backlog #179 часть 5): пустое значение = сообщение
  // не обработано. Причина уже записана в лог внутри; здесь просто молчим —
  // отвечать игроку нечем, а рассылать пустую строку в канал нельзя.
  if (not chatVerdict)
    return;

  const auto& verdict = *chatVerdict;

  if (verdict.commandVerdict)
  {
    for (const auto& response : verdict.commandVerdict->result)
    {
      _chatterServer.QueueCommand<protocol::ChatCmdChannelChatTrs>(clientId, [response]()
      {
        return protocol::ChatCmdChannelChatTrs{
          .messageAuthor = "",
          .message = response,
          .role = protocol::ChatCmdChat::Role::GameMaster};
      });
    }

    return;
  }

  if (verdict.isMuted)
  {
    _chatterServer.QueueCommand<protocol::ChatCmdChannelChatTrs>(clientId, [verdict]()
    {
      return protocol::ChatCmdChannelChatTrs{
        .messageAuthor = "",
        .message = verdict.message,
        .role = protocol::ChatCmdChat::Role::GameMaster};
    });

    return;
  }

  // ChatCmdChatTrs did not work in any way shape or form, the handler seemed to just do nothing
  // Opted for ChatCmdChannelChatTrs for global chat
  protocol::ChatCmdChannelChatTrs notify{
    .messageAuthor = isGameMaster
      ? std::format("[GM] {}", characterName)
      : characterName,
    .message = command.message,
    .role = protocol::ChatCmdChat::Role::User};
  
  // LOA-fix (R80-4, round80, backlog #235): ОБХОД ИДЁТ ПО КОПИИ, СНЯТОЙ ПОД
  // РАЗДЕЛЯЕМЫМ ЗАМКОМ.
  //
  // ★ЭТУ ГОНКУ ПРИВОДИТ СЮДА САМ РАУНД, ПОЭТОМУ ЕЁ ЗАКРЫВАЕТ ОН ЖЕ. До R80
  // карту трогал ровно один поток, и голый обход был законен. Раунд заводит
  // чужепоточного писателя (`CloseSessionsOfCharacter` с потока лобби), а тот
  // правит ровно те поля, которые читает этот цикл.
  // ★КОПИЯ, А НЕ ЗАМОК НА ВЕСЬ ЦИКЛ: внутри стоит выход наружу
  // (`QueueCommand`), а под замком карты не должно быть ни одного вызова в
  // чужой код. Форма дословно R78-шная (`BroadcastPresenceOfCharacter`).
  const auto clientsSnapshot = [this]
  {
    const std::shared_lock lock(_clientsMutex);
    return _clients;
  }();

  for (const auto& [onlineClientId, onlineClientContext] : clientsSnapshot)
  {
    // Skip unauthenticated clients
    if (not onlineClientContext.isAuthenticated)
      continue;

    _chatterServer.QueueCommand<decltype(notify)>(onlineClientId, [notify]()
    {
      return notify;
    });
  }
}

void AllChatDirector::HandleChatterInputState(
  network::ClientId clientId,
  const protocol::ChatCmdInputState& command)
{
  // Note: might have to do with login state i.e. remember last online status (online/offline/away)
  const auto& clientContext = GetClientContext(clientId);

  // Get character's friends list
  std::set<data::Uid> friends{};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&friends](const data::Character& character)
    {
      friends = character.contacts.groups().at(0).members;
    });

  // Prepare notify command
  protocol::ChatCmdInputStateTrs notify{
    .unk0 = clientContext.characterUid, // Assumed, unknown effect
    .state = command.state};

  // LOA-fix (R80-4, round80, backlog #235): обход по КОПИИ под разделяемым
  // замком — тот же довод, что у `HandleChatterChat` выше.
  const auto clientsSnapshot = [this]
  {
    const std::shared_lock lock(_clientsMutex);
    return _clients;
  }();

  for (const auto& [onlineClientId, onlineClientContext] : clientsSnapshot)
  {
    // Skip unauthenticated clients
    if (not onlineClientContext.isAuthenticated)
      continue;

    // Notify friend
    if (friends.contains(onlineClientContext.characterUid))
      _chatterServer.QueueCommand<decltype(notify)>(onlineClientId, [notify](){ return notify; });
  }
}

} // namespace server
