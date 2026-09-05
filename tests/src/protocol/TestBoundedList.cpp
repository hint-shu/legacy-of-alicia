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

//! LOA (R74, round74, backlog #170): ЮНИТ-ГЕЙТ ОГРАНИЧЕННЫХ СПИСКОВ.
//!
//! ★ПРОВЕРКИ НЕ ЧЕРЕЗ `assert`. Боевой образ собирается `RelWithDebInfo`, то
//! есть с `-DNDEBUG`, и тест на `assert` был бы вечнозелёным ровно в той
//! конфигурации, которая едет в прод. Каждая проверка — явный `if`, печать и
//! ненулевой код возврата.

#include <libserver/data/DataDefinitions.hpp>
#include <libserver/data/helper/ProtocolHelper.hpp>
#include <libserver/network/command/proto/CommonStructureDefinitions.hpp>
#include <libserver/network/command/proto/LobbyMessageDefinitions.hpp>
#include <libserver/network/command/proto/RaceMessageDefinitions.hpp>
#include <libserver/network/command/proto/RanchMessageDefinitions.hpp>
#include <libserver/network/chatter/proto/ChatterMessageDefinitions.hpp>
#include <libserver/util/BoundedList.hpp>
#include <libserver/util/Locale.hpp>
#include <libserver/util/Stream.hpp>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <array>
#include <limits>
#include <optional>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using server::SinkStream;
using server::protocol::Item;

//! Буфер одной команды. Число не выдумано тестом: столько же стоит в
//! `CommandServer` (`MaxCommandDataSize`), и превышение его в проде означает
//! выброшенного клиента.
constexpr std::size_t MaxCommandDataSize = 8192;

int failures = 0;

void Check(const bool ok, const char* what, const long long got = 0, const long long want = 0)
{
  if (ok)
    return;
  std::printf("FAIL: %s (got %lld, want %lld)\n", what, got, want);
  ++failures;
}

//! Элемент фиксированного размера — 4 байта на провод.
struct Quad
{
  uint32_t value{};

  static void Write(const Quad& v, SinkStream& stream) { stream.Write(v.value); }
};

//! Элемент 16 байт — столько же весит протокольный `Item`.
struct Sixteen
{
  std::array<uint32_t, 4> value{};

  static void Write(const Sixteen& v, SinkStream& stream)
  {
    for (const auto part : v.value)
      stream.Write(part);
  }
};

template <typename T>
std::vector<T> Fill(const std::size_t count)
{
  std::vector<T> out;
  out.resize(count);
  return out;
}

//! Читает счётчик по смещению из уже записанного буфера.
template <typename CountType>
CountType ReadCount(const std::span<const std::byte> buffer, const std::size_t offset)
{
  CountType value{};
  std::memcpy(&value, buffer.data() + offset, sizeof(CountType));
  return value;
}

// ---------------------------------------------------------------- тесты 1..6

//! 1: список в пределах потолка уезжает целиком.
//! 2: список сверх потолка объявляется потолком и телом в потолок.
//! 3: ПОРЯДОК КЛАМПА. 264 элемента при потолке 16 обязаны дать 16, а не 8:
//!    `static_cast<uint8_t>(264)` == 8, и именно эта форма дефекта убивала кадр.
//! 4: 256 при потолке 255 обязаны дать 255, а не 0.
void TestCountBounds()
{
  {
    std::array<std::byte, 4096> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(10);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 16, .name = "t1"});
    Check(written == 10, "t1: written", static_cast<long long>(written), 10);
    Check(ReadCount<uint8_t>(storage, 0) == 10, "t1: declared", ReadCount<uint8_t>(storage, 0), 10);
    Check(sink.GetCursor() == 1 + 10 * 4, "t1: cursor",
      static_cast<long long>(sink.GetCursor()), 41);
  }

  {
    std::array<std::byte, 4096> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(40);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 16, .name = "t2"});
    Check(written == 16, "t2: written", static_cast<long long>(written), 16);
    Check(ReadCount<uint8_t>(storage, 0) == 16, "t2: declared", ReadCount<uint8_t>(storage, 0), 16);
    Check(sink.GetCursor() == 1 + 16 * 4, "t2: cursor",
      static_cast<long long>(sink.GetCursor()), 65);
  }

  {
    std::array<std::byte, 8192> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(264);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 16, .name = "t3"});
    Check(written == 16, "t3: written (264 at bound 16)", static_cast<long long>(written), 16);
    Check(ReadCount<uint8_t>(storage, 0) == 16,
      "t3: declared must be 16, NOT 8 (264 & 0xFF)", ReadCount<uint8_t>(storage, 0), 16);
  }

  {
    std::array<std::byte, 8192> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(256);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 255, .name = "t4"});
    Check(written == 255, "t4: written (256 at bound 255)", static_cast<long long>(written), 255);
    Check(ReadCount<uint8_t>(storage, 0) == 255,
      "t4: declared must be 255, NOT 0 (256 & 0xFF)", ReadCount<uint8_t>(storage, 0), 255);
  }
}

//! 5: ВМЕСТИМОСТЬ. 100 элементов по 16 Б в буфер 64 Б: броска нет, счётчик
//!    равен фактически записанному, курсор стоит на границе целого элемента.
void TestCapacity()
{
  std::array<std::byte, 64> storage{};
  SinkStream sink{std::span{storage}};
  const auto data = Fill<Sixteen>(100);

  std::size_t written = 0;
  bool threw = false;
  try
  {
    written = server::util::WriteBoundedList<uint8_t>(sink, data, {.name = "t5"});
  }
  catch (const std::exception&)
  {
    threw = true;
  }

  Check(not threw, "t5: no throw escapes the helper", threw ? 1 : 0, 0);
  Check(written == 3, "t5: written fits (64 - 1) / 16", static_cast<long long>(written), 3);
  Check(ReadCount<uint8_t>(storage, 0) == written,
    "t5: declared == written", ReadCount<uint8_t>(storage, 0), static_cast<long long>(written));
  Check(sink.GetCursor() == 1 + written * 16, "t5: cursor on an element boundary",
    static_cast<long long>(sink.GetCursor()), static_cast<long long>(1 + written * 16));
}

