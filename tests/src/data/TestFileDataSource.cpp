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

#include <atomic>
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

//! ★ПРОВЕРКА, КОТОРАЯ НЕ УМЕЕТ ИСЧЕЗНУТЬ (правка ревью, итерация 11).
//!
//! `assert` выше жив только потому, что файл снимает `NDEBUG` первой строкой;
//! снимут её (или соберут этот файл иначе) — и утверждения станут пустыми, а
//! тест «зелёным» ровно там, где перестал смотреть. `CHECK` вычисляет условие
//! ВСЕГДА, печатает место и валит прогон кодом возврата.
int g_failures = 0;

void CheckImpl(const bool condition, const char* const expression,
  const char* const what, const int line)
{
  if (condition)
    return;
  ++g_failures;
  std::fprintf(stderr, "TestFileDataSource.cpp:%d: НАРУШЕНО (%s): %s\n",
    line, expression, what);
}

#define CHECK(condition, what) CheckImpl((condition), #condition, (what), __LINE__)

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
  // к правде без перезапуска.
  //
  // ★НО ЧИНИТ ЕГО ПЛАНОВЫЙ ПРОХОД, А НЕ ЗАПРОС (правка ревью, итерация 9).
  // Раньше здесь стояла пауза в 2.2 с и повторный вопрос: починку покупал сам
  // вопрос об имени, то есть имя с провода заказывало полный обход каталога.
  // Хуже того, у ПОИСКА персонажа такого пути не было вовсе, и починенный файл
  // оставался невидимым для подарка, друга и письма неограниченно долго.
  // Теперь повод один и общий — `TickNameIndexMaintenance`, — и ждать нечего:
  // первый проход после старта идёт без задержки.
  WriteRaw(root / "guilds" / "9.json",
    R"({"uid": 9, "name": "Gamma", "owner": 1, "officers": [], "members": []})");
  WriteRaw(root / "characters" / "9.json",
    R"({"uid": 9, "name": "Delta"})");

  // Контроль направления: до планового прохода ответ ещё «занято» — то есть
  // чинит именно проход, а не время само по себе.
  assert(not source.IsGuildNameUnique("NobodyHasThisName"));
  source.TickNameIndexMaintenance();

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


