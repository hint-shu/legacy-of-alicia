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

//! LOA (R78, round78, backlog #255): вытеснение устаревшей мессенджер-сессии.
//!
//! ★ЗАЧЕМ ЮНИТ-ТЕСТ, А НЕ СТЕНД. Наблюдаемое следствие вытеснения на стенде —
//! КУДА уйдёт личка. Без вытеснения `GetClientByCharacterUid` выбирает сокет
//! порядком `unordered_map`, то есть монеткой: предикат «пришло на живой»
//! проходил бы на сломанном коде примерно в половине прогонов. Флаки-гейт
//! хуже отсутствующего, поэтому САМО ПРАВИЛО проверяется здесь и
//! детерминированно, а стенд ловит уже видимое следствие — закрытый сокет.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ собирается Release, `NDEBUG` гасит `assert`
//! целиком — тест, который не умеет провалиться, читается как зелёный.

#include "server/messenger/MessengerSessionEviction.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <unordered_map>

namespace
{

int g_failures = 0;

void Check(const bool condition, const char* const what)
{
  if (condition)
    return;
  std::fprintf(stderr, "FAIL: %s\n", what);
  ++g_failures;
}

//! Ровно те поля контекста мессенджера, которых касается вытеснение.
//! Двойник, а не сам `MessengerDirector::ClientContext`: тот приватный и тянет
//! за собой ServerInstance, а правило от этого не зависит.
struct Binding
{
  bool isAuthenticated{false};
  server::data::Uid characterUid{server::data::InvalidUid};
  std::optional<uint32_t> otpCode{};
};

using Map = std::unordered_map<server::network::ClientId, Binding>;

Binding Bound(const server::data::Uid uid)
{
  return Binding{
    .isAuthenticated = true,
    .characterUid = uid,
    .otpCode = 4242u};
}

bool Contains(
  const std::vector<server::network::ClientId>& ids,
  const server::network::ClientId id)
{
  return std::ranges::find(ids, id) != ids.end();
}

//! Проверяющий обязан уметь провалиться.
void TestCheckerCanFail()
{
  const int before = g_failures;
  Check(false, "(намеренный провал: проверяющий жив)");
  Check(g_failures == before + 1, "Check не считает провалы — тест слеп");
  g_failures = before;
}

//! Главный случай раунда: старый сокет держится (#235), клиент вошёл вторым.
void TestStaleSessionIsUnbound()
{
  Map clients{
    {0, Bound(4)},  // мёртвый сокет от входа в лобби
    {1, Bound(4)},  // соединение, которое только что вошло
  };

  const auto unbound = server::messenger::UnbindOtherSessionsOfCharacter(
    clients, 1, 4);

  Check(unbound.size() == 1, "отвязать обязано ровно одну чужую сессию");
  Check(Contains(unbound, 0), "отвязана обязана быть именно старая (клиент 0)");
  Check(not clients[0].isAuthenticated,
    "у отвязанной сессии обязан быть снят флаг аутентификации");
  Check(clients[0].characterUid == server::data::InvalidUid,
    "у отвязанной сессии обязана быть стёрта личность — иначе адресная "
    "доставка по-прежнему может выбрать её");
  Check(not clients[0].otpCode.has_value(),
    "у отвязанной сессии обязан быть стёрт запомненный код: по нему "
    "переаутентифицируется вход в гильдейский чат");
}

//! Вошедшее соединение вытеснять нельзя — иначе вход отменяет сам себя.
void TestKeptSessionSurvives()
{
  Map clients{
    {0, Bound(4)},
    {1, Bound(4)},
  };

  const auto unbound = server::messenger::UnbindOtherSessionsOfCharacter(
    clients, 1, 4);

  Check(not Contains(unbound, 1), "удерживаемое соединение не должно попасть в список");
  Check(clients[1].isAuthenticated, "удерживаемое соединение обязано остаться вошедшим");
  Check(clients[1].characterUid == 4, "удерживаемое соединение обязано сохранить личность");
  Check(clients[1].otpCode.has_value(), "удерживаемое соединение обязано сохранить код");
}

//! Соседи по серверу не задеты — вытеснение адресное, а не сплошное.
void TestOtherCharactersUntouched()
{
  Map clients{
    {0, Bound(4)},
    {1, Bound(4)},
    {2, Bound(7)},
    {3, Bound(9)},
  };

  const auto unbound = server::messenger::UnbindOtherSessionsOfCharacter(
    clients, 1, 4);

  Check(unbound.size() == 1, "отвязана обязана быть ровно одна сессия");
  Check(clients[2].isAuthenticated && clients[2].characterUid == 7,
    "чужой персонаж 7 не должен пострадать");
  Check(clients[3].isAuthenticated && clients[3].characterUid == 9,
    "чужой персонаж 9 не должен пострадать");
}

//! Вытесняются ВСЕ накопившиеся, а не только первая найденная.
void TestAllStaleSessionsAreUnbound()
{
  Map clients{
    {0, Bound(4)},
    {1, Bound(4)},
    {2, Bound(4)},
    {3, Bound(4)},
  };

  const auto unbound = server::messenger::UnbindOtherSessionsOfCharacter(
    clients, 3, 4);

  Check(unbound.size() == 3, "три устаревших сессии обязаны быть отвязаны все");
  Check(clients[3].isAuthenticated, "вошедшее соединение обязано выжить");
  for (const server::network::ClientId id : {0u, 1u, 2u})
  {
    Check(clients[id].characterUid == server::data::InvalidUid,
      "каждая устаревшая сессия обязана лишиться личности");
  }
}

//! ★НЕСУЩИЙ ГАРД. Свежее соединение до входа лежит в карте с `InvalidUid`.
//! Вызов с невыясненной личностью не имеет права совпасть с такими записями:
//! иначе вытеснение закрыло бы соединения ни в чём не повинных клиентов,
//! которые как раз подключаются.
void TestInvalidUidEvictsNothing()
{
  Map clients{
    {0, Binding{}},  // подключился, ещё не вошёл
    {1, Binding{}},  // подключился, ещё не вошёл
    {2, Bound(4)},
  };

  const auto unbound = server::messenger::UnbindOtherSessionsOfCharacter(
    clients, 2, server::data::InvalidUid);

  Check(unbound.empty(),
    "вызов с InvalidUid обязан не тронуть НИ ОДНОЙ записи — подключающиеся "
    "клиенты лежат в карте ровно с этим значением");
}

//! Единственная сессия персонажа: вытеснять некого, список пуст.
void TestSingleSessionIsNoOp()
{
  Map clients{{7, Bound(4)}};

  const auto unbound = server::messenger::UnbindOtherSessionsOfCharacter(
    clients, 7, 4);

  Check(unbound.empty(), "при единственной сессии список отвязанных обязан быть пуст");
  Check(clients[7].isAuthenticated, "единственная сессия обязана остаться вошедшей");
}

//! Неаутентифицированный сосед с той же личностью тоже отвязывается: личность
//! на записи — это и есть то, по чему выбирает адресная доставка.
void TestUnauthenticatedButBoundIsUnbound()
{
  Map clients{
    {0, Binding{.isAuthenticated = false, .characterUid = 4, .otpCode = {}}},
    {1, Bound(4)},
  };

  const auto unbound = server::messenger::UnbindOtherSessionsOfCharacter(
    clients, 1, 4);

  Check(unbound.size() == 1, "привязка без флага входа всё равно обязана быть снята");
  Check(clients[0].characterUid == server::data::InvalidUid,
    "личность обязана быть стёрта и у неаутентифицированной записи");
}

//! LOA (R78-fix4, находка ревю W1): ВЫХОД ИЗ ИГРЫ ГАСИТ ВСЕ СЕССИИ ПЕРСОНАЖА.
//!
//! ★ЗАЧЕМ ОТДЕЛЬНЫЙ ВХОД. При входе одну сессию щадят — вошедшую. При выходе
//! щадить некого, и «щадить некого» обязано быть выражено ТИПОМ: `ClientId` —
//! это `size_t`, у него нет запрещённого значения, поэтому часовой вроде
//! `(ClientId)-1` однажды совпал бы с настоящим соединением.
void TestLogoutUnbindsEverySession()
{
  Map clients{
    {0, Bound(4)},
    {1, Bound(4)},
    {2, Bound(4)},
    {3, Bound(9)},
  };

  const auto unbound = server::messenger::UnbindAllSessionsOfCharacter(clients, 4);

  Check(unbound.size() == 3,
    "★выход обязан отвязать ВСЕ три сессии персонажа, не щадя ни одной — "
    "иначе держатель подсмотренного ключа продолжит читать чужую почту");
  for (const server::network::ClientId id : {0u, 1u, 2u})
  {
    Check(not clients[id].isAuthenticated,
      "у каждой сессии вышедшего обязан быть снят флаг обслуживания");
    Check(clients[id].characterUid == server::data::InvalidUid,
      "у каждой сессии вышедшего обязана быть стёрта личность");
    Check(not clients[id].otpCode.has_value(),
      "у каждой сессии вышедшего обязан быть стёрт запомненный код");
  }
  Check(clients[3].isAuthenticated && clients[3].characterUid == 9,
    "чужой персонаж не должен пострадать от чужого выхода");
}

//! Тот же несущий гард, что и у входа: без личности не гасим никого.
void TestLogoutWithInvalidUidUnbindsNothing()
{
  Map clients{
    {0, Binding{}},
    {1, Binding{}},
    {2, Bound(4)},
  };

  const auto unbound = server::messenger::UnbindAllSessionsOfCharacter(
    clients, server::data::InvalidUid);

  Check(unbound.empty(),
    "выход без выясненной личности обязан не тронуть НИ ОДНОЙ записи — "
    "подключающиеся клиенты лежат в карте ровно с этим значением");
  Check(clients[2].isAuthenticated, "чужая живая сессия обязана уцелеть");
}

//! ★ВХОД И ВЫХОД РАЗЛИЧАЮТСЯ ИМЕННО ЭТИМ, и разница проверена, а не заявлена.
void TestLoginSparesOneAndLogoutSparesNone()
{
  Map loginCase{{0, Bound(4)}, {1, Bound(4)}};
  Map logoutCase{{0, Bound(4)}, {1, Bound(4)}};

  const auto onLogin = server::messenger::UnbindOtherSessionsOfCharacter(
    loginCase, 1, 4);
  const auto onLogout = server::messenger::UnbindAllSessionsOfCharacter(
    logoutCase, 4);

  Check(onLogin.size() == 1, "вход щадит вошедшую сессию");
  Check(onLogout.size() == 2, "выход не щадит ни одной");
  Check(loginCase[1].isAuthenticated, "после входа вошедшая сессия жива");
  Check(not logoutCase[1].isAuthenticated, "после выхода не жива ни одна");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestStaleSessionIsUnbound();
  TestKeptSessionSurvives();
  TestOtherCharactersUntouched();
  TestAllStaleSessionsAreUnbound();
  TestInvalidUidEvictsNothing();
  TestSingleSessionIsNoOp();
  TestUnauthenticatedButBoundIsUnbound();
  TestLogoutUnbindsEverySession();
  TestLogoutWithInvalidUidUnbindsNothing();
  TestLoginSparesOneAndLogoutSparesNone();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
