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

#include "server/lobby/LobbyDirector.hpp"
#include "libserver/util/QuietLog.hpp"

#include "server/lobby/LobbyNetworkHandler.hpp"
#include "server/ServerInstance.hpp"

namespace server
{

LobbyDirector::LobbyDirector(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
  , _networkHandler(new LobbyNetworkHandler(_serverInstance))
{
}

LobbyDirector::~LobbyDirector()
{
  delete _networkHandler;
}

void LobbyDirector::Initialize()
{
  _shopManager.GenerateShopList(_serverInstance.GetItemRegistry());
  _networkHandler->Initialize();
}

void LobbyDirector::Terminate()
{
  _networkHandler->Terminate();
}

void LobbyDirector::Tick()
{
  // Process the client login response queue.
  if (not _loginResponseQueue.empty())
  {
    ProcesLoginResponse();
  }

  // Process the client login request queue.
  if (not _loginRequestQueue.empty())
  {
    ProcessLoginRequest();
  }

  if (_serverInstance.GetAuthenticationService().HasAuthenticationVerdicts())
  {
    const auto authentications = _serverInstance.GetAuthenticationService().PollAuthenticationVerdicts();

    for (auto& loginContext : _clientLogins |  std::views::values)
    {
      for (const auto& authenticationVerdict : authentications)
      {
        if (authenticationVerdict.userName != loginContext.userName)
          continue;

        loginContext.isAuthenticated = authenticationVerdict.isAuthenticated;
      }
    }
  }

  _scheduler.Tick();
}

bool LobbyDirector::QueueClientConnect(network::ClientId clientId)
{
  const auto [iter, inserted] = _clientLogins.try_emplace(clientId);
  if (not inserted)
    return false;
  return true;
}

size_t LobbyDirector::QueueClientLogin(
  network::ClientId clientId,
  const std::string& userName,
  const std::string& userToken)
{
  const auto clientLoginIter = _clientLogins.find(clientId);
  if (clientLoginIter == _clientLogins.cend())
    return 99;

  // LOA-fix (R72-fix-4, round72, backlog #129-S1, находка Codex 4):
  // ★ОДИН КЛИЕНТ — НЕ БОЛЬШЕ ОДНОГО ВХОДА В ОЧЕРЕДЯХ.
  //
  // `AcCmdCLLogin` — команда ПРЕД-ЛОГИННАЯ по построению: гейт авторизации её
  // пропускает, иначе войти было бы нельзя. Пока очередь не дедуплицировалась,
  // один сокет мог положить в неё СВОЙ ЖЕ ClientId сколько угодно раз: очередь
  // росла по числу присланных пакетов (память, ничем не ограниченная клиентом),
  // а директор снимал по записи за тик и на каждой писал строку об отказе —
  // клиентский лог-флуд, живущий ещё долго ПОСЛЕ того, как клиент замолчал.
  // Сосед по файлу — `QueueClientConnect` — от этого защищён `try_emplace`;
  // здесь защиты не было вовсе.
  //
  // ★СВОЙСТВО, А НЕ ФЛАГ: «ClientId встречается в двух очередях входа не более
  // одного раза». Проверяем его прямо по очередям, а не отдельным полем
  // состояния, которое пришлось бы согласовывать со ВСЕМИ путями снятия
  // (отказ, ошибка данных, инфракция, успех, разрыв) — список мест обязательно
  // отстанет. Цена — линейный поиск по очередям, но именно этот дедуп и держит
  // их короткими (длина ≤ числа клиентов, входящих прямо сейчас).
  //
  // ★УЧЁТНЫЕ ДАННЫЕ НЕ ПЕРЕЗАПИСЫВАЮТСЯ: побеждает ПЕРВАЯ просьба. Иначе
  // второй пакет менял бы имя и токен под уже отправленной просьбой об
  // аутентификации, и вердикт относился бы не к тем учётным данным.
  const bool alreadyQueued =
    std::ranges::find(_loginRequestQueue, clientId) != _loginRequestQueue.cend()
    || std::ranges::find(_loginResponseQueue, clientId) != _loginResponseQueue.cend();

  if (alreadyQueued)
    return _loginRequestQueue.size() + _loginResponseQueue.size();

  clientLoginIter->second.userName = userName;
  clientLoginIter->second.userToken = userToken;

  _loginRequestQueue.emplace_back(clientId);

  return _loginRequestQueue.size() + _loginResponseQueue.size();
}

size_t LobbyDirector::GetClientQueuePosition(
  network::ClientId clientId)
{
  size_t position{0};

  // Distance in the login response queue
  const auto responseIter = std::ranges::find(_loginResponseQueue, clientId);
  if (responseIter != _loginResponseQueue.cend())
    position += std::ranges::distance(_loginResponseQueue.begin(), responseIter);

  // Distance in the login request queue
  const auto requestIter = std::ranges::find(_loginRequestQueue, clientId);
  if (requestIter != _loginRequestQueue.cend())
    position += std::ranges::distance(_loginRequestQueue.begin(), requestIter);

  return position;
}

void LobbyDirector::QueueClientDisconnect(
  network::ClientId clientId)
{
  _loginRequestQueue.remove(clientId);
  _loginResponseQueue.remove(clientId);
  _clientLogins.erase(clientId);
}

void LobbyDirector::QueueClientLogout(
  [[maybe_unused]] network::ClientId clientId,
  const std::string& userName)
{
  server::util::QuietLogInfo("User '{}' (client {}) logged out", userName, clientId);

  // LOA-fix (R50-9, round50, backlog #180): ВТОРАЯ ЗАЩЁЛКА, КОТОРАЯ БЛОКИРУЕТ
  // АККАУНТ. Отметка «пользователь в игре» снималась ПОСЛЕДНЕЙ строкой, а перед
  // ней стоит настоящая работа с данными (получение записи пользователя и
  // запись времени последнего входа) — она бросает по природе. Уцелевшая
  // отметка означает, что `try_emplace` на следующем входе не вставит запись и
  // игрок получит `Duplicated` — навсегда, до перезапуска сервера.
  //
  // ★СНИМАЕМ ПЕРВЫМ ДЕЙСТВИЕМ, А НЕ СТРАЖЕМ НА ВЫХОДЕ. Разница не
  // стилистическая: страж хранит КОПИЮ ключа, а ключ здесь строковый, то есть
  // его копия умеет бросить — и тогда не установится само обязательство, а
  // запись уже существует (её завёл вход в игру). Обязательство, которое умеет
  // не установиться, гарантией не является. `erase` по ключу ничего не
  // выделяет, поэтому снятие отметки первой строкой не может не состояться, и
  // всё, что ниже, вольно бросать сколько угодно.
  // Ниже отметка не нужна ни одной строчке, а потерянное время последнего входа
  // — приемлемая цена: сокета уже нет, и «в игре» пользователь числиться не
  // должен ни при каком исходе.
  {
    const std::unique_lock usersLock(_userInstancesMutex);
    _userInstances.erase(userName);
  }

  const auto userRecord = _serverInstance.GetDataDirector().GetUser(userName);
  if (userRecord.IsAvailable())
  {
    userRecord.Mutable([](data::User& user)
    {
      user.lastSeenOnline() = data::Clock::now();
    });
  }
}

bool LobbyDirector::IsUserOnline(const std::string& userName)
{
  const std::shared_lock usersLock(_userInstancesMutex);
  return _userInstances.contains(userName);
}

LobbyDirector::UserInstance LobbyDirector::GetUser(
  const std::string& userName)
{
  const std::shared_lock usersLock(_userInstancesMutex);

  const auto iter = _userInstances.find(userName);
  if (iter == _userInstances.cend())
  {
    throw std::runtime_error(
      std::format(
        "User instance '{}' not available",
        userName));
  }

  return iter->second;
}

LobbyDirector::UserInstance LobbyDirector::GetUserByCharacterUid(
  data::Uid characterUid)
{
  // ★ЗДЕСЬ ДЕТЕКТОР И ЛОВИЛ ГОНКУ: этот обход шёл с сетевого потока, пока поток
  // лобби вставлял нового вошедшего.
  const std::shared_lock usersLock(_userInstancesMutex);

  for (const auto& userInstance : _userInstances | std::views::values)
  {
    if (userInstance.characterUid == characterUid)
      return userInstance;
  }

  throw std::runtime_error(
    std::format(
      "User instance for character {} not available",
      characterUid));
}

void LobbyDirector::SetUserRoom(const std::string& userName, data::Uid roomUid)
{
  const std::unique_lock usersLock(_userInstancesMutex);

  const auto userIter = _userInstances.find(userName);
  if (userIter == _userInstances.cend())
    return;

  userIter->second.roomUid = roomUid;
}

void LobbyDirector::SetCharacterForcedIntoCreator(
  const data::Uid characterUid,
  const bool forced)
{
  if (forced)
    _charactersForcedIntoCreator.emplace(characterUid);
  else
    _charactersForcedIntoCreator.erase(characterUid);
}

bool LobbyDirector::IsCharacterForcedIntoCreator(
  const data::Uid characterUid) const
{
  return _charactersForcedIntoCreator.contains(characterUid);
}

void LobbyDirector::InviteCharacterToGuild(
  const data::Uid inviteeCharacterUid,
  const data::Uid guildUid,
  const data::Uid inviterCharacterUid)
{
  _guildInstances[guildUid].invites.emplace_back(inviteeCharacterUid);

  _networkHandler->SendCharacterGuildInvitation(
    inviteeCharacterUid,
    guildUid,
    inviterCharacterUid);
}

void LobbyDirector::SetCharacterVisitPreference(
  const data::Uid characterUid,
  const data::Uid rancherUid)
{
  _networkHandler->SetCharacterVisitPreference(characterUid, rancherUid);
}

void LobbyDirector::DisconnectCharacter(
  const data::Uid characterUid)
{
  _networkHandler->DisconnectCharacter(characterUid);
}

void LobbyDirector::MuteCharacter(
  const data::Uid characterUid,
  const data::Clock::time_point expiration)
{
  _networkHandler->MuteCharacter(characterUid, expiration);
}

void LobbyDirector::NotifyCharacter(
  const data::Uid characterUid,
  const std::string& message)
{
  _networkHandler->NotifyCharacter(characterUid, message);
}

void LobbyDirector::NotifyAchievementReward(
  const data::Uid characterUid)
{
  _networkHandler->NotifyAchievementReward(characterUid);
}

void LobbyDirector::NotifyMatchmakeResult(
  const data::Uid characterUid,
  const MatchmakingSystem::Result& result)
{
  _networkHandler->NotifyMatchmakeResult(
    characterUid,
    result);
}

std::vector<LobbyDirector::UserInstance> LobbyDirector::SnapshotUsers()
{
  const std::shared_lock usersLock(_userInstancesMutex);

  std::vector<UserInstance> snapshot;
  snapshot.reserve(_userInstances.size());
  for (const auto& userInstance : _userInstances | std::views::values)
    snapshot.push_back(userInstance);

  return snapshot;
}

void LobbyDirector::SetUserCharacterUid(
  const std::string& userName,
  const data::Uid characterUid)
{
  const std::unique_lock usersLock(_userInstancesMutex);

  const auto iter = _userInstances.find(userName);
  if (iter == _userInstances.cend())
  {
    // ★МОЛЧА НЕ УХОДИМ. Раньше здесь стоял `GetUser`, который БРОСАЛ, если
    // пользователя нет; проглотить это молча значило бы потерять привязку
    // персонажа и оставить игрока в игре без неё.
    throw std::runtime_error(
      std::format("User instance '{}' not available", userName));
  }

  iter->second.characterUid = characterUid;
}

size_t LobbyDirector::GetUserCount()
{
  const std::shared_lock usersLock(_userInstancesMutex);
  return _userInstances.size();
}

std::unordered_map<data::Uid, LobbyDirector::GuildInstance>& LobbyDirector::GetGuilds()
{
  return _guildInstances;
}

Config::Lobby& LobbyDirector::GetConfig()
{
  return _serverInstance.GetSettings().lobby;
}

Scheduler& LobbyDirector::GetScheduler()
{
  return _scheduler;
}

ShopManager& LobbyDirector::GetShopManager()
{
  return _shopManager;
}

LobbyNetworkHandler& LobbyDirector::GetNetworkHandler()
{
  return *_networkHandler;
}

void LobbyDirector::ProcessLoginRequest()
{
  const network::ClientId clientId = _loginRequestQueue.front();
  auto& loginContext = _clientLogins[clientId];

  // Request authentication of the user if not yet requested.
  if (not loginContext.userAuthenticationRequested)
  {
    // LOA-fix (R51-4b, round51, backlog #179): отметка «просьба отправлена»
    // ставится ТОЛЬКО если её действительно приняли. Иначе вход считался бы
    // отправленным, ответа не пришло бы никогда, и клиент висел бы на экране
    // входа до собственного таймаута. Очередь входов обрабатывается каждый
    // тик — значит отказ просто приводит к повторной попытке.
    if (not _serverInstance.GetAuthenticationService().QueueAuthentication(
      loginContext.userName,
      loginContext.userToken))
    {
      return;
    }

    loginContext.userAuthenticationRequested = true;
    return;
  }

  // If the authentication result is not available skip to the next user.
  if (not loginContext.isAuthenticated.has_value())
    return;

  if (not loginContext.isAuthenticated.value())
  {
    // LOA-fix (R72-fix-4, round72, находка Codex 4): ★СТРОКА ОБ ОТКАЗЕ
    // ДРОССЕЛИРОВАНА, НО НЕ ПОТЕРЯНА.
    //
    // Дедуп очереди выше убирает рост памяти и повторную работу, но НЕ
    // объём лога: вердикт аутентификации остаётся в контексте клиента, и
    // каждый следующий пакет `AcCmdCLLogin` снова доходит сюда — то есть один
    // сокет по-прежнему способен держать поток строк с частотой тика
    // директора (50/с). Дроссель гасит объём, а накопительный счётчик не даёт
    // проглоченным отказам исчезнуть: следующая выпущенная строка называет
    // полное число неудачных входов за жизнь процесса, и подбор пароля
    // остаётся видимым в логе числом, а не строкой на попытку.
    uint64_t suppressed = 0;
    uint64_t total = 0;
    if (_failedAuthenticationThrottle.Allow(suppressed, total))
    {
      server::util::QuietLogInfo(
        "User '{}' failed authentication; {} more failures suppressed since the "
        "previous line; {} failed logins total since start",
        loginContext.userName,
        suppressed,
        total);
    }

    _networkHandler->RejectLogin(
      clientId,
      protocol::AcCmdCLLoginCancel::Reason::InvalidUser);
    _loginRequestQueue.pop_front();
    return;
  }

  // Request the load of the user data if not requested yet.
  if (not loginContext.userLoadRequested)
  {
    _serverInstance.GetDataDirector().RequestLoadUserData(loginContext.userName);

    loginContext.userLoadRequested = true;
    return;
  }

  // If the data are still being loaded do not proceed with login.
  if (_serverInstance.GetDataDirector().AreDataBeingLoaded(loginContext.userName))
  {
    return;
  }

  _loginRequestQueue.pop_front();

  if (not _serverInstance.GetDataDirector().AreUserDataLoaded(loginContext.userName))
  {
    server::util::QuietLogError("User data for '{}' are not available", loginContext.userName);
    _networkHandler->RejectLogin(
      clientId,
      protocol::AcCmdCLLoginCancel::Reason::Generic);
    server::util::QuietLogWarn("Rejected login of user '{}' because of a server error", loginContext.userName);
    return;
  }

  const auto userRecord = _serverInstance.GetDataDirector().GetUser(
    loginContext.userName);
  assert(userRecord.IsAvailable());

  // Check for any infractions preventing the user from joining.
  const auto infractionVerdict = _serverInstance.GetInfractionSystem().CheckOutstandingPunishments(
    loginContext.userName);

  if (infractionVerdict.preventServerJoining)
  {
    _networkHandler->RejectLogin(
      clientId,
      protocol::AcCmdCLLoginCancel::Reason::DisconnectYourself);
    server::util::QuietLogInfo("Rejected login of user '{}' because of an infraction", loginContext.userName);
  }
  else
  {
    server::util::QuietLogInfo("Accepted login of user '{}'", loginContext.userName);
    // Queue the user response.
    _loginResponseQueue.emplace_back(clientId);
  }
}

void LobbyDirector::ProcesLoginResponse()
{
  const network::ClientId clientId = _loginResponseQueue.front();
  auto& loginContext = _clientLogins[clientId];

  // If the user character load was already requested wait for the load to complete.
  if (loginContext.userCharacterLoadRequested)
  {
    if (_serverInstance.GetDataDirector().AreDataBeingLoaded(loginContext.userName))
    {
      return;
    }
  }

  const auto userRecord = _serverInstance.GetDataDirector().GetUser(loginContext.userName);
  assert(userRecord.IsAvailable());

  auto characterUid = data::InvalidUid;
  userRecord.Immutable(
    [&characterUid](const data::User& user)
    {
      characterUid = user.characterUid();
    });

  const bool hasCharacter = characterUid != data::InvalidUid;

  // If the user has a character request the load.
  if (hasCharacter)
  {
    // If the user character is not loaded do not proceed.
    if (not loginContext.userCharacterLoadRequested)
    {
      _serverInstance.GetDataDirector().RequestLoadCharacterData(
        loginContext.userName,
        characterUid);

      loginContext.userCharacterLoadRequested = true;
      return;
    }
  }

  _loginResponseQueue.pop_front();

  // If the character was not loaded reject the login.
  if (characterUid != data::InvalidUid && not _serverInstance.GetDataDirector().AreCharacterDataLoaded(
    loginContext.userName))
  {
    server::util::QuietLogError("User character data for '{}' not available", clientId);
    _networkHandler->RejectLogin(
      clientId,
      protocol::AcCmdCLLoginCancel::Reason::Generic);
    server::util::QuietLogWarn("Rejected login of user '{}' because of a server error", loginContext.userName);
    return;
  }

  // ★ЗДЕСЬ БЫЛ ПИШУЩИЙ КОНЕЦ ГОНКИ, И ЗАМОК СТОИТ РОВНО ВОКРУГ РАБОТЫ С
  // КАРТОЙ, А НЕ ШИРЕ. Первая моя редакция брала исключительный замок и
  // держала его до конца функции — то есть и через `AcceptLogin`, который
  // возвращается в этот же директор. Получился самозахват нерекурсивного
  // замка: поток лобби вставал намертво, вход переставал работать целиком
  // (стенд показал 0 удачных заходов из 24). Ирония в том, что ровно про этот
  // риск я предупреждал для ПЕРЕБОРА и не применил к ЗАПИСИ.
  bool inserted = false;
  {
    const std::unique_lock usersLock(_userInstancesMutex);

    const auto [iter, insertedNow] = _userInstances.try_emplace(
      loginContext.userName);
    inserted = insertedNow;

    // ★ЗАПИСЬ ЗАПОЛНЯЕТСЯ ВНУТРИ ЗАМКА, А НЕ ПОСЛЕ. Раньше поля проставлялись
    // уже после `AcceptLogin`, и между вставкой и заполнением существовало
    // окно, в котором чужие потоки видели ПОЛУПУСТУЮ запись: имя пустое,
    // персонаж `InvalidUid`. Заполнение под тем же замком это окно закрывает —
    // наружу запись становится видна сразу целой.
    if (inserted)
    {
      auto& userInstance = iter->second;
      userInstance.userName = loginContext.userName;
      userInstance.characterUid = characterUid;
    }
  }

  if (not inserted)
  {
    _networkHandler->RejectLogin(
      clientId,
      protocol::AcCmdCLLoginCancel::Reason::Duplicated);
    server::util::QuietLogWarn(
      "Rejected login of user '{}' because the user is already logged in from different location",
      loginContext.userName);
    return;
  }

  const bool requiresCharacterCreator = _charactersForcedIntoCreator.erase(characterUid) > 0
    || characterUid == data::InvalidUid;

  _networkHandler->AcceptLogin(clientId, requiresCharacterCreator);

  server::util::QuietLogInfo(
    "User '{}' (client {}) logged in from {}",
    loginContext.userName,
    clientId,
    _networkHandler->GetClientAddress(clientId));

  userRecord.Mutable([](data::User& user)
  {
    // Set the last seen online time to 1 to indicate that the user is currently online.
    // NOTE: delete this once we have a proper api
    user.lastSeenOnline() = data::Clock::time_point(std::chrono::seconds(1));
  });

  _clientLogins.erase(clientId);
}

} // namespace server