//! ★РЕМОНТ ИНДЕКСА ПРИХОДИТ ПО РАСПИСАНИЮ, А НЕ ЗА ЧУЖОЙ ПОИСК (ревью, 9).
//!
//! До этой правки индекс персонажей чинил РОВНО ОДИН путь —
//! `IsCharacterNameUnique`, то есть создание или переименование персонажа. Файл,
//! нечитаемый на старте и починенный через минуту, оставался невидимым для
//! подарка, друга, письма и приглашения в заезд НЕОГРАНИЧЕННО ДОЛГО: поиск
//! перестройку не звал, а создавать персонажа на живом шарде может никто и не
//! начать. Здесь проверяются ОБА направления сразу:
//!   * сколько ни ищи по имени — индекс НЕ чинится (поиск не покупает обход);
//!   * плановый проход чинит его БЕЗ единого поиска.
void TestScheduledPassRepairsTheIndexAndLookupsDoNot()
{
  const auto root = MakeSandbox("scheduled-repair");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "characters" / "1.json", R"({"uid": 1, "name": "Alpha"})");
  WriteRaw(root / "characters" / "2.json", R"({"uid": 2, "name": "Beta"})");

  // Один файл нечитаем на СТАРТЕ: индекс выходит неполным, и это ровно тот
  // случай, ради которого «имя, которое мы не видели, читается как занятое».
  WriteRaw(root / "characters" / "1.json", "{ this is not json");

  server::FileDataSource source;
  source.Initialize(root);

  // Индекс неполон: любое имя читается как ЗАНЯТОЕ, даже заведомо свободное.
  assert(not source.IsCharacterNameUnique("Gamma"));
  // А `Beta` при этом прочиталась и адресуется — путь успеха не потерян.
  assert(source.RetrieveCharacterUidByName("Beta") == server::data::Uid{2});
  assert(source.RetrieveCharacterUidByName("Alpha") == server::data::InvalidUid);

  // Файл ПОЧИНЕН на диске рядом с работающим сервером.
  WriteRaw(root / "characters" / "1.json", R"({"uid": 1, "name": "Alpha"})");

  // ★СКОЛЬКО НИ ИЩИ — ИНДЕКС НЕ ЧИНИТСЯ. Именно это и было платой: раньше
  // каждый такой промах мог заказать полный обход каталога.
  for (int attempt = 0; attempt < 500; ++attempt)
  {
    assert(source.RetrieveCharacterUidByName("Alpha") == server::data::InvalidUid);
  }
  assert(not source.IsCharacterNameUnique("Gamma"));

  // ★А ПЛАНОВЫЙ ПРОХОД — ЧИНИТ, И БЕЗ ЕДИНОГО ПОИСКА. Первый проход после
  // старта идёт без ожидания: неполная стартовая сборка не имеет права ждать
  // минуту.
  source.TickNameIndexMaintenance();

  assert(source.RetrieveCharacterUidByName("Alpha") == server::data::Uid{1});
  assert(source.IsCharacterNameUnique("Gamma"));      // индекс снова полон
  assert(not source.IsCharacterNameUnique("alpha"));  // и говорит правду

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★ИМЯ, КОТОРОЕ ЛЕЖИТ НА ДИСКЕ, ОБЯЗАНО НАХОДИТЬСЯ — ДАЖЕ ЕСЛИ СЕГОДНЯ ТАК
//! НАЗВАТЬ УЖЕ НЕЛЬЗЯ (ревью, итерация 9).
//!
//! Имя персонажа — ПОЛЕ ВНУТРИ файла, названного uid'ом, а не компонент пути.
//! Гейт создания отвергает `/` и `\` совершенно правильно; поставленный в
//! ПОИСК, тот же гейт делал персонажа `A/B` вечно неадресуемым — притом что
//! до-раундовый точный поиск его находил, а индекс его прекрасно видит. Это
//! обмен пути успеха на путь отказа, и здесь проверено, что обмена больше нет,
//! И ЧТО СОЗДАНИЕ ПРИ ЭТОМ НЕ ОСЛАБЛО.
void TestLegacyNameIsFindableButNotCreatable()
{
  const auto root = MakeSandbox("legacy-name-lookup");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "characters" / "9.json", R"({"uid": 9, "name": "A/B"})");
  WriteRaw(root / "characters" / "10.json", R"({"uid": 10, "name": "Ordinary"})");

  server::FileDataSource source;
  source.Initialize(root);

  // НАХОДИТСЯ: подарок, друг, письмо и приглашение в заезд ходят сюда.
  assert(source.RetrieveCharacterUidByName("A/B") == server::data::Uid{9});
  assert(source.RetrieveCharacterUidByName("a/b") == server::data::Uid{9});
  // И тем же поиском — обычное имя: гейт не стал «всегда да».
  assert(source.RetrieveCharacterUidByName("Ordinary") == server::data::Uid{10});
  assert(source.RetrieveCharacterUidByName("Nobody") == server::data::InvalidUid);

  // НЕ СОЗДАЁТСЯ: гейт создания остался строгим, и это разные направления.
  assert(not source.IsCharacterNameUnique("A/B"));
  assert(not source.IsCharacterNameUnique("C\\D"));

  // Поиск всё-таки ОГРАНИЧЕН: то, что ломает журнал или обрывает строку, и то,
  // что длиннее потолка индекса, отбивается до всякой работы.
  assert(source.RetrieveCharacterUidByName(std::string("A\nB")) == server::data::InvalidUid);
  assert(source.RetrieveCharacterUidByName(std::string("A\rB")) == server::data::InvalidUid);
  assert(source.RetrieveCharacterUidByName(std::string("A\0B", 3)) == server::data::InvalidUid);
  assert(source.RetrieveCharacterUidByName(std::string(4096, 'x')) == server::data::InvalidUid);
  assert(source.RetrieveCharacterUidByName("") == server::data::InvalidUid);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★ДВА НАПИСАНИЯ ОДНОГО ЧИСЛА — ЭТО ДВА ФАЙЛА, А НЕ ОДНА ЗАПИСЬ (ревью, 9).