//! 6: МАСШТАБ СЧЁТА. Счётчик кругов объявляет секторы (×3).
void TestCountScale()
{
  {
    std::array<std::byte, 4096> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(10);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 10, .countScale = 3, .name = "t6a"});
    Check(written == 10, "t6a: written", static_cast<long long>(written), 10);
    Check(ReadCount<uint8_t>(storage, 0) == 30, "t6a: declared 10*3",
      ReadCount<uint8_t>(storage, 0), 30);
  }
  {
    std::array<std::byte, 4096> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(40);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 10, .countScale = 3, .name = "t6b"});
    Check(written == 10, "t6b: written clamped", static_cast<long long>(written), 10);
    Check(ReadCount<uint8_t>(storage, 0) == 30, "t6b: declared 30",
      ReadCount<uint8_t>(storage, 0), 30);
  }
  {
    // ★R74-fix-2 (subreview #1, WARN 5): ПРОВЕРКА СУДИТ ИНВАРИАНТ, А НЕ
    // ПОВЕДЕНИЕ РЕАЛИЗАЦИИ. Прежняя формулировка утверждала `declared == 0`
    // при `written == 5` как ОЖИДАЕМЫЙ результат — то есть единственная
    // проверка, способная поймать «счётчик лжёт о теле» внутри примитива, была
    // написана по тому, что код делает, а не по тому, что он обязан делать.
    // Инвариант: объявлено == записано × НАСЫЩЕННЫЙ масштаб.
    std::array<std::byte, 4096> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(5);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 8, .countScale = 0, .name = "t6c"});
    const long long declared = ReadCount<uint8_t>(storage, 0);
    Check(written == 5, "t6c: scale 0 does not divide by zero",
      static_cast<long long>(written), 5);
    Check(declared == static_cast<long long>(written * 1),
      "t6c: declared == written * max(scale, 1) — счётчик НЕ ЛЖЁТ о теле",
      declared, static_cast<long long>(written));
    Check(sink.GetCursor() == 1 + written * 4,
      "t6c: тело действительно записано целиком",
      static_cast<long long>(sink.GetCursor()),
      static_cast<long long>(1 + written * 4));
  }
  {
    // Потолок по счётчику при масштабе 3: 255/3 == 85, а не 255.
    std::array<std::byte, 8192> storage{};
    SinkStream sink{std::span{storage}};
    const auto data = Fill<Quad>(200);
    const auto written = server::util::WriteBoundedList<uint8_t>(
      sink, data, {.countScale = 3, .name = "t6d"});
    Check(written == 85, "t6d: scaled count limit is max/scale",
      static_cast<long long>(written), 85);
    Check(ReadCount<uint8_t>(storage, 0) == 255, "t6d: declared 85*3",
      ReadCount<uint8_t>(storage, 0), 255);
  }
}

// ------------------------------------------------------------------- тест 7

//! 7: БАЙТОВАЯ ТОЖДЕСТВЕННОСТЬ НА НОРМАЛЬНЫХ ДАННЫХ. Эталон пишется руками той
//!    же примитивной формой, что стояла до свипа.
void TestByteIdentity()
{
  server::protocol::LobbyCommandShowInventoryOK command{};
  for (uint32_t idx = 0; idx < 50; ++idx)
    command.items.push_back(Item{.uid = 1000 + idx, .tid = 20000 + idx, .count = idx});
  command.horses.resize(3);

  std::array<std::byte, MaxCommandDataSize> got{};
  SinkStream gotSink{std::span{got}};
  server::protocol::LobbyCommandShowInventoryOK::Write(command, gotSink);

  std::array<std::byte, MaxCommandDataSize> want{};
  SinkStream wantSink{std::span{want}};
  wantSink.Write(static_cast<uint8_t>(command.items.size()));
  for (const auto& item : command.items)
    wantSink.Write(item);
  wantSink.Write(static_cast<uint8_t>(command.horses.size()));
  for (const auto& horse : command.horses)
    wantSink.Write(horse);

  Check(gotSink.GetCursor() == wantSink.GetCursor(), "t7: same length",
    static_cast<long long>(gotSink.GetCursor()), static_cast<long long>(wantSink.GetCursor()));
  Check(std::memcmp(got.data(), want.data(), wantSink.GetCursor()) == 0,
    "t7: identical bytes for in-bound data");

  // Тот же вопрос для карты системного контента: эталон строится по
  // ОТСОРТИРОВАННЫМ ключам, потому что порядок обхода `unordered_map` не
  // определён и до раунда уезжал на провод как попало.
  server::protocol::LobbyCommandLoginOK::SystemContent content{};
  for (uint32_t idx = 0; idx < 40; ++idx)
    content.values.emplace(100 - idx, idx * 7);

  std::array<std::byte, MaxCommandDataSize> sysGot{};
  SinkStream sysGotSink{std::span{sysGot}};
  server::protocol::LobbyCommandLoginOK::SystemContent::Write(content, sysGotSink);

  std::array<std::byte, MaxCommandDataSize> sysWant{};
  SinkStream sysWantSink{std::span{sysWant}};
  sysWantSink.Write(static_cast<uint8_t>(content.values.size()));
  for (uint32_t key = 100 - 39; key <= 100; ++key)
    sysWantSink.Write(key).Write(content.values.at(key));

  Check(std::memcmp(sysGot.data(), sysWant.data(), sysWantSink.GetCursor()) == 0,
    "t7: system content is written in ascending key order");
}

// ------------------------------------------------------------------- тест 8

//! 8: БЮДЖЕТЫ ДВУХ ТЯЖЁЛЫХ КАДРОВ ДЛЯ ПРОФИЛЯ АРКИ A1 (264 предмета экипировки).
//!    Оба обязаны укладываться в буфер команды — иначе арка A1 стенда скрытно
//!    превратилась бы в арку вместимости и перестала бы судить счётчик.
void TestFrameBudgets()
{
  server::protocol::LobbyCommandLoginOK login{};
  login.name = "loatest-2";
  login.notice = std::string(200, 'n');
  login.introduction = std::string(200, 'i');
  for (uint32_t idx = 0; idx < 264; ++idx)
    login.equipmentItems.push_back(Item{.uid = idx, .tid = idx, .count = 1});

  std::array<std::byte, MaxCommandDataSize> storage{};
  SinkStream sink{std::span{storage}};
  bool threw = false;
  try
  {
    server::protocol::LobbyCommandLoginOK::Write(login, sink);
  }
  catch (const std::exception& e)
  {
    threw = true;
    std::printf("t8: LoginOK threw: %s\n", e.what());
  }
  Check(not threw, "t8: LoginOK for the A1 profile packs without throwing");
  Check(sink.GetCursor() > 0, "t8: LoginOK produced bytes",
    static_cast<long long>(sink.GetCursor()), 1);
  // Экипировка идёт сразу за головой кадра; смещение считается по ней, а не
  // угадывается: 4 (lobbyTime.low) + 4 (high) + 4 (member0) + 4 (uid)
  // + строки name/notice + 1 (gender) + строка introduction.
  const auto equipmentCounterOffset = 16
    + login.name.size() + 1
    + login.notice.size() + 1
    + 1
    + login.introduction.size() + 1;
  Check(ReadCount<uint8_t>(storage, equipmentCounterOffset) == 16,
    "t8: LoginOK declares 16 equipment items for the A1 profile",
    ReadCount<uint8_t>(storage, equipmentCounterOffset), 16);
  std::printf("t8: LobbyCommandLoginOK (264 equipment, bound 16) = %zu bytes, headroom %zu\n",
    sink.GetCursor(), MaxCommandDataSize - sink.GetCursor());
  Check(sink.GetCursor() < MaxCommandDataSize, "t8: LoginOK fits the command buffer",
    static_cast<long long>(sink.GetCursor()), static_cast<long long>(MaxCommandDataSize));

  server::protocol::AcCmdCREnterRanchOK ranch{};
  ranch.rancherUid = 2;
  ranch.rancherName = "loatest-2";
  ranch.ranchName = "loatest-2's ranch";
  auto& character = ranch.characters.emplace_back();
  character.uid = 2;
  character.name = "loatest-2";
  character.introduction = std::string(200, 'i');
  for (uint32_t idx = 0; idx < 264; ++idx)
    character.characterEquipment.push_back(Item{.uid = idx, .tid = idx, .count = 1});

  std::array<std::byte, MaxCommandDataSize> ranchStorage{};
  SinkStream ranchSink{std::span{ranchStorage}};
  threw = false;
  try
  {
    server::protocol::AcCmdCREnterRanchOK::Write(ranch, ranchSink);
  }
  catch (const std::exception& e)
  {
    threw = true;
    std::printf("t8: EnterRanchOK threw: %s\n", e.what());
  }
  Check(not threw, "t8: EnterRanchOK for the A1 profile packs without throwing");
  std::printf("t8: AcCmdCREnterRanchOK (one character, 264 equipment, bound 16) = %zu bytes,"
              " headroom %zu\n",
    ranchSink.GetCursor(), MaxCommandDataSize - ranchSink.GetCursor());
  Check(ranchSink.GetCursor() < MaxCommandDataSize, "t8: EnterRanchOK fits the command buffer",
    static_cast<long long>(ranchSink.GetCursor()), static_cast<long long>(MaxCommandDataSize));
}

