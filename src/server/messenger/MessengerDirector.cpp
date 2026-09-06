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

#include "server/messenger/MessengerDirector.hpp"
#include "server/messenger/MessengerSessionEviction.hpp"
#include "libserver/util/Locale.hpp"
#include "libserver/util/LogThrottle.hpp"
#include "libserver/util/QuietLog.hpp"

#include <algorithm>

#include "libserver/util/Cleanup.hpp"

#include "server/ServerInstance.hpp"

#include <boost/container_hash/hash.hpp>
#include <locale>
#include <shared_mutex>
#include <unordered_set>

namespace server
{

//! Alicia client defined friends category.
constexpr auto FriendsCategoryUid = 0;
constexpr auto OnlinePlayersCategoryUid = std::numeric_limits<uint32_t>::max() - 2;
constexpr std::string_view DateTimeFormat = "{:%H:%M:%S %d/%m/%Y} UTC";

//! ★R74-fix-4 (subreview #3, BLOCK 1/2): САМИ ЧИСЛА ЖИВУТ В ЗАГОЛОВКЕ
//! ПРОТОКОЛА ЧАТТЕРА (`ChatterMessageDefinitions.hpp`), рядом с писателями,
//! которые они и ограничивают. Здесь их нет намеренно: цель `alicia-server` в
//! тестовый бинарь не линкуется, и константа, объявленная тут, была бы
//! невидима юнит-гейту — то есть её подмена не могла бы покрасить ни одну
//! проверку. Ровно этот класс ревью уже ловило у отбора слотов макросов.

const std::string GetSystemNameFromType(data::Mail::MailType type)
{
  switch (type)
  {
    case data::Mail::MailType::BreedingReward:
      return "Breeding System";
    case data::Mail::MailType::CarnivalReward:
      return "Carnival System";
    case data::Mail::MailType::NoReply:
      return ""; // System mail
    default:
      throw std::runtime_error(
        std::format(
          "Unsupported system mail type '{}'",
          static_cast<uint32_t>(type)));
  }
}

MessengerDirector::MessengerDirector(ServerInstance& serverInstance)
  : _chatterServer(*this)
  , _serverInstance(serverInstance)
{
  // Register chatter command handlers
  _chatterServer.RegisterCommandHandler<protocol::ChatCmdLogin>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterLogin(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdBuddyAdd>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterBuddyAdd(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdBuddyAddReply>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterBuddyAddReply(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdBuddyDelete>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterBuddyDelete(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdBuddyMove>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterBuddyMove(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdGroupAdd>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterGroupAdd(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdGroupRename>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterGroupRename(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdGroupDelete>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterGroupDelete(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdLetterList>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterLetterList(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdLetterSend>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterLetterSend(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdLetterRead>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterLetterRead(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdLetterDelete>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterLetterDelete(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdUpdateState>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterUpdateState(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdChatInvite>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterChatInvite(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdChannelInfo>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterChannelInfo(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdGuildLogin>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterGuildLogin(clientId, command);
    });

  _chatterServer.RegisterCommandHandler<protocol::ChatCmdGameInvite>(
    [this](network::ClientId clientId, const auto& command)
    {
      HandleChatterGameInvite(clientId, command);
    });
}

void MessengerDirector::Initialize()
{
  server::util::QuietLogDebug(
    "Messenger server listening on {}:{}",
    GetConfig().listen.address.to_string(),
    GetConfig().listen.port);

  _chatterServer.BeginHost(GetConfig().listen.address, GetConfig().listen.port);
}

void MessengerDirector::Terminate()
{
  _chatterServer.EndHost();
}

MessengerDirector::ClientContext& MessengerDirector::GetClientContext(
  const network::ClientId clientId,
  const bool requireAuthentication)
{
  auto clientContextIter = _clients.find(clientId);
  if (clientContextIter == _clients.end())
    throw std::runtime_error("Messenger client is not available");

  auto& clientContext = clientContextIter->second;
  if (requireAuthentication && not clientContext.isAuthenticated)
    throw std::runtime_error("Messenger client is not authenticated");

  return clientContext;
}

std::optional<MessengerDirector::Client> MessengerDirector::GetClientByCharacterUid(
  const data::Uid characterUid) const
{
  std::optional<Client> client{};

  // LOA-fix (R78-fix2, round78, backlog #255, находка Codex 2): КОПИЯ — ПОД
  // РАЗДЕЛЯЕМЫМ ЗАМКОМ. Этот метод зовут потоки ранча и заезда, а вытеснение
  // пишет чужие записи с потока чата; без замка это гонка данных и UB.
  // Замок держится РОВНО НА КОПИРОВАНИИ: перебор идёт уже по снимку, и ни
  // одного выхода в чужой код под замком нет.
  const auto clientsSnapshot = [this]
  {
    const std::shared_lock lock(_clientsMutex);
    return _clients;
  }();
  // Find client iterator by character uid
  const auto& iter = std::ranges::find_if(
    clientsSnapshot,
    [characterUid](const auto& client)
    {
      const ClientContext& clientContext = client.second;
      return clientContext.characterUid == characterUid;
    });

  // If client is found, set client id
  if (iter != clientsSnapshot.cend())
    client.emplace(Client{
      .clientId = iter->first,
      .clientContext = iter->second
    });

  return client;
}

bool MessengerDirector::IsCharacterOnline(const data::Uid characterUid) const
{
  return GetClientByCharacterUid(characterUid).has_value();
}

void MessengerDirector::EvictOtherSessionsOfCharacter(
  const network::ClientId keepClientId,
  const data::Uid characterUid)
{
  // Фаза 1 — правка значений на месте под ИСКЛЮЧИТЕЛЬНЫМ замком, без вставок
  // и удалений. Замок обязателен: `GetClientByCharacterUid` снимает копию этой
  // карты с потоков ранча и заезда (находка Codex 2). Он снимается ДО фазы 2 —
  // отключение синхронно возвращается в `HandleClientDisconnected`, и держать
  // через это нерекурсивный замок значило бы самозахват (класс R59).
  std::vector<network::ClientId> unbound;
  {
    const std::unique_lock lock(_clientsMutex);
    unbound = messenger::UnbindOtherSessionsOfCharacter(
      _clients, keepClientId, characterUid);
  }

  if (unbound.empty())
    return;

  DisconnectUnboundSessions(unbound, "re-login", characterUid);
}

void MessengerDirector::CloseSessionsOfCharacter(const data::Uid characterUid)
{
  // Фаза 1 — отвязать ВСЕ сессии персонажа, не щадя ни одной: игрок вышел,
  // держать нечего.
  std::vector<network::ClientId> unbound;
  {
    const std::unique_lock lock(_clientsMutex);
    unbound = messenger::UnbindAllSessionsOfCharacter(_clients, characterUid);
  }

  if (unbound.empty())
    return;

  // ★ФАЗА 2 НЕ ЗДЕСЬ. Этот метод зовут ЧУЖИЕ потоки — лобби на выходе игрока и
  // поток чат-команд на GM-бане. Закрыть соединение отсюда значит синхронно
  // уйти в `Client::End()` → `HandleClientDisconnected` и стирать записи из
  // трёх карт (наша `_clients`, `Server::_clients`, `Server::_addressStates`
  // вовсе без замка), пока поток мессенджера их читает. Кладём в очередь и
  // уходим; закроет `DrainPendingDisconnects()` с тика ЧАТ-СЕРВЕРА
  // (`HandleNetworkTick`), не позже следующего тика (1 с). `Tick()`
  // директора к дренажу отношения не имеет и пуст (ревю #4, R1).
  {
    const std::lock_guard lock(_pendingDisconnectsMutex);
    for (const network::ClientId staleClientId : unbound)
    {
      // ★ЛИЧНОСТЬ КЛАДЁТСЯ В ОЧЕРЕДЬ ВМЕСТЕ С СОЕДИНЕНИЕМ (ревю #3 WARN-1):
      // фаза 1 её уже стёрла из контекста, а сливу она нужна, чтобы разослать
      // друзьям и гильдии «офлайн».
      _pendingDisconnects.emplace_back(
        PendingDisconnect{.clientId = staleClientId, .characterUid = characterUid});
    }
  }

  server::util::QuietLogInfo(
    "Evicted {} stale messenger session(s) of character {} on {}",
    unbound.size(),
    characterUid,
    "logout");
}

void MessengerDirector::HandleNetworkTick()
{
  // ★ЕДИНСТВЕННАЯ ТОЧКА, ПРИХОДЯЩАЯ С ПОТОКА ЧАТ-СЕРВЕРА. `Server::TickLoop`
  // армируется на `_io_ctx` того же сервера, поэтому тик приходит с того же
  // потока, что accept, чтение пакетов и разрывы — то есть с того, которому
  // карта клиентов принадлежит.
  //
  // LOA-fix (R80-3, round80, backlog #235): развёртка идёт ПЕРЕД сливом — то,
  // что она поставит в очередь, закрывается ТЕМ ЖЕ тиком, а не следующим.
  SweepChatSockets();
  DrainPendingDisconnects();
}

void MessengerDirector::DrainPendingDisconnects()
{
  // ★ИСПОЛНЯЕТСЯ НА ПОТОКЕ МЕССЕНДЖЕРА. `Server::TickLoop` армирован на
  // `io_context` этого же сервера, поэтому тик приходит с того же потока, что
  // accept, чтение пакетов и разрывы. Только здесь законно звать
  // `DisconnectClient` и позволять уборке стирать записи.
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
    // LOA-fix (R80-3, round80, backlog #235): ПОЯС НА ЭЛЕМЕНТ ОЧЕРЕДИ.
    //
    // ★ЧТО ЭТО ЧИНИТ. Очередь уже снята `swap`-ом: бросок посреди цикла унёс бы
    // ВЕСЬ остаток `pending` — те сокеты никто не закроет, в очередь их не
    // вернуть и переспросить некому, а наверху напечатается «Unhandled exception
    // in a network tick». До R80 это был редкий остаток: очередь наполняли
    // только логауты и GM-баны, личность в ней всегда валидна. R80 делает эту
    // очередь ШТАТНЫМ путём — каждая развёртка кладёт в неё всё, что зажала,
    // включая сокеты, которые никогда не привязывались. Форма — та же, что у
    // `util::RunCleanupStep` в уборке лобби: шаг вправе провалиться, проход
    // обязан дойти до конца.
    try
    {
      // ★ПЕРЕПРОВЕРКА ПЕРЕД РАЗРЫВОМ (NIT ревю #3 №3). Пока запись лежала в
      // очереди, сокет мог пройти повторный вход и СНОВА стать законной сессией
      // персонажа: гард повтора его не остановит, потому что фаза 1 сняла оба
      // поля. Рвать такую сессию значило бы бить по живому входу, которого
      // просьба о гашении не касалась. Монотонность `ClientId` спасает только от
      // НОВЫХ соединений, а это — то же самое.
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

      // ★РАССЫЛКА «ОФЛАЙН» ИДЁТ ДО РАЗРЫВА И ПО СОХРАНЁННОЙ ЛИЧНОСТИ
      // (ревю #3 WARN-1). После разрыва уборка синтезирует Offline сама, но
      // упирается в снятый фазой 1 флаг и молчит. Здесь флаг не спрашивается.
      //
      // LOA-fix (R80-3, round80, backlog #235): ГАРД `InvalidUid`.
      // ★ЛИЧНОСТИ МОЖЕТ НЕ БЫТЬ ВОВСЕ. Развёртка ставит в очередь и сокеты,
      // которые никогда не привязывались (P1 — сканеры, брошенные
      // рукопожатия). `BroadcastPresenceOfCharacter` первым делом идёт в
      // `GetCharacter(characterUid).Immutable(...)`, а тот БРОСАЕТ на
      // недоступной записи — то есть без гарда ПЕРВАЯ ЖЕ развёртка со сканером
      // уходила бы в пояс выше, и раунд молча перестал бы работать ровно в том
      // случае, ради которого заведён #235.
      // ★ГАРД НАКРЫВАЕТ И ТРАССУ: след `... (teardown)` печатает САМА рассылка
      // (R78-fix), поэтому отдельного оператора здесь нет и разделить их
      // невозможно — что и требуется: трасса без рассылки была бы ложной
      // уликой, а `... 0 (teardown)` стенд прочитал бы как «рассылка
      // состоялась».
      if (entry.characterUid != data::InvalidUid)
      {
        const protocol::Presence offlinePresence{
          .status = protocol::Status::Offline,
          .scene = protocol::Presence::Scene::Ranch,
          .sceneUid = 0};
        BroadcastPresenceOfCharacter(
          entry.characterUid, offlinePresence, entry.clientId, "teardown");
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
    "Closed {} deferred messenger session(s) on the messenger thread"
    " ({} rebound and spared)",
    closed,
    rebound);
}

chat::ReapThresholds MessengerDirector::GetReapThresholds() const
{
  const auto& cfg = _serverInstance.GetSettings().chatReap;
  return chat::ReapThresholds{
    .handshakeTimeout = std::chrono::seconds(cfg.handshakeTimeoutSeconds),
    .orphanGrace = std::chrono::seconds(cfg.orphanGraceSeconds),
    .absoluteIdle = std::chrono::seconds(cfg.absoluteIdleSeconds)};
}

void MessengerDirector::HandleClientActivity(const network::ClientId clientId)
{
  // LOA-fix (R80-3, round80, backlog #235): ШТАМП АКТИВНОСТИ ЧАТ-СОКЕТА.
  //
  // ★UPDATE-ONLY, как у `RanchDirector::HandleClientActivity` (R21-2a): здесь
  // НЕЛЬЗЯ `_clients[clientId]` — запись, созданную `operator[]` уже после
  // уборки, не подберёт ни один выходной путь, и она осталась бы сиротой
  // навсегда. `find` + присваивание такой сироты создать не может.
  // ★`find`, а не `GetClientContext`: тот бросает, а бросок отсюда уходит в
  // сетевой read-loop и рвёт клиенту соединение — то есть крючок «жив ли пир»
  // сам бы пира и убивал.
  // ★ЗАМОК ИСКЛЮЧИТЕЛЬНЫЙ И ЛИСТОВОЙ: под ним ровно `find` и одно
  // присваивание, ни одного вызова наружу. Разделяемого мало — мы ПИШЕМ поле,
  // а копию карты снимают чужие потоки (`GetClientByCharacterUid` с ранча и
  // заезда). Цена — одно неоспариваемое взятие мьютекса на входящий кадр;
  // чат-канал событийный (единицы кадров в минуту на клиента), это не горячий
  // путь.
  const std::unique_lock lock(_clientsMutex);
  const auto clientIter = _clients.find(clientId);
  if (clientIter != _clients.end())
    clientIter->second.lastActivity = chat::ReapClock::now();
}

void MessengerDirector::SweepChatSockets()
{
  // LOA-fix (R80-3, round80, backlog #235): РАЗВЁРТКА-BACKSTOP.
  //
  // ★ПОЧЕМУ ЗДЕСЬ, А НЕ В `Tick()`. `Tick()` приходит с потока задач директора
  // (`RunDirectorTaskLoop`, 50 Гц) — ЧУЖОГО и для `_clients`, и для
  // `Server::_clients`, и, главное, для `Server::_addressStates`, которая живёт
  // ВОВСЕ БЕЗ ЗАМКА и правится приёмом и разрывом соединения. Решение о жатве,
  // принятое там, пришлось бы там же и исполнять. Это тот же довод, которым
  // R78 завёл очередь отложенных разрывов.
  //
  // ★ЗАЧЕМ ЯРУС 2, КОГДА ЕСТЬ ЯРУС 1 (закрытие на выходе игрока). Ярус 1
  // гейтится `revoked == true` и вообще не случается, если ключ мессенджера не
  // выдавался или уже перевыдан. И он НИКОГДА не видит сокет, который не
  // доходил до входа: у сканера нет ни персонажа, ни ключа, ни логаута.
  //
  // ★КОНФИГ ЧИТАЕТСЯ ОДИН РАЗ ЗА РАЗВЁРТКУ И ДО ЗАМКА: одно решение судит всех
  // клиентов одной развёртки по ОДНИМ порогам, и под замком не остаётся ни
  // одного обращения к настройкам.
  const auto now = chat::ReapClock::now();
  const auto& reapConfig = _serverInstance.GetSettings().chatReap;
  const auto sweepInterval = std::chrono::seconds(reapConfig.sweepIntervalSeconds);
  if (now - _lastChatSweep < sweepInterval)
    return;
  _lastChatSweep = now;
  const chat::ReapThresholds thresholds = GetReapThresholds();

  // ★СНИМОК ЛОББИ — ОДИН НА РАЗВЁРТКУ, А НЕ НА КЛИЕНТА: `SnapshotUsers` берёт
  // чужой `shared_lock`, и дёргать его N раз за проход значит держать чужой
  // замок N раз без нужды.
  // ★ЗАМКИ НЕ ПЕРЕСЕКАЮТСЯ ВО ВРЕМЕНИ, и это сильнее, чем «порядок захвата
  // строгий»: `SnapshotUsers()` возвращает КОПИЮ, копия целиком
  // перекладывается в `charactersInGame`, и только ПОСЛЕ этого берётся
  // `_clientsMutex`. Инверсии порядка не существует, потому что нет
  // вложенности.
  // ★СБОЙ СНИМКА ВЫКЛЮЧАЕТ ТОЛЬКО P2/P3, НЕ ВЕСЬ РАУНД. `SnapshotUsers` не
  // `noexcept` и ВЫДЕЛЯЕТ ПАМЯТЬ; жать «сироту», не сумев спросить, в игре ли
  // персонаж, — ровно тот катастрофический ложно-зелёный, ради которого гейт
  // окна выката писался «в пользу игрока».
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

      // ★НЕИЗВЕСТНОСТЬ — В ПОЛЬЗУ ИГРОКА: снимка нет ⇒ считаем персонажа В
      // ИГРЕ, и тогда правило вернёт `Keep` по P2 (P3 на проде выключен).
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

      // ★ОТВЯЗКА ДО ЗАКРЫТИЯ — ОБЯЗАТЕЛЬНА, И СЛИВ НА НЕЁ ОПИРАЕТСЯ:
      // `DrainPendingDisconnects` пропускает записи, успевшие перевязаться, и
      // распознаёт их именно по `characterUid == InvalidUid`. Личность
      // сохраняем В ОЧЕРЕДЬ: она нужна сливу для рассылки Offline.
      condemned.emplace_back(
        PendingDisconnect{
          .clientId = clientId,
          .characterUid = clientContext.characterUid});
      clientContext.isAuthenticated = false;
      clientContext.characterUid = data::InvalidUid;
      clientContext.otpCode.reset();
    }
  }

  if (condemned.empty())
    return;

  // Фаза 2 — постановка в СУЩЕСТВУЮЩУЮ очередь R78. ★Второго кросс-поточного
  // пути раунд не заводит: закрывает по-прежнему один-единственный слив.
  {
    const std::lock_guard lock(_pendingDisconnectsMutex);
    for (const PendingDisconnect& entry : condemned)
      _pendingDisconnects.emplace_back(entry);
  }

  // ★ОДНА СТРОКА НА РАЗВЁРТКУ, И ТОЛЬКО КОГДА ДЕЙСТВИТЕЛЬНО ЖАЛИ. Пер-сокетной
  // диагностики нет намеренно: «одна строка на пакет» — ровно тот флуд, который
  // R57 поймал на проде (15 350 строк из 15 589 за час).
  server::util::QuietLogInfo(
    "Reaped {} chat socket(s) on {}: {} handshake, {} orphan, {} idle",
    condemned.size(),
    "the messenger",
    handshake,
    orphan,
    idle);
}

void MessengerDirector::DisconnectUnboundSessions(
  const std::vector<network::ClientId>& unbound,
  const char* const reason,
  const data::Uid characterUid)
{
  // Фаза 2 — закрытие, СТРОГО ВНЕ обхода карты.
  //
  // ★ПОЧЕМУ ДВЕ ФАЗЫ, А НЕ ОДИН ЦИКЛ. `Client::End()` СИНХРОННО зовёт
  // `OnClientDisconnected` (`Server.cpp:85`), тот приходит в наш же
  // `HandleClientDisconnected`, а он снимает запись из `_clients` стражем
  // `RegistryEraser`. Отключение внутри обхода стирало бы элемент карты,
  // по которой мы идём, — инвалидация итератора посреди цикла.
  //
  // ★ОТКЛЮЧЕНИЕ БЕЗОПАСНО ТОЛЬКО ПОТОМУ, ЧТО ФЛАГ УЖЕ СНЯТ ФАЗОЙ 1: уборка
  // соединения рассылает присутствие Offline, а `HandleChatterUpdateState`
  // выходит на `not isAuthenticated`. Иначе закрытие мёртвого сокета сообщило
  // бы друзьям, что игрок вышел, — сразу после того, как он вошёл.
  for (const network::ClientId staleClientId : unbound)
  {
    try
    {
      _chatterServer.DisconnectClient(staleClientId);
    }
    catch (const std::exception&)
    {
      // Соединения уже нет — `Server::GetClient` бросает «Invalid client».
      // Это ровно та цель, которой мы добивались; ронять вход из-за неё нельзя.
    }
  }

  // Одна строка на СОБЫТИЕ, а не на пакет: и вход, и выход — события редкие.
  // ★ЧИСЛО — ЭТО ОТВЯЗАННЫЕ ЗАПИСИ, А НЕ ЗАКРЫТЫЕ СОКЕТЫ (NIT ревью 1):
  // отвязка фазы 1 безусловна, а закрытие фазы 2 может не состояться, если
  // соединения уже нет. Именно отвязка и решает, куда пойдёт адресная доставка,
  // поэтому считаем её.
  server::util::QuietLogInfo(
    "Evicted {} stale messenger session(s) of character {} on {}",
    unbound.size(),
    characterUid,
    reason);
}

void MessengerDirector::SendStallionReward(
  data::Uid characterUid,
  data::Uid horseUid,
  const BreedingMarket::Earnings& earnings)
{
  std::string characterName{};
  _serverInstance.GetDataDirector().GetCharacter(characterUid).Immutable(
    [&characterName](const data::Character& character)
    {
      characterName = character.name();
    });

  std::string horseName{};
  _serverInstance.GetDataDirector().GetHorse(horseUid).Immutable(
    [&horseName](const data::Horse& horse)
    {
      horseName = horse.name();
    });

  // UTC now in seconds
  const auto& utcNow = std::chrono::floor<std::chrono::seconds>(util::Clock::now());
  const auto& formattedDt = std::format(DateTimeFormat, utcNow);

  const auto mailType = earnings.timesMated > 0
    ? data::Mail::MailType::BreedingReward
    : data::Mail::MailType::NoReply;

  const std::string mailHeader = std::format(
    "Hello {}~\n"
    "Your breeding registration at the Stato Breeding Centre for\n"
    "your horse \"{}\" has ended.\n\n",
    characterName,
    horseName);

  // Prepare mail body
  std::string mailBody;
  if (earnings.timesMated > 0)
  {
    mailBody = mailHeader + std::format(
      std::locale(""),
      "Times Bred: {}\n"
      "Total Revenue: {:L} Carrots\n"
      "Tax: {:.1Lf}%\n"
      "Final Payout: <font color=#C36A0C>{:L} Carrots</font>",
      earnings.timesMated,
      earnings.revenue,
      earnings.taxRate * 100.0f,
      earnings.earnings);
  }
  else
  {
    mailBody =
      mailHeader +
      "Unfortunately, your horse was not mated during this period.";
  }

  // Create and store mail
  auto mailRecord = _serverInstance.GetDataDirector().CreateMail();
  data::Uid mailUid{data::InvalidUid};
  mailRecord.Mutable([&mailUid, mailBody, utcNow, characterUid, claimUid = earnings.claimUid, mailType](data::Mail& mail)
  {
    // Set mail parameters
    mail.from() = 0; // System
    mail.to() = characterUid;

    mail.type() = mailType;
    mail.claimUid() = claimUid; // TODO: implement and use claim system to generate a claim uid

    mail.createdAt() = utcNow;
    mail.body() = mailBody;

    // Get mailUid to store in character record
    mailUid = mail.uid();
  });

  // Add the new mail to the recipient's inbox
  _serverInstance.GetDataDirector().GetCharacter(characterUid).Mutable(
    [mailUid](data::Character& character)
    {
      // Insert new mail to the beginning of the list
      // TODO: this operation is O(n), does dao support std::deque?
      character.mailbox.inbox().insert(
        character.mailbox.inbox().begin(),
        mailUid);

      // Set mail alarm
      character.mailbox.hasNewMail() = true;
    });

  // Check if recipient is online for live mail delivery
  //
  // LOA-fix (R78-fix3, round78, backlog #255, находка Codex 2 итерации 2):
  // ЧИТАЕМ ПО СНИМКУ ПОД РАЗДЕЛЯЕМЫМ ЗАМКОМ, а не по живой карте.
  //
  // ★ЭТО ВТОРОЙ ЧУЖОЙ ПОТОК, И Я ЕГО ПРОПУСТИЛА. Путь измерен ревьюером и
  // проверен мной: `RanchDirector::HandleUnregisterStallion` ->
  // `BreedingMarket::UnregisterStallion` -> `SendBreedingPayoutMail` ->
  // `SendStallionReward`. То есть перебор идёт с потока РАНЧА и может совпасть
  // с фазой 1 вытеснения, которая пишет элементы этой же карты с потока чата.
  // Первая редакция замка закрыла только `GetClientByCharacterUid` — «сколько
  // ещё читателей у карты» надо было СЧИТАТЬ, а не осматривать.
  const auto clientsSnapshot = [this]
  {
    const std::shared_lock lock(_clientsMutex);
    return _clients;
  }();

  auto client = std::ranges::find_if(
    clientsSnapshot,
    [characterUid](const std::pair<network::ClientId, ClientContext>& client)
    {
      return client.second.characterUid == characterUid;
    });

  if (client == clientsSnapshot.cend())
    // Character is not online, all good and handled
    return;

  const protocol::ChatCmdLetterArriveTrs notify{
    .mailUid = mailUid,
    .mailType = mailType,
    .claimUid = earnings.claimUid,
    .sender = GetSystemNameFromType(mailType),
    .date = formattedDt,
    .body = mailBody
  };

  const network::ClientId recipientClientId = client->first;
  _chatterServer.QueueCommand<decltype(notify)>(recipientClientId, [notify](){ return notify; });
}

void MessengerDirector::Tick()
{
  // ★ЗДЕСЬ ДРЕНАЖА НЕТ, И ЭТО ИЗМЕРЕНО, А НЕ ВЫВЕДЕНО (R78-fix7).
  //
  // `Tick()` зовёт `RunDirectorTaskLoop` со СВОЕГО потока директора
  // (`ServerInstance.cpp:246-252`), а пакеты мессенджера разбирает ДРУГОЙ поток
  // — тот, на котором крутится `_io_ctx` чат-сервера. Первая редакция фикса
  // дренажила отсюда, и предикат идентичности потока поймал это на стенде:
  // закрытие печаталось потоком директора (Thread 10), а `ChatCmdLogin`,
  // `HandleClientConnected` и чтения карты — потоком чата (Thread 14).
  // То есть работа лишь ПЕРЕЕХАЛА с чужого потока на другой чужой.
  //
  // Дренаж живёт в `HandleNetworkTick()` — единственной точке, приходящей
  // с того же потока, что и разбор пакетов.
}

Config::Messenger& MessengerDirector::GetConfig()
{
  return _serverInstance.GetSettings().messenger;
}

void MessengerDirector::HandleClientConnected(const network::ClientId clientId)
{
  server::util::QuietLogDebug("Client {} connected to the messenger server from {}",
    clientId,
    _chatterServer.GetClientAddress(clientId).to_string());
  // LOA-fix (R78-fix3, round78, backlog #255): ВСТАВКА — ПОД ИСКЛЮЧИТЕЛЬНЫМ
  // ЗАМКОМ. Найдено ПЕРЕПИСЬЮ всех 18 обращений к карте, а не осмотром: ревью
  // указало, что «сколько ещё читателей» надо считать. Вставка способна вызвать
  // РЕХЭШ, то есть для копирующего с чужого потока она опаснее правки значений.
  {
    const std::unique_lock lock(_clientsMutex);
    // LOA (R80-3, round80, backlog #235): ОБЕ МЕТКИ СТАВЯТСЯ ПРИ РОЖДЕНИИ
    // ЗАПИСИ. Через `try_emplace` + присваивание, а не агрегатом: `ClientContext`
    // уже имеет NSDMI, и агрегатная инициализация перечисляла бы поля, которых
    // раунд не касается.
    const auto now = chat::ReapClock::now();
    const auto [clientIter, inserted] = _clients.try_emplace(clientId);
    clientIter->second.connectedAt = now;
    clientIter->second.lastActivity = now;
  }
}

void MessengerDirector::HandleClientDisconnected(const network::ClientId clientId)
{
  server::util::QuietLogDebug("Client {} disconnected from the messenger server", clientId);

  // LOA-fix (R50-7, round50, backlog #180): рассылка присутствия «не в сети» —
  // бросающая работа, а запись реестра снималась после неё. Осиротевшая запись
  // держит присутствие живым: друзья видят игрока в сети, пока сервер не
  // перезапустят.
  // LOA-fix (R78-fix3, round78, backlog #255, находка Codex 1 итерации 2):
  // СНЯТИЕ ЗАПИСИ — ПОД ИСКЛЮЧИТЕЛЬНЫМ ЗАМКОМ.
  //
  // ★ПОЧЕМУ ЭТО ДЕЛО ИМЕННО R78. Прежний `util::RegistryEraser` о замке ничего не
  // знает и стирает запись голыми руками — ровно тогда, когда карту может
  // копировать `GetClientByCharacterUid` с потока ранча или заезда. Удаление к
  // тому же способно вызвать рехэш, то есть это ХУЖЕ правки значений. До раунда
  // сюда приходили только настоящие разрывы; ВЫТЕСНЕНИЕ (R78) зовёт этот путь
  // САМО, синхронно из `DisconnectClient`, — значит гонку приводит сюда раунд,
  // и закрыть её обязан он же.
  //
  // ★ГАРАНТИЯ R50 СОХРАНЕНА: страж остаётся RAII и снимает запись на выходе при
  // любом исходе, просто теперь под замком. Форма — та же, что у
  // `LobbyNetworkHandler::HandleClientDisconnected` (R64-3).
  //
  // ★САМОЗАХВАТА НЕТ: замок берётся ТОЛЬКО в деструкторе, то есть после
  // `RunCleanupStep`, а ВСЕ разрывы (обе фазы вытеснения, слив и отказ входа)
  // зовутся с отпущенным замком — см. R78-fix9 в `HandleChatterLogin`.
  //
  // LOA-fix (R78-fix10, round78, backlog #255, ревю #4 BLOCK, наблюдаемость):
  // СНЯТИЕ ЗАПИСИ ОТЧИТЫВАЕТСЯ О СЕБЕ.
  //
  // ★ЗАЧЕМ СТРОКА В ЛОГ. Снятие живёт в деструкторе с `catch(...)`, то есть
  // МОЛЧА переживает любой сбой — именно так самозахват замка (ревю #4) и
  // оставлял вечного зомби, не оставляя следа. Проверить «зомби не осталось»
  // по проводу нельзя: карта клиентов наружу не видна. Поэтому снятие само
  // печатает, ЧТО оно сняло и сколько сессий осталось, — и стенд судит по
  // состоянию сервера, а не по выводу «раз соединение закрылось, значит
  // запись снята».
  // ★СТРОКА ПЕЧАТАЕТСЯ УЖЕ БЕЗ ЗАМКА: под ним не остаётся ни одного выхода
  // в чужой код, включая логгер.
  struct LockedContextEraser final
  {
    MessengerDirector& director;
    network::ClientId clientId;

    ~LockedContextEraser() noexcept
    {
      try
      {
        size_t erased = 0;
        size_t remaining = 0;
        {
          const std::unique_lock lock(director._clientsMutex);
          erased = director._clients.erase(clientId);
          remaining = director._clients.size();
        }

        server::util::QuietLogDebug(
          "Messenger registry: erased {} entry for client {}, {} session(s) left",
          erased,
          clientId,
          remaining);
      }
      catch (...)
      {
        // Бросок из деструктора — это `std::terminate` (урок round49).
      }
    }
  } const eraser{*this, clientId};

  // Call update state like a client would do before disconnect
  util::RunCleanupStep(
    "messenger presence update",
    clientId,
    [&]()
    {
      HandleChatterUpdateState(clientId, protocol::ChatCmdUpdateState{
        .presence = protocol::Presence{
          .status = protocol::Status::Offline,
          .scene = protocol::Presence::Scene::Ranch,
          .sceneUid = 0
        }});
    });

  // TODO: broadcast notify to friends & guilds that character is offline
}

void MessengerDirector::HandleChatterLogin(
  const network::ClientId clientId,
  const protocol::ChatCmdLogin& command)
{
  // LOA-fix (R78-fix8, round78, backlog #255, находка ревю #3 WARN-2):
  // КЛЮЧ В ЛОГ НЕ ПОПАДАЕТ.
  //
  // ★ЧТО ИЗМЕНИЛ РАУНД. Строка стоит с базы, но раньше печаталось ОДНОРАЗОВОЕ
  // значение, потраченное первым же входом и протухавшее за 30 секунд. После
  // перехода на LTK то же поле — живой credential на весь лобби-сеанс, а
  // `logs/log.txt` пишется на уровне debug безусловно (`main.cpp:174`). Одной
  // строки лога хватало, чтобы собрать `uid` + `code` и войти как жертва.
  // Убираем аргумент — ровно так же, как строка ниже уже поступает с
  // `command.name`. `guildUid` оставляем: он не credential.
  server::util::QuietLogDebug("[{}] ChatCmdLogin: {} {}",
    clientId,
    command.characterUid,
    command.guildUid);

  auto& clientContext = GetClientContext(clientId, false);

  // LOA-fix (R78-fix5, round78, backlog #255, находка ревю W3): ПОВТОРНЫЙ ВХОД
  // НА ТОМ ЖЕ СОЕДИНЕНИИ НЕ ПЕРЕИГРЫВАЕТ ТЕЛО.
  //
  // ★ЧТО СЛОМАЛОСЬ ИМЕННО ЭТИМ РАУНДОМ. Прежний `AuthorizeCode` СТИРАЛ код при
  // успехе, поэтому на один выданный код приходился ровно ОДИН успешный вход.
  // `AuthorizeLtk` ключ не тратит — и без этого гарда один вошедший клиент мог
  // гнать `ChatCmdLogin` потоком, каждый раз заставляя сервер перечитывать
  // почтовый ящик, обходить все группы контактов и всех друзей и собирать
  // ответ, на ЕДИНСТВЕННОМ потоке чат-сервера. Раунд обязан не отдавать
  // усилитель, которого до него не было.
  //
  // ★СЦЕНАРИЙ РАУНДА НЕ ЗАДЕТ: честное переподключение приходит НОВЫМ
  // соединением с чистым контекстом (`isAuthenticated == false`), поэтому
  // гард его не видит. Срабатывает он только на повторе в ОДНОМ соединении,
  // чего настоящий клиент не делает.
  //
  // ★СВЕРЯЕМ И ЛИЧНОСТЬ: вход под ДРУГИМ персонажем обязан идти полным путём
  // через `AuthorizeLtk`, иначе гард стал бы дырой «вошёл как свой — обслужен
  // как чужой».
  // ★ЧТЕНИЕ — ПОД ЗАМКОМ (NIT ревю #2 N2). Оба поля вправе переписать фазой 1
  // гашения чужой поток (выход игрока, GM-бан), поэтому читать их голыми
  // руками — та же гонка, что и писать. Снимаем ОБА под одним разделяемым
  // замком: решение принимается по согласованной паре, а не по двум значениям
  // из разных мгновений.
  bool alreadyLoggedInAsSameCharacter = false;
  {
    const std::shared_lock lock(_clientsMutex);
    alreadyLoggedInAsSameCharacter =
      clientContext.isAuthenticated
      && clientContext.characterUid == command.characterUid;
  }

  if (alreadyLoggedInAsSameCharacter)
  {
    server::util::QuietLogDebug(
      "[{}] ChatCmdLogin: already logged in as character {}, replay ignored",
      clientId,
      command.characterUid);

    const protocol::ChatCmdLoginAckOK replayAck{};
    _chatterServer.QueueCommand<decltype(replayAck)>(
      clientId, [replayAck](){ return replayAck; });
    return;
  }

  // Generate identity hash based on the character uid from the command and
  // the messenger otp constant
  size_t identityHash = std::hash<uint32_t>()(command.characterUid);
  boost::hash_combine(identityHash, MessengerOtpConstant);

  // LOA-fix (R78-1, round78, backlog #255): МЕССЕНДЖЕР ЖИВЁТ НА LTK, КАК ALL-CHAT.
  //
  // Здесь стоял `AuthorizeCode`, а он СТИРАЕТ код при успехе и держит его всего
  // 30 секунд (`OtpSystem.cpp:22-37`). Код же выдаётся ровно один раз — на
  // `AcCmdCLGetMessengerInfo` при входе в лобби, и клиент помнит его всю сессию.
  // Поэтому первый вход код тратил, а любое переподключение мессенджера (возврат
  // из заезда, обрыв связи) присылало тот же код в пустоту: в проде 09:22:15
  // `failed authentication with auth code 1854698400` — тот самый код, что прошёл
  // в 09:03:31, — и 39 минут без лички и писем до полного перезахода.
  //
  // ★ЭТО НЕ НОВЫЙ ДИЗАЙН, А ДОВЕДЕНИЕ АПСТРИМНОГО. Апстрим ловил ровно этот баг
  // у all-chat: `fdf0474a` «Fix all chat disconnecting after entering race», затем
  // `78a3c287` «Implement LTK codes (all chat fix)» с формулировкой «the game
  // client keeps persistent key codes in memory». LTK не стирается и не протухает,
  // но привязан к конечной точке. Мессенджеру ту же правку тогда не сделали.
  //
  // ★ЧТО МЕНЯЕТСЯ ДЛЯ АТАКУЮЩЕГО. Раньше подсмотренный код работал 30 секунд
  // с ЛЮБОГО адреса; теперь — только с того IPv4, с которого пришло лобби-
  // соединение, получившее код. Для удалённого наблюдателя это ослабление его
  // возможностей, а не усиление. Ключ по-прежнему считается от `characterUid`
  // ИЗ ПАКЕТА, поэтому вход под чужим uid с подсмотренным кодом не сходится.
  const bool authorized = _serverInstance.GetOtpSystem().AuthorizeLtk(
    identityHash,
    command.code,
    _chatterServer.GetClientAddress(clientId).to_uint());

  // LOA-fix (R78-fix6, round78, backlog #255, находка ревю W5): ЗАПИСИ В КАРТУ
  // — ПОД ИСКЛЮЧИТЕЛЬНЫМ ЗАМКОМ.
  //
  // ★ЗАМОК, ВЗЯТЫЙ ТОЛЬКО ЧИТАТЕЛЕМ, СИНХРОНИЗАЦИЕЙ НЕ ЯВЛЯЕТСЯ. Предыдущая
  // редакция закрыла у пары «читатель ранча/заезда ↔ писатель-входа» ровно
  // ОДНУ сторону: копии в `GetClientByCharacterUid` и `SendStallionReward`
  // ходят под `shared_lock`, а вход писал те же записи голыми руками. Это
  // гонка данных, то есть UB, и живёт она на строке, которую раунд сам и
  // переписывает. Решение — сперва РЕШИТЬ, потом записать под замком.
  {
    const std::unique_lock lock(_clientsMutex);
    clientContext.isAuthenticated = authorized;
  }

  if (not authorized)
  {
    // Login failed, bad actor, log and return
    // Do not log with `command.name` (character name) to prevent some form of string manipulation in spdlog
    // ★УРОВЕНЬ WARN ВКЛЮЧЁН ВСЕГДА, а в принятом §10 регрессе (multi-WAN)
    // игрок валит авторизацию КАЖДОЙ попыткой — и каждая печатала бы его всё
    // ещё живой ключ. Код убран (ревю #3 WARN-2).
    server::util::QuietLogWarn("Client {} tried to login as character {} but failed authentication",
      clientId,
      command.characterUid);

    protocol::ChatCmdLoginAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::LoginFailed};

    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });

    // TODO: confirm the cancel command is sent before disconnecting the client
    _chatterServer.DisconnectClient(clientId);
    return;
  }

  // Store this otp code for reauthentication with the guild login command (if at all)
  protocol::ChatCmdLoginAckOK response{};

  // LOA (R78-fix7, NIT ревю #2 N4): МАРКЕР САМОГО ТЕЛА, а не ветки гарда.
  // Предикат «тело исполнено один раз» считался по строкам гарда, то есть
  // мерил соседнюю ветку, а не ту, что защищают. Строка стоит ВНУТРИ тяжёлой
  // части, ПОСЛЕ раннего выхода: если гард снять, она напечатается на каждый
  // пакет, и стенд это увидит.
  server::util::QuietLogDebug(
    "[{}] ChatCmdLogin body: building the roster for character {}",
    clientId,
    command.characterUid);

  // Client request could be logging in as another character
  //
  // ★ЛИЧНОСТЬ СНИМАЕТСЯ В ЛОКАЛЬНУЮ ПЕРЕМЕННУЮ, А НЕ ПРЯМО В КАРТУ (R78-fix6).
  // Запись сюда шла ИЗ ЛЯМБДЫ `Mutable`, то есть под замком записи персонажа у
  // `DataDirector`. Тянуть наш замок карты клиентов внутрь чужого замка нельзя
  // — это готовая инверсия порядка блокировок. Поэтому читаем наружу, а в карту
  // пишем одним движением ниже, уже выйдя из `Mutable`.
  data::Uid boundCharacterUid{data::InvalidUid};
  std::vector<data::Uid> inbox{};
  _serverInstance.GetDataDirector().GetCharacter(command.characterUid).Mutable(
    [&boundCharacterUid, &inbox](data::Character& character)
    {
      boundCharacterUid = character.uid();
      inbox = character.mailbox.inbox();
      character.mailbox.hasNewMail() = false;
    });

  // Три оставшиеся записи входа — одним исключительным замком, без единого
  // выхода в чужой код под ним.
  //
  // LOA-fix (R78-fix9, round78, backlog #255, находка ревю #4 BLOCK):
  // РЕШЕНИЕ ПРИНИМАЕТСЯ ПОД ЗАМКОМ, ДЕЙСТВИЕ ВЫПОЛНЯЕТСЯ ВНЕ ЕГО.
  //
  // ★ЧТО БЫЛО НЕВЕРНО. Предыдущая редакция на провале перепроверки звала
  // `DisconnectClient` НЕ ВЫХОДЯ из этого замка — то есть ровно то, что
  // запрещает строка над ним. Разрыв синхронный: `ChatterServer::
  // DisconnectClient` → `Client::End()` → `OnClientDisconnected` →
  // `HandleClientDisconnected`, и всё это НА ЭТОМ ЖЕ ПОТОКЕ. Уборка снова
  // берёт `_clientsMutex` (страж записи), а он `std::shared_mutex` и
  // рекурсию не поддерживает.
  //
  // ★ЦЕНА БЫЛА НЕ «ПОДВИСНЕТ», А «СЛОМАЕТСЯ НАВСЕГДА». На нашем gcc-15/glibc
  // самозахват не висит: реализация возвращает EDEADLK, `std::unique_lock`
  // бросает `system_error`, а страж — деструктор с `catch(...)`, поэтому
  // бросок глотается и `_clients.erase` НЕ ВЫПОЛНЯЕТСЯ. В карте остаётся
  // запись мёртвого соединения с поднятым флагом и НЕВЫЯСНЕННОЙ личностью,
  // а `ClientId` монотонный и не переиспользуется — то есть зомби вечный.
  // Дальше каждая рассылка присутствия (любой вход, выход, смена статуса
  // ЛЮБОГО игрока) шла бы по нему в `GetCharacter(InvalidUid).Immutable`,
  // а тот бросает — и рассылка обрывалась бы ДО отправки уведомлений
  // друзьям и гильдии, на весь сервер, до перезапуска процесса.
  //
  // ★ФОРМА ФИКСА — ТА ЖЕ, ЧТО У СОСЕДЕЙ: фаза 2 вытеснения
  // (`DisconnectUnboundSessions`) и слив (`DrainPendingDisconnects`) тоже
  // сперва решают под замком, а рвут соединение уже без него.
  bool keyLostDuringLogin = false;
  {
    const std::unique_lock lock(_clientsMutex);

    // LOA-fix (R78-fix8, round78, backlog #255, NIT ревю #3 №2): ПЕРЕПРОВЕРКА
    // КЛЮЧА В ТОЧКЕ ПРИВЯЗКИ.
    //
    // ★ОКНО, КОТОРОЕ ЭТИМ ЗАКРЫВАЕТСЯ. Между сверкой ключа и публикацией
    // личности лежит всё тяжёлое тело входа (чтение ящика, групп, друзей —
    // десятки миллисекунд, длина задаётся клиентом). Игрок мог за это время
    // выйти из игры: лобби сняло бы ключ и попросило погасить сессии — но
    // фаза 1 ищет по `characterUid`, которого этот вход ЕЩЁ НЕ опубликовал,
    // никого не находит и молча уходит. Вход довязывался бы к персонажу,
    // чей ключ уже отозван.
    //
    // ★СТОИТ ПОД ТЕМ ЖЕ ЗАМКОМ, ЧТО И ПУБЛИКАЦИЯ: между проверкой и записью
    // не остаётся ни одного оператора. Своего мьютекса `OtpSystem` не роняет
    // на наш — он никого не зовёт наружу, цикла блокировок нет.
    if (not _serverInstance.GetOtpSystem().AuthorizeLtk(
          identityHash,
          command.code,
          _chatterServer.GetClientAddress(clientId).to_uint()))
    {
      // ★ФЛАГ СНИМАЕТСЯ ЗДЕСЬ, И ЭТО НЕСУЩЕЕ, А НЕ ГИГИЕНА. Вход отвергнут,
      // значит к моменту разрыва сессия обязана выглядеть НЕаутентифицированной:
      // иначе уборка соединения пойдёт в `HandleChatterUpdateState` с поднятым
      // флагом и невыясненной личностью и станет рассылать присутствие за
      // `characterUid == InvalidUid`. Ровно так же устроена ветка
      // `not authorized` выше: там флаг к моменту разрыва уже `false`.
      clientContext.isAuthenticated = false;
      keyLostDuringLogin = true;
    }
    else
    {
      clientContext.otpCode.emplace(command.code);
      clientContext.characterUid = boundCharacterUid;
      clientContext.presence = protocol::Presence{
        .status = protocol::Status::Online,
        .scene = protocol::Presence::Scene::Ranch,
        .sceneUid = boundCharacterUid
      };
    }
  }

  if (keyLostDuringLogin)
  {
    server::util::QuietLogWarn(
      "Client {} lost its messenger key while logging in as character {}"
      " — the session is not bound",
      clientId,
      command.characterUid);

    protocol::ChatCmdLoginAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::LoginFailed};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });

    // ★С ЭТОЙ СТРОКИ `clientContext` — ВИСЯЧАЯ ССЫЛКА: разрыв синхронно уводит
    // в уборку, а та стирает запись из `_clients`. Ниже к ней не обращаемся —
    // как и ветка `not authorized`, которая устроена так же.
    _chatterServer.DisconnectClient(clientId);
    return;
  }

  // LOA-fix (R78-2, round78, backlog #255): ВХОД ОСТАВЛЯЕТ РОВНО ОДНУ ПРИВЯЗКУ.
  //
  // Стоит ИМЕННО ЗДЕСЬ, а не сразу после авторизации: личность соединения
  // становится известна только строкой выше — её берут из записи персонажа, а
  // не из пакета. Вытеснять по неподтверждённому `command.characterUid` было бы
  // вытеснением не того.
  //
  // ★БЕЗ ЭТОГО ПОЧИНКА ВХОДА (R78-1) НЕ ВИДНА ИГРОКУ. Старое соединение сервер
  // держит вечно (#235), клиент возвращается вторым — и обе записи оказались бы
  // привязаны к одному персонажу. Разбор — в `MessengerSessionEviction.hpp`.
  // #235 этим НЕ закрывается: течь чат-сокетов как класс остаётся.
  EvictOtherSessionsOfCharacter(clientId, boundCharacterUid);

  // Check if inbox contains any unread mails, count and populate response
  for (const data::Uid mailUid : inbox)
  {
    const auto& mailRecord = _serverInstance.GetDataDirector().GetMail(mailUid);
    if (not mailRecord)
      continue;

    mailRecord.Immutable([&response](const data::Mail& mail)
    {
      if (mail.isRead() or mail.isDeleted())
        return;

      // Increment unread mail counter
      response.mailAlarm.unreadMailCount++;
    });
  }

  if (response.mailAlarm.unreadMailCount != 0)
    response.mailAlarm.hasMail = true;

  // Load friends from character's stored friends list
  std::set<data::Uid> pendingFriends{};
  std::map<data::Uid, data::Character::Contacts::Group> groups{};
  _serverInstance.GetDataDirector().GetCharacter(command.characterUid).Immutable(
    [&pendingFriends, &groups](const data::Character& character)
    {
      pendingFriends = character.contacts.pending();
      groups = character.contacts.groups();
    });

  // Initialise with one group for now (friends)
  // LOA-fix (R78-fix6, round78, backlog #255, находка ревю W5): СНИМОК ВМЕСТО
  // ЖИВОЙ КАРТЫ. Оба цикла ниже читают присутствие соседей, а внутри зовут
  // `DataDirector` — держать замок карты клиентов через чужой вызов нельзя.
  // Снимаем копию под разделяемым замком и дальше идём по ней; это тот же
  // приём, что уже стоит в `HandleChatterUpdateState`.
  const auto onlineSnapshot = [this]
  {
    const std::shared_lock lock(_clientsMutex);
    return _clients;
  }();

  response.groups.emplace_back(FriendsCategoryUid, "");

  // Loop through every group to prepare response
  for (const auto& [groupUid, group] : groups)
  {
    // Add group to response
    response.groups.emplace_back(groupUid, group.name);

    // Build friends list for response
    for (const data::Uid& friendUid : group.members)
    {
      const auto friendCharacterRecord = _serverInstance.GetDataDirector().GetCharacter(friendUid);
      if (!friendCharacterRecord.IsAvailable())
        continue;

      auto& friendo = response.friends.emplace_back();
      friendCharacterRecord.Immutable([&friendo, groupUid](const data::Character& friendCharacter)
      {
        friendo.name = friendCharacter.name();
        friendo.uid = friendCharacter.uid();
        friendo.categoryUid = groupUid;
      });

      // Check if friend is online by looking for them in messenger clients
      friendo.status = protocol::Status::Offline;
      for (const auto& [onlineClientId, onlineClientContext] : onlineSnapshot)
      {
        if (onlineClientContext.isAuthenticated && onlineClientContext.characterUid == friendUid)
        {
          friendo.status = onlineClientContext.presence.status;
          friendo.scene = onlineClientContext.presence.scene;
          friendo.sceneUid = onlineClientContext.presence.sceneUid;
          break;
        }
      }
    }
  }

  // Pending friend requests
  for (const data::Uid& pendingUid : pendingFriends)
  {
    const auto friendCharacterRecord = _serverInstance.GetDataDirector().GetCharacter(pendingUid);
    if (!friendCharacterRecord.IsAvailable())
      continue;

    auto& friendo = response.friends.emplace_back();
    friendCharacterRecord.Immutable([&friendo](const data::Character& friendCharacter)
    {
      friendo.name = friendCharacter.name();
      friendo.uid = friendCharacter.uid();
      friendo.categoryUid = FriendsCategoryUid;
    });
    
    friendo.member5 = 2; // Pending request
    
    // Check if friend is online by looking for them in messenger clients
    friendo.status = protocol::Status::Offline;
    for (const auto& [onlineClientId, onlineClientContext] : onlineSnapshot)
    {
      if (onlineClientContext.isAuthenticated && onlineClientContext.characterUid == pendingUid)
      {
        friendo.status = onlineClientContext.presence.status;
        friendo.scene = onlineClientContext.presence.scene;
        friendo.sceneUid = onlineClientContext.presence.sceneUid;
        break;
      }
    }
  }

  _chatterServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  // The client sometimes fails to update online state with `ChatCmdUpdateState` command
  // and leaves the server (and subsequently friends/guildmates) in limbo, resulting incorrect online state.
  // Emit online state to relevant players in addition
  // to the client possibly invoking this command.
  HandleChatterUpdateState(clientId, protocol::ChatCmdUpdateState{
    .presence = protocol::Presence{
      .status = protocol::Status::Online,
      .scene = protocol::Presence::Scene::Ranch,
      // ★ЛОКАЛЬНАЯ ВЕЛИЧИНА, А НЕ ЧТЕНИЕ ЧЕРЕЗ ССЫЛКУ (R78-fix7, ревю #2).
      // Ссылка на запись карты живёт здесь через всё тяжёлое тело входа, и
      // читать её в конце значило бы читать поле, которое чужой поток вправе
      // переписать фазой 1 гашения. Значение уже снято и неизменно.
      .sceneUid = boundCharacterUid
    }});
}

void MessengerDirector::HandleChatterBuddyAdd(
  const network::ClientId clientId,
  const protocol::ChatCmdBuddyAdd& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Get target character uid by name, if any
  const data::Uid targetCharacterUid = 
    _serverInstance
    .GetDataDirector()
    .GetDataSource()
    .RetrieveCharacterUidByName(command.characterName);

  // Check if character by than name exists
  if (targetCharacterUid == data::InvalidUid)
  {
    // Character by that name does not exist
    // TODO: return protocol::ChatCmdBuddyAddAckCancel (BuddyAddCharacterDoesNotExist)
    return;
  }

  // Get invoker's character name
  // TODO: we could store character name in client context and check instead of retrieving character record
  std::string invokerCharacterName{};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&invokerCharacterName](const data::Character& character)
    {
      invokerCharacterName = character.name();
    });

  // Check if invoker is attempting to add itself
  if (command.characterName == invokerCharacterName)
  {
    // Character cannot add itself as a friend.
    // The game should already deny this, but we validate serverside too.
    // TODO: return protocol::ChatCmdBuddyAddAckCancel (BuddyAddCannotAddSelf)
    return;
  }

  // Check if there is already a pending request to the same character
  bool isAlreadyPending{false};
  _serverInstance.GetDataDirector().GetCharacter(targetCharacterUid).Immutable(
    [&isAlreadyPending, requestingCharacterUid = clientContext.characterUid](const data::Character& character)
    {
      isAlreadyPending = std::ranges::contains(
        character.contacts.pending(),
        requestingCharacterUid);
    });

  if (isAlreadyPending)
  {
    // TODO: handle this case (e.g. notify user)
    return;
  }

  // Add to pending friend request
  _serverInstance.GetDataDirector().GetCharacter(targetCharacterUid).Mutable(
    [&clientContext](data::Character& character)
    {
        character.contacts.pending().emplace(clientContext.characterUid);
    });

  // Check if character is online, if so send request live, 
  // else queue it up for when character next comes online.
  const auto clientsSnapshot = _clients;
  auto targetClient = std::ranges::find_if(
    clientsSnapshot,
    [targetCharacterUid](const auto& client)
    {
      return client.second.characterUid == targetCharacterUid;
    });

  // Notify responding character, if they are online
  if (targetClient != clientsSnapshot.cend())
  {
    // Target is online, send friend request to recipient
    const ClientId targetClientId = targetClient->first;
    protocol::ChatCmdBuddyAddRequestTrs notify{
      .requestingCharacterUid = clientContext.characterUid,
      .requestingCharacterName = invokerCharacterName};
    _chatterServer.QueueCommand<decltype(notify)>(targetClientId,[notify](){ return notify; });
  }
}

void MessengerDirector::HandleChatterBuddyAddReply(
  const network::ClientId clientId,
  const protocol::ChatCmdBuddyAddReply& command)
{
  server::util::QuietLogDebug("ChatCmdBuddyAddReply: {} {}",
    command.requestingCharacterUid,
    command.requestAccepted);

  const auto& clientContext = GetClientContext(clientId);
  
  // Get requesting character's record
  const auto& requestingCharacterRecord = _serverInstance.GetDataDirector().GetCharacter(
    command.requestingCharacterUid);
  
  // Validate such character exists
  if (not requestingCharacterRecord.IsAvailable())
  {
    // Responding character responded to a friend request from an unknown character uid
    // TODO: return protocol::ChatCmdBuddyAddAckCancel (BuddyAddUnknownCharacter) - will this work?
    return;
  }

  // Get requesting character's name
  std::string requestingCharacterName{};
  requestingCharacterRecord.Immutable(
    [&requestingCharacterName](const data::Character& character)
    {
      requestingCharacterName = character.name();
    });

  // Get responding character's record
  const auto& respondingCharacterRecord = _serverInstance.GetDataDirector().GetCharacter(
    clientContext.characterUid);

  // Get responding character's name
  std::string respondingCharacterName{};
  respondingCharacterRecord.Immutable(
    [&respondingCharacterName](const data::Character& character)
    {
      respondingCharacterName = character.name();
    });

  // If friend request accepted, add each other to friends list
  if (command.requestAccepted)
  {
    // Helper lambda to add a character to character's friends list
    const auto& acceptFriendRequest = [](
      const Record<data::Character>& characterRecord,
      const data::Uid characterUid)
    {
      characterRecord.Mutable(
        [characterUid](data::Character& character)
        {
          // Erase other character from character's pending friend requests
          character.contacts.pending().erase(characterUid);
          // Add other character to character's friends list
          auto& groups = character.contacts.groups();
          // Friends group might not be initially initialised, try create it
          auto [friendsGroupIter, created] = groups.try_emplace(FriendsCategoryUid);
          auto& friendsGroup = friendsGroupIter->second;

          if (created)
          {
            // Label the group for internal use only
            // TODO: is this needed? Helps visually but does not affect game
            friendsGroup.name = "_internal_friends_group_";
            friendsGroup.createdAt = util::Clock::now();
          }

          friendsGroup.members.emplace(characterUid);
        });
    };

    // Add responding character to requesting character's friends list
    acceptFriendRequest(requestingCharacterRecord, clientContext.characterUid);

    // Add requesting character to responding character's friends list
    acceptFriendRequest(respondingCharacterRecord, command.requestingCharacterUid);

    // Check if requesting character is online, if so send response live,
    // else simply add responding character to friends list
    const auto clientsSnapshot = _clients;
    const auto requestingClient = std::ranges::find_if(
      clientsSnapshot,
      [requestingCharacterUid = command.requestingCharacterUid](const auto& client)
      {
        return client.second.characterUid == requestingCharacterUid;
      });

    protocol::ChatCmdBuddyAddAckOk response{};

    // Keep track of the requesting character's presence (if they are even online)
    std::optional<protocol::Presence> requestingCharacterPresence{};

    // Check if requesting character is still online to notify of friend request result
    if (requestingClient != clientsSnapshot.cend())
    {
      // Requesting character is online
      const ClientId requestingClientId = requestingClient->first;

      const ClientContext& requestingClientContext = requestingClient->second;
      requestingCharacterPresence.emplace(requestingClientContext.presence);

      // Populate response with responding character's information
      response.characterUid = clientContext.characterUid;
      response.characterName = respondingCharacterName;
      response.unk2 = 0; // TODO: identify this
      response.status = clientContext.presence.status;

      // Send response to requesting character
      _chatterServer.QueueCommand<decltype(response)>(requestingClientId, [response](){ return response; });

      // Notify the requesting client of the (invoker) new friend's online state  
      protocol::ChatCmdUpdateStateTrs stateNotify{
        protocol::ChatCmdUpdateState{
          clientContext.presence},
        clientContext.characterUid};

      _chatterServer.QueueCommand<decltype(stateNotify)>(requestingClientId, [stateNotify](){ return stateNotify; });
    }

    // Populate response with requesting character's information
    response.characterUid = command.requestingCharacterUid;
    response.characterName = requestingCharacterName;
    response.unk2 = 0; // TODO: identify this

    // Prepare update state for invoker of new friend's online presence
    protocol::ChatCmdUpdateStateTrs stateNotify{
      protocol::ChatCmdUpdateState{
        protocol::Status::Offline},
      command.requestingCharacterUid};

    if (requestingCharacterPresence.has_value())
    {
      // Requesting character is online, populate fields
      const auto& presence = requestingCharacterPresence.value();
      response.status = presence.status;
      stateNotify.presence = presence;
    }

    // Send response to responding character
    _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });

    // Notify the responding client (invoker) of the requester's (new friend's) online state  
    _chatterServer.QueueCommand<decltype(stateNotify)>(clientId, [stateNotify](){ return stateNotify; });
  }
  else
  {
    // Friend request rejected
    respondingCharacterRecord.Mutable(
      [requestingCharacterUid = command.requestingCharacterUid](data::Character& character)
      {
        character.contacts.pending().erase(requestingCharacterUid);
      });
  }
}

void MessengerDirector::HandleChatterBuddyDelete(
  network::ClientId clientId,
  const protocol::ChatCmdBuddyDelete& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Check if character by that uid even exist
  const auto& targetCharacterRecord = _serverInstance.GetDataDirector().GetCharacter(
    command.characterUid);
  if (not targetCharacterRecord.IsAvailable())
  {
    // Character by that uid does not exist or not available
    protocol::ChatCmdBuddyDeleteAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::BuddyDeleteTargetCharacterUnavailable};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Helper lambda to delete a character to character's friends list
  const auto& deleteFriend = [](
    const server::Record<data::Character>& characterRecord,
    const data::Uid characterUid)
  {
    characterRecord.Mutable(
      [characterUid](data::Character& character)
      {
        // Go through all groups and ensure friend is erased
        auto& groups = character.contacts.groups();
        for (auto& [groupUid, group] : groups)
        {
          group.members.erase(characterUid);
        }
      });
  };

  // Delete invoking character from target character's friend list
  deleteFriend(targetCharacterRecord, clientContext.characterUid);

  // Delete target character from invoking character's friend list
  deleteFriend(
    _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid),
    command.characterUid);

  // Return delete confirmation response to invoking character
  protocol::ChatCmdBuddyDeleteAckOk response{
    .characterUid = command.characterUid};
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });

