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

//! LOA (R80-1, round80, backlog #235): ТАБЛИЦА ПРАВИЛА ЖАТВЫ ЧАТ-СОКЕТОВ.
//!
//! ★ЗАЧЕМ ЮНИТ-ТЕСТ, А НЕ ТОЛЬКО СТЕНД. Прод живёт на порогах 60/60/0, и
//! ждать по минуте в каждой ячейке матрицы значило бы завести проверку,
//! которую слишком дорого запускать — то есть которую не запустят. Правило —
//! чистая функция от порогов, поэтому ПРОД-ЗНАЧЕНИЯ проверяются здесь, а
//! стенд гоняет те же пути на секундах.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ собирается Release, `NDEBUG` гасит `assert`
//! целиком — тест, который не умеет провалиться, читается как зелёный.

#include "server/chat/ChatSocketReapRule.hpp"

#include <cstdio>

namespace
{

using server::chat::ChatSocketState;
using server::chat::DecideChatSocketReap;
using server::chat::ReapClock;
using server::chat::ReapThresholds;
using server::chat::ReapVerdict;

int g_failures = 0;

void Check(const bool condition, const char* const what)
{
  if (condition)
    return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++g_failures;
}

//! Условная «эпоха» прогона. Любой момент годится — часы монотонны, а правило
//! смотрит только на РАЗНОСТИ.
const ReapClock::time_point kNow = ReapClock::now();

ReapClock::time_point Ago(const int seconds)
{
  return kNow - std::chrono::seconds(seconds);
}

//! ПРОД-ПОРОГИ. Именно они и проверяются здесь, потому что стенд их проверить
//! не может: ждать по минуте в ячейке нельзя.
constexpr ReapThresholds kProd{
  .handshakeTimeout = std::chrono::seconds(60),
  .orphanGrace = std::chrono::seconds(60),
  .absoluteIdle = std::chrono::seconds(0)};

//! Те же пороги, но с ВКЛЮЧЁННЫМ P3 — то, что делает стендовая арка
//! `p3-enabled` одной переменной среды.
constexpr ReapThresholds kIdleOn{
  .handshakeTimeout = std::chrono::seconds(60),
  .orphanGrace = std::chrono::seconds(60),
  .absoluteIdle = std::chrono::seconds(10)};

ChatSocketState Unauthenticated(const int connectedSecondsAgo,
  const int lastActivitySecondsAgo)
{
  return ChatSocketState{
    .isAuthenticated = false,
    .characterUid = server::data::InvalidUid,
    .connectedAt = Ago(connectedSecondsAgo),
    .lastActivity = Ago(lastActivitySecondsAgo)};
}

ChatSocketState Bound(const server::data::Uid uid,
  const int connectedSecondsAgo,
  const int lastActivitySecondsAgo)
{
  return ChatSocketState{
    .isAuthenticated = true,
    .characterUid = uid,
    .connectedAt = Ago(connectedSecondsAgo),
    .lastActivity = Ago(lastActivitySecondsAgo)};
}

//! ★ПРОВЕРЯЮЩИЙ ОБЯЗАН УМЕТЬ ПРОВАЛИТЬСЯ. Без этой строки «все проверки
//! прошли» означало бы только, что `Check` ничего не делает.
void TestCheckerCanFail()
{
  const int before = g_failures;
  Check(false, "(намеренный провал самопроверки, ожидается)");
  Check(g_failures == before + 1, "Check обязан считать провалы");
  g_failures = before;
}

//! P1 — РУКОПОЖАТИЕ.
void TestHandshakeTimeout()
{
  Check(DecideChatSocketReap(Unauthenticated(59, 59), false, kNow, kProd)
      == ReapVerdict::Keep,
    "неаутентифицированный ДО порога живёт");
  Check(DecideChatSocketReap(Unauthenticated(60, 60), false, kNow, kProd)
      == ReapVerdict::Keep,
    "ровно на пороге ещё живёт — сравнение строгое, и это объявлено");
  Check(DecideChatSocketReap(Unauthenticated(61, 61), false, kNow, kProd)
      == ReapVerdict::Handshake,
    "неаутентифицированный ПОСЛЕ порога жнётся по P1");
}

//! I3 — P1 НЕ ОСВЕЖАЕТСЯ АКТИВНОСТЬЮ. Ловит negG.
void TestNoisyUnauthenticatedIsStillReaped()
{
  // Подключился 300 с назад, ШУМИТ каждую секунду — активность свежайшая.
  const auto noisy = Unauthenticated(300, 0);
  Check(DecideChatSocketReap(noisy, false, kNow, kProd) == ReapVerdict::Handshake,
    "шумящий, но не вошедший пир жнётся ровно так же: P1 считается от "
    "подключения, иначе сканер обходит гард одним циклом write()");
  Check(DecideChatSocketReap(noisy, true, kNow, kProd) == ReapVerdict::Handshake,
    "и «персонаж в игре» его не спасает: личности у него нет вовсе");
}

//! I3a — ФЛАГ ПОДНЯТ, ЛИЧНОСТИ НЕТ. Окно броска `Mutable` на входе
//! мессенджера: такой сокет обязан судиться по P1, а не жить вечно.
void TestAuthenticatedWithoutIdentityIsJudgedByHandshake()
{
  const ChatSocketState halfBound{
    .isAuthenticated = true,
    .characterUid = server::data::InvalidUid,
    .connectedAt = Ago(30),
    .lastActivity = Ago(0)};
  Check(DecideChatSocketReap(halfBound, false, kNow, kProd) == ReapVerdict::Keep,
    "полупривязанный ДО порога рукопожатия ещё живёт");

  const ChatSocketState halfBoundOld{
    .isAuthenticated = true,
    .characterUid = server::data::InvalidUid,
    .connectedAt = Ago(3600),
    .lastActivity = Ago(0)};
  Check(DecideChatSocketReap(halfBoundOld, false, kNow, kProd)
      == ReapVerdict::Handshake,
    "полупривязанный ПОСЛЕ порога жнётся по P1 — иначе он бессмертен");
  Check(DecideChatSocketReap(halfBoundOld, true, kNow, kProd)
      == ReapVerdict::Handshake,
    "и «в игре» его не спасает: судить по личности нечего, её нет");
}

//! I1 — ЖИВОЙ ИГРОК НЕ ЖНЁТСЯ НИКОГДА. Ловит negA.
void TestLivePlayerSurvivesAnySilence()
{
  Check(DecideChatSocketReap(Bound(7, 7200, 180), true, kNow, kProd)
      == ReapVerdict::Keep,
    "персонаж В ИГРЕ, молчит втрое дольше грейса — не жнём");
  Check(DecideChatSocketReap(Bound(7, 86400, 86400), true, kNow, kProd)
      == ReapVerdict::Keep,
    "персонаж В ИГРЕ, молчит СУТКИ — не жнём: P3 на проде выключен");
}

//! I4 — СИРОТА.
void TestOrphanNeedsBothConditions()
{
  Check(DecideChatSocketReap(Bound(9, 600, 59), false, kNow, kProd)
      == ReapVerdict::Keep,
    "персонажа нет в игре, но сокет молчит МЕНЬШЕ грейса — держим (ремень "
    "против гонки «лобби-сессия ещё не создана»)");
  Check(DecideChatSocketReap(Bound(9, 600, 61), false, kNow, kProd)
      == ReapVerdict::Orphan,
    "персонажа нет в игре И сокет молчит дольше грейса — сирота");
  Check(DecideChatSocketReap(Bound(9, 600, 6000), true, kNow, kProd)
      == ReapVerdict::Keep,
    "молчание без отсутствия в игре сиротой не делает");
}

//! P3 — АБСОЛЮТНЫЙ ПРОСТОЙ.
void TestAbsoluteIdle()
{
  Check(DecideChatSocketReap(Bound(3, 100000, 100000), true, kNow, kProd)
      == ReapVerdict::Keep,
    "absoluteIdle == 0 не жнёт НИКОГДА, сколько бы ни длился простой");
  Check(DecideChatSocketReap(Bound(3, 100, 11), true, kNow, kIdleOn)
      == ReapVerdict::Idle,
    "absoluteIdle > 0 жнёт и того, кто В ИГРЕ, — в этом весь его смысл и вся "
    "его опасность");
  Check(DecideChatSocketReap(Bound(3, 100, 9), true, kNow, kIdleOn)
      == ReapVerdict::Keep,
    "и только после порога");
}

//! ПОРЯДОК ПРЕДИКАТОВ: сирота обязана называться сиротой, а не простоем,
//! иначе предикат стенда `P_reason_is_expected` мерил бы не тот счётчик.
void TestOrphanWinsOverIdle()
{
  Check(DecideChatSocketReap(Bound(5, 600, 600), false, kNow, kIdleOn)
      == ReapVerdict::Orphan,
    "когда подходят оба, вердикт — Orphan: это несущая причина, Idle — "
    "запасная");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestHandshakeTimeout();
  TestNoisyUnauthenticatedIsStillReaped();
  TestAuthenticatedWithoutIdentityIsJudgedByHandshake();
  TestLivePlayerSurvivesAnySilence();
  TestOrphanNeedsBothConditions();
  TestAbsoluteIdle();
  TestOrphanWinsOverIdle();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
