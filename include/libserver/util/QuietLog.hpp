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

#ifndef QUIET_LOG_HPP
#define QUIET_LOG_HPP

#include <spdlog/spdlog.h>

#include <utility>

namespace server::util
{

//! ЖАЛОБА, КОТОРАЯ НЕ МОЖЕТ БРОСИТЬ (LOA-fix, round49, backlog #178).
//!
//! Наш вендоренный spdlog на НЕИЗВЕСТНОМ (не-`std`) исключении из приёмника
//! лога делает `err_handler_(...); throw;` — то есть выпускает его наружу
//! (`SPDLOG_LOGGER_CATCH`, `3rd-party/spdlog/include/spdlog/logger.h:40`).
//! Значит обычный `spdlog::error(...)`, стоящий внутри `noexcept`-тела или в
//! ветке перехвата под функцией потока, способен УБИТЬ ПРОЦЕСС — ровно там,
//! где мы страховались от падения.
//!
//! Эти обёртки гасят такой бросок на месте. Цена — потерянная строка лога;
//! альтернатива — потерянный сервер. Сигнатуры зеркалят spdlog, так что
//! замена вызова текстовая: `spdlog::error(…)` -> `util::QuietLogError(…)`.
//!
//! ★Где ОБЯЗАТЕЛЬНО: тело функции, объявленной `noexcept`; любая ветка
//! `catch` под входом `std::thread` (бросок оттуда — `std::terminate` всегда);
//! деструкторы. Где НЕ нужно: обычный код, чей бросок кто-то ловит выше —
//! там громкий отказ полезнее тихого.
template <typename... Args>
void QuietLogError(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept
{
  try
  {
    spdlog::error(fmt, std::forward<Args>(args)...);
  }
  catch (...)
  {
  }
}

template <typename... Args>
void QuietLogWarn(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept
{
  try
  {
    spdlog::warn(fmt, std::forward<Args>(args)...);
  }
  catch (...)
  {
  }
}

template <typename... Args>
void QuietLogInfo(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept
{
  try
  {
    spdlog::info(fmt, std::forward<Args>(args)...);
  }
  catch (...)
  {
  }
}

template <typename... Args>
void QuietLogDebug(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept
{
  try
  {
    spdlog::debug(fmt, std::forward<Args>(args)...);
  }
  catch (...)
  {
  }
}

template <typename... Args>
void QuietLogCritical(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept
{
  try
  {
    spdlog::critical(fmt, std::forward<Args>(args)...);
  }
  catch (...)
  {
  }
}

template <typename... Args>
void QuietLogTrace(spdlog::format_string_t<Args...> fmt, Args&&... args) noexcept
{
  try
  {
    spdlog::trace(fmt, std::forward<Args>(args)...);
  }
  catch (...)
  {
  }
}

} // namespace server::util

#endif // QUIET_LOG_HPP