  // Send delete confirmation to target character if they are online
  const auto clientsSnapshot = _clients;
  auto targetClient = std::ranges::find_if(
    clientsSnapshot,
    [targetCharacterUid = command.characterUid](const auto& client)
    {
      return client.second.characterUid == targetCharacterUid;
    });

  // If target character is online then send
  if (targetClient != clientsSnapshot.cend())
  {
    const ClientId targetClientId = targetClient->first;
    // Invoking character's uid to be used for indicating friend delete to target character
    response.characterUid = clientContext.characterUid;
    _chatterServer.QueueCommand<decltype(response)>(targetClientId, [response](){ return response; });
  }
}

void MessengerDirector::HandleChatterBuddyMove(
  network::ClientId clientId,
  const protocol::ChatCmdBuddyMove& command)
{
  server::util::QuietLogDebug("[{}] ChatCmdBuddyMove: {} {}",
    clientId,
    command.characterUid,
    command.groupUid);

  const auto& clientContext = GetClientContext(clientId);

  // 1. Check if group exists
  // 2. Check if already in that group
  // 3. Check if friends with target character
  // If all good, move friend to group

  std::optional<protocol::ChatterErrorCode> errorCode{};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&command, &errorCode](data::Character& character)
    {
      auto& groups = character.contacts.groups();

      // Check if group exists or if friend is in the target group
      if (not groups.contains(command.groupUid))
      {
        // Target group by that uid does not exist
        errorCode.emplace(protocol::ChatterErrorCode::BuddyMoveGroupDoesNotExist);
        return;
      }
      else if (std::ranges::contains(groups.at(command.groupUid).members, command.characterUid))
      {
        // Friend is already in the target group
        errorCode.emplace(protocol::ChatterErrorCode::BuddyMoveAlreadyInGroup);
        return;
      }

      // Go through groups, check if friends with character
      for (auto& [groupUid, group] : character.contacts.groups())
      {
        auto& members = group.members;

        // Find friend in this group
        auto friendIter = std::ranges::find_if(
          members,
          [command](const data::Uid& friendUid)
          {
            return friendUid == command.characterUid;
          });

        if (friendIter != members.cend())
        {
          // Friend found, move friend to the target group
          // Erase friend from current group
          members.erase(friendIter);

          // Add friend to target group
          auto& targetGroup = groups.at(command.groupUid);
          targetGroup.members.emplace(command.characterUid);
          return;
        }
      }

      // Loop did not early return, friend not found
      errorCode.emplace(protocol::ChatterErrorCode::BuddyMoveFriendNotFound);
    });

  if (errorCode.has_value())
  {
    protocol::ChatCmdBuddyMoveAckCancel cancel{
      .errorCode = errorCode.value()};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  protocol::ChatCmdBuddyMoveAckOk response{};
  response.characterUid = command.characterUid;
  response.groupUid = command.groupUid;
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterGroupAdd(
  network::ClientId clientId,
  const protocol::ChatCmdGroupAdd& command)
{
  server::util::QuietLogDebug("[{}] ChatCmdGroupAdd: {}", clientId, command.groupName);

  const auto& clientContext = GetClientContext(clientId);

  // TODO: implement the creation and storing of new group in character
  data::Uid groupUid{data::InvalidUid};
  std::optional<protocol::ChatterErrorCode> errorCode{};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&command, &groupUid, &errorCode](data::Character& character)
    {
      auto& groups = character.contacts.groups();

      // LOA-fix (R31-2, round31, backlog #127, SECURITY/REMOTE-CRASH):
      // ★rbegin() НА ПУСТОЙ std::map — ЭТО SIGSEGV. Строка ниже разыменовывает
      // rbegin() без единой проверки. Под -O2 -DNDEBUG это разыменование
      // нулевого узла дерева: не исключение, а нарушение памяти, поэтому
      // try/catch в ChatterServer::OnClientData его НЕ ЛОВИТ — падает весь
      // процесс сервера (лобби, ранчо, гонки и чат живут в одном бинаре).
      // ★КАК ДОХОДИТ ДО ПУСТОЙ MAP (2 шага, оба с провода): groups создаётся с
      // единственной группой по умолчанию {0} (LobbyNetworkHandler, создание
      // персонажа), а HandleChatterGroupDelete до R31-3 не запрещал удалять
      // группу 0 — клиент шлёт ChatCmdGroupDelete{groupUid = 0}, map пустеет,
      // следующий ChatCmdGroupAdd падает здесь.
      // ★ВТОРАЯ НОГА, БЕЗ УДАЛЕНИЯ ВООБЩЕ: аккаунты, заведённые через
      // register-on-first-use (#18c), стартуют с contacts.groups == null, то есть
      // с УЖЕ пустой картой — у них падает самый первый ChatCmdGroupAdd, и этот
      // guard покрывает в том числе такой свежий auth-аккаунт.
      // ★ЧИНИМ, А НЕ ОТКАЗЫВАЕМ. R31-3 закрывает вход, но у персонажей, чьи
      // сейвы УЖЕ испорчены до фикса, groups так и остаётся пустой — им отказ
      // не помог бы, а группа друзей нужна для всей контакт-логики. Поэтому
      // восстанавливаем инвариант «группа 0 существует всегда». Ровно так же
      // самолечится приём заявки в друзья выше по файлу (try_emplace
      // (FriendsCategoryUid) с пометкой «Friends group might not be initially
      // initialised»), так что поведение не новое, а уже принятое в этом файле.
      // После восстановления rbegin() указывает на группу 0 и nextGroupUid = 1 —
      // ровно то, что дал бы нетронутый сейв.
      if (groups.empty())
      {
        server::util::QuietLogWarn(
          "Character {} has no contact groups at all (default friends group was lost); "
          "restoring the default friends group before adding a new one",
          character.uid());

        auto& friendsGroup = groups[FriendsCategoryUid];
        friendsGroup.uid = FriendsCategoryUid;
        friendsGroup.name = "_internal_friends_group_";
        friendsGroup.createdAt = util::Clock::now();
      }

      const auto& nextGroupUid = groups.rbegin()->first + 1;

      // Sanity check if group uid is default friends group uid
      if (nextGroupUid == 0)
      {
        // We have somehow looped back to group uid 0 (which is default friends group)
        // TODO: respond with error code and return;
        return;
      }

      // Create group
      auto [iter, created] = groups.try_emplace(nextGroupUid);
      if (not created)
      {
        // Group by that new group uid already exists
        // Something went terribly wrong with the next group uid logic
        // TODO: respond with error code and return;
        return;
      }

      // Set response group uid
      groupUid = nextGroupUid;

      // Set group information
      auto& group = iter->second;
      group.uid = nextGroupUid;
      group.name = command.groupName;
      group.createdAt = util::Clock::now();
    });
  
  if (errorCode.has_value())
  {
    protocol::ChatCmdGroupAddAckCancel cancel{
      .errorCode = errorCode.value()};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  protocol::ChatCmdGroupAddAckOk response{
    .groupUid = groupUid,
    .groupName = command.groupName};
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterGroupRename(
  network::ClientId clientId,
  const protocol::ChatCmdGroupRename& command)
{
  const auto& clientContext = GetClientContext(clientId);

  std::optional<protocol::ChatterErrorCode> errorCode{};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&command, &errorCode](data::Character& character)
    {
      auto& groups = character.contacts.groups();

      // Check if group exists
      if (not groups.contains(command.groupUid))
      {
        // Group by that uid does not exist
        errorCode.emplace(protocol::ChatterErrorCode::GroupRenameGroupDoesNotExist);
        return;
      }

      // Check if group name is duplicate
      for (const auto& [groupUid, group] : groups)
      {
        if (group.name == command.groupName)
        {
          // Duplicate group name, cancel the rename
          errorCode.emplace(protocol::ChatterErrorCode::GroupRenameDuplicateName);
          return;
        }
      }

      // Set group name
      auto& group = groups.at(command.groupUid);
      group.name = command.groupName;
    });

  if (errorCode.has_value())
  {
    protocol::ChatCmdGroupRenameAckCancel cancel{
      .errorCode = errorCode.value()};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  protocol::ChatCmdGroupRenameAckOk response{};
  response.groupUid = command.groupUid;
  response.groupName = command.groupName;
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterGroupDelete(
  network::ClientId clientId,
  const protocol::ChatCmdGroupDelete& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Check if group by that uid exists
  std::optional<protocol::ChatterErrorCode> errorCode{};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&command, &errorCode](data::Character& character)
    {
      auto& groups = character.contacts.groups();

      // LOA-fix (R31-3, round31, backlog #127, SECURITY/REMOTE-CRASH):
      // ★ГРУППА ПО УМОЛЧАНИЮ НЕУДАЛЯЕМА. groupUid приходит С ПРОВОДА
      // (ChatCmdGroupDelete::Read читает один uint32) и до этого фикса ноль
      // проходил насквозь: contains(0) — правда, «перенос участников» копировал
      // группу 0 саму в себя, а затем erase(0) стирал её. Дальше:
      //   • следующий ChatCmdGroupAdd делает rbegin() на ПУСТОЙ map → SIGSEGV
      //     (корневая нога #127, см. R31-2);
      //   • AllChatDirector::HandleChatterInputState делает groups().at(0) →
      //     out_of_range на каждом изменении состояния ввода (ловится, но
      //     превращает чат игрока в мусор в логе);
      //   • сама ветка «default friend group missing» ниже становится
      //     недостижимой самопроверкой — она проверяла последствие, а не вход.
      // Отказ ставим ПЕРВЫМ, до contains(): группа 0 существует всегда, поэтому
      // порядок важен только для читаемости, но fail-closed-проверка входного
      // поля обязана стоять раньше любой работы с данными.
      // ★КОД ОШИБКИ переиспользуем существующий GroupDeleteGroupDoesNotExist:
      // клиент умеет его отрисовать, а заводить новый энумератор ChatterErrorCode
      // ради этого нельзя — клиентская таблица строк нам недоступна, неизвестный
      // код даст пустое окно вместо сообщения.
      if (command.groupUid == FriendsCategoryUid)
      {
        server::util::QuietLogWarn(
          "Character {} tried to delete the default friends group; refusing",
          character.uid());
        errorCode.emplace(protocol::ChatterErrorCode::GroupDeleteGroupDoesNotExist);
        return;
      }

      // Confirm the existence of the group and
      // sanity check the existence of default friends group
      if (not groups.contains(command.groupUid))
      {
        // Group does not exist, cannot delete
        errorCode.emplace(protocol::ChatterErrorCode::GroupDeleteGroupDoesNotExist);
        return;
      }
      else if (not groups.contains(FriendsCategoryUid))
      {
        // Default friend group somehow does not exist
        errorCode.emplace(protocol::ChatterErrorCode::GroupDeleteDefaultFriendGroupMissing);
        return;
      }

      // Group to be deleted
      const auto& group = groups.at(command.groupUid);
      // Default friends group
      auto& friendsGroup = groups.at(FriendsCategoryUid);

      // Move all group members back into the friends list
      for (const data::Uid& friendUid : group.members)
      {
        friendsGroup.members.emplace(friendUid);
      }

      // Delete invoked group
      groups.erase(command.groupUid);
    });

  if (errorCode.has_value())
  {
    protocol::ChatCmdGroupDeleteAckCancel cancel{
      .errorCode = errorCode.value()};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  protocol::ChatCmdGroupDeleteAckOk response{};
  response.groupUid = command.groupUid;
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterLetterList(
  network::ClientId clientId,
  const protocol::ChatCmdLetterList& command)
{
  bool isInboxRequested = command.mailboxFolder == protocol::MailboxFolder::Inbox;
  bool isSentRequested = command.mailboxFolder == protocol::MailboxFolder::Sent;
  server::util::QuietLogDebug("[{}] ChatCmdLetterList: {} [{} {}]",
    clientId,
    isInboxRequested ? "Inbox" :
      isSentRequested ? "Sent" : "Unknown",
    command.request.lastMailUid,
    command.request.count);

  if (not isInboxRequested and not isSentRequested)
  {
    server::util::QuietLogWarn("[{}] ChatCmdLetterList: requested unrecognised mailbox {}",
      clientId,
      static_cast<uint8_t>(command.mailboxFolder));

    protocol::ChatCmdLetterListAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::MailUnknownMailboxFolder};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  const auto& clientContext = GetClientContext(clientId);

  protocol::ChatCmdLetterListAckOk response{
    .mailboxFolder = command.mailboxFolder
  };

  std::optional<protocol::ChatterErrorCode> errorCode{};

  // Get corresponding mailbox
  std::vector<data::Uid> mailbox{};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&mailbox, &errorCode, folder = command.mailboxFolder](const data::Character& character)
    {
      // Get the mailbox based on the command request
      std::vector<data::Uid> _mailbox{};
      if (folder == protocol::MailboxFolder::Inbox)
        mailbox = character.mailbox.inbox();
      else if (folder == protocol::MailboxFolder::Sent)
        mailbox = character.mailbox.sent();
      else
        errorCode.emplace(protocol::ChatterErrorCode::MailUnknownMailboxFolder);
    });

  // If mailbox type is unrecognised, respond with cancel and return
  if (errorCode.has_value())
  {
    protocol::ChatCmdLetterListAckCancel cancel{
      .errorCode = errorCode.value()};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Validate whether the mail by lastMailUid exists
  if (command.request.lastMailUid != data::InvalidUid)
  {
    // Last mail uid requested, find it
    const auto iter = std::ranges::find(
      mailbox.cbegin(),
      mailbox.cend(),
      command.request.lastMailUid);

    // Safety mechanism, just in case no mail by that UID was found
    if (iter == mailbox.cend())
    {
      server::util::QuietLogWarn("Character {} tried to request mail after mail {} but that mail does not exist.",
        clientContext.characterUid,
        command.request.lastMailUid);
      errorCode.emplace(protocol::ChatterErrorCode::MailListInvalidUid);
      return;
    }
    // Mail found, move onto filtering
  }

  // Pre-process mailbox (filter out unavailable or soft deleted mails)
  std::erase_if(
    mailbox,
    [this](const data::Uid mailUid)
    {
      // Filter unavailable records
      const auto& mailRecord = _serverInstance.GetDataDirector().GetMail(mailUid);
      if (not mailRecord)
        return true;

      // Filter soft deleted records
      bool isDeleted = false;
      mailRecord.Immutable([&isDeleted](const data::Mail& mail)
      {
        isDeleted = mail.isDeleted();
      });
      return isDeleted;
    });

  bool hasMoreMail = false;
  std::vector<data::Uid> filteredMails{};

  {
    // Start from the beginning of the pre-processed emails
    // or from the requested last mail
    auto startIter = mailbox.cbegin();
    if (command.request.lastMailUid != data::InvalidUid)
    {
      // Find the mail by uid
      const auto iter = std::ranges::find(mailbox, command.request.lastMailUid);
      // If mail found, move onto the next one
      if (iter != mailbox.cend())
        startIter = iter + 1;
    }

    // Get remaining items left in the array, from the last mail uid (or beginning)
    const auto remaining = std::distance(
      startIter,
      mailbox.cend());

    // Copy n amounts of mail as per request (max MaxMailsPerRequest)
    constexpr size_t MaxMailsPerRequest = 10;
    const auto& res = std::ranges::copy_n(
      startIter,
      std::min<size_t>(
        std::min<size_t>(command.request.count, MaxMailsPerRequest),
        remaining),
      std::back_inserter(filteredMails));

    // Indicate that there are more mail after the current ending of response mail
    hasMoreMail = res.in != mailbox.cend();
  }

  // Build response mailbox
  for (const data::Uid& mailUid : filteredMails)
  {
    _serverInstance.GetDataDirector().GetMail(mailUid).Immutable(
      [this, &response, folder = command.mailboxFolder](const data::Mail& mail)
      {
        // Get mail correspondent depending on the request
        // Mail recipient if sent mailbox or mail sender if inbox mailbox
        data::Uid correspondentUid{data::InvalidUid};
        if (folder == protocol::MailboxFolder::Sent)
          correspondentUid = mail.to();
        else if (folder == protocol::MailboxFolder::Inbox)
          correspondentUid = mail.from();

        // Get correspondent's name to render mail response
        std::string correspondentName{};
        
        if (correspondentUid == data::InvalidUid)
        {
          correspondentName = GetSystemNameFromType(mail.type());
        }
        else
        {
          _serverInstance.GetDataDirector().GetCharacter(correspondentUid).Immutable(
            [&correspondentName](const data::Character& character)
            {
              correspondentName = character.name();
            });
        }

        // Format mail createdAt based on format
        const auto& createdAt = std::format(
          DateTimeFormat,
          std::chrono::floor<std::chrono::seconds>(mail.createdAt()));
        if (folder == protocol::MailboxFolder::Sent)
        {
          // Compile sent mail and add to sent mail list
          response.sentMails.emplace_back(
            protocol::ChatCmdLetterListAckOk::SentMail{
              .mailUid = mail.uid(),
              .recipient = correspondentName,
              .content = protocol::ChatCmdLetterListAckOk::SentMail::Content{
                .date = createdAt,
                .body = mail.body()
              }});
        }
        else if (folder == protocol::MailboxFolder::Inbox)
        {
          // Compile sent mail and add to sent mail list
          auto& inboxMail = response.inboxMails.emplace_back(
            protocol::ChatCmdLetterListAckOk::InboxMail{
              .uid = mail.uid(),
              .type = mail.type(),
              .claimUid = mail.claimUid(),
              .sender = correspondentName,
              .date = createdAt,
              .struct0 = protocol::ChatCmdLetterListAckOk::InboxMail::Struct0{
                .body = mail.body()
              }
            });

          if (mail.isRead())
            inboxMail.struct0.unk0 = "\x0F";
        }
      });
  }

  // `mailbox` size here directly correlates with the loop that processes it 
  // The client is to not be made aware of any skipped mails, adjust mail count
  response.mailboxInfo = protocol::ChatCmdLetterListAckOk::MailboxInfo{
    .mailCount = static_cast<uint32_t>(filteredMails.size()),
    .hasMoreMail = hasMoreMail};

  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterLetterSend(
  network::ClientId clientId,
  const protocol::ChatCmdLetterSend& command)
{
  server::util::QuietLogDebug("[{}] ChatCmdLetterSend: {} [{}]",
    clientId,
    command.recipient,
    command.body);
  
  const data::Uid& recipientCharacterUid = 
    _serverInstance.GetDataDirector().GetDataSource().RetrieveCharacterUidByName(command.recipient);

  if (recipientCharacterUid == data::InvalidUid)
  {
    // Character tried to send mail to a character that doesn't exist, no need to log
    protocol::ChatCmdLetterSendAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::CharacterDoesNotExist
    };

    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // TODO: bad word checks and/or deny sending the letter as a result?

  const auto& clientContext = GetClientContext(clientId);

  std::string senderName{};
  data::Uid senderUid{data::InvalidUid};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&senderUid, &senderName](const data::Character& character)
    {
      senderUid = character.uid();
      senderName = character.name();
    });

  // ★R74-fix-4 (subreview #3, BLOCK 1 и 2): ОТБРАСЫВАЕМ ДО ЗАПИСИ, ПО ФАКТИЧЕСКОЙ
  // ШИРИНЕ ИМЁН НА ПРОВОДЕ.
  //
  // Проверка стоит ЗДЕСЬ, а не выше по функции, именно потому, что раньше имени
  // отправителя ещё не знали — и прежняя редакция подставляла вместо него
  // выдуманные «не больше 16 байт». Теперь считаются РЕАЛЬНЫЕ ширины:
  // `locale::FromUtf8` — тот самый конвертер, которым строки уезжают на провод
  // (`SinkStream::Write`), так что это не оценка, а тот же байтовый счёт.
  //
  // Проверяются ВСЕ ТРИ кадра, куда попадает тело: страница ящика жертвы,
  // доставка жертве и квитанция отправителю. Письмо, не влезающее хотя бы в
  // один, нельзя ни показать, ни доставить, а сохранённое — оно навсегда встаёт
  // в голову инбокса и уже оттуда не убирается.
  const auto senderWireWidth = locale::FromUtf8(senderName).size();
  const auto recipientWireWidth = locale::FromUtf8(command.recipient).size();
  const auto bodyWireWidth = locale::FromUtf8(command.body).size();

  const auto pageAllowance = protocol::MailboxPageEntryBudget > protocol::MailboxEntryFixedBytes + senderWireWidth
    ? protocol::MailboxPageEntryBudget - protocol::MailboxEntryFixedBytes - senderWireWidth
    : std::size_t{0};
  const auto arriveAllowance = protocol::ChatterFramePayloadBytes > protocol::MailArriveFixedBytes + senderWireWidth
    ? protocol::ChatterFramePayloadBytes - protocol::MailArriveFixedBytes - senderWireWidth
    : std::size_t{0};
  const auto ackAllowance = protocol::ChatterFramePayloadBytes > protocol::MailSendAckFixedBytes + recipientWireWidth
    ? protocol::ChatterFramePayloadBytes - protocol::MailSendAckFixedBytes - recipientWireWidth
    : std::size_t{0};

  const auto allowance = std::min(
    {protocol::MaxMailBodyLength, pageAllowance, arriveAllowance, ackAllowance});

  if (bodyWireWidth > allowance)
  {
    static util::LogThrottle oversizedMailThrottle{std::chrono::minutes{5}};
    uint64_t suppressed = 0;
    uint64_t total = 0;
    if (oversizedMailThrottle.Allow(suppressed, total))
    {
      util::QuietLogWarn(
        "refused an oversized mail body from client {}: {} wire bytes over the {} these"
        " frames allow for this pair (sender {} B, recipient {} B); the recipient's"
        " mailbox page could not have shown it (suppressed {} more, {} in total)",
        clientId,
        bodyWireWidth,
        allowance,
        senderWireWidth,
        recipientWireWidth,
        suppressed,
        total);
    }

    protocol::ChatCmdLetterSendAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::LetterSendBodyTooLong};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // UTC now in seconds
  const auto& utcNow = std::chrono::floor<std::chrono::seconds>(util::Clock::now());
  const auto& formattedDt = std::format(DateTimeFormat, utcNow);

  // Create and store mail
  data::Uid mailUid{data::InvalidUid};
  auto mailRecord = _serverInstance.GetDataDirector().CreateMail();
  mailRecord.Mutable([&mailUid, &command, &utcNow, &senderUid, &recipientCharacterUid](data::Mail& mail)
  {
    // Set mail parameters
    mail.from() = senderUid;
    mail.to() = recipientCharacterUid;

    mail.type() = data::Mail::MailType::CanReply;
    mail.claimUid() = data::InvalidUid;

    mail.createdAt() = utcNow;
    mail.body() = command.body;

    // Get mailUid to store in character record
    mailUid = mail.uid();
  });

  // Add the new mail to the recipient's inbox
  _serverInstance.GetDataDirector().GetCharacter(recipientCharacterUid).Mutable(
    [&mailUid](data::Character& character)
    {
      // Insert new mail to the beginning of the list
      // TODO: this operation is O(n), does dao support std::deque?
      character.mailbox.inbox().insert(
        character.mailbox.inbox().begin(),
        mailUid);

      // Set mail alarm
      character.mailbox.hasNewMail() = true;
    });

  // Add the new mail to the sender's sent mailbox
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
    [&mailUid](data::Character& character)
    {
      // Insert new mail to the beginning of the list
      // TODO: this operation is O(n), does dao support std::deque?
      character.mailbox.sent().insert(
        character.mailbox.sent().begin(),
        mailUid);
    });

  protocol::ChatCmdLetterSendAckOk response{
    .mailUid = mailUid,
    .recipient = command.recipient,
    .date = formattedDt,
    .body = command.body
  };

  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });

  // Check if recipient is online for live mail delivery
  auto client = std::ranges::find_if(
    _clients,
    [&recipientCharacterUid](const std::pair<network::ClientId, ClientContext>& client)
    {
      return client.second.characterUid == recipientCharacterUid;
    });

  if (client == _clients.cend())
    // Character is not online, all good and handled
    return;

  protocol::ChatCmdLetterArriveTrs notify{
    .mailUid = mailUid,
    .mailType = data::Mail::MailType::CanReply,
    .claimUid = data::InvalidUid,
    .sender = senderName,
    .date = formattedDt,
    .body = command.body
  };

  const auto& recipientClientId = client->first;
  _chatterServer.QueueCommand<decltype(notify)>(recipientClientId, [notify](){ return notify; });
}

