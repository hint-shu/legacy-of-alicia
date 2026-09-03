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

// См. пояснение в TestNameGuard.cpp: `assert` не имеет права исчезнуть.
#undef NDEBUG

//! ★ЧТО ЗДЕСЬ ДОКАЗЫВАЕТСЯ И ПОЧЕМУ ЭТОГО НЕ БЫЛО РАНЬШЕ (правка ревью,
//! итерация 7).
//!
//! Итерация 6 завела три механизма в слое данных — удаление от дескриптора
//! каталога, индекс имён гильдий и его перестройку — и НИ ОДИН из них не имел
//! наблюдателя: юнит-тесты кончались на `AtomicFile.hpp`, а стенд ходит через
//! протокол и до этих исходов не достаёт. Ревью нашло в этих трёх механизмах
//! три дефекта, каждый из которых виден отсюда за секунду:
//!   * неудача удаления возвращалась как успех;
//!   * нечитаемый файл гильдии публиковал ПОЛНЫЙ на вид индекс, и его имя
//!     читалось как свободное;
//!   * личность записи в индексе бралась из `json["uid"]`, хотя адресуется
//!     запись ИМЕНЕМ ФАЙЛА.
//! Проверка, которой нет, — это не «свойство держится», это «никто не смотрел».

#include <libserver/data/file/FileDataSource.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

std::filesystem::path MakeSandbox(const std::string& name)
{
  const auto sandbox = std::filesystem::temp_directory_path()
    / std::filesystem::path("alicia-filedatasource-test") / name;
  std::error_code ignored;
  std::filesystem::remove_all(sandbox, ignored);
  std::filesystem::create_directories(sandbox);
  return sandbox;
}

void WriteRaw(const std::filesystem::path& path, const std::string& payload)
{
  std::ofstream file(path, std::ios::trunc);
  assert(file.is_open());
  file << payload;
}

server::data::Guild MakeGuild(
  const server::data::Uid uid, const std::string& name)
{
  server::data::Guild guild;
  guild.uid = uid;
  guild.name = name;
  guild.description = std::string{"a guild"};
  guild.owner = server::data::Uid{1};
  return guild;
}

//! ★НЕУДАЧА УДАЛЕНИЯ — ЭТО ОТКАЗ, А НЕ УСПЕХ.
//!
//! Итерация 6 заменила бросающий `std::filesystem::remove` на функцию с кодом
//! возврата и НЕ СТАЛА ЕГО ЧИТАТЬ. Отказ (права, ввод-вывод, подменённый
//! каталог) при этом выглядел как состоявшееся удаление: `DataDirector` возвращал
//! успех, `DataStorage` выбрасывал запись из кэша, имя объявлялось свободным, а
//! файл оставался лежать — и запись возвращалась после перезапуска.
void TestFailedDeleteIsNotSilentSuccess()
{
  const auto root = MakeSandbox("failed-delete");
  server::FileDataSource source;
  source.Initialize(root);

  source.StoreGuild(7, MakeGuild(7, "Alpha"));
  assert(not source.IsGuildNameUnique("alpha"));   // имя занято, регистр не важен

  // Запись становится НЕУДАЛЯЕМОЙ: на её месте каталог, и `unlinkat` даёт
  // EISDIR независимо от прав — то есть утверждение верно и под root-армом.
  const auto record = root / "guilds" / "7.json";
  std::error_code error;
  std::filesystem::remove(record, error);
  assert(not error);
  std::filesystem::create_directories(record);

  bool threw = false;
  try
  {
    source.DeleteGuild(7);
  }
  catch (const std::exception&)
  {
    threw = true;
  }
  assert(threw);

  // ★И ИМЯ ОСТАЛОСЬ ЗАНЯТЫМ. Это вторая половина находки: `Forget*Name` не
  // имеет права сработать раньше подтверждённого удаления.
  assert(not source.IsGuildNameUnique("Alpha"));

  // Контроль направления: та же гильдия удаляется, когда удаление ВОЗМОЖНО.
  std::filesystem::remove_all(record, error);
  source.StoreGuild(7, MakeGuild(7, "Alpha"));
  source.DeleteGuild(7);
  assert(source.IsGuildNameUnique("Alpha"));

  std::filesystem::remove_all(root, error);
}