// ------------------------------------------------------------------- тест 9

//! 9: БЮДЖЕТ БЛОКА МАКРОСОВ. Число `MaxMacroBlockWireBytes` не «на вкус»: с
//!    полным блоком на бюджете реалистичный кадр входа обязан оставаться внутри
//!    буфера команды, и запас печатается. Съест кто-нибудь запас — краснеет тест.
void TestMacroBudget()
{
  server::protocol::MacroOptions small{};
  for (auto& macro : small.macros)
    macro = std::string(200, 'a');
  const auto smallSize = server::protocol::MeasureMacroBlockWireSize(small);
  Check(smallSize <= server::protocol::MaxMacroBlockWireBytes,
    "t9: 8x200 bytes of macros are within the budget",
    static_cast<long long>(smallSize),
    static_cast<long long>(server::protocol::MaxMacroBlockWireBytes));

  server::protocol::MacroOptions huge{};
  for (auto& macro : huge.macros)
    macro = std::string(1800, 'A');
  const auto hugeSize = server::protocol::MeasureMacroBlockWireSize(huge);
  Check(hugeSize > server::protocol::MaxMacroBlockWireBytes,
    "t9: 8x1800 bytes of macros are over the budget",
    static_cast<long long>(hugeSize),
    static_cast<long long>(server::protocol::MaxMacroBlockWireBytes));

  // Блок ровно на бюджете: 8 строк по (budget/8 - 1) байт плюс NUL каждая.
  server::protocol::MacroOptions atBudget{};
  const auto perMacro = server::protocol::MaxMacroBlockWireBytes / 8 - 1;
  for (auto& macro : atBudget.macros)
    macro = std::string(perMacro, 'm');
  const auto atBudgetSize = server::protocol::MeasureMacroBlockWireSize(atBudget);
  Check(atBudgetSize == server::protocol::MaxMacroBlockWireBytes,
    "t9: the crafted block measures exactly the budget",
    static_cast<long long>(atBudgetSize),
    static_cast<long long>(server::protocol::MaxMacroBlockWireBytes));

  // Реалистичный худший кадр входа: прод-профиль `Nmax` (57 предметов инвентаря
  // -> в LoginOK едет экипировка на своём бонде, 13 квестов, 5 лошадей) плюс
  // полный блок макросов и клавиатура с геймпадом.
  server::protocol::LobbyCommandLoginOK login{};
  login.name = "Nmax";
  login.notice = std::string(255, 'n');
  login.introduction = std::string(255, 'i');
  login.val6 = std::string(255, 'v');
  for (uint32_t idx = 0; idx < 16; ++idx)
    login.equipmentItems.push_back(Item{.uid = idx, .tid = idx, .count = 1});
  for (uint32_t idx = 0; idx < 16; ++idx)
    login.expiredItems.push_back(Item{.uid = idx, .tid = idx, .count = 1});
  for (uint32_t idx = 0; idx < 17; ++idx)
  {
    auto& mission = login.missions.emplace_back();
    mission.id = static_cast<uint16_t>(idx);
    mission.progress.resize(4);
  }
  login.settings.typeBitset.set(server::protocol::Settings::Keyboard);
  login.settings.keyboardOptions.bindings.resize(64);
  login.settings.typeBitset.set(server::protocol::Settings::Gamepad);
  login.settings.gamepadOptions.bindings.resize(64);
  login.settings.typeBitset.set(server::protocol::Settings::Macros);
  login.settings.macroOptions = atBudget;

  std::array<std::byte, MaxCommandDataSize> storage{};
  SinkStream sink{std::span{storage}};
  bool threw = false;
  try
  {
    server::protocol::LobbyCommandLoginOK::Write(login, sink);
  }
  catch (const std::exception& e)
  {
    threw = true;
    std::printf("t9: LoginOK threw: %s\n", e.what());
  }
  Check(not threw, "t9: the worst realistic LoginOK packs without throwing");
  std::printf("t9: worst realistic LobbyCommandLoginOK = %zu bytes, headroom %zu"
              " (macro block %zu of %zu)\n",
    sink.GetCursor(),
    MaxCommandDataSize - sink.GetCursor(),
    atBudgetSize,
    server::protocol::MaxMacroBlockWireBytes);
  Check(sink.GetCursor() < MaxCommandDataSize,
    "t9: the worst realistic LoginOK still fits the command buffer",
    static_cast<long long>(sink.GetCursor()), static_cast<long long>(MaxCommandDataSize));
}

// ------------------------------------------------------------------ тест 10

//! 10: `rethrowOnCapacity` продолжает выпускать бросок наружу — иначе выкаченный
//!     измерительный гард входа в комнату перестал бы работать, оставшись на
//!     месте (вход разрешался бы с тихо обрезанным ростером).
void TestRethrowOnCapacity()
{
  std::array<std::byte, 64> storage{};
  SinkStream sink{std::span{storage}};
  const auto data = Fill<Sixteen>(100);

  bool threw = false;
  try
  {
    server::util::WriteBoundedList<uint8_t>(
      sink, data, {.name = "t10", .rethrowOnCapacity = true});
  }
  catch (const std::overflow_error&)
  {
    threw = true;
  }

  Check(threw, "t10: overflow_error escapes when rethrowOnCapacity is set");
  Check(sink.GetCursor() == 1 + 3 * 16,
    "t10: the cursor is still rolled back to the last whole element",
    static_cast<long long>(sink.GetCursor()), static_cast<long long>(1 + 3 * 16));
}

// ------------------------------------------------------------------ тест 11

//! 11: ТАБЛИЧНЫЙ ПРОХОД ПО САМЫМ РИСКОВАННЫМ СООБЩЕНИЯМ. Каждое наполняется
//!     заведомо сверх своего потолка и обязано СЕРИАЛИЗОВАТЬСЯ БЕЗ БРОСКА в
//!     буфер команды — кроме входа в комнату, где бросок несущий и ожидаем.
template <typename Command>
void ProbeMessage(
  const char* name,
  const Command& command,
  const bool expectLogicError,
  const long long expectDeclared = -1,
  const std::size_t counterOffset = 0,
  const std::size_t counterWidth = 0)
{
  std::array<std::byte, MaxCommandDataSize> storage{};
  SinkStream sink{std::span{storage}};

  bool threwLogic = false;
  bool threwOther = false;
  try
  {
    Command::Write(command, sink);
  }
  catch (const std::logic_error&)
  {
    threwLogic = true;
  }
  catch (const std::exception& e)
  {
    threwOther = true;
    std::printf("  %s threw: %s\n", name, e.what());
  }

  if (expectLogicError)
  {
    Check(threwLogic, (std::string{"t11: "} + name + " must still throw logic_error").c_str());
    return;
  }

  Check(not threwLogic && not threwOther,
    (std::string{"t11: "} + name + " serialises without throwing").c_str());
  Check(sink.GetCursor() <= MaxCommandDataSize,
    (std::string{"t11: "} + name + " stays inside the command buffer").c_str(),
    static_cast<long long>(sink.GetCursor()), static_cast<long long>(MaxCommandDataSize));

  if (expectDeclared < 0)
    return;

  long long declared = 0;
  if (counterWidth == 1)
    declared = ReadCount<uint8_t>(storage, counterOffset);
  else if (counterWidth == 2)
    declared = ReadCount<uint16_t>(storage, counterOffset);
  else
    declared = ReadCount<uint32_t>(storage, counterOffset);

  Check(declared == expectDeclared,
    (std::string{"t11: "} + name + " declares its bound").c_str(), declared, expectDeclared);
}