void MessengerDirector::HandleChatterLetterRead(
  network::ClientId clientId,
  const protocol::ChatCmdLetterRead& command)
{
  server::util::QuietLogDebug("[{}] ChatCmdLetterRead: {} {}",
    clientId,
    command.unk0,
    command.mailUid);

  const auto& clientContext = GetClientContext(clientId);

  // Confirm if the mail even exists
  const auto& mailRecord = _serverInstance.GetDataDirector().GetMail(command.mailUid);

  std::optional<protocol::ChatterErrorCode> errorCode{};
  if (command.mailUid == data::InvalidUid)
  {
    // Character tried to request an invalid mail
    server::util::QuietLogWarn("Character {} tried to request an invalid mail",
      clientContext.characterUid);
    errorCode.emplace(protocol::ChatterErrorCode::MailInvalidUid);
  }
  else if (not mailRecord.IsAvailable())
  {
    // Mail does not exist or is not available
    server::util::QuietLogWarn("Character {} tried to request a mail {} that does not exist or is not available",
      clientContext.characterUid,
      command.mailUid);
    errorCode.emplace(protocol::ChatterErrorCode::MailDoesNotExistOrNotAvailable);
  }

  // If we haven't encountered any errors so far, check the mail ownership
  if (not errorCode.has_value())
  {
    // Confirm the character has such mail in its mailbox
    _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
      [&command, &errorCode](const data::Character& character)
      {
        // Check if character has this mail in their `inbox`
        const bool& hasMail = std::ranges::contains(
          character.mailbox.inbox(),
          command.mailUid);

        if (not hasMail)
        {
          // Character does not own this mail
          errorCode.emplace(protocol::ChatterErrorCode::MailDoesNotBelongToCharacter);
        }
      });
  }

  // If an error occurred along the way, respond with cancel and return
  if (errorCode.has_value())
  {
    protocol::ChatCmdLetterReadAckCancel cancel{.errorCode = errorCode.value()};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Mark letter as read
  mailRecord.Mutable([](data::Mail& mail)
  {
    mail.isRead() = true;
  });

  protocol::ChatCmdLetterReadAckOk response{
    .unk0 = command.unk0,
    .mailUid = command.mailUid
  };
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterLetterDelete(
  network::ClientId clientId,
  const protocol::ChatCmdLetterDelete& command)
{
  bool isRequestSent = command.folder == protocol::MailboxFolder::Sent;
  bool isRequestInbox = command.folder == protocol::MailboxFolder::Inbox;
  server::util::QuietLogDebug("[{}] ChatCmdLetterDelete: {} {}",
    clientId,
    isRequestSent ? "Sent" :
      isRequestInbox ? "Inbox" : "Unknown",
    command.mailUid);

  const auto& clientContext = GetClientContext(clientId);
  
  Record<data::Mail> mailRecord{};
  std::optional<protocol::ChatterErrorCode> errorCode{};
  if (not isRequestSent and not isRequestInbox)
  {
    // Mailbox unrecognised
    server::util::QuietLogWarn("Character {} tried to delete a mail from unrecognised mailbox {}",
      clientContext.characterUid,
      static_cast<uint8_t>(command.folder));
    errorCode.emplace(protocol::ChatterErrorCode::LetterDeleteUnknownMailboxFolder);
  }
  else if (not (mailRecord = _serverInstance.GetDataDirector().GetMail(command.mailUid)).IsAvailable())
  {
    // Mail is not available
    server::util::QuietLogWarn("Character {} tried to delete mail {} which is currently not available",
      clientContext.characterUid,
      command.mailUid);
    errorCode.emplace(protocol::ChatterErrorCode::LetterDeleteUnknownMailboxFolder);
  }

  if (not errorCode.has_value())
  {
    // No errors yet, do ownership check
    _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Mutable(
      [this, &command, &errorCode](data::Character& character)
      {
        switch (command.folder)
        {
          case protocol::MailboxFolder::Sent:
          {
            // Sent mailbox
            // Find mail
            const auto& iter = std::ranges::find(character.mailbox.sent(), command.mailUid);
            // Check if character owns this mail
            if (iter == character.mailbox.sent().cend())
            {
              errorCode.emplace(protocol::ChatterErrorCode::LetterDeleteMailDoesNotBelongToCharacter);
              return;
            }
            break;
          }
          case protocol::MailboxFolder::Inbox:
          {
            // Inbox mailbox
            // Find mail
            const auto& iter = std::ranges::find(character.mailbox.inbox(), command.mailUid);
            // Check if character owns this mail
            if (iter == character.mailbox.inbox().cend())
            {
              errorCode.emplace(protocol::ChatterErrorCode::LetterDeleteMailDoesNotBelongToCharacter);
              return;
            }
            break;
          }
          default:
          {
            throw std::runtime_error(
              std::format(
                "Unrecognised mailbox {}",
                static_cast<uint8_t>(command.folder)));
          }
        }
      });
  }

  if (errorCode.has_value())
  {
    if (errorCode.value() == protocol::ChatterErrorCode::LetterDeleteMailDoesNotBelongToCharacter)
      server::util::QuietLogDebug("Character {} tried to delete mail {} which they do not own from {} mailbox",
        clientContext.characterUid,
        command.mailUid,
        command.folder == protocol::MailboxFolder::Sent ? "Sent" :
          command.folder == protocol::MailboxFolder::Inbox ? "Inbox" :
          throw std::runtime_error(
            std::format(
              "Unrecognised mailbox {}",
              static_cast<uint8_t>(command.folder))));

    protocol::ChatCmdLetterDeleteAckCancel cancel{.errorCode = errorCode.value()};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  // Mail exists and character owns this mail, soft delete
  _serverInstance.GetDataDirector().GetMail(command.mailUid).Mutable(
    [](data::Mail& mail)
    {
      mail.isDeleted() = true;
    });
  
  protocol::ChatCmdLetterDeleteAckOk response{
    .folder = command.folder,
    .mailUid = command.mailUid
  };
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterUpdateState(
  network::ClientId clientId,
  const protocol::ChatCmdUpdateState& command)
{
  auto& clientContext = GetClientContext(clientId, false);
  if (not clientContext.isAuthenticated)
    return;

  std::string status =
    command.presence.status == protocol::Status::Hidden ? "Hidden" :
    command.presence.status == protocol::Status::Offline ? "Offline" :
    command.presence.status == protocol::Status::Online ? "Online" :
    command.presence.status == protocol::Status::Away ? "Away" :
    command.presence.status == protocol::Status::Racing ? "Racing" :
    command.presence.status == protocol::Status::WaitingRoom ? "Waiting Room" :
    std::format("Unknown status {}", static_cast<uint8_t>(command.presence.status));

  std::string scene =
    command.presence.scene == protocol::Presence::Scene::Ranch ? "Ranch" :
    command.presence.scene == protocol::Presence::Scene::Race ? "Race" :
    std::format("Unknown scene {}", static_cast<uint32_t>(command.presence.scene));
    
  server::util::QuietLogDebug("[{}] ChatCmdUpdateState: [{}] [{}] {}",
    clientId,
    status,
    scene,
    command.presence.sceneUid);

  // Sometimes ChatCmdUpdateState is received with status value > 5 containing giberish and causes crashes
  if (static_cast<uint8_t>(command.presence.status) > 5)
  {
    server::util::QuietLogWarn("Client {} sent unrecognised ChatCmdUpdateState::Status {}",
      clientId,
      static_cast<uint8_t>(command.presence.status));
    return;
  }
  else if (command.presence.status == protocol::Status::Hidden)
  {
    // Do not broadcast this at all as it breaks the row containing 
    // the invoker in other characters' friends list
    return;
  }

  // Update state for client context
  clientContext.presence = command.presence;

  BroadcastPresenceOfCharacter(
    clientContext.characterUid, command.presence, clientId, nullptr);
}

void MessengerDirector::BroadcastPresenceOfCharacter(
  const data::Uid characterUid,
  const protocol::Presence& presence,
  const network::ClientId selfClientId,
  const char* const reason)
{
  // ★УЛИКУ ПЕЧАТАЕТ САМА РАССЫЛКА, И ЭТО НЕ КОСМЕТИКА. Сначала строка стояла
  // рядом с вызовом, в сливе, — и негатив, снимавший рассылку, оставлял след
  // нетронутым: ячейка `negP` оставалась ЗЕЛЁНОЙ на сломанном коде. Проверка
  // обязана мерить то, что защищает, поэтому след живёт ВНУТРИ измеряемого
  // действия и исчезает вместе с ним.
  // ★Печатается только на редких путях (гашение), а не на каждом обновлении
  // присутствия клиента: у штатного пути `reason == nullptr`. Пометка нужна
  // ещё и затем, чтобы предикат не удовлетворялся обычным Offline от клиента.
  if (reason != nullptr)
  {
    server::util::QuietLogDebug(
      "[{}] ChatCmdUpdateState: [Offline] [Ranch] {} ({})",
      selfClientId,
      characterUid,
      reason);
  }

  // LOA-fix (R78-fix8, round78, backlog #255, находка ревю #3 WARN-1):
  // РАССЫЛКА ПРИСУТСТВИЯ, НЕ ЗАВИСЯЩАЯ ОТ ФЛАГА КОНТЕКСТА.
  //
  // ★ЗАЧЕМ ВЫНЕСЕНО. Гашение сессии (фаза 1) снимает `isAuthenticated` и
  // обнуляет `characterUid`, а `HandleChatterUpdateState` выходит первой же
  // строкой именно по этому флагу. Значит синтезированный на разрыве Offline
  // никуда не уходил: друзья видели ушедшего игрока в сети до его следующего
  // входа, гильдия — тоже, а приглашение призраку не получало даже ack.
  // Личность передаётся ПАРАМЕТРОМ: её сохраняет фаза 1 до обнуления.
  data::Uid guildUid{data::InvalidUid};
  _serverInstance.GetDataDirector().GetCharacter(characterUid).Immutable(
    [&guildUid](const data::Character& character)
    {
      guildUid = character.guildUid();
    });

  std::vector<network::ClientId> guildMembersToNotify{};

  // This mechanism goes through all the online clients and checks if the invoker is in their stored friends list.
  std::vector<network::ClientId> friendsToNotify{};

  const auto clientsSnapshot = [this]
  {
    const std::shared_lock lock(_clientsMutex);
    return _clients;
  }();
  for (const auto& [onlineClientId, onlineClientContext] : clientsSnapshot)
  {
    // Skip unauthenticated clients
    // LOA-fix (R78-fix11, round78, backlog #255, ревю #4 BLOCK): и записи БЕЗ
    // ЛИЧНОСТИ тоже. Пропуск по одному флагу оставлял бы полусвязанной записи
    // право утащить в `GetCharacter(InvalidUid).Immutable` весь обход, а с ним
    // и уведомления друзьям и гильдии. Правило и его разбор —
    // `MessengerSessionEviction.hpp`, проверка — `MessengerTestSessionEviction`.
    if (not messenger::IsPresenceBroadcastable(onlineClientContext))
      continue;

    // Self broadcast is needed only for guild notification
    bool isSelf = onlineClientId == selfClientId;
    if (isSelf and guildUid != data::InvalidUid)
    {
      guildMembersToNotify.emplace_back(onlineClientId);
      continue;
    }

    // Check if invoker is in the online client's stored friends list
    bool isFriend = false;
    _serverInstance.GetDataDirector().GetCharacter(onlineClientContext.characterUid).Immutable(
      [&isFriend, characterUid](const data::Character& character)
      {
        isFriend = std::ranges::any_of(
          character.contacts.groups() | std::views::values,
          [characterUid](const data::Character::Contacts::Group& group)
          {
            return std::ranges::contains(group.members, characterUid);
          });
      });

    if (isFriend)
    {
      friendsToNotify.emplace_back(onlineClientId);
    }

    // Get online character's guild uid
    data::Uid onlineCharacterGuildUid{data::InvalidUid};
    _serverInstance.GetDataDirector().GetCharacter(onlineClientContext.characterUid).Immutable(
      [&onlineCharacterGuildUid](const data::Character& character)
      {
        onlineCharacterGuildUid = character.guildUid();
      });

    const bool isInvokerInAGuild = guildUid != data::InvalidUid;
    const bool isOnlineCharacterInAGuild = onlineCharacterGuildUid != data::InvalidUid;
    const bool isInvokerAndOnlineCharacterInSameGuild = guildUid == onlineCharacterGuildUid;

    // If invoker is in a guild and other client is in the same guild
    if (isInvokerInAGuild and isOnlineCharacterInAGuild and isInvokerAndOnlineCharacterInSameGuild)
    {
      guildMembersToNotify.emplace_back(onlineClientId);
    } 
  }

  if (not friendsToNotify.empty())
  {
    protocol::ChatCmdUpdateStateTrs notify{
      protocol::ChatCmdUpdateState{
        presence,},
      characterUid};

    for (const auto& targetClientId : friendsToNotify)
    {
      _chatterServer.QueueCommand<decltype(notify)>(targetClientId, [notify](){ return notify; });
    }
  }

  if (not guildMembersToNotify.empty())
  {
    protocol::ChatCmdUpdateGuildMemberStateTrs notify{};
    notify.affectedCharacterUid = characterUid;
    notify.presence = presence;

    for (const auto& targetClientId : guildMembersToNotify)
    {
      _chatterServer.QueueCommand<decltype(notify)>(targetClientId, [notify](){ return notify; });
    }
  }
}

void MessengerDirector::HandleChatterChatInvite(
  network::ClientId clientId,
  const protocol::ChatCmdChatInvite& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Get private chat config and check if private chat is enabled
  const auto& privateChatConfig = _serverInstance.GetPrivateChatDirector().GetConfig();
  
  if (not privateChatConfig.enabled)
  {
    // Private chat server is disabled
    // TODO: discover (if any) corresponding cancel response exists in game client
    // to get rid of the All/Guild chat tabs
    return;
  }

  constexpr auto concatParticipants =
    [](const std::vector<data::Uid> list, std::string separator = ", ")
    {
      std::string str{};
      for (size_t i = 0; i < list.size(); ++i)
      {
        str += std::to_string(list[i]);
        if (i + 1 < list.size())
          str += separator;
      }
      return str;
    };

  server::util::QuietLogDebug("[{}] ChatCmdChatInvite: [{}]",
    clientId,
    concatParticipants(command.chatParticipantUids));

  std::vector<network::ClientId> clientIdsToNotify{};
  for (const auto& [targetClientId, targetClientContext] : _clients)
  {
    // Skip unauthenticated clients
    if (not targetClientContext.isAuthenticated)
      continue;
    
    const bool isRequestedParticipant = std::ranges::contains(
      command.chatParticipantUids,
      targetClientContext.characterUid);
    if (isRequestedParticipant)
    {
      clientIdsToNotify.emplace_back(targetClientId);
      continue;
    }
  }

  if (clientIdsToNotify.empty())
  {
    // No characters by that UID found
    // TODO: ignore request? is there a cancel?
    return;
  }
  
  // TODO: Sent notify to invoker

  // Get lobby config to get the private chat advertisement address and port
  const auto& lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();
  const std::string hostname = lobbyConfig.advertisement.privateChat.address.to_string();
  const uint16_t port = lobbyConfig.advertisement.privateChat.port;

  // TODO: use unk2 as OTP value for both clients to authenticate with the server for the same conversation

  protocol::ChatCmdChatInvitationTrs notify{
    //.unk0 = 0xABCDEF09, // TODO: discover if this is even used by the client
    //.unk1 = 131,
    .unk2 = 0xABCDEF09, // TODO: potentially otp code?
    .hostname = hostname,
    .port = port
  };

  for (const auto& targetClientId : clientIdsToNotify)
  {
    const auto& targetClientContext = GetClientContext(targetClientId);

    // Initiate chat window for the invoker
    notify.unk1 = clientContext.characterUid;
    notify.unk5 = targetClientContext.characterUid;
    _chatterServer.QueueCommand<decltype(notify)>(
      clientId,
      [notify]()
      {
        return notify;
      });

    // Initiate chat window for the target character
    notify.unk1 = targetClientContext.characterUid;
    notify.unk5 = clientContext.characterUid;
    _chatterServer.QueueCommand<decltype(notify)>(
      targetClientId,
      [notify]()
      {
        return notify;
      });
  }
}

void MessengerDirector::HandleChatterGameInvite(
  const network::ClientId clientId,
  const protocol::ChatCmdGameInvite& command)
{
  const auto& clientContext = GetClientContext(clientId);

  // Get client id of recipient by character uid
  const std::optional<Client> recipientClient = GetClientByCharacterUid(command.recipientCharacterUid);
  if (not recipientClient.has_value())
  {
    // Character by that uid is not online, cancel
    // TODO: handle this, there's no cancel, ack maybe does something?
    return;
  }

  // TODO: do we need a friendship check?

  // Send game invite notify
  protocol::ChatCmdGameInviteTrs notify{
    .unk0 = clientContext.characterUid};

  const ClientId recipientClientId = recipientClient.value().clientId;
  _chatterServer.QueueCommand<decltype(notify)>(recipientClientId, [notify](){ return notify; });

  protocol::ChatCmdGameInviteAck response{
    .unk0 = clientContext.characterUid,
    .unk1 = command.recipientCharacterUid};
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });
}

void MessengerDirector::HandleChatterChannelInfo(
  const network::ClientId clientId,
  const protocol::ChatCmdChannelInfo&)
{
  server::util::QuietLogDebug("[{}] ChatCmdChannelInfo", clientId);

  const auto& clientContext = GetClientContext(clientId);

  // Get lobby config to get the chat advertisement address and port
  const auto& lobbyConfig = _serverInstance.GetLobbyDirector().GetConfig();

  // Get all chat config and check if all chat is enabled
  const auto& allChatConfig = _serverInstance.GetAllChatDirector().GetConfig();
  if (not allChatConfig.enabled)
  {
    // Chat server is disabled
    // TODO: discover (if any) corresponding cancel response exists in game client
    // to get rid of the All/Guild chat tabs
    return;
  }

  // Hash character uid with chat director's otp constant for a unique key
  size_t identityHash = std::hash<uint32_t>()(clientContext.characterUid);
  boost::hash_combine(identityHash, AllChatOtpConstant);
  const uint32_t code = _serverInstance.GetOtpSystem().GrantLtk(
    identityHash,
    _chatterServer.GetClientAddress(clientId).to_uint());

  // Send response for all chat
  protocol::ChatCmdChannelInfoAckOk response{
    .hostname = lobbyConfig.advertisement.allChat.address.to_string(),
    .port = lobbyConfig.advertisement.allChat.port,
    .code = code};
  _chatterServer.QueueCommand<decltype(response)>(clientId, [response](){ return response; });

  bool isInGuild{false};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&isInGuild](const data::Character& character)
    {
      isInGuild = character.guildUid() != 0;
    });

  // If not in a guild, then we're done handling the command
  if (not isInGuild)
    return;

  // Not sending this internally disables the guild chat on the client,
  // even if the client says that guild chat is connected

  // Send response for guild chat (which uses private chat type)
  // TODO: this is broken, needs proper implementing
  protocol::ChatCmdChannelInfoGuildRoomAckOk guildResponse{};
  guildResponse.hostname = lobbyConfig.advertisement.privateChat.address.to_string();
  guildResponse.port = lobbyConfig.advertisement.privateChat.port;
  guildResponse.code = code; // This value seemingly has no effect
  _chatterServer.QueueCommand<decltype(guildResponse)>(clientId, [guildResponse](){ return guildResponse; });
}

