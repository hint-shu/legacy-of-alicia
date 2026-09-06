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

#ifndef ALICIA_SERVER_MASTERYACCRUAL_HPP
#define ALICIA_SERVER_MASTERYACCRUAL_HPP

#include "server/tracker/RaceTracker.hpp"

#include <cstdint>

namespace server::race
{

//! LOA-fix (R81, backlog #273): АРИФМЕТИКА МАСТЕРСТВА — ЧИСТЫМИ ФУНКЦИЯМИ.
//!
//! ★ЗАЧЕМ ОТДЕЛЬНЫЙ ЗАГОЛОВОК, ХОТЯ ЕДИНСТВЕННЫЙ ВЫЗЫВАЮЩИЙ ОДИН.
//! Правила ниже уезжают в ВЕЧНЫЕ монотонные поля лошади, снять которые можно
//! только правкой файлов при ОСТАНОВЛЕННОМ сервере. Живя внутри лямбды
//! `RaceInstance::Stop()`, они проверялись бы только стендом — то есть только
//! теми значениями, до которых успевает дойти арка, и НИКОГДА на границах
//! (`== потолок` против `потолок + 1`) и на насыщении в 0xFFFFFFFF. Юнит-тест,
//! переписывающий ту же арифметику у себя, не проверяет НИЧЕГО: он сверяет
//! копию с копией. Прецедент выноса ради проверяемости — `RelayAuthz.hpp`
//! (R71) и `SpurTolerance.hpp` (этот же раунд).
//!
//! ★ЧТО ЗДЕСЬ НЕТ И БЫТЬ НЕ ДОЛЖНО: гейта «заезд доказан». Он общий с двумя
//! величинами R75 и живёт в `Stop()` ровно одной проверкой на всех.

//! Правдоподобно ли пер-заездное приращение «рывки + выданные предметы».
//! ★ОТБРАСЫВАЕМ, А НЕ КЛАМПИМ (см. вывод у самой константы): кламп позволил бы
//! модклиенту ПРИКОЛОТИТЬ вечное поле ровно к потолку.
[[nodiscard]] constexpr bool IsPlausibleSpurMagicDelta(
  const uint32_t raceSpurMagicCount) noexcept
{
  return raceSpurMagicCount <= tracker::MaxPlausibleSpurMagicPerRace;
}

//! Правдоподобно ли пер-заездное приращение удачных прыжков.
[[nodiscard]] constexpr bool IsPlausibleJumpDelta(
  const uint32_t raceJumpCount) noexcept
{
  return raceJumpCount <= tracker::MaxPlausibleJumpsPerRace;
}

//! Бюджет скольжения за заезд, мс: ДОЛЯ времени заезда.
//! ★64 БИТА НАМЕРЕННО: `courseTime` — `uint32_t` в миллисекундах, и умножение
//! на 25 в 32 битах переполнилось бы на 172-секундном заезде, то есть ровно на
//! честной длине круга — бюджет стал бы крошечным, и потолок начал бы
//! отбрасывать ЧЕСТНОЕ скольжение.
[[nodiscard]] constexpr uint64_t SlideBudgetMillis(
  const uint32_t courseTimeMs) noexcept
{
  return static_cast<uint64_t>(courseTimeMs)
    * tracker::MaxPlausibleSlideShareOfCoursePercent / 100u;
}

//! Правдоподобна ли пер-заездная сумма скольжения.
//! ★ЭТО НЕ ТО ЖЕ, ЧТО ПОТОЛОК ОДНОГО ОТРЕЗКА (`MaxPlausibleSlideDuration`,
//! применяется в `HandleRelay`): десять ЗАКРЫТЫХ отрезков по 30 с проходят
//! пер-отрезковый потолок и положили бы ~300 единиц в вечное поле.
[[nodiscard]] constexpr bool IsPlausibleSlideDelta(
  const uint32_t slidingMillis,
  const uint32_t courseTimeMs) noexcept
{
  return static_cast<uint64_t>(slidingMillis) <= SlideBudgetMillis(courseTimeMs);
}

//! Миллисекунды скольжения -> ЕДИНИЦЫ ХРАНИМОГО ПОЛЯ, то есть СЕКУНДЫ,
//! усечением вниз.
//! ★ПОЧЕМУ СЕКУНДЫ. Знаменатель панели 900 (progression.yaml): 900 секунд =
//! 15 минут суммарного заноса за жизнь коня — правдоподобная «полная полоса».
//! T76 даёт независимое подтверждение порядка: 4 заезда -> 15 единиц, то есть
//! ~4 единицы на заезд; в секундах это правдоподобно, а в миллисекундах
//! означало бы 4 мс скольжения за весь заезд, чего не бывает.
//! Единицу обязан подтвердить живой замер — это часть Definition of Done.
[[nodiscard]] constexpr uint32_t SlidingMillisToSeconds(
  const uint32_t slidingMillis) noexcept
{
  return slidingMillis / 1000u;
}

//! Накопительное НАСЫЩАЮЩЕЕ сложение для вечного счётчика.
//! ★СЧЁТЧИК, А НЕ РЕКОРД: `max` стёр бы историю коня каждым более скромным
//! заездом. ★НАСЫЩЕНИЕ, А НЕ ОБОРОТ: обёрнутый счётчик показал бы игроку
//! пустую полосу после полной.
[[nodiscard]] constexpr uint32_t AccumulateSaturating(
  const uint32_t current,
  const uint32_t delta) noexcept
{
  const uint64_t sum = static_cast<uint64_t>(current) + static_cast<uint64_t>(delta);
  return sum > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(sum);
}

} // namespace server::race

#endif // ALICIA_SERVER_MASTERYACCRUAL_HPP