//!
//! `007.json` читался индексом как uid 7. Одинокий `007.json` индексировался
//! под 7, а `DeleteCharacter(7)` удалял НЕСУЩЕСТВУЮЩИЙ `7.json`, считал ENOENT
//! успехом, забывал имя — и оставлял `007.json` живым с именем, которое теперь
//! числится свободным. Личность записи обязана быть однозначной; при этом ПОЛ
//! СЧЁТЧИКА uid обязан вести себя ПРОТИВОПОЛОЖНО и алиас резервировать, иначе
//! фикс сам раздал бы занятый uid.
void TestNoncanonicalFileNameIsNotARecord()
{
  const auto root = MakeSandbox("noncanonical-name");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "characters" / "007.json", R"({"uid": 7, "name": "Alias"})");
  WriteRaw(root / "characters" / "3.json", R"({"uid": 3, "name": "Real"})");

  server::FileDataSource source;
  source.Initialize(root);

  // Алиас — НЕ запись: его имя индекс не знает.
  assert(source.RetrieveCharacterUidByName("Alias") == server::data::InvalidUid);
  // Каноническая запись рядом читается как обычно.
  assert(source.RetrieveCharacterUidByName("Real") == server::data::Uid{3});
  // ★И ИНДЕКС ПРИ ЭТОМ ПОЛОН: посторонний файл в каталоге не имеет права
  // отказывать в создании ВСЕХ имён сразу.
  assert(source.IsCharacterNameUnique("Delta"));

  // ★А ПОЛ СЧЁТЧИКА uid АЛИАС ВСЁ-ТАКИ РЕЗЕРВИРУЕТ. Иначе следующий персонаж
  // получил бы uid 7, `ProduceDataFilePath` написал бы `7.json` — и рядом
  // остались бы два живых файла на одну личность.
  server::data::Character created;
  source.CreateCharacter(created);
  assert(created.uid() > server::data::Uid{7});

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★ПОЛНОТА И СОДЕРЖИМОЕ — ОДНО НАБЛЮДЕНИЕ (ревью, итерация 9).
//!
//! Настоящее окно этой находки — межпоточное: A промахивается по НЕПОЛНОМУ
//! индексу, B успевает пересобрать его и объявить полным, A читает НОВУЮ
//! полноту при СТАРОМ промахе и объявляет живое имя свободным. Стенд такую
//! чередовку не воспроизводит, и честнее сказать это, чем сделать вид, что
//! воспроизводит: детерминированный наблюдатель формы живёт в
//! `tools/name_index_invariants_gate.py` (проверка «один-снимок»), а здесь
//! проверяется ПОСЛЕДСТВИЕ, которое обязано держаться в любом случае: пока
//! индекс неполон, «свободно» не отвечается НИКОГДА, ни одним из двух путей.
void TestIncompleteIndexNeverAnswersFree()
{
  const auto root = MakeSandbox("incomplete-never-free");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "characters" / "1.json", "{ broken");
  WriteRaw(root / "characters" / "2.json", R"({"uid": 2, "name": "Beta"})");

  server::FileDataSource source;
  source.Initialize(root);

  std::atomic_bool stop{false};
  std::atomic_bool sawFree{false};

  // Читатель спрашивает заведомо свободное имя; писатель тем временем гоняет
  // индекс через переименования, то есть через обмен флага полноты.
  std::thread reader([&]()
    {
      while (not stop.load())
      {
        if (source.IsCharacterNameUnique("Gamma"))
          sawFree.store(true);
      }
    });

  for (int round = 0; round < 3000; ++round)
    source.StoreCharacter(2, [&]{
      server::data::Character character;
      character.uid = 2;
      character.name = std::string((round % 2) ? "Beta" : "Beta2");
      return character; }());

  stop.store(true);
  reader.join();

  // Индекс всё это время неполон (`1.json` так и не починен), значит ответ
  // «свободно» не имел права прозвучать ни разу.
  assert(not sawFree.load());

  std::error_code error;
  std::filesystem::remove_all(root, error);
}