void TestRiskiestMessages()
{
  constexpr std::size_t Overshoot = 50;

  {
    server::protocol::LobbyCommandShowInventoryOK cmd{};
    cmd.items.resize(250 + Overshoot);
    cmd.horses.resize(10 + Overshoot);
    ProbeMessage("LobbyCommandShowInventoryOK", cmd, false, 250, 0, 1);
  }
  {
    server::protocol::RanchCommandUserPetInfosOK cmd{};
    cmd.pets.resize(300);
    ProbeMessage("RanchCommandUserPetInfosOK", cmd, false);
  }
  {
    server::protocol::AcCmdCRUpdateEquipmentNotify cmd{};
    cmd.characterUid = 1;
    cmd.characterEquipment.resize(255 + Overshoot);
    cmd.mountEquipment.resize(10);
    // ★R74-fix-2 (WARN 2): потолок здесь ТОТ ЖЕ, что у того же хранимого
    // списка в кадре входа и в `RanchCharacter`, — 16, а не дефолтные 255.
    ProbeMessage("AcCmdCRUpdateEquipmentNotify", cmd, false,
      static_cast<long long>(server::protocol::MaxCharacterEquipmentCount), 4, 1);
  }
  {
    server::protocol::AcCmdCRRequestStorageOK cmd{};
    cmd.storedItems.resize(255 + Overshoot);
    ProbeMessage("AcCmdCRRequestStorageOK", cmd, false);
  }
  {
    server::protocol::AcCmdCRGuildMemberListOK cmd{};
    cmd.members.resize(255 + Overshoot);
    ProbeMessage("AcCmdCRGuildMemberListOK", cmd, false, 255, 0, 1);
  }
  {
    server::protocol::AcCmdCREnterRanchOK cmd{};
    cmd.rancherUid = 1;
    cmd.rancherName = "r";
    cmd.ranchName = "r";
    cmd.horses.resize(10 + Overshoot);
    cmd.characters.resize(20 + Overshoot);
    cmd.housing.resize(13 + Overshoot);
    ProbeMessage("AcCmdCREnterRanchOK", cmd, false);
  }
  {
    server::protocol::AcCmdCRStartRaceNotify cmd{};
    cmd.racers.resize(255 + Overshoot);
    ProbeMessage("AcCmdCRStartRaceNotify", cmd, false);
  }
  {
    server::protocol::AcCmdCRUseMagicItemNotify cmd{};
    cmd.characterOid = 1;
    cmd.magicItemId = 0x2;
    cmd.targetList.resize(255 + Overshoot);
    ProbeMessage("AcCmdCRUseMagicItemNotify", cmd, false);
  }
  {
    // MUST-NOT-TOUCH: бросок несущий, его ловит гард входа в комнату.
    server::protocol::AcCmdCREnterRoomOK cmd{};
    cmd.racers.resize(11);
    ProbeMessage("AcCmdCREnterRoomOK", cmd, true);
  }
  {
    server::protocol::ChatCmdLoginAckOK cmd{};
    cmd.groups.resize(100);
    cmd.friends.resize(100);
    ProbeMessage("ChatCmdLoginAckOK", cmd, false);
  }
  {
    server::protocol::ChatCmdLetterListAckOk cmd{};
    cmd.mailboxFolder = server::protocol::MailboxFolder::Inbox;
    cmd.mailboxInfo.mailCount = 9999;
    cmd.inboxMails.resize(10 + Overshoot);
    ProbeMessage("ChatCmdLetterListAckOk", cmd, false);
  }
  {
    server::protocol::AcCmdLCGoodsShopListData cmd{};
    cmd.data.resize(MaxCommandDataSize * 2);
    ProbeMessage("AcCmdLCGoodsShopListData", cmd, false);
  }
  {
    server::protocol::LobbyCommandLoginOK cmd{};
    cmd.equipmentItems.resize(16 + Overshoot);
    cmd.expiredItems.resize(250 + Overshoot);
    ProbeMessage("LobbyCommandLoginOK", cmd, false);
  }
}

//! Отдельная улика на почту: счётчик обязан совпасть с телом, а не с полем
//! `mailCount`, которое директор заполняет своим числом.
void TestMailCounterMatchesBody()
{
  server::protocol::ChatCmdLetterListAckOk cmd{};
  cmd.mailboxFolder = server::protocol::MailboxFolder::Inbox;
  cmd.mailboxInfo.mailCount = 9999;
  cmd.mailboxInfo.hasMoreMail = 1;
  cmd.inboxMails.resize(60);

  std::array<std::byte, MaxCommandDataSize> storage{};
  SinkStream sink{std::span{storage}};
  server::protocol::ChatCmdLetterListAckOk::Write(cmd, sink);

  const auto folderWidth = sizeof(server::protocol::MailboxFolder);
  const auto declared = ReadCount<uint32_t>(storage, folderWidth);
  Check(declared == 10, "t11m: the mail counter states the body, not mailCount",
    declared, 10);
}

//! 12: ПОДАВЛЕНИЕ ПОВТОРА. Одна площадка жалуется не чаще раза в окно.
//!
//! ★ЖАЛОБА ПОРОЖДАЕТСЯ КЛИЕНТОМ: список растёт от данных игрока, и вход он
//! может повторять сколько угодно. Незадросселированная строка стала бы флудом,
//! управляемым снаружи, — ровно тем, ради чего заведён `util::LogThrottle`.
//! Проверка ЧИТАЕТ настоящий вывод логгера, а не верит в наличие вызова.
void TestReportThrottle()
{
  auto captured = std::make_shared<std::ostringstream>();
  const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*captured);
  const auto previous = spdlog::default_logger();
  auto probe = std::make_shared<spdlog::logger>("bounded-list-probe", sink);
  probe->set_level(spdlog::level::trace);
  spdlog::set_default_logger(probe);

  const auto data = Fill<Quad>(40);
  for (int attempt = 0; attempt < 2; ++attempt)
  {
    std::array<std::byte, 4096> storage{};
    SinkStream sink2{std::span{storage}};
    // ОДНА И ТА ЖЕ строка вызова оба раза — иначе площадки были бы разные и
    // подавление тут ни при чём.
    server::util::WriteBoundedList<uint8_t>(
      sink2, data, {.maxCount = 4, .name = "t12"});
  }

  probe->flush();
  spdlog::set_default_logger(previous);

  const auto text = captured->str();
  std::size_t lines = 0;
  std::size_t at = 0;
  while ((at = text.find("bounded list truncated: t12", at)) != std::string::npos)
  {
    ++lines;
    at += 1;
  }
  Check(lines == 1,
    "t12: two truncations at one site inside the window produce ONE line",
    static_cast<long long>(lines), 1);
}


// ------------------------------------------------------------------ тест 13

