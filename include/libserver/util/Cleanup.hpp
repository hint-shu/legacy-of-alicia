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

#ifndef CLEANUP_HPP
#define CLEANUP_HPP

#include "libserver/network/NetworkDefinitions.hpp"
#include "libserver/util/QuietLog.hpp"

#include <exception>
#include <type_traits>

namespace server::util
{

//! УБОРКА СОЕДИНЕНИЯ, КОТОРАЯ ДОХОДИТ ДО КОНЦА (LOA-fix, round50, backlog #180).
//!
//! У пути разрыва нет второго шанса: `Client::End()` снимает свой одноразовый
//! гард ПЕРЕД тем, как вызвать обработчики, сокет к этому моменту закрыт, и
//! никаких новых событий asio по этому соединению уже не будет. Значит любой
//! бросок посреди уборки — это НАВСЕГДА недоубранное состояние, а не «повторим
//! на следующем событии».
//!
//! Отсюда два помощника:
//!   • `RegistryEraser` — снятие записи реестра, которое случится при ЛЮБОМ
//!     исходе; ставится в начале обработчика, срабатывает на выходе из области
//!     видимости — то есть ровно там, где раньше стояла последняя строка
//!     `erase`, и порядок операций не меняется;
//!   • `RunCleanupStep` — выполнить один шаг уборки так, чтобы его сбой не съел
//!     остальные шаги.
//!
//! ★ИЗОЛИРОВАТЬ НАДО НЕ ВСЁ. Шаг, который имеет смысл только после успеха
//! предыдущего, обязан остаться зависимым — см. возврат билета p2d в
//! `RaceNetworkHandler::HandleClientDisconnected`: вернуть билет в пул после
//! неудавшегося выхода из комнаты значит выдать его другому клиенту, пока
//! комната всё ещё ссылается на прежнего. Поэтому `RunCleanupStep` возвращает
//! признак успеха, а не void.

//! Снимает запись реестра по ключу при выходе из области видимости.
template <typename Registry>
class RegistryEraser final
{
public:
  using Key = typename Registry::key_type;

  //! ★СТРАЖ ОБЯЗАН БЫТЬ НЕБРОСАЮЩИМ ЦЕЛИКОМ, и это проверяется типом. Если
  //! копия ключа способна бросить (например строковый ключ и нехватка памяти),
  //! то сама УСТАНОВКА обязательства может провалиться — а обязательство,
  //! которое умеет не установиться, никакой гарантии не даёт. Для таких
  //! реестров правильный приём другой: снимать запись ПЕРВЫМ действием, до
  //! любой бросающей работы (см. `LobbyDirector::QueueClientLogout`, R50-9).
  static_assert(
    std::is_nothrow_copy_constructible_v<Key>,
    "ключ реестра обязан копироваться без броска: иначе страж сам умеет "
    "не установиться, и гарантия уборки превращается в пожелание");

  RegistryEraser(Registry& registry, const Key& key) noexcept
    : _registry(registry)
    , _key(key)
  {
  }

  RegistryEraser(const RegistryEraser&) = delete;
  RegistryEraser(RegistryEraser&&) = delete;
  RegistryEraser& operator=(const RegistryEraser&) = delete;
  RegistryEraser& operator=(RegistryEraser&&) = delete;

  //! Деструктор. Перехват здесь не «на всякий случай»: бросок из деструктора
  //! — это `std::terminate` (урок round49), а удаление по ключу вызывает
  //! пользовательские хеш и сравнение.
  ~RegistryEraser() noexcept
  {
    try
    {
      _registry.erase(_key);
    }
    catch (...)
    {
      QuietLogError(
        "Failed to erase the registry entry of a torn-down connection");
    }
  }

private:
  Registry& _registry;
  Key _key;
};

//! Выполняет один шаг уборки. Возвращает `true`, если шаг дошёл до конца.
template <typename Step>
bool RunCleanupStep(
  const char* stepName,
  const network::ClientId clientId,
  Step&& step) noexcept
{
  try
  {
    step();
    return true;
  }
  catch (const std::exception& x)
  {
    QuietLogWarn(
      "Cleanup step '{}' of client {} failed: {}", stepName, clientId, x.what());
  }
  catch (...)
  {
    QuietLogWarn(
      "Cleanup step '{}' of client {} failed: unknown exception",
      stepName,
      clientId);
  }

  return false;
}

} // namespace server::util

#endif // CLEANUP_HPP