void MessengerDirector::HandleChatterGuildLogin(
  const network::ClientId clientId,
  const protocol::ChatCmdGuildLogin& command)
{
  // ChatCmdGuildLogin is sent after ChatCmdLogin

  server::util::QuietLogDebug("[{}] ChatCmdGuildLogin: {} {}",
    clientId,
    command.characterUid,
    command.guildUid);

  auto& clientContext = GetClientContext(clientId);

  // Reauthenticate against the already-used otp code that the client
  // gave when authenticating with the `ChatCmdLogin` command handler.
  // The client reuses the otp code that was previously used in `ChatCmdLogin`.

  // Check if client context has an otp code and then authenticate
  if (not clientContext.otpCode.has_value() or command.code != clientContext.otpCode)
  {
    // Login failed, bad actor, log and return
    // Do not log with `command.name` (character name) to prevent some form of string manipulation in spdlog
    server::util::QuietLogWarn("Client '{}' tried to login to guild '{}' as character '{}' but failed authentication",
      clientId,
      command.guildUid,
      command.characterUid);

    protocol::ChatCmdGuildLoginAckCancel cancel{
      .errorCode = protocol::ChatterErrorCode::LoginFailed};
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  clientContext.isAuthenticated = true;

  // Check if client belongs to the guild in the command
  data::Uid characterGuildUid{data::InvalidUid};
  _serverInstance.GetDataDirector().GetCharacter(clientContext.characterUid).Immutable(
    [&characterGuildUid](const data::Character& character)
    {
      characterGuildUid = character.guildUid();
    });

  std::optional<protocol::ChatterErrorCode> errorCode{};
  if (not clientContext.isAuthenticated)
  {
    // Client is not authenticated with chatter server
    server::util::QuietLogWarn("Client {} tried to login to guild {} but is not authenticated with the chatter server.",
      clientId,
      command.guildUid);
    errorCode.emplace(protocol::ChatterErrorCode::GuildLoginClientNotAuthenticated);
  }
  else if (command.characterUid != clientContext.characterUid)
  {
    // Command `characterUid` does match the client context `characterUid 
    server::util::QuietLogWarn("Client {} tried to login, who is character {}, to guild {} on behalf of another character {}",
      clientId,
      clientContext.characterUid,
      command.guildUid,
      command.characterUid);
    errorCode.emplace(protocol::ChatterErrorCode::CommandCharacterIsNotClientCharacter);
  }
  else if (characterGuildUid != command.guildUid)
  {
    // Character does not belong to the guild in the guild login
    server::util::QuietLogWarn("Character {} tried to login to guild {} but character is not a guild member.",
      clientContext.characterUid,
      command.guildUid);
    errorCode.emplace(protocol::ChatterErrorCode::GuildLoginCharacterNotGuildMember);
  }

  if (errorCode.has_value())
  {
    // Some error has been encountered, respond with cancel and return
    protocol::ChatCmdGuildLoginAckCancel cancel{
      .errorCode = errorCode.value()
    };
    _chatterServer.QueueCommand<decltype(cancel)>(clientId, [cancel](){ return cancel; });
    return;
  }

  protocol::ChatCmdGuildLoginAckOK response{};
  _serverInstance.GetDataDirector().GetGuild(command.guildUid).Immutable(
    [this, &response](const data::Guild& guild)
    {
      for (const data::Uid& guildMemberUid : guild.members())
      {
        // Create a guild member for the response
        auto& chatGuildMember = response.guildMembers.emplace_back(
          protocol::ChatCmdGuildLoginAckOK::GuildMember{
            .characterUid = guildMemberUid});

        // Find if the guild member is connected to the messenger server
        const auto clientsSnapshot = _clients;
        for (auto& onlineClientContext : clientsSnapshot | std::views::values)
        {
          // If guild member is connected, set status to the one set by the character
          if (onlineClientContext.characterUid == guildMemberUid)
          {
            chatGuildMember.presence = onlineClientContext.presence;
            break;
          }
        }
      }
    });

  _chatterServer.QueueCommand<decltype(response)>(
    clientId,
    [response]()
    {
      return response;
    });

  // Broadcast invoker's online
  HandleChatterUpdateState(
    clientId,
    protocol::ChatCmdUpdateState{
      .presence = clientContext.presence});
}

} // namespace server