//! 13: БЮДЖЕТ КАДРА ВХОДА (★R74-fix-2, subreview #1 WARN 1).
//!
//! Профиль, собранный из СОБСТВЕННЫХ ПОТОЛКОВ трёх полей, длину которых задаёт
//! клиент: представление 4096 (`MaxIntroductionLength`), 255 клавиатурных и 254
//! геймпадных привязки (столько принимает `Settings::Read`), полный блок
//! макросов на бюджете и 16 предметов экипировки. Такой кадр НЕ ВЛЕЗАЕТ в
//! буфер команды — и это не гипотеза, тест это печатает. `BudgetLoginFrame`
//! обязан сделать его отправляемым, не тронув ничего на диске.
void TestLoginFrameBudget()
{
  const auto build = []()
  {
    server::protocol::LobbyCommandLoginOK login{};
    login.name = std::string(16, 'n');
    login.notice = std::string(255, 'o');
    login.val6 = std::string(255, 'v');
    login.introduction = std::string(4096, 'i');
    for (uint32_t idx = 0; idx < 16; ++idx)
      login.equipmentItems.push_back(Item{.uid = idx, .tid = idx, .count = 1});
    for (uint32_t idx = 0; idx < 17; ++idx)
    {
      auto& mission = login.missions.emplace_back();
      mission.progress.resize(4);
    }
    login.skillRanks.values.resize(20);
    login.trainingProgression.mapProggressInfos.resize(20);
    for (uint32_t idx = 0; idx < 69; ++idx)
      login.systemContent.values.emplace(idx, idx);
    login.settings.typeBitset.set(server::protocol::Settings::Keyboard);
    login.settings.keyboardOptions.bindings.resize(255);
    login.settings.typeBitset.set(server::protocol::Settings::Gamepad);
    login.settings.gamepadOptions.bindings.resize(254);
    login.settings.typeBitset.set(server::protocol::Settings::Macros);
    const auto perMacro = server::protocol::MaxMacroBlockWireBytes / 8 - 1;
    for (auto& macro : login.settings.macroOptions.macros)
      macro = std::string(perMacro, 'm');
    return login;
  };

  const auto measure = [](const server::protocol::LobbyCommandLoginOK& login,
                          bool& threw)
  {
    static std::array<std::byte, 1 << 20> big{};
    SinkStream sink{std::span{big}};
    threw = false;
    try
    {
      server::protocol::LobbyCommandLoginOK::Write(login, sink);
    }
    catch (const std::exception&)
    {
      threw = true;
    }
    return sink.GetCursor();
  };

  // (а) без бюджета кадр обязан НЕ влезать — иначе тест ничего не судит.
  auto poisoned = build();
  bool threw = false;
  const auto rawSize = measure(poisoned, threw);
  std::printf("t13: кадр на собственных потолках клиентских полей = %zu байт "
              "(буфер команды %zu)\n", rawSize, MaxCommandDataSize);
  Check(rawSize > server::protocol::MaxClientPacketDataBytes,
    "t13: без бюджета кадр НЕ влезает в потолок КЛИЕНТА",
    static_cast<long long>(rawSize),
    static_cast<long long>(server::protocol::MaxClientPacketDataBytes));

  // (б) с бюджетом обязан влезть, и сброшено обязано быть НАЗВАНО.
  const auto shed = server::protocol::BudgetLoginFrame(poisoned);
  const auto budgetedSize = measure(poisoned, threw);
  std::printf("t13: после BudgetLoginFrame = %zu байт, запас %zu, маска сброса 0x%x, "
              "представление %zu байт\n",
    budgetedSize,
    budgetedSize < server::protocol::MaxClientPacketDataBytes
      ? server::protocol::MaxClientPacketDataBytes - budgetedSize : 0,
    shed, poisoned.introduction.size());
  Check(not threw, "t13: забюджетированный кадр пишется без броска");
  // ★W2-5: цель — ПОТОЛОК КЛИЕНТА (7168 по собственному RE проекта), а не
  // буфер сервера: 8192 физически ограничивает запись, но кадр в полосе
  // 7169..8192 сервер запишет, а клиент не прочтёт.
  Check(budgetedSize <= server::protocol::MaxClientPacketDataBytes,
    "t13: забюджетированный кадр влезает в ПОТОЛОК КЛИЕНТА",
    static_cast<long long>(budgetedSize),
    static_cast<long long>(server::protocol::MaxClientPacketDataBytes));
  Check(server::protocol::MaxClientPacketDataBytes < MaxCommandDataSize,
    "t13: потолок клиента строго ниже буфера сервера",
    static_cast<long long>(server::protocol::MaxClientPacketDataBytes),
    static_cast<long long>(MaxCommandDataSize));
  Check(shed != static_cast<uint32_t>(server::protocol::LoginFrameShed::Nothing),
    "t13: сброс НАЗВАН, а не сделан молча", shed, 1);
  Check((shed & static_cast<uint32_t>(server::protocol::LoginFrameShed::StillTooLarge)) == 0,
    "t13: причина влезания — клиентские поля, а не что-то ещё", shed, 0);

  // (в) честный профиль не трогается вовсе.
  server::protocol::LobbyCommandLoginOK honest{};
  honest.name = "Nmax";
  honest.introduction = std::string(120, 'i');
  honest.settings.typeBitset.set(server::protocol::Settings::Keyboard);
  honest.settings.keyboardOptions.bindings.resize(40);
  honest.settings.typeBitset.set(server::protocol::Settings::Macros);
  for (auto& macro : honest.settings.macroOptions.macros)
    macro = std::string(40, 'm');
  const auto honestShed = server::protocol::BudgetLoginFrame(honest);
  Check(honestShed == static_cast<uint32_t>(server::protocol::LoginFrameShed::Nothing),
    "t13: честный профиль не сбрасывается ничем", honestShed, 0);
  Check(honest.introduction.size() == 120,
    "t13: честное представление не тронуто",
    static_cast<long long>(honest.introduction.size()), 120);
  Check(honest.settings.typeBitset.test(server::protocol::Settings::Macros),
    "t13: честные макросы остались в кадре");
}

// ------------------------------------------------------------------ тест 14

//! 14: ОДИН ХРАНИМЫЙ СПИСОК — ОДИН ПОТОЛОК НА ВСЕХ ТРЁХ ПЛОЩАДКАХ
//!     (★R74-fix-2, subreview #1 WARN 2).
void TestEquipmentCeilingIsConsistent()
{
  constexpr std::size_t Seeded = 20;

  server::protocol::AcCmdCRUpdateEquipmentNotify notify{};
  notify.characterUid = 1;
  notify.characterEquipment.resize(Seeded);
  notify.mountEquipment.resize(2);

  std::array<std::byte, MaxCommandDataSize> notifyStorage{};
  SinkStream notifySink{std::span{notifyStorage}};
  server::protocol::AcCmdCRUpdateEquipmentNotify::Write(notify, notifySink);
  // u32 characterUid, затем счётчик экипировки.
  const auto notifyDeclared = ReadCount<uint8_t>(notifyStorage, 4);

  server::protocol::RanchCharacter ranchCharacter{};
  ranchCharacter.uid = 1;
  ranchCharacter.characterEquipment.resize(Seeded);
  std::array<std::byte, MaxCommandDataSize> ranchStorage{};
  SinkStream ranchSink{std::span{ranchStorage}};
  server::protocol::RanchCharacter::Write(ranchCharacter, ranchSink);

  server::protocol::LobbyCommandLoginOK login{};
  login.equipmentItems.resize(Seeded);
  std::array<std::byte, MaxCommandDataSize> loginStorage{};
  SinkStream loginSink{std::span{loginStorage}};
  server::protocol::LobbyCommandLoginOK::Write(login, loginSink);
  const auto loginOffset = 16 + login.name.size() + 1 + login.notice.size() + 1 + 1
    + login.introduction.size() + 1;
  const auto loginDeclared = ReadCount<uint8_t>(loginStorage, loginOffset);

  Check(loginDeclared == server::protocol::MaxCharacterEquipmentCount,
    "t14: LoginOK объявляет потолок экипировки", loginDeclared,
    static_cast<long long>(server::protocol::MaxCharacterEquipmentCount));
  Check(notifyDeclared == server::protocol::MaxCharacterEquipmentCount,
    "t14: AcCmdCRUpdateEquipmentNotify объявляет ТОТ ЖЕ потолок",
    notifyDeclared, static_cast<long long>(server::protocol::MaxCharacterEquipmentCount));
}

