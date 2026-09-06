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

#ifndef ALICIA_SERVER_SPURTOLERANCE_HPP
#define ALICIA_SERVER_SPURTOLERANCE_HPP

#include "server/tracker/RaceTracker.hpp"

#include <cstdint>

namespace server::race
{

//! LOA-fix (R81, backlog #270): РЕШЕНИЕ О НЕХВАТКЕ ШКАЛЫ — ЧИСТОЙ ФУНКЦИЕЙ.
//!
//! ★ЗАЧЕМ ОТДЕЛЬНЫЙ ЗАГОЛОВОК, А НЕ ТРИ КОНЪЮНКТА В ТЕЛЕ ОБРАБОТЧИКА. Ровно
//! тот же довод, что у `RelayAuthz.hpp` (R71): правило, живущее внутри
//! сетевого обработчика, проверяется только поднятым сервером, то есть на
//! стенде и только теми значениями, до которых арка успевает дойти. Здесь оба
//! потолка и обе их границы (`< N` против `<= N`) проверяются юнит-тестом за
//! микросекунды — а на стенде остаётся то, чего юнит не видит: живой кадр,
//! ответы, цепочка и отсутствие строки `[error]`.
//!
//! ★ЧТО ЭТА ФУНКЦИЯ НЕ РЕШАЕТ. Она НЕ отвечает на вопрос «хватило ли очков» —
//! это сравнение со шкалой, оно остаётся в обработчике. Она отвечает ровно на
//! один вопрос: «нехватка уже случилась; правдоподобен ли этот рывок?».
//!
//! ★ЛОЖЬ ОТБИТА ТРЕМЯ НЕЗАВИСИМЫМИ УСЛОВИЯМИ, И НИ ОДНО КЛИЕНТ ОБЪЯВИТЬ НЕ
//! МОЖЕТ: заезд реально идёт (серверный `IsRaceUnderway`), рывков за заезд
//! меньше потолка, прощений меньше бюджета.
//!
//! @param underway              «заезд реально идёт» (`IsRaceUnderway`).
//! @param paidSpurCount         сколько платных рывков УЖЕ засчитано за заезд.
//! @param toleratedShortfalls   сколько нехваток УЖЕ прощено за заезд.
//! @returns true — рывок засчитывается (шкала уйдёт в ноль); false — отказ.
[[nodiscard]] constexpr bool SpurShortfallDecision(
  const bool underway,
  const uint32_t paidSpurCount,
  const uint32_t toleratedShortfalls) noexcept
{
  return underway
    && paidSpurCount < tracker::MaxPlausibleSpursPerRace
    && toleratedShortfalls < tracker::SpurShortfallGrace;
}

} // namespace server::race

#endif // ALICIA_SERVER_SPURTOLERANCE_HPP
