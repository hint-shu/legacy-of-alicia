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

#include "server/authentication/AuthenticationService.hpp"
#include "libserver/util/QuietLog.hpp"

#include "server/authentication/LocalAuthenticationBackend.hpp"
#include "server/authentication/PostgresAuthenticationBackend.hpp"
#include "server/ServerInstance.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>

namespace server
{

AuthenticationService::AuthenticationService(ServerInstance& serverInstance)
  : _serverInstance(serverInstance)
{
}

void AuthenticationService::Initialize()
{
  const auto& authenticationSettings = _serverInstance.GetSettings().authentication;

  if (authenticationSettings.backend == "postgres")
  {
    try
    {
      _backend = std::make_unique<PostgresAuthenticationBackend>(
        authenticationSettings.postgres.connectionUri);
      server::util::QuietLogInfo("Authentication service is using Postgres backend");
    }
    catch (const std::exception& x)
    {
      server::util::QuietLogError("Exception initializing Postgres backend for authentication service: {}", x.what());
    }
  }
  else if (authenticationSettings.backend == "local")
  {
    try
    {
      // LOA-fix (#18b): каталог users берём из ТОГО ЖЕ источника, что и
      // FileDataSource — resourceDirectory/"data"/"users" (ServerInstance.cpp
      // :45 отдаёт DataDirector'у resourceDirectory/"data", FileDataSource
      // :58 добавляет "users"). Прежний вариант строил его из
      // GetSettings().data.file.basePath = "./data" — относительно cwd процесса
      // (/opt/alicia-server), где users НЕТ → exists() всегда false → локаут
      // всех аккаунтов. Абсолютный resourceDirectory гарантирует, что путь
      // auth'а и путь хранилища не разъедутся.
      const auto usersDirectory =
        _serverInstance.GetResourceDirectory() / "data" / "users";
      _backend = std::make_unique<LocalAuthenticationBackend>(usersDirectory);
      server::util::QuietLogInfo("Authentication service is using local backend");
    }
    catch (const std::exception& x)
    {
      server::util::QuietLogError("Exception initializing local backend for authentication service: {}", x.what());
    }
  }
  else
  {
    server::util::QuietLogError("Unknown backend for authentication service: '{}'", authenticationSettings.backend);
  }

  if (not _backend)
    throw std::runtime_error("Authentication service backend is not available");
}

void AuthenticationService::Terminate() noexcept
{
  if (_backend)
    _backend.reset();
}

// LOA-fix (R51-3, round51, backlog #179): ПОЯС С СОХРАНЕНИЕМ ПОВТОРА.
// Насос авторизации копирует запрос (строки), ходит в бэкенд (у Postgres это
// настоящий ввод-вывод) и растит вектор вердиктов — всё это бросает по природе,
// а функция `noexcept`, то есть любой сбой был мгновенной смертью процесса.
// ★Почему НЕ «снять noexcept»: насос крутится на СВОЁМ потоке, и бросок дошёл
// бы до функции потока, где пояс R49 считает отказ фатальным и снимает общий
// флаг работы — сервер ушёл бы из обслуживания целиком (R50 доказал это на
// лобби-директоре). Поэтому поведение отказа определяется ЗДЕСЬ: итерация
// насоса не удалась, запрос ОСТАЁТСЯ В ОЧЕРЕДИ, повторим на следующем тике.
// Снятие с очереди и так стоит последней операцией, поэтому семантика повтора
// получается сама — пояс её только фиксирует.
void AuthenticationService::Tick() noexcept
try
{
  if (not _backend)
    return;

  // LOA-fix (#18c): копируем запрос ПОД локом и сразу отпускаем — тяжёлый хеш
  // (100k раундов) не держит _queueMutex, иначе login-flood сериализует сервис
  // и тормозит lobby-поток. Единственный потребитель очереди — этот поток, так
  // что front стабилен между копией и pop. Re-check под локом заодно чинит
  // гонку чтения _queue.empty() вне лока.
  Authentication request;
  {
    std::scoped_lock lock(_queueMutex);
    if (_queue.empty())
      return;
    request = _queue.front();
  }

  // Хешируем/проверяем БЕЗ лока (самая долгая операция).
  const auto result = _backend->Authenticate(request.userName, request.userToken);

  if (not result)
    return; // нет вердикта (для нашего бэкенда недостижимо) — не поппим, ретрай

  {
    std::scoped_lock verdictsLock(_verdictsMutex);
    _verdicts.emplace_back(Verdict{
      .userName = request.userName,
      .isAuthenticated = result.value_or(false)});
  }

  _hasVerdicts.store(true, std::memory_order::release);

  {
    std::scoped_lock lock(_queueMutex);
    if (not _queue.empty())
      _queue.pop();
  }
}
catch (const std::exception& x)
{
  server::util::QuietLogWarn(
    "Authentication pump iteration failed, the request stays queued: {}",
    x.what());
}
catch (...)
{
  server::util::QuietLogWarn(
    "Authentication pump iteration failed with an unknown exception, "
    "the request stays queued");
}

// LOA-fix (R51-4, round51, backlog #179): ИЗМЕНЕНИЕ КОНТРАКТА, намеренное.
// Постановка просьбы копирует две строки в очередь, то есть выделяет память, а
// функция была `noexcept` — сбой означал смерть процесса на ровном месте, в
// самом начале входа игрока в игру. Теперь функция ГОВОРИТ, удалось ли принять
// просьбу, и обязанность вызывающего — не помечать вход отправленным при
// отказе (R51-4b): очередь входов обрабатывается каждый тик, поэтому попытка
// повторится сама. Игрок в худшем случае войдёт чуть позже — вместо зависшего
// клиента и упавшего сервера.
bool AuthenticationService::QueueAuthentication(
  const std::string& userName,
  const std::string& userToken) noexcept
{
  try
  {
    std::scoped_lock lock(_queueMutex);
    _queue.emplace(Authentication{
      .userName = userName,
      .userToken = userToken});
  }
  catch (const std::exception& x)
  {
    server::util::QuietLogWarn(
      "Failed to queue the authentication of user '{}': {}", userName, x.what());
    return false;
  }
  catch (...)
  {
    server::util::QuietLogWarn(
      "Failed to queue the authentication of user '{}': unknown exception",
      userName);
    return false;
  }

  return true;
}

bool AuthenticationService::HasAuthenticationVerdicts() noexcept
{
  return _hasVerdicts.load(std::memory_order::acquire);
}

std::vector<AuthenticationService::Verdict>
// LOA-fix (R51-5b, round51, backlog #179): захват замка тоже бросает, и
// оставлять его голым под `noexcept` нельзя — иначе правка выше (убрать копию)
// закрыла бы одну бросающую работу и оставила соседнюю. Поведение отказа:
// вердиктов в этот раз нет. Ничего не теряется — `_verdicts` и флаг наличия
// остаются нетронутыми, и вызывающий заберёт их следующим тиком.
AuthenticationService::PollAuthenticationVerdicts() noexcept
try
{
  std::scoped_lock lock(_verdictsMutex);

  if (_verdicts.empty())
    return {};

  // LOA-fix (R51-5, round51, backlog #179): ПЕРЕМЕЩЕНИЕ ВМЕСТО КОПИИ. Копия
  // вектора вердиктов выделяла память под `noexcept` — то есть решать было
  // нечего, надо просто убрать бросающую работу. Перемещение вектора
  // небросающее, поведение прежнее: вызывающий получает вердикты, оригинал
  // остаётся пустым.
  auto results = std::move(_verdicts);

  _verdicts.clear();
  _hasVerdicts.store(false, std::memory_order::release);

  return results;
}
catch (...)
{
  server::util::QuietLogWarn("Failed to poll authentication verdicts, they stay queued");
  return {};
}

} // namespace server