// ------------------------------------------------------------------ тест 15

//! 15: ВЕТКА ПРИНЯТИЯ БЛОКА МАКРОСОВ ИСПОЛНЯЕТСЯ (★R74-fix-2, subreview #1 WARN 4).
//!
//! До этой проверки ни один образ и ни один тест не проходил по ветке, которая
//! макросы ПРИНИМАЕТ: все прод-записи идут с `macros: null`, а стенд сеял только
//! яд. Строка `a6_macros_bit` не имела образа, на котором обязана краснеть, —
//! то есть нарушала собственный стандарт вердикта.
void TestMacroAcceptingBranch()
{
  server::data::Settings stored{};
  std::array<std::string, 8> macros{};
  for (auto& macro : macros)
    macro = std::string(20, 'a');
  stored.macros() = macros;

  server::protocol::Settings published{};
  server::protocol::BuildProtocolSettings(published, stored);

  Check(published.typeBitset.test(server::protocol::Settings::Macros),
    "t15: блок макросов В БЮДЖЕТЕ обязан быть ОПУБЛИКОВАН");
  Check(published.macroOptions.macros[0] == macros[0],
    "t15: опубликован именно сохранённый блок, а не пустой");

  // Зеркало: отравленная запись обязана НЕ публиковаться.
  server::data::Settings poisoned{};
  std::array<std::string, 8> poisonedMacros{};
  for (auto& macro : poisonedMacros)
    macro = std::string(1800, 'A');
  poisoned.macros() = poisonedMacros;

  server::protocol::Settings withheld{};
  server::protocol::BuildProtocolSettings(withheld, poisoned);
  Check(not withheld.typeBitset.test(server::protocol::Settings::Macros),
    "t15: блок макросов СВЕРХ бюджета не публикуется");
}

// ------------------------------------------------------------------ тест 16

//! 16: ЧАСТИЧНЫЙ ПРИЁМ БЛОКА МАКРОСОВ (★subreview #1 WARN 3, #2 WARN 1 и NIT 4).
//!
//! ★ЗОВЁТ ХЕЛПЕР, А НЕ ПОВТОРЯЕТ ЕГО. Прежняя редакция переписывала цикл
//! отбора копией, а сам обработчик в тестовый бинарь не линкуется — значит
//! мутация оригинала оставляла тест зелёным. Теперь отбор живёт в libserver
//! (`SelectMacroSlotsWithinBudget`), и обе стороны зовут ОДНО И ТО ЖЕ.
void TestMacroPartialAcceptance()
{
  // (а) блок целиком сверх бюджета, слоты одинаковые — принимается префикс.
  server::protocol::MacroOptions oversized{};
  for (auto& macro : oversized.macros)
    macro = std::string(1800, 'A');
  Check(server::protocol::MeasureMacroBlockWireSize(oversized)
          > server::protocol::MaxMacroBlockWireBytes,
    "t16: исходный блок действительно сверх бюджета");

  server::protocol::MacroOptions accepted{};
  const auto slots = server::protocol::SelectMacroSlotsWithinBudget(oversized, accepted);
  std::printf("t16: слотов принято %zu из 8, размер принятого блока %zu из %zu\n",
    slots,
    server::protocol::MeasureMacroBlockWireSize(accepted),
    server::protocol::MaxMacroBlockWireBytes);
  Check(slots > 0, "t16: хотя бы один слот обязан быть принят",
    static_cast<long long>(slots), 1);
  Check(slots < 8, "t16: но не все — иначе блок не был бы сверх бюджета",
    static_cast<long long>(slots), 7);
  Check(server::protocol::MeasureMacroBlockWireSize(accepted)
          <= server::protocol::MaxMacroBlockWireBytes,
    "t16: принятое влезает в бюджет");

  // (б) ★ГЛАВНЫЙ СЛУЧАЙ subreview #2 WARN 1: ПЕРВЫЙ слот один бьёт бюджет,
  //     остальные семь крошечные. Прежний отбор `break`-ал на слоте 0 и давал
  //     НОЛЬ принятых, затирая хранимый блок восемью пустыми строками.
  server::protocol::MacroOptions headHeavy{};
  headHeavy.macros[0] = std::string(2100, 'A');
  for (std::size_t slot = 1; slot < headHeavy.macros.size(); ++slot)
    headHeavy.macros[slot] = "abc";

  server::protocol::MacroOptions kept{};
  const auto keptSlots = server::protocol::SelectMacroSlotsWithinBudget(headHeavy, kept);
  std::printf("t16: голова 2100 Б + семь коротких -> принято %zu слотов\n", keptSlots);
  Check(keptSlots == 7,
    "t16: семь влезающих слотов принимаются, несмотря на неподъёмный слот 0",
    static_cast<long long>(keptSlots), 7);
  Check(kept.macros[0].empty(),
    "t16: неподъёмный слот 0 пропущен");
  Check(kept.macros[7] == "abc",
    "t16: ИНДЕКСЫ СОХРАНЕНЫ — слот 7 остался слотом 7");
  std::size_t nonEmpty = 0;
  for (const auto& macro : kept.macros)
    if (not macro.empty())
      ++nonEmpty;
  Check(nonEmpty == 7, "t16: хранимый блок НЕ пуст",
    static_cast<long long>(nonEmpty), 7);

  // (в) ни один слот не влезает — принимать нечего, и это отличимо от «всё ок».
  server::protocol::MacroOptions allHuge{};
  for (auto& macro : allHuge.macros)
    macro = std::string(3000, 'A');
  server::protocol::MacroOptions none{};
  Check(server::protocol::SelectMacroSlotsWithinBudget(allHuge, none) == 0,
    "t16: ни один слот не влез — принято 0 (вызывающий не трогает хранимое)");

  // NIT 2 итерации 1: размер сверх мерочного скретча описывается честно.
  const auto unknown = server::protocol::DescribeMacroBlockWireSize(
    std::numeric_limits<std::size_t>::max());
  Check(unknown.find("exact size unknown") != std::string::npos,
    "t16: неизмеримый размер не печатается как число байт");
  Check(server::protocol::DescribeMacroBlockWireSize(123) == "123 bytes",
    "t16: измеримый размер печатается как есть");
}

// ------------------------------------------------------------------ тест 17

