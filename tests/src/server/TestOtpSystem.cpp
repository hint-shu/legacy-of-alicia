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

//! LOA (R78, round78, backlog #255): два вида ключей выдачи.
//!
//! ★ЗАЧЕМ ЭТОТ ТЕСТ СУЩЕСТВУЕТ. Раунд не меняет `OtpSystem` ни на строку — он
//! переводит мессенджер с одного вида ключа на другой. Значит вся правка стоит
//! ровно на двух утверждениях: «LTK переживает повторную сверку и привязан к
//! конечной точке» и «одноразовый код ОСТАЛСЯ одноразовым». Второе — не
//! формальность: тем же `AuthorizeCode` авторизуются вход на ранчо
//! (`RanchDirector`) и вход в комнату заезда (`RaceNetworkHandler`), и
//! ослабление там было бы регрессом безопасности в двух чужих путях.
//! До раунда эти свойства не проверял никто.
//!
//! ★ЧЕГО ЗДЕСЬ НЕТ. Срок жизни одноразового кода — 30 секунд; ждать их в тесте
//! значит не проверять. Время в `OtpSystem` не подаётся снаружи, поэтому
//! истечение проверяется стендом, а здесь — только одноразовость.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ собирается Release, `NDEBUG` гасит `assert`
//! целиком — тест, который не умеет провалиться, читается как зелёный.

#include "server/system/OtpSystem.hpp"

#include <cstdio>

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

constexpr size_t KeyA = 0x1111'2222'3333'4444ull;
constexpr size_t KeyB = 0x5555'6666'7777'8888ull;
constexpr uint32_t AddressA = 0x7F00'0001u;  // 127.0.0.1
constexpr uint32_t AddressB = 0x0A00'0001u;  // 10.0.0.1

void TestCheckerCanFail()
{
  const int before = g_failures;
  Check(false, "(намеренный провал: проверяющий жив)");
  Check(g_failures == before + 1, "Check не считает провалы — тест слеп");
  g_failures = before;
}

//! ГЛАВНОЕ СВОЙСТВО РАУНДА: ключ переживает сверку и работает СНОВА.
//! Именно этого не умел одноразовый код, и именно поэтому личка умирала после
//! каждого заезда.
void TestLtkSurvivesRepeatedAuthorization()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantLtk(KeyA, AddressA);

  Check(otp.AuthorizeLtk(KeyA, code, AddressA), "первая сверка LTK обязана пройти");
  Check(otp.AuthorizeLtk(KeyA, code, AddressA),
    "★вторая сверка ТЕМ ЖЕ кодом обязана пройти — переподключение мессенджера "
    "присылает тот же код, что и первый вход");
  Check(otp.AuthorizeLtk(KeyA, code, AddressA), "и третья тоже: ключ не тратится");
}

//! Привязка к конечной точке — то, чем LTK платит за долгую жизнь.
void TestLtkIsBoundToEndpoint()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantLtk(KeyA, AddressA);

  Check(not otp.AuthorizeLtk(KeyA, code, AddressB),
    "★верный код с ЧУЖОГО адреса обязан быть отбит — иначе подсмотревший код "
    "получает бессрочный вход");
  Check(otp.AuthorizeLtk(KeyA, code, AddressA),
    "отказ по адресу не должен портить ключ для законного адреса");
}

//! Ни чужой ключ, ни выдуманный код, ни ключ без выдачи не проходят.
void TestLtkRejectsForeignAndUnknown()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantLtk(KeyA, AddressA);

  Check(not otp.AuthorizeLtk(KeyB, code, AddressA),
    "★код персонажа A под ключом персонажа B обязан быть отбит");
  Check(not otp.AuthorizeLtk(KeyA, code + 1u, AddressA),
    "подобранный мимо код обязан быть отбит");
  Check(not otp.AuthorizeLtk(KeyB, 0u, AddressA),
    "ключ, которому ничего не выдавали, обязан быть отбит");
}

//! Повторная выдача ротирует ключ: старый перестаёт работать.
void TestLtkRegrantRotates()
{
  server::OtpSystem otp;
  const uint32_t first = otp.GrantLtk(KeyA, AddressA);
  const uint32_t second = otp.GrantLtk(KeyA, AddressA);

  if (first == second)
  {
    // Один шанс на 2^32; отдельной проверкой отличаем совпадение от «выдача
    // ничего не делает».
    Check(otp.AuthorizeLtk(KeyA, second, AddressA), "новый ключ обязан работать");
    return;
  }

  Check(otp.AuthorizeLtk(KeyA, second, AddressA), "новый ключ обязан работать");
  Check(not otp.AuthorizeLtk(KeyA, first, AddressA),
    "старый ключ после перевыдачи обязан перестать работать");
}

//! ★СТОРОЖ ЧУЖИХ ПУТЕЙ: одноразовый код обязан ОСТАТЬСЯ одноразовым.
//! Им авторизуются вход на ранчо и вход в комнату заезда.
void TestCodeIsStillSingleUse()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantCode(KeyA);

  Check(otp.AuthorizeCode(KeyA, code), "первая сверка одноразового кода обязана пройти");
  Check(not otp.AuthorizeCode(KeyA, code),
    "★вторая сверка ТЕМ ЖЕ кодом обязана быть отбита — код тратится при успехе");
}

//! Неудачная сверка кода его не тратит: иначе чужой пакет отменял бы вход.
void TestCodeIsNotConsumedOnFailure()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantCode(KeyA);

  Check(not otp.AuthorizeCode(KeyA, code + 1u), "неверный код обязан быть отбит");
  Check(otp.AuthorizeCode(KeyA, code),
    "верный код после чужой неудачной попытки обязан ещё работать");
}

//! Две выдачи — два независимых хранилища. Раунд опирается на это: мессенджер
//! уходит в LTK, а ранч и заезд остаются на одноразовом коде с ТЕМ ЖЕ ключом,
//! если он вдруг совпадёт.
void TestCodeAndLtkAreIndependent()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantCode(KeyA);
  const uint32_t ltk = otp.GrantLtk(KeyA, AddressA);

  Check(otp.AuthorizeLtk(KeyA, ltk, AddressA), "LTK обязан работать при живом коде");
  Check(otp.AuthorizeCode(KeyA, code), "код обязан работать при живом LTK");
  Check(otp.AuthorizeLtk(KeyA, ltk, AddressA),
    "трата одноразового кода не должна задевать LTK на том же ключе");
}

} // namespace

int main()
{
  TestCheckerCanFail();
  TestLtkSurvivesRepeatedAuthorization();
  TestLtkIsBoundToEndpoint();
  TestLtkRejectsForeignAndUnknown();
  TestLtkRegrantRotates();
  TestCodeIsStillSingleUse();
  TestCodeIsNotConsumedOnFailure();
  TestCodeAndLtkAreIndependent();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
