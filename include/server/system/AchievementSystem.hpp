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

#ifndef ACHIEVEMENTSYSTEM_HPP
#define ACHIEVEMENTSYSTEM_HPP

#include "libserver/data/DataDefinitions.hpp"
#include "libserver/network/command/proto/RaceMessageDefinitions.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace server
{

class ServerInstance;

//! Продвигает достижения по событиям, которые сервер ИЗМЕРИЛ САМ.
//!
//! ★ГРАНИЦА ДОВЕРИЯ. Система принимает событие только от серверного кода —
//! от места, где действие УЖЕ произошло и проверено (лошадь помыта, заезд
//! финиширован). Репорты клиента (команда 0x16b) сюда не попадают вовсе:
//! из 246 достижений каталога 202 сервер меряет сам, и именно они включаются
//! первыми. Клиентские 44 получат свой путь позже — с потолками правдоподобия
//! и БЕЗ выплат.
//!
//! ★ЧЕГО СИСТЕМА НЕ ДЕЛАЕТ В ЭТОМ РАУНДЕ: не платит морковками (в каталоге
//! выплаты обнулены решением владельца), не трогает книги, очки и звания, не
//! принимает 0x16b. Она двигает прогресс, отмечает взятые тиры и говорит об
//! этом клиенту.
class AchievementSystem
{
public:
  //! Обстоятельства ЗАЕЗДА, в котором произошло событие.
  //!
  //! ★ОТСУТСТВИЕ КОНТЕКСТА = «событие не гоночное»: фильтр режима и состава не
  //! применяется вовсе, и поведение ДЕВЯТИ существующих ранчевых вызовов
  //! `RanchDirector::SendAchievementEvent` (`RanchDirector.cpp:2892, 3449, 3735,
  //! 3736, 3750, 3751, 4559, 6742, 6932`) не меняется ни на байт. Сам
  //! `OnServerEvent` при этом зовётся из ОДНОГО места — `RanchDirector.cpp:11507`,
  //! изнутри `SendAchievementEvent`; два множества не путать.
  //! Это не удобство, а страховка от регрессии: ИЗМЕРЕНО — у всех 20 записей на
  //! уже проведённых шинах `gameModeFlag == 0` и `numPlayer == 0`, поэтому
  //! фильтр на них был бы тождественным; но «был бы» проверять нечем, а
  //! «не применяется» проверяется предикатом `P_ranch_unchanged`.
  struct EventContext
  {
    //! Чистый бит режима: 1 speed-solo, 2 speed-team, 4 magic-solo,
    //! 8 magic-team; 0 — режим вне этой четвёрки (обучение и прочее).
    uint32_t modeBit{};
    //! Сколько ЛЮДЕЙ стартовало в заезде. Боты сюда НЕ входят: их нет в
    //! трекере (`RaceInstance.hpp`, `_aiRacers`).
    uint32_t playerCount{};
  };

  explicit AchievementSystem(ServerInstance& serverInstance);

  //! Событие, измеренное сервером.
  //! @param characterUid Персонаж.
  //! @param event Номер шины событий (UserAchvEvent клиентской таблицы).
  //! @param increment На сколько продвинуть прогресс, обычно единица.
  //! @param provenConditions Имена условий (колонка Function каталога),
  //!        КОТОРЫЕ ВЫЗЫВАЮЩИЙ КОД ПРОВЕРИЛ САМ на месте события. Система
  //!        исполняет только «просто считай событие» (Function = TRUE); всё
  //!        остальное требует данных, которых у неё нет — сытость лошади,
  //!        класс жеребёнка, вес. Такие условия проверяет тот, у кого данные
  //!        под рукой, и называет их здесь. ★Неназванное условие не
  //!        срабатывает никогда: ошибочно выданный тир назад не забирается.
  //! @returns Нотификации клиенту по каждому продвинувшемуся достижению;
  //!          отправляет их вызывающая сторона — тем же способом, каким она
  //!          уже отправляет нотификации дневных заданий.
  [[nodiscard]] std::vector<protocol::AcCmdRCAchievementUpdateNotify> OnServerEvent(
    data::Uid characterUid,
    uint16_t event,
    uint32_t increment = 1,
    std::span<const std::string_view> provenConditions = {},
    //! @param context Обстоятельства заезда. `nullopt` = событие не гоночное,
    //!        фильтр `CountsInMode` не применяется.
    std::optional<EventContext> context = std::nullopt);

private:
  ServerInstance& _serverInstance;
};

} // namespace server

#endif // ACHIEVEMENTSYSTEM_HPP