//! ★ЛИЧНОСТЬ ЗАПИСИ В ИНДЕКСЕ — ИМЯ ФАЙЛА, А НЕ ПОЛЕ ВНУТРИ НЕГО.
//!
//! `StoreGuild`/`DeleteGuild` адресуют файл именем; перестройка брала uid из
//! `json["uid"]`. `7.json` с полем `uid: 8` делал `DeleteGuild(7)` неспособным
//! освободить имя — оно оставалось занятым до перезапуска.
void TestIndexIdentityComesFromTheFileName()
{
  const auto root = MakeSandbox("identity");
  std::filesystem::create_directories(root / "guilds");
  WriteRaw(root / "guilds" / "7.json",
    R"({"uid": 8, "name": "Beta", "owner": 1, "officers": [], "members": []})");

  server::FileDataSource source;
  source.Initialize(root);

  assert(not source.IsGuildNameUnique("Beta"));    // имя с диска занято
  source.DeleteGuild(7);                           // снимаем ФАЙЛ 7.json
  assert(source.IsGuildNameUnique("Beta"));        // и имя обязано освободиться

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★ИМЯ, КОТОРОГО МЫ НЕ ВИДЕЛИ, НЕ ЧИТАЕТСЯ КАК СВОБОДНОЕ.
//!
//! Перестройка МОЛЧА пропускала нечитаемый файл, битый JSON, пустое имя и имя
//! файла, из которого не читается uid, — и публиковала набор, выглядящий полным.
//! Гильдия, чей файл на старте оказался временно нечитаемым, отдавала своё имя
//! следующему желающему на всё время работы сервера.
void TestUnreadableRecordMakesEveryNameTaken()
{
  const auto root = MakeSandbox("unreadable");
  std::filesystem::create_directories(root / "guilds");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "guilds" / "9.json", "{ this is not json");
  WriteRaw(root / "characters" / "9.json", "{ this is not json either");

  server::FileDataSource source;
  source.Initialize(root);

  // Индекс неполон -> занято ВСЁ, а не «свободно всё, кроме увиденного».
  assert(not source.IsGuildNameUnique("NobodyHasThisName"));
  assert(not source.IsCharacterNameUnique("NobodyHasThisNameEither"));

  // ★И ЭТО СОСТОЯНИЕ САМО ЧИНИТСЯ. Файл стал читаемым — ответ обязан вернуться
  // к правде без перезапуска. Пауза длиннее предела частоты попыток (2 с).
  WriteRaw(root / "guilds" / "9.json",
    R"({"uid": 9, "name": "Gamma", "owner": 1, "officers": [], "members": []})");
  WriteRaw(root / "characters" / "9.json",
    R"({"uid": 9, "name": "Delta"})");
  std::this_thread::sleep_for(std::chrono::milliseconds(2200));

  assert(source.IsGuildNameUnique("NobodyHasThisName"));
  assert(not source.IsGuildNameUnique("Gamma"));
  assert(source.IsCharacterNameUnique("NobodyHasThisNameEither"));
  assert(not source.IsCharacterNameUnique("Delta"));

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★ИМЯ ГИЛЬДИИ С ПРОВОДА ПРОХОДИТ СТРУКТУРНЫЙ ГЕЙТ, А НЕ ТОЛЬКО ИНДЕКС.
//!
//! Директива ведущего требовала провести создание гильдии через NameGuard И
//! индекс; итерация 6 поставила только индекс, и правило «имя с провода не
//! оплачивается работой хранилища» держалось вежливостью ЕДИНСТВЕННОГО
//! вызывающего (`RanchDirector` зовёт `locale::IsNameValid` с потолком 18
//! байт). Второй вызывающий, написанный по образцу соседей, унаследовал бы
//! отсутствие гейта — а соседи (`RetrieveCharacterUidByName`,
//! `IsUserNameUnique`) свой гейт имеют.
//!
//! ★НАПРАВЛЕНИЕ ОТКАЗА ЗДЕСЬ ПРОТИВОПОЛОЖНО ПОИСКУ: имя, которое физически не
//! может лежать на диске, обязано читаться как ЗАНЯТОЕ (отказ в создании), а не
//! как свободное — иначе гейт стал бы способом пройти проверку уникальности.
void TestGuildNameGateRefusesUnstorableNames()
{
  const auto root = MakeSandbox("guild-name-gate");
  server::FileDataSource source;
  source.Initialize(root);
  source.StoreGuild(3, MakeGuild(3, "Alpha"));

  // Контроль направления СНАЧАЛА: гейт не имеет права быть «всегда занято».
  assert(not source.IsGuildNameUnique("Alpha"));       // занято индексом
  assert(source.IsGuildNameUnique("Bravo"));           // свободно

  // Имя, которого не может быть на диске, — отказ, а не «свободно».
  assert(not source.IsGuildNameUnique(std::string("Bad\x01Name")));
  assert(not source.IsGuildNameUnique(std::string("Bad\x7fName")));
  assert(not source.IsGuildNameUnique("../../etc/passwd"));
  assert(not source.IsGuildNameUnique("back\\slash"));
  assert(not source.IsGuildNameUnique(""));
  assert(not source.IsGuildNameUnique(std::string(4096, 'x')));  // 8 КБ с провода

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★ПОТОЛОК ГЕЙТА БЕРЁТСЯ ИЗ ИНДЕКСА, А НЕ ИЗ КОНСТАНТЫ.
//!
//! Гейт, который строже индекса, который он охраняет, отнимает путь успеха:
//! гильдия, названная ДО появления валидатора (сегодня `IsNameValid` держит 18
//! байт, вчера не держал ничего), обязана остаться спрашиваемой. Тот же вывод
//! уже сделан для персонажей (правка ревью, итерация 1) — здесь он проверяется
//! для гильдий.
void TestGuildNameCeilingComesFromTheIndex()
{
  const auto root = MakeSandbox("guild-name-ceiling");
  std::filesystem::create_directories(root / "guilds");

  // 100 байт — длиннее пола гейта (64), то есть без подъёма потолка это имя
  // было бы неспрашиваемым, хотя оно ЛЕЖИТ на диске.
  const std::string longName(100, 'q');
  WriteRaw(root / "guilds" / "5.json",
    std::string(R"({"uid": 5, "name": ")") + longName
      + R"(", "owner": 1, "officers": [], "members": []})");

  server::FileDataSource source;
  source.Initialize(root);

  assert(not source.IsGuildNameUnique(longName));      // лежит -> занято
  // Другое имя той же длины гейт пропускает (потолок поднят), и индекс
  // отвечает правду: свободно.
  assert(source.IsGuildNameUnique(std::string(100, 'z')));
  // Но потолок конечен: 8 КБ с провода по-прежнему отбиваются.
  assert(not source.IsGuildNameUnique(std::string(4096, 'z')));

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★И ТО ЖЕ ПРАВИЛО ДЛЯ ПЕРСОНАЖА — ОДИН КЛАСС, ОДНО ПРАВИЛО.
//!
//! Гейт у персонажей стоял в ПОИСКЕ, а поиск отвечает «не нашёл»; проверка
//! уникальности читала этот ответ как «свободно». Персонаж, созданный с именем,
//! которого не может быть на диске, оказался бы навсегда неадресуем — подарок,
//! приглашение в заезд, друг и письмо ходят через тот же поиск, который это имя
//! отбивает. Чинить второе место списком было бы возвратом к «перечню сайтов»:
//! правило одно и живёт в хранилище.
void TestCharacterNameGateRefusesUnstorableNames()
{
  const auto root = MakeSandbox("character-name-gate");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "characters" / "4.json", R"({"uid": 4, "name": "Echo"})");

  server::FileDataSource source;
  source.Initialize(root);

  // Контроль направления: гейт не имеет права быть «всегда занято».
  assert(not source.IsCharacterNameUnique("Echo"));    // занято индексом
  assert(source.IsCharacterNameUnique("Foxtrot"));     // свободно

  // Имя, которого не может быть на диске, — отказ в создании, а не «свободно».
  assert(not source.IsCharacterNameUnique(std::string("Bad\x01Name")));
  assert(not source.IsCharacterNameUnique("../../etc/passwd"));
  assert(not source.IsCharacterNameUnique(""));
  assert(not source.IsCharacterNameUnique(std::string(4096, 'x')));

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

} // namespace

int main()
{
  TestFailedDeleteIsNotSilentSuccess();
  TestIndexIdentityComesFromTheFileName();
  TestUnreadableRecordMakesEveryNameTaken();
  TestGuildNameGateRefusesUnstorableNames();
  TestGuildNameCeilingComesFromTheIndex();
  TestCharacterNameGateRefusesUnstorableNames();
  std::puts("TestFileDataSource: ok");
}
