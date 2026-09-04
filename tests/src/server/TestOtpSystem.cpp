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
//! ★СРОК ЖИЗНИ ОДНОРАЗОВОГО КОДА ПРОВЕРЯЕТСЯ ЗДЕСЬ, И ЭТО СТОИТ 31 СЕКУНДУ
//! РЕАЛЬНОГО ВРЕМЕНИ. Первая редакция этого файла писала, что истечение
//! проверяет стенд, — и это было НЕПРАВДОЙ (находка Codex 3): стендовая арка
//! `ttl` проверяет ровно обратное утверждение, что НОВЫЙ ключ мессенджера
//! переживает 36 секунд, а по ранч- и заезд-коду не ходила вовсе. Утверждение
//! «одноразовый код по-прежнему протухает» не фальсифицировала ни одна
//! проверка раунда.
//! `OtpSystem` берёт время у `steady_clock` напрямую, подать его снаружи
//! нельзя, поэтому единственный честный способ — подождать. Цена признаётся
//! осознанно: раунд переносит на LTK ровно один директор, а ранч и заезд
//! остаются на этом коде, и потеря его срока была бы регрессом
//! безопасности в двух чужих путях.
//!
//! ★ПОЧЕМУ НЕ `assert`: образ собирается Release, `NDEBUG` гасит `assert`
//! целиком — тест, который не умеет провалиться, читается как зелёный.

#include "server/system/OtpSystem.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

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

//! ★СТОРОЖ ЧУЖИХ ПУТЕЙ, ЧАСТЬ ВТОРАЯ: одноразовый код обязан ПРОТУХАТЬ.
//! Ждём реальные 31 с (TTL = 30 с, `OtpSystem.cpp:16`). Единственный тест
//! раунда, который что-то ждёт; см. разбор в шапке файла.
void TestCodeExpiresAfterTtl()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantCode(KeyA);

  // ★LTK ВЫДАЁТСЯ ДО ОЖИДАНИЯ, И ЭТО НЕ КОСМЕТИКА. Первая редакция выдавала его
  // ПОСЛЕ сна и проверяла свежий ключ — то есть утверждение «LTK переживает
  // ожидание» было вакуумным и осталось бы зелёным, даже если бы у LTK завёлся
  // срок жизни (находка Codex 3 итерации 2, ЛОЖНО-ЗЕЛЁНОЕ В МОЁМ СОБСТВЕННОМ
  // НОВОМ ТЕСТЕ). Ключ обязан пролежать ТО ЖЕ ВРЕМЯ, что и одноразовый код.
  const uint32_t ltk = otp.GrantLtk(KeyB, AddressA);

  // Контроль самой проверки: до истечения код обязан подходить, иначе
  // «протух» ничего не доказывает — мы бы не отличили TTL от опечатки в ключе.
  server::OtpSystem control;
  const uint32_t controlCode = control.GrantCode(KeyA);
  Check(control.AuthorizeCode(KeyA, controlCode),
    "свежий одноразовый код обязан подходить — иначе тест ниже вакуумен");

  std::this_thread::sleep_for(std::chrono::seconds(31));

  Check(not otp.AuthorizeCode(KeyA, code),
    "★одноразовый код, пролежавший 31 с, обязан быть отбит: ранч и заезд "
    "по-прежнему зависят от его срока жизни");

  // И LTK, пролежавший СТОЛЬКО ЖЕ, обязан ВЫЖИТЬ — иначе «протух» означало бы,
  // что сломалось время, а не что сроки у двух видов ключей разные.
  Check(otp.AuthorizeLtk(KeyB, ltk, AddressA),
    "★LTK, пролежавший 31 с, обязан подходить: у него нет срока жизни");
}

//! LOA (R78-fix1, находка Codex 1): снятие ключа вместе с сеансом.
void TestLtkRevokeIsCodeMatched()
{
  server::OtpSystem otp;
  const uint32_t code = otp.GrantLtk(KeyA, AddressA);

  Check(not otp.RevokeLtk(KeyA, code + 1u),
    "снятие ЧУЖИМ значением обязано ничего не сделать");
  Check(otp.AuthorizeLtk(KeyA, code, AddressA),
    "после неудачного снятия ключ обязан остаться рабочим");

  Check(otp.RevokeLtk(KeyA, code), "снятие СВОИМ значением обязано сработать");
  Check(not otp.AuthorizeLtk(KeyA, code, AddressA),
    "★снятый ключ обязан перестать подходить — иначе выход из игры не "
    "ограничивает срок жизни ключа");
  Check(not otp.RevokeLtk(KeyA, code), "повторное снятие обязано вернуть false");
}

//! ★ГОНКА «ОПОЗДАВШИЙ ВЫХОД СТИРАЕТ СВЕЖИЙ КЛЮЧ». Уборка разорванного
//! соединения приходит ПОЗЖЕ события; игрок, успевший перезайти, уже держит
//! новый ключ. Снятие обязано пройти мимо него.
void TestLateRevokeDoesNotKillTheNewKey()
{
  server::OtpSystem otp;
  const uint32_t oldCode = otp.GrantLtk(KeyA, AddressA);
  const uint32_t newCode = otp.GrantLtk(KeyA, AddressA);  // перезаход

  if (oldCode == newCode)
    return;  // один шанс на 2^32; проверять нечего

  Check(not otp.RevokeLtk(KeyA, oldCode),
    "опоздавшее снятие СТАРЫМ значением не должно ничего снять");
  Check(otp.AuthorizeLtk(KeyA, newCode, AddressA),
    "★свежий ключ обязан пережить опоздавший выход прошлой сессии");
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
  TestLtkRevokeIsCodeMatched();
  TestLateRevokeDoesNotKillTheNewKey();
  TestCodeExpiresAfterTtl();

  if (g_failures != 0)
  {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("all checks passed\n");
  return 0;
}