//! ★ПРОСЬБА С ПРОВОДА НЕ УМЕЕТ ОПУСТИТЬ ПОЛ (ревью, итерация 11, находка 1).
//!
//! `RequestScheduledNameIndexRepair` ставила срок ближайшего прохода в прошлое,
//! а плановый проход читал это как «пора». То есть путь, до которого достаёт
//! имя с провода, ОТМЕНЯЛ шестидесятисекундный предел: при постоянно битом
//! файле клиент, повторяющий запрос после каждого неудачного прохода, покупал
//! полный обход каталога на каждом тике директора данных (50 Гц). Здесь
//! проверено обратное утверждение: сколько ни проси, раньше пола проход не
//! случится, а на полу случится РОВНО ОДИН — просьбы не копятся в очередь
//! проходов.
void TestRequestsCannotLowerTheRepairFloor()
{
  const auto root = MakeSandbox("repair-floor");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "characters" / "1.json", "{ this is not json");
  WriteRaw(root / "characters" / "2.json", R"({"uid": 2, "name": "Beta"})");

  server::FileDataSource source;
  source.Initialize(root);

  const auto base = std::chrono::steady_clock::now();
  CHECK(source.ScheduledNameIndexPassCount() == 0,
    "до первого тика проходов быть не может");

  // Первый проход после старта идёт без ожидания — неполная стартовая сборка
  // не имеет права ждать минуту.
  source.TickNameIndexMaintenanceAt(base);
  CHECK(source.ScheduledNameIndexPassCount() == 1,
    "первый тик после старта обязан осмотреть индексы сразу");

  // ТРИ ПРОСЬБЫ ВНУТРИ МИНУТЫ — и между ними тики, как у живого директора.
  for (int second = 1; second <= 3; ++second)
  {
    source.RequestScheduledNameIndexRepairForTest();
    for (int tick = 0; tick < 50; ++tick)
      source.TickNameIndexMaintenanceAt(base + std::chrono::seconds(second));
  }
  CHECK(source.ScheduledNameIndexPassCount() == 1,
    "просьба с провода опустила пол: проход случился раньше минуты");

  // За секунду до пола — всё ещё нет.
  source.TickNameIndexMaintenanceAt(base + std::chrono::seconds(59));
  CHECK(source.ScheduledNameIndexPassCount() == 1,
    "проход случился до истечения пола");

  // НА ПОЛУ — ровно один проход на все три просьбы.
  source.TickNameIndexMaintenanceAt(base + std::chrono::seconds(60));
  CHECK(source.ScheduledNameIndexPassCount() == 2,
    "на полу отложенная просьба обязана быть исполнена");
  source.TickNameIndexMaintenanceAt(base + std::chrono::seconds(61));
  source.TickNameIndexMaintenanceAt(base + std::chrono::seconds(120));
  CHECK(source.ScheduledNameIndexPassCount() == 2,
    "три просьбы дали больше одного прохода — они копятся в очередь");

  // И БЕЗ ПРОСЬБ проход всё равно приходит сам — реже, по периоду.
  source.TickNameIndexMaintenanceAt(base + std::chrono::seconds(60 + 300));
  CHECK(source.ScheduledNameIndexPassCount() == 3,
    "периодический осмотр перестал случаться сам");

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

//! ★ОТМЕТКА О НЕУДАЧЕ И ПУБЛИКАЦИЯ — ОДИН ПРОТОКОЛ (ревью, итерация 11, 2).
//!
//! Прежде путь отказа `IndexUserName` писал `false`, а поколение неудачи
//! поднимал ПОСЛЕ — и между двумя записями помещался целый обход: он читал ещё
//! не поднятое поколение и клал `true`. Итог — «индекс полон» при пропавшем
//! аккаунте.
//!
//! ★ЧЕРЕДОВКА ВОСПРОИЗВОДИТСЯ, А НЕ ВЫЖИДАЕТСЯ. Гонка двух потоков дала бы
//! тест, который молчит чаще, чем ловит. Крючок стоит РОВНО в точке бывшего
//! разрыва, и опоздавший обход пробует опубликовать оттуда `true`
//! неблокирующе: отказ `try_lock` — это и есть «чередовки нет».
server::FileDataSource* g_interleaveSource = nullptr;
std::size_t g_interleaveGeneration = 0;
bool g_interleaveHookRan = false;
bool g_interleavePublished = false;

void AttemptStalePublicationFromInsideTheFailure()
{
  g_interleaveHookRan = true;
  g_interleavePublished =
    g_interleaveSource->TryPublishStaleUserIndexStampForTest(
      g_interleaveGeneration);
}