//! 17: СУХОЙ ПРОГОН НЕ ОСТАВЛЯЕТ НИ СТРОКИ, НИ СЧЁТЧИКА ОКНА
//!     (★subreview #2, WARN 2).
void TestDryRunLeavesNoTrace()
{
  auto captured = std::make_shared<std::ostringstream>();
  const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(*captured);
  const auto previous = spdlog::default_logger();
  auto probe = std::make_shared<spdlog::logger>("dry-run-probe", sink);
  probe->set_level(spdlog::level::trace);
  spdlog::set_default_logger(probe);

  const auto data = Fill<Quad>(40);

  // ★R74-fix-4 (subreview #3, NIT 1): ОБЕ ЗАПИСИ — С ОДНОЙ И ТОЙ ЖЕ СТРОКИ
  // ВЫЗОВА. Дроссель ключится по `(file, line)` из `source_location`, взятого
  // аргументом по умолчанию, — а прежняя редакция ставила сухой прогон и
  // «настоящую» запись на РАЗНЫЕ строки, то есть на разные площадки. Второе
  // утверждение тогда не могло упасть по названной причине: мутант, который
  // держит строку подавленной, но ЖЖЁТ окно, проходил обе проверки. Один
  // вызов в цикле снимает вопрос — площадка буквально одна.
  const auto writeSite = [&data](bool dryRun)
  {
    std::array<std::byte, 4096> storage{};
    SinkStream sink{std::span{storage}};
    // Глушитель — необязательный, а ВЫЗОВ РОВНО ОДИН: только так обе записи
    // приходят с одной строки, то есть с одной площадки дросселя.
    std::optional<server::util::ScopedBoundedListSilence> silence;
    if (dryRun)
      silence.emplace();
    server::util::WriteBoundedList<uint8_t>(
      sink, data, {.maxCount = 4, .name = "t17"});
  };

  writeSite(true);
  probe->flush();
  const auto afterDryRun = captured->str();

  // Настоящая запись С ТОЙ ЖЕ ПЛОЩАДКИ обязана дать строку — то есть окно
  // подавления сухим прогоном НЕ взведено.
  writeSite(false);
  probe->flush();
  spdlog::set_default_logger(previous);
  const auto afterReal = captured->str();

  Check(afterDryRun.find("bounded list truncated: t17") == std::string::npos,
    "t17: сухой прогон не печатает ни одной строки усечения");
  Check(afterReal.find("bounded list truncated: t17") != std::string::npos,
    "t17: настоящая запись после сухого прогона строку ДАЁТ — окно не сожжено");
}

// ------------------------------------------------------------------ тест 18

//! 18: ОДИН ПЕРЕРОСШИЙ ЭЛЕМЕНТ НЕ ОБНУЛЯЕТ СТРАНИЦУ (★subreview #2, WARN 4).
void TestOversizedElementIsSkipped()
{
  server::protocol::ChatCmdLetterListAckOk page{};
  page.mailboxFolder = server::protocol::MailboxFolder::Inbox;
  page.mailboxInfo.hasMoreMail = 0;

  // Элемент 0 — отравленное письмо, за ним девять коротких.
  auto& poisoned = page.inboxMails.emplace_back();
  poisoned.uid = 1;
  poisoned.sender = "attacker";
  poisoned.date = "00:00:00 01/01/2026 UTC";
  poisoned.struct0.unk0 = "\x0F";
  poisoned.struct0.body = std::string(4040, 'X');
  for (uint32_t index = 0; index < 9; ++index)
  {
    auto& mail = page.inboxMails.emplace_back();
    mail.uid = 100 + index;
    mail.sender = "friend";
    mail.date = "00:00:00 01/01/2026 UTC";
    mail.struct0.unk0 = "\x0F";
    mail.struct0.body = "hi";
  }

  // Кадр чаттера, а не команды: 4092 минус четырёхбайтовый заголовок.
  std::array<std::byte, 4088> storage{};
  SinkStream sink{std::span{storage}};
  bool threw = false;
  try
  {
    server::protocol::ChatCmdLetterListAckOk::Write(page, sink);
  }
  catch (const std::exception&)
  {
    threw = true;
  }

  // u8 folder, затем u32 счётчик.
  const auto declared = ReadCount<uint32_t>(storage, 1);
  std::printf("t18: на проводе объявлено %u писем из 10 (кадр %zu Б)\n",
    declared, sink.GetCursor());
  Check(not threw, "t18: страница пишется без броска");
  Check(declared >= 9,
    "t18: девять коротких писем доезжают, несмотря на переросшее письмо 0",
    declared, 9);
}

// ------------------------------------------------------------------ тест 20