void TestStaleScanCannotPublishOverAFailure()
{
  server::FileDataSource source;

  // Обход УЖЕ идёт: его поколение — то самое, которое отметит наша неудача.
  const std::size_t generation = source.BeginUserIndexScanForTest();
  CHECK(generation == 1, "поколение обхода обязано начинаться с единицы");

  g_interleaveSource = &source;
  g_interleaveGeneration = generation;
  g_interleaveHookRan = false;
  g_interleavePublished = false;
  source.SetUserIndexFailureInterleaveHookForTest(
    &AttemptStalePublicationFromInsideTheFailure);

  source.MarkUserIndexFailureForTest();

  source.SetUserIndexFailureInterleaveHookForTest(nullptr);
  g_interleaveSource = nullptr;

  CHECK(g_interleaveHookRan,
    "наблюдатель не был позван — тест не смотрел на предмет");
  CHECK(not g_interleavePublished,
    "опоздавший обход опубликовал отметку ВНУТРИ отметки о неудаче");
  CHECK(not source.UserIndexStampValidForTest(),
    "после неудачи регистрации индекс объявлен полным");

  // И ПОСЛЕ секции — замок свободен, публикация физически возможна, но
  // поколение уже отмечено, поэтому её вердикт всё равно «не полон».
  CHECK(source.TryPublishStaleUserIndexStampForTest(generation),
    "замок остался занят после отметки о неудаче");
  CHECK(not source.UserIndexStampValidForTest(),
    "обход своего поколения объявил индекс полным поверх отметки о неудаче");

  // А обход СЛЕДУЮЩЕГО поколения читает каталог уже после нашей записи и
  // потому вправе объявить себя полным — иначе фикс запер бы индекс навсегда.
  const std::size_t later = source.BeginUserIndexScanForTest();
  CHECK(source.TryPublishStaleUserIndexStampForTest(later),
    "замок остался занят");
  CHECK(source.UserIndexStampValidForTest(),
    "следующий обход больше не умеет объявить индекс полным");
}

//! ★АЛИАС ЛЮБОЙ ШИРИНЫ — ЭТО ТО ЖЕ ЧИСЛО (ревью, итерация 11, находка W5).
//!
//! Граница в десять байт стояла ПЕРЕД снятием ведущих нулей, поэтому
//! `00000000007.json` (одиннадцать байт) не был ни записью, ни алиасом: пол
//! счётчика uid его не резервировал. Следующему персонажу выдался бы uid 7,
//! `ProduceDataFilePath` написал бы `7.json` — и рядом остались бы два живых
//! файла на одну личность, ровно тот исход, который закрывала W5.
void TestWidelyPaddedAliasIsStillReserved()
{
  const auto root = MakeSandbox("padded-alias");
  std::filesystem::create_directories(root / "characters");
  WriteRaw(root / "characters" / "00000000007.json",
    R"({"uid": 7, "name": "PaddedAlias"})");
  WriteRaw(root / "characters" / "3.json", R"({"uid": 3, "name": "Real"})");

  server::FileDataSource source;
  source.Initialize(root);

  // Алиас — НЕ запись, каким бы широким ни было написание.
  CHECK(source.RetrieveCharacterUidByName("PaddedAlias")
      == server::data::InvalidUid,
    "неканоническое имя файла снова читается как запись");
  CHECK(source.RetrieveCharacterUidByName("Real") == server::data::Uid{3},
    "соседняя каноническая запись перестала находиться");
  CHECK(source.IsCharacterNameUnique("Delta"),
    "посторонний файл сделал индекс неполным");

  // ★НО ПОЛ СЧЁТЧИКА ЕГО РЕЗЕРВИРУЕТ.
  server::data::Character created;
  source.CreateCharacter(created);
  CHECK(created.uid() > server::data::Uid{7},
    "широко дополненный алиас не зарезервирован: uid выдан занятым");

  // И имя, чьё ЗНАЧЕНИЕ не представимо, записью по-прежнему не становится —
  // снятие нулей не превратило границу в «любая длина».
  WriteRaw(root / "characters" / "99999999999.json",
    R"({"uid": 5, "name": "TooBig"})");
  server::FileDataSource second;
  second.Initialize(root);
  CHECK(second.RetrieveCharacterUidByName("TooBig") == server::data::InvalidUid,
    "непредставимое число стало записью");

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
  TestScheduledPassRepairsTheIndexAndLookupsDoNot();
  TestLegacyNameIsFindableButNotCreatable();
  TestNoncanonicalFileNameIsNotARecord();
  TestIncompleteIndexNeverAnswersFree();
  TestRequestsCannotLowerTheRepairFloor();
  TestStaleScanCannotPublishOverAFailure();
  TestWidelyPaddedAliasIsStillReserved();
  if (g_failures > 0)
  {
    std::fprintf(stderr, "TestFileDataSource: %d НАРУШЕНИЙ\n", g_failures);
    return 1;
  }
  std::puts("TestFileDataSource: ok");
  return 0;
}