//! 20: ПОТОЛОК ТЕЛА ПИСЬМА ДЕРЖИТ ВСЕ ТРИ КАДРА ПРИ САМОМ ШИРОКОМ ИМЕНИ
//!     (★subreview #3, BLOCK 1 и BLOCK 2).
//!
//! ★ЮНИТ 18 ЭТОГО НЕ ВИДЕЛ ПО ПОСТРОЕНИЮ: он брал отправителя `attacker`
//! (8 байт) и тело 4040, которое гейт и так отклоняет, — то есть проверял
//! МЕХАНИЗМ пропуска и ни разу саму константу. Здесь имя берётся максимально
//! широким из тех, что пропускает `locale::IsNameValid(name, 18)`: 18
//! кириллических букв, которые счётчик считает узкими, а EUC-KR тратит по два
//! байта — 36 байт на проводе.
void TestMailCeilingWithWidestSender()
{
  // 18 кириллических букв: столько пропускает гейт имени, и ровно они дают
  // худшую ширину на проводе.
  const std::string wideSender = "Александрапетрович";
  const std::string asciiSender(18, 'A');
  const auto wideWireWidth = server::locale::FromUtf8(wideSender).size();
  const auto asciiWireWidth = server::locale::FromUtf8(asciiSender).size();
  std::printf("t20: имя 18 кириллических букв = %zu Б провода, 18 ASCII = %zu Б\n",
    wideWireWidth, asciiWireWidth);
  Check(wideWireWidth == 36,
    "t20: 18 кириллических букв стоят 36 байт EUC-KR",
    static_cast<long long>(wideWireWidth), 36);

  //! Потолок раунда. Число живёт в `MessengerDirector`, недоступном тесту, —
  //! поэтому здесь оно повторено ЯВНО и тут же проверено арифметикой кадров.
  constexpr std::size_t MailBodyCeiling = 4006;
  constexpr std::size_t ChatterPayload = 4088;

  const auto buildPage = [](const std::string& sender, std::size_t bodyBytes)
  {
    server::protocol::ChatCmdLetterListAckOk page{};
    page.mailboxFolder = server::protocol::MailboxFolder::Inbox;
    page.mailboxInfo.hasMoreMail = 0;
    auto& mail = page.inboxMails.emplace_back();
    mail.uid = 1;
    mail.sender = sender;
    mail.date = "00:00:00 01/01/2026 UTC";
    mail.struct0.unk0 = "\x0F";
    mail.struct0.body = std::string(bodyBytes, 'X');
    return page;
  };

  // (1) СТРАНИЦА ЯЩИКА: письмо РОВНО на потолке от самого широкого отправителя
  //     обязано доехать. До правки константа была 4026 и счётчик уходил в 0.
  {
    const auto page = buildPage(wideSender, MailBodyCeiling);
    std::array<std::byte, ChatterPayload> storage{};
    SinkStream sink{std::span{storage}};
    bool threw = false;
    try { server::protocol::ChatCmdLetterListAckOk::Write(page, sink); }
    catch (const std::exception&) { threw = true; }
    const auto declared = ReadCount<uint32_t>(storage, 1);
    std::printf("t20: страница с телом %zu Б и именем %zu Б -> %zu Б кадра, "
                "объявлено %u\n",
      MailBodyCeiling, wideWireWidth, sink.GetCursor(), declared);
    Check(not threw, "t20: страница на потолке пишется без броска");
    Check(declared == 1,
      "t20: письмо НА ПОТОЛКЕ доезжает на проводе (было 0 при константе 4026)",
      declared, 1);
  }

  // (2) ПОТОЛОК + 1 обязан НЕ влезать — иначе константа выбрана с запасом и
  //     ничего не доказывает.
  {
    const auto page = buildPage(wideSender, MailBodyCeiling + 1);
    std::array<std::byte, ChatterPayload> storage{};
    SinkStream sink{std::span{storage}};
    server::protocol::ChatCmdLetterListAckOk::Write(page, sink);
    const auto declared = ReadCount<uint32_t>(storage, 1);
    Check(declared == 0,
      "t20: потолок+1 в страницу уже НЕ влезает — константа стоит на границе",
      declared, 0);
  }

  // (3) ДОСТАВКА ЖЕРТВЕ (`ChatCmdLetterArriveTrs`) — плоский писатель без
  //     бюджета: бросок отсюда уходит в поставщик записи ЖЕРТВЫ и рвёт ей
  //     соединение. Проверяем на том же худшем имени и теле на потолке.
  {
    server::protocol::ChatCmdLetterArriveTrs arrive{};
    arrive.mailUid = 1;
    arrive.sender = wideSender;
    arrive.date = "00:00:00 01/01/2026 UTC";
    arrive.body = std::string(MailBodyCeiling, 'X');
    std::array<std::byte, ChatterPayload> storage{};
    SinkStream sink{std::span{storage}};
    bool threw = false;
    try { server::protocol::ChatCmdLetterArriveTrs::Write(arrive, sink); }
    catch (const std::exception&) { threw = true; }
    std::printf("t20: ArriveTrs = %zu Б из %zu\n", sink.GetCursor(), ChatterPayload);
    Check(not threw,
      "t20: доставка на потолке НЕ бросает — иначе жертву выкидывает из мессенджера");
  }

  // (4) КВИТАНЦИЯ ОТПРАВИТЕЛЮ (`ChatCmdLetterSendAckOk`) — зеркальный случай,
  //     роняющий уже отправителя, причём ПОСЛЕ того как письмо сохранено.
  {
    server::protocol::ChatCmdLetterSendAckOk ack{};
    ack.mailUid = 1;
    ack.recipient = wideSender;
    ack.date = "00:00:00 01/01/2026 UTC";
    ack.body = std::string(MailBodyCeiling, 'X');
    std::array<std::byte, ChatterPayload> storage{};
    SinkStream sink{std::span{storage}};
    bool threw = false;
    try { server::protocol::ChatCmdLetterSendAckOk::Write(ack, sink); }
    catch (const std::exception&) { threw = true; }
    std::printf("t20: SendAckOk = %zu Б из %zu\n", sink.GetCursor(), ChatterPayload);
    Check(not threw, "t20: квитанция на потолке НЕ бросает");
  }

  // (5) ДЕСЯТЬ писем на потолке от широкого отправителя: страница обязана
  //     показать хотя бы одно. Это и есть сценарий BLOCK 1 целиком.
  {
    server::protocol::ChatCmdLetterListAckOk page{};
    page.mailboxFolder = server::protocol::MailboxFolder::Inbox;
    for (uint32_t index = 0; index < 10; ++index)
    {
      auto& mail = page.inboxMails.emplace_back();
      mail.uid = 100 + index;
      mail.sender = wideSender;
      mail.date = "00:00:00 01/01/2026 UTC";
      mail.struct0.unk0 = "\x0F";
      mail.struct0.body = std::string(MailBodyCeiling, 'X');
    }
    std::array<std::byte, ChatterPayload> storage{};
    SinkStream sink{std::span{storage}};
    server::protocol::ChatCmdLetterListAckOk::Write(page, sink);
    const auto declared = ReadCount<uint32_t>(storage, 1);
    std::printf("t20: десять писем на потолке -> объявлено %u\n", declared);
    Check(declared >= 1,
      "t20: ящик НЕ пуст — жертва видит письмо и может его удалить",
      declared, 1);
  }
}

// ------------------------------------------------------------------ тест 19

//! 19: ОБРЕЗКА ПРЕДСТАВЛЕНИЯ НЕ РВЁТ МНОГОБАЙТНЫЙ СИМВОЛ (★subreview #2, NIT 5).
void TestIntroductionTruncationKeepsCharacters()
{
  const auto isValidUtf8 = [](const std::string& value)
  {
    std::size_t index = 0;
    while (index < value.size())
    {
      const auto lead = static_cast<unsigned char>(value[index]);
      std::size_t length = 0;
      if (lead < 0x80) length = 1;
      else if ((lead & 0xE0) == 0xC0) length = 2;
      else if ((lead & 0xF0) == 0xE0) length = 3;
      else if ((lead & 0xF8) == 0xF0) length = 4;
      else return false;
      if (index + length > value.size())
        return false;
      for (std::size_t k = 1; k < length; ++k)
        if ((static_cast<unsigned char>(value[index + k]) & 0xC0) != 0x80)
          return false;
      index += length;
    }
    return true;
  };

  // Хангыль (3 байта на символ) и кириллица (2) — те, что рвутся; все прежние
  // фикстуры раунда были ASCII и этого не видели.
  for (const auto* sample : {"\uD55C", "\u043F"})
  {
    std::string text;
    while (text.size() < 4096)
      text += sample;

    server::protocol::LobbyCommandLoginOK login{};
    login.name = "wedge";
    login.introduction = text;
    login.settings.typeBitset.set(server::protocol::Settings::Keyboard);
    login.settings.keyboardOptions.bindings.resize(255);
    login.settings.typeBitset.set(server::protocol::Settings::Gamepad);
    login.settings.gamepadOptions.bindings.resize(254);
    login.settings.typeBitset.set(server::protocol::Settings::Macros);
    const auto perMacro = server::protocol::MaxMacroBlockWireBytes / 8 - 1;
    for (auto& macro : login.settings.macroOptions.macros)
      macro = std::string(perMacro, 'm');
    for (uint32_t index = 0; index < 69; ++index)
      login.systemContent.values.emplace(index, index);

    (void)server::protocol::BudgetLoginFrame(login);
    Check(isValidUtf8(login.introduction),
      "t19: после обрезки представление остаётся валидным UTF-8",
      static_cast<long long>(login.introduction.size()), 0);
  }
}

} // namespace

int main()
{
  TestCountBounds();
  TestCapacity();
  TestCountScale();
  TestByteIdentity();
  TestFrameBudgets();
  TestMacroBudget();
  TestRethrowOnCapacity();
  TestRiskiestMessages();
  TestMailCounterMatchesBody();
  TestReportThrottle();
  TestLoginFrameBudget();
  TestEquipmentCeilingIsConsistent();
  TestMacroAcceptingBranch();
  TestMacroPartialAcceptance();
  TestDryRunLeavesNoTrace();
  TestOversizedElementIsSkipped();
  TestMailCeilingWithWidestSender();
  TestIntroductionTruncationKeepsCharacters();

  if (failures != 0)
  {
    std::printf("TestBoundedList: %d checks FAILED\n", failures);
    return 1;
  }

  std::printf("TestBoundedList: all checks passed\n");
  return 0;
}
